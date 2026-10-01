// Power Failure Manager -- policy validation and supply tolerances.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0 (see LICENSE).
#include "pfm/policy.hpp"

#include <string>

namespace summon::pfm {
namespace {

Status invalid(std::string message, std::string context) {
  return Status::error(StatusCode::InvalidArgument, std::move(message))
      .with_context(std::move(context));
}

Result<void> require_non_negative(Duration value, const char* field) {
  if (value.value() < 0) {
    return invalid("duration must not be negative", field);
  }
  return {};
}

Result<void> require_percent(MilliPercent value, const char* field) {
  if (!milli_percent_in_range(value)) {
    return invalid("percentage must be between 0 and 100000 thousandths of a percent", field);
  }
  return {};
}

}  // namespace

ElectricalPolicy default_policy() noexcept {
  ElectricalPolicy policy{};
  policy.evidence_freshness_window = duration_from_seconds(15);
  policy.evidence_expiry_window = duration_from_seconds(60);
  policy.verification_validity = duration_from_seconds(30);
  policy.stability_dwell = duration_from_seconds(20);
  policy.acknowledgement_window = duration_from_seconds(10);
  policy.verification_window = duration_from_seconds(45);
  policy.generator_start_window = duration_from_seconds(12);
  policy.synchronization_window = duration_from_seconds(20);
  policy.retransfer_hold = duration_from_seconds(60);
  policy.reclose_dwell = duration_from_seconds(30);
  policy.max_attempts_per_request = 3;
  policy.reserve_floor = milli_percent_from_value(50000);
  policy.reserve_critical_floor = milli_percent_from_value(20000);
  policy.nominal_voltage = MilliVolts::from_value(230000);
  policy.voltage_tolerance = milli_percent_from_value(10000);
  policy.nominal_frequency = MilliHertz::from_value(50000);
  policy.frequency_tolerance = milli_percent_from_value(1000);
  policy.treat_stale_evidence_as_failure = true;
  policy.require_shared_domain_resolution_for_recovery = true;
  policy.require_operator_authorization_for_recovery = true;
  policy.require_verified_isolation_for_recovery = true;
  policy.require_reserve_floor_for_recovery = true;
  return policy;
}

Result<void> validate(const ElectricalPolicy& policy) {
  if (auto r = require_non_negative(policy.evidence_freshness_window, "policy.freshness");
      !r.ok()) {
    return r;
  }
  if (auto r = require_non_negative(policy.evidence_expiry_window, "policy.expiry"); !r.ok()) {
    return r;
  }
  if (policy.evidence_freshness_window > policy.evidence_expiry_window) {
    return invalid("freshness window must not exceed the expiry window", "policy.freshness");
  }
  for (const auto* pair :
       {&policy.acknowledgement_window, &policy.verification_window, &policy.stability_dwell,
        &policy.generator_start_window, &policy.synchronization_window,
        &policy.retransfer_hold, &policy.reclose_dwell, &policy.verification_validity}) {
    if (pair->value() < 0) {
      return invalid("duration must not be negative", "policy.window");
    }
  }
  if (policy.max_attempts_per_request == 0 || policy.max_attempts_per_request > 16) {
    return invalid("attempt bound must be between 1 and 16", "policy.max_attempts");
  }
  if (auto r = require_percent(policy.reserve_floor, "policy.reserve_floor"); !r.ok()) {
    return r;
  }
  if (auto r = require_percent(policy.reserve_critical_floor, "policy.reserve_critical_floor");
      !r.ok()) {
    return r;
  }
  if (policy.reserve_critical_floor > policy.reserve_floor) {
    return invalid("critical reserve floor must not exceed the reserve floor",
                   "policy.reserve_critical_floor");
  }
  if (policy.nominal_voltage.value() <= 0) {
    return invalid("nominal voltage must be positive", "policy.nominal_voltage");
  }
  if (policy.nominal_frequency.value() <= 0) {
    return invalid("nominal frequency must be positive", "policy.nominal_frequency");
  }
  if (!milli_percent_in_range(policy.voltage_tolerance) || policy.voltage_tolerance.value() == 0 ||
      policy.voltage_tolerance.value() >= 50000) {
    return invalid("voltage tolerance must be between 0 and 50 percent", "policy.voltage_tolerance");
  }
  if (!milli_percent_in_range(policy.frequency_tolerance) ||
      policy.frequency_tolerance.value() == 0 ||
      policy.frequency_tolerance.value() >= 50000) {
    return invalid("frequency tolerance must be between 0 and 50 percent",
                   "policy.frequency_tolerance");
  }
  return {};
}

Result<void> validate(const Bounds& bounds) {
  const auto check = [](std::uint32_t value, std::uint32_t low, std::uint32_t high,
                        const char* field) -> Result<void> {
    if (value < low || value > high) {
      return invalid("bound is outside the accepted range", field);
    }
    return {};
  };
  if (auto r = check(bounds.max_topology_elements, 1, 1u << 20, "bounds.max_topology_elements");
      !r.ok()) {
    return r;
  }
  if (auto r = check(bounds.max_failure_domains, 1, 1u << 20, "bounds.max_failure_domains");
      !r.ok()) {
    return r;
  }
  if (auto r = check(bounds.max_observations, 1, 1u << 22, "bounds.max_observations"); !r.ok()) {
    return r;
  }
  if (auto r = check(bounds.max_obligations, 1, 1u << 20, "bounds.max_obligations"); !r.ok()) {
    return r;
  }
  if (auto r = check(bounds.max_requests_per_plan, 1, 1u << 16, "bounds.max_requests_per_plan");
      !r.ok()) {
    return r;
  }
  if (auto r = check(bounds.max_attempts_retained, 1, 1u << 20, "bounds.max_attempts_retained");
      !r.ok()) {
    return r;
  }
  if (auto r = check(bounds.max_journal_entries, 1, 1u << 22, "bounds.max_journal_entries");
      !r.ok()) {
    return r;
  }
  if (auto r = check(bounds.max_transitions, 1, 1u << 22, "bounds.max_transitions"); !r.ok()) {
    return r;
  }
  if (auto r = check(bounds.max_reason_codes, 1, 1u << 16, "bounds.max_reason_codes"); !r.ok()) {
    return r;
  }
  if (auto r = check(bounds.max_reason_evidence_refs, 1, 1u << 16,
                     "bounds.max_reason_evidence_refs");
      !r.ok()) {
    return r;
  }
  if (auto r = check(bounds.max_scope_elements, 1, 1u << 20, "bounds.max_scope_elements");
      !r.ok()) {
    return r;
  }
  if (auto r = check(bounds.max_protected_obligations_per_request, 1, 1u << 16,
                     "bounds.max_protected_obligations_per_request");
      !r.ok()) {
    return r;
  }
  if (bounds.max_provenance_length < 8 || bounds.max_provenance_length > 4096) {
    return invalid("provenance bound is outside the accepted range", "bounds.max_provenance_length");
  }
  if (bounds.max_label_length < 8 || bounds.max_label_length > 4096) {
    return invalid("label bound is outside the accepted range", "bounds.max_label_length");
  }
  if (bounds.max_note_length < 8 || bounds.max_note_length > 4096) {
    return invalid("note bound is outside the accepted range", "bounds.max_note_length");
  }
  return {};
}

Result<MilliVolts> voltage_deviation_limit(const ElectricalPolicy& policy) {
  if (auto r = validate(policy); !r.ok()) {
    return r.status();
  }
  auto scaled = checked_mul_i64(policy.nominal_voltage.value(), policy.voltage_tolerance.value());
  if (!scaled.ok()) {
    return scaled.status();
  }
  return MilliVolts::from_value(scaled.value() / kMilliPercentFull);
}

Result<MilliHertz> frequency_deviation_limit(const ElectricalPolicy& policy) {
  if (auto r = validate(policy); !r.ok()) {
    return r.status();
  }
  auto scaled = checked_mul_i64(policy.nominal_frequency.value(), policy.frequency_tolerance.value());
  if (!scaled.ok()) {
    return scaled.status();
  }
  return MilliHertz::from_value(scaled.value() / kMilliPercentFull);
}

Result<bool> voltage_within_tolerance(const ElectricalPolicy& policy, MilliVolts reading) {
  if (reading.value() <= 0) {
    return false;
  }
  auto limit = voltage_deviation_limit(policy);
  if (!limit.ok()) {
    return limit.status();
  }
  auto difference = checked_sub_i64(reading.value(), policy.nominal_voltage.value());
  if (!difference.ok()) {
    return difference.status();
  }
  const std::int64_t magnitude = difference.value() < 0 ? -difference.value() : difference.value();
  return magnitude <= limit.value().value();
}

Result<bool> frequency_within_tolerance(const ElectricalPolicy& policy, MilliHertz reading) {
  if (reading.value() <= 0) {
    return false;
  }
  auto limit = frequency_deviation_limit(policy);
  if (!limit.ok()) {
    return limit.status();
  }
  auto difference = checked_sub_i64(reading.value(), policy.nominal_frequency.value());
  if (!difference.ok()) {
    return difference.status();
  }
  const std::int64_t magnitude = difference.value() < 0 ? -difference.value() : difference.value();
  return magnitude <= limit.value().value();
}

}  // namespace summon::pfm
