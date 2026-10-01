// Power Failure Manager -- authority fencing.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0 (see LICENSE).
#include "pfm/authority.hpp"

#include <string>

namespace summon::pfm {
namespace {

Status fence_error(StatusCode code, std::string message, std::string context) {
  return Status::error(code, std::move(message)).with_context(std::move(context));
}

}  // namespace

Result<void> fence_authority(const AuthorityToken& token, const AuthorityExpectation& expected) {
  // The order of these checks is a documented contract, not an implementation
  // detail: the same invalid request always yields the same primary code.
  if (!token.has_epoch_and_incarnation()) {
    return fence_error(StatusCode::MissingAuthority,
                       "authority token has no control epoch or controller incarnation",
                       "authority.epoch");
  }
  if (token.epoch < expected.epoch) {
    return fence_error(StatusCode::StaleEpoch, "authority epoch is behind the durable epoch",
                       "authority.epoch");
  }
  if (token.epoch > expected.epoch) {
    return fence_error(StatusCode::FutureEpoch,
                       "authority epoch is ahead of the durable epoch; roll the epoch first",
                       "authority.epoch");
  }
  if (token.incarnation != expected.incarnation) {
    return fence_error(StatusCode::StaleIncarnation,
                       "controller incarnation does not match the durable incarnation",
                       "authority.incarnation");
  }
  if (token.incident.is_absent() && token.generation.is_set()) {
    return fence_error(StatusCode::InvalidArgument,
                       "an incident generation without an incident identity is meaningless",
                       "authority.incident");
  }
  if (!expected.require_incident) {
    return {};
  }
  if (!expected.incident_live) {
    if (token.incident.is_set()) {
      return fence_error(StatusCode::NoActiveIncident,
                         "a token names an incident but no incident is live",
                         "authority.incident");
    }
  } else {
    if (token.incident.is_absent()) {
      return fence_error(StatusCode::NoActiveIncident,
                         "operation requires an incident-bound token",
                         "authority.incident");
    }
    if (!(token.incident == expected.incident)) {
      return fence_error(StatusCode::CrossIncidentAuthority,
                         "token incident differs from the live incident",
                         "authority.incident");
    }
    if (token.generation < expected.generation) {
      return fence_error(StatusCode::StaleGeneration,
                         "incident generation is behind the live generation",
                         "authority.generation");
    }
    if (token.generation > expected.generation) {
      return fence_error(StatusCode::FutureGeneration,
                         "incident generation is ahead of the live generation",
                         "authority.generation");
    }
  }
  if (token.revision < expected.revision) {
    return fence_error(StatusCode::StaleRevision,
                       "observed state revision is behind the durable revision",
                       "authority.revision");
  }
  if (token.revision > expected.revision) {
    return fence_error(StatusCode::FutureRevision,
                       "observed state revision is ahead of the durable revision",
                       "authority.revision");
  }
  return {};
}

std::string describe_authority(const AuthorityToken& token) {
  std::string text;
  text.append("incident=").append(std::to_string(token.incident.value()));
  text.append(" generation=").append(std::to_string(token.generation.value()));
  text.append(" epoch=").append(std::to_string(token.epoch.value()));
  text.append(" incarnation=").append(std::to_string(token.incarnation.value()));
  text.append(" revision=").append(std::to_string(token.revision.value()));
  return text;
}

std::string describe_expectation(const AuthorityExpectation& expected) {
  std::string text;
  text.append("live=").append(expected.incident_live ? "yes" : "no");
  text.append(" incident=").append(std::to_string(expected.incident.value()));
  text.append(" generation=").append(std::to_string(expected.generation.value()));
  text.append(" epoch=").append(std::to_string(expected.epoch.value()));
  text.append(" incarnation=").append(std::to_string(expected.incarnation.value()));
  text.append(" revision=").append(std::to_string(expected.revision.value()));
  return text;
}

}  // namespace summon::pfm
