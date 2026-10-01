// Power Failure Manager -- status codes.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0 (see LICENSE).
#include "pfm/status.hpp"

#include <array>
#include <cstdio>
#include <cstdlib>
#include <string>

namespace summon::pfm {
namespace {

struct CodeEntry {
  StatusCode code;
  std::string_view name;
};

// The name table is the machine contract: the string is stable for a given
// code, and every assigned code appears exactly once.
constexpr CodeEntry kCodes[] = {
    {StatusCode::Ok, "pfm.ok"},

    {StatusCode::InvalidArgument, "pfm.invalid_argument"},
    {StatusCode::EmptyField, "pfm.empty_field"},
    {StatusCode::FieldTooLong, "pfm.field_too_long"},
    {StatusCode::InvalidCharacter, "pfm.invalid_character"},
    {StatusCode::ValueOutOfRange, "pfm.value_out_of_range"},
    {StatusCode::DuplicateElement, "pfm.duplicate_element"},
    {StatusCode::TooManyElements, "pfm.too_many_elements"},
    {StatusCode::MalformedEncoding, "pfm.malformed_encoding"},
    {StatusCode::ReservedFieldNotZero, "pfm.reserved_field_not_zero"},
    {StatusCode::UnknownEnumValue, "pfm.unknown_enum_value"},

    {StatusCode::MissingAuthority, "pfm.missing_authority"},
    {StatusCode::StaleIncarnation, "pfm.stale_incarnation"},
    {StatusCode::StaleEpoch, "pfm.stale_epoch"},
    {StatusCode::FutureEpoch, "pfm.future_epoch"},
    {StatusCode::StaleRevision, "pfm.stale_revision"},
    {StatusCode::FutureRevision, "pfm.future_revision"},
    {StatusCode::StaleGeneration, "pfm.stale_generation"},
    {StatusCode::FutureGeneration, "pfm.future_generation"},
    {StatusCode::CrossIncidentAuthority, "pfm.cross_incident_authority"},
    {StatusCode::AuthorityFenced, "pfm.authority_fenced"},
    {StatusCode::AuthorityRequired, "pfm.authority_required"},

    {StatusCode::PreconditionFailed, "pfm.precondition_failed"},
    {StatusCode::IllegalTransition, "pfm.illegal_transition"},
    {StatusCode::NoActiveIncident, "pfm.no_active_incident"},
    {StatusCode::IncidentNotFound, "pfm.incident_not_found"},
    {StatusCode::IncidentClosed, "pfm.incident_closed"},
    {StatusCode::RecoveryNotEligible, "pfm.recovery_not_eligible"},
    {StatusCode::GateNotSatisfied, "pfm.gate_not_satisfied"},
    {StatusCode::EvidenceNotCurrent, "pfm.evidence_not_current"},
    {StatusCode::IncidentAlreadyOpen, "pfm.incident_already_open"},
    {StatusCode::PlanNotCurrent, "pfm.plan_not_current"},

    {StatusCode::EvidenceRejected, "pfm.evidence_rejected"},
    {StatusCode::EvidenceStale, "pfm.evidence_stale"},
    {StatusCode::EvidenceFuture, "pfm.evidence_future"},
    {StatusCode::EvidenceOutOfOrder, "pfm.evidence_out_of_order"},
    {StatusCode::EvidenceContradictory, "pfm.evidence_contradictory"},
    {StatusCode::EvidenceUnavailable, "pfm.evidence_unavailable"},
    {StatusCode::EvidenceUnknownTarget, "pfm.evidence_unknown_target"},
    {StatusCode::EvidenceDuplicate, "pfm.evidence_duplicate"},
    {StatusCode::EvidenceOriginUntrusted, "pfm.evidence_origin_untrusted"},

    {StatusCode::TopologyUnknownElement, "pfm.topology_unknown_element"},
    {StatusCode::TopologyCycle, "pfm.topology_cycle"},
    {StatusCode::TopologyAmbiguousUpstream, "pfm.topology_ambiguous_upstream"},
    {StatusCode::TopologyBoundsExceeded, "pfm.topology_bounds_exceeded"},
    {StatusCode::ScopeUnresolved, "pfm.scope_unresolved"},
    {StatusCode::FailureDomainUnknown, "pfm.failure_domain_unknown"},
    {StatusCode::SharedDomainUnresolved, "pfm.shared_domain_unresolved"},

    {StatusCode::ProtectedObligationViolation, "pfm.protected_obligation_violation"},
    {StatusCode::ObligationNotRelaxable, "pfm.obligation_not_relaxable"},
    {StatusCode::ObligationUnknown, "pfm.obligation_unknown"},
    {StatusCode::ObligationViolated, "pfm.obligation_violated"},
    {StatusCode::ObligationAlreadyRelaxed, "pfm.obligation_already_relaxed"},
    {StatusCode::ObligationNotRelaxed, "pfm.obligation_not_relaxed"},

    {StatusCode::RequestNotFound, "pfm.request_not_found"},
    {StatusCode::RequestStateConflict, "pfm.request_state_conflict"},
    {StatusCode::RequestExpired, "pfm.request_expired"},
    {StatusCode::IdempotencyConflict, "pfm.idempotency_conflict"},
    {StatusCode::EffectUnverified, "pfm.effect_unverified"},
    {StatusCode::RequestTargetMismatch, "pfm.request_target_mismatch"},
    {StatusCode::RequestFailed, "pfm.request_failed"},
    {StatusCode::TransportFailure, "pfm.transport_failure"},
    {StatusCode::DispatchIndeterminate, "pfm.dispatch_indeterminate"},
    {StatusCode::AttemptExhausted, "pfm.attempt_exhausted"},

    {StatusCode::ResourceExhausted, "pfm.resource_exhausted"},
    {StatusCode::BoundsExceeded, "pfm.bounds_exceeded"},
    {StatusCode::CheckpointUnavailable, "pfm.checkpoint_unavailable"},
    {StatusCode::ArithmeticOverflow, "pfm.arithmetic_overflow"},

    {StatusCode::StoreNotFound, "pfm.store_not_found"},
    {StatusCode::StoreCorrupt, "pfm.store_corrupt"},
    {StatusCode::StoreVersionUnsupported, "pfm.store_version_unsupported"},
    {StatusCode::StoreTruncated, "pfm.store_truncated"},
    {StatusCode::StoreTrailingBytes, "pfm.store_trailing_bytes"},
    {StatusCode::StoreIntegrityFailure, "pfm.store_integrity_failure"},
    {StatusCode::StoreLocked, "pfm.store_locked"},
    {StatusCode::StorePathInvalid, "pfm.store_path_invalid"},
    {StatusCode::StoreIoError, "pfm.store_io_error"},
    {StatusCode::StoreReadbackMismatch, "pfm.store_readback_mismatch"},
    {StatusCode::NoAuthoritativeGeneration, "pfm.no_authoritative_generation"},
    {StatusCode::ReplayDivergence, "pfm.replay_divergence"},
    {StatusCode::StoreAlreadyExists, "pfm.store_already_exists"},

    {StatusCode::RuntimeClosed, "pfm.runtime_closed"},
    {StatusCode::ShutdownInProgress, "pfm.shutdown_in_progress"},
    {StatusCode::ReentrancyRefused, "pfm.reentrancy_refused"},
    {StatusCode::NotSupported, "pfm.not_supported"},
    {StatusCode::Internal, "pfm.internal"},
    {StatusCode::ConcurrencyConflict, "pfm.concurrency_conflict"},
};

}  // namespace

std::string_view status_code_name(StatusCode code) noexcept {
  for (const auto& entry : kCodes) {
    if (entry.code == code) {
      return entry.name;
    }
  }
  return "pfm.unassigned";
}

std::optional<StatusCode> status_code_from_value(std::uint16_t value) noexcept {
  for (const auto& entry : kCodes) {
    if (static_cast<std::uint16_t>(entry.code) == value) {
      return entry.code;
    }
  }
  return std::nullopt;
}

std::string Status::to_string() const {
  std::string text{status_code_name(code_)};
  if (!message_.empty()) {
    text.append(": ");
    text.append(message_);
  }
  if (!context_.empty()) {
    text.append(" [");
    text.append(context_);
    text.append("]");
  }
  return text;
}

void trap(const char* expression, const char* file, int line) {
  std::fprintf(stderr, "pfm trap: %s at %s:%d\n", expression, file, line);
  std::fflush(stderr);
  std::abort();
}

}  // namespace summon::pfm
