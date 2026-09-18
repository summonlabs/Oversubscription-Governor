// Oversubscription Governor — durable state and audit history.
//
// Durable state is exactly: policy, durable domain configuration, provenance
// bookkeeping, audit history, fencing, epoch, incarnations, generations. Dynamic
// evidence (capacity, reservations, admissions) is NEVER durable: after a
// restart its generations survive but its content does not, so authority must be
// re-established from fresh publications.
#ifndef OVERSUB_STATE_HPP
#define OVERSUB_STATE_HPP

#include <string>
#include <vector>

#include "oversub/digest.hpp"
#include "oversub/engine.hpp"
#include "oversub/evidence.hpp"
#include "oversub/policy.hpp"

namespace oversub {

inline constexpr u32 kStateSchemaVersion = 1;
inline constexpr u32 kJournalFormatVersion = 1;

inline constexpr u32 kMaxPublishers = 4096;
inline constexpr u32 kMaxDomainsState = 4096;
inline constexpr u64 kMaxEvidenceSlots = 16384;
inline constexpr u32 kMaxAuditRecords = 65536;
inline constexpr u32 kMaxAuditSummary = 256;
inline constexpr u32 kMaxStoreNameLength = 128;

struct PublisherRegistration {
  PublisherId publisher;
  IncarnationId incarnation;
  FabricEpoch epoch;
  u64 last_sequence{0};
  bool fenced{false};
  ReasonCode fence_reason{ReasonCode::None};
  u64 registered_tick{0};
};

// Durable per-domain runtime state: sticky severity, confirmation counters, and
// the domain fence. This is authoritative state, not telemetry.
struct DomainRuntimeState {
  OversubscriptionDomainId domain;
  DomainGeneration generation;
  Outcome sticky_outcome{Outcome::WithinPolicy};
  u32 confirmations{0};
  u64 last_escalation_tick{0};
  bool fenced{false};
  ReasonCode fence_reason{ReasonCode::None};
  FabricEpoch fence_epoch;
  DecisionId last_decision;
  Digest last_decision_digest;
  Outcome last_raw_outcome{Outcome::WithinPolicy};
};

// Durable evidence bookkeeping. The generation continuum survives restart; the
// content does not, and every slot is marked revalidation_required on load.
struct EvidenceSlotState {
  OversubscriptionDomainId domain;
  EvidenceKind kind{EvidenceKind::Capacity};
  u64 generation{0};
  EvidenceId evidence_id;
  PublisherId publisher;
  IncarnationId incarnation;
  u64 sequence{0};
  u64 observed_tick{0};
  Digest payload_digest;
  bool revalidation_required{false};
};

struct AuditRecord {
  AuditRecordId id;
  u64 tick{0};
  u64 wall_millis{0};
  IncarnationId incarnation;
  FabricEpoch epoch;
  OversubscriptionDomainId domain;
  DomainGeneration domain_generation;
  PolicyGeneration policy_generation;
  Outcome raw_outcome{Outcome::Unknown};
  Outcome effective_outcome{Outcome::Unknown};
  ReasonCode reason{ReasonCode::None};
  Constraint binding_constraint{Constraint::None};
  DecisionId decision;
  Digest decision_digest;
  Digest input_digest;
  bool authorized{false};
  u64 authorized_increment{0};
  bool fenced{false};
  u32 confirmations{0};
  u64 last_escalation_tick{0};
  u64 ceiling_total{0};
  u64 committed_total{0};
  u64 overage{0};
  std::string summary;
};

struct DurableState {
  u32 schema_version{kStateSchemaVersion};
  u64 revision{0};
  IncarnationId incarnation;   // runtime boot incarnation; advances on every open
  FabricEpoch epoch;
  PolicyGeneration policy_generation;
  Digest policy_digest;
  OversubscriptionPolicy policy;   // durable configuration (validated on load)
  std::vector<PublisherRegistration> publishers;
  std::vector<DomainRuntimeState> domains;
  std::vector<EvidenceSlotState> evidence;
  std::vector<AuditRecord> audit;   // bounded tail, newest last
  u64 decisions_issued{0};
  u64 tick{0};
  u64 journal_records{0};
};

Status serialize_state(const DurableState& state, std::vector<u8>& out);
Status deserialize_state(const std::vector<u8>& buffer, DurableState& out);
Status serialize_audit_record(const AuditRecord& record, std::vector<u8>& out);
Status deserialize_audit_record(const std::vector<u8>& buffer, AuditRecord& out);

struct ReplayReport {
  u64 records_seen{0};
  u64 records_applied{0};
  u64 records_skipped{0};
  ReasonCode last_reason{ReasonCode::None};
  std::string detail;
};

// Rebuilds durable runtime state (hysteresis, fencing, counters) from the audit
// journal alone. Used for recovery and verified by tests against the live state.
Status rebuild_from_audit(DurableState& state, const std::vector<AuditRecord>& records, ReplayReport& report);

}  // namespace oversub

#endif  // OVERSUB_STATE_HPP
