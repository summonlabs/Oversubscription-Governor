#include "oversub/clock.hpp"

#include <chrono>

namespace oversub {

bool LogicalClock::advance(u64 delta) {
  u64 next = 0;
  if (!add_checked(tick_, delta, next)) return false;
  tick_ = next;
  return true;
}

bool LogicalClock::observe(u64 candidate) {
  if (candidate <= tick_) return false;
  tick_ = candidate;
  return true;
}

bool LogicalClock::restore(u64 value) {
  tick_ = value;
  return true;
}

u64 wall_clock_millis() {
  const auto now = std::chrono::system_clock::now().time_since_epoch();
  const auto millis = std::chrono::duration_cast<std::chrono::milliseconds>(now).count();
  if (millis < 0) return 0;
  return static_cast<u64>(millis);
}

}  // namespace oversub
