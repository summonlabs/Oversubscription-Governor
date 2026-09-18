// Quiet telemetry is not permission. Evidence older than the policy's freshness
// budget yields UNKNOWN authority, never a silently preserved boundary.
#include "eg.hpp"

int main() {
  using namespace oversub;
  eg::ScratchDirectory dir("stale-evidence");
  SynthConfig config = eg::base_config();
  config.policy_id = OversubscriptionPolicyId(1);
  SynthesizedPopulation population = synthesize(config);

  Governor governor;
  if (int code = eg::report(eg::open_demo_governor(governor, dir.path(), population.policy), "open")) {
    return code;
  }
  if (int code = eg::report(eg::install(governor, population), "install")) return code;

  EvaluationRequest request;
  request.domain = OversubscriptionDomainId(1);
  request.tick = governor.policy().max_admission_age_ticks + 5;   // beyond the freshness budget
  Decision decision;
  if (int code = eg::report(governor.evaluate(request, decision), "evaluate")) return code;
  eg::print_decision(governor, decision);
  std::printf("authorized=%s outcome=%s\n",
              decision.new_oversubscription_authorized ? "true" : "false",
              to_string(decision.evaluation.outcome));
  return decision.new_oversubscription_authorized ? 1 : 0;
}
