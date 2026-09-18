// A contingent demand spike pushes the domain past its legal ceiling: the
// governor withholds new authority and emits bounded corrective intent.
#include "eg.hpp"

int main() {
  using namespace oversub;
  eg::ScratchDirectory dir("reduction-required");
  SynthesizedPopulation population = synthesize(eg::base_config());
  // Every contingent record commits far beyond its share of the ceiling.
  for (auto& record : population.admissions.records) {
    if (record.kind == DemandKind::Contingent) record.committed_capacity *= 8;
  }
  population.admissions.provenance.payload_digest = admission_snapshot_digest(population.admissions);

  Governor governor;
  if (int code = eg::report(eg::open_demo_governor(governor, dir.path(), population.policy), "open")) {
    return code;
  }
  if (int code = eg::report(eg::install(governor, population), "install")) return code;

  EvaluationRequest request;
  request.domain = OversubscriptionDomainId(1);
  Decision decision;
  if (int code = eg::report(governor.evaluate(request, decision), "evaluate")) return code;
  eg::print_decision(governor, decision);

  RecordingIntentSink sink;
  const Status dispatched = dispatch_corrective_intent(decision, sink);
  std::printf("intent delivered=%llu dropped=%llu status=%s\n",
              static_cast<unsigned long long>(sink.delivered()),
              static_cast<unsigned long long>(sink.dropped()), dispatched.to_string().c_str());
  for (const auto& record : sink.records()) {
    std::printf("  intent %s amount=%llu reason=%s\n", to_string(record.kind),
                static_cast<unsigned long long>(record.amount_units), to_string(record.reason));
  }
  // Corrective intent is produced, but the governor itself performs nothing.
  return decision.new_oversubscription_authorized ? 1 : 0;
}
