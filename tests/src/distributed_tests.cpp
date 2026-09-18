// Real multiprocess tests: separate OS processes, real loopback TCP transport,
// real kills and restarts, durable coordinator state across process death.
#include <chrono>
#include <map>
#include <string>
#include <thread>
#include <vector>

#include "coordinator.hpp"
#include "framework.hpp"
#include "oversub/process.hpp"
#include "oversub/synth.hpp"
#include "proto_test_util.hpp"
#include "testutil.hpp"
#include "worker.hpp"

using namespace oversub;
using namespace oversub::dist;
using namespace otest;

namespace {

std::string dist_binary() { return std::string(OVERSUB_DIST_BINARY); }

std::map<std::string, std::string> parse_report(const std::string& path) {
  std::map<std::string, std::string> values;
  const std::string text = otest::read_text(path);
  std::size_t start = 0;
  while (start < text.size()) {
    std::size_t end = text.find('\n', start);
    if (end == std::string::npos) end = text.size();
    const std::string line = text.substr(start, end - start);
    const std::size_t split = line.find('=');
    if (split != std::string::npos) values[line.substr(0, split)] = line.substr(split + 1);
    start = end + 1;
  }
  return values;
}

WorkerOptions worker_options(u16 port, u64 publisher, u64 incarnation, u64 domain) {
  WorkerOptions options;
  options.address = "127.0.0.1";
  options.port = port;
  options.publisher = PublisherId(publisher);
  options.incarnation = IncarnationId(incarnation);
  options.domain = OversubscriptionDomainId(domain);
  return options;
}

}  // namespace

TEST(dist_protocol_decoders_reject_arbitrary_bytes) {
  u64 state = 0xDEADBEEFULL;
  for (int iteration = 0; iteration < 3000; ++iteration) {
    state = state * 6364136223846793005ULL + 1442695040888963407ULL;
    const std::size_t length = static_cast<std::size_t>(state % 256);
    std::vector<u8> buffer(length);
    for (std::size_t i = 0; i < length; ++i) {
      state = state * 6364136223846793005ULL + 1442695040888963407ULL;
      buffer[i] = static_cast<u8>(state >> 29);
    }
    HelloRequest hello;
    decode_hello(buffer, hello);
    HelloResponse response;
    decode_hello_response(buffer, response);
    Status status;
    decode_status(buffer, status);
    PublishAck ack;
    decode_publish_ack(buffer, ack);
    RevalidateAck revalidation;
    decode_revalidate_ack(buffer, revalidation);
    CapacitySnapshot capacity;
    decode_capacity_snapshot(buffer, capacity);
    ReservationSnapshot reservations;
    decode_reservation_snapshot(buffer, reservations);
    AdmissionSnapshot admissions;
    decode_admission_snapshot(buffer, admissions);
    Decision decision;
    decode_decision(buffer, decision);
    // A decoded decision must always be self-consistent enough to re-encode.
    if (decode_decision(buffer, decision)) {
      std::vector<u8> again;
      encode_decision(decision, again);
      Decision round_trip;
      CHECK(decode_decision(again, round_trip));
    }
  }
}

TEST(dist_in_process_publish_evaluate_and_revalidate) {
  NetworkRuntime runtime;
  REQUIRE(runtime.ok());
  SynthConfig config;
  config.domains = 1;
  config.resources_per_domain = 2;
  config.capacity_per_resource = 10000;
  SynthesizedPopulation population = synthesize(config);

  Coordinator coordinator;
  CoordinatorOptions options;
  options.governor.state_directory = otest::TempDir("coord-inproc").path();
  REQUIRE(coordinator.start(options, population.policy).ok());
  REQUIRE(coordinator.port() != 0);

  WorkerClient client;
  WorkerOptions worker = worker_options(coordinator.port(), 1, 1, 1);
  REQUIRE(client.connect(worker).ok());
  CHECK_EQ(client.hello().epoch.value(), u64{1});

  CapacitySnapshot capacity = population.capacity;
  ReservationSnapshot reservations = population.reservations;
  AdmissionSnapshot admissions = population.admissions;
  capacity.generation = CapacitySnapshotGeneration(client.hello().capacity_generation + 1);
  reservations.generation = ReservationSnapshotGeneration(client.hello().reservation_generation + 1);
  admissions.generation = AdmissionSnapshotGeneration(client.hello().admission_generation + 1);
  PublishAck ack;
  REQUIRE(client.publish_capacity(capacity, ack).ok());
  CHECK_EQ(ack.code, StatusCode::Ok);
  CHECK_EQ(ack.generation, u64{1});
  REQUIRE(client.publish_reservations(reservations, ack).ok());
  CHECK_EQ(ack.code, StatusCode::Ok);
  REQUIRE(client.publish_admissions(admissions, ack).ok());
  CHECK_EQ(ack.code, StatusCode::Ok);

  Decision decision;
  REQUIRE(client.evaluate(OversubscriptionDomainId(1), 0, decision).ok());
  CHECK(decision.evaluation.outcome == Outcome::WithinPolicy || decision.evaluation.outcome == Outcome::AtLimit);
  CHECK(decision.decision_digest == decision_digest_of(decision));
  RevalidateAck revalidation;
  REQUIRE(client.revalidate(decision, revalidation).ok());
  CHECK_EQ(revalidation.detail, std::string("decision is still bound to the current authority"));
  CHECK_EQ(revalidation.reason, ReasonCode::None);
  CHECK(revalidation.valid);
  REQUIRE(client.shutdown().ok());
  REQUIRE(coordinator.stop().ok());
  CHECK(coordinator.connections() >= 1);
}

TEST(dist_rejects_protocol_violations) {
  NetworkRuntime runtime;
  REQUIRE(runtime.ok());
  SynthConfig config;
  config.domains = 1;
  config.resources_per_domain = 1;
  SynthesizedPopulation population = synthesize(config);
  otest::TempDir dir("coord-violations");
  Coordinator coordinator;
  CoordinatorOptions options;
  options.governor.state_directory = dir.path();
  REQUIRE(coordinator.start(options, population.policy).ok());

  {
    // First frame is not HELLO.
    Socket socket;
    REQUIRE(tcp_connect("127.0.0.1", coordinator.port(), socket).ok());
    FramedConnection connection(std::move(socket));
    REQUIRE(connection.send_frame(static_cast<u32>(MessageType::Evaluate), {1, 2, 3}).ok());
    u32 type = 0;
    std::vector<u8> payload;
    REQUIRE(connection.recv_frame(type, payload).ok());
    CHECK_EQ(type, static_cast<u32>(MessageType::Reject));
    Status status;
    REQUIRE(decode_status(payload, status));
    CHECK_EQ(status.reason, ReasonCode::ProtocolViolation);
    connection.close();
  }
  {
    // Wrong protocol version.
    Socket socket;
    REQUIRE(tcp_connect("127.0.0.1", coordinator.port(), socket).ok());
    FramedConnection connection(std::move(socket));
    HelloRequest hello;
    hello.protocol_version = kProtocolVersion + 7;
    hello.publisher = PublisherId(1);
    hello.incarnation = IncarnationId(1);
    hello.domain = OversubscriptionDomainId(1);
    std::vector<u8> payload;
    encode_hello(hello, payload);
    REQUIRE(connection.send_frame(static_cast<u32>(MessageType::Hello), payload).ok());
    u32 type = 0;
    std::vector<u8> response;
    REQUIRE(connection.recv_frame(type, response).ok());
    CHECK_EQ(type, static_cast<u32>(MessageType::Reject));
    Status status;
    REQUIRE(decode_status(response, status));
    CHECK_EQ(status.reason, ReasonCode::PublisherProtocolMismatch);
    connection.close();
  }
  {
    // Malformed HELLO payload.
    Socket socket;
    REQUIRE(tcp_connect("127.0.0.1", coordinator.port(), socket).ok());
    FramedConnection connection(std::move(socket));
    REQUIRE(connection.send_frame(static_cast<u32>(MessageType::Hello), {9, 9}).ok());
    u32 type = 0;
    std::vector<u8> response;
    REQUIRE(connection.recv_frame(type, response).ok());
    CHECK_EQ(type, static_cast<u32>(MessageType::Reject));
    connection.close();
  }
  REQUIRE(coordinator.stop().ok());
}

TEST(dist_real_worker_process_publishes_and_evaluates) {
  NetworkRuntime runtime;
  REQUIRE(runtime.ok());
  SynthConfig config;
  config.domains = 2;
  config.resources_per_domain = 2;
  config.guarantees_per_resource = 2;
  config.demand_records_per_resource = 4;
  config.capacity_per_resource = 10000;
  SynthesizedPopulation population = synthesize(config);
  otest::TempDir dir("coord-process");
  otest::TempDir report_dir("report");
  Coordinator coordinator;
  CoordinatorOptions options;
  options.governor.state_directory = dir.path();
  REQUIRE(coordinator.start(options, population.policy).ok());

  const std::string report_path = report_dir.file("worker.txt");
  ChildProcess worker;
  ProcessSpec spec;
  spec.executable = dist_binary();
  spec.arguments = {"worker",
                    "--port",
                    std::to_string(coordinator.port()),
                    "--publisher",
                    "3",
                    "--incarnation",
                    "5",
                    "--domain",
                    "2",
                    "--domains",
                    "2",
                    "--resources",
                    "2",
                    "--guarantees",
                    "2",
                    "--demand",
                    "4",
                    "--capacity",
                    "10000",
                    "--revalidate",
                    "--replay",
                    "--report",
                    report_path};
  REQUIRE(worker.spawn(spec).ok());
  u32 exit_code = 1;
  REQUIRE(worker.wait(exit_code).ok());
  if (exit_code != 0) std::fprintf(stderr, "worker report:\n%s\n", otest::read_text(report_path).c_str());
  CHECK_EQ(exit_code, u32{0});
  const auto report = parse_report(report_path);
  REQUIRE(!report.empty());
  CHECK_EQ(report.at("role"), std::string("worker"));
  CHECK_EQ(report.at("capacity_code"), std::string("Ok"));
  CHECK_EQ(report.at("reservation_code"), std::string("Ok"));
  CHECK_EQ(report.at("admission_code"), std::string("Ok"));
  CHECK_EQ(report.at("capacity_generation"), std::string("1"));
  REQUIRE(report.count("outcome") == 1);
  CHECK(report.at("outcome") == "WITHIN_POLICY" || report.at("outcome") == "AT_LIMIT");
  CHECK_EQ(report.at("revalidate_valid"), std::string("1"));
  CHECK_EQ(report.at("replay_code"), std::string("Ok"));
  CHECK_EQ(report.at("replay_duplicate"), std::string("1"));

  // The coordinator's durable view agrees with what the worker reported.
  u64 capacity_generation = 0;
  u64 reservation_generation = 0;
  u64 admission_generation = 0;
  REQUIRE(coordinator.governor()
              .evidence_generations(OversubscriptionDomainId(2), capacity_generation, reservation_generation,
                                    admission_generation)
              .ok());
  CHECK_EQ(capacity_generation, u64{1});
  CHECK(coordinator.governor().stats().decisions >= 1);
  REQUIRE(coordinator.stop().ok());
}

TEST(dist_stale_epoch_is_rejected_over_the_wire) {
  NetworkRuntime runtime;
  REQUIRE(runtime.ok());
  SynthConfig config;
  config.domains = 1;
  config.resources_per_domain = 1;
  SynthesizedPopulation population = synthesize(config);
  otest::TempDir dir("coord-epoch");
  otest::TempDir report_dir("report-epoch");
  Coordinator coordinator;
  CoordinatorOptions options;
  options.governor.state_directory = dir.path();
  REQUIRE(coordinator.start(options, population.policy).ok());
  const std::string report_path = report_dir.file("worker.txt");
  ChildProcess worker;
  ProcessSpec spec;
  spec.executable = dist_binary();
  spec.arguments = {"worker",     "--port",    std::to_string(coordinator.port()), "--publisher", "1",
                    "--incarnation", "1",       "--domain", "1",                      "--domains",   "1",
                    "--resources", "1",         "--guarantees", "1",                  "--demand",    "2",
                    "--capacity",  "1000000",   "--stale-epoch", "--report",           report_path};
  REQUIRE(worker.spawn(spec).ok());
  u32 exit_code = 1;
  REQUIRE(worker.wait(exit_code).ok());
  if (exit_code != 0) std::fprintf(stderr, "worker report:\n%s\n", otest::read_text(report_path).c_str());
  CHECK_EQ(exit_code, u32{0});
  const auto report = parse_report(report_path);
  CHECK_EQ(report.at("stale_capacity_code"), std::string("Stale"));
  CHECK_EQ(report.at("stale_capacity_reason"), std::string("EpochMismatch"));
  REQUIRE(coordinator.stop().ok());
}

TEST(dist_worker_kill_and_incarnation_fencing) {
  NetworkRuntime runtime;
  REQUIRE(runtime.ok());
  SynthConfig config;
  config.domains = 1;
  config.resources_per_domain = 1;
  SynthesizedPopulation population = synthesize(config);
  otest::TempDir dir("coord-kill");
  Coordinator coordinator;
  CoordinatorOptions options;
  options.governor.state_directory = dir.path();
  REQUIRE(coordinator.start(options, population.policy).ok());

  // A worker publishes once with incarnation 1 and is then killed abruptly.
  {
    WorkerClient client;
    REQUIRE(client.connect(worker_options(coordinator.port(), 1, 1, 1)).ok());
    CapacitySnapshot capacity = population.capacity;
    capacity.generation = CapacitySnapshotGeneration(1);
    PublishAck ack;
    REQUIRE(client.publish_capacity(capacity, ack).ok());
    CHECK_EQ(ack.code, StatusCode::Ok);
    client.close();
  }
  // The publisher's next incarnation cannot silently take over ...
  {
    WorkerClient client;
    const Status status = client.connect(worker_options(coordinator.port(), 1, 2, 1));
    CHECK(!status.ok());
    CHECK_EQ(status.reason, ReasonCode::PublisherIncarnationStale);
  }
  // ... unless the operator adopts it explicitly.
  {
    WorkerClient client;
    WorkerOptions adopted = worker_options(coordinator.port(), 1, 2, 1);
    adopted.adopt_incarnation = true;
    REQUIRE(client.connect(adopted).ok());
    CapacitySnapshot capacity = population.capacity;
    capacity.generation = CapacitySnapshotGeneration(2);
    PublishAck ack;
    REQUIRE(client.publish_capacity(capacity, ack).ok());
    CHECK_EQ(ack.code, StatusCode::Ok);
    REQUIRE(client.shutdown().ok());
  }
  REQUIRE(coordinator.wait_until_stopped().ok());
  REQUIRE(coordinator.stop().ok());
}

TEST(dist_coordinator_restart_preserves_durable_authority_and_invalidates_live_evidence) {
  NetworkRuntime runtime;
  REQUIRE(runtime.ok());
  SynthConfig config;
  config.domains = 1;
  config.resources_per_domain = 2;
  config.capacity_per_resource = 10000;
  SynthesizedPopulation population = synthesize(config);
  otest::TempDir dir("coord-restart");
  otest::TempDir report_dir("report-restart");
  const std::string report_path = report_dir.file("worker.txt");

  // Start the coordinator as a real child process and publish through it.
  const u16 port = 45731;
  ChildProcess first;
  ProcessSpec spec;
  spec.executable = dist_binary();
  spec.arguments = {"coordinator", "--state-dir", dir.path(), "--port", std::to_string(port),
                    "--domains", "1", "--resources", "2", "--guarantees", "2", "--demand", "3",
                    "--capacity", "10000"};
  REQUIRE(first.spawn(spec).ok());
  std::this_thread::sleep_for(std::chrono::milliseconds(400));

  Decision before_restart;
  {
    WorkerClient client;
    Status connected = client.connect(worker_options(port, 1, 1, 1));
    for (int attempt = 0; attempt < 50 && !connected.ok(); ++attempt) {
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
      connected = client.connect(worker_options(port, 1, 1, 1));
    }
    REQUIRE(connected.ok());
    CapacitySnapshot capacity = population.capacity;
    ReservationSnapshot reservations = population.reservations;
    AdmissionSnapshot admissions = population.admissions;
    capacity.generation = CapacitySnapshotGeneration(client.hello().capacity_generation + 1);
    reservations.generation = ReservationSnapshotGeneration(client.hello().reservation_generation + 1);
    admissions.generation = AdmissionSnapshotGeneration(client.hello().admission_generation + 1);
    PublishAck ack;
    REQUIRE(client.publish_capacity(capacity, ack).ok());
    CHECK_EQ(ack.detail, std::string());
    CHECK_EQ(ack.code, StatusCode::Ok);
    REQUIRE(client.publish_reservations(reservations, ack).ok());
    CHECK_EQ(ack.code, StatusCode::Ok);
    REQUIRE(client.publish_admissions(admissions, ack).ok());
    CHECK_EQ(ack.code, StatusCode::Ok);
    REQUIRE(client.evaluate(OversubscriptionDomainId(1), 0, before_restart).ok());
    const DomainAccounting& accounting = before_restart.evaluation.accounting;
    const std::string summary = "usable=" + std::to_string(accounting.effective_usable_capacity) +
                                " guarantees=" + std::to_string(accounting.guarantees_total) +
                                " reserve=" + std::to_string(accounting.emergency_reserve) +
                                " protected=" + std::to_string(accounting.protected_required) +
                                " ceiling=" + std::to_string(accounting.ceiling_total) +
                                " contingent=" + std::to_string(accounting.contingent_committed) +
                                " outcome=" + to_string(before_restart.evaluation.outcome) + " detail=" +
                                before_restart.evaluation.detail;
    CHECK_EQ(before_restart.evaluation.detail, std::string("the domain is within its oversubscription policy"));
    CHECK(before_restart.new_oversubscription_authorized);
    RevalidateAck revalidation;
    REQUIRE(client.revalidate(before_restart, revalidation).ok());
    CHECK_EQ(revalidation.detail, std::string("decision is still bound to the current authority"));
    CHECK(revalidation.valid);
    client.close();
  }

  // Kill the coordinator process without a clean shutdown.
  REQUIRE(first.terminate().ok());
  u32 exit_code = 0;
  REQUIRE(first.wait(exit_code).ok());

  ChildProcess second;
  REQUIRE(second.spawn(spec).ok());
  std::this_thread::sleep_for(std::chrono::milliseconds(400));
  {
    WorkerClient client;
    Status connected = client.connect(worker_options(port, 1, 1, 1));
    for (int attempt = 0; attempt < 50 && !connected.ok(); ++attempt) {
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
      connected = client.connect(worker_options(port, 1, 1, 1));
    }
    REQUIRE(connected.ok());
    // Durable generations survive the restart; live evidence does not.
    CHECK_EQ(client.hello().capacity_generation, u64{1});
    CHECK_EQ(client.hello().epoch.value(), u64{1});
    // The previous decision is not restored as live authority.
    RevalidateAck revalidation;
    REQUIRE(client.revalidate(before_restart, revalidation).ok());
    CHECK(!revalidation.valid);
    CHECK(revalidation.reason == ReasonCode::DecisionStale ||
          revalidation.reason == ReasonCode::DecisionEpochMismatch ||
          revalidation.reason == ReasonCode::DecisionSuperseded);
    // Fresh evidence re-establishes authority deterministically.
    CapacitySnapshot capacity = population.capacity;
    ReservationSnapshot reservations = population.reservations;
    AdmissionSnapshot admissions = population.admissions;
    const u64 next = client.hello().capacity_generation + 1;
    capacity.generation = CapacitySnapshotGeneration(next);
    reservations.generation = ReservationSnapshotGeneration(next);
    admissions.generation = AdmissionSnapshotGeneration(next);
    PublishAck ack;
    REQUIRE(client.publish_capacity(capacity, ack).ok());
    CHECK_EQ(ack.code, StatusCode::Ok);
    CHECK_EQ(ack.generation, next);
    REQUIRE(client.publish_reservations(reservations, ack).ok());
    REQUIRE(client.publish_admissions(admissions, ack).ok());
    Decision after_restart;
    REQUIRE(client.evaluate(OversubscriptionDomainId(1), 1, after_restart).ok());
    CHECK(after_restart.new_oversubscription_authorized);
    CHECK_EQ(after_restart.incarnation.value() >= 2, true);
    REQUIRE(client.shutdown().ok());
  }
  REQUIRE(second.wait(exit_code).ok());
  CHECK_EQ(exit_code, u32{0});
  (void)report_path;
}
