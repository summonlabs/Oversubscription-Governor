// Oversubscription Governor — SHA-256 digests and canonical hashing.
#ifndef OVERSUB_DIGEST_HPP
#define OVERSUB_DIGEST_HPP

#include <array>
#include <iosfwd>
#include <string>
#include <string_view>

#include "oversub/checked.hpp"

namespace oversub {

struct Digest {
  std::array<u8, 32> bytes{};

  friend bool operator==(const Digest& a, const Digest& b) { return a.bytes == b.bytes; }
  friend bool operator!=(const Digest& a, const Digest& b) { return a.bytes != b.bytes; }
  friend bool operator<(const Digest& a, const Digest& b) { return a.bytes < b.bytes; }
  bool is_zero() const;
  std::string to_hex() const;
  static Digest zero() { return Digest{}; }
};

std::ostream& operator<<(std::ostream& os, const Digest& digest);

class Sha256 {
 public:
  Sha256();
  void update(const void* data, std::size_t len);
  void update(std::string_view text);
  Digest finish();

 private:
  void transform(const u8* block);
  u32 state_[8];
  u64 bit_length_;
  u8 buffer_[64];
  std::size_t buffer_len_;
  bool finished_;
};

Digest sha256(const void* data, std::size_t len);
Digest sha256(std::string_view text);

// Deterministic canonical hasher. Fields must be added in a fixed order; every
// multi-byte integer is encoded little-endian with a fixed width so digests are
// identical across compilers, platforms, and process restarts.
class CanonicalHasher {
 public:
  void add_u8(u8 value);
  void add_u32(u32 value);
  void add_u64(u64 value);
  void add_bool(bool value);
  void add_string(std::string_view value);
  void add_bytes(const void* data, std::size_t len);
  void add_digest(const Digest& digest);
  // Compact 64-bit fold used where a full digest is unnecessary (resource-set
  // fingerprints inside authority vectors).
  u64 fold64() const;
  Digest digest() const;

 private:
  Sha256 sha_;
};

}  // namespace oversub

#endif  // OVERSUB_DIGEST_HPP
