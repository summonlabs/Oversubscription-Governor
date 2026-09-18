// Deliberate oversubscription that stays inside its policy boundary.
#include "eg.hpp"

int main() {
  using namespace oversub;
  eg::ScratchDirectory dir("within-policy");
  SynthesizedPopulation population = synthesize(eg::base_config());
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
  return decision.new_oversubscription_authorized ? 0 : 1;
}
