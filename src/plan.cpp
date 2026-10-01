// Power Failure Manager -- deterministic response planning.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0 (see LICENSE).
#include "pfm/plan.hpp"

#include <algorithm>
#include <map>
#include <string>
#include <vector>

#include "pfm/codec.hpp"

namespace summon::pfm {
namespace {

struct RequestTemplate {
  RequestKind kind{RequestKind::Unknown};
  RefToken target{};
  ElementKind target_kind{ElementKind::Unknown};
  RequestOwner owner{RequestOwner::Unknown};
  RefToken controller{};
  bool has_magnitude{false};
  BasisPoints magnitude{};
  ReasonCode justification{ReasonCode::None};
  RefToken justification_subject{};
  ProofKind required_proof{ProofKind::None};
};

bool template_less(const RequestTemplate& a, const RequestTemplate& b) noexcept {
  if (a.kind != b.kind) {
    return static_cast<std::uint8_t>(a.kind) < static_cast<std::uint8_t>(b.kind);
  }
  if (!(a.target == b.target)) {
    return a.target < b.target;
  }
  if (a.owner != b.owner) {
    return static_cast<std::uint8_t>(a.owner) < static_cast<std::uint8_t>(b.owner);
  }
  return a.magnitude < b.magnitude;
}

bool same_operation(const RequestTemplate& a, const RequestTemplate& b) noexcept {
  return a.kind == b.kind && a.target == b.target && a.owner == b.owner &&
         a.has_magnitude == b.has_magnitude &&
         (!a.has_magnitude || a.magnitude == b.magnitude);
}

void add_template(std::vector<RequestTemplate>& out, RequestTemplate item) {
  for (const auto& existing : out) {
    if (same_operation(existing, item)) {
      return;
    }
  }
  out.push_back(std::move(item));
}

BasisPoints reserve_floor_as_basis_points(MilliPercent floor) noexcept {
  return BasisPoints::from_value(floor.value() / 10);
}

}  // namespace

const ResponseRequest* ResponsePlan::find_request(ResponseRequestId id) const noexcept {
  for (const auto& request : requests) {
    if (request.id == id) {
      return &request;
    }
  }
  return nullptr;
}

void sort_requests(std::vector<ResponseRequest>& requests) {
  std::sort(requests.begin(), requests.end(), [](const ResponseRequest& a,
                                                 const ResponseRequest& b) {
    if (a.kind != b.kind) {
      return static_cast<std::uint8_t>(a.kind) < static_cast<std::uint8_t>(b.kind);
    }
    if (a.target.owner != b.target.owner) {
      return static_cast<std::uint8_t>(a.target.owner) <
             static_cast<std::uint8_t>(b.target.owner);
    }
    if (!(a.target.element == b.target.element)) {
      return a.target.element < b.target.element;
    }
    if (a.has_magnitude != b.has_magnitude) {
      return !a.has_magnitude;
    }
    if (a.has_magnitude && !(a.magnitude == b.magnitude)) {
      return a.magnitude < b.magnitude;
    }
    return a.id < b.id;
  });
}

Result<ResponsePlan> plan_response(const PlanInputs& inputs) {
  if (inputs.topology == nullptr) {
    return Status::error(StatusCode::PreconditionFailed,
                         "planning requires a published electrical topology")
        .with_context("plan.topology");
  }
  if (auto r = validate(inputs.policy); !r.ok()) {
    return r.status();
  }
  if (auto r = validate(inputs.bounds); !r.ok()) {
    return r.status();
  }
  const TopologyIndex& topology = *inputs.topology;

  auto classification =
      classify(inputs.current_observations, inputs.now, topology, inputs.policy, inputs.bounds);
  if (!classification.ok()) {
    return classification.status();
  }
  auto scope = resolve_scope(classification.value(), topology, inputs.policy, inputs.bounds);
  if (!scope.ok()) {
    return scope.status();
  }

  // Elements that used to report and no longer do are unresolved, not healthy:
  // they enter the plan as ambiguous failures and block recovery.
  {
    std::vector<RefToken> unevidenced = inputs.unevidenced_elements;
    std::sort(unevidenced.begin(), unevidenced.end());
    unevidenced.erase(std::unique(unevidenced.begin(), unevidenced.end()), unevidenced.end());
    if (unevidenced.size() > inputs.bounds.max_scope_elements) {
      return Status::error(StatusCode::BoundsExceeded,
                           "the unevidenced element set exceeds the configured bound")
          .with_context("plan.unevidenced");
    }
    for (const auto& element : unevidenced) {
      if (std::binary_search(scope.value().unevidenced_elements.begin(),
                             scope.value().unevidenced_elements.end(), element)) {
        continue;
      }
      scope.value().unevidenced_elements.push_back(element);
      const auto* found = topology.find(element);
      ClassifiedFailure failure;
      failure.klass = FailureClass::AmbiguousElectricalEvidence;
      failure.element = element;
      failure.kind = found != nullptr ? found->kind : ElementKind::Unknown;
      failure.primary_reason = ReasonCode::EvidenceMissing;
      failure.evidence_current = false;
      ReasonStep step;
      step.code = ReasonCode::EvidenceMissing;
      step.subject = element;
      step.freshness = Freshness::Unknown;
      failure.reasons.push_back(step);
      classification.value().failures.push_back(failure);
      classification.value().unresolved_elements.push_back(element);
    }
    std::sort(scope.value().unevidenced_elements.begin(), scope.value().unevidenced_elements.end());
    scope.value().unevidenced_elements.erase(
        std::unique(scope.value().unevidenced_elements.begin(),
                    scope.value().unevidenced_elements.end()),
        scope.value().unevidenced_elements.end());
    std::sort(classification.value().failures.begin(), classification.value().failures.end());
    std::sort(classification.value().unresolved_elements.begin(),
              classification.value().unresolved_elements.end());
    classification.value().unresolved_elements.erase(
        std::unique(classification.value().unresolved_elements.begin(),
                    classification.value().unresolved_elements.end()),
        classification.value().unresolved_elements.end());
  }

  ResponsePlan plan;
  plan.generation = inputs.next_plan_generation;
  plan.incident = inputs.incident;
  plan.incident_generation = inputs.incident_generation;
  plan.topology_generation = topology.generation();
  plan.policy_generation = inputs.policy.generation;
  plan.epoch = inputs.epoch;
  plan.decided_at = inputs.now;
  plan.failures = classification.value().failures;
  plan.scope = scope.value();
  plan.primary_failure = classification.value().primary_class();
  plan.primary_element =
      classification.value().failures.empty() ? RefToken{} : classification.value().failures.front().element;
  plan.reasons = {};
  for (const auto& failure : plan.failures) {
    for (const auto& reason : failure.reasons) {
      plan.reasons.push_back(reason);
    }
  }
  std::sort(plan.reasons.begin(), plan.reasons.end());
  plan.reasons.erase(std::unique(plan.reasons.begin(), plan.reasons.end()), plan.reasons.end());
  if (plan.reasons.size() > inputs.bounds.max_reason_codes) {
    plan.reasons.resize(inputs.bounds.max_reason_codes);
  }

  std::vector<RequestTemplate> templates;

  // --- isolation and protection requirements ------------------------------

  for (const auto& failure : plan.failures) {
    if (failure.element.kind() == RefKind::FailureDomain) {
      // A shared failure domain is a grouping, not a switchable element: it
      // widens the affected scope, and the concrete member failures carry the
      // isolation and verification requests. Asking a controller to isolate a
      // grouping would be a request it cannot answer.
      continue;
    }
    const auto* element = topology.find(failure.element);
    const ElementKind kind = element != nullptr ? element->kind : failure.kind;

    IsolationRequirement requirement;
    requirement.element = failure.element;
    RefToken point;
    requirement.has_isolation_point = topology.isolation_point_for(failure.element, point);
    requirement.isolation_point = point;
    requirement.required_proof = ProofKind::DeEnergization;
    requirement.failure = failure.klass;
    requirement.reason = failure.primary_reason;
    plan.isolations.push_back(requirement);

    if (failure_class_is_ambiguous(failure.klass)) {
      // Unresolved evidence is never acted on blindly: the owning controller is
      // asked to prove the electrical state, not to change it.
      add_template(templates,
                   RequestTemplate{RequestKind::VerifyDeEnergization, failure.element, kind,
                                   owner_for_kind(RequestKind::VerifyDeEnergization), RefToken{},
                                   false, BasisPoints{}, failure.primary_reason, failure.element,
                                   ProofKind::DeEnergization});
      continue;
    }

    switch (failure.klass) {
      case FailureClass::UtilityFeedLoss:
      case FailureClass::UtilityFeedDegradation: {
        add_template(templates,
                     RequestTemplate{RequestKind::IsolateElement, failure.element, kind,
                                     owner_for_kind(RequestKind::IsolateElement), RefToken{}, false,
                                     BasisPoints{}, failure.primary_reason, failure.element,
                                     ProofKind::Isolation});
        add_template(templates,
                     RequestTemplate{RequestKind::VerifyDeEnergization, failure.element, kind,
                                     owner_for_kind(RequestKind::VerifyDeEnergization), RefToken{},
                                     false, BasisPoints{}, failure.primary_reason, failure.element,
                                     ProofKind::DeEnergization});
        break;
      }
      case FailureClass::SwitchgearFailure:
      case FailureClass::BusFailure:
      case FailureClass::PduFailure:
      case FailureClass::PduBranchFailure:
      case FailureClass::UpsFailure: {
        add_template(templates,
                     RequestTemplate{RequestKind::IsolateElement, failure.element, kind,
                                     owner_for_kind(RequestKind::IsolateElement), RefToken{}, false,
                                     BasisPoints{}, failure.primary_reason, failure.element,
                                     ProofKind::Isolation});
        add_template(templates,
                     RequestTemplate{RequestKind::VerifyDeEnergization, failure.element, kind,
                                     owner_for_kind(RequestKind::VerifyDeEnergization), RefToken{},
                                     false, BasisPoints{}, failure.primary_reason, failure.element,
                                     ProofKind::DeEnergization});
        break;
      }
      case FailureClass::BreakerFailure:
      case FailureClass::CircuitFailure: {
        add_template(templates,
                     RequestTemplate{RequestKind::DeferReclose, failure.element, kind,
                                     owner_for_kind(RequestKind::DeferReclose), RefToken{}, false,
                                     BasisPoints{}, ReasonCode::BreakerTripped, failure.element,
                                     ProofKind::BreakerOpen});
        add_template(templates,
                     RequestTemplate{RequestKind::VerifyDeEnergization, failure.element, kind,
                                     owner_for_kind(RequestKind::VerifyDeEnergization), RefToken{},
                                     false, BasisPoints{}, failure.primary_reason, failure.element,
                                     ProofKind::DeEnergization});
        break;
      }
      case FailureClass::UpsReserveInsufficient: {
        add_template(templates, RequestTemplate{
                                    RequestKind::PreserveReserve,
                                    failure.element,
                                    kind,
                                    owner_for_kind(RequestKind::PreserveReserve),
                                    RefToken{},
                                    true,
                                    reserve_floor_as_basis_points(inputs.policy.reserve_floor),
                                    failure.primary_reason,
                                    failure.element,
                                    ProofKind::Reserve});
        for (const auto& load : topology.load_groups_under(failure.element)) {
          add_template(templates, RequestTemplate{
                                      RequestKind::ShedLoad,
                                      load,
                                      ElementKind::LoadGroup,
                                      owner_for_kind(RequestKind::ShedLoad),
                                      RefToken{},
                                      false,
                                      BasisPoints{},
                                      ReasonCode::UpsReserveBelowFloor,
                                      failure.element,
                                      ProofKind::LoadShed});
        }
        break;
      }
      case FailureClass::GeneratorStartFailure: {
        add_template(templates,
                     RequestTemplate{RequestKind::StartGenerator, failure.element, kind,
                                     owner_for_kind(RequestKind::StartGenerator), RefToken{}, false,
                                     BasisPoints{}, failure.primary_reason, failure.element,
                                     ProofKind::Generation});
        break;
      }
      case FailureClass::GeneratorSyncFailure: {
        add_template(templates,
                     RequestTemplate{RequestKind::SynchronizeGenerator, failure.element, kind,
                                     owner_for_kind(RequestKind::SynchronizeGenerator), RefToken{},
                                     false, BasisPoints{}, failure.primary_reason, failure.element,
                                     ProofKind::Synchronization});
        break;
      }
      case FailureClass::GeneratorTransferFailure: {
        add_template(templates,
                     RequestTemplate{RequestKind::TransferToGenerator, failure.element, kind,
                                     owner_for_kind(RequestKind::TransferToGenerator), RefToken{},
                                     false, BasisPoints{}, failure.primary_reason, failure.element,
                                     ProofKind::Transfer});
        break;
      }
      case FailureClass::GeneratorFailure: {
        add_template(templates,
                     RequestTemplate{RequestKind::IsolateElement, failure.element, kind,
                                     owner_for_kind(RequestKind::IsolateElement), RefToken{}, false,
                                     BasisPoints{}, failure.primary_reason, failure.element,
                                     ProofKind::Isolation});
        break;
      }
      case FailureClass::SharedUpstreamDomainFailure:
      case FailureClass::AmbiguousElectricalEvidence:
      case FailureClass::None:
        break;
    }
  }

  // A feed loss with a generator available in the affected domains justifies a
  // bounded start request, and every UPS in the affected scope must hold its
  // reserve while the supply is unresolved.
  const bool feed_failure = plan.primary_failure == FailureClass::UtilityFeedLoss ||
                            plan.primary_failure == FailureClass::UtilityFeedDegradation ||
                            plan.primary_failure == FailureClass::SharedUpstreamDomainFailure;
  if (feed_failure) {
    std::vector<RefToken> generators;
    for (const auto& element : topology.descendants(plan.primary_element)) {
      const auto* found = topology.find(element);
      if (found != nullptr && found->kind == ElementKind::Generator) {
        generators.push_back(element);
      }
    }
    if (generators.empty()) {
      for (const auto& domain : plan.scope.domains) {
        const auto* found_domain = topology.domain(domain);
        if (found_domain == nullptr) {
          continue;
        }
        for (const auto& member : found_domain->members) {
          const auto* found = topology.find(member);
          if (found != nullptr && found->kind == ElementKind::Generator) {
            generators.push_back(member);
          }
        }
      }
    }
    std::sort(generators.begin(), generators.end());
    generators.erase(std::unique(generators.begin(), generators.end()), generators.end());
    for (const auto& generator : generators) {
      add_template(templates,
                   RequestTemplate{RequestKind::StartGenerator, generator,
                                   ElementKind::Generator,
                                   owner_for_kind(RequestKind::StartGenerator), RefToken{}, false,
                                   BasisPoints{}, ReasonCode::FeedDeEnergized,
                                   plan.primary_element, ProofKind::Generation});
    }
  }

  // --- restoration --------------------------------------------------------
  //
  // An element that is still unhealthy inside the incident scope is restored
  // through a bounded request to the authority that owns the switching, never
  // by assuming it came back.
  {
    std::vector<RefToken> restore = inputs.unhealthy_scope_elements;
    std::sort(restore.begin(), restore.end());
    restore.erase(std::unique(restore.begin(), restore.end()), restore.end());
    if (restore.size() > inputs.bounds.max_scope_elements) {
      return Status::error(StatusCode::BoundsExceeded,
                           "the unhealthy scope set exceeds the configured bound")
          .with_context("plan.unhealthy");
    }
    for (const auto& element : restore) {
      const auto* found = topology.find(element);
      if (found == nullptr) {
        continue;
      }
      RefToken point;
      const ElectricalObservation* point_observation = nullptr;
      if (topology.isolation_point_for(element, point)) {
        for (const auto& observation : inputs.current_observations) {
          if (observation.element == point) {
            point_observation = &observation;
          }
        }
      }
      if (point_observation != nullptr &&
          (point_observation->breaker == BreakerPosition::Open ||
           point_observation->breaker == BreakerPosition::RackedOut)) {
        add_template(templates,
                     RequestTemplate{RequestKind::Reclose, point, ElementKind::Breaker,
                                     owner_for_kind(RequestKind::Reclose), RefToken{}, false,
                                     BasisPoints{},
                                     ReasonCode::ElementDeEnergizedWhileUpstreamEnergized, element,
                                     ProofKind::BreakerOpen});
      } else {
        add_template(templates,
                     RequestTemplate{RequestKind::RestoreNormalFeed, element, found->kind,
                                     owner_for_kind(RequestKind::RestoreNormalFeed), RefToken{},
                                     false, BasisPoints{},
                                     ReasonCode::ElementDeEnergizedWhileUpstreamEnergized, element,
                                     ProofKind::StableSource});
      }
    }
  }

  // --- protection requirements -------------------------------------------

  for (const auto& obligation : inputs.obligations) {
    if (auto r = validate(obligation, inputs.bounds); !r.ok()) {
      return r.status();
    }
    const bool in_scope = plan.scope.contains(obligation.target) ||
                          obligation.target == plan.primary_element;
    ProtectionRequirement requirement;
    requirement.element = obligation.target;
    requirement.protection = obligation.protection;
    requirement.has_obligation = true;
    requirement.obligation = obligation.id;
    requirement.has_reserve_floor = obligation.has_reserve_floor;
    requirement.reserve_floor = obligation.reserve_floor;
    requirement.reason = in_scope ? ReasonCode::DomainMemberFailed
                                  : ReasonCode::DownstreamHealthyDoesNotImplyRecovery;
    plan.protections.push_back(requirement);
  }

  // --- requests -----------------------------------------------------------

  std::sort(templates.begin(), templates.end(), template_less);

  std::map<Fingerprint, const ResponseRequest*> open_by_fingerprint;
  for (const auto& request : inputs.open_requests) {
    if (request_state_is_open(request.state)) {
      open_by_fingerprint.emplace(request.fingerprint, &request);
    }
  }

  ResponseRequestId next_id = inputs.first_request_id;
  AttemptId next_attempt = inputs.first_attempt_id;

  for (const auto& item : templates) {
    if (plan.requests.size() >= inputs.bounds.max_requests_per_plan) {
      return Status::error(StatusCode::ResourceExhausted,
                           "plan exceeds the bounded request count")
          .with_context("plan.requests");
    }
    ResponseRequest request;
    request.kind = item.kind;
    request.target.element = item.target;
    request.target.kind = item.target_kind;
    request.target.owner = item.owner;
    request.target.controller = item.controller;
    request.has_magnitude = item.has_magnitude;
    request.magnitude = item.magnitude;
    request.plan_generation = plan.generation;
    request.incident_generation = inputs.incident_generation;
    request.required_proof = item.required_proof;
    request.justification = item.justification;
    request.justification_subject = item.justification_subject;
    request.created_at = inputs.now;
    request.fingerprint = response_request_fingerprint(request);

    const auto existing = open_by_fingerprint.find(request.fingerprint);
    if (existing != open_by_fingerprint.end()) {
      // The same consequential operation is already outstanding. It is carried
      // forward under its original identity rather than planned again.
      ResponseRequest carried = *existing->second;
      carried.plan_generation = plan.generation;
      plan.requests.push_back(std::move(carried));
      continue;
    }

    request.id = next_id;
    request.attempt = next_attempt;
    request.attempt_ordinal = 1;
    request.idempotency = IdempotencyKey::from_fingerprint(request.fingerprint);
    request.state = RequestState::Planned;
    auto deadline = checked_add(inputs.now, inputs.policy.verification_window);
    if (!deadline.ok()) {
      return deadline.status();
    }
    request.deadline = deadline.value();
    next_id = ResponseRequestId::from_value(next_id.value() + 1);
    next_attempt = AttemptId::from_value(next_attempt.value() + 1);
    plan.requests.push_back(std::move(request));
  }

  sort_requests(plan.requests);

  codec::Writer writer;
  codec::encode(writer, plan.generation);
  codec::encode(writer, plan.incident);
  codec::encode(writer, plan.incident_generation);
  codec::encode(writer, plan.topology_generation);
  codec::encode(writer, plan.policy_generation);
  codec::encode(writer, plan.epoch);
  writer.u8(static_cast<std::uint8_t>(plan.primary_failure));
  codec::encode(writer, plan.primary_element);
  for (const auto& failure : plan.failures) {
    writer.u8(static_cast<std::uint8_t>(failure.klass));
    codec::encode(writer, failure.element);
    writer.u16(static_cast<std::uint16_t>(failure.primary_reason));
    writer.flag(failure.evidence_current);
  }
  for (const auto& reason : plan.reasons) {
    writer.u16(static_cast<std::uint16_t>(reason.code));
    codec::encode(writer, reason.subject);
  }
  for (const auto& element : plan.scope.impacted_elements) {
    codec::encode(writer, element);
  }
  for (const auto& isolation : plan.isolations) {
    codec::encode(writer, isolation.element);
    codec::encode(writer, isolation.isolation_point);
    writer.flag(isolation.has_isolation_point);
  }
  for (const auto& protection : plan.protections) {
    codec::encode(writer, protection.element);
    writer.u8(static_cast<std::uint8_t>(protection.protection));
    writer.u64(protection.obligation.value());
  }
  for (const auto& request : plan.requests) {
    codec::encode(writer, request.fingerprint);
    codec::encode(writer, request.id);
  }
  if (!writer.ok()) {
    return writer.error();
  }
  plan.fingerprint = codec::fnv1a128(writer.bytes());
  return plan;
}

}  // namespace summon::pfm
