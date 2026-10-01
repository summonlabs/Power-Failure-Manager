// Power Failure Manager -- protected obligations.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0 (see LICENSE).
#include "pfm/obligations.hpp"

#include <string>

#include "pfm/evidence.hpp"

namespace summon::pfm {

std::string_view protection_class_name(ProtectionClass value) noexcept {
  switch (value) {
    case ProtectionClass::HardSafetyInterlock: return "hard-safety-interlock";
    case ProtectionClass::Regulatory: return "regulatory";
    case ProtectionClass::ServiceLevel: return "service-level";
    case ProtectionClass::AdvisoryOptimization: return "advisory-optimization";
  }
  return "unknown";
}

std::string_view obligation_status_name(ObligationStatus value) noexcept {
  switch (value) {
    case ObligationStatus::Unreported: return "unreported";
    case ObligationStatus::Satisfied: return "satisfied";
    case ObligationStatus::AtRisk: return "at-risk";
    case ObligationStatus::Violated: return "violated";
    case ObligationStatus::Unknown: return "unknown";
  }
  return "unknown";
}

Result<void> validate(const ProtectedObligation& obligation, const Bounds& bounds) {
  if (obligation.id.is_absent()) {
    return Status::error(StatusCode::InvalidArgument, "obligation has no identity")
        .with_context("obligation.id");
  }
  if (!obligation.target.is_set()) {
    return Status::error(StatusCode::InvalidArgument, "obligation has no protected target")
        .with_context("obligation.target");
  }
  if (obligation.target.kind() != RefKind::LoadGroup && obligation.target.kind() != RefKind::Rack &&
      !ref_kind_is_element(obligation.target.kind())) {
    return Status::error(StatusCode::InvalidArgument,
                         "obligation target is not a load group, rack, or electrical element")
        .with_context(obligation.target.to_string());
  }
  if (!obligation.reporting_authority.is_set()) {
    return Status::error(StatusCode::InvalidArgument, "obligation has no reporting authority")
        .with_context(obligation.target.to_string());
  }
  if (obligation.has_reserve_floor && !milli_percent_in_range(obligation.reserve_floor)) {
    return Status::error(StatusCode::ValueOutOfRange,
                         "obligation reserve floor is not a valid percentage")
        .with_context(obligation.target.to_string());
  }
  if (obligation.has_max_outage && obligation.max_outage.value() < 0) {
    return Status::error(StatusCode::ValueOutOfRange, "obligation max outage must not be negative")
        .with_context(obligation.target.to_string());
  }
  if (obligation.has_report && obligation.reported_at.value() == 0) {
    return Status::error(StatusCode::InvalidArgument,
                         "obligation has a report flag but no report instant")
        .with_context(obligation.target.to_string());
  }
  static_cast<void>(bounds);
  return {};
}

bool obligation_blocks_recovery(const ProtectedObligation& obligation, Timestamp now,
                                const ElectricalPolicy& policy) {
  if (!obligation.has_report) {
    return true;
  }
  // Only a current report counts. A report that has gone stale, expired, or
  // that is dated in the future is not a report: remembered status never
  // silently becomes current, and the owning authority has to reaffirm it.
  if (classify_freshness(obligation.reported_at, now, policy) != Freshness::Fresh) {
    return true;
  }
  switch (obligation.status) {
    case ObligationStatus::Satisfied:
      return false;
    case ObligationStatus::AtRisk:
    case ObligationStatus::Violated:
    case ObligationStatus::Unknown:
    case ObligationStatus::Unreported:
      return true;
  }
  return true;
}

bool obligation_relaxable(ProtectionClass protection) noexcept {
  // A hard safety interlock and a regulatory obligation are never relaxed by
  // electrical-failure response authority, whatever the pressure to recover.
  return protection == ProtectionClass::ServiceLevel ||
         protection == ProtectionClass::AdvisoryOptimization;
}

}  // namespace summon::pfm
