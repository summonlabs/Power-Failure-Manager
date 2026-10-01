// Power Failure Manager -- durable domain state and its single mutator.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0 (see LICENSE).
#include "pfm/state.hpp"

#include <algorithm>
#include <map>
#include <string>

namespace summon::pfm {
namespace {

Status stale(StatusCode code, std::string message, std::string context) {
  return Status::error(code, std::move(message)).with_context(std::move(context));
}

bool slot_less(const ObservationSlot& a, const ObservationSlot& b) noexcept {
  return a.element < b.element;
}

bool request_slot_less(const RequestSlot& a, const RequestSlot& b) noexcept {
  return a.request.id < b.request.id;
}

bool obligation_slot_less(const ObligationSlot& a, const ObligationSlot& b) noexcept {
  return a.obligation.id < b.obligation.id;
}

}  // namespace

std::string_view incident_lifecycle_name(IncidentLifecycle value) noexcept {
  switch (value) {
    case IncidentLifecycle::None: return "none";
    case IncidentLifecycle::Active: return "active";
    case IncidentLifecycle::Isolating: return "isolating";
    case IncidentLifecycle::Stabilizing: return "stabilizing";
    case IncidentLifecycle::Recovering: return "recovering";
    case IncidentLifecycle::Recovered: return "recovered";
    case IncidentLifecycle::Closed: return "closed";
  }
  return "unknown";
}

bool incident_lifecycle_transition_legal(IncidentLifecycle from, IncidentLifecycle to) noexcept {
  if (from == to) {
    return false;
  }
  switch (from) {
    case IncidentLifecycle::None:
      return to == IncidentLifecycle::Active;
    case IncidentLifecycle::Active:
      return to == IncidentLifecycle::Isolating || to == IncidentLifecycle::Stabilizing ||
             to == IncidentLifecycle::Closed;
    case IncidentLifecycle::Isolating:
      return to == IncidentLifecycle::Stabilizing || to == IncidentLifecycle::Active ||
             to == IncidentLifecycle::Closed;
    case IncidentLifecycle::Stabilizing:
      // A failure that appears while the incident is stabilizing puts it back
      // into isolation: the decision that produced this transition is a plan
      // that carries failures and still requires isolation work.
      return to == IncidentLifecycle::Recovering || to == IncidentLifecycle::Active ||
             to == IncidentLifecycle::Isolating || to == IncidentLifecycle::Closed;
    case IncidentLifecycle::Recovering:
      // A new failure returns the incident to active response, whether or not
      // isolation is required for it.
      return to == IncidentLifecycle::Recovered || to == IncidentLifecycle::Stabilizing ||
             to == IncidentLifecycle::Active || to == IncidentLifecycle::Isolating ||
             to == IncidentLifecycle::Closed;
    case IncidentLifecycle::Recovered:
      return to == IncidentLifecycle::Closed || to == IncidentLifecycle::Active ||
             to == IncidentLifecycle::Isolating;
    case IncidentLifecycle::Closed:
      return false;
  }
  return false;
}

const ObservationSlot* DomainState::find_observation(const RefToken& element) const noexcept {
  for (const auto& slot : observations) {
    if (slot.element == element) {
      return &slot;
    }
  }
  return nullptr;
}

const RequestSlot* DomainState::find_request(ResponseRequestId id) const noexcept {
  for (const auto& slot : requests) {
    if (slot.request.id == id) {
      return &slot;
    }
  }
  return nullptr;
}

RequestSlot* DomainState::find_request(ResponseRequestId id) noexcept {
  for (auto& slot : requests) {
    if (slot.request.id == id) {
      return &slot;
    }
  }
  return nullptr;
}

const ObligationSlot* DomainState::find_obligation(ObligationId id) const noexcept {
  for (const auto& slot : obligations) {
    if (slot.obligation.id == id) {
      return &slot;
    }
  }
  return nullptr;
}

AuthorityExpectation expectation_of(const DomainState& state) {
  AuthorityExpectation expected;
  expected.incident_live = state.incident.lifecycle != IncidentLifecycle::None &&
                           state.incident.lifecycle != IncidentLifecycle::Closed;
  expected.incident = state.incident.incident;
  expected.generation = state.incident.generation;
  expected.epoch = state.epoch;
  expected.incarnation = state.incarnation;
  expected.revision = state.revision;
  return expected;
}

Result<void> validate(const DomainState& state, const Bounds& bounds) {
  if (auto r = pfm::validate(bounds); !r.ok()) {
    return r.status();
  }
  if (auto r = pfm::validate(state.policy); !r.ok()) {
    return r.status();
  }
  if (state.revision.is_absent()) {
    return stale(StatusCode::InvalidArgument, "state has no revision", "state.revision");
  }
  if (state.epoch.is_absent() || state.incarnation.is_absent()) {
    return stale(StatusCode::MissingAuthority, "state has no control epoch or incarnation",
                 "state.authority");
  }
  if (state.observations.size() > bounds.max_observations) {
    return stale(StatusCode::BoundsExceeded, "observation slots exceed the bound",
                 "state.observations");
  }
  if (state.requests.size() > bounds.max_attempts_retained) {
    return stale(StatusCode::BoundsExceeded, "request slots exceed the bound", "state.requests");
  }
  if (state.obligations.size() > bounds.max_obligations) {
    return stale(StatusCode::BoundsExceeded, "obligation slots exceed the bound",
                 "state.obligations");
  }
  if (state.transitions.size() > bounds.max_transitions) {
    return stale(StatusCode::BoundsExceeded, "transition history exceeds the bound",
                 "state.transitions");
  }
  for (std::size_t i = 1; i < state.observations.size(); ++i) {
    if (!(state.observations[i - 1].element < state.observations[i].element)) {
      return stale(StatusCode::DuplicateElement,
                   "observation slots are not in canonical order or contain a duplicate",
                   "state.observations");
    }
  }
  for (std::size_t i = 1; i < state.requests.size(); ++i) {
    if (!(state.requests[i - 1].request.id < state.requests[i].request.id)) {
      return stale(StatusCode::DuplicateElement,
                   "request slots are not in canonical order or contain a duplicate",
                   "state.requests");
    }
  }
  for (std::size_t i = 1; i < state.obligations.size(); ++i) {
    if (!(state.obligations[i - 1].obligation.id < state.obligations[i].obligation.id)) {
      return stale(StatusCode::DuplicateElement,
                   "obligation slots are not in canonical order or contain a duplicate",
                   "state.obligations");
    }
  }
  for (const auto& slot : state.observations) {
    if (!slot.present) {
      continue;
    }
    if (auto r = pfm::validate(slot.observation, bounds); !r.ok()) {
      return r.status();
    }
  }
  for (const auto& slot : state.requests) {
    if (auto r = pfm::validate(slot.request, bounds); !r.ok()) {
      return r.status();
    }
  }
  if (state.has_plan) {
    if (state.plan.generation.is_absent()) {
      return stale(StatusCode::InvalidArgument, "the published plan has no generation",
                   "state.plan");
    }
    if (state.plan.requests.size() > bounds.max_requests_per_plan) {
      return stale(StatusCode::BoundsExceeded, "the published plan exceeds the request bound",
                   "state.plan");
    }
    for (const auto& request : state.plan.requests) {
      if (auto r = pfm::validate(request, bounds); !r.ok()) {
        return r.status();
      }
      if (state.find_request(request.id) == nullptr) {
        return stale(StatusCode::RequestNotFound,
                     "the published plan names a request that is not retained",
                     "state.plan.requests");
      }
    }
    if (state.incident.plan_generation.value() != state.plan.generation.value()) {
      return stale(StatusCode::PlanNotCurrent,
                   "the incident projection does not agree with the published plan",
                   "state.plan");
    }
  }
  if (state.has_topology) {
    if (auto r = pfm::validate(state.topology, bounds); !r.ok()) {
      return r.status();
    }
    if (state.topology.generation.value() != state.topology_generation.value()) {
      return stale(StatusCode::StaleGeneration,
                   "the retained topology does not match the topology generation",
                   "state.topology");
    }
  }
  return {};
}

std::vector<ElectricalObservation> current_observations(const DomainState& state, Timestamp now,
                                                        const ElectricalPolicy& policy) {
  std::vector<ElectricalObservation> result;
  for (const auto& slot : state.observations) {
    if (!slot.present || slot.recovered || slot.contradicted) {
      continue;
    }
    if (classify_freshness(slot.observation.observed_at, now, policy) != Freshness::Fresh) {
      continue;
    }
    result.push_back(slot.observation);
  }
  return result;
}

void refresh_currency(DomainState& state, Timestamp now) {
  for (auto& slot : state.observations) {
    if (!slot.present || slot.recovered) {
      slot.freshness = Freshness::Unknown;
      continue;
    }
    slot.freshness = classify_freshness(slot.observation.observed_at, now, state.policy);
  }
  for (auto& slot : state.obligations) {
    slot.report_current = !obligation_blocks_recovery(slot.obligation, now, state.policy);
  }
}

Result<void> apply_journal_entry(DomainState& state, const JournalEntry& entry) {
  if (auto r = validate(entry, state.bounds); !r.ok()) {
    return r.status();
  }
  if (!(entry.revision == StateRevision::from_value(state.revision.value() + 1))) {
    return stale(StatusCode::EvidenceOutOfOrder,
                 std::string{"journal entry "} + std::string{journal_kind_name(entry.kind)} +
                     " carries revision " + std::to_string(entry.revision.value()) +
                     " but the state is at revision " + std::to_string(state.revision.value()),
                 "state.revision");
  }

  const auto touch = [&state, &entry]() {
    state.revision = entry.revision;
    state.incident.updated_at = entry.recorded_at;
    TransitionRecord record;
    record.sequence = TransitionSequence::from_value(state.incident.transitions.value() + 1);
    record.recorded_at = entry.recorded_at;
    record.kind = entry.kind;
    record.from_revision = StateRevision::from_value(entry.revision.value() - 1);
    record.to_revision = entry.revision;
    record.reason = entry.payload.reason;
    record.subject = entry.payload.source;
    state.incident.transitions = record.sequence;
    state.transitions.push_back(record);
    if (state.transitions.size() > state.bounds.max_transitions) {
      state.transitions.erase(state.transitions.begin(),
                              state.transitions.begin() +
                                  static_cast<std::ptrdiff_t>(state.transitions.size() -
                                                              state.bounds.max_transitions));
    }
  };

  const auto apply_request_transition =
      [&state, &entry](RequestState to) -> Result<void> {
    auto* slot = state.find_request(entry.payload.request);
    if (slot == nullptr) {
      return stale(StatusCode::RequestNotFound, "journal entry names an unknown request",
                   "request.id");
    }
    const RequestState from = slot->request.state;
    if (entry.payload.has_request_state && !(entry.payload.from_state == from)) {
      return stale(StatusCode::RequestStateConflict,
                   std::string{"journal entry "} + std::string{journal_kind_name(entry.kind)} +
                       " expects request " + std::to_string(entry.payload.request.value()) +
                       " in state " + std::string{request_state_name(entry.payload.from_state)} +
                       " but it is in " + std::string{request_state_name(from)},
                   "request.state");
    }
    if (!request_state_transition_legal(from, to)) {
      return stale(StatusCode::RequestStateConflict,
                   std::string{"illegal request transition from "} +
                       std::string{request_state_name(from)} + " to " +
                       std::string{request_state_name(to)},
                   "request.state");
    }
    slot->request.state = to;
    if (entry.payload.attempt.is_set()) {
      slot->request.attempt = entry.payload.attempt;
    }
    if (entry.payload.source.is_set()) {
      slot->request.verification_source = entry.payload.source;
    }
    if (entry.payload.evidence.is_set()) {
      slot->request.verification_evidence = entry.payload.evidence;
      slot->request.verification_observed_at = entry.recorded_at;
      slot->request.evidence.push_back(entry.payload.evidence);
      if (slot->request.evidence.size() > state.bounds.max_reason_evidence_refs) {
        slot->request.evidence.erase(
            slot->request.evidence.begin(),
            slot->request.evidence.begin() +
                static_cast<std::ptrdiff_t>(slot->request.evidence.size() -
                                            state.bounds.max_reason_evidence_refs));
      }
    }
    if (!entry.payload.note.empty()) {
      slot->request.owner_note = entry.payload.note;
    }
    switch (to) {
      case RequestState::Issued:
        slot->request.has_issued_at = true;
        slot->request.issued_at = entry.recorded_at;
        break;
      case RequestState::Acknowledged:
        slot->request.has_acknowledged_at = true;
        slot->request.acknowledged_at = entry.recorded_at;
        break;
      case RequestState::Observed:
        slot->request.has_observed_at = true;
        slot->request.observed_at = entry.recorded_at;
        break;
      case RequestState::Verified:
        slot->request.has_verified_at = true;
        slot->request.verified_at = entry.recorded_at;
        break;
      case RequestState::Superseded:
        slot->request.superseded_by = entry.payload.attempt;
        slot->request.terminal_reason = entry.payload.reason;
        break;
      default:
        slot->request.terminal_reason = entry.payload.reason;
        break;
    }
    // The published plan keeps its decisions but its requests follow the live
    // lifecycle, so recovery gates read current request states rather than the
    // states they happened to hold when the plan was computed.
    for (auto& planned : state.plan.requests) {
      if (planned.id == slot->request.id) {
        planned = slot->request;
      }
    }
    return {};
  };

  switch (entry.kind) {
    case JournalKind::None:
      return stale(StatusCode::InvalidArgument, "journal entry has no kind", "journal.kind");

    case JournalKind::IncidentOpened: {
      if (!entry.payload.has_incident) {
        return stale(StatusCode::InvalidArgument, "incident open entry carries no identity",
                     "journal.payload");
      }
      if (state.incident.lifecycle == IncidentLifecycle::None) {
        state.incident.incident = entry.payload.incident;
        state.incident.generation = entry.payload.incident_generation;
        state.incident.opened_at = entry.recorded_at;
        state.incident.impacted_scope.clear();
      } else if (!(state.incident.incident == entry.payload.incident)) {
        // A later incident gets a new identity and a new generation.
        state.incident = IncidentProjection{};
        state.incident.incident = entry.payload.incident;
        state.incident.generation = entry.payload.incident_generation;
        state.incident.opened_at = entry.recorded_at;
      } else {
        return stale(StatusCode::IncidentAlreadyOpen, "the incident is already open",
                     "incident.id");
      }
      state.incident.lifecycle = IncidentLifecycle::Active;
      state.has_plan = false;
      state.plan = ResponsePlan{};
      break;
    }

    case JournalKind::IncidentClosed: {
      if (state.incident.lifecycle == IncidentLifecycle::None) {
        return stale(StatusCode::NoActiveIncident, "no incident is live", "incident.lifecycle");
      }
      if (!incident_lifecycle_transition_legal(state.incident.lifecycle,
                                               IncidentLifecycle::Closed)) {
        return stale(StatusCode::IllegalTransition,
                     "the incident lifecycle does not allow closure from its current state",
                     "incident.lifecycle");
      }
      state.incident.lifecycle = IncidentLifecycle::Closed;
      break;
    }

    case JournalKind::EvidenceAdmitted: {
      if (!entry.payload.has_observation) {
        return stale(StatusCode::InvalidArgument, "evidence entry carries no observation",
                     "journal.payload");
      }
      const auto& observation = entry.payload.observation;
      auto found = std::find_if(state.observations.begin(), state.observations.end(),
                                [&observation](const ObservationSlot& slot) {
                                  return slot.element == observation.element;
                                });
      if (found == state.observations.end()) {
        if (state.observations.size() >= state.bounds.max_observations) {
          return stale(StatusCode::BoundsExceeded, "observation set exceeds the bound",
                       "state.observations");
        }
        ObservationSlot slot;
        slot.element = observation.element;
        slot.kind = observation.kind;
        slot.observation = observation;
        slot.present = true;
        slot.recovered = false;
        state.observations.push_back(slot);
      } else {
        if (found->observation.sequence > observation.sequence) {
          return stale(StatusCode::EvidenceOutOfOrder,
                       "observation sequence is older than the retained observation",
                       observation.element.to_string());
        }
        if (found->present && !found->recovered &&
            observations_conflict(found->observation, observation, state.policy)) {
          found->contradicted = true;
          found->conflict_evidence.push_back(observation.evidence_fingerprint);
          if (found->conflict_evidence.size() > state.bounds.max_reason_evidence_refs) {
            found->conflict_evidence.erase(
                found->conflict_evidence.begin(),
                found->conflict_evidence.begin() +
                    static_cast<std::ptrdiff_t>(found->conflict_evidence.size() -
                                                state.bounds.max_reason_evidence_refs));
          }
        } else if (found->contradicted &&
                   found->observation.sequence == observation.sequence) {
          found->contradicted = false;
          found->conflict_evidence.clear();
          found->observation = observation;
        } else {
          found->observation = observation;
          found->contradicted = false;
          found->conflict_evidence.clear();
        }
        found->present = true;
        found->recovered = false;
        found->kind = observation.kind;
      }
      std::sort(state.observations.begin(), state.observations.end(), slot_less);
      break;
    }

    case JournalKind::EvidenceInvalidated: {
      if (!entry.payload.has_observation) {
        return stale(StatusCode::InvalidArgument, "evidence entry carries no observation",
                     "journal.payload");
      }
      const auto& observation = entry.payload.observation;
      auto found = std::find_if(state.observations.begin(), state.observations.end(),
                                [&observation](const ObservationSlot& slot) {
                                  return slot.element == observation.element;
                                });
      if (found == state.observations.end()) {
        return stale(StatusCode::EvidenceUnknownTarget,
                     "no retained observation for the element being invalidated",
                     observation.element.to_string());
      }
      found->present = false;
      found->recovered = false;
      found->contradicted = false;
      found->conflict_evidence.clear();
      found->freshness = Freshness::Unknown;
      break;
    }

    case JournalKind::TopologyPublished: {
      if (!entry.payload.has_topology) {
        return stale(StatusCode::InvalidArgument, "topology entry carries no snapshot",
                     "journal.payload");
      }
      if (auto r = validate(entry.payload.topology, state.bounds); !r.ok()) {
        return r.status();
      }
      state.topology = entry.payload.topology;
      canonicalise(state.topology);
      state.has_topology = true;
      state.topology_generation = state.topology.generation;
      break;
    }

    case JournalKind::PolicyPublished: {
      if (!entry.payload.has_policy) {
        return stale(StatusCode::InvalidArgument, "policy entry carries no policy",
                     "journal.payload");
      }
      if (auto r = validate(entry.payload.policy); !r.ok()) {
        return r.status();
      }
      state.policy = entry.payload.policy;
      state.policy_generation = state.policy.generation;
      break;
    }

    case JournalKind::ObligationsPublished: {
      if (!entry.payload.has_obligations) {
        return stale(StatusCode::InvalidArgument, "obligation entry carries no obligations",
                     "journal.payload");
      }
      if (entry.payload.obligations.size() > state.bounds.max_obligations) {
        return stale(StatusCode::BoundsExceeded, "obligation set exceeds the bound",
                     "state.obligations");
      }
      std::vector<ObligationSlot> slots;
      slots.reserve(entry.payload.obligations.size());
      for (const auto& obligation : entry.payload.obligations) {
        if (auto r = validate(obligation, state.bounds); !r.ok()) {
          return r.status();
        }
        ObligationSlot slot;
        slot.obligation = obligation;
        slots.push_back(std::move(slot));
      }
      std::sort(slots.begin(), slots.end(), obligation_slot_less);
      if (std::adjacent_find(slots.begin(), slots.end(),
                             [](const ObligationSlot& a, const ObligationSlot& b) {
                               return a.obligation.id == b.obligation.id;
                             }) != slots.end()) {
        return stale(StatusCode::DuplicateElement, "obligation set contains a duplicate identity",
                     "state.obligations");
      }
      state.obligations = std::move(slots);
      break;
    }

    case JournalKind::PlanComputed: {
      if (!entry.payload.has_plan) {
        return stale(StatusCode::InvalidArgument, "plan entry carries no plan", "journal.payload");
      }
      if (state.incident.lifecycle == IncidentLifecycle::None ||
          state.incident.lifecycle == IncidentLifecycle::Closed) {
        return stale(StatusCode::NoActiveIncident, "a plan requires a live incident",
                     "incident.lifecycle");
      }
      state.plan = entry.payload.plan;
      state.has_plan = true;
      state.next_plan_generation =
          PlanGeneration::from_value(state.plan.generation.value() + 1);
      state.incident.has_plan = true;
      state.incident.plan_generation = state.plan.generation;
      state.incident.plan_fingerprint = state.plan.fingerprint;
      state.incident.primary_failure = state.plan.primary_failure;
      state.incident.primary_element = state.plan.primary_element;
      for (const auto& element : state.plan.scope.impacted_elements) {
        state.incident.impacted_scope.push_back(element);
      }
      for (const auto& element : state.plan.scope.failed_elements) {
        state.incident.impacted_scope.push_back(element);
      }
      std::sort(state.incident.impacted_scope.begin(), state.incident.impacted_scope.end());
      state.incident.impacted_scope.erase(
          std::unique(state.incident.impacted_scope.begin(), state.incident.impacted_scope.end()),
          state.incident.impacted_scope.end());
      if (state.incident.impacted_scope.size() > state.bounds.max_scope_elements) {
        state.incident.impacted_scope.resize(state.bounds.max_scope_elements);
      }
      // The lifecycle follows the decision: a plan carrying failures means the
      // incident is active response, isolation in progress when isolation is
      // required, and stabilization once no failure remains. Recovery is still
      // entered only by an explicit, authorized request.
      IncidentLifecycle target = state.incident.lifecycle;
      if (state.plan.has_failure()) {
        target = state.plan.isolations.empty() ? IncidentLifecycle::Active
                                               : IncidentLifecycle::Isolating;
        state.incident.has_stable_since = false;
      } else if (state.incident.lifecycle == IncidentLifecycle::Active ||
                 state.incident.lifecycle == IncidentLifecycle::Isolating) {
        target = IncidentLifecycle::Stabilizing;
      }
      if (target != state.incident.lifecycle &&
          incident_lifecycle_transition_legal(state.incident.lifecycle, target)) {
        state.incident.lifecycle = target;
      }
      for (const auto& request : state.plan.requests) {
        if (state.next_request_id <= request.id) {
          state.next_request_id = ResponseRequestId::from_value(request.id.value() + 1);
        }
        if (state.next_attempt_id <= request.attempt) {
          state.next_attempt_id = AttemptId::from_value(request.attempt.value() + 1);
        }
        auto* slot = state.find_request(request.id);
        if (slot == nullptr) {
          if (state.requests.size() >= state.bounds.max_attempts_retained) {
            return stale(StatusCode::BoundsExceeded, "request history exceeds the bound",
                         "state.requests");
          }
          RequestSlot fresh;
          fresh.request = request;
          state.requests.push_back(std::move(fresh));
        } else {
          slot->request.plan_generation = request.plan_generation;
        }
      }
      std::sort(state.requests.begin(), state.requests.end(), request_slot_less);
      break;
    }

    // Request transitions advance the state revision like every other entry, so
    // each case falls through to the shared bookkeeping at the end rather than
    // returning early.
    case JournalKind::RequestIssued:
    case JournalKind::RequestAcknowledged:
    case JournalKind::RequestObserved:
    case JournalKind::RequestVerified:
    case JournalKind::RequestFailed:
    case JournalKind::RequestRefused:
    case JournalKind::RequestAbandoned:
    case JournalKind::RequestIndeterminate:
    case JournalKind::RequestResolved: {
      RequestState target = RequestState::Planned;
      switch (entry.kind) {
        case JournalKind::RequestIssued: target = RequestState::Issued; break;
        case JournalKind::RequestAcknowledged: target = RequestState::Acknowledged; break;
        case JournalKind::RequestObserved: target = RequestState::Observed; break;
        case JournalKind::RequestVerified: target = RequestState::Verified; break;
        case JournalKind::RequestFailed: target = RequestState::Failed; break;
        case JournalKind::RequestRefused: target = RequestState::Refused; break;
        case JournalKind::RequestAbandoned: target = RequestState::Abandoned; break;
        case JournalKind::RequestIndeterminate: target = RequestState::Indeterminate; break;
        case JournalKind::RequestResolved: {
          if (!entry.payload.has_request_state) {
            return stale(StatusCode::InvalidArgument,
                         "resolved request entry carries no target state", "journal.payload");
          }
          target = entry.payload.to_state;
          break;
        }
        default:
          break;
      }
      if (auto r = apply_request_transition(target); !r.ok()) {
        return r;
      }
      break;
    }

    case JournalKind::RequestSuperseded: {
      if (auto r = apply_request_transition(RequestState::Superseded); !r.ok()) {
        return r;
      }
      auto* slot = state.find_request(entry.payload.request);
      if (slot != nullptr && entry.payload.attempt.is_set()) {
        slot->request.superseded_by = entry.payload.attempt;
      }
      break;
    }

    case JournalKind::RecoveryAuthorized: {
      if (!entry.payload.has_recovery_authorization) {
        return stale(StatusCode::InvalidArgument, "authorization entry carries no instant",
                     "journal.payload");
      }
      state.incident.operator_authorized = true;
      state.incident.authorized_at = entry.payload.authorized_at;
      state.incident.authorized_generation = entry.payload.incident_generation;
      break;
    }

    case JournalKind::RecoveryStarted: {
      if (!incident_lifecycle_transition_legal(state.incident.lifecycle,
                                               IncidentLifecycle::Recovering)) {
        return stale(StatusCode::IllegalTransition,
                     "the incident lifecycle does not allow entering recovery",
                     "incident.lifecycle");
      }
      state.incident.lifecycle = IncidentLifecycle::Recovering;
      break;
    }

    case JournalKind::RecoveryCompleted: {
      if (!incident_lifecycle_transition_legal(state.incident.lifecycle,
                                               IncidentLifecycle::Recovered)) {
        return stale(StatusCode::IllegalTransition,
                     "the incident lifecycle does not allow completing recovery",
                     "incident.lifecycle");
      }
      state.incident.lifecycle = IncidentLifecycle::Recovered;
      state.incident.recovery_steps += 1;
      break;
    }

    case JournalKind::ObligationRelaxed: {
      if (!entry.payload.has_obligation) {
        return stale(StatusCode::InvalidArgument, "relaxation entry carries no obligation",
                     "journal.payload");
      }
      const auto* slot = state.find_obligation(entry.payload.obligation);
      if (slot == nullptr) {
        return stale(StatusCode::ObligationUnknown, "no such protected obligation",
                     "obligation.id");
      }
      if (!obligation_relaxable(slot->obligation.protection)) {
        return stale(StatusCode::ObligationNotRelaxable,
                     "the protection class may never be relaxed by response authority",
                     "obligation.protection");
      }
      for (const auto& record : state.relaxations) {
        if (record.obligation == entry.payload.obligation &&
            record.incident == state.incident.incident) {
          return stale(StatusCode::ObligationAlreadyRelaxed,
                       "the obligation is already relaxed for this incident", "obligation.id");
        }
      }
      RelaxationRecord record;
      record.obligation = entry.payload.obligation;
      record.protection = slot->obligation.protection;
      record.epoch = state.epoch;
      record.incident = state.incident.incident;
      record.granted_at = entry.recorded_at;
      record.granted_by = entry.payload.granted_by;
      state.relaxations.push_back(record);
      break;
    }

    case JournalKind::StabilityObserved: {
      if (!state.incident.has_stable_since) {
        state.incident.has_stable_since = true;
        state.incident.stable_since = entry.recorded_at;
      }
      break;
    }

    case JournalKind::StabilityCleared: {
      state.incident.has_stable_since = false;
      state.incident.stable_since = Timestamp{};
      break;
    }

    case JournalKind::EpochRolled: {
      state.epoch = entry.payload.epoch;
      state.incarnation = entry.payload.incarnation;
      // A rolled epoch never inherits recovery progress, and every non-terminal
      // request of the previous authority is superseded under its own identity
      // rather than retried. A resume by the same controller is different: the
      // dispatch may or may not have reached the controller, so the request is
      // resolved as indeterminate and is never blindly repeated.
      for (auto& slot : state.requests) {
        if (!request_state_is_open(slot.request.state)) {
          continue;
        }
        if (entry.payload.epoch_rollover) {
          slot.request.state = RequestState::Superseded;
          slot.request.terminal_reason = ReasonCode::JustificationWithdrawn;
          slot.request.superseded_by = AttemptId{};
        } else {
          slot.request.state = RequestState::Indeterminate;
          slot.request.terminal_reason = ReasonCode::DispatchOutcomeUnknown;
        }
      }
      state.incident.operator_authorized = false;
      state.incident.authorized_generation = IncidentGeneration{};
      state.incident.has_stable_since = false;
      state.incident.stable_since = Timestamp{};
      state.incident.recovery_steps = 0;
      state.has_plan = false;
      state.plan = ResponsePlan{};
      state.incident.has_plan = false;
      // Restored durable evidence is not current physical evidence.
      for (auto& slot : state.observations) {
        if (slot.present) {
          slot.recovered = true;
          slot.freshness = Freshness::Unknown;
        }
      }
      break;
    }

  }

  touch();
  return {};
}

}  // namespace summon::pfm
