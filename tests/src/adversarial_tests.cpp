// Adversarial input tests: malformed, contradictory, oversized, and hostile
// evidence must be rejected precisely and must never grant authority.
#include <string>
#include <vector>

#include "framework.hpp"
#include "oversub/synth.hpp"
#include "testutil.hpp"

using namespace oversub;
using namespace otest;

TEST(adversarial_rejects_structurally_invalid_evidence) {
  {
    Scenario scenario = Scenario::make();
    scenario.capacity.resources[0].usable_capacity = 2000;   // > physical
    scenario.stamp(0);
    CHECK_EQ(scenario.validate().reason, ReasonCode::ResourceUsableExceedsPhysical);
  }
  {
    Scenario scenario = Scenario::make();
    scenario.capacity.resources.clear();
    scenario.stamp(0);
    CHECK(scenario.validate().ok());   // an empty snapshot is structurally valid...
    const EvaluationResult result = evaluate_scenario(scenario, 0);
    CHECK_EQ(result.outcome, Outcome::Unknown);   // ...but never authoritative
    CHECK_EQ(result.reason, ReasonCode::ResourceMissingFromCapacitySnapshot);
  }
  {
    Scenario scenario = Scenario::make();
    scenario.reservations.guarantees.push_back(Guarantee{});
    scenario.reservations.guarantees[0].reservation = ReservationId(1);
    scenario.reservations.guarantees[0].resource = ResourceId(1);
    scenario.reservations.guarantees[0].pool = PoolId(1);
    scenario.reservations.guarantees[0].service_class = ServiceClassId(1);
    scenario.reservations.guarantees[0].generation = ReservationGeneration(1);
    scenario.reservations.guarantees[0].preemptible = true;
    scenario.stamp(0);
    CHECK_EQ(scenario.validate().reason, ReasonCode::GuaranteeContradiction);
  }
  {
    Scenario scenario = Scenario::make();
    scenario.admissions.records.push_back(DemandRecord{});
    scenario.admissions.records[0].admission = AdmissionId(1);
    scenario.admissions.records[0].resource = ResourceId(1);
    scenario.admissions.records[0].pool = PoolId(1);
    scenario.admissions.records[0].service_class = ServiceClassId(2);
    scenario.admissions.records[0].generation = AdmissionGeneration(1);
    scenario.admissions.records[0].kind = DemandKind::Unknown;
    scenario.stamp(0);
    CHECK_EQ(scenario.validate().reason, ReasonCode::AdmissionKindConflict);
  }
  {
    CapacitySnapshot snapshot;
    snapshot.domain = OversubscriptionDomainId(1);
    snapshot.generation = CapacitySnapshotGeneration(1);
    snapshot.epoch = FabricEpoch(1);
    snapshot.resources.resize(kHardMaxResourcesPerSnapshot + 1);
    snapshot.provenance.publisher = PublisherId(1);
    snapshot.provenance.incarnation = IncarnationId(1);
    snapshot.provenance.epoch = FabricEpoch(1);
    snapshot.provenance.sequence = 1;
    snapshot.provenance.evidence_id = EvidenceId(1);
    CHECK_EQ(validate_capacity_snapshot(snapshot).reason, ReasonCode::SnapshotTooLarge);
  }
  {
    Scenario scenario = Scenario::make();
    scenario.capacity.provenance.payload_digest = sha256("tampered");
    CHECK_EQ(validate_capacity_snapshot(scenario.capacity).reason, ReasonCode::SnapshotDigestMismatch);
  }
  {
    Scenario scenario = Scenario::make();
    scenario.capacity.epoch = FabricEpoch(0);
    scenario.refresh_digests();
    CHECK_EQ(scenario.validate().reason, ReasonCode::EpochMismatch);
  }
  {
    // Generation zero is the explicit "assign the next generation" request and is
    // only meaningful at ingestion, where the runtime stamps the assigned value.
    Scenario scenario = Scenario::make();
    scenario.capacity.generation = CapacitySnapshotGeneration(0);
    scenario.refresh_digests();
    CHECK(scenario.validate().ok());
    otest::GovernorFixture fixture("assign-generation");
    REQUIRE(fixture.open(scenario.policy).ok());
    REQUIRE(fixture.governor()
                .register_publisher(scenario.publisher, scenario.incarnation, FabricEpoch(1), 0)
                .ok());
    const IngestOutcome outcome = fixture.governor().ingest_capacity(scenario.capacity);
    CHECK(outcome.status.ok());
    CHECK_EQ(outcome.generation, u64{1});
    u64 capacity_generation = 0;
    u64 reservation_generation = 0;
    u64 admission_generation = 0;
    REQUIRE(fixture.governor()
                .evidence_generations(OversubscriptionDomainId(1), capacity_generation,
                                      reservation_generation, admission_generation)
                .ok());
    CHECK_EQ(capacity_generation, u64{1});
  }
  {
    Scenario scenario = Scenario::make();
    scenario.capacity.provenance.publisher = PublisherId(0);
    scenario.refresh_digests();
    CHECK_EQ(scenario.validate().reason, ReasonCode::EvidenceMalformed);
  }
  {
    Scenario scenario = Scenario::make();
    scenario.capacity.resources[0].physical_capacity = kHardMaxCapacityUnits + 1;
    scenario.capacity.resources[0].usable_capacity = kHardMaxCapacityUnits + 1;
    scenario.refresh_digests();
    CHECK_EQ(scenario.validate().reason, ReasonCode::ResourceCapacityInvalid);
  }
}

TEST(adversarial_rejects_out_of_scope_and_unknown_references) {
  {
    Scenario scenario = Scenario::make();
    scenario.domain().members.push_back(ResourceId(99));
    // The extra member has no capacity evidence.
    scenario.reservations.guarantees.clear();
    scenario.admissions.records.clear();
    scenario.stamp(0);
    const EvaluationResult result = evaluate_scenario(scenario, 0);
    CHECK_EQ(result.outcome, Outcome::Unknown);
    CHECK_EQ(result.reason, ReasonCode::ResourceMissingFromCapacitySnapshot);
  }
  {
    Scenario scenario = Scenario::make();
    scenario.add_guarantee(ResourceId(1), ServiceClassId(9), 100);
    scenario.stamp(0);
    const EvaluationResult result = evaluate_scenario(scenario, 0);
    CHECK_EQ(result.outcome, Outcome::ConflictingInput);
    CHECK_EQ(result.reason, ReasonCode::GuaranteeServiceClassUnknown);
  }
  {
    Scenario scenario = Scenario::make();
    scenario.add_demand(ResourceId(1), ServiceClassId(9), DemandKind::Contingent, 100);
    scenario.stamp(0);
    const EvaluationResult result = evaluate_scenario(scenario, 0);
    CHECK_EQ(result.outcome, Outcome::ConflictingInput);
    CHECK_EQ(result.reason, ReasonCode::AdmissionServiceClassUnknown);
  }
  {
    Scenario scenario = Scenario::make();
    scenario.capacity.resources[0].pool = PoolId(77);
    scenario.stamp(0);
    const EvaluationResult result = evaluate_scenario(scenario, 0);
    CHECK_EQ(result.outcome, Outcome::ConflictingInput);
    CHECK_EQ(result.reason, ReasonCode::ResourcePoolMismatch);
  }
  {
    Scenario scenario = Scenario::make(1, 1000, Ratio{2, 1}, 100);
    scenario.mirror_guarantees();
    scenario.add_demand(ResourceId(50), ServiceClassId(2), DemandKind::Contingent, 500);   // out of scope
    scenario.stamp(0);
    const EvaluationResult result = evaluate_scenario(scenario, 0);
    CHECK_EQ(result.outcome, Outcome::WithinPolicy);
    CHECK_EQ(result.accounting.out_of_scope_demand, u64{500});
  }
}

TEST(adversarial_rejects_capacity_beyond_the_policy_bound) {
  Scenario scenario = Scenario::make(1, 1000, Ratio{2, 1}, 0);
  scenario.policy.limits.max_capacity_units = 100;
  scenario.stamp(0);
  const EvaluationResult result = evaluate_scenario(scenario, 0);
  CHECK_EQ(result.outcome, Outcome::ConflictingInput);
  CHECK_EQ(result.reason, ReasonCode::ResourceCapacityInvalid);
}

TEST(adversarial_rejects_contradictory_guarantees_and_overcommit_of_guarantees) {
  {
    Scenario scenario = Scenario::make(1, 1000, Ratio{2, 1}, 900);
    scenario.mirror_guarantees();
    scenario.add_guarantee(ResourceId(1), ServiceClassId(1), 900);   // total 1800 > 1000 usable
    scenario.stamp(0);
    const EvaluationResult result = evaluate_scenario(scenario, 0);
    CHECK_EQ(result.outcome, Outcome::EmergencyReduction);
    CHECK_EQ(result.reason, ReasonCode::GuaranteeSumExceedsUsable);
    CHECK(result.fence_required);
  }
  {
    Scenario scenario = Scenario::make(1, 1000, Ratio{2, 1}, 100);
    scenario.domain().min_protected_headroom_units = 5000;
    scenario.stamp(0);
    const EvaluationResult result = evaluate_scenario(scenario, 0);
    CHECK_EQ(result.outcome, Outcome::PolicyRejected);
    CHECK_EQ(result.reason, ReasonCode::GuaranteeFloorExceedsCeiling);
  }
}

TEST(adversarial_rejects_oversized_snapshots_against_policy_limits) {
  Scenario scenario = Scenario::make(1, 1000, Ratio{2, 1}, 0);
  scenario.policy.limits.max_guarantees_per_snapshot = 2;
  for (int i = 0; i < 5; ++i) scenario.add_guarantee(ResourceId(1), ServiceClassId(1), 10);
  scenario.stamp(0);
  const EvaluationResult result = evaluate_scenario(scenario, 0);
  CHECK_EQ(result.outcome, Outcome::ConflictingInput);
  CHECK_EQ(result.reason, ReasonCode::SnapshotTooLarge);
}

TEST(adversarial_huge_values_do_not_wrap) {
  Scenario scenario = Scenario::make(1, UINT64_MAX / 2, Ratio{4, 1}, UINT64_MAX / 8);
  scenario.mirror_guarantees();
  scenario.add_demand(ResourceId(1), ServiceClassId(2), DemandKind::Contingent, UINT64_MAX / 8);
  scenario.stamp(0);
  const EvaluationResult result = evaluate_scenario(scenario, 0);
  CHECK(result.outcome == Outcome::Unknown || result.accounting.arithmetic_closed);
  if (result.outcome == Outcome::Unknown) {
    CHECK_EQ(result.reason, ReasonCode::ArithmeticOverflow);
    CHECK(!result.authority_valid);
  }
}

TEST(adversarial_governor_rejects_evidence_for_uncovered_domains) {
  otest::GovernorFixture fixture("uncovered-evidence");
  Scenario scenario = Scenario::make();
  scenario.stamp(0);
  REQUIRE(fixture.open(scenario.policy).ok());
  REQUIRE(fixture.governor()
              .register_publisher(PublisherId(1), IncarnationId(1), FabricEpoch(1), 0)
              .ok());
  CapacitySnapshot other = scenario.capacity;
  other.domain = OversubscriptionDomainId(77);
  other.provenance.sequence = 2;
  other.provenance.evidence_id = EvidenceId(77);
  other.provenance.payload_digest = capacity_snapshot_digest(other);
  const IngestOutcome outcome = fixture.governor().ingest_capacity(other);
  CHECK(!outcome.status.ok());
  CHECK_EQ(outcome.status.reason, ReasonCode::DomainNotInPolicy);
}

TEST(adversarial_governor_rejects_unregistered_and_stale_publishers) {
  otest::GovernorFixture fixture("publisher-gate");
  Scenario scenario = Scenario::make();
  scenario.stamp(0);
  REQUIRE(fixture.open(scenario.policy).ok());
  // Unregistered publisher.
  const IngestOutcome unregistered = fixture.governor().ingest_capacity(scenario.capacity);
  CHECK(!unregistered.status.ok());
  CHECK_EQ(unregistered.status.reason, ReasonCode::PublisherNotRegistered);
  REQUIRE(fixture.governor()
              .register_publisher(PublisherId(1), IncarnationId(1), FabricEpoch(1), 0)
              .ok());
  // Stale incarnation.
  CapacitySnapshot stale = scenario.capacity;
  stale.provenance.incarnation = IncarnationId(5);
  stale.provenance.sequence = 10;
  stale.provenance.evidence_id = EvidenceId(11);
  stale.provenance.payload_digest = capacity_snapshot_digest(stale);
  const IngestOutcome stale_outcome = fixture.governor().ingest_capacity(stale);
  CHECK(!stale_outcome.status.ok());
  CHECK_EQ(stale_outcome.status.reason, ReasonCode::PublisherIncarnationStale);
}

TEST(adversarial_decode_rejects_random_bytes) {
  // The bounded codec must never read out of bounds or accept malformed input.
  u64 state = 0x12345678ULL;
  for (int iteration = 0; iteration < 2000; ++iteration) {
    state = state * 6364136223846793005ULL + 1442695040888963407ULL;
    const std::size_t length = static_cast<std::size_t>(state % 200);
    std::vector<u8> buffer(length);
    for (std::size_t i = 0; i < length; ++i) {
      state = state * 6364136223846793005ULL + 1442695040888963407ULL;
      buffer[i] = static_cast<u8>(state >> 33);
    }
    AuditRecord record;
    const Status status = deserialize_audit_record(buffer, record);
    if (status.ok()) {
      // Anything that decodes must re-serialize deterministically.
      std::vector<u8> again;
      CHECK(serialize_audit_record(record, again).ok());
      AuditRecord round_trip;
      CHECK(deserialize_audit_record(again, round_trip).ok());
      CHECK_EQ(round_trip.id.value(), record.id.value());
    }
    DurableState state_value;
    (void)deserialize_state(buffer, state_value);
  }
}

TEST(adversarial_store_and_governor_bounds_are_enforced) {
  otest::TempDir dir("hardening-bounds");
  // A serialized state larger than the configured bound is refused at save time.
  {
    Scenario scenario = Scenario::make();
    compute_policy_digest(scenario.policy);
    DurableState state;
    state.incarnation = IncarnationId(1);
    state.epoch = FabricEpoch(1);
    state.policy = scenario.policy;
    state.policy_generation = scenario.policy.generation;
    state.policy_digest = scenario.policy.digest;
    state.audit.resize(64);
    Store store;
    StoreOptions options;
    options.directory = dir.path();
    options.max_state_bytes = 512;
    options.sync_writes = false;
    REQUIRE(store.open(options).ok());
    const Status status = store.save(state);
    CHECK(!status.ok());
    CHECK_EQ(status.code, StatusCode::OutOfRange);
  }
  // A state header that declares a huge payload is rejected before allocation.
  {
    otest::TempDir other("hardening-header");
    std::vector<u8> file;
    const char magic[8] = {'O', 'S', 'G', 'V', 'S', 'T', 'A', '1'};
    file.insert(file.end(), magic, magic + 8);
    for (int i = 0; i < 4; ++i) file.push_back(1);          // format version 1
    for (int i = 0; i < 4; ++i) file.push_back(0);          // reserved
    for (int i = 0; i < 8; ++i) file.push_back(i == 0 ? 0xFF : 0xFF);   // payload length 2^64-1
    REQUIRE(write_file_atomic(other.file("state.bin"), file, false).ok());
    Store store;
    StoreOptions options;
    options.directory = other.path();
    options.sync_writes = false;
    REQUIRE(store.open(options).ok());
    DurableState state;
    LoadReport report;
    const Status status = store.load(state, report);
    CHECK(!status.ok());
    CHECK(status.code == StatusCode::OutOfRange || status.code == StatusCode::CorruptData);
  }
  // Directories are not readable files.
  {
    std::vector<u8> bytes;
    const Status status = read_file_bounded(dir.path(), 1024, bytes);
    CHECK(!status.ok());
  }
}

TEST(adversarial_governor_rejects_use_after_close_and_tick_overflow) {
  otest::TempDir dir("hardening-lifecycle");
  Scenario scenario = Scenario::make(1, 1000, Ratio{2, 1}, 100);
  scenario.mirror_guarantees();
  scenario.stamp(0);
  Governor governor;
  GovernorOptions options;
  options.state_directory = dir.path();
  REQUIRE(governor.open(options, scenario.policy).ok());
  REQUIRE(otest::install_scenario(governor, scenario).ok());
  // The clock is a logical tick and advances monotonically until it is exhausted.
  CHECK(governor.advance_tick(UINT64_MAX).ok());
  const Status overflow = governor.advance_tick(1);
  CHECK(!overflow.ok());
  CHECK_EQ(overflow.code, StatusCode::Overflow);
  REQUIRE(governor.close().ok());
  EvaluationRequest request;
  request.domain = OversubscriptionDomainId(1);
  Decision decision;
  CHECK(!governor.evaluate(request, decision).ok());
  // Closing twice is safe.
  CHECK(governor.close().ok());
  // Opening the same directory twice with two live governors is refused by the
  // second only for the same instance; a fresh instance adopts the durable state.
  Governor second;
  REQUIRE(second.open(options, OversubscriptionPolicy{}).ok());
  CHECK_EQ(second.incarnation().value(), u64{2});
  CHECK(second.close().ok());
}

TEST(adversarial_state_round_trip_is_stable) {
  DurableState state;
  state.incarnation = IncarnationId(3);
  state.epoch = FabricEpoch(4);
  state.revision = 9;
  Scenario scenario = Scenario::make();
  compute_policy_digest(scenario.policy);
  state.policy = scenario.policy;
  state.policy_generation = scenario.policy.generation;
  state.policy_digest = scenario.policy.digest;
  DomainRuntimeState runtime;
  runtime.domain = OversubscriptionDomainId(1);
  runtime.generation = DomainGeneration(1);
  runtime.sticky_outcome = Outcome::ReductionRequired;
  state.domains.push_back(runtime);
  PublisherRegistration registration;
  registration.publisher = PublisherId(2);
  registration.incarnation = IncarnationId(2);
  registration.epoch = FabricEpoch(4);
  state.publishers.push_back(registration);
  EvidenceSlotState slot;
  slot.domain = OversubscriptionDomainId(1);
  slot.kind = EvidenceKind::Capacity;
  slot.generation = 5;
  state.evidence.push_back(slot);
  std::vector<u8> buffer;
  REQUIRE(serialize_state(state, buffer).ok());
  DurableState decoded;
  REQUIRE(deserialize_state(buffer, decoded).ok());
  CHECK_EQ(decoded.incarnation.value(), u64{3});
  CHECK_EQ(decoded.epoch.value(), u64{4});
  CHECK_EQ(decoded.domains.size(), std::size_t{1});
  CHECK_EQ(decoded.domains[0].sticky_outcome, Outcome::ReductionRequired);
  CHECK_EQ(decoded.evidence.size(), std::size_t{1});
  std::vector<u8> again;
  REQUIRE(serialize_state(decoded, again).ok());
  CHECK(buffer == again);
  // Trailing bytes are rejected.
  std::vector<u8> extended = buffer;
  extended.push_back(0);
  CHECK(!deserialize_state(extended, decoded).ok());
}
