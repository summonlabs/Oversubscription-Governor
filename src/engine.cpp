#include "oversub/engine.hpp"

#include <algorithm>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace oversub {
namespace {

inline u64 sat_add(u64 a, u64 b) { return add_sat(a, b); }

struct ResourceAccum {
  const ResourceCapacity* capacity{nullptr};
  u64 effective_usable{0};
  u64 guarantees{0};
  u64 contingent_committed{0};
  u64 contingent_pending{0};
  u64 committed{0};
  u64 ceiling{0};
  u64 contingent_ceiling{0};
  u64 contingent_free{0};
  u64 overage{0};
  u64 protected_headroom{0};
};

void add_intent(EvaluationResult& r, const PolicyLimits& limits, CorrectiveIntent intent) {
  if (r.intents.size() >= limits.max_corrective_intents) return;
  r.intents.push_back(std::move(intent));
}

CorrectiveIntent base_intent(const DomainConfig& domain, CorrectiveKind kind, CorrectiveTarget target,
                             ReasonCode reason, std::string rationale) {
  CorrectiveIntent i;
  i.kind = kind;
  i.target = target;
  i.domain = domain.id;
  i.pool = domain.pool;
  i.reason = reason;
  i.rationale = std::move(rationale);
  return i;
}

void note(EvaluationResult& r, const PolicyLimits& limits, std::string text) {
  if (r.notes.size() >= limits.max_explanation_lines) return;
  r.notes.push_back(std::move(text));
}

bool digest_of_snapshot_payload(const Digest& stored, const Digest& computed, Digest& out) {
  out = stored.is_zero() ? computed : stored;
  return true;
}

}  // namespace

bool AuthorityVector::complete() const {
  return domain.valid() && domain_generation.valid() && policy.valid() && policy_generation.valid() &&
         capacity_generation.valid() && reservation_generation.valid() && admission_generation.valid() &&
         risk_budget.valid() && risk_budget_generation.valid() && epoch.valid();
}

std::string AuthorityVector::to_string() const {
  std::string s;
  s += "domain=";
  s += domain.to_string();
  s += "@";
  s += domain_generation.to_string();
  s += " policy=";
  s += policy.to_string();
  s += "@";
  s += policy_generation.to_string();
  s += " capacity=";
  s += capacity_generation.to_string();
  s += " reservations=";
  s += reservation_generation.to_string();
  s += " admissions=";
  s += admission_generation.to_string();
  s += " risk_budget=";
  s += risk_budget.to_string();
  s += "@";
  s += risk_budget_generation.to_string();
  s += " epoch=";
  s += epoch.to_string();
  s += " resource_fold=";
  s += std::to_string(resource_set_fold);
  s += " input=";
  s += input_digest.to_hex().substr(0, 16);
  return s;
}

EvaluationResult evaluate_domain(const OversubscriptionPolicy& policy, const PolicyIndex& index,
                                 const DomainConfig& domain, const AuthorityExpectation& expectation,
                                 const DomainEvidence& evidence, u64 tick) {
  EvaluationResult r;
  DomainAccounting& acc = r.accounting;
  acc.domain = domain.id;
  acc.pool = domain.pool;
  acc.configured_ratio = domain.configured_ratio;
  acc.effective_ratio = domain.configured_ratio;
  acc.configured_ratio_ppm = ratio_ppm(domain.configured_ratio);
  acc.effective_ratio_ppm = acc.configured_ratio_ppm;
  acc.emergency_reserve = domain.emergency_reserve_units;
  acc.min_protected_headroom = domain.min_protected_headroom_units;

  r.authority.domain = domain.id;
  r.authority.domain_generation = domain.generation;
  r.authority.policy = policy.id;
  r.authority.policy_generation = policy.generation;
  r.authority.risk_budget = policy.risk_budget.id;
  r.authority.risk_budget_generation = policy.risk_budget.generation;
  r.authority.epoch = expectation.epoch;

  const PolicyLimits& limits = policy.limits;

  auto reject = [&](Outcome outcome, ReasonCode reason, std::string detail) {
    r.outcome = outcome;
    r.reason = reason;
    r.detail = std::move(detail);
    if (outcome == Outcome::Stale || outcome == Outcome::Unknown) {
      add_intent(r, limits,
                 base_intent(domain, CorrectiveKind::RevalidateEvidence, CorrectiveTarget::Domain,
                             reason == ReasonCode::None ? ReasonCode::EvidenceIncomplete : reason,
                             "authority cannot be established from the current evidence; revalidate"));
    } else if (outcome == Outcome::ConflictingInput) {
      add_intent(r, limits,
                 base_intent(domain, CorrectiveKind::ResolveConflictingEvidence, CorrectiveTarget::Domain,
                             ReasonCode::EvidenceContradictory,
                             "evidence is structurally contradictory; resolve before any authority"));
      add_intent(r, limits, base_intent(domain, CorrectiveKind::RevalidateEvidence, CorrectiveTarget::Domain,
                                        ReasonCode::EvidenceContradictory,
                                        "revalidate after the contradiction is resolved"));
    } else if (outcome == Outcome::PolicyRejected) {
      add_intent(r, limits,
                 base_intent(domain, CorrectiveKind::FenceOversubscribedAdmission, CorrectiveTarget::Domain,
                             reason, "policy does not authorize the current oversubscription state"));
    }
    r.fence_required = (outcome == Outcome::EmergencyReduction || outcome == Outcome::PolicyRejected ||
                        outcome == Outcome::ConflictingInput);
    finalize_evaluation_result(r);
    return r;
  };

  // ---------------------------------------------------------------- binding --
  if (evidence.capacity == nullptr || evidence.reservations == nullptr || evidence.admissions == nullptr) {
    std::string missing;
    if (evidence.capacity == nullptr) missing += "capacity ";
    if (evidence.reservations == nullptr) missing += "reservation ";
    if (evidence.admissions == nullptr) missing += "admission ";
    return reject(Outcome::Unknown, ReasonCode::EvidenceIncomplete, "missing evidence: " + missing);
  }
  const CapacitySnapshot& cap = *evidence.capacity;
  const ReservationSnapshot& res = *evidence.reservations;
  const AdmissionSnapshot& adm = *evidence.admissions;

  if (!expectation.epoch.valid()) {
    return reject(Outcome::Unknown, ReasonCode::GenerationUnset, "runtime fabric epoch is unset");
  }
  if (!expectation.domain_generation.valid() || !expectation.policy_generation.valid() ||
      !expectation.capacity_generation.valid() || !expectation.reservation_generation.valid() ||
      !expectation.admission_generation.valid() || !expectation.risk_budget_generation.valid()) {
    return reject(Outcome::Unknown, ReasonCode::GenerationUnset, "runtime authority generations are unset");
  }
  if (cap.domain != domain.id || res.domain != domain.id || adm.domain != domain.id) {
    return reject(Outcome::ConflictingInput, ReasonCode::EvidenceContradictory,
                  "evidence was published for a different domain");
  }
  if (cap.epoch != expectation.epoch || res.epoch != expectation.epoch || adm.epoch != expectation.epoch) {
    return reject(Outcome::Stale, ReasonCode::EpochMismatch, "evidence epoch does not match the fabric epoch");
  }
  if (domain.generation != expectation.domain_generation) {
    return reject(Outcome::Stale, ReasonCode::DomainGenerationMismatch,
                  "domain configuration generation changed since the evaluation was requested");
  }
  if (policy.generation != expectation.policy_generation) {
    return reject(Outcome::Stale, ReasonCode::PolicyGenerationMismatch,
                  "policy generation changed since the evaluation was requested");
  }
  if (policy.risk_budget.generation != expectation.risk_budget_generation) {
    return reject(Outcome::Stale, ReasonCode::PolicyRiskBudgetGenerationMismatch,
                  "risk budget generation changed since the evaluation was requested");
  }
  if (cap.generation != expectation.capacity_generation) {
    return reject(Outcome::Stale,
                  cap.generation > expectation.capacity_generation ? ReasonCode::GenerationAheadOfRuntime
                                                                   : ReasonCode::SnapshotGenerationMismatch,
                  "capacity snapshot generation does not match the authoritative generation");
  }
  if (res.generation != expectation.reservation_generation) {
    return reject(Outcome::Stale,
                  res.generation > expectation.reservation_generation ? ReasonCode::GenerationAheadOfRuntime
                                                                      : ReasonCode::SnapshotGenerationMismatch,
                  "reservation snapshot generation does not match the authoritative generation");
  }
  if (adm.generation != expectation.admission_generation) {
    return reject(Outcome::Stale,
                  adm.generation > expectation.admission_generation ? ReasonCode::GenerationAheadOfRuntime
                                                                    : ReasonCode::SnapshotGenerationMismatch,
                  "admission snapshot generation does not match the authoritative generation");
  }

  r.authority.capacity_generation = cap.generation;
  r.authority.reservation_generation = res.generation;
  r.authority.admission_generation = adm.generation;
  {
    CanonicalHasher input;
    input.add_string("oversub.input.v1");
    input.add_u64(domain.id.value());
    input.add_u64(domain.generation.value());
    input.add_u64(policy.id.value());
    input.add_u64(policy.generation.value());
    input.add_u64(policy.risk_budget.generation.value());
    input.add_u64(expectation.epoch.value());
    input.add_u64(tick);
    input.add_digest(cap.provenance.payload_digest.is_zero() ? capacity_snapshot_digest(cap)
                                                             : cap.provenance.payload_digest);
    input.add_digest(res.provenance.payload_digest.is_zero() ? reservation_snapshot_digest(res)
                                                             : res.provenance.payload_digest);
    input.add_digest(adm.provenance.payload_digest.is_zero() ? admission_snapshot_digest(adm)
                                                             : adm.provenance.payload_digest);
    r.authority.input_digest = input.digest();
  }

  // -------------------------------------------------------------- freshness --
  auto stale_or_future = [&](u64 observed, u64 budget, const char* what, ReasonCode stale_reason) -> bool {
    if (observed > tick) {
      r.outcome = Outcome::ConflictingInput;
      r.reason = ReasonCode::EvidenceFutureDated;
      r.detail = std::string(what) + " was observed after the evaluation tick";
      return true;
    }
    const u64 age = tick - observed;
    if (age > budget) {
      r.outcome = Outcome::Stale;
      r.reason = stale_reason;
      r.detail = std::string(what) + " is stale: age " + std::to_string(age) + " ticks exceeds the budget of " +
                 std::to_string(budget);
      return true;
    }
    return false;
  };
  {
    bool bad = false;
    if (stale_or_future(cap.observed_tick, policy.max_capacity_age_ticks, "capacity evidence",
                        ReasonCode::SnapshotStale)) {
      bad = true;
    } else if (stale_or_future(res.observed_tick, policy.max_reservation_age_ticks, "reservation evidence",
                               ReasonCode::SnapshotStale)) {
      bad = true;
    } else if (stale_or_future(adm.observed_tick, policy.max_admission_age_ticks, "admission evidence",
                               ReasonCode::SnapshotStale)) {
      bad = true;
    }
    if (bad) {
      return reject(r.outcome, r.reason, r.detail);
    }
  }

  // ----------------------------------------------------------------- bounds --
  if (cap.resources.size() > kHardMaxResourcesPerSnapshot) {
    return reject(Outcome::ConflictingInput, ReasonCode::SnapshotTooLarge,
                  "capacity snapshot exceeds the structural bound");
  }
  if (res.guarantees.size() > limits.max_guarantees_per_snapshot ||
      res.guarantees.size() > kHardMaxGuaranteesPerSnapshot) {
    return reject(Outcome::ConflictingInput, ReasonCode::SnapshotTooLarge,
                  "reservation snapshot exceeds the policy bound");
  }
  if (adm.records.size() > limits.max_demand_records_per_snapshot ||
      adm.records.size() > kHardMaxDemandRecordsPerSnapshot) {
    return reject(Outcome::ConflictingInput, ReasonCode::SnapshotTooLarge,
                  "admission snapshot exceeds the policy bound");
  }

  std::unordered_map<u64, const ResourceCapacity*> capacity_by_resource;
  capacity_by_resource.reserve(cap.resources.size() * 2 + 1);
  for (const auto& rc : cap.resources) {
    const auto inserted = capacity_by_resource.emplace(rc.resource.value(), &rc);
    if (!inserted.second) {
      return reject(Outcome::ConflictingInput, ReasonCode::SnapshotDuplicate,
                    "capacity snapshot repeats a resource identity");
    }
  }

  std::vector<ResourceAccum> resources;
  resources.reserve(domain.members.size());
  std::unordered_map<u64, std::size_t> index_by_resource;
  index_by_resource.reserve(domain.members.size() * 2 + 1);
  {
    std::vector<ResourceId> members = domain.members;
    std::sort(members.begin(), members.end());
    for (const auto& member : members) {
      const auto it = capacity_by_resource.find(member.value());
      if (it == capacity_by_resource.end()) {
        return reject(Outcome::Unknown, ReasonCode::ResourceMissingFromCapacitySnapshot,
                      "capacity evidence does not cover member resource " + member.to_string());
      }
      const ResourceCapacity& rc = *it->second;
      if (rc.pool != domain.pool) {
        return reject(Outcome::ConflictingInput, ReasonCode::ResourcePoolMismatch,
                      "member resource " + member.to_string() + " is reported in a different pool");
      }
      if (rc.health == ResourceHealth::Unknown) {
        return reject(Outcome::Unknown, ReasonCode::ResourceHealthUnknown,
                      "member resource " + member.to_string() + " has unknown health");
      }
      ResourceAccum ra;
      ra.capacity = &rc;
      switch (rc.health) {
        case ResourceHealth::Healthy:
          ra.effective_usable = rc.usable_capacity;
          break;
        case ResourceHealth::Degraded:
          if (!ppm_of(rc.usable_capacity, rc.degraded_capacity_ppm, ra.effective_usable)) {
            return reject(Outcome::Unknown, ReasonCode::ArithmeticOverflow,
                          "degraded capacity scaling overflowed");
          }
          acc.degraded_resources += 1;
          break;
        case ResourceHealth::Failed:
          ra.effective_usable = 0;
          acc.failed_resources += 1;
          break;
        case ResourceHealth::Unknown:
          ra.effective_usable = 0;
          break;
      }
      index_by_resource.emplace(member.value(), resources.size());
      resources.push_back(ra);
    }
  }

  {
    CanonicalHasher fold;
    fold.add_string("oversub.resourceset.v1");
    for (const auto& ra : resources) {
      fold.add_u64(ra.capacity->resource.value());
      fold.add_u64(ra.capacity->generation.value());
    }
    r.authority.resource_set_fold = fold.fold64();
  }

  // -------------------------------------------------------------- guarantees --
  bool closed = true;
  std::unordered_map<u64, std::unordered_map<u64, u64>> guarantees_by_class;
  {
    std::unordered_set<u64> reservation_ids;
    reservation_ids.reserve(res.guarantees.size() * 2 + 1);
    for (const auto& g : res.guarantees) {
      if (!reservation_ids.insert(g.reservation.value()).second) {
        return reject(Outcome::ConflictingInput, ReasonCode::GuaranteeDuplicate,
                      "reservation snapshot repeats reservation " + g.reservation.to_string());
      }
      const auto it = index_by_resource.find(g.resource.value());
      if (it == index_by_resource.end()) {
        acc.out_of_scope_guarantees = sat_add(acc.out_of_scope_guarantees, g.guaranteed_capacity);
        continue;
      }
      if (!g.protected_obligation || g.preemptible) {
        return reject(Outcome::ConflictingInput, ReasonCode::GuaranteeContradiction,
                      "guarantee " + g.reservation.to_string() + " is not a protected obligation");
      }
      if (g.pool != domain.pool) {
        return reject(Outcome::ConflictingInput, ReasonCode::ResourcePoolMismatch,
                      "guarantee " + g.reservation.to_string() + " is reported in a different pool");
      }
      if (index.service_class(g.service_class) == nullptr) {
        return reject(Outcome::ConflictingInput, ReasonCode::GuaranteeServiceClassUnknown,
                      "guarantee " + g.reservation.to_string() + " references an unknown service class");
      }
      ResourceAccum& ra = resources[it->second];
      if (!add_checked(ra.guarantees, g.guaranteed_capacity, ra.guarantees)) {
        return reject(Outcome::Unknown, ReasonCode::ReservationSumOverflow,
                      "guaranteed capacity sum overflowed");
      }
      u64& per_class = guarantees_by_class[g.resource.value()][g.service_class.value()];
      if (!add_checked(per_class, g.guaranteed_capacity, per_class)) {
        return reject(Outcome::Unknown, ReasonCode::ReservationSumOverflow,
                      "guaranteed capacity sum overflowed");
      }
    }
  }
  {
    u64 total = 0;
    for (const auto& ra : resources) {
      if (!add_checked(total, ra.guarantees, total)) {
        return reject(Outcome::Unknown, ReasonCode::ReservationSumOverflow,
                      "guaranteed capacity total overflowed");
      }
    }
    acc.guarantees_total = total;
  }

  // ---------------------------------------------------------------- demand --
  std::unordered_map<u64, u64> contingent_by_class;
  std::unordered_map<u64, std::unordered_map<u64, u64>> admitted_by_class;
  u64 class_ineligible_amount = 0;
  u64 class_ineligible_class = 0;
  {
    std::unordered_set<u64> admission_ids;
    admission_ids.reserve(adm.records.size() * 2 + 1);
    for (const auto& rec : adm.records) {
      if (!admission_ids.insert(rec.admission.value()).second) {
        return reject(Outcome::ConflictingInput, ReasonCode::AdmissionDuplicate,
                      "admission snapshot repeats admission " + rec.admission.to_string());
      }
      const auto it = index_by_resource.find(rec.resource.value());
      if (it == index_by_resource.end()) {
        acc.out_of_scope_demand = sat_add(acc.out_of_scope_demand, rec.committed_capacity);
        continue;
      }
      if (rec.pool != domain.pool) {
        return reject(Outcome::ConflictingInput, ReasonCode::ResourcePoolMismatch,
                      "admission " + rec.admission.to_string() + " is reported in a different pool");
      }
      const ServiceClassRef* cls = index.service_class(rec.service_class);
      if (cls == nullptr) {
        return reject(Outcome::ConflictingInput, ReasonCode::AdmissionServiceClassUnknown,
                      "admission " + rec.admission.to_string() + " references an unknown service class");
      }
      ResourceAccum& ra = resources[it->second];
      switch (rec.kind) {
        case DemandKind::Guaranteed: {
          if (!add_checked(acc.guaranteed_admitted, rec.committed_capacity, acc.guaranteed_admitted)) {
            return reject(Outcome::Unknown, ReasonCode::AdmissionSumOverflow,
                          "guaranteed admitted total overflowed");
          }
          u64& per_class = admitted_by_class[rec.resource.value()][rec.service_class.value()];
          if (!add_checked(per_class, rec.committed_capacity, per_class)) {
            return reject(Outcome::Unknown, ReasonCode::AdmissionSumOverflow,
                          "guaranteed admitted total overflowed");
          }
          break;
        }
        case DemandKind::Contingent:
        case DemandKind::Burst: {
          if (!cls->oversubscription_eligible) {
            class_ineligible_amount = sat_add(class_ineligible_amount,
                                              sat_add(rec.committed_capacity, rec.pending_capacity));
            class_ineligible_class = rec.service_class.value();
            break;
          }
          if (!add_checked(ra.contingent_committed, rec.committed_capacity, ra.contingent_committed)) {
            return reject(Outcome::Unknown, ReasonCode::AdmissionSumOverflow,
                          "contingent committed total overflowed");
          }
          if (!add_checked(acc.contingent_committed, rec.committed_capacity, acc.contingent_committed)) {
            return reject(Outcome::Unknown, ReasonCode::AdmissionSumOverflow,
                          "contingent committed total overflowed");
          }
          if (!add_checked(ra.contingent_pending, rec.pending_capacity, ra.contingent_pending)) {
            return reject(Outcome::Unknown, ReasonCode::AdmissionSumOverflow, "pending demand overflowed");
          }
          if (!add_checked(acc.contingent_pending, rec.pending_capacity, acc.contingent_pending)) {
            return reject(Outcome::Unknown, ReasonCode::AdmissionSumOverflow, "pending demand overflowed");
          }
          if (rec.kind == DemandKind::Burst) {
            if (!add_checked(acc.burst_committed, rec.committed_capacity, acc.burst_committed)) {
              return reject(Outcome::Unknown, ReasonCode::AdmissionSumOverflow, "burst committed overflowed");
            }
          }
          u64& per_class = contingent_by_class[rec.service_class.value()];
          if (!add_checked(per_class, rec.committed_capacity, per_class)) {
            return reject(Outcome::Unknown, ReasonCode::AdmissionSumOverflow,
                          "contingent committed total overflowed");
          }
          break;
        }
        case DemandKind::Unknown:
          return reject(Outcome::ConflictingInput, ReasonCode::AdmissionKindConflict,
                        "admission " + rec.admission.to_string() + " has no kind");
      }
    }
  }

  // Guaranteed-class admissions must be backed by reservations for the same
  // resource and service class. A mismatch is contradictory evidence, not an
  // oversubscription finding.
  u64 unmapped_guaranteed = 0;
  for (const auto& per_resource : admitted_by_class) {
    const auto res_it = guarantees_by_class.find(per_resource.first);
    for (const auto& per_class : per_resource.second) {
      u64 backed = 0;
      if (res_it != guarantees_by_class.end()) {
        const auto g_it = res_it->second.find(per_class.first);
        if (g_it != res_it->second.end()) backed = g_it->second;
      }
      if (per_class.second > backed) {
        unmapped_guaranteed = sat_add(unmapped_guaranteed, per_class.second - backed);
      }
    }
  }

  // ---------------------------------------------------------------- totals --
  for (auto& ra : resources) {
    acc.physical_capacity = sat_add(acc.physical_capacity, ra.capacity->physical_capacity);
    acc.usable_capacity = sat_add(acc.usable_capacity, ra.capacity->usable_capacity);
    if (!add_checked(acc.effective_usable_capacity, ra.effective_usable, acc.effective_usable_capacity)) {
      closed = false;
    }
    if (!add_checked(ra.committed, ra.guarantees, ra.contingent_committed)) closed = false;
  }
  if (acc.physical_capacity > limits.max_capacity_units || acc.usable_capacity > limits.max_capacity_units) {
    return reject(Outcome::ConflictingInput, ReasonCode::ResourceCapacityInvalid,
                  "domain capacity exceeds the policy capacity bound");
  }

  // -------------------------------------------------------------- boundary --
  const bool degraded = acc.degraded_resources > 0 || acc.failed_resources > 0;
  Ratio effective = domain.configured_ratio;
  Constraint ratio_constraint = Constraint::ConfiguredRatio;
  if (ratio_gt(effective, policy.policy_ratio_cap)) {
    effective = policy.policy_ratio_cap;
    ratio_constraint = Constraint::PolicyRatioCap;
  }
  if (degraded) {
    const Ratio degraded_cap = domain.has_degraded_ratio_cap ? domain.degraded_ratio_cap : Ratio{1, 1};
    if (ratio_lt(degraded_cap, effective)) {
      effective = degraded_cap;
      ratio_constraint = Constraint::DegradedRatioCap;
      acc.ratio_reduced_by_degradation = true;
    }
  }
  acc.effective_ratio = effective;
  acc.effective_ratio_ppm = ratio_ppm(effective);

  struct Candidate {
    Constraint constraint;
    u64 value;
  };
  std::vector<Candidate> candidates;
  u64 ratio_ceiling = 0;
  if (!mul_div_floor(acc.effective_usable_capacity, effective.num, effective.den, ratio_ceiling)) {
    closed = false;
    ratio_ceiling = 0;
  } else {
    candidates.push_back({ratio_constraint, ratio_ceiling});
  }
  const RiskBudgetPolicy& risk = policy.risk_budget;
  u64 risk_ceiling = 0;
  if (risk.has_absolute) {
    u64 value = 0;
    if (!add_checked(acc.effective_usable_capacity, risk.max_exposure_units, value)) {
      closed = false;
    } else {
      if (risk_ceiling == 0 || value < risk_ceiling) risk_ceiling = value;
      candidates.push_back({Constraint::RiskBudgetExposureAbsolute, value});
    }
  }
  if (risk.has_ppm) {
    u64 value = 0;
    if (!mul_div_floor(acc.effective_usable_capacity, kPpmScale + risk.max_exposure_ppm, kPpmScale, value)) {
      closed = false;
    } else {
      if (risk_ceiling == 0 || value < risk_ceiling) risk_ceiling = value;
      candidates.push_back({Constraint::RiskBudgetExposurePpm, value});
    }
  }

  u64 ceiling = ratio_ceiling;
  Constraint binding = ratio_constraint;
  for (const Candidate& candidate : candidates) {
    if (candidate.value < ceiling) {
      ceiling = candidate.value;
      binding = candidate.constraint;
    }
  }
  if (acc.effective_usable_capacity == 0) {
    binding = Constraint::NoCapacity;
    ceiling = 0;
  }
  acc.ceiling_total = ceiling;
  acc.risk_budget_ceiling = risk_ceiling != 0 ? risk_ceiling : ceiling;
  acc.binding_constraint = binding;

  u64 protected_required = 0;
  if (!add_checked(acc.guarantees_total, acc.emergency_reserve, protected_required)) closed = false;
  acc.protected_required = protected_required;
  u64 floor_required = 0;
  if (!add_checked(protected_required, acc.min_protected_headroom, floor_required)) closed = false;
  acc.floor_required = floor_required;

  if (ceiling >= protected_required) {
    acc.contingent_ceiling = ceiling - protected_required;
  } else {
    acc.contingent_ceiling = 0;
  }
  if (!add_checked(acc.guarantees_total, acc.contingent_committed, acc.committed_total)) closed = false;
  acc.overage = acc.committed_total > ceiling ? acc.committed_total - ceiling : 0;
  acc.contingent_free = acc.contingent_ceiling > acc.contingent_committed
                            ? acc.contingent_ceiling - acc.contingent_committed
                            : 0;
  acc.protected_headroom = acc.effective_usable_capacity > acc.protected_required
                               ? acc.effective_usable_capacity - acc.protected_required
                               : 0;
  acc.exposure = acc.committed_total > acc.effective_usable_capacity
                     ? acc.committed_total - acc.effective_usable_capacity
                     : 0;
  acc.risk_exposure_remaining = acc.risk_budget_ceiling > acc.committed_total
                                    ? acc.risk_budget_ceiling - acc.committed_total
                                    : 0;
  acc.risk_budget_exceeded = risk_ceiling != 0 && acc.committed_total > risk_ceiling;
  acc.used_ppm_of_ceiling = acc.contingent_ceiling == 0
                                ? (acc.contingent_committed == 0 ? 0 : kPpmScale)
                                : mul_div_floor_sat(acc.contingent_committed, kPpmScale,
                                                    acc.contingent_ceiling);
  acc.approaching_limit = acc.overage == 0 && acc.used_ppm_of_ceiling >= policy.thresholds.warn_used_ppm;

  // per-resource view: where deliberate oversubscription may occur.
  const std::size_t breakdown = limits.max_resource_breakdown;
  bool truncated = false;
  for (std::size_t i = 0; i < resources.size(); ++i) {
    ResourceAccum& ra = resources[i];
    u64 resource_ceiling = 0;
    if (!mul_div_floor(ra.effective_usable, effective.num, effective.den, resource_ceiling)) {
      closed = false;
    }
    ra.ceiling = resource_ceiling;
    ra.contingent_ceiling = resource_ceiling > ra.guarantees ? resource_ceiling - ra.guarantees : 0;
    ra.contingent_free =
        ra.contingent_ceiling > ra.contingent_committed ? ra.contingent_ceiling - ra.contingent_committed : 0;
    ra.overage = ra.committed > resource_ceiling ? ra.committed - resource_ceiling : 0;
    ra.protected_headroom = ra.effective_usable > ra.guarantees ? ra.effective_usable - ra.guarantees : 0;
    if (i < breakdown) {
      ResourceAccounting out;
      out.resource = ra.capacity->resource;
      out.pool = ra.capacity->pool;
      out.health = ra.capacity->health;
      out.physical_capacity = ra.capacity->physical_capacity;
      out.usable_capacity = ra.capacity->usable_capacity;
      out.effective_usable_capacity = ra.effective_usable;
      out.guarantees = ra.guarantees;
      out.contingent_committed = ra.contingent_committed;
      out.contingent_pending = ra.contingent_pending;
      out.ceiling = ra.ceiling;
      out.contingent_ceiling = ra.contingent_ceiling;
      out.contingent_free = ra.contingent_free;
      out.overage = ra.overage;
      out.protected_headroom = ra.protected_headroom;
      out.binding_constraint = ra.overage > 0 ? binding : Constraint::None;
      r.resources.push_back(out);
    } else {
      truncated = true;
    }
  }
  if (truncated) {
    note(r, limits, "resource breakdown truncated at the policy limit; domain totals are complete");
  }

  u64 resource_guarantee_violation = 0;
  u64 resource_guarantee_violation_id = 0;
  for (const auto& ra : resources) {
    if (ra.guarantees > ra.effective_usable) {
      resource_guarantee_violation = sat_add(resource_guarantee_violation, ra.guarantees - ra.effective_usable);
      resource_guarantee_violation_id = ra.capacity->resource.value();
    }
  }

  if (acc.degraded_resources > 0 || acc.failed_resources > 0) {
    note(r, limits,
         "degradation present: " + std::to_string(acc.degraded_resources) + " degraded, " +
             std::to_string(acc.failed_resources) + " failed resources; effective ratio " +
             ratio_to_string(acc.effective_ratio) + " (configured " + ratio_to_string(acc.configured_ratio) + ")");
  }
  if (!closed) {
    acc.arithmetic_closed = false;
    return reject(Outcome::Unknown, ReasonCode::ArithmeticOverflow,
                  "checked arithmetic could not close the domain boundary");
  }

  // -------------------------------------------------------------- outcome --
  auto finish = [&](Outcome outcome, ReasonCode reason, std::string detail) {
    r.outcome = outcome;
    r.reason = reason;
    r.detail = std::move(detail);
    r.fence_required = (outcome == Outcome::EmergencyReduction || outcome == Outcome::PolicyRejected ||
                        outcome == Outcome::ConflictingInput);

    if (acc.overage > 0) {
      CorrectiveIntent reduce =
          base_intent(domain, CorrectiveKind::ReduceContingentAdmissionBudget, CorrectiveTarget::Domain,
                      ReasonCode::AdmissionExceedsContingentCeiling,
                      "reduce the contingent admission budget by the overage");
      reduce.amount_units = acc.overage;
      add_intent(r, limits, std::move(reduce));
    }
    if (unmapped_guaranteed > 0) {
      add_intent(r, limits, base_intent(domain, CorrectiveKind::RevalidateEvidence, CorrectiveTarget::Domain,
                                        ReasonCode::AdmissionKindConflict,
                                        "guaranteed-class admissions exceed their reservations"));
    }
    if (class_ineligible_amount > 0) {
      CorrectiveIntent intent =
          base_intent(domain, CorrectiveKind::FenceOversubscribedAdmission, CorrectiveTarget::ServiceClass,
                      ReasonCode::PolicyServiceClassIneligibleDemand,
                      "service class is not eligible for deliberate oversubscription");
      intent.service_class = ServiceClassId(class_ineligible_class);
      intent.amount_units = class_ineligible_amount;
      add_intent(r, limits, std::move(intent));
    }
    if (resource_guarantee_violation > 0 || acc.guarantees_total > acc.effective_usable_capacity) {
      const u64 shortfall = acc.guarantees_total > acc.effective_usable_capacity
                                ? acc.guarantees_total - acc.effective_usable_capacity
                                : resource_guarantee_violation;
      CorrectiveIntent headroom =
          base_intent(domain, CorrectiveKind::IncreaseProtectedHeadroom, CorrectiveTarget::Domain,
                      ReasonCode::GuaranteeSumExceedsUsable,
                      "restore capacity so that protected guarantees are covered again");
      headroom.amount_units = shortfall;
      add_intent(r, limits, std::move(headroom));
      CorrectiveIntent recall =
          base_intent(domain, CorrectiveKind::RequestBorrowedCapacityRecall, CorrectiveTarget::Domain,
                      ReasonCode::GuaranteeSumExceedsUsable,
                      "request recall of capacity lent to other domains; the governor does not know where it "
                      "was lent, so this intent is a bounded request to the owning capacity system");
      recall.amount_units = shortfall;
      add_intent(r, limits, std::move(recall));
    }
    if (domain.burst_pool_enabled && acc.burst_committed > domain.burst_pool_cap_units) {
      CorrectiveIntent burst = base_intent(domain, CorrectiveKind::ReduceBurstPool, CorrectiveTarget::Domain,
                                           ReasonCode::BurstPoolExceeded, "burst pool exceeds its cap");
      burst.amount_units = acc.burst_committed - domain.burst_pool_cap_units;
      add_intent(r, limits, std::move(burst));
    }
    if (outcome == Outcome::EmergencyReduction) {
      CorrectiveIntent fence =
          base_intent(domain, CorrectiveKind::FenceOversubscribedAdmission, CorrectiveTarget::Domain, reason,
                      "fence new oversubscribed admission in this domain");
      fence.amount_units = acc.contingent_committed;
      add_intent(r, limits, std::move(fence));
    }
    if (outcome == Outcome::Stale || outcome == Outcome::Unknown) {
      add_intent(r, limits, base_intent(domain, CorrectiveKind::RevalidateEvidence, CorrectiveTarget::Domain,
                                        reason, "authority cannot be established from the current evidence"));
    }
    if (outcome == Outcome::ConflictingInput) {
      add_intent(r, limits,
                 base_intent(domain, CorrectiveKind::ResolveConflictingEvidence, CorrectiveTarget::Domain, reason,
                             "evidence is structurally contradictory"));
      add_intent(r, limits, base_intent(domain, CorrectiveKind::RevalidateEvidence, CorrectiveTarget::Domain,
                                        reason, "revalidate after the contradiction is resolved"));
    }
    if (outcome == Outcome::PolicyRejected) {
      add_intent(r, limits,
                 base_intent(domain, CorrectiveKind::FenceOversubscribedAdmission, CorrectiveTarget::Domain, reason,
                             "policy does not authorize the current oversubscription state"));
    }
    finalize_evaluation_result(r);
    return r;
  };

  if (unmapped_guaranteed > 0) {
    return finish(Outcome::ConflictingInput, ReasonCode::AdmissionKindConflict,
                  "guaranteed-class admissions exceed their reservations by " +
                      std::to_string(unmapped_guaranteed) + " units");
  }
  if (class_ineligible_amount > 0) {
    return finish(Outcome::PolicyRejected, ReasonCode::PolicyServiceClassIneligibleDemand,
                  "an ineligible service class holds " + std::to_string(class_ineligible_amount) +
                      " units of oversubscribed demand");
  }
  if (resource_guarantee_violation > 0) {
    return finish(Outcome::EmergencyReduction, ReasonCode::GuaranteeSumExceedsUsable,
                  "guarantees on resource " + std::to_string(resource_guarantee_violation_id) +
                      " exceed that resource's effective usable capacity");
  }
  if (acc.guarantees_total > acc.effective_usable_capacity) {
    return finish(Outcome::EmergencyReduction, ReasonCode::GuaranteeSumExceedsUsable,
                  "guaranteed obligations exceed effective usable capacity");
  }
  if (acc.protected_required > acc.effective_usable_capacity) {
    return finish(Outcome::EmergencyReduction, ReasonCode::GuaranteeSumExceedsUsable,
                  "protected obligations (guarantees plus emergency reserve) exceed effective usable capacity");
  }
  if (acc.floor_required > acc.effective_usable_capacity) {
    return finish(Outcome::PolicyRejected, ReasonCode::GuaranteeFloorExceedsCeiling,
                  "the policy minimum protected headroom cannot be satisfied by usable capacity");
  }
  if (acc.floor_required > acc.ceiling_total) {
    return finish(Outcome::PolicyRejected, ReasonCode::GuaranteeFloorExceedsCeiling,
                  "the legal commitment ceiling is below the protected guarantee floor");
  }
  if (acc.effective_usable_capacity == 0) {
    if (acc.committed_total > 0) {
      return finish(Outcome::EmergencyReduction, ReasonCode::AdmissionNoContingentCeiling,
                    "no usable capacity remains but commitments are outstanding");
    }
    return finish(Outcome::AtLimit, ReasonCode::ResourceFailed, "no usable capacity in this domain");
  }
  if (acc.overage > 0) {
    u64 overage_ppm = 0;
    if (acc.ceiling_total == 0) {
      overage_ppm = kPpmScale;
    } else if (!mul_div_ceil(acc.overage, kPpmScale, acc.ceiling_total, overage_ppm)) {
      overage_ppm = kPpmScale;
    }
    if (overage_ppm >= policy.thresholds.emergency_overage_ppm) {
      return finish(Outcome::EmergencyReduction, ReasonCode::AdmissionExceedsContingentCeiling,
                    "oversubscription exceeds the legal ceiling by " + std::to_string(overage_ppm) +
                        " ppm of the ceiling");
    }
    return finish(Outcome::ReductionRequired, ReasonCode::AdmissionExceedsContingentCeiling,
                  "oversubscription exceeds the legal ceiling by " + std::to_string(acc.overage) + " units");
  }
  if (domain.burst_pool_enabled && acc.burst_committed > domain.burst_pool_cap_units) {
    return finish(Outcome::ReductionRequired, ReasonCode::BurstPoolExceeded,
                  "burst pool commitment exceeds the configured burst pool cap");
  }
  for (const auto& per_class : contingent_by_class) {
    const ServiceClassRef* cls = index.service_class(ServiceClassId(per_class.first));
    if (cls == nullptr || !cls->has_contingent_share_cap) continue;
    u64 allowed = 0;
    if (!mul_div_floor(acc.contingent_ceiling, cls->contingent_share_cap_ppm, kPpmScale, allowed)) {
      return finish(Outcome::Unknown, ReasonCode::ArithmeticOverflow, "service class share overflowed");
    }
    if (per_class.second > allowed) {
      CorrectiveIntent intent =
          base_intent(domain, CorrectiveKind::ReduceContingentAdmissionBudget, CorrectiveTarget::ServiceClass,
                      ReasonCode::ServiceClassShareExceeded, "service class exceeds its contingent share");
      intent.service_class = ServiceClassId(per_class.first);
      intent.amount_units = per_class.second - allowed;
      add_intent(r, limits, std::move(intent));
      return finish(Outcome::ReductionRequired, ReasonCode::ServiceClassShareExceeded,
                    "a service class exceeds its contingent share of the ceiling");
    }
  }
  if (acc.contingent_free == 0) {
    return finish(Outcome::AtLimit, ReasonCode::None, "the domain is exactly at its legal ceiling");
  }
  if (acc.approaching_limit) {
    note(r, limits, "used " + std::to_string(acc.used_ppm_of_ceiling) + " ppm of the contingent ceiling");
  }
  return finish(Outcome::WithinPolicy, ReasonCode::None, "the domain is within its oversubscription policy");
}

Digest finalize_evaluation_result(EvaluationResult& result) {
  result.authority_valid = outcome_is_authoritative(result.outcome);
  result.new_oversubscription_authorized = outcome_allows_new_authority(result.outcome);
  result.authorized_increment_units =
      result.new_oversubscription_authorized ? result.accounting.contingent_free : 0;
  result.result_digest = evaluation_result_digest(result);
  return result.result_digest;
}

Digest evaluation_result_digest(const EvaluationResult& result) {
  CanonicalHasher h;
  h.add_string("oversub.result.v1");
  h.add_u32(static_cast<u32>(result.outcome));
  h.add_u32(static_cast<u32>(result.reason));
  const DomainAccounting& a = result.accounting;
  h.add_u64(a.domain.value());
  h.add_u64(a.pool.value());
  h.add_u64(a.physical_capacity);
  h.add_u64(a.usable_capacity);
  h.add_u64(a.effective_usable_capacity);
  h.add_u64(a.guarantees_total);
  h.add_u64(a.emergency_reserve);
  h.add_u64(a.min_protected_headroom);
  h.add_u64(a.protected_required);
  h.add_u64(a.floor_required);
  h.add_u64(a.configured_ratio.num);
  h.add_u64(a.configured_ratio.den);
  h.add_u64(a.effective_ratio.num);
  h.add_u64(a.effective_ratio.den);
  h.add_bool(a.ratio_reduced_by_degradation);
  h.add_u64(a.ceiling_total);
  h.add_u64(a.contingent_ceiling);
  h.add_u64(a.contingent_committed);
  h.add_u64(a.contingent_pending);
  h.add_u64(a.burst_committed);
  h.add_u64(a.guaranteed_admitted);
  h.add_u64(a.contingent_free);
  h.add_u64(a.committed_total);
  h.add_u64(a.overage);
  h.add_u64(a.protected_headroom);
  h.add_u64(a.used_ppm_of_ceiling);
  h.add_u64(a.exposure);
  h.add_u64(a.risk_budget_ceiling);
  h.add_u64(a.risk_exposure_remaining);
  h.add_bool(a.risk_budget_exceeded);
  h.add_u64(a.out_of_scope_guarantees);
  h.add_u64(a.out_of_scope_demand);
  h.add_u32(a.degraded_resources);
  h.add_u32(a.failed_resources);
  h.add_u32(static_cast<u32>(a.binding_constraint));
  h.add_bool(a.arithmetic_closed);
  h.add_u64(result.resources.size());
  for (const auto& res : result.resources) {
    h.add_u64(res.resource.value());
    h.add_u32(static_cast<u32>(res.health));
    h.add_u64(res.effective_usable_capacity);
    h.add_u64(res.guarantees);
    h.add_u64(res.contingent_committed);
    h.add_u64(res.ceiling);
    h.add_u64(res.contingent_ceiling);
    h.add_u64(res.contingent_free);
    h.add_u64(res.overage);
  }
  h.add_u64(result.intents.size());
  for (const auto& intent : result.intents) {
    h.add_u32(static_cast<u32>(intent.kind));
    h.add_u32(static_cast<u32>(intent.target));
    h.add_u64(intent.amount_units);
    h.add_u32(static_cast<u32>(intent.reason));
  }
  h.add_bool(result.authority_valid);
  h.add_bool(result.new_oversubscription_authorized);
  h.add_u64(result.authorized_increment_units);
  h.add_bool(result.fence_required);
  h.add_digest(result.authority.input_digest);
  return h.digest();
}

std::string explain_evaluation(const OversubscriptionPolicy& policy, const EvaluationResult& result,
                               const ExplainOptions& options) {
  const DomainAccounting& a = result.accounting;
  const u32 max_lines = std::min<u32>(options.max_lines == 0 ? 1 : options.max_lines, kHardMaxExplanationLines);
  const u32 max_bytes = std::min<u32>(options.max_bytes == 0 ? 1 : options.max_bytes, kHardMaxExplanationBytes);
  std::string out;
  u32 line_count = 0;
  // Bounded and linear: the line counter and byte budget are tracked as the
  // explanation is produced, never recomputed from the buffer.
  auto push = [&](const std::string& line) {
    if (out.size() >= max_bytes || line_count >= max_lines) return;
    const std::size_t room = max_bytes - out.size();
    const std::size_t wanted = line.size() + 1;
    if (wanted > room) {
      out.append(line, 0, room > 0 ? room - 1 : 0);
      out.push_back('\n');
      ++line_count;
      return;
    }
    out += line;
    out.push_back('\n');
    ++line_count;
  };

  push(std::string("outcome: ") + to_string(result.outcome) +
       (result.reason == ReasonCode::None ? std::string() : std::string(" (") + to_string(result.reason) + ")"));
  if (!result.detail.empty()) push("detail: " + result.detail);
  push("domain: " + a.domain.to_string() + " pool: " + a.pool.to_string());
  push("capacity: physical=" + std::to_string(a.physical_capacity) +
       " usable=" + std::to_string(a.usable_capacity) +
       " effective_usable=" + std::to_string(a.effective_usable_capacity));
  push("guarantees: total=" + std::to_string(a.guarantees_total) +
       " emergency_reserve=" + std::to_string(a.emergency_reserve) +
       " min_headroom=" + std::to_string(a.min_protected_headroom) +
       " protected_required=" + std::to_string(a.protected_required));
  push("contingent: committed=" + std::to_string(a.contingent_committed) +
       " pending=" + std::to_string(a.contingent_pending) + " burst=" + std::to_string(a.burst_committed) +
       " ceiling=" + std::to_string(a.contingent_ceiling) + " free=" + std::to_string(a.contingent_free));
  push("ratio: configured=" + ratio_to_string(a.configured_ratio) + " effective=" +
       ratio_to_string(a.effective_ratio) + " (" + std::to_string(a.effective_ratio_ppm) + " ppm)" +
       (a.ratio_reduced_by_degradation ? " reduced_by_degradation=true" : ""));
  push("commitment: ceiling_total=" + std::to_string(a.ceiling_total) +
       " committed_total=" + std::to_string(a.committed_total) + " overage=" + std::to_string(a.overage) +
       " headroom_protected=" + std::to_string(a.protected_headroom));
  push("risk_budget: ceiling=" + std::to_string(a.risk_budget_ceiling) +
       " remaining=" + std::to_string(a.risk_exposure_remaining) +
       " exposure=" + std::to_string(a.exposure) +
       (a.risk_budget_exceeded ? " exceeded=true" : " exceeded=false"));
  push(std::string("binding_constraint: ") + to_string(a.binding_constraint));
  push("health: degraded=" + std::to_string(a.degraded_resources) +
       " failed=" + std::to_string(a.failed_resources));
  push("authority: valid=" + std::string(result.authority_valid ? "true" : "false") +
       " new_oversubscription=" + (result.new_oversubscription_authorized ? "true" : "false") +
       " increment=" + std::to_string(result.authorized_increment_units) +
       " fence=" + (result.fence_required ? "true" : "false"));
  push("authority_vector: " + result.authority.to_string());
  if (options.include_resources && !result.resources.empty()) {
    push("resources:");
    for (const auto& res : result.resources) {
      push("  resource=" + res.resource.to_string() + " health=" + to_string(res.health) +
           " usable=" + std::to_string(res.effective_usable_capacity) +
           " guarantees=" + std::to_string(res.guarantees) +
           " contingent=" + std::to_string(res.contingent_committed) +
           " ceiling=" + std::to_string(res.ceiling) + " free=" + std::to_string(res.contingent_free) +
           " overage=" + std::to_string(res.overage));
    }
  }
  if (options.include_intents && !result.intents.empty()) {
    push("corrective_intent:");
    for (const auto& intent : result.intents) {
      std::string line = std::string("  ") + to_string(intent.kind) + " target=" + to_string(intent.target) +
                         " amount=" + std::to_string(intent.amount_units);
      if (intent.service_class.valid()) line += " service_class=" + intent.service_class.to_string();
      if (intent.resource.valid()) line += " resource=" + intent.resource.to_string();
      line += " reason=" + std::string(to_string(intent.reason));
      push(line);
      if (!intent.rationale.empty()) push("    rationale: " + intent.rationale);
    }
  }
  if (options.include_notes && !result.notes.empty()) {
    push("notes:");
    for (const auto& n : result.notes) push("  " + n);
  }
  push("result_digest: " + result.result_digest.to_hex());
  push("policy: id=" + policy.id.to_string() + " generation=" + policy.generation.to_string());
  return out;
}

}  // namespace oversub
