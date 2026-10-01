// Power Failure Manager -- authority tokens and deterministic fencing.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0 (see LICENSE).
#pragma once

#include <string>

#include "pfm/ids.hpp"
#include "pfm/status.hpp"

namespace summon::pfm {

// Every authority-bearing mutation carries the identity it was planned
// against. Nothing about authority is inferred from existence, observation,
// acknowledgement, an earlier success, recovered state, apparent health, or
// topology.
struct AuthorityToken {
  IncidentId incident{};
  IncidentGeneration generation{};
  ControlEpoch epoch{};
  ControllerIncarnation incarnation{};
  StateRevision revision{};

  [[nodiscard]] static AuthorityToken from_values(IncidentId incident,
                                                  IncidentGeneration generation,
                                                  ControlEpoch epoch,
                                                  ControllerIncarnation incarnation,
                                                  StateRevision revision) noexcept {
    return AuthorityToken{incident, generation, epoch, incarnation, revision};
  }

  [[nodiscard]] bool has_epoch_and_incarnation() const noexcept {
    return epoch.is_set() && incarnation.is_set();
  }
};

// The durable projection an incoming token is fenced against.
struct AuthorityExpectation {
  // False for operations that establish an incident rather than acting inside
  // one: the incident checks are skipped in both directions.
  bool require_incident{true};
  bool incident_live{false};
  IncidentId incident{};
  IncidentGeneration generation{};
  ControlEpoch epoch{};
  ControllerIncarnation incarnation{};
  StateRevision revision{};
};

// Validation precedence is fixed and documented so that the same invalid
// request always produces the same primary machine-readable code:
//
//   1.  MissingAuthority        zero epoch or zero incarnation
//   2.  StaleEpoch              token epoch below the durable epoch
//   3.  FutureEpoch             token epoch above the durable epoch
//   4.  StaleIncarnation        same epoch, different (older) incarnation
//   5.  NoActiveIncident        incident-bound token with no live incident
//   6.  CrossIncidentAuthority  token incident differs from the live incident
//   7.  StaleGeneration         token generation below the current generation
//   8.  FutureGeneration        token generation above the current generation
//   9.  StaleRevision           observed revision below the current revision
//  10.  FutureRevision          observed revision above the current revision
[[nodiscard]] Result<void> fence_authority(const AuthorityToken& token,
                                           const AuthorityExpectation& expected);

[[nodiscard]] std::string describe_authority(const AuthorityToken& token);
[[nodiscard]] std::string describe_expectation(const AuthorityExpectation& expected);

}  // namespace summon::pfm
