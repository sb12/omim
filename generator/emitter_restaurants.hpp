#include "generator/emitter_interface.hpp"
#include "generator/feature_builder.hpp"

#include <vector>
#include <string>

namespace generator
{
class EmitterRestaurants : public EmitterInterface
{
public:
  EmitterRestaurants(std::vector<feature::FeatureBuilder> & features);

  // EmitterInterface overrides:
  void Process(feature::FeatureBuilder & fb) override;
  void GetNames(std::vector<std::string> & names) const override;
  bool Finish() override;

private:
  struct Stats
  {
    // Number of features of any "food type".
    uint32_t m_restaurantsPoi = 0;
    uint32_t m_restaurantsBuilding = 0;
    uint32_t m_unexpectedFeatures = 0;
  };

  std::vector<feature::FeatureBuilder> & m_features;
  Stats m_stats;
};
}  // namespace generator
