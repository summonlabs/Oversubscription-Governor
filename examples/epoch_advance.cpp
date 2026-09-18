// Advancing the fabric epoch invalidates every binding: cached evidence is
// dropped, publishers must re-register, and domains stay fenced until an
// authoritative result is produced from fresh evidence.
#include "eg.hpp"

int main() {
  using namespace oversub;
  eg::ScratchDirectory dir("epoch-advance");
  SynthesizedPopulation population = synthesize(eg::base_config());
  Governor governor;
  if (int code = eg::report(eg::open_demo_governor(governor, dir.path(), population.policy), "open")) {
    return code;
  }
  if (int code = eg::report(eg::install(governor, population), "install")) return code;

  EvaluationRequest request;
  request.domain = OversubscriptionDomainId(1);
  Decision before;
  if (int code = eg::report(governor.evaluate(request, before), "evaluate")) return code;
  if (int code = eg::report(governor.advance_epoch(FabricEpoch(2)), "advance_epoch")) return code;

  Decision after;
  if (int code = eg::report(governor.evaluate(request, after), "evaluate")) return code;
  DomainRuntimeState state;
  if (int code = eg::report(governor.domain_state(OversubscriptionDomainId(1), state), "domain_state")) {
    return code;
  }
  std::printf("epoch=%llu outcome=%s fenced=%s\n",
              static_cast<unsigned long long>(governor.epoch().value()), to_string(after.evaluation.outcome),
              state.fenced ? "true" : "false");

  // Republishing under the new epoch re-establishes authority.
  SynthesizedPopulation fresh = population;
  fresh.capacity.epoch = FabricEpoch(2);
  fresh.reservations.epoch = FabricEpoch(2);
  fresh.admissions.epoch = FabricEpoch(2);
  fresh.capacity.generation = CapacitySnapshotGeneration(2);
  fresh.reservations.generation = ReservationSnapshotGeneration(2);
  fresh.admissions.generation = AdmissionSnapshotGeneration(2);
  fresh.capacity.provenance.sequence = 4;
  fresh.reservations.provenance.sequence = 5;
  fresh.admissions.provenance.sequence = 6;
  fresh.capacity.provenance.evidence_id = EvidenceId(4);
  fresh.reservations.provenance.evidence_id = EvidenceId(5);
  fresh.admissions.provenance.evidence_id = EvidenceId(6);
  fresh.capacity.provenance.epoch = FabricEpoch(2);
  fresh.reservations.provenance.epoch = FabricEpoch(2);
  fresh.admissions.provenance.epoch = FabricEpoch(2);
  fresh.capacity.provenance.payload_digest = capacity_snapshot_digest(fresh.capacity);
  fresh.reservations.provenance.payload_digest = reservation_snapshot_digest(fresh.reservations);
  fresh.admissions.provenance.payload_digest = admission_snapshot_digest(fresh.admissions);
  if (int code = eg::report(governor.adopt_incarnation(fresh.capacity.provenance.publisher,
                                                      fresh.capacity.provenance.incarnation, FabricEpoch(2), 0),
                            "adopt_incarnation")) {
    return code;
  }
  if (int code = eg::report(governor.ingest_capacity(fresh.capacity).status, "ingest_capacity")) return code;
  if (int code = eg::report(governor.ingest_reservations(fresh.reservations).status, "ingest_reservations")) {
    return code;
  }
  if (int code = eg::report(governor.ingest_admissions(fresh.admissions).status, "ingest_admissions")) {
    return code;
  }
  Decision recovered;
  if (int code = eg::report(governor.evaluate(request, recovered), "evaluate")) return code;
  std::printf("after revalidation: outcome=%s authorized=%s\n", to_string(recovered.evaluation.outcome),
              recovered.new_oversubscription_authorized ? "true" : "false");
  return recovered.new_oversubscription_authorized ? 0 : 1;
}
