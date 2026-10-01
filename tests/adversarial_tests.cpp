// Power Failure Manager -- adversarial proofs.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0 (see LICENSE).
//
// Every case attacks the public API with input the library never sees in a
// normal run: a topology that fights itself, evidence that arrives backwards or
// contradicts itself, requests that are resolved twice, fencing tokens that are
// stale, a transport that calls back into the runtime mid-dispatch, four
// threads mutating one runtime at once, and 64-bit boundary values. A case
// passes only when the API answers with the documented code (or survives with a
// documented one), never by crashing, hanging, or silently accepting the input.
#include <algorithm>
#include <atomic>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <limits>
#include <memory>
#include <string>
#include <system_error>
#include <thread>
#include <vector>

#include "framework.hpp"
#include "support.hpp"

#include "pfm/codec.hpp"
#include "pfm/store.hpp"
#include "pfm/transport.hpp"

namespace pfm = summon::pfm;

namespace {

using Bytes = std::vector<std::uint8_t>;

// --- fixtures and small helpers -------------------------------------------

std::unique_ptr<pfmtest::PlantFixture> open_fixture(
    const std::string& directory, pfm::StoreMode mode,
    const pfm::SyntheticPlantConfig& config = pfm::SyntheticPlantConfig{}) {
  auto created = pfmtest::PlantFixture::make(config, mode, directory);
  if (!created.ok()) {
    pfmtest::report_failure(__FILE__, __LINE__, "the plant fixture could not be opened: " +
                                                    std::string{created.status().to_string()});
    return nullptr;
  }
  return std::move(created.value());
}

pfm::RefToken make_ref(pfm::RefKind kind, const std::string& name) {
  auto made = pfm::RefToken::make(kind, name);
  if (!made.ok()) {
    pfmtest::report_failure(__FILE__, __LINE__,
                            "the reference " + name + " was refused: " +
                                std::string{made.status().to_string()});
    return pfm::RefToken{};
  }
  return made.value();
}

pfm::ElectricalObservation observation_of(const pfm::RefToken& element, pfm::ElementKind kind,
                                          pfm::Timestamp instant,
                                          pfm::ObservationSequence sequence) {
  pfm::ElectricalObservation observation;
  observation.element = element;
  observation.kind = kind;
  observation.origin = pfm::ObservationOrigin::ExternalMeter;
  observation.quality = pfm::EvidenceQuality::Good;
  observation.sequence = sequence;
  observation.observed_at = instant;
  observation.provenance = "adversarial-probe";
  observation.energization = pfm::EnergizationState::Energized;
  observation.breaker = pfm::BreakerPosition::Closed;
  observation.generator = pfm::GeneratorState::Off;
  observation.transfer = pfm::TransferState::OnUtility;
  observation.voltage = pfm::MilliVolts::from_value(230000);
  observation.has_voltage = true;
  observation.frequency = pfm::MilliHertz::from_value(50000);
  observation.has_frequency = true;
  return observation;
}

pfm::TopologyElement topology_element(const pfm::RefToken& ref, pfm::ElementKind kind) {
  pfm::TopologyElement element;
  element.element = ref;
  element.kind = kind;
  element.label = ref.name();
  return element;
}

pfm::TopologySnapshot topology_snapshot(std::vector<pfm::TopologyElement> elements) {
  pfm::TopologySnapshot snapshot;
  snapshot.generation = pfm::TopologyGeneration::from_value(1);
  snapshot.elements = std::move(elements);
  return snapshot;
}

bool contains(const std::vector<pfm::RefToken>& values, const pfm::RefToken& ref) {
  return std::find(values.begin(), values.end(), ref) != values.end();
}

// A code a concurrent or adversarial call may legitimately return. Anything
// outside this set (Internal, StoreCorrupt, StoreIoError, ArithmeticOverflow,
// AuthorityFenced, ReplayDivergence, ...) would be a real defect, so the
// concurrency case fails on it.
bool documented_code(pfm::StatusCode code) {
  switch (code) {
    case pfm::StatusCode::Ok:
    case pfm::StatusCode::InvalidArgument:
    case pfm::StatusCode::EvidenceOutOfOrder:
    case pfm::StatusCode::EvidenceUnknownTarget:
    case pfm::StatusCode::EvidenceNotCurrent:
    case pfm::StatusCode::EvidenceStale:
    case pfm::StatusCode::EvidenceRejected:
    case pfm::StatusCode::StaleRevision:
    case pfm::StatusCode::FutureRevision:
    case pfm::StatusCode::StaleGeneration:
    case pfm::StatusCode::FutureGeneration:
    case pfm::StatusCode::NoActiveIncident:
    case pfm::StatusCode::CrossIncidentAuthority:
    case pfm::StatusCode::ReentrancyRefused:
    case pfm::StatusCode::PreconditionFailed:
    case pfm::StatusCode::PlanNotCurrent:
    case pfm::StatusCode::RequestStateConflict:
    case pfm::StatusCode::RequestNotFound:
    case pfm::StatusCode::AttemptExhausted:
    case pfm::StatusCode::BoundsExceeded:
    case pfm::StatusCode::ResourceExhausted:
      return true;
    default:
      return false;
  }
}

// Evidence that either carries, or deliberately does not carry, the proof a
// request kind demands.
pfm::ElectricalObservation proof_evidence(pfm::ProofKind proof, const pfm::RefToken& element,
                                          pfm::ElementKind kind, pfm::Timestamp instant,
                                          pfm::ObservationSequence sequence,
                                          bool carries_proof) {
  auto observation = observation_of(element, kind, instant, sequence);
  switch (proof) {
    case pfm::ProofKind::None:
      break;
    case pfm::ProofKind::Isolation:
    case pfm::ProofKind::DeEnergization:
      observation.energization = carries_proof ? pfm::EnergizationState::DeEnergized
                                               : pfm::EnergizationState::Energized;
      break;
    case pfm::ProofKind::BreakerOpen:
      observation.breaker =
          carries_proof ? pfm::BreakerPosition::Open : pfm::BreakerPosition::Closed;
      break;
    case pfm::ProofKind::Transfer:
      observation.transfer =
          carries_proof ? pfm::TransferState::OnGenerator : pfm::TransferState::Transferring;
      break;
    case pfm::ProofKind::Synchronization:
      observation.generator =
          carries_proof ? pfm::GeneratorState::Synchronized : pfm::GeneratorState::Running;
      break;
    case pfm::ProofKind::Generation:
      observation.generator =
          carries_proof ? pfm::GeneratorState::Running : pfm::GeneratorState::Off;
      if (!carries_proof) {
        // "Energized" also proves generation, so the un-proving report must not
        // claim it.
        observation.energization = pfm::EnergizationState::DeEnergized;
      }
      break;
    case pfm::ProofKind::Reserve:
      observation.has_reserve = true;
      observation.reserve = pfm::milli_percent_from_value(carries_proof ? 90000 : 1000);
      break;
    case pfm::ProofKind::LoadShed:
      observation.has_current = true;
      observation.current = pfm::MilliAmps::from_value(carries_proof ? 0 : 5000);
      break;
    case pfm::ProofKind::StableSource:
    case pfm::ProofKind::FeedSelected:
      observation.energization = carries_proof ? pfm::EnergizationState::Energized
                                               : pfm::EnergizationState::DeEnergized;
      break;
  }
  return observation;
}

// A runtime driven by the synthetic plant, which answers every dispatch without
// actuating anything: requests stay outstanding, so an attack always has
// something real to attack.
struct RequestRuntime {
  std::unique_ptr<pfm::SyntheticPlant> plant{};
  std::shared_ptr<pfm::ManualClock> clock{};
  std::unique_ptr<pfm::PowerFailureRuntime> runtime{};

  static std::unique_ptr<RequestRuntime> make(pfm::StoreMode mode, const std::string& directory,
                                              const pfm::Bounds& bounds = pfm::Bounds{}) {
    pfm::SyntheticPlantConfig config;
    config.actuate_on_request = false;
    auto made = std::unique_ptr<RequestRuntime>{new RequestRuntime{}};
    made->plant = std::make_unique<pfm::SyntheticPlant>(config);
    made->clock = pfmtest::make_clock();
    pfm::RuntimeOptions options = mode == pfm::StoreMode::Durable
                                      ? pfmtest::durable_options(directory)
                                      : pfmtest::volatile_options();
    options.bounds = bounds;
    auto opened = pfm::PowerFailureRuntime::open(
        options, made->clock,
        std::shared_ptr<pfm::ResponseTransport>{made->plant.get(),
                                               [](pfm::ResponseTransport*) {}});
    if (!opened.ok()) {
      pfmtest::report_failure(__FILE__, __LINE__, "the runtime could not be opened: " +
                                                      std::string{opened.status().to_string()});
      return nullptr;
    }
    made->runtime = std::move(opened.value());
    return made;
  }

  [[nodiscard]] pfm::Result<void> prime(std::size_t feeds_to_lose = 1) {
    auto token = runtime->current_authority();
    if (!token.ok()) {
      return token.status();
    }
    if (auto published = runtime->publish_topology(plant->topology(), token.value());
        !published.ok()) {
      return published;
    }
    token = runtime->current_authority();
    if (!token.ok()) {
      return token.status();
    }
    token.value().incident = pfm::IncidentId::from_value(1);
    token.value().generation = pfm::IncidentGeneration::from_value(1);
    if (auto opened = runtime->open_incident(token.value()); !opened.ok()) {
      return opened;
    }
    const auto feeds = plant->elements_of_kind(pfm::ElementKind::UtilityFeed);
    if (feeds.empty()) {
      return pfm::Status::error(pfm::StatusCode::Internal, "the plant has no utility feed");
    }
    const std::size_t count = feeds_to_lose < feeds.size() ? feeds_to_lose : feeds.size();
    for (std::size_t i = 0; i < count; ++i) {
      plant->lose_utility(feeds[i]);
    }
    return {};
  }

  [[nodiscard]] pfm::Result<pfm::EvaluationOutcome> step() {
    if (auto advanced = clock->advance(pfm::duration_from_millis(1000)); !advanced.ok()) {
      return advanced.status();
    }
    auto now = clock->now();
    if (!now.ok()) {
      return now.status();
    }
    auto token = runtime->current_authority();
    if (!token.ok()) {
      return token.status();
    }
    if (auto admitted = runtime->admit_evidence(plant->observe(now.value()), token.value());
        !admitted.ok()) {
      return admitted.status();
    }
    token = runtime->current_authority();
    if (!token.ok()) {
      return token.status();
    }
    return runtime->evaluate(token.value());
  }
};

bool find_open_request(const std::vector<pfm::ResponseRequest>& requests,
                       pfm::ResponseRequestId skip, pfm::ResponseRequest& out) {
  for (const auto& request : requests) {
    if (!pfm::request_state_is_open(request.state)) {
      continue;
    }
    if (request.id == skip) {
      continue;
    }
    out = request;
    return true;
  }
  return false;
}

// A transport that calls back into the runtime while its own dispatch is in
// flight.
class ReentrantTransport final : public pfm::ResponseTransport {
 public:
  pfm::PowerFailureRuntime* runtime{nullptr};
  pfm::ElectricalObservation observation{};
  std::vector<pfm::StatusCode> mutation_codes{};
  std::vector<pfm::StatusCode> read_codes{};
  std::atomic<int> dispatches{0};

  [[nodiscard]] pfm::Result<pfm::DispatchOutcome> dispatch(
      const pfm::ResponseRequest& request) override {
    static_cast<void>(request);
    dispatches.fetch_add(1);
    if (runtime != nullptr) {
      auto token = runtime->current_authority();
      if (token.ok()) {
        mutation_codes.push_back(runtime->admit_evidence(observation, token.value()).code());
        mutation_codes.push_back(runtime->evaluate(token.value()).code());
        mutation_codes.push_back(
            runtime
                ->roll_epoch(pfm::ControlEpoch::from_value(9),
                             pfm::ControllerIncarnation::from_value(9))
                .code());
      }
      auto status = runtime->status();
      read_codes.push_back(status.ok() ? pfm::StatusCode::Ok : status.code());
      auto state = runtime->state();
      read_codes.push_back(state.ok() ? pfm::StatusCode::Ok : state.code());
    }
    return pfm::DispatchOutcome::accepted(pfm::RefToken{}, "reentrant transport answered");
  }
};

}  // namespace

// ---------------------------------------------------------------------------
// Evidence intake.
// ---------------------------------------------------------------------------
PFM_TEST(evidence_attacks_are_refused_with_exact_codes) {
  pfmtest::TempDirectory directory("adv-evidence");
  auto fixture = open_fixture(directory.path(), pfm::StoreMode::Volatile);
  PFM_REQUIRE(fixture != nullptr);
  PFM_CHECK_OK(fixture->prime());

  auto now = fixture->clock->now();
  PFM_REQUIRE(now.ok());
  auto buses = fixture->plant->elements_of_kind(pfm::ElementKind::Bus);
  PFM_REQUIRE(!buses.empty());
  const auto bus = buses.front();

  auto current = fixture->runtime->current_authority();
  PFM_REQUIRE(current.ok());

  // An element the published topology does not hold.
  const auto ghost_ref = make_ref(pfm::RefKind::Bus, "bus-ghost");
  auto ghost = observation_of(ghost_ref, pfm::ElementKind::Bus, now.value(),
                              pfm::ObservationSequence::from_value(9001));
  PFM_CHECK_CODE(fixture->runtime->admit_evidence(ghost, current.value()),
                 pfm::StatusCode::EvidenceUnknownTarget);

  // A reference kind that does not match the element kind.
  auto mismatched = observation_of(bus, pfm::ElementKind::UpsUnit, now.value(),
                                   pfm::ObservationSequence::from_value(9002));
  PFM_CHECK_CODE(fixture->runtime->admit_evidence(mismatched, current.value()),
                 pfm::StatusCode::EvidenceUnknownTarget);

  // An observation with no attributable origin.
  auto unset_origin = observation_of(bus, pfm::ElementKind::Bus, now.value(),
                                     pfm::ObservationSequence::from_value(9003));
  unset_origin.origin = pfm::ObservationOrigin::Unknown;
  PFM_CHECK_CODE(fixture->runtime->admit_evidence(unset_origin, current.value()),
                 pfm::StatusCode::EvidenceOriginUntrusted);

  // An observation with no instant.
  auto no_instant = observation_of(bus, pfm::ElementKind::Bus, pfm::Timestamp{},
                                   pfm::ObservationSequence::from_value(9004));
  PFM_CHECK_CODE(fixture->runtime->admit_evidence(no_instant, current.value()),
                 pfm::StatusCode::EvidenceRejected);

  // An absurd provenance length.
  auto long_provenance = observation_of(bus, pfm::ElementKind::Bus, now.value(),
                                        pfm::ObservationSequence::from_value(9005));
  long_provenance.provenance.assign(129, 'p');  // the default bound is 128
  PFM_CHECK_CODE(fixture->runtime->admit_evidence(long_provenance, current.value()),
                 pfm::StatusCode::FieldTooLong);

  // A reserve above full scale, and one below zero.
  auto over_reserve = observation_of(bus, pfm::ElementKind::Bus, now.value(),
                                     pfm::ObservationSequence::from_value(9006));
  over_reserve.has_reserve = true;
  over_reserve.reserve = pfm::milli_percent_from_value(100001);
  PFM_CHECK_CODE(fixture->runtime->admit_evidence(over_reserve, current.value()),
                 pfm::StatusCode::EvidenceRejected);
  auto negative_reserve = over_reserve;
  negative_reserve.reserve = pfm::milli_percent_from_value(-1);
  PFM_CHECK_CODE(fixture->runtime->admit_evidence(negative_reserve, current.value()),
                 pfm::StatusCode::EvidenceRejected);

  // Negative voltage, frequency, and current.
  auto negative_voltage = observation_of(bus, pfm::ElementKind::Bus, now.value(),
                                         pfm::ObservationSequence::from_value(9007));
  negative_voltage.voltage = pfm::MilliVolts::from_value(-1);
  PFM_CHECK_CODE(fixture->runtime->admit_evidence(negative_voltage, current.value()),
                 pfm::StatusCode::EvidenceRejected);
  auto negative_frequency = negative_voltage;
  negative_frequency.voltage = pfm::MilliVolts::from_value(230000);
  negative_frequency.frequency = pfm::MilliHertz::from_value(-1);
  PFM_CHECK_CODE(fixture->runtime->admit_evidence(negative_frequency, current.value()),
                 pfm::StatusCode::EvidenceRejected);
  auto negative_current = negative_voltage;
  negative_current.has_current = true;
  negative_current.current = pfm::MilliAmps::from_value(-1);
  PFM_CHECK_CODE(fixture->runtime->admit_evidence(negative_current, current.value()),
                 pfm::StatusCode::EvidenceRejected);

  // The exact boundaries are accepted: the attacks above are the values just
  // outside them, not the values themselves.
  auto full_reserve = observation_of(bus, pfm::ElementKind::Bus, now.value(),
                                     pfm::ObservationSequence::from_value(9010));
  full_reserve.has_reserve = true;
  full_reserve.reserve = pfm::milli_percent_from_value(100000);
  current = fixture->runtime->current_authority();
  PFM_REQUIRE(current.ok());
  PFM_CHECK_OK(fixture->runtime->admit_evidence(full_reserve, current.value()));
  auto zero_readings = observation_of(bus, pfm::ElementKind::Bus, now.value(),
                                      pfm::ObservationSequence::from_value(9011));
  zero_readings.voltage = pfm::MilliVolts::from_value(0);
  zero_readings.frequency = pfm::MilliHertz::from_value(0);
  zero_readings.current = pfm::MilliAmps::from_value(0);
  zero_readings.has_current = true;
  zero_readings.reserve = pfm::milli_percent_from_value(0);
  zero_readings.has_reserve = true;
  current = fixture->runtime->current_authority();
  PFM_REQUIRE(current.ok());
  PFM_CHECK_OK(fixture->runtime->admit_evidence(zero_readings, current.value()));
}

PFM_TEST(evidence_order_duplicates_and_contradictions_are_explicit) {
  pfmtest::TempDirectory directory("adv-evidence-order");
  auto fixture = open_fixture(directory.path(), pfm::StoreMode::Volatile);
  PFM_REQUIRE(fixture != nullptr);
  PFM_CHECK_OK(fixture->prime());
  auto now = fixture->clock->now();
  PFM_REQUIRE(now.ok());
  auto buses = fixture->plant->elements_of_kind(pfm::ElementKind::Bus);
  PFM_REQUIRE(buses.size() >= 2);
  const auto first_bus = buses[0];
  const auto second_bus = buses[1];

  // An identical redelivery is not a contradiction.
  auto first = observation_of(first_bus, pfm::ElementKind::Bus, now.value(),
                              pfm::ObservationSequence::from_value(100));
  auto current = fixture->runtime->current_authority();
  PFM_REQUIRE(current.ok());
  PFM_CHECK_OK(fixture->runtime->admit_evidence(first, current.value()));
  current = fixture->runtime->current_authority();
  PFM_REQUIRE(current.ok());
  PFM_CHECK_OK(fixture->runtime->admit_evidence(first, current.value()));
  auto state = fixture->runtime->state();
  PFM_REQUIRE(state.ok());
  const auto* slot = state.value().find_observation(first_bus);
  PFM_REQUIRE(slot != nullptr);
  PFM_CHECK(slot->present);
  PFM_CHECK_MSG(!slot->contradicted, "an identical redelivery was treated as a contradiction");
  PFM_CHECK(slot->conflict_evidence.empty());

  // The same origin and sequence reporting different facts is still a duplicate
  // delivery, not a contradiction: the newer content simply replaces the older.
  auto overwrite = first;
  overwrite.energization = pfm::EnergizationState::DeEnergized;
  overwrite.breaker = pfm::BreakerPosition::Open;
  current = fixture->runtime->current_authority();
  PFM_REQUIRE(current.ok());
  PFM_CHECK_OK(fixture->runtime->admit_evidence(overwrite, current.value()));
  state = fixture->runtime->state();
  PFM_REQUIRE(state.ok());
  slot = state.value().find_observation(first_bus);
  PFM_REQUIRE(slot != nullptr);
  PFM_CHECK_MSG(!slot->contradicted, "a same-sequence redelivery was treated as a contradiction");
  PFM_CHECK_EQ(slot->observation.energization, pfm::EnergizationState::DeEnergized);

  // A report whose sequence is behind the retained one is refused.
  auto older = observation_of(first_bus, pfm::ElementKind::Bus, now.value(),
                              pfm::ObservationSequence::from_value(99));
  current = fixture->runtime->current_authority();
  PFM_REQUIRE(current.ok());
  PFM_CHECK_CODE(fixture->runtime->admit_evidence(older, current.value()),
                 pfm::StatusCode::EvidenceOutOfOrder);
  {
    auto unchanged = fixture->runtime->state();
    PFM_REQUIRE(unchanged.ok());
    const auto* retained = unchanged.value().find_observation(first_bus);
    PFM_REQUIRE(retained != nullptr);
    PFM_CHECK_EQ(retained->observation.sequence.value(), std::uint64_t{100});
  }

  // Two contradictory reports of one element inside a single batch: the batch
  // is accepted as evidence and the element becomes contradicted, never
  // current, and never healthy.
  auto alive = observation_of(second_bus, pfm::ElementKind::Bus, now.value(),
                              pfm::ObservationSequence::from_value(200));
  auto dead = observation_of(second_bus, pfm::ElementKind::Bus, now.value(),
                             pfm::ObservationSequence::from_value(201));
  dead.origin = pfm::ObservationOrigin::ExternalController;
  dead.energization = pfm::EnergizationState::DeEnergized;
  dead.breaker = pfm::BreakerPosition::Open;
  current = fixture->runtime->current_authority();
  PFM_REQUIRE(current.ok());
  PFM_CHECK_OK(fixture->runtime->admit_evidence({alive, dead}, current.value()));
  state = fixture->runtime->state();
  PFM_REQUIRE(state.ok());
  const auto* conflicted = state.value().find_observation(second_bus);
  PFM_REQUIRE(conflicted != nullptr);
  PFM_CHECK_MSG(conflicted->contradicted, "contradictory reports were not flagged");
  PFM_CHECK(!conflicted->conflict_evidence.empty());

  const auto current_observations =
      pfm::current_observations(state.value(), now.value(), state.value().policy);
  PFM_CHECK_MSG(std::none_of(current_observations.begin(), current_observations.end(),
                             [&second_bus](const pfm::ElectricalObservation& observation) {
                               return observation.element == second_bus;
                             }),
                "a contradicted element was treated as current");

  current = fixture->runtime->current_authority();
  PFM_REQUIRE(current.ok());
  auto outcome = fixture->runtime->evaluate(current.value());
  PFM_CHECK_OK(outcome);
  auto classification = fixture->runtime->classification();
  PFM_REQUIRE(classification.ok());
  PFM_CHECK_MSG(contains(classification.value().unresolved_elements, second_bus),
                "the contradicted element was not listed as unresolved");
  // The published plan must account for the contradicted element: either as an
  // ambiguous failure or as an element inside the boundary with no current
  // evidence. It is never silently dropped, and never read as health.
  auto published = fixture->runtime->plan();
  PFM_REQUIRE(published.ok());
  bool reported_ambiguous = false;
  for (const auto& failure : published.value().failures) {
    if (failure.element == second_bus && failure.klass == pfm::FailureClass::AmbiguousElectricalEvidence) {
      reported_ambiguous = true;
    }
  }
  PFM_CHECK_MSG(reported_ambiguous ||
                    contains(published.value().scope.unevidenced_elements, second_bus),
                "the contradicted element vanished from the published plan");
}

// ---------------------------------------------------------------------------
// Topology intake.
// ---------------------------------------------------------------------------
PFM_TEST(topology_attacks_are_refused_whole_with_exact_codes) {
  pfmtest::TempDirectory directory("adv-topology");
  auto fixture = open_fixture(directory.path(), pfm::StoreMode::Volatile);
  PFM_REQUIRE(fixture != nullptr);
  auto token = fixture->runtime->current_authority();
  PFM_REQUIRE(token.ok());

  const auto bus_a = make_ref(pfm::RefKind::Bus, "bus-a");
  const auto bus_b = make_ref(pfm::RefKind::Bus, "bus-b");
  const auto bus_c = make_ref(pfm::RefKind::Bus, "bus-c");
  const auto breaker_a = make_ref(pfm::RefKind::Breaker, "brk-a");
  const auto domain_a = make_ref(pfm::RefKind::FailureDomain, "fd-a");
  const auto domain_b = make_ref(pfm::RefKind::FailureDomain, "fd-b");
  const auto load_group = make_ref(pfm::RefKind::LoadGroup, "lg-a");

  // Control: a well-formed snapshot is accepted, so every refusal below is a
  // refusal of the attack and not of the shape of a legal snapshot.
  {
    auto snapshot = topology_snapshot({topology_element(bus_a, pfm::ElementKind::Bus)});
    PFM_CHECK_OK(fixture->runtime->publish_topology(snapshot, token.value()));
    token = fixture->runtime->current_authority();
    PFM_REQUIRE(token.ok());
  }

  // An upstream cycle.
  {
    auto a = topology_element(bus_a, pfm::ElementKind::Bus);
    a.upstream = bus_b;
    auto b = topology_element(bus_b, pfm::ElementKind::Bus);
    b.upstream = bus_a;
    PFM_CHECK_CODE(fixture->runtime->publish_topology(topology_snapshot({a, b}), token.value()),
                   pfm::StatusCode::TopologyCycle);
  }
  // A self-supplying element.
  {
    auto a = topology_element(bus_a, pfm::ElementKind::Bus);
    a.upstream = bus_a;
    PFM_CHECK_CODE(fixture->runtime->publish_topology(topology_snapshot({a}), token.value()),
                   pfm::StatusCode::TopologyCycle);
  }
  // A duplicate element.
  {
    auto a = topology_element(bus_a, pfm::ElementKind::Bus);
    auto duplicate = a;
    PFM_CHECK_CODE(
        fixture->runtime->publish_topology(topology_snapshot({a, duplicate}), token.value()),
        pfm::StatusCode::DuplicateElement);
  }
  // An isolation point that is not a switching device.
  {
    auto a = topology_element(bus_a, pfm::ElementKind::Bus);
    a.has_isolation_point = true;
    a.isolation_point = bus_b;
    auto b = topology_element(bus_b, pfm::ElementKind::Bus);
    PFM_CHECK_CODE(fixture->runtime->publish_topology(topology_snapshot({a, b}), token.value()),
                   pfm::StatusCode::InvalidArgument);
  }
  // An isolation point that is not in the snapshot at all.
  {
    auto a = topology_element(bus_a, pfm::ElementKind::Bus);
    a.has_isolation_point = true;
    a.isolation_point = breaker_a;
    PFM_CHECK_CODE(fixture->runtime->publish_topology(topology_snapshot({a}), token.value()),
                   pfm::StatusCode::TopologyUnknownElement);
  }
  // An isolation point flag without a reference.
  {
    auto a = topology_element(bus_a, pfm::ElementKind::Bus);
    a.has_isolation_point = true;
    PFM_CHECK_CODE(fixture->runtime->publish_topology(topology_snapshot({a}), token.value()),
                   pfm::StatusCode::InvalidArgument);
  }
  // An unknown upstream.
  {
    auto a = topology_element(bus_a, pfm::ElementKind::Bus);
    a.upstream = bus_c;
    PFM_CHECK_CODE(fixture->runtime->publish_topology(topology_snapshot({a}), token.value()),
                   pfm::StatusCode::TopologyUnknownElement);
  }
  // An unknown failure domain.
  {
    auto a = topology_element(bus_a, pfm::ElementKind::Bus);
    a.domain = domain_a;
    PFM_CHECK_CODE(fixture->runtime->publish_topology(topology_snapshot({a}), token.value()),
                   pfm::StatusCode::FailureDomainUnknown);
  }
  // A failure domain naming an element the snapshot does not hold.
  {
    auto a = topology_element(bus_a, pfm::ElementKind::Bus);
    pfm::FailureDomain domain;
    domain.domain = domain_a;
    domain.members = {bus_c};
    auto snapshot = topology_snapshot({a});
    snapshot.domains = {domain};
    PFM_CHECK_CODE(fixture->runtime->publish_topology(snapshot, token.value()),
                   pfm::StatusCode::TopologyUnknownElement);
  }
  // A duplicated failure domain.
  {
    auto a = topology_element(bus_a, pfm::ElementKind::Bus);
    auto b = topology_element(bus_b, pfm::ElementKind::Bus);
    pfm::FailureDomain first;
    first.domain = domain_a;
    first.members = {bus_a};
    pfm::FailureDomain second;
    second.domain = domain_a;
    second.members = {bus_b};
    auto snapshot = topology_snapshot({a, b});
    snapshot.domains = {first, second};
    PFM_CHECK_CODE(fixture->runtime->publish_topology(snapshot, token.value()),
                   pfm::StatusCode::DuplicateElement);
  }
  // A failure-domain reference that is not a failure domain.
  {
    pfm::FailureDomain domain;
    domain.domain = load_group;
    auto snapshot = topology_snapshot({topology_element(bus_a, pfm::ElementKind::Bus)});
    snapshot.domains = {domain};
    PFM_CHECK_CODE(fixture->runtime->publish_topology(snapshot, token.value()),
                   pfm::StatusCode::InvalidArgument);
  }
  // An absurd label.
  {
    auto a = topology_element(bus_a, pfm::ElementKind::Bus);
    a.label.assign(129, 'x');  // the default bound is 128
    PFM_CHECK_CODE(fixture->runtime->publish_topology(topology_snapshot({a}), token.value()),
                   pfm::StatusCode::FieldTooLong);
  }
  // No generation at all.
  {
    auto snapshot = topology_snapshot({topology_element(bus_a, pfm::ElementKind::Bus)});
    snapshot.generation = pfm::TopologyGeneration{};
    PFM_CHECK_CODE(fixture->runtime->publish_topology(snapshot, token.value()),
                   pfm::StatusCode::InvalidArgument);
  }
  // No identity at all.
  {
    auto a = topology_element(bus_a, pfm::ElementKind::Bus);
    a.element = pfm::RefToken{};
    PFM_CHECK_CODE(fixture->runtime->publish_topology(topology_snapshot({a}), token.value()),
                   pfm::StatusCode::InvalidArgument);
  }
  // A snapshot beyond the configured bounds.
  {
    pfm::RuntimeOptions options = pfmtest::volatile_options();
    options.bounds.max_topology_elements = 2;
    auto clock = pfmtest::make_clock();
    auto bounded = pfm::PowerFailureRuntime::open(options, clock, nullptr);
    PFM_REQUIRE(bounded.ok());
    auto bounded_token = bounded.value()->current_authority();
    PFM_REQUIRE(bounded_token.ok());
    auto snapshot = topology_snapshot({topology_element(bus_a, pfm::ElementKind::Bus),
                                       topology_element(bus_b, pfm::ElementKind::Bus),
                                       topology_element(bus_c, pfm::ElementKind::Bus)});
    PFM_CHECK_CODE(bounded.value()->publish_topology(snapshot, bounded_token.value()),
                   pfm::StatusCode::TopologyBoundsExceeded);
    // The same snapshot is a legal topology once the bound allows it.
    options.bounds.max_topology_elements = 3;
    auto permissive = pfm::PowerFailureRuntime::open(options, clock, nullptr);
    PFM_REQUIRE(permissive.ok());
    auto permissive_token = permissive.value()->current_authority();
    PFM_REQUIRE(permissive_token.ok());
    PFM_CHECK_OK(permissive.value()->publish_topology(snapshot, permissive_token.value()));
  }
  // The published topology is still the last accepted one.
  {
    auto state = fixture->runtime->state();
    PFM_REQUIRE(state.ok());
    PFM_CHECK(state.value().has_topology);
    PFM_CHECK_EQ(state.value().topology.elements.size(), std::size_t{1});
    PFM_CHECK(state.value().topology.elements.front().element == bus_a);
  }
}

// ---------------------------------------------------------------------------
// Request lifecycle.
// ---------------------------------------------------------------------------
PFM_TEST(request_attacks_are_refused_with_exact_codes) {
  pfmtest::TempDirectory directory("adv-request");
  auto fixture = RequestRuntime::make(pfm::StoreMode::Volatile, directory.path());
  PFM_REQUIRE(fixture != nullptr);
  PFM_CHECK_OK(fixture->prime());
  auto outcome = fixture->step();
  PFM_REQUIRE(outcome.ok());
  PFM_REQUIRE(outcome.value().requests_planned > 0);

  auto requests = fixture->runtime->requests();
  PFM_REQUIRE(requests.ok());
  const pfm::ResponseRequest* target = nullptr;
  for (const auto& request : requests.value()) {
    if (pfm::request_state_is_open(request.state) &&
        request.required_proof != pfm::ProofKind::None) {
      target = &request;
      break;
    }
  }
  if (target == nullptr) {
    pfmtest::report_failure(__FILE__, __LINE__,
                            "no outstanding request required a proof to be attacked");
    return;
  }
  const auto target_id = target->id;
  const auto required = target->required_proof;
  const auto target_element = target->target.element;
  const auto target_kind = target->target.kind;

  auto now = fixture->clock->now();
  PFM_REQUIRE(now.ok());
  pfm::ObservationSequence sequence = pfm::ObservationSequence::from_value(5000);

  // Evidence for a different element.
  pfm::RefToken other_element{};
  pfm::ElementKind other_kind = pfm::ElementKind::Unknown;
  for (const auto& element : fixture->plant->topology().elements) {
    if (!(element.element == target_element)) {
      other_element = element.element;
      other_kind = element.kind;
      break;
    }
  }
  PFM_REQUIRE(other_element.is_set());
  auto token = fixture->runtime->current_authority();
  PFM_REQUIRE(token.ok());
  auto elsewhere = observation_of(other_element, other_kind, now.value(), sequence);
  PFM_CHECK_CODE(fixture->runtime->record_effect(target_id, elsewhere, token.value()),
                 pfm::StatusCode::RequestTargetMismatch);

  // Evidence that is not current.
  sequence = pfm::ObservationSequence::from_value(sequence.value() + 1);
  auto expired = proof_evidence(required, target_element, target_kind,
                                pfm::Timestamp::from_value(now.value().value() - 300000), sequence,
                                true);
  PFM_CHECK_CODE(fixture->runtime->record_effect(target_id, expired, token.value()),
                 pfm::StatusCode::EvidenceNotCurrent);

  // Evidence that does not carry the required proof: the request reaches
  // Observed and never Verified.
  sequence = pfm::ObservationSequence::from_value(sequence.value() + 1);
  auto unproving = proof_evidence(required, target_element, target_kind, now.value(), sequence,
                                  false);
  PFM_CHECK_OK(fixture->runtime->record_effect(target_id, unproving, token.value()));
  auto observed = fixture->runtime->request(target_id);
  PFM_REQUIRE(observed.ok());
  PFM_CHECK_MSG(observed.value().state == pfm::RequestState::Observed,
                "an un-proving report did not leave the request Observed");
  PFM_CHECK_MSG(observed.value().state != pfm::RequestState::Verified,
                "an un-proving report verified the request");
  PFM_CHECK(!observed.value().has_verified_at);
  PFM_CHECK(observed.value().has_observed_at);

  // The proof it actually asked for does verify it.
  sequence = pfm::ObservationSequence::from_value(sequence.value() + 1);
  auto proving = proof_evidence(required, target_element, target_kind, now.value(), sequence, true);
  token = fixture->runtime->current_authority();
  PFM_REQUIRE(token.ok());
  PFM_CHECK_OK(fixture->runtime->record_effect(target_id, proving, token.value()));
  auto verified = fixture->runtime->request(target_id);
  PFM_REQUIRE(verified.ok());
  PFM_CHECK_EQ(verified.value().state, pfm::RequestState::Verified);
  PFM_CHECK(verified.value().has_verified_at);
  PFM_CHECK(verified.value().verification_source.is_set());

  // Resolving a request that is not indeterminate.
  pfm::ResponseRequest second;
  auto refreshed = fixture->runtime->requests();
  PFM_REQUIRE(refreshed.ok());
  if (find_open_request(refreshed.value(), target_id, second)) {
    token = fixture->runtime->current_authority();
    PFM_REQUIRE(token.ok());
    PFM_CHECK_CODE(fixture->runtime->resolve_indeterminate(second.id, false,
                                                           pfm::ElectricalObservation{},
                                                           token.value()),
                   pfm::StatusCode::RequestStateConflict);

    // Retrying a terminal request.
    PFM_CHECK_OK(fixture->runtime->fail_request(second.id, pfm::ReasonCode::AttemptFailed,
                                                "adversarial", token.value()));
    auto failed = fixture->runtime->request(second.id);
    PFM_REQUIRE(failed.ok());
    PFM_CHECK_EQ(failed.value().state, pfm::RequestState::Failed);
    PFM_CHECK(pfm::request_state_is_terminal(failed.value().state));
    token = fixture->runtime->current_authority();
    PFM_REQUIRE(token.ok());
    PFM_CHECK_CODE(fixture->runtime->retry_request(second.id, token.value()),
                   pfm::StatusCode::RequestStateConflict);
  } else {
    pfmtest::report_failure(__FILE__, __LINE__, "no second outstanding request to attack");
  }

  // Unknown request identities.
  token = fixture->runtime->current_authority();
  PFM_REQUIRE(token.ok());
  const auto unknown_id = pfm::ResponseRequestId::from_value(999999);
  PFM_CHECK_CODE(fixture->runtime->request(unknown_id), pfm::StatusCode::RequestNotFound);
  PFM_CHECK_CODE(fixture->runtime->record_effect(unknown_id, proving, token.value()),
                 pfm::StatusCode::RequestNotFound);
  PFM_CHECK_CODE(fixture->runtime->acknowledge_request(unknown_id, {}, "late", token.value()),
                 pfm::StatusCode::RequestNotFound);
  PFM_CHECK_CODE(fixture->runtime->fail_request(unknown_id, pfm::ReasonCode::AttemptFailed, {},
                                                token.value()),
                 pfm::StatusCode::RequestNotFound);
  PFM_CHECK_CODE(fixture->runtime->cancel_request(unknown_id, pfm::ReasonCode::AttemptFailed,
                                                  token.value()),
                 pfm::StatusCode::RequestNotFound);
  PFM_CHECK_CODE(fixture->runtime->resolve_indeterminate(unknown_id, false, {}, token.value()),
                 pfm::StatusCode::RequestNotFound);
  PFM_CHECK_CODE(fixture->runtime->retry_request(unknown_id, token.value()),
                 pfm::StatusCode::RequestNotFound);
}

PFM_TEST(retrying_past_the_attempt_bound_is_exhausted) {
  pfmtest::TempDirectory directory("adv-attempts");
  auto fixture = RequestRuntime::make(pfm::StoreMode::Volatile, directory.path());
  PFM_REQUIRE(fixture != nullptr);
  PFM_CHECK_OK(fixture->prime());
  auto outcome = fixture->step();
  PFM_REQUIRE(outcome.ok());

  auto requests = fixture->runtime->requests();
  PFM_REQUIRE(requests.ok());
  pfm::ResponseRequest current;
  PFM_REQUIRE(find_open_request(requests.value(), pfm::ResponseRequestId{}, current));
  const auto fingerprint = current.fingerprint;
  const auto bound = fixture->runtime->state();
  PFM_REQUIRE(bound.ok());
  const auto max_attempts = bound.value().policy.max_attempts_per_request;
  PFM_REQUIRE(max_attempts > 1);

  std::size_t retries = 0;
  bool exhausted = false;
  for (std::size_t i = 0; i < 8; ++i) {
    auto token = fixture->runtime->current_authority();
    PFM_REQUIRE(token.ok());
    auto retried = fixture->runtime->retry_request(current.id, token.value());
    if (!retried.ok()) {
      PFM_CHECK_MSG(retried.status().code() == pfm::StatusCode::AttemptExhausted,
                    "retrying past the bound produced " +
                        std::string{retried.status().to_string()});
      exhausted = true;
      break;
    }
    retries += 1;
    auto refreshed = fixture->runtime->requests();
    PFM_REQUIRE(refreshed.ok());
    pfm::ResponseRequest replacement;
    bool found = false;
    for (const auto& request : refreshed.value()) {
      if (request.fingerprint == fingerprint && !(request.id == current.id) &&
          request.state == pfm::RequestState::Planned) {
        replacement = request;
        found = true;
      }
    }
    if (!found) {
      pfmtest::report_failure(__FILE__, __LINE__,
                              "the retry did not publish a replacement attempt");
      break;
    }
    current = replacement;
    PFM_CHECK_EQ(current.attempt_ordinal, static_cast<std::uint32_t>(retries + 1));
  }
  PFM_CHECK_MSG(exhausted, "the attempt bound was never reached");
  PFM_CHECK_EQ(retries, static_cast<std::size_t>(max_attempts - 1));
}

// ---------------------------------------------------------------------------
// Authority fencing.
// ---------------------------------------------------------------------------
PFM_TEST(authority_fencing_reports_every_code_exactly) {
  pfmtest::TempDirectory directory("adv-authority");
  auto fixture = open_fixture(directory.path(), pfm::StoreMode::Durable);
  PFM_REQUIRE(fixture != nullptr);

  auto now = fixture->clock->now();
  PFM_REQUIRE(now.ok());
  const auto free_reference = make_ref(pfm::RefKind::Bus, "bus-free");
  auto free_observation = observation_of(free_reference, pfm::ElementKind::Bus, now.value(),
                                         pfm::ObservationSequence::from_value(1));

  // An incident-bound operation with no live incident at all.
  {
    auto token = fixture->runtime->current_authority();
    PFM_REQUIRE(token.ok());
    token.value().incident = pfm::IncidentId::from_value(1);
    token.value().generation = pfm::IncidentGeneration::from_value(1);
    PFM_CHECK_CODE(fixture->runtime->admit_evidence(free_observation, token.value()),
                   pfm::StatusCode::NoActiveIncident);
  }

  PFM_CHECK_OK(fixture->prime());
  const auto topology = fixture->plant->topology();
  auto buses = fixture->plant->elements_of_kind(pfm::ElementKind::Bus);
  PFM_REQUIRE(!buses.empty());
  auto observation = observation_of(buses.front(), pfm::ElementKind::Bus, now.value(),
                                    pfm::ObservationSequence::from_value(10));

  // Control: the current token is accepted.
  {
    auto token = fixture->runtime->current_authority();
    PFM_REQUIRE(token.ok());
    PFM_CHECK_OK(fixture->runtime->publish_topology(topology, token.value()));
  }

  const auto fresh = [&fixture]() {
    auto token = fixture->runtime->current_authority();
    if (!token.ok()) {
      pfmtest::report_failure(__FILE__, __LINE__,
                              "the runtime has no authority: " +
                                  std::string{token.status().to_string()});
    }
    return token.value_or(pfm::AuthorityToken{});
  };

  // MissingAuthority: no epoch, or no incarnation.
  {
    auto token = fresh();
    token.epoch = pfm::ControlEpoch::from_value(0);
    PFM_CHECK_CODE(fixture->runtime->publish_topology(topology, token),
                   pfm::StatusCode::MissingAuthority);
    token = fresh();
    token.incarnation = pfm::ControllerIncarnation::from_value(0);
    PFM_CHECK_CODE(fixture->runtime->publish_topology(topology, token),
                   pfm::StatusCode::MissingAuthority);
  }
  // FutureEpoch: ahead of the durable epoch, without rolling it.
  {
    auto token = fresh();
    token.epoch = pfm::ControlEpoch::from_value(token.epoch.value() + 1);
    PFM_CHECK_CODE(fixture->runtime->publish_topology(topology, token),
                   pfm::StatusCode::FutureEpoch);
  }
  // StaleIncarnation: the same epoch under a different controller identity.
  {
    auto token = fresh();
    token.incarnation = pfm::ControllerIncarnation::from_value(token.incarnation.value() + 7);
    PFM_CHECK_CODE(fixture->runtime->publish_topology(topology, token),
                   pfm::StatusCode::StaleIncarnation);
  }
  // CrossIncidentAuthority: a token bound to another incident.
  {
    auto token = fresh();
    token.incident = pfm::IncidentId::from_value(token.incident.value() + 5);
    PFM_CHECK_CODE(fixture->runtime->admit_evidence(observation, token),
                   pfm::StatusCode::CrossIncidentAuthority);
  }
  // StaleGeneration and FutureGeneration.
  {
    auto token = fresh();
    token.generation = pfm::IncidentGeneration::from_value(token.generation.value() - 1);
    PFM_CHECK_CODE(fixture->runtime->admit_evidence(observation, token),
                   pfm::StatusCode::StaleGeneration);
    token = fresh();
    token.generation = pfm::IncidentGeneration::from_value(token.generation.value() + 1);
    PFM_CHECK_CODE(fixture->runtime->admit_evidence(observation, token),
                   pfm::StatusCode::FutureGeneration);
  }
  // StaleRevision and FutureRevision.
  {
    auto token = fresh();
    token.revision = pfm::StateRevision::from_value(token.revision.value() - 1);
    PFM_CHECK_CODE(fixture->runtime->admit_evidence(observation, token),
                   pfm::StatusCode::StaleRevision);
    token = fresh();
    token.revision = pfm::StateRevision::from_value(token.revision.value() + 1);
    PFM_CHECK_CODE(fixture->runtime->admit_evidence(observation, token),
                   pfm::StatusCode::FutureRevision);
  }
  // An incident generation without an incident identity is meaningless.
  {
    auto token = fresh();
    token.incident = pfm::IncidentId{};
    token.generation = pfm::IncidentGeneration::from_value(1);
    PFM_CHECK_CODE(fixture->runtime->publish_topology(topology, token),
                   pfm::StatusCode::InvalidArgument);
  }

  // A token issued before a rollover is fenced as a stale epoch, and the new
  // authority works.
  auto before_rollover = fresh();
  const auto old_epoch = before_rollover.epoch.value();
  PFM_CHECK_OK(fixture->runtime->roll_epoch(pfm::ControlEpoch::from_value(old_epoch + 1),
                                            pfm::ControllerIncarnation::from_value(1)));
  PFM_CHECK_CODE(fixture->runtime->admit_evidence(observation, before_rollover),
                 pfm::StatusCode::StaleEpoch);
  {
    auto token = fresh();
    PFM_CHECK_EQ(token.epoch.value(), old_epoch + 1);
    auto later = observation_of(buses.front(), pfm::ElementKind::Bus, now.value(),
                                pfm::ObservationSequence::from_value(20));
    PFM_CHECK_OK(fixture->runtime->admit_evidence(later, token));
  }
}

PFM_TEST(a_failed_durable_commit_fences_the_runtime_instead_of_continuing) {
  pfmtest::TempDirectory directory("adv-fenced");
  auto fixture = open_fixture(directory.path(), pfm::StoreMode::Durable);
  PFM_REQUIRE(fixture != nullptr);

  // Block both staging paths with a non-empty directory: the store cannot
  // create the file it stages a commit in, which is a durable write failure.
  for (unsigned index = 0; index < 2; ++index) {
    const auto path = (std::filesystem::path{directory.path()} /
                       ("state." + std::to_string(index) + ".staging"))
                          .string();
    std::error_code code;
    std::filesystem::create_directory(path, code);
    PFM_REQUIRE(!code);
    PFM_CHECK(std::filesystem::is_directory(path));
    const auto occupant = (std::filesystem::path{path} / "occupied").string();
    std::ofstream occupant_stream{occupant, std::ios::binary};
    occupant_stream << "x";
    occupant_stream.close();
    PFM_CHECK(std::filesystem::exists(occupant));
  }

  auto now = fixture->clock->now();
  PFM_REQUIRE(now.ok());
  const auto reference = make_ref(pfm::RefKind::Bus, "bus-fenced");
  auto observation = observation_of(reference, pfm::ElementKind::Bus, now.value(),
                                    pfm::ObservationSequence::from_value(1));
  auto token = fixture->runtime->current_authority();
  PFM_REQUIRE(token.ok());
  PFM_CHECK_CODE(fixture->runtime->admit_evidence(observation, token.value()),
                 pfm::StatusCode::StoreIoError);

  // Every later mutation is refused rather than silently applied in memory.
  token = fixture->runtime->current_authority();
  PFM_REQUIRE(token.ok());
  auto later = observation_of(reference, pfm::ElementKind::Bus, now.value(),
                              pfm::ObservationSequence::from_value(2));
  PFM_CHECK_CODE(fixture->runtime->admit_evidence(later, token.value()),
                 pfm::StatusCode::AuthorityFenced);
  PFM_CHECK_CODE(fixture->runtime->publish_topology(fixture->plant->topology(), token.value()),
                 pfm::StatusCode::AuthorityFenced);
  PFM_CHECK_CODE(fixture->runtime->publish_policy(pfm::default_policy(), token.value()),
                 pfm::StatusCode::AuthorityFenced);
  PFM_CHECK_CODE(fixture->runtime->publish_obligations({}, token.value()),
                 pfm::StatusCode::AuthorityFenced);
  PFM_CHECK_CODE(fixture->runtime->evaluate(token.value()), pfm::StatusCode::AuthorityFenced);
  PFM_CHECK_CODE(fixture->runtime->checkpoint(token.value()), pfm::StatusCode::AuthorityFenced);
  PFM_CHECK_CODE(fixture->runtime->close_incident(token.value()),
                 pfm::StatusCode::AuthorityFenced);
  PFM_CHECK_CODE(fixture->runtime->open_incident(token.value()),
                 pfm::StatusCode::AuthorityFenced);
  PFM_CHECK_CODE(fixture->runtime->retry_request(pfm::ResponseRequestId::from_value(1),
                                                 token.value()),
                 pfm::StatusCode::AuthorityFenced);
  // Read-only calls are unaffected.
  auto status = fixture->runtime->status();
  PFM_REQUIRE(status.ok());
  PFM_CHECK(status.value().commits > 0);
  PFM_CHECK_OK(fixture->runtime->state());

  // Removing the blockage and rolling the control epoch establishes a new
  // authority, and the runtime is usable again.
  for (unsigned index = 0; index < 2; ++index) {
    const auto path = (std::filesystem::path{directory.path()} /
                       ("state." + std::to_string(index) + ".staging"))
                          .string();
    std::error_code code;
    std::filesystem::remove_all(path, code);
    PFM_REQUIRE(!code);
  }
  PFM_CHECK_OK(fixture->runtime->roll_epoch(pfm::ControlEpoch::from_value(2),
                                            pfm::ControllerIncarnation::from_value(1)));
  auto recovered = fixture->runtime->current_authority();
  PFM_REQUIRE(recovered.ok());
  PFM_CHECK_EQ(recovered.value().epoch.value(), std::uint64_t{2});
  PFM_CHECK_OK(fixture->runtime->publish_topology(fixture->plant->topology(), recovered.value()));
}

// ---------------------------------------------------------------------------
// Re-entrancy.
// ---------------------------------------------------------------------------
PFM_TEST(a_transport_that_reenters_the_runtime_is_refused_without_deadlock) {
  pfmtest::TempDirectory directory("adv-reentrant");
  pfm::SyntheticPlantConfig config;
  config.actuate_on_request = false;
  auto plant = std::make_unique<pfm::SyntheticPlant>(config);
  auto clock = pfmtest::make_clock();
  auto transport = std::make_shared<ReentrantTransport>();
  auto opened = pfm::PowerFailureRuntime::open(pfmtest::volatile_options(), clock, transport);
  PFM_REQUIRE(opened.ok());
  auto runtime = std::move(opened.value());
  transport->runtime = runtime.get();

  auto token = runtime->current_authority();
  PFM_REQUIRE(token.ok());
  PFM_CHECK_OK(runtime->publish_topology(plant->topology(), token.value()));
  token = runtime->current_authority();
  PFM_REQUIRE(token.ok());
  token.value().incident = pfm::IncidentId::from_value(1);
  token.value().generation = pfm::IncidentGeneration::from_value(1);
  PFM_CHECK_OK(runtime->open_incident(token.value()));

  auto feeds = plant->elements_of_kind(pfm::ElementKind::UtilityFeed);
  PFM_REQUIRE(!feeds.empty());
  plant->lose_utility(feeds.front());

  auto now = clock->now();
  PFM_REQUIRE(now.ok());
  transport->observation = observation_of(feeds.front(), pfm::ElementKind::UtilityFeed, now.value(),
                                          pfm::ObservationSequence::from_value(1));
  token = runtime->current_authority();
  PFM_REQUIRE(token.ok());
  auto admitted = runtime->admit_evidence(plant->observe(now.value()), token.value());
  PFM_REQUIRE(admitted.ok());
  token = runtime->current_authority();
  PFM_REQUIRE(token.ok());
  auto outcome = runtime->evaluate(token.value());
  PFM_REQUIRE(outcome.ok());
  PFM_CHECK_MSG(outcome.value().requests_dispatched > 0, "no request was dispatched");
  PFM_CHECK_MSG(transport->dispatches.load() > 0, "the transport was never called");
  PFM_REQUIRE(transport->mutation_codes.size() >= 3);
  PFM_CHECK_EQ(transport->mutation_codes[0], pfm::StatusCode::ReentrancyRefused);
  PFM_CHECK_EQ(transport->mutation_codes[1], pfm::StatusCode::ReentrancyRefused);
  PFM_CHECK_EQ(transport->mutation_codes[2], pfm::StatusCode::ReentrancyRefused);
  PFM_CHECK(!transport->read_codes.empty());
  for (const auto code : transport->read_codes) {
    PFM_CHECK_EQ(code, pfm::StatusCode::Ok);
  }
  // The outer dispatch completed and was recorded normally.
  PFM_CHECK(outcome.value().accepted > 0);
  token = runtime->current_authority();
  PFM_REQUIRE(token.ok());
  PFM_CHECK_OK(runtime->publish_topology(plant->topology(), token.value()));
}

// ---------------------------------------------------------------------------
// Concurrency.
// ---------------------------------------------------------------------------
PFM_TEST(four_threads_mutating_one_runtime_leave_a_replayable_store) {
  constexpr std::size_t kThreads = 4;
  constexpr std::size_t kIterations = 20;
  pfmtest::TempDirectory directory("adv-concurrency");
  pfm::RuntimeOptions options = pfmtest::durable_options(directory.path());
  auto plant = std::make_unique<pfm::SyntheticPlant>(pfm::SyntheticPlantConfig{});
  auto clock = pfmtest::make_clock();
  auto transport = std::make_shared<pfm::RefusingTransport>();
  auto opened = pfm::PowerFailureRuntime::open(options, clock, transport);
  PFM_REQUIRE(opened.ok());
  auto runtime = std::move(opened.value());

  auto token = runtime->current_authority();
  PFM_REQUIRE(token.ok());
  PFM_CHECK_OK(runtime->publish_topology(plant->topology(), token.value()));
  token = runtime->current_authority();
  PFM_REQUIRE(token.ok());
  token.value().incident = pfm::IncidentId::from_value(1);
  token.value().generation = pfm::IncidentGeneration::from_value(1);
  PFM_CHECK_OK(runtime->open_incident(token.value()));
  auto feeds = plant->elements_of_kind(pfm::ElementKind::UtilityFeed);
  PFM_REQUIRE(!feeds.empty());
  plant->lose_utility(feeds.front());

  // The plant is a single-threaded model: every observation is produced here,
  // before any thread starts, with one element per thread and a strictly
  // increasing sequence per element.
  std::vector<std::pair<pfm::RefToken, pfm::ElementKind>> targets;
  for (const auto& element : plant->topology().elements) {
    if (element.kind == pfm::ElementKind::LoadGroup || element.kind == pfm::ElementKind::Rack) {
      continue;
    }
    targets.emplace_back(element.element, element.kind);
  }
  PFM_REQUIRE(targets.size() >= kThreads);
  auto now = clock->now();
  PFM_REQUIRE(now.ok());
  std::vector<std::vector<pfm::ElectricalObservation>> pools(kThreads);
  for (std::size_t thread = 0; thread < kThreads; ++thread) {
    for (std::size_t iteration = 0; iteration < kIterations; ++iteration) {
      auto observation =
          observation_of(targets[thread].first, targets[thread].second, now.value(),
                         pfm::ObservationSequence::from_value(1000 + iteration));
      observation.voltage =
          pfm::MilliVolts::from_value(230000 + static_cast<std::int64_t>(iteration));
      pools[thread].push_back(observation);
    }
  }

  std::vector<std::vector<pfm::StatusCode>> codes(kThreads);
  std::vector<std::thread> threads;
  threads.reserve(kThreads);
  for (std::size_t thread = 0; thread < kThreads; ++thread) {
    threads.emplace_back([&runtime, &pools, &codes, thread]() {
      for (std::size_t iteration = 0; iteration < kIterations; ++iteration) {
        auto first = runtime->current_authority();
        if (!first.ok()) {
          codes[thread].push_back(first.code());
          continue;
        }
        codes[thread].push_back(
            runtime->admit_evidence(pools[thread][iteration], first.value()).code());
        auto second = runtime->current_authority();
        if (second.ok()) {
          codes[thread].push_back(runtime->evaluate(second.value()).code());
        } else {
          codes[thread].push_back(second.code());
        }
        auto status = runtime->status();
        codes[thread].push_back(status.ok() ? pfm::StatusCode::Ok : status.code());
        auto state = runtime->state();
        codes[thread].push_back(state.ok() ? pfm::StatusCode::Ok : state.code());
        auto requests = runtime->requests();
        codes[thread].push_back(requests.ok() ? pfm::StatusCode::Ok : requests.code());
      }
    });
  }
  for (auto& thread : threads) {
    thread.join();
  }

  std::size_t checked = 0;
  for (const auto& per_thread : codes) {
    for (const auto code : per_thread) {
      checked += 1;
      if (!documented_code(code)) {
        pfmtest::report_failure(__FILE__, __LINE__,
                                std::string{"a concurrent call returned the undocumented code "} +
                                    std::string{pfm::status_code_name(code)});
      }
    }
  }
  PFM_CHECK_EQ(checked, kThreads * kIterations * 5);

  auto final_status = runtime->status();
  PFM_REQUIRE(final_status.ok());
  PFM_CHECK(final_status.value().commit_sequence.value() > 0);
  PFM_CHECK(final_status.value().commits > 0);
  PFM_CHECK_OK(runtime->shutdown());
  runtime.reset();

  // The store the four threads produced is complete and replayable: the
  // checkpoint plus the retained journal reproduces the live state exactly, and
  // the sequence the runtime last reported is the sequence the store holds.
  pfm::StoreOptions store_options;
  store_options.directory = directory.path();
  store_options.mode = pfm::StoreMode::Durable;
  auto store = pfm::DurableStore::open(store_options);
  PFM_REQUIRE(store.ok());
  auto loaded = store.value()->load();
  PFM_REQUIRE(loaded.ok());
  PFM_CHECK_EQ(store.value()->status().commit_sequence.value(),
               final_status.value().commit_sequence.value());
  PFM_CHECK_EQ(loaded.value().live.revision.value(), final_status.value().revision.value());
  pfm::DomainState folded = loaded.value().checkpoint;
  for (const auto& entry : loaded.value().journal) {
    auto applied = pfm::apply_journal_entry(folded, entry);
    PFM_REQUIRE(applied.ok());
  }
  PFM_CHECK_MSG(pfm::encode_state(folded) == pfm::encode_state(loaded.value().live),
                "the store the threads produced does not replay");
  PFM_CHECK_OK(store.value()->close());
}

// ---------------------------------------------------------------------------
// Lifecycle.
// ---------------------------------------------------------------------------
PFM_TEST(lifecycle_repeat_open_shutdown_and_use_after_close) {
  pfmtest::TempDirectory directory("adv-lifecycle");
  for (std::uint64_t repeat = 1; repeat <= 3; ++repeat) {
    auto fixture = open_fixture(directory.path(), pfm::StoreMode::Durable);
    PFM_REQUIRE(fixture != nullptr);
    if (repeat == 1) {
      PFM_CHECK_OK(fixture->prime());
    } else {
      auto token = fixture->token();
      PFM_REQUIRE(token.ok());
      PFM_CHECK_OK(fixture->runtime->close_incident(token.value()));
      PFM_CHECK_OK(fixture->prime(pfm::IncidentId::from_value(repeat),
                                  pfm::IncidentGeneration::from_value(1)));
    }
    // An evaluation so that the projection after the close has a plan.
    PFM_CHECK_OK(fixture->tick(pfm::duration_from_millis(1000)));
    PFM_CHECK_OK(fixture->runtime->shutdown());
    PFM_CHECK_OK(fixture->runtime->shutdown());

    // A read-only projection after a shutdown is either refused with
    // RuntimeClosed or served from the last committed generation; every write
    // is refused (proven by all_mutating_entry_points_refuse_after_shutdown).
    // What matters here is that nothing crashes, deadlocks, or returns an
    // undocumented code.
    const auto read_code = [](pfm::StatusCode code) {
      return code == pfm::StatusCode::Ok || code == pfm::StatusCode::RuntimeClosed;
    };
    PFM_CHECK_MSG(read_code(fixture->runtime->current_authority().code()),
                  "current_authority after shutdown returned an undocumented code");
    PFM_CHECK_MSG(read_code(fixture->runtime->status().code()),
                  "status after shutdown returned an undocumented code");
    PFM_CHECK_MSG(read_code(fixture->runtime->state().code()),
                  "state after shutdown returned an undocumented code");
    PFM_CHECK_MSG(read_code(fixture->runtime->requests().code()),
                  "requests after shutdown returned an undocumented code");
    PFM_CHECK_MSG(read_code(fixture->runtime->journal().code()),
                  "journal after shutdown returned an undocumented code");
    PFM_CHECK_MSG(read_code(fixture->runtime->transitions().code()),
                  "transitions after shutdown returned an undocumented code");
    PFM_CHECK_MSG(read_code(fixture->runtime->store_status().code()),
                  "store_status after shutdown returned an undocumented code");
    PFM_CHECK_MSG(read_code(fixture->runtime->plan().code()),
                  "plan after shutdown returned an undocumented code");
    const auto single = fixture->runtime->request(pfm::ResponseRequestId::from_value(1)).code();
    PFM_CHECK_MSG(read_code(single) || single == pfm::StatusCode::RequestNotFound,
                  "request after shutdown returned an undocumented code");
    fixture->runtime.reset();  // the destructor after a shutdown is harmless
  }
}

PFM_TEST(all_mutating_entry_points_refuse_after_shutdown) {
  pfmtest::TempDirectory directory("adv-closed");
  auto fixture = open_fixture(directory.path(), pfm::StoreMode::Durable);
  PFM_REQUIRE(fixture != nullptr);
  PFM_CHECK_OK(fixture->prime());
  auto token = fixture->token();
  PFM_REQUIRE(token.ok());
  auto now = fixture->clock->now();
  PFM_REQUIRE(now.ok());
  auto buses = fixture->plant->elements_of_kind(pfm::ElementKind::Bus);
  PFM_REQUIRE(!buses.empty());
  auto observation = observation_of(buses.front(), pfm::ElementKind::Bus, now.value(),
                                    pfm::ObservationSequence::from_value(500));
  PFM_CHECK_OK(fixture->runtime->shutdown());

  const auto authority = token.value();
  const auto request_id = pfm::ResponseRequestId::from_value(1);
  const auto obligation_id = pfm::ObligationId::from_value(1);
  const auto controller = make_ref(pfm::RefKind::Controller, "controller-closed");

  PFM_CHECK_CODE(fixture->runtime->admit_evidence(observation, authority),
                 pfm::StatusCode::RuntimeClosed);
  PFM_CHECK_CODE(fixture->runtime->publish_topology(fixture->plant->topology(), authority),
                 pfm::StatusCode::RuntimeClosed);
  PFM_CHECK_CODE(fixture->runtime->publish_policy(pfm::default_policy(), authority),
                 pfm::StatusCode::RuntimeClosed);
  PFM_CHECK_CODE(fixture->runtime->publish_obligations({}, authority),
                 pfm::StatusCode::RuntimeClosed);
  PFM_CHECK_CODE(fixture->runtime->evaluate(authority), pfm::StatusCode::RuntimeClosed);
  PFM_CHECK_CODE(fixture->runtime->checkpoint(authority), pfm::StatusCode::RuntimeClosed);
  PFM_CHECK_CODE(fixture->runtime->open_incident(authority), pfm::StatusCode::RuntimeClosed);
  PFM_CHECK_CODE(fixture->runtime->close_incident(authority), pfm::StatusCode::RuntimeClosed);
  PFM_CHECK_CODE(fixture->runtime->authorize_recovery(authority),
                 pfm::StatusCode::RuntimeClosed);
  PFM_CHECK_CODE(fixture->runtime->begin_recovery(authority), pfm::StatusCode::RuntimeClosed);
  PFM_CHECK_CODE(fixture->runtime->complete_recovery(authority), pfm::StatusCode::RuntimeClosed);
  PFM_CHECK_CODE(fixture->runtime->relax_obligation(obligation_id, controller, authority),
                 pfm::StatusCode::RuntimeClosed);
  PFM_CHECK_CODE(fixture->runtime->acknowledge_request(request_id, controller, "closed", authority),
                 pfm::StatusCode::RuntimeClosed);
  PFM_CHECK_CODE(fixture->runtime->record_effect(request_id, observation, authority),
                 pfm::StatusCode::RuntimeClosed);
  PFM_CHECK_CODE(fixture->runtime->fail_request(request_id, pfm::ReasonCode::AttemptFailed, {},
                                                authority),
                 pfm::StatusCode::RuntimeClosed);
  PFM_CHECK_CODE(fixture->runtime->cancel_request(request_id, pfm::ReasonCode::AttemptFailed,
                                                  authority),
                 pfm::StatusCode::RuntimeClosed);
  PFM_CHECK_CODE(fixture->runtime->resolve_indeterminate(request_id, false, {}, authority),
                 pfm::StatusCode::RuntimeClosed);
  PFM_CHECK_CODE(fixture->runtime->retry_request(request_id, authority),
                 pfm::StatusCode::RuntimeClosed);
}

PFM_TEST(abandoned_requests_and_closing_an_incident_twice) {
  pfmtest::TempDirectory directory("adv-abandoned");
  auto fixture = RequestRuntime::make(pfm::StoreMode::Volatile, directory.path());
  PFM_REQUIRE(fixture != nullptr);
  PFM_CHECK_OK(fixture->prime());
  auto outcome = fixture->step();
  PFM_REQUIRE(outcome.ok());
  auto requests = fixture->runtime->requests();
  PFM_REQUIRE(requests.ok());
  pfm::ResponseRequest target;
  PFM_REQUIRE(find_open_request(requests.value(), pfm::ResponseRequestId{}, target));

  const auto controller = make_ref(pfm::RefKind::Controller, "controller-abandon");
  auto token = fixture->runtime->current_authority();
  PFM_REQUIRE(token.ok());
  PFM_CHECK_OK(fixture->runtime->cancel_request(target.id, pfm::ReasonCode::JustificationWithdrawn,
                                                token.value()));
  auto abandoned = fixture->runtime->request(target.id);
  PFM_REQUIRE(abandoned.ok());
  PFM_CHECK_EQ(abandoned.value().state, pfm::RequestState::Abandoned);
  PFM_CHECK(pfm::request_state_is_terminal(abandoned.value().state));
  PFM_CHECK(!pfm::request_state_is_open(abandoned.value().state));

  token = fixture->runtime->current_authority();
  PFM_REQUIRE(token.ok());
  PFM_CHECK_CODE(fixture->runtime->cancel_request(target.id, pfm::ReasonCode::JustificationWithdrawn,
                                                  token.value()),
                 pfm::StatusCode::RequestStateConflict);
  PFM_CHECK_CODE(fixture->runtime->retry_request(target.id, token.value()),
                 pfm::StatusCode::RequestStateConflict);
  PFM_CHECK_CODE(
      fixture->runtime->resolve_indeterminate(target.id, false, {}, token.value()),
      pfm::StatusCode::RequestStateConflict);
  PFM_CHECK_CODE(
      fixture->runtime->acknowledge_request(target.id, controller, "late", token.value()),
      pfm::StatusCode::RequestStateConflict);
  PFM_CHECK_CODE(fixture->runtime->fail_request(target.id, pfm::ReasonCode::AttemptFailed, {},
                                                token.value()),
                 pfm::StatusCode::RequestStateConflict);

  // Closing the incident twice.
  auto before_close = fixture->runtime->current_authority();
  PFM_REQUIRE(before_close.ok());
  const auto closed_incident = before_close.value().incident;
  const auto closed_generation = before_close.value().generation;
  PFM_CHECK_OK(fixture->runtime->close_incident(before_close.value()));
  auto status = fixture->runtime->status();
  PFM_REQUIRE(status.ok());
  PFM_CHECK_EQ(status.value().lifecycle, pfm::IncidentLifecycle::Closed);
  PFM_CHECK(!status.value().incident_live);

  auto after_close = fixture->runtime->current_authority();
  PFM_REQUIRE(after_close.ok());
  PFM_CHECK_CODE(fixture->runtime->close_incident(after_close.value()),
                 pfm::StatusCode::NoActiveIncident);
  PFM_CHECK_CODE(fixture->runtime->evaluate(after_close.value()),
                 pfm::StatusCode::NoActiveIncident);
  PFM_CHECK_CODE(fixture->runtime->begin_recovery(after_close.value()),
                 pfm::StatusCode::NoActiveIncident);

  // Reopening the same incident identity after closure is refused.
  auto same_identity = fixture->runtime->current_authority();
  PFM_REQUIRE(same_identity.ok());
  same_identity.value().incident = closed_incident;
  same_identity.value().generation = closed_generation;
  PFM_CHECK_CODE(fixture->runtime->open_incident(same_identity.value()),
                 pfm::StatusCode::IncidentAlreadyOpen);
  // A new identity opens cleanly.
  auto new_identity = fixture->runtime->current_authority();
  PFM_REQUIRE(new_identity.ok());
  new_identity.value().incident = pfm::IncidentId::from_value(closed_incident.value() + 1);
  new_identity.value().generation = pfm::IncidentGeneration::from_value(1);
  PFM_CHECK_OK(fixture->runtime->open_incident(new_identity.value()));
  auto reopened = fixture->runtime->status();
  PFM_REQUIRE(reopened.ok());
  PFM_CHECK(reopened.value().incident_live);
  PFM_CHECK_EQ(reopened.value().incident.value(), closed_incident.value() + 1);
}

// ---------------------------------------------------------------------------
// Integer extremes and configured bounds.
// ---------------------------------------------------------------------------
PFM_TEST(integer_extremes_do_not_wrap_or_crash) {
  constexpr std::int64_t kMax = std::numeric_limits<std::int64_t>::max();
  constexpr std::int64_t kMin = std::numeric_limits<std::int64_t>::min();
  pfmtest::TempDirectory directory("adv-extremes");
  auto fixture = open_fixture(directory.path(), pfm::StoreMode::Volatile);
  PFM_REQUIRE(fixture != nullptr);
  PFM_CHECK_OK(fixture->prime());
  auto now = fixture->clock->now();
  PFM_REQUIRE(now.ok());
  auto buses = fixture->plant->elements_of_kind(pfm::ElementKind::Bus);
  PFM_REQUIRE(buses.size() >= 3);

  // Instants at both ends of the representable range.
  auto future = observation_of(buses[0], pfm::ElementKind::Bus, pfm::Timestamp::from_value(kMax),
                               pfm::ObservationSequence::from_value(1));
  auto current = fixture->runtime->current_authority();
  PFM_REQUIRE(current.ok());
  PFM_CHECK_OK(fixture->runtime->admit_evidence(future, current.value()));
  auto past = observation_of(buses[1], pfm::ElementKind::Bus, pfm::Timestamp::from_value(kMin),
                             pfm::ObservationSequence::from_value(2));
  current = fixture->runtime->current_authority();
  PFM_REQUIRE(current.ok());
  PFM_CHECK_OK(fixture->runtime->admit_evidence(past, current.value()));

  // Quantities at their extremes.
  auto extreme = observation_of(buses[2], pfm::ElementKind::Bus, now.value(),
                                pfm::ObservationSequence::from_value(3));
  extreme.voltage = pfm::MilliVolts::from_value(kMax);
  extreme.frequency = pfm::MilliHertz::from_value(kMax);
  extreme.current = pfm::MilliAmps::from_value(kMax);
  extreme.has_current = true;
  extreme.reserve = pfm::milli_percent_from_value(100000);
  extreme.has_reserve = true;
  current = fixture->runtime->current_authority();
  PFM_REQUIRE(current.ok());
  PFM_CHECK_OK(fixture->runtime->admit_evidence(extreme, current.value()));

  current = fixture->runtime->current_authority();
  PFM_REQUIRE(current.ok());
  auto outcome = fixture->runtime->evaluate(current.value());
  PFM_CHECK_MSG(outcome.ok() || outcome.code() == pfm::StatusCode::ArithmeticOverflow,
                "an extreme reading produced " + std::string{outcome.status_or_ok().to_string()});

  // The unrepresentable instants are never read as health.
  auto state = fixture->runtime->state();
  PFM_REQUIRE(state.ok());
  const auto current_observations =
      pfm::current_observations(state.value(), now.value(), state.value().policy);
  PFM_CHECK_MSG(std::none_of(current_observations.begin(), current_observations.end(),
                             [&buses](const pfm::ElectricalObservation& observation) {
                               return observation.element == buses[0] ||
                                      observation.element == buses[1];
                             }),
                "an observation with an unrepresentable instant was treated as current");

  // Counters at their extremes are fenced, not wrapped.
  auto token = fixture->runtime->current_authority();
  PFM_REQUIRE(token.ok());
  token.value().revision = pfm::StateRevision::from_value(std::numeric_limits<std::uint64_t>::max());
  auto observation = observation_of(buses[2], pfm::ElementKind::Bus, now.value(),
                                    pfm::ObservationSequence::from_value(4));
  PFM_CHECK_CODE(fixture->runtime->admit_evidence(observation, token.value()),
                 pfm::StatusCode::FutureRevision);
  token = fixture->runtime->current_authority();
  PFM_REQUIRE(token.ok());
  token.value().generation =
      pfm::IncidentGeneration::from_value(std::numeric_limits<std::uint64_t>::max());
  PFM_CHECK_CODE(fixture->runtime->admit_evidence(observation, token.value()),
                 pfm::StatusCode::FutureGeneration);
  token = fixture->runtime->current_authority();
  PFM_REQUIRE(token.ok());
  token.value().epoch = pfm::ControlEpoch::from_value(std::numeric_limits<std::uint64_t>::max());
  PFM_CHECK_CODE(fixture->runtime->admit_evidence(observation, token.value()),
                 pfm::StatusCode::FutureEpoch);

  // Bounds outside the accepted range are refused when the runtime opens.
  {
    pfm::RuntimeOptions options = pfmtest::volatile_options();
    options.bounds.max_observations = std::numeric_limits<std::uint32_t>::max();
    PFM_CHECK_CODE(pfm::PowerFailureRuntime::open(options, pfmtest::make_clock(), nullptr),
                   pfm::StatusCode::InvalidArgument);
  }
  {
    pfm::RuntimeOptions options = pfmtest::volatile_options();
    options.bounds.max_journal_entries = 0;
    PFM_CHECK_CODE(pfm::PowerFailureRuntime::open(options, pfmtest::make_clock(), nullptr),
                   pfm::StatusCode::InvalidArgument);
  }
  // A policy whose windows cannot coexist is refused rather than partly used.
  {
    pfm::RuntimeOptions options = pfmtest::volatile_options();
    options.policy.evidence_freshness_window = pfm::duration_from_millis(kMax);
    options.policy.evidence_expiry_window = pfm::duration_from_millis(kMax - 1);
    PFM_CHECK_CODE(pfm::PowerFailureRuntime::open(options, pfmtest::make_clock(), nullptr),
                   pfm::StatusCode::InvalidArgument);
  }
  {
    pfm::RuntimeOptions options = pfmtest::volatile_options();
    options.policy.verification_window = pfm::duration_from_millis(kMin);
    PFM_CHECK_CODE(pfm::PowerFailureRuntime::open(options, pfmtest::make_clock(), nullptr),
                   pfm::StatusCode::InvalidArgument);
  }
  {
    pfm::RuntimeOptions options = pfmtest::volatile_options();
    options.policy.max_attempts_per_request = std::numeric_limits<std::uint32_t>::max();
    PFM_CHECK_CODE(pfm::PowerFailureRuntime::open(options, pfmtest::make_clock(), nullptr),
                   pfm::StatusCode::InvalidArgument);
  }
}

PFM_TEST(bounds_exhaustion_is_reported_with_exact_codes) {
  // A probe run establishes how many requests the scenario justifies.
  pfmtest::TempDirectory probe_directory("adv-bounds-probe");
  auto probe = RequestRuntime::make(pfm::StoreMode::Volatile, probe_directory.path());
  PFM_REQUIRE(probe != nullptr);
  PFM_CHECK_OK(probe->prime(std::numeric_limits<std::size_t>::max()));
  auto probe_outcome = probe->step();
  PFM_REQUIRE(probe_outcome.ok());
  const std::size_t planned = probe_outcome.value().requests_planned;
  PFM_REQUIRE(planned >= 2);

  // The planner reports ResourceExhausted when a plan would exceed the bound.
  {
    pfmtest::TempDirectory directory("adv-bounds-plan");
    pfm::Bounds bounds;
    bounds.max_requests_per_plan = static_cast<std::uint32_t>(planned - 1);
    auto fixture = RequestRuntime::make(pfm::StoreMode::Volatile, directory.path(), bounds);
    PFM_REQUIRE(fixture != nullptr);
    PFM_CHECK_OK(fixture->prime(std::numeric_limits<std::size_t>::max()));
    auto outcome = fixture->step();
    PFM_CHECK_CODE(outcome, pfm::StatusCode::ResourceExhausted);
  }

  // Evidence intake beyond the observation bound.
  {
    pfmtest::TempDirectory directory("adv-bounds-evidence");
    pfm::Bounds bounds;
    bounds.max_observations = 1;
    auto fixture = RequestRuntime::make(pfm::StoreMode::Volatile, directory.path(), bounds);
    PFM_REQUIRE(fixture != nullptr);
    PFM_CHECK_OK(fixture->prime(0));
    auto now = fixture->clock->now();
    PFM_REQUIRE(now.ok());
    auto observations = fixture->plant->observe(now.value());
    PFM_REQUIRE(observations.size() > 1);
    auto token = fixture->runtime->current_authority();
    PFM_REQUIRE(token.ok());
    PFM_CHECK_CODE(fixture->runtime->admit_evidence(observations, token.value()),
                   pfm::StatusCode::BoundsExceeded);
    // A single observation still fits.
    token = fixture->runtime->current_authority();
    PFM_REQUIRE(token.ok());
    PFM_CHECK_OK(fixture->runtime->admit_evidence(observations.front(), token.value()));
  }

  // A topology beyond the element bound.
  {
    pfmtest::TempDirectory directory("adv-bounds-topology");
    pfm::Bounds bounds;
    bounds.max_topology_elements = 2;
    auto fixture = RequestRuntime::make(pfm::StoreMode::Volatile, directory.path(), bounds);
    PFM_REQUIRE(fixture != nullptr);
    auto token = fixture->runtime->current_authority();
    PFM_REQUIRE(token.ok());
    PFM_CHECK_CODE(fixture->runtime->publish_topology(fixture->plant->topology(), token.value()),
                   pfm::StatusCode::TopologyBoundsExceeded);
  }

  // An obligation set beyond its bound.
  {
    pfmtest::TempDirectory directory("adv-bounds-obligations");
    pfm::Bounds bounds;
    bounds.max_obligations = 1;
    auto fixture = RequestRuntime::make(pfm::StoreMode::Volatile, directory.path(), bounds);
    PFM_REQUIRE(fixture != nullptr);
    auto token = fixture->runtime->current_authority();
    PFM_REQUIRE(token.ok());
    PFM_CHECK_OK(fixture->runtime->publish_topology(fixture->plant->topology(), token.value()));
    token = fixture->runtime->current_authority();
    PFM_REQUIRE(token.ok());
    token.value().incident = pfm::IncidentId::from_value(1);
    token.value().generation = pfm::IncidentGeneration::from_value(1);
    PFM_CHECK_OK(fixture->runtime->open_incident(token.value()));
    auto now = fixture->clock->now();
    PFM_REQUIRE(now.ok());
    const auto authority = make_ref(pfm::RefKind::Controller, "controller-obligations");
    const auto buses = fixture->plant->elements_of_kind(pfm::ElementKind::Bus);
    PFM_REQUIRE(!buses.empty());
    std::vector<pfm::ProtectedObligation> obligations;
    for (std::uint64_t index = 1; index <= 2; ++index) {
      pfm::ProtectedObligation obligation;
      obligation.id = pfm::ObligationId::from_value(index);
      obligation.target = buses.front();
      obligation.protection = pfm::ProtectionClass::ServiceLevel;
      obligation.status = pfm::ObligationStatus::Satisfied;
      obligation.has_report = true;
      obligation.reported_at = now.value();
      obligation.reporting_authority = authority;
      obligations.push_back(obligation);
    }
    token = fixture->runtime->current_authority();
    PFM_REQUIRE(token.ok());
    PFM_CHECK_CODE(fixture->runtime->publish_obligations(obligations, token.value()),
                   pfm::StatusCode::BoundsExceeded);
  }

  // A scope beyond the bound the plan is allowed to reason about.
  {
    pfmtest::TempDirectory directory("adv-bounds-scope");
    pfm::Bounds bounds;
    bounds.max_scope_elements = 1;
    auto fixture = RequestRuntime::make(pfm::StoreMode::Volatile, directory.path(), bounds);
    PFM_REQUIRE(fixture != nullptr);
    PFM_CHECK_OK(fixture->prime(std::numeric_limits<std::size_t>::max()));
    auto outcome = fixture->step();
    PFM_CHECK_CODE(outcome, pfm::StatusCode::BoundsExceeded);
  }
}

PFM_TEST_MAIN()
