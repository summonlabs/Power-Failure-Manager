// Power Failure Manager -- electrical observations and their currency.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0 (see LICENSE).
#include "pfm/evidence.hpp"

#include <string>

namespace summon::pfm {
namespace {

struct ElementKindEntry {
  ElementKind kind;
  std::string_view name;
  RefKind ref_kind;
  bool supply;
};

constexpr ElementKindEntry kElementKinds[] = {
    {ElementKind::Unknown, "unknown", RefKind::Unknown, false},
    {ElementKind::UtilityFeed, "utility-feed", RefKind::UtilityFeed, true},
    {ElementKind::Switchgear, "switchgear", RefKind::Switchgear, true},
    {ElementKind::Bus, "bus", RefKind::Bus, true},
    {ElementKind::Breaker, "breaker", RefKind::Breaker, true},
    {ElementKind::Circuit, "circuit", RefKind::Circuit, true},
    {ElementKind::Pdu, "pdu", RefKind::Pdu, true},
    {ElementKind::PduBranch, "pdu-branch", RefKind::PduBranch, true},
    {ElementKind::UpsUnit, "ups", RefKind::UpsUnit, true},
    {ElementKind::UpsBus, "ups-bus", RefKind::UpsBus, true},
    {ElementKind::StaticTransferSwitch, "sts", RefKind::StaticTransferSwitch, true},
    {ElementKind::AutomaticTransferSwitch, "ats", RefKind::AutomaticTransferSwitch, true},
    {ElementKind::Generator, "generator", RefKind::Generator, true},
    {ElementKind::LoadGroup, "load-group", RefKind::LoadGroup, false},
    {ElementKind::Rack, "rack", RefKind::Rack, false},
};

Status rejected(std::string message, std::string context) {
  return Status::error(StatusCode::EvidenceRejected, std::move(message))
      .with_context(std::move(context));
}

}  // namespace

std::string_view element_kind_name(ElementKind kind) noexcept {
  for (const auto& entry : kElementKinds) {
    if (entry.kind == kind) {
      return entry.name;
    }
  }
  return "unknown";
}

RefKind ref_kind_for_element(ElementKind kind) noexcept {
  for (const auto& entry : kElementKinds) {
    if (entry.kind == kind) {
      return entry.ref_kind;
    }
  }
  return RefKind::Unknown;
}

bool element_kind_is_supply(ElementKind kind) noexcept {
  for (const auto& entry : kElementKinds) {
    if (entry.kind == kind) {
      return entry.supply;
    }
  }
  return false;
}

std::string_view energization_state_name(EnergizationState value) noexcept {
  switch (value) {
    case EnergizationState::Unknown: return "unknown";
    case EnergizationState::Energized: return "energized";
    case EnergizationState::DeEnergized: return "de-energized";
    case EnergizationState::PartiallyEnergized: return "partially-energized";
  }
  return "unknown";
}

std::string_view breaker_position_name(BreakerPosition value) noexcept {
  switch (value) {
    case BreakerPosition::Unknown: return "unknown";
    case BreakerPosition::Closed: return "closed";
    case BreakerPosition::Open: return "open";
    case BreakerPosition::Tripped: return "tripped";
    case BreakerPosition::RackedOut: return "racked-out";
  }
  return "unknown";
}

std::string_view generator_state_name(GeneratorState value) noexcept {
  switch (value) {
    case GeneratorState::Unknown: return "unknown";
    case GeneratorState::Off: return "off";
    case GeneratorState::Cranking: return "cranking";
    case GeneratorState::Running: return "running";
    case GeneratorState::Synchronized: return "synchronized";
    case GeneratorState::Faulted: return "faulted";
    case GeneratorState::CoolDown: return "cool-down";
  }
  return "unknown";
}

std::string_view transfer_state_name(TransferState value) noexcept {
  switch (value) {
    case TransferState::Unknown: return "unknown";
    case TransferState::OnUtility: return "on-utility";
    case TransferState::OnGenerator: return "on-generator";
    case TransferState::Transferring: return "transferring";
    case TransferState::Failed: return "failed";
    case TransferState::Isolated: return "isolated";
  }
  return "unknown";
}

std::string_view observation_origin_name(ObservationOrigin value) noexcept {
  switch (value) {
    case ObservationOrigin::Unknown: return "unknown";
    case ObservationOrigin::SyntheticPlant: return "synthetic-plant";
    case ObservationOrigin::ExternalMeter: return "external-meter";
    case ObservationOrigin::ExternalController: return "external-controller";
    case ObservationOrigin::OperatorConsole: return "operator-console";
  }
  return "unknown";
}

std::string_view evidence_quality_name(EvidenceQuality value) noexcept {
  switch (value) {
    case EvidenceQuality::Unknown: return "unknown";
    case EvidenceQuality::Good: return "good";
    case EvidenceQuality::Suspect: return "suspect";
    case EvidenceQuality::Bad: return "bad";
  }
  return "unknown";
}

std::string_view freshness_name(Freshness value) noexcept {
  switch (value) {
    case Freshness::Unknown: return "unknown";
    case Freshness::Fresh: return "fresh";
    case Freshness::Stale: return "stale";
    case Freshness::Expired: return "expired";
    case Freshness::Future: return "future";
  }
  return "unknown";
}

bool origin_is_external(ObservationOrigin origin) noexcept {
  // The synthetic plant stands in for an external source in the synthetic
  // configurations, and it is labelled as such wherever evidence is reported.
  return origin == ObservationOrigin::ExternalMeter ||
         origin == ObservationOrigin::ExternalController ||
         origin == ObservationOrigin::OperatorConsole ||
         origin == ObservationOrigin::SyntheticPlant;
}

bool ElectricalObservation::is_empty() const noexcept {
  return energization == EnergizationState::Unknown && breaker == BreakerPosition::Unknown &&
         generator == GeneratorState::Unknown && transfer == TransferState::Unknown &&
         !has_voltage && !has_frequency && !has_reserve && !has_current;
}

Result<void> validate(const ElectricalObservation& observation, const Bounds& bounds) {
  if (!observation.element.is_set()) {
    return rejected("observation has no element reference", "evidence.element");
  }
  if (observation.kind == ElementKind::Unknown) {
    return rejected("observation has no element kind", "evidence.kind");
  }
  if (ref_kind_for_element(observation.kind) != observation.element.kind()) {
    return Status::error(StatusCode::EvidenceUnknownTarget,
                         "element kind does not match the reference kind")
        .with_context(observation.element.to_string());
  }
  if (observation.origin == ObservationOrigin::Unknown) {
    return Status::error(StatusCode::EvidenceOriginUntrusted,
                         "observation has no attributable origin")
        .with_context(observation.element.to_string());
  }
  if (observation.provenance.size() > bounds.max_provenance_length) {
    return Status::error(StatusCode::FieldTooLong, "provenance text exceeds the bound")
        .with_context("evidence.provenance");
  }
  if (observation.observed_at.value() == 0) {
    return rejected("observation has no observation instant", "evidence.observed_at");
  }
  if (observation.has_voltage && observation.voltage.value() < 0) {
    return rejected("voltage reading must not be negative", "evidence.voltage");
  }
  if (observation.has_frequency && observation.frequency.value() < 0) {
    return rejected("frequency reading must not be negative", "evidence.frequency");
  }
  if (observation.has_current && observation.current.value() < 0) {
    return rejected("current reading must not be negative", "evidence.current");
  }
  if (observation.has_reserve && !milli_percent_in_range(observation.reserve)) {
    return rejected("reserve reading must be between 0 and 100000 thousandths of a percent",
                    "evidence.reserve");
  }
  if (observation.is_empty()) {
    return rejected("observation carries no electrical fact", "evidence.empty");
  }
  return {};
}

Freshness classify_freshness(Timestamp observed_at, Timestamp now,
                             const ElectricalPolicy& policy) noexcept {
  if (observed_at.value() == 0) {
    return Freshness::Unknown;
  }
  auto delta = checked_sub_i64(now.value(), observed_at.value());
  if (!delta.ok()) {
    return Freshness::Unknown;
  }
  // Clock skew never creates freshness: an instant ahead of "now" is future.
  if (delta.value() < 0) {
    return Freshness::Future;
  }
  if (delta.value() <= policy.evidence_freshness_window.value()) {
    return Freshness::Fresh;
  }
  if (delta.value() <= policy.evidence_expiry_window.value()) {
    return Freshness::Stale;
  }
  return Freshness::Expired;
}

bool observations_conflict(const ElectricalObservation& a, const ElectricalObservation& b,
                           const ElectricalPolicy& policy) noexcept {
  if (!observations_contradict(a, b)) {
    return false;
  }
  if (a.origin == b.origin) {
    return false;
  }
  return within_window(a.observed_at, b.observed_at, policy.evidence_freshness_window);
}

bool observations_contradict(const ElectricalObservation& a,
                             const ElectricalObservation& b) noexcept {
  if (!(a.element == b.element)) {
    return false;
  }
  // Two reports of the same origin at the same sequence are a duplicate
  // delivery, not a contradiction.
  if (a.origin == b.origin && a.sequence == b.sequence) {
    return false;
  }
  if (a.energization != EnergizationState::Unknown && b.energization != EnergizationState::Unknown &&
      a.energization != b.energization) {
    return true;
  }
  const auto breaker_closed = [](BreakerPosition value) {
    return value == BreakerPosition::Closed;
  };
  if (a.breaker != BreakerPosition::Unknown && b.breaker != BreakerPosition::Unknown &&
      breaker_closed(a.breaker) != breaker_closed(b.breaker)) {
    return true;
  }
  if (a.generator != GeneratorState::Unknown && b.generator != GeneratorState::Unknown &&
      a.generator != b.generator) {
    const auto running = [](GeneratorState value) {
      return value == GeneratorState::Running || value == GeneratorState::Synchronized;
    };
    const auto stopped = [](GeneratorState value) {
      return value == GeneratorState::Off || value == GeneratorState::Faulted;
    };
    if ((running(a.generator) && stopped(b.generator)) ||
        (running(b.generator) && stopped(a.generator))) {
      return true;
    }
  }
  if (a.transfer != TransferState::Unknown && b.transfer != TransferState::Unknown &&
      a.transfer != b.transfer) {
    return true;
  }
  return false;
}

}  // namespace summon::pfm
