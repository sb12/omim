#include "geocoder/geocoder.hpp"

#include "geocoder/hierarchy_reader.hpp"

#include "search/house_numbers_matcher.hpp"

#include "indexer/search_string_utils.hpp"

#include "base/assert.hpp"
#include "base/logging.hpp"
#include "base/scope_guard.hpp"
#include "base/stl_helpers.hpp"
#include "base/string_utils.hpp"
#include "base/timer.hpp"

#include <algorithm>
#include <numeric>
#include <set>
#include <thread>
#include <utility>

#include <boost/optional.hpp>

using namespace std;

namespace
{
size_t const kMaxResults = 100;

double GetWeight(geocoder::Type t)
{
  switch (t)
  {
  case geocoder::Type::Country: return 10.0;
  case geocoder::Type::Region: return 5.0;
  case geocoder::Type::Subregion: return 4.0;
  case geocoder::Type::Locality: return 3.0;
  case geocoder::Type::Suburb: return 3.0;
  case geocoder::Type::Sublocality: return 2.0;
  case geocoder::Type::Street: return 1.0;
  case geocoder::Type::Building: return 0.1;
  case geocoder::Type::Count: return 0.0;
  }
  UNREACHABLE();
}

// todo(@m) This is taken from search/geocoder.hpp. Refactor.
struct ScopedMarkTokens
{
  using Type = geocoder::Type;

  // The range is [l, r).
  ScopedMarkTokens(geocoder::Geocoder::Context & context, Type type, size_t l, size_t r)
    : m_context(context), m_type(type), m_l(l), m_r(r)
  {
    CHECK_LESS_OR_EQUAL(l, r, ());
    CHECK_LESS_OR_EQUAL(r, context.GetNumTokens(), ());

    for (size_t i = m_l; i < m_r; ++i)
      m_context.MarkToken(i, m_type);
  }

  ~ScopedMarkTokens()
  {
    for (size_t i = m_l; i < m_r; ++i)
      m_context.MarkToken(i, Type::Count);
  }

  geocoder::Geocoder::Context & m_context;
  Type const m_type;
  size_t m_l;
  size_t m_r;
};

geocoder::Type NextType(geocoder::Type type)
{
  CHECK_NOT_EQUAL(type, geocoder::Type::Count, ());
  auto t = static_cast<size_t>(type);
  return static_cast<geocoder::Type>(t + 1);
}

strings::UniString MakeHouseNumber(geocoder::Tokens const & tokens)
{
  return strings::MakeUniString(strings::JoinStrings(tokens, " "));
}
}  // namespace

namespace geocoder
{
// Geocoder::Context -------------------------------------------------------------------------------
Geocoder::Context::Context(string const & query) : m_beam(kMaxResults)
{
  search::NormalizeAndTokenizeAsUtf8(query, m_tokens);
  m_tokenTypes.assign(m_tokens.size(), Type::Count);
  m_numUsedTokens = 0;
}

vector<Type> & Geocoder::Context::GetTokenTypes() { return m_tokenTypes; }

size_t Geocoder::Context::GetNumTokens() const { return m_tokens.size(); }

size_t Geocoder::Context::GetNumUsedTokens() const
{
  CHECK_LESS_OR_EQUAL(m_numUsedTokens, m_tokens.size(), ());
  return m_numUsedTokens;
}

Type Geocoder::Context::GetTokenType(size_t id) const
{
  CHECK_LESS(id, m_tokenTypes.size(), ());
  return m_tokenTypes[id];
}

string const & Geocoder::Context::GetToken(size_t id) const
{
  CHECK_LESS(id, m_tokens.size(), ());
  return m_tokens[id];
}

void Geocoder::Context::MarkToken(size_t id, Type type)
{
  CHECK_LESS(id, m_tokens.size(), ());
  bool wasUsed = m_tokenTypes[id] != Type::Count;
  m_tokenTypes[id] = type;
  bool nowUsed = m_tokenTypes[id] != Type::Count;

  if (wasUsed && !nowUsed)
    --m_numUsedTokens;
  if (!wasUsed && nowUsed)
    ++m_numUsedTokens;
}

bool Geocoder::Context::IsTokenUsed(size_t id) const
{
  CHECK_LESS(id, m_tokens.size(), ());
  return m_tokenTypes[id] != Type::Count;
}

bool Geocoder::Context::AllTokensUsed() const { return m_numUsedTokens == m_tokens.size(); }

void Geocoder::Context::AddResult(base::GeoObjectId const & osmId, double certainty, Type type,
                                  vector<size_t> const & tokenIds, vector<Type> const & allTypes)
{
  m_beam.Add(BeamKey(osmId, type, tokenIds, allTypes), certainty);
}

void Geocoder::Context::FillResults(vector<Result> & results) const
{
  results.clear();
  results.reserve(m_beam.GetEntries().size());

  set<base::GeoObjectId> seen;
  bool const hasPotentialHouseNumber = !m_houseNumberPositionsInQuery.empty();
  for (auto const & e : m_beam.GetEntries())
  {
    if (!seen.insert(e.m_key.m_osmId).second)
      continue;

    if (hasPotentialHouseNumber && !IsGoodForPotentialHouseNumberAt(e.m_key, m_houseNumberPositionsInQuery))
      continue;

    results.emplace_back(e.m_key.m_osmId, e.m_value /* certainty */);
  }

  if (!results.empty())
  {
    auto const by = results.front().m_certainty;
    for (auto & r : results)
    {
      r.m_certainty /= by;
      ASSERT_GREATER_OR_EQUAL(r.m_certainty, 0.0, ());
      ASSERT_LESS_OR_EQUAL(r.m_certainty, 1.0, ());
    }
  }

  ASSERT(is_sorted(results.rbegin(), results.rend(), base::LessBy(&Result::m_certainty)), ());
  ASSERT_LESS_OR_EQUAL(results.size(), kMaxResults, ());
}

vector<Geocoder::Layer> & Geocoder::Context::GetLayers() { return m_layers; }

vector<Geocoder::Layer> const & Geocoder::Context::GetLayers() const { return m_layers; }

void Geocoder::Context::MarkHouseNumberPositionsInQuery(vector<size_t> const & tokenIds)
{
  m_houseNumberPositionsInQuery.insert(tokenIds.begin(), tokenIds.end());
}

bool Geocoder::Context::IsGoodForPotentialHouseNumberAt(BeamKey const & beamKey,
                                                        set<size_t> const & tokenIds) const
{
  if (beamKey.m_tokenIds.size() == m_tokens.size())
    return true;

  if (IsBuildingWithAddress(beamKey))
    return true;

  // Pass street, locality or region with number in query address parts.
  if (HasLocalityOrRegion(beamKey) && ContainsTokenIds(beamKey, tokenIds))
    return true;

  return false;
}

bool Geocoder::Context::IsBuildingWithAddress(BeamKey const & beamKey) const
{
  if (beamKey.m_type != Type::Building)
    return false;

  bool gotLocality = false;
  bool gotStreet = false;
  bool gotBuilding = false;
  for (Type t : beamKey.m_allTypes)
  {
    if (t == Type::Region || t == Type::Subregion || t == Type::Locality)
      gotLocality = true;
    if (t == Type::Street)
      gotStreet = true;
    if (t == Type::Building)
      gotBuilding = true;
  }
  return gotLocality && gotStreet && gotBuilding;
}

bool Geocoder::Context::HasLocalityOrRegion(BeamKey const & beamKey) const
{
  for (Type t : beamKey.m_allTypes)
  {
    if (t == Type::Region || t == Type::Subregion || t == Type::Locality)
      return true;
  }

  return false;
}

bool Geocoder::Context::ContainsTokenIds(BeamKey const & beamKey, set<size_t> const & needTokenIds) const
{
  auto const & keyTokenIds = beamKey.m_tokenIds;
  return base::Includes(keyTokenIds.begin(), keyTokenIds.end(), needTokenIds.begin(), needTokenIds.end());
}

// Geocoder ----------------------------------------------------------------------------------------
Geocoder::Geocoder(string const & pathToJsonHierarchy, unsigned int loadThreadsCount)
  : Geocoder{HierarchyReader{pathToJsonHierarchy}.Read(loadThreadsCount), loadThreadsCount}
{
}

Geocoder::Geocoder(istream & jsonHierarchy, unsigned int loadThreadsCount)
  : Geocoder{HierarchyReader{jsonHierarchy}.Read(loadThreadsCount), loadThreadsCount}
{
}

Geocoder::Geocoder(Hierarchy && hierarchy, unsigned int loadThreadsCount)
  : m_hierarchy(move(hierarchy)), m_index(m_hierarchy, loadThreadsCount)
{
}

void Geocoder::ProcessQuery(string const & query, vector<Result> & results) const
{
#if defined(DEBUG)
  base::Timer timer;
  SCOPE_GUARD(printDuration, [&timer]() {
    LOG(LINFO, ("Total geocoding time:", timer.ElapsedSeconds(), "seconds"));
  });
#endif

  Context ctx(query);
  Go(ctx, Type::Country);
  ctx.FillResults(results);
}

Hierarchy const & Geocoder::GetHierarchy() const { return m_hierarchy; }

Index const & Geocoder::GetIndex() const { return m_index; }

void Geocoder::Go(Context & ctx, Type type) const
{
  if (ctx.GetNumTokens() == 0)
    return;

  if (ctx.AllTokensUsed())
    return;

  if (type == Type::Count)
    return;

  Tokens subquery;
  vector<size_t> subqueryTokenIds;
  for (size_t i = 0; i < ctx.GetNumTokens(); ++i)
  {
    subquery.clear();
    subqueryTokenIds.clear();
    for (size_t j = i; j < ctx.GetNumTokens(); ++j)
    {
      if (ctx.IsTokenUsed(j))
        break;

      subquery.push_back(ctx.GetToken(j));
      subqueryTokenIds.push_back(j);

      Layer curLayer;
      curLayer.m_type = type;

      // Buildings are indexed separately.
      if (type == Type::Building)
      {
        FillBuildingsLayer(ctx, subquery, subqueryTokenIds, curLayer);
      }
      else
      {
        FillRegularLayer(ctx, type, subquery, curLayer);
      }

      if (curLayer.m_entries.empty())
        continue;

      ScopedMarkTokens mark(ctx, type, i, j + 1);
      boost::optional<ScopedMarkTokens> streetSynonymMark;

      double certainty = 0;
      vector<size_t> tokenIds;
      vector<Type> allTypes;
      for (size_t tokId = 0; tokId < ctx.GetNumTokens(); ++tokId)
      {
        auto const t = ctx.GetTokenType(tokId);
        if (type == Type::Street && t == Type::Count && !streetSynonymMark)
        {
          if (search::IsStreetSynonym(strings::MakeUniString(ctx.GetToken(tokId))))
            streetSynonymMark.emplace(ctx, Type::Street, tokId, tokId + 1);
        }

        certainty += GetWeight(t);
        if (t != Type::Count)
        {
          tokenIds.push_back(tokId);
          allTypes.push_back(t);
        }
      }

      for (auto const & docId : curLayer.m_entries)
        ctx.AddResult(m_index.GetDoc(docId).m_osmId, certainty, type, tokenIds, allTypes);

      ctx.GetLayers().emplace_back(move(curLayer));
      SCOPE_GUARD(pop, [&] { ctx.GetLayers().pop_back(); });

      Go(ctx, NextType(type));
    }
  }

  Go(ctx, NextType(type));
}

void Geocoder::FillBuildingsLayer(Context & ctx, Tokens const & subquery, vector<size_t> const & subqueryTokenIds,
                                  Layer & curLayer) const
{
  if (ctx.GetLayers().empty())
    return;

  auto const & subqueryHN = MakeHouseNumber(subquery);

  if (!search::house_numbers::LooksLikeHouseNumber(subqueryHN, false /* isPrefix */))
    return;

  for_each(ctx.GetLayers().rbegin(), ctx.GetLayers().rend(), [&, this] (auto const & layer) {
    if (layer.m_type != Type::Street && layer.m_type != Type::Locality)
      return;

    // We've already filled a street/location layer and now see something that resembles
    // a house number. While it still can be something else (a zip code, for example)
    // let's stay on the safer side and mark the tokens as potential house number.
    ctx.MarkHouseNumberPositionsInQuery(subqueryTokenIds);

    for (auto const & docId : layer.m_entries)
    {
      m_index.ForEachRelatedBuilding(docId, [&](Index::DocId const & buildingDocId) {
        auto const & bld = m_index.GetDoc(buildingDocId);
        auto const & multipleHN = bld.GetNormalizedMultipleNames(
            Type::Building, m_hierarchy.GetNormalizedNameDictionary());
        auto const & realHN = multipleHN.GetMainName();
        auto const & realHNUniStr = strings::MakeUniString(realHN);
        if (search::house_numbers::HouseNumbersMatch(realHNUniStr, subqueryHN,
                                                     false /* queryIsPrefix */))
        {
          curLayer.m_entries.emplace_back(buildingDocId);
        }
      });
    }
  });
}

void Geocoder::FillRegularLayer(Context const & ctx, Type type, Tokens const & subquery,
                                Layer & curLayer) const
{
  m_index.ForEachDocId(subquery, [&](Index::DocId const & docId) {
    auto const & d = m_index.GetDoc(docId);
    if (d.m_type != type)
      return;

    if (ctx.GetLayers().empty() || HasParent(ctx.GetLayers(), d))
      curLayer.m_entries.emplace_back(docId);
  });
}

bool Geocoder::HasParent(vector<Geocoder::Layer> const & layers, Hierarchy::Entry const & e) const
{
  CHECK(!layers.empty(), ());
  auto const & layer = layers.back();
  for (auto const & docId : layer.m_entries)
  {
    // Note that the relationship is somewhat inverted: every ancestor
    // is stored in the address but the nodes have no information
    // about their children.
    if (m_hierarchy.IsParentTo(m_index.GetDoc(docId), e))
      return true;
  }
  return false;
}
}  // namespace geocoder
