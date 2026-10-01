// Power Failure Manager -- canonical binary encoding primitives.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0 (see LICENSE).
#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include "pfm/classification.hpp"
#include "pfm/evidence.hpp"
#include "pfm/ids.hpp"
#include "pfm/journal.hpp"
#include "pfm/obligations.hpp"
#include "pfm/plan.hpp"
#include "pfm/policy.hpp"
#include "pfm/recovery.hpp"
#include "pfm/refs.hpp"
#include "pfm/request.hpp"
#include "pfm/scope.hpp"
#include "pfm/state.hpp"
#include "pfm/status.hpp"
#include "pfm/topology.hpp"
#include "pfm/units.hpp"

namespace summon::pfm::codec {

// CRC-32C (Castagnoli), reflected, init 0xFFFFFFFF, final xor 0xFFFFFFFF.
[[nodiscard]] std::uint32_t crc32c(std::span<const std::uint8_t> data) noexcept;
[[nodiscard]] std::uint32_t crc32c(std::uint32_t seed, std::span<const std::uint8_t> data) noexcept;

// 128-bit FNV-1a over canonical bytes, used for semantic fingerprints and
// idempotency keys. Chosen for stability, not adversarial collision resistance:
// a fingerprint is always also compared field by field, so a collision can
// never silently satisfy a different request.
[[nodiscard]] Fingerprint fnv1a128(std::span<const std::uint8_t> data) noexcept;

// Canonical writer: little-endian integers, explicit 32-bit counts, no padding.
// Every variable-length field is bounded before it is written.
class Writer {
 public:
  Writer() = default;

  void u8(std::uint8_t value);
  void u16(std::uint16_t value);
  void u32(std::uint32_t value);
  void u64(std::uint64_t value);
  void i64(std::int64_t value);
  void flag(bool value);
  void raw(std::span<const std::uint8_t> data);
  // Length-prefixed text. Latches an error and writes nothing when the text is
  // longer than max_len, so an oversized field can never be encoded.
  bool text(const std::string& value, std::size_t max_len);

  [[nodiscard]] const std::vector<std::uint8_t>& bytes() const noexcept { return bytes_; }
  [[nodiscard]] std::vector<std::uint8_t>& bytes() noexcept { return bytes_; }
  [[nodiscard]] std::size_t size() const noexcept { return bytes_.size(); }
  [[nodiscard]] bool ok() const noexcept { return ok_; }
  [[nodiscard]] const Status& error() const noexcept { return error_; }

  void fail(Status status);

 private:
  std::vector<std::uint8_t> bytes_{};
  bool ok_{true};
  Status error_{};
};

// Strict reader. Every read is bounds-checked, the first failure is latched,
// and later reads become no-ops, so a decoder can be written linearly and
// validated once. Trailing bytes are detected with at_end().
class Reader {
 public:
  explicit Reader(std::span<const std::uint8_t> data) noexcept : data_(data) {}

  [[nodiscard]] std::uint8_t u8();
  [[nodiscard]] std::uint16_t u16();
  [[nodiscard]] std::uint32_t u32();
  [[nodiscard]] std::uint64_t u64();
  [[nodiscard]] std::int64_t i64();
  [[nodiscard]] bool flag();

  [[nodiscard]] std::span<const std::uint8_t> raw(std::size_t n);
  bool text(std::string& out, std::size_t max_len);

  [[nodiscard]] bool ok() const noexcept { return ok_; }
  [[nodiscard]] const Status& error() const noexcept { return error_; }
  [[nodiscard]] std::size_t remaining() const noexcept { return data_.size() - position_; }
  [[nodiscard]] bool at_end() const noexcept { return position_ == data_.size(); }
  [[nodiscard]] std::size_t position() const noexcept { return position_; }

  void fail(Status status);

 private:
  [[nodiscard]] bool need(std::size_t n);

  std::span<const std::uint8_t> data_{};
  std::size_t position_{0};
  bool ok_{true};
  Status error_{};
};

// --- scalar and enum encoders ---------------------------------------------

void encode(Writer& writer, Timestamp value);
void decode(Reader& reader, Timestamp& value);
void encode(Writer& writer, Duration value);
void decode(Reader& reader, Duration& value);
void encode(Writer& writer, BasisPoints value);
void decode(Reader& reader, BasisPoints& value);
void encode(Writer& writer, MilliVolts value);
void decode(Reader& reader, MilliVolts& value);
void encode(Writer& writer, MilliHertz value);
void decode(Reader& reader, MilliHertz& value);
void encode(Writer& writer, MilliAmps value);
void decode(Reader& reader, MilliAmps& value);
void encode(Writer& writer, MilliPercent value);
void decode(Reader& reader, MilliPercent& value);
void encode(Writer& writer, Fingerprint value);
void decode(Reader& reader, Fingerprint& value);
void encode(Writer& writer, IdempotencyKey value);
void decode(Reader& reader, IdempotencyKey& value);
void encode(Writer& writer, const RefToken& value);
void decode(Reader& reader, RefToken& value);

// Strongly typed identities and generations are all 64-bit counters; the tag
// type is erased only here, at the encoding boundary.
template <class Tag, class Rep>
void encode(Writer& writer, Id<Tag, Rep> value) {
  writer.u64(static_cast<std::uint64_t>(value.value()));
}
template <class Tag, class Rep>
void decode(Reader& reader, Id<Tag, Rep>& value) {
  value = Id<Tag, Rep>::from_value(static_cast<Rep>(reader.u64()));
}

void encode(Writer& writer, FailureClass value);
void decode(Reader& reader, FailureClass& value);
void encode(Writer& writer, ReasonCode value);
void decode(Reader& reader, ReasonCode& value);
void encode(Writer& writer, ElementKind value);
void decode(Reader& reader, ElementKind& value);
void encode(Writer& writer, EnergizationState value);
void decode(Reader& reader, EnergizationState& value);
void encode(Writer& writer, BreakerPosition value);
void decode(Reader& reader, BreakerPosition& value);
void encode(Writer& writer, GeneratorState value);
void decode(Reader& reader, GeneratorState& value);
void encode(Writer& writer, TransferState value);
void decode(Reader& reader, TransferState& value);
void encode(Writer& writer, ObservationOrigin value);
void decode(Reader& reader, ObservationOrigin& value);
void encode(Writer& writer, EvidenceQuality value);
void decode(Reader& reader, EvidenceQuality& value);
void encode(Writer& writer, Freshness value);
void decode(Reader& reader, Freshness& value);
void encode(Writer& writer, ProtectionClass value);
void decode(Reader& reader, ProtectionClass& value);
void encode(Writer& writer, ObligationStatus value);
void decode(Reader& reader, ObligationStatus& value);
void encode(Writer& writer, RequestKind value);
void decode(Reader& reader, RequestKind& value);
void encode(Writer& writer, RequestOwner value);
void decode(Reader& reader, RequestOwner& value);
void encode(Writer& writer, ProofKind value);
void decode(Reader& reader, ProofKind& value);
void encode(Writer& writer, RequestState value);
void decode(Reader& reader, RequestState& value);
void encode(Writer& writer, IncidentLifecycle value);
void decode(Reader& reader, IncidentLifecycle& value);
void encode(Writer& writer, RecoveryGate value);
void decode(Reader& reader, RecoveryGate& value);
void encode(Writer& writer, JournalKind value);
void decode(Reader& reader, JournalKind& value);

// --- composite encoders ----------------------------------------------------

void encode(Writer& writer, const TopologyElement& value, const Bounds& bounds);
void decode(Reader& reader, TopologyElement& value, const Bounds& bounds);
void encode(Writer& writer, const FailureDomain& value, const Bounds& bounds);
void decode(Reader& reader, FailureDomain& value, const Bounds& bounds);
void encode(Writer& writer, const TopologySnapshot& value, const Bounds& bounds);
void decode(Reader& reader, TopologySnapshot& value, const Bounds& bounds);

void encode(Writer& writer, const ElectricalObservation& value, const Bounds& bounds);
void decode(Reader& reader, ElectricalObservation& value, const Bounds& bounds);

void encode(Writer& writer, const ProtectedObligation& value, const Bounds& bounds);
void decode(Reader& reader, ProtectedObligation& value, const Bounds& bounds);

void encode(Writer& writer, const ReasonStep& value, const Bounds& bounds);
void decode(Reader& reader, ReasonStep& value, const Bounds& bounds);

void encode(Writer& writer, const RequestTarget& value, const Bounds& bounds);
void decode(Reader& reader, RequestTarget& value, const Bounds& bounds);
void encode(Writer& writer, const ResponseRequest& value, const Bounds& bounds);
void decode(Reader& reader, ResponseRequest& value, const Bounds& bounds);

void encode(Writer& writer, const IsolationRequirement& value, const Bounds& bounds);
void decode(Reader& reader, IsolationRequirement& value, const Bounds& bounds);
void encode(Writer& writer, const ProtectionRequirement& value, const Bounds& bounds);
void decode(Reader& reader, ProtectionRequirement& value, const Bounds& bounds);

void encode(Writer& writer, const AffectedScope& value, const Bounds& bounds);
void decode(Reader& reader, AffectedScope& value, const Bounds& bounds);
void encode(Writer& writer, const ResponsePlan& value, const Bounds& bounds);
void decode(Reader& reader, ResponsePlan& value, const Bounds& bounds);
void encode(Writer& writer, const GateEvaluation& value, const Bounds& bounds);
void decode(Reader& reader, GateEvaluation& value, const Bounds& bounds);
void encode(Writer& writer, const RecoveryAssessment& value, const Bounds& bounds);
void decode(Reader& reader, RecoveryAssessment& value, const Bounds& bounds);

void encode(Writer& writer, const ElectricalPolicy& value);
void decode(Reader& reader, ElectricalPolicy& value);
void encode(Writer& writer, const Bounds& value);
void decode(Reader& reader, Bounds& value);

void encode(Writer& writer, const ObservationSlot& value, const Bounds& bounds);
void decode(Reader& reader, ObservationSlot& value, const Bounds& bounds);
void encode(Writer& writer, const RequestSlot& value, const Bounds& bounds);
void decode(Reader& reader, RequestSlot& value, const Bounds& bounds);
void encode(Writer& writer, const ObligationSlot& value, const Bounds& bounds);
void decode(Reader& reader, ObligationSlot& value, const Bounds& bounds);
void encode(Writer& writer, const RelaxationRecord& value, const Bounds& bounds);
void decode(Reader& reader, RelaxationRecord& value, const Bounds& bounds);
void encode(Writer& writer, const TransitionRecord& value, const Bounds& bounds);
void decode(Reader& reader, TransitionRecord& value, const Bounds& bounds);
void encode(Writer& writer, const IncidentProjection& value, const Bounds& bounds);
void decode(Reader& reader, IncidentProjection& value, const Bounds& bounds);

void encode(Writer& writer, const DomainState& value, const Bounds& bounds);
void decode(Reader& reader, DomainState& value, const Bounds& bounds);

void encode(Writer& writer, const JournalPayload& value, const Bounds& bounds);
void decode(Reader& reader, JournalPayload& value, const Bounds& bounds);
void encode(Writer& writer, const JournalEntry& value, const Bounds& bounds);
void decode(Reader& reader, JournalEntry& value, const Bounds& bounds);

}  // namespace summon::pfm::codec

namespace summon::pfm {

// Decodes a complete domain state from canonical bytes. Rejects trailing bytes.
[[nodiscard]] Result<DomainState> decode_state(std::span<const std::uint8_t> bytes,
                                               const Bounds& bounds);

}  // namespace summon::pfm
