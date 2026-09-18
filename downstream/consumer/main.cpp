// Independent consumer of the installed Oversubscription Governor package.
//
// This program is deliberately small: it proves that a downstream project can
// find the package, link the exported target, include the public headers, run a
// real governance evaluation, and read the version and digest API.
#include <cstdio>
#include <string>

#include "oversub/adapters.hpp"
#include "oversub/governor.hpp"
#include "oversub/synth.hpp"

int main() {
  using namespace oversub;

  // A synthetic population keeps the consumer self-contained: no evidence files,
  // no network, no services.
  SynthConfig config;
  config.domains = 2;
  config.resources_per_domain = 4;
  config.guarantees_per_resource = 2;
  config.demand_records_per_resource = 4;
  config.capacity_per_resource = 100000;

  SynthesizedPopulation population = synthesize(config);
  const Status policy_status = validate_policy(population.policy);
  if (!policy_status.ok()) {
    std::fprintf(stderr, "policy invalid: %s\n", policy_status.to_string().c_str());
    return 1;
  }

  const std::string directory = "oversub-consumer-state";
  Governor governor;
  GovernorOptions options;
  options.state_directory = directory;
  options.sync_writes = false;
  Status status = governor.open(options, population.policy);
  if (!status.ok()) {
    std::fprintf(stderr, "open failed: %s\n", status.to_string().c_str());
    return 1;
  }
  status = governor.register_publisher(population.capacity.provenance.publisher,
                                      population.capacity.provenance.incarnation,
                                      population.capacity.epoch, 0);
  if (!status.ok()) {
    std::fprintf(stderr, "register failed: %s\n", status.to_string().c_str());
    return 1;
  }
  const IngestOutcome capacity = governor.ingest_capacity(population.capacity);
  const IngestOutcome reservations = governor.ingest_reservations(population.reservations);
  const IngestOutcome admissions = governor.ingest_admissions(population.admissions);
  if (!capacity.status.ok() || !reservations.status.ok() || !admissions.status.ok()) {
    std::fprintf(stderr, "evidence rejected\n");
    return 1;
  }

  EvaluationRequest request;
  request.domain = OversubscriptionDomainId(1);
  Decision decision;
  status = governor.evaluate(request, decision);
  if (!status.ok()) {
    std::fprintf(stderr, "evaluate failed: %s\n", status.to_string().c_str());
    return 1;
  }

  RecordingIntentSink sink;
  const Status dispatched = dispatch_corrective_intent(decision, sink);

  std::printf("oversubscription governor %s\n", kVersionString);
  std::printf("outcome=%s effective=%s authorized=%s increment=%llu\n",
              to_string(decision.evaluation.outcome), to_string(decision.effective_outcome),
              decision.new_oversubscription_authorized ? "true" : "false",
              static_cast<unsigned long long>(decision.authorized_increment_units));
  std::printf("ceiling=%llu committed=%llu contingent_free=%llu binding=%s\n",
              static_cast<unsigned long long>(decision.evaluation.accounting.ceiling_total),
              static_cast<unsigned long long>(decision.evaluation.accounting.committed_total),
              static_cast<unsigned long long>(decision.evaluation.accounting.contingent_free),
              to_string(decision.evaluation.accounting.binding_constraint));
  std::printf("decision_digest=%s intents=%llu dispatch=%s\n", decision.decision_digest.to_hex().c_str(),
              static_cast<unsigned long long>(sink.delivered()), dispatched.to_string().c_str());

  std::string explanation;
  ExplainOptions explain;
  explain.max_lines = 12;
  explain.max_bytes = 1024;
  if (governor.explain(decision, explain, explanation).ok()) {
    std::printf("--- explanation (bounded) ---\n%s", explanation.c_str());
  }
  const Status closed = governor.close();
  return closed.ok() ? 0 : 1;
}
