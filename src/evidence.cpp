#include "oversub/evidence.hpp"

#include <algorithm>

namespace oversub {
namespace {

Status malformed(ReasonCode reason, std::string detail) {
  return Status::failure(StatusCode::Conflict, reason, std::move(detail));
}

Status out_of_range(ReasonCode reason, std::string detail) {
  return Status::failure(StatusCode::OutOfRange, reason, std::move(detail));
}

Status invalid(ReasonCode reason, std::string detail) {
  return Status::failure(StatusCode::InvalidArgument, reason, std::move(detail));
}

// Duplicate identities are detected by sorting the identity values, never by a
// quadratic scan: snapshot sizes are externally influenced.
bool has_duplicate_ids(std::vector<u64> ids) {
  if (ids.size() < 2) return false;
  std::sort(ids.begin(), ids.end());
  return std::adjacent_find(ids.begin(), ids.end()) != ids.end();
}

Status check_common(const OversubscriptionDomainId domain, const FabricEpoch epoch, const u64 observed_tick,
                    const Provenance& provenance, const Digest& payload_digest, const Digest& computed) {
  if (!domain.valid()) return invalid(ReasonCode::InvalidIdentity, "evidence domain identity unset");
  if (!epoch.valid()) return invalid(ReasonCode::EpochMismatch, "evidence epoch unset");
  if (observed_tick > kHardMaxTicks) return out_of_range(ReasonCode::InvalidField, "observation tick out of bound");
  if (!provenance.complete()) {
    return malformed(ReasonCode::EvidenceMalformed, "provenance incomplete");
  }
  if (!payload_digest.is_zero() && payload_digest != computed) {
    return Status::failure(StatusCode::IntegrityFailure, ReasonCode::SnapshotDigestMismatch,
                           "payload digest does not match the canonical snapshot digest");
  }
  return Status::success();
}

}  // namespace

const char* to_string(EvidenceKind kind) {
  switch (kind) {
    case EvidenceKind::Capacity: return "CAPACITY";
    case EvidenceKind::Reservation: return "RESERVATION";
    case EvidenceKind::Admission: return "ADMISSION";
  }
  return "UNKNOWN";
}

Digest capacity_snapshot_digest(const CapacitySnapshot& snapshot) {
  std::vector<const ResourceCapacity*> items;
  items.reserve(snapshot.resources.size());
  for (const auto& r : snapshot.resources) items.push_back(&r);
  std::sort(items.begin(), items.end(), [](const ResourceCapacity* a, const ResourceCapacity* b) {
    if (a->resource != b->resource) return a->resource < b->resource;
    return a->pool < b->pool;
  });
  CanonicalHasher h;
  h.add_string("oversub.capacity.v1");
  h.add_u64(snapshot.domain.value());
  h.add_u64(snapshot.generation.value());
  h.add_u64(snapshot.epoch.value());
  h.add_u64(snapshot.observed_tick);
  h.add_u64(items.size());
  for (const ResourceCapacity* r : items) {
    h.add_u64(r->resource.value());
    h.add_u64(r->pool.value());
    h.add_u64(r->physical_capacity);
    h.add_u64(r->usable_capacity);
    h.add_u32(static_cast<u32>(r->health));
    h.add_u64(r->degraded_capacity_ppm);
    h.add_u64(r->generation.value());
  }
  return h.digest();
}

Digest reservation_snapshot_digest(const ReservationSnapshot& snapshot) {
  std::vector<const Guarantee*> items;
  items.reserve(snapshot.guarantees.size());
  for (const auto& g : snapshot.guarantees) items.push_back(&g);
  std::sort(items.begin(), items.end(),
            [](const Guarantee* a, const Guarantee* b) { return a->reservation < b->reservation; });
  CanonicalHasher h;
  h.add_string("oversub.reservation.v1");
  h.add_u64(snapshot.domain.value());
  h.add_u64(snapshot.generation.value());
  h.add_u64(snapshot.epoch.value());
  h.add_u64(snapshot.observed_tick);
  h.add_u64(items.size());
  for (const Guarantee* g : items) {
    h.add_u64(g->reservation.value());
    h.add_u64(g->resource.value());
    h.add_u64(g->pool.value());
    h.add_u64(g->service_class.value());
    h.add_u64(g->guaranteed_capacity);
    h.add_u32(g->priority.value);
    h.add_bool(g->protected_obligation);
    h.add_bool(g->preemptible);
    h.add_u64(g->generation.value());
  }
  return h.digest();
}

Digest admission_snapshot_digest(const AdmissionSnapshot& snapshot) {
  std::vector<const DemandRecord*> items;
  items.reserve(snapshot.records.size());
  for (const auto& r : snapshot.records) items.push_back(&r);
  std::sort(items.begin(), items.end(),
            [](const DemandRecord* a, const DemandRecord* b) { return a->admission < b->admission; });
  CanonicalHasher h;
  h.add_string("oversub.admission.v1");
  h.add_u64(snapshot.domain.value());
  h.add_u64(snapshot.generation.value());
  h.add_u64(snapshot.epoch.value());
  h.add_u64(snapshot.observed_tick);
  h.add_u64(items.size());
  for (const DemandRecord* r : items) {
    h.add_u64(r->admission.value());
    h.add_u64(r->resource.value());
    h.add_u64(r->pool.value());
    h.add_u64(r->service_class.value());
    h.add_u32(static_cast<u32>(r->kind));
    h.add_u64(r->committed_capacity);
    h.add_u64(r->pending_capacity);
    h.add_u64(r->generation.value());
  }
  return h.digest();
}

Status validate_capacity_snapshot(const CapacitySnapshot& snapshot) {
  const Digest computed = capacity_snapshot_digest(snapshot);
  Status common = check_common(snapshot.domain, snapshot.epoch, snapshot.observed_tick, snapshot.provenance,
                               snapshot.provenance.payload_digest, computed);
  if (!common.ok()) return common;
  if (snapshot.resources.size() > kHardMaxResourcesPerSnapshot) {
    return out_of_range(ReasonCode::SnapshotTooLarge, "capacity snapshot has too many resources");
  }
  for (const auto& r : snapshot.resources) {
    if (!r.resource.valid() || !r.pool.valid()) {
      return invalid(ReasonCode::InvalidIdentity, "resource identity unset");
    }
    if (!r.generation.valid()) return invalid(ReasonCode::GenerationUnset, "resource generation unset");
    if (r.usable_capacity > r.physical_capacity) {
      return malformed(ReasonCode::ResourceUsableExceedsPhysical, "usable capacity exceeds physical capacity");
    }
    if (r.physical_capacity > kHardMaxCapacityUnits) {
      return out_of_range(ReasonCode::ResourceCapacityInvalid, "physical capacity out of bound");
    }
    if (r.degraded_capacity_ppm > kPpmScale) {
      return out_of_range(ReasonCode::ResourceCapacityInvalid, "degraded capacity ppm out of bound");
    }
  }
  {
    std::vector<u64> ids;
    ids.reserve(snapshot.resources.size());
    for (const auto& r : snapshot.resources) ids.push_back(r.resource.value());
    if (has_duplicate_ids(std::move(ids))) {
      return malformed(ReasonCode::SnapshotDuplicate, "duplicate resource entry in a capacity snapshot");
    }
  }
  return Status::success();
}

Status validate_reservation_snapshot(const ReservationSnapshot& snapshot) {
  const Digest computed = reservation_snapshot_digest(snapshot);
  Status common = check_common(snapshot.domain, snapshot.epoch, snapshot.observed_tick, snapshot.provenance,
                               snapshot.provenance.payload_digest, computed);
  if (!common.ok()) return common;
  if (snapshot.guarantees.size() > kHardMaxGuaranteesPerSnapshot) {
    return out_of_range(ReasonCode::SnapshotTooLarge, "reservation snapshot has too many guarantees");
  }
  for (const auto& g : snapshot.guarantees) {
    if (!g.reservation.valid() || !g.resource.valid() || !g.pool.valid() || !g.generation.valid()) {
      return invalid(ReasonCode::InvalidIdentity, "guarantee identity unset");
    }
    if (g.guaranteed_capacity > kHardMaxCapacityUnits) {
      return out_of_range(ReasonCode::ResourceCapacityInvalid, "guaranteed capacity out of bound");
    }
    if (!g.protected_obligation || g.preemptible) {
      return malformed(ReasonCode::GuaranteeContradiction,
                       "a protected guarantee cannot be unprotected or preemptible");
    }
  }
  {
    std::vector<u64> ids;
    ids.reserve(snapshot.guarantees.size());
    for (const auto& g : snapshot.guarantees) ids.push_back(g.reservation.value());
    if (has_duplicate_ids(std::move(ids))) {
      return malformed(ReasonCode::GuaranteeDuplicate, "duplicate reservation id in a reservation snapshot");
    }
  }
  return Status::success();
}

Status validate_admission_snapshot(const AdmissionSnapshot& snapshot) {
  const Digest computed = admission_snapshot_digest(snapshot);
  Status common = check_common(snapshot.domain, snapshot.epoch, snapshot.observed_tick, snapshot.provenance,
                               snapshot.provenance.payload_digest, computed);
  if (!common.ok()) return common;
  if (snapshot.records.size() > kHardMaxDemandRecordsPerSnapshot) {
    return out_of_range(ReasonCode::SnapshotTooLarge, "admission snapshot has too many demand records");
  }
  for (const auto& r : snapshot.records) {
    if (!r.admission.valid() || !r.resource.valid() || !r.pool.valid() || !r.generation.valid()) {
      return invalid(ReasonCode::InvalidIdentity, "demand record identity unset");
    }
    if (r.kind == DemandKind::Unknown) {
      return malformed(ReasonCode::AdmissionKindConflict, "demand record has no kind");
    }
    if (r.committed_capacity > kHardMaxCapacityUnits || r.pending_capacity > kHardMaxCapacityUnits) {
      return out_of_range(ReasonCode::ResourceCapacityInvalid, "demand capacity out of bound");
    }
  }
  {
    std::vector<u64> ids;
    ids.reserve(snapshot.records.size());
    for (const auto& r : snapshot.records) ids.push_back(r.admission.value());
    if (has_duplicate_ids(std::move(ids))) {
      return malformed(ReasonCode::AdmissionDuplicate, "duplicate admission id in an admission snapshot");
    }
  }
  return Status::success();
}

}  // namespace oversub
