// Power Failure Manager -- state machine tests.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0 (see LICENSE).
//
// Covers classification, affected scope, plan determinism, the recovery gates,
// authority fencing, the request lifecycle, and randomized property checks
// against tests/reference_model.hpp.
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include "framework.hpp"
#include "reference_model.hpp"
#include "support.hpp"

#include "pfm/authority.hpp"
#include "pfm/classification.hpp"
#include "pfm/codec.hpp"
#include "pfm/plan.hpp"
#include "pfm/recovery.hpp"
#include "pfm/request.hpp"
#include "pfm/scope.hpp"
#include "pfm/topology.hpp"

using namespace summon::pfm;

namespace {

// --- small builders --------------------------------------------------------

RefToken make_ref(RefKind kind, const std::string& name) {
  auto made = RefToken::make(kind, name);
  if (!made.ok()) {
    ::pfmtest::report_failure(__FILE__, __LINE__, "cannot build reference " + name);
    return RefToken{};
  }
  return made.value();
}

RefToken parse_ref(const std::string& text) {
  auto parsed = RefToken::parse(text);
  if (!parsed.ok()) {
    ::pfmtest::report_failure(__FILE__, __LINE__, "cannot parse reference " + text);
    return RefToken{};
  }
  return parsed.value();
}

struct TopoBuilder {
  TopologySnapshot snapshot{};

  TopoBuilder() { snapshot.generation = TopologyGeneration::from_value(1); }

  TopoBuilder& add(ElementKind kind, const std::string& name, const std::string& upstream = {},
                   const std::string& domain = {}, const std::string& isolation = {},
                   const std::string& load_group = {}) {
    TopologyElement element;
    element.element = make_ref(ref_kind_for_element(kind), name);
    element.kind = kind;
    if (!upstream.empty()) {
      element.upstream = parse_ref(upstream);
    }
    if (!domain.empty()) {
      element.domain = parse_ref(domain);
    }
    if (!isolation.empty()) {
      element.isolation_point = parse_ref(isolation);
      element.has_isolation_point = true;
    }
    if (!load_group.empty()) {
      element.load_group = parse_ref(load_group);
    }
    element.transfer_capable = kind == ElementKind::UpsUnit ||
                               kind == ElementKind::StaticTransferSwitch ||
                               kind == ElementKind::AutomaticTransferSwitch;
    element.label = name;
    snapshot.elements.push_back(std::move(element));
    return *this;
  }

  TopoBuilder& fail_domain(const std::string& name, const std::vector<std::string>& members,
                           bool shared) {
    FailureDomain domain;
    domain.domain = make_ref(RefKind::FailureDomain, name);
    for (const auto& member : members) {
      domain.members.push_back(parse_ref(member));
    }
    domain.shared = shared;
    snapshot.domains.push_back(std::move(domain));
    return *this;
  }
};

TopologyIndex build_index(const TopologySnapshot& snapshot, const Bounds& bounds) {
  auto built = TopologyIndex::build(snapshot, bounds);
  if (!built.ok()) {
    ::pfmtest::report_failure(__FILE__, __LINE__,
                              std::string{"TopologyIndex::build failed: "} +
                                  std::string{built.status().message()});
    return TopologyIndex{};
  }
  return std::move(built.value());
}

ElectricalObservation make_observation(const RefToken& element, ElementKind kind, Timestamp at,
                                       ObservationSequence& next) {
  ElectricalObservation observation;
  observation.element = element;
  observation.kind = kind;
  observation.origin = ObservationOrigin::SyntheticPlant;
  observation.quality = EvidenceQuality::Good;
  observation.sequence = next;
  next = ObservationSequence::from_value(next.value() + 1);
  observation.observed_at = at;
  observation.provenance = "state-machine-tests";
  observation.energization = EnergizationState::Energized;
  observation.breaker = BreakerPosition::Closed;
  observation.transfer = TransferState::OnUtility;
  observation.voltage = MilliVolts::from_value(230000);
  observation.has_voltage = true;
  observation.frequency = MilliHertz::from_value(50000);
  observation.has_frequency = true;
  return observation;
}

// A fixed distribution chain with every element kind PFM classifies.
struct ChainFixture {
  TopologySnapshot snapshot{};
  Timestamp now{timestamp_from_unix_millis(1767225600000LL)};
  ElectricalPolicy policy{default_policy()};
  Bounds bounds{};
  std::vector<ElectricalObservation> observations{};
  ObservationSequence next_sequence{ObservationSequence::from_value(1)};

  RefToken feed{};
  RefToken brk_feed{};
  RefToken sg{};
  RefToken gen{};
  RefToken ats{};
  RefToken bus{};
  RefToken brk_bus{};
  RefToken ups{};
  RefToken pdu{};
  RefToken brk_pdu{};
  RefToken ckt{};
  RefToken branch{};
  RefToken load{};
  RefToken fd_shared{};
  RefToken fd_downstream{};
  RefToken fd_mixed{};
};

ChainFixture make_chain() {
  ChainFixture fx;
  fx.policy.generation = PolicyGeneration::from_value(1);

  TopoBuilder builder;
  builder.add(ElementKind::UtilityFeed, "feed-1", "", "power-domain:pd-1");
  builder.add(ElementKind::Breaker, "brk-1", "utility-feed:feed-1");
  builder.add(ElementKind::Switchgear, "sg-1", "breaker:brk-1", "power-domain:pd-1",
              "breaker:brk-1");
  builder.add(ElementKind::Generator, "gen-1", "switchgear:sg-1", "power-domain:pd-1");
  builder.add(ElementKind::AutomaticTransferSwitch, "ats-1", "switchgear:sg-1",
              "power-domain:pd-1");
  builder.add(ElementKind::Bus, "bus-1", "switchgear:sg-1", "power-domain:pd-1",
              "breaker:brk-bus-1");
  builder.add(ElementKind::Breaker, "brk-bus-1", "bus:bus-1");
  builder.add(ElementKind::UpsUnit, "ups-1", "breaker:brk-bus-1", "power-domain:pd-1");
  builder.add(ElementKind::Pdu, "pdu-1", "ups:ups-1", "power-domain:pd-2", "breaker:brk-pdu-1");
  builder.add(ElementKind::Breaker, "brk-pdu-1", "pdu:pdu-1");
  builder.add(ElementKind::Circuit, "ckt-1", "breaker:brk-pdu-1");
  builder.add(ElementKind::PduBranch, "branch-1", "pdu:pdu-1", "power-domain:pd-2", "",
              "load-group:load-1");
  builder.add(ElementKind::LoadGroup, "load-1", "pdu-branch:branch-1");
  builder.fail_domain("fd-shared",
                      {"utility-feed:feed-1", "switchgear:sg-1", "generator:gen-1", "ups:ups-1"},
                      true);
  builder.fail_domain("fd-downstream", {"pdu:pdu-1", "pdu-branch:branch-1"}, false);
  builder.fail_domain("fd-mixed", {"bus:bus-1", "breaker:brk-bus-1", "pdu:pdu-1"}, false);
  fx.snapshot = builder.snapshot;

  fx.feed = parse_ref("utility-feed:feed-1");
  fx.brk_feed = parse_ref("breaker:brk-1");
  fx.sg = parse_ref("switchgear:sg-1");
  fx.gen = parse_ref("generator:gen-1");
  fx.ats = parse_ref("ats:ats-1");
  fx.bus = parse_ref("bus:bus-1");
  fx.brk_bus = parse_ref("breaker:brk-bus-1");
  fx.ups = parse_ref("ups:ups-1");
  fx.pdu = parse_ref("pdu:pdu-1");
  fx.brk_pdu = parse_ref("breaker:brk-pdu-1");
  fx.ckt = parse_ref("circuit:ckt-1");
  fx.branch = parse_ref("pdu-branch:branch-1");
  fx.load = parse_ref("load-group:load-1");
  fx.fd_shared = parse_ref("failure-domain:fd-shared");
  fx.fd_downstream = parse_ref("failure-domain:fd-downstream");
  fx.fd_mixed = parse_ref("failure-domain:fd-mixed");

  for (const auto& element : fx.snapshot.elements) {
    ElectricalObservation observation =
        make_observation(element.element, element.kind, fx.now, fx.next_sequence);
    switch (element.kind) {
      case ElementKind::UpsUnit:
        observation.transfer = TransferState::OnUtility;
        observation.reserve = milli_percent_from_value(80000);
        observation.has_reserve = true;
        break;
      case ElementKind::Generator:
        observation.generator = GeneratorState::Synchronized;
        observation.transfer = TransferState::OnGenerator;
        break;
      case ElementKind::PduBranch:
        observation.current = MilliAmps::from_value(10000);
        observation.has_current = true;
        break;
      case ElementKind::LoadGroup:
        observation.current = MilliAmps::from_value(10000);
        observation.has_current = true;
        break;
      default:
        break;
    }
    fx.observations.push_back(std::move(observation));
  }
  return fx;
}

template <class Fn>
void mutate(ChainFixture& fx, const RefToken& element, Fn fn) {
  for (auto& observation : fx.observations) {
    if (observation.element == element) {
      fn(observation);
      return;
    }
  }
  ::pfmtest::report_failure(__FILE__, __LINE__, "no observation for the mutated element");
}

void drop_observation(ChainFixture& fx, const RefToken& element) {
  const auto before = fx.observations.size();
  fx.observations.erase(std::remove_if(fx.observations.begin(), fx.observations.end(),
                                       [&element](const ElectricalObservation& observation) {
                                         return observation.element == element;
                                       }),
                        fx.observations.end());
  if (fx.observations.size() == before) {
    ::pfmtest::report_failure(__FILE__, __LINE__, "no observation to drop");
  }
}

ClassificationResult classify_or_fail(const ChainFixture& fx, const TopologyIndex& index) {
  auto result = classify(fx.observations, fx.now, index, fx.policy, fx.bounds);
  if (!result.ok()) {
    ::pfmtest::report_failure(__FILE__, __LINE__,
                              std::string{"classify failed: "} +
                                  std::string{result.status().message()});
    return ClassificationResult{};
  }
  return std::move(result.value());
}

const ClassifiedFailure* find_failure(const ClassificationResult& result, const RefToken& element,
                                      FailureClass klass) {
  for (const auto& failure : result.failures) {
    if (failure.element == element && failure.klass == klass) {
      return &failure;
    }
  }
  return nullptr;
}

bool has_failure(const ClassificationResult& result, const RefToken& element, FailureClass klass) {
  return find_failure(result, element, klass) != nullptr;
}

bool has_reason(const ClassifiedFailure& failure, ReasonCode code) {
  if (failure.primary_reason == code) {
    return true;
  }
  for (const auto& step : failure.reasons) {
    if (step.code == code) {
      return true;
    }
  }
  return false;
}

AffectedScope scope_or_fail(const ClassificationResult& classification,
                            const TopologyIndex& index, const ElectricalPolicy& policy,
                            const Bounds& bounds) {
  auto scope = resolve_scope(classification, index, policy, bounds);
  if (!scope.ok()) {
    ::pfmtest::report_failure(__FILE__, __LINE__,
                              std::string{"resolve_scope failed: "} +
                                  std::string{scope.status().message()});
    return AffectedScope{};
  }
  return std::move(scope.value());
}

RecoveryAssessment assess_or_fail(const RecoveryInputs& inputs) {
  auto assessment = assess_recovery(inputs);
  if (!assessment.ok()) {
    ::pfmtest::report_failure(__FILE__, __LINE__,
                              std::string{"assess_recovery failed: "} +
                                  std::string{assessment.status().message()});
    return RecoveryAssessment{};
  }
  return std::move(assessment.value());
}

bool gate_satisfied(const RecoveryAssessment& assessment, RecoveryGate gate) {
  const GateEvaluation* evaluation = assessment.find_gate(gate);
  return evaluation != nullptr && evaluation->satisfied;
}

ReasonCode gate_reason(const RecoveryAssessment& assessment, RecoveryGate gate) {
  const GateEvaluation* evaluation = assessment.find_gate(gate);
  return evaluation != nullptr ? evaluation->reason : ReasonCode::None;
}

bool sorted_unique(const std::vector<RefToken>& values) {
  if (!std::is_sorted(values.begin(), values.end())) {
    return false;
  }
  return std::adjacent_find(values.begin(), values.end()) == values.end();
}

std::string refs_to_text(const std::vector<RefToken>& values) {
  std::string text;
  for (const auto& value : values) {
    if (!text.empty()) {
      text.append(",");
    }
    text.append(value.to_string());
  }
  return text;
}

// --- classification --------------------------------------------------------

PFM_TEST(classification_utility_feed_loss) {
  ChainFixture fx = make_chain();
  TopologyIndex index = build_index(fx.snapshot, fx.bounds);
  mutate(fx, fx.feed, [](ElectricalObservation& observation) {
    observation.energization = EnergizationState::DeEnergized;
    observation.voltage = MilliVolts::from_value(0);
    observation.frequency = MilliHertz::from_value(0);
  });
  const ClassificationResult result = classify_or_fail(fx, index);
  const ClassifiedFailure* failure =
      find_failure(result, fx.feed, FailureClass::UtilityFeedLoss);
  PFM_REQUIRE(failure != nullptr);
  PFM_CHECK_EQ(failure->primary_reason, ReasonCode::FeedDeEnergized);
  PFM_CHECK_EQ(failure->kind, ElementKind::UtilityFeed);
  PFM_CHECK(failure->evidence_current);
  PFM_CHECK_EQ(result.primary_class(), FailureClass::UtilityFeedLoss);
  PFM_CHECK(std::find(result.resolved_elements.begin(), result.resolved_elements.end(), fx.feed) !=
            result.resolved_elements.end());
  PFM_CHECK(std::find(result.unresolved_elements.begin(), result.unresolved_elements.end(),
                      fx.feed) == result.unresolved_elements.end());
  // A lost feed is a domain member failure, so the shared domain is promoted.
  PFM_CHECK(has_failure(result, fx.fd_shared, FailureClass::SharedUpstreamDomainFailure));
}

PFM_TEST(classification_utility_feed_degradation) {
  {
    ChainFixture fx = make_chain();
    TopologyIndex index = build_index(fx.snapshot, fx.bounds);
    mutate(fx, fx.feed, [](ElectricalObservation& observation) {
      observation.voltage = MilliVolts::from_value(180000);
    });
    const ClassificationResult result = classify_or_fail(fx, index);
    const ClassifiedFailure* failure =
        find_failure(result, fx.feed, FailureClass::UtilityFeedDegradation);
    PFM_REQUIRE(failure != nullptr);
    PFM_CHECK_EQ(failure->primary_reason, ReasonCode::FeedVoltageOutOfBand);
    PFM_CHECK(failure->evidence_current);
    PFM_CHECK(!has_failure(result, fx.feed, FailureClass::UtilityFeedLoss));
    PFM_CHECK_EQ(result.primary_class(), FailureClass::UtilityFeedDegradation);
  }
  {
    ChainFixture fx = make_chain();
    TopologyIndex index = build_index(fx.snapshot, fx.bounds);
    mutate(fx, fx.feed, [](ElectricalObservation& observation) {
      observation.frequency = MilliHertz::from_value(48000);
    });
    const ClassificationResult result = classify_or_fail(fx, index);
    const ClassifiedFailure* failure =
        find_failure(result, fx.feed, FailureClass::UtilityFeedDegradation);
    PFM_REQUIRE(failure != nullptr);
    PFM_CHECK_EQ(failure->primary_reason, ReasonCode::FeedFrequencyOutOfBand);
    PFM_CHECK(has_reason(*failure, ReasonCode::FeedFrequencyOutOfBand));
  }
  {
    // A partially energized feed is degradation, not a loss.
    ChainFixture fx = make_chain();
    TopologyIndex index = build_index(fx.snapshot, fx.bounds);
    mutate(fx, fx.feed, [](ElectricalObservation& observation) {
      observation.energization = EnergizationState::PartiallyEnergized;
    });
    const ClassificationResult result = classify_or_fail(fx, index);
    PFM_CHECK(has_failure(result, fx.feed, FailureClass::UtilityFeedDegradation));
    PFM_CHECK(!has_failure(result, fx.feed, FailureClass::UtilityFeedLoss));
  }
}

PFM_TEST(classification_breaker_and_circuit_failure) {
  ChainFixture fx = make_chain();
  TopologyIndex index = build_index(fx.snapshot, fx.bounds);
  mutate(fx, fx.brk_feed, [](ElectricalObservation& observation) {
    observation.breaker = BreakerPosition::Tripped;
  });
  mutate(fx, fx.ckt, [](ElectricalObservation& observation) {
    observation.breaker = BreakerPosition::Tripped;
  });
  const ClassificationResult result = classify_or_fail(fx, index);
  const ClassifiedFailure* breaker =
      find_failure(result, fx.brk_feed, FailureClass::BreakerFailure);
  PFM_REQUIRE(breaker != nullptr);
  PFM_CHECK_EQ(breaker->primary_reason, ReasonCode::BreakerTripped);
  PFM_CHECK_EQ(breaker->kind, ElementKind::Breaker);
  const ClassifiedFailure* circuit = find_failure(result, fx.ckt, FailureClass::CircuitFailure);
  PFM_REQUIRE(circuit != nullptr);
  PFM_CHECK_EQ(circuit->primary_reason, ReasonCode::BreakerTripped);
  PFM_CHECK_EQ(circuit->kind, ElementKind::Circuit);
}

PFM_TEST(classification_bus_and_pdu_failure) {
  {
    ChainFixture fx = make_chain();
    TopologyIndex index = build_index(fx.snapshot, fx.bounds);
    mutate(fx, fx.bus, [](ElectricalObservation& observation) {
      observation.energization = EnergizationState::DeEnergized;
    });
    const ClassificationResult result = classify_or_fail(fx, index);
    const ClassifiedFailure* failure = find_failure(result, fx.bus, FailureClass::BusFailure);
    PFM_REQUIRE(failure != nullptr);
    PFM_CHECK_EQ(failure->primary_reason, ReasonCode::ElementDeEnergizedWhileUpstreamEnergized);
    PFM_CHECK_EQ(failure->kind, ElementKind::Bus);
  }
  {
    ChainFixture fx = make_chain();
    TopologyIndex index = build_index(fx.snapshot, fx.bounds);
    mutate(fx, fx.pdu, [](ElectricalObservation& observation) {
      observation.energization = EnergizationState::DeEnergized;
    });
    const ClassificationResult result = classify_or_fail(fx, index);
    const ClassifiedFailure* failure = find_failure(result, fx.pdu, FailureClass::PduFailure);
    PFM_REQUIRE(failure != nullptr);
    PFM_CHECK_EQ(failure->primary_reason, ReasonCode::ElementDeEnergizedWhileUpstreamEnergized);
  }
  {
    // A de-energized bus whose upstream state is not established is still a
    // failure, and the reason says the upstream is unknown.
    ChainFixture fx = make_chain();
    TopologyIndex index = build_index(fx.snapshot, fx.bounds);
    drop_observation(fx, fx.sg);
    mutate(fx, fx.bus, [](ElectricalObservation& observation) {
      observation.energization = EnergizationState::DeEnergized;
    });
    const ClassificationResult result = classify_or_fail(fx, index);
    const ClassifiedFailure* failure = find_failure(result, fx.bus, FailureClass::BusFailure);
    PFM_REQUIRE(failure != nullptr);
    PFM_CHECK_EQ(failure->primary_reason, ReasonCode::ElementDeEnergizedWithUpstreamUnknown);
  }
}

PFM_TEST(classification_ups_reserve_insufficient) {
  {
    ChainFixture fx = make_chain();
    TopologyIndex index = build_index(fx.snapshot, fx.bounds);
    mutate(fx, fx.ups, [](ElectricalObservation& observation) {
      observation.reserve = milli_percent_from_value(40000);
    });
    const ClassificationResult result = classify_or_fail(fx, index);
    const ClassifiedFailure* failure =
        find_failure(result, fx.ups, FailureClass::UpsReserveInsufficient);
    PFM_REQUIRE(failure != nullptr);
    PFM_CHECK_EQ(failure->primary_reason, ReasonCode::UpsReserveBelowFloor);
    PFM_CHECK(failure->evidence_current);
    PFM_CHECK(!has_failure(result, fx.ups, FailureClass::UpsFailure));
  }
  {
    ChainFixture fx = make_chain();
    TopologyIndex index = build_index(fx.snapshot, fx.bounds);
    mutate(fx, fx.ups, [](ElectricalObservation& observation) {
      observation.reserve = milli_percent_from_value(10000);
    });
    const ClassificationResult result = classify_or_fail(fx, index);
    const ClassifiedFailure* failure =
        find_failure(result, fx.ups, FailureClass::UpsReserveInsufficient);
    PFM_REQUIRE(failure != nullptr);
    PFM_CHECK_EQ(failure->primary_reason, ReasonCode::UpsReserveBelowCriticalFloor);
  }
  {
    // The floor is inclusive: a reserve exactly at the floor is insufficient.
    ChainFixture fx = make_chain();
    TopologyIndex index = build_index(fx.snapshot, fx.bounds);
    mutate(fx, fx.ups, [&fx](ElectricalObservation& observation) {
      observation.reserve = fx.policy.reserve_floor;
    });
    const ClassificationResult result = classify_or_fail(fx, index);
    const ClassifiedFailure* failure =
        find_failure(result, fx.ups, FailureClass::UpsReserveInsufficient);
    PFM_REQUIRE(failure != nullptr);
    PFM_CHECK_EQ(failure->primary_reason, ReasonCode::UpsReserveBelowFloor);
  }
  {
    // Above the floor there is no reserve failure at all.
    ChainFixture fx = make_chain();
    TopologyIndex index = build_index(fx.snapshot, fx.bounds);
    const ClassificationResult result = classify_or_fail(fx, index);
    PFM_CHECK(!has_failure(result, fx.ups, FailureClass::UpsReserveInsufficient));
  }
}

PFM_TEST(classification_ups_de_energized) {
  {
    ChainFixture fx = make_chain();
    TopologyIndex index = build_index(fx.snapshot, fx.bounds);
    mutate(fx, fx.ups, [](ElectricalObservation& observation) {
      observation.energization = EnergizationState::DeEnergized;
      observation.reserve = milli_percent_from_value(0);
    });
    const ClassificationResult result = classify_or_fail(fx, index);
    const ClassifiedFailure* failure = find_failure(result, fx.ups, FailureClass::UpsFailure);
    PFM_REQUIRE(failure != nullptr);
    PFM_CHECK_EQ(failure->primary_reason, ReasonCode::UpsFaulted);
    PFM_CHECK_EQ(failure->kind, ElementKind::UpsUnit);
  }
  {
    ChainFixture fx = make_chain();
    TopologyIndex index = build_index(fx.snapshot, fx.bounds);
    mutate(fx, fx.ups, [](ElectricalObservation& observation) {
      observation.energization = EnergizationState::PartiallyEnergized;
    });
    const ClassificationResult result = classify_or_fail(fx, index);
    const ClassifiedFailure* failure = find_failure(result, fx.ups, FailureClass::UpsFailure);
    PFM_REQUIRE(failure != nullptr);
    PFM_CHECK_EQ(failure->primary_reason, ReasonCode::UpsOnBattery);
  }
}

PFM_TEST(classification_generator_failures) {
  {
    ChainFixture fx = make_chain();
    TopologyIndex index = build_index(fx.snapshot, fx.bounds);
    mutate(fx, fx.gen, [](ElectricalObservation& observation) {
      observation.generator = GeneratorState::Faulted;
    });
    const ClassificationResult result = classify_or_fail(fx, index);
    const ClassifiedFailure* failure =
        find_failure(result, fx.gen, FailureClass::GeneratorFailure);
    PFM_REQUIRE(failure != nullptr);
    PFM_CHECK_EQ(failure->primary_reason, ReasonCode::GeneratorFaulted);
  }
  {
    // Running but never synchronized.
    ChainFixture fx = make_chain();
    TopologyIndex index = build_index(fx.snapshot, fx.bounds);
    mutate(fx, fx.gen, [](ElectricalObservation& observation) {
      observation.generator = GeneratorState::Running;
      observation.transfer = TransferState::Failed;
    });
    const ClassificationResult result = classify_or_fail(fx, index);
    const ClassifiedFailure* failure =
        find_failure(result, fx.gen, FailureClass::GeneratorSyncFailure);
    PFM_REQUIRE(failure != nullptr);
    PFM_CHECK_EQ(failure->primary_reason, ReasonCode::GeneratorNotSynchronized);
    PFM_CHECK(!has_failure(result, fx.gen, FailureClass::GeneratorFailure));
  }
  {
    // Synchronized but the transfer failed.
    ChainFixture fx = make_chain();
    TopologyIndex index = build_index(fx.snapshot, fx.bounds);
    mutate(fx, fx.gen, [](ElectricalObservation& observation) {
      observation.generator = GeneratorState::Synchronized;
      observation.transfer = TransferState::Failed;
    });
    const ClassificationResult result = classify_or_fail(fx, index);
    const ClassifiedFailure* failure =
        find_failure(result, fx.gen, FailureClass::GeneratorTransferFailure);
    PFM_REQUIRE(failure != nullptr);
    PFM_CHECK_EQ(failure->primary_reason, ReasonCode::GeneratorTransferFailed);
  }
  {
    // A transfer still in progress on a running generator is a sync failure.
    ChainFixture fx = make_chain();
    TopologyIndex index = build_index(fx.snapshot, fx.bounds);
    mutate(fx, fx.gen, [](ElectricalObservation& observation) {
      observation.generator = GeneratorState::Running;
      observation.transfer = TransferState::Transferring;
    });
    const ClassificationResult result = classify_or_fail(fx, index);
    const ClassifiedFailure* failure =
        find_failure(result, fx.gen, FailureClass::GeneratorSyncFailure);
    PFM_REQUIRE(failure != nullptr);
    PFM_CHECK_EQ(failure->primary_reason, ReasonCode::GeneratorNotSynchronized);
  }
  {
    // A generator that never started.
    ChainFixture fx = make_chain();
    TopologyIndex index = build_index(fx.snapshot, fx.bounds);
    mutate(fx, fx.gen, [](ElectricalObservation& observation) {
      observation.generator = GeneratorState::Off;
      observation.transfer = TransferState::Failed;
    });
    const ClassificationResult result = classify_or_fail(fx, index);
    const ClassifiedFailure* failure =
        find_failure(result, fx.gen, FailureClass::GeneratorStartFailure);
    PFM_REQUIRE(failure != nullptr);
    PFM_CHECK_EQ(failure->primary_reason, ReasonCode::GeneratorStartWindowExpired);
  }
  {
    // The same class is produced for a transfer switch that cannot transfer.
    ChainFixture fx = make_chain();
    TopologyIndex index = build_index(fx.snapshot, fx.bounds);
    mutate(fx, fx.ats, [](ElectricalObservation& observation) {
      observation.transfer = TransferState::Failed;
    });
    const ClassificationResult result = classify_or_fail(fx, index);
    const ClassifiedFailure* failure =
        find_failure(result, fx.ats, FailureClass::GeneratorTransferFailure);
    PFM_REQUIRE(failure != nullptr);
    PFM_CHECK_EQ(failure->primary_reason, ReasonCode::GeneratorTransferFailed);
  }
}

PFM_TEST(classification_contradictory_reports) {
  ChainFixture fx = make_chain();
  TopologyIndex index = build_index(fx.snapshot, fx.bounds);
  ElectricalObservation opposing =
      make_observation(fx.ups, ElementKind::UpsUnit, fx.now, fx.next_sequence);
  opposing.origin = ObservationOrigin::ExternalMeter;
  opposing.reserve = milli_percent_from_value(80000);
  opposing.has_reserve = true;
  opposing.energization = EnergizationState::DeEnergized;
  fx.observations.push_back(opposing);

  const ClassificationResult result = classify_or_fail(fx, index);
  const ClassifiedFailure* failure =
      find_failure(result, fx.ups, FailureClass::AmbiguousElectricalEvidence);
  PFM_REQUIRE(failure != nullptr);
  PFM_CHECK_EQ(failure->primary_reason, ReasonCode::EvidenceContradictory);
  PFM_CHECK(!failure->evidence_current);
  PFM_CHECK(std::find(result.unresolved_elements.begin(), result.unresolved_elements.end(),
                      fx.ups) != result.unresolved_elements.end());
  PFM_CHECK(std::find(result.resolved_elements.begin(), result.resolved_elements.end(), fx.ups) ==
            result.resolved_elements.end());
  // The contradiction is never resolved by preferring one report.
  PFM_CHECK(!has_failure(result, fx.ups, FailureClass::UpsFailure));
  PFM_CHECK(!has_failure(result, fx.ups, FailureClass::UpsReserveInsufficient));
  PFM_CHECK_EQ(result.primary_class(), FailureClass::AmbiguousElectricalEvidence);

  // Two agreeing reports are not a contradiction.
  ChainFixture agreeing = make_chain();
  TopologyIndex agreeing_index = build_index(agreeing.snapshot, agreeing.bounds);
  ElectricalObservation second =
      make_observation(agreeing.ups, ElementKind::UpsUnit, agreeing.now, agreeing.next_sequence);
  second.reserve = milli_percent_from_value(80000);
  second.has_reserve = true;
  agreeing.observations.push_back(second);
  const ClassificationResult agreed = classify_or_fail(agreeing, agreeing_index);
  PFM_CHECK(!has_failure(agreed, agreeing.ups, FailureClass::AmbiguousElectricalEvidence));
}

PFM_TEST(classification_stale_and_expired_evidence) {
  {
    ChainFixture fx = make_chain();
    TopologyIndex index = build_index(fx.snapshot, fx.bounds);
    mutate(fx, fx.feed, [](ElectricalObservation& observation) {
      observation.energization = EnergizationState::DeEnergized;
      observation.observed_at =
          Timestamp::from_value(observation.observed_at.value() - 30000);
    });
    const ClassificationResult result = classify_or_fail(fx, index);
    const ClassifiedFailure* failure =
        find_failure(result, fx.feed, FailureClass::AmbiguousElectricalEvidence);
    PFM_REQUIRE(failure != nullptr);
    PFM_CHECK_EQ(failure->primary_reason, ReasonCode::EvidenceStale);
    PFM_CHECK_EQ(failure->reasons.front().freshness, Freshness::Stale);
    PFM_CHECK(!failure->evidence_current);
    // Stale evidence is never read as health, and never as a concrete failure.
    PFM_CHECK(!has_failure(result, fx.feed, FailureClass::UtilityFeedLoss));
    PFM_CHECK(std::find(result.unresolved_elements.begin(), result.unresolved_elements.end(),
                        fx.feed) != result.unresolved_elements.end());
    PFM_CHECK(std::find(result.resolved_elements.begin(), result.resolved_elements.end(),
                        fx.feed) == result.resolved_elements.end());
  }
  {
    ChainFixture fx = make_chain();
    TopologyIndex index = build_index(fx.snapshot, fx.bounds);
    mutate(fx, fx.feed, [](ElectricalObservation& observation) {
      observation.observed_at =
          Timestamp::from_value(observation.observed_at.value() - 120000);
    });
    const ClassificationResult result = classify_or_fail(fx, index);
    const ClassifiedFailure* failure =
        find_failure(result, fx.feed, FailureClass::AmbiguousElectricalEvidence);
    PFM_REQUIRE(failure != nullptr);
    PFM_CHECK_EQ(failure->primary_reason, ReasonCode::EvidenceExpired);
    PFM_CHECK(!failure->evidence_current);
    // Even a perfectly healthy-looking expired reading is not health: there is
    // no concrete feed failure, only the ambiguity (and the domain promotion).
    PFM_CHECK(!has_failure(result, fx.feed, FailureClass::UtilityFeedLoss));
    for (const auto& candidate : result.failures) {
      PFM_CHECK(candidate.element != fx.feed ||
                candidate.klass == FailureClass::AmbiguousElectricalEvidence);
    }
  }
  {
    // Clock skew never creates freshness.
    ChainFixture fx = make_chain();
    TopologyIndex index = build_index(fx.snapshot, fx.bounds);
    mutate(fx, fx.feed, [](ElectricalObservation& observation) {
      observation.observed_at = Timestamp::from_value(observation.observed_at.value() + 5000);
    });
    const ClassificationResult result = classify_or_fail(fx, index);
    const ClassifiedFailure* failure =
        find_failure(result, fx.feed, FailureClass::AmbiguousElectricalEvidence);
    PFM_REQUIRE(failure != nullptr);
    PFM_CHECK(!failure->evidence_current);
    PFM_CHECK(std::find(result.resolved_elements.begin(), result.resolved_elements.end(),
                        fx.feed) == result.resolved_elements.end());
  }
  {
    // Bad quality is unresolved even when it is fresh.
    ChainFixture fx = make_chain();
    TopologyIndex index = build_index(fx.snapshot, fx.bounds);
    mutate(fx, fx.bus, [](ElectricalObservation& observation) {
      observation.quality = EvidenceQuality::Bad;
    });
    const ClassificationResult result = classify_or_fail(fx, index);
    const ClassifiedFailure* failure =
        find_failure(result, fx.bus, FailureClass::AmbiguousElectricalEvidence);
    PFM_REQUIRE(failure != nullptr);
    PFM_CHECK_EQ(failure->primary_reason, ReasonCode::EvidenceQualityBad);
    PFM_CHECK(!failure->evidence_current);
  }
}

PFM_TEST(classification_collateral_is_not_a_root_cause) {
  // A feed loss cascades: every element below it loses supply through the chain
  // that already explains it, so only the feed is a root cause.
  ChainFixture fx = make_chain();
  TopologyIndex index = build_index(fx.snapshot, fx.bounds);
  for (const auto& element : {fx.feed, fx.brk_feed, fx.sg, fx.bus}) {
    mutate(fx, element, [](ElectricalObservation& observation) {
      observation.energization = EnergizationState::DeEnergized;
    });
  }
  const ClassificationResult result = classify_or_fail(fx, index);
  PFM_CHECK(has_failure(result, fx.feed, FailureClass::UtilityFeedLoss));
  PFM_CHECK(!has_failure(result, fx.brk_feed, FailureClass::BreakerFailure));
  PFM_CHECK(!has_failure(result, fx.sg, FailureClass::SwitchgearFailure));
  PFM_CHECK(!has_failure(result, fx.bus, FailureClass::BusFailure));

  // A failed ancestor also absorbs a downstream loss of supply even when the
  // intermediate evidence still reports the ancestor as energized.
  ChainFixture second = make_chain();
  TopologyIndex second_index = build_index(second.snapshot, second.bounds);
  mutate(second, second.sg, [](ElectricalObservation& observation) {
    observation.breaker = BreakerPosition::Tripped;
  });
  mutate(second, second.bus, [](ElectricalObservation& observation) {
    observation.energization = EnergizationState::DeEnergized;
  });
  const ClassificationResult absorbed = classify_or_fail(second, second_index);
  PFM_CHECK(has_failure(absorbed, second.sg, FailureClass::SwitchgearFailure));
  PFM_CHECK(!has_failure(absorbed, second.bus, FailureClass::BusFailure));
  // The collateral element is captured by the affected scope instead.
  const AffectedScope scope =
      scope_or_fail(absorbed, second_index, second.policy, second.bounds);
  PFM_CHECK(scope.contains(second.bus));
}

PFM_TEST(classification_shared_domain_failure) {
  ChainFixture fx = make_chain();
  TopologyIndex index = build_index(fx.snapshot, fx.bounds);
  mutate(fx, fx.bus, [](ElectricalObservation& observation) {
    observation.energization = EnergizationState::DeEnergized;
  });
  const ClassificationResult result = classify_or_fail(fx, index);
  const ClassifiedFailure* failure =
      find_failure(result, fx.fd_mixed, FailureClass::SharedUpstreamDomainFailure);
  PFM_REQUIRE(failure != nullptr);
  PFM_CHECK_EQ(failure->primary_reason, ReasonCode::DomainMemberFailed);
  PFM_CHECK_EQ(failure->kind, ElementKind::Unknown);
  PFM_CHECK(has_reason(*failure, ReasonCode::DomainMemberFailed));
  PFM_CHECK(has_reason(*failure, ReasonCode::SharedDomainUnresolved));
  PFM_CHECK(failure->evidence_current);

  // The domain is shared because its members sit in more than one downstream
  // power domain, not because the snapshot flagged it.
  PFM_CHECK(!index.domain_is_shared(fx.fd_downstream));
  PFM_CHECK(index.domain_is_shared(fx.fd_mixed));
  PFM_CHECK(index.domain_is_shared(fx.fd_shared));
  PFM_CHECK(!has_failure(result, fx.fd_downstream, FailureClass::SharedUpstreamDomainFailure));

  // A failure below a shared domain is promoted once, not once per member.
  int promoted = 0;
  for (const auto& candidate : result.failures) {
    if (candidate.klass == FailureClass::SharedUpstreamDomainFailure &&
        candidate.element == fx.fd_mixed) {
      promoted += 1;
    }
  }
  PFM_CHECK_EQ(promoted, 1);
}

PFM_TEST(classification_output_order_is_deterministic) {
  ChainFixture fx = make_chain();
  TopologyIndex index = build_index(fx.snapshot, fx.bounds);
  mutate(fx, fx.feed, [](ElectricalObservation& observation) {
    observation.energization = EnergizationState::DeEnergized;
  });
  mutate(fx, fx.pdu, [](ElectricalObservation& observation) {
    observation.energization = EnergizationState::DeEnergized;
  });
  mutate(fx, fx.brk_feed, [](ElectricalObservation& observation) {
    observation.breaker = BreakerPosition::Tripped;
  });
  mutate(fx, fx.ups, [](ElectricalObservation& observation) {
    observation.reserve = milli_percent_from_value(10000);
  });
  ElectricalObservation opposing =
      make_observation(fx.ats, ElementKind::AutomaticTransferSwitch, fx.now, fx.next_sequence);
  opposing.origin = ObservationOrigin::OperatorConsole;
  opposing.transfer = TransferState::OnGenerator;
  fx.observations.push_back(opposing);

  const ClassificationResult first = classify_or_fail(fx, index);
  PFM_REQUIRE(first.failures.size() >= 5);
  PFM_CHECK_EQ(first.failures.front().klass, FailureClass::AmbiguousElectricalEvidence);
  for (std::size_t i = 1; i < first.failures.size(); ++i) {
    const ClassifiedFailure& previous = first.failures[i - 1];
    const ClassifiedFailure& current = first.failures[i];
    const std::uint8_t previous_severity = failure_class_severity(previous.klass);
    const std::uint8_t current_severity = failure_class_severity(current.klass);
    PFM_CHECK(previous_severity >= current_severity);
    if (previous_severity == current_severity) {
      if (previous.klass == current.klass) {
        PFM_CHECK(previous.element < current.element);
      } else {
        PFM_CHECK(static_cast<std::uint8_t>(previous.klass) <
                  static_cast<std::uint8_t>(current.klass));
      }
    }
  }
  // The documented severity ladder.
  PFM_CHECK(failure_class_severity(FailureClass::AmbiguousElectricalEvidence) >
            failure_class_severity(FailureClass::UtilityFeedLoss));
  PFM_CHECK(failure_class_severity(FailureClass::UtilityFeedLoss) >
            failure_class_severity(FailureClass::SwitchgearFailure));
  PFM_CHECK(failure_class_severity(FailureClass::SwitchgearFailure) >
            failure_class_severity(FailureClass::BusFailure));
  PFM_CHECK(failure_class_severity(FailureClass::BusFailure) >
            failure_class_severity(FailureClass::UpsFailure));
  PFM_CHECK(failure_class_severity(FailureClass::UpsFailure) >
            failure_class_severity(FailureClass::PduFailure));
  PFM_CHECK(failure_class_severity(FailureClass::PduFailure) >
            failure_class_severity(FailureClass::GeneratorFailure));
  PFM_CHECK(failure_class_severity(FailureClass::GeneratorFailure) >
            failure_class_severity(FailureClass::UpsReserveInsufficient));
  PFM_CHECK(failure_class_severity(FailureClass::UpsReserveInsufficient) >
            failure_class_severity(FailureClass::BreakerFailure));
  PFM_CHECK(failure_class_severity(FailureClass::BreakerFailure) >
            failure_class_severity(FailureClass::UtilityFeedDegradation));
  // The domain finding amplifies scope; it is never the primary cause.
  PFM_CHECK(failure_class_severity(FailureClass::UtilityFeedDegradation) >
            failure_class_severity(FailureClass::SharedUpstreamDomainFailure));
  PFM_CHECK(failure_class_severity(FailureClass::SharedUpstreamDomainFailure) >
            failure_class_severity(FailureClass::None));
  for (const auto& failure : first.failures) {
    PFM_CHECK_EQ(static_cast<int>(failure_class_severity(failure.klass)),
                 pfmref::model_severity(failure.klass));
  }

  // The same evidence in a different arrival order gives the same list.
  ChainFixture shuffled = fx;
  std::reverse(shuffled.observations.begin(), shuffled.observations.end());
  const ClassificationResult second = classify_or_fail(shuffled, index);
  PFM_REQUIRE(second.failures.size() == first.failures.size());
  for (std::size_t i = 0; i < first.failures.size(); ++i) {
    PFM_CHECK(first.failures[i].klass == second.failures[i].klass);
    PFM_CHECK(first.failures[i].element == second.failures[i].element);
    PFM_CHECK_EQ(first.failures[i].primary_reason, second.failures[i].primary_reason);
  }
  PFM_CHECK(first.unresolved_elements == second.unresolved_elements);
  PFM_CHECK(first.resolved_elements == second.resolved_elements);
}

PFM_TEST(classification_bounds_are_refused) {
  ChainFixture fx = make_chain();
  TopologyIndex index = build_index(fx.snapshot, fx.bounds);
  Bounds tight = fx.bounds;
  tight.max_observations = 3;
  PFM_CHECK_CODE(classify(fx.observations, fx.now, index, fx.policy, tight),
                 StatusCode::BoundsExceeded);
  Bounds unhealthy = fx.bounds;
  unhealthy.max_topology_elements = 2;
  PFM_CHECK_CODE(TopologyIndex::build(fx.snapshot, unhealthy),
                 StatusCode::TopologyBoundsExceeded);
}

// --- affected scope --------------------------------------------------------

PFM_TEST(scope_closure_load_groups_and_isolation_points) {
  ChainFixture fx = make_chain();
  TopologyIndex index = build_index(fx.snapshot, fx.bounds);
  mutate(fx, fx.bus, [](ElectricalObservation& observation) {
    observation.energization = EnergizationState::DeEnergized;
  });
  const ClassificationResult result = classify_or_fail(fx, index);
  const AffectedScope scope = scope_or_fail(result, index, fx.policy, fx.bounds);

  // The impacted set is the whole downstream closure, computed independently.
  const pfmref::ModelTopology model = pfmref::ModelTopology::from(fx.snapshot);
  const std::vector<RefToken> expected_impact = model.closure(fx.bus);
  for (const auto& element : expected_impact) {
    PFM_CHECK_MSG(scope.contains(element), element.to_string().c_str());
  }
  PFM_CHECK(scope.contains(fx.bus));
  PFM_CHECK(scope.contains(fx.load));

  // Load groups are reached directly and through the branch that serves them.
  PFM_CHECK(std::find(scope.load_groups.begin(), scope.load_groups.end(), fx.load) !=
            scope.load_groups.end());
  PFM_CHECK(sorted_unique(scope.load_groups));

  // Isolation points come from the topology: the element's own and the nearest
  // upstream one.
  RefToken own;
  PFM_CHECK(index.isolation_point_for(fx.bus, own));
  PFM_CHECK_EQ(own, fx.brk_bus);
  PFM_CHECK(std::find(scope.isolation_points.begin(), scope.isolation_points.end(), fx.brk_bus) !=
            scope.isolation_points.end());
  PFM_CHECK(std::find(scope.isolation_points.begin(), scope.isolation_points.end(), fx.brk_feed) !=
            scope.isolation_points.end());
  RefToken expected_own;
  PFM_CHECK(model.isolation_point_for(fx.bus, expected_own));
  PFM_CHECK_EQ(expected_own, own);
}

PFM_TEST(scope_unresolved_upstream_and_unevidenced_elements) {
  ChainFixture fx = make_chain();
  TopologyIndex index = build_index(fx.snapshot, fx.bounds);
  mutate(fx, fx.bus, [](ElectricalObservation& observation) {
    observation.energization = EnergizationState::DeEnergized;
  });
  drop_observation(fx, fx.sg);   // an ancestor with no current evidence
  drop_observation(fx, fx.ups);  // an element inside the scope with no evidence
  const ClassificationResult result = classify_or_fail(fx, index);
  const AffectedScope scope = scope_or_fail(result, index, fx.policy, fx.bounds);

  PFM_CHECK(scope.upstream_evidence_unresolved);
  PFM_CHECK(std::find(scope.unresolved_upstream.begin(), scope.unresolved_upstream.end(),
                      fx.sg) != scope.unresolved_upstream.end());
  PFM_CHECK(std::find(scope.unresolved_upstream.begin(), scope.unresolved_upstream.end(),
                      fx.feed) == scope.unresolved_upstream.end());
  PFM_CHECK(std::find(scope.unevidenced_elements.begin(), scope.unevidenced_elements.end(),
                      fx.ups) != scope.unevidenced_elements.end());
  PFM_CHECK(std::find(scope.unevidenced_elements.begin(), scope.unevidenced_elements.end(),
                      fx.sg) == scope.unevidenced_elements.end());
  PFM_CHECK(sorted_unique(scope.unresolved_upstream));
  PFM_CHECK(sorted_unique(scope.unevidenced_elements));
  // A locally healthy descendant is still inside the boundary.
  PFM_CHECK(scope.contains(fx.pdu));

  // The reference model derives the same boundary from the snapshot.
  const pfmref::ModelTopology model = pfmref::ModelTopology::from(fx.snapshot);
  const pfmref::ModelScope expected =
      pfmref::model_scope(model, result.failures, result.resolved_elements,
                          result.unresolved_elements);
  PFM_CHECK(scope.failed_elements == expected.failed_elements);
  PFM_CHECK(scope.impacted_elements == expected.impacted_elements);
  PFM_CHECK(scope.load_groups == expected.load_groups);
  PFM_CHECK(scope.domains == expected.domains);
  PFM_CHECK(scope.isolation_points == expected.isolation_points);
  PFM_CHECK(scope.shared_domains == expected.shared_domains);
  PFM_CHECK(scope.unresolved_upstream == expected.unresolved_upstream);
  PFM_CHECK(scope.unevidenced_elements == expected.unevidenced_elements);
  PFM_CHECK_EQ(scope.shared_domain_impacted, expected.shared_domain_impacted);
  PFM_CHECK_EQ(scope.upstream_evidence_unresolved, expected.upstream_evidence_unresolved);
}

PFM_TEST(scope_lists_are_sorted_unique_and_flagged) {
  ChainFixture fx = make_chain();
  TopologyIndex index = build_index(fx.snapshot, fx.bounds);
  mutate(fx, fx.feed, [](ElectricalObservation& observation) {
    observation.energization = EnergizationState::DeEnergized;
  });
  mutate(fx, fx.bus, [](ElectricalObservation& observation) {
    observation.energization = EnergizationState::DeEnergized;
  });
  mutate(fx, fx.pdu, [](ElectricalObservation& observation) {
    observation.energization = EnergizationState::DeEnergized;
  });
  const ClassificationResult result = classify_or_fail(fx, index);
  const AffectedScope scope = scope_or_fail(result, index, fx.policy, fx.bounds);

  PFM_CHECK(!scope.failed_elements.empty());
  for (std::size_t i = 1; i < scope.failed_elements.size(); ++i) {
    PFM_CHECK(scope.failed_elements[i - 1] < scope.failed_elements[i]);
  }
  for (std::size_t i = 1; i < scope.impacted_elements.size(); ++i) {
    PFM_CHECK(scope.impacted_elements[i - 1] < scope.impacted_elements[i]);
  }
  PFM_CHECK(sorted_unique(scope.failed_elements));
  PFM_CHECK(sorted_unique(scope.impacted_elements));
  PFM_CHECK(sorted_unique(scope.load_groups));
  PFM_CHECK(sorted_unique(scope.domains));
  PFM_CHECK(sorted_unique(scope.isolation_points));
  PFM_CHECK(sorted_unique(scope.shared_domains));
  PFM_CHECK(sorted_unique(scope.unevidenced_elements));
  PFM_CHECK(scope.shared_domain_impacted);
  PFM_CHECK(!scope.shared_domains.empty());

  // Every failed electrical element is impacted and answers contains(); a
  // promoted failure domain is inside the boundary as a domain instead.
  for (const auto& element : scope.failed_elements) {
    if (element.kind() == RefKind::FailureDomain) {
      PFM_CHECK(std::find(scope.domains.begin(), scope.domains.end(), element) !=
                scope.domains.end());
      continue;
    }
    PFM_CHECK_MSG(scope.contains(element), element.to_string().c_str());
  }
  PFM_CHECK_EQ(scope.topology_generation, fx.snapshot.generation);
}

PFM_TEST(scope_bounds_are_refused) {
  ChainFixture fx = make_chain();
  TopologyIndex index = build_index(fx.snapshot, fx.bounds);
  mutate(fx, fx.feed, [](ElectricalObservation& observation) {
    observation.energization = EnergizationState::DeEnergized;
  });
  const ClassificationResult result = classify_or_fail(fx, index);
  Bounds tight = fx.bounds;
  tight.max_scope_elements = 2;
  PFM_CHECK_CODE(resolve_scope(result, index, fx.policy, tight), StatusCode::BoundsExceeded);
  PFM_CHECK_OK(resolve_scope(result, index, fx.policy, fx.bounds));
}

// --- planning --------------------------------------------------------------

void fault_rich(ChainFixture& fx) {
  mutate(fx, fx.brk_feed, [](ElectricalObservation& observation) {
    observation.breaker = BreakerPosition::Tripped;
  });
  mutate(fx, fx.bus, [](ElectricalObservation& observation) {
    observation.energization = EnergizationState::DeEnergized;
  });
  mutate(fx, fx.ups, [](ElectricalObservation& observation) {
    observation.reserve = milli_percent_from_value(10000);
  });
  mutate(fx, fx.gen, [](ElectricalObservation& observation) {
    observation.generator = GeneratorState::Faulted;
  });
  mutate(fx, fx.ats, [](ElectricalObservation& observation) {
    observation.transfer = TransferState::Failed;
  });
}

PlanInputs make_plan_inputs(const ChainFixture& fx, const TopologyIndex& index) {
  PlanInputs inputs;
  inputs.incident = IncidentId::from_value(7);
  inputs.incident_generation = IncidentGeneration::from_value(3);
  inputs.epoch = ControlEpoch::from_value(1);
  inputs.revision = StateRevision::from_value(11);
  inputs.next_plan_generation = PlanGeneration::from_value(1);
  inputs.now = fx.now;
  inputs.policy = fx.policy;
  inputs.bounds = fx.bounds;
  inputs.topology = &index;
  inputs.current_observations = fx.observations;
  inputs.first_request_id = ResponseRequestId::from_value(100);
  inputs.first_attempt_id = AttemptId::from_value(200);
  return inputs;
}

std::vector<std::uint8_t> encode_plan(const ResponsePlan& plan, const Bounds& bounds) {
  codec::Writer writer;
  codec::encode(writer, plan, bounds);
  PFM_CHECK(writer.ok());
  return writer.bytes();
}

bool request_key_less(const ResponseRequest& a, const ResponseRequest& b) {
  if (a.kind != b.kind) {
    return static_cast<std::uint8_t>(a.kind) < static_cast<std::uint8_t>(b.kind);
  }
  if (a.target.owner != b.target.owner) {
    return static_cast<std::uint8_t>(a.target.owner) < static_cast<std::uint8_t>(b.target.owner);
  }
  if (!(a.target.element == b.target.element)) {
    return a.target.element < b.target.element;
  }
  if (a.has_magnitude != b.has_magnitude) {
    return !a.has_magnitude;
  }
  if (a.has_magnitude && !(a.magnitude == b.magnitude)) {
    return a.magnitude < b.magnitude;
  }
  return a.id < b.id;
}

ResponseRequest hand_request(RequestKind kind, RequestOwner owner, const RefToken& target,
                             ElementKind target_kind, bool has_magnitude, std::int64_t magnitude,
                             std::uint64_t id) {
  ResponseRequest request;
  request.id = ResponseRequestId::from_value(id);
  request.kind = kind;
  request.target.element = target;
  request.target.kind = target_kind;
  request.target.owner = owner;
  request.attempt = AttemptId::from_value(id);
  request.attempt_ordinal = 1;
  request.created_at = Timestamp::from_value(1);
  request.has_magnitude = has_magnitude;
  request.magnitude = BasisPoints::from_value(magnitude);
  return request;
}

PFM_TEST(plan_is_deterministic_and_byte_identical) {
  ChainFixture fx = make_chain();
  fault_rich(fx);
  TopologyIndex index = build_index(fx.snapshot, fx.bounds);
  PlanInputs inputs = make_plan_inputs(fx, index);

  auto first = plan_response(inputs);
  PFM_REQUIRE(first.ok());
  auto second = plan_response(inputs);
  PFM_REQUIRE(second.ok());

  PFM_CHECK(first.value().has_failure());
  PFM_CHECK(first.value().requests.size() > 2);
  PFM_CHECK(first.value().fingerprint.is_set());
  PFM_CHECK_EQ(first.value().fingerprint, second.value().fingerprint);
  PFM_CHECK_EQ(first.value().primary_failure, second.value().primary_failure);
  PFM_CHECK_EQ(first.value().primary_element, second.value().primary_element);
  PFM_CHECK_EQ(first.value().generation, inputs.next_plan_generation);
  PFM_CHECK_EQ(first.value().incident, inputs.incident);
  PFM_CHECK_EQ(first.value().incident_generation, inputs.incident_generation);
  PFM_CHECK_EQ(first.value().epoch, inputs.epoch);
  PFM_CHECK_EQ(first.value().topology_generation, fx.snapshot.generation);

  PFM_REQUIRE(first.value().requests.size() == second.value().requests.size());
  for (std::size_t i = 0; i < first.value().requests.size(); ++i) {
    const ResponseRequest& a = first.value().requests[i];
    const ResponseRequest& b = second.value().requests[i];
    PFM_CHECK_EQ(a.id, b.id);
    PFM_CHECK_EQ(a.attempt, b.attempt);
    PFM_CHECK_EQ(a.idempotency, b.idempotency);
    PFM_CHECK_EQ(a.fingerprint, b.fingerprint);
    PFM_CHECK_EQ(a.kind, b.kind);
    PFM_CHECK_EQ(a.required_proof, b.required_proof);
    PFM_CHECK(a.target.element == b.target.element);
    PFM_CHECK_EQ(a.state, RequestState::Planned);
    PFM_CHECK(a.id.is_set());
    PFM_CHECK(a.attempt.is_set());
    PFM_CHECK(a.idempotency == IdempotencyKey::from_fingerprint(a.fingerprint));
  }

  const std::vector<std::uint8_t> bytes_a = encode_plan(first.value(), fx.bounds);
  const std::vector<std::uint8_t> bytes_b = encode_plan(second.value(), fx.bounds);
  PFM_CHECK(!bytes_a.empty());
  PFM_CHECK(bytes_a == bytes_b);

  // The canonical encoding is independent of the arrival order of evidence.
  PlanInputs reordered = inputs;
  std::reverse(reordered.current_observations.begin(), reordered.current_observations.end());
  auto third = plan_response(reordered);
  PFM_REQUIRE(third.ok());
  PFM_CHECK_EQ(third.value().fingerprint, first.value().fingerprint);
  PFM_CHECK(encode_plan(third.value(), fx.bounds) == bytes_a);
}

PFM_TEST(plan_request_ordering) {
  ChainFixture fx = make_chain();
  TopologyIndex index = build_index(fx.snapshot, fx.bounds);

  // The documented order is (kind, owner, target, magnitude), with an absent
  // magnitude before a present one and the identity as the final tie break.
  std::vector<ResponseRequest> requests;
  requests.push_back(hand_request(RequestKind::VerifyDeEnergization, RequestOwner::PowerControlPlane,
                                  fx.bus, ElementKind::Bus, false, 0, 9));
  requests.push_back(hand_request(RequestKind::IsolateElement, RequestOwner::PduControl, fx.pdu,
                                  ElementKind::Pdu, false, 0, 2));
  requests.push_back(hand_request(RequestKind::IsolateElement, RequestOwner::FeedAuthority, fx.bus,
                                  ElementKind::Bus, false, 0, 5));
  requests.push_back(hand_request(RequestKind::PreserveReserve, RequestOwner::UpsControl, fx.ups,
                                  ElementKind::UpsUnit, true, 4000, 1));
  requests.push_back(hand_request(RequestKind::PreserveReserve, RequestOwner::UpsControl, fx.ups,
                                  ElementKind::UpsUnit, true, 1000, 7));
  requests.push_back(hand_request(RequestKind::PreserveReserve, RequestOwner::UpsControl, fx.ups,
                                  ElementKind::UpsUnit, false, 0, 3));
  requests.push_back(hand_request(RequestKind::IsolateElement, RequestOwner::FeedAuthority, fx.bus,
                                  ElementKind::Bus, true, 10, 4));
  requests.push_back(hand_request(RequestKind::IsolateElement, RequestOwner::FeedAuthority, fx.bus,
                                  ElementKind::Bus, true, 5, 6));
  const std::vector<ResponseRequest> original = requests;

  sort_requests(requests);
  PFM_CHECK(std::is_sorted(requests.begin(), requests.end(), request_key_less));
  PFM_CHECK(requests.size() == original.size());

  // Every element is present exactly once.
  for (const auto& candidate : original) {
    int found = 0;
    for (const auto& sorted : requests) {
      if (sorted.id == candidate.id) {
        found += 1;
      }
    }
    PFM_CHECK_EQ(found, 1);
  }
  // Kind dominates everything else.
  for (std::size_t i = 1; i < requests.size(); ++i) {
    PFM_CHECK(static_cast<std::uint8_t>(requests[i - 1].kind) <=
              static_cast<std::uint8_t>(requests[i].kind));
  }
  // Within a kind, the owner decides.
  PFM_CHECK_EQ(requests.front().kind, RequestKind::IsolateElement);
  PFM_CHECK_EQ(requests.front().target.owner, RequestOwner::FeedAuthority);
  // The magnitude tie break is exercised inside PreserveReserve.
  bool saw_small = false;
  for (const auto& request : requests) {
    if (request.kind != RequestKind::PreserveReserve) {
      continue;
    }
    if (!saw_small && request.has_magnitude) {
      if (request.magnitude.value() == 1000) {
        saw_small = true;
      }
    } else if (saw_small && request.has_magnitude) {
      PFM_CHECK(request.magnitude.value() >= 1000);
    }
  }
  PFM_CHECK(saw_small);

  // A planned list obeys the same order.
  fault_rich(fx);
  TopologyIndex rich_index = build_index(fx.snapshot, fx.bounds);
  PlanInputs inputs = make_plan_inputs(fx, rich_index);
  auto plan = plan_response(inputs);
  PFM_REQUIRE(plan.ok());
  PFM_CHECK(std::is_sorted(plan.value().requests.begin(), plan.value().requests.end(),
                           request_key_less));
  PFM_CHECK(plan.value().requests.size() > 3);
}

PFM_TEST(plan_carries_forward_open_requests) {
  ChainFixture fx = make_chain();
  fault_rich(fx);
  TopologyIndex index = build_index(fx.snapshot, fx.bounds);
  PlanInputs inputs = make_plan_inputs(fx, index);

  auto first = plan_response(inputs);
  PFM_REQUIRE(first.ok());
  PFM_REQUIRE(first.value().requests.size() > 2);
  const std::vector<ResponseRequest> planned = first.value().requests;

  std::vector<ResponseRequest> open = planned;
  for (auto& request : open) {
    request.state = RequestState::Acknowledged;
    request.has_acknowledged_at = true;
    request.acknowledged_at = fx.now;
  }

  PlanInputs next = inputs;
  next.next_plan_generation = PlanGeneration::from_value(2);
  next.open_requests = open;
  auto second = plan_response(next);
  PFM_REQUIRE(second.ok());

  // The same operations, carried forward under their original identity: not
  // re-planned, not duplicated, not given a fresh id or attempt.
  PFM_CHECK_EQ(second.value().requests.size(), planned.size());
  PFM_CHECK_EQ(second.value().generation, PlanGeneration::from_value(2));
  for (const auto& carried : second.value().requests) {
    const ResponseRequest* original_request = nullptr;
    for (const auto& candidate : planned) {
      if (candidate.fingerprint == carried.fingerprint) {
        original_request = &candidate;
      }
    }
    PFM_REQUIRE(original_request != nullptr);
    PFM_CHECK_EQ(carried.id, original_request->id);
    PFM_CHECK_EQ(carried.attempt, original_request->attempt);
    PFM_CHECK_EQ(carried.idempotency, original_request->idempotency);
    PFM_CHECK_EQ(carried.state, RequestState::Acknowledged);
    PFM_CHECK_EQ(carried.plan_generation, PlanGeneration::from_value(2));
    // A carried request is not re-identified: its identity is one the first
    // plan already allocated.
    bool known_identity = false;
    for (const auto& candidate : planned) {
      if (candidate.id == carried.id) {
        known_identity = true;
      }
    }
    PFM_CHECK(known_identity);
  }
  // Every identity in the second plan is one the first plan already had, so
  // nothing was planned as new work.
  for (const auto& request : second.value().requests) {
    bool known_identity = false;
    for (const auto& candidate : planned) {
      if (candidate.id == request.id) {
        known_identity = true;
      }
    }
    PFM_CHECK(known_identity);
  }
  // A closed request is not carried forward; it is planned again as new work.
  PlanInputs redo = inputs;
  for (auto& request : open) {
    request.state = RequestState::Verified;
    request.required_proof = ProofKind::None;
  }
  redo.open_requests = open;
  auto third = plan_response(redo);
  PFM_REQUIRE(third.ok());
  PFM_CHECK_EQ(third.value().requests.size(), planned.size());
  for (const auto& request : third.value().requests) {
    PFM_CHECK_EQ(request.state, RequestState::Planned);
    PFM_CHECK(request.id.value() >= inputs.first_request_id.value());
  }
}

PFM_TEST(plan_bounds_are_refused_not_truncated) {
  ChainFixture fx = make_chain();
  fault_rich(fx);
  TopologyIndex index = build_index(fx.snapshot, fx.bounds);
  PlanInputs inputs = make_plan_inputs(fx, index);

  PlanInputs tight_requests = inputs;
  tight_requests.bounds.max_requests_per_plan = 1;
  PFM_CHECK_CODE(plan_response(tight_requests), StatusCode::ResourceExhausted);

  PlanInputs tight_scope = inputs;
  tight_scope.bounds.max_scope_elements = 2;
  PFM_CHECK_CODE(plan_response(tight_scope), StatusCode::BoundsExceeded);

  // The declared input sets are bounded before anything is derived from them,
  // and an over-long list is refused rather than quietly shortened.
  ChainFixture clean = make_chain();
  TopologyIndex clean_index = build_index(clean.snapshot, clean.bounds);
  PlanInputs tight_unevidenced = make_plan_inputs(clean, clean_index);
  tight_unevidenced.bounds.max_scope_elements = 2;
  tight_unevidenced.unevidenced_elements = {clean.feed, clean.bus, clean.pdu};
  PFM_CHECK_CODE(plan_response(tight_unevidenced), StatusCode::BoundsExceeded);

  PlanInputs tight_unhealthy = make_plan_inputs(clean, clean_index);
  tight_unhealthy.bounds.max_scope_elements = 2;
  tight_unhealthy.unhealthy_scope_elements = {clean.feed, clean.bus, clean.pdu};
  PFM_CHECK_CODE(plan_response(tight_unhealthy), StatusCode::BoundsExceeded);

  PlanInputs no_topology = inputs;
  no_topology.topology = nullptr;
  PFM_CHECK_CODE(plan_response(no_topology), StatusCode::PreconditionFailed);

  // The refused plan is not published, and the same inputs still plan whole.
  PFM_CHECK_OK(plan_response(inputs));
}

// --- recovery gates --------------------------------------------------------

ResponsePlan base_plan(const ChainFixture& fx) {
  ResponsePlan plan;
  plan.generation = PlanGeneration::from_value(4);
  plan.incident = IncidentId::from_value(7);
  plan.incident_generation = IncidentGeneration::from_value(3);
  plan.topology_generation = TopologyGeneration::from_value(1);
  plan.policy_generation = PolicyGeneration::from_value(1);
  plan.epoch = ControlEpoch::from_value(1);
  plan.decided_at = fx.now;
  plan.scope.topology_generation = TopologyGeneration::from_value(1);
  return plan;
}

RecoveryInputs recovery_inputs(const ChainFixture& fx, const ResponsePlan& plan) {
  RecoveryInputs inputs;
  inputs.now = fx.now;
  inputs.policy = fx.policy;
  inputs.bounds = fx.bounds;
  inputs.plan = &plan;
  inputs.current_observations = fx.observations;
  inputs.has_stable_since = true;
  inputs.stable_since =
      Timestamp::from_value(fx.now.value() - fx.policy.stability_dwell.value());
  inputs.operator_authorized = true;
  inputs.authorized_generation = IncidentGeneration::from_value(3);
  inputs.incident_generation = IncidentGeneration::from_value(3);
  return inputs;
}

void add_failure(ResponsePlan& plan, const RefToken& element, ElementKind kind,
                 FailureClass klass) {
  ClassifiedFailure failure;
  failure.klass = klass;
  failure.element = element;
  failure.kind = kind;
  failure.evidence_current = true;
  plan.failures.push_back(failure);
  plan.primary_failure = klass;
  plan.primary_element = element;
}

void add_request(ResponsePlan& plan, RequestKind kind, const RefToken& target,
                 ElementKind target_kind, ProofKind proof, RequestState state, std::uint64_t id) {
  ResponseRequest request = hand_request(kind, owner_for_kind(kind), target, target_kind, false, 0,
                                         id);
  request.required_proof = proof;
  request.state = state;
  plan.requests.push_back(request);
}

PFM_TEST(recovery_gate_fault_cleared) {
  ChainFixture fx = make_chain();
  {
    // Baseline: nothing is classified, nothing blocks.
    const ResponsePlan plan = base_plan(fx);
    const RecoveryAssessment assessment = assess_or_fail(recovery_inputs(fx, plan));
    PFM_CHECK(gate_satisfied(assessment, RecoveryGate::FaultCleared));
    PFM_CHECK(assessment.eligible);
  }
  {
    // The classified failure is still observed.
    ResponsePlan plan = base_plan(fx);
    add_failure(plan, fx.bus, ElementKind::Bus, FailureClass::BusFailure);
    plan.scope.impacted_elements = {fx.bus};
    mutate(fx, fx.bus, [](ElectricalObservation& observation) {
      observation.energization = EnergizationState::DeEnergized;
    });
    const RecoveryAssessment assessment = assess_or_fail(recovery_inputs(fx, plan));
    PFM_CHECK(!gate_satisfied(assessment, RecoveryGate::FaultCleared));
    PFM_CHECK(!assessment.eligible);
    PFM_CHECK_EQ(gate_reason(assessment, RecoveryGate::FaultCleared),
                 ReasonCode::ElementDeEnergizedWhileUpstreamEnergized);
    PFM_CHECK(assessment.find_gate(RecoveryGate::FaultCleared)->subject == fx.bus);
  }
  {
    // A healthy observation clears the same failure.
    ResponsePlan plan = base_plan(fx);
    add_failure(plan, fx.ups, ElementKind::UpsUnit, FailureClass::UpsFailure);
    const RecoveryAssessment assessment = assess_or_fail(recovery_inputs(fx, plan));
    PFM_CHECK(gate_satisfied(assessment, RecoveryGate::FaultCleared));
    PFM_CHECK(assessment.eligible);
  }
  {
    // A failure with no observation at all is not cleared.
    ResponsePlan plan = base_plan(fx);
    add_failure(plan, fx.pdu, ElementKind::Pdu, FailureClass::PduFailure);
    drop_observation(fx, fx.pdu);
    const RecoveryAssessment assessment = assess_or_fail(recovery_inputs(fx, plan));
    PFM_CHECK(!gate_satisfied(assessment, RecoveryGate::FaultCleared));
    PFM_CHECK_EQ(gate_reason(assessment, RecoveryGate::FaultCleared), ReasonCode::EvidenceMissing);
  }
  {
    // The cumulative incident scope is the question, not only the newest plan:
    // an element that the newest plan no longer classifies still blocks until it
    // reports healthy again.
    const ResponsePlan plan = base_plan(fx);
    RecoveryInputs inputs = recovery_inputs(fx, plan);
    inputs.incident_scope = {fx.feed, fx.ups};
    RecoveryAssessment assessment = assess_or_fail(inputs);
    PFM_CHECK(gate_satisfied(assessment, RecoveryGate::FaultCleared));
    PFM_CHECK(assessment.eligible);

    drop_observation(fx, fx.ups);
    inputs = recovery_inputs(fx, plan);
    inputs.incident_scope = {fx.feed, fx.ups};
    assessment = assess_or_fail(inputs);
    PFM_CHECK(!gate_satisfied(assessment, RecoveryGate::FaultCleared));
    PFM_CHECK_EQ(gate_reason(assessment, RecoveryGate::FaultCleared), ReasonCode::EvidenceMissing);
    PFM_CHECK(!assessment.eligible);

    // A de-energized scope element is not healthy either.
    mutate(fx, fx.feed, [](ElectricalObservation& observation) {
      observation.energization = EnergizationState::DeEnergized;
    });
    inputs = recovery_inputs(fx, plan);
    inputs.incident_scope = {fx.feed, fx.ups};
    assessment = assess_or_fail(inputs);
    PFM_CHECK(!gate_satisfied(assessment, RecoveryGate::FaultCleared));
    PFM_CHECK(!assessment.eligible);
  }
}

PFM_TEST(recovery_gate_isolation_verified) {
  ChainFixture fx = make_chain();
  ResponsePlan plan = base_plan(fx);
  IsolationRequirement requirement;
  requirement.element = fx.bus;
  requirement.isolation_point = fx.brk_bus;
  requirement.has_isolation_point = true;
  plan.isolations.push_back(requirement);

  {
    // Nothing is proven yet.
    const RecoveryAssessment assessment = assess_or_fail(recovery_inputs(fx, plan));
    PFM_CHECK(!gate_satisfied(assessment, RecoveryGate::IsolationVerified));
    PFM_CHECK(!assessment.eligible);
  }
  {
    // An acknowledgement is not proof, even with the right proof kind.
    ResponsePlan acked = plan;
    add_request(acked, RequestKind::IsolateElement, fx.bus, ElementKind::Bus, ProofKind::Isolation,
                RequestState::Acknowledged, 1);
    const RecoveryAssessment assessment = assess_or_fail(recovery_inputs(fx, acked));
    PFM_CHECK(!gate_satisfied(assessment, RecoveryGate::IsolationVerified));
    PFM_CHECK(!assessment.eligible);
  }
  {
    // An observation is not a verification either.
    ResponsePlan observed = plan;
    add_request(observed, RequestKind::IsolateElement, fx.bus, ElementKind::Bus,
                ProofKind::Isolation, RequestState::Observed, 1);
    const RecoveryAssessment assessment = assess_or_fail(recovery_inputs(fx, observed));
    PFM_CHECK(!gate_satisfied(assessment, RecoveryGate::IsolationVerified));
  }
  {
    // A verified request for a different proof does not prove isolation.
    ResponsePlan wrong_proof = plan;
    add_request(wrong_proof, RequestKind::VerifyDeEnergization, fx.bus, ElementKind::Bus,
                ProofKind::DeEnergization, RequestState::Verified, 1);
    const RecoveryAssessment assessment = assess_or_fail(recovery_inputs(fx, wrong_proof));
    PFM_CHECK(!gate_satisfied(assessment, RecoveryGate::IsolationVerified));
    PFM_CHECK(gate_satisfied(assessment, RecoveryGate::DeEnergizationVerified));
  }
  {
    // The isolation point itself may be the verified target.
    ResponsePlan on_point = plan;
    add_request(on_point, RequestKind::IsolateElement, fx.brk_bus, ElementKind::Breaker,
                ProofKind::Isolation, RequestState::Verified, 1);
    add_request(on_point, RequestKind::VerifyDeEnergization, fx.brk_bus, ElementKind::Breaker,
                ProofKind::DeEnergization, RequestState::Verified, 2);
    const RecoveryAssessment assessment = assess_or_fail(recovery_inputs(fx, on_point));
    PFM_CHECK(gate_satisfied(assessment, RecoveryGate::IsolationVerified));
    PFM_CHECK(gate_satisfied(assessment, RecoveryGate::DeEnergizationVerified));
    PFM_CHECK(assessment.eligible);
  }
  {
    // An isolation requirement with no isolation point can never be satisfied.
    ResponsePlan unachievable = base_plan(fx);
    IsolationRequirement missing;
    missing.element = fx.feed;
    missing.has_isolation_point = false;
    unachievable.isolations.push_back(missing);
    const RecoveryAssessment assessment = assess_or_fail(recovery_inputs(fx, unachievable));
    PFM_CHECK(!gate_satisfied(assessment, RecoveryGate::IsolationVerified));
    PFM_CHECK(!assessment.eligible);
  }
  {
    // The whole gate is waived only by policy, never by convenience.
    ResponsePlan waived = base_plan(fx);
    waived.isolations.push_back(requirement);
    RecoveryInputs inputs = recovery_inputs(fx, waived);
    inputs.policy.require_verified_isolation_for_recovery = false;
    const RecoveryAssessment assessment = assess_or_fail(inputs);
    PFM_CHECK(gate_satisfied(assessment, RecoveryGate::IsolationVerified));
  }
}

PFM_TEST(recovery_gate_de_energization_verified) {
  ChainFixture fx = make_chain();
  ResponsePlan plan = base_plan(fx);
  IsolationRequirement requirement;
  requirement.element = fx.pdu;
  requirement.isolation_point = fx.brk_pdu;
  requirement.has_isolation_point = true;
  plan.isolations.push_back(requirement);

  RecoveryAssessment assessment = assess_or_fail(recovery_inputs(fx, plan));
  PFM_CHECK(!gate_satisfied(assessment, RecoveryGate::DeEnergizationVerified));
  PFM_CHECK(!assessment.eligible);

  ResponsePlan proven = plan;
  add_request(proven, RequestKind::VerifyDeEnergization, fx.pdu, ElementKind::Pdu,
              ProofKind::DeEnergization, RequestState::Verified, 1);
  assessment = assess_or_fail(recovery_inputs(fx, proven));
  PFM_CHECK(gate_satisfied(assessment, RecoveryGate::DeEnergizationVerified));
  // Isolation is proven separately and is still open here.
  PFM_CHECK(!gate_satisfied(assessment, RecoveryGate::IsolationVerified));
  PFM_CHECK(!assessment.eligible);
}

PFM_TEST(recovery_gate_breaker_state_proven) {
  ChainFixture fx = make_chain();
  ResponsePlan plan = base_plan(fx);
  add_failure(plan, fx.brk_feed, ElementKind::Breaker, FailureClass::BreakerFailure);
  plan.scope.impacted_elements = {fx.brk_feed};

  RecoveryAssessment assessment = assess_or_fail(recovery_inputs(fx, plan));
  PFM_CHECK(gate_satisfied(assessment, RecoveryGate::BreakerStateProven));
  PFM_CHECK(assessment.eligible);

  mutate(fx, fx.brk_feed, [](ElectricalObservation& observation) {
    observation.breaker = BreakerPosition::Unknown;
  });
  assessment = assess_or_fail(recovery_inputs(fx, plan));
  PFM_CHECK(!gate_satisfied(assessment, RecoveryGate::BreakerStateProven));
  PFM_CHECK_EQ(gate_reason(assessment, RecoveryGate::BreakerStateProven),
               ReasonCode::EvidenceMissing);
  PFM_CHECK(!assessment.eligible);

  mutate(fx, fx.brk_feed, [](ElectricalObservation& observation) {
    observation.breaker = BreakerPosition::Tripped;
  });
  assessment = assess_or_fail(recovery_inputs(fx, plan));
  PFM_CHECK(!gate_satisfied(assessment, RecoveryGate::BreakerStateProven));
  PFM_CHECK_EQ(gate_reason(assessment, RecoveryGate::BreakerStateProven),
               ReasonCode::BreakerTripped);

  // No evidence at all is not proof either.
  drop_observation(fx, fx.brk_feed);
  assessment = assess_or_fail(recovery_inputs(fx, plan));
  PFM_CHECK(!gate_satisfied(assessment, RecoveryGate::BreakerStateProven));
  PFM_CHECK(!assessment.eligible);
}

PFM_TEST(recovery_gate_transfer_stable) {
  ChainFixture fx = make_chain();
  ResponsePlan plan = base_plan(fx);
  plan.scope.impacted_elements = {fx.ups, fx.ats};

  RecoveryAssessment assessment = assess_or_fail(recovery_inputs(fx, plan));
  PFM_CHECK(gate_satisfied(assessment, RecoveryGate::TransferStable));
  PFM_CHECK(assessment.eligible);

  mutate(fx, fx.ups, [](ElectricalObservation& observation) {
    observation.transfer = TransferState::Transferring;
  });
  assessment = assess_or_fail(recovery_inputs(fx, plan));
  PFM_CHECK(!gate_satisfied(assessment, RecoveryGate::TransferStable));
  PFM_CHECK_EQ(gate_reason(assessment, RecoveryGate::TransferStable),
               ReasonCode::GeneratorTransferFailed);
  PFM_CHECK(!assessment.eligible);

  mutate(fx, fx.ups, [](ElectricalObservation& observation) {
    observation.transfer = TransferState::Unknown;
  });
  assessment = assess_or_fail(recovery_inputs(fx, plan));
  PFM_CHECK(!gate_satisfied(assessment, RecoveryGate::TransferStable));

  mutate(fx, fx.ups, [](ElectricalObservation& observation) {
    observation.transfer = TransferState::OnUtility;
  });
  mutate(fx, fx.ats, [](ElectricalObservation& observation) {
    observation.transfer = TransferState::Failed;
  });
  assessment = assess_or_fail(recovery_inputs(fx, plan));
  PFM_CHECK(!gate_satisfied(assessment, RecoveryGate::TransferStable));
}

PFM_TEST(recovery_gate_generation_stable) {
  ChainFixture fx = make_chain();
  ResponsePlan plan = base_plan(fx);
  plan.scope.impacted_elements = {fx.gen};

  RecoveryAssessment assessment = assess_or_fail(recovery_inputs(fx, plan));
  PFM_CHECK(gate_satisfied(assessment, RecoveryGate::GenerationStable));
  PFM_CHECK(assessment.eligible);

  mutate(fx, fx.gen, [](ElectricalObservation& observation) {
    observation.generator = GeneratorState::Running;
  });
  assessment = assess_or_fail(recovery_inputs(fx, plan));
  PFM_CHECK(gate_satisfied(assessment, RecoveryGate::GenerationStable));

  mutate(fx, fx.gen, [](ElectricalObservation& observation) {
    observation.generator = GeneratorState::Cranking;
  });
  assessment = assess_or_fail(recovery_inputs(fx, plan));
  PFM_CHECK(!gate_satisfied(assessment, RecoveryGate::GenerationStable));
  PFM_CHECK_EQ(gate_reason(assessment, RecoveryGate::GenerationStable),
               ReasonCode::GeneratorNotSynchronized);
  PFM_CHECK(!assessment.eligible);

  mutate(fx, fx.gen, [](ElectricalObservation& observation) {
    observation.generator = GeneratorState::Synchronized;
    observation.transfer = TransferState::Failed;
  });
  assessment = assess_or_fail(recovery_inputs(fx, plan));
  PFM_CHECK(!gate_satisfied(assessment, RecoveryGate::GenerationStable));
  PFM_CHECK_EQ(gate_reason(assessment, RecoveryGate::GenerationStable),
               ReasonCode::GeneratorTransferFailed);
}

PFM_TEST(recovery_gate_reserve_restored) {
  ChainFixture fx = make_chain();
  ResponsePlan plan = base_plan(fx);
  plan.scope.impacted_elements = {fx.ups};

  RecoveryAssessment assessment = assess_or_fail(recovery_inputs(fx, plan));
  PFM_CHECK(gate_satisfied(assessment, RecoveryGate::ReserveRestored));
  PFM_CHECK(assessment.eligible);

  mutate(fx, fx.ups, [](ElectricalObservation& observation) {
    observation.reserve = milli_percent_from_value(40000);
  });
  assessment = assess_or_fail(recovery_inputs(fx, plan));
  PFM_CHECK(!gate_satisfied(assessment, RecoveryGate::ReserveRestored));
  PFM_CHECK_EQ(gate_reason(assessment, RecoveryGate::ReserveRestored),
               ReasonCode::UpsReserveBelowFloor);
  PFM_CHECK(!assessment.eligible);

  mutate(fx, fx.ups, [](ElectricalObservation& observation) {
    observation.has_reserve = false;
  });
  assessment = assess_or_fail(recovery_inputs(fx, plan));
  PFM_CHECK(!gate_satisfied(assessment, RecoveryGate::ReserveRestored));
  PFM_CHECK_EQ(gate_reason(assessment, RecoveryGate::ReserveRestored),
               ReasonCode::EvidenceMissing);

  // A protected obligation can raise the floor above the policy default.
  mutate(fx, fx.ups, [](ElectricalObservation& observation) {
    observation.reserve = milli_percent_from_value(80000);
    observation.has_reserve = true;
  });
  ProtectedObligation obligation;
  obligation.id = ObligationId::from_value(1);
  obligation.target = fx.ups;
  obligation.protection = ProtectionClass::ServiceLevel;
  obligation.status = ObligationStatus::Satisfied;
  obligation.has_report = true;
  obligation.reported_at = fx.now;
  obligation.reporting_authority = make_ref(RefKind::Controller, "facility-obligations");
  obligation.has_reserve_floor = true;
  obligation.reserve_floor = milli_percent_from_value(90000);
  RecoveryInputs inputs = recovery_inputs(fx, plan);
  inputs.obligations = {obligation};
  assessment = assess_or_fail(inputs);
  PFM_CHECK(!gate_satisfied(assessment, RecoveryGate::ReserveRestored));
  PFM_CHECK(!assessment.eligible);

  // The gate is waived only by policy.
  RecoveryInputs waived = recovery_inputs(fx, plan);
  waived.obligations = {obligation};
  waived.policy.require_reserve_floor_for_recovery = false;
  assessment = assess_or_fail(waived);
  PFM_CHECK(gate_satisfied(assessment, RecoveryGate::ReserveRestored));
}

PFM_TEST(recovery_gate_shared_domain_resolved) {
  ChainFixture fx = make_chain();
  ResponsePlan plan = base_plan(fx);

  RecoveryAssessment assessment = assess_or_fail(recovery_inputs(fx, plan));
  PFM_CHECK(gate_satisfied(assessment, RecoveryGate::SharedDomainResolved));
  PFM_CHECK(assessment.eligible);

  plan.scope.shared_domains = {fx.fd_shared};
  assessment = assess_or_fail(recovery_inputs(fx, plan));
  PFM_CHECK(!gate_satisfied(assessment, RecoveryGate::SharedDomainResolved));
  PFM_CHECK_EQ(gate_reason(assessment, RecoveryGate::SharedDomainResolved),
               ReasonCode::SharedDomainUnresolved);
  PFM_CHECK(assessment.find_gate(RecoveryGate::SharedDomainResolved)->subject == fx.fd_shared);
  PFM_CHECK(!assessment.eligible);
  PFM_CHECK(std::find(assessment.blocking_elements.begin(), assessment.blocking_elements.end(),
                      fx.fd_shared) != assessment.blocking_elements.end());

  RecoveryInputs waived = recovery_inputs(fx, plan);
  waived.policy.require_shared_domain_resolution_for_recovery = false;
  assessment = assess_or_fail(waived);
  PFM_CHECK(gate_satisfied(assessment, RecoveryGate::SharedDomainResolved));
}

PFM_TEST(recovery_gate_upstream_evidence_current) {
  ChainFixture fx = make_chain();
  ResponsePlan plan = base_plan(fx);

  RecoveryAssessment assessment = assess_or_fail(recovery_inputs(fx, plan));
  PFM_CHECK(gate_satisfied(assessment, RecoveryGate::UpstreamEvidenceCurrent));
  PFM_CHECK(assessment.eligible);

  plan.scope.unresolved_upstream = {fx.feed, fx.sg};
  assessment = assess_or_fail(recovery_inputs(fx, plan));
  PFM_CHECK(!gate_satisfied(assessment, RecoveryGate::UpstreamEvidenceCurrent));
  PFM_CHECK_EQ(gate_reason(assessment, RecoveryGate::UpstreamEvidenceCurrent),
               ReasonCode::UpstreamEvidenceUnresolved);
  PFM_CHECK(assessment.find_gate(RecoveryGate::UpstreamEvidenceCurrent)->subject == fx.feed);
  PFM_CHECK(!assessment.eligible);
}

PFM_TEST(recovery_gate_obligations_satisfied) {
  ChainFixture fx = make_chain();
  const ResponsePlan plan = base_plan(fx);

  ProtectedObligation obligation;
  obligation.id = ObligationId::from_value(1);
  obligation.target = fx.load;
  obligation.protection = ProtectionClass::ServiceLevel;
  obligation.status = ObligationStatus::Satisfied;
  obligation.has_report = true;
  obligation.reported_at = fx.now;
  obligation.reporting_authority = make_ref(RefKind::Controller, "facility-obligations");

  RecoveryInputs inputs = recovery_inputs(fx, plan);
  inputs.obligations = {obligation};
  RecoveryAssessment assessment = assess_or_fail(inputs);
  PFM_CHECK(gate_satisfied(assessment, RecoveryGate::ObligationsSatisfied));
  PFM_CHECK(assessment.eligible);

  ProtectedObligation violated = obligation;
  violated.status = ObligationStatus::Violated;
  inputs.obligations = {violated};
  assessment = assess_or_fail(inputs);
  PFM_CHECK(!gate_satisfied(assessment, RecoveryGate::ObligationsSatisfied));
  PFM_CHECK(!assessment.eligible);

  ProtectedObligation at_risk = obligation;
  at_risk.status = ObligationStatus::AtRisk;
  inputs.obligations = {at_risk};
  assessment = assess_or_fail(inputs);
  PFM_CHECK(!gate_satisfied(assessment, RecoveryGate::ObligationsSatisfied));

  ProtectedObligation unknown = obligation;
  unknown.status = ObligationStatus::Unknown;
  inputs.obligations = {unknown};
  assessment = assess_or_fail(inputs);
  PFM_CHECK(!gate_satisfied(assessment, RecoveryGate::ObligationsSatisfied));

  ProtectedObligation unreported = obligation;
  unreported.status = ObligationStatus::Unreported;
  unreported.has_report = false;
  inputs.obligations = {unreported};
  assessment = assess_or_fail(inputs);
  PFM_CHECK(!gate_satisfied(assessment, RecoveryGate::ObligationsSatisfied));

  // A satisfied report that is no longer current is not a satisfied obligation.
  ProtectedObligation expired = obligation;
  expired.reported_at = Timestamp::from_value(fx.now.value() - 120000);
  inputs.obligations = {expired};
  assessment = assess_or_fail(inputs);
  PFM_CHECK(!gate_satisfied(assessment, RecoveryGate::ObligationsSatisfied));
  PFM_CHECK(!assessment.eligible);
}

PFM_TEST(recovery_gate_evidence_current) {
  ChainFixture fx = make_chain();
  ResponsePlan plan = base_plan(fx);

  RecoveryAssessment assessment = assess_or_fail(recovery_inputs(fx, plan));
  PFM_CHECK(gate_satisfied(assessment, RecoveryGate::EvidenceCurrent));
  PFM_CHECK(assessment.eligible);

  plan.scope.unevidenced_elements = {fx.ups};
  assessment = assess_or_fail(recovery_inputs(fx, plan));
  PFM_CHECK(!gate_satisfied(assessment, RecoveryGate::EvidenceCurrent));
  PFM_CHECK_EQ(gate_reason(assessment, RecoveryGate::EvidenceCurrent),
               ReasonCode::EvidenceMissing);
  PFM_CHECK(assessment.find_gate(RecoveryGate::EvidenceCurrent)->subject == fx.ups);
  PFM_CHECK(!assessment.eligible);
}

PFM_TEST(recovery_gate_stability_dwell) {
  ChainFixture fx = make_chain();
  const ResponsePlan plan = base_plan(fx);

  RecoveryInputs inputs = recovery_inputs(fx, plan);
  RecoveryAssessment assessment = assess_or_fail(inputs);
  PFM_CHECK(gate_satisfied(assessment, RecoveryGate::StabilityDwell));
  PFM_CHECK(assessment.eligible);

  // One millisecond short of the dwell is short.
  inputs.stable_since =
      Timestamp::from_value(fx.now.value() - (fx.policy.stability_dwell.value() - 1));
  assessment = assess_or_fail(inputs);
  PFM_CHECK(!gate_satisfied(assessment, RecoveryGate::StabilityDwell));
  PFM_CHECK_EQ(gate_reason(assessment, RecoveryGate::StabilityDwell), ReasonCode::EvidenceStale);
  PFM_CHECK(!assessment.eligible);

  // No stable run at all.
  inputs.has_stable_since = false;
  inputs.stable_since = Timestamp{};
  assessment = assess_or_fail(inputs);
  PFM_CHECK(!gate_satisfied(assessment, RecoveryGate::StabilityDwell));
  PFM_CHECK_EQ(gate_reason(assessment, RecoveryGate::StabilityDwell),
               ReasonCode::EvidenceMissing);

  // Stability in the future is not an elapsed dwell.
  inputs.has_stable_since = true;
  inputs.stable_since = Timestamp::from_value(fx.now.value() + 1000);
  assessment = assess_or_fail(inputs);
  PFM_CHECK(!gate_satisfied(assessment, RecoveryGate::StabilityDwell));
}

PFM_TEST(recovery_gate_operator_authorization) {
  ChainFixture fx = make_chain();
  const ResponsePlan plan = base_plan(fx);

  RecoveryInputs inputs = recovery_inputs(fx, plan);
  RecoveryAssessment assessment = assess_or_fail(inputs);
  PFM_CHECK(gate_satisfied(assessment, RecoveryGate::OperatorAuthorization));
  PFM_CHECK(assessment.eligible);

  inputs.operator_authorized = false;
  assessment = assess_or_fail(inputs);
  PFM_CHECK(!gate_satisfied(assessment, RecoveryGate::OperatorAuthorization));
  PFM_CHECK_EQ(gate_reason(assessment, RecoveryGate::OperatorAuthorization),
               ReasonCode::AuthorizationNotCurrent);
  PFM_CHECK(!assessment.eligible);

  // An authorization for a different incident generation is not current.
  inputs.operator_authorized = true;
  inputs.authorized_generation = IncidentGeneration::from_value(2);
  assessment = assess_or_fail(inputs);
  PFM_CHECK(!gate_satisfied(assessment, RecoveryGate::OperatorAuthorization));
  PFM_CHECK(!assessment.eligible);

  RecoveryInputs waived = recovery_inputs(fx, plan);
  waived.operator_authorized = false;
  waived.policy.require_operator_authorization_for_recovery = false;
  assessment = assess_or_fail(waived);
  PFM_CHECK(gate_satisfied(assessment, RecoveryGate::OperatorAuthorization));
}

PFM_TEST(recovery_gate_requests_settled) {
  ChainFixture fx = make_chain();
  ResponsePlan plan = base_plan(fx);

  RecoveryAssessment assessment = assess_or_fail(recovery_inputs(fx, plan));
  PFM_CHECK(gate_satisfied(assessment, RecoveryGate::RequestsSettled));
  PFM_CHECK(assessment.eligible);

  add_request(plan, RequestKind::IsolateElement, fx.feed, ElementKind::UtilityFeed,
              ProofKind::Isolation, RequestState::Issued, 1);
  assessment = assess_or_fail(recovery_inputs(fx, plan));
  PFM_CHECK(!gate_satisfied(assessment, RecoveryGate::RequestsSettled));
  PFM_CHECK(!assessment.eligible);
  PFM_CHECK(assessment.find_gate(RecoveryGate::RequestsSettled)->subject == fx.feed);

  // Indeterminate is the outcome that is unknown, and unknown is not settled.
  plan.requests.clear();
  add_request(plan, RequestKind::IsolateElement, fx.feed, ElementKind::UtilityFeed,
              ProofKind::Isolation, RequestState::Indeterminate, 1);
  assessment = assess_or_fail(recovery_inputs(fx, plan));
  PFM_CHECK(!gate_satisfied(assessment, RecoveryGate::RequestsSettled));
  PFM_CHECK_EQ(gate_reason(assessment, RecoveryGate::RequestsSettled),
               ReasonCode::EvidenceMissing);

  // Settled terminal states do not block.
  for (const RequestState state : {RequestState::Verified, RequestState::Failed,
                                   RequestState::Refused, RequestState::Superseded,
                                   RequestState::Abandoned, RequestState::Expired}) {
    ResponsePlan settled = base_plan(fx);
    add_request(settled, RequestKind::IsolateElement, fx.feed, ElementKind::UtilityFeed,
                ProofKind::Isolation, state, 1);
    const RecoveryAssessment settled_assessment =
        assess_or_fail(recovery_inputs(fx, settled));
    PFM_CHECK_MSG(gate_satisfied(settled_assessment, RecoveryGate::RequestsSettled),
                  request_state_name(state).data());
  }
}

PFM_TEST(recovery_acknowledgement_alone_never_proves) {
  ChainFixture fx = make_chain();
  ResponsePlan plan = base_plan(fx);
  IsolationRequirement requirement;
  requirement.element = fx.bus;
  requirement.isolation_point = fx.brk_bus;
  requirement.has_isolation_point = true;
  plan.isolations.push_back(requirement);
  // Every proof kind the planner can require, acknowledged but never verified.
  for (const ProofKind proof : {ProofKind::Isolation, ProofKind::DeEnergization,
                                ProofKind::BreakerOpen, ProofKind::Transfer,
                                ProofKind::Synchronization, ProofKind::Generation,
                                ProofKind::Reserve, ProofKind::LoadShed, ProofKind::StableSource}) {
    ResponseRequest request = hand_request(RequestKind::IsolateElement,
                                           owner_for_kind(RequestKind::IsolateElement), fx.bus,
                                           ElementKind::Bus, false, 0, 1);
    request.required_proof = proof;
    request.state = RequestState::Acknowledged;
    PFM_CHECK(!request.satisfies(proof));
    request.state = RequestState::Observed;
    PFM_CHECK(!request.satisfies(proof));
    request.state = RequestState::Verified;
    PFM_CHECK(request.satisfies(proof));
    request.state = RequestState::Failed;
    PFM_CHECK(!request.satisfies(proof));
  }
  // The gate reports unsatisfied while only an acknowledgement exists.
  ResponsePlan acked = plan;
  add_request(acked, RequestKind::IsolateElement, fx.bus, ElementKind::Bus, ProofKind::Isolation,
              RequestState::Acknowledged, 1);
  add_request(acked, RequestKind::VerifyDeEnergization, fx.bus, ElementKind::Bus,
              ProofKind::DeEnergization, RequestState::Acknowledged, 2);
  const RecoveryAssessment assessment = assess_or_fail(recovery_inputs(fx, acked));
  PFM_CHECK(!gate_satisfied(assessment, RecoveryGate::IsolationVerified));
  PFM_CHECK(!gate_satisfied(assessment, RecoveryGate::DeEnergizationVerified));
  PFM_CHECK(!assessment.eligible);
}

PFM_TEST(recovery_locally_healthy_below_unresolved_upstream) {
  ChainFixture fx = make_chain();
  ResponsePlan plan = base_plan(fx);
  // The newest plan classifies nothing and every element in the boundary is
  // locally healthy ...
  plan.scope.impacted_elements = {fx.pdu, fx.branch, fx.load};
  plan.scope.unresolved_upstream = {fx.sg};
  plan.scope.upstream_evidence_unresolved = true;
  const RecoveryAssessment assessment = assess_or_fail(recovery_inputs(fx, plan));
  PFM_CHECK(!assessment.eligible);
  PFM_CHECK(!gate_satisfied(assessment, RecoveryGate::UpstreamEvidenceCurrent));
  PFM_CHECK_EQ(gate_reason(assessment, RecoveryGate::UpstreamEvidenceCurrent),
               ReasonCode::UpstreamEvidenceUnresolved);
  // Everything else is fine: local health is not the question.
  PFM_CHECK(gate_satisfied(assessment, RecoveryGate::FaultCleared));
  PFM_CHECK(gate_satisfied(assessment, RecoveryGate::EvidenceCurrent));
}

PFM_TEST(recovery_matches_reference_model_on_planned_inputs) {
  ChainFixture fx = make_chain();
  fault_rich(fx);
  TopologyIndex index = build_index(fx.snapshot, fx.bounds);
  PlanInputs inputs = make_plan_inputs(fx, index);
  auto planned = plan_response(inputs);
  PFM_REQUIRE(planned.ok());

  RecoveryInputs library_inputs = recovery_inputs(fx, planned.value());
  library_inputs.incident_scope = planned.value().scope.impacted_elements;
  for (const auto& failed : planned.value().scope.failed_elements) {
    library_inputs.incident_scope.push_back(failed);
  }
  const RecoveryAssessment assessment = assess_or_fail(library_inputs);

  pfmref::ModelRecoveryInputs model_inputs;
  model_inputs.now = library_inputs.now;
  model_inputs.policy = library_inputs.policy;
  model_inputs.plan = &planned.value();
  model_inputs.incident_scope = library_inputs.incident_scope;
  model_inputs.observations = library_inputs.current_observations;
  model_inputs.obligations = library_inputs.obligations;
  model_inputs.has_stable_since = library_inputs.has_stable_since;
  model_inputs.stable_since = library_inputs.stable_since;
  model_inputs.operator_authorized = library_inputs.operator_authorized;
  model_inputs.authorized_generation = library_inputs.authorized_generation;
  model_inputs.incident_generation = library_inputs.incident_generation;
  const pfmref::ModelRecovery model = pfmref::model_recovery(model_inputs);

  PFM_CHECK_EQ(assessment.eligible, model.eligible);
  for (const RecoveryGate gate :
       {RecoveryGate::FaultCleared, RecoveryGate::IsolationVerified,
        RecoveryGate::DeEnergizationVerified, RecoveryGate::BreakerStateProven,
        RecoveryGate::TransferStable, RecoveryGate::GenerationStable,
        RecoveryGate::ReserveRestored, RecoveryGate::SharedDomainResolved,
        RecoveryGate::UpstreamEvidenceCurrent, RecoveryGate::ObligationsSatisfied,
        RecoveryGate::EvidenceCurrent, RecoveryGate::StabilityDwell,
        RecoveryGate::OperatorAuthorization, RecoveryGate::RequestsSettled}) {
    PFM_CHECK_MSG(gate_satisfied(assessment, gate) == model.gate(gate),
                  recovery_gate_name(gate).data());
  }
}

// --- authority fencing -----------------------------------------------------

AuthorityExpectation live_expectation() {
  AuthorityExpectation expected;
  expected.require_incident = true;
  expected.incident_live = true;
  expected.incident = IncidentId::from_value(7);
  expected.generation = IncidentGeneration::from_value(3);
  expected.epoch = ControlEpoch::from_value(5);
  expected.incarnation = ControllerIncarnation::from_value(2);
  expected.revision = StateRevision::from_value(11);
  return expected;
}

AuthorityToken live_token() {
  return AuthorityToken::from_values(IncidentId::from_value(7), IncidentGeneration::from_value(3),
                                     ControlEpoch::from_value(5),
                                     ControllerIncarnation::from_value(2),
                                     StateRevision::from_value(11));
}

PFM_TEST(authority_fencing_each_rule) {
  const AuthorityExpectation expected = live_expectation();
  PFM_CHECK_OK(fence_authority(live_token(), expected));

  {
    AuthorityToken token = live_token();
    token.epoch = ControlEpoch{};
    PFM_CHECK_CODE(fence_authority(token, expected), StatusCode::MissingAuthority);
  }
  {
    AuthorityToken token = live_token();
    token.incarnation = ControllerIncarnation{};
    PFM_CHECK_CODE(fence_authority(token, expected), StatusCode::MissingAuthority);
  }
  {
    AuthorityToken token = live_token();
    token.epoch = ControlEpoch::from_value(4);
    PFM_CHECK_CODE(fence_authority(token, expected), StatusCode::StaleEpoch);
    // The comparison is on the epoch only: every other field is valid.
    token.revision = expected.revision;
    PFM_CHECK_CODE(fence_authority(token, expected), StatusCode::StaleEpoch);
  }
  {
    AuthorityToken token = live_token();
    token.epoch = ControlEpoch::from_value(6);
    PFM_CHECK_CODE(fence_authority(token, expected), StatusCode::FutureEpoch);
  }
  {
    // An incarnation is an identity, not an ordering: any difference is stale.
    AuthorityToken token = live_token();
    token.incarnation = ControllerIncarnation::from_value(3);
    PFM_CHECK_CODE(fence_authority(token, expected), StatusCode::StaleIncarnation);
    token.incarnation = ControllerIncarnation::from_value(1);
    PFM_CHECK_CODE(fence_authority(token, expected), StatusCode::StaleIncarnation);
  }
  {
    // A token that names no incident while one is live cannot act inside it.
    AuthorityToken token = live_token();
    token.incident = IncidentId{};
    token.generation = IncidentGeneration{};
    PFM_CHECK_CODE(fence_authority(token, expected), StatusCode::NoActiveIncident);
  }
  {
    // The other direction: an incident-bound token with no live incident.
    AuthorityExpectation none = expected;
    none.incident_live = false;
    AuthorityToken token = live_token();
    PFM_CHECK_CODE(fence_authority(token, none), StatusCode::NoActiveIncident);
  }
  {
    AuthorityToken token = live_token();
    token.incident = IncidentId::from_value(8);
    PFM_CHECK_CODE(fence_authority(token, expected), StatusCode::CrossIncidentAuthority);
  }
  {
    AuthorityToken token = live_token();
    token.generation = IncidentGeneration::from_value(2);
    PFM_CHECK_CODE(fence_authority(token, expected), StatusCode::StaleGeneration);
    token.generation = IncidentGeneration{};
    PFM_CHECK_CODE(fence_authority(token, expected), StatusCode::StaleGeneration);
  }
  {
    AuthorityToken token = live_token();
    token.generation = IncidentGeneration::from_value(4);
    PFM_CHECK_CODE(fence_authority(token, expected), StatusCode::FutureGeneration);
  }
  {
    AuthorityToken token = live_token();
    token.revision = StateRevision::from_value(10);
    PFM_CHECK_CODE(fence_authority(token, expected), StatusCode::StaleRevision);
  }
  {
    AuthorityToken token = live_token();
    token.revision = StateRevision::from_value(12);
    PFM_CHECK_CODE(fence_authority(token, expected), StatusCode::FutureRevision);
  }
  {
    // An incident generation without an incident identity is meaningless.
    AuthorityToken token = live_token();
    token.incident = IncidentId{};
    PFM_CHECK_CODE(fence_authority(token, expected), StatusCode::InvalidArgument);
  }
  {
    // Operations that establish an incident skip the incident checks.
    AuthorityExpectation establishing = expected;
    establishing.require_incident = false;
    AuthorityToken token = live_token();
    token.incident = IncidentId{};
    token.generation = IncidentGeneration{};
    PFM_CHECK_OK(fence_authority(token, establishing));
  }
}

PFM_TEST(authority_fencing_precedence) {
  const AuthorityExpectation expected = live_expectation();
  // Documented order: MissingAuthority, StaleEpoch, FutureEpoch,
  // StaleIncarnation, NoActiveIncident, CrossIncidentAuthority,
  // StaleGeneration, FutureGeneration, StaleRevision, FutureRevision.
  {
    AuthorityToken token = live_token();
    token.epoch = ControlEpoch{};
    token.incarnation = ControllerIncarnation{};
    token.incident = IncidentId::from_value(99);
    token.generation = IncidentGeneration::from_value(1);
    token.revision = StateRevision::from_value(1);
    PFM_CHECK_CODE(fence_authority(token, expected), StatusCode::MissingAuthority);
  }
  {
    AuthorityToken token = live_token();
    token.epoch = ControlEpoch::from_value(4);
    token.incarnation = ControllerIncarnation::from_value(9);
    token.incident = IncidentId::from_value(99);
    token.generation = IncidentGeneration::from_value(1);
    token.revision = StateRevision::from_value(1);
    PFM_CHECK_CODE(fence_authority(token, expected), StatusCode::StaleEpoch);
  }
  {
    AuthorityToken token = live_token();
    token.epoch = ControlEpoch::from_value(6);
    token.incarnation = ControllerIncarnation::from_value(9);
    token.incident = IncidentId::from_value(99);
    PFM_CHECK_CODE(fence_authority(token, expected), StatusCode::FutureEpoch);
  }
  {
    AuthorityToken token = live_token();
    token.incarnation = ControllerIncarnation::from_value(9);
    token.incident = IncidentId::from_value(99);
    token.generation = IncidentGeneration::from_value(1);
    token.revision = StateRevision::from_value(1);
    PFM_CHECK_CODE(fence_authority(token, expected), StatusCode::StaleIncarnation);
  }
  {
    AuthorityToken token = live_token();
    token.incident = IncidentId::from_value(99);
    token.generation = IncidentGeneration::from_value(1);
    token.revision = StateRevision::from_value(1);
    PFM_CHECK_CODE(fence_authority(token, expected), StatusCode::CrossIncidentAuthority);
  }
  {
    AuthorityToken token = live_token();
    token.generation = IncidentGeneration::from_value(4);
    token.revision = StateRevision::from_value(1);
    PFM_CHECK_CODE(fence_authority(token, expected), StatusCode::FutureGeneration);
  }
  {
    AuthorityToken token = live_token();
    token.generation = IncidentGeneration::from_value(2);
    token.revision = StateRevision::from_value(1);
    PFM_CHECK_CODE(fence_authority(token, expected), StatusCode::StaleGeneration);
  }
  {
    AuthorityToken token = live_token();
    token.revision = StateRevision::from_value(12);
    PFM_CHECK_CODE(fence_authority(token, expected), StatusCode::FutureRevision);
    token.revision = StateRevision::from_value(10);
    PFM_CHECK_CODE(fence_authority(token, expected), StatusCode::StaleRevision);
  }
  {
    // With no live incident, the incident rules are decided before generation
    // and revision.
    AuthorityExpectation none = expected;
    none.incident_live = false;
    AuthorityToken token = live_token();
    token.incident = IncidentId::from_value(99);
    token.generation = IncidentGeneration::from_value(9);
    token.revision = StateRevision::from_value(99);
    PFM_CHECK_CODE(fence_authority(token, none), StatusCode::NoActiveIncident);
  }
  {
    // Generation is decided before revision when both are wrong.
    AuthorityToken token = live_token();
    token.generation = IncidentGeneration::from_value(4);
    token.revision = StateRevision::from_value(1);
    PFM_CHECK_CODE(fence_authority(token, expected), StatusCode::FutureGeneration);
  }
}

// --- the runtime surface ---------------------------------------------------

struct Rig {
  std::unique_ptr<SyntheticPlant> plant{};
  std::shared_ptr<ManualClock> clock{};
  std::shared_ptr<ScriptedTransport> transport{};
  std::unique_ptr<PowerFailureRuntime> runtime{};

  static Rig make(SyntheticPlantConfig config) {
    Rig rig;
    rig.plant = std::make_unique<SyntheticPlant>(config);
    rig.clock = pfmtest::make_clock();
    rig.transport = std::make_shared<ScriptedTransport>();
    auto opened = PowerFailureRuntime::open(pfmtest::volatile_options(), rig.clock, rig.transport);
    if (!opened.ok()) {
      ::pfmtest::report_failure(__FILE__, __LINE__, "cannot open the runtime");
      return rig;
    }
    rig.runtime = std::move(opened.value());
    return rig;
  }

  [[nodiscard]] Result<AuthorityToken> token() const { return runtime->current_authority(); }

  Result<void> publish_topology() {
    auto current = token();
    if (!current.ok()) {
      return current.status();
    }
    return runtime->publish_topology(plant->topology(), current.value());
  }

  Result<void> open_incident(IncidentId incident, IncidentGeneration generation) {
    auto current = token();
    if (!current.ok()) {
      return current.status();
    }
    current.value().incident = incident;
    current.value().generation = generation;
    return runtime->open_incident(current.value());
  }

  Result<void> admit(Timestamp now) {
    auto current = token();
    if (!current.ok()) {
      return current.status();
    }
    return runtime->admit_evidence(plant->observe(now), current.value());
  }

  Result<EvaluationOutcome> evaluate(Duration step) {
    auto advanced = clock->advance(step);
    if (!advanced.ok()) {
      return advanced.status();
    }
    auto admitted = admit(clock->now().value());
    if (!admitted.ok()) {
      return admitted.status();
    }
    auto current = token();
    if (!current.ok()) {
      return current.status();
    }
    return runtime->evaluate(current.value());
  }
};

const ResponseRequest* find_request_of(const std::vector<ResponseRequest>& requests,
                                       RequestKind kind, const RefToken& element) {
  for (const auto& request : requests) {
    if (request.kind == kind && request.target.element == element) {
      return &request;
    }
  }
  return nullptr;
}

const ResponseRequest* find_state(const std::vector<ResponseRequest>& requests,
                                  RequestState state) {
  for (const auto& request : requests) {
    if (request.state == state) {
      return &request;
    }
  }
  return nullptr;
}

SyntheticPlantConfig small_plant_config() {
  SyntheticPlantConfig config;
  config.bus_fanout = 1;
  config.switchgear_fanout = 1;
  return config;
}

PFM_TEST(authority_fencing_is_enforced_by_the_runtime) {
  Rig rig = Rig::make(small_plant_config());
  PFM_REQUIRE(rig.runtime != nullptr);
  PFM_CHECK_OK(rig.publish_topology());
  PFM_CHECK_OK(rig.open_incident(IncidentId::from_value(1), IncidentGeneration::from_value(1)));
  PFM_CHECK_OK(rig.evaluate(duration_from_seconds(1)));

  const auto current = rig.token();
  PFM_REQUIRE(current.ok());
  const AuthorityToken good = current.value();
  PFM_CHECK_EQ(good.epoch, ControlEpoch::from_value(1));
  PFM_CHECK_EQ(good.incarnation, ControllerIncarnation::from_value(1));

  {
    AuthorityToken token = good;
    token.revision = StateRevision::from_value(good.revision.value() - 1);
    PFM_CHECK_CODE(rig.runtime->evaluate(token), StatusCode::StaleRevision);
  }
  {
    AuthorityToken token = good;
    token.revision = StateRevision::from_value(good.revision.value() + 1);
    PFM_CHECK_CODE(rig.runtime->evaluate(token), StatusCode::FutureRevision);
  }
  {
    AuthorityToken token = good;
    token.epoch = ControlEpoch{};
    PFM_CHECK_CODE(rig.runtime->evaluate(token), StatusCode::MissingAuthority);
  }
  {
    AuthorityToken token = good;
    token.incarnation = ControllerIncarnation{};
    PFM_CHECK_CODE(rig.runtime->evaluate(token), StatusCode::MissingAuthority);
  }
  {
    AuthorityToken token = good;
    token.epoch = ControlEpoch::from_value(2);
    PFM_CHECK_CODE(rig.runtime->evaluate(token), StatusCode::FutureEpoch);
  }
  {
    AuthorityToken token = good;
    token.incarnation = ControllerIncarnation::from_value(2);
    PFM_CHECK_CODE(rig.runtime->evaluate(token), StatusCode::StaleIncarnation);
  }
  {
    AuthorityToken token = good;
    token.incident = IncidentId::from_value(99);
    PFM_CHECK_CODE(rig.runtime->evaluate(token), StatusCode::CrossIncidentAuthority);
  }
  {
    AuthorityToken token = good;
    token.generation = IncidentGeneration::from_value(2);
    PFM_CHECK_CODE(rig.runtime->evaluate(token), StatusCode::FutureGeneration);
  }
  {
    AuthorityToken token = good;
    token.generation = IncidentGeneration{};
    PFM_CHECK_CODE(rig.runtime->evaluate(token), StatusCode::StaleGeneration);
  }
  {
    // A fenced mutation leaves nothing behind.
    auto state = rig.runtime->state();
    PFM_REQUIRE(state.ok());
    PFM_CHECK_EQ(state.value().incident.incident, IncidentId::from_value(1));
    PFM_CHECK_EQ(state.value().incident.generation, IncidentGeneration::from_value(1));
  }
}

// --- request lifecycle -----------------------------------------------------

PFM_TEST(request_lifecycle_planned_issued_acknowledged_observed_verified) {
  Rig rig = Rig::make(small_plant_config());
  PFM_REQUIRE(rig.runtime != nullptr);
  PFM_CHECK_OK(rig.publish_topology());
  PFM_CHECK_OK(rig.open_incident(IncidentId::from_value(1), IncidentGeneration::from_value(1)));
  PFM_CHECK_OK(rig.evaluate(duration_from_seconds(1)));

  const auto feed = rig.plant->element_ref(RefKind::UtilityFeed, "feed-0");
  PFM_REQUIRE(feed.ok());
  rig.plant->lose_utility(feed.value());
  const auto outcome = rig.evaluate(duration_from_seconds(1));
  PFM_REQUIRE(outcome.ok());
  PFM_CHECK_EQ(outcome.value().primary_failure, FailureClass::UtilityFeedLoss);
  PFM_CHECK(outcome.value().requests_planned > 0);
  PFM_CHECK(outcome.value().requests_dispatched > 0);
  PFM_CHECK(outcome.value().accepted > 0);

  const auto requests = rig.runtime->requests();
  PFM_REQUIRE(requests.ok());
  const ResponseRequest* isolation =
      find_request_of(requests.value(), RequestKind::IsolateElement, feed.value());
  PFM_REQUIRE(isolation != nullptr);
  const ResponseRequestId isolation_id = isolation->id;
  const AttemptId first_attempt = isolation->attempt;
  const IdempotencyKey first_key = isolation->idempotency;

  // Planned -> Issued -> Acknowledged all happened inside one evaluation, and
  // the acknowledgement is only an acknowledgement.
  PFM_CHECK_EQ(isolation->state, RequestState::Acknowledged);
  PFM_CHECK(isolation->has_issued_at);
  PFM_CHECK(isolation->has_acknowledged_at);
  PFM_CHECK(!isolation->has_observed_at);
  PFM_CHECK(!isolation->has_verified_at);
  PFM_CHECK_EQ(isolation->attempt_ordinal, 1u);
  PFM_CHECK_EQ(isolation->required_proof, ProofKind::Isolation);
  PFM_CHECK(!isolation->satisfies(ProofKind::Isolation));
  PFM_CHECK(!isolation->satisfies(ProofKind::None));
  PFM_CHECK(!isolation->verification_source.is_set());
  PFM_CHECK(!isolation->verification_evidence.is_set());
  PFM_CHECK_EQ(isolation->idempotency, IdempotencyKey::from_fingerprint(isolation->fingerprint));

  {
    auto current = rig.token();
    PFM_REQUIRE(current.ok());
    PFM_CHECK_CODE(rig.runtime->acknowledge_request(isolation_id,
                                                    make_ref(RefKind::Controller, "ops"),
                                                    "acknowledged twice", current.value()),
                   StatusCode::RequestStateConflict);
  }

  // A current reading that does not carry the required fact only reaches
  // Observed: an observation is not a verification.
  ElectricalObservation not_proving =
      rig.plant->observe_element(feed.value(), rig.clock->now().value());
  PFM_REQUIRE(not_proving.element.is_set());
  not_proving.origin = ObservationOrigin::ExternalMeter;
  not_proving.energization = EnergizationState::Energized;
  not_proving.breaker = BreakerPosition::Closed;
  {
    auto current = rig.token();
    PFM_REQUIRE(current.ok());
    PFM_CHECK_OK(rig.runtime->record_effect(isolation_id, not_proving, current.value()));
  }
  {
    const auto observed = rig.runtime->request(isolation_id);
    PFM_REQUIRE(observed.ok());
    PFM_CHECK_EQ(observed.value().state, RequestState::Observed);
    PFM_CHECK(observed.value().has_observed_at);
    PFM_CHECK(!observed.value().has_verified_at);
    PFM_CHECK(!observed.value().satisfies(ProofKind::Isolation));
    PFM_CHECK_EQ(observed.value().attempt, first_attempt);
  }

  // Only evidence that carries the required fact verifies the effect.
  ElectricalObservation proving = not_proving;
  proving.energization = EnergizationState::DeEnergized;
  proving.breaker = BreakerPosition::Open;
  proving.voltage = MilliVolts::from_value(0);
  proving.frequency = MilliHertz::from_value(0);
  proving.sequence = ObservationSequence::from_value(not_proving.sequence.value() + 7);
  proving.evidence_fingerprint = Fingerprint{0x1234, 0x5678};
  {
    auto current = rig.token();
    PFM_REQUIRE(current.ok());
    PFM_CHECK_OK(rig.runtime->record_effect(isolation_id, proving, current.value()));
  }
  {
    const auto verified = rig.runtime->request(isolation_id);
    PFM_REQUIRE(verified.ok());
    PFM_CHECK_EQ(verified.value().state, RequestState::Verified);
    PFM_CHECK(verified.value().has_verified_at);
    PFM_CHECK(verified.value().has_observed_at);
    PFM_CHECK(verified.value().satisfies(ProofKind::Isolation));
    PFM_CHECK(verified.value().verification_source.is_set());
    PFM_CHECK_EQ(verified.value().verification_evidence, proving.evidence_fingerprint);
    PFM_CHECK_EQ(verified.value().idempotency, first_key);
  }
  {
    // A verification is not new evidence of the electrical state, and the
    // request is terminal from here.
    const auto verified = rig.runtime->request(isolation_id);
    PFM_REQUIRE(verified.ok());
    PFM_CHECK(request_state_is_terminal(verified.value().state));
    PFM_CHECK(!request_state_is_open(verified.value().state));
  }
}

PFM_TEST(request_lifecycle_illegal_transitions_are_refused) {
  Rig rig = Rig::make(small_plant_config());
  PFM_REQUIRE(rig.runtime != nullptr);
  PFM_CHECK_OK(rig.publish_topology());
  PFM_CHECK_OK(rig.open_incident(IncidentId::from_value(1), IncidentGeneration::from_value(1)));
  PFM_CHECK_OK(rig.evaluate(duration_from_seconds(1)));
  const auto feed = rig.plant->element_ref(RefKind::UtilityFeed, "feed-0");
  PFM_REQUIRE(feed.ok());
  rig.plant->lose_utility(feed.value());
  PFM_REQUIRE(rig.evaluate(duration_from_seconds(1)).ok());

  const auto requests = rig.runtime->requests();
  PFM_REQUIRE(requests.ok());
  const ResponseRequest* isolation =
      find_request_of(requests.value(), RequestKind::IsolateElement, feed.value());
  PFM_REQUIRE(isolation != nullptr);
  const ResponseRequestId id = isolation->id;

  // The documented transition table, checked directly.
  PFM_CHECK(request_state_transition_legal(RequestState::Planned, RequestState::Issued));
  PFM_CHECK(request_state_transition_legal(RequestState::Issued, RequestState::Acknowledged));
  PFM_CHECK(request_state_transition_legal(RequestState::Acknowledged, RequestState::Observed));
  PFM_CHECK(request_state_transition_legal(RequestState::Observed, RequestState::Verified));
  PFM_CHECK(!request_state_transition_legal(RequestState::Planned, RequestState::Acknowledged));
  PFM_CHECK(!request_state_transition_legal(RequestState::Planned, RequestState::Observed));
  PFM_CHECK(!request_state_transition_legal(RequestState::Acknowledged, RequestState::Issued));
  PFM_CHECK(!request_state_transition_legal(RequestState::Verified, RequestState::Observed));
  PFM_CHECK(!request_state_transition_legal(RequestState::Planned, RequestState::Planned));

  // An unknown identity is not a state conflict.
  PFM_CHECK_CODE(rig.runtime->request(ResponseRequestId::from_value(9999)),
                 StatusCode::RequestNotFound);
  PFM_CHECK_CODE(rig.runtime->retry_request(ResponseRequestId::from_value(9999),
                                            rig.token().value()),
                 StatusCode::RequestNotFound);

  // A reading that is not attributable to an external authority proves nothing.
  ElectricalObservation unattributed =
      rig.plant->observe_element(feed.value(), rig.clock->now().value());
  PFM_REQUIRE(unattributed.element.is_set());
  unattributed.origin = ObservationOrigin::Unknown;
  {
    auto current = rig.token();
    PFM_REQUIRE(current.ok());
    PFM_CHECK_CODE(rig.runtime->record_effect(id, unattributed, current.value()),
                   StatusCode::EvidenceOriginUntrusted);
  }
  // Evidence for a different element is not evidence of this request.
  const auto gen = rig.plant->element_ref(RefKind::Generator, "gen-0");
  PFM_REQUIRE(gen.ok());
  {
    ElectricalObservation wrong = rig.plant->observe_element(gen.value(), rig.clock->now().value());
    PFM_REQUIRE(wrong.element.is_set());
    wrong.origin = ObservationOrigin::ExternalMeter;
    auto current = rig.token();
    PFM_REQUIRE(current.ok());
    PFM_CHECK_CODE(rig.runtime->record_effect(id, wrong, current.value()),
                   StatusCode::RequestTargetMismatch);
  }
  // Evidence that is not current cannot confirm an effect.
  {
    ElectricalObservation stale = rig.plant->observe_element(feed.value(), rig.clock->now().value());
    PFM_REQUIRE(stale.element.is_set());
    stale.origin = ObservationOrigin::ExternalMeter;
    stale.observed_at = Timestamp::from_value(stale.observed_at.value() - 120000);
    auto current = rig.token();
    PFM_REQUIRE(current.ok());
    PFM_CHECK_CODE(rig.runtime->record_effect(id, stale, current.value()),
                   StatusCode::EvidenceNotCurrent);
  }

  // Verify it, then every further transition out of Verified is refused.
  ElectricalObservation proving = rig.plant->observe_element(feed.value(), rig.clock->now().value());
  PFM_REQUIRE(proving.element.is_set());
  proving.origin = ObservationOrigin::ExternalMeter;
  proving.energization = EnergizationState::DeEnergized;
  proving.breaker = BreakerPosition::Open;
  {
    auto current = rig.token();
    PFM_REQUIRE(current.ok());
    PFM_CHECK_OK(rig.runtime->record_effect(id, proving, current.value()));
  }
  {
    auto current = rig.token();
    PFM_REQUIRE(current.ok());
    PFM_CHECK_CODE(rig.runtime->acknowledge_request(id, make_ref(RefKind::Controller, "ops"), "",
                                                    current.value()),
                   StatusCode::RequestStateConflict);
    PFM_CHECK_CODE(rig.runtime->record_effect(id, proving, current.value()),
                   StatusCode::RequestStateConflict);
    PFM_CHECK_CODE(rig.runtime->fail_request(id, ReasonCode::AttemptFailed, "", current.value()),
                   StatusCode::RequestStateConflict);
    PFM_CHECK_CODE(rig.runtime->cancel_request(id, ReasonCode::JustificationWithdrawn,
                                               current.value()),
                   StatusCode::RequestStateConflict);
    PFM_CHECK_CODE(rig.runtime->resolve_indeterminate(id, false, proving, current.value()),
                   StatusCode::RequestStateConflict);
    PFM_CHECK_CODE(rig.runtime->retry_request(id, current.value()),
                   StatusCode::RequestStateConflict);
  }
}

PFM_TEST(request_retry_reuses_the_key_and_supersedes_the_attempt) {
  Rig rig = Rig::make(small_plant_config());
  PFM_REQUIRE(rig.runtime != nullptr);
  PFM_CHECK_OK(rig.publish_topology());
  PFM_CHECK_OK(rig.open_incident(IncidentId::from_value(1), IncidentGeneration::from_value(1)));
  PFM_CHECK_OK(rig.evaluate(duration_from_seconds(1)));
  const auto feed = rig.plant->element_ref(RefKind::UtilityFeed, "feed-0");
  PFM_REQUIRE(feed.ok());
  rig.plant->lose_utility(feed.value());
  PFM_REQUIRE(rig.evaluate(duration_from_seconds(1)).ok());

  auto requests = rig.runtime->requests();
  PFM_REQUIRE(requests.ok());
  const ResponseRequest* isolation =
      find_request_of(requests.value(), RequestKind::IsolateElement, feed.value());
  PFM_REQUIRE(isolation != nullptr);
  const ResponseRequestId first_id = isolation->id;
  const AttemptId first_attempt = isolation->attempt;
  const IdempotencyKey key = isolation->idempotency;
  const Fingerprint fingerprint = isolation->fingerprint;
  const RequestKind kind = isolation->kind;
  const ProofKind proof = isolation->required_proof;
  const RefToken target = isolation->target.element;

  {
    auto current = rig.token();
    PFM_REQUIRE(current.ok());
    PFM_CHECK_OK(rig.runtime->retry_request(first_id, current.value()));
  }
  const auto after_first = rig.runtime->requests();
  PFM_REQUIRE(after_first.ok());
  const ResponseRequest* second = find_state(after_first.value(), RequestState::Planned);
  PFM_REQUIRE(second != nullptr);
  PFM_CHECK_EQ(second->attempt_ordinal, 2u);
  PFM_CHECK(second->attempt != first_attempt);
  PFM_CHECK(second->id != first_id);
  // The identity is one nothing else has ever used.
  for (const auto& candidate : after_first.value()) {
    if (candidate.id == second->id) {
      PFM_CHECK_EQ(candidate.attempt, second->attempt);
    }
  }
  PFM_CHECK_EQ(second->idempotency, key);
  PFM_CHECK_EQ(second->fingerprint, fingerprint);
  PFM_CHECK_EQ(second->kind, kind);
  PFM_CHECK_EQ(second->required_proof, proof);
  PFM_CHECK(second->target.element == target);
  PFM_CHECK_EQ(response_request_fingerprint(*second), fingerprint);
  PFM_CHECK_EQ(second->state, RequestState::Planned);
  PFM_CHECK(!second->has_issued_at);

  const ResponseRequestId second_id = second->id;
  {
    const auto superseded = rig.runtime->request(first_id);
    PFM_REQUIRE(superseded.ok());
    PFM_CHECK_EQ(superseded.value().state, RequestState::Superseded);
    PFM_CHECK_EQ(superseded.value().superseded_by, second->attempt);
    PFM_CHECK(!request_state_is_open(superseded.value().state));
    PFM_CHECK(!superseded.value().satisfies(ProofKind::Isolation));
  }
  // A superseded attempt is not retried again: only the live attempt is.
  {
    auto current = rig.token();
    PFM_REQUIRE(current.ok());
    PFM_CHECK_CODE(rig.runtime->retry_request(first_id, current.value()),
                   StatusCode::RequestStateConflict);
    // An acknowledgement cannot skip the issue step of the new attempt.
    PFM_CHECK_CODE(rig.runtime->acknowledge_request(second_id,
                                                    make_ref(RefKind::Controller, "ops"), "",
                                                    current.value()),
                   StatusCode::RequestStateConflict);
    // Retrying the live attempt creates a third and final attempt.
    PFM_CHECK_OK(rig.runtime->retry_request(second_id, current.value()));
  }
  const auto after_second = rig.runtime->requests();
  PFM_REQUIRE(after_second.ok());
  const ResponseRequest* third = find_state(after_second.value(), RequestState::Planned);
  PFM_REQUIRE(third != nullptr);
  PFM_CHECK_EQ(third->attempt_ordinal, 3u);
  PFM_CHECK_EQ(third->idempotency, key);
  const ResponseRequestId third_id = third->id;
  {
    // The attempt bound is enforced, not truncated.
    auto current = rig.token();
    PFM_REQUIRE(current.ok());
    PFM_CHECK_CODE(rig.runtime->retry_request(third_id, current.value()),
                   StatusCode::AttemptExhausted);
    const auto unchanged = rig.runtime->request(third_id);
    PFM_REQUIRE(unchanged.ok());
    PFM_CHECK_EQ(unchanged.value().state, RequestState::Planned);
    PFM_CHECK_EQ(unchanged.value().attempt_ordinal, 3u);
  }
  {
    // Exactly one attempt of this operation is still live; the others are
    // terminal, and nothing re-planned the operation as new work.
    const auto all = rig.runtime->requests();
    PFM_REQUIRE(all.ok());
    int open_for_the_operation = 0;
    for (const auto& request : all.value()) {
      if (request.fingerprint == fingerprint && request_state_is_open(request.state)) {
        open_for_the_operation += 1;
      }
    }
    PFM_CHECK_EQ(open_for_the_operation, 1);
  }
}

// --- end to end: the cumulative incident scope -----------------------------

// Isolate, restore, prove, authorize: the recovery question spans the whole
// incident, not only the newest plan.
PFM_TEST(recovery_end_to_end_isolate_restore_then_authorize) {
  auto made = pfmtest::PlantFixture::make(small_plant_config(), StoreMode::Volatile);
  PFM_REQUIRE(made.ok());
  const std::unique_ptr<pfmtest::PlantFixture> fixture = std::move(made.value());
  PFM_REQUIRE(fixture->runtime != nullptr);
  PFM_CHECK_OK(fixture->prime());

  const auto feed = fixture->plant->element_ref(RefKind::UtilityFeed, "feed-0");
  PFM_REQUIRE(feed.ok());
  PFM_CHECK_OK(fixture->tick(duration_from_seconds(1)));

  // The fault: the feed is lost and isolation is requested.
  fixture->plant->lose_utility(feed.value());
  const auto faulted = fixture->tick(duration_from_seconds(1));
  PFM_REQUIRE(faulted.ok());
  PFM_CHECK_EQ(faulted.value().primary_failure, FailureClass::UtilityFeedLoss);
  {
    const auto assessment = fixture->runtime->recovery();
    PFM_REQUIRE(assessment.ok());
    PFM_CHECK(!assessment.value().eligible);
  }

  // The isolation was requested, applied, and proven by an external effect: an
  // acknowledgement alone never settled it.
  {
    const auto requests = fixture->runtime->requests();
    PFM_REQUIRE(requests.ok());
    const ResponseRequest* isolation =
        find_request_of(requests.value(), RequestKind::IsolateElement, feed.value());
    PFM_REQUIRE(isolation != nullptr);
    PFM_CHECK_EQ(isolation->state, RequestState::Verified);
    PFM_CHECK(isolation->verification_source.is_set());
    PFM_CHECK(isolation->has_verified_at);
    PFM_CHECK(isolation->verification_observed_at.value() != 0);
  }

  // Restore the feed and close the isolation it opened, then let the response
  // settle: the plan of a tick is computed from the evidence available at the
  // start of that tick, so an applied effect lands one tick later.
  fixture->plant->restore_utility(feed.value());
  fixture->plant->close_breaker(feed.value());
  bool clean = false;
  for (int attempt = 0; attempt < 8 && !clean; ++attempt) {
    PFM_CHECK_OK(fixture->tick(duration_from_seconds(1)));
    const auto plan = fixture->runtime->plan();
    PFM_REQUIRE(plan.ok());
    clean = plan.value().failures.empty();
  }
  PFM_CHECK_MSG(clean, "the newest plan is clean once the feed is restored");

  const auto state = fixture->runtime->state();
  PFM_REQUIRE(state.ok());
  PFM_CHECK(!state.value().incident.impacted_scope.empty());
  {
    const auto plan = fixture->runtime->plan();
    PFM_REQUIRE(plan.ok());
    PFM_CHECK(plan.value().primary_failure == FailureClass::None);
  }
  // Recovery is still refused: the dwell has not been sustained and no operator
  // has authorized this incident generation.
  {
    const auto blocked = fixture->runtime->recovery();
    PFM_REQUIRE(blocked.ok());
    PFM_CHECK(!blocked.value().eligible);
    PFM_CHECK(!gate_satisfied(blocked.value(), RecoveryGate::OperatorAuthorization));
    PFM_CHECK(!gate_satisfied(blocked.value(), RecoveryGate::StabilityDwell));
    // The gate that must report the whole cumulative incident scope healthy is
    // the one that finally allows recovery; it is evaluated against every
    // element the incident ever touched, not only the newest plan.
    const GateEvaluation* fault_gate = blocked.value().find_gate(RecoveryGate::FaultCleared);
    PFM_REQUIRE(fault_gate != nullptr);
    PFM_CHECK(fault_gate->satisfied);
    const auto newest = fixture->runtime->plan();
    PFM_REQUIRE(newest.ok());
    bool beyond_newest_plan = false;
    for (const auto& element : state.value().incident.impacted_scope) {
      if (!pfmref::model_ref_is_monitored(element.kind())) {
        continue;
      }
      bool classified_now = false;
      for (const auto& failure : newest.value().failures) {
        if (failure.element == element) {
          classified_now = true;
        }
      }
      if (!classified_now) {
        beyond_newest_plan = true;
      }
    }
    PFM_CHECK_MSG(beyond_newest_plan,
                  "the incident scope reaches beyond the newest plan's failures");
  }

  {
    auto current = fixture->token();
    PFM_REQUIRE(current.ok());
    PFM_CHECK_OK(fixture->runtime->authorize_recovery(current.value()));
  }
  // Let the stable run cover the dwell, then assess again.
  PFM_CHECK_OK(fixture->clock->advance(
      Duration::from_value(fixture->runtime->state().value().policy.stability_dwell.value() + 1000)));
  PFM_CHECK_OK(fixture->tick(duration_from_seconds(1)));
  {
    const auto assessment = fixture->runtime->recovery();
    PFM_REQUIRE(assessment.ok());
    PFM_CHECK_MSG(assessment.value().eligible, assessment.value().summarize().c_str());
  }
  {
    auto current = fixture->token();
    PFM_REQUIRE(current.ok());
    PFM_CHECK_OK(fixture->runtime->begin_recovery(current.value()));
    auto refreshed = fixture->token();
    PFM_REQUIRE(refreshed.ok());
    PFM_CHECK_OK(fixture->runtime->complete_recovery(refreshed.value()));
  }
}

// --- randomized property checks --------------------------------------------

class Lcg {
 public:
  explicit Lcg(std::uint64_t seed) : state_(seed) {}

  std::uint64_t next() {
    state_ = state_ * 6364136223846793005ULL + 1442695040888963407ULL;
    return state_ >> 11;
  }

  std::uint32_t below(std::uint32_t bound) {
    return bound == 0 ? 0u : static_cast<std::uint32_t>(next() % bound);
  }

  bool chance(std::uint32_t percent) { return below(100) < percent; }

 private:
  std::uint64_t state_{0};
};

std::string hex_seed(std::uint64_t seed) {
  char buffer[32] = {};
  std::snprintf(buffer, sizeof(buffer), "0x%016llX", static_cast<unsigned long long>(seed));
  return std::string{buffer};
}

std::string reproduction(std::uint64_t seed, std::uint32_t iteration,
                         const TopologySnapshot& snapshot) {
  std::string text = "seed=" + hex_seed(seed) + " iteration=" + std::to_string(iteration) +
                     " elements=" + std::to_string(snapshot.elements.size()) + " domains=" +
                     std::to_string(snapshot.domains.size()) + " [";
  for (const auto& element : snapshot.elements) {
    text.append(element.element.to_string());
    text.push_back(' ');
  }
  text.append("]");
  return text;
}

bool failure_element_is_in(const TopologySnapshot& snapshot, const RefToken& element) {
  for (const auto& candidate : snapshot.elements) {
    if (candidate.element == element) {
      return true;
    }
  }
  for (const auto& domain : snapshot.domains) {
    if (domain.domain == element) {
      return true;
    }
  }
  return false;
}

std::vector<std::string> failure_keys(const std::vector<ClassifiedFailure>& failures) {
  std::vector<std::string> keys;
  keys.reserve(failures.size());
  for (const auto& failure : failures) {
    keys.push_back(failure.element.to_string() + "|" +
                   std::string{failure_class_name(failure.klass)} + "|" +
                   std::string{reason_code_name(failure.primary_reason)} + "|" +
                   (failure.evidence_current ? "current" : "unresolved"));
  }
  std::sort(keys.begin(), keys.end());
  return keys;
}

std::string join_keys(const std::vector<std::string>& keys) {
  std::string text;
  for (const auto& key : keys) {
    if (!text.empty()) {
      text.append(" ; ");
    }
    text.append(key);
  }
  return text;
}

struct KindOption {
  ElementKind kind;
  const char* prefix;
};

const KindOption kKindOptions[] = {
    {ElementKind::UtilityFeed, "feed"},  {ElementKind::Switchgear, "sg"},
    {ElementKind::Bus, "bus"},           {ElementKind::Breaker, "brk"},
    {ElementKind::Circuit, "ckt"},       {ElementKind::Pdu, "pdu"},
    {ElementKind::PduBranch, "branch"},  {ElementKind::UpsUnit, "ups"},
    {ElementKind::UpsBus, "upsbus"},     {ElementKind::Generator, "gen"},
    {ElementKind::AutomaticTransferSwitch, "ats"},
    {ElementKind::StaticTransferSwitch, "sts"},
    {ElementKind::LoadGroup, "load"},
};

constexpr std::uint32_t kKindOptionCount =
    static_cast<std::uint32_t>(sizeof(kKindOptions) / sizeof(kKindOptions[0]));

bool kind_is_switching(ElementKind kind) {
  return kind == ElementKind::Breaker || kind == ElementKind::Circuit ||
         kind == ElementKind::Switchgear || kind == ElementKind::StaticTransferSwitch ||
         kind == ElementKind::AutomaticTransferSwitch;
}

struct RandomPlant {
  TopologySnapshot snapshot{};
  std::vector<ElectricalObservation> observations{};
};

RandomPlant make_random_plant(Lcg& rng, Timestamp now, ObservationSequence& sequence) {
  RandomPlant plant;
  plant.snapshot.generation = TopologyGeneration::from_value(1);
  const std::uint32_t count = 1 + rng.below(7);
  std::vector<std::size_t> switching;
  for (std::uint32_t i = 0; i < count; ++i) {
    const KindOption& option = kKindOptions[rng.below(kKindOptionCount)];
    TopologyElement element;
    element.element = make_ref(ref_kind_for_element(option.kind),
                               std::string{option.prefix} + "-" + std::to_string(i));
    element.kind = option.kind;
    if (i > 0 && rng.chance(80)) {
      element.upstream = plant.snapshot.elements[rng.below(i)].element;
    }
    if (rng.chance(60)) {
      element.domain = make_ref(RefKind::PowerDomain, rng.chance(50) ? "pd-1" : "pd-2");
    }
    if (option.kind == ElementKind::PduBranch && rng.chance(60)) {
      element.load_group = make_ref(RefKind::LoadGroup, "load-served-" + std::to_string(i));
    }
    if (!switching.empty() && rng.chance(30)) {
      const auto pick = static_cast<std::uint32_t>(switching.size());
      element.isolation_point = plant.snapshot.elements[switching[rng.below(pick)]].element;
      element.has_isolation_point = true;
    }
    element.label = element.element.name();
    if (kind_is_switching(option.kind)) {
      switching.push_back(plant.snapshot.elements.size());
    }
    plant.snapshot.elements.push_back(std::move(element));
  }

  const std::uint32_t domain_count = rng.below(3);
  for (std::uint32_t d = 0; d < domain_count; ++d) {
    FailureDomain domain;
    domain.domain = make_ref(RefKind::FailureDomain, "fd-" + std::to_string(d));
    domain.shared = rng.chance(40);
    for (const auto& element : plant.snapshot.elements) {
      if (rng.chance(45)) {
        domain.members.push_back(element.element);
      }
    }
    if (domain.members.empty()) {
      domain.members.push_back(plant.snapshot.elements.front().element);
    }
    plant.snapshot.domains.push_back(std::move(domain));
  }

  // A leaf is never an ancestor of another element, so giving a leaf a second,
  // contradicting report cannot change how any other element's supply chain is
  // read.
  std::vector<RefToken> leaves;
  for (const auto& element : plant.snapshot.elements) {
    bool has_child = false;
    for (const auto& other : plant.snapshot.elements) {
      if (other.upstream == element.element) {
        has_child = true;
      }
    }
    if (!has_child) {
      leaves.push_back(element.element);
    }
  }

  for (const auto& element : plant.snapshot.elements) {
    if (rng.chance(15)) {
      continue;  // never reported at all
    }
    ElectricalObservation observation =
        make_observation(element.element, element.kind, now, sequence);
    const bool de_energized = rng.chance(15);
    const bool tripped = rng.chance(10);
    switch (element.kind) {
      case ElementKind::UtilityFeed: {
        if (de_energized) {
          observation.energization = EnergizationState::DeEnergized;
          observation.voltage = MilliVolts::from_value(0);
          observation.frequency = MilliHertz::from_value(0);
        } else if (rng.chance(15)) {
          observation.voltage = MilliVolts::from_value(180000);
        } else if (rng.chance(15)) {
          observation.frequency = MilliHertz::from_value(48000);
        } else if (rng.chance(8)) {
          observation.energization = EnergizationState::PartiallyEnergized;
        }
        break;
      }
      case ElementKind::Switchgear:
      case ElementKind::Bus:
      case ElementKind::Breaker:
      case ElementKind::Circuit: {
        if (tripped) {
          observation.breaker = BreakerPosition::Tripped;
        } else if (rng.chance(8)) {
          observation.breaker = BreakerPosition::Open;
        }
        if (de_energized) {
          observation.energization = EnergizationState::DeEnergized;
        }
        break;
      }
      case ElementKind::Pdu:
      case ElementKind::PduBranch:
      case ElementKind::UpsBus: {
        if (de_energized) {
          observation.energization = EnergizationState::DeEnergized;
        } else if (rng.chance(10)) {
          observation.energization = EnergizationState::PartiallyEnergized;
        }
        if (element.kind == ElementKind::UpsBus) {
          observation.has_reserve = true;
          const std::uint32_t pick = rng.below(4);
          observation.reserve = milli_percent_from_value(
              pick == 0 ? 10000 : (pick == 1 ? 40000 : (pick == 2 ? 60000 : 90000)));
        }
        break;
      }
      case ElementKind::UpsUnit: {
        if (de_energized) {
          observation.energization = EnergizationState::DeEnergized;
        } else if (rng.chance(10)) {
          observation.energization = EnergizationState::PartiallyEnergized;
        }
        observation.has_reserve = true;
        const std::uint32_t pick = rng.below(4);
        observation.reserve = milli_percent_from_value(
            pick == 0 ? 10000 : (pick == 1 ? 40000 : (pick == 2 ? 60000 : 90000)));
        observation.transfer = static_cast<TransferState>(
            1 + rng.below(5));  // OnUtility .. Isolated
        break;
      }
      case ElementKind::Generator: {
        observation.generator = static_cast<GeneratorState>(1 + rng.below(6));
        observation.transfer = static_cast<TransferState>(1 + rng.below(5));
        break;
      }
      case ElementKind::AutomaticTransferSwitch:
      case ElementKind::StaticTransferSwitch: {
        observation.transfer = static_cast<TransferState>(1 + rng.below(5));
        break;
      }
      case ElementKind::LoadGroup:
      case ElementKind::Rack:
      case ElementKind::Unknown:
        break;
    }
    if (rng.chance(15)) {
      observation.observed_at = Timestamp::from_value(now.value() - 30000);
    } else if (rng.chance(8)) {
      observation.observed_at = Timestamp::from_value(now.value() - 300000);
    } else if (rng.chance(5)) {
      observation.observed_at = Timestamp::from_value(now.value() + 5000);
    }
    if (rng.chance(5)) {
      observation.quality = EvidenceQuality::Bad;
    }
    plant.observations.push_back(std::move(observation));
  }

  const std::size_t observed = plant.observations.size();
  for (std::size_t i = 0; i < observed; ++i) {
    const RefToken element = plant.observations[i].element;
    if (std::find(leaves.begin(), leaves.end(), element) == leaves.end() || !rng.chance(10)) {
      continue;
    }
    ElectricalObservation duplicate = plant.observations[i];
    duplicate.origin = ObservationOrigin::ExternalMeter;
    duplicate.sequence = sequence;
    sequence = ObservationSequence::from_value(sequence.value() + 1);
    duplicate.energization = duplicate.energization == EnergizationState::Energized
                                 ? EnergizationState::DeEnergized
                                 : EnergizationState::Energized;
    plant.observations.push_back(std::move(duplicate));
  }
  return plant;
}

void check_case(const TopologySnapshot& snapshot,
                const std::vector<ElectricalObservation>& observations, Timestamp now,
                const ElectricalPolicy& policy, const Bounds& bounds, std::uint64_t seed,
                std::uint32_t iteration) {
  const std::string repro = reproduction(seed, iteration, snapshot);
  auto built = TopologyIndex::build(snapshot, bounds);
  PFM_CHECK_MSG(built.ok(),
                repro + " topology refused: " + std::string{built.status().code_name()} + " " +
                    std::string{built.status().message()} + " @" +
                    built.status().context());
  if (!built.ok()) {
    return;
  }
  const TopologyIndex& index = built.value();

  auto library = classify(observations, now, index, policy, bounds);
  PFM_CHECK_MSG(library.ok(), repro.c_str());
  if (!library.ok()) {
    return;
  }
  const ClassificationResult& classification = library.value();
  PFM_CHECK_MSG(sorted_unique(classification.resolved_elements), repro.c_str());
  PFM_CHECK_MSG(sorted_unique(classification.unresolved_elements), repro.c_str());
  for (const auto& failure : classification.failures) {
    PFM_CHECK_MSG(failure_element_is_in(snapshot, failure.element), repro.c_str());
  }

  // Classification is deterministic for the same inputs.
  auto repeated = classify(observations, now, index, policy, bounds);
  PFM_CHECK_MSG(repeated.ok(), repro.c_str());
  if (repeated.ok()) {
    PFM_CHECK_MSG(failure_keys(repeated.value().failures) == failure_keys(classification.failures),
                  repro.c_str());
    PFM_CHECK_MSG(repeated.value().resolved_elements == classification.resolved_elements,
                  repro.c_str());
    PFM_CHECK_MSG(repeated.value().unresolved_elements == classification.unresolved_elements,
                  repro.c_str());
    PFM_CHECK_MSG(repeated.value().primary_class() == classification.primary_class(),
                  repro.c_str());
  }

  // The independent model derives the same classification.
  const pfmref::ModelTopology model = pfmref::ModelTopology::from(snapshot);
  const pfmref::ModelClassification expected =
      pfmref::model_classify(observations, now, model, policy);
  const std::vector<std::string> library_keys = failure_keys(classification.failures);
  const std::vector<std::string> model_keys = failure_keys(expected.failures);
  PFM_CHECK_MSG(library_keys == model_keys,
                repro + " classification library=[" + join_keys(library_keys) + "] model=[" +
                    join_keys(model_keys) + "]");
  PFM_CHECK_MSG(classification.resolved_elements == expected.resolved,
                repro + " resolved=[" + refs_to_text(classification.resolved_elements) +
                    "] model=[" + refs_to_text(expected.resolved) + "]");
  PFM_CHECK_MSG(classification.unresolved_elements == expected.unresolved,
                repro + " unresolved=[" + refs_to_text(classification.unresolved_elements) +
                    "] model=[" + refs_to_text(expected.unresolved) + "]");

  // The affected scope matches the independent closure, is sorted, unique, and
  // contains every failed element.
  auto scoped = resolve_scope(classification, index, policy, bounds);
  PFM_CHECK_MSG(scoped.ok(), repro.c_str());
  if (!scoped.ok()) {
    return;
  }
  const AffectedScope& scope = scoped.value();
  const pfmref::ModelScope expected_scope =
      pfmref::model_scope(model, classification.failures, classification.resolved_elements,
                          classification.unresolved_elements);
  PFM_CHECK_MSG(scope.failed_elements == expected_scope.failed_elements,
                repro + " failed=[" + refs_to_text(scope.failed_elements) + "] model=[" +
                    refs_to_text(expected_scope.failed_elements) + "]");
  PFM_CHECK_MSG(scope.impacted_elements == expected_scope.impacted_elements,
                repro + " impacted=[" + refs_to_text(scope.impacted_elements) + "] model=[" +
                    refs_to_text(expected_scope.impacted_elements) + "]");
  PFM_CHECK_MSG(scope.load_groups == expected_scope.load_groups,
                repro + " load_groups=[" + refs_to_text(scope.load_groups) + "] model=[" +
                    refs_to_text(expected_scope.load_groups) + "]");
  PFM_CHECK_MSG(scope.domains == expected_scope.domains, repro + " domains differ");
  PFM_CHECK_MSG(scope.isolation_points == expected_scope.isolation_points,
                repro + " isolation_points=[" + refs_to_text(scope.isolation_points) +
                    "] model=[" + refs_to_text(expected_scope.isolation_points) + "]");
  PFM_CHECK_MSG(scope.shared_domains == expected_scope.shared_domains,
                repro + " shared_domains differ");
  PFM_CHECK_MSG(scope.unresolved_upstream == expected_scope.unresolved_upstream,
                repro + " unresolved_upstream=[" + refs_to_text(scope.unresolved_upstream) +
                    "] model=[" + refs_to_text(expected_scope.unresolved_upstream) + "]");
  PFM_CHECK_MSG(scope.unevidenced_elements == expected_scope.unevidenced_elements,
                repro + " unevidenced=[" + refs_to_text(scope.unevidenced_elements) +
                    "] model=[" + refs_to_text(expected_scope.unevidenced_elements) + "]");
  PFM_CHECK_MSG(scope.shared_domain_impacted == expected_scope.shared_domain_impacted,
                repro.c_str());
  PFM_CHECK_MSG(scope.upstream_evidence_unresolved == expected_scope.upstream_evidence_unresolved,
                repro.c_str());
  PFM_CHECK_MSG(sorted_unique(scope.impacted_elements), repro.c_str());
  PFM_CHECK_MSG(sorted_unique(scope.isolation_points), repro.c_str());
  PFM_CHECK_MSG(sorted_unique(scope.unevidenced_elements), repro.c_str());
  for (const auto& failed : scope.failed_elements) {
    if (failed.kind() == RefKind::FailureDomain) {
      // A promoted failure domain is inside the boundary as a domain, not as
      // an impacted electrical element.
      PFM_CHECK_MSG(std::find(scope.domains.begin(), scope.domains.end(), failed) !=
                        scope.domains.end(),
                    repro + " failed domain not in scope.domains: " + failed.to_string());
      continue;
    }
    PFM_CHECK_MSG(scope.contains(failed),
                  repro + " failed element not impacted: " + failed.to_string());
  }

  // Planning is reproducible, and the recovery gates agree with the model.
  PlanInputs inputs;
  inputs.incident = IncidentId::from_value(1);
  inputs.incident_generation = IncidentGeneration::from_value(1);
  inputs.epoch = ControlEpoch::from_value(1);
  inputs.revision = StateRevision::from_value(1);
  inputs.next_plan_generation = PlanGeneration::from_value(1);
  inputs.now = now;
  inputs.policy = policy;
  inputs.bounds = bounds;
  inputs.topology = &index;
  inputs.current_observations = observations;
  inputs.first_request_id = ResponseRequestId::from_value(1);
  inputs.first_attempt_id = AttemptId::from_value(1);
  auto planned = plan_response(inputs);
  PFM_CHECK_MSG(planned.ok(), repro.c_str());
  if (!planned.ok()) {
    return;
  }
  const ResponsePlan& plan = planned.value();
  PFM_CHECK_MSG(std::is_sorted(plan.requests.begin(), plan.requests.end(), request_key_less),
                repro.c_str());
  auto replanned = plan_response(inputs);
  PFM_CHECK_MSG(replanned.ok(), repro.c_str());
  if (replanned.ok()) {
    PFM_CHECK_MSG(replanned.value().fingerprint == plan.fingerprint, repro.c_str());
  }

  RecoveryInputs recovery;
  recovery.now = now;
  recovery.policy = policy;
  recovery.bounds = bounds;
  recovery.plan = &plan;
  recovery.current_observations = observations;
  recovery.has_stable_since = true;
  recovery.stable_since = Timestamp::from_value(now.value() - policy.stability_dwell.value());
  recovery.operator_authorized = true;
  recovery.authorized_generation = inputs.incident_generation;
  recovery.incident_generation = inputs.incident_generation;
  recovery.incident_scope = plan.scope.impacted_elements;
  for (const auto& failed : plan.scope.failed_elements) {
    recovery.incident_scope.push_back(failed);
  }
  auto assessment = assess_recovery(recovery);
  PFM_CHECK_MSG(assessment.ok(), repro.c_str());
  if (!assessment.ok()) {
    return;
  }

  pfmref::ModelRecoveryInputs model_inputs;
  model_inputs.now = recovery.now;
  model_inputs.policy = recovery.policy;
  model_inputs.plan = &plan;
  model_inputs.incident_scope = recovery.incident_scope;
  model_inputs.observations = recovery.current_observations;
  model_inputs.obligations = recovery.obligations;
  model_inputs.has_stable_since = recovery.has_stable_since;
  model_inputs.stable_since = recovery.stable_since;
  model_inputs.operator_authorized = recovery.operator_authorized;
  model_inputs.authorized_generation = recovery.authorized_generation;
  model_inputs.incident_generation = recovery.incident_generation;
  const pfmref::ModelRecovery model_assessment = pfmref::model_recovery(model_inputs);
  PFM_CHECK_MSG(assessment.value().eligible == model_assessment.eligible,
                repro + " eligibility library=" +
                    (assessment.value().eligible ? "eligible" : "blocked") +
                    " model=" + (model_assessment.eligible ? "eligible" : "blocked") +
                    " summary=" + assessment.value().summarize());
  for (const RecoveryGate gate :
       {RecoveryGate::FaultCleared, RecoveryGate::IsolationVerified,
        RecoveryGate::DeEnergizationVerified, RecoveryGate::BreakerStateProven,
        RecoveryGate::TransferStable, RecoveryGate::GenerationStable,
        RecoveryGate::ReserveRestored, RecoveryGate::SharedDomainResolved,
        RecoveryGate::UpstreamEvidenceCurrent, RecoveryGate::ObligationsSatisfied,
        RecoveryGate::EvidenceCurrent, RecoveryGate::StabilityDwell,
        RecoveryGate::OperatorAuthorization, RecoveryGate::RequestsSettled}) {
    PFM_CHECK_MSG(gate_satisfied(assessment.value(), gate) == model_assessment.gate(gate),
                  repro + " gate " + std::string{recovery_gate_name(gate)});
  }
}

TopologySnapshot chain_snapshot(std::uint32_t count) {
  TopoBuilder builder;
  builder.add(ElementKind::UtilityFeed, "chain-0");
  RefToken parent = make_ref(RefKind::UtilityFeed, "chain-0");
  for (std::uint32_t i = 1; i < count; ++i) {
    const std::string name = "chain-" + std::to_string(i);
    builder.add(ElementKind::Bus, name, parent.to_string());
    parent = make_ref(RefKind::Bus, name);
  }
  return builder.snapshot;
}

std::vector<ElectricalObservation> chain_observations(const TopologySnapshot& snapshot,
                                                      Timestamp now,
                                                      ObservationSequence& sequence,
                                                      bool energize) {
  std::vector<ElectricalObservation> observations;
  for (const auto& element : snapshot.elements) {
    ElectricalObservation observation =
        make_observation(element.element, element.kind, now, sequence);
    if (!energize) {
      observation.energization = EnergizationState::DeEnergized;
      observation.voltage = MilliVolts::from_value(0);
      observation.frequency = MilliHertz::from_value(0);
    }
    observations.push_back(std::move(observation));
  }
  return observations;
}

PFM_TEST(randomized_property_checks) {
  const std::uint64_t seeds[] = {0x5EED0001ULL, 0x0BADF00DULL, 0x1234567890ABCDEFULL};
  const std::uint32_t iterations = 250;
  ElectricalPolicy policy = default_policy();
  policy.generation = PolicyGeneration::from_value(1);
  const Bounds bounds;
  const Timestamp now = timestamp_from_unix_millis(1767225600000LL);

  for (const std::uint64_t seed : seeds) {
    std::printf("state_machine_tests: randomized seed=%s iterations=%u topologies=%s\n",
                hex_seed(seed).c_str(), iterations, "random");
    std::fflush(stdout);
    Lcg rng(seed);
    ObservationSequence sequence = ObservationSequence::from_value(1);
    for (std::uint32_t iteration = 0; iteration < iterations; ++iteration) {
      RandomPlant plant = make_random_plant(rng, now, sequence);
      check_case(plant.snapshot, plant.observations, now, policy, bounds, seed, iteration);
    }
  }
  std::printf("state_machine_tests: randomized property checks used seeds %s, %s, %s\n",
              hex_seed(seeds[0]).c_str(), hex_seed(seeds[1]).c_str(), hex_seed(seeds[2]).c_str());
  std::fflush(stdout);
}

PFM_TEST(randomized_boundary_cases) {
  ElectricalPolicy policy = default_policy();
  policy.generation = PolicyGeneration::from_value(1);
  const Bounds bounds;
  const Timestamp now = timestamp_from_unix_millis(1767225600000LL);
  ObservationSequence sequence = ObservationSequence::from_value(1);

  // One element, and it failed.
  {
    TopoBuilder builder;
    builder.add(ElementKind::UtilityFeed, "only-1");
    std::vector<ElectricalObservation> observations;
    ElectricalObservation observation = make_observation(
        make_ref(RefKind::UtilityFeed, "only-1"), ElementKind::UtilityFeed, now, sequence);
    observation.energization = EnergizationState::DeEnergized;
    observation.voltage = MilliVolts::from_value(0);
    observation.frequency = MilliHertz::from_value(0);
    observations.push_back(std::move(observation));
    check_case(builder.snapshot, observations, now, policy, bounds, 0xFEED0001ULL, 0);
  }
  // Maximum fan-out, nothing failed.
  {
    TopoBuilder builder;
    builder.add(ElementKind::UtilityFeed, "fan-feed");
    for (int i = 0; i < 64; ++i) {
      builder.add(ElementKind::Breaker, "fan-brk-" + std::to_string(i),
                  "utility-feed:fan-feed");
    }
    std::vector<ElectricalObservation> observations =
        chain_observations(builder.snapshot, now, sequence, true);
    check_case(builder.snapshot, observations, now, policy, bounds, 0xFEED0002ULL, 0);
  }
  // Maximum fan-out with every child de-energized while the feed is live.
  {
    TopoBuilder builder;
    builder.add(ElementKind::UtilityFeed, "fan-feed");
    for (int i = 0; i < 64; ++i) {
      builder.add(ElementKind::Breaker, "fan-brk-" + std::to_string(i),
                  "utility-feed:fan-feed");
    }
    std::vector<ElectricalObservation> observations;
    for (const auto& element : builder.snapshot.elements) {
      ElectricalObservation observation =
          make_observation(element.element, element.kind, now, sequence);
      if (element.kind == ElementKind::Breaker) {
        observation.energization = EnergizationState::DeEnergized;
      }
      observations.push_back(std::move(observation));
    }
    check_case(builder.snapshot, observations, now, policy, bounds, 0xFEED0003ULL, 0);
  }
  // Maximum fan-out with the feed lost: every child is collateral.
  {
    TopoBuilder builder;
    builder.add(ElementKind::UtilityFeed, "fan-feed");
    for (int i = 0; i < 64; ++i) {
      builder.add(ElementKind::Breaker, "fan-brk-" + std::to_string(i),
                  "utility-feed:fan-feed");
    }
    std::vector<ElectricalObservation> observations =
        chain_observations(builder.snapshot, now, sequence, false);
    check_case(builder.snapshot, observations, now, policy, bounds, 0xFEED0004ULL, 0);
  }
  // All failed along a chain: only the root is a root cause.
  {
    const TopologySnapshot snapshot = chain_snapshot(6);
    const std::vector<ElectricalObservation> observations =
        chain_observations(snapshot, now, sequence, false);
    check_case(snapshot, observations, now, policy, bounds, 0xFEED0005ULL, 0);
  }
  // None failed along the same chain.
  {
    const TopologySnapshot snapshot = chain_snapshot(6);
    const std::vector<ElectricalObservation> observations =
        chain_observations(snapshot, now, sequence, true);
    check_case(snapshot, observations, now, policy, bounds, 0xFEED0006ULL, 0);
  }
  // Every element unresolved: the evidence is present but no longer current.
  {
    const TopologySnapshot snapshot = chain_snapshot(6);
    std::vector<ElectricalObservation> observations =
        chain_observations(snapshot, now, sequence, true);
    for (auto& observation : observations) {
      observation.observed_at = Timestamp::from_value(now.value() - 300000);
    }
    check_case(snapshot, observations, now, policy, bounds, 0xFEED0007ULL, 0);
  }
  // No element reports at all.
  {
    const TopologySnapshot snapshot = chain_snapshot(6);
    const std::vector<ElectricalObservation> observations;
    check_case(snapshot, observations, now, policy, bounds, 0xFEED0008ULL, 0);
  }
}

}  // namespace

PFM_TEST_MAIN()



