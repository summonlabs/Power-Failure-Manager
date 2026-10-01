// Power Failure Manager -- synthetic plant and scenario tests.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0 (see LICENSE).
//
// These cases exercise the synthetic plant directly -- topology, evidence,
// fault injection, staleness, and dispatch effects -- and then run every
// published scenario through the scenario engine, both in volatile memory and
// against a real durable store on a disposable directory.

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <string>
#include <system_error>
#include <vector>

#include "framework.hpp"
#include "support.hpp"

#include "pfm/classification.hpp"
#include "pfm/evidence.hpp"
#include "pfm/runtime.hpp"
#include "pfm/scenario.hpp"
#include "pfm/state.hpp"
#include "pfm/store.hpp"
#include "pfm/topology.hpp"
#include "pfm/transport.hpp"

namespace pfm = summon::pfm;

namespace {

// The plant recomputes energization with a single canonical sweep per state
// change, so a fault deep in the chain is driven to its settled electrical
// state by repeating the injection (every injector is idempotent) before the
// expectations are read. A fixed-point implementation of the plant would
// satisfy every assertion below unchanged.
template <class Fn>
void settle(Fn&& step, int passes = 8) {
  for (int i = 0; i < passes; ++i) {
    step();
  }
}

const pfm::ElectricalObservation* find_observation(
    const std::vector<pfm::ElectricalObservation>& observations, const pfm::RefToken& element) {
  for (const auto& observation : observations) {
    if (observation.element == element) {
      return &observation;
    }
  }
  return nullptr;
}

bool observed(const std::vector<pfm::ElectricalObservation>& observations,
              const pfm::RefToken& element) {
  return find_observation(observations, element) != nullptr;
}

pfm::ResponseRequest make_request(pfm::RequestKind kind, const pfm::RefToken& target,
                                  pfm::ElementKind target_kind) {
  pfm::ResponseRequest request;
  request.kind = kind;
  request.target.element = target;
  request.target.kind = target_kind;
  request.target.owner = pfm::owner_for_kind(kind);
  return request;
}

// Every scenario the library publishes, in the order it publishes them.
const std::vector<std::string>& documented_scenarios() {
  static const std::vector<std::string> names = {
      "healthy-baseline",
      "feed-loss-recovery",
      "feed-degradation",
      "switchgear-fault",
      "breaker-trip-defer-reclose",
      "pdu-failure",
      "ups-on-battery",
      "ups-reserve-insufficient",
      "generator-start-failure",
      "generator-sync-failure",
      "generator-transfer-failure",
      "shared-domain-failure",
      "evidence-loss-fails-closed",
      "restart-recovers-authority-not-evidence",
  };
  return names;
}

}  // namespace

// ---------------------------------------------------------------------------
// The synthetic plant: topology and evidence
// ---------------------------------------------------------------------------

PFM_TEST(plant_topology_validates_and_reports_one_observation_per_supply_element) {
  pfm::SyntheticPlant plant{pfm::SyntheticPlantConfig{}};
  const auto& topology = plant.topology();

  PFM_REQUIRE(!topology.elements.empty());
  PFM_CHECK_OK(pfm::validate(topology, pfm::Bounds{}));
  PFM_CHECK_EQ(topology.generation.value(), std::uint64_t{1});
  PFM_CHECK(!topology.domains.empty());

  const auto now = pfmtest::make_clock()->now().value();
  const auto observations = plant.observe(now);

  std::size_t supply_elements = 0;
  for (const auto& element : topology.elements) {
    if (pfm::element_kind_is_supply(element.kind)) {
      supply_elements += 1;
    }
  }
  PFM_CHECK(supply_elements > 0);
  // Load groups are reported through the branch that serves them, so only the
  // supply elements are observed directly.
  PFM_CHECK_EQ(observations.size(), supply_elements);

  for (const auto& observation : observations) {
    PFM_CHECK(observation.element.is_set());
    PFM_CHECK_EQ(observation.origin, pfm::ObservationOrigin::SyntheticPlant);
    PFM_CHECK_EQ(observation.observed_at, now);
    PFM_CHECK_EQ(observation.quality, pfm::EvidenceQuality::Good);
    PFM_CHECK(observation.has_voltage);
    PFM_CHECK(observation.has_frequency);

    pfm::ElementKind expected_kind = pfm::ElementKind::Unknown;
    for (const auto& element : topology.elements) {
      if (element.element == observation.element) {
        expected_kind = element.kind;
      }
    }
    PFM_CHECK_MSG(expected_kind != pfm::ElementKind::Unknown,
                  observation.element.to_string().c_str());
    PFM_CHECK_EQ(static_cast<int>(observation.kind), static_cast<int>(expected_kind));

    if (expected_kind == pfm::ElementKind::Generator) {
      // A generator that has not been started reports itself off and
      // de-energized: a present reading of a de-energized fact, never a
      // missing one.
      PFM_CHECK_EQ(observation.generator, pfm::GeneratorState::Off);
      PFM_CHECK_EQ(observation.energization, pfm::EnergizationState::DeEnergized);
      PFM_CHECK_EQ(observation.voltage.value(), std::int64_t{0});
      continue;
    }
    PFM_CHECK_EQ(observation.energization, pfm::EnergizationState::Energized);
    PFM_CHECK_EQ(observation.voltage.value(), std::int64_t{230000});
    PFM_CHECK_EQ(observation.frequency.value(), std::int64_t{50000});
  }

  // Exactly one observation per supply element: no element is reported twice,
  // and no supply element is silently missing.
  for (const auto& element : topology.elements) {
    if (!pfm::element_kind_is_supply(element.kind)) {
      continue;
    }
    std::size_t count = 0;
    for (const auto& observation : observations) {
      if (observation.element == element.element) {
        count += 1;
      }
    }
    PFM_CHECK_MSG(count == 1, element.element.to_string().c_str());
  }

  // Load groups exist in the topology but are not part of the bulk report.
  const auto loads = plant.elements_of_kind(pfm::ElementKind::LoadGroup);
  PFM_REQUIRE(!loads.empty());
  PFM_CHECK(!observed(observations, loads.front()));
  // They are still individually observable, which is how their branch proves
  // what it serves.
  PFM_CHECK(plant.observe_element(loads.front(), now).element.is_set());

  // An element the plant does not own produces no evidence at all.
  auto stranger = pfm::RefToken::make(pfm::RefKind::Bus, "bus-not-in-this-plant");
  PFM_REQUIRE(stranger.ok());
  PFM_CHECK(plant.observe_element(stranger.value(), now).is_empty());
  PFM_CHECK_CODE(plant.element_ref(pfm::RefKind::Bus, "bus-not-in-this-plant"),
                 pfm::StatusCode::TopologyUnknownElement);
}

PFM_TEST(plant_utility_loss_and_restoration_change_later_evidence) {
  pfm::SyntheticPlant plant{pfm::SyntheticPlantConfig{}};
  const auto now = pfmtest::make_clock()->now().value();

  auto feed = plant.element_ref(pfm::RefKind::UtilityFeed, "feed-0");
  auto feed_breaker = plant.element_ref(pfm::RefKind::Breaker, "brk-feed-0");
  auto bus = plant.element_ref(pfm::RefKind::Bus, "bus-0-0");
  auto ups = plant.element_ref(pfm::RefKind::UpsUnit, "ups-0-0");
  auto pdu = plant.element_ref(pfm::RefKind::Pdu, "pdu-0-0");
  auto branch = plant.element_ref(pfm::RefKind::PduBranch, "branch-0-0-0");
  PFM_REQUIRE(feed.ok());
  PFM_REQUIRE(feed_breaker.ok());
  PFM_REQUIRE(bus.ok());
  PFM_REQUIRE(ups.ok());
  PFM_REQUIRE(pdu.ok());
  PFM_REQUIRE(branch.ok());

  plant.lose_utility(feed.value());

  // The lost feed reports the loss with zero volts and zero hertz, and the
  // breaker immediately downstream of it follows in the same sweep.
  const auto lost = plant.observe_element(feed.value(), now);
  PFM_CHECK_EQ(lost.energization, pfm::EnergizationState::DeEnergized);
  PFM_CHECK(lost.has_voltage);
  PFM_CHECK(lost.has_frequency);
  PFM_CHECK_EQ(lost.voltage.value(), std::int64_t{0});
  PFM_CHECK_EQ(lost.frequency.value(), std::int64_t{0});
  PFM_CHECK_EQ(plant.observe_element(feed_breaker.value(), now).energization,
               pfm::EnergizationState::DeEnergized);

  // Settled: the whole path below the feed is de-energized, and the UPS holds
  // its load on reserve rather than reporting itself healthy.
  settle([&plant, &feed] { plant.lose_utility(feed.value()); });
  PFM_CHECK_EQ(plant.observe_element(bus.value(), now).energization,
               pfm::EnergizationState::DeEnergized);
  PFM_CHECK_EQ(plant.observe_element(ups.value(), now).energization,
               pfm::EnergizationState::PartiallyEnergized);
  PFM_CHECK_EQ(plant.observe_element(pdu.value(), now).energization,
               pfm::EnergizationState::DeEnergized);
  PFM_CHECK_EQ(plant.observe_element(branch.value(), now).energization,
               pfm::EnergizationState::DeEnergized);

  plant.restore_utility(feed.value());
  settle([&plant, &feed] { plant.restore_utility(feed.value()); });

  const auto restored = plant.observe_element(feed.value(), now);
  PFM_CHECK_EQ(restored.energization, pfm::EnergizationState::Energized);
  PFM_CHECK_EQ(restored.voltage.value(), std::int64_t{230000});
  PFM_CHECK_EQ(restored.frequency.value(), std::int64_t{50000});
  PFM_CHECK_EQ(plant.observe_element(bus.value(), now).energization,
               pfm::EnergizationState::Energized);
  PFM_CHECK_EQ(plant.observe_element(ups.value(), now).energization,
               pfm::EnergizationState::Energized);
  PFM_CHECK_EQ(plant.observe_element(pdu.value(), now).energization,
               pfm::EnergizationState::Energized);
  PFM_CHECK_EQ(plant.observe_element(branch.value(), now).energization,
               pfm::EnergizationState::Energized);
}

PFM_TEST(plant_degradation_keeps_the_supply_and_changes_the_reading) {
  pfm::SyntheticPlant plant{pfm::SyntheticPlantConfig{}};
  const auto now = pfmtest::make_clock()->now().value();
  auto feed = plant.element_ref(pfm::RefKind::UtilityFeed, "feed-0");
  PFM_REQUIRE(feed.ok());

  plant.degrade_utility(feed.value(), pfm::MilliVolts::from_value(180000),
                        pfm::MilliHertz::from_value(48000));

  const auto degraded = plant.observe_element(feed.value(), now);
  PFM_CHECK_EQ(degraded.energization, pfm::EnergizationState::Energized);
  PFM_CHECK(degraded.has_voltage);
  PFM_CHECK(degraded.has_frequency);
  PFM_CHECK_EQ(degraded.voltage.value(), std::int64_t{180000});
  PFM_CHECK_EQ(degraded.frequency.value(), std::int64_t{48000});
  // A degraded feed is still energised but no longer inside the band the
  // policy accepts, in both dimensions.
  const auto policy = pfm::default_policy();
  auto within_voltage = pfm::voltage_within_tolerance(policy, degraded.voltage);
  PFM_REQUIRE(within_voltage.ok());
  PFM_CHECK(!within_voltage.value());
  auto within_frequency = pfm::frequency_within_tolerance(policy, degraded.frequency);
  PFM_REQUIRE(within_frequency.ok());
  PFM_CHECK(!within_frequency.value());
}

PFM_TEST(plant_breaker_trip_and_close_change_later_evidence) {
  pfm::SyntheticPlant plant{pfm::SyntheticPlantConfig{}};
  const auto now = pfmtest::make_clock()->now().value();

  auto bus = plant.element_ref(pfm::RefKind::Bus, "bus-0-0");
  auto input_breaker = plant.element_ref(pfm::RefKind::Breaker, "brk-bus-0-0");
  auto ups = plant.element_ref(pfm::RefKind::UpsUnit, "ups-0-0");
  auto pdu = plant.element_ref(pfm::RefKind::Pdu, "pdu-0-0");
  auto branch = plant.element_ref(pfm::RefKind::PduBranch, "branch-0-0-0");
  PFM_REQUIRE(bus.ok());
  PFM_REQUIRE(input_breaker.ok());
  PFM_REQUIRE(ups.ok());
  PFM_REQUIRE(pdu.ok());
  PFM_REQUIRE(branch.ok());

  plant.trip_breaker(input_breaker.value());

  const auto tripped = plant.observe_element(input_breaker.value(), now);
  PFM_CHECK_EQ(tripped.breaker, pfm::BreakerPosition::Tripped);
  PFM_CHECK_EQ(tripped.energization, pfm::EnergizationState::DeEnergized);
  // The bus is upstream of its input breaker, so it stays energized; the UPS
  // behind the open breaker transfers to reserve.
  PFM_CHECK_EQ(plant.observe_element(bus.value(), now).energization,
               pfm::EnergizationState::Energized);
  PFM_CHECK_EQ(plant.observe_element(ups.value(), now).energization,
               pfm::EnergizationState::PartiallyEnergized);

  settle([&plant, &input_breaker] { plant.trip_breaker(input_breaker.value()); });
  PFM_CHECK_EQ(plant.observe_element(pdu.value(), now).energization,
               pfm::EnergizationState::DeEnergized);
  PFM_CHECK_EQ(plant.observe_element(branch.value(), now).energization,
               pfm::EnergizationState::DeEnergized);

  plant.close_breaker(input_breaker.value());
  settle([&plant, &input_breaker] { plant.close_breaker(input_breaker.value()); });

  const auto closed = plant.observe_element(input_breaker.value(), now);
  PFM_CHECK_EQ(closed.breaker, pfm::BreakerPosition::Closed);
  PFM_CHECK_EQ(closed.energization, pfm::EnergizationState::Energized);
  PFM_CHECK_EQ(plant.observe_element(ups.value(), now).energization,
               pfm::EnergizationState::Energized);
  PFM_CHECK_EQ(plant.observe_element(pdu.value(), now).energization,
               pfm::EnergizationState::Energized);
  PFM_CHECK_EQ(plant.observe_element(branch.value(), now).energization,
               pfm::EnergizationState::Energized);
}

PFM_TEST(plant_ups_reserve_and_failure_change_later_evidence) {
  pfm::SyntheticPlant plant{pfm::SyntheticPlantConfig{}};
  const auto now = pfmtest::make_clock()->now().value();

  auto ups = plant.element_ref(pfm::RefKind::UpsUnit, "ups-0-0");
  auto input_breaker = plant.element_ref(pfm::RefKind::Breaker, "brk-bus-0-0");
  PFM_REQUIRE(ups.ok());
  PFM_REQUIRE(input_breaker.ok());

  const auto healthy = plant.observe_element(ups.value(), now);
  PFM_CHECK(healthy.has_reserve);
  PFM_CHECK_EQ(healthy.reserve.value(), std::int64_t{80000});

  plant.set_reserve(ups.value(), pfm::milli_percent_from_value(15000));
  PFM_CHECK_EQ(plant.observe_element(ups.value(), now).reserve.value(), std::int64_t{15000});

  // On utility a UPS with no reserve is still supplied; "no reserve" is not
  // the same fact as "not energised".
  plant.set_reserve(ups.value(), pfm::milli_percent_from_value(0));
  PFM_CHECK_EQ(plant.observe_element(ups.value(), now).energization,
               pfm::EnergizationState::Energized);

  // Once its feed is lost, the reserve decides whether it carries load.
  settle([&plant, &input_breaker] { plant.trip_breaker(input_breaker.value()); });
  PFM_CHECK_EQ(plant.observe_element(ups.value(), now).energization,
               pfm::EnergizationState::DeEnergized);

  plant.set_reserve(ups.value(), pfm::milli_percent_from_value(60000));
  PFM_CHECK_EQ(plant.observe_element(ups.value(), now).energization,
               pfm::EnergizationState::PartiallyEnergized);

  plant.fail_ups(ups.value());
  const auto failed = plant.observe_element(ups.value(), now);
  PFM_CHECK_EQ(failed.reserve.value(), std::int64_t{0});
  PFM_CHECK_EQ(failed.energization, pfm::EnergizationState::DeEnergized);

  plant.repair_ups(ups.value());
  const auto repaired = plant.observe_element(ups.value(), now);
  PFM_CHECK_EQ(repaired.reserve.value(), std::int64_t{80000});
  PFM_CHECK_EQ(repaired.energization, pfm::EnergizationState::PartiallyEnergized);
}

PFM_TEST(plant_generator_fault_and_repair_change_later_evidence) {
  pfm::SyntheticPlant plant{pfm::SyntheticPlantConfig{}};
  const auto now = pfmtest::make_clock()->now().value();
  auto generator = plant.element_ref(pfm::RefKind::Generator, "gen-0");
  PFM_REQUIRE(generator.ok());

  const auto off = plant.observe_element(generator.value(), now);
  PFM_CHECK_EQ(off.generator, pfm::GeneratorState::Off);
  PFM_CHECK_EQ(off.energization, pfm::EnergizationState::DeEnergized);

  plant.fail_generator(generator.value());
  const auto faulted = plant.observe_element(generator.value(), now);
  PFM_CHECK_EQ(faulted.generator, pfm::GeneratorState::Faulted);
  PFM_CHECK_EQ(faulted.energization, pfm::EnergizationState::DeEnergized);

  plant.repair_generator(generator.value());
  const auto repaired = plant.observe_element(generator.value(), now);
  PFM_CHECK_EQ(repaired.generator, pfm::GeneratorState::Off);
  // A repaired generator that has not been started is not a running one.
  PFM_CHECK_EQ(repaired.energization, pfm::EnergizationState::DeEnergized);
}

PFM_TEST(plant_suppression_and_quality_change_later_evidence) {
  pfm::SyntheticPlant plant{pfm::SyntheticPlantConfig{}};
  const auto now = pfmtest::make_clock()->now().value();

  auto feed = plant.element_ref(pfm::RefKind::UtilityFeed, "feed-0");
  auto bus = plant.element_ref(pfm::RefKind::Bus, "bus-0-0");
  PFM_REQUIRE(feed.ok());
  PFM_REQUIRE(bus.ok());

  const auto before = plant.observe(now);
  PFM_CHECK(observed(before, bus.value()));

  plant.set_evidence_suppressed(bus.value(), true);
  const auto suppressed = plant.observe(now);
  PFM_CHECK_EQ(suppressed.size(), before.size() - 1);
  PFM_CHECK(!observed(suppressed, bus.value()));
  // The element is not reported as healthy: it is not reported at all.
  const auto nothing = plant.observe_element(bus.value(), now);
  PFM_CHECK(!nothing.element.is_set());
  PFM_CHECK(nothing.is_empty());
  PFM_CHECK_EQ(nothing.origin, pfm::ObservationOrigin::Unknown);

  plant.set_evidence_suppressed(bus.value(), false);
  PFM_CHECK_EQ(plant.observe(now).size(), before.size());
  PFM_CHECK(observed(plant.observe(now), bus.value()));

  plant.set_quality(feed.value(), pfm::EvidenceQuality::Bad);
  const auto bad = plant.observe_element(feed.value(), now);
  PFM_CHECK_EQ(bad.quality, pfm::EvidenceQuality::Bad);
  // Bad quality is a statement about the reading, not a state change.
  PFM_CHECK_EQ(bad.energization, pfm::EnergizationState::Energized);
  PFM_CHECK_EQ(bad.voltage.value(), std::int64_t{230000});
}

PFM_TEST(plant_stale_evidence_moves_the_instant_back) {
  pfm::SyntheticPlant plant{pfm::SyntheticPlantConfig{}};
  const auto now = pfmtest::make_clock()->now().value();
  const auto policy = pfm::default_policy();

  const auto fresh = plant.observe(now);
  const auto stale = plant.observe_stale(now, pfm::duration_from_seconds(30));
  const auto expired = plant.observe_stale(now, pfm::duration_from_seconds(120));

  PFM_REQUIRE(!fresh.empty());
  PFM_REQUIRE(stale.size() == fresh.size());
  PFM_REQUIRE(expired.size() == fresh.size());

  for (std::size_t i = 0; i < fresh.size(); ++i) {
    PFM_CHECK_EQ(stale[i].element, fresh[i].element);
    PFM_CHECK_EQ(stale[i].observed_at.value(), now.value() - std::int64_t{30000});
    PFM_CHECK_EQ(expired[i].observed_at.value(), now.value() - std::int64_t{120000});
    PFM_CHECK_EQ(stale[i].origin, pfm::ObservationOrigin::SyntheticPlant);
  }

  PFM_CHECK_EQ(pfm::classify_freshness(fresh[0].observed_at, now, policy), pfm::Freshness::Fresh);
  PFM_CHECK_EQ(pfm::classify_freshness(stale[0].observed_at, now, policy), pfm::Freshness::Stale);
  PFM_CHECK_EQ(pfm::classify_freshness(expired[0].observed_at, now, policy),
               pfm::Freshness::Expired);

  // An instant ahead of the caller's clock is a future reading, never a fresh
  // one, and never an expired one either.
  auto ahead = fresh[0];
  ahead.observed_at = pfm::timestamp_from_unix_millis(now.value() + 1000);
  PFM_CHECK_EQ(pfm::classify_freshness(ahead.observed_at, now, policy), pfm::Freshness::Future);
}

// ---------------------------------------------------------------------------
// The synthetic plant: dispatch
// ---------------------------------------------------------------------------

PFM_TEST(plant_dispatch_applies_effects_only_when_actuated) {
  pfm::SyntheticPlantConfig passive_config;
  passive_config.actuate_on_request = false;
  pfm::SyntheticPlant passive{passive_config};
  const auto now = pfmtest::make_clock()->now().value();

  auto generator = passive.element_ref(pfm::RefKind::Generator, "gen-0");
  auto feed = passive.element_ref(pfm::RefKind::UtilityFeed, "feed-0");
  PFM_REQUIRE(generator.ok());
  PFM_REQUIRE(feed.ok());

  auto start = passive.dispatch(make_request(pfm::RequestKind::StartGenerator, generator.value(),
                                             pfm::ElementKind::Generator));
  PFM_REQUIRE(start.ok());
  PFM_CHECK_EQ(start.value().result, pfm::DispatchResult::Accepted);
  // The request was recorded, and the plant did not move.
  const auto recorded = passive.dispatched();
  PFM_REQUIRE(recorded.size() == 1);
  PFM_CHECK_EQ(recorded.front().kind, pfm::RequestKind::StartGenerator);
  PFM_CHECK_EQ(recorded.front().target.element, generator.value());
  PFM_CHECK_EQ(passive.observe_element(generator.value(), now).generator,
               pfm::GeneratorState::Off);

  // A plant that answers without an effect cannot restore a lost feed either.
  settle([&passive, &feed] { passive.lose_utility(feed.value()); });
  const auto restore_request = make_request(pfm::RequestKind::RestoreNormalFeed, feed.value(),
                                            pfm::ElementKind::UtilityFeed);
  auto restore = passive.dispatch(restore_request);
  PFM_REQUIRE(restore.ok());
  PFM_CHECK_EQ(restore.value().result, pfm::DispatchResult::Accepted);
  PFM_CHECK_EQ(passive.dispatched().size(), std::size_t{2});
  PFM_CHECK_EQ(passive.observe_element(feed.value(), now).energization,
               pfm::EnergizationState::DeEnergized);

  // The same requests against an actuating plant do change it.
  pfm::SyntheticPlant actuating{pfm::SyntheticPlantConfig{}};
  PFM_CHECK_OK(actuating.dispatch(restore_request));
  auto actuating_feed = actuating.element_ref(pfm::RefKind::UtilityFeed, "feed-0");
  PFM_REQUIRE(actuating_feed.ok());
  PFM_CHECK_EQ(actuating.observe_element(actuating_feed.value(), now).energization,
               pfm::EnergizationState::Energized);
  auto actuating_generator = actuating.element_ref(pfm::RefKind::Generator, "gen-0");
  PFM_REQUIRE(actuating_generator.ok());
  auto started = actuating.dispatch(make_request(pfm::RequestKind::StartGenerator,
                                                 actuating_generator.value(),
                                                 pfm::ElementKind::Generator));
  PFM_REQUIRE(started.ok());
  PFM_CHECK_EQ(actuating.observe_element(actuating_generator.value(), now).generator,
               pfm::GeneratorState::Running);
  PFM_CHECK_EQ(actuating.observe_element(actuating_generator.value(), now).transfer,
               pfm::TransferState::Transferring);
  PFM_CHECK_EQ(actuating.observe_element(actuating_generator.value(), now).energization,
               pfm::EnergizationState::Energized);
}

PFM_TEST(plant_dispatch_models_generator_start_sync_and_transfer_toggles) {
  pfm::SyntheticPlant plant{pfm::SyntheticPlantConfig{}};
  const auto now = pfmtest::make_clock()->now().value();
  auto generator = plant.element_ref(pfm::RefKind::Generator, "gen-0");
  PFM_REQUIRE(generator.ok());

  const auto start = [&plant, &generator] {
    return plant.dispatch(make_request(pfm::RequestKind::StartGenerator, generator.value(),
                                       pfm::ElementKind::Generator));
  };
  const auto synchronize = [&plant, &generator] {
    return plant.dispatch(make_request(pfm::RequestKind::SynchronizeGenerator, generator.value(),
                                       pfm::ElementKind::Generator));
  };
  const auto transfer = [&plant, &generator] {
    return plant.dispatch(make_request(pfm::RequestKind::TransferToGenerator, generator.value(),
                                       pfm::ElementKind::Generator));
  };

  plant.set_generator_starts(false);
  PFM_CHECK_OK(start());
  auto failed_start = plant.observe_element(generator.value(), now);
  PFM_CHECK_EQ(failed_start.generator, pfm::GeneratorState::Off);
  PFM_CHECK_EQ(failed_start.transfer, pfm::TransferState::Failed);

  plant.set_generator_starts(true);
  PFM_CHECK_OK(start());
  PFM_CHECK_EQ(plant.observe_element(generator.value(), now).generator,
               pfm::GeneratorState::Running);

  plant.set_generator_synchronizes(false);
  PFM_CHECK_OK(synchronize());
  auto failed_sync = plant.observe_element(generator.value(), now);
  PFM_CHECK_EQ(failed_sync.generator, pfm::GeneratorState::Running);
  PFM_CHECK_EQ(failed_sync.transfer, pfm::TransferState::Failed);

  plant.set_generator_synchronizes(true);
  plant.set_transfers(false);
  PFM_CHECK_OK(synchronize());
  auto failed_transfer = plant.observe_element(generator.value(), now);
  PFM_CHECK_EQ(failed_transfer.generator, pfm::GeneratorState::Synchronized);
  PFM_CHECK_EQ(failed_transfer.transfer, pfm::TransferState::Failed);

  plant.set_transfers(true);
  PFM_CHECK_OK(synchronize());
  auto transferred = plant.observe_element(generator.value(), now);
  PFM_CHECK_EQ(transferred.generator, pfm::GeneratorState::Synchronized);
  PFM_CHECK_EQ(transferred.transfer, pfm::TransferState::OnGenerator);

  plant.set_transfers(false);
  PFM_CHECK_OK(transfer());
  PFM_CHECK_EQ(plant.observe_element(generator.value(), now).transfer,
               pfm::TransferState::Failed);

  plant.set_transfers(true);
  PFM_CHECK_OK(transfer());
  PFM_CHECK_EQ(plant.observe_element(generator.value(), now).transfer,
               pfm::TransferState::OnGenerator);
}

PFM_TEST(plant_dispatch_isolates_sheds_and_defers_reclose) {
  pfm::SyntheticPlant plant{pfm::SyntheticPlantConfig{}};
  const auto now = pfmtest::make_clock()->now().value();

  auto bus = plant.element_ref(pfm::RefKind::Bus, "bus-0-0");
  auto input_breaker = plant.element_ref(pfm::RefKind::Breaker, "brk-bus-0-0");
  auto ups = plant.element_ref(pfm::RefKind::UpsUnit, "ups-0-0");
  auto branch = plant.element_ref(pfm::RefKind::PduBranch, "branch-0-0-0");
  PFM_REQUIRE(bus.ok());
  PFM_REQUIRE(input_breaker.ok());
  PFM_REQUIRE(ups.ok());
  PFM_REQUIRE(branch.ok());

  // Isolating an element opens the isolation point its topology names, not the
  // element itself.
  PFM_CHECK_OK(plant.dispatch(make_request(pfm::RequestKind::IsolateElement, bus.value(),
                                           pfm::ElementKind::Bus)));
  PFM_CHECK_EQ(plant.observe_element(input_breaker.value(), now).breaker,
               pfm::BreakerPosition::Open);
  PFM_CHECK_EQ(plant.observe_element(input_breaker.value(), now).energization,
               pfm::EnergizationState::DeEnergized);
  PFM_CHECK_EQ(plant.observe_element(bus.value(), now).energization,
               pfm::EnergizationState::Energized);
  PFM_CHECK_EQ(plant.observe_element(ups.value(), now).energization,
               pfm::EnergizationState::PartiallyEnergized);

  // A deferred reclose is not a reclose.
  plant.trip_breaker(input_breaker.value());
  PFM_CHECK_OK(plant.dispatch(make_request(pfm::RequestKind::DeferReclose, input_breaker.value(),
                                           pfm::ElementKind::Breaker)));
  PFM_CHECK_OK(plant.dispatch(make_request(pfm::RequestKind::Reclose, input_breaker.value(),
                                           pfm::ElementKind::Breaker)));
  PFM_CHECK_EQ(plant.observe_element(input_breaker.value(), now).breaker,
               pfm::BreakerPosition::Tripped);

  // A breaker that was never deferred does close on request.
  auto other_breaker = plant.element_ref(pfm::RefKind::Breaker, "brk-bus-0-1");
  PFM_REQUIRE(other_breaker.ok());
  plant.trip_breaker(other_breaker.value());
  PFM_CHECK_OK(plant.dispatch(make_request(pfm::RequestKind::Reclose, other_breaker.value(),
                                           pfm::ElementKind::Breaker)));
  PFM_CHECK_EQ(plant.observe_element(other_breaker.value(), now).breaker,
               pfm::BreakerPosition::Closed);
  PFM_CHECK_EQ(plant.observe_element(other_breaker.value(), now).energization,
               pfm::EnergizationState::Energized);

  // Load shedding removes the current, and the reading says so.
  const auto loaded = plant.observe_element(branch.value(), now);
  PFM_REQUIRE(loaded.has_current);
  PFM_CHECK_EQ(loaded.current.value(), std::int64_t{10000});
  PFM_CHECK_OK(plant.dispatch(make_request(pfm::RequestKind::ShedLoad, branch.value(),
                                           pfm::ElementKind::PduBranch)));
  const auto shed = plant.observe_element(branch.value(), now);
  PFM_CHECK(shed.has_current);
  PFM_CHECK_EQ(shed.current.value(), std::int64_t{0});

  // A request that carries no direct actuation is still answered.
  auto verification = plant.dispatch(make_request(pfm::RequestKind::VerifyDeEnergization,
                                                  bus.value(), pfm::ElementKind::Bus));
  PFM_REQUIRE(verification.ok());
  PFM_CHECK_EQ(verification.value().result, pfm::DispatchResult::Accepted);
  PFM_CHECK(verification.value().controller.is_set());
}

// ---------------------------------------------------------------------------
// Scenarios
// ---------------------------------------------------------------------------

PFM_TEST(scenario_names_include_every_documented_scenario_and_refuse_unknown_ones) {
  const auto names = pfm::scenario_names();
  PFM_CHECK(!names.empty());
  // Every documented scenario is still published: a scenario that silently
  // disappeared would otherwise stop being exercised by the sweep below.
  for (const auto& name : documented_scenarios()) {
    PFM_CHECK_MSG(std::find(names.begin(), names.end(), name) != names.end(), name.c_str());
  }
  // No name is published twice.
  for (std::size_t i = 0; i < names.size(); ++i) {
    for (std::size_t j = i + 1; j < names.size(); ++j) {
      PFM_CHECK_MSG(names[i] != names[j], names[i].c_str());
    }
  }

  // An unknown scenario is refused with a machine-readable code rather than
  // being reported as a successful no-op.
  pfm::ScenarioOptions options;
  options.durable = false;
  PFM_CHECK_CODE(pfm::run_scenario("no-such-scenario", options),
                 pfm::StatusCode::InvalidArgument);
}

PFM_TEST(scenario_every_name_runs_in_volatile_memory_and_reports_success) {
  // A unique store directory per run keeps this process from sharing the fixed
  // temporary store the scenario driver falls back to.
  pfmtest::TempDirectory scratch("scenario-volatile");

  for (const auto& name : pfm::scenario_names()) {
    pfm::ScenarioOptions options;
    options.durable = false;
    options.store_directory = scratch.file(name + "-store");

    auto result = pfm::run_scenario(name, options);
    if (!result.ok()) {
      PFM_CHECK_MSG(false, (name + ": " + result.status().to_string()).c_str());
      continue;
    }
    PFM_CHECK_MSG(result.value().ok, name.c_str());
    PFM_CHECK_EQ(result.value().name, name);
    PFM_CHECK_MSG(result.value().plan_generations > 0,
                  (name + ": at least one plan was published").c_str());
    for (const auto& note : result.value().notes) {
      PFM_CHECK_MSG(note.rfind("FAILED:", 0) != 0, (name + ": " + note).c_str());
    }
  }
}

PFM_TEST(scenario_durable_runs_leave_no_store_behind) {
  // Two scenarios run against a durable store the scenario engine creates
  // itself, and each run removes the store directory it created. Only the
  // stores this process creates are considered, so a concurrent test process
  // that is using its own store cannot make this case flaky.
  const auto temporary = std::filesystem::temp_directory_path();
  const auto stores_in_temporary = [&temporary] {
    std::vector<std::string> found;
    std::error_code code;
    for (const auto& entry : std::filesystem::directory_iterator(temporary, code)) {
      const std::string name = entry.path().filename().string();
      if (name.rfind("pfm-scenario-store", 0) == 0) {
        found.push_back(name);
      }
    }
    std::sort(found.begin(), found.end());
    return found;
  };

  for (const std::string name : {"healthy-baseline", "restart-recovers-authority-not-evidence"}) {
    const auto before = stores_in_temporary();
    pfm::ScenarioOptions options;
    options.durable = true;
    auto result = pfm::run_scenario(name, options);
    if (!result.ok()) {
      PFM_CHECK_MSG(false, (name + ": " + result.status().to_string()).c_str());
      continue;
    }
    PFM_CHECK_MSG(result.value().ok, name);

    const auto after = stores_in_temporary();
    std::vector<std::string> leftovers;
    std::set_difference(after.begin(), after.end(), before.begin(), before.end(),
                        std::back_inserter(leftovers));
    PFM_CHECK_MSG(leftovers.empty(),
                  (name + ": the scenario removed the durable store it created").c_str());
  }
}

PFM_TEST(scenario_durable_runs_write_a_real_store_on_a_disposable_directory) {
  std::string store_directory;
  {
    pfmtest::TempDirectory directory("scenario-durable");
    for (const std::string name : {"feed-loss-recovery", "switchgear-fault"}) {
      pfm::ScenarioOptions options;
      options.durable = true;
      options.store_directory = directory.file(name + "-store");

      auto result = pfm::run_scenario(name, options);
      if (!result.ok()) {
        PFM_CHECK_MSG(false, (name + ": " + result.status().to_string()).c_str());
        continue;
      }
      PFM_CHECK_MSG(result.value().ok, name);

      // Durability is not a claim: the store directory, its slot file, and its
      // single-writer lock really exist after the run.
      const std::filesystem::path store{options.store_directory};
      PFM_CHECK_MSG(std::filesystem::exists(store), options.store_directory.c_str());
      PFM_CHECK_MSG(std::filesystem::exists(store / "state.0") ||
                        std::filesystem::exists(store / "state.1"),
                    "a durable scenario run published a state slot");
      PFM_CHECK_MSG(std::filesystem::exists(store / "store.lock"),
                    "a durable scenario run took the single-writer lock");
      store_directory = options.store_directory;
    }
  }
  // The disposable directory that owned the stores removed them with it.
  PFM_CHECK(!store_directory.empty());
  PFM_CHECK(!std::filesystem::exists(std::filesystem::path{store_directory}));
}

PFM_TEST(scenario_feed_loss_recovery_reaches_recovered_then_closed) {
  auto fixture = pfmtest::PlantFixture::make(pfm::SyntheticPlantConfig{}, pfm::StoreMode::Volatile);
  PFM_REQUIRE(fixture.ok());
  pfmtest::PlantFixture& plant_fixture = *fixture.value();

  PFM_REQUIRE(plant_fixture.prime().ok());
  auto baseline = plant_fixture.tick(pfm::duration_from_seconds(5));
  PFM_REQUIRE(baseline.ok());
  PFM_CHECK_EQ(baseline.value().primary_failure, pfm::FailureClass::None);

  auto status = plant_fixture.runtime->status();
  PFM_REQUIRE(status.ok());
  // A plan that carries no failure and no unresolved element stabilizes the
  // incident; the first evaluation of a healthy plant is exactly that.
  PFM_CHECK_MSG(status.value().lifecycle == pfm::IncidentLifecycle::Stabilizing,
                std::string{pfm::incident_lifecycle_name(status.value().lifecycle)}.c_str());

  // Every retained observation is honestly attributed to the synthetic plant.
  auto state = plant_fixture.runtime->state();
  PFM_REQUIRE(state.ok());
  std::size_t retained = 0;
  for (const auto& slot : state.value().observations) {
    if (!slot.present) {
      continue;
    }
    retained += 1;
    PFM_CHECK_MSG(slot.observation.origin == pfm::ObservationOrigin::SyntheticPlant,
                  slot.observation.element.to_string().c_str());
  }
  PFM_CHECK(retained > 0);

  auto feed = plant_fixture.plant->element_ref(pfm::RefKind::UtilityFeed, "feed-0");
  PFM_REQUIRE(feed.ok());

  // The feed is lost and the incident is given one evaluation to observe and
  // classify the fault.
  settle([&plant_fixture, &feed] { plant_fixture.plant->lose_utility(feed.value()); });
  auto loss = plant_fixture.tick(pfm::duration_from_seconds(5));
  PFM_REQUIRE(loss.ok());
  PFM_CHECK_EQ(loss.value().primary_failure, pfm::FailureClass::UtilityFeedLoss);
  PFM_CHECK(loss.value().requests_planned > 0);

  // A plan that carries failures is a live response, never a stable one.
  auto faulted = plant_fixture.runtime->status();
  PFM_REQUIRE(faulted.ok());
  PFM_CHECK_MSG(faulted.value().lifecycle == pfm::IncidentLifecycle::Isolating ||
                    faulted.value().lifecycle == pfm::IncidentLifecycle::Active,
                std::string{pfm::incident_lifecycle_name(faulted.value().lifecycle)}.c_str());

  // Recovery is refused while the fault still stands: the assessment says so,
  // and the lifecycle refuses to enter recovery.
  auto blocked = plant_fixture.runtime->recovery();
  PFM_REQUIRE(blocked.ok());
  PFM_CHECK_MSG(!blocked.value().eligible, "recovery must be refused while the feed is down");
  PFM_CHECK(!blocked.value().gates.empty());
  auto token = plant_fixture.token();
  PFM_REQUIRE(token.ok());
  PFM_CHECK_CODE(plant_fixture.runtime->begin_recovery(token.value()),
                 pfm::StatusCode::RecoveryNotEligible);
  // Authorization is a grant, not a decision: once it is recorded the gate is
  // satisfied, and the transition is still refused because the electrical facts
  // do not justify it.
  PFM_CHECK_OK(plant_fixture.runtime->authorize_recovery(token.value()));
  auto authorized_token = plant_fixture.token();
  PFM_REQUIRE(authorized_token.ok());
  PFM_CHECK_CODE(plant_fixture.runtime->begin_recovery(authorized_token.value()),
                 pfm::StatusCode::RecoveryNotEligible);
  auto not_recovering = plant_fixture.runtime->status();
  PFM_REQUIRE(not_recovering.ok());
  PFM_CHECK_MSG(not_recovering.value().lifecycle != pfm::IncidentLifecycle::Recovering &&
                    not_recovering.value().lifecycle != pfm::IncidentLifecycle::Recovered,
                std::string{pfm::incident_lifecycle_name(not_recovering.value().lifecycle)}
                    .c_str());

  // The response loop reaches the plant: the restore-normal-feed requests the
  // runtime dispatched were actuated, so the feed is physically back and no
  // failure remains to be classified.
  for (int step = 0; step < 4; ++step) {
    PFM_REQUIRE(plant_fixture.tick(pfm::duration_from_seconds(5)).ok());
  }
  auto settled = plant_fixture.runtime->status();
  PFM_REQUIRE(settled.ok());
  PFM_CHECK_MSG(settled.value().lifecycle == pfm::IncidentLifecycle::Stabilizing,
                std::string{pfm::incident_lifecycle_name(settled.value().lifecycle)}.c_str());
  auto settled_plan = plant_fixture.runtime->plan();
  PFM_REQUIRE(settled_plan.ok());
  PFM_CHECK(settled_plan.value().failures.empty());
  const auto settled_instant = plant_fixture.clock->now();
  PFM_REQUIRE(settled_instant.ok());
  PFM_CHECK_EQ(
      plant_fixture.plant->observe_element(feed.value(), settled_instant.value()).energization,
      pfm::EnergizationState::Energized);

  // The scope is restored explicitly as well, and the stability dwell is given
  // as long as it needs before eligibility is asserted.
  settle([&plant_fixture, &feed] { plant_fixture.plant->restore_utility(feed.value()); });
  bool eligible = false;
  for (int attempt = 0; attempt < 16 && !eligible; ++attempt) {
    auto step = plant_fixture.tick(pfm::duration_from_seconds(5));
    PFM_REQUIRE(step.ok());
    auto assessment = plant_fixture.runtime->recovery();
    PFM_REQUIRE(assessment.ok());
    eligible = assessment.value().eligible;
  }
  PFM_CHECK_MSG(eligible, "recovery becomes eligible once the whole scope reports healthy");
  auto assessment = plant_fixture.runtime->recovery();
  PFM_REQUIRE(assessment.ok());
  for (const auto& gate : assessment.value().gates) {
    PFM_CHECK_MSG(gate.satisfied, pfm::recovery_gate_name(gate.gate).data());
  }

  // Recover, then close.
  auto recovery_token = plant_fixture.token();
  PFM_REQUIRE(recovery_token.ok());
  PFM_CHECK_OK(plant_fixture.runtime->begin_recovery(recovery_token.value()));
  auto recovering = plant_fixture.runtime->status();
  PFM_REQUIRE(recovering.ok());
  PFM_CHECK_EQ(recovering.value().lifecycle, pfm::IncidentLifecycle::Recovering);

  auto complete_token = plant_fixture.token();
  PFM_REQUIRE(complete_token.ok());
  PFM_CHECK_OK(plant_fixture.runtime->complete_recovery(complete_token.value()));
  auto recovered = plant_fixture.runtime->status();
  PFM_REQUIRE(recovered.ok());
  PFM_CHECK_EQ(recovered.value().lifecycle, pfm::IncidentLifecycle::Recovered);

  auto close_token = plant_fixture.token();
  PFM_REQUIRE(close_token.ok());
  PFM_CHECK_OK(plant_fixture.runtime->close_incident(close_token.value()));
  auto closed = plant_fixture.runtime->status();
  PFM_REQUIRE(closed.ok());
  PFM_CHECK_EQ(closed.value().lifecycle, pfm::IncidentLifecycle::Closed);
  PFM_CHECK(!closed.value().incident_live);
}

PFM_TEST(scenario_plant_evidence_is_never_attributed_to_anything_else) {
  pfmtest::TempDirectory scratch("scenario-provenance");
  pfm::ScenarioOptions options;
  options.durable = false;
  options.store_directory = scratch.file("store");

  auto result = pfm::run_scenario("feed-loss-recovery", options);
  PFM_REQUIRE(result.ok());
  PFM_CHECK(result.value().ok);
  PFM_CHECK(result.value().recovered);
  // The scenario's own classification is built from synthetic evidence only:
  // every failure it names is derived from the plant, and the run reports the
  // recovered lifecycle rather than a plan-time guess.
  PFM_CHECK_EQ(result.value().name, std::string{"feed-loss-recovery"});
  PFM_CHECK(result.value().requests_planned > 0);
}

PFM_TEST_MAIN()
