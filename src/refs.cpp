// Power Failure Manager -- reference tokens.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0 (see LICENSE).
#include "pfm/refs.hpp"

#include <array>
#include <cctype>

namespace summon::pfm {
namespace {

struct KindEntry {
  RefKind kind;
  std::string_view name;
  bool element;
};

constexpr KindEntry kKinds[] = {
    {RefKind::Unknown, "unknown", false},
    {RefKind::Site, "site", false},
    {RefKind::PowerDomain, "power-domain", false},
    {RefKind::FailureDomain, "failure-domain", false},
    {RefKind::UtilityFeed, "utility-feed", true},
    {RefKind::Switchgear, "switchgear", true},
    {RefKind::Bus, "bus", true},
    {RefKind::Breaker, "breaker", true},
    {RefKind::Circuit, "circuit", true},
    {RefKind::Pdu, "pdu", true},
    {RefKind::PduBranch, "pdu-branch", true},
    {RefKind::UpsUnit, "ups", true},
    {RefKind::UpsBus, "ups-bus", true},
    {RefKind::StaticTransferSwitch, "sts", true},
    {RefKind::AutomaticTransferSwitch, "ats", true},
    {RefKind::Generator, "generator", true},
    {RefKind::LoadGroup, "load-group", false},
    {RefKind::Rack, "rack", false},
    {RefKind::Incident, "incident", false},
    {RefKind::Policy, "policy", false},
    {RefKind::Controller, "controller", false},
    {RefKind::Obligation, "obligation", false},
};

// A reference name is an opaque token, never a path: separators, traversal
// sequences, control characters, and non-ASCII bytes are all refused.
bool canonical_char(char c) noexcept {
  const auto uc = static_cast<unsigned char>(c);
  if (uc >= 'a' && uc <= 'z') {
    return true;
  }
  if (uc >= 'A' && uc <= 'Z') {
    return true;
  }
  if (uc >= '0' && uc <= '9') {
    return true;
  }
  return c == '.' || c == '_' || c == '-' || c == ':' || c == '@' || c == '#';
}

}  // namespace

std::string_view ref_kind_name(RefKind kind) noexcept {
  for (const auto& entry : kKinds) {
    if (entry.kind == kind) {
      return entry.name;
    }
  }
  return "unknown";
}

bool ref_kind_from_name(std::string_view name, RefKind& out) noexcept {
  for (const auto& entry : kKinds) {
    if (entry.name == name) {
      out = entry.kind;
      return true;
    }
  }
  return false;
}

bool ref_kind_is_element(RefKind kind) noexcept {
  for (const auto& entry : kKinds) {
    if (entry.kind == kind) {
      return entry.element;
    }
  }
  return false;
}

bool ref_name_is_canonical(std::string_view name) noexcept {
  if (name.empty() || name.size() > kMaxRefNameLength) {
    return false;
  }
  if (name.front() == '.' || name.front() == ':' || name.front() == '-' ||
      name.front() == '_') {
    return false;
  }
  if (name.back() == '.' || name.back() == ':' || name.back() == '-' || name.back() == '_') {
    return false;
  }
  if (name.find("..") != std::string_view::npos) {
    return false;
  }
  for (const char c : name) {
    if (!canonical_char(c)) {
      return false;
    }
  }
  return true;
}

Result<RefToken> RefToken::make(RefKind kind, std::string_view name) {
  if (kind == RefKind::Unknown) {
    return Status::error(StatusCode::InvalidArgument, "reference kind is unknown")
        .with_context("ref.kind");
  }
  if (name.empty()) {
    return Status::error(StatusCode::EmptyField, "reference name is empty")
        .with_context("ref.name");
  }
  if (name.size() > kMaxRefNameLength) {
    return Status::error(StatusCode::FieldTooLong, "reference name is too long")
        .with_context("ref.name");
  }
  if (!ref_name_is_canonical(name)) {
    return Status::error(StatusCode::InvalidCharacter,
                         "reference name is not a canonical opaque token")
        .with_context(std::string{name});
  }
  RefToken token;
  token.kind_ = kind;
  token.name_.assign(name);
  return token;
}

Result<RefToken> RefToken::parse(std::string_view text) {
  if (text.size() > kMaxRefTextLength) {
    return Status::error(StatusCode::FieldTooLong, "reference text is too long")
        .with_context("ref.text");
  }
  const auto separator = text.find(':');
  if (separator == std::string_view::npos || separator == 0 || separator + 1 >= text.size()) {
    return Status::error(StatusCode::MalformedEncoding,
                         "reference text is not \"kind:name\"")
        .with_context(std::string{text});
  }
  RefKind kind = RefKind::Unknown;
  if (!ref_kind_from_name(text.substr(0, separator), kind)) {
    return Status::error(StatusCode::UnknownEnumValue, "reference kind is not known")
        .with_context(std::string{text.substr(0, separator)});
  }
  return make(kind, text.substr(separator + 1));
}

std::string RefToken::to_string() const {
  if (!is_set()) {
    return "unknown:";
  }
  std::string text{ref_kind_name(kind_)};
  text.push_back(':');
  text.append(name_);
  return text;
}

}  // namespace summon::pfm
