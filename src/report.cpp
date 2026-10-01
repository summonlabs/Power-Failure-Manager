// Power Failure Manager -- deterministic text rendering.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0 (see LICENSE).
#include "pfm/report.hpp"

#include <sstream>
#include <string>

namespace summon::pfm {
namespace {

std::string refs_to_text(const std::vector<RefToken>& values) {
  if (values.empty()) {
    return "-";
  }
  std::string text;
  for (std::size_t i = 0; i < values.size(); ++i) {
    if (i != 0) {
      text.append(", ");
    }
    text.append(values[i].to_string());
  }
  return text;
}

const char* yes_no(bool value) { return value ? "yes" : "no"; }

}  // namespace

std::string render_evidence(const ElectricalObservation& observation) {
  std::ostringstream out;
  out << observation.element.to_string() << " (" << element_kind_name(observation.kind) << ")";
  out << " energization=" << energization_state_name(observation.energization);
  out << " breaker=" << breaker_position_name(observation.breaker);
  out << " generator=" << generator_state_name(observation.generator);
  out << " transfer=" << transfer_state_name(observation.transfer);
  if (observation.has_voltage) {
    out << " voltage=" << observation.voltage.value() << "mV";
  }
  if (observation.has_frequency) {
    out << " frequency=" << observation.frequency.value() << "mHz";
  }
  if (observation.has_reserve) {
    out << " reserve=" << observation.reserve.value() << "m%";
  }
  if (observation.has_current) {
    out << " current=" << observation.current.value() << "mA";
  }
  out << " origin=" << observation_origin_name(observation.origin);
  out << " quality=" << evidence_quality_name(observation.quality);
  out << " observed_at=" << observation.observed_at.value();
  if (!observation.provenance.empty()) {
    out << " provenance=" << observation.provenance;
  }
  return out.str();
}

std::string render_classification(const ClassificationResult& classification) {
  std::ostringstream out;
  out << "classification: " << classification.failures.size() << " failure(s)\n";
  for (const auto& failure : classification.failures) {
    out << "  " << failure_class_code(failure.klass) << " " << failure.element.to_string()
        << " reason=" << reason_code_name(failure.primary_reason)
        << " evidence_current=" << yes_no(failure.evidence_current) << "\n";
    for (const auto& reason : failure.reasons) {
      out << "      - " << reason_code_name(reason.code) << " " << reason.subject.to_string()
          << " freshness=" << freshness_name(reason.freshness) << "\n";
    }
  }
  out << "  resolved: " << refs_to_text(classification.resolved_elements) << "\n";
  out << "  unresolved: " << refs_to_text(classification.unresolved_elements) << "\n";
  return out.str();
}

std::string render_scope(const AffectedScope& scope) {
  std::ostringstream out;
  out << "scope: topology_generation=" << scope.topology_generation.value() << "\n";
  out << "  failed: " << refs_to_text(scope.failed_elements) << "\n";
  out << "  impacted: " << refs_to_text(scope.impacted_elements) << "\n";
  out << "  load groups: " << refs_to_text(scope.load_groups) << "\n";
  out << "  domains: " << refs_to_text(scope.domains) << "\n";
  out << "  isolation points: " << refs_to_text(scope.isolation_points) << "\n";
  out << "  shared domains: " << refs_to_text(scope.shared_domains) << "\n";
  out << "  unresolved upstream: " << refs_to_text(scope.unresolved_upstream) << "\n";
  out << "  unevidenced: " << refs_to_text(scope.unevidenced_elements) << "\n";
  out << "  shared_domain_impacted=" << yes_no(scope.shared_domain_impacted)
      << " upstream_evidence_unresolved=" << yes_no(scope.upstream_evidence_unresolved) << "\n";
  return out.str();
}

std::string render_request(const ResponseRequest& request) {
  std::ostringstream out;
  out << "request#" << request.id.value() << " " << request_kind_code(request.kind) << " -> "
      << request.target.element.to_string() << " owner=" << request_owner_name(request.target.owner)
      << " state=" << request_state_name(request.state)
      << " proof=" << proof_kind_name(request.required_proof);
  if (request.has_magnitude) {
    out << " magnitude=" << request.magnitude.value() << "bp";
  }
  out << " attempt=" << request.attempt.value() << "/" << request.attempt_ordinal
      << " plan=" << request.plan_generation.value()
      << " idempotency=" << request.idempotency.to_hex()
      << " justification=" << reason_code_name(request.justification);
  if (request.justification_subject.is_set()) {
    out << " subject=" << request.justification_subject.to_string();
  }
  if (request.has_issued_at) {
    out << " issued_at=" << request.issued_at.value();
  }
  if (request.has_acknowledged_at) {
    out << " acknowledged_at=" << request.acknowledged_at.value();
  }
  if (request.has_verified_at) {
    out << " verified_at=" << request.verified_at.value();
  }
  if (request.verification_source.is_set()) {
    out << " verified_by=" << request.verification_source.to_string();
  }
  if (request.terminal_reason != ReasonCode::None) {
    out << " terminal=" << reason_code_name(request.terminal_reason);
  }
  if (!request.owner_note.empty()) {
    out << " note=\"" << request.owner_note << "\"";
  }
  return out.str();
}

std::string render_requests(const std::vector<ResponseRequest>& requests) {
  std::ostringstream out;
  out << "requests: " << requests.size() << "\n";
  for (const auto& request : requests) {
    out << "  " << render_request(request) << "\n";
  }
  return out.str();
}

std::string render_plan(const ResponsePlan& plan) {
  std::ostringstream out;
  out << "plan#" << plan.generation.value() << " incident=" << plan.incident.value()
      << " incident_generation=" << plan.incident_generation.value()
      << " topology_generation=" << plan.topology_generation.value()
      << " policy_generation=" << plan.policy_generation.value()
      << " epoch=" << plan.epoch.value() << "\n";
  out << "  decided_at=" << plan.decided_at.value()
      << " primary=" << failure_class_code(plan.primary_failure)
      << " element=" << plan.primary_element.to_string()
      << " fingerprint=" << plan.fingerprint.to_hex() << "\n";
  out << render_classification(ClassificationResult{plan.failures, {}, {}});
  out << render_scope(plan.scope);
  out << "  isolations:\n";
  for (const auto& isolation : plan.isolations) {
    out << "    " << isolation.element.to_string() << " point="
        << (isolation.has_isolation_point ? isolation.isolation_point.to_string()
                                          : std::string{"none"})
        << " proof=" << proof_kind_name(isolation.required_proof)
        << " failure=" << failure_class_code(isolation.failure) << "\n";
  }
  out << "  protections:\n";
  for (const auto& protection : plan.protections) {
    out << "    " << protection.element.to_string() << " class="
        << protection_class_name(protection.protection)
        << " obligation=" << protection.obligation.value()
        << " reason=" << reason_code_name(protection.reason);
    if (protection.has_reserve_floor) {
      out << " reserve_floor=" << protection.reserve_floor.value() << "m%";
    }
    out << "\n";
  }
  out << render_requests(plan.requests);
  return out.str();
}

std::string render_recovery(const RecoveryAssessment& assessment) {
  std::ostringstream out;
  out << "recovery: " << (assessment.eligible ? "eligible" : "blocked")
      << " plan=" << assessment.plan_generation.value()
      << " evaluated_at=" << assessment.evaluated_at.value() << "\n";
  for (const auto& gate : assessment.gates) {
    out << "  " << recovery_gate_code(gate.gate) << " " << (gate.satisfied ? "satisfied" : "blocked")
        << " reason=" << reason_code_name(gate.reason);
    if (gate.subject.is_set()) {
      out << " subject=" << gate.subject.to_string();
    }
    if (!gate.detail.empty()) {
      out << " (" << gate.detail << ")";
    }
    out << "\n";
  }
  out << "  blocking elements: " << refs_to_text(assessment.blocking_elements) << "\n";
  return out.str();
}

std::string render_state(const DomainState& state) {
  std::ostringstream out;
  out << "state: epoch=" << state.epoch.value() << " incarnation=" << state.incarnation.value()
      << " revision=" << state.revision.value()
      << " topology_generation=" << state.topology_generation.value()
      << " policy_generation=" << state.policy_generation.value()
      << " evidence_generation=" << state.evidence_generation.value() << "\n";
  out << "  incident=" << state.incident.incident.value()
      << " generation=" << state.incident.generation.value()
      << " lifecycle=" << incident_lifecycle_name(state.incident.lifecycle)
      << " primary=" << failure_class_code(state.incident.primary_failure)
      << " operator_authorized=" << yes_no(state.incident.operator_authorized)
      << " stable_since=" << (state.incident.has_stable_since
                                  ? std::to_string(state.incident.stable_since.value())
                                  : std::string{"-"} )
      << "\n";
  out << "  observations: " << state.observations.size() << " requests: " << state.requests.size()
      << " obligations: " << state.obligations.size()
      << " relaxations: " << state.relaxations.size()
      << " transitions: " << state.transitions.size() << "\n";
  for (const auto& slot : state.observations) {
    out << "    " << slot.element.to_string() << " freshness=" << freshness_name(slot.freshness)
        << " present=" << yes_no(slot.present) << " recovered=" << yes_no(slot.recovered)
        << " contradicted=" << yes_no(slot.contradicted) << "\n";
  }
  for (const auto& slot : state.obligations) {
    out << "    obligation#" << slot.obligation.id.value() << " "
        << slot.obligation.target.to_string() << " class="
        << protection_class_name(slot.obligation.protection)
        << " status=" << obligation_status_name(slot.obligation.status)
        << " report_current=" << yes_no(slot.report_current) << "\n";
  }
  if (state.has_plan) {
    out << render_plan(state.plan);
  }
  return out.str();
}

std::string render_store_status(const StoreStatus& status) {
  std::ostringstream out;
  out << "store: mode=" << (status.mode == StoreMode::Durable ? "durable" : "volatile") << "\n";
  if (status.mode == StoreMode::Durable) {
    out << "  directory=" << status.directory << "\n";
  }
  out << "  has_state=" << yes_no(status.has_state)
      << " commit_sequence=" << status.commit_sequence.value()
      << " store_generation=" << status.generation.value()
      << " committed_at=" << status.committed_at.value()
      << " commits=" << status.commits << "\n";
  out << "  fell_back=" << yes_no(status.fell_back);
  if (!status.fallback_detail.empty()) {
    out << " detail=\"" << status.fallback_detail << "\"";
  }
  out << "\n";
  out << "  retired_journal_entries=" << status.retired_journal_entries << "\n";
  for (std::size_t i = 0; i < status.slots.size(); ++i) {
    const auto& slot = status.slots[i];
    out << "  slot[" << i << "] valid=" << yes_no(slot.valid)
        << " commit_sequence=" << slot.commit_sequence.value()
        << " generation=" << slot.generation.value()
        << " committed_at=" << slot.committed_at.value();
    if (!slot.detail.empty()) {
      out << " detail=\"" << slot.detail << "\"";
    }
    out << "\n";
  }
  return out.str();
}

}  // namespace summon::pfm
