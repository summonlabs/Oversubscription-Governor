// Shared test fixtures: temporary state directories, scenario builders, and a
// governor fixture. Everything here is deterministic.
#ifndef OVERSUB_TEST_UTIL_HPP
#define OVERSUB_TEST_UTIL_HPP

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "oversub/governor.hpp"
#include "oversub/synth.hpp"

namespace otest {

using namespace oversub;

// Unique temporary directory, removed on destruction.
class TempDir {
 public:
  explicit TempDir(const std::string& tag) {
    static unsigned long long counter = 0;
    const std::filesystem::path base = std::filesystem::temp_directory_path();
    for (int attempt = 0; attempt < 64; ++attempt) {
      const std::string name =
          "oversub-" + tag + "-" + std::to_string(++counter) + "-" + std::to_string(attempt);
      const std::filesystem::path candidate = base / name;
      std::error_code ec;
      if (std::filesystem::create_directories(candidate, ec) && !ec) {
        path_ = candidate.string();
        return;
      }
    }
    path_ = (base / ("oversub-" + tag + "-fallback")).string();
    std::error_code ec;
    std::filesystem::create_directories(path_, ec);
  }

  ~TempDir() {
    std::error_code ec;
    std::filesystem::remove_all(path_, ec);
  }

  TempDir(const TempDir&) = delete;
  TempDir& operator=(const TempDir&) = delete;

  const std::string& path() const { return path_; }
  std::string file(const std::string& name) const { return (std::filesystem::path(path_) / name).string(); }

 private:
  std::string path_;
};

inline std::string read_text(const std::string& path) {
  std::ifstream in(path, std::ios::binary);
  if (!in) return std::string();
  return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

inline void write_text(const std::string& path, const std::string& text) {
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  out << text;
}

inline bool exists(const std::string& path) { return std::filesystem::exists(path); }

inline void overwrite_bytes(const std::string& path, u64 offset, const std::vector<u8>& bytes) {
  std::fstream file(path, std::ios::binary | std::ios::in | std::ios::out);
  if (!file) return;
  file.seekp(static_cast<std::streamoff>(offset));
  file.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
}

inline u64 file_size(const std::string& path) {
  std::error_code ec;
  const auto size = std::filesystem::file_size(path, ec);
  return ec ? 0 : static_cast<u64>(size);
}

inline void truncate_file(const std::string& path, u64 bytes) {
  std::error_code ec;
  std::filesystem::resize_file(path, bytes, ec);
}

// A mutable scenario: policy plus three evidence snapshots that stay mutually
// consistent (guaranteed-class admissions never exceed their reservations).
struct Scenario {
  OversubscriptionPolicy policy;
  CapacitySnapshot capacity;
  ReservationSnapshot reservations;
  AdmissionSnapshot admissions;
  PublisherId publisher{1};
  IncarnationId incarnation{1};
  u64 sequence{0};

  static Scenario make(u32 resources = 2, u64 usable = 1000, Ratio ratio = Ratio{2, 1},
                       u64 guarantees_per_resource = 0) {
    Scenario scenario;
    scenario.policy.id = OversubscriptionPolicyId(1);
    scenario.policy.generation = PolicyGeneration(1);
    scenario.policy.name = "test";
    scenario.policy.policy_ratio_cap = Ratio{4, 1};
    scenario.policy.risk_budget.id = RiskBudgetId(1);
    scenario.policy.risk_budget.generation = RiskBudgetGeneration(1);
    scenario.policy.risk_budget.has_ppm = true;
    scenario.policy.risk_budget.max_exposure_ppm = 5000000;   // 5x usable: not binding by default
    scenario.policy.thresholds.warn_used_ppm = 900000;
    scenario.policy.thresholds.reduce_overage_ppm = 1;
    scenario.policy.thresholds.emergency_overage_ppm = 100000;
    scenario.policy.max_capacity_age_ticks = 10;
    scenario.policy.max_reservation_age_ticks = 10;
    scenario.policy.max_admission_age_ticks = 10;

    ServiceClassRef guaranteed;
    guaranteed.id = ServiceClassId(1);
    guaranteed.name = "protected";
    guaranteed.oversubscription_eligible = false;
    scenario.policy.service_classes.push_back(guaranteed);

    ServiceClassRef contingent;
    contingent.id = ServiceClassId(2);
    contingent.name = "contingent";
    contingent.oversubscription_eligible = true;
    scenario.policy.service_classes.push_back(contingent);

    DomainConfig domain;
    domain.id = OversubscriptionDomainId(1);
    domain.generation = DomainGeneration(1);
    domain.name = "d0";
    domain.pool = PoolId(1);
    domain.configured_ratio = ratio;
    for (u32 i = 0; i < resources; ++i) domain.members.push_back(ResourceId(i + 1));
    scenario.policy.domains.push_back(domain);

    scenario.capacity.domain = OversubscriptionDomainId(1);
    scenario.capacity.generation = CapacitySnapshotGeneration(1);
    scenario.capacity.epoch = FabricEpoch(1);
    for (u32 i = 0; i < resources; ++i) {
      ResourceCapacity resource;
      resource.resource = ResourceId(i + 1);
      resource.pool = PoolId(1);
      resource.physical_capacity = usable;
      resource.usable_capacity = usable;
      resource.health = ResourceHealth::Healthy;
      resource.degraded_capacity_ppm = kPpmScale;
      resource.generation = ResourceGeneration(1);
      scenario.capacity.resources.push_back(resource);
    }
    scenario.reservations.domain = OversubscriptionDomainId(1);
    scenario.reservations.generation = ReservationSnapshotGeneration(1);
    scenario.reservations.epoch = FabricEpoch(1);
    scenario.admissions.domain = OversubscriptionDomainId(1);
    scenario.admissions.generation = AdmissionSnapshotGeneration(1);
    scenario.admissions.epoch = FabricEpoch(1);

    for (u32 i = 0; i < resources && guarantees_per_resource > 0; ++i) {
      scenario.add_guarantee(ResourceId(i + 1), ServiceClassId(1), guarantees_per_resource);
    }
    scenario.stamp(0);
    return scenario;
  }

  DomainConfig& domain() { return policy.domains[0]; }
  const DomainConfig& domain() const { return policy.domains[0]; }

  Guarantee& add_guarantee(ResourceId resource, ServiceClassId cls, u64 units) {
    Guarantee guarantee;
    guarantee.reservation = ReservationId(reservations.guarantees.size() + 1);
    guarantee.resource = resource;
    guarantee.pool = PoolId(1);
    guarantee.service_class = cls;
    guarantee.guaranteed_capacity = units;
    guarantee.priority = PriorityRef{100};
    guarantee.protected_obligation = true;
    guarantee.preemptible = false;
    guarantee.generation = ReservationGeneration(1);
    reservations.guarantees.push_back(guarantee);
    return reservations.guarantees.back();
  }

  DemandRecord& add_demand(ResourceId resource, ServiceClassId cls, DemandKind kind, u64 committed,
                           u64 pending = 0) {
    DemandRecord record;
    record.admission = AdmissionId(admissions.records.size() + 1);
    record.resource = resource;
    record.pool = PoolId(1);
    record.service_class = cls;
    record.kind = kind;
    record.committed_capacity = committed;
    record.pending_capacity = pending;
    record.generation = AdmissionGeneration(1);
    admissions.records.push_back(record);
    return admissions.records.back();
  }

  // Guaranteed-class demand that exactly mirrors a reservation, keeping the
  // reservation and admission views consistent.
  void mirror_guarantees() {
    for (const auto& guarantee : reservations.guarantees) {
      add_demand(guarantee.resource, guarantee.service_class, DemandKind::Guaranteed,
                 guarantee.guaranteed_capacity);
    }
  }

  void stamp(u64 tick) {
    auto set = [&](Provenance& provenance, u64 id) {
      ++sequence;
      provenance.publisher = publisher;
      provenance.incarnation = incarnation;
      provenance.epoch = capacity.epoch;
      provenance.sequence = sequence;
      provenance.observed_tick = tick;
      provenance.observed_wall_millis = 0;
      provenance.evidence_id = EvidenceId(id);
    };
    capacity.observed_tick = tick;
    reservations.observed_tick = tick;
    admissions.observed_tick = tick;
    set(capacity.provenance, 100 + sequence);
    set(reservations.provenance, 200 + sequence);
    set(admissions.provenance, 300 + sequence);
    capacity.provenance.payload_digest = capacity_snapshot_digest(capacity);
    reservations.provenance.payload_digest = reservation_snapshot_digest(reservations);
    admissions.provenance.payload_digest = admission_snapshot_digest(admissions);
  }

  // Recomputes payload digests after the caller edited a snapshot in place.
  void refresh_digests() {
    capacity.provenance.payload_digest = capacity_snapshot_digest(capacity);
    reservations.provenance.payload_digest = reservation_snapshot_digest(reservations);
    admissions.provenance.payload_digest = admission_snapshot_digest(admissions);
  }

  Status validate() const {
    Status status = validate_policy(policy);
    if (!status.ok()) return status;
    status = validate_capacity_snapshot(capacity);
    if (!status.ok()) return status;
    status = validate_reservation_snapshot(reservations);
    if (!status.ok()) return status;
    return validate_admission_snapshot(admissions);
  }
};

// Governor fixture bound to a temporary durable directory.
class GovernorFixture {
 public:
  explicit GovernorFixture(const std::string& tag = "gov") : dir_("governor-" + tag) {}

  // Tests that are not about durability disable per-write fsync so the suite
  // measures governance rather than storage latency. Durable write behaviour is
  // covered by the persistence and distributed suites, which use the default
  // (synchronous) store configuration.
  Status open(const OversubscriptionPolicy& policy, u32 workers = 0, u64 save_every = 1) {
    GovernorOptions options;
    options.state_directory = dir_.path();
    options.worker_threads = workers;
    options.save_state_every_decisions = save_every == 0 ? 1 : save_every;
    options.sync_writes = false;
    return governor_.open(options, policy);
  }

  Status reopen(const OversubscriptionPolicy& policy = OversubscriptionPolicy{}, u32 workers = 0) {
    GovernorOptions options;
    options.state_directory = dir_.path();
    options.worker_threads = workers;
    options.save_state_every_decisions = 1;
    options.sync_writes = false;
    return governor_.open(options, policy);
  }

  Governor& governor() { return governor_; }
  const std::string& directory() const { return dir_.path(); }
  std::string file(const std::string& name) const { return dir_.file(name); }

 private:
  TempDir dir_;
  Governor governor_;
};

// Installs a scenario into a governor: policy, then the three evidence snapshots.
inline Status install_scenario(Governor& governor, const Scenario& scenario, u64 tick = 0) {
  Status status = governor.install_policy(scenario.policy, tick);
  if (!status.ok()) return status;
  // The registered publisher must be the one the evidence actually came from.
  status = governor.register_publisher(scenario.capacity.provenance.publisher,
                                       scenario.capacity.provenance.incarnation, scenario.capacity.epoch, tick);
  if (!status.ok()) return status;
  IngestOutcome outcome = governor.ingest_capacity(scenario.capacity);
  if (!outcome.status.ok()) return outcome.status;
  outcome = governor.ingest_reservations(scenario.reservations);
  if (!outcome.status.ok()) return outcome.status;
  outcome = governor.ingest_admissions(scenario.admissions);
  if (!outcome.status.ok()) return outcome.status;
  return Status::success();
}

// The authority a scenario's own generations represent.
inline AuthorityExpectation scenario_expectation(const Scenario& scenario) {
  const DomainConfig* domain = find_domain(scenario.policy, scenario.capacity.domain);
  AuthorityExpectation expectation;
  expectation.epoch = scenario.capacity.epoch;
  expectation.policy_generation = scenario.policy.generation;
  expectation.domain_generation = domain != nullptr ? domain->generation : DomainGeneration(0);
  expectation.capacity_generation = scenario.capacity.generation;
  expectation.reservation_generation = scenario.reservations.generation;
  expectation.admission_generation = scenario.admissions.generation;
  expectation.risk_budget_generation = scenario.policy.risk_budget.generation;
  return expectation;
}

// Evaluation through the pure engine with an explicit expectation.
inline EvaluationResult evaluate_scenario_with(const Scenario& scenario, const AuthorityExpectation& expectation,
                                               u64 tick) {
  PolicyIndex index(scenario.policy);
  const DomainConfig* domain = find_domain(scenario.policy, scenario.capacity.domain);
  DomainEvidence evidence;
  evidence.capacity = &scenario.capacity;
  evidence.reservations = &scenario.reservations;
  evidence.admissions = &scenario.admissions;
  if (domain == nullptr) {
    EvaluationResult result;
    result.outcome = Outcome::Unknown;
    result.reason = ReasonCode::DomainNotInPolicy;
    return result;
  }
  return evaluate_domain(scenario.policy, index, *domain, expectation, evidence, tick);
}

// Evaluation through the pure engine, using the scenario's own generations as
// the authoritative expectation.
inline EvaluationResult evaluate_scenario(const Scenario& scenario, u64 tick) {
  return evaluate_scenario_with(scenario, scenario_expectation(scenario), tick);
}

}  // namespace otest

#endif  // OVERSUB_TEST_UTIL_HPP
