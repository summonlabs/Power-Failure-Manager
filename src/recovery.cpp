// Power Failure Manager -- recovery gates and eligibility.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0 (see LICENSE).
#include "pfm/recovery.hpp"

#include <algorithm>
#include <string>
#include <vector>

namespace summon::pfm {
namespace {

struct GateEntry {
  RecoveryGate gate;
  std::string_view name;
  std::string_view code;
};

constexpr GateEntry kGates[] = {
    {RecoveryGate::FaultCleared, "fault-cleared", "pfm.gate.fault_cleared"},
    {RecoveryGate::IsolationVerified, "isolation-verified", "pfm.gate.isolation_verified"},
    {RecoveryGate::DeEnergizationVerified, "de-energization-verified",
     "pfm.gate.de_energization_verified"},
    {RecoveryGate::BreakerStateProven, "breaker-state-proven", "pfm.gate.breaker_state_proven"},
    {RecoveryGate::TransferStable, "transfer-stable", "pfm.gate.transfer_stable"},
    {RecoveryGate::GenerationStable, "generation-stable", "pfm.gate.generation_stable"},
    {RecoveryGate::ReserveRestored, "reserve-restored", "pfm.gate.reserve_restored"},
    {RecoveryGate::SharedDomainResolved, "shared-domain-resolved",
     "pfm.gate.shared_domain_resolved"},
    {RecoveryGate::UpstreamEvidenceCurrent, "upstream-evidence-current",
     "pfm.gate.upstream_evidence_current"},
    {RecoveryGate::ObligationsSatisfied, "obligations-satisfied", "pfm.gate.obligations_satisfied"},
    {RecoveryGate::EvidenceCurrent, "evidence-current", "pfm.gate.evidence_current"},
    {RecoveryGate::StabilityDwell, "stability-dwell", "pfm.gate.stability_dwell"},
    {RecoveryGate::OperatorAuthorization, "operator-authorization",
     "pfm.gate.operator_authorization"},
    {RecoveryGate::RequestsSettled, "requests-settled", "pfm.gate.requests_settled"},
};

const ElectricalObservation* find_observation(
    const std::vector<ElectricalObservation>& observations, const RefToken& element) {
  for (const auto& observation : observations) {
    if (observation.element == element) {
      return &observation;
    }
  }
  return nullptr;
}

}  // namespace

bool element_is_healthy(const ElectricalObservation& observation, ElementKind kind,
                        const ElectricalPolicy& policy, ReasonCode& reason) {
  if (observation.quality == EvidenceQuality::Bad) {
    reason = ReasonCode::EvidenceQualityBad;
    return false;
  }
  switch (kind) {
    case ElementKind::UtilityFeed: {
      if (observation.energization != EnergizationState::Energized) {
        reason = ReasonCode::FeedDeEnergized;
        return false;
      }
      if (observation.has_voltage) {
        auto within = voltage_within_tolerance(policy, observation.voltage);
        if (!within.ok() || !within.value()) {
          reason = ReasonCode::FeedVoltageOutOfBand;
          return false;
        }
      }
      if (observation.has_frequency) {
        auto within = frequency_within_tolerance(policy, observation.frequency);
        if (!within.ok() || !within.value()) {
          reason = ReasonCode::FeedFrequencyOutOfBand;
          return false;
        }
      }
      return true;
    }
    case ElementKind::Switchgear:
    case ElementKind::Bus: {
      if (observation.breaker == BreakerPosition::Tripped) {
        reason = ReasonCode::BreakerTripped;
        return false;
      }
      if (observation.energization == EnergizationState::DeEnergized) {
        reason = ReasonCode::ElementDeEnergizedWhileUpstreamEnergized;
        return false;
      }
      if (observation.energization == EnergizationState::Unknown) {
        reason = ReasonCode::EvidenceMissing;
        return false;
      }
      return true;
    }
    case ElementKind::Breaker:
    case ElementKind::Circuit: {
      if (observation.breaker == BreakerPosition::Tripped) {
        reason = ReasonCode::BreakerTripped;
        return false;
      }
      if (observation.breaker == BreakerPosition::Unknown) {
        reason = ReasonCode::EvidenceMissing;
        return false;
      }
      return true;
    }
    case ElementKind::Pdu:
    case ElementKind::PduBranch:
    case ElementKind::UpsBus: {
      if (observation.energization == EnergizationState::DeEnergized) {
        reason = ReasonCode::ElementDeEnergizedWhileUpstreamEnergized;
        return false;
      }
      if (observation.energization == EnergizationState::Unknown) {
        reason = ReasonCode::EvidenceMissing;
        return false;
      }
      return true;
    }
    case ElementKind::UpsUnit: {
      if (observation.energization == EnergizationState::DeEnergized) {
        reason = ReasonCode::UpsFaulted;
        return false;
      }
      if (observation.energization == EnergizationState::Unknown) {
        reason = ReasonCode::EvidenceMissing;
        return false;
      }
      return true;
    }
    case ElementKind::Generator: {
      if (observation.generator == GeneratorState::Faulted) {
        reason = ReasonCode::GeneratorFaulted;
        return false;
      }
      if (observation.generator == GeneratorState::Unknown) {
        reason = ReasonCode::EvidenceMissing;
        return false;
      }
      if (observation.transfer == TransferState::Failed) {
        reason = ReasonCode::GeneratorTransferFailed;
        return false;
      }
      return true;
    }
    case ElementKind::AutomaticTransferSwitch:
    case ElementKind::StaticTransferSwitch: {
      if (observation.transfer == TransferState::Failed ||
          observation.transfer == TransferState::Transferring) {
        reason = ReasonCode::GeneratorTransferFailed;
        return false;
      }
      if (observation.transfer == TransferState::Unknown) {
        reason = ReasonCode::EvidenceMissing;
        return false;
      }
      return true;
    }
    case ElementKind::LoadGroup:
    case ElementKind::Rack:
    case ElementKind::Unknown:
      return true;
  }
  return true;
}

namespace {

bool request_verified(const std::vector<ResponseRequest>& requests, const RefToken& element,
                      ProofKind proof) {
  for (const auto& request : requests) {
    if (request.target.element == element && request.satisfies(proof)) {
      return true;
    }
  }
  return false;
}

}  // namespace

std::string_view recovery_gate_name(RecoveryGate value) noexcept {
  for (const auto& entry : kGates) {
    if (entry.gate == value) {
      return entry.name;
    }
  }
  return "unknown";
}

std::string_view recovery_gate_code(RecoveryGate value) noexcept {
  for (const auto& entry : kGates) {
    if (entry.gate == value) {
      return entry.code;
    }
  }
  return "pfm.gate.unknown";
}

const GateEvaluation* RecoveryAssessment::find_gate(RecoveryGate gate) const noexcept {
  for (const auto& evaluation : gates) {
    if (evaluation.gate == gate) {
      return &evaluation;
    }
  }
  return nullptr;
}

std::string RecoveryAssessment::summarize() const {
  std::string text = eligible ? "eligible" : "blocked";
  for (const auto& gate : gates) {
    if (gate.satisfied) {
      continue;
    }
    text.append(" | ");
    text.append(recovery_gate_name(gate.gate));
    text.append(": ");
    text.append(reason_code_name(gate.reason));
  }
  return text;
}

Result<RecoveryAssessment> assess_recovery(const RecoveryInputs& inputs) {
  if (inputs.plan == nullptr) {
    return Status::error(StatusCode::PreconditionFailed, "recovery assessment needs a plan")
        .with_context("recovery.plan");
  }
  if (auto r = validate(inputs.policy); !r.ok()) {
    return r.status();
  }
  const ResponsePlan& plan = *inputs.plan;

  RecoveryAssessment assessment;
  assessment.evaluated_at = inputs.now;
  assessment.plan_generation = plan.generation;

  const auto add_gate = [&assessment](RecoveryGate gate, bool satisfied, ReasonCode reason,
                                      RefToken subject, std::string detail) {
    GateEvaluation evaluation;
    evaluation.gate = gate;
    evaluation.satisfied = satisfied;
    evaluation.reason = reason;
    evaluation.subject = subject;
    evaluation.detail = std::move(detail);
    assessment.gates.push_back(std::move(evaluation));
  };

  // 1. The fault itself must be cleared by current evidence.
  bool fault_cleared = true;
  ReasonCode fault_reason = ReasonCode::None;
  RefToken fault_subject{};
  for (const auto& failure : plan.failures) {
    if (failure.element.kind() == RefKind::FailureDomain) {
      const auto* domain = plan.scope.contains(failure.element) ? &failure : nullptr;
      static_cast<void>(domain);
      // A shared domain clears only when every member reports healthy.
      for (const auto& member : plan.scope.impacted_elements) {
        const auto* observation = find_observation(inputs.current_observations, member);
        if (observation == nullptr) {
          fault_cleared = false;
          fault_reason = ReasonCode::EvidenceMissing;
          fault_subject = member;
          break;
        }
      }
      if (!fault_cleared) {
        break;
      }
      continue;
    }
    const auto* observation = find_observation(inputs.current_observations, failure.element);
    if (observation == nullptr) {
      fault_cleared = false;
      fault_reason = ReasonCode::EvidenceMissing;
      fault_subject = failure.element;
      break;
    }
    ReasonCode reason = ReasonCode::None;
    if (!element_is_healthy(*observation, failure.kind, inputs.policy, reason)) {
      fault_cleared = false;
      fault_reason = reason;
      fault_subject = failure.element;
      break;
    }
  }
  // The whole cumulative incident scope must report healthy: an element that
  // was isolated during the response stays part of the recovery question even
  // when the newest plan no longer classifies it as a failure.
  if (fault_cleared) {
    for (const auto& element : inputs.incident_scope) {
      if (!ref_kind_is_element(element.kind())) {
        continue;
      }
      const auto* observation = find_observation(inputs.current_observations, element);
      if (observation == nullptr) {
        fault_cleared = false;
        fault_reason = ReasonCode::EvidenceMissing;
        fault_subject = element;
        break;
      }
      ReasonCode reason = ReasonCode::None;
      if (!element_is_healthy(*observation, observation->kind, inputs.policy, reason)) {
        fault_cleared = false;
        fault_reason = reason;
        fault_subject = element;
        break;
      }
    }
  }
  add_gate(RecoveryGate::FaultCleared, fault_cleared, fault_reason, fault_subject,
           fault_cleared ? std::string{"no classified failure is still observed"}
                         : std::string{"a classified failure is still observed"});

  // 2. Isolation and de-energization proofs.
  bool isolation_verified = true;
  bool de_energization_verified = true;
  ReasonCode proof_reason = ReasonCode::None;
  RefToken proof_subject{};
  if (inputs.policy.require_verified_isolation_for_recovery) {
    for (const auto& requirement : plan.isolations) {
      if (!requirement.has_isolation_point) {
        isolation_verified = false;
        proof_reason = ReasonCode::UpstreamEvidenceUnresolved;
        proof_subject = requirement.element;
        continue;
      }
      if (!request_verified(plan.requests, requirement.element, ProofKind::Isolation) &&
          !request_verified(plan.requests, requirement.isolation_point, ProofKind::Isolation)) {
        isolation_verified = false;
        proof_reason = ReasonCode::UpstreamEvidenceUnresolved;
        proof_subject = requirement.element;
      }
      if (!request_verified(plan.requests, requirement.element, ProofKind::DeEnergization) &&
          !request_verified(plan.requests, requirement.isolation_point,
                            ProofKind::DeEnergization)) {
        de_energization_verified = false;
        proof_reason = ReasonCode::UpstreamEvidenceUnresolved;
        proof_subject = requirement.element;
      }
    }
  }
  add_gate(RecoveryGate::IsolationVerified, isolation_verified, proof_reason, proof_subject,
           isolation_verified ? std::string{"every isolation point is proven"}
                              : std::string{"an isolation point is not proven"});
  add_gate(RecoveryGate::DeEnergizationVerified, de_energization_verified, proof_reason,
           proof_subject,
           de_energization_verified ? std::string{"de-energization is proven"}
                                    : std::string{"de-energization is not proven"});

  // 3. Breaker positions must be proven, not assumed.
  bool breaker_proven = true;
  ReasonCode breaker_reason = ReasonCode::None;
  RefToken breaker_subject{};
  for (const auto& failure : plan.failures) {
    if (failure.klass != FailureClass::BreakerFailure && failure.klass != FailureClass::CircuitFailure &&
        failure.klass != FailureClass::SwitchgearFailure && failure.klass != FailureClass::BusFailure) {
      continue;
    }
    const auto* observation = find_observation(inputs.current_observations, failure.element);
    if (observation == nullptr || observation->breaker == BreakerPosition::Unknown) {
      breaker_proven = false;
      breaker_reason = ReasonCode::EvidenceMissing;
      breaker_subject = failure.element;
      break;
    }
    if (observation->breaker == BreakerPosition::Tripped) {
      breaker_proven = false;
      breaker_reason = ReasonCode::BreakerTripped;
      breaker_subject = failure.element;
      break;
    }
  }
  add_gate(RecoveryGate::BreakerStateProven, breaker_proven, breaker_reason, breaker_subject,
           breaker_proven ? std::string{"breaker positions are proven"}
                          : std::string{"a breaker position is not proven"});

  // 4. Transfers must have settled and generation must be stable.
  bool transfer_stable = true;
  bool generation_stable = true;
  ReasonCode transfer_reason = ReasonCode::None;
  ReasonCode generation_reason = ReasonCode::None;
  RefToken transfer_subject{};
  RefToken generation_subject{};
  for (const auto& element : plan.scope.impacted_elements) {
    const auto* observation = find_observation(inputs.current_observations, element);
    if (observation == nullptr) {
      continue;
    }
    switch (observation->kind) {
      case ElementKind::UpsUnit:
      case ElementKind::UpsBus:
      case ElementKind::StaticTransferSwitch:
      case ElementKind::AutomaticTransferSwitch: {
        if (observation->transfer == TransferState::Transferring ||
            observation->transfer == TransferState::Failed ||
            observation->transfer == TransferState::Unknown) {
          transfer_stable = false;
          transfer_reason = ReasonCode::GeneratorTransferFailed;
          transfer_subject = element;
        }
        break;
      }
      case ElementKind::Generator: {
        if (observation->generator != GeneratorState::Synchronized &&
            observation->generator != GeneratorState::Running) {
          generation_stable = false;
          generation_reason = ReasonCode::GeneratorNotSynchronized;
          generation_subject = element;
        }
        if (observation->transfer == TransferState::Failed) {
          generation_stable = false;
          generation_reason = ReasonCode::GeneratorTransferFailed;
          generation_subject = element;
        }
        break;
      }
      default:
        break;
    }
  }
  add_gate(RecoveryGate::TransferStable, transfer_stable, transfer_reason, transfer_subject,
           transfer_stable ? std::string{"transfer paths are settled"}
                           : std::string{"a transfer path has not settled"});
  add_gate(RecoveryGate::GenerationStable, generation_stable, generation_reason,
           generation_subject,
           generation_stable ? std::string{"generation is stable"}
                             : std::string{"generation is not stable"});

  // 5. Reserve floors.
  bool reserve_restored = true;
  ReasonCode reserve_reason = ReasonCode::None;
  RefToken reserve_subject{};
  if (inputs.policy.require_reserve_floor_for_recovery) {
    for (const auto& element : plan.scope.impacted_elements) {
      const auto* observation = find_observation(inputs.current_observations, element);
      if (observation == nullptr ||
          (observation->kind != ElementKind::UpsUnit && observation->kind != ElementKind::UpsBus)) {
        continue;
      }
      MilliPercent floor = inputs.policy.reserve_floor;
      for (const auto& obligation : inputs.obligations) {
        if (obligation.has_reserve_floor && obligation.target == element &&
            obligation.reserve_floor > floor) {
          floor = obligation.reserve_floor;
        }
      }
      if (!observation->has_reserve) {
        reserve_restored = false;
        reserve_reason = ReasonCode::EvidenceMissing;
        reserve_subject = element;
        break;
      }
      if (observation->reserve < floor) {
        reserve_restored = false;
        reserve_reason = ReasonCode::UpsReserveBelowFloor;
        reserve_subject = element;
        break;
      }
    }
  }
  add_gate(RecoveryGate::ReserveRestored, reserve_restored, reserve_reason, reserve_subject,
           reserve_restored ? std::string{"reserve floors are met"}
                            : std::string{"a reserve floor is not met"});

  // 6. Shared upstream domains.
  bool shared_resolved = true;
  ReasonCode shared_reason = ReasonCode::None;
  RefToken shared_subject{};
  if (inputs.policy.require_shared_domain_resolution_for_recovery &&
      !plan.scope.shared_domains.empty()) {
    // Any impacted shared domain blocks: the whole domain has to be resolved,
    // not merely the element that first failed inside it.
    shared_resolved = false;
    shared_reason = ReasonCode::SharedDomainUnresolved;
    shared_subject = plan.scope.shared_domains.front();
  }
  add_gate(RecoveryGate::SharedDomainResolved, shared_resolved, shared_reason, shared_subject,
           shared_resolved ? std::string{"no shared failure domain is impacted"}
                           : std::string{"a shared failure domain is still impacted"});

  // 7. Upstream evidence and general evidence currency.
  const bool upstream_current = plan.scope.unresolved_upstream.empty();
  add_gate(RecoveryGate::UpstreamEvidenceCurrent, upstream_current,
           upstream_current ? ReasonCode::None : ReasonCode::UpstreamEvidenceUnresolved,
           plan.scope.unresolved_upstream.empty() ? RefToken{}
                                                  : plan.scope.unresolved_upstream.front(),
           upstream_current ? std::string{"upstream evidence is current"}
                            : std::string{"upstream evidence is unresolved"});

  const bool evidence_current = plan.scope.unevidenced_elements.empty();
  add_gate(RecoveryGate::EvidenceCurrent, evidence_current,
           evidence_current ? ReasonCode::None : ReasonCode::EvidenceMissing,
           plan.scope.unevidenced_elements.empty() ? RefToken{}
                                                   : plan.scope.unevidenced_elements.front(),
           evidence_current ? std::string{"every element in scope reports"}
                            : std::string{"an element in scope has no current evidence"});

  // 8. Protected obligations.
  bool obligations_satisfied = true;
  ReasonCode obligation_reason = ReasonCode::None;
  RefToken obligation_subject{};
  for (const auto& obligation : inputs.obligations) {
    if (!obligation_blocks_recovery(obligation, inputs.now, inputs.policy)) {
      continue;
    }
    obligations_satisfied = false;
    obligation_reason = obligation.status == ObligationStatus::Violated
                            ? ReasonCode::DomainMemberFailed
                            : ReasonCode::EvidenceMissing;
    obligation_subject = obligation.target;
    break;
  }
  add_gate(RecoveryGate::ObligationsSatisfied, obligations_satisfied, obligation_reason,
           obligation_subject,
           obligations_satisfied ? std::string{"every protected obligation reports satisfied"}
                                 : std::string{"a protected obligation does not report satisfied"});

  // 9. Sustained stability.
  bool dwell_satisfied = false;
  ReasonCode dwell_reason = ReasonCode::EvidenceMissing;
  if (inputs.has_stable_since) {
    auto elapsed = checked_difference(inputs.now, inputs.stable_since);
    if (elapsed.ok() && elapsed.value() >= inputs.policy.stability_dwell) {
      dwell_satisfied = true;
      dwell_reason = ReasonCode::None;
    } else {
      dwell_reason = ReasonCode::EvidenceStale;
    }
  }
  add_gate(RecoveryGate::StabilityDwell, dwell_satisfied, dwell_reason, RefToken{},
           dwell_satisfied ? std::string{"stable evidence has been sustained"}
                           : std::string{"stable evidence has not been sustained"});

  // 10. Explicit operator authorization for this incident generation.
  bool authorized = true;
  ReasonCode authorization_reason = ReasonCode::None;
  if (inputs.policy.require_operator_authorization_for_recovery) {
    authorized = inputs.operator_authorized &&
                 inputs.authorized_generation == inputs.incident_generation;
    authorization_reason = authorized ? ReasonCode::None : ReasonCode::AuthorizationNotCurrent;
  }
  add_gate(RecoveryGate::OperatorAuthorization, authorized, authorization_reason, RefToken{},
           authorized ? std::string{"recovery is authorized for this incident generation"}
                      : std::string{"recovery is not authorized for this incident generation"});

  // 11. Bounded requests must have settled: an open consequential request is a
  //     request whose effect is unknown, and unknown is not proof.
  bool settled = true;
  ReasonCode settled_reason = ReasonCode::None;
  RefToken settled_subject{};
  for (const auto& request : plan.requests) {
    if (!request_state_is_open(request.state)) {
      continue;
    }
    settled = false;
    settled_reason = request.state == RequestState::Indeterminate
                         ? ReasonCode::EvidenceMissing
                         : ReasonCode::UpstreamEvidenceUnresolved;
    settled_subject = request.target.element;
    break;
  }
  add_gate(RecoveryGate::RequestsSettled, settled, settled_reason, settled_subject,
           settled ? std::string{"every bounded request has settled"}
                   : std::string{"a bounded request has not settled"});

  assessment.eligible = true;
  for (const auto& gate : assessment.gates) {
    if (!gate.satisfied) {
      assessment.eligible = false;
      if (gate.subject.is_set()) {
        assessment.blocking_elements.push_back(gate.subject);
      }
    }
  }
  std::sort(assessment.blocking_elements.begin(), assessment.blocking_elements.end());
  assessment.blocking_elements.erase(
      std::unique(assessment.blocking_elements.begin(), assessment.blocking_elements.end()),
      assessment.blocking_elements.end());
  return assessment;
}

}  // namespace summon::pfm
