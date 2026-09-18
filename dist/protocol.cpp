#include "protocol.hpp"

#include <algorithm>

namespace oversub {
namespace dist {
namespace {

void write_provenance(ByteWriter& w, const Provenance& provenance) {
  w.u64(provenance.publisher.value());
  w.u64(provenance.incarnation.value());
  w.u64(provenance.epoch.value());
  w.u64(provenance.sequence);
  w.u64(provenance.observed_tick);
  w.u64(provenance.observed_wall_millis);
  w.u64(provenance.evidence_id.value());
  w.digest(provenance.payload_digest);
}

bool read_provenance(ByteReader& r, Provenance& provenance) {
  u64 publisher = 0;
  u64 incarnation = 0;
  u64 epoch = 0;
  u64 evidence_id = 0;
  if (!r.u64(publisher) || !r.u64(incarnation) || !r.u64(epoch) || !r.u64(provenance.sequence) ||
      !r.u64(provenance.observed_tick) || !r.u64(provenance.observed_wall_millis) || !r.u64(evidence_id) ||
      !r.digest(provenance.payload_digest)) {
    return false;
  }
  provenance.publisher = PublisherId(publisher);
  provenance.incarnation = IncarnationId(incarnation);
  provenance.epoch = FabricEpoch(epoch);
  provenance.evidence_id = EvidenceId(evidence_id);
  return true;
}

void write_authority(ByteWriter& w, const AuthorityVector& authority) {
  w.u64(authority.domain.value());
  w.u64(authority.domain_generation.value());
  w.u64(authority.policy.value());
  w.u64(authority.policy_generation.value());
  w.u64(authority.capacity_generation.value());
  w.u64(authority.reservation_generation.value());
  w.u64(authority.admission_generation.value());
  w.u64(authority.risk_budget.value());
  w.u64(authority.risk_budget_generation.value());
  w.u64(authority.epoch.value());
  w.u64(authority.resource_set_fold);
  w.digest(authority.input_digest);
}

bool read_authority(ByteReader& r, AuthorityVector& authority) {
  u64 domain = 0;
  u64 domain_generation = 0;
  u64 policy = 0;
  u64 policy_generation = 0;
  u64 capacity_generation = 0;
  u64 reservation_generation = 0;
  u64 admission_generation = 0;
  u64 risk_budget = 0;
  u64 risk_budget_generation = 0;
  u64 epoch = 0;
  if (!r.u64(domain) || !r.u64(domain_generation) || !r.u64(policy) || !r.u64(policy_generation) ||
      !r.u64(capacity_generation) || !r.u64(reservation_generation) || !r.u64(admission_generation) ||
      !r.u64(risk_budget) || !r.u64(risk_budget_generation) || !r.u64(epoch) ||
      !r.u64(authority.resource_set_fold) || !r.digest(authority.input_digest)) {
    return false;
  }
  authority.domain = OversubscriptionDomainId(domain);
  authority.domain_generation = DomainGeneration(domain_generation);
  authority.policy = OversubscriptionPolicyId(policy);
  authority.policy_generation = PolicyGeneration(policy_generation);
  authority.capacity_generation = CapacitySnapshotGeneration(capacity_generation);
  authority.reservation_generation = ReservationSnapshotGeneration(reservation_generation);
  authority.admission_generation = AdmissionSnapshotGeneration(admission_generation);
  authority.risk_budget = RiskBudgetId(risk_budget);
  authority.risk_budget_generation = RiskBudgetGeneration(risk_budget_generation);
  authority.epoch = FabricEpoch(epoch);
  return true;
}

void write_accounting(ByteWriter& w, const DomainAccounting& a) {
  w.u64(a.domain.value());
  w.u64(a.pool.value());
  w.u64(a.physical_capacity);
  w.u64(a.usable_capacity);
  w.u64(a.effective_usable_capacity);
  w.u64(a.guarantees_total);
  w.u64(a.emergency_reserve);
  w.u64(a.min_protected_headroom);
  w.u64(a.protected_required);
  w.u64(a.floor_required);
  w.u64(a.configured_ratio.num);
  w.u64(a.configured_ratio.den);
  w.u64(a.effective_ratio.num);
  w.u64(a.effective_ratio.den);
  w.u64(a.configured_ratio_ppm);
  w.u64(a.effective_ratio_ppm);
  w.boolean(a.ratio_reduced_by_degradation);
  w.u64(a.ceiling_total);
  w.u64(a.contingent_ceiling);
  w.u64(a.contingent_committed);
  w.u64(a.contingent_pending);
  w.u64(a.burst_committed);
  w.u64(a.guaranteed_admitted);
  w.u64(a.contingent_free);
  w.u64(a.committed_total);
  w.u64(a.overage);
  w.u64(a.protected_headroom);
  w.u64(a.used_ppm_of_ceiling);
  w.u64(a.exposure);
  w.u64(a.risk_budget_ceiling);
  w.u64(a.risk_exposure_remaining);
  w.boolean(a.risk_budget_exceeded);
  w.u64(a.out_of_scope_guarantees);
  w.u64(a.out_of_scope_demand);
  w.u32(a.degraded_resources);
  w.u32(a.failed_resources);
  w.u32(static_cast<u32>(a.binding_constraint));
  w.boolean(a.approaching_limit);
  w.boolean(a.arithmetic_closed);
}

bool read_accounting(ByteReader& r, DomainAccounting& a) {
  u64 domain = 0;
  u64 pool = 0;
  u32 constraint = 0;
  if (!r.u64(domain) || !r.u64(pool) || !r.u64(a.physical_capacity) || !r.u64(a.usable_capacity) ||
      !r.u64(a.effective_usable_capacity) || !r.u64(a.guarantees_total) || !r.u64(a.emergency_reserve) ||
      !r.u64(a.min_protected_headroom) || !r.u64(a.protected_required) || !r.u64(a.floor_required) ||
      !r.u64(a.configured_ratio.num) || !r.u64(a.configured_ratio.den) || !r.u64(a.effective_ratio.num) ||
      !r.u64(a.effective_ratio.den) || !r.u64(a.configured_ratio_ppm) || !r.u64(a.effective_ratio_ppm) ||
      !r.boolean(a.ratio_reduced_by_degradation) || !r.u64(a.ceiling_total) || !r.u64(a.contingent_ceiling) ||
      !r.u64(a.contingent_committed) || !r.u64(a.contingent_pending) || !r.u64(a.burst_committed) ||
      !r.u64(a.guaranteed_admitted) || !r.u64(a.contingent_free) || !r.u64(a.committed_total) ||
      !r.u64(a.overage) || !r.u64(a.protected_headroom) || !r.u64(a.used_ppm_of_ceiling) || !r.u64(a.exposure) ||
      !r.u64(a.risk_budget_ceiling) || !r.u64(a.risk_exposure_remaining) ||
      !r.boolean(a.risk_budget_exceeded) || !r.u64(a.out_of_scope_guarantees) ||
      !r.u64(a.out_of_scope_demand) || !r.u32(a.degraded_resources) || !r.u32(a.failed_resources) ||
      !r.u32(constraint) || !r.boolean(a.approaching_limit) || !r.boolean(a.arithmetic_closed)) {
    return false;
  }
  a.domain = OversubscriptionDomainId(domain);
  a.pool = PoolId(pool);
  a.binding_constraint = static_cast<Constraint>(constraint);
  return true;
}

void write_intent(ByteWriter& w, const CorrectiveIntent& intent) {
  w.u32(static_cast<u32>(intent.kind));
  w.u32(static_cast<u32>(intent.target));
  w.u64(intent.domain.value());
  w.u64(intent.resource.value());
  w.u64(intent.pool.value());
  w.u64(intent.service_class.value());
  w.u64(intent.amount_units);
  w.u32(static_cast<u32>(intent.reason));
  w.string(intent.rationale, kMaxDetailLength);
}

bool read_intent(ByteReader& r, CorrectiveIntent& intent) {
  u32 kind = 0;
  u32 target = 0;
  u64 domain = 0;
  u64 resource = 0;
  u64 pool = 0;
  u64 service_class = 0;
  u32 reason = 0;
  if (!r.u32(kind) || !r.u32(target) || !r.u64(domain) || !r.u64(resource) || !r.u64(pool) ||
      !r.u64(service_class) || !r.u64(intent.amount_units) || !r.u32(reason) ||
      !r.string(intent.rationale, kMaxDetailLength)) {
    return false;
  }
  intent.kind = static_cast<CorrectiveKind>(kind);
  intent.target = static_cast<CorrectiveTarget>(target);
  intent.domain = OversubscriptionDomainId(domain);
  intent.resource = ResourceId(resource);
  intent.pool = PoolId(pool);
  intent.service_class = ServiceClassId(service_class);
  intent.reason = static_cast<ReasonCode>(reason);
  return true;
}

}  // namespace

const char* to_string(MessageType type) {
  switch (type) {
    case MessageType::Hello: return "HELLO";
    case MessageType::HelloAck: return "HELLO_ACK";
    case MessageType::Reject: return "REJECT";
    case MessageType::Publish: return "PUBLISH";
    case MessageType::PublishAck: return "PUBLISH_ACK";
    case MessageType::Evaluate: return "EVALUATE";
    case MessageType::DecisionMessage: return "DECISION";
    case MessageType::Revalidate: return "REVALIDATE";
    case MessageType::RevalidateAck: return "REVALIDATE_ACK";
    case MessageType::FencePublisher: return "FENCE_PUBLISHER";
    case MessageType::Shutdown: return "SHUTDOWN";
    case MessageType::ShutdownAck: return "SHUTDOWN_ACK";
  }
  return "UNKNOWN";
}

void encode_hello(const HelloRequest& request, std::vector<u8>& out) {
  ByteWriter w;
  w.u32(request.protocol_version);
  w.u64(request.publisher.value());
  w.u64(request.incarnation.value());
  w.u64(request.domain.value());
  w.boolean(request.adopt_incarnation);
  w.u64(request.tick);
  out = w.data();
}

bool decode_hello(const std::vector<u8>& in, HelloRequest& out) {
  ByteReader r(in);
  u64 publisher = 0;
  u64 incarnation = 0;
  u64 domain = 0;
  if (!r.u32(out.protocol_version) || !r.u64(publisher) || !r.u64(incarnation) || !r.u64(domain) ||
      !r.boolean(out.adopt_incarnation) || !r.u64(out.tick) || !r.at_end()) {
    return false;
  }
  out.publisher = PublisherId(publisher);
  out.incarnation = IncarnationId(incarnation);
  out.domain = OversubscriptionDomainId(domain);
  return true;
}

void encode_hello_response(const HelloResponse& response, std::vector<u8>& out) {
  ByteWriter w;
  w.u32(response.protocol_version);
  w.u64(response.epoch.value());
  w.u64(response.policy_generation.value());
  w.u64(response.capacity_generation);
  w.u64(response.reservation_generation);
  w.u64(response.admission_generation);
  w.u64(response.last_sequence);
  out = w.data();
}

bool decode_hello_response(const std::vector<u8>& in, HelloResponse& out) {
  ByteReader r(in);
  u64 epoch = 0;
  u64 policy_generation = 0;
  if (!r.u32(out.protocol_version) || !r.u64(epoch) || !r.u64(policy_generation) ||
      !r.u64(out.capacity_generation) || !r.u64(out.reservation_generation) ||
      !r.u64(out.admission_generation) || !r.u64(out.last_sequence) || !r.at_end()) {
    return false;
  }
  out.epoch = FabricEpoch(epoch);
  out.policy_generation = PolicyGeneration(policy_generation);
  return true;
}

void encode_status(const Status& status, std::vector<u8>& out) {
  ByteWriter w;
  w.u32(static_cast<u32>(status.code));
  w.u32(static_cast<u32>(status.reason));
  std::string detail = status.detail;
  if (detail.size() > kMaxDetailLength) detail.resize(kMaxDetailLength);
  w.string(detail, kMaxDetailLength);
  out = w.data();
}

bool decode_status(const std::vector<u8>& in, Status& out) {
  ByteReader r(in);
  u32 code = 0;
  u32 reason = 0;
  std::string detail;
  if (!r.u32(code) || !r.u32(reason) || !r.string(detail, kMaxDetailLength) || !r.at_end()) return false;
  out.code = static_cast<StatusCode>(code);
  out.reason = static_cast<ReasonCode>(reason);
  out.detail = std::move(detail);
  return true;
}

void encode_publish_ack(const PublishAck& ack, std::vector<u8>& out) {
  ByteWriter w;
  w.u32(static_cast<u32>(ack.code));
  w.u32(static_cast<u32>(ack.reason));
  w.u64(ack.generation);
  w.boolean(ack.duplicate);
  std::string detail = ack.detail;
  if (detail.size() > kMaxDetailLength) detail.resize(kMaxDetailLength);
  w.string(detail, kMaxDetailLength);
  out = w.data();
}

bool decode_publish_ack(const std::vector<u8>& in, PublishAck& out) {
  ByteReader r(in);
  u32 code = 0;
  u32 reason = 0;
  if (!r.u32(code) || !r.u32(reason) || !r.u64(out.generation) || !r.boolean(out.duplicate) ||
      !r.string(out.detail, kMaxDetailLength) || !r.at_end()) {
    return false;
  }
  out.code = static_cast<StatusCode>(code);
  out.reason = static_cast<ReasonCode>(reason);
  return true;
}

void encode_revalidate_ack(const RevalidateAck& ack, std::vector<u8>& out) {
  ByteWriter w;
  w.boolean(ack.valid);
  w.u32(static_cast<u32>(ack.reason));
  std::string detail = ack.detail;
  if (detail.size() > kMaxDetailLength) detail.resize(kMaxDetailLength);
  w.string(detail, kMaxDetailLength);
  out = w.data();
}

bool decode_revalidate_ack(const std::vector<u8>& in, RevalidateAck& out) {
  ByteReader r(in);
  u32 reason = 0;
  if (!r.boolean(out.valid) || !r.u32(reason) || !r.string(out.detail, kMaxDetailLength) || !r.at_end()) {
    return false;
  }
  out.reason = static_cast<ReasonCode>(reason);
  return true;
}

void encode_capacity_snapshot(const CapacitySnapshot& snapshot, std::vector<u8>& out) {
  ByteWriter w;
  w.u64(snapshot.domain.value());
  w.u64(snapshot.generation.value());
  w.u64(snapshot.epoch.value());
  w.u64(snapshot.observed_tick);
  w.u64(snapshot.resources.size());
  for (const auto& resource : snapshot.resources) {
    w.u64(resource.resource.value());
    w.u64(resource.pool.value());
    w.u64(resource.physical_capacity);
    w.u64(resource.usable_capacity);
    w.u32(static_cast<u32>(resource.health));
    w.u64(resource.degraded_capacity_ppm);
    w.u64(resource.generation.value());
  }
  write_provenance(w, snapshot.provenance);
  out = w.data();
}

bool decode_capacity_snapshot(const std::vector<u8>& in, CapacitySnapshot& out) {
  ByteReader r(in);
  u64 domain = 0;
  u64 generation = 0;
  u64 epoch = 0;
  u64 count = 0;
  if (!r.u64(domain) || !r.u64(generation) || !r.u64(epoch) || !r.u64(out.observed_tick) || !r.u64(count)) {
    return false;
  }
  if (count > kHardMaxResourcesPerSnapshot) return false;
  out.domain = OversubscriptionDomainId(domain);
  out.generation = CapacitySnapshotGeneration(generation);
  out.epoch = FabricEpoch(epoch);
  out.resources.clear();
  out.resources.reserve(static_cast<std::size_t>(count));
  for (u64 i = 0; i < count; ++i) {
    ResourceCapacity resource;
    u64 id = 0;
    u64 pool = 0;
    u32 health = 0;
    u64 resource_generation = 0;
    if (!r.u64(id) || !r.u64(pool) || !r.u64(resource.physical_capacity) || !r.u64(resource.usable_capacity) ||
        !r.u32(health) || !r.u64(resource.degraded_capacity_ppm) || !r.u64(resource_generation)) {
      return false;
    }
    resource.resource = ResourceId(id);
    resource.pool = PoolId(pool);
    resource.health = static_cast<ResourceHealth>(health);
    resource.generation = ResourceGeneration(resource_generation);
    out.resources.push_back(resource);
  }
  return read_provenance(r, out.provenance) && r.at_end();
}

void encode_reservation_snapshot(const ReservationSnapshot& snapshot, std::vector<u8>& out) {
  ByteWriter w;
  w.u64(snapshot.domain.value());
  w.u64(snapshot.generation.value());
  w.u64(snapshot.epoch.value());
  w.u64(snapshot.observed_tick);
  w.u64(snapshot.guarantees.size());
  for (const auto& guarantee : snapshot.guarantees) {
    w.u64(guarantee.reservation.value());
    w.u64(guarantee.resource.value());
    w.u64(guarantee.pool.value());
    w.u64(guarantee.service_class.value());
    w.u64(guarantee.guaranteed_capacity);
    w.u32(guarantee.priority.value);
    w.boolean(guarantee.protected_obligation);
    w.boolean(guarantee.preemptible);
    w.u64(guarantee.generation.value());
  }
  write_provenance(w, snapshot.provenance);
  out = w.data();
}

bool decode_reservation_snapshot(const std::vector<u8>& in, ReservationSnapshot& out) {
  ByteReader r(in);
  u64 domain = 0;
  u64 generation = 0;
  u64 epoch = 0;
  u64 count = 0;
  if (!r.u64(domain) || !r.u64(generation) || !r.u64(epoch) || !r.u64(out.observed_tick) || !r.u64(count)) {
    return false;
  }
  if (count > kHardMaxGuaranteesPerSnapshot) return false;
  out.domain = OversubscriptionDomainId(domain);
  out.generation = ReservationSnapshotGeneration(generation);
  out.epoch = FabricEpoch(epoch);
  out.guarantees.clear();
  out.guarantees.reserve(static_cast<std::size_t>(count));
  for (u64 i = 0; i < count; ++i) {
    Guarantee guarantee;
    u64 reservation = 0;
    u64 resource = 0;
    u64 pool = 0;
    u64 service_class = 0;
    u64 guarantee_generation = 0;
    if (!r.u64(reservation) || !r.u64(resource) || !r.u64(pool) || !r.u64(service_class) ||
        !r.u64(guarantee.guaranteed_capacity) || !r.u32(guarantee.priority.value) ||
        !r.boolean(guarantee.protected_obligation) || !r.boolean(guarantee.preemptible) ||
        !r.u64(guarantee_generation)) {
      return false;
    }
    guarantee.reservation = ReservationId(reservation);
    guarantee.resource = ResourceId(resource);
    guarantee.pool = PoolId(pool);
    guarantee.service_class = ServiceClassId(service_class);
    guarantee.generation = ReservationGeneration(guarantee_generation);
    out.guarantees.push_back(guarantee);
  }
  return read_provenance(r, out.provenance) && r.at_end();
}

void encode_admission_snapshot(const AdmissionSnapshot& snapshot, std::vector<u8>& out) {
  ByteWriter w;
  w.u64(snapshot.domain.value());
  w.u64(snapshot.generation.value());
  w.u64(snapshot.epoch.value());
  w.u64(snapshot.observed_tick);
  w.u64(snapshot.records.size());
  for (const auto& record : snapshot.records) {
    w.u64(record.admission.value());
    w.u64(record.resource.value());
    w.u64(record.pool.value());
    w.u64(record.service_class.value());
    w.u32(static_cast<u32>(record.kind));
    w.u64(record.committed_capacity);
    w.u64(record.pending_capacity);
    w.u64(record.generation.value());
  }
  write_provenance(w, snapshot.provenance);
  out = w.data();
}

bool decode_admission_snapshot(const std::vector<u8>& in, AdmissionSnapshot& out) {
  ByteReader r(in);
  u64 domain = 0;
  u64 generation = 0;
  u64 epoch = 0;
  u64 count = 0;
  if (!r.u64(domain) || !r.u64(generation) || !r.u64(epoch) || !r.u64(out.observed_tick) || !r.u64(count)) {
    return false;
  }
  if (count > kHardMaxDemandRecordsPerSnapshot) return false;
  out.domain = OversubscriptionDomainId(domain);
  out.generation = AdmissionSnapshotGeneration(generation);
  out.epoch = FabricEpoch(epoch);
  out.records.clear();
  out.records.reserve(static_cast<std::size_t>(count));
  for (u64 i = 0; i < count; ++i) {
    DemandRecord record;
    u64 admission = 0;
    u64 resource = 0;
    u64 pool = 0;
    u64 service_class = 0;
    u32 kind = 0;
    u64 record_generation = 0;
    if (!r.u64(admission) || !r.u64(resource) || !r.u64(pool) || !r.u64(service_class) || !r.u32(kind) ||
        !r.u64(record.committed_capacity) || !r.u64(record.pending_capacity) || !r.u64(record_generation)) {
      return false;
    }
    record.admission = AdmissionId(admission);
    record.resource = ResourceId(resource);
    record.pool = PoolId(pool);
    record.service_class = ServiceClassId(service_class);
    record.kind = static_cast<DemandKind>(kind);
    record.generation = AdmissionGeneration(record_generation);
    out.records.push_back(record);
  }
  return read_provenance(r, out.provenance) && r.at_end();
}

void encode_decision(const Decision& decision, std::vector<u8>& out) {
  ByteWriter w;
  w.u64(decision.id.value());
  w.u32(static_cast<u32>(decision.evaluation.outcome));
  w.u32(static_cast<u32>(decision.evaluation.reason));
  std::string detail = decision.evaluation.detail;
  if (detail.size() > kMaxDetailLength) detail.resize(kMaxDetailLength);
  w.string(detail, kMaxDetailLength);
  w.digest(decision.evaluation.result_digest);
  write_accounting(w, decision.evaluation.accounting);
  w.boolean(decision.evaluation.authority_valid);
  w.boolean(decision.evaluation.new_oversubscription_authorized);
  w.u64(decision.evaluation.authorized_increment_units);
  w.boolean(decision.evaluation.fence_required);
  write_authority(w, decision.evaluation.authority);
  w.u64(decision.evaluation.resources.size() > kMaxWireResources ? kMaxWireResources
                                                                 : decision.evaluation.resources.size());
  for (std::size_t i = 0; i < decision.evaluation.resources.size() && i < kMaxWireResources; ++i) {
    const ResourceAccounting& resource = decision.evaluation.resources[i];
    w.u64(resource.resource.value());
    w.u64(resource.pool.value());
    w.u32(static_cast<u32>(resource.health));
    w.u64(resource.physical_capacity);
    w.u64(resource.usable_capacity);
    w.u64(resource.effective_usable_capacity);
    w.u64(resource.guarantees);
    w.u64(resource.contingent_committed);
    w.u64(resource.contingent_pending);
    w.u64(resource.ceiling);
    w.u64(resource.contingent_ceiling);
    w.u64(resource.contingent_free);
    w.u64(resource.overage);
    w.u64(resource.protected_headroom);
    w.u32(static_cast<u32>(resource.binding_constraint));
  }
  w.u64(decision.evaluation.intents.size() > kMaxWireIntents ? kMaxWireIntents
                                                            : decision.evaluation.intents.size());
  for (std::size_t i = 0; i < decision.evaluation.intents.size() && i < kMaxWireIntents; ++i) {
    write_intent(w, decision.evaluation.intents[i]);
  }
  w.u32(static_cast<u32>(decision.raw_outcome));
  w.u32(static_cast<u32>(decision.effective_outcome));
  w.boolean(decision.new_oversubscription_authorized);
  w.u64(decision.authorized_increment_units);
  w.boolean(decision.fenced);
  w.u32(decision.confirmations);
  w.u64(decision.issued_tick);
  w.u64(decision.incarnation.value());
  w.u64(decision.epoch.value());
  w.digest(decision.decision_digest);
  out = w.data();
}

bool decode_decision(const std::vector<u8>& in, Decision& out) {
  ByteReader r(in);
  u64 id = 0;
  u32 outcome = 0;
  u32 reason = 0;
  std::string detail;
  u64 resource_count = 0;
  if (!r.u64(id) || !r.u32(outcome) || !r.u32(reason) || !r.string(detail, kMaxDetailLength) ||
      !r.digest(out.evaluation.result_digest)) {
    return false;
  }
  out.id = DecisionId(id);
  out.evaluation.outcome = static_cast<Outcome>(outcome);
  out.evaluation.reason = static_cast<ReasonCode>(reason);
  out.evaluation.detail = std::move(detail);
  if (!read_accounting(r, out.evaluation.accounting)) return false;
  if (!r.boolean(out.evaluation.authority_valid) ||
      !r.boolean(out.evaluation.new_oversubscription_authorized) ||
      !r.u64(out.evaluation.authorized_increment_units) || !r.boolean(out.evaluation.fence_required)) {
    return false;
  }
  if (!read_authority(r, out.evaluation.authority)) return false;
  if (!r.u64(resource_count)) return false;
  if (resource_count > kMaxWireResources) return false;
  out.evaluation.resources.clear();
  out.evaluation.resources.reserve(static_cast<std::size_t>(resource_count));
  for (u64 i = 0; i < resource_count; ++i) {
    ResourceAccounting resource;
    u64 resource_id = 0;
    u64 pool = 0;
    u32 health = 0;
    u32 constraint = 0;
    if (!r.u64(resource_id) || !r.u64(pool) || !r.u32(health) || !r.u64(resource.physical_capacity) ||
        !r.u64(resource.usable_capacity) || !r.u64(resource.effective_usable_capacity) ||
        !r.u64(resource.guarantees) || !r.u64(resource.contingent_committed) ||
        !r.u64(resource.contingent_pending) || !r.u64(resource.ceiling) ||
        !r.u64(resource.contingent_ceiling) || !r.u64(resource.contingent_free) || !r.u64(resource.overage) ||
        !r.u64(resource.protected_headroom) || !r.u32(constraint)) {
      return false;
    }
    resource.resource = ResourceId(resource_id);
    resource.pool = PoolId(pool);
    resource.health = static_cast<ResourceHealth>(health);
    resource.binding_constraint = static_cast<Constraint>(constraint);
    out.evaluation.resources.push_back(resource);
  }
  u64 intent_count = 0;
  if (!r.u64(intent_count)) return false;
  if (intent_count > kMaxWireIntents) return false;
  out.evaluation.intents.clear();
  out.evaluation.intents.reserve(static_cast<std::size_t>(intent_count));
  for (u64 i = 0; i < intent_count; ++i) {
    CorrectiveIntent intent;
    if (!read_intent(r, intent)) return false;
    out.evaluation.intents.push_back(std::move(intent));
  }
  u32 raw = 0;
  u32 effective = 0;
  u64 incarnation = 0;
  u64 epoch = 0;
  if (!r.u32(raw) || !r.u32(effective) || !r.boolean(out.new_oversubscription_authorized) ||
      !r.u64(out.authorized_increment_units) || !r.boolean(out.fenced) || !r.u32(out.confirmations) ||
      !r.u64(out.issued_tick) || !r.u64(incarnation) || !r.u64(epoch) || !r.digest(out.decision_digest) ||
      !r.at_end()) {
    return false;
  }
  out.raw_outcome = static_cast<Outcome>(raw);
  out.effective_outcome = static_cast<Outcome>(effective);
  out.incarnation = IncarnationId(incarnation);
  out.epoch = FabricEpoch(epoch);
  return true;
}

void encode_revalidate_request(const Decision& decision, std::vector<u8>& out) {
  ByteWriter w;
  w.u64(decision.id.value());
  w.digest(decision.decision_digest);
  w.boolean(decision.evaluation.authority_valid);
  w.u64(decision.incarnation.value());
  w.u64(decision.epoch.value());
  write_authority(w, decision.evaluation.authority);
  out = w.data();
}

bool decode_revalidate_request(const std::vector<u8>& in, Decision& out) {
  ByteReader r(in);
  u64 id = 0;
  u64 incarnation = 0;
  u64 epoch = 0;
  if (!r.u64(id) || !r.digest(out.decision_digest) || !r.boolean(out.evaluation.authority_valid) ||
      !r.u64(incarnation) || !r.u64(epoch) || !read_authority(r, out.evaluation.authority) || !r.at_end()) {
    return false;
  }
  out.id = DecisionId(id);
  out.incarnation = IncarnationId(incarnation);
  out.epoch = FabricEpoch(epoch);
  return true;
}

}  // namespace dist
}  // namespace oversub
