// Concurrency, cancellation, and shutdown tests.
//
// These tests contain no timeouts: every wait is a predicate wait that must be
// satisfied by the runtime. A hang is a defect, not a slow test.
#include <atomic>
#include <string>
#include <thread>
#include <vector>

#include "framework.hpp"
#include "oversub/synth.hpp"
#include "testutil.hpp"

using namespace oversub;
using namespace otest;

TEST(concurrency_parallel_evaluations_are_serialized_correctly) {
  otest::GovernorFixture fixture("parallel-eval");
  Scenario scenario = Scenario::make(2, 10000, Ratio{2, 1}, 1000);
  scenario.mirror_guarantees();
  scenario.add_demand(ResourceId(1), ServiceClassId(2), DemandKind::Contingent, 1000);
  scenario.add_demand(ResourceId(2), ServiceClassId(2), DemandKind::Contingent, 1000);
  scenario.stamp(0);
  REQUIRE(fixture.open(scenario.policy).ok());
  REQUIRE(otest::install_scenario(fixture.governor(), scenario).ok());

  constexpr int kThreads = 8;
  constexpr int kPerThread = 25;
  std::atomic<int> failures{0};
  std::atomic<int> successes{0};
  std::vector<std::thread> threads;
  threads.reserve(kThreads);
  for (int t = 0; t < kThreads; ++t) {
    threads.emplace_back([&, t] {
      for (int i = 0; i < kPerThread; ++i) {
        EvaluationRequest request;
        request.domain = OversubscriptionDomainId(1);
        // The tick is held inside the policy's freshness budget so that the
        // evaluation is about contention, not staleness.
        request.tick = 0;
        Decision decision;
        const Status status = fixture.governor().evaluate(request, decision);
        if (!status.ok()) {
          failures.fetch_add(1);
          continue;
        }
        if (decision.evaluation.outcome != Outcome::WithinPolicy) failures.fetch_add(1);
        successes.fetch_add(1);
      }
    });
  }
  for (auto& thread : threads) thread.join();
  CHECK_EQ(failures.load(), 0);
  CHECK_EQ(successes.load(), kThreads * kPerThread);
  const GovernorStats stats = fixture.governor().stats();
  CHECK_EQ(stats.decisions, static_cast<u64>(kThreads * kPerThread));
}

TEST(concurrency_concurrent_evidence_publication_stays_consistent) {
  otest::GovernorFixture fixture("parallel-publish");
  Scenario scenario = Scenario::make(1, 10000, Ratio{2, 1}, 1000);
  scenario.mirror_guarantees();
  scenario.stamp(0);
  REQUIRE(fixture.open(scenario.policy).ok());
  REQUIRE(fixture.governor().install_policy(scenario.policy, 0).ok());
  REQUIRE(fixture.governor().register_publisher(PublisherId(1), IncarnationId(1), FabricEpoch(1), 0).ok());

  constexpr int kThreads = 6;
  std::atomic<int> accepted{0};
  std::atomic<int> rejected{0};
  std::vector<std::thread> threads;
  for (int t = 0; t < kThreads; ++t) {
    threads.emplace_back([&, t] {
      CapacitySnapshot snapshot = scenario.capacity;
      snapshot.generation = CapacitySnapshotGeneration(0);   // auto-assigned
      snapshot.provenance.sequence = static_cast<u64>(t + 1);
      snapshot.provenance.evidence_id = EvidenceId(static_cast<u64>(1000 + t));
      snapshot.provenance.payload_digest = capacity_snapshot_digest(snapshot);
      const IngestOutcome outcome = fixture.governor().ingest_capacity(snapshot);
      if (outcome.status.ok() && !outcome.duplicate) {
        accepted.fetch_add(1);
      } else {
        rejected.fetch_add(1);
      }
    });
  }
  for (auto& thread : threads) thread.join();
  CHECK_EQ(accepted.load() + rejected.load(), kThreads);
  // At least one publication wins; no publication may corrupt the generation.
  CHECK(accepted.load() >= 1);
  u64 capacity_generation = 0;
  u64 reservation_generation = 0;
  u64 admission_generation = 0;
  REQUIRE(fixture.governor()
              .evidence_generations(OversubscriptionDomainId(1), capacity_generation, reservation_generation,
                                    admission_generation)
              .ok());
  CHECK(capacity_generation >= 1);
}

TEST(concurrency_exact_duplicate_publications_are_idempotent) {
  otest::GovernorFixture fixture("duplicate-publish");
  Scenario scenario = Scenario::make(1, 1000, Ratio{2, 1}, 100);
  scenario.mirror_guarantees();
  scenario.stamp(0);
  REQUIRE(fixture.open(scenario.policy).ok());
  REQUIRE(fixture.governor().install_policy(scenario.policy, 0).ok());
  REQUIRE(fixture.governor().register_publisher(PublisherId(1), IncarnationId(1), FabricEpoch(1), 0).ok());

  constexpr int kThreads = 4;
  std::atomic<int> duplicates{0};
  std::atomic<int> accepted{0};
  std::vector<std::thread> threads;
  for (int t = 0; t < kThreads; ++t) {
    threads.emplace_back([&] {
      const IngestOutcome outcome = fixture.governor().ingest_capacity(scenario.capacity);
      if (!outcome.status.ok()) return;
      if (outcome.duplicate) {
        duplicates.fetch_add(1);
      } else {
        accepted.fetch_add(1);
      }
    });
  }
  for (auto& thread : threads) thread.join();
  CHECK_EQ(accepted.load(), 1);
  CHECK_EQ(duplicates.load() + accepted.load(), kThreads);
  const GovernorStats stats = fixture.governor().stats();
  CHECK_EQ(stats.evidence_duplicates, static_cast<u64>(kThreads - 1));
}

TEST(concurrency_policy_install_racing_evaluations_never_grants_stale_authority) {
  otest::GovernorFixture fixture("policy-race");
  Scenario scenario = Scenario::make(1, 10000, Ratio{2, 1}, 1000);
  scenario.mirror_guarantees();
  scenario.add_demand(ResourceId(1), ServiceClassId(2), DemandKind::Contingent, 1000);
  scenario.stamp(0);
  REQUIRE(fixture.open(scenario.policy).ok());
  REQUIRE(otest::install_scenario(fixture.governor(), scenario).ok());

  std::atomic<bool> stop{false};
  std::atomic<int> grants{0};
  std::atomic<int> denials{0};
  std::thread evaluator([&] {
    while (!stop.load()) {
      EvaluationRequest request;
      request.domain = OversubscriptionDomainId(1);
      Decision decision;
      if (!fixture.governor().evaluate(request, decision).ok()) continue;
      if (decision.new_oversubscription_authorized) {
        grants.fetch_add(1);
        // Authority that was granted must be bound to the generation that
        // produced it, and it must be revalidatable at that moment.
        RevalidationReport report;
        if (fixture.governor().revalidate(decision, report).ok() && !report.valid &&
            decision.evaluation.authority.policy_generation.value() ==
                fixture.governor().policy_generation().value()) {
          // Only a superseding decision may invalidate it here.
          CHECK(report.reason == ReasonCode::DecisionSuperseded);
        }
      } else {
        denials.fetch_add(1);
      }
    }
  });

  // The evaluator thread holds references to these locals, so it must be joined
  // before the test returns for any reason.
  std::string install_error;
  for (int i = 0; i < 5 && install_error.empty(); ++i) {
    Scenario changed = scenario;
    changed.domain().configured_ratio = i % 2 == 0 ? Ratio{3, 2} : Ratio{2, 1};
    changed.domain().emergency_reserve_units = static_cast<u64>(i * 10);
    // Generation 0 asks the runtime to assign the next generation.
    changed.capacity.generation = CapacitySnapshotGeneration(0);
    changed.reservations.generation = ReservationSnapshotGeneration(0);
    changed.admissions.generation = AdmissionSnapshotGeneration(0);
    changed.stamp(static_cast<u64>(i + 1));
    const Status policy_status = fixture.governor().install_policy(changed.policy, 0);
    if (!policy_status.ok()) {
      install_error = "install_policy: " + policy_status.to_string();
      break;
    }
    const IngestOutcome capacity_outcome = fixture.governor().ingest_capacity(changed.capacity);
    if (!capacity_outcome.status.ok()) {
      install_error = "ingest_capacity: " + capacity_outcome.status.to_string();
      break;
    }
    const IngestOutcome reservation_outcome = fixture.governor().ingest_reservations(changed.reservations);
    if (!reservation_outcome.status.ok()) {
      install_error = "ingest_reservations: " + reservation_outcome.status.to_string();
      break;
    }
    const IngestOutcome admission_outcome = fixture.governor().ingest_admissions(changed.admissions);
    if (!admission_outcome.status.ok()) {
      install_error = "ingest_admissions: " + admission_outcome.status.to_string();
      break;
    }
    // The publisher's sequence and evidence ids keep advancing between rounds.
    scenario.sequence = changed.sequence;
  }
  stop.store(true);
  evaluator.join();
  CHECK_EQ(install_error, std::string());
  CHECK(grants.load() + denials.load() > 0);
}

TEST(concurrency_cancelled_work_never_mutates_state) {
  otest::GovernorFixture fixture("cancel");
  Scenario scenario = Scenario::make(1, 10000, Ratio{2, 1}, 1000);
  scenario.mirror_guarantees();
  scenario.stamp(0);
  REQUIRE(fixture.open(scenario.policy, 4).ok());
  REQUIRE(otest::install_scenario(fixture.governor(), scenario).ok());
  const u64 before = fixture.governor().stats().decisions;

  std::vector<std::shared_ptr<EvaluationJob>> jobs;
  for (int i = 0; i < 16; ++i) {
    std::shared_ptr<EvaluationJob> job;
    EvaluationRequest request;
    request.domain = OversubscriptionDomainId(1);
    REQUIRE(fixture.governor().submit(request, job).ok());
    job->cancel();
    jobs.push_back(job);
  }
  u64 cancelled = 0;
  u64 completed = 0;
  for (auto& job : jobs) {
    Decision decision;
    const Status status = job->get(decision);
    if (status.code == StatusCode::Cancelled) {
      ++cancelled;
      CHECK(job->cancelled());
    } else {
      CHECK(status.ok());
      ++completed;
    }
  }
  CHECK_EQ(cancelled + completed, u64{16});
  const u64 after = fixture.governor().stats().decisions;
  CHECK_EQ(after - before, completed);
  for (auto& job : jobs) {
    if (!job->cancelled()) continue;
    Decision decision;
    CHECK_EQ(job->get(decision).code, StatusCode::Cancelled);
  }
}

TEST(concurrency_shutdown_drains_and_never_hangs_a_waiter) {
  otest::GovernorFixture fixture("shutdown");
  Scenario scenario = Scenario::make(1, 10000, Ratio{2, 1}, 1000);
  scenario.mirror_guarantees();
  scenario.stamp(0);
  REQUIRE(fixture.open(scenario.policy, 4).ok());
  REQUIRE(otest::install_scenario(fixture.governor(), scenario).ok());

  std::vector<std::shared_ptr<EvaluationJob>> jobs;
  for (int i = 0; i < 32; ++i) {
    std::shared_ptr<EvaluationJob> job;
    EvaluationRequest request;
    request.domain = OversubscriptionDomainId(1);
    REQUIRE(fixture.governor().submit(request, job).ok());
    jobs.push_back(job);
  }
  REQUIRE(fixture.governor().shutdown().ok());
  u64 cancelled = 0;
  u64 completed = 0;
  for (auto& job : jobs) {
    Decision decision;
    const Status status = job->get(decision);   // must never block forever
    CHECK(status.ok() || status.code == StatusCode::Cancelled);
    if (status.code == StatusCode::Cancelled) {
      ++cancelled;
    } else {
      ++completed;
    }
  }
  CHECK_EQ(cancelled + completed, u64{32});
  // After shutdown the runtime accepts no further work.
  EvaluationRequest request;
  request.domain = OversubscriptionDomainId(1);
  Decision decision;
  CHECK_EQ(fixture.governor().evaluate(request, decision).code, StatusCode::ShuttingDown);
  std::shared_ptr<EvaluationJob> late;
  CHECK(!fixture.governor().submit(request, late).ok());
  // Repeated shutdown is idempotent.
  CHECK(fixture.governor().shutdown().ok());
}

TEST(concurrency_pending_job_queue_is_bounded) {
  otest::GovernorFixture fixture("queue-bound");
  Scenario scenario = Scenario::make(1, 10000, Ratio{2, 1}, 1000);
  scenario.mirror_guarantees();
  scenario.stamp(0);
  GovernorOptions options;
  options.state_directory = fixture.directory();
  options.worker_threads = 1;
  options.max_pending_jobs = 2;
  options.sync_writes = false;
  REQUIRE(fixture.governor().open(options, scenario.policy).ok());
  REQUIRE(otest::install_scenario(fixture.governor(), scenario).ok());

  // Submitting more work than the queue can hold is refused deterministically
  // once the bound is reached; accepted work always completes.
  std::vector<std::shared_ptr<EvaluationJob>> jobs;
  u64 refused = 0;
  for (int i = 0; i < 64; ++i) {
    std::shared_ptr<EvaluationJob> job;
    EvaluationRequest request;
    request.domain = OversubscriptionDomainId(1);
    const Status status = fixture.governor().submit(request, job);
    if (!status.ok()) {
      CHECK_EQ(status.code, StatusCode::QueueFull);
      ++refused;
      continue;
    }
    jobs.push_back(job);
  }
  for (auto& job : jobs) {
    Decision decision;
    const Status status = job->get(decision);
    CHECK(status.ok() || status.code == StatusCode::Cancelled);
  }
  CHECK(refused > 0);
  CHECK_EQ(fixture.governor().shutdown().ok(), true);
}

TEST(concurrency_public_api_surface_is_free_of_lock_reentry) {
  // Every public entry point is exercised while evaluations run. A read/write
  // re-entry on the state lock, a nested queue/state acquisition, or a shutdown
  // path that blocks the work it awaits would hang this test.
  otest::GovernorFixture fixture("api-surface");
  Scenario scenario = Scenario::make(2, 10000, Ratio{2, 1}, 1000);
  scenario.mirror_guarantees();
  scenario.stamp(0);
  REQUIRE(fixture.open(scenario.policy, 2).ok());
  REQUIRE(otest::install_scenario(fixture.governor(), scenario).ok());

  std::atomic<bool> stop{false};
  std::atomic<int> errors{0};
  std::thread reader([&] {
    while (!stop.load()) {
      std::vector<AuditRecord> history;
      if (!fixture.governor().history(history, 5).ok()) errors.fetch_add(1);
      DomainRuntimeState state;
      (void)fixture.governor().domain_state(OversubscriptionDomainId(1), state);
      (void)fixture.governor().stats();
      u64 capacity_generation = 0;
      u64 reservation_generation = 0;
      u64 admission_generation = 0;
      if (!fixture.governor()
               .evidence_generations(OversubscriptionDomainId(1), capacity_generation, reservation_generation,
                                     admission_generation)
               .ok()) {
        errors.fetch_add(1);
      }
      if (!fixture.governor().save().ok()) errors.fetch_add(1);
      if (!fixture.governor().advance_tick(1).ok()) errors.fetch_add(1);
    }
  });
  std::thread writer([&] {
    for (int i = 0; i < 60; ++i) {
      EvaluationRequest request;
      request.domain = OversubscriptionDomainId(1);
      Decision decision;
      if (!fixture.governor().evaluate(request, decision).ok()) errors.fetch_add(1);
      ExplainOptions options;
      options.max_bytes = 512;
      std::string text;
      if (!fixture.governor().explain(decision, options, text).ok()) errors.fetch_add(1);
      RevalidationReport report;
      if (!fixture.governor().revalidate(decision, report).ok()) errors.fetch_add(1);
    }
  });
  writer.join();
  stop.store(true);
  reader.join();
  CHECK_EQ(errors.load(), 0);
}
