// Power Failure Manager -- bounded response requests and their lifecycle.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0 (see LICENSE).
#include "pfm/request.hpp"

#include <algorithm>
#include <string>

#include "pfm/codec.hpp"

namespace summon::pfm {
namespace {

struct KindEntry {
  RequestKind kind;
  std::string_view name;
  std::string_view code;
  RequestOwner owner;
  ProofKind proof;
};

constexpr KindEntry kRequestKinds[] = {
    {RequestKind::Unknown, "unknown", "pfm.request.unknown", RequestOwner::Unknown,
     ProofKind::None},
    {RequestKind::IsolateElement, "isolate-element", "pfm.request.isolate_element",
     RequestOwner::PowerControlPlane, ProofKind::Isolation},
    {RequestKind::VerifyDeEnergization, "verify-de-energization",
     "pfm.request.verify_de_energization", RequestOwner::PowerControlPlane,
     ProofKind::DeEnergization},
    {RequestKind::BlockTransfer, "block-transfer", "pfm.request.block_transfer",
     RequestOwner::UpsControl, ProofKind::Transfer},
    {RequestKind::SelectFeed, "select-feed", "pfm.request.select_feed",
     RequestOwner::FeedAuthority, ProofKind::FeedSelected},
    {RequestKind::StartGenerator, "start-generator", "pfm.request.start_generator",
     RequestOwner::GeneratorControl, ProofKind::Generation},
    {RequestKind::SynchronizeGenerator, "synchronize-generator",
     "pfm.request.synchronize_generator", RequestOwner::GeneratorControl,
     ProofKind::Synchronization},
    {RequestKind::TransferToGenerator, "transfer-to-generator",
     "pfm.request.transfer_to_generator", RequestOwner::GeneratorControl, ProofKind::Transfer},
    {RequestKind::PreserveReserve, "preserve-reserve", "pfm.request.preserve_reserve",
     RequestOwner::UpsControl, ProofKind::Reserve},
    {RequestKind::ShedLoad, "shed-load", "pfm.request.shed_load", RequestOwner::LoadShedding,
     ProofKind::LoadShed},
    {RequestKind::CapPower, "cap-power", "pfm.request.cap_power", RequestOwner::PowerCapacity,
     ProofKind::StableSource},
    {RequestKind::DeferReclose, "defer-reclose", "pfm.request.defer_reclose",
     RequestOwner::PowerControlPlane, ProofKind::BreakerOpen},
    {RequestKind::Reclose, "reclose", "pfm.request.reclose", RequestOwner::PowerControlPlane,
     ProofKind::StableSource},
    {RequestKind::RestoreNormalFeed, "restore-normal-feed", "pfm.request.restore_normal_feed",
     RequestOwner::FeedAuthority, ProofKind::StableSource},
};

const KindEntry& kind_entry(RequestKind kind) noexcept {
  for (const auto& entry : kRequestKinds) {
    if (entry.kind == kind) {
      return entry;
    }
  }
  return kRequestKinds[0];
}

}  // namespace

std::string_view request_kind_name(RequestKind value) noexcept { return kind_entry(value).name; }

std::string_view request_kind_code(RequestKind value) noexcept { return kind_entry(value).code; }

std::string_view request_owner_name(RequestOwner value) noexcept {
  switch (value) {
    case RequestOwner::Unknown: return "unknown";
    case RequestOwner::FeedAuthority: return "feed-authority";
    case RequestOwner::PowerControlPlane: return "power-control-plane";
    case RequestOwner::PduControl: return "pdu-control";
    case RequestOwner::UpsControl: return "ups-control";
    case RequestOwner::GeneratorControl: return "generator-control";
    case RequestOwner::LoadShedding: return "load-shedding";
    case RequestOwner::PowerCapacity: return "power-capacity";
    case RequestOwner::FacilityFailureDomainRegistry: return "facility-failure-domain-registry";
    case RequestOwner::IncidentStateFabric: return "incident-state-fabric";
  }
  return "unknown";
}

RequestOwner owner_for_kind(RequestKind kind) noexcept { return kind_entry(kind).owner; }

std::string_view proof_kind_name(ProofKind value) noexcept {
  switch (value) {
    case ProofKind::None: return "none";
    case ProofKind::Isolation: return "isolation";
    case ProofKind::DeEnergization: return "de-energization";
    case ProofKind::BreakerOpen: return "breaker-open";
    case ProofKind::Transfer: return "transfer";
    case ProofKind::Synchronization: return "synchronization";
    case ProofKind::Generation: return "generation";
    case ProofKind::Reserve: return "reserve";
    case ProofKind::LoadShed: return "load-shed";
    case ProofKind::StableSource: return "stable-source";
    case ProofKind::FeedSelected: return "feed-selected";
  }
  return "unknown";
}

ProofKind required_proof_for_kind(RequestKind kind) noexcept { return kind_entry(kind).proof; }

std::string_view request_state_name(RequestState value) noexcept {
  switch (value) {
    case RequestState::Planned: return "planned";
    case RequestState::Issued: return "issued";
    case RequestState::Acknowledged: return "acknowledged";
    case RequestState::Observed: return "observed";
    case RequestState::Verified: return "verified";
    case RequestState::Failed: return "failed";
    case RequestState::Superseded: return "superseded";
    case RequestState::Abandoned: return "abandoned";
    case RequestState::Refused: return "refused";
    case RequestState::Expired: return "expired";
    case RequestState::Indeterminate: return "indeterminate";
  }
  return "unknown";
}

bool request_state_is_terminal(RequestState value) noexcept {
  switch (value) {
    case RequestState::Verified:
    case RequestState::Failed:
    case RequestState::Superseded:
    case RequestState::Abandoned:
    case RequestState::Refused:
    case RequestState::Expired:
      return true;
    default:
      return false;
  }
}

bool request_state_is_open(RequestState value) noexcept {
  switch (value) {
    case RequestState::Planned:
    case RequestState::Issued:
    case RequestState::Acknowledged:
    case RequestState::Observed:
    case RequestState::Indeterminate:
      return true;
    default:
      return false;
  }
}

bool request_state_transition_legal(RequestState from, RequestState to) noexcept {
  if (from == to) {
    return false;
  }
  switch (from) {
    case RequestState::Planned:
      return to == RequestState::Issued || to == RequestState::Refused ||
             to == RequestState::Abandoned || to == RequestState::Superseded ||
             to == RequestState::Failed;
    case RequestState::Issued:
      return to == RequestState::Acknowledged || to == RequestState::Observed ||
             to == RequestState::Verified || to == RequestState::Failed ||
             to == RequestState::Expired || to == RequestState::Indeterminate ||
             to == RequestState::Superseded || to == RequestState::Abandoned ||
             to == RequestState::Refused;
    case RequestState::Acknowledged:
      return to == RequestState::Observed || to == RequestState::Verified ||
             to == RequestState::Failed || to == RequestState::Expired ||
             to == RequestState::Indeterminate || to == RequestState::Superseded ||
             to == RequestState::Abandoned;
    case RequestState::Observed:
      return to == RequestState::Verified || to == RequestState::Failed ||
             to == RequestState::Expired || to == RequestState::Superseded ||
             to == RequestState::Abandoned || to == RequestState::Indeterminate;
    case RequestState::Indeterminate:
      // An indeterminate dispatch is resolved explicitly: it either happened,
      // it did not, or it was replaced by a later attempt.
      return to == RequestState::Verified || to == RequestState::Failed ||
             to == RequestState::Superseded || to == RequestState::Abandoned ||
             to == RequestState::Observed;
    case RequestState::Verified:
    case RequestState::Failed:
    case RequestState::Superseded:
    case RequestState::Abandoned:
    case RequestState::Refused:
    case RequestState::Expired:
      return false;
  }
  return false;
}

bool ResponseRequest::satisfies(ProofKind required) const noexcept {
  if (state != RequestState::Verified) {
    return false;
  }
  if (required == ProofKind::None) {
    return true;
  }
  return required_proof == required;
}

Fingerprint response_request_fingerprint(const ResponseRequest& request) noexcept {
  // The fingerprint covers exactly the fields that make this a distinct
  // consequential operation. Attempt identity, plan generation, and instants
  // are deliberately excluded: re-planning or re-attempting the same operation
  // must reuse the same idempotency key so that a lost response can never cause
  // a second consequential mutation.
  codec::Writer writer;
  writer.u16(static_cast<std::uint16_t>(request.kind));
  codec::encode(writer, request.target.element);
  writer.u8(static_cast<std::uint8_t>(request.target.kind));
  writer.u8(static_cast<std::uint8_t>(request.target.owner));
  codec::encode(writer, request.target.controller);
  writer.flag(request.has_magnitude);
  codec::encode(writer, request.magnitude);
  writer.u64(request.incident_generation.value());
  if (!writer.ok()) {
    return Fingerprint{};
  }
  return codec::fnv1a128(writer.bytes());
}

Result<void> validate(const ResponseRequest& request, const Bounds& bounds) {
  if (request.id.is_absent()) {
    return Status::error(StatusCode::InvalidArgument, "request has no identity")
        .with_context("request.id");
  }
  if (request.kind == RequestKind::Unknown) {
    return Status::error(StatusCode::InvalidArgument, "request has no kind")
        .with_context("request.id");
  }
  if (!request.target.element.is_set() || request.target.kind == ElementKind::Unknown) {
    return Status::error(StatusCode::RequestTargetMismatch, "request target is incomplete")
        .with_context("request.target");
  }
  if (ref_kind_for_element(request.target.kind) != request.target.element.kind()) {
    return Status::error(StatusCode::RequestTargetMismatch,
                         "request target kind does not match the target reference")
        .with_context(request.target.element.to_string());
  }
  if (request.target.owner == RequestOwner::Unknown) {
    return Status::error(StatusCode::RequestTargetMismatch, "request has no owning authority")
        .with_context(request.target.element.to_string());
  }
  if (!request.idempotency.is_set()) {
    return Status::error(StatusCode::InvalidArgument, "request has no idempotency key")
        .with_context("request.idempotency");
  }
  if (!request.fingerprint.is_set()) {
    return Status::error(StatusCode::InvalidArgument, "request has no fingerprint")
        .with_context("request.idempotency");
  }
  if (!(request.fingerprint == response_request_fingerprint(request))) {
    return Status::error(StatusCode::IdempotencyConflict,
                         "request fingerprint does not match its own content")
        .with_context("request.fingerprint");
  }
  if (!(request.idempotency == IdempotencyKey::from_fingerprint(request.fingerprint))) {
    return Status::error(StatusCode::IdempotencyConflict,
                         "idempotency key is not derived from the request fingerprint")
        .with_context("request.idempotency");
  }
  if (request.attempt.is_absent()) {
    return Status::error(StatusCode::InvalidArgument, "request has no attempt identity")
        .with_context("request.id");
  }
  if (request.attempt_ordinal == 0) {
    return Status::error(StatusCode::InvalidArgument, "attempt ordinal starts at one")
        .with_context("request.attempt");
  }
  if (request.created_at.value() == 0) {
    return Status::error(StatusCode::InvalidArgument, "request has no creation instant")
        .with_context("request.created_at");
  }
  if (request.has_magnitude && !basis_points_in_range(request.magnitude)) {
    return Status::error(StatusCode::ValueOutOfRange, "request magnitude is not a valid ratio")
        .with_context("request.magnitude");
  }
  if (request.owner_note.size() > bounds.max_note_length) {
    return Status::error(StatusCode::FieldTooLong, "request note exceeds the bound")
        .with_context("request.note");
  }
  if (request.evidence.size() > bounds.max_reason_evidence_refs) {
    return Status::error(StatusCode::BoundsExceeded, "request evidence list exceeds the bound")
        .with_context("request.evidence");
  }
  if (request.state == RequestState::Verified && request.required_proof != ProofKind::None &&
      !request.verification_source.is_set()) {
    return Status::error(StatusCode::EffectUnverified,
                         "a verified request must name the source that confirmed the effect")
        .with_context("request.verification");
  }
  return {};
}

}  // namespace summon::pfm
