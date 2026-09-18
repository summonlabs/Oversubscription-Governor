// Oversubscription Governor — multiprocess deployment tool.
//
// Both roles are real processes: the coordinator owns durable authority and the
// worker publishes generation-bound evidence over framed TCP. Nothing here
// simulates a network; loopback TCP is a real transport with real processes.
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>
#include <vector>

#include "coordinator.hpp"
#include "oversub/process.hpp"
#include "oversub/synth.hpp"
#include "worker.hpp"

namespace {

using namespace oversub;
using namespace oversub::dist;

struct Arguments {
  std::string role;
  std::string address{"127.0.0.1"};
  u16 port{0};
  std::string state_directory;
  std::string report_path;
  u64 publisher{1};
  u64 incarnation{1};
  u64 domain{1};
  u64 tick{0};
  u32 domains{1};
  u32 resources{4};
  u32 guarantees{2};
  u32 demand{4};
  u64 capacity{1000000};
  u32 rounds{1};
  bool adopt{false};
  bool auto_generations{false};
  bool explicit_generations{true};
  bool revalidate{false};
  bool replay{false};
  bool stale_epoch{false};
  std::string expect_outcome;
  bool valid{true};
  std::string error;
};

u64 parse_u64(const std::string& text, bool& ok) {
  if (text.empty()) {
    ok = false;
    return 0;
  }
  u64 value = 0;
  for (const char c : text) {
    if (c < '0' || c > '9') {
      ok = false;
      return 0;
    }
    const u64 digit = static_cast<u64>(c - '0');
    if (value > (UINT64_MAX - digit) / 10) {
      ok = false;
      return 0;
    }
    value = value * 10 + digit;
  }
  ok = true;
  return value;
}

Arguments parse(int argc, char** argv) {
  Arguments args;
  std::vector<std::string> tokens;
  for (int i = 1; i < argc; ++i) tokens.emplace_back(argv[i]);
  if (tokens.empty()) {
    args.valid = false;
    args.error = "role argument is required";
    return args;
  }
  args.role = tokens[0];
  for (std::size_t i = 1; i < tokens.size(); ++i) {
    const std::string& token = tokens[i];
    auto next = [&](std::string& out) {
      if (i + 1 >= tokens.size()) {
        args.valid = false;
        args.error = "missing value for " + token;
        return false;
      }
      out = tokens[++i];
      return true;
    };
    auto next_u64 = [&](u64& out) {
      std::string text;
      if (!next(text)) return false;
      bool ok = false;
      out = parse_u64(text, ok);
      if (!ok) {
        args.valid = false;
        args.error = "invalid numeric value for " + token;
        return false;
      }
      return true;
    };
    if (token == "--address") {
      if (!next(args.address)) return args;
    } else if (token == "--port") {
      u64 value = 0;
      if (!next_u64(value) || value > 65535) {
        args.valid = false;
        args.error = "invalid port";
        return args;
      }
      args.port = static_cast<u16>(value);
    } else if (token == "--state-dir") {
      if (!next(args.state_directory)) return args;
    } else if (token == "--report") {
      if (!next(args.report_path)) return args;
    } else if (token == "--publisher") {
      if (!next_u64(args.publisher)) return args;
    } else if (token == "--incarnation") {
      if (!next_u64(args.incarnation)) return args;
    } else if (token == "--domain") {
      if (!next_u64(args.domain)) return args;
    } else if (token == "--tick") {
      if (!next_u64(args.tick)) return args;
    } else if (token == "--domains") {
      u64 value = 0;
      if (!next_u64(value) || value == 0 || value > 64) {
        args.valid = false;
        args.error = "invalid --domains";
        return args;
      }
      args.domains = static_cast<u32>(value);
    } else if (token == "--resources") {
      u64 value = 0;
      if (!next_u64(value) || value == 0 || value > 64) {
        args.valid = false;
        args.error = "invalid --resources";
        return args;
      }
      args.resources = static_cast<u32>(value);
    } else if (token == "--guarantees") {
      u64 value = 0;
      if (!next_u64(value) || value > 16) {
        args.valid = false;
        args.error = "invalid --guarantees";
        return args;
      }
      args.guarantees = static_cast<u32>(value);
    } else if (token == "--demand") {
      u64 value = 0;
      if (!next_u64(value) || value > 64) {
        args.valid = false;
        args.error = "invalid --demand";
        return args;
      }
      args.demand = static_cast<u32>(value);
    } else if (token == "--capacity") {
      u64 value = 0;
      if (!next_u64(value) || value == 0 || value > kHardMaxCapacityUnits) {
        args.valid = false;
        args.error = "invalid --capacity";
        return args;
      }
      args.capacity = value;
    } else if (token == "--rounds") {
      u64 value = 0;
      if (!next_u64(value) || value == 0 || value > 64) {
        args.valid = false;
        args.error = "invalid --rounds";
        return args;
      }
      args.rounds = static_cast<u32>(value);
    } else if (token == "--expect-outcome") {
      if (!next(args.expect_outcome)) return args;
    } else if (token == "--adopt") {
      args.adopt = true;
    } else if (token == "--auto-generations") {
      args.auto_generations = true;
      args.explicit_generations = false;
    } else if (token == "--revalidate") {
      args.revalidate = true;
    } else if (token == "--replay") {
      args.replay = true;
    } else if (token == "--stale-epoch") {
      args.stale_epoch = true;
    } else {
      args.valid = false;
      args.error = "unknown argument: " + token;
      return args;
    }
  }
  return args;
}

SynthConfig config_from(const Arguments& args) {
  SynthConfig config;
  config.domains = args.domains;
  config.resources_per_domain = args.resources;
  config.guarantees_per_resource = args.guarantees;
  config.demand_records_per_resource = args.demand;
  config.capacity_per_resource = args.capacity;
  config.publisher = PublisherId(args.publisher);
  config.incarnation = IncarnationId(args.incarnation);
  return config;
}

std::string outcome_name(Outcome outcome) { return to_string(outcome); }

struct Report {
  std::vector<std::string> lines;
  void add(const std::string& key, const std::string& value) { lines.push_back(key + "=" + value); }
  void add(const std::string& key, u64 value) { add(key, std::to_string(value)); }
  bool write(const std::string& path) const {
    if (path.empty()) return true;
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) return false;
    for (const auto& line : lines) out << line << "\n";
    return static_cast<bool>(out);
  }
};

int run_coordinator(const Arguments& args) {
  if (args.state_directory.empty()) {
    std::fprintf(stderr, "coordinator: --state-dir is required\n");
    return 2;
  }
  SynthConfig config = config_from(args);
  SynthesizedPopulation population = synthesize(config);
  CoordinatorOptions options;
  options.address = args.address;
  options.port = args.port;
  options.governor.state_directory = args.state_directory;
  options.max_connections = 8;
  Coordinator coordinator;
  Status status = coordinator.start(options, population.policy);
  if (!status.ok()) {
    std::fprintf(stderr, "coordinator: %s\n", status.to_string().c_str());
    return 3;
  }
  std::printf("coordinator listening on %s:%u\n", args.address.c_str(),
              static_cast<unsigned>(coordinator.port()));
  std::fflush(stdout);
  status = coordinator.wait_until_stopped();
  Status stop_status = coordinator.stop();
  if (!status.ok() || !stop_status.ok()) {
    std::fprintf(stderr, "coordinator: shutdown failed\n");
    return 4;
  }
  return 0;
}

int run_worker(const Arguments& args) {
  Report report;
  report.add("role", "worker");
  report.add("publisher", args.publisher);
  report.add("incarnation", args.incarnation);
  report.add("domain", args.domain);

  SynthConfig config = config_from(args);
  SynthesizedPopulation population = synthesize(config);
  SynthesizedPopulation slice =
      slice_domain(population, OversubscriptionDomainId(args.domain));
  slice.policy = population.policy;

  WorkerOptions options;
  options.address = args.address;
  options.port = args.port;
  options.publisher = PublisherId(args.publisher);
  options.incarnation = IncarnationId(args.incarnation);
  options.domain = OversubscriptionDomainId(args.domain);
  options.adopt_incarnation = args.adopt;
  options.tick = args.tick;

  WorkerClient client;
  Status status = client.connect(options);
  if (!status.ok()) {
    report.add("connect", status.to_string());
    report.write(args.report_path);
    std::fprintf(stderr, "worker: connect failed: %s\n", status.to_string().c_str());
    return 3;
  }
  report.add("epoch", client.hello().epoch.value());
  report.add("policy_generation", client.hello().policy_generation.value());
  report.add("resumed_sequence", client.hello().last_sequence);
  if (args.stale_epoch) client.set_epoch_override(FabricEpoch(client.hello().epoch.value() + 1));

  int exit_code = 0;
  const auto record_ack = [&](const char* what, const PublishAck& ack) {
    report.add(std::string(what) + "_code", to_string(ack.code));
    report.add(std::string(what) + "_reason", to_string(ack.reason));
    report.add(std::string(what) + "_generation", ack.generation);
    if (ack.duplicate) report.add(std::string(what) + "_duplicate", u64{1});
    if (ack.code != StatusCode::Ok && ack.code != StatusCode::Duplicate) {
      std::fprintf(stderr, "worker: %s rejected: %s\n", what, ack.detail.c_str());
      exit_code = 5;
    }
  };

  for (u32 round = 0; round < args.rounds && exit_code == 0; ++round) {
    CapacitySnapshot capacity = slice.capacity;
    ReservationSnapshot reservations = slice.reservations;
    AdmissionSnapshot admissions = slice.admissions;
    const u64 tick = args.tick + round;
    capacity.observed_tick = tick;
    reservations.observed_tick = tick;
    admissions.observed_tick = tick;
    if (!args.auto_generations) {
      capacity.generation = CapacitySnapshotGeneration(client.hello().capacity_generation + round + 1);
      reservations.generation = ReservationSnapshotGeneration(client.hello().reservation_generation + round + 1);
      admissions.generation = AdmissionSnapshotGeneration(client.hello().admission_generation + round + 1);
    } else {
      capacity.generation = CapacitySnapshotGeneration(0);
      reservations.generation = ReservationSnapshotGeneration(0);
      admissions.generation = AdmissionSnapshotGeneration(0);
    }

    PublishAck ack;
    status = client.publish_capacity(capacity, ack);
    if (!status.ok()) {
      report.add("publish_error", status.to_string());
      exit_code = 6;
      break;
    }
    if (args.stale_epoch) {
      report.add("stale_capacity_code", to_string(ack.code));
      report.add("stale_capacity_reason", to_string(ack.reason));
      exit_code = 0;
      break;
    }
    record_ack("capacity", ack);
    if (exit_code != 0) break;

    status = client.publish_reservations(reservations, ack);
    if (!status.ok()) {
      report.add("publish_error", status.to_string());
      exit_code = 6;
      break;
    }
    record_ack("reservation", ack);
    if (exit_code != 0) break;

    status = client.publish_admissions(admissions, ack);
    if (!status.ok()) {
      report.add("publish_error", status.to_string());
      exit_code = 6;
      break;
    }
    record_ack("admission", ack);
    if (exit_code != 0) break;

    if (args.replay) {
      const u64 saved_sequence = client.sequence();
      const u64 saved_generation = capacity.generation.value();
      CapacitySnapshot replay = capacity;
      replay.generation = CapacitySnapshotGeneration(saved_generation);
      client.set_sequence(saved_sequence);
      PublishAck replay_ack;
      status = client.publish_capacity(replay, replay_ack);
      if (!status.ok()) {
        report.add("replay_error", status.to_string());
        exit_code = 7;
        break;
      }
      report.add("replay_code", to_string(replay_ack.code));
      report.add("replay_reason", to_string(replay_ack.reason));
      report.add("replay_duplicate", replay_ack.duplicate ? u64{1} : u64{0});
    }

    Decision decision;
    status = client.evaluate(OversubscriptionDomainId(args.domain), tick, decision);
    if (!status.ok()) {
      report.add("evaluate_error", status.to_string());
      exit_code = 8;
      break;
    }
    report.add("outcome", outcome_name(decision.evaluation.outcome));
    report.add("effective", outcome_name(decision.effective_outcome));
    report.add("authorized", decision.new_oversubscription_authorized ? u64{1} : u64{0});
    report.add("increment", decision.authorized_increment_units);
    report.add("binding", to_string(decision.evaluation.accounting.binding_constraint));
    report.add("ceiling", decision.evaluation.accounting.ceiling_total);
    report.add("committed", decision.evaluation.accounting.committed_total);
    report.add("contingent_free", decision.evaluation.accounting.contingent_free);
    report.add("decision_digest", decision.decision_digest.to_hex());
    report.add("result_digest", decision.evaluation.result_digest.to_hex());
    report.add("decision_id", decision.id.value());

    if (!args.expect_outcome.empty() && outcome_name(decision.evaluation.outcome) != args.expect_outcome) {
      std::fprintf(stderr, "worker: expected %s, observed %s\n", args.expect_outcome.c_str(),
                   outcome_name(decision.evaluation.outcome).c_str());
      exit_code = 9;
      break;
    }
    if (args.revalidate) {
      RevalidateAck revalidation;
      status = client.revalidate(decision, revalidation);
      if (!status.ok()) {
        report.add("revalidate_error", status.to_string());
        exit_code = 10;
        break;
      }
      report.add("revalidate_valid", revalidation.valid ? u64{1} : u64{0});
      report.add("revalidate_reason", to_string(revalidation.reason));
    }
  }

  if (exit_code == 0) {
    status = client.shutdown();
    if (!status.ok()) {
      report.add("shutdown_error", status.to_string());
      exit_code = 11;
    }
  }
  report.add("exit", static_cast<u64>(exit_code));
  if (!report.write(args.report_path)) {
    std::fprintf(stderr, "worker: cannot write report file\n");
    return 12;
  }
  return exit_code;
}

}  // namespace

int main(int argc, char** argv) {
  const Arguments args = parse(argc, argv);
  if (!args.valid) {
    std::fprintf(stderr, "oversub_dist: %s\n", args.error.c_str());
    std::fprintf(stderr,
                 "usage: oversub_dist coordinator --state-dir DIR [--port P] [--domains N] [--resources R]\n"
                 "       oversub_dist worker --port P --publisher N --incarnation N --domain D [options]\n");
    return 2;
  }
  if (args.role == "coordinator") return run_coordinator(args);
  if (args.role == "worker") return run_worker(args);
  std::fprintf(stderr, "oversub_dist: unknown role '%s'\n", args.role.c_str());
  return 2;
}
