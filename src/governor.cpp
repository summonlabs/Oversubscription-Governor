#include "oversub/governor.hpp"

#include <algorithm>
#include <utility>

#include "oversub/clock.hpp"

namespace oversub {
namespace {

Status fail(StatusCode code, ReasonCode reason, std::string detail = {}) {
  return Status::failure(code, reason, std::move(detail));
}

std::pair<u64, u32> slot_key(OversubscriptionDomainId domain, EvidenceKind kind) {
  return {domain.value(), static_cast<u32>(kind)};
}

Digest policy_content_digest(const OversubscriptionPolicy& policy) {
  OversubscriptionPolicy copy = policy;
  copy.generation = PolicyGeneration(1);
  for (auto& domain : copy.domains) domain.generation = DomainGeneration(1);
  return compute_policy_digest(copy);
}

Digest decision_digest(const Decision& decision) {
  CanonicalHasher h;
  h.add_string("oversub.decision.v1");
  h.add_u64(decision.id.value());
  h.add_digest(decision.evaluation.result_digest);
  h.add_u32(static_cast<u32>(decision.raw_outcome));
  h.add_u32(static_cast<u32>(decision.effective_outcome));
  h.add_bool(decision.new_oversubscription_authorized);
  h.add_u64(decision.authorized_increment_units);
  h.add_bool(decision.fenced);
  h.add_u64(decision.issued_tick);
  h.add_u64(decision.incarnation.value());
  h.add_u64(decision.epoch.value());
  return h.digest();
}

bool fence_worthy(Outcome outcome) {
  return outcome == Outcome::EmergencyReduction || outcome == Outcome::PolicyRejected ||
         outcome == Outcome::ConflictingInput;
}

}  // namespace

// -------------------------------------------------------------- EvaluationJob --

bool EvaluationJob::ready() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return done_;
}

bool EvaluationJob::cancelled() const { return cancel_flag_.load(); }

bool EvaluationJob::cancel() {
  std::lock_guard<std::mutex> lock(mutex_);
  if (done_) return false;
  cancel_requested_ = true;
  cancel_flag_.store(true);
  return true;
}

Status EvaluationJob::get(Decision& out) {
  std::unique_lock<std::mutex> lock(mutex_);
  cv_.wait(lock, [this] { return done_; });
  if (status_.ok()) out = decision_;
  return status_;
}

void EvaluationJob::finish(const Status& status, const Decision& decision) {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    status_ = status;
    decision_ = decision;
    done_ = true;
  }
  cv_.notify_all();
}

// ------------------------------------------------------------------ Governor --

Governor::Governor() = default;

Governor::~Governor() { (void)close(); }

u64 Governor::tick() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return clock_.tick();
}

OversubscriptionPolicy Governor::policy() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return state_.policy;
}

PolicyGeneration Governor::policy_generation() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return state_.policy.generation;
}

FabricEpoch Governor::epoch() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return state_.epoch;
}

IncarnationId Governor::incarnation() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return state_.incarnation;
}

Status Governor::open(const GovernorOptions& options, OversubscriptionPolicy initial_policy) {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (open_) return fail(StatusCode::AlreadyExists, ReasonCode::None, "governor is already open");
  }
  if (options.state_directory.empty()) {
    return fail(StatusCode::InvalidArgument, ReasonCode::PersistenceNotInitialized,
                "state_directory is required");
  }
  if (options.worker_threads > 64) {
    return fail(StatusCode::OutOfRange, ReasonCode::LimitExceeded, "worker_threads out of bound");
  }
  if (options.max_pending_jobs == 0) {
    return fail(StatusCode::OutOfRange, ReasonCode::LimitExceeded, "max_pending_jobs must be non-zero");
  }
  options_ = options;

  StoreOptions store_options;
  store_options.directory = options_.state_directory;
  store_options.max_state_bytes = options_.max_state_bytes;
  store_options.max_journal_bytes = options_.max_journal_bytes;
  store_options.max_journal_records = options_.max_journal_records;
  store_options.max_audit_records = options_.max_audit_records;
  store_options.sync_writes = options_.sync_writes;
  Status status = store_.open(store_options);
  if (!status.ok()) return status;

  DurableState loaded;
  LoadReport report;
  status = store_.load(loaded, report);
  if (!status.ok()) {
    store_.close();
    return status;
  }

  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (report.outcome == LoadOutcome::Fresh) {
      status = initialize_locked(options_, std::move(initial_policy));
      if (!status.ok()) {
        store_.close();
        return status;
      }
    } else {
      state_ = std::move(loaded);
      if (state_.incarnation.value() == std::numeric_limits<u64>::max()) {
        store_.close();
        return fail(StatusCode::Overflow, ReasonCode::ArithmeticOverflow, "incarnation overflow");
      }
      state_.incarnation = IncarnationId(state_.incarnation.value() + 1);
      if (!state_.epoch.valid()) state_.epoch = FabricEpoch(1);
      clock_.restore(state_.tick);
      slots_.clear();
      slot_index_.clear();
      for (auto& slot_state : state_.evidence) {
        // Durable evidence bookkeeping survives; evidence content does not.
        slot_state.revalidation_required = true;
        EvidenceSlot slot;
        slot.state = slot_state;
        slot.has_content = false;
        slot_index_.emplace(slot_key(slot.state.domain, slot.state.kind), slots_.size());
        slots_.push_back(std::move(slot));
      }
      index_.rebuild(state_.policy);
      state_dirty_ = true;
      status = persist_state_locked(true);
      if (!status.ok()) {
        store_.close();
        return status;
      }
    }
    open_ = true;
    accepting_ = true;
  }

  workers_.reserve(options_.worker_threads);
  for (u32 i = 0; i < options_.worker_threads; ++i) {
    workers_.emplace_back([this] { worker_loop(); });
  }
  return Status::success();
}

Status Governor::initialize_locked(const GovernorOptions& options, OversubscriptionPolicy initial_policy) {
  (void)options;
  if (!initial_policy.id.valid()) initial_policy.id = OversubscriptionPolicyId(1);
  initial_policy.generation = PolicyGeneration(1);
  for (auto& domain : initial_policy.domains) {
    if (!domain.generation.valid()) domain.generation = DomainGeneration(1);
  }
  compute_policy_digest(initial_policy);
  Status status = validate_policy(initial_policy);
  if (!status.ok()) return status;

  DurableState fresh;
  fresh.schema_version = kStateSchemaVersion;
  fresh.revision = 1;
  fresh.incarnation = IncarnationId(1);
  fresh.epoch = FabricEpoch(1);
  fresh.policy = std::move(initial_policy);
  fresh.policy_generation = fresh.policy.generation;
  fresh.policy_digest = fresh.policy.digest;
  state_ = std::move(fresh);
  index_.rebuild(state_.policy);
  slots_.clear();
  slot_index_.clear();
  ++stats_.policy_installs;
  state_dirty_ = true;
  return persist_state_locked(true);
}

Status Governor::close() {
  Status status = shutdown();
  std::lock_guard<std::mutex> lock(mutex_);
  if (open_) {
    store_.close();
    open_ = false;
  }
  return status;
}

Status Governor::shutdown() {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    accepting_ = false;
  }
  u64 cancelled = 0;
  std::vector<std::shared_ptr<EvaluationJob>> abandoned;
  {
    std::lock_guard<std::mutex> lock(queue_mutex_);
    shutting_down_ = true;
    for (auto& pending : queue_) {
      if (pending.job && pending.job->cancel()) ++cancelled;
      if (pending.job) abandoned.push_back(pending.job);
    }
    queue_.clear();
  }
  // Queued work is cancelled, and every cancelled job is completed with a
  // Cancelled status so a waiter never blocks forever on work that will not run.
  for (auto& job : abandoned) {
    job->finish(fail(StatusCode::Cancelled, ReasonCode::DecisionCancelled,
                     "job was cancelled by runtime shutdown"),
                Decision{});
  }
  queue_cv_.notify_all();
  for (auto& worker : workers_) {
    if (worker.joinable()) worker.join();
  }
  workers_.clear();
  std::lock_guard<std::mutex> lock(mutex_);
  stats_.cancellations += cancelled;
  if (!open_) return Status::success();
  return persist_state_locked(true);
}

// ------------------------------------------------------------- configuration --

Status Governor::install_policy(const OversubscriptionPolicy& policy, u64 tick_value) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (!open_) return fail(StatusCode::InvalidArgument, ReasonCode::PersistenceNotInitialized, "not open");
  if (!accepting_) return fail(StatusCode::ShuttingDown, ReasonCode::ShuttingDown, "runtime is shutting down");
  if (tick_value != 0) clock_.observe(tick_value);

  OversubscriptionPolicy next = policy;
  const u64 next_policy_generation = state_.policy.generation.valid() ? state_.policy.generation.value() + 1 : 1;
  if (next_policy_generation == 0) {
    return fail(StatusCode::Overflow, ReasonCode::ArithmeticOverflow, "policy generation overflow");
  }
  if (!next.id.valid()) next.id = state_.policy.id.valid() ? state_.policy.id : OversubscriptionPolicyId(1);
  next.generation = PolicyGeneration(next_policy_generation);
  for (auto& domain : next.domains) {
    const DomainConfig* previous = find_domain(state_.policy, domain.id);
    if (previous == nullptr) {
      if (!domain.generation.valid()) domain.generation = DomainGeneration(1);
      continue;
    }
    if (domain_config_digest(*previous) == domain_config_digest(domain)) {
      domain.generation = previous->generation;   // unchanged content keeps its generation
    } else if (previous->generation.value() + 1 > domain.generation.value()) {
      domain.generation = DomainGeneration(previous->generation.value() + 1);
    }
    if (!domain.generation.valid()) domain.generation = DomainGeneration(1);
  }
  compute_policy_digest(next);

  if (state_.policy.generation.valid() &&
      policy_content_digest(state_.policy) == policy_content_digest(next)) {
    return Status::success();  // idempotent reinstall of identical content
  }
  Status status = validate_policy(next);
  if (!status.ok()) return status;

  drop_evidence_content_locked(ReasonCode::PolicyGenerationMismatch);
  state_.policy = std::move(next);
  state_.policy_generation = state_.policy.generation;
  state_.policy_digest = state_.policy.digest;
  index_.rebuild(state_.policy);
  ++stats_.policy_installs;
  state_dirty_ = true;
  return persist_state_locked(true);
}

Status Governor::advance_epoch(FabricEpoch next) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (!open_) return fail(StatusCode::InvalidArgument, ReasonCode::PersistenceNotInitialized, "not open");
  if (!next.valid() || next.value() <= state_.epoch.value()) {
    return fail(StatusCode::InvalidArgument, ReasonCode::EpochNotAdvanced,
                "the fabric epoch must strictly advance");
  }
  state_.epoch = next;
  ++stats_.epoch_advances;
  drop_evidence_content_locked(ReasonCode::EpochMismatch);
  for (auto& registration : state_.publishers) {
    registration.fenced = true;
    registration.fence_reason = ReasonCode::EpochNotAdvanced;
  }
  for (auto& domain : state_.domains) {
    domain.fenced = true;
    domain.fence_reason = ReasonCode::EpochMismatch;
    domain.fence_epoch = next;
    domain.sticky_outcome = Outcome::Stale;
  }
  state_dirty_ = true;
  return persist_state_locked(true);
}

Status Governor::advance_tick(u64 delta) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (!open_) return fail(StatusCode::InvalidArgument, ReasonCode::PersistenceNotInitialized, "not open");
  if (!clock_.advance(delta)) {
    return fail(StatusCode::Overflow, ReasonCode::ArithmeticOverflow, "tick overflow");
  }
  state_.tick = clock_.tick();
  state_dirty_ = true;
  return Status::success();
}

Status Governor::save() {
  std::lock_guard<std::mutex> lock(mutex_);
  if (!open_) return fail(StatusCode::InvalidArgument, ReasonCode::PersistenceNotInitialized, "not open");
  return persist_state_locked(true);
}

// --------------------------------------------------------------- publishers --

Status Governor::register_publisher(PublisherId publisher, IncarnationId incarnation, FabricEpoch epoch,
                                    u64 tick_value) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (!open_) return fail(StatusCode::InvalidArgument, ReasonCode::PersistenceNotInitialized, "not open");
  if (!accepting_) return fail(StatusCode::ShuttingDown, ReasonCode::ShuttingDown, "runtime is shutting down");
  if (!publisher.valid() || !incarnation.valid()) {
    return fail(StatusCode::InvalidArgument, ReasonCode::InvalidIdentity, "publisher identity unset");
  }
  if (!epoch.valid() || epoch.value() != state_.epoch.value()) {
    return fail(StatusCode::Stale, ReasonCode::EpochMismatch, "publisher epoch does not match the fabric epoch");
  }
  if (tick_value != 0) clock_.observe(tick_value);
  for (auto& registration : state_.publishers) {
    if (!(registration.publisher == publisher)) continue;
    if (registration.incarnation == incarnation) {
      if (registration.fenced) {
        registration.fenced = false;
        registration.fence_reason = ReasonCode::None;
        registration.epoch = epoch;
        registration.last_sequence = 0;
        registration.registered_tick = clock_.tick();
        state_dirty_ = true;
        return persist_state_locked(true);
      }
      return Status::success();
    }
    return fail(StatusCode::Fenced, ReasonCode::PublisherIncarnationStale,
                "a different incarnation is registered; adopt_incarnation is required to fence it");
  }
  if (state_.publishers.size() >= kMaxPublishers) {
    return fail(StatusCode::OutOfRange, ReasonCode::LimitExceeded, "publisher table is full");
  }
  PublisherRegistration registration;
  registration.publisher = publisher;
  registration.incarnation = incarnation;
  registration.epoch = epoch;
  registration.registered_tick = clock_.tick();
  state_.publishers.push_back(registration);
  state_dirty_ = true;
  return persist_state_locked(true);
}

Status Governor::adopt_incarnation(PublisherId publisher, IncarnationId incarnation, FabricEpoch epoch,
                                   u64 tick_value) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (!open_) return fail(StatusCode::InvalidArgument, ReasonCode::PersistenceNotInitialized, "not open");
  if (!accepting_) return fail(StatusCode::ShuttingDown, ReasonCode::ShuttingDown, "runtime is shutting down");
  if (!publisher.valid() || !incarnation.valid() || !epoch.valid()) {
    return fail(StatusCode::InvalidArgument, ReasonCode::InvalidIdentity, "publisher identity unset");
  }
  if (epoch.value() != state_.epoch.value()) {
    return fail(StatusCode::Stale, ReasonCode::EpochMismatch, "publisher epoch does not match the fabric epoch");
  }
  if (tick_value != 0) clock_.observe(tick_value);
  for (auto& registration : state_.publishers) {
    if (!(registration.publisher == publisher)) continue;
    if (registration.incarnation == incarnation) {
      // Re-registration of the same incarnation (for example after an epoch
      // advance) clears the fence and starts a fresh sequence in the new epoch.
      if (!registration.fenced && registration.epoch.value() == epoch.value()) return Status::success();
      registration.fenced = false;
      registration.fence_reason = ReasonCode::None;
      registration.epoch = epoch;
      registration.last_sequence = 0;
      registration.registered_tick = clock_.tick();
      for (auto& slot : slots_) {
        if (slot.state.publisher == publisher) slot.state.revalidation_required = true;
      }
      state_dirty_ = true;
      return persist_state_locked(true);
    }
    registration.incarnation = incarnation;
    registration.epoch = epoch;
    registration.last_sequence = 0;   // the new incarnation starts its own sequence
    registration.fenced = false;
    registration.fence_reason = ReasonCode::None;
    registration.registered_tick = clock_.tick();
    for (auto& slot : slots_) {
      if (slot.state.publisher == publisher) slot.state.revalidation_required = true;
    }
    state_dirty_ = true;
    return persist_state_locked(true);
  }
  return fail(StatusCode::NotFound, ReasonCode::PublisherNotRegistered, "publisher is not registered");
}

Status Governor::fence_publisher(PublisherId publisher, ReasonCode reason) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (!open_) return fail(StatusCode::InvalidArgument, ReasonCode::PersistenceNotInitialized, "not open");
  for (auto& registration : state_.publishers) {
    if (!(registration.publisher == publisher)) continue;
    registration.fenced = true;
    registration.fence_reason = reason;
    state_dirty_ = true;
    return persist_state_locked(true);
  }
  return fail(StatusCode::NotFound, ReasonCode::PublisherNotRegistered, "publisher is not registered");
}

// ------------------------------------------------------------------ evidence --

Governor::EvidenceSlot* Governor::find_slot(OversubscriptionDomainId domain, EvidenceKind kind) {
  const auto it = slot_index_.find(slot_key(domain, kind));
  if (it == slot_index_.end()) return nullptr;
  return &slots_[it->second];
}

Governor::EvidenceSlot& Governor::ensure_slot(OversubscriptionDomainId domain, EvidenceKind kind) {
  const auto key = slot_key(domain, kind);
  const auto it = slot_index_.find(key);
  if (it != slot_index_.end()) return slots_[it->second];
  EvidenceSlot slot;
  slot.state.domain = domain;
  slot.state.kind = kind;
  slot_index_.emplace(key, slots_.size());
  slots_.push_back(std::move(slot));
  return slots_.back();
}

void Governor::sync_evidence_state_locked() {
  state_.evidence.clear();
  state_.evidence.reserve(slots_.size());
  for (const auto& slot : slots_) state_.evidence.push_back(slot.state);
}

void Governor::drop_evidence_content_locked(ReasonCode reason) {
  (void)reason;
  for (auto& slot : slots_) {
    slot.has_content = false;
    slot.capacity.reset();
    slot.reservations.reset();
    slot.admissions.reset();
    slot.state.revalidation_required = true;
  }
}

DomainRuntimeState* Governor::find_domain_state_locked(OversubscriptionDomainId domain) {
  for (auto& entry : state_.domains) {
    if (entry.domain == domain) return &entry;
  }
  return nullptr;
}

Governor::IngestRollback Governor::capture_rollback_locked(OversubscriptionDomainId domain, EvidenceKind kind,
                                                           PublisherId publisher) {
  IngestRollback rollback;
  if (const EvidenceSlot* slot = find_slot(domain, kind); slot != nullptr) {
    rollback.has_slot = true;
    rollback.slot = slot->state;
  }
  for (const auto& registration : state_.publishers) {
    if (registration.publisher == publisher) {
      rollback.has_registration = true;
      rollback.publisher = publisher;
      rollback.sequence = registration.last_sequence;
      break;
    }
  }
  return rollback;
}

void Governor::restore_rollback_locked(const IngestRollback& rollback, OversubscriptionDomainId domain,
                                       EvidenceKind kind) {
  if (rollback.has_slot) {
    EvidenceSlot& slot = ensure_slot(domain, kind);
    slot.state = rollback.slot;
  }
  if (rollback.has_registration) {
    for (auto& registration : state_.publishers) {
      if (registration.publisher == rollback.publisher) {
        registration.last_sequence = rollback.sequence;
        break;
      }
    }
  }
}

Status Governor::ingest_locked(EvidenceKind kind, OversubscriptionDomainId domain, u64 claimed_generation,
                               FabricEpoch epoch, u64 observed_tick, const Provenance& provenance,
                               const Digest& payload_digest, u64& out_generation, bool& duplicate) {
  duplicate = false;
  if (!open_) return fail(StatusCode::InvalidArgument, ReasonCode::PersistenceNotInitialized, "not open");
  if (!accepting_) return fail(StatusCode::ShuttingDown, ReasonCode::ShuttingDown, "runtime is shutting down");
  if (index_.domain(domain) == nullptr) {
    return fail(StatusCode::NotFound, ReasonCode::DomainNotInPolicy,
                "evidence was published for a domain the policy does not cover");
  }
  if (epoch.value() != state_.epoch.value()) {
    return fail(StatusCode::Stale, ReasonCode::EpochMismatch, "evidence epoch does not match the fabric epoch");
  }
  PublisherRegistration* registration = nullptr;
  for (auto& entry : state_.publishers) {
    if (entry.publisher == provenance.publisher) {
      registration = &entry;
      break;
    }
  }
  if (registration == nullptr) {
    return fail(StatusCode::Denied, ReasonCode::PublisherNotRegistered, "publisher is not registered");
  }
  if (registration->fenced) {
    return fail(StatusCode::Fenced, ReasonCode::PublisherFenced, "publisher is fenced");
  }
  if (registration->epoch.value() != state_.epoch.value()) {
    return fail(StatusCode::Stale, ReasonCode::WorkerEpochStale,
                "publisher registration belongs to an older epoch");
  }
  if (registration->incarnation != provenance.incarnation) {
    return fail(StatusCode::Fenced, ReasonCode::PublisherIncarnationStale,
                "evidence comes from an incarnation that is not the registered one");
  }
  if (provenance.sequence == 0) {
    return fail(StatusCode::InvalidArgument, ReasonCode::InvalidField, "evidence sequence must be non-zero");
  }

  EvidenceSlot& slot = ensure_slot(domain, kind);
  if (slot.state.evidence_id.valid() && slot.state.evidence_id == provenance.evidence_id) {
    if (slot.state.payload_digest == payload_digest) {
      duplicate = true;
      out_generation = slot.state.generation;
      return Status::success();
    }
    return fail(StatusCode::Conflict, ReasonCode::EvidenceContradictory,
                "evidence id was already published with different content");
  }
  if (provenance.sequence <= registration->last_sequence) {
    return fail(StatusCode::Stale, ReasonCode::PublisherSequenceReplayed,
                "evidence sequence is not ahead of the last accepted sequence");
  }
  const u64 expected = slot.state.generation + 1;
  if (expected == 0) return fail(StatusCode::Overflow, ReasonCode::ArithmeticOverflow, "generation overflow");
  if (claimed_generation == slot.state.generation && slot.state.generation != 0) {
    if (slot.state.payload_digest == payload_digest) {
      duplicate = true;
      out_generation = slot.state.generation;
      return Status::success();
    }
    return fail(StatusCode::Stale, ReasonCode::SnapshotOutOfOrder,
                "generation was already consumed by different content");
  }
  if (claimed_generation == 0) {
    if (!options_.auto_assign_generations) {
      return fail(StatusCode::InvalidArgument, ReasonCode::GenerationUnset, "evidence generation unset");
    }
  } else if (claimed_generation != expected) {
    return fail(StatusCode::Stale, ReasonCode::SnapshotOutOfOrder,
                "evidence generation is not the next generation");
  }
  out_generation = expected;
  registration->last_sequence = provenance.sequence;
  slot.state.generation = expected;
  slot.state.evidence_id = provenance.evidence_id;
  slot.state.publisher = provenance.publisher;
  slot.state.incarnation = provenance.incarnation;
  slot.state.sequence = provenance.sequence;
  slot.state.observed_tick = observed_tick;
  slot.state.payload_digest = payload_digest;
  slot.state.revalidation_required = false;
  slot.has_content = false;
  slot.capacity.reset();
  slot.reservations.reset();
  slot.admissions.reset();
  ++stats_.evidence_accepted;
  state_dirty_ = true;
  return Status::success();
}

IngestOutcome Governor::ingest_capacity(const CapacitySnapshot& snapshot) {
  CapacitySnapshot copy = snapshot;
  if (copy.provenance.payload_digest.is_zero()) {
    copy.provenance.payload_digest = capacity_snapshot_digest(copy);
  }
  Status status = validate_capacity_snapshot(copy);
  if (!status.ok()) {
    std::lock_guard<std::mutex> lock(mutex_);
    ++stats_.evidence_rejected;
    return IngestOutcome{status, 0, false};
  }
  std::lock_guard<std::mutex> lock(mutex_);
  const IngestRollback rollback =
      capture_rollback_locked(copy.domain, EvidenceKind::Capacity, copy.provenance.publisher);
  u64 generation = 0;
  bool duplicate = false;
  status = ingest_locked(EvidenceKind::Capacity, copy.domain, copy.generation.value(), copy.epoch,
                         copy.observed_tick, copy.provenance, copy.provenance.payload_digest, generation,
                         duplicate);
  if (!status.ok()) {
    ++stats_.evidence_rejected;
    return IngestOutcome{status, 0, false};
  }
  if (duplicate) {
    ++stats_.evidence_duplicates;
    return IngestOutcome{Status::success(), generation, true};
  }
  EvidenceSlot& slot = ensure_slot(copy.domain, EvidenceKind::Capacity);
  copy.generation = CapacitySnapshotGeneration(generation);
  copy.provenance.payload_digest = capacity_snapshot_digest(copy);
  slot.capacity = std::make_shared<const CapacitySnapshot>(std::move(copy));
  slot.has_content = true;
  sync_evidence_state_locked();
  const Status save_status = persist_state_locked(true);
  if (!save_status.ok()) {
    slot.has_content = false;
    slot.capacity.reset();
    restore_rollback_locked(rollback, snapshot.domain, EvidenceKind::Capacity);
    --stats_.evidence_accepted;
    ++stats_.evidence_rejected;
    return IngestOutcome{save_status, 0, false};
  }
  return IngestOutcome{Status::success(), generation, false};
}

IngestOutcome Governor::ingest_reservations(const ReservationSnapshot& snapshot) {
  ReservationSnapshot copy = snapshot;
  if (copy.provenance.payload_digest.is_zero()) {
    copy.provenance.payload_digest = reservation_snapshot_digest(copy);
  }
  Status status = validate_reservation_snapshot(copy);
  if (!status.ok()) {
    std::lock_guard<std::mutex> lock(mutex_);
    ++stats_.evidence_rejected;
    return IngestOutcome{status, 0, false};
  }
  std::lock_guard<std::mutex> lock(mutex_);
  const IngestRollback rollback =
      capture_rollback_locked(copy.domain, EvidenceKind::Reservation, copy.provenance.publisher);
  u64 generation = 0;
  bool duplicate = false;
  status = ingest_locked(EvidenceKind::Reservation, copy.domain, copy.generation.value(), copy.epoch,
                         copy.observed_tick, copy.provenance, copy.provenance.payload_digest, generation,
                         duplicate);
  if (!status.ok()) {
    ++stats_.evidence_rejected;
    return IngestOutcome{status, 0, false};
  }
  if (duplicate) {
    ++stats_.evidence_duplicates;
    return IngestOutcome{Status::success(), generation, true};
  }
  EvidenceSlot& slot = ensure_slot(copy.domain, EvidenceKind::Reservation);
  copy.generation = ReservationSnapshotGeneration(generation);
  copy.provenance.payload_digest = reservation_snapshot_digest(copy);
  slot.reservations = std::make_shared<const ReservationSnapshot>(std::move(copy));
  slot.has_content = true;
  sync_evidence_state_locked();
  const Status save_status = persist_state_locked(true);
  if (!save_status.ok()) {
    slot.has_content = false;
    slot.reservations.reset();
    restore_rollback_locked(rollback, snapshot.domain, EvidenceKind::Reservation);
    --stats_.evidence_accepted;
    ++stats_.evidence_rejected;
    return IngestOutcome{save_status, 0, false};
  }
  return IngestOutcome{Status::success(), generation, false};
}

IngestOutcome Governor::ingest_admissions(const AdmissionSnapshot& snapshot) {
  AdmissionSnapshot copy = snapshot;
  if (copy.provenance.payload_digest.is_zero()) {
    copy.provenance.payload_digest = admission_snapshot_digest(copy);
  }
  Status status = validate_admission_snapshot(copy);
  if (!status.ok()) {
    std::lock_guard<std::mutex> lock(mutex_);
    ++stats_.evidence_rejected;
    return IngestOutcome{status, 0, false};
  }
  std::lock_guard<std::mutex> lock(mutex_);
  const IngestRollback rollback =
      capture_rollback_locked(copy.domain, EvidenceKind::Admission, copy.provenance.publisher);
  u64 generation = 0;
  bool duplicate = false;
  status = ingest_locked(EvidenceKind::Admission, copy.domain, copy.generation.value(), copy.epoch,
                         copy.observed_tick, copy.provenance, copy.provenance.payload_digest, generation,
                         duplicate);
  if (!status.ok()) {
    ++stats_.evidence_rejected;
    return IngestOutcome{status, 0, false};
  }
  if (duplicate) {
    ++stats_.evidence_duplicates;
    return IngestOutcome{Status::success(), generation, true};
  }
  EvidenceSlot& slot = ensure_slot(copy.domain, EvidenceKind::Admission);
  copy.generation = AdmissionSnapshotGeneration(generation);
  copy.provenance.payload_digest = admission_snapshot_digest(copy);
  slot.admissions = std::make_shared<const AdmissionSnapshot>(std::move(copy));
  slot.has_content = true;
  sync_evidence_state_locked();
  const Status save_status = persist_state_locked(true);
  if (!save_status.ok()) {
    slot.has_content = false;
    slot.admissions.reset();
    restore_rollback_locked(rollback, snapshot.domain, EvidenceKind::Admission);
    --stats_.evidence_accepted;
    ++stats_.evidence_rejected;
    return IngestOutcome{save_status, 0, false};
  }
  return IngestOutcome{Status::success(), generation, false};
}

// --------------------------------------------------------------- governance --

Status Governor::append_journal_locked(const AuditRecord& record) {
  Status status = store_.append_audit(record);
  if (status.code == StatusCode::Overflow) {
    status = compact_journal_locked();
    if (!status.ok()) return status;
    status = store_.append_audit(record);
  }
  if (!status.ok()) return status;
  state_.audit.push_back(record);
  if (state_.audit.size() > options_.max_audit_records) {
    const std::size_t excess = state_.audit.size() - options_.max_audit_records;
    state_.audit.erase(state_.audit.begin(), state_.audit.begin() + static_cast<std::ptrdiff_t>(excess));
  }
  ++stats_.journal_appends;
  return Status::success();
}

Status Governor::compact_journal_locked() {
  const u64 keep = std::min<u64>(options_.journal_compaction_records, options_.max_journal_records);
  Status status = store_.compact_journal(state_.audit, keep);
  if (!status.ok()) return status;
  if (state_.audit.size() > keep) {
    const std::size_t excess = state_.audit.size() - static_cast<std::size_t>(keep);
    state_.audit.erase(state_.audit.begin(), state_.audit.begin() + static_cast<std::ptrdiff_t>(excess));
  }
  ++stats_.journal_compactions;
  state_dirty_ = true;
  return Status::success();
}

Status Governor::persist_state_locked(bool force) {
  if (!open_) {
    // During open() the durable store is live before the runtime is published.
    if (store_.is_open()) {
      sync_evidence_state_locked();
      state_.tick = clock_.tick();
      state_.revision += 1;
      Status status = store_.save(state_);
      if (status.ok()) {
        decisions_since_save_ = 0;
        state_dirty_ = false;
        ++stats_.state_saves;
      }
      return status;
    }
    return fail(StatusCode::InvalidArgument, ReasonCode::PersistenceNotInitialized, "not open");
  }
  if (!force && !state_dirty_) return Status::success();
  if (!force && decisions_since_save_ < options_.save_state_every_decisions) return Status::success();
  sync_evidence_state_locked();
  state_.tick = clock_.tick();
  state_.revision += 1;
  Status status = store_.save(state_);
  if (!status.ok()) return status;
  decisions_since_save_ = 0;
  state_dirty_ = false;
  ++stats_.state_saves;
  return Status::success();
}

Status Governor::evaluate_locked(const EvaluationRequest& request, Decision& out,
                                 const std::atomic<bool>* cancel) {
  if (!open_) return fail(StatusCode::InvalidArgument, ReasonCode::PersistenceNotInitialized, "not open");
  // Work that was accepted before shutdown is drained rather than refused: the
  // accepting_ gate is enforced at the public entry points only.
  if (!request.domain.valid()) {
    return fail(StatusCode::InvalidArgument, ReasonCode::InvalidIdentity, "evaluation domain unset");
  }
  u64 tick_value = request.tick;
  if (tick_value < clock_.tick()) tick_value = clock_.tick();
  clock_.observe(tick_value);

  AuthorityExpectation expectation;
  expectation.epoch = state_.epoch;
  expectation.policy_generation = state_.policy.generation;
  expectation.risk_budget_generation = state_.policy.risk_budget.generation;
  const DomainConfig* domain = index_.domain(request.domain);
  expectation.domain_generation = domain != nullptr ? domain->generation : DomainGeneration(0);
  const EvidenceSlot* capacity_slot = find_slot(request.domain, EvidenceKind::Capacity);
  const EvidenceSlot* reservation_slot = find_slot(request.domain, EvidenceKind::Reservation);
  const EvidenceSlot* admission_slot = find_slot(request.domain, EvidenceKind::Admission);
  expectation.capacity_generation =
      CapacitySnapshotGeneration(capacity_slot != nullptr ? capacity_slot->state.generation : 0);
  expectation.reservation_generation =
      ReservationSnapshotGeneration(reservation_slot != nullptr ? reservation_slot->state.generation : 0);
  expectation.admission_generation =
      AdmissionSnapshotGeneration(admission_slot != nullptr ? admission_slot->state.generation : 0);

  DomainEvidence evidence;
  if (capacity_slot != nullptr && capacity_slot->has_content) evidence.capacity = capacity_slot->capacity.get();
  if (reservation_slot != nullptr && reservation_slot->has_content) {
    evidence.reservations = reservation_slot->reservations.get();
  }
  if (admission_slot != nullptr && admission_slot->has_content) {
    evidence.admissions = admission_slot->admissions.get();
  }

  EvaluationResult result;
  if (domain == nullptr) {
    result.outcome = Outcome::Unknown;
    result.reason = ReasonCode::DomainNotInPolicy;
    result.detail = "the policy does not cover this domain";
    result.accounting.domain = request.domain;
    result.authority.domain = request.domain;
    result.authority.policy = state_.policy.id;
    result.authority.policy_generation = state_.policy.generation;
    result.authority.epoch = state_.epoch;
    result.authority.risk_budget = state_.policy.risk_budget.id;
    result.authority.risk_budget_generation = state_.policy.risk_budget.generation;
    result.authority.capacity_generation = expectation.capacity_generation;
    result.authority.reservation_generation = expectation.reservation_generation;
    result.authority.admission_generation = expectation.admission_generation;
    result.intents.push_back(CorrectiveIntent{CorrectiveKind::RevalidateEvidence, CorrectiveTarget::Domain,
                                              request.domain, ResourceId{}, PoolId{}, ServiceClassId{}, 0,
                                              ReasonCode::DomainNotInPolicy,
                                              "the domain is not covered by policy; no authority exists"});
  } else {
    result = evaluate_domain(state_.policy, index_, *domain, expectation, evidence, tick_value);
  }

  DomainRuntimeState* previous = find_domain_state_locked(request.domain);
  DomainRuntimeState working;
  if (previous != nullptr) {
    working = *previous;
  } else {
    working.domain = request.domain;
    working.generation = expectation.domain_generation;
    working.sticky_outcome = result.outcome;
    working.last_raw_outcome = result.outcome;
  }

  const HysteresisPolicy& hysteresis = state_.policy.hysteresis;
  Outcome effective = result.outcome;
  const u32 raw_severity = outcome_severity(result.outcome);
  const u32 sticky_severity = outcome_severity(working.sticky_outcome);
  if (raw_severity >= 2) {
    // Restriction is never delayed by hysteresis.
    if (raw_severity > sticky_severity) working.last_escalation_tick = tick_value;
    working.sticky_outcome = result.outcome;
    // Restriction is immediate; any partially accumulated relaxation
    // confirmations are discarded rather than carried into the next relaxation.
    working.confirmations = 0;
    effective = result.outcome;
  } else if (raw_severity == 1) {
    if (sticky_severity >= 2) {
      effective = working.sticky_outcome;
      working.confirmations = 0;
    } else {
      if (working.sticky_outcome != Outcome::AtLimit) {
        working.sticky_outcome = Outcome::AtLimit;
        working.confirmations = 0;
      }
      working.confirmations += 1;
      effective = working.confirmations >= hysteresis.escalation_confirmations ? Outcome::AtLimit
                                                                              : Outcome::WithinPolicy;
    }
  } else if (sticky_severity == 0) {
    working.sticky_outcome = result.outcome;
    working.confirmations = 0;
    effective = result.outcome;
  } else {
    const u64 used = std::min<u64>(result.accounting.used_ppm_of_ceiling, kPpmScale);
    const bool margin_ok = used + hysteresis.deescalation_margin_ppm <= kPpmScale;
    const bool cooldown_ok = tick_value >= working.last_escalation_tick &&
                             tick_value - working.last_escalation_tick >= hysteresis.cooldown_ticks;
    if (!margin_ok || !cooldown_ok) {
      working.confirmations = 0;
      effective = working.sticky_outcome;
    } else {
      working.confirmations += 1;
      if (working.confirmations >= hysteresis.deescalation_confirmations) {
        working.sticky_outcome = result.outcome;
        working.confirmations = 0;
        effective = result.outcome;
      } else {
        effective = working.sticky_outcome;
      }
    }
  }
  working.last_raw_outcome = result.outcome;
  working.generation = expectation.domain_generation;

  Decision decision;
  decision.id = DecisionId(state_.decisions_issued + 1);
  decision.evaluation = result;
  decision.raw_outcome = result.outcome;
  decision.effective_outcome = effective;
  decision.new_oversubscription_authorized =
      effective == Outcome::WithinPolicy && result.new_oversubscription_authorized;
  decision.authorized_increment_units =
      decision.new_oversubscription_authorized ? result.authorized_increment_units : 0;
  // A fence is raised by an outcome that requires fencing and is only cleared by
  // an authoritative result: a fence raised by an epoch advance or a policy
  // change survives every non-authoritative evaluation.
  decision.fenced = fence_worthy(effective) ||
                    (working.fenced && !outcome_is_authoritative(effective));
  decision.confirmations = working.confirmations;
  decision.issued_tick = tick_value;
  decision.incarnation = state_.incarnation;
  decision.epoch = state_.epoch;
  decision.decision_digest = decision_digest(decision);

  if (cancel != nullptr && cancel->load()) {
    ++stats_.cancellations;
    return fail(StatusCode::Cancelled, ReasonCode::DecisionCancelled,
                "evaluation was cancelled before its authoritative completion boundary");
  }

  AuditRecord record;
  record.id = AuditRecordId(decision.id.value());
  record.tick = tick_value;
  record.wall_millis = wall_clock_millis();
  record.incarnation = state_.incarnation;
  record.epoch = state_.epoch;
  record.domain = request.domain;
  record.domain_generation = expectation.domain_generation;
  record.policy_generation = state_.policy.generation;
  record.raw_outcome = result.outcome;
  record.effective_outcome = effective;
  record.reason = result.reason;
  record.binding_constraint = result.accounting.binding_constraint;
  record.decision = decision.id;
  record.decision_digest = decision.decision_digest;
  record.input_digest = result.authority.input_digest;
  record.authorized = decision.new_oversubscription_authorized;
  record.authorized_increment = decision.authorized_increment_units;
  record.fenced = decision.fenced;
  record.confirmations = working.confirmations;
  record.last_escalation_tick = working.last_escalation_tick;
  record.ceiling_total = result.accounting.ceiling_total;
  record.committed_total = result.accounting.committed_total;
  record.overage = result.accounting.overage;
  record.summary = std::string(to_string(result.outcome));
  if (result.reason != ReasonCode::None) {
    record.summary += ":";
    record.summary += to_string(result.reason);
  }
  if (record.summary.size() > kMaxAuditSummary) record.summary.resize(kMaxAuditSummary);

  // Every precondition that could reject the decision is checked before the
  // durable boundary, so a durable audit record always has matching state.
  if (previous == nullptr && state_.domains.size() >= kMaxDomainsState) {
    return fail(StatusCode::OutOfRange, ReasonCode::LimitExceeded, "domain runtime table is full");
  }

  // Durable first: the decision is acknowledged only after its audit record is
  // durable on stable storage. Nothing below runs if the append fails.
  Status status = append_journal_locked(record);
  if (!status.ok()) return status;

  ++stats_.evaluations;
  working.fenced = decision.fenced;
  working.fence_reason = decision.fenced ? result.reason : ReasonCode::None;
  working.fence_epoch = decision.fenced ? state_.epoch : FabricEpoch(0);
  working.last_decision = decision.id;
  working.last_decision_digest = decision.decision_digest;
  if (previous != nullptr) {
    *previous = working;
  } else {
    state_.domains.push_back(working);
  }
  state_.decisions_issued = decision.id.value();
  state_.tick = tick_value;
  ++stats_.decisions;
  ++decisions_since_save_;
  state_dirty_ = true;
  status = persist_state_locked(false);
  if (!status.ok()) return status;
  out = std::move(decision);
  return Status::success();
}

Status Governor::evaluate(const EvaluationRequest& request, Decision& out) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (!accepting_) return fail(StatusCode::ShuttingDown, ReasonCode::ShuttingDown, "runtime is shutting down");
  return evaluate_locked(request, out, nullptr);
}

Status Governor::submit(const EvaluationRequest& request, std::shared_ptr<EvaluationJob>& out) {
  auto job = std::make_shared<EvaluationJob>();
  u32 worker_threads = 0;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!open_) return fail(StatusCode::InvalidArgument, ReasonCode::PersistenceNotInitialized, "not open");
    if (!accepting_) return fail(StatusCode::ShuttingDown, ReasonCode::ShuttingDown, "runtime is shutting down");
    worker_threads = options_.worker_threads;
  }
  if (worker_threads == 0) {
    Decision decision;
    Status status = evaluate(request, decision);
    job->finish(status, decision);
    out = std::move(job);
    return Status::success();
  }
  {
    std::lock_guard<std::mutex> lock(queue_mutex_);
    if (shutting_down_) {
      return fail(StatusCode::ShuttingDown, ReasonCode::ShuttingDown, "runtime is shutting down");
    }
    if (queue_.size() >= options_.max_pending_jobs) {
      return fail(StatusCode::QueueFull, ReasonCode::QueueLimitExceeded, "pending job queue is full");
    }
    queue_.push_back(PendingJob{job, request});
  }
  queue_cv_.notify_one();
  out = std::move(job);
  return Status::success();
}

void Governor::worker_loop() {
  for (;;) {
    PendingJob pending;
    {
      std::unique_lock<std::mutex> lock(queue_mutex_);
      queue_cv_.wait(lock, [this] { return !queue_.empty() || shutting_down_; });
      if (queue_.empty()) {
        if (shutting_down_) return;
        continue;
      }
      pending = std::move(queue_.front());
      queue_.pop_front();
    }
    if (!pending.job) continue;
    if (pending.job->cancelled()) {
      pending.job->finish(
          fail(StatusCode::Cancelled, ReasonCode::DecisionCancelled, "job was cancelled before it started"),
          Decision{});
      continue;
    }
    Decision decision;
    Status status;
    try {
      std::lock_guard<std::mutex> lock(mutex_);
      status = evaluate_locked(pending.request, decision, &pending.job->cancel_flag_);
    } catch (const std::exception& error) {
      status = fail(StatusCode::InternalError, ReasonCode::Internal,
                    std::string("evaluation failed: ") + error.what());
    } catch (...) {
      status = fail(StatusCode::InternalError, ReasonCode::Internal, "evaluation failed");
    }
    if (status.ok()) {
      pending.job->finish(status, decision);
    } else {
      pending.job->finish(status, Decision{});
    }
  }
}

Status Governor::revalidate(const Decision& decision, RevalidationReport& report) {
  report = RevalidationReport{};
  std::lock_guard<std::mutex> lock(mutex_);
  if (!open_) return fail(StatusCode::InvalidArgument, ReasonCode::PersistenceNotInitialized, "not open");
  ++stats_.revalidations;
  auto invalid = [&](ReasonCode reason, std::string detail) {
    report.valid = false;
    report.reason = reason;
    report.detail = std::move(detail);
    ++stats_.revalidations_failed;
    return Status::success();
  };
  const AuthorityVector& authority = decision.evaluation.authority;
  if (!authority.complete() || !decision.evaluation.authority_valid) {
    return invalid(ReasonCode::DecisionNotAuthoritative, "the decision never carried authority");
  }
  if (decision.incarnation != state_.incarnation) {
    return invalid(ReasonCode::DecisionStale, "the runtime was restarted; live authority is not restored");
  }
  if (decision.epoch.value() != state_.epoch.value()) {
    return invalid(ReasonCode::DecisionEpochMismatch, "the fabric epoch advanced");
  }
  if (authority.policy_generation.value() != state_.policy.generation.value()) {
    return invalid(ReasonCode::DecisionGenerationMismatch, "the policy generation changed");
  }
  if (authority.risk_budget_generation.value() != state_.policy.risk_budget.generation.value()) {
    return invalid(ReasonCode::DecisionGenerationMismatch, "the risk budget generation changed");
  }
  const DomainConfig* domain = index_.domain(authority.domain);
  if (domain == nullptr) {
    return invalid(ReasonCode::DecisionGenerationMismatch, "the domain is no longer covered by the policy");
  }
  if (authority.domain_generation.value() != domain->generation.value()) {
    return invalid(ReasonCode::DecisionGenerationMismatch, "the domain configuration generation changed");
  }
  const DomainRuntimeState* runtime = nullptr;
  for (const auto& entry : state_.domains) {
    if (entry.domain == authority.domain) {
      runtime = &entry;
      break;
    }
  }
  if (runtime != nullptr && runtime->fenced) {
    return invalid(ReasonCode::DecisionAlreadyFenced, "the domain is fenced");
  }
  if (runtime != nullptr && runtime->last_decision.valid() && runtime->last_decision != decision.id) {
    return invalid(ReasonCode::DecisionSuperseded, "a newer decision supersedes this one");
  }
  struct SlotCheck {
    EvidenceKind kind;
    u64 generation;
    const char* what;
  };
  const SlotCheck checks[3] = {{EvidenceKind::Capacity, authority.capacity_generation.value(), "capacity"},
                               {EvidenceKind::Reservation, authority.reservation_generation.value(),
                                "reservation"},
                               {EvidenceKind::Admission, authority.admission_generation.value(), "admission"}};
  for (const SlotCheck& check : checks) {
    const EvidenceSlot* slot = nullptr;
    const auto it = slot_index_.find(slot_key(authority.domain, check.kind));
    if (it != slot_index_.end()) slot = &slots_[it->second];
    if (slot == nullptr || !slot->has_content || slot->state.revalidation_required ||
        slot->state.generation != check.generation) {
      return invalid(ReasonCode::DecisionStale,
                     std::string("the ") + check.what +
                         " evidence that justified the decision is no longer authoritative");
    }
  }
  report.valid = true;
  report.reason = ReasonCode::None;
  report.detail = "decision is still bound to the current authority";
  return Status::success();
}

Status Governor::explain(const Decision& decision, const ExplainOptions& options, std::string& out) const {
  std::lock_guard<std::mutex> lock(mutex_);
  out = explain_evaluation(state_.policy, decision.evaluation, options);
  out += "decision: id=" + decision.id.to_string() + " raw=" + to_string(decision.raw_outcome) +
         " effective=" + to_string(decision.effective_outcome) +
         " authorized=" + (decision.new_oversubscription_authorized ? "true" : "false") +
         " increment=" + std::to_string(decision.authorized_increment_units) +
         " fenced=" + (decision.fenced ? "true" : "false") +
         " confirmations=" + std::to_string(decision.confirmations) + "\n";
  out += "decision_digest: " + decision.decision_digest.to_hex() + "\n";
  return Status::success();
}

Status Governor::history(std::vector<AuditRecord>& out, std::size_t limit) const {
  std::lock_guard<std::mutex> lock(mutex_);
  if (!open_) return fail(StatusCode::InvalidArgument, ReasonCode::PersistenceNotInitialized, "not open");
  out.clear();
  const std::size_t count = std::min(limit, state_.audit.size());
  out.reserve(count);
  for (std::size_t i = state_.audit.size() - count; i < state_.audit.size(); ++i) {
    out.push_back(state_.audit[i]);
  }
  return Status::success();
}

Status Governor::evidence_generations(OversubscriptionDomainId domain, u64& capacity_generation,
                                      u64& reservation_generation, u64& admission_generation) const {
  std::lock_guard<std::mutex> lock(mutex_);
  if (!open_) return fail(StatusCode::InvalidArgument, ReasonCode::PersistenceNotInitialized, "not open");
  capacity_generation = 0;
  reservation_generation = 0;
  admission_generation = 0;
  const auto read = [&](EvidenceKind kind) -> u64 {
    const auto it = slot_index_.find(slot_key(domain, kind));
    if (it == slot_index_.end()) return 0;
    return slots_[it->second].state.generation;
  };
  capacity_generation = read(EvidenceKind::Capacity);
  reservation_generation = read(EvidenceKind::Reservation);
  admission_generation = read(EvidenceKind::Admission);
  return Status::success();
}

Status Governor::publisher_sequence(PublisherId publisher, u64& sequence) const {
  std::lock_guard<std::mutex> lock(mutex_);
  sequence = 0;
  for (const auto& registration : state_.publishers) {
    if (registration.publisher == publisher) {
      sequence = registration.last_sequence;
      return Status::success();
    }
  }
  return fail(StatusCode::NotFound, ReasonCode::PublisherNotRegistered, "publisher is not registered");
}

Status Governor::domain_state(OversubscriptionDomainId domain, DomainRuntimeState& out) const {
  std::lock_guard<std::mutex> lock(mutex_);
  if (!open_) return fail(StatusCode::InvalidArgument, ReasonCode::PersistenceNotInitialized, "not open");
  for (const auto& entry : state_.domains) {
    if (entry.domain == domain) {
      out = entry;
      return Status::success();
    }
  }
  return fail(StatusCode::NotFound, ReasonCode::DomainNotInPolicy, "no runtime state for the domain");
}

GovernorStats Governor::stats() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return stats_;
}

}  // namespace oversub
