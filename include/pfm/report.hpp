// Power Failure Manager -- deterministic text rendering.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0 (see LICENSE).
#pragma once

#include <string>
#include <vector>

#include "pfm/classification.hpp"
#include "pfm/plan.hpp"
#include "pfm/recovery.hpp"
#include "pfm/state.hpp"
#include "pfm/store.hpp"

namespace summon::pfm {

// Rendering is for inspection and audit output. Canonical byte-for-byte
// determinism is claimed for the persisted encoding and for replay; text
// rendering is deterministic in practice but is not part of that contract.
[[nodiscard]] std::string render_classification(const ClassificationResult& classification);
[[nodiscard]] std::string render_scope(const AffectedScope& scope);
[[nodiscard]] std::string render_plan(const ResponsePlan& plan);
[[nodiscard]] std::string render_requests(const std::vector<ResponseRequest>& requests);
[[nodiscard]] std::string render_request(const ResponseRequest& request);
[[nodiscard]] std::string render_recovery(const RecoveryAssessment& assessment);
[[nodiscard]] std::string render_state(const DomainState& state);
[[nodiscard]] std::string render_store_status(const StoreStatus& status);
[[nodiscard]] std::string render_evidence(const ElectricalObservation& observation);

}  // namespace summon::pfm
