// Persistence, restart, recovery, and durability tests.
#include <string>
#include <vector>

#include "framework.hpp"
#include "oversub/persistence.hpp"
#include "oversub/synth.hpp"
#include "testutil.hpp"

using namespace oversub;
using namespace otest;

namespace {

Scenario durable_scenario() {
  Scenario scenario = Scenario::make(2, 10000, Ratio{2, 1}, 1000);
  scenario.mirror_guarantees();
  scenario.add_demand(ResourceId(1), ServiceClassId(2), DemandKind::Contingent, 2000);
  scenario.stamp(0);
  return scenario;
}

}  // namespace

TEST(persistence_round_trip_preserves_durable_state_and_not_evidence) {
  otest::TempDir dir("round-trip");
  Scenario scenario = durable_scenario();
  Decision decision;
  {
    Governor governor;
    GovernorOptions options;
    options.state_directory = dir.path();
    REQUIRE(governor.open(options, scenario.policy).ok());
    REQUIRE(otest::install_scenario(governor, scenario).ok());
    EvaluationRequest request;
    request.domain = OversubscriptionDomainId(1);
    REQUIRE(governor.evaluate(request, decision).ok());
    CHECK(decision.new_oversubscription_authorized);
    REQUIRE(governor.close().ok());
  }
  {
    Governor governor;
    GovernorOptions options;
    options.state_directory = dir.path();
    REQUIRE(governor.open(options, OversubscriptionPolicy{}).ok());
    CHECK_EQ(governor.policy().domains.size(), std::size_t{1});
    CHECK_EQ(governor.epoch().value(), u64{1});
    CHECK_EQ(governor.policy_generation().value(), u64{1});
    CHECK_EQ(governor.incarnation().value(), u64{2});   // a fresh boot incarnation
    // Durable generations survive; evidence content does not, so the runtime
    // refuses to grant authority until evidence is republished.
    EvaluationRequest request;
    request.domain = OversubscriptionDomainId(1);
    Decision fresh;
    REQUIRE(governor.evaluate(request, fresh).ok());
    CHECK_EQ(fresh.evaluation.outcome, Outcome::Unknown);
    CHECK_EQ(fresh.evaluation.reason, ReasonCode::EvidenceIncomplete);
    CHECK(!fresh.new_oversubscription_authorized);
    // Live authority from the previous incarnation is never restored.
    RevalidationReport report;
    REQUIRE(governor.revalidate(decision, report).ok());
    CHECK(!report.valid);
    CHECK_EQ(report.reason, ReasonCode::DecisionStale);
    REQUIRE(governor.close().ok());
  }
}

TEST(persistence_hysteresis_and_fencing_survive_restart) {
  otest::TempDir dir("hysteresis-restart");
  Scenario scenario = durable_scenario();
  scenario.admissions.records.back().committed_capacity = 100000;   // force an emergency
  scenario.stamp(0);
  {
    Governor governor;
    GovernorOptions options;
    options.state_directory = dir.path();
    REQUIRE(governor.open(options, scenario.policy).ok());
    REQUIRE(otest::install_scenario(governor, scenario).ok());
    EvaluationRequest request;
    request.domain = OversubscriptionDomainId(1);
    Decision decision;
    REQUIRE(governor.evaluate(request, decision).ok());
    CHECK_EQ(decision.effective_outcome, Outcome::EmergencyReduction);
    CHECK(decision.fenced);
    REQUIRE(governor.close().ok());
  }
  {
    Governor governor;
    GovernorOptions options;
    options.state_directory = dir.path();
    REQUIRE(governor.open(options, OversubscriptionPolicy{}).ok());
    DomainRuntimeState state;
    REQUIRE(governor.domain_state(OversubscriptionDomainId(1), state).ok());
    CHECK(state.fenced);
    CHECK_EQ(state.sticky_outcome, Outcome::EmergencyReduction);
    // The fence is durable, and it is reflected in the journal-rebuilt state.
    std::vector<AuditRecord> history;
    REQUIRE(governor.history(history, 10).ok());
    CHECK(!history.empty());
    CHECK(history.back().fenced);
    REQUIRE(governor.close().ok());
  }
}

TEST(persistence_journal_replay_rebuilds_runtime_state) {
  DurableState state;
  state.epoch = FabricEpoch(3);
  state.decisions_issued = 0;
  std::vector<AuditRecord> records;
  for (u64 i = 1; i <= 5; ++i) {
    AuditRecord record;
    record.id = AuditRecordId(i);
    record.tick = i * 10;
    record.epoch = FabricEpoch(3);
    record.domain = OversubscriptionDomainId(1);
    record.domain_generation = DomainGeneration(1);
    record.policy_generation = PolicyGeneration(1);
    record.raw_outcome = i >= 4 ? Outcome::ReductionRequired : Outcome::WithinPolicy;
    record.effective_outcome = record.raw_outcome;
    record.reason = i >= 4 ? ReasonCode::AdmissionExceedsContingentCeiling : ReasonCode::None;
    record.decision = DecisionId(i);
    record.decision_digest = sha256(std::to_string(i));
    record.fenced = i >= 4;
    record.confirmations = 1;
    record.last_escalation_tick = i >= 4 ? i * 10 : 0;
    records.push_back(record);
  }
  ReplayReport report;
  REQUIRE(rebuild_from_audit(state, records, report).ok());
  CHECK_EQ(report.records_applied, u64{5});
  REQUIRE(state.domains.size() == 1);
  CHECK_EQ(state.domains[0].sticky_outcome, Outcome::ReductionRequired);
  CHECK(state.domains[0].fenced);
  CHECK_EQ(state.decisions_issued, u64{5});
  CHECK_EQ(state.tick, u64{50});

  // Out-of-order records are skipped rather than silently mis-applied.
  std::vector<AuditRecord> reordered{records[1], records[0]};
  DurableState second;
  ReplayReport second_report;
  REQUIRE(rebuild_from_audit(second, reordered, second_report).ok());
  CHECK_EQ(second_report.records_skipped, u64{1});
  CHECK_EQ(second_report.last_reason, ReasonCode::EvidenceSequenceRegression);
}

TEST(persistence_rejects_corrupt_and_truncated_state) {
  otest::TempDir dir("corrupt");
  Scenario scenario = durable_scenario();
  {
    Governor governor;
    GovernorOptions options;
    options.state_directory = dir.path();
    REQUIRE(governor.open(options, scenario.policy).ok());
    REQUIRE(otest::install_scenario(governor, scenario).ok());
    REQUIRE(governor.close().ok());
  }
  const std::string state_path = dir.file("state.bin");
  const u64 size = otest::file_size(state_path);
  REQUIRE(size > 64);
  std::vector<u8> original;
  REQUIRE(read_file_bounded(state_path, 1 << 20, original).ok());

  // A flipped payload byte breaks the integrity check.
  {
    std::vector<u8> corrupted = original;
    corrupted[corrupted.size() / 2] ^= 0xFF;
    REQUIRE(write_file_atomic(state_path, corrupted, true).ok());
    Governor governor;
    GovernorOptions options;
    options.state_directory = dir.path();
    const Status status = governor.open(options, scenario.policy);
    CHECK(!status.ok());
    CHECK(status.code == StatusCode::IntegrityFailure || status.code == StatusCode::CorruptData);
  }
  // A truncated file is rejected.
  {
    REQUIRE(write_file_atomic(state_path, std::vector<u8>(original.begin(), original.begin() + 32), true).ok());
    Governor governor;
    GovernorOptions options;
    options.state_directory = dir.path();
    const Status status = governor.open(options, scenario.policy);
    CHECK(!status.ok());
    CHECK(status.code == StatusCode::CorruptData || status.code == StatusCode::IoError);
  }
  // A wrong magic is rejected.
  {
    std::vector<u8> corrupted = original;
    corrupted[0] = 'X';
    REQUIRE(write_file_atomic(state_path, corrupted, true).ok());
    Governor governor;
    GovernorOptions options;
    options.state_directory = dir.path();
    CHECK(!governor.open(options, scenario.policy).ok());
  }
  // A future format version is rejected as unsupported.
  {
    std::vector<u8> corrupted = original;
    corrupted[8] = 9;
    corrupted[9] = 0;
    corrupted[10] = 0;
    corrupted[11] = 0;
    REQUIRE(write_file_atomic(state_path, corrupted, true).ok());
    Governor governor;
    GovernorOptions options;
    options.state_directory = dir.path();
    const Status status = governor.open(options, scenario.policy);
    CHECK(!status.ok());
    CHECK_EQ(status.code, StatusCode::Unsupported);
  }
  // Restore and confirm the state still loads.
  {
    REQUIRE(write_file_atomic(state_path, original, true).ok());
    Governor governor;
    GovernorOptions options;
    options.state_directory = dir.path();
    CHECK(governor.open(options, OversubscriptionPolicy{}).ok());
    CHECK(governor.close().ok());
  }
}

TEST(persistence_rejects_an_invalid_durable_policy) {
  otest::TempDir dir("invalid-policy");
  Scenario scenario = durable_scenario();
  DurableState state;
  state.incarnation = IncarnationId(1);
  state.epoch = FabricEpoch(1);
  state.policy = scenario.policy;
  state.policy.domains[0].configured_ratio = Ratio{1, 0};   // invalid
  compute_policy_digest(state.policy);
  state.policy_generation = state.policy.generation;
  state.policy_digest = state.policy.digest;

  Store store;
  StoreOptions options;
  options.directory = dir.path();
  options.sync_writes = false;
  REQUIRE(store.open(options).ok());
  REQUIRE(store.save(state).ok());
  DurableState loaded;
  LoadReport report;
  const Status status = store.load(loaded, report);
  CHECK(!status.ok());
  CHECK_EQ(status.code, StatusCode::CorruptData);
}

TEST(persistence_recovers_a_truncated_journal_tail) {
  otest::TempDir dir("journal-tail");
  Scenario scenario = durable_scenario();
  {
    Governor governor;
    GovernorOptions options;
    options.state_directory = dir.path();
    REQUIRE(governor.open(options, scenario.policy).ok());
    REQUIRE(otest::install_scenario(governor, scenario).ok());
    EvaluationRequest request;
    request.domain = OversubscriptionDomainId(1);
    for (int i = 0; i < 3; ++i) {
      Decision decision;
      REQUIRE(governor.evaluate(request, decision).ok());
    }
    REQUIRE(governor.close().ok());
  }
  const std::string journal_path = dir.file("journal.bin");
  const u64 before = otest::file_size(journal_path);
  REQUIRE(before > 40);
  // Simulate a crash in the middle of a journal append.
  otest::truncate_file(journal_path, before - 17);
  const u64 truncated = otest::file_size(journal_path);
  CHECK(truncated < before);
  {
    Store store;
    StoreOptions options;
    options.directory = dir.path();
    options.sync_writes = false;
    REQUIRE(store.open(options).ok());
    std::vector<AuditRecord> records;
    LoadReport report;
    REQUIRE(store.load_journal(records, report).ok());
    CHECK_EQ(report.outcome, LoadOutcome::Recovered);
    CHECK_EQ(report.reason, ReasonCode::PersistenceJournalPartial);
    CHECK_EQ(records.size(), std::size_t{2});
  }
  {
    Governor governor;
    GovernorOptions options;
    options.state_directory = dir.path();
    REQUIRE(governor.open(options, OversubscriptionPolicy{}).ok());
    std::vector<AuditRecord> history;
    REQUIRE(governor.history(history, 64).ok());
    CHECK_EQ(history.size(), std::size_t{2});   // the incomplete record is dropped
    CHECK_EQ(history[0].id.value(), u64{1});
    CHECK_EQ(history[1].id.value(), u64{2});
    REQUIRE(governor.close().ok());
  }
  // Recovery rewrote the journal with exactly the valid prefix, so later appends
  // are never placed behind an unreadable frame.
  CHECK(otest::file_size(journal_path) < before);
}

TEST(persistence_recovers_from_a_corrupt_journal_frame) {
  otest::TempDir dir("journal-corrupt");
  Scenario scenario = durable_scenario();
  {
    Governor governor;
    GovernorOptions options;
    options.state_directory = dir.path();
    REQUIRE(governor.open(options, scenario.policy).ok());
    REQUIRE(otest::install_scenario(governor, scenario).ok());
    EvaluationRequest request;
    request.domain = OversubscriptionDomainId(1);
    for (int i = 0; i < 2; ++i) {
      Decision decision;
      REQUIRE(governor.evaluate(request, decision).ok());
    }
    REQUIRE(governor.close().ok());
  }
  const std::string journal_path = dir.file("journal.bin");
  std::vector<u8> bytes;
  REQUIRE(read_file_bounded(journal_path, 1 << 20, bytes).ok());
  // Corrupt a payload byte in the first frame: the valid prefix ends there and
  // no journal record is applied.
  std::vector<u8> corrupted = bytes;
  REQUIRE(corrupted.size() > 24);
  corrupted[24] ^= 0x5A;
  REQUIRE(write_file_atomic(journal_path, corrupted, true).ok());
  {
    Store store;
    StoreOptions options;
    options.directory = dir.path();
    options.sync_writes = false;
    REQUIRE(store.open(options).ok());
    std::vector<AuditRecord> records;
    LoadReport report;
    REQUIRE(store.load_journal(records, report).ok());
    CHECK_EQ(report.outcome, LoadOutcome::Recovered);
    CHECK_EQ(records.size(), std::size_t{0});
    CHECK_EQ(report.discarded_tail_bytes, corrupted.size());
  }
  // The runtime still opens: the durable state file is the compacted truth.
  {
    Governor governor;
    GovernorOptions options;
    options.state_directory = dir.path();
    REQUIRE(governor.open(options, OversubscriptionPolicy{}).ok());
    REQUIRE(governor.close().ok());
  }
}

TEST(persistence_enforces_journal_bounds_and_compacts) {
  otest::TempDir dir("journal-bound");
  Scenario scenario = durable_scenario();
  Governor governor;
  GovernorOptions options;
  options.state_directory = dir.path();
  options.max_journal_bytes = 4096;
  options.journal_compaction_records = 4;
  options.max_audit_records = 16;
  options.save_state_every_decisions = 1;
  options.sync_writes = false;
  REQUIRE(governor.open(options, scenario.policy).ok());
  REQUIRE(otest::install_scenario(governor, scenario).ok());
  EvaluationRequest request;
  request.domain = OversubscriptionDomainId(1);
  for (int i = 0; i < 60; ++i) {
    Decision decision;
    REQUIRE(governor.evaluate(request, decision).ok());
  }
  const GovernorStats stats = governor.stats();
  CHECK(stats.journal_compactions > 0);
  CHECK(otest::file_size(dir.file("journal.bin")) <= options.max_journal_bytes);
  std::vector<AuditRecord> history;
  REQUIRE(governor.history(history, 64).ok());
  CHECK(history.size() <= options.max_audit_records);
  CHECK_EQ(history.back().id.value(), u64{60});
  REQUIRE(governor.close().ok());

  // The compacted journal still recovers the newest decision.
  Governor reopened;
  REQUIRE(reopened.open(options, OversubscriptionPolicy{}).ok());
  DomainRuntimeState state;
  REQUIRE(reopened.domain_state(OversubscriptionDomainId(1), state).ok());
  CHECK_EQ(state.last_decision.value(), u64{60});
  REQUIRE(reopened.close().ok());
}

TEST(persistence_state_file_is_written_atomically) {
  otest::TempDir dir("atomic");
  Scenario scenario = durable_scenario();
  Store store;
  StoreOptions options;
  options.directory = dir.path();
  REQUIRE(store.open(options).ok());
  DurableState state;
  state.incarnation = IncarnationId(1);
  state.epoch = FabricEpoch(1);
  state.policy = scenario.policy;
  compute_policy_digest(state.policy);
  state.policy_generation = state.policy.generation;
  state.policy_digest = state.policy.digest;
  REQUIRE(store.save(state).ok());
  CHECK(otest::exists(dir.file("state.bin")));
  CHECK(!otest::exists(dir.file("state.bin.tmp")));
  DurableState loaded;
  LoadReport report;
  REQUIRE(store.load(loaded, report).ok());
  CHECK_EQ(report.outcome, LoadOutcome::Loaded);
  CHECK_EQ(loaded.incarnation.value(), u64{1});
}

TEST(persistence_fresh_directory_reports_fresh) {
  otest::TempDir dir("fresh");
  Store store;
  StoreOptions options;
  options.directory = dir.path();
  REQUIRE(store.open(options).ok());
  DurableState state;
  LoadReport report;
  REQUIRE(store.load(state, report).ok());
  CHECK_EQ(report.outcome, LoadOutcome::Fresh);
  CHECK_EQ(report.reason, ReasonCode::PersistenceNotInitialized);
}

TEST(persistence_rejects_oversized_files) {
  otest::TempDir dir("oversized");
  std::vector<u8> big(2048, 0xAB);
  REQUIRE(write_file_atomic(dir.file("state.bin"), big, false).ok());
  std::vector<u8> bytes;
  const Status status = read_file_bounded(dir.file("state.bin"), 128, bytes);
  CHECK(!status.ok());
  CHECK_EQ(status.code, StatusCode::OutOfRange);
}
