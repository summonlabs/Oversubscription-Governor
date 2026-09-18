// Oversubscription Governor — the governance runtime.
//
// The runtime owns durable authority: policy, domain configuration, publisher
// registrations, evidence generations, fencing, hysteresis, audit history, and
// the fabric epoch. It binds every decision to the exact generations and epoch
// that justified it, and refuses to grant authority from stale, missing, or
// contradictory evidence.
//
// Threading: a single state mutex protects all runtime state, and a separate
// queue mutex protects the pending-job queue. The two are never held at the same
// time, and no user code is invoked while either is held.
#ifndef OVERSUB_GOVERNOR_HPP
#define OVERSUB_GOVERNOR_HPP

#include <atomic>
#include <condition_variable>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "oversub/clock.hpp"
#include "oversub/engine.hpp"
#include "oversub/persistence.hpp"
#include "oversub/state.hpp"

namespace oversub {

struct GovernorOptions {
  std::string state_directory;            // durable directory (required)
  u32 worker_threads{0};                  // 0 = synchronous evaluation only
  u32 max_pending_jobs{64};
  u64 journal_compaction_records{4096};   // newest records kept when compacting
  u64 save_state_every_decisions{256};    // bounded state-file lag behind the journal
  bool auto_assign_generations{true};     // accept generation 0 as "assign the next one"
  u64 max_state_bytes{kDefaultMaxStateBytes};
  u64 max_journal_bytes{kDefaultMaxJournalBytes};
  u64 max_journal_records{kDefaultMaxJournalRecords};
  u32 max_audit_records{kMaxAuditRecords};
  bool sync_writes{true};
};

struct Decision {
  DecisionId id;
  EvaluationResult evaluation;
  Outcome raw_outcome{Outcome::Unknown};
  Outcome effective_outcome{Outcome::Unknown};
  bool new_oversubscription_authorized{false};
  u64 authorized_increment_units{0};
  bool fenced{false};
  u32 confirmations{0};
  u64 issued_tick{0};
  IncarnationId incarnation;
  FabricEpoch epoch;
  Digest decision_digest;
};

struct RevalidationReport {
  bool valid{false};
  ReasonCode reason{ReasonCode::None};
  std::string detail;
};

struct GovernorStats {
  u64 evaluations{0};
  u64 decisions{0};
  u64 evidence_accepted{0};
  u64 evidence_duplicates{0};
  u64 evidence_rejected{0};
  u64 journal_appends{0};
  u64 journal_compactions{0};
  u64 state_saves{0};
  u64 cancellations{0};
  u64 epoch_advances{0};
  u64 policy_installs{0};
  u64 revalidations{0};
  u64 revalidations_failed{0};
};

struct IngestOutcome {
  Status status;
  u64 generation{0};
  bool duplicate{false};
};

struct EvaluationRequest {
  OversubscriptionDomainId domain;
  u64 tick{0};   // 0 => use the runtime clock
};

// A submitted evaluation. Cancelled work never carries a decision and never
// mutates authoritative state.
class EvaluationJob {
 public:
  bool ready() const;
  bool cancelled() const;
  bool cancel();
  Status get(Decision& out);

 private:
  friend class Governor;
  void finish(const Status& status, const Decision& decision);
  mutable std::mutex mutex_;
  std::condition_variable cv_;
  bool done_{false};
  bool cancel_requested_{false};
  std::atomic<bool> cancel_flag_{false};
  Status status_{};
  Decision decision_{};
};

class Governor {
 public:
  Governor();
  ~Governor();
  Governor(const Governor&) = delete;
  Governor& operator=(const Governor&) = delete;

  // Opens durable state. On fresh state the supplied policy becomes the initial
  // durable policy. Returns an error (never a silent reset) when durable state
  // is corrupt or fails validation.
  Status open(const GovernorOptions& options, OversubscriptionPolicy initial_policy);

  // Stops accepting work, drains in-flight work, persists, and closes the store.
  Status close();
  bool is_open() const { return open_; }

  // ------------------------------------------------------------ configuration
  Status install_policy(const OversubscriptionPolicy& policy, u64 tick);
  Status advance_epoch(FabricEpoch next);
  Status advance_tick(u64 delta);
  Status save();

  // Returned by value: runtime configuration can be replaced by another thread
  // at any time, so no reference into mutable state is ever handed out.
  OversubscriptionPolicy policy() const;
  PolicyGeneration policy_generation() const;
  FabricEpoch epoch() const;
  IncarnationId incarnation() const;
  u64 tick() const;

  // ---------------------------------------------------------------- publishers
  Status register_publisher(PublisherId publisher, IncarnationId incarnation, FabricEpoch epoch, u64 tick);
  Status adopt_incarnation(PublisherId publisher, IncarnationId incarnation, FabricEpoch epoch, u64 tick);
  Status fence_publisher(PublisherId publisher, ReasonCode reason);

  // ------------------------------------------------------------------ evidence
  IngestOutcome ingest_capacity(const CapacitySnapshot& snapshot);
  IngestOutcome ingest_reservations(const ReservationSnapshot& snapshot);
  IngestOutcome ingest_admissions(const AdmissionSnapshot& snapshot);

  // ----------------------------------------------------------------- governance
  Status evaluate(const EvaluationRequest& request, Decision& out);
  Status submit(const EvaluationRequest& request, std::shared_ptr<EvaluationJob>& out);
  Status revalidate(const Decision& decision, RevalidationReport& report);
  Status explain(const Decision& decision, const ExplainOptions& options, std::string& out) const;

  Status history(std::vector<AuditRecord>& out, std::size_t limit) const;
  // Current authoritative evidence generations for one domain. Publishers use
  // this to claim the next generation explicitly instead of auto-assignment.
  Status evidence_generations(OversubscriptionDomainId domain, u64& capacity_generation,
                              u64& reservation_generation, u64& admission_generation) const;
  // Highest accepted evidence sequence for a publisher. A reconnecting publisher
  // resumes from here so its retransmissions stay idempotent.
  Status publisher_sequence(PublisherId publisher, u64& sequence) const;
  Status domain_state(OversubscriptionDomainId domain, DomainRuntimeState& out) const;
  GovernorStats stats() const;

  // Stops accepting work and drains in-flight work. Idempotent.
  Status shutdown();

 private:
  struct EvidenceSlot {
    EvidenceSlotState state;
    std::shared_ptr<const CapacitySnapshot> capacity;
    std::shared_ptr<const ReservationSnapshot> reservations;
    std::shared_ptr<const AdmissionSnapshot> admissions;
    bool has_content{false};
  };

  struct PendingJob {
    std::shared_ptr<EvaluationJob> job;
    EvaluationRequest request;
  };

  // A publication mutates durable generation bookkeeping. When the durable save
  // fails, the mutation is rolled back so the runtime never acknowledges state
  // that is not durable.
  struct IngestRollback {
    bool has_slot{false};
    EvidenceSlotState slot;
    bool has_registration{false};
    PublisherId publisher;
    u64 sequence{0};
  };

  Status initialize_locked(const GovernorOptions& options, OversubscriptionPolicy initial_policy);
  EvidenceSlot* find_slot(OversubscriptionDomainId domain, EvidenceKind kind);
  EvidenceSlot& ensure_slot(OversubscriptionDomainId domain, EvidenceKind kind);
  void sync_evidence_state_locked();
  void drop_evidence_content_locked(ReasonCode reason);
  Status ingest_locked(EvidenceKind kind, OversubscriptionDomainId domain, u64 claimed_generation,
                       FabricEpoch epoch, u64 observed_tick, const Provenance& provenance,
                       const Digest& payload_digest, u64& out_generation, bool& duplicate);
  IngestRollback capture_rollback_locked(OversubscriptionDomainId domain, EvidenceKind kind, PublisherId publisher);
  void restore_rollback_locked(const IngestRollback& rollback, OversubscriptionDomainId domain,
                               EvidenceKind kind);
  Status append_journal_locked(const AuditRecord& record);
  Status persist_state_locked(bool force);
  Status compact_journal_locked();
  Status evaluate_locked(const EvaluationRequest& request, Decision& out, const std::atomic<bool>* cancel);
  void worker_loop();
  DomainRuntimeState* find_domain_state_locked(OversubscriptionDomainId domain);

  GovernorOptions options_;
  mutable std::mutex mutex_;   // protects every field below
  bool open_{false};
  bool accepting_{false};
  DurableState state_;
  Store store_;
  PolicyIndex index_;
  std::vector<EvidenceSlot> slots_;
  std::map<std::pair<u64, u32>, std::size_t> slot_index_;
  LogicalClock clock_;
  GovernorStats stats_;
  u64 decisions_since_save_{0};
  bool state_dirty_{false};

  std::mutex queue_mutex_;
  std::condition_variable queue_cv_;
  std::deque<PendingJob> queue_;
  std::vector<std::thread> workers_;
  bool shutting_down_{false};
};

// Self-contained decision explanation (does not require the policy).
std::string explain_decision(const Decision& decision, const ExplainOptions& options);

}  // namespace oversub

#endif  // OVERSUB_GOVERNOR_HPP
