// Power Failure Manager -- canonical encoding of every domain value.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0 (see LICENSE).
#include <cstdint>
#include <string>
#include <vector>

#include "pfm/codec.hpp"
#include "pfm/state.hpp"

namespace summon::pfm::codec {
namespace {

template <class E>
void put_enum(Writer& writer, E value) {
  writer.u8(static_cast<std::uint8_t>(value));
}

template <class E, class Validator>
void get_enum(Reader& reader, E& value, Validator&& valid, const char* what) {
  const auto raw = reader.u8();
  if (!reader.ok()) {
    return;
  }
  const auto candidate = static_cast<E>(raw);
  if (!valid(candidate)) {
    reader.fail(Status::error(StatusCode::UnknownEnumValue, std::string{"unknown "} + what)
                    .with_context("codec.enum"));
    return;
  }
  value = candidate;
}

// A declared collection length is checked against its structural bound before
// anything is reserved for it.
bool begin_list(Reader& reader, std::uint32_t bound, std::uint32_t& count, const char* what) {
  count = reader.u32();
  if (!reader.ok()) {
    return false;
  }
  if (count > bound) {
    reader.fail(Status::error(StatusCode::BoundsExceeded,
                              std::string{"declared "} + what + " count exceeds the bound")
                    .with_context("codec.list"));
    return false;
  }
  return true;
}

void put_ref(Writer& writer, const RefToken& value) {
  put_enum(writer, value.kind());
  writer.text(value.name(), kMaxRefNameLength);
}

void get_ref(Reader& reader, RefToken& value) {
  RefKind kind = RefKind::Unknown;
  get_enum(reader, kind,
           [](RefKind candidate) {
             // An unset reference is encoded as kind zero with an empty name;
             // every other value must name a known kind.
             return candidate == RefKind::Unknown || ref_kind_name(candidate) != "unknown";
           },
           "reference kind");
  std::string name;
  reader.text(name, kMaxRefNameLength);
  if (!reader.ok()) {
    return;
  }
  if (kind == RefKind::Unknown) {
    if (!name.empty()) {
      reader.fail(Status::error(StatusCode::MalformedEncoding,
                                "an unset reference must carry an empty name")
                      .with_context("codec.ref"));
      return;
    }
    value = RefToken{};
    return;
  }
  auto token = RefToken::make(kind, name);
  if (!token.ok()) {
    reader.fail(token.status());
    return;
  }
  value = token.value();
}

void put_fingerprints(Writer& writer, const std::vector<Fingerprint>& values,
                      std::uint32_t bound) {
  const auto size = static_cast<std::uint32_t>(values.size() > bound ? bound : values.size());
  writer.u32(size);
  for (std::uint32_t i = 0; i < size; ++i) {
    encode(writer, values[i]);
  }
}

void get_fingerprints(Reader& reader, std::vector<Fingerprint>& values, std::uint32_t bound) {
  std::uint32_t count = 0;
  if (!begin_list(reader, bound, count, "fingerprint")) {
    return;
  }
  values.resize(count);
  for (std::uint32_t i = 0; i < count; ++i) {
    decode(reader, values[i]);
  }
}

void put_refs(Writer& writer, const std::vector<RefToken>& values, std::uint32_t bound) {
  const auto size = static_cast<std::uint32_t>(values.size() > bound ? bound : values.size());
  writer.u32(size);
  for (std::uint32_t i = 0; i < size; ++i) {
    put_ref(writer, values[i]);
  }
}

void get_refs(Reader& reader, std::vector<RefToken>& values, std::uint32_t bound) {
  std::uint32_t count = 0;
  if (!begin_list(reader, bound, count, "reference")) {
    return;
  }
  values.resize(count);
  for (std::uint32_t i = 0; i < count; ++i) {
    get_ref(reader, values[i]);
  }
}

}  // namespace

// --- primitive quantities -------------------------------------------------

void encode(Writer& writer, Timestamp value) { writer.i64(value.value()); }
void decode(Reader& reader, Timestamp& value) { value = Timestamp::from_value(reader.i64()); }
void encode(Writer& writer, Duration value) { writer.i64(value.value()); }
void decode(Reader& reader, Duration& value) { value = Duration::from_value(reader.i64()); }
void encode(Writer& writer, BasisPoints value) { writer.i64(value.value()); }
void decode(Reader& reader, BasisPoints& value) { value = BasisPoints::from_value(reader.i64()); }
void encode(Writer& writer, MilliVolts value) { writer.i64(value.value()); }
void decode(Reader& reader, MilliVolts& value) { value = MilliVolts::from_value(reader.i64()); }
void encode(Writer& writer, MilliHertz value) { writer.i64(value.value()); }
void decode(Reader& reader, MilliHertz& value) { value = MilliHertz::from_value(reader.i64()); }
void encode(Writer& writer, MilliAmps value) { writer.i64(value.value()); }
void decode(Reader& reader, MilliAmps& value) { value = MilliAmps::from_value(reader.i64()); }
void encode(Writer& writer, MilliPercent value) { writer.i64(value.value()); }
void decode(Reader& reader, MilliPercent& value) {
  value = MilliPercent::from_value(reader.i64());
}

void encode(Writer& writer, Fingerprint value) {
  writer.u64(value.high());
  writer.u64(value.low());
}
void decode(Reader& reader, Fingerprint& value) {
  const auto high = reader.u64();
  const auto low = reader.u64();
  value = Fingerprint{high, low};
}
void encode(Writer& writer, IdempotencyKey value) {
  writer.u64(value.high());
  writer.u64(value.low());
}
void decode(Reader& reader, IdempotencyKey& value) {
  const auto high = reader.u64();
  const auto low = reader.u64();
  value = IdempotencyKey{high, low};
}
void encode(Writer& writer, const RefToken& value) { put_ref(writer, value); }
void decode(Reader& reader, RefToken& value) { get_ref(reader, value); }

// --- enumerations ---------------------------------------------------------

void encode(Writer& writer, FailureClass value) { put_enum(writer, value); }
void decode(Reader& reader, FailureClass& value) {
  get_enum(reader, value,
           [](FailureClass candidate) {
             return failure_class_name(candidate) != "unknown" ||
                    candidate == FailureClass::None;
           },
           "failure class");
}
void encode(Writer& writer, ReasonCode value) { put_enum(writer, value); }
void decode(Reader& reader, ReasonCode& value) {
  get_enum(reader, value,
           [](ReasonCode candidate) { return reason_code_name(candidate) != "unknown"; },
           "reason code");
}
void encode(Writer& writer, ElementKind value) { put_enum(writer, value); }
void decode(Reader& reader, ElementKind& value) {
  get_enum(reader, value,
           [](ElementKind candidate) { return candidate == ElementKind::Unknown || element_kind_name(candidate) != "unknown"; },
           "element kind");
}
void encode(Writer& writer, EnergizationState value) { put_enum(writer, value); }
void decode(Reader& reader, EnergizationState& value) {
  get_enum(reader, value,
           [](EnergizationState candidate) {
             return candidate == EnergizationState::Unknown || energization_state_name(candidate) != "unknown";
           },
           "energization state");
}
void encode(Writer& writer, BreakerPosition value) { put_enum(writer, value); }
void decode(Reader& reader, BreakerPosition& value) {
  get_enum(reader, value,
           [](BreakerPosition candidate) { return candidate == BreakerPosition::Unknown || breaker_position_name(candidate) != "unknown"; },
           "breaker position");
}
void encode(Writer& writer, GeneratorState value) { put_enum(writer, value); }
void decode(Reader& reader, GeneratorState& value) {
  get_enum(reader, value,
           [](GeneratorState candidate) { return candidate == GeneratorState::Unknown || generator_state_name(candidate) != "unknown"; },
           "generator state");
}
void encode(Writer& writer, TransferState value) { put_enum(writer, value); }
void decode(Reader& reader, TransferState& value) {
  get_enum(reader, value,
           [](TransferState candidate) { return candidate == TransferState::Unknown || transfer_state_name(candidate) != "unknown"; },
           "transfer state");
}
void encode(Writer& writer, ObservationOrigin value) { put_enum(writer, value); }
void decode(Reader& reader, ObservationOrigin& value) {
  get_enum(reader, value,
           [](ObservationOrigin candidate) {
             return candidate == ObservationOrigin::Unknown || observation_origin_name(candidate) != "unknown";
           },
           "observation origin");
}
void encode(Writer& writer, EvidenceQuality value) { put_enum(writer, value); }
void decode(Reader& reader, EvidenceQuality& value) {
  get_enum(reader, value,
           [](EvidenceQuality candidate) { return candidate == EvidenceQuality::Unknown || evidence_quality_name(candidate) != "unknown"; },
           "evidence quality");
}
void encode(Writer& writer, Freshness value) { put_enum(writer, value); }
void decode(Reader& reader, Freshness& value) {
  get_enum(reader, value,
           [](Freshness candidate) { return candidate == Freshness::Unknown || freshness_name(candidate) != "unknown"; }, "freshness");
}
void encode(Writer& writer, ProtectionClass value) { put_enum(writer, value); }
void decode(Reader& reader, ProtectionClass& value) {
  get_enum(reader, value,
           [](ProtectionClass candidate) {
             return protection_class_name(candidate) != "unknown";
           },
           "protection class");
}
void encode(Writer& writer, ObligationStatus value) { put_enum(writer, value); }
void decode(Reader& reader, ObligationStatus& value) {
  get_enum(reader, value,
           [](ObligationStatus candidate) {
             return candidate == ObligationStatus::Unknown || obligation_status_name(candidate) != "unknown";
           },
           "obligation status");
}
void encode(Writer& writer, RequestKind value) { put_enum(writer, value); }
void decode(Reader& reader, RequestKind& value) {
  get_enum(reader, value,
           [](RequestKind candidate) { return request_kind_name(candidate) != "unknown"; },
           "request kind");
}
void encode(Writer& writer, RequestOwner value) { put_enum(writer, value); }
void decode(Reader& reader, RequestOwner& value) {
  get_enum(reader, value,
           [](RequestOwner candidate) { return candidate == RequestOwner::Unknown || request_owner_name(candidate) != "unknown"; },
           "request owner");
}
void encode(Writer& writer, ProofKind value) { put_enum(writer, value); }
void decode(Reader& reader, ProofKind& value) {
  get_enum(reader, value,
           [](ProofKind candidate) { return proof_kind_name(candidate) != "unknown"; },
           "proof kind");
}
void encode(Writer& writer, RequestState value) { put_enum(writer, value); }
void decode(Reader& reader, RequestState& value) {
  get_enum(reader, value,
           [](RequestState candidate) { return request_state_name(candidate) != "unknown"; },
           "request state");
}
void encode(Writer& writer, IncidentLifecycle value) { put_enum(writer, value); }
void decode(Reader& reader, IncidentLifecycle& value) {
  get_enum(reader, value,
           [](IncidentLifecycle candidate) {
             return incident_lifecycle_name(candidate) != "unknown";
           },
           "incident lifecycle");
}
void encode(Writer& writer, RecoveryGate value) { put_enum(writer, value); }
void decode(Reader& reader, RecoveryGate& value) {
  get_enum(reader, value,
           [](RecoveryGate candidate) { return recovery_gate_name(candidate) != "unknown"; },
           "recovery gate");
}
void encode(Writer& writer, JournalKind value) { put_enum(writer, value); }
void decode(Reader& reader, JournalKind& value) {
  get_enum(reader, value,
           [](JournalKind candidate) { return journal_kind_name(candidate) != "unknown"; },
           "journal kind");
}

// --- policy and bounds ----------------------------------------------------

void encode(Writer& writer, const Bounds& value) {
  writer.u32(value.max_topology_elements);
  writer.u32(value.max_failure_domains);
  writer.u32(value.max_observations);
  writer.u32(value.max_obligations);
  writer.u32(value.max_requests_per_plan);
  writer.u32(value.max_attempts_retained);
  writer.u32(value.max_journal_entries);
  writer.u32(value.max_transitions);
  writer.u32(value.max_reason_codes);
  writer.u32(value.max_reason_evidence_refs);
  writer.u32(value.max_scope_elements);
  writer.u32(value.max_protected_obligations_per_request);
  writer.u32(static_cast<std::uint32_t>(value.max_provenance_length));
  writer.u32(static_cast<std::uint32_t>(value.max_label_length));
  writer.u32(static_cast<std::uint32_t>(value.max_note_length));
}

void decode(Reader& reader, Bounds& value) {
  value.max_topology_elements = reader.u32();
  value.max_failure_domains = reader.u32();
  value.max_observations = reader.u32();
  value.max_obligations = reader.u32();
  value.max_requests_per_plan = reader.u32();
  value.max_attempts_retained = reader.u32();
  value.max_journal_entries = reader.u32();
  value.max_transitions = reader.u32();
  value.max_reason_codes = reader.u32();
  value.max_reason_evidence_refs = reader.u32();
  value.max_scope_elements = reader.u32();
  value.max_protected_obligations_per_request = reader.u32();
  value.max_provenance_length = reader.u32();
  value.max_label_length = reader.u32();
  value.max_note_length = reader.u32();
  if (!reader.ok()) {
    return;
  }
  if (auto r = validate(value); !r.ok()) {
    reader.fail(r.status());
  }
}

void encode(Writer& writer, const ElectricalPolicy& value) {
  encode(writer, value.generation);
  encode(writer, value.evidence_freshness_window);
  encode(writer, value.evidence_expiry_window);
  encode(writer, value.verification_validity);
  encode(writer, value.stability_dwell);
  encode(writer, value.acknowledgement_window);
  encode(writer, value.verification_window);
  encode(writer, value.generator_start_window);
  encode(writer, value.synchronization_window);
  encode(writer, value.retransfer_hold);
  encode(writer, value.reclose_dwell);
  writer.u32(value.max_attempts_per_request);
  encode(writer, value.reserve_floor);
  encode(writer, value.reserve_critical_floor);
  encode(writer, value.nominal_voltage);
  encode(writer, value.nominal_frequency);
  encode(writer, value.voltage_tolerance);
  encode(writer, value.frequency_tolerance);
  writer.flag(value.treat_stale_evidence_as_failure);
  writer.flag(value.require_shared_domain_resolution_for_recovery);
  writer.flag(value.require_operator_authorization_for_recovery);
  writer.flag(value.require_verified_isolation_for_recovery);
  writer.flag(value.require_reserve_floor_for_recovery);
}

void decode(Reader& reader, ElectricalPolicy& value) {
  decode(reader, value.generation);
  decode(reader, value.evidence_freshness_window);
  decode(reader, value.evidence_expiry_window);
  decode(reader, value.verification_validity);
  decode(reader, value.stability_dwell);
  decode(reader, value.acknowledgement_window);
  decode(reader, value.verification_window);
  decode(reader, value.generator_start_window);
  decode(reader, value.synchronization_window);
  decode(reader, value.retransfer_hold);
  decode(reader, value.reclose_dwell);
  value.max_attempts_per_request = reader.u32();
  decode(reader, value.reserve_floor);
  decode(reader, value.reserve_critical_floor);
  decode(reader, value.nominal_voltage);
  decode(reader, value.nominal_frequency);
  decode(reader, value.voltage_tolerance);
  decode(reader, value.frequency_tolerance);
  value.treat_stale_evidence_as_failure = reader.flag();
  value.require_shared_domain_resolution_for_recovery = reader.flag();
  value.require_operator_authorization_for_recovery = reader.flag();
  value.require_verified_isolation_for_recovery = reader.flag();
  value.require_reserve_floor_for_recovery = reader.flag();
  if (!reader.ok()) {
    return;
  }
  if (auto r = validate(value); !r.ok()) {
    reader.fail(r.status());
  }
}

// --- topology -------------------------------------------------------------

void encode(Writer& writer, const TopologyElement& value, const Bounds& bounds) {
  put_ref(writer, value.element);
  put_enum(writer, value.kind);
  put_ref(writer, value.upstream);
  put_ref(writer, value.domain);
  put_ref(writer, value.load_group);
  put_ref(writer, value.isolation_point);
  writer.flag(value.has_isolation_point);
  writer.flag(value.transfer_capable);
  writer.text(value.label, bounds.max_label_length);
}

void decode(Reader& reader, TopologyElement& value, const Bounds& bounds) {
  get_ref(reader, value.element);
  get_enum(reader, value.kind,
           [](ElementKind candidate) { return candidate == ElementKind::Unknown || element_kind_name(candidate) != "unknown"; },
           "element kind");
  get_ref(reader, value.upstream);
  get_ref(reader, value.domain);
  get_ref(reader, value.load_group);
  get_ref(reader, value.isolation_point);
  value.has_isolation_point = reader.flag();
  value.transfer_capable = reader.flag();
  reader.text(value.label, bounds.max_label_length);
}

void encode(Writer& writer, const FailureDomain& value, const Bounds& bounds) {
  put_ref(writer, value.domain);
  put_refs(writer, value.members, bounds.max_topology_elements);
  writer.flag(value.shared);
}

void decode(Reader& reader, FailureDomain& value, const Bounds& bounds) {
  get_ref(reader, value.domain);
  get_refs(reader, value.members, bounds.max_topology_elements);
  value.shared = reader.flag();
}

void encode(Writer& writer, const TopologySnapshot& value, const Bounds& bounds) {
  encode(writer, value.generation);
  writer.u32(static_cast<std::uint32_t>(value.elements.size()));
  for (const auto& element : value.elements) {
    encode(writer, element, bounds);
  }
  writer.u32(static_cast<std::uint32_t>(value.domains.size()));
  for (const auto& domain : value.domains) {
    encode(writer, domain, bounds);
  }
}

void decode(Reader& reader, TopologySnapshot& value, const Bounds& bounds) {
  decode(reader, value.generation);
  std::uint32_t count = 0;
  if (!begin_list(reader, bounds.max_topology_elements, count, "topology element")) {
    return;
  }
  value.elements.resize(count);
  for (std::uint32_t i = 0; i < count; ++i) {
    decode(reader, value.elements[i], bounds);
  }
  if (!begin_list(reader, bounds.max_failure_domains, count, "failure domain")) {
    return;
  }
  value.domains.resize(count);
  for (std::uint32_t i = 0; i < count; ++i) {
    decode(reader, value.domains[i], bounds);
  }
}

// --- evidence -------------------------------------------------------------

void encode(Writer& writer, const ElectricalObservation& value, const Bounds& bounds) {
  put_ref(writer, value.element);
  put_enum(writer, value.kind);
  put_enum(writer, value.origin);
  put_enum(writer, value.quality);
  encode(writer, value.sequence);
  encode(writer, value.observed_at);
  encode(writer, value.evidence_fingerprint);
  writer.text(value.provenance, bounds.max_provenance_length);
  put_enum(writer, value.energization);
  put_enum(writer, value.breaker);
  put_enum(writer, value.generator);
  put_enum(writer, value.transfer);
  writer.flag(value.has_voltage);
  encode(writer, value.voltage);
  writer.flag(value.has_frequency);
  encode(writer, value.frequency);
  writer.flag(value.has_reserve);
  encode(writer, value.reserve);
  writer.flag(value.has_current);
  encode(writer, value.current);
}

void decode(Reader& reader, ElectricalObservation& value, const Bounds& bounds) {
  get_ref(reader, value.element);
  get_enum(reader, value.kind,
           [](ElementKind candidate) { return candidate == ElementKind::Unknown || element_kind_name(candidate) != "unknown"; },
           "element kind");
  get_enum(reader, value.origin,
           [](ObservationOrigin candidate) {
             return candidate == ObservationOrigin::Unknown || observation_origin_name(candidate) != "unknown";
           },
           "observation origin");
  get_enum(reader, value.quality,
           [](EvidenceQuality candidate) { return candidate == EvidenceQuality::Unknown || evidence_quality_name(candidate) != "unknown"; },
           "evidence quality");
  decode(reader, value.sequence);
  decode(reader, value.observed_at);
  decode(reader, value.evidence_fingerprint);
  reader.text(value.provenance, bounds.max_provenance_length);
  get_enum(reader, value.energization,
           [](EnergizationState candidate) {
             return candidate == EnergizationState::Unknown || energization_state_name(candidate) != "unknown";
           },
           "energization state");
  get_enum(reader, value.breaker,
           [](BreakerPosition candidate) { return candidate == BreakerPosition::Unknown || breaker_position_name(candidate) != "unknown"; },
           "breaker position");
  get_enum(reader, value.generator,
           [](GeneratorState candidate) { return candidate == GeneratorState::Unknown || generator_state_name(candidate) != "unknown"; },
           "generator state");
  get_enum(reader, value.transfer,
           [](TransferState candidate) { return candidate == TransferState::Unknown || transfer_state_name(candidate) != "unknown"; },
           "transfer state");
  value.has_voltage = reader.flag();
  decode(reader, value.voltage);
  value.has_frequency = reader.flag();
  decode(reader, value.frequency);
  value.has_reserve = reader.flag();
  decode(reader, value.reserve);
  value.has_current = reader.flag();
  decode(reader, value.current);
}

// --- obligations ----------------------------------------------------------

void encode(Writer& writer, const ProtectedObligation& value, const Bounds& bounds) {
  encode(writer, value.id);
  put_ref(writer, value.target);
  put_enum(writer, value.protection);
  put_enum(writer, value.status);
  writer.flag(value.has_report);
  encode(writer, value.reported_at);
  put_ref(writer, value.reporting_authority);
  writer.flag(value.has_reserve_floor);
  encode(writer, value.reserve_floor);
  writer.flag(value.has_max_outage);
  encode(writer, value.max_outage);
  encode(writer, value.policy_generation);
  static_cast<void>(bounds);
}

void decode(Reader& reader, ProtectedObligation& value, const Bounds& bounds) {
  decode(reader, value.id);
  get_ref(reader, value.target);
  get_enum(reader, value.protection,
           [](ProtectionClass candidate) {
             return protection_class_name(candidate) != "unknown";
           },
           "protection class");
  get_enum(reader, value.status,
           [](ObligationStatus candidate) {
             return candidate == ObligationStatus::Unknown || obligation_status_name(candidate) != "unknown";
           },
           "obligation status");
  value.has_report = reader.flag();
  decode(reader, value.reported_at);
  get_ref(reader, value.reporting_authority);
  value.has_reserve_floor = reader.flag();
  decode(reader, value.reserve_floor);
  value.has_max_outage = reader.flag();
  decode(reader, value.max_outage);
  decode(reader, value.policy_generation);
  if (!reader.ok()) {
    return;
  }
  if (auto r = validate(value, bounds); !r.ok()) {
    reader.fail(r.status());
  }
}

// --- reasons, scope, requests, plans --------------------------------------

void encode(Writer& writer, const ReasonStep& value, const Bounds& bounds) {
  put_enum(writer, value.code);
  put_ref(writer, value.subject);
  put_enum(writer, value.freshness);
  put_fingerprints(writer, value.evidence, bounds.max_reason_evidence_refs);
}

void decode(Reader& reader, ReasonStep& value, const Bounds& bounds) {
  get_enum(reader, value.code,
           [](ReasonCode candidate) { return reason_code_name(candidate) != "unknown"; },
           "reason code");
  get_ref(reader, value.subject);
  get_enum(reader, value.freshness,
           [](Freshness candidate) { return candidate == Freshness::Unknown || freshness_name(candidate) != "unknown"; }, "freshness");
  get_fingerprints(reader, value.evidence, bounds.max_reason_evidence_refs);
}

void encode(Writer& writer, const RequestTarget& value, const Bounds& bounds) {
  put_ref(writer, value.element);
  put_enum(writer, value.kind);
  put_enum(writer, value.owner);
  put_ref(writer, value.controller);
  static_cast<void>(bounds);
}

void decode(Reader& reader, RequestTarget& value, const Bounds& bounds) {
  get_ref(reader, value.element);
  get_enum(reader, value.kind,
           [](ElementKind candidate) { return candidate == ElementKind::Unknown || element_kind_name(candidate) != "unknown"; },
           "element kind");
  get_enum(reader, value.owner,
           [](RequestOwner candidate) { return candidate == RequestOwner::Unknown || request_owner_name(candidate) != "unknown"; },
           "request owner");
  get_ref(reader, value.controller);
  static_cast<void>(bounds);
}

void encode(Writer& writer, const ResponseRequest& value, const Bounds& bounds) {
  encode(writer, value.id);
  put_enum(writer, value.kind);
  encode(writer, value.target, bounds);
  encode(writer, value.plan_generation);
  encode(writer, value.incident_generation);
  encode(writer, value.attempt);
  writer.u32(value.attempt_ordinal);
  encode(writer, value.idempotency);
  encode(writer, value.fingerprint);
  put_enum(writer, value.state);
  put_enum(writer, value.required_proof);
  writer.flag(value.has_magnitude);
  encode(writer, value.magnitude);
  encode(writer, value.created_at);
  writer.flag(value.has_issued_at);
  encode(writer, value.issued_at);
  writer.flag(value.has_acknowledged_at);
  encode(writer, value.acknowledged_at);
  writer.flag(value.has_observed_at);
  encode(writer, value.observed_at);
  writer.flag(value.has_verified_at);
  encode(writer, value.verified_at);
  encode(writer, value.deadline);
  put_enum(writer, value.justification);
  put_ref(writer, value.justification_subject);
  put_fingerprints(writer, value.evidence, bounds.max_reason_evidence_refs);
  writer.text(value.owner_note, bounds.max_note_length);
  put_ref(writer, value.verification_source);
  encode(writer, value.verification_evidence);
  encode(writer, value.verification_observed_at);
  encode(writer, value.superseded_by);
  put_enum(writer, value.terminal_reason);
}

void decode(Reader& reader, ResponseRequest& value, const Bounds& bounds) {
  decode(reader, value.id);
  get_enum(reader, value.kind,
           [](RequestKind candidate) { return request_kind_name(candidate) != "unknown"; },
           "request kind");
  decode(reader, value.target, bounds);
  decode(reader, value.plan_generation);
  decode(reader, value.incident_generation);
  decode(reader, value.attempt);
  value.attempt_ordinal = reader.u32();
  decode(reader, value.idempotency);
  decode(reader, value.fingerprint);
  get_enum(reader, value.state,
           [](RequestState candidate) { return request_state_name(candidate) != "unknown"; },
           "request state");
  get_enum(reader, value.required_proof,
           [](ProofKind candidate) { return proof_kind_name(candidate) != "unknown"; },
           "proof kind");
  value.has_magnitude = reader.flag();
  decode(reader, value.magnitude);
  decode(reader, value.created_at);
  value.has_issued_at = reader.flag();
  decode(reader, value.issued_at);
  value.has_acknowledged_at = reader.flag();
  decode(reader, value.acknowledged_at);
  value.has_observed_at = reader.flag();
  decode(reader, value.observed_at);
  value.has_verified_at = reader.flag();
  decode(reader, value.verified_at);
  decode(reader, value.deadline);
  get_enum(reader, value.justification,
           [](ReasonCode candidate) { return reason_code_name(candidate) != "unknown"; },
           "reason code");
  get_ref(reader, value.justification_subject);
  get_fingerprints(reader, value.evidence, bounds.max_reason_evidence_refs);
  reader.text(value.owner_note, bounds.max_note_length);
  get_ref(reader, value.verification_source);
  decode(reader, value.verification_evidence);
  decode(reader, value.verification_observed_at);
  decode(reader, value.superseded_by);
  get_enum(reader, value.terminal_reason,
           [](ReasonCode candidate) { return reason_code_name(candidate) != "unknown"; },
           "reason code");
  if (!reader.ok()) {
    return;
  }
  if (auto r = validate(value, bounds); !r.ok()) {
    reader.fail(r.status());
  }
}

void encode(Writer& writer, const IsolationRequirement& value, const Bounds& bounds) {
  put_ref(writer, value.element);
  put_ref(writer, value.isolation_point);
  writer.flag(value.has_isolation_point);
  put_enum(writer, value.required_proof);
  put_enum(writer, value.failure);
  put_enum(writer, value.reason);
  static_cast<void>(bounds);
}

void decode(Reader& reader, IsolationRequirement& value, const Bounds& bounds) {
  get_ref(reader, value.element);
  get_ref(reader, value.isolation_point);
  value.has_isolation_point = reader.flag();
  get_enum(reader, value.required_proof,
           [](ProofKind candidate) { return proof_kind_name(candidate) != "unknown"; },
           "proof kind");
  get_enum(reader, value.failure,
           [](FailureClass candidate) {
             return failure_class_name(candidate) != "unknown" ||
                    candidate == FailureClass::None;
           },
           "failure class");
  get_enum(reader, value.reason,
           [](ReasonCode candidate) { return reason_code_name(candidate) != "unknown"; },
           "reason code");
  static_cast<void>(bounds);
}

void encode(Writer& writer, const ProtectionRequirement& value, const Bounds& bounds) {
  put_ref(writer, value.element);
  put_enum(writer, value.protection);
  writer.flag(value.has_obligation);
  encode(writer, value.obligation);
  writer.flag(value.has_reserve_floor);
  encode(writer, value.reserve_floor);
  put_enum(writer, value.reason);
  static_cast<void>(bounds);
}

void decode(Reader& reader, ProtectionRequirement& value, const Bounds& bounds) {
  get_ref(reader, value.element);
  get_enum(reader, value.protection,
           [](ProtectionClass candidate) {
             return protection_class_name(candidate) != "unknown";
           },
           "protection class");
  value.has_obligation = reader.flag();
  decode(reader, value.obligation);
  value.has_reserve_floor = reader.flag();
  decode(reader, value.reserve_floor);
  get_enum(reader, value.reason,
           [](ReasonCode candidate) { return reason_code_name(candidate) != "unknown"; },
           "reason code");
  static_cast<void>(bounds);
}

void encode(Writer& writer, const AffectedScope& value, const Bounds& bounds) {
  encode(writer, value.topology_generation);
  put_refs(writer, value.failed_elements, bounds.max_scope_elements);
  put_refs(writer, value.impacted_elements, bounds.max_scope_elements);
  put_refs(writer, value.load_groups, bounds.max_scope_elements);
  put_refs(writer, value.domains, bounds.max_scope_elements);
  put_refs(writer, value.isolation_points, bounds.max_scope_elements);
  put_refs(writer, value.shared_domains, bounds.max_scope_elements);
  put_refs(writer, value.unresolved_upstream, bounds.max_scope_elements);
  put_refs(writer, value.unevidenced_elements, bounds.max_scope_elements);
  writer.flag(value.shared_domain_impacted);
  writer.flag(value.upstream_evidence_unresolved);
}

void decode(Reader& reader, AffectedScope& value, const Bounds& bounds) {
  decode(reader, value.topology_generation);
  get_refs(reader, value.failed_elements, bounds.max_scope_elements);
  get_refs(reader, value.impacted_elements, bounds.max_scope_elements);
  get_refs(reader, value.load_groups, bounds.max_scope_elements);
  get_refs(reader, value.domains, bounds.max_scope_elements);
  get_refs(reader, value.isolation_points, bounds.max_scope_elements);
  get_refs(reader, value.shared_domains, bounds.max_scope_elements);
  get_refs(reader, value.unresolved_upstream, bounds.max_scope_elements);
  get_refs(reader, value.unevidenced_elements, bounds.max_scope_elements);
  value.shared_domain_impacted = reader.flag();
  value.upstream_evidence_unresolved = reader.flag();
}

void encode(Writer& writer, const ResponsePlan& value, const Bounds& bounds) {
  encode(writer, value.generation);
  encode(writer, value.incident);
  encode(writer, value.incident_generation);
  encode(writer, value.topology_generation);
  encode(writer, value.policy_generation);
  encode(writer, value.epoch);
  encode(writer, value.decided_at);
  put_enum(writer, value.primary_failure);
  put_ref(writer, value.primary_element);
  writer.u32(static_cast<std::uint32_t>(value.failures.size()));
  for (const auto& failure : value.failures) {
    put_enum(writer, failure.klass);
    put_ref(writer, failure.element);
    put_enum(writer, failure.kind);
    put_enum(writer, failure.primary_reason);
    writer.flag(failure.evidence_current);
    writer.u32(static_cast<std::uint32_t>(failure.reasons.size()));
    for (const auto& reason : failure.reasons) {
      encode(writer, reason, bounds);
    }
  }
  writer.u32(static_cast<std::uint32_t>(value.reasons.size()));
  for (const auto& reason : value.reasons) {
    encode(writer, reason, bounds);
  }
  encode(writer, value.scope, bounds);
  writer.u32(static_cast<std::uint32_t>(value.isolations.size()));
  for (const auto& isolation : value.isolations) {
    encode(writer, isolation, bounds);
  }
  writer.u32(static_cast<std::uint32_t>(value.protections.size()));
  for (const auto& protection : value.protections) {
    encode(writer, protection, bounds);
  }
  writer.u32(static_cast<std::uint32_t>(value.requests.size()));
  for (const auto& request : value.requests) {
    encode(writer, request, bounds);
  }
  encode(writer, value.fingerprint);
}

void decode(Reader& reader, ResponsePlan& value, const Bounds& bounds) {
  decode(reader, value.generation);
  decode(reader, value.incident);
  decode(reader, value.incident_generation);
  decode(reader, value.topology_generation);
  decode(reader, value.policy_generation);
  decode(reader, value.epoch);
  decode(reader, value.decided_at);
  get_enum(reader, value.primary_failure,
           [](FailureClass candidate) {
             return failure_class_name(candidate) != "unknown" ||
                    candidate == FailureClass::None;
           },
           "failure class");
  get_ref(reader, value.primary_element);
  std::uint32_t count = 0;
  if (!begin_list(reader, bounds.max_scope_elements, count, "failure")) {
    return;
  }
  value.failures.resize(count);
  for (std::uint32_t i = 0; i < count; ++i) {
    auto& failure = value.failures[i];
    get_enum(reader, failure.klass,
             [](FailureClass candidate) {
               return failure_class_name(candidate) != "unknown" ||
                      candidate == FailureClass::None;
             },
             "failure class");
    get_ref(reader, failure.element);
    get_enum(reader, failure.kind,
             [](ElementKind candidate) { return candidate == ElementKind::Unknown || element_kind_name(candidate) != "unknown"; },
             "element kind");
    get_enum(reader, failure.primary_reason,
             [](ReasonCode candidate) { return reason_code_name(candidate) != "unknown"; },
             "reason code");
    failure.evidence_current = reader.flag();
    std::uint32_t reason_count = 0;
    if (!begin_list(reader, bounds.max_reason_codes, reason_count, "reason")) {
      return;
    }
    failure.reasons.resize(reason_count);
    for (std::uint32_t j = 0; j < reason_count; ++j) {
      decode(reader, failure.reasons[j], bounds);
    }
  }
  if (!begin_list(reader, bounds.max_reason_codes, count, "reason")) {
    return;
  }
  value.reasons.resize(count);
  for (std::uint32_t i = 0; i < count; ++i) {
    decode(reader, value.reasons[i], bounds);
  }
  decode(reader, value.scope, bounds);
  if (!begin_list(reader, bounds.max_scope_elements, count, "isolation requirement")) {
    return;
  }
  value.isolations.resize(count);
  for (std::uint32_t i = 0; i < count; ++i) {
    decode(reader, value.isolations[i], bounds);
  }
  if (!begin_list(reader, bounds.max_obligations, count, "protection requirement")) {
    return;
  }
  value.protections.resize(count);
  for (std::uint32_t i = 0; i < count; ++i) {
    decode(reader, value.protections[i], bounds);
  }
  if (!begin_list(reader, bounds.max_requests_per_plan, count, "request")) {
    return;
  }
  value.requests.resize(count);
  for (std::uint32_t i = 0; i < count; ++i) {
    decode(reader, value.requests[i], bounds);
  }
  decode(reader, value.fingerprint);
}

void encode(Writer& writer, const GateEvaluation& value, const Bounds& bounds) {
  put_enum(writer, value.gate);
  writer.flag(value.satisfied);
  put_enum(writer, value.reason);
  put_ref(writer, value.subject);
  writer.text(value.detail, bounds.max_note_length);
}

void decode(Reader& reader, GateEvaluation& value, const Bounds& bounds) {
  get_enum(reader, value.gate,
           [](RecoveryGate candidate) { return recovery_gate_name(candidate) != "unknown"; },
           "recovery gate");
  value.satisfied = reader.flag();
  get_enum(reader, value.reason,
           [](ReasonCode candidate) { return reason_code_name(candidate) != "unknown"; },
           "reason code");
  get_ref(reader, value.subject);
  reader.text(value.detail, bounds.max_note_length);
}

void encode(Writer& writer, const RecoveryAssessment& value, const Bounds& bounds) {
  writer.flag(value.eligible);
  encode(writer, value.evaluated_at);
  encode(writer, value.plan_generation);
  writer.u32(static_cast<std::uint32_t>(value.gates.size()));
  for (const auto& gate : value.gates) {
    encode(writer, gate, bounds);
  }
  put_refs(writer, value.blocking_elements, bounds.max_scope_elements);
}

void decode(Reader& reader, RecoveryAssessment& value, const Bounds& bounds) {
  value.eligible = reader.flag();
  decode(reader, value.evaluated_at);
  decode(reader, value.plan_generation);
  std::uint32_t count = 0;
  if (!begin_list(reader, 64, count, "gate")) {
    return;
  }
  value.gates.resize(count);
  for (std::uint32_t i = 0; i < count; ++i) {
    decode(reader, value.gates[i], bounds);
  }
  get_refs(reader, value.blocking_elements, bounds.max_scope_elements);
}

// --- durable state --------------------------------------------------------

void encode(Writer& writer, const ObservationSlot& value, const Bounds& bounds) {
  put_ref(writer, value.element);
  put_enum(writer, value.kind);
  encode(writer, value.observation, bounds);
  // Currency is a function of the instant it is asked about, not a durable
  // fact: it is encoded canonically and recomputed from the observation
  // instant, so a replayed generation and a live one are byte-identical.
  put_enum(writer, Freshness::Unknown);
  writer.flag(value.present);
  writer.flag(value.recovered);
  writer.flag(value.contradicted);
  put_fingerprints(writer, value.conflict_evidence, bounds.max_reason_evidence_refs);
}

void decode(Reader& reader, ObservationSlot& value, const Bounds& bounds) {
  get_ref(reader, value.element);
  get_enum(reader, value.kind,
           [](ElementKind candidate) { return candidate == ElementKind::Unknown || element_kind_name(candidate) != "unknown"; },
           "element kind");
  decode(reader, value.observation, bounds);
  get_enum(reader, value.freshness,
           [](Freshness candidate) { return candidate == Freshness::Unknown || freshness_name(candidate) != "unknown"; }, "freshness");
  value.present = reader.flag();
  value.recovered = reader.flag();
  value.contradicted = reader.flag();
  get_fingerprints(reader, value.conflict_evidence, bounds.max_reason_evidence_refs);
}

void encode(Writer& writer, const RequestSlot& value, const Bounds& bounds) {
  encode(writer, value.request, bounds);
}

void decode(Reader& reader, RequestSlot& value, const Bounds& bounds) {
  decode(reader, value.request, bounds);
}

void encode(Writer& writer, const ObligationSlot& value, const Bounds& bounds) {
  encode(writer, value.obligation, bounds);
  // Derived the same way as observation currency.
  writer.flag(false);
}

void decode(Reader& reader, ObligationSlot& value, const Bounds& bounds) {
  decode(reader, value.obligation, bounds);
  value.report_current = reader.flag();
}

void encode(Writer& writer, const RelaxationRecord& value, const Bounds& bounds) {
  encode(writer, value.obligation);
  put_enum(writer, value.protection);
  encode(writer, value.epoch);
  encode(writer, value.incident);
  encode(writer, value.granted_at);
  put_ref(writer, value.granted_by);
  static_cast<void>(bounds);
}

void decode(Reader& reader, RelaxationRecord& value, const Bounds& bounds) {
  decode(reader, value.obligation);
  get_enum(reader, value.protection,
           [](ProtectionClass candidate) {
             return protection_class_name(candidate) != "unknown";
           },
           "protection class");
  decode(reader, value.epoch);
  decode(reader, value.incident);
  decode(reader, value.granted_at);
  get_ref(reader, value.granted_by);
  static_cast<void>(bounds);
}

void encode(Writer& writer, const TransitionRecord& value, const Bounds& bounds) {
  encode(writer, value.sequence);
  encode(writer, value.recorded_at);
  put_enum(writer, value.kind);
  encode(writer, value.from_revision);
  encode(writer, value.to_revision);
  put_enum(writer, value.reason);
  put_ref(writer, value.subject);
  static_cast<void>(bounds);
}

void decode(Reader& reader, TransitionRecord& value, const Bounds& bounds) {
  decode(reader, value.sequence);
  decode(reader, value.recorded_at);
  get_enum(reader, value.kind,
           [](JournalKind candidate) { return journal_kind_name(candidate) != "unknown"; },
           "journal kind");
  decode(reader, value.from_revision);
  decode(reader, value.to_revision);
  get_enum(reader, value.reason,
           [](ReasonCode candidate) { return reason_code_name(candidate) != "unknown"; },
           "reason code");
  get_ref(reader, value.subject);
  static_cast<void>(bounds);
}

void encode(Writer& writer, const IncidentProjection& value, const Bounds& bounds) {
  encode(writer, value.incident);
  encode(writer, value.generation);
  put_enum(writer, value.lifecycle);
  encode(writer, value.opened_at);
  encode(writer, value.updated_at);
  put_enum(writer, value.primary_failure);
  put_ref(writer, value.primary_element);
  writer.flag(value.has_plan);
  encode(writer, value.plan_generation);
  encode(writer, value.plan_fingerprint);
  writer.flag(value.operator_authorized);
  encode(writer, value.authorized_at);
  encode(writer, value.authorized_generation);
  writer.flag(value.has_stable_since);
  encode(writer, value.stable_since);
  writer.u64(value.recovery_steps);
  encode(writer, value.transitions);
  put_refs(writer, value.impacted_scope, bounds.max_scope_elements);
}

void decode(Reader& reader, IncidentProjection& value, const Bounds& bounds) {
  decode(reader, value.incident);
  decode(reader, value.generation);
  get_enum(reader, value.lifecycle,
           [](IncidentLifecycle candidate) {
             return incident_lifecycle_name(candidate) != "unknown";
           },
           "incident lifecycle");
  decode(reader, value.opened_at);
  decode(reader, value.updated_at);
  get_enum(reader, value.primary_failure,
           [](FailureClass candidate) {
             return failure_class_name(candidate) != "unknown" ||
                    candidate == FailureClass::None;
           },
           "failure class");
  get_ref(reader, value.primary_element);
  value.has_plan = reader.flag();
  decode(reader, value.plan_generation);
  decode(reader, value.plan_fingerprint);
  value.operator_authorized = reader.flag();
  decode(reader, value.authorized_at);
  decode(reader, value.authorized_generation);
  value.has_stable_since = reader.flag();
  decode(reader, value.stable_since);
  value.recovery_steps = reader.u64();
  decode(reader, value.transitions);
  get_refs(reader, value.impacted_scope, bounds.max_scope_elements);
}

void encode(Writer& writer, const DomainState& value, const Bounds& bounds) {
  encode(writer, value.epoch);
  encode(writer, value.incarnation);
  encode(writer, value.revision);
  encode(writer, value.topology_generation);
  encode(writer, value.evidence_generation);
  encode(writer, value.dispatch_sequence);
  encode(writer, value.policy_generation);
  encode(writer, value.policy);
  encode(writer, value.bounds);
  encode(writer, value.incident, bounds);
  writer.flag(value.has_plan);
  encode(writer, value.plan, bounds);
  writer.flag(value.has_topology);
  encode(writer, value.topology, bounds);
  writer.u32(static_cast<std::uint32_t>(value.observations.size()));
  for (const auto& slot : value.observations) {
    encode(writer, slot, bounds);
  }
  writer.u32(static_cast<std::uint32_t>(value.requests.size()));
  for (const auto& slot : value.requests) {
    encode(writer, slot, bounds);
  }
  writer.u32(static_cast<std::uint32_t>(value.obligations.size()));
  for (const auto& slot : value.obligations) {
    encode(writer, slot, bounds);
  }
  writer.u32(static_cast<std::uint32_t>(value.relaxations.size()));
  for (const auto& record : value.relaxations) {
    encode(writer, record, bounds);
  }
  writer.u32(static_cast<std::uint32_t>(value.transitions.size()));
  for (const auto& record : value.transitions) {
    encode(writer, record, bounds);
  }
  encode(writer, value.next_request_id);
  encode(writer, value.next_attempt_id);
  encode(writer, value.next_plan_generation);
}

void decode(Reader& reader, DomainState& value, const Bounds& bounds) {
  decode(reader, value.epoch);
  decode(reader, value.incarnation);
  decode(reader, value.revision);
  decode(reader, value.topology_generation);
  decode(reader, value.evidence_generation);
  decode(reader, value.dispatch_sequence);
  decode(reader, value.policy_generation);
  decode(reader, value.policy);
  decode(reader, value.bounds);
  decode(reader, value.incident, bounds);
  value.has_plan = reader.flag();
  decode(reader, value.plan, bounds);
  value.has_topology = reader.flag();
  decode(reader, value.topology, bounds);
  std::uint32_t count = 0;
  if (!begin_list(reader, bounds.max_observations, count, "observation slot")) {
    return;
  }
  value.observations.resize(count);
  for (std::uint32_t i = 0; i < count; ++i) {
    decode(reader, value.observations[i], bounds);
  }
  if (!begin_list(reader, bounds.max_attempts_retained, count, "request slot")) {
    return;
  }
  value.requests.resize(count);
  for (std::uint32_t i = 0; i < count; ++i) {
    decode(reader, value.requests[i], bounds);
  }
  if (!begin_list(reader, bounds.max_obligations, count, "obligation slot")) {
    return;
  }
  value.obligations.resize(count);
  for (std::uint32_t i = 0; i < count; ++i) {
    decode(reader, value.obligations[i], bounds);
  }
  if (!begin_list(reader, bounds.max_obligations, count, "relaxation")) {
    return;
  }
  value.relaxations.resize(count);
  for (std::uint32_t i = 0; i < count; ++i) {
    decode(reader, value.relaxations[i], bounds);
  }
  if (!begin_list(reader, bounds.max_transitions, count, "transition")) {
    return;
  }
  value.transitions.resize(count);
  for (std::uint32_t i = 0; i < count; ++i) {
    decode(reader, value.transitions[i], bounds);
  }
  decode(reader, value.next_request_id);
  decode(reader, value.next_attempt_id);
  decode(reader, value.next_plan_generation);
}

// --- journal --------------------------------------------------------------

void encode(Writer& writer, const JournalPayload& value, const Bounds& bounds) {
  // An absent optional member is not written at all. Every optional member of a
  // payload is a complete, independently validated value, so writing a
  // placeholder for one that is absent would make the entry undecodable (and
  // would change its fingerprint).
  writer.flag(value.has_observation);
  if (value.has_observation) {
    encode(writer, value.observation, bounds);
  }
  writer.flag(value.has_topology);
  if (value.has_topology) {
    encode(writer, value.topology, bounds);
  }
  writer.flag(value.has_policy);
  if (value.has_policy) {
    encode(writer, value.policy);
  }
  writer.flag(value.has_obligations);
  if (value.has_obligations) {
    writer.u32(static_cast<std::uint32_t>(value.obligations.size()));
    for (const auto& obligation : value.obligations) {
      encode(writer, obligation, bounds);
    }
  }
  writer.flag(value.has_plan);
  if (value.has_plan) {
    encode(writer, value.plan, bounds);
  }
  encode(writer, value.request);
  put_enum(writer, value.from_state);
  put_enum(writer, value.to_state);
  writer.flag(value.has_request_state);
  encode(writer, value.attempt);
  put_ref(writer, value.source);
  encode(writer, value.evidence);
  put_enum(writer, value.reason);
  writer.text(value.note, bounds.max_note_length);
  encode(writer, value.incident);
  encode(writer, value.incident_generation);
  writer.flag(value.has_incident);
  encode(writer, value.epoch);
  encode(writer, value.incarnation);
  writer.flag(value.epoch_rollover);
  writer.flag(value.has_recovery_authorization);
  encode(writer, value.authorized_at);
  encode(writer, value.obligation);
  writer.flag(value.has_obligation);
  put_enum(writer, value.relaxation_class);
  put_ref(writer, value.granted_by);
}

void decode(Reader& reader, JournalPayload& value, const Bounds& bounds) {
  value.has_observation = reader.flag();
  if (value.has_observation) {
    decode(reader, value.observation, bounds);
  }
  value.has_topology = reader.flag();
  if (value.has_topology) {
    decode(reader, value.topology, bounds);
  }
  value.has_policy = reader.flag();
  if (value.has_policy) {
    decode(reader, value.policy);
  }
  value.has_obligations = reader.flag();
  std::uint32_t count = 0;
  if (value.has_obligations) {
    if (!begin_list(reader, bounds.max_obligations, count, "obligation")) {
      return;
    }
    value.obligations.resize(count);
    for (std::uint32_t i = 0; i < count; ++i) {
      decode(reader, value.obligations[i], bounds);
    }
  }
  value.has_plan = reader.flag();
  if (value.has_plan) {
    decode(reader, value.plan, bounds);
  }
  decode(reader, value.request);
  get_enum(reader, value.from_state,
           [](RequestState candidate) { return request_state_name(candidate) != "unknown"; },
           "request state");
  get_enum(reader, value.to_state,
           [](RequestState candidate) { return request_state_name(candidate) != "unknown"; },
           "request state");
  value.has_request_state = reader.flag();
  decode(reader, value.attempt);
  get_ref(reader, value.source);
  decode(reader, value.evidence);
  get_enum(reader, value.reason,
           [](ReasonCode candidate) { return reason_code_name(candidate) != "unknown"; },
           "reason code");
  reader.text(value.note, bounds.max_note_length);
  decode(reader, value.incident);
  decode(reader, value.incident_generation);
  value.has_incident = reader.flag();
  decode(reader, value.epoch);
  decode(reader, value.incarnation);
  value.epoch_rollover = reader.flag();
  value.has_recovery_authorization = reader.flag();
  decode(reader, value.authorized_at);
  decode(reader, value.obligation);
  value.has_obligation = reader.flag();
  get_enum(reader, value.relaxation_class,
           [](ProtectionClass candidate) {
             return protection_class_name(candidate) != "unknown";
           },
           "protection class");
  get_ref(reader, value.granted_by);
}

void encode(Writer& writer, const JournalEntry& value, const Bounds& bounds) {
  encode(writer, value.sequence);
  put_enum(writer, value.kind);
  encode(writer, value.recorded_at);
  encode(writer, value.revision);
  encode(writer, value.payload, bounds);
  encode(writer, value.fingerprint);
}

void decode(Reader& reader, JournalEntry& value, const Bounds& bounds) {
  decode(reader, value.sequence);
  get_enum(reader, value.kind,
           [](JournalKind candidate) { return journal_kind_name(candidate) != "unknown"; },
           "journal kind");
  decode(reader, value.recorded_at);
  decode(reader, value.revision);
  decode(reader, value.payload, bounds);
  decode(reader, value.fingerprint);
  if (!reader.ok()) {
    return;
  }
  if (auto r = validate(value, bounds); !r.ok()) {
    reader.fail(r.status());
  }
}

}  // namespace summon::pfm::codec

namespace summon::pfm {

std::vector<std::uint8_t> encode_state(const DomainState& state) {
  codec::Writer writer;
  codec::encode(writer, state, state.bounds);
  if (!writer.ok()) {
    return {};
  }
  return writer.bytes();
}

Result<DomainState> decode_state(std::span<const std::uint8_t> bytes, const Bounds& bounds) {
  codec::Reader reader{bytes};
  DomainState state;
  codec::decode(reader, state, bounds);
  if (!reader.ok()) {
    return reader.error();
  }
  if (!reader.at_end()) {
    return Status::error(StatusCode::StoreTrailingBytes,
                         "encoded state has trailing bytes after the final field")
        .with_context("codec.state");
  }
  return state;
}

}  // namespace summon::pfm
