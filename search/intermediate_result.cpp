#include "search/intermediate_result.hpp"
#include "search/geometry_utils.hpp"
#include "search/reverse_geocoder.hpp"

#include "storage/country_info_getter.hpp"

#include "indexer/categories_holder.hpp"
#include "indexer/classificator.hpp"
#include "indexer/cuisines.hpp"
#include "indexer/feature.hpp"
#include "indexer/feature_algo.hpp"
#include "indexer/ftypes_matcher.hpp"
#include "indexer/ftypes_sponsored.hpp"
#include "indexer/scales.hpp"

#include "geometry/angles.hpp"

#include "platform/measurement_utils.hpp"

#include "base/logging.hpp"
#include "base/string_utils.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>

#include "3party/opening_hours/opening_hours.hpp"

using namespace std;

namespace search
{
namespace
{
char const * const kPricingSymbol = "$";

class SkipRegionInfo
{
  static size_t const kCount = 2;
  uint32_t m_types[kCount];

public:
  SkipRegionInfo()
  {
    char const * arr[][2] = {
      {"place", "continent"},
      {"place", "country"}
    };
    static_assert(kCount == ARRAY_SIZE(arr), "");

    Classificator const & c = classif();
    for (size_t i = 0; i < kCount; ++i)
      m_types[i] = c.GetTypeByPath(vector<string>(arr[i], arr[i] + 2));
  }

  bool IsSkip(uint32_t type) const
  {
    for (uint32_t t : m_types)
    {
      if (t == type)
        return true;
    }
    return false;
  }
};
}  // namespace

// PreRankerResult ---------------------------------------------------------------------------------
PreRankerResult::PreRankerResult(FeatureID const & id, PreRankingInfo const & info,
                                 vector<ResultTracer::Branch> const & provenance)
  : m_id(id), m_info(info), m_provenance(provenance)
{
  ASSERT(m_id.IsValid(), ());
}

// static
bool PreRankerResult::LessRankAndPopularity(PreRankerResult const & lhs,
                                            PreRankerResult const & rhs)
{
  if (lhs.m_info.m_rank != rhs.m_info.m_rank)
    return lhs.m_info.m_rank > rhs.m_info.m_rank;
  if (lhs.m_info.m_popularity != rhs.m_info.m_popularity)
    return lhs.m_info.m_popularity > rhs.m_info.m_popularity;
  return lhs.m_info.m_distanceToPivot < rhs.m_info.m_distanceToPivot;
}

// static
bool PreRankerResult::LessDistance(PreRankerResult const & lhs, PreRankerResult const & rhs)
{
  if (lhs.m_info.m_distanceToPivot != rhs.m_info.m_distanceToPivot)
    return lhs.m_info.m_distanceToPivot < rhs.m_info.m_distanceToPivot;
  return lhs.m_info.m_rank > rhs.m_info.m_rank;
}

// static
bool PreRankerResult::LessByExactMatch(PreRankerResult const & lhs, PreRankerResult const & rhs)
{
  auto const lhsScore = lhs.m_info.m_exactMatch && lhs.m_info.m_allTokensUsed;
  auto const rhsScore = rhs.m_info.m_exactMatch && rhs.m_info.m_allTokensUsed;
  if (lhsScore != rhsScore)
    return lhsScore;

  return LessDistance(lhs, rhs);
}

bool PreRankerResult::CategoriesComparator::operator()(PreRankerResult const & lhs,
                                                       PreRankerResult const & rhs) const
{
  if (m_positionIsInsideViewport)
    return lhs.GetDistance() < rhs.GetDistance();

  if (m_detailedScale)
  {
    bool const lhsInside = m_viewport.IsPointInside(lhs.GetInfo().m_center);
    bool const rhsInside = m_viewport.IsPointInside(rhs.GetInfo().m_center);
    if (lhsInside && !rhsInside)
      return true;
    if (rhsInside && !lhsInside)
      return false;
  }
  return lhs.GetPopularity() > rhs.GetPopularity();
}

// RankerResult ------------------------------------------------------------------------------------
RankerResult::RankerResult(FeatureType & f, m2::PointD const & center, m2::PointD const & pivot,
                           string const & displayName, string const & fileName)
  : m_id(f.GetID())
  , m_types(f)
  , m_str(displayName)
  , m_resultType(ftypes::IsBuildingChecker::Instance()(m_types) ? TYPE_BUILDING : TYPE_FEATURE)
  , m_geomType(f.GetGeomType())
{
  ASSERT(m_id.IsValid(), ());
  ASSERT(!m_types.Empty(), ());

  m_types.SortBySpec();

  m_region.SetParams(fileName, center);
  m_distance = PointDistance(center, pivot);

  ProcessMetadata(f, m_metadata);
}

RankerResult::RankerResult(double lat, double lon)
  : m_str("(" + measurement_utils::FormatLatLon(lat, lon) + ")"), m_resultType(TYPE_LATLON)
{
  m_region.SetParams(string(), MercatorBounds::FromLatLon(lat, lon));
}

bool RankerResult::GetCountryId(storage::CountryInfoGetter const & infoGetter, uint32_t ftype,
                                storage::CountryId & countryId) const
{
  static SkipRegionInfo const checker;
  if (checker.IsSkip(ftype))
    return false;
  return m_region.GetCountryId(infoGetter, countryId);
}

bool RankerResult::IsEqualCommon(RankerResult const & r) const
{
  if ((m_geomType != r.m_geomType) || (m_str != r.m_str))
    return false;

  auto const bestType = GetBestType();
  auto const rBestType = r.GetBestType();
  if (bestType == rBestType)
    return true;

  auto const & checker = ftypes::IsWayChecker::Instance();
  return checker(bestType) && checker(rBestType);
}

bool RankerResult::IsStreet() const { return ftypes::IsStreetOrSuburbChecker::Instance()(m_types); }

uint32_t RankerResult::GetBestType(vector<uint32_t> const & preferredTypes) const
{
  ASSERT(is_sorted(preferredTypes.begin(), preferredTypes.end()), ());
  if (!preferredTypes.empty())
  {
    for (uint32_t type : m_types)
    {
      if (binary_search(preferredTypes.begin(), preferredTypes.end(), type))
        return type;
    }
  }

  return m_types.GetBestType();
}

// RankerResult::RegionInfo ------------------------------------------------------------------------
bool RankerResult::RegionInfo::GetCountryId(storage::CountryInfoGetter const & infoGetter,
                                            storage::CountryId & countryId) const
{
  if (!m_countryId.empty())
  {
    countryId = m_countryId;
    return true;
  }

  auto const id = infoGetter.GetRegionCountryId(m_point);
  if (id != storage::kInvalidCountryId)
  {
    countryId = id;
    return true;
  }

  return false;
}

// Functions ---------------------------------------------------------------------------------------
void ProcessMetadata(FeatureType & ft, Result::Metadata & meta)
{
  if (meta.m_isInitialized)
    return;

  feature::Metadata const & src = ft.GetMetadata();

  auto const cuisinesMeta = src.Get(feature::Metadata::FMD_CUISINE);
  if (cuisinesMeta.empty())
  {
    meta.m_cuisine = "";
  }
  else
  {
    vector<string> cuisines;
    osm::Cuisines::Instance().ParseAndLocalize(cuisinesMeta, cuisines);
    meta.m_cuisine = strings::JoinStrings(cuisines, " • ");
  }

  meta.m_airportIata = src.Get(feature::Metadata::FMD_AIRPORT_IATA);
  meta.m_brand = src.Get(feature::Metadata::FMD_BRAND);

  string const openHours = src.Get(feature::Metadata::FMD_OPEN_HOURS);
  if (!openHours.empty())
  {
    osmoh::OpeningHours const oh(openHours);
    // TODO: We should check closed/open time for specific feature's timezone.
    time_t const now = time(nullptr);
    if (oh.IsValid() && !oh.IsUnknown(now))
      meta.m_isOpenNow = oh.IsOpen(now) ? osm::Yes : osm::No;
    // In else case value us osm::Unknown, it's set in preview's constructor.
  }

  if (strings::to_int(src.Get(feature::Metadata::FMD_STARS), meta.m_stars))
    meta.m_stars = base::clamp(meta.m_stars, 0, 5);
  else
    meta.m_stars = 0;

  bool const isSponsoredHotel = ftypes::IsBookingChecker::Instance()(ft);
  meta.m_isSponsoredHotel = isSponsoredHotel;
  meta.m_isHotel = ftypes::IsHotelChecker::Instance()(ft);

  if (isSponsoredHotel)
  {
    auto const r = src.Get(feature::Metadata::FMD_RATING);
    if (!r.empty())
    {
      float raw;
      if (strings::to_float(r.c_str(), raw))
        meta.m_hotelRating = raw;
    }


    int pricing;
    if (!strings::to_int(src.Get(feature::Metadata::FMD_PRICE_RATE), pricing))
      pricing = 0;
    string pricingStr;
    CHECK_GREATER_OR_EQUAL(pricing, 0, ("Pricing must be positive!"));
    for (auto i = 0; i < pricing; i++)
      pricingStr.append(kPricingSymbol);

    meta.m_hotelPricing = pricing;
    meta.m_hotelApproximatePricing = pricingStr;
  }

  meta.m_isInitialized = true;
}

string DebugPrint(RankerResult const & r)
{
  stringstream ss;
  ss << "RankerResult ["
     << "Name: " << r.GetName()
     << "; Type: " << r.GetBestType();

    if (!r.GetProvenance().empty())
      ss << "; Provenance: " << ::DebugPrint(r.GetProvenance());

     ss << "; " << DebugPrint(r.GetRankingInfo())
     << "; Linear model rank: " << r.GetLinearModelRank()
     << "]";
  return ss.str();
}
}  // namespace search
