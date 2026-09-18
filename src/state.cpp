#include "oversub/state.hpp"

#include <algorithm>
#include <unordered_map>

#include "oversub/bytes.hpp"

namespace oversub {
namespace {

Status corrupt(ReasonCode reason, std::string detail) {
  return Status::failure(StatusCode::CorruptData, reason, std::move(detail));
}

void write_ratio(ByteWriter& w, const Ratio& r) {
  w.u64(r.num);
  w.u64(r.den);
}

bool read_ratio(ByteReader& r, Ratio& out) {
  return r.u64(out.num) && r.u64(out.den);
}

void write_domain(ByteWriter& w, const DomainConfig& d) {
  w.u64(d.id.value());
  w.u64(d.generation.value());
  w.string(d.name, kHardMaxNameLength);
  w.u64(d.pool.value());
  w.u64(d.members.size());
  for (const auto& m : d.members) w.u64(m.value());
  write_ratio(w, d.configured_ratio);
  w.boolean(d.has_degraded_ratio_cap);
  write_ratio(w, d.degraded_ratio_cap);
  w.u64(d.emergency_reserve_units);
  w.u64(d.min_protected_headroom_units);
  w.boolean(d.burst_pool_enabled);
  w.u64(d.burst_pool_cap_units);
}

bool read_domain(ByteReader& r, DomainConfig& d) {
  u64 id = 0;
  u64 generation = 0;
  u64 pool = 0;
  u64 members = 0;
  if (!r.u64(id) || !r.u64(generation)) return false;
  if (!r.string(d.name, kHardMaxNameLength)) return false;
  if (!r.u64(pool)) return false;
  if (!r.u64(members)) return false;
  if (members > kHardMaxResourcesPerDomain) return false;
  d.id = OversubscriptionDomainId(id);
  d.generation = DomainGeneration(generation);
  d.pool = PoolId(pool);
  d.members.clear();
  d.members.reserve(static_cast<std::size_t>(members));
  for (u64 i = 0; i < members; ++i) {
    u64 value = 0;
    if (!r.u64(value)) return false;
    d.members.push_back(ResourceId(value));
  }
  if (!read_ratio(r, d.configured_ratio)) return false;
  if (!r.boolean(d.has_degraded_ratio_cap)) return false;
  if (!read_ratio(r, d.degraded_ratio_cap)) return false;
  return r.u64(d.emergency_reserve_units) && r.u64(d.min_protected_headroom_units) &&
         r.boolean(d.burst_pool_enabled) && r.u64(d.burst_pool_cap_units);
}

void write_policy(ByteWriter& w, const OversubscriptionPolicy& p) {
  w.u64(p.id.value());
  w.u64(p.generation.value());
  w.string(p.name, kHardMaxNameLength);
  write_ratio(w, p.policy_ratio_cap);
  w.u64(p.domains.size());
  for (const auto& d : p.domains) write_domain(w, d);
  w.u64(p.service_classes.size());
  for (const auto& c : p.service_classes) {
    w.u64(c.id.value());
    w.string(c.name, kHardMaxNameLength);
    w.u32(c.priority.value);
    w.boolean(c.oversubscription_eligible);
    w.u32(c.risk_weight_ppm);
    w.boolean(c.has_contingent_share_cap);
    w.u64(c.contingent_share_cap_ppm);
  }
  w.u64(p.risk_budget.id.value());
  w.u64(p.risk_budget.generation.value());
  w.boolean(p.risk_budget.has_absolute);
  w.u64(p.risk_budget.max_exposure_units);
  w.boolean(p.risk_budget.has_ppm);
  w.u64(p.risk_budget.max_exposure_ppm);
  w.u64(p.thresholds.warn_used_ppm);
  w.u64(p.thresholds.reduce_overage_ppm);
  w.u64(p.thresholds.emergency_overage_ppm);
  w.u64(p.hysteresis.escalation_confirmations);
  w.u64(p.hysteresis.deescalation_confirmations);
  w.u64(p.hysteresis.cooldown_ticks);
  w.u64(p.hysteresis.deescalation_margin_ppm);
  w.u32(p.limits.max_domains);
  w.u32(p.limits.max_resources_per_domain);
  w.u32(p.limits.max_service_classes);
  w.u64(p.limits.max_guarantees_per_snapshot);
  w.u64(p.limits.max_demand_records_per_snapshot);
  w.u32(p.limits.max_corrective_intents);
  w.u32(p.limits.max_explanation_lines);
  w.u32(p.limits.max_explanation_bytes);
  w.u32(p.limits.max_resource_breakdown);
  w.u64(p.limits.max_capacity_units);
  w.u64(p.max_capacity_age_ticks);
  w.u64(p.max_reservation_age_ticks);
  w.u64(p.max_admission_age_ticks);
  w.digest(p.digest);
}

bool read_policy(ByteReader& r, OversubscriptionPolicy& p) {
  u64 id = 0;
  u64 generation = 0;
  u64 domains = 0;
  u64 classes = 0;
  if (!r.u64(id) || !r.u64(generation)) return false;
  if (!r.string(p.name, kHardMaxNameLength)) return false;
  if (!read_ratio(r, p.policy_ratio_cap)) return false;
  if (!r.u64(domains)) return false;
  if (domains > kHardMaxDomains) return false;
  p.id = OversubscriptionPolicyId(id);
  p.generation = PolicyGeneration(generation);
  p.domains.clear();
  p.domains.reserve(static_cast<std::size_t>(domains));
  for (u64 i = 0; i < domains; ++i) {
    DomainConfig d;
    if (!read_domain(r, d)) return false;
    p.domains.push_back(std::move(d));
  }
  if (!r.u64(classes)) return false;
  if (classes > kHardMaxServiceClasses) return false;
  p.service_classes.clear();
  p.service_classes.reserve(static_cast<std::size_t>(classes));
  for (u64 i = 0; i < classes; ++i) {
    ServiceClassRef c;
    u64 class_id = 0;
    if (!r.u64(class_id)) return false;
    if (!r.string(c.name, kHardMaxNameLength)) return false;
    if (!r.u32(c.priority.value)) return false;
    if (!r.boolean(c.oversubscription_eligible)) return false;
    if (!r.u32(c.risk_weight_ppm)) return false;
    if (!r.boolean(c.has_contingent_share_cap)) return false;
    if (!r.u64(c.contingent_share_cap_ppm)) return false;
    c.id = ServiceClassId(class_id);
    p.service_classes.push_back(std::move(c));
  }
  u64 risk_id = 0;
  u64 risk_generation = 0;
  if (!r.u64(risk_id) || !r.u64(risk_generation)) return false;
  p.risk_budget.id = RiskBudgetId(risk_id);
  p.risk_budget.generation = RiskBudgetGeneration(risk_generation);
  if (!r.boolean(p.risk_budget.has_absolute)) return false;
  if (!r.u64(p.risk_budget.max_exposure_units)) return false;
  if (!r.boolean(p.risk_budget.has_ppm)) return false;
  if (!r.u64(p.risk_budget.max_exposure_ppm)) return false;
  if (!r.u64(p.thresholds.warn_used_ppm) || !r.u64(p.thresholds.reduce_overage_ppm) ||
      !r.u64(p.thresholds.emergency_overage_ppm)) {
    return false;
  }
  if (!r.u64(p.hysteresis.escalation_confirmations) || !r.u64(p.hysteresis.deescalation_confirmations) ||
      !r.u64(p.hysteresis.cooldown_ticks) || !r.u64(p.hysteresis.deescalation_margin_ppm)) {
    return false;
  }
  if (!r.u32(p.limits.max_domains) || !r.u32(p.limits.max_resources_per_domain) ||
      !r.u32(p.limits.max_service_classes) || !r.u64(p.limits.max_guarantees_per_snapshot) ||
      !r.u64(p.limits.max_demand_records_per_snapshot) || !r.u32(p.limits.max_corrective_intents) ||
      !r.u32(p.limits.max_explanation_lines) || !r.u32(p.limits.max_explanation_bytes) ||
      !r.u32(p.limits.max_resource_breakdown) || !r.u64(p.limits.max_capacity_units)) {
    return false;
  }
  if (!r.u64(p.max_capacity_age_ticks) || !r.u64(p.max_reservation_age_ticks) ||
      !r.u64(p.max_admission_age_ticks)) {
    return false;
  }
  return r.digest(p.digest);
}

}  // namespace

Status serialize_state(const DurableState& state, std::vector<u8>& out) {
  ByteWriter w;
  w.u32(state.schema_version);
  w.u64(state.revision);
  w.u64(state.incarnation.value());
  w.u64(state.epoch.value());
  w.u64(state.policy_generation.value());
  w.digest(state.policy_digest);
  write_policy(w, state.policy);
  w.u64(state.publishers.size());
  for (const auto& p : state.publishers) {
    w.u64(p.publisher.value());
    w.u64(p.incarnation.value());
    w.u64(p.epoch.value());
    w.u64(p.last_sequence);
    w.boolean(p.fenced);
    w.u32(static_cast<u32>(p.fence_reason));
    w.u64(p.registered_tick);
  }
  w.u64(state.domains.size());
  for (const auto& d : state.domains) {
    w.u64(d.domain.value());
    w.u64(d.generation.value());
    w.u32(static_cast<u32>(d.sticky_outcome));
    w.u32(d.confirmations);
    w.u64(d.last_escalation_tick);
    w.boolean(d.fenced);
    w.u32(static_cast<u32>(d.fence_reason));
    w.u64(d.fence_epoch.value());
    w.u64(d.last_decision.value());
    w.digest(d.last_decision_digest);
    w.u32(static_cast<u32>(d.last_raw_outcome));
  }
  w.u64(state.evidence.size());
  for (const auto& e : state.evidence) {
    w.u64(e.domain.value());
    w.u32(static_cast<u32>(e.kind));
    w.u64(e.generation);
    w.u64(e.evidence_id.value());
    w.u64(e.publisher.value());
    w.u64(e.incarnation.value());
    w.u64(e.sequence);
    w.u64(e.observed_tick);
    w.digest(e.payload_digest);
    w.boolean(e.revalidation_required);
  }
  w.u64(state.audit.size());
  for (const auto& a : state.audit) {
    std::vector<u8> record;
    Status status = serialize_audit_record(a, record);
    if (!status.ok()) return status;
    w.u64(record.size());
    w.raw(record.data(), record.size());
  }
  w.u64(state.decisions_issued);
  w.u64(state.tick);
  w.u64(state.journal_records);
  out = w.data();
  return Status::success();
}

Status deserialize_state(const std::vector<u8>& buffer, DurableState& out) {
  ByteReader r(buffer);
  DurableState state;
  u32 schema = 0;
  u64 publishers = 0;
  u64 domains = 0;
  u64 slots = 0;
  u64 audit = 0;
  if (!r.u32(schema)) return corrupt(ReasonCode::PersistenceCorrupt, "state header truncated");
  if (schema != kStateSchemaVersion) {
    return Status::failure(StatusCode::Unsupported, ReasonCode::PersistenceVersionUnsupported,
                           "unsupported state schema version " + std::to_string(schema));
  }
  state.schema_version = schema;
  u64 incarnation = 0;
  u64 epoch = 0;
  u64 policy_generation = 0;
  if (!r.u64(state.revision) || !r.u64(incarnation) || !r.u64(epoch) || !r.u64(policy_generation)) {
    return corrupt(ReasonCode::PersistenceCorrupt, "state header truncated");
  }
  state.incarnation = IncarnationId(incarnation);
  state.epoch = FabricEpoch(epoch);
  state.policy_generation = PolicyGeneration(policy_generation);
  if (!r.digest(state.policy_digest)) return corrupt(ReasonCode::PersistenceCorrupt, "policy digest truncated");
  if (!read_policy(r, state.policy)) return corrupt(ReasonCode::PersistenceCorrupt, "policy payload malformed");
  if (!r.u64(publishers)) return corrupt(ReasonCode::PersistenceCorrupt, "publisher count truncated");
  if (publishers > kMaxPublishers) return corrupt(ReasonCode::PersistenceTooLarge, "publisher table too large");
  state.publishers.reserve(static_cast<std::size_t>(publishers));
  for (u64 i = 0; i < publishers; ++i) {
    PublisherRegistration p;
    u64 publisher = 0;
    u64 pincarnation = 0;
    u64 pepoch = 0;
    u32 reason = 0;
    if (!r.u64(publisher) || !r.u64(pincarnation) || !r.u64(pepoch) || !r.u64(p.last_sequence) ||
        !r.boolean(p.fenced) || !r.u32(reason) || !r.u64(p.registered_tick)) {
      return corrupt(ReasonCode::PersistenceCorrupt, "publisher record truncated");
    }
    p.publisher = PublisherId(publisher);
    p.incarnation = IncarnationId(pincarnation);
    p.epoch = FabricEpoch(pepoch);
    p.fence_reason = static_cast<ReasonCode>(reason);
    state.publishers.push_back(p);
  }
  if (!r.u64(domains)) return corrupt(ReasonCode::PersistenceCorrupt, "domain count truncated");
  if (domains > kMaxDomainsState) return corrupt(ReasonCode::PersistenceTooLarge, "domain table too large");
  state.domains.reserve(static_cast<std::size_t>(domains));
  for (u64 i = 0; i < domains; ++i) {
    DomainRuntimeState d;
    u64 domain = 0;
    u64 generation = 0;
    u32 sticky = 0;
    u32 fence_reason = 0;
    u64 fence_epoch = 0;
    u64 last_decision = 0;
    u32 last_raw = 0;
    if (!r.u64(domain) || !r.u64(generation) || !r.u32(sticky) || !r.u32(d.confirmations) ||
        !r.u64(d.last_escalation_tick) || !r.boolean(d.fenced) || !r.u32(fence_reason) ||
        !r.u64(fence_epoch) || !r.u64(last_decision) || !r.digest(d.last_decision_digest) || !r.u32(last_raw)) {
      return corrupt(ReasonCode::PersistenceCorrupt, "domain runtime record truncated");
    }
    d.domain = OversubscriptionDomainId(domain);
    d.generation = DomainGeneration(generation);
    d.sticky_outcome = static_cast<Outcome>(sticky);
    d.fence_reason = static_cast<ReasonCode>(fence_reason);
    d.fence_epoch = FabricEpoch(fence_epoch);
    d.last_decision = DecisionId(last_decision);
    d.last_raw_outcome = static_cast<Outcome>(last_raw);
    state.domains.push_back(d);
  }
  if (!r.u64(slots)) return corrupt(ReasonCode::PersistenceCorrupt, "evidence slot count truncated");
  if (slots > kMaxEvidenceSlots) return corrupt(ReasonCode::PersistenceTooLarge, "evidence table too large");
  state.evidence.reserve(static_cast<std::size_t>(slots));
  for (u64 i = 0; i < slots; ++i) {
    EvidenceSlotState e;
    u64 domain = 0;
    u32 kind = 0;
    u64 evidence_id = 0;
    u64 publisher = 0;
    u64 slot_incarnation = 0;
    if (!r.u64(domain) || !r.u32(kind) || !r.u64(e.generation) || !r.u64(evidence_id) || !r.u64(publisher) ||
        !r.u64(slot_incarnation) || !r.u64(e.sequence) || !r.u64(e.observed_tick) || !r.digest(e.payload_digest) ||
        !r.boolean(e.revalidation_required)) {
      return corrupt(ReasonCode::PersistenceCorrupt, "evidence slot truncated");
    }
    e.domain = OversubscriptionDomainId(domain);
    e.kind = static_cast<EvidenceKind>(kind);
    e.evidence_id = EvidenceId(evidence_id);
    e.publisher = PublisherId(publisher);
    e.incarnation = IncarnationId(slot_incarnation);
    state.evidence.push_back(e);
  }
  if (!r.u64(audit)) return corrupt(ReasonCode::PersistenceCorrupt, "audit count truncated");
  if (audit > kMaxAuditRecords) return corrupt(ReasonCode::PersistenceTooLarge, "audit table too large");
  state.audit.reserve(static_cast<std::size_t>(audit));
  for (u64 i = 0; i < audit; ++i) {
    u64 size = 0;
    if (!r.u64(size)) return corrupt(ReasonCode::PersistenceCorrupt, "audit record size truncated");
    if (size > r.left()) return corrupt(ReasonCode::PersistenceCorrupt, "audit record overruns the payload");
    std::vector<u8> record(size);
    if (size > 0 && !r.raw(record.data(), static_cast<std::size_t>(size))) {
      return corrupt(ReasonCode::PersistenceCorrupt, "audit record truncated");
    }
    AuditRecord decoded;
    Status status = deserialize_audit_record(record, decoded);
    if (!status.ok()) return status;
    state.audit.push_back(std::move(decoded));
  }
  if (!r.u64(state.decisions_issued) || !r.u64(state.tick) || !r.u64(state.journal_records)) {
    return corrupt(ReasonCode::PersistenceCorrupt, "state trailer truncated");
  }
  if (!r.at_end()) return corrupt(ReasonCode::PersistenceCorrupt, "trailing bytes after the state payload");
  out = std::move(state);
  return Status::success();
}

Status serialize_audit_record(const AuditRecord& record, std::vector<u8>& out) {
  ByteWriter w;
  w.u64(record.id.value());
  w.u64(record.tick);
  w.u64(record.wall_millis);
  w.u64(record.incarnation.value());
  w.u64(record.epoch.value());
  w.u64(record.domain.value());
  w.u64(record.domain_generation.value());
  w.u64(record.policy_generation.value());
  w.u32(static_cast<u32>(record.raw_outcome));
  w.u32(static_cast<u32>(record.effective_outcome));
  w.u32(static_cast<u32>(record.reason));
  w.u32(static_cast<u32>(record.binding_constraint));
  w.u64(record.decision.value());
  w.digest(record.decision_digest);
  w.digest(record.input_digest);
  w.boolean(record.authorized);
  w.u64(record.authorized_increment);
  w.boolean(record.fenced);
  w.u32(record.confirmations);
  w.u64(record.last_escalation_tick);
  w.u64(record.ceiling_total);
  w.u64(record.committed_total);
  w.u64(record.overage);
  if (!w.string(record.summary, kMaxAuditSummary)) {
    return Status::failure(StatusCode::OutOfRange, ReasonCode::LimitExceeded, "audit summary too long");
  }
  out = w.data();
  return Status::success();
}

Status deserialize_audit_record(const std::vector<u8>& buffer, AuditRecord& out) {
  ByteReader r(buffer);
  AuditRecord record;
  u64 id = 0;
  u64 incarnation = 0;
  u64 epoch = 0;
  u64 domain = 0;
  u64 domain_generation = 0;
  u64 policy_generation = 0;
  u32 raw = 0;
  u32 effective = 0;
  u32 reason = 0;
  u32 constraint = 0;
  u64 decision = 0;
  if (!r.u64(id) || !r.u64(record.tick) || !r.u64(record.wall_millis) || !r.u64(incarnation) || !r.u64(epoch) ||
      !r.u64(domain) || !r.u64(domain_generation) || !r.u64(policy_generation) || !r.u32(raw) ||
      !r.u32(effective) || !r.u32(reason) || !r.u32(constraint) || !r.u64(decision) ||
      !r.digest(record.decision_digest) || !r.digest(record.input_digest) || !r.boolean(record.authorized) ||
      !r.u64(record.authorized_increment) || !r.boolean(record.fenced) || !r.u32(record.confirmations) ||
      !r.u64(record.last_escalation_tick) || !r.u64(record.ceiling_total) || !r.u64(record.committed_total) ||
      !r.u64(record.overage) || !r.string(record.summary, kMaxAuditSummary)) {
    return corrupt(ReasonCode::PersistenceCorrupt, "audit record malformed");
  }
  record.id = AuditRecordId(id);
  record.incarnation = IncarnationId(incarnation);
  record.epoch = FabricEpoch(epoch);
  record.domain = OversubscriptionDomainId(domain);
  record.domain_generation = DomainGeneration(domain_generation);
  record.policy_generation = PolicyGeneration(policy_generation);
  record.raw_outcome = static_cast<Outcome>(raw);
  record.effective_outcome = static_cast<Outcome>(effective);
  record.reason = static_cast<ReasonCode>(reason);
  record.binding_constraint = static_cast<Constraint>(constraint);
  record.decision = DecisionId(decision);
  if (!r.at_end()) return corrupt(ReasonCode::PersistenceCorrupt, "trailing bytes after the audit record");
  out = std::move(record);
  return Status::success();
}

Status rebuild_from_audit(DurableState& state, const std::vector<AuditRecord>& records, ReplayReport& report) {
  std::unordered_map<u64, std::size_t> index;
  index.reserve(state.domains.size() * 2 + 1);
  for (std::size_t i = 0; i < state.domains.size(); ++i) index.emplace(state.domains[i].domain.value(), i);

  u64 previous_id = 0;
  for (const auto& record : records) {
    ++report.records_seen;
    if (record.id.value() <= previous_id) {
      ++report.records_skipped;
      report.last_reason = ReasonCode::EvidenceSequenceRegression;
      report.detail = "audit records are not strictly ordered by id";
      continue;
    }
    previous_id = record.id.value();
    auto it = index.find(record.domain.value());
    if (it == index.end()) {
      if (state.domains.size() >= kMaxDomainsState) {
        ++report.records_skipped;
        report.last_reason = ReasonCode::PersistenceTooLarge;
        continue;
      }
      DomainRuntimeState fresh;
      fresh.domain = record.domain;
      fresh.generation = record.domain_generation;
      state.domains.push_back(fresh);
      it = index.emplace(record.domain.value(), state.domains.size() - 1).first;
    }
    DomainRuntimeState& domain = state.domains[it->second];
    domain.generation = record.domain_generation;
    domain.sticky_outcome = record.effective_outcome;
    domain.last_raw_outcome = record.raw_outcome;
    domain.confirmations = record.confirmations;
    domain.last_escalation_tick = record.last_escalation_tick;
    domain.fenced = record.fenced;
    domain.fence_reason = record.fenced ? record.reason : ReasonCode::None;
    domain.fence_epoch = record.epoch;
    domain.last_decision = record.decision;
    domain.last_decision_digest = record.decision_digest;
    state.decisions_issued = std::max(state.decisions_issued, record.id.value());
    state.tick = std::max(state.tick, record.tick);
    state.epoch = state.epoch.value() >= record.epoch.value() ? state.epoch : record.epoch;
    ++report.records_applied;
  }
  return Status::success();
}

}  // namespace oversub
