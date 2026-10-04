# Power Failure Manager

Power Failure Manager (PFM) owns facility electrical-failure coordination: the
authoritative failure state during a feed, switchgear, PDU, UPS, generator, or
circuit failure, the affected scope that failure implies, the isolation and
protection requirements that must hold, the bounded requests that may be sent to
the authorities that own the switching, and the evidence required before
recovery may begin.

Version 1.0.0.

## The question this repository answers

Given current electrical-failure evidence and facility obligations:

* what failure state is authoritative;
* what must be isolated or protected;
* which bounded response requests are justified;
* what evidence is required for safe recovery.

## Owned boundary

PFM owns, end to end:

* electrical-failure identity, including incident and incident generations;
* the electrical-failure classification and its reason traces;
* the lifecycle of an electrical incident as PFM models it;
* control epoch, controller incarnation, and stale-plan fencing;
* electrical evidence intake, its currency, and its contradiction handling;
* affected-scope resolution over supplied electrical topology and failure
  domains, including shared upstream domains;
* isolation and protection requirements derived from a classification;
* deterministic response planning and ordering;
* bounded requests to the owning electrical controllers;
* acknowledgement, observed effect, and verification of those requests;
* recovery gates, dwell, and eligibility;
* durable attempt, evidence, transition, and decision history.

## Explicit non-ownership

PFM never performs any of the following, and its public API cannot be used to do
so:

* discovering, inferring, or repairing facility electrical topology;
* actuating a breaker, switch, transfer switch, PDU, UPS, generator, or any
  other electrical device;
* owning feed selection, bus topology, or device control;
* executing load shedding;
* deriving facility power capacity;
* owning generic incident state beyond the electrical-failure domain;
* owning cross-domain recovery orchestration;
* bypassing a safety interlock;
* claiming an electrical effect from an acknowledgement.

Adjacent authorities are addressed only through typed, attributable requests,
and their state arrives only as typed, generation-stamped evidence or as opaque
references. Electrical topology and failure domains are supplied by the
authority that owns them and are treated as opaque, validated, and
attributable.

## Architecture

The runtime is a C++20 library (namespace `summon::pfm`) with a small
administration CLI, an example, a benchmark, a test suite, and an installable
CMake package.

| Header | Contents |
| --- | --- |
| `pfm/status.hpp` | stable machine-readable `StatusCode` values, `Status`, `Result<T>` |
| `pfm/ids.hpp` | strongly typed identities, generations, epochs, revisions, fingerprints |
| `pfm/units.hpp` | exact integer quantities, checked arithmetic, timestamps, durations |
| `pfm/refs.hpp` | canonically validated opaque reference tokens per kind |
| `pfm/authority.hpp` | authority tokens and deterministic fencing precedence |
| `pfm/policy.hpp` | freshness windows, thresholds, reserve floors, resource bounds |
| `pfm/evidence.hpp` | electrical observations, currency classification, conflict rules |
| `pfm/topology.hpp` | supplied electrical topology, failure domains, and their index |
| `pfm/classification.hpp` | typed failure classes, reason codes, reason traces |
| `pfm/scope.hpp` | affected scope, isolation boundary, unresolved upstream |
| `pfm/obligations.hpp` | protected obligations, protection classes, blocking rules |
| `pfm/request.hpp` | the bounded request vocabulary and its lifecycle |
| `pfm/plan.hpp` | deterministic response planning |
| `pfm/recovery.hpp` | recovery gates and eligibility |
| `pfm/journal.hpp` | the authoritative input journal and transition records |
| `pfm/state.hpp` | durable domain state, derivation, and the single `apply` mutator |
| `pfm/codec.hpp` | canonical encoding primitives and checksums |
| `pfm/store.hpp` | the dual-slot durable store, the OS lock, and snapshot codecs |
| `pfm/transport.hpp` | the adjacent-controller transport and dispatch outcomes |
| `pfm/clock.hpp` | the injected clock |
| `pfm/runtime.hpp` | the public `PowerFailureRuntime` API |
| `pfm/scenario.hpp` | the synthetic electrical plant and scenario engine |
| `pfm/report.hpp` | text rendering for inspection and audit output |

### Typed electrical failures

A failure class is a decision about what failed, derived from typed current
evidence and the supplied topology. It is never a restatement of an alarm.

| Class | Machine code | Derived from |
| --- | --- | --- |
| `UtilityFeedLoss` | `pfm.failure.utility_feed_loss` | a utility feed observed de-energized |
| `UtilityFeedDegradation` | `pfm.failure.utility_feed_degradation` | a feed present but outside the voltage or frequency band |
| `SwitchgearFailure` | `pfm.failure.switchgear` | switchgear tripped, or de-energized while its whole supply chain is energized |
| `BusFailure` | `pfm.failure.bus` | a bus in the same two situations |
| `BreakerFailure` | `pfm.failure.breaker` | a breaker tripped, or open with supply present upstream |
| `CircuitFailure` | `pfm.failure.circuit` | a branch circuit in the same situations |
| `PduFailure` | `pfm.failure.pdu` | a PDU de-energized while its supply chain is energized |
| `PduBranchFailure` | `pfm.failure.pdu_branch` | a PDU branch circuit in the same situation |
| `UpsFailure` | `pfm.failure.ups` | a UPS de-energized or running on battery |
| `UpsReserveInsufficient` | `pfm.failure.ups_reserve` | a UPS reserve at or below the floor, or at or below the critical floor |
| `GeneratorFailure` | `pfm.failure.generator` | a generator reporting a fault |
| `GeneratorStartFailure` | `pfm.failure.generator_start` | transfer demanded while the generator is off or cooling down |
| `GeneratorSyncFailure` | `pfm.failure.generator_sync` | a running generator that has not synchronized, or a transfer that never completed synchronization |
| `GeneratorTransferFailure` | `pfm.failure.generator_transfer` | a synchronized generator whose transfer failed |
| `SharedUpstreamDomainFailure` | `pfm.failure.shared_upstream_domain` | a failure inside a failure domain that carries more than one downstream domain |
| `AmbiguousElectricalEvidence` | `pfm.failure.ambiguous_evidence` | stale, expired, missing, contradicted, or bad-quality evidence |

Ordering is by severity, then class, then element reference. Ambiguous evidence
outranks every concrete failure, because unresolved evidence is never downgraded
into a smaller problem; the concrete root cause outranks the shared-domain
finding it triggers.

### Evidence currency and fail-closed rules

Every observation carries an origin, a quality, an instant, a sequence, and a
content fingerprint. Absent readings are flagged, never zeroed: "missing" and
"zero" are different facts.

* evidence inside the freshness window is current and may justify a decision;
* evidence inside the expiry window is stale: it stops justifying anything;
* evidence older than that is expired, and is treated as unknown rather than as
  a healthy reading;
* an instant ahead of "now" is future, and clock skew never creates freshness;
* two reports of one element are in genuine conflict only when they come from
  different sources, describe instants inside one freshness window of each
  other, and assert facts that cannot both hold. Successive reports from one
  source are an evolution of state, and the newer one supersedes the older;
* an element that used to report and no longer does is unresolved, enters the
  plan as an ambiguous failure, and blocks recovery.

A de-energized element is a root failure only when its entire supply chain is
energized. A loss of supply that the upstream chain already explains is
collateral: it widens the affected scope instead of inventing a second root
cause.

### Affected scope and shared failure domains

The scope is resolved from the classification over the supplied topology:

* failed elements, and the downstream closure of each;
* the load groups served through that closure;
* the power and failure domains involved;
* the isolation points that can de-energize the failed elements;
* the upstream elements whose current state could not be established;
* the elements inside the boundary that no longer report.

Every list is sorted and unique, so the scope never depends on iteration order.

A failure domain that carries more than one downstream domain is shared. A
failure inside a shared domain raises `SharedUpstreamDomainFailure`, expands the
scope to the whole domain, and blocks recovery while the domain is impacted. A
locally healthy component inside a shared domain is not recoverable while that
domain is unresolved.

### Response planning

The plan is the decision record. It is bound to the incident, incident
generation, topology generation, policy generation, and control epoch it was
computed from, and carries a fingerprint over all of them.

For each classified failure the planner derives isolation requirements and the
bounded requests that follow from them:

| Failure | Bounded requests (owner) |
| --- | --- |
| utility feed loss or degradation | isolate the element, verify de-energization (power control plane); start generator for an available generator in the affected domains |
| switchgear, bus, PDU, PDU branch, UPS failure | isolate the element, verify de-energization (power control plane) |
| breaker or circuit failure | defer reclose until the fault is proven clear, verify de-energization (power control plane) |
| UPS reserve insufficient | preserve the reserve floor (UPS control); shed the load groups served |
| generator start, sync, or transfer failure | start, synchronize, or transfer (generator control) |
| generator fault | isolate the generator (power control plane) |
| ambiguous evidence | verify de-energization (power control plane): the controller is asked to prove the state, never to change it blindly |
| element still unhealthy after the fault clears | reclose the isolation point, or restore the normal feed (power control plane, feed authority) |

Requests are ordered by kind, then owning authority, then target reference, then
magnitude, so two runs over the same evidence produce the same sequence. A plan
that would exceed the configured request bound is refused rather than truncated.

Every request carries the reason code and the subject that justified it, the
evidence fingerprints behind it, its plan generation, its attempt identity, and
an idempotency key derived from the logical operation. The key deliberately
excludes the attempt, the plan generation, and every timestamp: re-planning or
re-attempting the same operation reuses the same key, so a lost response can
never cause a second consequential mutation.

### Request lifecycle

`Planned -> Issued -> Acknowledged -> Observed -> Verified`, with the terminal
states `Failed`, `Superseded`, `Abandoned`, `Refused`, `Expired`, and the
resolvable state `Indeterminate`.

An acknowledgement is not an observation, and an observation is not a
verification. Only current external evidence that carries the required proof
kind moves a request to `Verified`, and the proof kind is checked against the
evidence: an isolation proof needs proven de-energization or an open breaker, a
transfer proof needs a settled transfer state, a reserve proof needs a reserve
at or above the floor, and so on. Evidence that does not carry the proof moves
the request only to `Observed`.

Requests are durable before they leave the process. The intent, its plan
generation, its attempt identity, and its idempotency key are committed before
dispatch, so a death around dispatch can never produce a blind duplicate
consequential request. A request that was in flight when a controller stopped is
resolved as `Indeterminate` and is never silently retried; a re-attempt is a new
attempt identity with the same idempotency key, and it is bounded by the
configured attempt count.

### Recovery gates

Recovery is gated, not implied. Every gate is satisfied only by current,
independent evidence:

| Gate | Satisfied when |
| --- | --- |
| `FaultCleared` | no classified failure is still observed, and every electrical element in the cumulative incident scope reports healthy |
| `IsolationVerified` | every derived isolation point has a verified isolation proof |
| `DeEnergizationVerified` | every derived isolation requirement has a verified de-energization proof |
| `BreakerStateProven` | every breaker involved reports a known position that is not tripped |
| `TransferStable` | every transfer path has settled |
| `GenerationStable` | every generator in scope is running or synchronized, with no failed transfer |
| `ReserveRestored` | every UPS in scope holds the policy floor and any obligation floor above it |
| `SharedDomainResolved` | no shared failure domain is impacted |
| `UpstreamEvidenceCurrent` | no upstream element of the failed elements lacks current evidence |
| `ObligationsSatisfied` | no protected obligation blocks recovery |
| `EvidenceCurrent` | no element inside the scope is unevidenced |
| `StabilityDwell` | a clean plan has been sustained for the configured dwell |
| `OperatorAuthorization` | an explicit authorization exists for this incident generation |
| `RequestsSettled` | no bounded request is still open |

The incident scope is cumulative: every element the incident has ever impacted
stays part of the recovery question, so isolating an element during the response
cannot silently drop it. Recovery is entered only through an explicit,
authorized request while the incident is stabilizing, and a new failure returns
the incident to active response without keeping recovery progress.

### Protected obligations

| Protection class | Response authority may relax |
| --- | --- |
| `HardSafetyInterlock` | never |
| `Regulatory` | never |
| `ServiceLevel` | only with an explicit relaxation recorded against the incident and the granting authority |
| `AdvisoryOptimization` | only with an explicit relaxation recorded the same way |

An obligation that is violated, at risk, unknown, unreported, or whose report is
no longer current blocks recovery. Nothing in the planner or the escalation path
can route around that.

## Authority, generations, and fencing

Every authority-bearing mutation carries an authority token: incident, incident
generation, control epoch, controller incarnation, and the state revision the
caller observed. Validation precedence is fixed, so the same invalid request
always produces the same primary machine-readable code:

1. missing authority (zero epoch or zero incarnation);
2. stale epoch; 3. future epoch;
4. stale incarnation;
5. incident-bound token with no live incident; 6. cross-incident token;
7. stale generation; 8. future generation;
9. stale revision; 10. future revision.

Opening a store that already holds durable state is itself fenced:

* a lower control epoch than the durable one is refused;
* the same epoch with a different incarnation is refused: a new controller must
  roll the epoch explicitly;
* a higher epoch performs a rollover: every non-terminal request of the previous
  authority is superseded, recovery progress is reset, and the restored evidence
  counts for nothing until it is replaced by live evidence;
* the same epoch and incarnation resume the same authority: in-flight dispatches
  are resolved as indeterminate rather than repeated, and the incident identity
  is kept.

Restored durable state is not current physical evidence. On every reopen, all
restored observations are marked as recovered and are excluded from decisions
until a live observation from an external source replaces them, and no plan
survives a restart as current.

## Persistence and recovery

The store is a single directory containing two slot files and an OS lock file.

* Each slot holds a complete snapshot: a 56-byte header (magic, format version,
  two reserved words, store generation, commit sequence, commit instant, payload
  length, payload CRC-32C, header CRC-32C over the preceding 48 bytes, and a
  trailing reserved word), a payload, and an 8-byte trailer (magic plus total
  length).
* The payload carries the checkpoint state, the live state, and the retained
  journal entries, each length-prefixed.
* A commit encodes, bounds-checks, stages into a file in the same directory,
  writes it, flushes it to the device, reads it back and compares it, and
  finally replaces the inactive slot atomically. The commit point is that atomic
  replacement: before it, the previous slot is authoritative.
* The authoritative slot is the valid slot with the highest commit sequence. If
  the newest slot is unusable, the store falls back to the previous complete
  generation and reports that it did so.
* Decoding is strict: magic, format version, reserved fields, header and payload
  checksums, exact length agreement, exact trailing-byte rejection, hard bounds
  on every declared length before allocation, and rejection of impossible enum
  values.
* Every load verifies the replay invariant: applying the retained journal to the
  checkpoint must reproduce the persisted live state byte for byte. A mismatch
  is reported as a replay divergence, not silently accepted.
* A store in which no slot file was ever written is empty and may start its
  first generation. A store whose slot files exist but are unusable is refused:
  silently starting a new generation over an unreadable one would discard
  authoritative history.
* Single-writer authority is an OS-level exclusive handle on a canonicalised
  lock file inside the store directory, so two processes cannot hold the same
  store and a killed process cannot leave a stale lock.

Derived values are not durable facts. Observation currency and obligation report
currency are functions of the instant they are asked about, so they are encoded
canonically and recomputed, which keeps a replayed generation byte-identical to
the live one.

The journal is bounded. When the bound is reached, the retained entries are
folded into the checkpoint and retired, so replay always starts from a complete
checkpoint. The retirement count is store metadata and is deliberately outside
the replay equality check.

## Concurrency model

One writer. Durable mutation is serialised by the OS lock, in-process state by a
single mutex. Transport code is never called with that mutex held: an evaluation
computes and persists its decisions, releases the lock, dispatches to the
adjacent controller, and then records the answers in a second committed step.
The persisted intent is therefore the write-ahead record for every external
request. A transport that calls back into the runtime is refused with
`pfm.reentrancy_refused` instead of deadlocking. A failed durable commit fences
the runtime: no further authority-bearing operation is accepted until it is
reopened from the store.

## Build, test, and install

Requirements: CMake 3.25 or newer, and a C++20 compiler. Windows/MSVC is the
primary target; the core is portable.

```sh
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
cmake --install build --config Release --prefix install
```

Debug and sanitizer configurations:

```sh
cmake -S . -B build-debug -G "Visual Studio 17 2022" -A x64
cmake --build build-debug --config Debug
ctest --test-dir build-debug -C Debug --output-on-failure

cmake -S . -B build-asan -G "Visual Studio 17 2022" -A x64 -DPFM_ENABLE_ASAN=ON
cmake --build build-asan --config RelWithDebInfo
ctest --test-dir build-asan -C RelWithDebInfo --output-on-failure
```

First-party code is built with `/W4 /permissive- /WX` on MSVC (and
`-Wall -Wextra -Wpedantic -Wshadow -Werror` elsewhere). No warning is globally
suppressed.

CMake options: `PFM_BUILD_TESTS`, `PFM_BUILD_TOOLS`, `PFM_BUILD_EXAMPLES`,
`PFM_BUILD_BENCHMARKS`, `PFM_WARNINGS_AS_ERRORS`, `PFM_ENABLE_ASAN`.

## Consuming the installed package

The installed package exports the namespaced target `Summon::pfm`:

```cmake
find_package(PowerFailureManager 1.0 REQUIRED)
target_link_libraries(your_target PRIVATE Summon::pfm)
```

`tests/consumer` is an independent out-of-tree project that configures against
an installed prefix, links the exported target, opens a durable runtime,
publishes a topology, admits evidence, evaluates, verifies that the store
replays, and removes its own store. It never references the build tree.

## Examples, CLI, and benchmark

`examples/electrical_failure.cpp` drives a complete electrical failure through
the library: a lost utility feed, the classification and the bounded requests
that follow, the acknowledgements and the confirmed effects, the restoration of
the feed and of the isolated elements, an authorized recovery, and a reopen of
the store from a fresh runtime.

`pfmctl`:

```sh
pfmctl version
pfmctl demo [--store DIR] [--keep]
pfmctl scenario --name feed-loss-recovery
pfmctl scenario --all
pfmctl scenarios
pfmctl inspect --store DIR
pfmctl verify --store DIR
pfmctl boundaries
```

`pfm_bench` measures completed operations only, with warm-up runs discarded:

```sh
pfm_bench --iterations=500
```

## Tests

Six test binaries run plainly, with no timeout mechanism of any kind:

| Binary | Proof obligations | Cases |
| --- | --- | --- |
| `pfm_unit_tests` | exact arithmetic and boundary refusal, identity typing, reference validation, the status-code contract, policy and obligation validation, codec and checksum vectors, topology validation and queries, request fingerprint and idempotency rules | 66 |
| `pfm_state_machine_tests` | classification over every element kind, collateral and shared-domain scope, plan determinism, request lifecycle, authority fencing precedence, every recovery gate, and a seeded randomized comparison of classification, scope and gate evaluation against an independent reference model | 47 |
| `pfm_persistence_tests` | snapshot round trip, header/payload/trailer corruption, truncation, trailing bytes, version and reserved-field rejection, dual-slot fallback, staging-file safety, replay divergence, journal retention, lock exclusivity, volatile mode | 10 |
| `pfm_process_tests` | real OS processes: single-writer exclusion, lock release on hard kill, crash consistency across a killed commit loop, no blind duplicate consequential request after a death around dispatch, epoch rollover across processes | 6 |
| `pfm_scenario_tests` | the synthetic plant model, every named scenario, durable and volatile lifecycle runs, and the recovery refusal/eligibility narrative | 17 |
| `pfm_adversarial_tests` | malformed, hostile and extreme input, contradictions, reordered and duplicate evidence, reentrancy, four-thread concurrent mutation, repeated lifecycle calls, integer extremes, bounds exhaustion | 14 |

160 cases in total, all passing in every configuration below.

## Real, synthetic, and unsupported proof

* **REAL** — the library, the CLI, the example, the benchmark, the durable store,
  the file lock, crash and kill behaviour of real OS processes, package install,
  and downstream `find_package` consumption are exercised as software on this
  host. Every test, benchmark, and validation result below was produced by real
  processes and real files.
* **SYNTHETIC** — the electrical plant, its topology, its failure domains, its
  evidence, and the adjacent controllers that answer bounded requests are
  modelled in-process. Every scenario, including the end-to-end recovery, is
  synthetic, and every observation it produces is labelled
  `ObservationOrigin::SyntheticPlant`. No utility feed, switchgear, breaker,
  bus, PDU, UPS, generator, ATS/STS, BMS, DCIM, or PLC was contacted, and no
  device was actuated.
* **UNSUPPORTED** — there is no hardware validation of any kind: no real
  electrical equipment, no real meter or sensor, no real BMS/DCIM integration,
  and no real interconnect with a Feed Authority, Power Control Plane,
  PDU/UPS/Generator Control, Load Shedding, Power Capacity, Incident State
  Fabric, or Facility Failure Domain Registry implementation. Timing behaviour
  against a physical plant is therefore unproven here.

## Validation performed

All results below were produced on this host (Windows, MSVC 19.44, x64, CMake
4.3.2) from the repository state described by this version.

* **Release** build: zero warnings with `/W4 /permissive- /WX`; complete suite
  passes (6 binaries, 160 cases).
* **Debug** build: zero warnings, complete suite passes.
* **AddressSanitizer** build (`/fsanitize=address`, MSVC, RelWithDebInfo, tests
  run with the sanitizer runtime on the library path, and confirmed as a
  load-time dependency of the test binaries): complete suite passes with no
  sanitizer report.
* **Real multiprocess proofs**: while a child process holds the store, a second
  independent process is refused with `pfm.store_locked`, and after a hard kill
  of the holder a fresh process opens the store because the kernel released the
  lock. A child killed at six distinct points of a durable commit loop — while
  priming, mid-loop, and around the publication of a slot — always left exactly
  one whole usable generation, which a third, fresh process resolved with
  `fell_back=no` and an identical state digest from two independent verifier
  processes. A staging file flushed by a process that died before the commit
  point never became authoritative.
* **No blind duplicate after a death around dispatch**: a child that died with a
  dispatch in flight left the operation `Indeterminate`; its idempotency key
  never reached a transport again, and an explicit `retry_request` minted a new
  attempt identity carrying the same key.
* **Persistence proofs**: single-byte corruption of header, payload and trailer;
  truncation at eight lengths; appended bytes; unsupported format version;
  non-zero reserved header words; a payload length disagreeing with the file
  length; an unusable newest slot with fallback to the previous complete
  generation and `fell_back` reported; a deliberately divergent live state
  refused as `pfm.replay_divergence`; journal folding with byte-for-byte replay
  afterwards; lock exclusivity; and volatile mode having no durable content.
* **Randomized and property proofs**: three printed seeds × 250 iterations
  (750 generated topologies) plus eight boundary cases (one element, maximum
  fan-out, everything failed, nothing failed, everything unresolved, nothing
  reporting), each mutation checked for scope and ordering invariants and
  compared against an independent reference model of classification, scope
  resolution and all fourteen recovery gates.
* **Adversarial proofs**: reentrant transport callbacks refused with
  `pfm.reentrancy_refused` without deadlocking; four-thread concurrent mutation
  with a replayable store afterwards; `INT64`/`UINT64` extremes; bounds
  exhaustion reported with exact codes.
* **Package proofs**: install into a clean prefix, then configure, build and run
  the independent out-of-tree consumer through `find_package`; it printed
  `PFM_CONSUMER ok=1 plan_generation=3 incident_generation=1
  primary_failure=pfm.failure.utility_feed_loss current_failure=pfm.failure.none
  requests=25 verified=25 lifecycle=stabilizing replay=ok`.
* **Independent cross-check of the store format**: a separately written CRC-32C
  implementation reproduced the header and payload checksums of a slot the
  library wrote, which is how a checksum-range defect was found and fixed.

### Benchmark

`pfm_bench --iterations=500` on the host described above, Release build, 50
warm-up operations discarded, measuring completed operations only:

| Workload | Unit | Completed ops/s | Mean latency |
| --- | --- | --- | --- |
| Decision cycle: classify + plan over 36 current observations of a faulted plant | completed cycle | 6,075.6 | 164.6 us |
| Durable mutation: evidence admission + evaluation + commit | completed operation | 20.2 | 49.4 ms |
| Durable lifecycle: failure, isolation, restoration, recovery, closure | completed lifecycle | 3.6 | 277.1 ms |

The facility model is SYNTHETIC (44 topology elements, site `bench-site`,
switchgear fanout 2, bus fanout 2). The persistence is REAL: the durable figures
include the complete commit path — encode, stage, flush to the device, read
back, and atomically replace the inactive slot — at three durable commits per
completed mutation and forty-five per completed lifecycle. Commit cost grows
with the retained journal and the size of the persisted state, because a commit
publishes a complete snapshot; the retention bound bounds that growth without
eliminating the trade-off.

## Limitations

* No hardware validation exists in this repository. All facility behaviour is
  synthetic, and every request the runtime issues is answered by a modelled
  controller.
* The request owner vocabulary names the adjacent DCCP boundaries; the default
  planner uses a subset of it, and no implementation of those boundaries is
  present here. The status-code vocabulary likewise includes values that the
  current code paths do not return.
* Commit cost grows with the retained journal and the size of the persisted
  state. The retention bound bounds that growth; it does not eliminate the
  trade-off.
* The durable store assumes a local filesystem with atomic same-volume
  replacement. Network filesystems with weaker rename semantics are not covered.
* Durability is verified by device flush plus read-back comparison. No
  power-loss testing of the storage device itself was performed.
* Verification of a requested effect is only as good as the external evidence
  supplied: PFM checks that the evidence is current, independently originated,
  and carries the required proof kind, and never interprets it further.
* Byte-for-byte determinism is proven for the persisted encoding and for replay;
  it is not claimed for text rendering.
* The synthetic plant models electrical state as discrete facts (energization,
  breaker position, generator state, transfer state, reserve, voltage,
  frequency, current) and propagates them along the supplied topology. That is a
  modelling choice for exercising semantics, not a power-flow simulation.
* Recovery requires an operator authorization by default. A deployment that
  turns that policy switch off is supported by the same code path, but the
  authorization gate is then trivially satisfied and the remaining gates carry
  the whole burden.
* Request dispatch is synchronous: an evaluation hands each justified request to
  the transport on the calling thread after its intent is durable. A transport
  that blocks delays that evaluation; it cannot corrupt it, because the answers
  are recorded in a separate committed step and a request whose state moved on
  is skipped.

## License

Apache License 2.0. Copyright 2026 Summon Software Labs. No telemetry transmission.
