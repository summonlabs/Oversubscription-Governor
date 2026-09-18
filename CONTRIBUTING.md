# Contributing to Oversubscription Governor

Contributions are accepted under the Apache License 2.0. No Contributor License
Agreement is required.

## Ground rules

- Keep the runtime boundary intact. The governor owns policy and authority for
  deliberate oversubscription; it does not discover capacity, admit flows,
  arbitrate bandwidth, reserve capacity, schedule flows, place paths, enforce
  rates, or perform congestion control. Changes that absorb an adjacent system
  will not be accepted.
- Missing, stale, or contradictory evidence must never become positive
  authority. `UNKNOWN`, `STALE`, and `CONFLICTING_INPUT` are honest results and
  must stay reachable.
- Arithmetic that touches externally influenced sizes, capacities, ratios, or
  budgets must use the checked helpers in `include/oversub/checked.hpp`.
- Authoritative decisions must stay deterministic and explainable: identical
  inputs and generations must produce identical decision digests.
- Durable state must not silently restore process liveness, evidence freshness,
  leases, worker authority, or publisher authority.

## Building and testing

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

The suite has no test timeouts: a hanging test is a defect, not a slow test.
Please also run the Debug configuration, and an AddressSanitizer configuration
where the platform supports it:

```sh
cmake -S . -B build-asan -DOVERSUB_ENABLE_ASAN=ON
cmake --build build-asan --parallel
ctest --test-dir build-asan --output-on-failure
```

## Style

- C++20, four-space indent, 110-column soft limit.
- No new third-party dependencies in the runtime library.
- New behaviour needs a test that fails without it, and new limits need a bound.
- Do not add AI attribution or co-author trailers to commits.
