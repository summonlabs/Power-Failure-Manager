// Power Failure Manager -- typed electrical observations and their currency.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0 (see LICENSE).
#pragma once

#include <cstdint>
#include <string>

#include "pfm/ids.hpp"
#include "pfm/policy.hpp"
#include "pfm/refs.hpp"
#include "pfm/status.hpp"
#include "pfm/units.hpp"

namespace summon::pfm {

enum class ElementKind : std::uint8_t {
  Unknown = 0,
  UtilityFeed = 1,
  Switchgear = 2,
  Bus = 3,
  Breaker = 4,
  Circuit = 5,
  Pdu = 6,
  PduBranch = 7,
  UpsUnit = 8,
  UpsBus = 9,
  StaticTransferSwitch = 10,
  AutomaticTransferSwitch = 11,
  Generator = 12,
  LoadGroup = 13,
  Rack = 14,
};

[[nodiscard]] std::string_view element_kind_name(ElementKind kind) noexcept;
[[nodiscard]] RefKind ref_kind_for_element(ElementKind kind) noexcept;
[[nodiscard]] bool element_kind_is_supply(ElementKind kind) noexcept;

// Energization is a three-valued observation. Unknown is not "healthy" and is
// never collapsed into either of the other two.
enum class EnergizationState : std::uint8_t {
  Unknown = 0,
  Energized = 1,
  DeEnergized = 2,
  PartiallyEnergized = 3,
};

enum class BreakerPosition : std::uint8_t {
  Unknown = 0,
  Closed = 1,
  Open = 2,
  Tripped = 3,
  RackedOut = 4,
};

enum class GeneratorState : std::uint8_t {
  Unknown = 0,
  Off = 1,
  Cranking = 2,
  Running = 3,
  Synchronized = 4,
  Faulted = 5,
  CoolDown = 6,
};

enum class TransferState : std::uint8_t {
  Unknown = 0,
  OnUtility = 1,
  OnGenerator = 2,
  Transferring = 3,
  Failed = 4,
  Isolated = 5,
};

// Where an observation came from. Only an origin marked external can confirm
// the effect of a requested action: the runtime never treats its own intent,
// or an adjacent controller's acknowledgement, as proof.
enum class ObservationOrigin : std::uint8_t {
  Unknown = 0,
  SyntheticPlant = 1,
  ExternalMeter = 2,
  ExternalController = 3,
  OperatorConsole = 4,
};

enum class EvidenceQuality : std::uint8_t {
  Unknown = 0,
  Good = 1,
  Suspect = 2,
  Bad = 3,
};

enum class Freshness : std::uint8_t {
  Unknown = 0,
  Fresh = 1,
  Stale = 2,
  Expired = 3,
  Future = 4,
};

[[nodiscard]] std::string_view energization_state_name(EnergizationState value) noexcept;
[[nodiscard]] std::string_view breaker_position_name(BreakerPosition value) noexcept;
[[nodiscard]] std::string_view generator_state_name(GeneratorState value) noexcept;
[[nodiscard]] std::string_view transfer_state_name(TransferState value) noexcept;
[[nodiscard]] std::string_view observation_origin_name(ObservationOrigin value) noexcept;
[[nodiscard]] std::string_view evidence_quality_name(EvidenceQuality value) noexcept;
[[nodiscard]] std::string_view freshness_name(Freshness value) noexcept;

[[nodiscard]] bool origin_is_external(ObservationOrigin origin) noexcept;

// One reading of one electrical element at one instant. Absent readings are
// flagged, not zeroed: "missing" and "zero" are different facts.
struct ElectricalObservation {
  RefToken element{};
  ElementKind kind{ElementKind::Unknown};
  ObservationOrigin origin{ObservationOrigin::Unknown};
  EvidenceQuality quality{EvidenceQuality::Unknown};
  ObservationSequence sequence{};
  Timestamp observed_at{};
  Fingerprint evidence_fingerprint{};
  std::string provenance{};

  EnergizationState energization{EnergizationState::Unknown};
  BreakerPosition breaker{BreakerPosition::Unknown};
  GeneratorState generator{GeneratorState::Unknown};
  TransferState transfer{TransferState::Unknown};

  bool has_voltage{false};
  MilliVolts voltage{};
  bool has_frequency{false};
  MilliHertz frequency{};
  bool has_reserve{false};
  MilliPercent reserve{};
  bool has_current{false};
  MilliAmps current{};

  // True when this observation carries no usable electrical fact at all.
  [[nodiscard]] bool is_empty() const noexcept;
};

[[nodiscard]] Result<void> validate(const ElectricalObservation& observation,
                                    const Bounds& bounds);

// Currency classification. A future instant is refused rather than rounded:
// clock skew must not create freshness.
[[nodiscard]] Freshness classify_freshness(Timestamp observed_at, Timestamp now,
                                           const ElectricalPolicy& policy) noexcept;

// True when two observations of the same element assert facts that cannot both
// hold, at the level of individual facts.
[[nodiscard]] bool observations_contradict(const ElectricalObservation& a,
                                           const ElectricalObservation& b) noexcept;

// True when two reports of one element are in genuine conflict: they come from
// different sources, they describe instants inside one freshness window of each
// other, and their facts cannot both hold. Successive reports from the same
// source are an evolution of state and the newer one supersedes the older; that
// is not a contradiction, and treating it as one would make every real state
// change look like ambiguous evidence.
[[nodiscard]] bool observations_conflict(const ElectricalObservation& a,
                                         const ElectricalObservation& b,
                                         const ElectricalPolicy& policy) noexcept;

}  // namespace summon::pfm
