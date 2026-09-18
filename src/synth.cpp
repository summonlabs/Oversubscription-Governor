#include "oversub/synth.hpp"

#include <algorithm>

namespace oversub {
namespace {

u64 mix(u64 value) {
  value += 0x9E3779B97F4A7C15ULL;
  value = (value ^ (value >> 30)) * 0xBF58476D1CE4E5B9ULL;
  value = (value ^ (value >> 27)) * 0x94D049BB133111EBULL;
  return value ^ (value >> 31);
}

u64 spread(u64 seed, u64 index, u64 low, u64 high) {
  if (high <= low) return low;
  const u64 span = high - low + 1;
  return low + (mix(seed ^ (index * 0x2545F4914F6CDD1DULL)) % span);
}

ServiceClassId guaranteed_class(u32 index) { return ServiceClassId(1 + index); }
ServiceClassId contingent_class(u32 index, u32 distinct) { return ServiceClassId(1 + distinct + index); }

}  // namespace

SynthesizedPopulation synthesize(const SynthConfig& config) {
  SynthesizedPopulation population;
  OversubscriptionPolicy& policy = population.policy;
  policy.id = config.policy_id;
  policy.generation = PolicyGeneration(1);
  policy.name = "synthetic";
  policy.policy_ratio_cap = Ratio{4, 1};
  policy.risk_budget.id = RiskBudgetId(1);
  policy.risk_budget.generation = RiskBudgetGeneration(1);
  policy.risk_budget.has_ppm = true;
  policy.risk_budget.max_exposure_ppm = config.risk_exposure_ppm;
  policy.thresholds.warn_used_ppm = 900000;
  policy.thresholds.reduce_overage_ppm = 1;
  policy.thresholds.emergency_overage_ppm = 100000;
  policy.hysteresis.escalation_confirmations = 1;
  policy.hysteresis.deescalation_confirmations = 1;
  policy.hysteresis.cooldown_ticks = 0;
  policy.hysteresis.deescalation_margin_ppm = 0;
  policy.limits.max_domains = std::max<u32>(config.domains + 8, 16);
  policy.limits.max_resources_per_domain = std::max<u32>(config.resources_per_domain + 8, 16);
  policy.limits.max_service_classes = std::max<u32>(config.distinct_service_classes * 2 + 4, 8);
  policy.limits.max_guarantees_per_snapshot =
      std::max<u64>(static_cast<u64>(config.resources_per_domain) * config.guarantees_per_resource * 4 + 16, 64);
  policy.limits.max_demand_records_per_snapshot =
      std::max<u64>(static_cast<u64>(config.resources_per_domain) * config.demand_records_per_resource * 4 + 16,
                    64);
  policy.limits.max_corrective_intents = 8;
  policy.limits.max_explanation_lines = 64;
  policy.limits.max_explanation_bytes = 8192;
  policy.limits.max_resource_breakdown = 32;
  policy.limits.max_capacity_units = kHardMaxCapacityUnits;
  policy.max_capacity_age_ticks = 1000;
  policy.max_reservation_age_ticks = 1000;
  policy.max_admission_age_ticks = 1000;

  const u32 distinct = std::max<u32>(config.distinct_service_classes, 1);
  for (u32 i = 0; i < distinct; ++i) {
    ServiceClassRef cls;
    cls.id = guaranteed_class(i);
    cls.name = "guaranteed-" + std::to_string(i);
    cls.priority = PriorityRef{static_cast<u32>(100 + i)};
    cls.oversubscription_eligible = false;   // protected classes never consume oversubscription
    cls.risk_weight_ppm = static_cast<u32>(kPpmScale);
    policy.service_classes.push_back(cls);
  }
  for (u32 i = 0; i < distinct; ++i) {
    ServiceClassRef cls;
    cls.id = contingent_class(i, distinct);
    cls.name = "contingent-" + std::to_string(i);
    cls.priority = PriorityRef{static_cast<u32>(500 + i)};
    cls.oversubscription_eligible = true;
    cls.risk_weight_ppm = static_cast<u32>(kPpmScale / 2);
    policy.service_classes.push_back(cls);
  }

  std::vector<u64> guarantee_units;
  std::vector<u64> contingent_units;
  guarantee_units.reserve(config.domains * config.resources_per_domain * config.guarantees_per_resource);
  contingent_units.reserve(config.domains * config.resources_per_domain);

  for (u32 d = 0; d < config.domains; ++d) {
    DomainConfig domain;
    domain.id = OversubscriptionDomainId(config.first_domain_id.value() + d);
    domain.generation = DomainGeneration(1);
    domain.name = "domain-" + std::to_string(d);
    domain.pool = PoolId(domain.id.value());
    domain.configured_ratio = config.configured_ratio;
    domain.has_degraded_ratio_cap = config.has_degraded_ratio_cap;
    domain.degraded_ratio_cap = config.degraded_ratio_cap;
    u64 usable_total = 0;
    if (!mul_checked(config.capacity_per_resource, config.resources_per_domain, usable_total)) {
      usable_total = kHardMaxCapacityUnits;
    }
    domain.emergency_reserve_units = mul_div_floor_sat(usable_total, config.emergency_reserve_ppm, kPpmScale);
    domain.min_protected_headroom_units =
        config.min_protected_headroom_ppm == 0
            ? 0
            : mul_div_floor_sat(usable_total, config.min_protected_headroom_ppm, kPpmScale);
    domain.burst_pool_enabled = config.burst_pool_enabled;
    domain.burst_pool_cap_units = config.burst_pool_enabled ? config.burst_pool_units : 0;
    for (u32 r = 0; r < config.resources_per_domain; ++r) {
      domain.members.push_back(synth_resource_id(domain.id, r));
    }
    policy.domains.push_back(std::move(domain));
  }

  for (u32 d = 0; d < config.domains; ++d) {
    const OversubscriptionDomainId domain_id(config.first_domain_id.value() + d);
    for (u32 r = 0; r < config.resources_per_domain; ++r) {
      const ResourceId resource = synth_resource_id(domain_id, r);
      const u64 usable = config.capacity_per_resource;
      const u64 guarantee_budget = mul_div_floor_sat(usable, config.guarantee_ppm_of_usable, kPpmScale);
      u64 allocated = 0;
      for (u32 g = 0; g < config.guarantees_per_resource; ++g) {
        const u64 index = (static_cast<u64>(d) * config.resources_per_domain + r) * config.guarantees_per_resource + g;
        Guarantee guarantee;
        guarantee.reservation = ReservationId(index + 1);
        guarantee.resource = resource;
        guarantee.pool = PoolId(domain_id.value());
        guarantee.service_class = guaranteed_class(static_cast<u32>(g % distinct));
        u64 share = guarantee_budget / config.guarantees_per_resource;
        if (g + 1 == config.guarantees_per_resource) share = guarantee_budget > allocated ? guarantee_budget - allocated : 0;
        guarantee.guaranteed_capacity = spread(config.seed, index, share / 2, share);
        guarantee.priority = PriorityRef{100};
        guarantee.protected_obligation = true;
        guarantee.preemptible = false;
        guarantee.generation = ReservationGeneration(1 + index);
        allocated += guarantee.guaranteed_capacity;
        population.reservations.guarantees.push_back(guarantee);
        guarantee_units.push_back(guarantee.guaranteed_capacity);
      }
      u64 guarantee_total = 0;
      for (const auto& guarantee : population.reservations.guarantees) {
        if (guarantee.resource == resource) guarantee_total += guarantee.guaranteed_capacity;
      }

      // Guaranteed-class admissions mirror the reservations exactly, so the
      // guaranteed and contingent views stay consistent.
      u64 demand_index = 0;
      for (const auto& guarantee : population.reservations.guarantees) {
        if (!(guarantee.resource == resource)) continue;
        DemandRecord record;
        record.admission = AdmissionId((static_cast<u64>(d) * config.resources_per_domain + r) * 1000 + demand_index + 1);
        record.resource = resource;
        record.pool = PoolId(domain_id.value());
        record.service_class = guarantee.service_class;
        record.kind = DemandKind::Guaranteed;
        record.committed_capacity = guarantee.guaranteed_capacity;
        record.pending_capacity = 0;
        record.generation = AdmissionGeneration(1 + demand_index);
        population.admissions.records.push_back(record);
        ++demand_index;
      }

      const u64 ratio_num = config.configured_ratio.num;
      const u64 ratio_den = config.configured_ratio.den;
      const u64 ceiling = mul_div_floor_sat(usable, ratio_num, ratio_den);
      u64 contingent_budget = 0;
      if (ceiling > guarantee_total) {
        const u64 headroom = ceiling - guarantee_total;
        contingent_budget = mul_div_floor_sat(headroom, config.contingent_ppm_of_ceiling, kPpmScale);
      }
      u32 contingent_records = config.demand_records_per_resource > config.guarantees_per_resource
                                   ? config.demand_records_per_resource - config.guarantees_per_resource
                                   : 1;
      if (contingent_records == 0) contingent_records = 1;
      u64 contingent_allocated = 0;
      for (u32 c = 0; c < contingent_records; ++c) {
        const u64 index = (static_cast<u64>(d) * config.resources_per_domain + r) * 97 + c;
        DemandRecord record;
        record.admission = AdmissionId((static_cast<u64>(d) * config.resources_per_domain + r) * 1000 + 500 + c + 1);
        record.resource = resource;
        record.pool = PoolId(domain_id.value());
        record.service_class = contingent_class(static_cast<u32>(c % distinct), distinct);
        record.kind = DemandKind::Contingent;
        const u64 share = contingent_budget / contingent_records;
        record.committed_capacity = spread(config.seed + 7, index, share / 2, share);
        record.pending_capacity = spread(config.seed + 11, index, 0, share / 2);
        record.generation = AdmissionGeneration(1 + index % 1000);
        contingent_allocated += record.committed_capacity;
        population.admissions.records.push_back(record);
      }
      contingent_units.push_back(contingent_allocated);

      ResourceCapacity capacity;
      capacity.resource = resource;
      capacity.pool = PoolId(domain_id.value());
      capacity.physical_capacity = usable;
      capacity.usable_capacity = usable;
      capacity.health = ResourceHealth::Healthy;
      capacity.degraded_capacity_ppm = kPpmScale;
      capacity.generation = ResourceGeneration(1);
      population.capacity.resources.push_back(capacity);
    }
  }

  // Health scenario: fail and degrade from the tail of each domain's members.
  {
    u32 remaining_degraded = config.degraded_resources;
    u32 remaining_failed = config.failed_resources;
    for (auto& resource : population.capacity.resources) {
      if (remaining_failed > 0) {
        resource.health = ResourceHealth::Failed;
        --remaining_failed;
      } else if (remaining_degraded > 0) {
        resource.health = ResourceHealth::Degraded;
        resource.degraded_capacity_ppm = kPpmScale / 2;
        --remaining_degraded;
      }
    }
  }

  population.capacity.domain = OversubscriptionDomainId(config.first_domain_id.value());
  population.capacity.generation = CapacitySnapshotGeneration(1);
  population.capacity.epoch = config.epoch;
  population.capacity.observed_tick = 0;
  population.reservations.domain = population.capacity.domain;
  population.reservations.generation = ReservationSnapshotGeneration(1);
  population.reservations.epoch = config.epoch;
  population.reservations.observed_tick = 0;
  population.admissions.domain = population.capacity.domain;
  population.admissions.generation = AdmissionSnapshotGeneration(1);
  population.admissions.epoch = config.epoch;
  population.admissions.observed_tick = 0;

  auto stamp = [&](Provenance& provenance, EvidenceId evidence_id, u64 sequence) {
    provenance.publisher = config.publisher;
    provenance.incarnation = config.incarnation;
    provenance.epoch = config.epoch;
    provenance.sequence = sequence;
    provenance.observed_tick = 0;
    provenance.observed_wall_millis = 0;
    provenance.evidence_id = evidence_id;
  };
  stamp(population.capacity.provenance, EvidenceId(1), 1);
  stamp(population.reservations.provenance, EvidenceId(2), 2);
  stamp(population.admissions.provenance, EvidenceId(3), 3);
  population.capacity.provenance.payload_digest = capacity_snapshot_digest(population.capacity);
  population.reservations.provenance.payload_digest = reservation_snapshot_digest(population.reservations);
  population.admissions.provenance.payload_digest = admission_snapshot_digest(population.admissions);

  compute_policy_digest(policy);
  return population;
}

SynthesizedPopulation slice_domain(const SynthesizedPopulation& population, OversubscriptionDomainId domain) {
  SynthesizedPopulation slice;
  slice.policy = population.policy;
  slice.capacity.domain = domain;
  slice.reservations.domain = domain;
  slice.admissions.domain = domain;
  slice.capacity.generation = population.capacity.generation;
  slice.reservations.generation = population.reservations.generation;
  slice.admissions.generation = population.admissions.generation;
  slice.capacity.epoch = population.capacity.epoch;
  slice.reservations.epoch = population.reservations.epoch;
  slice.admissions.epoch = population.admissions.epoch;
  slice.capacity.observed_tick = population.capacity.observed_tick;
  slice.reservations.observed_tick = population.reservations.observed_tick;
  slice.admissions.observed_tick = population.admissions.observed_tick;
  slice.capacity.provenance = population.capacity.provenance;
  slice.reservations.provenance = population.reservations.provenance;
  slice.admissions.provenance = population.admissions.provenance;

  const DomainConfig* config = find_domain(population.policy, domain);
  if (config == nullptr) return slice;
  auto is_member = [&](ResourceId resource) {
    return std::find(config->members.begin(), config->members.end(), resource) != config->members.end();
  };
  for (const auto& resource : population.capacity.resources) {
    if (is_member(resource.resource)) slice.capacity.resources.push_back(resource);
  }
  for (const auto& guarantee : population.reservations.guarantees) {
    if (is_member(guarantee.resource)) slice.reservations.guarantees.push_back(guarantee);
  }
  for (const auto& record : population.admissions.records) {
    if (is_member(record.resource)) slice.admissions.records.push_back(record);
  }
  slice.capacity.provenance.payload_digest = capacity_snapshot_digest(slice.capacity);
  slice.reservations.provenance.payload_digest = reservation_snapshot_digest(slice.reservations);
  slice.admissions.provenance.payload_digest = admission_snapshot_digest(slice.admissions);
  return slice;
}

}  // namespace oversub
