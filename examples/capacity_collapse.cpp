// A degraded fabric lowers the legal boundary: the effective ratio is reduced,
// previously legal oversubscription becomes an overage, and the domain is
// fenced until capacity recovers.
#include "eg.hpp"

int main() {
  using namespace oversub;
  eg::ScratchDirectory dir("capacity-collapse");
  SynthConfig config = eg::base_config();
  config.domains = 1;
  config.resources_per_domain = 4;
  config.degraded_resources = 2;
  config.failed_resources = 1;
  config.has_degraded_ratio_cap = true;
  config.degraded_ratio_cap = Ratio{1, 1};
  SynthesizedPopulation population = synthesize(config);

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
  std::printf("configured_ratio=%s effective_ratio=%s reduced=%s\n",
              ratio_to_string(decision.evaluation.accounting.configured_ratio).c_str(),
              ratio_to_string(decision.evaluation.accounting.effective_ratio).c_str(),
              decision.evaluation.accounting.ratio_reduced_by_degradation ? "true" : "false");
  return 0;
}
