#include "oversub/status.hpp"

#include <ostream>

namespace oversub {

const char* to_string(StatusCode code) {
  switch (code) {
    case StatusCode::Ok: return "Ok";
    case StatusCode::InvalidArgument: return "InvalidArgument";
    case StatusCode::OutOfRange: return "OutOfRange";
    case StatusCode::NotFound: return "NotFound";
    case StatusCode::AlreadyExists: return "AlreadyExists";
    case StatusCode::Stale: return "Stale";
    case StatusCode::Conflict: return "Conflict";
    case StatusCode::Denied: return "Denied";
    case StatusCode::Fenced: return "Fenced";
    case StatusCode::CorruptData: return "CorruptData";
    case StatusCode::IntegrityFailure: return "IntegrityFailure";
    case StatusCode::IoError: return "IoError";
    case StatusCode::Overflow: return "Overflow";
    case StatusCode::Unsupported: return "Unsupported";
    case StatusCode::Cancelled: return "Cancelled";
    case StatusCode::ShuttingDown: return "ShuttingDown";
    case StatusCode::Busy: return "Busy";
    case StatusCode::Duplicate: return "Duplicate";
    case StatusCode::QueueFull: return "QueueFull";
    case StatusCode::NotAuthoritative: return "NotAuthoritative";
    case StatusCode::InternalError: return "InternalError";
  }
  return "UnknownStatusCode";
}

const char* to_string(ReasonCode code) {
  switch (code) {
    case ReasonCode::None: return "None";
    case ReasonCode::InvalidIdentity: return "InvalidIdentity";
    case ReasonCode::InvalidField: return "InvalidField";
    case ReasonCode::PolicyRatioInvalid: return "PolicyRatioInvalid";
    case ReasonCode::PolicyRatioOutOfBound: return "PolicyRatioOutOfBound";
    case ReasonCode::PolicyThresholdsInverted: return "PolicyThresholdsInverted";
    case ReasonCode::PolicyFreshnessUnbounded: return "PolicyFreshnessUnbounded";
    case ReasonCode::PolicyDomainDuplicate: return "PolicyDomainDuplicate";
    case ReasonCode::PolicyDomainEmpty: return "PolicyDomainEmpty";
    case ReasonCode::PolicyDomainUnknown: return "PolicyDomainUnknown";
    case ReasonCode::PolicyDomainTooLarge: return "PolicyDomainTooLarge";
    case ReasonCode::PolicyDomainMemberDuplicate: return "PolicyDomainMemberDuplicate";
    case ReasonCode::PolicyServiceClassDuplicate: return "PolicyServiceClassDuplicate";
    case ReasonCode::PolicyServiceClassIneligibleDemand: return "PolicyServiceClassIneligibleDemand";
    case ReasonCode::PolicyRiskBudgetInvalid: return "PolicyRiskBudgetInvalid";
    case ReasonCode::PolicyRiskBudgetGenerationMismatch: return "PolicyRiskBudgetGenerationMismatch";
    case ReasonCode::PolicyGenerationMismatch: return "PolicyGenerationMismatch";
    case ReasonCode::PolicyDigestMismatch: return "PolicyDigestMismatch";
    case ReasonCode::PolicyLimitExceeded: return "PolicyLimitExceeded";
    case ReasonCode::PolicyReserveInvalid: return "PolicyReserveInvalid";
    case ReasonCode::DomainNotInPolicy: return "DomainNotInPolicy";
    case ReasonCode::DomainGenerationMismatch: return "DomainGenerationMismatch";
    case ReasonCode::DomainMembershipChanged: return "DomainMembershipChanged";
    case ReasonCode::ResourceNotInDomain: return "ResourceNotInDomain";
    case ReasonCode::ResourceGenerationMismatch: return "ResourceGenerationMismatch";
    case ReasonCode::ResourceMissingFromCapacitySnapshot: return "ResourceMissingFromCapacitySnapshot";
    case ReasonCode::ResourceDuplicateInCapacitySnapshot: return "ResourceDuplicateInCapacitySnapshot";
    case ReasonCode::ResourceCapacityInvalid: return "ResourceCapacityInvalid";
    case ReasonCode::ResourceUsableExceedsPhysical: return "ResourceUsableExceedsPhysical";
    case ReasonCode::ResourceFailed: return "ResourceFailed";
    case ReasonCode::ResourceDegraded: return "ResourceDegraded";
    case ReasonCode::ResourceHealthUnknown: return "ResourceHealthUnknown";
    case ReasonCode::ResourcePoolMismatch: return "ResourcePoolMismatch";
    case ReasonCode::EpochMismatch: return "EpochMismatch";
    case ReasonCode::EpochRegression: return "EpochRegression";
    case ReasonCode::EpochNotAdvanced: return "EpochNotAdvanced";
    case ReasonCode::GenerationAheadOfRuntime: return "GenerationAheadOfRuntime";
    case ReasonCode::GenerationUnset: return "GenerationUnset";
    case ReasonCode::SnapshotMissing: return "SnapshotMissing";
    case ReasonCode::SnapshotEmpty: return "SnapshotEmpty";
    case ReasonCode::SnapshotTooLarge: return "SnapshotTooLarge";
    case ReasonCode::SnapshotGenerationMismatch: return "SnapshotGenerationMismatch";
    case ReasonCode::SnapshotFutureDated: return "SnapshotFutureDated";
    case ReasonCode::SnapshotStale: return "SnapshotStale";
    case ReasonCode::SnapshotDigestMismatch: return "SnapshotDigestMismatch";
    case ReasonCode::SnapshotOutOfOrder: return "SnapshotOutOfOrder";
    case ReasonCode::SnapshotDuplicate: return "SnapshotDuplicate";
    case ReasonCode::GuaranteeContradiction: return "GuaranteeContradiction";
    case ReasonCode::GuaranteeDuplicate: return "GuaranteeDuplicate";
    case ReasonCode::GuaranteeSumExceedsUsable: return "GuaranteeSumExceedsUsable";
    case ReasonCode::GuaranteeFloorExceedsCeiling: return "GuaranteeFloorExceedsCeiling";
    case ReasonCode::GuaranteeOutOfScope: return "GuaranteeOutOfScope";
    case ReasonCode::GuaranteeServiceClassUnknown: return "GuaranteeServiceClassUnknown";
    case ReasonCode::ReservationSumOverflow: return "ReservationSumOverflow";
    case ReasonCode::AdmissionDuplicate: return "AdmissionDuplicate";
    case ReasonCode::AdmissionKindConflict: return "AdmissionKindConflict";
    case ReasonCode::AdmissionServiceClassUnknown: return "AdmissionServiceClassUnknown";
    case ReasonCode::AdmissionSumOverflow: return "AdmissionSumOverflow";
    case ReasonCode::AdmissionExceedsContingentCeiling: return "AdmissionExceedsContingentCeiling";
    case ReasonCode::AdmissionNoContingentCeiling: return "AdmissionNoContingentCeiling";
    case ReasonCode::BurstPoolExceeded: return "BurstPoolExceeded";
    case ReasonCode::ServiceClassShareExceeded: return "ServiceClassShareExceeded";
    case ReasonCode::RiskBudgetExceeded: return "RiskBudgetExceeded";
    case ReasonCode::RiskBudgetExposureOverflow: return "RiskBudgetExposureOverflow";
    case ReasonCode::ArithmeticOverflow: return "ArithmeticOverflow";
    case ReasonCode::EvidenceIncomplete: return "EvidenceIncomplete";
    case ReasonCode::EvidenceContradictory: return "EvidenceContradictory";
    case ReasonCode::EvidenceDuplicate: return "EvidenceDuplicate";
    case ReasonCode::EvidenceReplayed: return "EvidenceReplayed";
    case ReasonCode::EvidenceFutureDated: return "EvidenceFutureDated";
    case ReasonCode::EvidenceSequenceRegression: return "EvidenceSequenceRegression";
    case ReasonCode::EvidencePayloadTooLarge: return "EvidencePayloadTooLarge";
    case ReasonCode::EvidenceMalformed: return "EvidenceMalformed";
    case ReasonCode::PublisherNotRegistered: return "PublisherNotRegistered";
    case ReasonCode::PublisherFenced: return "PublisherFenced";
    case ReasonCode::PublisherIncarnationStale: return "PublisherIncarnationStale";
    case ReasonCode::PublisherIncarnationUnknown: return "PublisherIncarnationUnknown";
    case ReasonCode::PublisherProtocolMismatch: return "PublisherProtocolMismatch";
    case ReasonCode::PublisherSequenceReplayed: return "PublisherSequenceReplayed";
    case ReasonCode::WorkerIncarnationFenced: return "WorkerIncarnationFenced";
    case ReasonCode::WorkerEpochStale: return "WorkerEpochStale";
    case ReasonCode::AttemptAbandoned: return "AttemptAbandoned";
    case ReasonCode::DecisionStale: return "DecisionStale";
    case ReasonCode::DecisionSuperseded: return "DecisionSuperseded";
    case ReasonCode::DecisionEpochMismatch: return "DecisionEpochMismatch";
    case ReasonCode::DecisionGenerationMismatch: return "DecisionGenerationMismatch";
    case ReasonCode::DecisionNotAuthoritative: return "DecisionNotAuthoritative";
    case ReasonCode::DecisionCancelled: return "DecisionCancelled";
    case ReasonCode::DecisionAlreadyFenced: return "DecisionAlreadyFenced";
    case ReasonCode::PersistenceCorrupt: return "PersistenceCorrupt";
    case ReasonCode::PersistenceVersionUnsupported: return "PersistenceVersionUnsupported";
    case ReasonCode::PersistenceIntegrityFailure: return "PersistenceIntegrityFailure";
    case ReasonCode::PersistenceTruncated: return "PersistenceTruncated";
    case ReasonCode::PersistenceTooLarge: return "PersistenceTooLarge";
    case ReasonCode::PersistenceJournalPartial: return "PersistenceJournalPartial";
    case ReasonCode::PersistenceNotInitialized: return "PersistenceNotInitialized";
    case ReasonCode::TransportClosed: return "TransportClosed";
    case ReasonCode::TransportError: return "TransportError";
    case ReasonCode::FrameTooLarge: return "FrameTooLarge";
    case ReasonCode::FrameTruncated: return "FrameTruncated";
    case ReasonCode::FrameMalformed: return "FrameMalformed";
    case ReasonCode::ProtocolViolation: return "ProtocolViolation";
    case ReasonCode::BufferLimitExceeded: return "BufferLimitExceeded";
    case ReasonCode::QueueLimitExceeded: return "QueueLimitExceeded";
    case ReasonCode::PayloadTooLarge: return "PayloadTooLarge";
    case ReasonCode::ShuttingDown: return "ShuttingDown";
    case ReasonCode::Cancelled: return "Cancelled";
    case ReasonCode::LimitExceeded: return "LimitExceeded";
    case ReasonCode::UnsupportedOperation: return "UnsupportedOperation";
    case ReasonCode::Internal: return "Internal";
  }
  return "UnknownReasonCode";
}

std::string Status::to_string() const {
  std::string s = oversub::to_string(code);
  if (reason != ReasonCode::None) {
    s += " (";
    s += oversub::to_string(reason);
    s += ")";
  }
  if (!detail.empty()) {
    s += ": ";
    s += detail;
  }
  return s;
}

const char* to_string(Outcome outcome) {
  switch (outcome) {
    case Outcome::WithinPolicy: return "WITHIN_POLICY";
    case Outcome::AtLimit: return "AT_LIMIT";
    case Outcome::ReductionRequired: return "REDUCTION_REQUIRED";
    case Outcome::EmergencyReduction: return "EMERGENCY_REDUCTION";
    case Outcome::PolicyRejected: return "POLICY_REJECTED";
    case Outcome::Unknown: return "UNKNOWN";
    case Outcome::Stale: return "STALE";
    case Outcome::ConflictingInput: return "CONFLICTING_INPUT";
  }
  return "UNKNOWN";
}

bool outcome_is_authoritative(Outcome outcome) {
  switch (outcome) {
    case Outcome::WithinPolicy:
    case Outcome::AtLimit:
    case Outcome::ReductionRequired:
    case Outcome::EmergencyReduction:
      return true;
    default:
      return false;
  }
}

bool outcome_allows_new_authority(Outcome outcome) {
  return outcome == Outcome::WithinPolicy;
}

bool outcome_requires_corrective(Outcome outcome) {
  switch (outcome) {
    case Outcome::ReductionRequired:
    case Outcome::EmergencyReduction:
    case Outcome::PolicyRejected:
    case Outcome::ConflictingInput:
      return true;
    default:
      return false;
  }
}

u32 outcome_severity(Outcome outcome) {
  switch (outcome) {
    case Outcome::WithinPolicy: return 0;
    case Outcome::AtLimit: return 1;
    case Outcome::ReductionRequired: return 2;
    case Outcome::EmergencyReduction: return 3;
    case Outcome::PolicyRejected: return 4;
    case Outcome::Stale: return 5;
    case Outcome::ConflictingInput: return 6;
    case Outcome::Unknown: return 7;
  }
  return 7;
}

const char* to_string(Constraint constraint) {
  switch (constraint) {
    case Constraint::None: return "NONE";
    case Constraint::ConfiguredRatio: return "CONFIGURED_RATIO";
    case Constraint::PolicyRatioCap: return "POLICY_RATIO_CAP";
    case Constraint::DegradedRatioCap: return "DEGRADED_RATIO_CAP";
    case Constraint::RiskBudgetExposureAbsolute: return "RISK_BUDGET_EXPOSURE_ABSOLUTE";
    case Constraint::RiskBudgetExposurePpm: return "RISK_BUDGET_EXPOSURE_PPM";
    case Constraint::MinimumProtectedHeadroom: return "MINIMUM_PROTECTED_HEADROOM";
    case Constraint::EmergencyReserve: return "EMERGENCY_RESERVE";
    case Constraint::GuaranteeProtectionFloor: return "GUARANTEE_PROTECTION_FLOOR";
    case Constraint::UsableCapacity: return "USABLE_CAPACITY";
    case Constraint::NoCapacity: return "NO_CAPACITY";
    case Constraint::ServiceClassExclusion: return "SERVICE_CLASS_EXCLUSION";
    case Constraint::BurstPoolCap: return "BURST_POOL_CAP";
    case Constraint::DomainNotCovered: return "DOMAIN_NOT_COVERED";
  }
  return "NONE";
}

const char* to_string(CorrectiveKind kind) {
  switch (kind) {
    case CorrectiveKind::None: return "NONE";
    case CorrectiveKind::ReduceContingentAdmissionBudget: return "REDUCE_CONTINGENT_ADMISSION_BUDGET";
    case CorrectiveKind::RequestBorrowedCapacityRecall: return "REQUEST_BORROWED_CAPACITY_RECALL";
    case CorrectiveKind::ReduceBurstPool: return "REDUCE_BURST_POOL";
    case CorrectiveKind::IncreaseProtectedHeadroom: return "INCREASE_PROTECTED_HEADROOM";
    case CorrectiveKind::FenceOversubscribedAdmission: return "FENCE_OVERSUBSCRIBED_ADMISSION";
    case CorrectiveKind::RevalidateEvidence: return "REVALIDATE_EVIDENCE";
    case CorrectiveKind::ResolveConflictingEvidence: return "RESOLVE_CONFLICTING_EVIDENCE";
  }
  return "NONE";
}

const char* to_string(CorrectiveTarget target) {
  switch (target) {
    case CorrectiveTarget::Domain: return "DOMAIN";
    case CorrectiveTarget::Resource: return "RESOURCE";
    case CorrectiveTarget::Pool: return "POOL";
    case CorrectiveTarget::ServiceClass: return "SERVICE_CLASS";
  }
  return "DOMAIN";
}

const char* to_string(ResourceHealth health) {
  switch (health) {
    case ResourceHealth::Unknown: return "UNKNOWN";
    case ResourceHealth::Healthy: return "HEALTHY";
    case ResourceHealth::Degraded: return "DEGRADED";
    case ResourceHealth::Failed: return "FAILED";
  }
  return "UNKNOWN";
}

const char* to_string(DemandKind kind) {
  switch (kind) {
    case DemandKind::Unknown: return "UNKNOWN";
    case DemandKind::Guaranteed: return "GUARANTEED";
    case DemandKind::Contingent: return "CONTINGENT";
    case DemandKind::Burst: return "BURST";
  }
  return "UNKNOWN";
}

std::ostream& operator<<(std::ostream& os, StatusCode value) { return os << to_string(value); }
std::ostream& operator<<(std::ostream& os, ReasonCode value) { return os << to_string(value); }
std::ostream& operator<<(std::ostream& os, Outcome value) { return os << to_string(value); }
std::ostream& operator<<(std::ostream& os, Constraint value) { return os << to_string(value); }
std::ostream& operator<<(std::ostream& os, CorrectiveKind value) { return os << to_string(value); }
std::ostream& operator<<(std::ostream& os, CorrectiveTarget value) { return os << to_string(value); }
std::ostream& operator<<(std::ostream& os, ResourceHealth value) { return os << to_string(value); }
std::ostream& operator<<(std::ostream& os, DemandKind value) { return os << to_string(value); }

}  // namespace oversub
