// Oversubscription Governor — checked integer arithmetic for capacity math.
//
// Every externally influenced size, capacity, ratio, and threshold flows through
// these helpers. Callers must treat a `false` return as "the boundary could not
// be established", never as zero.
#ifndef OVERSUB_CHECKED_HPP
#define OVERSUB_CHECKED_HPP

#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>
#include <vector>

#include "oversub/version.hpp"

namespace oversub {

using u8 = std::uint8_t;
using u16 = std::uint16_t;
using u32 = std::uint32_t;
using u64 = std::uint64_t;
using i64 = std::int64_t;

inline constexpr u64 kPpmScale = 1000000ULL;
inline constexpr u64 kMaxRatioTerm = 1000000ULL;
// Hard bounds on externally influenced capacity and time values.
inline constexpr u64 kHardMaxCapacityUnits = 1ULL << 48;
inline constexpr u64 kHardMaxTicks = 1ULL << 62;

inline bool add_checked(u64 a, u64 b, u64& out) {
  if (a > std::numeric_limits<u64>::max() - b) return false;
  out = a + b;
  return true;
}

inline bool sub_checked(u64 a, u64 b, u64& out) {
  if (b > a) return false;
  out = a - b;
  return true;
}

inline bool mul_checked(u64 a, u64 b, u64& out) {
  if (a != 0 && b > std::numeric_limits<u64>::max() / a) return false;
  out = a * b;
  return true;
}

inline u64 add_sat(u64 a, u64 b) {
  u64 r = 0;
  return add_checked(a, b, r) ? r : std::numeric_limits<u64>::max();
}

inline u64 mul_sat(u64 a, u64 b) {
  u64 r = 0;
  return mul_checked(a, b, r) ? r : std::numeric_limits<u64>::max();
}

// floor((a * b) / d) with a widening intermediate. d == 0 fails.
bool mul_div_floor(u64 a, u64 b, u64 d, u64& out);
// ceil((a * b) / d). d == 0 fails. Returns false only on overflow of the result.
bool mul_div_ceil(u64 a, u64 b, u64 d, u64& out);
// Saturating variants used by explanation and accounting paths where clamping is
// the documented behaviour rather than an error.
u64 mul_div_floor_sat(u64 a, u64 b, u64 d);
// Portable bitwise long-division reference used to cross-check the fast path.
bool mul_div_floor_reference(u64 a, u64 b, u64 d, u64& out);

inline bool ppm_of(u64 value, u64 ppm, u64& out) {
  return mul_div_floor(value, ppm, kPpmScale, out);
}

// Checked sum of a value range. Returns false on overflow.
bool sum_checked(const std::vector<u64>& values, u64& out);

// A non-negative rational used for configured/policy/degraded oversubscription
// ratios. Deliberate oversubscription ratios are >= 1 (1:1 means "no deliberate
// oversubscription"); terms are bounded so cross-multiplication cannot overflow.
struct Ratio {
  u64 num{1};
  u64 den{1};
};

bool ratio_valid(const Ratio& r);
bool ratio_ge(const Ratio& a, const Ratio& b);
bool ratio_le(const Ratio& a, const Ratio& b);
bool ratio_gt(const Ratio& a, const Ratio& b);
bool ratio_lt(const Ratio& a, const Ratio& b);
bool ratio_eq(const Ratio& a, const Ratio& b);
Ratio ratio_min(const Ratio& a, const Ratio& b);
u64 ratio_ppm(const Ratio& r);
std::string ratio_to_string(const Ratio& r);

}  // namespace oversub

#endif  // OVERSUB_CHECKED_HPP
