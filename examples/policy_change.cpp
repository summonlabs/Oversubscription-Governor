// Changing policy changes the legal boundary. Every decision bound to the old
// generation becomes stale, and evidence must be republished before authority
// is granted again.
#include "eg.hpp"

int main() {
  using namespace oversub;
  eg::ScratchDirectory dir("policy-change");
  SynthesizedPopulation population = synthesize(eg::base_config());
  Governor governor;
  if (int code = eg::report(eg::open_demo_governor(governor, dir.path(), population.policy), "open")) {
    return code;
  }
  if (int code = eg::report(eg::install(governor, population), "install")) return code;

  EvaluationRequest request;
  request.domain = OversubscriptionDomainId(1);
  Decision before;
  if (int code = eg::report(governor.evaluate(request, before), "evaluate")) return code;

  SynthesizedPopulation tightened = population;
  tightened.policy.domains[0].configured_ratio = Ratio{3, 2};
  tightened.policy.domains[0].emergency_reserve_units = 5000;
  if (int code = eg::report(governor.install_policy(tightened.policy, 1), "install_policy")) return code;

  RevalidationReport report;
  if (int code = eg::report(governor.revalidate(before, report), "revalidate")) return code;
  std::printf("after policy install: generation=%llu revalidate_valid=%s reason=%s\n",
              static_cast<unsigned long long>(governor.policy_generation().value()),
              report.valid ? "true" : "false", to_string(report.reason));

  Decision after;
  request.tick = 1;
  if (int code = eg::report(governor.evaluate(request, after), "evaluate")) return code;
  std::printf("without fresh evidence: outcome=%s authorized=%s\n", to_string(after.evaluation.outcome),
              after.new_oversubscription_authorized ? "true" : "false");
  return report.valid ? 1 : 0;
}
