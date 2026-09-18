// Durable authority across a restart: policy, generations, epoch, hysteresis,
// and fencing survive; live evidence and live authority do not.
#include "eg.hpp"

int main() {
  using namespace oversub;
  eg::ScratchDirectory dir("restart-recovery");
  SynthesizedPopulation population = synthesize(eg::base_config());
  Decision before;
  {
    Governor governor;
    if (int code = eg::report(eg::open_demo_governor(governor, dir.path(), population.policy), "open")) {
      return code;
    }
    if (int code = eg::report(eg::install(governor, population), "install")) return code;
    EvaluationRequest request;
    request.domain = OversubscriptionDomainId(1);
    if (int code = eg::report(governor.evaluate(request, before), "evaluate")) return code;
    if (int code = eg::report(governor.close(), "close")) return code;
  }
  Governor reopened;
  if (int code = eg::report(eg::open_demo_governor(reopened, dir.path(), OversubscriptionPolicy{}), "reopen")) {
    return code;
  }
  std::printf("after restart: incarnation=%llu epoch=%llu policy_generation=%llu\n",
              static_cast<unsigned long long>(reopened.incarnation().value()),
              static_cast<unsigned long long>(reopened.epoch().value()),
              static_cast<unsigned long long>(reopened.policy_generation().value()));
  EvaluationRequest request;
  request.domain = OversubscriptionDomainId(1);
  Decision after;
  if (int code = eg::report(reopened.evaluate(request, after), "evaluate")) return code;
  RevalidationReport report;
  if (int code = eg::report(reopened.revalidate(before, report), "revalidate")) return code;
  std::printf("evidence-after-restart outcome=%s revalidate_valid=%s reason=%s\n",
              to_string(after.evaluation.outcome), report.valid ? "true" : "false",
              to_string(report.reason));
  // Authority is never silently restored: it must be re-established.
  return (after.new_oversubscription_authorized || report.valid) ? 1 : 0;
}
