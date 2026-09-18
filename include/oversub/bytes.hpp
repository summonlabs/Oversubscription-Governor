// Oversubscription Governor — bounded little-endian binary codec.
//
// Every read is length-checked. Strings and blobs carry an explicit bound so a
// corrupt or hostile payload cannot cause an unbounded allocation.
#ifndef OVERSUB_BYTES_HPP
#define OVERSUB_BYTES_HPP

#include <cstring>
#include <string>
#include <vector>

#include "oversub/checked.hpp"
#include "oversub/digest.hpp"

namespace oversub {

// The scalar accessors are named after their widths, so their parameter and
// local types are spelled with explicit namespace qualification.
class ByteWriter {
 public:
  void u8(oversub::u8 value) { buf_.push_back(value); }

  void u32(oversub::u32 value) {
    for (int i = 0; i < 4; ++i) buf_.push_back(static_cast<oversub::u8>((value >> (i * 8)) & 0xFF));
  }

  void u64(oversub::u64 value) {
    for (int i = 0; i < 8; ++i) buf_.push_back(static_cast<oversub::u8>((value >> (i * 8)) & 0xFF));
  }

  void boolean(bool value) { u8(value ? 1 : 0); }

  bool string(const std::string& value, oversub::u32 max_length) {
    if (value.size() > max_length) return false;
    u32(static_cast<oversub::u32>(value.size()));
    buf_.insert(buf_.end(), value.begin(), value.end());
    return true;
  }

  void raw(const void* data, std::size_t len) {
    const oversub::u8* p = static_cast<const oversub::u8*>(data);
    buf_.insert(buf_.end(), p, p + len);
  }

  void digest(const Digest& value) { raw(value.bytes.data(), value.bytes.size()); }

  std::size_t size() const { return buf_.size(); }
  const std::vector<oversub::u8>& data() const { return buf_; }
  std::vector<oversub::u8>& data() { return buf_; }

 private:
  std::vector<oversub::u8> buf_;
};

class ByteReader {
 public:
  ByteReader(const oversub::u8* data, std::size_t size) : data_(data), size_(size) {}
  explicit ByteReader(const std::vector<oversub::u8>& buffer)
      : data_(buffer.data()), size_(buffer.size()) {}

  bool u8(oversub::u8& out) {
    if (offset_ + 1 > size_) return false;
    out = data_[offset_++];
    return true;
  }

  bool u32(oversub::u32& out) {
    if (offset_ + 4 > size_) return false;
    out = 0;
    for (int i = 0; i < 4; ++i) out |= static_cast<oversub::u32>(data_[offset_ + i]) << (i * 8);
    offset_ += 4;
    return true;
  }

  bool u64(oversub::u64& out) {
    if (offset_ + 8 > size_) return false;
    out = 0;
    for (int i = 0; i < 8; ++i) out |= static_cast<oversub::u64>(data_[offset_ + i]) << (i * 8);
    offset_ += 8;
    return true;
  }

  bool boolean(bool& out) {
    oversub::u8 value = 0;
    if (!u8(value)) return false;
    out = value != 0;
    return true;
  }

  bool string(std::string& out, oversub::u32 max_length) {
    oversub::u32 length = 0;
    if (!u32(length)) return false;
    if (length > max_length) return false;
    if (offset_ + length > size_) return false;
    out.assign(reinterpret_cast<const char*>(data_ + offset_), length);
    offset_ += length;
    return true;
  }

  bool raw(void* out, std::size_t len) {
    if (offset_ + len > size_) return false;
    std::memcpy(out, data_ + offset_, len);
    offset_ += len;
    return true;
  }

  bool digest(Digest& out) { return raw(out.bytes.data(), out.bytes.size()); }

  std::size_t offset() const { return offset_; }
  std::size_t left() const { return size_ - offset_; }
  bool at_end() const { return offset_ == size_; }

 private:
  const oversub::u8* data_;
  std::size_t size_;
  std::size_t offset_{0};
};

}  // namespace oversub

#endif  // OVERSUB_BYTES_HPP
