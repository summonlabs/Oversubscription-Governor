// Shared helpers for the runnable examples.
#ifndef OVERSUB_EXAMPLES_EG_HPP
#define OVERSUB_EXAMPLES_EG_HPP

#include <cstdio>
#include <filesystem>
#include <string>
#include <system_error>

#include "oversub/adapters.hpp"
#include "oversub/governor.hpp"
#include "oversub/synth.hpp"

namespace eg {

using namespace oversub;

inline SynthConfig base_config() {
  SynthConfig config;
  config.domains = 1;
  config.resources_per_domain = 2;
  config.guarantees_per_resource = 2;
  config.demand_records_per_resource = 4;
  config.capacity_per_resource = 100000;
  config.guarantee_ppm_of_usable = 300000;
  config.contingent_ppm_of_ceiling = 400000;
  config.configured_ratio = Ratio{2, 1};
  return config;
}

// Self-cleaning scratch directory for durable state.
class ScratchDirectory {
 public:
  explicit ScratchDirectory(const std::string& tag) {
    std::error_code ec;
    path_ = (std::filesystem::temp_directory_path(ec) / ("oversub-example-" + tag)).string();
    std::filesystem::remove_all(path_, ec);
    std::filesystem::create_directories(path_, ec);
  }
  ~ScratchDirectory() {
    std::error_code ec;
    std::filesystem::remove_all(path_, ec);
  }
  ScratchDirectory(const ScratchDirectory&) = delete;
  ScratchDirectory& operator=(const ScratchDirectory&) = delete;
  const std::string& path() const { return path_; }

 private:
  std::string path_;
};

inline Status open_demo_governor(Governor& governor, const std::string& directory,
                                 const OversubscriptionPolicy& policy) {
  GovernorOptions options;
  options.state_directory = directory;
  options.sync_writes = false;
  return governor.open(options, policy);
}

inline Status install(Governor& governor, const SynthesizedPopulation& population, u64 tick = 0) {
  Status status = governor.install_policy(population.policy, tick);
  if (!status.ok()) return status;
  status = governor.register_publisher(population.capacity.provenance.publisher,
                                       population.capacity.provenance.incarnation,
                                       population.capacity.epoch, tick);
  if (!status.ok()) return status;
  IngestOutcome outcome = governor.ingest_capacity(population.capacity);
  if (!outcome.status.ok()) return outcome.status;
  outcome = governor.ingest_reservations(population.reservations);
  if (!outcome.status.ok()) return outcome.status;
  outcome = governor.ingest_admissions(population.admissions);
  if (!outcome.status.ok()) return outcome.status;
  return Status::success();
}

inline void print_decision(const Governor& governor, const Decision& decision) {
  ExplainOptions options;
  options.max_lines = 40;
  options.max_bytes = 4096;
  std::string text;
  if (governor.explain(decision, options, text).ok()) std::fputs(text.c_str(), stdout);
}

inline int report(const Status& status, const char* what) {
  if (status.ok()) return 0;
  std::fprintf(stderr, "%s failed: %s\n", what, status.to_string().c_str());
  return 1;
}

}  // namespace eg

#endif  // OVERSUB_EXAMPLES_EG_HPP
