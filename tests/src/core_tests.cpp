// Core governance tests: arithmetic, policy, engine outcomes, authority binding.
#include <string>

#include "framework.hpp"
#include "oversub/engine.hpp"
#include "oversub/synth.hpp"
#include "testutil.hpp"

using namespace oversub;
using namespace otest;

// ------------------------------------------------------------------ digests --

TEST(sha256_known_vectors) {
  CHECK_EQ(sha256("").to_hex(),
           std::string("e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"));
  CHECK_EQ(sha256("abc").to_hex(),
           std::string("ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"));
  CHECK_EQ(sha256("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq").to_hex(),
           std::string("248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1"));
  std::string long_input(1000000, 'a');
  CHECK_EQ(sha256(long_input).to_hex(),
           std::string("cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0"));
}

TEST(canonical_hasher_is_order_sensitive_and_stable) {
  CanonicalHasher a;
  a.add_string("x");
  a.add_u64(1);
  CanonicalHasher b;
  b.add_string("x");
  b.add_u64(1);
  CHECK(a.digest() == b.digest());
  CanonicalHasher c;
  c.add_u64(1);
  c.add_string("x");
  CHECK(!(a.digest() == c.digest()));
}

// --------------------------------------------------------------- arithmetic --

TEST(checked_arithmetic_bounds) {
  u64 out = 7;
  CHECK(!add_checked(UINT64_MAX, 1, out));
  CHECK_EQ(out, u64{7});
  CHECK(add_checked(UINT64_MAX - 1, 1, out));
  CHECK_EQ(out, UINT64_MAX);
  CHECK(!sub_checked(0, 1, out));
  CHECK(!mul_checked(UINT64_MAX, 2, out));
  CHECK(add_checked(0, 0, out));
  CHECK_EQ(add_sat(UINT64_MAX, 5), UINT64_MAX);
}

TEST(mul_div_matches_reference_and_detects_overflow) {
  const u64 values[] = {0, 1, 2, 3, 7, 1000, 1ULL << 32, (1ULL << 63) + 1, UINT64_MAX, UINT64_MAX - 1};
  for (const u64 a : values) {
    for (const u64 b : values) {
      for (const u64 d : {1ULL, 2ULL, 3ULL, 1000000ULL, 1ULL << 40}) {
        u64 fast = 0;
        u64 reference = 0;
        const bool fast_ok = mul_div_floor(a, b, d, fast);
        const bool reference_ok = mul_div_floor_reference(a, b, d, reference);
        CHECK_EQ(fast_ok, reference_ok);
        if (fast_ok && reference_ok) CHECK_EQ(fast, reference);
      }
    }
  }
  u64 out = 0;
  CHECK(!mul_div_floor(1, 1, 0, out));
  CHECK(mul_div_floor(UINT64_MAX, 1, 1, out));
  CHECK_EQ(out, UINT64_MAX);
  CHECK(!mul_div_floor(UINT64_MAX, 2, 1, out));
  u64 ceil_value = 0;
  CHECK(mul_div_ceil(10, 1, 3, ceil_value));
  CHECK_EQ(ceil_value, u64{4});
  CHECK(mul_div_ceil(9, 1, 3, ceil_value));
  CHECK_EQ(ceil_value, u64{3});
}

TEST(ratio_ordering_and_bounds) {
  CHECK(ratio_valid(Ratio{2, 1}));
  CHECK(!ratio_valid(Ratio{0, 1}));
  CHECK(!ratio_valid(Ratio{1, 0}));
  CHECK(!ratio_valid(Ratio{1, 2}));
  CHECK(!ratio_valid(Ratio{kMaxRatioTerm + 1, 1}));
  CHECK(ratio_gt(Ratio{3, 2}, Ratio{4, 3}));
  CHECK(ratio_eq(Ratio{4, 2}, Ratio{2, 1}));
  CHECK_EQ(ratio_ppm(Ratio{3, 2}), u64{1500000});
  CHECK_EQ(ratio_to_string(Ratio{3, 2}), std::string("3:2"));
}

// -------------------------------------------------------------- policy gate --

TEST(policy_validation_accepts_a_valid_policy) {
  Scenario scenario = Scenario::make();
  CHECK(scenario.validate().ok());
}

TEST(policy_validation_rejects_malformed_configuration) {
  {
    Scenario scenario = Scenario::make();
    scenario.policy.policy_ratio_cap = Ratio{1, 0};
    CHECK_EQ(scenario.validate().reason, ReasonCode::PolicyRatioInvalid);
  }
  {
    Scenario scenario = Scenario::make();
    scenario.policy.policy_ratio_cap = Ratio{1, 2};
    CHECK_EQ(scenario.validate().reason, ReasonCode::PolicyRatioOutOfBound);
  }
  {
    Scenario scenario = Scenario::make();
    scenario.policy.policy_ratio_cap = Ratio{2, 1};
    scenario.policy.domains[0].configured_ratio = Ratio{3, 1};
    CHECK_EQ(scenario.validate().reason, ReasonCode::PolicyRatioOutOfBound);
  }
  {
    Scenario scenario = Scenario::make();
    scenario.policy.domains[0].has_degraded_ratio_cap = true;
    scenario.policy.domains[0].degraded_ratio_cap = Ratio{3, 1};
    CHECK_EQ(scenario.validate().reason, ReasonCode::PolicyRatioOutOfBound);
  }
  {
    Scenario scenario = Scenario::make();
    scenario.policy.thresholds.reduce_overage_ppm = 500000;
    scenario.policy.thresholds.emergency_overage_ppm = 100;
    CHECK_EQ(scenario.validate().reason, ReasonCode::PolicyThresholdsInverted);
  }
  {
    Scenario scenario = Scenario::make();
    scenario.policy.max_capacity_age_ticks = 0;
    CHECK_EQ(scenario.validate().reason, ReasonCode::PolicyFreshnessUnbounded);
  }
  {
    Scenario scenario = Scenario::make();
    scenario.policy.domains.push_back(scenario.policy.domains[0]);
    CHECK_EQ(scenario.validate().reason, ReasonCode::PolicyDomainDuplicate);
  }
  {
    Scenario scenario = Scenario::make();
    scenario.policy.domains[0].members.push_back(scenario.policy.domains[0].members[0]);
    CHECK_EQ(scenario.validate().reason, ReasonCode::PolicyDomainMemberDuplicate);
  }
  {
    Scenario scenario = Scenario::make();
    scenario.policy.domains[0].members.clear();
    CHECK_EQ(scenario.validate().reason, ReasonCode::PolicyDomainEmpty);
  }
  {
    Scenario scenario = Scenario::make();
    scenario.policy.risk_budget.has_ppm = false;
    CHECK_EQ(scenario.validate().reason, ReasonCode::PolicyRiskBudgetInvalid);
  }
  {
    Scenario scenario = Scenario::make();
    scenario.policy.domains[0].burst_pool_enabled = false;
    scenario.policy.domains[0].burst_pool_cap_units = 10;
    CHECK_EQ(scenario.validate().reason, ReasonCode::PolicyReserveInvalid);
  }
  {
    Scenario scenario = Scenario::make();
    scenario.policy.service_classes.push_back(scenario.policy.service_classes[0]);
    CHECK_EQ(scenario.validate().reason, ReasonCode::PolicyServiceClassDuplicate);
  }
  {
    Scenario scenario = Scenario::make();
    scenario.policy.digest = sha256("not the policy");
    CHECK_EQ(scenario.validate().reason, ReasonCode::PolicyDigestMismatch);
  }
}

TEST(policy_digest_is_content_addressed) {
  Scenario a = Scenario::make();
  Scenario b = Scenario::make();
  compute_policy_digest(a.policy);
  compute_policy_digest(b.policy);
  CHECK(a.policy.digest == b.policy.digest);
  CHECK(domain_config_digest(a.policy.domains[0]) == domain_config_digest(b.policy.domains[0]));
  b.policy.domains[0].configured_ratio = Ratio{3, 2};
  compute_policy_digest(b.policy);
  CHECK(!(a.policy.digest == b.policy.digest));
  CHECK(!(domain_config_digest(a.policy.domains[0]) == domain_config_digest(b.policy.domains[0])));
  // Changing only the generation does not change the domain's content digest.
  Scenario c = a;
  c.policy.domains[0].generation = DomainGeneration(9);
  CHECK_EQ(domain_config_digest(a.policy.domains[0]), domain_config_digest(c.policy.domains[0]));
}

// ------------------------------------------------------------------- engine --

TEST(engine_reports_within_policy_with_remaining_authority) {
  Scenario scenario = Scenario::make(2, 1000, Ratio{2, 1}, 200);
  scenario.mirror_guarantees();
  scenario.add_demand(ResourceId(1), ServiceClassId(2), DemandKind::Contingent, 100);
  scenario.add_demand(ResourceId(2), ServiceClassId(2), DemandKind::Contingent, 100);
  scenario.stamp(0);
  const EvaluationResult result = evaluate_scenario(scenario, 0);
  CHECK_EQ(result.outcome, Outcome::WithinPolicy);
  CHECK(result.new_oversubscription_authorized);
  CHECK(result.accounting.contingent_free > 0);
  CHECK_EQ(result.accounting.guarantees_total, u64{400});
  // ceiling = 2 * (1000 + 1000) = 4000, protected = 400 guarantees, contingent
  // ceiling is 3600 and only 200 is committed.
  CHECK_EQ(result.accounting.ceiling_total, u64{4000});
  CHECK_EQ(result.accounting.contingent_ceiling, u64{3600});
  CHECK_EQ(result.accounting.contingent_free, u64{3400});
  CHECK_EQ(result.authorized_increment_units, u64{3400});
  CHECK(result.authority.complete());
}

TEST(engine_is_deterministic_and_explains_itself) {
  Scenario scenario = Scenario::make(2, 1000, Ratio{2, 1}, 100);
  scenario.mirror_guarantees();
  scenario.add_demand(ResourceId(1), ServiceClassId(2), DemandKind::Contingent, 500);
  scenario.stamp(0);
  const EvaluationResult first = evaluate_scenario(scenario, 0);
  const EvaluationResult second = evaluate_scenario(scenario, 0);
  CHECK(first.result_digest == second.result_digest);
  CHECK_EQ(first.accounting.ceiling_total, second.accounting.ceiling_total);
  ExplainOptions options;
  options.max_lines = 64;
  options.max_bytes = 8192;
  const std::string text = explain_evaluation(scenario.policy, first, options);
  CHECK(text.find("outcome: WITHIN_POLICY") != std::string::npos);
  CHECK(text.find("binding_constraint:") != std::string::npos);
  CHECK(text.find("authority_vector:") != std::string::npos);
  CHECK(text.find("ratio: configured=2:1") != std::string::npos);
  CHECK(text.size() <= 8192);
}

TEST(engine_reports_at_limit_when_authority_is_exhausted) {
  Scenario scenario = Scenario::make(1, 1000, Ratio{2, 1}, 200);
  scenario.mirror_guarantees();
  // ceiling = 2000, protected = 200, contingent ceiling = 1800.
  scenario.add_demand(ResourceId(1), ServiceClassId(2), DemandKind::Contingent, 1800);
  scenario.stamp(0);
  const EvaluationResult result = evaluate_scenario(scenario, 0);
  CHECK_EQ(result.outcome, Outcome::AtLimit);
  CHECK_EQ(result.accounting.contingent_free, u64{0});
  CHECK(!result.new_oversubscription_authorized);
  CHECK(result.authority_valid);
}

TEST(engine_reports_reduction_required_for_a_moderate_overage) {
  Scenario scenario = Scenario::make(1, 1000, Ratio{2, 1}, 100);
  scenario.mirror_guarantees();
  scenario.add_demand(ResourceId(1), ServiceClassId(2), DemandKind::Contingent, 1950);
  scenario.stamp(0);
  const EvaluationResult result = evaluate_scenario(scenario, 0);
  CHECK_EQ(result.outcome, Outcome::ReductionRequired);
  CHECK_EQ(result.reason, ReasonCode::AdmissionExceedsContingentCeiling);
  CHECK_EQ(result.accounting.overage, u64{50});
  REQUIRE(!result.intents.empty());
  CHECK_EQ(result.intents[0].kind, CorrectiveKind::ReduceContingentAdmissionBudget);
  CHECK_EQ(result.intents[0].amount_units, u64{50});
  CHECK(!result.new_oversubscription_authorized);
}

TEST(engine_escalates_to_emergency_reduction_beyond_the_threshold) {
  Scenario scenario = Scenario::make(1, 1000, Ratio{2, 1}, 100);
  scenario.mirror_guarantees();
  scenario.add_demand(ResourceId(1), ServiceClassId(2), DemandKind::Contingent, 3000);
  scenario.stamp(0);
  const EvaluationResult result = evaluate_scenario(scenario, 0);
  CHECK_EQ(result.outcome, Outcome::EmergencyReduction);
  CHECK(result.fence_required);
  bool fenced = false;
  for (const auto& intent : result.intents) {
    if (intent.kind == CorrectiveKind::FenceOversubscribedAdmission) fenced = true;
  }
  CHECK(fenced);
}

TEST(engine_treats_unbacked_guaranteed_admissions_as_contradictory) {
  Scenario scenario = Scenario::make(1, 1000, Ratio{2, 1}, 100);
  scenario.add_demand(ResourceId(1), ServiceClassId(1), DemandKind::Guaranteed, 500);
  scenario.stamp(0);
  const EvaluationResult result = evaluate_scenario(scenario, 0);
  CHECK_EQ(result.outcome, Outcome::ConflictingInput);
  CHECK_EQ(result.reason, ReasonCode::AdmissionKindConflict);
}

TEST(engine_rejects_oversubscription_in_an_ineligible_class) {
  Scenario scenario = Scenario::make(1, 1000, Ratio{2, 1}, 100);
  scenario.mirror_guarantees();
  scenario.add_demand(ResourceId(1), ServiceClassId(1), DemandKind::Contingent, 50);
  scenario.stamp(0);
  const EvaluationResult result = evaluate_scenario(scenario, 0);
  CHECK_EQ(result.outcome, Outcome::PolicyRejected);
  CHECK_EQ(result.reason, ReasonCode::PolicyServiceClassIneligibleDemand);
  CHECK(result.fence_required);
}

TEST(engine_reports_unknown_for_missing_evidence_and_unknown_health) {
  {
    Scenario scenario = Scenario::make();
    scenario.stamp(0);
    PolicyIndex index(scenario.policy);
    const DomainConfig* domain = find_domain(scenario.policy, OversubscriptionDomainId(1));
    AuthorityExpectation expectation;
    expectation.epoch = scenario.capacity.epoch;
    expectation.policy_generation = scenario.policy.generation;
    expectation.domain_generation = domain->generation;
    expectation.capacity_generation = scenario.capacity.generation;
    expectation.reservation_generation = scenario.reservations.generation;
    expectation.admission_generation = scenario.admissions.generation;
    expectation.risk_budget_generation = scenario.policy.risk_budget.generation;
    DomainEvidence evidence;
    evidence.capacity = &scenario.capacity;
    const EvaluationResult result =
        evaluate_domain(scenario.policy, index, *domain, expectation, evidence, 0);
    CHECK_EQ(result.outcome, Outcome::Unknown);
    CHECK_EQ(result.reason, ReasonCode::EvidenceIncomplete);
    CHECK(!result.authority_valid);
  }
  {
    Scenario scenario = Scenario::make();
    scenario.capacity.resources[0].health = ResourceHealth::Unknown;
    scenario.stamp(0);
    const EvaluationResult result = evaluate_scenario(scenario, 0);
    CHECK_EQ(result.outcome, Outcome::Unknown);
    CHECK_EQ(result.reason, ReasonCode::ResourceHealthUnknown);
  }
}

TEST(engine_reports_stale_for_generation_and_freshness_mismatches) {
  {
    // The runtime owns generation 1; evidence claiming 9 is ahead of it.
    Scenario scenario = Scenario::make();
    scenario.stamp(0);
    scenario.capacity.generation = CapacitySnapshotGeneration(9);
    AuthorityExpectation expectation = scenario_expectation(scenario);
    expectation.capacity_generation = CapacitySnapshotGeneration(1);
    const EvaluationResult result = evaluate_scenario_with(scenario, expectation, 0);
    CHECK_EQ(result.outcome, Outcome::Stale);
    CHECK_EQ(result.reason, ReasonCode::GenerationAheadOfRuntime);
  }
  {
    // Evidence that lags the runtime is stale, never authoritative.
    Scenario scenario = Scenario::make();
    scenario.stamp(0);
    AuthorityExpectation expectation = scenario_expectation(scenario);
    expectation.reservation_generation = ReservationSnapshotGeneration(4);
    const EvaluationResult result = evaluate_scenario_with(scenario, expectation, 0);
    CHECK_EQ(result.outcome, Outcome::Stale);
    CHECK_EQ(result.reason, ReasonCode::SnapshotGenerationMismatch);
  }
  {
    Scenario scenario = Scenario::make();
    scenario.stamp(0);
    const EvaluationResult result = evaluate_scenario(scenario, 50);
    CHECK_EQ(result.outcome, Outcome::Stale);
    CHECK_EQ(result.reason, ReasonCode::SnapshotStale);
  }
  {
    Scenario scenario = Scenario::make();
    scenario.stamp(0);
    scenario.reservations.epoch = FabricEpoch(2);
    scenario.refresh_digests();
    const EvaluationResult result = evaluate_scenario(scenario, 0);
    CHECK_EQ(result.outcome, Outcome::Stale);
    CHECK_EQ(result.reason, ReasonCode::EpochMismatch);
  }
}

TEST(engine_reports_conflicting_input_for_structural_contradictions) {
  {
    Scenario scenario = Scenario::make();
    scenario.capacity.resources.push_back(scenario.capacity.resources[0]);
    scenario.stamp(0);
    const EvaluationResult result = evaluate_scenario(scenario, 0);
    CHECK_EQ(result.outcome, Outcome::ConflictingInput);
    CHECK_EQ(result.reason, ReasonCode::SnapshotDuplicate);
  }
  {
    Scenario scenario = Scenario::make();
    scenario.reservations.guarantees.push_back(Guarantee{});
    scenario.reservations.guarantees[0].reservation = ReservationId(1);
    scenario.reservations.guarantees[0].resource = ResourceId(1);
    scenario.reservations.guarantees[0].pool = PoolId(1);
    scenario.reservations.guarantees[0].service_class = ServiceClassId(1);
    scenario.reservations.guarantees[0].guaranteed_capacity = 10;
    scenario.reservations.guarantees[0].protected_obligation = true;
    scenario.reservations.guarantees[0].generation = ReservationGeneration(1);
    scenario.reservations.guarantees.push_back(scenario.reservations.guarantees[0]);
    scenario.stamp(0);
    const EvaluationResult result = evaluate_scenario(scenario, 0);
    CHECK_EQ(result.outcome, Outcome::ConflictingInput);
    CHECK_EQ(result.reason, ReasonCode::GuaranteeDuplicate);
  }
  {
    Scenario scenario = Scenario::make();
    scenario.stamp(0);
    scenario.admissions.observed_tick = 100;   // future-dated evidence
    scenario.refresh_digests();
    const EvaluationResult result = evaluate_scenario(scenario, 0);
    CHECK_EQ(result.outcome, Outcome::ConflictingInput);
    CHECK_EQ(result.reason, ReasonCode::EvidenceFutureDated);
  }
}

TEST(engine_protects_guarantees_and_separates_contingent_capacity) {
  Scenario scenario = Scenario::make(1, 1000, Ratio{2, 1}, 300);
  scenario.mirror_guarantees();
  scenario.add_demand(ResourceId(1), ServiceClassId(2), DemandKind::Contingent, 1000);
  scenario.stamp(0);
  const EvaluationResult result = evaluate_scenario(scenario, 0);
  // ceiling 2000, protected 300 => contingent ceiling 1700, committed 1000.
  CHECK_EQ(result.accounting.ceiling_total, u64{2000});
  CHECK_EQ(result.accounting.guarantees_total, u64{300});
  CHECK_EQ(result.accounting.contingent_ceiling, u64{1700});
  CHECK_EQ(result.accounting.contingent_free, u64{700});
  CHECK_EQ(result.accounting.committed_total, u64{1300});
  CHECK_EQ(result.accounting.protected_headroom, u64{700});
  // The guaranteed capacity is never counted as contingent authority.
  CHECK(result.accounting.contingent_ceiling < result.accounting.ceiling_total);
}

TEST(engine_withdraws_authority_when_guarantees_exceed_capacity) {
  Scenario scenario = Scenario::make(1, 1000, Ratio{2, 1}, 1200);
  scenario.mirror_guarantees();
  scenario.stamp(0);
  const EvaluationResult result = evaluate_scenario(scenario, 0);
  CHECK_EQ(result.outcome, Outcome::EmergencyReduction);
  CHECK_EQ(result.reason, ReasonCode::GuaranteeSumExceedsUsable);
  CHECK(result.fence_required);
  bool recall = false;
  for (const auto& intent : result.intents) {
    if (intent.kind == CorrectiveKind::RequestBorrowedCapacityRecall) recall = true;
  }
  CHECK(recall);
}

TEST(engine_changes_the_boundary_when_capacity_degrades) {
  Scenario healthy = Scenario::make(1, 1000, Ratio{2, 1}, 100);
  healthy.domain().has_degraded_ratio_cap = true;
  healthy.domain().degraded_ratio_cap = Ratio{3, 2};
  healthy.mirror_guarantees();
  healthy.add_demand(ResourceId(1), ServiceClassId(2), DemandKind::Contingent, 1700);
  healthy.stamp(0);
  const EvaluationResult healthy_result = evaluate_scenario(healthy, 0);
  CHECK_EQ(healthy_result.outcome, Outcome::WithinPolicy);
  CHECK_EQ(healthy_result.accounting.effective_ratio_ppm, u64{2000000});

  Scenario degraded = healthy;
  degraded.capacity.resources[0].health = ResourceHealth::Degraded;
  degraded.capacity.resources[0].degraded_capacity_ppm = kPpmScale / 2;
  degraded.stamp(0);
  const EvaluationResult degraded_result = evaluate_scenario(degraded, 0);
  CHECK(degraded_result.accounting.ratio_reduced_by_degradation);
  CHECK_EQ(degraded_result.accounting.effective_ratio_ppm, u64{1500000});
  CHECK_EQ(degraded_result.accounting.binding_constraint, Constraint::DegradedRatioCap);
  CHECK(degraded_result.accounting.effective_ratio_ppm <=
        degraded_result.accounting.configured_ratio_ppm);
  CHECK(outcome_severity(degraded_result.outcome) >= outcome_severity(Outcome::ReductionRequired));
  CHECK(!degraded_result.new_oversubscription_authorized);
}

TEST(engine_does_not_preserve_the_old_ratio_when_no_degraded_cap_exists) {
  Scenario scenario = Scenario::make(1, 1000, Ratio{2, 1}, 100);
  scenario.domain().has_degraded_ratio_cap = false;
  scenario.mirror_guarantees();
  scenario.add_demand(ResourceId(1), ServiceClassId(2), DemandKind::Contingent, 500);
  scenario.stamp(0);
  const EvaluationResult healthy = evaluate_scenario(scenario, 0);
  CHECK_EQ(healthy.outcome, Outcome::WithinPolicy);
  scenario.capacity.resources[0].health = ResourceHealth::Failed;
  scenario.stamp(0);
  const EvaluationResult failed = evaluate_scenario(scenario, 0);
  CHECK_EQ(failed.accounting.effective_ratio_ppm, u64{1000000});
  CHECK(failed.accounting.effective_ratio_ppm <= failed.accounting.configured_ratio_ppm);
  CHECK_EQ(failed.outcome, Outcome::EmergencyReduction);
}

TEST(engine_identifies_the_binding_constraint) {
  {
    Scenario scenario = Scenario::make(1, 1000, Ratio{2, 1}, 0);
    scenario.policy.policy_ratio_cap = Ratio{3, 2};
    scenario.policy.risk_budget.has_ppm = true;
    scenario.policy.risk_budget.max_exposure_ppm = 5000000;
    scenario.stamp(0);
    const EvaluationResult result = evaluate_scenario(scenario, 0);
    CHECK_EQ(result.accounting.binding_constraint, Constraint::PolicyRatioCap);
    CHECK_EQ(result.accounting.ceiling_total, u64{1500});
  }
  {
    Scenario scenario = Scenario::make(1, 1000, Ratio{2, 1}, 0);
    scenario.policy.risk_budget.has_ppm = true;
    scenario.policy.risk_budget.max_exposure_ppm = 100000;   // 10% beyond usable
    scenario.stamp(0);
    const EvaluationResult result = evaluate_scenario(scenario, 0);
    CHECK_EQ(result.accounting.binding_constraint, Constraint::RiskBudgetExposurePpm);
    CHECK_EQ(result.accounting.ceiling_total, u64{1100});
    CHECK_EQ(result.accounting.risk_budget_ceiling, u64{1100});
  }
  {
    Scenario scenario = Scenario::make(1, 1000, Ratio{2, 1}, 0);
    scenario.policy.risk_budget.has_ppm = false;
    scenario.policy.risk_budget.has_absolute = true;
    scenario.policy.risk_budget.max_exposure_units = 50;
    scenario.stamp(0);
    const EvaluationResult result = evaluate_scenario(scenario, 0);
    CHECK_EQ(result.accounting.binding_constraint, Constraint::RiskBudgetExposureAbsolute);
    CHECK_EQ(result.accounting.ceiling_total, u64{1050});
  }
}

TEST(engine_handles_zero_capacity_and_arithmetic_overflow) {
  {
    Scenario scenario = Scenario::make(1, 0, Ratio{2, 1}, 0);
    scenario.stamp(0);
    const EvaluationResult result = evaluate_scenario(scenario, 0);
    CHECK_EQ(result.outcome, Outcome::AtLimit);
    CHECK_EQ(result.accounting.binding_constraint, Constraint::NoCapacity);
    CHECK_EQ(result.accounting.ceiling_total, u64{0});
  }
  {
    Scenario scenario = Scenario::make(1, 0, Ratio{2, 1}, 0);
    scenario.add_demand(ResourceId(1), ServiceClassId(2), DemandKind::Contingent, 10);
    scenario.stamp(0);
    const EvaluationResult result = evaluate_scenario(scenario, 0);
    CHECK_EQ(result.outcome, Outcome::EmergencyReduction);
    CHECK_EQ(result.reason, ReasonCode::AdmissionNoContingentCeiling);
  }
  {
    // Capacity within the policy bound multiplied by a huge ratio overflows: the
    // boundary cannot be established, so no authority may be granted.
    Scenario scenario = Scenario::make(1, kHardMaxCapacityUnits, Ratio{1000000, 1}, 0);
    scenario.policy.policy_ratio_cap = Ratio{1000000, 1};
    scenario.stamp(0);
    const EvaluationResult result = evaluate_scenario(scenario, 0);
    CHECK_EQ(result.outcome, Outcome::Unknown);
    CHECK_EQ(result.reason, ReasonCode::ArithmeticOverflow);
    CHECK(!result.authority_valid);
    CHECK(!result.accounting.arithmetic_closed);
  }
}

TEST(engine_bounds_the_resource_breakdown) {
  SynthConfig config;
  config.domains = 1;
  config.resources_per_domain = 40;
  SynthesizedPopulation population = synthesize(config);
  Scenario scenario;
  scenario.policy = population.policy;
  scenario.capacity = population.capacity;
  scenario.reservations = population.reservations;
  scenario.admissions = population.admissions;
  const EvaluationResult result = evaluate_scenario(scenario, 0);
  CHECK(result.resources.size() <= scenario.policy.limits.max_resource_breakdown);
  CHECK(result.accounting.usable_capacity > 0);
}

TEST(explanation_is_bounded) {
  SynthConfig config;
  config.domains = 1;
  config.resources_per_domain = 32;
  SynthesizedPopulation population = synthesize(config);
  Scenario scenario;
  scenario.policy = population.policy;
  scenario.capacity = population.capacity;
  scenario.reservations = population.reservations;
  scenario.admissions = population.admissions;
  const EvaluationResult result = evaluate_scenario(scenario, 0);
  ExplainOptions options;
  options.max_lines = 5;
  options.max_bytes = 128;
  const std::string text = explain_evaluation(scenario.policy, result, options);
  CHECK(text.size() <= 128);
  std::size_t lines = 0;
  for (const char c : text) {
    if (c == '\n') ++lines;
  }
  CHECK(lines <= 6);
}

// ----------------------------------------------------------------- governor --

TEST(governor_grant_authority_and_revalidate_it) {
  otest::GovernorFixture fixture("grant");
  Scenario scenario = Scenario::make(2, 1000, Ratio{2, 1}, 100);
  scenario.mirror_guarantees();
  scenario.add_demand(ResourceId(1), ServiceClassId(2), DemandKind::Contingent, 500);
  scenario.add_demand(ResourceId(2), ServiceClassId(2), DemandKind::Contingent, 500);
  scenario.stamp(0);
  REQUIRE(fixture.open(scenario.policy).ok());
  REQUIRE(otest::install_scenario(fixture.governor(), scenario).ok());
  Decision decision;
  EvaluationRequest request;
  request.domain = OversubscriptionDomainId(1);
  CHECK(fixture.governor().evaluate(request, decision).ok());
  CHECK_EQ(decision.evaluation.outcome, Outcome::WithinPolicy);
  CHECK(decision.new_oversubscription_authorized);
  CHECK_EQ(decision.effective_outcome, Outcome::WithinPolicy);
  CHECK(decision.incarnation.valid());
  CHECK_EQ(decision.epoch.value(), u64{1});
  RevalidationReport report;
  CHECK(fixture.governor().revalidate(decision, report).ok());
  CHECK(report.valid);
  CHECK_EQ(report.reason, ReasonCode::None);
}

TEST(governor_revalidation_fails_after_new_evidence) {
  otest::GovernorFixture fixture("reval");
  Scenario scenario = Scenario::make(1, 1000, Ratio{2, 1}, 100);
  scenario.mirror_guarantees();
  scenario.stamp(0);
  REQUIRE(fixture.open(scenario.policy).ok());
  REQUIRE(otest::install_scenario(fixture.governor(), scenario).ok());
  Decision decision;
  EvaluationRequest request;
  request.domain = OversubscriptionDomainId(1);
  REQUIRE(fixture.governor().evaluate(request, decision).ok());
  RevalidationReport report;
  REQUIRE(fixture.governor().revalidate(decision, report).ok());
  REQUIRE(report.valid);
  // A newer capacity generation invalidates the decision's binding.
  scenario.capacity.generation = CapacitySnapshotGeneration(2);
  scenario.stamp(5);
  REQUIRE(fixture.governor().ingest_capacity(scenario.capacity).status.ok());
  REQUIRE(fixture.governor().revalidate(decision, report).ok());
  CHECK(!report.valid);
  CHECK_EQ(report.reason, ReasonCode::DecisionStale);
}

TEST(governor_revalidation_fails_after_a_policy_install) {
  otest::GovernorFixture fixture("reval-policy");
  Scenario scenario = Scenario::make(1, 1000, Ratio{2, 1}, 100);
  scenario.mirror_guarantees();
  scenario.stamp(0);
  REQUIRE(fixture.open(scenario.policy).ok());
  REQUIRE(otest::install_scenario(fixture.governor(), scenario).ok());
  Decision decision;
  EvaluationRequest request;
  request.domain = OversubscriptionDomainId(1);
  REQUIRE(fixture.governor().evaluate(request, decision).ok());
  Scenario changed = scenario;
  changed.domain().configured_ratio = Ratio{3, 2};
  changed.stamp(1);
  REQUIRE(fixture.governor().install_policy(changed.policy, 1).ok());
  CHECK_EQ(fixture.governor().policy_generation().value(), u64{2});
  RevalidationReport report;
  REQUIRE(fixture.governor().revalidate(decision, report).ok());
  CHECK(!report.valid);
  CHECK_EQ(report.reason, ReasonCode::DecisionGenerationMismatch);
}

TEST(governor_rejects_stale_and_replayed_evidence) {
  otest::GovernorFixture fixture("stale");
  Scenario scenario = Scenario::make(1, 1000, Ratio{2, 1}, 100);
  scenario.mirror_guarantees();
  scenario.stamp(0);
  REQUIRE(fixture.open(scenario.policy).ok());
  REQUIRE(otest::install_scenario(fixture.governor(), scenario).ok());

  // Exact republication is idempotent.
  const IngestOutcome duplicate = fixture.governor().ingest_capacity(scenario.capacity);
  CHECK(duplicate.status.ok());
  CHECK(duplicate.duplicate);

  // Sequence regression is a replay.
  CapacitySnapshot replay = scenario.capacity;
  replay.provenance.sequence = 1;
  replay.provenance.evidence_id = EvidenceId(999);
  const IngestOutcome replayed = fixture.governor().ingest_capacity(replay);
  CHECK(!replayed.status.ok());
  CHECK_EQ(replayed.status.reason, ReasonCode::PublisherSequenceReplayed);

  // Wrong epoch is stale.
  CapacitySnapshot wrong_epoch = scenario.capacity;
  wrong_epoch.provenance.sequence = 99;
  wrong_epoch.provenance.evidence_id = EvidenceId(998);
  wrong_epoch.epoch = FabricEpoch(2);
  wrong_epoch.provenance.epoch = FabricEpoch(2);
  wrong_epoch.provenance.payload_digest = capacity_snapshot_digest(wrong_epoch);
  const IngestOutcome stale = fixture.governor().ingest_capacity(wrong_epoch);
  CHECK(!stale.status.ok());
  CHECK_EQ(stale.status.reason, ReasonCode::EpochMismatch);

  // Same evidence id with different content is contradictory.
  CapacitySnapshot conflicting = scenario.capacity;
  conflicting.provenance.sequence = 100;
  conflicting.resources[0].usable_capacity = 900;
  conflicting.provenance.payload_digest = capacity_snapshot_digest(conflicting);
  const IngestOutcome conflict = fixture.governor().ingest_capacity(conflicting);
  CHECK(!conflict.status.ok());
  CHECK_EQ(conflict.status.code, StatusCode::Conflict);
}

TEST(governor_fences_publishers_and_requires_incarnation_adoption) {
  otest::GovernorFixture fixture("fence");
  Scenario scenario = Scenario::make(1, 1000, Ratio{2, 1}, 100);
  scenario.mirror_guarantees();
  scenario.stamp(0);
  REQUIRE(fixture.open(scenario.policy).ok());
  REQUIRE(otest::install_scenario(fixture.governor(), scenario).ok());

  // A new incarnation cannot silently take over.
  CHECK(!fixture.governor()
             .register_publisher(PublisherId(1), IncarnationId(2), FabricEpoch(1), 0)
             .ok());
  CHECK(fixture.governor().adopt_incarnation(PublisherId(1), IncarnationId(2), FabricEpoch(1), 0).ok());

  CapacitySnapshot next = scenario.capacity;
  next.generation = CapacitySnapshotGeneration(2);
  next.provenance.incarnation = IncarnationId(2);
  next.provenance.sequence = 1;
  next.provenance.evidence_id = EvidenceId(500);
  next.provenance.payload_digest = capacity_snapshot_digest(next);
  CHECK(fixture.governor().ingest_capacity(next).status.ok());

  CHECK(fixture.governor().fence_publisher(PublisherId(1), ReasonCode::PublisherFenced).ok());
  CapacitySnapshot fenced = next;
  fenced.generation = CapacitySnapshotGeneration(3);
  fenced.provenance.sequence = 2;
  fenced.provenance.evidence_id = EvidenceId(501);
  fenced.provenance.payload_digest = capacity_snapshot_digest(fenced);
  const IngestOutcome rejected = fixture.governor().ingest_capacity(fenced);
  CHECK(!rejected.status.ok());
  CHECK_EQ(rejected.status.reason, ReasonCode::PublisherFenced);
}

TEST(governor_epoch_advance_invalidates_evidence_and_fences_domains) {
  otest::GovernorFixture fixture("epoch");
  Scenario scenario = Scenario::make(1, 1000, Ratio{2, 1}, 100);
  scenario.mirror_guarantees();
  scenario.stamp(0);
  REQUIRE(fixture.open(scenario.policy).ok());
  REQUIRE(otest::install_scenario(fixture.governor(), scenario).ok());
  Decision decision;
  EvaluationRequest request;
  request.domain = OversubscriptionDomainId(1);
  REQUIRE(fixture.governor().evaluate(request, decision).ok());
  REQUIRE(fixture.governor().advance_epoch(FabricEpoch(2)).ok());
  Decision after;
  CHECK(fixture.governor().evaluate(request, after).ok());
  CHECK_EQ(after.evaluation.outcome, Outcome::Unknown);
  CHECK(!after.new_oversubscription_authorized);
  DomainRuntimeState state;
  CHECK(fixture.governor().domain_state(OversubscriptionDomainId(1), state).ok());
  CHECK(state.fenced);
  RevalidationReport report;
  REQUIRE(fixture.governor().revalidate(decision, report).ok());
  CHECK(!report.valid);
  CHECK_EQ(report.reason, ReasonCode::DecisionEpochMismatch);
}

TEST(governor_hysteresis_delays_relaxation_but_never_restriction) {
  otest::GovernorFixture fixture("hysteresis");
  Scenario scenario = Scenario::make(1, 1000, Ratio{2, 1}, 100);
  scenario.policy.hysteresis.deescalation_confirmations = 2;
  scenario.policy.hysteresis.cooldown_ticks = 0;
  scenario.mirror_guarantees();
  scenario.add_demand(ResourceId(1), ServiceClassId(2), DemandKind::Contingent, 3000);
  scenario.stamp(0);
  REQUIRE(fixture.open(scenario.policy).ok());
  REQUIRE(otest::install_scenario(fixture.governor(), scenario).ok());
  EvaluationRequest request;
  request.domain = OversubscriptionDomainId(1);
  Decision escalated;
  REQUIRE(fixture.governor().evaluate(request, escalated).ok());
  CHECK_EQ(escalated.effective_outcome, Outcome::EmergencyReduction);
  CHECK(escalated.fenced);

  // Remove the overage: relaxation needs two confirmations.
  Scenario relaxed = scenario;
  relaxed.admissions.records.pop_back();
  relaxed.admissions.generation = AdmissionSnapshotGeneration(0);   // assign the next generation
  relaxed.stamp(0);
  REQUIRE(fixture.governor().ingest_admissions(relaxed.admissions).status.ok());
  Decision first;
  REQUIRE(fixture.governor().evaluate(request, first).ok());
  CHECK_EQ(first.evaluation.reason, ReasonCode::None);
  CHECK_EQ(first.raw_outcome, Outcome::WithinPolicy);
  CHECK_EQ(first.effective_outcome, Outcome::EmergencyReduction);
  CHECK(!first.new_oversubscription_authorized);
  Decision second;
  REQUIRE(fixture.governor().evaluate(request, second).ok());
  CHECK_EQ(second.effective_outcome, Outcome::WithinPolicy);
  CHECK(second.new_oversubscription_authorized);
}

TEST(governor_at_limit_requires_confirmation_when_configured) {
  otest::GovernorFixture fixture("limit-confirm");
  Scenario scenario = Scenario::make(1, 1000, Ratio{2, 1}, 200);
  scenario.policy.hysteresis.escalation_confirmations = 2;
  scenario.mirror_guarantees();
  scenario.add_demand(ResourceId(1), ServiceClassId(2), DemandKind::Contingent, 1800);
  scenario.stamp(0);
  REQUIRE(fixture.open(scenario.policy).ok());
  REQUIRE(otest::install_scenario(fixture.governor(), scenario).ok());
  EvaluationRequest request;
  request.domain = OversubscriptionDomainId(1);
  Decision first;
  REQUIRE(fixture.governor().evaluate(request, first).ok());
  CHECK_EQ(first.raw_outcome, Outcome::AtLimit);
  CHECK_EQ(first.effective_outcome, Outcome::WithinPolicy);
  CHECK_EQ(first.authorized_increment_units, u64{0});   // no authority is granted either way
  Decision second;
  REQUIRE(fixture.governor().evaluate(request, second).ok());
  CHECK_EQ(second.effective_outcome, Outcome::AtLimit);
}

TEST(governor_serves_history_and_stats) {
  otest::GovernorFixture fixture("history");
  Scenario scenario = Scenario::make(1, 1000, Ratio{2, 1}, 100);
  scenario.mirror_guarantees();
  scenario.stamp(0);
  REQUIRE(fixture.open(scenario.policy).ok());
  REQUIRE(otest::install_scenario(fixture.governor(), scenario).ok());
  EvaluationRequest request;
  request.domain = OversubscriptionDomainId(1);
  for (int i = 0; i < 3; ++i) {
    Decision decision;
    REQUIRE(fixture.governor().evaluate(request, decision).ok());
  }
  std::vector<AuditRecord> history;
  REQUIRE(fixture.governor().history(history, 10).ok());
  CHECK_EQ(history.size(), std::size_t{3});
  CHECK_EQ(history[0].id.value(), u64{1});
  CHECK_EQ(history[2].id.value(), u64{3});
  CHECK(history[2].decision.valid());
  const GovernorStats stats = fixture.governor().stats();
  CHECK_EQ(stats.decisions, u64{3});
  CHECK_EQ(stats.evaluations, u64{3});
  CHECK(stats.journal_appends >= 3);
}

TEST(governor_refuses_evaluation_for_an_uncovered_domain) {
  otest::GovernorFixture fixture("uncovered");
  Scenario scenario = Scenario::make(1, 1000, Ratio{2, 1}, 100);
  scenario.stamp(0);
  REQUIRE(fixture.open(scenario.policy).ok());
  EvaluationRequest request;
  request.domain = OversubscriptionDomainId(42);
  Decision decision;
  REQUIRE(fixture.governor().evaluate(request, decision).ok());
  CHECK_EQ(decision.evaluation.outcome, Outcome::Unknown);
  CHECK_EQ(decision.evaluation.reason, ReasonCode::DomainNotInPolicy);
  CHECK(!decision.new_oversubscription_authorized);
}
