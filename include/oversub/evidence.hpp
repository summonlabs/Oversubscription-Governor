// Oversubscription Governor — capacity, reservation, and admission evidence.
//
// Evidence is generation-bound, provenance-carrying, and always domain-scoped:
// a publisher publishes evidence for one contention domain. Nothing in this
// header grants authority; evidence only becomes authoritative when the runtime
// binds it to the generations it currently owns.
#ifndef OVERSUB_EVIDENCE_HPP
#define OVERSUB_EVIDENCE_HPP

#include <string>
#include <vector>

#include "oversub/digest.hpp"
#include "oversub/ids.hpp"
#include "oversub/status.hpp"

namespace oversub {

// Hard structural bounds applied before any evidence is accepted. They are
// compile-time constants so a corrupt durable file or a hostile publisher cannot
// raise them.
inline constexpr u64 kHardMaxResourcesPerSnapshot = 4096;
inline constexpr u64 kHardMaxGuaranteesPerSnapshot = 65536;
inline constexpr u64 kHardMaxDemandRecordsPerSnapshot = 65536;

// Provenance of one evidence publication. The governor binds every decision to
// the provenance that justified it.
struct Provenance {
  PublisherId publisher;
  IncarnationId incarnation;      // publisher process incarnation
  FabricEpoch epoch;              // fabric epoch the publisher believed it was in
  u64 sequence{0};                // strictly increasing per (publisher, incarnation)
  u64 observed_tick{0};           // logical observation tick
  u64 observed_wall_millis{0};    // correlation only, never an input to a decision
  EvidenceId evidence_id;         // idempotency key
  Digest payload_digest;          // canonical digest of the payload

  bool complete() const {
    return publisher.valid() && incarnation.valid() && epoch.valid() && sequence != 0 && evidence_id.valid();
  }
};

enum class EvidenceKind : u32 {
  Capacity = 0,
  Reservation,
  Admission,
};

const char* to_string(EvidenceKind kind);

struct ResourceCapacity {
  ResourceId resource;
  PoolId pool;
  u64 physical_capacity{0};
  u64 usable_capacity{0};          // usable after framing/overhead; <= physical
  ResourceHealth health{ResourceHealth::Unknown};
  u64 degraded_capacity_ppm{0};    // usable scale while DEGRADED (0 => no usable capacity)
  ResourceGeneration generation;
};

struct CapacitySnapshot {
  OversubscriptionDomainId domain;
  CapacitySnapshotGeneration generation;
  FabricEpoch epoch;
  u64 observed_tick{0};
  std::vector<ResourceCapacity> resources;
  Provenance provenance;
};

struct Guarantee {
  ReservationId reservation;
  ResourceId resource;
  PoolId pool;
  ServiceClassId service_class;
  u64 guaranteed_capacity{0};
  PriorityRef priority;
  bool protected_obligation{true};  // must be true; false is a contradiction
  bool preemptible{false};          // a protected guarantee cannot be preemptible
  ReservationGeneration generation;
};

struct ReservationSnapshot {
  OversubscriptionDomainId domain;
  ReservationSnapshotGeneration generation;
  FabricEpoch epoch;
  u64 observed_tick{0};
  std::vector<Guarantee> guarantees;
  Provenance provenance;
};

struct DemandRecord {
  AdmissionId admission;
  ResourceId resource;
  PoolId pool;
  ServiceClassId service_class;
  DemandKind kind{DemandKind::Unknown};
  u64 committed_capacity{0};   // already admitted
  u64 pending_capacity{0};     // requested, not yet admitted
  AdmissionGeneration generation;
};

struct AdmissionSnapshot {
  OversubscriptionDomainId domain;
  AdmissionSnapshotGeneration generation;
  FabricEpoch epoch;
  u64 observed_tick{0};
  std::vector<DemandRecord> records;
  Provenance provenance;
};

// Canonical digests. Independent of element order: the digest is computed over
// elements sorted by identity, so a re-ordered but otherwise identical payload
// hashes identically (and a re-ordered payload with a different identity set
// does not).
Digest capacity_snapshot_digest(const CapacitySnapshot& snapshot);
Digest reservation_snapshot_digest(const ReservationSnapshot& snapshot);
Digest admission_snapshot_digest(const AdmissionSnapshot& snapshot);

// Structural validation. Returns Ok or the precise rejection reason. A snapshot
// generation of zero means "assign the next generation at ingestion"; any other
// value is compared against the runtime's generation continuum by the governor.
// Duplicate identities inside one snapshot are contradictory.
Status validate_capacity_snapshot(const CapacitySnapshot& snapshot);
Status validate_reservation_snapshot(const ReservationSnapshot& snapshot);
Status validate_admission_snapshot(const AdmissionSnapshot& snapshot);

}  // namespace oversub

#endif  // OVERSUB_EVIDENCE_HPP
