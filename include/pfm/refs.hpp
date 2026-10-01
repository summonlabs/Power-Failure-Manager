// Power Failure Manager -- opaque, canonically validated reference tokens.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0 (see LICENSE).
#pragma once

#include <compare>
#include <cstdint>
#include <string>
#include <string_view>

#include "pfm/status.hpp"

namespace summon::pfm {

// The kind of facility electrical entity a reference designates. PFM never
// discovers topology or names equipment: every reference is supplied by the
// authority that owns it and is treated as opaque, validated, and attributable.
enum class RefKind : std::uint8_t {
  Unknown = 0,
  Site = 1,
  PowerDomain = 2,
  FailureDomain = 3,
  UtilityFeed = 4,
  Switchgear = 5,
  Bus = 6,
  Breaker = 7,
  Circuit = 8,
  Pdu = 9,
  PduBranch = 10,
  UpsUnit = 11,
  UpsBus = 12,
  StaticTransferSwitch = 13,
  AutomaticTransferSwitch = 14,
  Generator = 15,
  LoadGroup = 16,
  Rack = 17,
  Incident = 18,
  Policy = 19,
  Controller = 20,
  Obligation = 21,
};

[[nodiscard]] std::string_view ref_kind_name(RefKind kind) noexcept;
[[nodiscard]] bool ref_kind_from_name(std::string_view name, RefKind& out) noexcept;
[[nodiscard]] bool ref_kind_is_element(RefKind kind) noexcept;

// A bounded, canonical opaque token: a kind plus a name. The name is validated
// against a strict character set and length so that a reference can never be
// a path, an escape sequence, or an unbounded string.
class RefToken {
 public:
  RefToken() = default;

  [[nodiscard]] static Result<RefToken> make(RefKind kind, std::string_view name);
  // Parses "kind:name" as produced by to_string().
  [[nodiscard]] static Result<RefToken> parse(std::string_view text);

  [[nodiscard]] RefKind kind() const noexcept { return kind_; }
  [[nodiscard]] const std::string& name() const noexcept { return name_; }
  [[nodiscard]] bool is_set() const noexcept { return kind_ != RefKind::Unknown && !name_.empty(); }

  [[nodiscard]] std::string to_string() const;

  [[nodiscard]] friend bool operator==(const RefToken& a, const RefToken& b) noexcept {
    return a.kind_ == b.kind_ && a.name_ == b.name_;
  }
  // Canonical ordering is (kind, name), never insertion or hash order.
  [[nodiscard]] friend bool operator<(const RefToken& a, const RefToken& b) noexcept {
    if (a.kind_ != b.kind_) {
      return static_cast<std::uint8_t>(a.kind_) < static_cast<std::uint8_t>(b.kind_);
    }
    return a.name_ < b.name_;
  }
  [[nodiscard]] friend bool operator>(const RefToken& a, const RefToken& b) noexcept { return b < a; }
  [[nodiscard]] friend bool operator<=(const RefToken& a, const RefToken& b) noexcept {
    return !(b < a);
  }
  [[nodiscard]] friend bool operator>=(const RefToken& a, const RefToken& b) noexcept {
    return !(a < b);
  }

 private:
  RefKind kind_{RefKind::Unknown};
  std::string name_{};
};

// Structural bounds for reference names. A reference longer than this is
// refused before any storage is reserved for it.
inline constexpr std::size_t kMaxRefNameLength = 96;
inline constexpr std::size_t kMaxRefTextLength = kMaxRefNameLength + 24;

[[nodiscard]] bool ref_name_is_canonical(std::string_view name) noexcept;

}  // namespace summon::pfm
