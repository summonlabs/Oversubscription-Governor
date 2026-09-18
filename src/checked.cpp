#include "oversub/checked.hpp"

namespace oversub {
namespace {

// Portable 128-bit by 64-bit division producing quotient and remainder. Used as
// the fallback fast path and as the reference implementation in tests.
bool long_division_ex(u64 a, u64 b, u64 d, u64& quotient, u64& remainder) {
  if (d == 0) return false;
  u64 hi = 0;
  u64 lo = 0;
  if (!mul_checked(a, b, lo)) {
    // Schoolbook 128-bit product over 32-bit limbs. Every partial product fits
    // in 64 bits, and the carry chain cannot overflow because the sum is exactly
    // the 128-bit product of two 64-bit values.
    const u64 a0 = a & 0xFFFFFFFFULL;
    const u64 a1 = a >> 32;
    const u64 b0 = b & 0xFFFFFFFFULL;
    const u64 b1 = b >> 32;
    const u64 p00 = a0 * b0;
    const u64 p01 = a0 * b1;
    const u64 p10 = a1 * b0;
    const u64 p11 = a1 * b1;
    const u64 mid = (p00 >> 32) + (p01 & 0xFFFFFFFFULL) + (p10 & 0xFFFFFFFFULL);
    lo = (p00 & 0xFFFFFFFFULL) | (mid << 32);
    hi = p11 + (p01 >> 32) + (p10 >> 32) + (mid >> 32);
  }
  if (hi == 0) {
    quotient = lo / d;
    remainder = lo % d;
    return true;
  }
  if (hi >= d) return false;  // quotient does not fit in 64 bits
  u64 rem = hi;
  u64 quot = 0;
  for (int bit = 63; bit >= 0; --bit) {
    const u64 carry = rem >> 63;
    rem = (rem << 1) | ((lo >> bit) & 1ULL);
    quot <<= 1;
    if (carry != 0 || rem >= d) {
      rem -= d;
      quot |= 1ULL;
    }
  }
  quotient = quot;
  remainder = rem;
  return true;
}

}  // namespace

bool mul_div_floor_reference(u64 a, u64 b, u64 d, u64& out) {
  u64 q = 0;
  u64 r = 0;
  if (!long_division_ex(a, b, d, q, r)) return false;
  out = q;
  return true;
}

bool mul_div_floor(u64 a, u64 b, u64 d, u64& out) {
  if (d == 0) return false;
  if (a == 0 || b == 0) {
    out = 0;
    return true;
  }
#if defined(__SIZEOF_INT128__)
  const unsigned __int128 product =
      static_cast<unsigned __int128>(a) * static_cast<unsigned __int128>(b);
  const unsigned __int128 quotient = product / static_cast<unsigned __int128>(d);
  if (quotient > static_cast<unsigned __int128>(std::numeric_limits<u64>::max())) return false;
  out = static_cast<u64>(quotient);
  return true;
#elif defined(_MSC_VER) && defined(_M_X64)
  u64 hi = 0;
  const u64 lo = _umul128(a, b, &hi);
  if (hi >= d) return false;
  u64 remainder = 0;
  out = _udiv128(hi, lo, d, &remainder);
  return true;
#else
  return mul_div_floor_reference(a, b, d, out);
#endif
}

u64 mul_div_floor_sat(u64 a, u64 b, u64 d) {
  u64 r = 0;
  if (!mul_div_floor(a, b, d, r)) return std::numeric_limits<u64>::max();
  return r;
}

bool mul_div_ceil(u64 a, u64 b, u64 d, u64& out) {
  if (d == 0) return false;
  if (a == 0 || b == 0) {
    out = 0;
    return true;
  }
  u64 q = 0;
  u64 rem = 0;
#if defined(__SIZEOF_INT128__)
  const unsigned __int128 product =
      static_cast<unsigned __int128>(a) * static_cast<unsigned __int128>(b);
  const unsigned __int128 quotient = product / static_cast<unsigned __int128>(d);
  if (quotient > static_cast<unsigned __int128>(std::numeric_limits<u64>::max())) return false;
  q = static_cast<u64>(quotient);
  rem = static_cast<u64>(product % static_cast<unsigned __int128>(d));
#elif defined(_MSC_VER) && defined(_M_X64)
  u64 hi = 0;
  const u64 lo = _umul128(a, b, &hi);
  if (hi >= d) return false;
  q = _udiv128(hi, lo, d, &rem);
#else
  if (!long_division_ex(a, b, d, q, rem)) return false;
#endif
  if (rem == 0) {
    out = q;
    return true;
  }
  return add_checked(q, 1, out);
}

bool sum_checked(const std::vector<u64>& values, u64& out) {
  u64 acc = 0;
  for (const u64 v : values) {
    if (!add_checked(acc, v, acc)) return false;
  }
  out = acc;
  return true;
}

bool ratio_valid(const Ratio& r) {
  if (r.den == 0) return false;
  if (r.num == 0) return false;
  if (r.den > kMaxRatioTerm || r.num > kMaxRatioTerm) return false;
  return ratio_ge(r, Ratio{1, 1});
}

bool ratio_ge(const Ratio& a, const Ratio& b) {
  // Both terms are bounded by kMaxRatioTerm, so cross products fit in 64 bits.
  return a.num * b.den >= b.num * a.den;
}
bool ratio_le(const Ratio& a, const Ratio& b) { return ratio_ge(b, a); }
bool ratio_gt(const Ratio& a, const Ratio& b) { return !ratio_ge(b, a); }
bool ratio_lt(const Ratio& a, const Ratio& b) { return !ratio_ge(a, b); }
bool ratio_eq(const Ratio& a, const Ratio& b) { return a.num * b.den == b.num * a.den; }
Ratio ratio_min(const Ratio& a, const Ratio& b) { return ratio_le(a, b) ? a : b; }

u64 ratio_ppm(const Ratio& r) {
  if (r.den == 0) return std::numeric_limits<u64>::max();
  return mul_div_floor_sat(r.num, kPpmScale, r.den);
}

std::string ratio_to_string(const Ratio& r) {
  std::string s = std::to_string(r.num);
  s += ':';
  s += std::to_string(r.den);
  return s;
}

}  // namespace oversub
