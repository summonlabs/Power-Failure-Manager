// Power Failure Manager -- version identity.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0 (see LICENSE).
#pragma once

#include <cstdint>
#include <string_view>

namespace summon::pfm {

inline constexpr std::uint32_t kVersionMajor = 1;
inline constexpr std::uint32_t kVersionMinor = 0;
inline constexpr std::uint32_t kVersionPatch = 0;

// DCCP program boundary owned by this repository.
inline constexpr std::uint32_t kDccpBoundary = 53;

// Bumped only when the persisted format changes incompatibly. Refusing an
// unknown format version is required; silently accepting one is not.
inline constexpr std::uint32_t kStoreFormatVersion = 1;

[[nodiscard]] std::string_view version_string() noexcept;
[[nodiscard]] std::string_view component_name() noexcept;

}  // namespace summon::pfm
