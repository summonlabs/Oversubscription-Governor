// Oversubscription Governor — logical time.
//
// Governance decisions are a pure function of logical ticks, never wall-clock
// time. Wall-clock time is recorded for provenance only.
#ifndef OVERSUB_CLOCK_HPP
#define OVERSUB_CLOCK_HPP

#include <cstdint>
#include <string>

#include "oversub/checked.hpp"

namespace oversub {

class LogicalClock {
 public:
  explicit LogicalClock(u64 start = 0) : tick_(start) {}

  u64 tick() const { return tick_; }

  // Advance by a bounded delta. Returns false when the advance would overflow.
  bool advance(u64 delta);

  // Move forward to the candidate tick if it is ahead of the current tick. Never
  // moves backwards. Returns true when the tick changed.
  bool observe(u64 candidate);

  // Force the tick to an exact value. Used only when restoring durable state.
  bool restore(u64 value);

 private:
  u64 tick_;
};

// Wall-clock milliseconds since the Unix epoch. Recorded in provenance so that
// audit records can be correlated with operator logs. It is never an input to a
// governance decision.
u64 wall_clock_millis();

}  // namespace oversub

#endif  // OVERSUB_CLOCK_HPP
