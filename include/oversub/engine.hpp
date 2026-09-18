// Oversubscription Governor — the deterministic governance engine.
//
// evaluate_domain() is a pure function. Given a validated policy, the authority
// the runtime currently owns, and the exact evidence snapshots, it returns the
// legal oversubscription boundary, the binding constraint, and bounded
// corrective intent. It performs no I/O, allocates no threads, and reads no
// wall-clock time, so identical inputs always produce identical results.
#ifndef OVERSUB_ENGINE_HPP
#define OVERSUB_ENGINE_HPP

#include <string>
#include <vector>

#include "oversub/digest.hpp"
#include "oversub/evidence.hpp"
#include "oversub/policy.hpp"
#include "oversub/status.hpp"

namespace oversub {

// Sorted lookup tables over a policy. Holds non-owning pointers: the referenced
// policy must outlive the index.
class PolicyIndex {
 public:
  PolicyIndex() = default;
  explicit PolicyIndex(const OversubscriptionPolicy& policy) { rebuild(policy); }
  void rebuild(const OversubscriptionPolicy& policy);
  const DomainConfig* domain(OversubscriptionDomainId id) const;
  const ServiceClassRef* service_class(ServiceClassId id) const;
  std::size_t domain_count() const { return domains_.size(); }

 private:
  std::vector<const DomainConfig*> domains_;
  std::vector<const ServiceClassRef*> classes_;
};

// The exact generations/epoch that justify a decision. A decision is only valid
// while every field still matches the runtime's current authority.
struct AuthorityVector {
  OversubscriptionDomainId domain;
  DomainGeneration domain_generation;
  OversubscriptionPolicyId policy;
  PolicyGeneration policy_generation;
  CapacitySnapshotGeneration capacity_generation;
  ReservationSnapshotGeneration reservation_generation;
  AdmissionSnapshotGeneration admission_generation;
  RiskBudgetId risk_budget;
  RiskBudgetGeneration risk_budget_generation;
  FabricEpoch epoch;
  u64 resource_set_fold{0};   // fold over member resource generations
  Digest input_digest;        // canonical digest of the evaluated evidence

  bool complete() const;
  std::string to_string() const;
};

// What the runtime currently owns. Evidence claiming anything else is stale.
struct AuthorityExpectation {
  FabricEpoch epoch;
  PolicyGeneration policy_generation;
  DomainGeneration domain_generation;
  CapacitySnapshotGeneration capacity_generation;
  ReservationSnapshotGeneration reservation_generation;
  AdmissionSnapshotGeneration admission_generation;
  RiskBudgetGeneration risk_budget_generation;
};

struct DomainEvidence {
  const CapacitySnapshot* capacity{nullptr};
  const ReservationSnapshot* reservations{nullptr};
  const AdmissionSnapshot* admissions{nullptr};
};

// Per-resource answer to "where may deliberate oversubscription occur".
struct ResourceAccounting {
  ResourceId resource;
  PoolId pool;
  ResourceHealth health{ResourceHealth::Unknown};
  u64 physical_capacity{0};
  u64 usable_capacity{0};            // as advertised
  u64 effective_usable_capacity{0};  // after health degradation
  u64 guarantees{0};
  u64 contingent_committed{0};
  u64 contingent_pending{0};
  u64 ceiling{0};                    // legal total commitment on this resource
  u64 contingent_ceiling{0};
  u64 contingent_free{0};
  u64 overage{0};
  u64 protected_headroom{0};
  Constraint binding_constraint{Constraint::None};
};

struct DomainAccounting {
  OversubscriptionDomainId domain;
  PoolId pool;

  u64 physical_capacity{0};
  u64 usable_capacity{0};
  u64 effective_usable_capacity{0};   // after health degradation
  u64 guarantees_total{0};
  u64 emergency_reserve{0};
  u64 min_protected_headroom{0};
  u64 protected_required{0};          // guarantees + emergency reserve
  u64 floor_required{0};              // protected_required + minimum headroom

  Ratio configured_ratio{1, 1};
  Ratio effective_ratio{1, 1};
  u64 configured_ratio_ppm{kPpmScale};
  u64 effective_ratio_ppm{kPpmScale};
  bool ratio_reduced_by_degradation{false};

  u64 ceiling_total{0};               // maximum legal total commitment
  u64 contingent_ceiling{0};
  u64 contingent_committed{0};
  u64 contingent_pending{0};
  u64 burst_committed{0};
  u64 guaranteed_admitted{0};
  u64 contingent_free{0};
  u64 committed_total{0};
  u64 overage{0};
  u64 protected_headroom{0};
  u64 used_ppm_of_ceiling{0};

  u64 exposure{0};                    // commitment beyond usable capacity
  u64 risk_budget_ceiling{0};
  u64 risk_exposure_remaining{0};
  bool risk_budget_exceeded{false};

  u64 out_of_scope_guarantees{0};
  u64 out_of_scope_demand{0};
  u32 degraded_resources{0};
  u32 failed_resources{0};
  Constraint binding_constraint{Constraint::None};
  bool approaching_limit{false};
  bool arithmetic_closed{true};
};

struct CorrectiveIntent {
  CorrectiveKind kind{CorrectiveKind::None};
  CorrectiveTarget target{CorrectiveTarget::Domain};
  OversubscriptionDomainId domain;
  ResourceId resource;
  PoolId pool;
  ServiceClassId service_class;
  u64 amount_units{0};
  ReasonCode reason{ReasonCode::None};
  std::string rationale;
};

struct EvaluationResult {
  Outcome outcome{Outcome::Unknown};
  ReasonCode reason{ReasonCode::None};
  std::string detail;

  DomainAccounting accounting;
  std::vector<ResourceAccounting> resources;
  std::vector<CorrectiveIntent> intents;
  std::vector<std::string> notes;
  AuthorityVector authority;

  // Authority derived from the outcome. Withholding authority is always allowed;
  // granting it requires an authoritative WITHIN_POLICY result.
  bool authority_valid{false};
  bool new_oversubscription_authorized{false};
  u64 authorized_increment_units{0};
  bool fence_required{false};

  Digest result_digest;
};

struct ExplainOptions {
  u32 max_lines{64};
  u32 max_bytes{8192};
  bool include_resources{true};
  bool include_notes{true};
  bool include_intents{true};
};

// Pure governance evaluation. The policy must have passed validate_policy(); the
// engine defensively re-checks the values it consumes. The domain must belong to
// the policy.
EvaluationResult evaluate_domain(const OversubscriptionPolicy& policy,
                                 const PolicyIndex& index,
                                 const DomainConfig& domain,
                                 const AuthorityExpectation& expectation,
                                 const DomainEvidence& evidence,
                                 u64 tick);

// Recomputes result_digest and the authority grant fields from the outcome and
// accounting. Called by evaluate_domain() and by callers that synthesize a
// result directly (for example a domain that policy does not cover).
Digest finalize_evaluation_result(EvaluationResult& result);

// Canonical digest of an evaluation result (outcome, reason, accounting,
// intents). Deterministic across processes and restarts.
Digest evaluation_result_digest(const EvaluationResult& result);

// Human-readable, bounded explanation.
std::string explain_evaluation(const OversubscriptionPolicy& policy,
                               const EvaluationResult& result,
                               const ExplainOptions& options);

}  // namespace oversub

#endif  // OVERSUB_ENGINE_HPP
