// Test helper mirroring the coordinator's decision digest computation.
#ifndef OVERSUB_TEST_PROTO_UTIL_HPP
#define OVERSUB_TEST_PROTO_UTIL_HPP

#include "oversub/digest.hpp"
#include "oversub/governor.hpp"

namespace otest {

// The decision digest is recomputed here from first principles so a transport
// round trip can be checked for integrity rather than trusted.
inline oversub::Digest decision_digest_of(const oversub::Decision& decision) {
  oversub::CanonicalHasher h;
  h.add_string("oversub.decision.v1");
  h.add_u64(decision.id.value());
  h.add_digest(decision.evaluation.result_digest);
  h.add_u32(static_cast<oversub::u32>(decision.raw_outcome));
  h.add_u32(static_cast<oversub::u32>(decision.effective_outcome));
  h.add_bool(decision.new_oversubscription_authorized);
  h.add_u64(decision.authorized_increment_units);
  h.add_bool(decision.fenced);
  h.add_u64(decision.issued_tick);
  h.add_u64(decision.incarnation.value());
  h.add_u64(decision.epoch.value());
  return h.digest();
}

}  // namespace otest

#endif  // OVERSUB_TEST_PROTO_UTIL_HPP
