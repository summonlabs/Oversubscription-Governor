// Oversubscription Governor benchmark.
//
// SYNTHETIC. Every population measured here is generated in memory by
// synth.cpp. No physical network, NIC, switch, RDMA, or optical hardware is
// involved, and none of these numbers describe hardware behaviour. The
// benchmark measures completed governance work: an evaluation is counted only
// after it returns a decision, and the outcome class of every completed
// evaluation is reported so the work cannot be mistaken for a no-op.
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

#include "oversub/governor.hpp"
#include "oversub/synth.hpp"

namespace {

using namespace oversub;

struct Case {
  u32 domains{1};
  u32 resources{4};
  u32 guarantees{2};
  u32 demand{4};
  u32 service_classes{2};
  u64 capacity{1000000};
  u64 evaluations{2000};
  u64 demand_scale_ppm{kPpmScale};   // deliberate over-demand factor for the overload cases
};

struct Result {
  Case configuration;
  u64 evaluations{0};
  u64 decisions{0};
  double engine_millis{0};
  double governor_millis{0};
  u64 within_policy{0};
  u64 at_limit{0};
  u64 reduction{0};
  u64 emergency{0};
  u64 denied{0};
  bool ok{false};
  std::string decision_error;
  int engine_unknown{0};
  std::string engine_unknown_reason;
};

double millis_since(const std::chrono::steady_clock::time_point& start) {
  const auto elapsed = std::chrono::steady_clock::now() - start;
  return std::chrono::duration<double, std::milli>(elapsed).count();
}

void classify(const Outcome outcome, Result& result) {
  switch (outcome) {
    case Outcome::WithinPolicy: ++result.within_policy; break;
    case Outcome::AtLimit: ++result.at_limit; break;
    case Outcome::ReductionRequired: ++result.reduction; break;
    case Outcome::EmergencyReduction: ++result.emergency; break;
    default: ++result.denied; break;
  }
}

Result run_case(const Case& configuration, const std::string& state_directory, bool durable_writes) {
  Result result;
  result.configuration = configuration;
  SynthConfig config;
  config.domains = configuration.domains;
  config.resources_per_domain = configuration.resources;
  config.guarantees_per_resource = configuration.guarantees;
  config.demand_records_per_resource = configuration.demand;
  config.distinct_service_classes = configuration.service_classes;
  config.capacity_per_resource = configuration.capacity;
  SynthesizedPopulation population = synthesize(config);
  if (configuration.demand_scale_ppm != kPpmScale) {
    for (auto& record : population.admissions.records) {
      if (record.kind != DemandKind::Contingent) continue;
      record.committed_capacity =
          mul_div_floor_sat(record.committed_capacity, configuration.demand_scale_ppm, kPpmScale);
      record.pending_capacity =
          mul_div_floor_sat(record.pending_capacity, configuration.demand_scale_ppm, kPpmScale);
    }
    population.admissions.provenance.payload_digest = admission_snapshot_digest(population.admissions);
  }
  Status status = validate_policy(population.policy);
  if (!status.ok()) {
    std::fprintf(stderr, "policy invalid: %s\n", status.to_string().c_str());
    return result;
  }

  // Pure engine: the deterministic governance core over one domain's evidence,
  // which is exactly how a domain-scoped caller uses it. No I/O.
  {
    const SynthesizedPopulation slice =
        slice_domain(population, OversubscriptionDomainId(population.capacity.domain.value()));
    if (slice.capacity.resources.empty()) {
      std::fprintf(stderr, "slice produced no evidence\n");
      return result;
    }
    PolicyIndex index(population.policy);
    const DomainConfig* domain = find_domain(population.policy, slice.capacity.domain);
    if (domain == nullptr) {
      std::fprintf(stderr, "domain is not covered by the policy\n");
      return result;
    }
    AuthorityExpectation expectation;
    expectation.epoch = slice.capacity.epoch;
    expectation.policy_generation = population.policy.generation;
    expectation.domain_generation = domain->generation;
    expectation.capacity_generation = slice.capacity.generation;
    expectation.reservation_generation = slice.reservations.generation;
    expectation.admission_generation = slice.admissions.generation;
    expectation.risk_budget_generation = population.policy.risk_budget.generation;
    DomainEvidence evidence;
    evidence.capacity = &slice.capacity;
    evidence.reservations = &slice.reservations;
    evidence.admissions = &slice.admissions;
    // The accumulator keeps the measured loop from being optimised away without
    // adding any I/O to the timed region.
    volatile u64 accumulator = 0;
    const auto start = std::chrono::steady_clock::now();
    for (u64 i = 0; i < configuration.evaluations; ++i) {
      const EvaluationResult evaluated = evaluate_domain(population.policy, index, *domain, expectation, evidence, 0);
      accumulator ^= evaluated.result_digest.bytes[0];
      if (evaluated.outcome == Outcome::Unknown && result.engine_unknown == 0) {
        result.engine_unknown = 1;
        result.engine_unknown_reason = evaluated.detail;
      }
      ++result.evaluations;
    }
    result.engine_millis = millis_since(start);
    if (accumulator == 0x5A5A5A5A5A5A5A5AULL) std::fprintf(stderr, "");

  }

  // Governor path: durable ingestion plus authoritative decisions.
  {
    std::error_code ec;
    std::filesystem::remove_all(state_directory, ec);
    Governor governor;
    GovernorOptions options;
    options.state_directory = state_directory;
    options.sync_writes = durable_writes;
    status = governor.open(options, population.policy);
    if (!status.ok()) {
      std::fprintf(stderr, "open failed: %s\n", status.to_string().c_str());
      return result;
    }
    status = governor.register_publisher(population.capacity.provenance.publisher,
                                         population.capacity.provenance.incarnation,
                                         population.capacity.epoch, 0);
    if (!status.ok()) {
      std::fprintf(stderr, "register_publisher failed: %s\n", status.to_string().c_str());
      return result;
    }
    // Evidence is published per domain: a domain with no evidence can only
    // produce UNKNOWN, which would not measure governance work.
    u64 sequence = 0;
    for (u32 domain = 0; domain < configuration.domains; ++domain) {
      SynthesizedPopulation slice =
          slice_domain(population, OversubscriptionDomainId(population.capacity.domain.value() + domain));
      auto stamp = [&](Provenance& provenance, u64 tick) {
        provenance.sequence = ++sequence;
        provenance.evidence_id = EvidenceId(sequence);
        provenance.observed_tick = tick;
        provenance.payload_digest = Digest::zero();
      };
      stamp(slice.capacity.provenance, 0);
      stamp(slice.reservations.provenance, 0);
      stamp(slice.admissions.provenance, 0);
      const IngestOutcome capacity_outcome = governor.ingest_capacity(slice.capacity);
      if (!capacity_outcome.status.ok()) {
        std::fprintf(stderr, "ingest_capacity failed: %s\n", capacity_outcome.status.to_string().c_str());
        return result;
      }
      const IngestOutcome reservation_outcome = governor.ingest_reservations(slice.reservations);
      if (!reservation_outcome.status.ok()) {
        std::fprintf(stderr, "ingest_reservations failed: %s\n",
                     reservation_outcome.status.to_string().c_str());
        return result;
      }
      const IngestOutcome admission_outcome = governor.ingest_admissions(slice.admissions);
      if (!admission_outcome.status.ok()) {
        std::fprintf(stderr, "ingest_admissions failed: %s\n", admission_outcome.status.to_string().c_str());
        return result;
      }
    }
    const auto start = std::chrono::steady_clock::now();
    for (u64 i = 0; i < configuration.evaluations; ++i) {
      EvaluationRequest request;
      request.domain = OversubscriptionDomainId(population.capacity.domain.value() + (i % configuration.domains));
      Decision decision;
      const Status evaluated = governor.evaluate(request, decision);
      if (!evaluated.ok()) {
        if (result.decision_error.empty()) result.decision_error = evaluated.to_string();
        break;
      }
      classify(decision.effective_outcome, result);
      ++result.decisions;
    }
    result.governor_millis = millis_since(start);
    (void)governor.close();
    std::filesystem::remove_all(state_directory, ec);
  }
  result.ok = result.evaluations == configuration.evaluations &&
              result.decisions == configuration.evaluations;
  return result;
}

}  // namespace

int main(int argc, char** argv) {
  bool quick = false;
  bool durable = false;
  for (int i = 1; i < argc; ++i) {
    const std::string token = argv[i];
    if (token == "--quick") {
      quick = true;
    } else if (token == "--durable") {
      durable = true;
    } else {
      std::fprintf(stderr, "usage: oversub_bench [--quick] [--durable]\n");
      return 2;
    }
  }

  std::vector<Case> cases;
  const u64 evaluations = quick ? 400 : 4000;
  cases.push_back(Case{1, 4, 2, 4, 2, 1000000, evaluations, kPpmScale});
  cases.push_back(Case{4, 8, 2, 6, 2, 1000000, evaluations, kPpmScale});
  cases.push_back(Case{16, 16, 4, 8, 4, 1000000, evaluations / 2, kPpmScale});
  cases.push_back(Case{1, 64, 8, 16, 8, 1000000, evaluations, kPpmScale});
  cases.push_back(Case{8, 64, 8, 16, 8, 1000000, evaluations / 2, kPpmScale});
  cases.push_back(Case{64, 64, 16, 32, 8, 10000000, evaluations / 4, kPpmScale});
  // Deliberate over-demand: the same populations with contingent demand at 3x.
  cases.push_back(Case{4, 8, 2, 6, 2, 1000000, evaluations, 3 * kPpmScale});
  cases.push_back(Case{16, 16, 4, 8, 4, 1000000, evaluations / 2, 3 * kPpmScale});
  cases.push_back(Case{8, 64, 8, 16, 8, 1000000, evaluations / 2, 3 * kPpmScale});

  std::printf("Oversubscription Governor benchmark (SYNTHETIC populations)\n");
  std::printf("SYNTHETIC: in-memory populations only; no physical network is exercised.\n");
  std::printf("durable_writes=%s\n", durable ? "true" : "false");
  std::printf("%6s %10s %5s %7s %7s %10s %11s %9s %11s %9s %8s %8s %8s\n", "domain", "resources", "oblig",
              "demand", "scale", "decisions", "engine_ms", "engine/s", "gov_ms", "gov/s", "within",
              "reduce", "denied");

  bool all_ok = true;
  double total_engine_evaluations = 0;
  double total_engine_millis = 0;
  double total_governor_decisions = 0;
  double total_governor_millis = 0;
  u64 within = 0;
  u64 reduce = 0;
  u64 denied = 0;
  std::string last_error;
  for (const Case& configuration : cases) {
    const std::string directory =
        (std::filesystem::temp_directory_path() / "oversub-bench-state").string();
    const Result result = run_case(configuration, directory, durable);
    if (!result.ok) all_ok = false;
    const double engine_rate = result.engine_millis > 0 ? result.evaluations / (result.engine_millis / 1000.0) : 0;
    const double governor_rate =
        result.governor_millis > 0 ? result.decisions / (result.governor_millis / 1000.0) : 0;
    std::printf("%6u %10u %5u %7u %6.2fx %10u %11.1f %9.0f %11.1f %9.0f %8llu %8llu %8llu\n",
                configuration.domains, configuration.resources, configuration.guarantees,
                configuration.demand, static_cast<double>(configuration.demand_scale_ppm) / kPpmScale,
                static_cast<unsigned>(result.decisions), result.engine_millis,
                engine_rate, result.governor_millis, governor_rate,
                static_cast<unsigned long long>(result.within_policy),
                static_cast<unsigned long long>(result.reduction + result.emergency),
                static_cast<unsigned long long>(result.denied));
    if (!result.decision_error.empty() && last_error.empty()) last_error = result.decision_error;
    if (result.engine_unknown != 0 && last_error.empty()) {
      last_error = "engine returned UNKNOWN: " + result.engine_unknown_reason;
    }
    total_engine_evaluations += static_cast<double>(result.evaluations);
    total_engine_millis += result.engine_millis;
    total_governor_decisions += static_cast<double>(result.decisions);
    total_governor_millis += result.governor_millis;
    within += result.within_policy;
    reduce += result.reduction + result.emergency;
    denied += result.denied;
  }
  std::printf("\ncompleted: engine_evaluations=%llu governor_decisions=%llu\n",
              static_cast<unsigned long long>(total_engine_evaluations),
              static_cast<unsigned long long>(total_governor_decisions));
  std::printf("outcomes: within_policy=%llu reductions=%llu denied_or_unknown=%llu\n",
              static_cast<unsigned long long>(within), static_cast<unsigned long long>(reduce),
              static_cast<unsigned long long>(denied));
  if (total_engine_millis > 0) {
    std::printf("aggregate: engine=%.0f evaluations/s governor=%.0f decisions/s\n",
                total_engine_evaluations / (total_engine_millis / 1000.0),
                total_governor_millis > 0 ? total_governor_decisions / (total_governor_millis / 1000.0) : 0);
  }
  std::printf("result: %s\n", all_ok ? "COMPLETED" : "INCOMPLETE");
  if (!last_error.empty()) std::fprintf(stderr, "first failure: %s\n", last_error.c_str());
  return all_ok ? 0 : 1;
}
