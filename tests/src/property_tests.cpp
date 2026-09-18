// Seeded randomized property tests over the pure governance engine.
#include <random>
#include <string>
#include <vector>

#include "framework.hpp"
#include "oversub/synth.hpp"
#include "testutil.hpp"

using namespace oversub;
using namespace otest;

namespace {

struct Seeded {
  explicit Seeded(u64 seed) : engine(seed) {}
  std::mt19937_64 engine;
  u64 next(u64 low, u64 high) {
    if (high <= low) return low;
    std::uniform_int_distribution<u64> distribution(low, high);
    return distribution(engine);
  }
};

Scenario scenario_from(const SynthConfig& config) {
  SynthesizedPopulation population = synthesize(config);
  Scenario scenario;
  scenario.policy = population.policy;
  scenario.capacity = population.capacity;
  scenario.reservations = population.reservations;
  scenario.admissions = population.admissions;
  return scenario;
}

}  // namespace

TEST(property_authorized_increment_never_exceeds_the_policy_cap) {
  for (u64 seed = 1; seed <= 40; ++seed) {
    Seeded random(seed);
    SynthConfig config;
    config.seed = seed;
    config.domains = 1;
    config.resources_per_domain = static_cast<u32>(random.next(1, 6));
    config.guarantees_per_resource = static_cast<u32>(random.next(0, 3));
    config.demand_records_per_resource = static_cast<u32>(random.next(1, 5));
    config.capacity_per_resource = random.next(1, 100000);
    config.guarantee_ppm_of_usable = random.next(0, 700000);
    config.contingent_ppm_of_ceiling = random.next(0, 1200000);
    config.configured_ratio = Ratio{1 + random.next(0, 2), 1};
    config.has_degraded_ratio_cap = random.next(0, 1) != 0;
    config.degraded_ratio_cap = Ratio{1, 1};
    config.risk_exposure_ppm = random.next(0, 2000000);
    const Scenario scenario = scenario_from(config);
    REQUIRE(scenario.validate().ok());
    const EvaluationResult result = evaluate_scenario(scenario, config.domains);
    const DomainAccounting& accounting = result.accounting;
    if (!accounting.arithmetic_closed || result.outcome == Outcome::Unknown) continue;
    const u64 cap = scenario.policy.policy_ratio_cap.num;
    const u64 cap_den = scenario.policy.policy_ratio_cap.den;
    const u64 usable = accounting.effective_usable_capacity;
    u64 bound = 0;
    CHECK(mul_div_floor(usable, cap, cap_den, bound));
    if (result.new_oversubscription_authorized) {
      CHECK(accounting.committed_total + result.authorized_increment_units <= accounting.ceiling_total);
      CHECK(accounting.committed_total + result.authorized_increment_units <= bound);
    }
    CHECK(accounting.ceiling_total <= bound);
    CHECK(accounting.contingent_free <= accounting.contingent_ceiling);
  }
}

TEST(property_guarantees_are_never_counted_as_contingent_authority) {
  for (u64 seed = 100; seed <= 140; ++seed) {
    SynthConfig config;
    config.seed = seed;
    config.domains = 1;
    config.resources_per_domain = 2;
    config.guarantees_per_resource = 2;
    config.demand_records_per_resource = 3;
    config.capacity_per_resource = 1000 * (1 + seed % 7);
    config.guarantee_ppm_of_usable = 100000 + (seed % 5) * 100000;
    config.contingent_ppm_of_ceiling = 400000;
    config.emergency_reserve_ppm = 10000 * (seed % 4);
    const Scenario scenario = scenario_from(config);
    REQUIRE(scenario.validate().ok());
    const EvaluationResult result = evaluate_scenario(scenario, 0);
    const DomainAccounting& accounting = result.accounting;
    if (!accounting.arithmetic_closed) continue;
    if (accounting.ceiling_total < accounting.protected_required) continue;
    CHECK_EQ(accounting.contingent_ceiling,
             accounting.ceiling_total - accounting.protected_required);
    CHECK_EQ(accounting.protected_required,
             accounting.guarantees_total + accounting.emergency_reserve);
    CHECK(accounting.contingent_committed + accounting.guarantees_total <=
          accounting.committed_total + accounting.guarantees_total);
  }
}

TEST(property_arithmetic_closes_across_resources) {
  for (u64 seed = 200; seed <= 240; ++seed) {
    SynthConfig config;
    config.seed = seed;
    config.domains = 1;
    config.resources_per_domain = 1 + static_cast<u32>(seed % 5);
    config.guarantees_per_resource = 1 + static_cast<u32>(seed % 3);
    config.demand_records_per_resource = 2 + static_cast<u32>(seed % 3);
    config.capacity_per_resource = 1000 + seed * 13;
    const Scenario scenario = scenario_from(config);
    REQUIRE(scenario.validate().ok());
    const EvaluationResult result = evaluate_scenario(scenario, 0);
    if (!result.accounting.arithmetic_closed) continue;
    u64 physical = 0;
    u64 usable = 0;
    u64 guarantees = 0;
    for (const auto& resource : result.resources) {
      physical += resource.physical_capacity;
      usable += resource.effective_usable_capacity;
      guarantees += resource.guarantees;
    }
    CHECK(physical <= result.accounting.physical_capacity);
    CHECK(usable <= result.accounting.effective_usable_capacity);
    CHECK(guarantees <= result.accounting.guarantees_total);
    for (const auto& resource : result.resources) {
      CHECK(resource.contingent_free <= resource.contingent_ceiling);
      CHECK(resource.overage == (resource.guarantees + resource.contingent_committed > resource.ceiling
                                     ? resource.guarantees + resource.contingent_committed - resource.ceiling
                                     : 0));
    }
  }
}

TEST(property_evaluation_is_deterministic) {
  for (u64 seed = 300; seed <= 340; ++seed) {
    SynthConfig config;
    config.seed = seed;
    config.domains = 1;
    config.resources_per_domain = 3;
    config.capacity_per_resource = 5000 + seed;
    const Scenario scenario = scenario_from(config);
    const EvaluationResult first = evaluate_scenario(scenario, 0);
    const EvaluationResult second = evaluate_scenario(scenario, 0);
    CHECK(first.result_digest == second.result_digest);
    CHECK_EQ(first.outcome, second.outcome);
    CHECK_EQ(first.accounting.ceiling_total, second.accounting.ceiling_total);
  }
}

TEST(property_more_demand_never_relaxes_the_outcome) {
  for (u64 seed = 400; seed <= 430; ++seed) {
    Scenario scenario = Scenario::make(1, 10000, Ratio{2, 1}, 200);
    scenario.mirror_guarantees();
    const u64 base = 1000 + seed * 7;
    scenario.add_demand(ResourceId(1), ServiceClassId(2), DemandKind::Contingent, base);
    scenario.stamp(0);
    const EvaluationResult low = evaluate_scenario(scenario, 0);

    Scenario heavier = scenario;
    heavier.admissions.records.back().committed_capacity = base * 2 + 5000;
    heavier.stamp(0);
    const EvaluationResult high = evaluate_scenario(heavier, 0);

    CHECK(outcome_severity(high.outcome) >= outcome_severity(low.outcome));
    if (low.new_oversubscription_authorized && high.new_oversubscription_authorized) {
      CHECK(high.authorized_increment_units <= low.authorized_increment_units);
    }
  }
}

TEST(property_less_capacity_never_increases_authority) {
  for (u64 seed = 500; seed <= 530; ++seed) {
    Scenario scenario = Scenario::make(1, 10000, Ratio{2, 1}, 1000);
    scenario.mirror_guarantees();
    scenario.add_demand(ResourceId(1), ServiceClassId(2), DemandKind::Contingent, 4000);
    scenario.stamp(0);
    const EvaluationResult large = evaluate_scenario(scenario, 0);

    Scenario smaller = scenario;
    smaller.capacity.resources[0].usable_capacity = 5000;
    smaller.capacity.resources[0].physical_capacity = 5000;
    smaller.stamp(0);
    const EvaluationResult small = evaluate_scenario(smaller, 0);

    CHECK(outcome_severity(small.outcome) >= outcome_severity(large.outcome));
    CHECK(small.accounting.ceiling_total <= large.accounting.ceiling_total);
    CHECK(small.accounting.contingent_free <= large.accounting.contingent_free);
  }
}

TEST(property_degradation_never_raises_the_effective_ratio) {
  for (u64 seed = 600; seed <= 640; ++seed) {
    SynthConfig config;
    config.seed = seed;
    config.domains = 1;
    config.resources_per_domain = 4;
    config.capacity_per_resource = 10000;
    config.configured_ratio = Ratio{2, 1};
    config.has_degraded_ratio_cap = true;
    config.degraded_ratio_cap = Ratio{1 + seed % 3, 2};
    config.degraded_resources = static_cast<u32>(seed % 4);
    config.failed_resources = static_cast<u32>(seed % 2);
    if (!ratio_valid(config.degraded_ratio_cap) ||
        ratio_gt(config.degraded_ratio_cap, config.configured_ratio)) {
      continue;
    }
    const Scenario scenario = scenario_from(config);
    if (!scenario.validate().ok()) continue;
    const EvaluationResult result = evaluate_scenario(scenario, 0);
    if (!result.accounting.arithmetic_closed) continue;
    CHECK(result.accounting.effective_ratio_ppm <= result.accounting.configured_ratio_ppm);
    if (result.accounting.degraded_resources > 0 || result.accounting.failed_resources > 0) {
      const u64 cap_ppm = ratio_ppm(config.degraded_ratio_cap);
      CHECK(result.accounting.effective_ratio_ppm <= cap_ppm);
    }
  }
}

TEST(property_explanation_always_fits_its_budget) {
  for (u64 seed = 700; seed <= 720; ++seed) {
    SynthConfig config;
    config.seed = seed;
    config.domains = 1;
    config.resources_per_domain = 1 + static_cast<u32>(seed % 20);
    config.capacity_per_resource = 1000 * (1 + seed % 5);
    const Scenario scenario = scenario_from(config);
    const EvaluationResult result = evaluate_scenario(scenario, 0);
    ExplainOptions options;
    options.max_lines = static_cast<u32>(1 + seed % 40);
    options.max_bytes = static_cast<u32>(64 + seed % 2048);
    const std::string text = explain_evaluation(scenario.policy, result, options);
    CHECK(text.size() <= options.max_bytes);
    std::size_t lines = 0;
    for (const char c : text) {
      if (c == '\n') ++lines;
    }
    CHECK(lines <= static_cast<std::size_t>(options.max_lines) + 1);
  }
}

TEST(property_governor_agrees_with_the_pure_engine) {
  for (u64 seed = 800; seed <= 820; ++seed) {
    SynthConfig config;
    config.seed = seed;
    config.domains = 1;
    config.resources_per_domain = 2;
    config.capacity_per_resource = 1000 + seed;
    config.publisher = PublisherId(7);
    config.incarnation = IncarnationId(9);
    const Scenario scenario = scenario_from(config);
    REQUIRE(scenario.validate().ok());
    otest::GovernorFixture fixture("prop");
    const Status opened = fixture.open(scenario.policy);
    CHECK_EQ(opened.to_string(), std::string("Ok"));
    const Status installed = otest::install_scenario(fixture.governor(), scenario);
    CHECK_EQ(installed.to_string(), std::string("Ok"));
    Decision decision;
    EvaluationRequest request;
    request.domain = OversubscriptionDomainId(1);
    REQUIRE(fixture.governor().evaluate(request, decision).ok());
    const EvaluationResult expected = evaluate_scenario(scenario, 0);
    CHECK_EQ(decision.evaluation.outcome, expected.outcome);
    CHECK_EQ(decision.evaluation.accounting.ceiling_total, expected.accounting.ceiling_total);
    CHECK_EQ(decision.evaluation.accounting.contingent_free, expected.accounting.contingent_free);
    CHECK(decision.evaluation.result_digest == expected.result_digest);
  }
}
