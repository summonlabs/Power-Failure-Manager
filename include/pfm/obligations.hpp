// Power Failure Manager -- protected facility obligations.
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

// How strongly an obligation constrains a response decision.
enum class ProtectionClass : std::uint8_t {
  HardSafetyInterlock = 0,
  Regulatory = 1,
  ServiceLevel = 2,
  AdvisoryOptimization = 3,
};

[[nodiscard]] std::string_view protection_class_name(ProtectionClass value) noexcept;

// The status of an obligation as last reported by the authority that owns it.
// Unreported and Unknown are distinct from Satisfied and from each other.
enum class ObligationStatus : std::uint8_t {
  Unreported = 0,
  Satisfied = 1,
  AtRisk = 2,
  Violated = 3,
  Unknown = 4,
};

[[nodiscard]] std::string_view obligation_status_name(ObligationStatus value) noexcept;

// A protected obligation that constrains electrical-failure response. PFM owns
// the *reference* to the obligation and the decision rules that use it; the
// obligation's meaning and its status are owned by its authority.
struct ProtectedObligation {
  ObligationId id{};
  RefToken target{};
  ProtectionClass protection{ProtectionClass::ServiceLevel};
  ObligationStatus status{ObligationStatus::Unreported};
  // Reporting currency. An obligation whose report is not current does not
  // count as satisfied.
  bool has_report{false};
  Timestamp reported_at{};
  RefToken reporting_authority{};
  // Minimum reserve that must be held for the protected target, when stated.
  bool has_reserve_floor{false};
  MilliPercent reserve_floor{};
  // Maximum tolerated outage, when stated.
  bool has_max_outage{false};
  Duration max_outage{};
  PolicyGeneration policy_generation{};
};

[[nodiscard]] Result<void> validate(const ProtectedObligation& obligation, const Bounds& bounds);

// True when this obligation must block recovery at this instant. Unknown,
// unreported, at-risk, or violated all block; a report that is no longer
// current blocks as well.
[[nodiscard]] bool obligation_blocks_recovery(const ProtectedObligation& obligation,
                                              Timestamp now, const ElectricalPolicy& policy);

// True when the protection class may be relaxed by response authority at all.
[[nodiscard]] bool obligation_relaxable(ProtectionClass protection) noexcept;

}  // namespace summon::pfm
