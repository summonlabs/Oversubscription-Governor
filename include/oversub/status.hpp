// Oversubscription Governor — status, reason codes, and governance outcomes.
#ifndef OVERSUB_STATUS_HPP
#define OVERSUB_STATUS_HPP

#include <iosfwd>
#include <string>

#include "oversub/checked.hpp"

namespace oversub {

enum class StatusCode : u32 {
  Ok = 0,
  InvalidArgument,
  OutOfRange,
  NotFound,
  AlreadyExists,
  Stale,
  Conflict,
  Denied,
  Fenced,
  CorruptData,
  IntegrityFailure,
  IoError,
  Overflow,
  Unsupported,
  Cancelled,
  ShuttingDown,
  Busy,
  Duplicate,
  QueueFull,
  NotAuthoritative,
  InternalError,
};

const char* to_string(StatusCode code);

// Reason codes are the machine-readable "why". They are stable, bounded, and
// appear verbatim in explanations and audit records.
enum class ReasonCode : u32 {
  None = 0,

  // Policy / configuration
  InvalidIdentity,
  InvalidField,
  PolicyRatioInvalid,
  PolicyRatioOutOfBound,
  PolicyThresholdsInverted,
  PolicyFreshnessUnbounded,
  PolicyDomainDuplicate,
  PolicyDomainEmpty,
  PolicyDomainUnknown,
  PolicyDomainTooLarge,
  PolicyDomainMemberDuplicate,
  PolicyServiceClassDuplicate,
  PolicyServiceClassIneligibleDemand,
  PolicyRiskBudgetInvalid,
  PolicyRiskBudgetGenerationMismatch,
  PolicyGenerationMismatch,
  PolicyDigestMismatch,
  PolicyLimitExceeded,
  PolicyReserveInvalid,

  // Identity / generations / epoch
  DomainNotInPolicy,
  DomainGenerationMismatch,
  DomainMembershipChanged,
  ResourceNotInDomain,
  ResourceGenerationMismatch,
  ResourceMissingFromCapacitySnapshot,
  ResourceDuplicateInCapacitySnapshot,
  ResourceCapacityInvalid,
  ResourceUsableExceedsPhysical,
  ResourceFailed,
  ResourceDegraded,
  ResourceHealthUnknown,
  ResourcePoolMismatch,
  EpochMismatch,
  EpochRegression,
  EpochNotAdvanced,
  GenerationAheadOfRuntime,
  GenerationUnset,

  // Snapshot structure and freshness
  SnapshotMissing,
  SnapshotEmpty,
  SnapshotTooLarge,
  SnapshotGenerationMismatch,
  SnapshotFutureDated,
  SnapshotStale,
  SnapshotDigestMismatch,
  SnapshotOutOfOrder,
  SnapshotDuplicate,

  // Guarantees / reservations
  GuaranteeContradiction,
  GuaranteeDuplicate,
  GuaranteeSumExceedsUsable,
  GuaranteeFloorExceedsCeiling,
  GuaranteeOutOfScope,
  GuaranteeServiceClassUnknown,
  ReservationSumOverflow,

  // Admissions / contingent demand
  AdmissionDuplicate,
  AdmissionKindConflict,
  AdmissionServiceClassUnknown,
  AdmissionSumOverflow,
  AdmissionExceedsContingentCeiling,
  AdmissionNoContingentCeiling,
  BurstPoolExceeded,
  ServiceClassShareExceeded,

  // Risk budget
  RiskBudgetExceeded,
  RiskBudgetExposureOverflow,

  // Arithmetic / evidence
  ArithmeticOverflow,
  EvidenceIncomplete,
  EvidenceContradictory,
  EvidenceDuplicate,
  EvidenceReplayed,
  EvidenceFutureDated,
  EvidenceSequenceRegression,
  EvidencePayloadTooLarge,
  EvidenceMalformed,

  // Publisher / fencing
  PublisherNotRegistered,
  PublisherFenced,
  PublisherIncarnationStale,
  PublisherIncarnationUnknown,
  PublisherProtocolMismatch,
  PublisherSequenceReplayed,
  WorkerIncarnationFenced,
  WorkerEpochStale,
  AttemptAbandoned,

  // Decision lifecycle
  DecisionStale,
  DecisionSuperseded,
  DecisionEpochMismatch,
  DecisionGenerationMismatch,
  DecisionNotAuthoritative,
  DecisionCancelled,
  DecisionAlreadyFenced,

  // Persistence / journal
  PersistenceCorrupt,
  PersistenceVersionUnsupported,
  PersistenceIntegrityFailure,
  PersistenceTruncated,
  PersistenceTooLarge,
  PersistenceJournalPartial,
  PersistenceNotInitialized,

  // Transport / protocol
  TransportClosed,
  TransportError,
  FrameTooLarge,
  FrameTruncated,
  FrameMalformed,
  ProtocolViolation,
  BufferLimitExceeded,
  QueueLimitExceeded,
  PayloadTooLarge,

  // Lifecycle
  ShuttingDown,
  Cancelled,
  LimitExceeded,
  UnsupportedOperation,
  Internal,
};

const char* to_string(ReasonCode code);

struct Status {
  StatusCode code{StatusCode::Ok};
  ReasonCode reason{ReasonCode::None};
  std::string detail;

  bool ok() const { return code == StatusCode::Ok; }
  explicit operator bool() const { return ok(); }
  static Status success() { return Status{}; }
  static Status failure(StatusCode code, ReasonCode reason, std::string detail = {}) {
    Status s;
    s.code = code;
    s.reason = reason;
    s.detail = std::move(detail);
    return s;
  }
  std::string to_string() const;
};

// Governance outcome. This is the authoritative answer to "how much deliberate
// oversubscription is legal right now, in this domain".
enum class Outcome : u32 {
  WithinPolicy = 0,      // legal, with contingent authority remaining
  AtLimit,               // legal, zero contingent authority remaining
  ReductionRequired,     // current commitments exceed the legal boundary
  EmergencyReduction,    // guarantees at risk or exposure beyond emergency threshold
  PolicyRejected,        // policy does not authorize the state at all
  Unknown,               // evidence insufficient to establish a boundary
  Stale,                 // evidence/policy generations or freshness are outdated
  ConflictingInput,      // structurally contradictory evidence
};

const char* to_string(Outcome outcome);
bool outcome_is_authoritative(Outcome outcome);
bool outcome_allows_new_authority(Outcome outcome);
bool outcome_requires_corrective(Outcome outcome);
// Severity ordering used for hysteresis: higher is more restrictive. Unknown,
// Stale, and ConflictingInput are treated as maximally restrictive because they
// deny authority rather than grant it.
u32 outcome_severity(Outcome outcome);

// The single binding constraint that determined the legal boundary.
enum class Constraint : u32 {
  None = 0,
  ConfiguredRatio,
  PolicyRatioCap,
  DegradedRatioCap,
  RiskBudgetExposureAbsolute,
  RiskBudgetExposurePpm,
  MinimumProtectedHeadroom,
  EmergencyReserve,
  GuaranteeProtectionFloor,
  UsableCapacity,
  NoCapacity,
  ServiceClassExclusion,
  BurstPoolCap,
  DomainNotCovered,
};

const char* to_string(Constraint constraint);

// Bounded corrective intent. The governor never performs these actions itself.
enum class CorrectiveKind : u32 {
  None = 0,
  ReduceContingentAdmissionBudget,
  RequestBorrowedCapacityRecall,
  ReduceBurstPool,
  IncreaseProtectedHeadroom,
  FenceOversubscribedAdmission,
  RevalidateEvidence,
  ResolveConflictingEvidence,
};

const char* to_string(CorrectiveKind kind);

enum class CorrectiveTarget : u32 {
  Domain = 0,
  Resource,
  Pool,
  ServiceClass,
};

const char* to_string(CorrectiveTarget target);

enum class ResourceHealth : u32 {
  Unknown = 0,
  Healthy,
  Degraded,
  Failed,
};

const char* to_string(ResourceHealth health);

// How an admitted demand record consumes capacity.
enum class DemandKind : u32 {
  Unknown = 0,
  Guaranteed,   // backed by a protected guarantee
  Contingent,   // relies on deliberate oversubscription
  Burst,        // short-horizon burst pool
};

const char* to_string(DemandKind kind);

// Streaming helpers: enums print as their stable names in diagnostics, test
// failures, and audit tooling.
std::ostream& operator<<(std::ostream& os, StatusCode value);
std::ostream& operator<<(std::ostream& os, ReasonCode value);
std::ostream& operator<<(std::ostream& os, Outcome value);
std::ostream& operator<<(std::ostream& os, Constraint value);
std::ostream& operator<<(std::ostream& os, CorrectiveKind value);
std::ostream& operator<<(std::ostream& os, CorrectiveTarget value);
std::ostream& operator<<(std::ostream& os, ResourceHealth value);
std::ostream& operator<<(std::ostream& os, DemandKind value);

}  // namespace oversub

#endif  // OVERSUB_STATUS_HPP
