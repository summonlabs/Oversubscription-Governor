// Oversubscription Governor — policy, contention domains, and risk budget.
//
// Policy is durable configuration: it is written by operators, versioned by
// generation, integrity-checked on load, and never derived from telemetry.
#ifndef OVERSUB_POLICY_HPP
#define OVERSUB_POLICY_HPP

#include <string>
#include <vector>

#include "oversub/checked.hpp"
#include "oversub/digest.hpp"
#include "oversub/ids.hpp"
#include "oversub/status.hpp"

namespace oversub {

// Hard compile-time bounds. Durable input can never raise them.
inline constexpr u32 kHardMaxDomains = 4096;
inline constexpr u32 kHardMaxResourcesPerDomain = 4096;
inline constexpr u32 kHardMaxServiceClasses = 4096;
inline constexpr u32 kHardMaxCorrectiveIntents = 64;
inline constexpr u32 kHardMaxExplanationLines = 4096;
inline constexpr u32 kHardMaxExplanationBytes = 262144;
inline constexpr u32 kHardMaxNameLength = 128;
inline constexpr u64 kHardMaxRatioTermBound = 1000000ULL;
inline constexpr u64 kHardMaxExposurePpm = 10000000ULL;   // 10x usable capacity
inline constexpr u64 kHardMaxFreshnessTicks = 1ULL << 40;
inline constexpr u64 kHardMaxCooldownTicks = 1ULL << 32;
inline constexpr u64 kHardMaxConfirmations = 64;
inline constexpr u64 kHardMaxGuaranteesPerPolicy = 65536;
inline constexpr u64 kHardMaxDemandRecordsPerPolicy = 65536;

// A QoS/service-class reference. The governor never assigns priorities or
// weights; it only consumes references owned by the adjacent QoS system.
struct ServiceClassRef {
  ServiceClassId id;
  std::string name;
  PriorityRef priority;
  bool oversubscription_eligible{true};
  u32 risk_weight_ppm{static_cast<u32>(kPpmScale)};
  bool has_contingent_share_cap{false};
  u64 contingent_share_cap_ppm{0};   // share of the contingent ceiling
};

struct RiskBudgetPolicy {
  RiskBudgetId id;
  RiskBudgetGeneration generation;
  bool has_absolute{false};
  u64 max_exposure_units{0};   // commitment allowed beyond usable capacity
  bool has_ppm{false};
  u64 max_exposure_ppm{0};     // commitment allowed beyond usable capacity, in ppm of usable
};

struct OversubscriptionThresholds {
  u64 warn_used_ppm{900000};          // announcement threshold; not an authority change
  u64 reduce_overage_ppm{1};          // any overage reaches REDUCTION_REQUIRED
  u64 emergency_overage_ppm{100000};  // overage >= this share of the ceiling is an emergency
};

struct HysteresisPolicy {
  u64 escalation_confirmations{1};    // confirmations before announcing AT_LIMIT
  u64 deescalation_confirmations{1};  // confirmations before relaxing severity
  u64 cooldown_ticks{0};              // minimum ticks between escalation and relaxation
  u64 deescalation_margin_ppm{0};     // slack required before relaxing
};

struct DomainConfig {
  OversubscriptionDomainId id;
  DomainGeneration generation;
  std::string name;
  PoolId pool;
  std::vector<ResourceId> members;      // contention-domain membership
  Ratio configured_ratio;               // deliberate oversubscription configuration
  bool has_degraded_ratio_cap{false};
  Ratio degraded_ratio_cap;             // applied when any member is degraded or failed
  u64 emergency_reserve_units{0};       // capacity withheld from every commitment
  u64 min_protected_headroom_units{0};  // capacity that must stay uncommitted
  bool burst_pool_enabled{false};
  u64 burst_pool_cap_units{0};
};

struct PolicyLimits {
  u32 max_domains{256};
  u32 max_resources_per_domain{256};
  u32 max_service_classes{256};
  u64 max_guarantees_per_snapshot{16384};
  u64 max_demand_records_per_snapshot{32768};
  u32 max_corrective_intents{8};
  u32 max_explanation_lines{64};
  u32 max_explanation_bytes{8192};
  u32 max_resource_breakdown{32};
  u64 max_capacity_units{kHardMaxCapacityUnits};
};

struct OversubscriptionPolicy {
  OversubscriptionPolicyId id;
  PolicyGeneration generation;
  std::string name;
  Ratio policy_ratio_cap;   // hard cap; deliberate oversubscription never exceeds it
  std::vector<DomainConfig> domains;
  std::vector<ServiceClassRef> service_classes;
  RiskBudgetPolicy risk_budget;
  OversubscriptionThresholds thresholds;
  HysteresisPolicy hysteresis;
  PolicyLimits limits;
  u64 max_capacity_age_ticks{1};
  u64 max_reservation_age_ticks{1};
  u64 max_admission_age_ticks{1};
  Digest digest;   // canonical digest of everything above; verified on load
};

// Full validation. Any failure means the policy is not installable.
Status validate_policy(const OversubscriptionPolicy& policy);
// Recomputes and stores policy.digest. Returns the digest.
Digest compute_policy_digest(OversubscriptionPolicy& policy);
Digest policy_digest(const OversubscriptionPolicy& policy);
// Digest of one domain's configuration, excluding its generation. Used to decide
// whether a policy install actually changed the domain's legal boundary.
Digest domain_config_digest(const DomainConfig& domain);

const DomainConfig* find_domain(const OversubscriptionPolicy& policy, OversubscriptionDomainId id);
const ServiceClassRef* find_service_class(const OversubscriptionPolicy& policy, ServiceClassId id);

}  // namespace oversub

#endif  // OVERSUB_POLICY_HPP
