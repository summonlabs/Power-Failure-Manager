// Power Failure Manager -- recovery gates and eligibility.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0 (see LICENSE).
#pragma once

#include <string>
#include <vector>

#include "pfm/obligations.hpp"
#include "pfm/plan.hpp"
#include "pfm/request.hpp"

namespace summon::pfm {

// Recovery is gated, not implied. A gate is satisfied only by current,
// independent evidence, never by an elapsed time, an acknowledgement, an
// earlier success, or a locally healthy component.
enum class RecoveryGate : std::uint8_t {
  FaultCleared = 0,
  IsolationVerified = 1,
  DeEnergizationVerified = 2,
  BreakerStateProven = 3,
  TransferStable = 4,
  GenerationStable = 5,
  ReserveRestored = 6,
  SharedDomainResolved = 7,
  UpstreamEvidenceCurrent = 8,
  ObligationsSatisfied = 9,
  EvidenceCurrent = 10,
  StabilityDwell = 11,
  OperatorAuthorization = 12,
  RequestsSettled = 13,
};

[[nodiscard]] std::string_view recovery_gate_name(RecoveryGate value) noexcept;
[[nodiscard]] std::string_view recovery_gate_code(RecoveryGate value) noexcept;

struct GateEvaluation {
  RecoveryGate gate{RecoveryGate::FaultCleared};
  bool satisfied{false};
  ReasonCode reason{ReasonCode::None};
  RefToken subject{};
  std::string detail{};
};

struct RecoveryAssessment {
  bool eligible{false};
  Timestamp evaluated_at{};
  PlanGeneration plan_generation{};
  std::vector<GateEvaluation> gates{};
  std::vector<RefToken> blocking_elements{};

  [[nodiscard]] const GateEvaluation* find_gate(RecoveryGate gate) const noexcept;
  [[nodiscard]] std::string summarize() const;
};

// Whether a current observation reports an element as healthy for its kind.
// "reason" receives the code explaining the first unhealthy fact, so a gate can
// report why it is blocked rather than only that it is.
[[nodiscard]] bool element_is_healthy(const ElectricalObservation& observation, ElementKind kind,
                                      const ElectricalPolicy& policy, ReasonCode& reason);

struct RecoveryInputs {
  Timestamp now{};
  ElectricalPolicy policy{};
  Bounds bounds{};
  const ResponsePlan* plan{nullptr};
  // Every element the incident has touched, from the durable incident
  // projection. Recovery is assessed against this cumulative scope.
  std::vector<RefToken> incident_scope{};
  std::vector<ElectricalObservation> current_observations{};
  std::vector<ProtectedObligation> obligations{};
  // Instant from which the current, uninterrupted run of stable evidence began,
  // or an unset timestamp when there is no such run.
  bool has_stable_since{false};
  Timestamp stable_since{};
  // Explicit operator authorization for this incident generation.
  bool operator_authorized{false};
  Timestamp authorized_at{};
  IncidentGeneration authorized_generation{};
  IncidentGeneration incident_generation{};
};

// Evaluates every gate. A gate that cannot be evaluated for want of current
// evidence is unsatisfied, with the reason saying so.
[[nodiscard]] Result<RecoveryAssessment> assess_recovery(const RecoveryInputs& inputs);

}  // namespace summon::pfm
