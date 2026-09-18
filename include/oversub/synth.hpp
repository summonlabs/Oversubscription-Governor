// Oversubscription Governor — synthetic population generator.
//
// SYNTHETIC. Everything produced here is a deterministic in-memory population
// used by tests, benchmarks, examples, and the multiprocess proof. It is not
// physical-network telemetry and must never be presented as such.
#ifndef OVERSUB_SYNTH_HPP
#define OVERSUB_SYNTH_HPP

#include <string>

#include "oversub/evidence.hpp"
#include "oversub/policy.hpp"

namespace oversub {

struct SynthConfig {
  u64 seed{1};
  u32 domains{1};
  u32 resources_per_domain{4};
  u32 guarantees_per_resource{2};
  u32 demand_records_per_resource{4};
  u64 capacity_per_resource{1000000};
  u64 guarantee_ppm_of_usable{400000};    // share of usable capacity held as guarantees
  u64 contingent_ppm_of_ceiling{500000};  // share of the legal ceiling consumed by contingent demand
  Ratio configured_ratio{2, 1};
  bool has_degraded_ratio_cap{true};
  Ratio degraded_ratio_cap{3, 2};
  u64 emergency_reserve_ppm{20000};
  u64 min_protected_headroom_ppm{0};
  u64 risk_exposure_ppm{200000};
  u32 degraded_resources{0};
  u32 failed_resources{0};
  bool burst_pool_enabled{false};
  u64 burst_pool_units{0};
  u32 distinct_service_classes{2};
  OversubscriptionPolicyId policy_id{1};
  OversubscriptionDomainId first_domain_id{1};
  PublisherId publisher{1};
  IncarnationId incarnation{1};
  FabricEpoch epoch{1};
};

// Resource identity helper shared by every synthetic population.
inline ResourceId synth_resource_id(OversubscriptionDomainId domain, u32 index) {
  return ResourceId(domain.value() * 100000ULL + index + 1);
}

struct SynthesizedPopulation {
  OversubscriptionPolicy policy;
  CapacitySnapshot capacity;
  ReservationSnapshot reservations;
  AdmissionSnapshot admissions;
};

// Deterministic synthetic population. Identical configuration produces
// byte-identical policy and evidence digests.
SynthesizedPopulation synthesize(const SynthConfig& config);

// Deep-copies the population for one domain out of a multi-domain population, so
// it can be published as domain-scoped evidence.
SynthesizedPopulation slice_domain(const SynthesizedPopulation& population, OversubscriptionDomainId domain);

}  // namespace oversub

#endif  // OVERSUB_SYNTH_HPP
