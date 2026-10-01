// Power Failure Manager -- delivery of bounded requests to owning controllers.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0 (see LICENSE).
#include "pfm/transport.hpp"

#include <algorithm>
#include <string>

namespace summon::pfm {

std::string_view dispatch_result_name(DispatchResult value) noexcept {
  switch (value) {
    case DispatchResult::Accepted: return "accepted";
    case DispatchResult::Refused: return "refused";
    case DispatchResult::Failed: return "failed";
    case DispatchResult::Unavailable: return "unavailable";
    case DispatchResult::Indeterminate: return "indeterminate";
  }
  return "unknown";
}

Result<DispatchOutcome> RefusingTransport::dispatch(const ResponseRequest& request) {
  // Refusing is not failing: the request was understood and declined, and the
  // runtime records that distinction rather than treating it as an effect.
  return DispatchOutcome{ DispatchResult::Refused, RefToken{},
                          "no adjacent controller is attached: " +
                              std::string{request_kind_name(request.kind)} + " for " +
                              request.target.element.to_string() + " was refused" };
}

void ScriptedTransport::add_rule(Rule rule) {
  std::lock_guard<std::mutex> guard{mutex_};
  rules_.push_back(std::move(rule));
  rule_hits_.assign(rules_.size(), 0);
}

void ScriptedTransport::clear_rules() {
  std::lock_guard<std::mutex> guard{mutex_};
  rules_.clear();
  rule_hits_.clear();
}

void ScriptedTransport::set_default(DispatchResult result, std::string note) {
  std::lock_guard<std::mutex> guard{mutex_};
  default_result_ = result;
  default_note_ = std::move(note);
}

Result<DispatchOutcome> ScriptedTransport::dispatch(const ResponseRequest& request) {
  std::lock_guard<std::mutex> guard{mutex_};
  dispatched_.push_back(request);
  for (std::size_t i = 0; i < rules_.size(); ++i) {
    const auto& rule = rules_[i];
    if (!rule.any_kind && rule.kind != request.kind) {
      continue;
    }
    const std::uint32_t hits = rule_hits_[i]++;
    DispatchOutcome outcome;
    if (rule.indeterminate_first > hits) {
      outcome.result = DispatchResult::Indeterminate;
      outcome.note = "scripted indeterminate answer";
      outcome.controller = rule.controller;
      return outcome;
    }
    outcome.result = rule.result;
    outcome.controller = rule.controller;
    outcome.note = rule.note.empty() ? default_note_ : rule.note;
    return outcome;
  }
  DispatchOutcome outcome;
  outcome.result = default_result_;
  outcome.note = default_note_;
  return outcome;
}

std::vector<ResponseRequest> ScriptedTransport::dispatched() const {
  std::lock_guard<std::mutex> guard{mutex_};
  return dispatched_;
}

std::size_t ScriptedTransport::dispatch_count() const {
  std::lock_guard<std::mutex> guard{mutex_};
  return dispatched_.size();
}

std::size_t ScriptedTransport::count_of(RequestKind kind) const {
  std::lock_guard<std::mutex> guard{mutex_};
  return static_cast<std::size_t>(
      std::count_if(dispatched_.begin(), dispatched_.end(),
                    [kind](const ResponseRequest& request) { return request.kind == kind; }));
}

}  // namespace summon::pfm
