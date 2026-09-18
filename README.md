# Oversubscription Governor

Oversubscription Governor is an open-source, vendor-neutral C++20 runtime for
generation-bound governance of deliberate network oversubscription across
contention domains, protected guarantees, contingent demand, risk budgets, and
headroom.

Its core systems question is:

> Given physical and usable capacity, committed guarantees, admitted demand,
> reservation state, service classes, contention domains, headroom, risk policy,
> and the current generations, how much deliberate oversubscription is legal
> right now, where may it occur, which obligations remain protected, and when
> must oversubscription be reduced, fenced, revalidated, or rejected?

The thesis is:

> Oversubscription is a policy decision, not an accident to be discovered later.

A domain may be configured for a 2:1 oversubscription ratio while every
guarantee it carries is already unprotected because a member uplink degraded. A
contingent admission budget may be legal at one capacity generation and illegal
at the next. A fabric epoch may advance while publishers keep sending evidence
from the previous one. Telemetry may simply go quiet, and quiet telemetry is not
permission to keep honouring a ratio. Oversubscription Governor turns those
inputs into one deterministic, explainable answer about the legal boundary, binds
that answer to the exact generations and epoch that justified it, and produces
bounded corrective intent when the boundary has been crossed.

## Systems boundary

Oversubscription Governor owns:

- contention-domain representation and membership;
- capacity, reservation, and admission evidence ingestion with provenance;
- configured and effective oversubscription ratio authority;
- protected-guarantee accounting and contingent-versus-guaranteed separation;
- emergency reserve and minimum protected headroom obligations;
- risk budget accounting and exposure limits;
- service-class eligibility and per-class contingent share limits;
- health-driven boundary changes (degraded and failed members);
- freshness and staleness authority over evidence;
- binding-constraint identification and deterministic explanations;
- policy, generation, epoch, and publisher authority with fencing;
- corrective intent: reduce contingent admission budget, request borrowed
  capacity recall, reduce burst pool, increase protected headroom, fence
  oversubscribed admission, revalidate evidence, resolve conflicting evidence;
- durable policy, domain configuration, provenance, audit history, fencing, and
  epoch, with crash-safe journals and replay;
- the publisher/coordinator transport and the multiprocess proof.

Oversubscription Governor does NOT own discovering capacity, admitting individual
flows, arbitrating bandwidth, reserving capacity, scheduling flows, placing
paths, enforcing rates, or performing congestion control. It never performs an
adjacent-system action: it produces bounded intent and the operator decides how
that intent reaches the system that owns the action.

## Core principles

1. **Oversubscription is not accidental overcommit.** A configured ratio is a
   deliberate policy. Guarantees that no longer fit inside usable capacity are a
   capacity emergency, and they are reported as one, not as oversubscription.
2. **Configured ratio is not current authority.** The legal boundary is the
   minimum of the configured ratio, the policy ratio cap, the degraded ratio
   cap, the risk budget, and the protected-obligation floor, all measured
   against *effective* usable capacity.
3. **Contingent capacity is not guaranteed capacity.** Contingent authority is
   computed after guarantees and the emergency reserve have been removed from
   the ceiling, and the two are never summed into one number.
4. **Quiet telemetry is not permission.** Evidence outside its freshness budget
   produces `STALE`; missing evidence produces `UNKNOWN`. Neither grants
   authority, and neither preserves a previous answer.
5. **Degradation changes the legal boundary.** A degraded or failed member
   rescales capacity and applies the degraded ratio cap. If policy defines no
   degraded cap, the effective ratio collapses to 1:1 rather than silently
   preserving the old ratio.
6. **Arithmetic closes or authority is denied.** Every externally influenced
   size, capacity, ratio, and budget is computed with checked helpers. Overflow
   yields `UNKNOWN` with no authority.
7. **Decisions are bound to generations.** A decision is valid only while its
   authority vector still matches the runtime. Restart, epoch advance, policy
   install, new evidence, or an incarnation change invalidate it.

## Model

Strongly typed identities and generations: `OversubscriptionDomainId`,
`OversubscriptionPolicyId`, `ResourceId`, `PoolId`, `RiskBudgetId`,
`ServiceClassId`, `ReservationId`, `AdmissionId`, `EvidenceId`,
`PublisherId`, `DecisionId`, `IncarnationId`, plus
`DomainGeneration`, `PolicyGeneration`, `ResourceGeneration`,
`CapacitySnapshotGeneration`, `ReservationSnapshotGeneration`,
`AdmissionSnapshotGeneration`, `RiskBudgetGeneration`, and `FabricEpoch`.

Evidence is domain-scoped and generation-bound:

- **CapacitySnapshot** — per resource physical capacity, usable capacity, health
  (`HEALTHY`, `DEGRADED`, `FAILED`, `UNKNOWN`), degraded capacity scale, and
  resource generation.
- **ReservationSnapshot** — protected guarantees per resource and service class,
  with a reservation generation and priority reference owned by the external QoS
  system.
- **AdmissionSnapshot** — admitted and pending demand per resource and service
  class classified as `GUARANTEED`, `CONTINGENT`, or `BURST`.

Every snapshot carries provenance: publisher, publisher incarnation, fabric
epoch, monotonic sequence, logical observation tick, wall-clock correlation
only, idempotency evidence id, and the canonical SHA-256 payload digest.

## Policy model

A policy declares a hard ratio cap, one or more contention domains, service
classes, a risk budget, thresholds, hysteresis, freshness budgets, and explicit
bounds. Each domain declares its pool, member resources, configured ratio, an
optional degraded ratio cap, an emergency reserve, a minimum protected headroom,
and an optional burst pool with a cap.

Policy is validated exhaustively before installation: zero or out-of-bound ratio
terms, configured ratios above the policy cap, degraded caps above the configured
ratio, inverted thresholds, unbounded freshness budgets, duplicate domains,
duplicate or empty membership, unknown service classes, invalid risk budgets,
reserve overflow, and a digest that does not match its contents are all
rejected with a precise reason code.

## Evaluation

`evaluate_domain()` is a pure function of policy, authority expectation, and
evidence. For each domain it:

1. binds evidence to the runtime's epoch, generations, and policies;
2. rejects future-dated, stale, structurally duplicated, or policy-oversized
   evidence;
3. rejects contradictory guarantees, unsupported guaranteed-class admissions,
   and oversubscription in ineligible service classes;
4. computes effective capacity after health degradation;
5. computes `protected_required = guarantees + emergency reserve`,
   `floor_required = protected_required + minimum protected headroom`, and the
   legal ceiling as the minimum of the applicable ratio and risk-budget caps;
6. derives `contingent_ceiling`, `contingent_free`, `overage`, exposure, and
   risk-budget consumption;
7. identifies the single binding constraint, including per-resource ceilings;
8. produces one outcome and bounded corrective intent.

Outcomes:

| Outcome | Meaning |
| --- | --- |
| `WITHIN_POLICY` | Legal, with contingent authority remaining. |
| `AT_LIMIT` | Legal, zero contingent authority remaining. |
| `REDUCTION_REQUIRED` | Commitments exceed the legal ceiling. |
| `EMERGENCY_REDUCTION` | Guarantees at risk or overage beyond the emergency threshold. |
| `POLICY_REJECTED` | Policy does not authorize the state (for example an ineligible class holding oversubscribed demand, or a floor the ceiling cannot satisfy). |
| `UNKNOWN` | Evidence is missing, health is unknown, a member has no capacity evidence, or arithmetic could not close. |
| `STALE` | Generations, epoch, or freshness do not match the runtime. |
| `CONFLICTING_INPUT` | Evidence is structurally contradictory. |

Authority is granted only by `WITHIN_POLICY`. `AT_LIMIT` is authoritative but
grants no increment. `REDUCTION_REQUIRED` and `EMERGENCY_REDUCTION` are
authoritative and produce corrective intent. `POLICY_REJECTED`,
`CONFLICTING_INPUT`, `STALE`, and `UNKNOWN` grant nothing.

Because the runtime never admits flows itself, the answer to "where may
deliberate oversubscription occur" is expressed as per-resource contingent
ceilings and free authority, plus the domain-level ceiling after domain-wide
constraints. Allocating a domain-wide limit across resources is left to the
adjacent admission system.

## Invariants

- Guaranteed obligations are never counted as contingent authority, and
  contingent authority is computed only from capacity left after guarantees and
  the emergency reserve.
- Authorized increments never exceed the policy ratio cap, and every ceiling is
  bounded by it.
- Degradation never raises the effective ratio, and an effective ratio is never
  above the configured ratio.
- Increasing demand never relaxes the outcome; decreasing capacity never
  increases authority.
- Stale, missing, or contradictory evidence invalidates authority rather than
  preserving a previous answer.
- Decision digests are reproducible across processes and restarts for identical
  inputs.

These are enforced by unit tests, seeded randomized property tests that check the
invariants over generated populations, and adversarial tests.

## Authority, hysteresis, and fencing

Each decision carries an authority vector: domain and domain generation, policy
and policy generation, capacity, reservation, admission, and risk-budget
generations, the fabric epoch, a fold over member resource generations, and the
canonical digest of the evaluated evidence. `revalidate()` re-checks that vector
against the runtime and reports whether the decision is still bound, superseded,
fenced, or invalidated by a restart.

Restriction is never delayed: an escalation takes effect on the evaluation that
observes it. Relaxation is hysteresis-controlled, requiring configured
confirmations, a cooldown since the last escalation, and remaining slack before
the sticky severity is lowered. `AT_LIMIT` announcements can require
confirmations, and because they grant no authority either way, that
confirmation delay can never grant anything.

Fences are durable. A domain is fenced by an emergency, a policy rejection, or a
conflicting-input result, and a fence raised by an epoch advance survives every
non-authoritative evaluation until an authoritative result is produced from
fresh evidence.

## Evidence ingestion and publisher authority

Publishers register with a publisher id, an incarnation, and the fabric epoch.
Every publication is checked for epoch agreement, registration, fence state,
incarnation match, sequence monotonicity, evidence-id idempotency, and generation
ordering. The results are precise:

- an exact republication is an idempotent duplicate and changes nothing;
- the same evidence id with different content is `CONFLICTING_INPUT`;
- a sequence that is not ahead of the last accepted one is `STALE`;
- evidence generation must be exactly the next generation, or zero to ask the
  runtime to assign it;
- a different incarnation of a registered publisher is refused until the
  operator explicitly adopts it, which fences the previous incarnation and
  resets its sequence.

Advancing the fabric epoch drops all cached evidence, fences every publisher
registration and every governed domain, and refuses evidence from the old epoch
until publishers re-register in the new one.

## Persistence and restart

`Store` writes a versioned state file and an append-only audit journal. Both are
framed, length-bounded, and integrity-checked with SHA-256 over the payload;
state replacement is atomic (temporary file, flush to stable storage, atomic
replace, directory sync on POSIX). The journal is compacted under a configured
byte and record bound, and a truncated or corrupt tail is recovered to the last
valid frame and rewritten so later appends can never sit behind unreadable
bytes. Corrupt, truncated, oversized, and future-version state files are
rejected; the runtime never silently resets to a fresh state.

Durable state is exactly: policy, durable domain configuration, publisher
registrations, evidence generation bookkeeping, hysteresis and fencing, epoch,
incarnations, decision counters, and audit history. Dynamic evidence is never
durable. After a restart the generation continuum survives but the evidence does
not, so authority must be re-established from fresh publications, and decisions
issued by the previous incarnation fail revalidation. Hysteresis and fences are
rebuilt by replaying the journal, and `oversub_cli replay` performs that
reconstruction explicitly.

## Real multiprocess proof

`oversub_dist` provides two real roles: a coordinator (the runtime and
listener) and a worker (a publisher client). They communicate over framed TCP on
loopback with a bounded, versioned message set. The distributed test spawns them
as separate OS processes and verifies:

- a worker process publishes capacity, reservation, and admission evidence, gets
  a decision, and revalidates it (all three evidence kinds are exchanged over a
  real socket, and the decision digest is recomputed and checked in the client);
- an exact republication by the same worker is reported as an idempotent
  duplicate;
- a stale or future epoch claim is rejected with `STALE`/`EpochMismatch`;
- a new incarnation of a publisher is refused until it is explicitly adopted;
- a coordinator process killed without a clean shutdown restarts from durable
  state, preserves its epoch and evidence generations, refuses to restore the
  previous decision as live authority, and accepts the next generation of
  evidence to re-establish authority;
- protocol violations (a non-`HELLO` first frame, a version mismatch, a
  malformed payload, an oversized frame declaration) are rejected without
  disturbing the coordinator.

## REAL / SYNTHETIC / UNSUPPORTED

- **REAL**: the governance runtime, checked arithmetic, canonical digests,
  policy validation, deterministic evaluation, durable state with integrity
  checks and crash recovery, journal replay, publisher/incarnation/epoch
  fencing, cancellation and shutdown semantics, loopback TCP framing, real OS
  process spawning, killing, and restart, and the multiprocess proofs above.
- **SYNTHETIC**: every population produced by `synth.cpp` and used by tests,
  examples, the CLI, the benchmark, and the distributed tool. These are in-memory
  populations. They are never presented as physical-network measurements.
- **UNSUPPORTED**: no physical network hardware is exercised anywhere in this
  repository. There is no NIC, switch, DPU, RDMA, NVLink, optical, or
  fabric-telemetry integration, and no claim of one. Enforcement of corrective
  intent is not implemented: adapters record or forward intent, and the operator
  owns the integration.

## Benchmark

`oversub_bench` measures completed governance work over synthetic populations:
it counts an evaluation only after a decision is returned, and it reports the
outcome mix so the work cannot be mistaken for a no-op. `--durable` additionally
enables synchronous durable writes on the governor path.

Measured on this machine (Windows 11 x64, MSVC 19.44, Release, 16 logical cores),
with `durable_writes=false`:

| domains | resources/domain | obligations | demand/record | demand scale | decisions | engine evals/s | governor decisions/s | outcome mix |
| --- | --- | --- | --- | --- | --- | --- | --- | --- |
| 1 | 4 | 2 | 4 | 1.00x | 4000 | 164014 | 9698 | 4000 within policy |
| 4 | 8 | 2 | 6 | 1.00x | 4000 | 90518 | 8280 | 4000 within policy |
| 16 | 16 | 4 | 8 | 1.00x | 2000 | 38858 | 7189 | 2000 within policy |
| 1 | 64 | 8 | 16 | 1.00x | 4000 | 6785 | 3367 | 4000 within policy |
| 8 | 64 | 8 | 16 | 1.00x | 2000 | 5642 | 3070 | 2000 within policy |
| 64 | 64 | 16 | 32 | 1.00x | 1000 | 4747 | 2305 | 1000 within policy |
| 4 | 8 | 2 | 6 | 3.00x | 4000 | 89548 | 6388 | 4000 reduction required |
| 16 | 16 | 4 | 8 | 3.00x | 2000 | 37029 | 6722 | 2000 reduction required |
| 8 | 64 | 8 | 16 | 3.00x | 2000 | 6470 | 3277 | 2000 reduction required |

The engine column is the pure deterministic core; the governor column includes
ingestion-checked, generation-bound decisions with audit journaling. These
figures are synthetic-population throughput, not network performance.

## Build, install, and consume

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
ctest --test-dir build --output-on-failure
cmake --install build --prefix <prefix>
```

A downstream project consumes the installed package with:

```cmake
find_package(OversubscriptionGovernor 1.0 CONFIG REQUIRED)
target_link_libraries(my_app PRIVATE OversubscriptionGovernor::OversubscriptionGovernor)
```

`downstream/consumer` is a real, independent consumer that is built against the
installed prefix and runs a governance evaluation. CMake options:
`OVERSUB_BUILD_TESTS`, `OVERSUB_BUILD_DIST`, `OVERSUB_BUILD_EXAMPLES`,
`OVERSUB_BUILD_CLI`, `OVERSUB_BUILD_BENCHMARKS`, `OVERSUB_STRICT_WARNINGS`,
`OVERSUB_ENABLE_ASAN`.

The CLI (`oversub_cli`) provides `version`, `evaluate`, `validate-state`,
`history`, `replay`, and `digest`. Runnable examples cover within-policy
governance, reduction required, capacity collapse, stale evidence, policy change,
restart recovery, and epoch advance.

## Validation

The suite is dependency-free and has no timeouts: a hanging test is a defect.

| Suite | Coverage |
| --- | --- |
| `core_tests` | digests, checked arithmetic (including reference cross-checks), ratio algebra, policy validation, all eight outcomes, guarantee protection, contingent/guaranteed separation, degradation, zero capacity, overflow, binding constraints, bounded explanations, authority binding, hysteresis, epoch and policy invalidation. |
| `property_tests` | seeded randomized populations checking cap containment, guarantee protection, arithmetic closure, determinism, monotonicity in demand and capacity, degradation monotonicity, explanation bounds, and agreement between the runtime and the pure engine. |
| `adversarial_tests` | malformed, contradictory, oversized, future-dated, out-of-scope, and hostile evidence; contradictory guarantees; capacity beyond policy bounds; huge values; random-byte decoding; lifecycle bounds; corrupt and truncated durable state. |
| `concurrency_tests` | parallel evaluations, concurrent publication, idempotent duplicates, policy installs racing evaluations, cancellation, bounded queues, shutdown draining with no lost or phantom decisions, and a public-API surface sweep that would deadlock on lock re-entry. |
| `persistence_tests` | round trip, restart semantics, journal replay, recovery from truncated and corrupt journal frames, journal bounds and compaction, atomic state replacement, rejection of corrupt, truncated, wrong-magic, unsupported-version, and invalid-policy state files. |
| `net_tests` | frame round trips at bound sizes, oversized declarations, clean close versus mid-frame truncation, fragmented reassembly, sequential and concurrent clients, invalid addresses. |
| `distributed_tests` | protocol decoder fuzzing, in-process coordinator with wire round trips, protocol violations, real worker processes, stale-epoch rejection over the wire, incarnation fencing after an abrupt kill, and coordinator restart from durable state after an abrupt kill. |

Status on this machine:

- Release and Debug both build with MSVC `/W4 /WX /permissive-` and pass all
  seven suites.
- AddressSanitizer (`-DOVERSUB_ENABLE_ASAN=ON`, `/fsanitize=address`) passes all
  seven suites with no reports. Leak detection is not supported by MSVC
  AddressSanitizer on Windows, so leak checking is explicitly out of scope here.
- MSVC `/analyze` over the runtime library reports no first-party findings.

## Limitations

- Enforcement is not implemented. Corrective intent is produced and delivered to
  operator-supplied adapters; the governor never touches an adjacent system.
- Capacity, reservation, and admission evidence must be supplied by systems that
  own those facts. This repository validates and governs that evidence; it does
  not measure it.
- Only loopback TCP transport is exercised. No physical network, NIC, switch,
  DPU, RDMA, NVLink, or optical hardware is used or claimed.
- The distributed proof runs coordinator and workers as local processes on one
  host. It proves real framing, real process boundaries, fencing, and durable
  restart; it does not claim multi-host or network-fabric validation.
- A store directory is owned by one runtime instance; concurrent writers to the
  same directory are outside the supported model.
- LeakSanitizer is unavailable with MSVC AddressSanitizer on Windows, so
  lifetime errors are covered by AddressSanitizer's other checks, by the
  concurrency and shutdown tests, and by review rather than by leak detection.

## License

Apache License 2.0. Copyright 2026 Summon Software Labs. No telemetry transmission.
