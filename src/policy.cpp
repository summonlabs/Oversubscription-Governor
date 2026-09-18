#include "oversub/policy.hpp"

#include <algorithm>

#include "oversub/engine.hpp"

namespace oversub {
namespace {

Status invalid(ReasonCode reason, std::string detail) {
  return Status::failure(StatusCode::InvalidArgument, reason, std::move(detail));
}

bool ratio_shape_bad(const Ratio& r) { return r.den == 0 || r.num == 0; }
bool ratio_out_of_bound(const Ratio& r) {
  return r.num > kHardMaxRatioTermBound || r.den > kHardMaxRatioTermBound || ratio_lt(r, Ratio{1, 1});
}

bool has_duplicate_values(std::vector<u64> values) {
  if (values.size() < 2) return false;
  std::sort(values.begin(), values.end());
  return std::adjacent_find(values.begin(), values.end()) != values.end();
}

std::vector<u64> sorted_member_values(const std::vector<ResourceId>& members) {
  std::vector<u64> values;
  values.reserve(members.size());
  for (const auto& m : members) values.push_back(m.value());
  std::sort(values.begin(), values.end());
  return values;
}

}  // namespace

Digest compute_policy_digest(OversubscriptionPolicy& policy) {
  CanonicalHasher h;
  h.add_string("oversub.policy.v1");
  h.add_u64(policy.id.value());
  h.add_u64(policy.generation.value());
  h.add_string(policy.name);
  h.add_u64(policy.policy_ratio_cap.num);
  h.add_u64(policy.policy_ratio_cap.den);

  std::vector<const DomainConfig*> domains;
  domains.reserve(policy.domains.size());
  for (const auto& d : policy.domains) domains.push_back(&d);
  std::sort(domains.begin(), domains.end(),
            [](const DomainConfig* a, const DomainConfig* b) { return a->id < b->id; });
  h.add_u64(domains.size());
  for (const DomainConfig* d : domains) {
    h.add_u64(d->id.value());
    h.add_u64(d->generation.value());
    h.add_string(d->name);
    h.add_u64(d->pool.value());
    const std::vector<u64> members = sorted_member_values(d->members);
    h.add_u64(members.size());
    for (const u64 m : members) h.add_u64(m);
    h.add_u64(d->configured_ratio.num);
    h.add_u64(d->configured_ratio.den);
    h.add_bool(d->has_degraded_ratio_cap);
    h.add_u64(d->degraded_ratio_cap.num);
    h.add_u64(d->degraded_ratio_cap.den);
    h.add_u64(d->emergency_reserve_units);
    h.add_u64(d->min_protected_headroom_units);
    h.add_bool(d->burst_pool_enabled);
    h.add_u64(d->burst_pool_cap_units);
  }

  std::vector<const ServiceClassRef*> classes;
  classes.reserve(policy.service_classes.size());
  for (const auto& c : policy.service_classes) classes.push_back(&c);
  std::sort(classes.begin(), classes.end(),
            [](const ServiceClassRef* a, const ServiceClassRef* b) { return a->id < b->id; });
  h.add_u64(classes.size());
  for (const ServiceClassRef* c : classes) {
    h.add_u64(c->id.value());
    h.add_string(c->name);
    h.add_u32(c->priority.value);
    h.add_bool(c->oversubscription_eligible);
    h.add_u32(c->risk_weight_ppm);
    h.add_bool(c->has_contingent_share_cap);
    h.add_u64(c->contingent_share_cap_ppm);
  }

  h.add_u64(policy.risk_budget.id.value());
  h.add_u64(policy.risk_budget.generation.value());
  h.add_bool(policy.risk_budget.has_absolute);
  h.add_u64(policy.risk_budget.max_exposure_units);
  h.add_bool(policy.risk_budget.has_ppm);
  h.add_u64(policy.risk_budget.max_exposure_ppm);

  h.add_u64(policy.thresholds.warn_used_ppm);
  h.add_u64(policy.thresholds.reduce_overage_ppm);
  h.add_u64(policy.thresholds.emergency_overage_ppm);

  h.add_u64(policy.hysteresis.escalation_confirmations);
  h.add_u64(policy.hysteresis.deescalation_confirmations);
  h.add_u64(policy.hysteresis.cooldown_ticks);
  h.add_u64(policy.hysteresis.deescalation_margin_ppm);

  h.add_u32(policy.limits.max_domains);
  h.add_u32(policy.limits.max_resources_per_domain);
  h.add_u32(policy.limits.max_service_classes);
  h.add_u64(policy.limits.max_guarantees_per_snapshot);
  h.add_u64(policy.limits.max_demand_records_per_snapshot);
  h.add_u32(policy.limits.max_corrective_intents);
  h.add_u32(policy.limits.max_explanation_lines);
  h.add_u32(policy.limits.max_explanation_bytes);
  h.add_u32(policy.limits.max_resource_breakdown);
  h.add_u64(policy.limits.max_capacity_units);

  h.add_u64(policy.max_capacity_age_ticks);
  h.add_u64(policy.max_reservation_age_ticks);
  h.add_u64(policy.max_admission_age_ticks);

  policy.digest = h.digest();
  return policy.digest;
}

Digest domain_config_digest(const DomainConfig& domain) {
  CanonicalHasher h;
  h.add_string("oversub.domain.v1");
  h.add_u64(domain.id.value());
  h.add_string(domain.name);
  h.add_u64(domain.pool.value());
  const std::vector<u64> members = sorted_member_values(domain.members);
  h.add_u64(members.size());
  for (const u64 m : members) h.add_u64(m);
  h.add_u64(domain.configured_ratio.num);
  h.add_u64(domain.configured_ratio.den);
  h.add_bool(domain.has_degraded_ratio_cap);
  h.add_u64(domain.degraded_ratio_cap.num);
  h.add_u64(domain.degraded_ratio_cap.den);
  h.add_u64(domain.emergency_reserve_units);
  h.add_u64(domain.min_protected_headroom_units);
  h.add_bool(domain.burst_pool_enabled);
  h.add_u64(domain.burst_pool_cap_units);
  return h.digest();
}

Digest policy_digest(const OversubscriptionPolicy& policy) {
  OversubscriptionPolicy copy = policy;
  return compute_policy_digest(copy);
}

Status validate_policy(const OversubscriptionPolicy& policy) {
  if (!policy.id.valid()) return invalid(ReasonCode::InvalidIdentity, "policy id unset");
  if (!policy.generation.valid()) return invalid(ReasonCode::InvalidIdentity, "policy generation unset");
  if (policy.name.size() > kHardMaxNameLength) return invalid(ReasonCode::InvalidField, "policy name too long");

  if (ratio_shape_bad(policy.policy_ratio_cap)) {
    return invalid(ReasonCode::PolicyRatioInvalid, "policy ratio cap has a zero term");
  }
  if (ratio_out_of_bound(policy.policy_ratio_cap)) {
    return invalid(ReasonCode::PolicyRatioOutOfBound, "policy ratio cap out of bound: " +
                                                          ratio_to_string(policy.policy_ratio_cap));
  }

  const PolicyLimits& lim = policy.limits;
  if (lim.max_domains == 0 || lim.max_domains > kHardMaxDomains) {
    return invalid(ReasonCode::PolicyLimitExceeded, "max_domains out of bound");
  }
  if (lim.max_resources_per_domain == 0 || lim.max_resources_per_domain > kHardMaxResourcesPerDomain) {
    return invalid(ReasonCode::PolicyLimitExceeded, "max_resources_per_domain out of bound");
  }
  if (lim.max_service_classes == 0 || lim.max_service_classes > kHardMaxServiceClasses) {
    return invalid(ReasonCode::PolicyLimitExceeded, "max_service_classes out of bound");
  }
  if (lim.max_guarantees_per_snapshot == 0 || lim.max_guarantees_per_snapshot > kHardMaxGuaranteesPerSnapshot) {
    return invalid(ReasonCode::PolicyLimitExceeded, "max_guarantees_per_snapshot out of bound");
  }
  if (lim.max_demand_records_per_snapshot == 0 ||
      lim.max_demand_records_per_snapshot > kHardMaxDemandRecordsPerSnapshot) {
    return invalid(ReasonCode::PolicyLimitExceeded, "max_demand_records_per_snapshot out of bound");
  }
  if (lim.max_corrective_intents == 0 || lim.max_corrective_intents > kHardMaxCorrectiveIntents) {
    return invalid(ReasonCode::PolicyLimitExceeded, "max_corrective_intents out of bound");
  }
  if (lim.max_explanation_lines == 0 || lim.max_explanation_lines > kHardMaxExplanationLines) {
    return invalid(ReasonCode::PolicyLimitExceeded, "max_explanation_lines out of bound");
  }
  if (lim.max_explanation_bytes == 0 || lim.max_explanation_bytes > kHardMaxExplanationBytes) {
    return invalid(ReasonCode::PolicyLimitExceeded, "max_explanation_bytes out of bound");
  }
  if (lim.max_resource_breakdown == 0 || lim.max_resource_breakdown > kHardMaxResourcesPerDomain) {
    return invalid(ReasonCode::PolicyLimitExceeded, "max_resource_breakdown out of bound");
  }
  if (lim.max_capacity_units == 0 || lim.max_capacity_units > kHardMaxCapacityUnits) {
    return invalid(ReasonCode::PolicyLimitExceeded, "max_capacity_units out of bound");
  }
  if (policy.domains.size() > lim.max_domains || policy.domains.size() > kHardMaxDomains) {
    return invalid(ReasonCode::PolicyLimitExceeded, "too many domains");
  }
  if (policy.service_classes.size() > lim.max_service_classes ||
      policy.service_classes.size() > kHardMaxServiceClasses) {
    return invalid(ReasonCode::PolicyLimitExceeded, "too many service classes");
  }

  {
    std::vector<u64> domain_ids;
    domain_ids.reserve(policy.domains.size());
    for (const auto& d : policy.domains) domain_ids.push_back(d.id.value());
    if (has_duplicate_values(std::move(domain_ids))) {
      return invalid(ReasonCode::PolicyDomainDuplicate, "duplicate domain id");
    }
  }

  for (const DomainConfig& d : policy.domains) {
    if (!d.id.valid() || !d.pool.valid() || !d.generation.valid()) {
      return invalid(ReasonCode::InvalidIdentity, "domain identity unset");
    }
    if (d.name.size() > kHardMaxNameLength) return invalid(ReasonCode::InvalidField, "domain name too long");
    if (d.members.empty()) return invalid(ReasonCode::PolicyDomainEmpty, "domain has no member resources");
    if (d.members.size() > lim.max_resources_per_domain ||
        d.members.size() > kHardMaxResourcesPerDomain) {
      return invalid(ReasonCode::PolicyDomainTooLarge, "domain has too many member resources");
    }
    for (const auto& member : d.members) {
      if (!member.valid()) return invalid(ReasonCode::InvalidIdentity, "member resource identity unset");
    }
    if (has_duplicate_values(sorted_member_values(d.members))) {
      return invalid(ReasonCode::PolicyDomainMemberDuplicate, "duplicate member resource");
    }
    if (ratio_shape_bad(d.configured_ratio)) {
      return invalid(ReasonCode::PolicyRatioInvalid, "domain configured ratio has a zero term");
    }
    if (ratio_out_of_bound(d.configured_ratio)) {
      return invalid(ReasonCode::PolicyRatioOutOfBound,
                     "domain configured ratio out of bound: " + ratio_to_string(d.configured_ratio));
    }
    if (ratio_gt(d.configured_ratio, policy.policy_ratio_cap)) {
      return invalid(ReasonCode::PolicyRatioOutOfBound, "domain configured ratio exceeds the policy cap");
    }
    if (d.has_degraded_ratio_cap) {
      if (ratio_shape_bad(d.degraded_ratio_cap)) {
        return invalid(ReasonCode::PolicyRatioInvalid, "degraded ratio cap has a zero term");
      }
      if (ratio_out_of_bound(d.degraded_ratio_cap)) {
        return invalid(ReasonCode::PolicyRatioOutOfBound,
                       "degraded ratio cap out of bound: " + ratio_to_string(d.degraded_ratio_cap));
      }
      if (ratio_gt(d.degraded_ratio_cap, policy.policy_ratio_cap)) {
        return invalid(ReasonCode::PolicyRatioOutOfBound, "degraded ratio cap exceeds the policy cap");
      }
      if (ratio_gt(d.degraded_ratio_cap, d.configured_ratio)) {
        return invalid(ReasonCode::PolicyRatioOutOfBound,
                       "degraded ratio cap exceeds the configured ratio (degradation must not raise the boundary)");
      }
    }
    u64 reserve_sum = 0;
    if (!add_checked(d.emergency_reserve_units, d.min_protected_headroom_units, reserve_sum)) {
      return invalid(ReasonCode::PolicyReserveInvalid, "emergency reserve plus minimum headroom overflows");
    }
    if (d.emergency_reserve_units > lim.max_capacity_units ||
        d.min_protected_headroom_units > lim.max_capacity_units) {
      return invalid(ReasonCode::PolicyReserveInvalid, "reserve or headroom exceeds the capacity bound");
    }
    if (d.burst_pool_enabled) {
      if (d.burst_pool_cap_units == 0) {
        return invalid(ReasonCode::PolicyReserveInvalid, "burst pool enabled with a zero cap");
      }
      if (d.burst_pool_cap_units > lim.max_capacity_units) {
        return invalid(ReasonCode::PolicyReserveInvalid, "burst pool cap exceeds the capacity bound");
      }
    } else if (d.burst_pool_cap_units != 0) {
      return invalid(ReasonCode::PolicyReserveInvalid, "burst pool cap set while the burst pool is disabled");
    }
  }

  {
    std::vector<u64> class_ids;
    class_ids.reserve(policy.service_classes.size());
    for (const auto& c : policy.service_classes) class_ids.push_back(c.id.value());
    if (has_duplicate_values(std::move(class_ids))) {
      return invalid(ReasonCode::PolicyServiceClassDuplicate, "duplicate service class id");
    }
  }

  for (const ServiceClassRef& c : policy.service_classes) {
    if (!c.id.valid()) return invalid(ReasonCode::InvalidIdentity, "service class identity unset");
    if (c.name.size() > kHardMaxNameLength) return invalid(ReasonCode::InvalidField, "service class name too long");
    if (c.risk_weight_ppm > kPpmScale) {
      return invalid(ReasonCode::InvalidField, "service class risk weight exceeds 1e6 ppm");
    }
    if (c.has_contingent_share_cap && c.contingent_share_cap_ppm > kPpmScale) {
      return invalid(ReasonCode::InvalidField, "service class contingent share cap exceeds 1e6 ppm");
    }
  }

  if (!policy.risk_budget.id.valid() || !policy.risk_budget.generation.valid()) {
    return invalid(ReasonCode::PolicyRiskBudgetInvalid, "risk budget identity or generation unset");
  }
  if (!policy.risk_budget.has_absolute && !policy.risk_budget.has_ppm) {
    return invalid(ReasonCode::PolicyRiskBudgetInvalid, "risk budget has neither an absolute nor a ppm ceiling");
  }
  if (policy.risk_budget.has_ppm && policy.risk_budget.max_exposure_ppm > kHardMaxExposurePpm) {
    return invalid(ReasonCode::PolicyRiskBudgetInvalid, "risk budget ppm ceiling out of bound");
  }
  if (policy.risk_budget.has_absolute && policy.risk_budget.max_exposure_units > lim.max_capacity_units) {
    return invalid(ReasonCode::PolicyRiskBudgetInvalid, "risk budget absolute ceiling out of bound");
  }

  const OversubscriptionThresholds& th = policy.thresholds;
  if (th.warn_used_ppm == 0 || th.warn_used_ppm > kPpmScale) {
    return invalid(ReasonCode::InvalidField, "warn threshold out of bound");
  }
  if (th.reduce_overage_ppm == 0 || th.reduce_overage_ppm > kPpmScale) {
    return invalid(ReasonCode::PolicyThresholdsInverted, "reduce threshold out of bound");
  }
  if (th.emergency_overage_ppm < th.reduce_overage_ppm || th.emergency_overage_ppm > kPpmScale) {
    return invalid(ReasonCode::PolicyThresholdsInverted, "emergency threshold below the reduce threshold");
  }

  const HysteresisPolicy& hy = policy.hysteresis;
  if (hy.escalation_confirmations == 0 || hy.escalation_confirmations > kHardMaxConfirmations) {
    return invalid(ReasonCode::InvalidField, "escalation confirmations out of bound");
  }
  if (hy.deescalation_confirmations == 0 || hy.deescalation_confirmations > kHardMaxConfirmations) {
    return invalid(ReasonCode::InvalidField, "deescalation confirmations out of bound");
  }
  if (hy.cooldown_ticks > kHardMaxCooldownTicks) {
    return invalid(ReasonCode::InvalidField, "cooldown ticks out of bound");
  }
  if (hy.deescalation_margin_ppm > kPpmScale) {
    return invalid(ReasonCode::InvalidField, "deescalation margin out of bound");
  }

  const u64 ages[3] = {policy.max_capacity_age_ticks, policy.max_reservation_age_ticks,
                       policy.max_admission_age_ticks};
  for (const u64 age : ages) {
    if (age == 0) return invalid(ReasonCode::PolicyFreshnessUnbounded, "evidence freshness budget of zero ticks");
    if (age > kHardMaxFreshnessTicks) {
      return invalid(ReasonCode::PolicyLimitExceeded, "evidence freshness budget out of bound");
    }
  }

  if (!policy.digest.is_zero()) {
    const Digest recomputed = policy_digest(policy);
    if (recomputed != policy.digest) {
      return Status::failure(StatusCode::IntegrityFailure, ReasonCode::PolicyDigestMismatch,
                             "policy digest does not match its contents");
    }
  }
  return Status::success();
}

const DomainConfig* find_domain(const OversubscriptionPolicy& policy, OversubscriptionDomainId id) {
  for (const auto& d : policy.domains) {
    if (d.id == id) return &d;
  }
  return nullptr;
}

const ServiceClassRef* find_service_class(const OversubscriptionPolicy& policy, ServiceClassId id) {
  for (const auto& c : policy.service_classes) {
    if (c.id == id) return &c;
  }
  return nullptr;
}

void PolicyIndex::rebuild(const OversubscriptionPolicy& policy) {
  domains_.clear();
  classes_.clear();
  domains_.reserve(policy.domains.size());
  for (const auto& d : policy.domains) domains_.push_back(&d);
  std::sort(domains_.begin(), domains_.end(),
            [](const DomainConfig* a, const DomainConfig* b) { return a->id < b->id; });
  classes_.reserve(policy.service_classes.size());
  for (const auto& c : policy.service_classes) classes_.push_back(&c);
  std::sort(classes_.begin(), classes_.end(),
            [](const ServiceClassRef* a, const ServiceClassRef* b) { return a->id < b->id; });
}

const DomainConfig* PolicyIndex::domain(OversubscriptionDomainId id) const {
  const auto it = std::lower_bound(domains_.begin(), domains_.end(), id,
                                   [](const DomainConfig* d, OversubscriptionDomainId v) { return d->id < v; });
  if (it == domains_.end() || !((*it)->id == id)) return nullptr;
  return *it;
}

const ServiceClassRef* PolicyIndex::service_class(ServiceClassId id) const {
  const auto it = std::lower_bound(classes_.begin(), classes_.end(), id,
                                   [](const ServiceClassRef* c, ServiceClassId v) { return c->id < v; });
  if (it == classes_.end() || !((*it)->id == id)) return nullptr;
  return *it;
}

}  // namespace oversub
