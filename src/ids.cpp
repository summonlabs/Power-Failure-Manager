// Power Failure Manager -- identity rendering.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0 (see LICENSE).
#include "pfm/ids.hpp"

#include <array>
#include <cstdio>

namespace summon::pfm {
namespace {

std::string hex128(std::uint64_t high, std::uint64_t low) {
  std::array<char, 33> buffer{};
  std::snprintf(buffer.data(), buffer.size(), "%016llx%016llx",
                static_cast<unsigned long long>(high), static_cast<unsigned long long>(low));
  return std::string{buffer.data()};
}

}  // namespace

std::string Fingerprint::to_hex() const { return hex128(high_, low_); }

std::string IdempotencyKey::to_hex() const { return hex128(high_, low_); }

}  // namespace summon::pfm
