// Oversubscription Governor — crash-safe, integrity-checked persistence.
#ifndef OVERSUB_PERSISTENCE_HPP
#define OVERSUB_PERSISTENCE_HPP

#include <iosfwd>
#include <string>
#include <vector>

#include "oversub/state.hpp"

namespace oversub {

inline constexpr u64 kDefaultMaxStateBytes = 32ULL * 1024 * 1024;
inline constexpr u64 kDefaultMaxJournalBytes = 64ULL * 1024 * 1024;
inline constexpr u64 kDefaultMaxJournalRecords = 200000;

struct StoreOptions {
  std::string directory;
  u64 max_state_bytes{kDefaultMaxStateBytes};
  u64 max_journal_bytes{kDefaultMaxJournalBytes};
  u64 max_journal_records{kDefaultMaxJournalRecords};
  u32 max_audit_records{kMaxAuditRecords};
  bool sync_writes{true};   // fsync state/journal writes (disabled only by benchmarks)
};

enum class LoadOutcome : u32 {
  Fresh = 0,        // no durable state present; caller initializes
  Loaded,           // durable state loaded and verified
  Recovered,        // durable state loaded with a truncated/corrupt journal tail
};

const char* to_string(LoadOutcome outcome);
std::ostream& operator<<(std::ostream& os, LoadOutcome outcome);

struct LoadReport {
  LoadOutcome outcome{LoadOutcome::Fresh};
  u64 journal_records{0};
  u64 journal_bytes_recovered{0};
  u64 discarded_tail_bytes{0};
  ReasonCode reason{ReasonCode::None};
  std::string detail;
};

class Store {
 public:
  Store() = default;
  ~Store();
  Store(const Store&) = delete;
  Store& operator=(const Store&) = delete;

  Status open(StoreOptions options);
  void close();
  bool is_open() const { return open_; }
  const StoreOptions& options() const { return options_; }
  std::string state_path() const;
  std::string journal_path() const;

  // Loads durable state and recovers the journal. A missing state file yields
  // Fresh; a corrupt state file is an error (never silently ignored).
  Status load(DurableState& state, LoadReport& report);

  // Atomic, integrity-checked state replacement.
  Status save(const DurableState& state);

  // Appends one journal record. Returns Overflow (with the journal left intact)
  // when the configured bound is reached so the caller can compact first.
  Status append_audit(const AuditRecord& record);

  Status load_journal(std::vector<AuditRecord>& records, LoadReport& report);
  // Rewrites the journal with the newest `keep` records.
  Status compact_journal(const std::vector<AuditRecord>& records, u64 keep);

  u64 journal_bytes() const { return journal_bytes_; }
  u64 journal_records() const { return journal_records_; }

 private:
  Status ensure_directory();
  StoreOptions options_;
  bool open_{false};
  u64 journal_bytes_{0};
  u64 journal_records_{0};
};

// Reads a whole file with a hard bound. Returns NotFound when absent.
Status read_file_bounded(const std::string& path, u64 max_bytes, std::vector<u8>& out);
// Writes bytes to `path` durably via a temporary file and an atomic replace.
Status write_file_atomic(const std::string& path, const std::vector<u8>& bytes, bool sync_writes);
// Appends to a file, optionally flushing to stable storage.
Status append_file(const std::string& path, const std::vector<u8>& bytes, bool sync_writes);
// Removes a file if it exists.
Status remove_file(const std::string& path);

}  // namespace oversub

#endif  // OVERSUB_PERSISTENCE_HPP
