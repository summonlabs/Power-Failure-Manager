// Power Failure Manager -- decision policy and resource bounds.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0 (see LICENSE).
#pragma once

#include <cstddef>
#include <cstdint>

#include "pfm/ids.hpp"
#include "pfm/status.hpp"
#include "pfm/units.hpp"

namespace summon::pfm {

// Thresholds, windows, and floors that bound electrical-failure decisions.
// Every field is exact and validated; a policy that cannot produce a decision
// is refused rather than partially applied.
struct ElectricalPolicy {
  PolicyGeneration generation{};

  // Evidence currency. Evidence older than the freshness window is stale and
  // stops justifying a decision; older than the expiry window it is expired
  // and treated as unknown rather than as a healthy reading.
  Duration evidence_freshness_window{};
  Duration evidence_expiry_window{};
  // How long an external confirmation of a requested effect stays current.
  Duration verification_validity{};
  // Sustained-stability dwell required before recovery eligibility.
  Duration stability_dwell{};
  // Deadlines for acknowledgement and for evidence of the requested effect.
  Duration acknowledgement_window{};
  Duration verification_window{};
  // Bounded windows for generator start and synchronization.
  Duration generator_start_window{};
  Duration synchronization_window{};
  // Hold before a retransfer back to utility is permitted.
  Duration retransfer_hold{};
  // Dwell before a reclose of a faulted circuit may be requested at all.
  Duration reclose_dwell{};

  std::uint32_t max_attempts_per_request{3};

  // UPS reserve floors as thousandths of a percent (100000 == 100%).
  MilliPercent reserve_floor{};
  MilliPercent reserve_critical_floor{};

  // Nominal supply characteristics and the tolerance band around them.
  MilliVolts nominal_voltage{};
  MilliPercent voltage_tolerance{};
  MilliHertz nominal_frequency{};
  MilliPercent frequency_tolerance{};

  // Fail-closed switches. Turning one off is an explicit operator decision and
  // is recorded in the policy generation.
  bool treat_stale_evidence_as_failure{true};
  bool require_shared_domain_resolution_for_recovery{true};
  bool require_operator_authorization_for_recovery{true};
  bool require_verified_isolation_for_recovery{true};
  bool require_reserve_floor_for_recovery{true};

  // Two policies are the same policy when every field agrees. Identity is
  // never inferred from a matching generation alone.
  [[nodiscard]] friend bool operator==(const ElectricalPolicy& a,
                                       const ElectricalPolicy& b) noexcept = default;
};

// Resource bounds. Every declared length and collection is checked against
// these before allocation or iteration.
struct Bounds {
  std::uint32_t max_topology_elements{4096};
  std::uint32_t max_failure_domains{512};
  std::uint32_t max_observations{8192};
  std::uint32_t max_obligations{1024};
  std::uint32_t max_requests_per_plan{512};
  std::uint32_t max_attempts_retained{1024};
  std::uint32_t max_journal_entries{2048};
  std::uint32_t max_transitions{4096};
  std::uint32_t max_reason_codes{64};
  std::uint32_t max_reason_evidence_refs{64};
  std::uint32_t max_scope_elements{4096};
  std::uint32_t max_protected_obligations_per_request{32};
  std::size_t max_provenance_length{128};
  std::size_t max_label_length{128};
  std::size_t max_note_length{256};
};

[[nodiscard]] Result<void> validate(const ElectricalPolicy& policy);
[[nodiscard]] Result<void> validate(const Bounds& bounds);

// Defaults used by the scenario engine, the CLI demo, and the examples. The
// defaults are a documentation artifact as much as a convenience: they show
// the intended order of magnitude of every window.
[[nodiscard]] ElectricalPolicy default_policy() noexcept;

// Half-width of the accepted voltage band, from nominal and tolerance.
[[nodiscard]] Result<MilliVolts> voltage_deviation_limit(const ElectricalPolicy& policy);
[[nodiscard]] Result<MilliHertz> frequency_deviation_limit(const ElectricalPolicy& policy);

// A reading of zero is never treated as "in tolerance": presence is signalled
// separately, and an absent reading is unknown.
[[nodiscard]] Result<bool> voltage_within_tolerance(const ElectricalPolicy& policy,
                                                    MilliVolts reading);
[[nodiscard]] Result<bool> frequency_within_tolerance(const ElectricalPolicy& policy,
                                                      MilliHertz reading);

}  // namespace summon::pfm
