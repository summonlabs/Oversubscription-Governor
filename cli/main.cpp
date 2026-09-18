// Oversubscription Governor command-line tool.
//
// The CLI is a thin operator surface over the same public API the library
// exposes. It never invents authority: every command either validates durable
// state, replays durable history, or evaluates a SYNTHETIC population.
#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

#include "oversub/governor.hpp"
#include "oversub/net.hpp"
#include "oversub/persistence.hpp"
#include "oversub/synth.hpp"

namespace {

using namespace oversub;

struct Options {
  std::string command;
  std::string directory;
  std::string file;
  u32 domains{1};
  u32 resources{2};
  u32 guarantees{2};
  u32 demand{4};
  u64 capacity{1000000};
  u64 ratio_num{2};
  u64 ratio_den{1};
  u32 degraded{0};
  u32 failed{0};
  u64 over_demand_ppm{1000000};
  u64 tick{0};
  u64 limit{16};
  bool valid{true};
  std::string error;
};

bool parse_u64(const std::string& text, u64& out) {
  if (text.empty()) return false;
  u64 value = 0;
  for (const char c : text) {
    if (c < '0' || c > '9') return false;
    const u64 digit = static_cast<u64>(c - '0');
    if (value > (UINT64_MAX - digit) / 10) return false;
    value = value * 10 + digit;
  }
  out = value;
  return true;
}

Options parse(int argc, char** argv) {
  Options options;
  std::vector<std::string> tokens;
  for (int i = 1; i < argc; ++i) tokens.emplace_back(argv[i]);
  if (tokens.empty()) {
    options.valid = false;
    options.error = "a command is required";
    return options;
  }
  options.command = tokens[0];
  for (std::size_t i = 1; i < tokens.size(); ++i) {
    const std::string& token = tokens[i];
    auto value = [&](std::string& out) {
      if (i + 1 >= tokens.size()) {
        options.valid = false;
        options.error = "missing value for " + token;
        return false;
      }
      out = tokens[++i];
      return true;
    };
    auto number = [&](u64& out) {
      std::string text;
      if (!value(text)) return false;
      if (!parse_u64(text, out)) {
        options.valid = false;
        options.error = "invalid number for " + token;
        return false;
      }
      return true;
    };
    if (token == "--dir" || token == "--state-dir") {
      if (!value(options.directory)) return options;
    } else if (token == "--file") {
      if (!value(options.file)) return options;
    } else if (token == "--domains") {
      u64 parsed = 0;
      if (!number(parsed) || parsed == 0 || parsed > 64) {
        options.valid = false;
        options.error = "invalid --domains";
        return options;
      }
      options.domains = static_cast<u32>(parsed);
    } else if (token == "--resources") {
      u64 parsed = 0;
      if (!number(parsed) || parsed == 0 || parsed > 64) {
        options.valid = false;
        options.error = "invalid --resources";
        return options;
      }
      options.resources = static_cast<u32>(parsed);
    } else if (token == "--guarantees") {
      u64 parsed = 0;
      if (!number(parsed) || parsed > 16) {
        options.valid = false;
        options.error = "invalid --guarantees";
        return options;
      }
      options.guarantees = static_cast<u32>(parsed);
    } else if (token == "--demand") {
      u64 parsed = 0;
      if (!number(parsed) || parsed > 64) {
        options.valid = false;
        options.error = "invalid --demand";
        return options;
      }
      options.demand = static_cast<u32>(parsed);
    } else if (token == "--capacity") {
      if (!number(options.capacity) || options.capacity == 0 || options.capacity > kHardMaxCapacityUnits) {
        options.valid = false;
        options.error = "invalid --capacity";
        return options;
      }
    } else if (token == "--ratio") {
      std::string text;
      if (!value(text)) return options;
      const std::size_t split = text.find(':');
      if (split == std::string::npos || !parse_u64(text.substr(0, split), options.ratio_num) ||
          !parse_u64(text.substr(split + 1), options.ratio_den) ||
          !ratio_valid(Ratio{options.ratio_num, options.ratio_den})) {
        options.valid = false;
        options.error = "invalid --ratio (expected NUM:DEN with NUM >= DEN >= 1)";
        return options;
      }
    } else if (token == "--degraded") {
      u64 parsed = 0;
      if (!number(parsed) || parsed > 64) {
        options.valid = false;
        options.error = "invalid --degraded";
        return options;
      }
      options.degraded = static_cast<u32>(parsed);
    } else if (token == "--failed") {
      u64 parsed = 0;
      if (!number(parsed) || parsed > 64) {
        options.valid = false;
        options.error = "invalid --failed";
        return options;
      }
      options.failed = static_cast<u32>(parsed);
    } else if (token == "--over-demand-ppm") {
      if (!number(options.over_demand_ppm)) return options;
    } else if (token == "--tick") {
      if (!number(options.tick)) return options;
    } else if (token == "--limit") {
      u64 parsed = 0;
      if (!number(parsed) || parsed == 0 || parsed > 100000) {
        options.valid = false;
        options.error = "invalid --limit";
        return options;
      }
      options.limit = parsed;
    } else {
      options.valid = false;
      options.error = "unknown argument: " + token;
      return options;
    }
  }
  return options;
}

void usage() {
  std::fputs(
      "usage: oversub_cli <command> [options]\n"
      "  version                                  print the runtime version\n"
      "  evaluate   [scenario options]            evaluate a SYNTHETIC domain population\n"
      "  validate-state --dir DIR                 validate durable state\n"
      "  history    --dir DIR [--limit N]         print the durable audit tail\n"
      "  replay     --dir DIR                     rebuild runtime state from the journal\n"
      "  digest     --file PATH                   SHA-256 of a file (bounded)\n"
      "scenario options: --domains N --resources N --guarantees N --demand N --capacity N\n"
      "                  --ratio NUM:DEN --degraded N --failed N --over-demand-ppm N --tick N\n",
      stdout);
}

int command_version() {
  std::printf("Oversubscription Governor %s\n", OVERSUB_VERSION_STRING);
  std::printf("protocol_version=%u max_frame_payload=%u\n", kProtocolVersion, kMaxFramePayloadBytes);
  return 0;
}

int command_evaluate(const Options& options) {
  SynthConfig config;
  config.domains = options.domains;
  config.resources_per_domain = options.resources;
  config.guarantees_per_resource = options.guarantees;
  config.demand_records_per_resource = options.demand;
  config.capacity_per_resource = options.capacity;
  config.configured_ratio = Ratio{options.ratio_num, options.ratio_den};
  config.has_degraded_ratio_cap = true;
  config.degraded_ratio_cap = Ratio{1, 1};
  config.degraded_resources = options.degraded;
  config.failed_resources = options.failed;
  SynthesizedPopulation population = synthesize(config);
  // Scale contingent demand to exercise the overage paths deterministically.
  for (auto& record : population.admissions.records) {
    if (record.kind != DemandKind::Contingent) continue;
    record.committed_capacity = mul_div_floor_sat(record.committed_capacity, options.over_demand_ppm, kPpmScale);
  }
  population.admissions.provenance.payload_digest = admission_snapshot_digest(population.admissions);

  const std::filesystem::path directory =
      std::filesystem::temp_directory_path() / "oversub-cli-evaluate";
  std::error_code ec;
  std::filesystem::remove_all(directory, ec);
  Governor governor;
  GovernorOptions governor_options;
  governor_options.state_directory = directory.string();
  governor_options.sync_writes = false;
  Status status = governor.open(governor_options, population.policy);
  if (!status.ok()) {
    std::fprintf(stderr, "open failed: %s\n", status.to_string().c_str());
    return 1;
  }
  auto install = [&](const Status& candidate, const char* what) {
    if (candidate.ok()) return true;
    std::fprintf(stderr, "%s failed: %s\n", what, candidate.to_string().c_str());
    return false;
  };
  if (!install(governor.register_publisher(population.capacity.provenance.publisher,
                                           population.capacity.provenance.incarnation,
                                           population.capacity.epoch, options.tick),
               "register_publisher")) {
    return 1;
  }
  if (!install(governor.ingest_capacity(population.capacity).status, "ingest_capacity")) return 1;
  if (!install(governor.ingest_reservations(population.reservations).status, "ingest_reservations")) return 1;
  if (!install(governor.ingest_admissions(population.admissions).status, "ingest_admissions")) return 1;

  std::printf("population: SYNTHETIC domains=%u resources=%u guarantees=%u demand=%u capacity=%llu\n",
              options.domains, options.resources, options.guarantees, options.demand,
              static_cast<unsigned long long>(options.capacity));
  int exit_code = 0;
  for (u32 domain = 1; domain <= options.domains; ++domain) {
    EvaluationRequest request;
    request.domain = OversubscriptionDomainId(domain);
    request.tick = options.tick;
    Decision decision;
    if (!install(governor.evaluate(request, decision), "evaluate")) return 1;
    std::string text;
    ExplainOptions explain;
    explain.max_lines = 48;
    explain.max_bytes = 8192;
    if (governor.explain(decision, explain, text).ok()) std::fputs(text.c_str(), stdout);
    if (decision.effective_outcome != Outcome::WithinPolicy &&
        decision.effective_outcome != Outcome::AtLimit) {
      exit_code = 2;
    }
  }
  std::filesystem::remove_all(directory, ec);
  return exit_code;
}

int command_validate_state(const Options& options) {
  if (options.directory.empty()) {
    std::fprintf(stderr, "validate-state requires --dir\n");
    return 1;
  }
  Store store;
  StoreOptions store_options;
  store_options.directory = options.directory;
  Status status = store.open(store_options);
  if (!status.ok()) {
    std::fprintf(stderr, "open failed: %s\n", status.to_string().c_str());
    return 1;
  }
  DurableState state;
  LoadReport report;
  status = store.load(state, report);
  if (!status.ok()) {
    std::printf("state: INVALID (%s)\n", status.to_string().c_str());
    return 2;
  }
  std::printf("state: VALID\n");
  std::printf("outcome=%s journal_records=%llu discarded_tail_bytes=%llu\n", to_string(report.outcome),
              static_cast<unsigned long long>(report.journal_records),
              static_cast<unsigned long long>(report.discarded_tail_bytes));
  std::printf("incarnation=%llu epoch=%llu policy_generation=%llu decisions=%llu audit_records=%llu\n",
              static_cast<unsigned long long>(state.incarnation.value()),
              static_cast<unsigned long long>(state.epoch.value()),
              static_cast<unsigned long long>(state.policy_generation.value()),
              static_cast<unsigned long long>(state.decisions_issued),
              static_cast<unsigned long long>(state.audit.size()));
  std::printf("domains=%llu publishers=%llu evidence_slots=%llu\n",
              static_cast<unsigned long long>(state.domains.size()),
              static_cast<unsigned long long>(state.publishers.size()),
              static_cast<unsigned long long>(state.evidence.size()));
  for (const auto& domain : state.domains) {
    std::printf("  domain=%llu generation=%llu sticky=%s fenced=%s confirmations=%u\n",
                static_cast<unsigned long long>(domain.domain.value()),
                static_cast<unsigned long long>(domain.generation.value()),
                to_string(domain.sticky_outcome), domain.fenced ? "true" : "false", domain.confirmations);
  }
  return 0;
}

int command_history(const Options& options) {
  if (options.directory.empty()) {
    std::fprintf(stderr, "history requires --dir\n");
    return 1;
  }
  Governor governor;
  GovernorOptions governor_options;
  governor_options.state_directory = options.directory;
  governor_options.sync_writes = false;
  Status status = governor.open(governor_options, OversubscriptionPolicy{});
  if (!status.ok()) {
    std::fprintf(stderr, "open failed: %s\n", status.to_string().c_str());
    return 1;
  }
  std::vector<AuditRecord> records;
  status = governor.history(records, static_cast<std::size_t>(options.limit));
  if (!status.ok()) {
    std::fprintf(stderr, "history failed: %s\n", status.to_string().c_str());
    return 1;
  }
  std::printf("audit records: %llu\n", static_cast<unsigned long long>(records.size()));
  for (const auto& record : records) {
    std::printf("  id=%llu tick=%llu domain=%llu raw=%s effective=%s authorized=%s increment=%llu fenced=%s\n",
                static_cast<unsigned long long>(record.id.value()),
                static_cast<unsigned long long>(record.tick),
                static_cast<unsigned long long>(record.domain.value()), to_string(record.raw_outcome),
                to_string(record.effective_outcome), record.authorized ? "true" : "false",
                static_cast<unsigned long long>(record.authorized_increment),
                record.fenced ? "true" : "false");
  }
  return governor.close().ok() ? 0 : 1;
}

int command_replay(const Options& options) {
  if (options.directory.empty()) {
    std::fprintf(stderr, "replay requires --dir\n");
    return 1;
  }
  Store store;
  StoreOptions store_options;
  store_options.directory = options.directory;
  store_options.sync_writes = false;
  Status status = store.open(store_options);
  if (!status.ok()) {
    std::fprintf(stderr, "open failed: %s\n", status.to_string().c_str());
    return 1;
  }
  DurableState state;
  LoadReport report;
  status = store.load(state, report);
  if (!status.ok()) {
    std::printf("replay: FAILED (%s)\n", status.to_string().c_str());
    return 2;
  }
  std::vector<AuditRecord> records;
  LoadReport journal_report;
  status = store.load_journal(records, journal_report);
  if (!status.ok()) {
    std::printf("journal load failed: %s\n", status.to_string().c_str());
    return 2;
  }
  DurableState rebuilt = state;
  ReplayReport replay;
  status = rebuild_from_audit(rebuilt, records, replay);
  if (!status.ok()) {
    std::printf("replay failed: %s\n", status.to_string().c_str());
    return 2;
  }
  std::printf("journal: outcome=%s records=%llu discarded_tail_bytes=%llu\n",
              to_string(journal_report.outcome), static_cast<unsigned long long>(records.size()),
              static_cast<unsigned long long>(journal_report.discarded_tail_bytes));
  std::printf("replay: seen=%llu applied=%llu skipped=%llu\n",
              static_cast<unsigned long long>(replay.records_seen),
              static_cast<unsigned long long>(replay.records_applied),
              static_cast<unsigned long long>(replay.records_skipped));
  std::printf("rebuild agrees with durable state: %s\n",
              rebuilt.decisions_issued == state.decisions_issued ? "true" : "false");
  return rebuilt.decisions_issued == state.decisions_issued ? 0 : 3;
}

int command_digest(const Options& options) {
  if (options.file.empty()) {
    std::fprintf(stderr, "digest requires --file\n");
    return 1;
  }
  std::vector<u8> bytes;
  Status status = read_file_bounded(options.file, kDefaultMaxStateBytes, bytes);
  if (!status.ok()) {
    std::fprintf(stderr, "read failed: %s\n", status.to_string().c_str());
    return 1;
  }
  std::printf("sha256=%s bytes=%llu\n", sha256(bytes.data(), bytes.size()).to_hex().c_str(),
              static_cast<unsigned long long>(bytes.size()));
  return 0;
}

}  // namespace

int main(int argc, char** argv) {
  const Options options = parse(argc, argv);
  if (!options.valid) {
    std::fprintf(stderr, "oversub_cli: %s\n", options.error.c_str());
    usage();
    return 2;
  }
  if (options.command == "version") return command_version();
  if (options.command == "evaluate") return command_evaluate(options);
  if (options.command == "validate-state") return command_validate_state(options);
  if (options.command == "history") return command_history(options);
  if (options.command == "replay") return command_replay(options);
  if (options.command == "digest") return command_digest(options);
  std::fprintf(stderr, "oversub_cli: unknown command '%s'\n", options.command.c_str());
  usage();
  return 2;
}
