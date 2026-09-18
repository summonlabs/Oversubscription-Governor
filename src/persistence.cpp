#include "oversub/persistence.hpp"

#include <cstdio>
#include <filesystem>
#include <ostream>
#include <system_error>

#if defined(_WIN32)
#include <io.h>
#include <windows.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif

namespace oversub {
namespace {

constexpr u8 kStateMagic[8] = {'O', 'S', 'G', 'V', 'S', 'T', 'A', '1'};
constexpr u32 kStateFormatVersion = 1;
constexpr u8 kJournalMagic[4] = {'O', 'S', 'J', '1'};
constexpr std::size_t kStateHeaderBytes = 8 + 4 + 4 + 8;
constexpr std::size_t kJournalHeaderBytes = 4 + 4 + 4 + 8;

Status io_error(ReasonCode reason, std::string detail) {
  return Status::failure(StatusCode::IoError, reason, std::move(detail));
}

Status corrupt(ReasonCode reason, std::string detail) {
  return Status::failure(StatusCode::CorruptData, reason, std::move(detail));
}

bool flush_stream(std::FILE* file) {
#if defined(_WIN32)
  return _commit(_fileno(file)) == 0;
#else
  return ::fsync(fileno(file)) == 0;
#endif
}

bool sync_directory(const std::string& directory) {
#if defined(_WIN32)
  (void)directory;
  return true;
#else
  const int fd = ::open(directory.c_str(), O_RDONLY);
  if (fd < 0) return false;
  const bool ok = ::fsync(fd) == 0;
  ::close(fd);
  return ok;
#endif
}

bool replace_file(const std::string& from, const std::string& to) {
#if defined(_WIN32)
  const std::wstring wide_from = std::filesystem::path(from).wstring();
  const std::wstring wide_to = std::filesystem::path(to).wstring();
  return ::MoveFileExW(wide_from.c_str(), wide_to.c_str(),
                       MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
#else
  return ::rename(from.c_str(), to.c_str()) == 0;
#endif
}

void append_u32(std::vector<u8>& out, u32 value) {
  for (int i = 0; i < 4; ++i) out.push_back(static_cast<u8>((value >> (i * 8)) & 0xFF));
}

void append_u64(std::vector<u8>& out, u64 value) {
  for (int i = 0; i < 8; ++i) out.push_back(static_cast<u8>((value >> (i * 8)) & 0xFF));
}

bool read_u32(const std::vector<u8>& in, std::size_t& offset, u32& value) {
  if (offset + 4 > in.size()) return false;
  value = 0;
  for (int i = 0; i < 4; ++i) value |= static_cast<u32>(in[offset + i]) << (i * 8);
  offset += 4;
  return true;
}

bool read_u64(const std::vector<u8>& in, std::size_t& offset, u64& value) {
  if (offset + 8 > in.size()) return false;
  value = 0;
  for (int i = 0; i < 8; ++i) value |= static_cast<u64>(in[offset + i]) << (i * 8);
  offset += 8;
  return true;
}

std::vector<u8> make_state_file(const std::vector<u8>& payload) {
  std::vector<u8> out;
  out.reserve(kStateHeaderBytes + payload.size() + 32);
  out.insert(out.end(), kStateMagic, kStateMagic + 8);
  append_u32(out, kStateFormatVersion);
  append_u32(out, 0);
  append_u64(out, payload.size());
  out.insert(out.end(), payload.begin(), payload.end());
  const Digest digest = sha256(payload.data(), payload.size());
  out.insert(out.end(), digest.bytes.begin(), digest.bytes.end());
  return out;
}

std::vector<u8> make_journal_frame(const std::vector<u8>& payload) {
  std::vector<u8> out;
  out.reserve(kJournalHeaderBytes + payload.size() + 32);
  out.insert(out.end(), kJournalMagic, kJournalMagic + 4);
  append_u32(out, kJournalFormatVersion);
  append_u32(out, 0);
  append_u64(out, payload.size());
  out.insert(out.end(), payload.begin(), payload.end());
  const Digest digest = sha256(payload.data(), payload.size());
  out.insert(out.end(), digest.bytes.begin(), digest.bytes.end());
  return out;
}

}  // namespace

Status read_file_bounded(const std::string& path, u64 max_bytes, std::vector<u8>& out) {
  std::error_code ec;
  const auto status = std::filesystem::status(path, ec);
  if (ec || !std::filesystem::exists(status)) {
    return Status::failure(StatusCode::NotFound, ReasonCode::None, "file not found: " + path);
  }
  if (!std::filesystem::is_regular_file(status)) {
    return io_error(ReasonCode::PersistenceCorrupt, "not a regular file: " + path);
  }
  const std::uintmax_t size = std::filesystem::file_size(path, ec);
  if (ec) return io_error(ReasonCode::None, "cannot size file: " + path);
  if (size > max_bytes) {
    return Status::failure(StatusCode::OutOfRange, ReasonCode::PersistenceTooLarge,
                           "file exceeds the configured bound: " + path);
  }
  std::FILE* file = std::fopen(path.c_str(), "rb");
  if (file == nullptr) return io_error(ReasonCode::None, "cannot open file: " + path);
  std::vector<u8> buffer(static_cast<std::size_t>(size));
  const std::size_t read = buffer.empty() ? 0 : std::fread(buffer.data(), 1, buffer.size(), file);
  const bool short_read = read != buffer.size();
  std::fclose(file);
  if (short_read) return io_error(ReasonCode::PersistenceTruncated, "short read: " + path);
  out = std::move(buffer);
  return Status::success();
}

Status write_file_atomic(const std::string& path, const std::vector<u8>& bytes, bool sync_writes) {
  const std::string temp = path + ".tmp";
  {
    std::FILE* file = std::fopen(temp.c_str(), "wb");
    if (file == nullptr) return io_error(ReasonCode::None, "cannot create temporary file: " + temp);
    const std::size_t written = bytes.empty() ? 0 : std::fwrite(bytes.data(), 1, bytes.size(), file);
    const bool flushed = std::fflush(file) == 0;
    const bool synced = !sync_writes || flush_stream(file);
    const bool closed = std::fclose(file) == 0;
    if (written != bytes.size() || !flushed || !synced || !closed) {
      (void)remove_file(temp);
      return io_error(ReasonCode::None, "failed to write temporary file: " + temp);
    }
  }
  if (!replace_file(temp, path)) {
    (void)remove_file(temp);
    return io_error(ReasonCode::None, "atomic replace failed: " + path);
  }
  if (sync_writes) {
    const std::string directory = std::filesystem::path(path).parent_path().string();
    if (!directory.empty()) (void)sync_directory(directory);
  }
  return Status::success();
}

Status append_file(const std::string& path, const std::vector<u8>& bytes, bool sync_writes) {
  std::FILE* file = std::fopen(path.c_str(), "ab");
  if (file == nullptr) return io_error(ReasonCode::None, "cannot append to file: " + path);
  const std::size_t written = bytes.empty() ? 0 : std::fwrite(bytes.data(), 1, bytes.size(), file);
  const bool flushed = std::fflush(file) == 0;
  const bool synced = !sync_writes || flush_stream(file);
  const bool closed = std::fclose(file) == 0;
  if (written != bytes.size() || !flushed || !synced || !closed) {
    return io_error(ReasonCode::None, "append failed: " + path);
  }
  return Status::success();
}

Status remove_file(const std::string& path) {
  std::error_code ec;
  std::filesystem::remove(path, ec);
  if (ec) return io_error(ReasonCode::None, "cannot remove file: " + path);
  return Status::success();
}

const char* to_string(LoadOutcome outcome) {
  switch (outcome) {
    case LoadOutcome::Fresh: return "FRESH";
    case LoadOutcome::Loaded: return "LOADED";
    case LoadOutcome::Recovered: return "RECOVERED";
  }
  return "UNKNOWN";
}

std::ostream& operator<<(std::ostream& os, LoadOutcome outcome) { return os << to_string(outcome); }

Store::~Store() { close(); }

Status Store::open(StoreOptions options) {
  if (options.directory.empty()) {
    return Status::failure(StatusCode::InvalidArgument, ReasonCode::PersistenceNotInitialized,
                           "store directory is empty");
  }
  if (options.max_state_bytes == 0 || options.max_journal_bytes == 0 || options.max_journal_records == 0) {
    return Status::failure(StatusCode::InvalidArgument, ReasonCode::LimitExceeded,
                           "store bounds must be non-zero");
  }
  if (options.max_audit_records == 0 || options.max_audit_records > kMaxAuditRecords) {
    return Status::failure(StatusCode::InvalidArgument, ReasonCode::LimitExceeded,
                           "max_audit_records out of bound");
  }
  options_ = std::move(options);
  Status status = ensure_directory();
  if (!status.ok()) return status;
  open_ = true;
  journal_bytes_ = 0;
  journal_records_ = 0;
  std::error_code ec;
  const auto journal_size = std::filesystem::file_size(journal_path(), ec);
  if (!ec) journal_bytes_ = static_cast<u64>(journal_size);
  return Status::success();
}

void Store::close() {
  open_ = false;
  journal_bytes_ = 0;
  journal_records_ = 0;
}

std::string Store::state_path() const { return (std::filesystem::path(options_.directory) / "state.bin").string(); }

std::string Store::journal_path() const {
  return (std::filesystem::path(options_.directory) / "journal.bin").string();
}

Status Store::ensure_directory() {
  std::error_code ec;
  std::filesystem::create_directories(options_.directory, ec);
  if (ec) return io_error(ReasonCode::None, "cannot create store directory: " + options_.directory);
  if (!std::filesystem::is_directory(options_.directory)) {
    return io_error(ReasonCode::None, "store path is not a directory: " + options_.directory);
  }
  return Status::success();
}

Status Store::save(const DurableState& state) {
  if (!open_) {
    return Status::failure(StatusCode::InvalidArgument, ReasonCode::PersistenceNotInitialized,
                           "store is not open");
  }
  std::vector<u8> payload;
  Status status = serialize_state(state, payload);
  if (!status.ok()) return status;
  if (payload.size() > options_.max_state_bytes) {
    return Status::failure(StatusCode::OutOfRange, ReasonCode::PersistenceTooLarge,
                           "serialized state exceeds max_state_bytes");
  }
  return write_file_atomic(state_path(), make_state_file(payload), options_.sync_writes);
}

Status Store::load(DurableState& state, LoadReport& report) {
  if (!open_) {
    return Status::failure(StatusCode::InvalidArgument, ReasonCode::PersistenceNotInitialized,
                           "store is not open");
  }
  report = LoadReport{};
  std::vector<u8> bytes;
  Status status = read_file_bounded(state_path(), options_.max_state_bytes + kStateHeaderBytes + 32, bytes);
  if (!status.ok()) {
    if (status.code == StatusCode::NotFound) {
      report.outcome = LoadOutcome::Fresh;
      report.reason = ReasonCode::PersistenceNotInitialized;
      report.detail = "no durable state present";
      return Status::success();
    }
    return status;
  }
  if (bytes.size() < kStateHeaderBytes + 32) {
    return corrupt(ReasonCode::PersistenceTruncated, "state file is shorter than its header");
  }
  if (!std::equal(kStateMagic, kStateMagic + 8, bytes.begin())) {
    return corrupt(ReasonCode::PersistenceCorrupt, "state file magic mismatch");
  }
  std::size_t offset = 8;
  u32 format = 0;
  u32 reserved = 0;
  u64 payload_length = 0;
  if (!read_u32(bytes, offset, format) || !read_u32(bytes, offset, reserved) ||
      !read_u64(bytes, offset, payload_length)) {
    return corrupt(ReasonCode::PersistenceCorrupt, "state file header truncated");
  }
  if (format != kStateFormatVersion) {
    return Status::failure(StatusCode::Unsupported, ReasonCode::PersistenceVersionUnsupported,
                           "unsupported state file format version " + std::to_string(format));
  }
  if (payload_length > options_.max_state_bytes) {
    return Status::failure(StatusCode::OutOfRange, ReasonCode::PersistenceTooLarge,
                           "state payload exceeds max_state_bytes");
  }
  if (bytes.size() != kStateHeaderBytes + payload_length + 32) {
    return corrupt(ReasonCode::PersistenceTruncated, "state file length does not match its header");
  }
  std::vector<u8> payload(bytes.begin() + static_cast<std::ptrdiff_t>(kStateHeaderBytes),
                          bytes.begin() + static_cast<std::ptrdiff_t>(kStateHeaderBytes + payload_length));
  Digest stored;
  std::copy(bytes.end() - 32, bytes.end(), stored.bytes.begin());
  const Digest computed = sha256(payload.data(), payload.size());
  if (computed != stored) {
    return Status::failure(StatusCode::IntegrityFailure, ReasonCode::PersistenceIntegrityFailure,
                           "state payload digest mismatch");
  }
  DurableState decoded;
  status = deserialize_state(payload, decoded);
  if (!status.ok()) return status;
  status = validate_policy(decoded.policy);
  if (!status.ok()) {
    return Status::failure(StatusCode::CorruptData, ReasonCode::PersistenceCorrupt,
                           std::string("durable policy is invalid: ") + status.to_string());
  }
  if (decoded.policy_digest != decoded.policy.digest) {
    return Status::failure(StatusCode::IntegrityFailure, ReasonCode::PolicyDigestMismatch,
                           "durable policy digest does not match the serialized policy");
  }

  std::vector<AuditRecord> records;
  LoadReport journal_report;
  Status journal_status = load_journal(records, journal_report);
  if (!journal_status.ok()) return journal_status;
  report = journal_report;
  report.journal_records = records.size();
  if (report.outcome == LoadOutcome::Fresh) report.outcome = LoadOutcome::Loaded;

  if (!records.empty()) {
    std::vector<AuditRecord> merged;
    merged.reserve(decoded.audit.size() + records.size());
    const u64 oldest_journal_id = records.empty() ? 0 : records.front().id.value();
    for (const auto& record : decoded.audit) {
      if (record.id.value() >= oldest_journal_id) continue;  // the journal holds the newer records
      merged.push_back(record);
    }
    for (const auto& record : records) merged.push_back(record);
    if (merged.size() > options_.max_audit_records) {
      const std::size_t excess = merged.size() - options_.max_audit_records;
      merged.erase(merged.begin(), merged.begin() + static_cast<std::ptrdiff_t>(excess));
    }
    decoded.audit = std::move(merged);
    ReplayReport replay;
    Status replay_status = rebuild_from_audit(decoded, records, replay);
    if (!replay_status.ok()) return replay_status;
    if (replay.records_skipped > 0) {
      report.detail += " some audit records were skipped during replay";
      report.reason = replay.last_reason;
    }
  }
  // Dynamic evidence is never restored: its generations survive, its content
  // does not, so every slot requires revalidation.
  for (auto& slot : decoded.evidence) slot.revalidation_required = true;
  decoded.journal_records = records.size();
  state = std::move(decoded);
  return Status::success();
}

Status Store::append_audit(const AuditRecord& record) {
  if (!open_) {
    return Status::failure(StatusCode::InvalidArgument, ReasonCode::PersistenceNotInitialized,
                           "store is not open");
  }
  std::vector<u8> payload;
  Status status = serialize_audit_record(record, payload);
  if (!status.ok()) return status;
  std::vector<u8> frame = make_journal_frame(payload);
  if (journal_bytes_ + frame.size() > options_.max_journal_bytes ||
      journal_records_ + 1 > options_.max_journal_records) {
    return Status::failure(StatusCode::Overflow, ReasonCode::PersistenceTooLarge,
                           "journal bound reached; compaction required");
  }
  status = append_file(journal_path(), frame, options_.sync_writes);
  if (!status.ok()) return status;
  journal_bytes_ += frame.size();
  journal_records_ += 1;
  return Status::success();
}

Status Store::load_journal(std::vector<AuditRecord>& records, LoadReport& report) {
  if (!open_) {
    return Status::failure(StatusCode::InvalidArgument, ReasonCode::PersistenceNotInitialized,
                           "store is not open");
  }
  records.clear();
  report = LoadReport{};
  report.outcome = LoadOutcome::Fresh;
  journal_bytes_ = 0;
  journal_records_ = 0;
  std::vector<u8> bytes;
  Status status = read_file_bounded(journal_path(), options_.max_journal_bytes + (1ULL << 20), bytes);
  if (!status.ok()) {
    if (status.code == StatusCode::NotFound) return Status::success();
    return status;
  }
  std::size_t offset = 0;
  std::size_t damaged_from = bytes.size();
  bool damaged = false;
  while (offset < bytes.size()) {
    const std::size_t frame_start = offset;
    damaged_from = frame_start;
    if (bytes.size() - offset < kJournalHeaderBytes + 32) {
      damaged = true;
      break;
    }
    if (!std::equal(kJournalMagic, kJournalMagic + 4, bytes.begin() + static_cast<std::ptrdiff_t>(offset))) {
      damaged = true;
      break;
    }
    offset += 4;
    u32 version = 0;
    u32 reserved = 0;
    u64 payload_length = 0;
    if (!read_u32(bytes, offset, version) || !read_u32(bytes, offset, reserved) ||
        !read_u64(bytes, offset, payload_length)) {
      damaged = true;
      break;
    }
    if (version != kJournalFormatVersion || payload_length > options_.max_state_bytes) {
      damaged = true;
      break;
    }
    if (bytes.size() - offset < payload_length + 32) {
      damaged = true;
      break;
    }
    std::vector<u8> payload(bytes.begin() + static_cast<std::ptrdiff_t>(offset),
                            bytes.begin() + static_cast<std::ptrdiff_t>(offset + payload_length));
    Digest stored;
    std::copy(bytes.begin() + static_cast<std::ptrdiff_t>(offset + payload_length),
              bytes.begin() + static_cast<std::ptrdiff_t>(offset + payload_length + 32), stored.bytes.begin());
    const Digest computed = sha256(payload.data(), payload.size());
    if (computed != stored) {
      damaged = true;
      break;
    }
    AuditRecord record;
    Status record_status = deserialize_audit_record(payload, record);
    if (!record_status.ok()) {
      damaged = true;
      break;
    }
    records.push_back(std::move(record));
    offset += payload_length + 32;
    if (records.size() > options_.max_journal_records) {
      damaged = true;
      break;
    }
  }
  report.journal_records = records.size();
  report.journal_bytes_recovered = offset;
  if (damaged) {
    report.outcome = LoadOutcome::Recovered;
    report.reason = ReasonCode::PersistenceJournalPartial;
    // Everything from the start of the damaged frame onward is unusable.
    report.discarded_tail_bytes = bytes.size() - damaged_from;
    report.detail = "journal tail was incomplete or corrupt; the valid prefix was retained";
    std::vector<u8> rebuilt;
    for (const auto& record : records) {
      std::vector<u8> payload;
      Status serialize_status = serialize_audit_record(record, payload);
      if (!serialize_status.ok()) return serialize_status;
      const std::vector<u8> frame = make_journal_frame(payload);
      rebuilt.insert(rebuilt.end(), frame.begin(), frame.end());
    }
    Status rewrite = write_file_atomic(journal_path(), rebuilt, options_.sync_writes);
    if (!rewrite.ok()) return rewrite;
    journal_bytes_ = rebuilt.size();
  } else {
    report.outcome = records.empty() ? LoadOutcome::Fresh : LoadOutcome::Loaded;
    journal_bytes_ = offset;
  }
  journal_records_ = records.size();
  return Status::success();
}

Status Store::compact_journal(const std::vector<AuditRecord>& records, u64 keep) {
  if (!open_) {
    return Status::failure(StatusCode::InvalidArgument, ReasonCode::PersistenceNotInitialized,
                           "store is not open");
  }
  if (keep > options_.max_journal_records) keep = options_.max_journal_records;
  const std::size_t begin = records.size() > keep ? records.size() - static_cast<std::size_t>(keep) : 0;
  std::vector<u8> rebuilt;
  u64 kept = 0;
  for (std::size_t i = begin; i < records.size(); ++i) {
    std::vector<u8> payload;
    Status status = serialize_audit_record(records[i], payload);
    if (!status.ok()) return status;
    const std::vector<u8> frame = make_journal_frame(payload);
    if (rebuilt.size() + frame.size() > options_.max_journal_bytes) break;
    rebuilt.insert(rebuilt.end(), frame.begin(), frame.end());
    ++kept;
  }
  Status status = write_file_atomic(journal_path(), rebuilt, options_.sync_writes);
  if (!status.ok()) return status;
  journal_bytes_ = rebuilt.size();
  journal_records_ = kept;
  return Status::success();
}

}  // namespace oversub
