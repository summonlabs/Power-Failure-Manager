// Power Failure Manager -- deterministic response planning.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0 (see LICENSE).
#pragma once

#include <vector>

#include "pfm/classification.hpp"
#include "pfm/ids.hpp"
#include "pfm/obligations.hpp"
#include "pfm/policy.hpp"
#include "pfm/request.hpp"
#include "pfm/scope.hpp"

namespace summon::pfm {

// An element that must be isolated, and the point that isolates it. A
// requirement with no isolation point is not dropped: it is planned as an
// unachievable isolation, which blocks recovery and is visible in the trace.
struct IsolationRequirement {
  RefToken element{};
  RefToken isolation_point{};
  bool has_isolation_point{false};
  ProofKind required_proof{ProofKind::DeEnergization};
  FailureClass failure{FailureClass::None};
  ReasonCode reason{ReasonCode::None};
};

// A protected element that must remain supplied or held above a reserve floor.
struct ProtectionRequirement {
  RefToken element{};
  ProtectionClass protection{ProtectionClass::ServiceLevel};
  bool has_obligation{false};
  ObligationId obligation{};
  bool has_reserve_floor{false};
  MilliPercent reserve_floor{};
  ReasonCode reason{ReasonCode::None};
};

// The plan is the decision record: what PFM concluded, what must be isolated
// or protected, and which bounded requests are justified. It is bound to the
// evidence generation, topology generation, policy generation, incident
// generation, and control epoch it was computed from, and is fenced as a whole.
struct ResponsePlan {
  PlanGeneration generation{};
  IncidentId incident{};
  IncidentGeneration incident_generation{};
  TopologyGeneration topology_generation{};
  PolicyGeneration policy_generation{};
  ControlEpoch epoch{};
  Timestamp decided_at{};

  FailureClass primary_failure{FailureClass::None};
  RefToken primary_element{};
  std::vector<ClassifiedFailure> failures{};
  std::vector<ReasonStep> reasons{};
  AffectedScope scope{};

  std::vector<IsolationRequirement> isolations{};
  std::vector<ProtectionRequirement> protections{};
  std::vector<ResponseRequest> requests{};

  Fingerprint fingerprint{};

  [[nodiscard]] bool has_failure() const noexcept { return primary_failure != FailureClass::None; }
  [[nodiscard]] const ResponseRequest* find_request(ResponseRequestId id) const noexcept;
};

// Everything the planner is allowed to see. The planner performs no I/O, holds
// no lock, and reads no clock of its own.
struct PlanInputs {
  IncidentId incident{};
  IncidentGeneration incident_generation{};
  ControlEpoch epoch{};
  StateRevision revision{};
  PlanGeneration next_plan_generation{};
  Timestamp now{};
  ElectricalPolicy policy{};
  Bounds bounds{};
  const TopologyIndex* topology{nullptr};
  std::vector<ElectricalObservation> current_observations{};
  std::vector<ProtectedObligation> obligations{};
  // Requests still open from previous plans. They are carried forward as
  // existing operations, never re-planned as new ones.
  std::vector<ResponseRequest> open_requests{};
  // Elements that have reported before and whose evidence is no longer
  // current. They are inside the boundary and unresolved, never healthy.
  std::vector<RefToken> unevidenced_elements{};
  // Supply elements inside the cumulative incident scope that current evidence
  // still reports as unhealthy or de-energized. They justify bounded
  // restoration requests; they are never assumed to have recovered.
  std::vector<RefToken> unhealthy_scope_elements{};
  // Identity allocation is an input, so that a plan is reproducible from the
  // state it was computed from and never depends on a hidden counter.
  ResponseRequestId first_request_id{};
  AttemptId first_attempt_id{};
};

// Builds the plan. The result is fully deterministic: same inputs, same plan,
// byte for byte, including identifiers and ordering.
[[nodiscard]] Result<ResponsePlan> plan_response(const PlanInputs& inputs);

// Stable ordering applied to every planned list: request kind first, then
// owner, then target reference, then magnitude.
void sort_requests(std::vector<ResponseRequest>& requests);

}  // namespace summon::pfm
