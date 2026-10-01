// Power Failure Manager -- the authoritative input journal.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0 (see LICENSE).
#include "pfm/journal.hpp"

#include <string>

#include "pfm/codec.hpp"

namespace summon::pfm {
namespace {

struct JournalKindEntry {
  JournalKind kind;
  std::string_view name;
};

constexpr JournalKindEntry kJournalKinds[] = {
    {JournalKind::None, "none"},
    {JournalKind::IncidentOpened, "incident-opened"},
    {JournalKind::IncidentClosed, "incident-closed"},
    {JournalKind::EvidenceAdmitted, "evidence-admitted"},
    {JournalKind::EvidenceInvalidated, "evidence-invalidated"},
    {JournalKind::TopologyPublished, "topology-published"},
    {JournalKind::PolicyPublished, "policy-published"},
    {JournalKind::ObligationsPublished, "obligations-published"},
    {JournalKind::PlanComputed, "plan-computed"},
    {JournalKind::RequestIssued, "request-issued"},
    {JournalKind::RequestAcknowledged, "request-acknowledged"},
    {JournalKind::RequestObserved, "request-observed"},
    {JournalKind::RequestVerified, "request-verified"},
    {JournalKind::RequestFailed, "request-failed"},
    {JournalKind::RequestRefused, "request-refused"},
    {JournalKind::RequestSuperseded, "request-superseded"},
    {JournalKind::RequestAbandoned, "request-abandoned"},
    {JournalKind::RequestIndeterminate, "request-indeterminate"},
    {JournalKind::RequestResolved, "request-resolved"},
    {JournalKind::RecoveryAuthorized, "recovery-authorized"},
    {JournalKind::RecoveryStarted, "recovery-started"},
    {JournalKind::RecoveryCompleted, "recovery-completed"},
    {JournalKind::ObligationRelaxed, "obligation-relaxed"},
    {JournalKind::EpochRolled, "epoch-rolled"},
    {JournalKind::StabilityObserved, "stability-observed"},
    {JournalKind::StabilityCleared, "stability-cleared"},
};

// The fingerprint is recomputed from the entry's own bytes, so a corrupted,
// truncated, reordered, or substituted entry is detected on load.
Bounds fingerprint_bounds() noexcept {
  Bounds bounds;
  bounds.max_note_length = 4096;
  bounds.max_provenance_length = 4096;
  bounds.max_label_length = 4096;
  return bounds;
}

}  // namespace

std::string_view journal_kind_name(JournalKind value) noexcept {
  for (const auto& entry : kJournalKinds) {
    if (entry.kind == value) {
      return entry.name;
    }
  }
  return "unknown";
}

Fingerprint journal_entry_fingerprint(const JournalEntry& entry) noexcept {
  codec::Writer writer;
  writer.u16(static_cast<std::uint16_t>(entry.kind));
  writer.u64(entry.sequence.value());
  writer.u64(entry.revision.value());
  writer.i64(entry.recorded_at.value());
  codec::encode(writer, entry.payload, fingerprint_bounds());
  if (!writer.ok()) {
    return Fingerprint{};
  }
  return codec::fnv1a128(writer.bytes());
}

Result<void> validate(const JournalEntry& entry, const Bounds& bounds) {
  if (entry.kind == JournalKind::None) {
    return Status::error(StatusCode::InvalidArgument, "journal entry has no kind")
        .with_context("journal.kind");
  }
  if (entry.sequence.is_absent()) {
    return Status::error(StatusCode::InvalidArgument, "journal entry has no sequence")
        .with_context("journal.sequence");
  }
  if (entry.revision.is_absent()) {
    return Status::error(StatusCode::InvalidArgument, "journal entry has no revision")
        .with_context("journal.revision");
  }
  if (entry.recorded_at.value() == 0) {
    return Status::error(StatusCode::InvalidArgument, "journal entry has no instant")
        .with_context("journal.recorded_at");
  }
  if (!entry.fingerprint.is_set()) {
    return Status::error(StatusCode::InvalidArgument, "journal entry has no fingerprint")
        .with_context("journal.fingerprint");
  }
  if (!(entry.fingerprint == journal_entry_fingerprint(entry))) {
    return Status::error(StatusCode::StoreIntegrityFailure,
                         "journal entry fingerprint does not match its content")
        .with_context("journal.fingerprint");
  }
  if (entry.payload.note.size() > bounds.max_note_length) {
    return Status::error(StatusCode::FieldTooLong, "journal note exceeds the bound")
        .with_context("journal.note");
  }
  return {};
}

}  // namespace summon::pfm
