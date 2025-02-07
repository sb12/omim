#pragma once

#include "generator/feature_builder.hpp"

#include <queue>

struct OsmElement;

namespace generator
{
namespace cache
{
class IntermediateDataReader;
}  // namespace cache

// Abstract class FeatureMakerBase is responsible for the conversion OsmElement to FeatureBuilder1.
// The main task of this class is to create features of the necessary types.
// At least one feature should turn out from one OSM element. You can get several features from one element.
class FeatureMakerBase
{
public:
  explicit FeatureMakerBase(cache::IntermediateDataReader & cache);
  virtual ~FeatureMakerBase() = default;

  bool Add(OsmElement & element);
  // The function returns true when the receiving feature was successful and a false when not successful.
  bool GetNextFeature(feature::FeatureBuilder & feature);
  size_t Size() const;
  bool Empty() const;

protected:
  virtual bool BuildFromNode(OsmElement & element, FeatureParams const & params) = 0;
  virtual bool BuildFromWay(OsmElement & element, FeatureParams const & params) = 0;
  virtual bool BuildFromRelation(OsmElement & element, FeatureParams const & params) = 0;

  virtual void ParseParams(FeatureParams & params, OsmElement & element) const  = 0;

  cache::IntermediateDataReader & m_cache;
  std::queue<feature::FeatureBuilder> m_queue;
};

void TransformAreaToPoint(feature::FeatureBuilder & feature);
void TransformAreaToLine(feature::FeatureBuilder & feature);

feature::FeatureBuilder MakePointFromArea(feature::FeatureBuilder const & feature);
feature::FeatureBuilder MakeLineFromArea(feature::FeatureBuilder const & feature);
}  // namespace generator
