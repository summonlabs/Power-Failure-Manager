// Power Failure Manager -- version identity.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0 (see LICENSE).
#include "pfm/version.hpp"

namespace summon::pfm {

std::string_view version_string() noexcept { return "1.0.0"; }

std::string_view component_name() noexcept { return "Power Failure Manager"; }

}  // namespace summon::pfm
