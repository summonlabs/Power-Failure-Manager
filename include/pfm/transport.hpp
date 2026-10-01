// Power Failure Manager -- delivery of bounded requests to owning controllers.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0 (see LICENSE).
#pragma once

#include <atomic>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "pfm/plan.hpp"
#include "pfm/request.hpp"
#include "pfm/status.hpp"

namespace summon::pfm {

// What a controller answered. Accepted means the controller took the request;
// it is not evidence that the effect happened.
enum class DispatchResult : std::uint8_t {
  Accepted = 0,
  Refused = 1,
  Failed = 2,
  Unavailable = 3,
  // The controller did not answer, or the outcome is unknowable. Never
  // interpreted as success, and never retried blindly.
  Indeterminate = 4,
};

[[nodiscard]] std::string_view dispatch_result_name(DispatchResult value) noexcept;

struct DispatchOutcome {
  DispatchResult result{DispatchResult::Indeterminate};
  RefToken controller{};
  std::string note{};

  [[nodiscard]] static DispatchOutcome accepted(RefToken controller, std::string note) {
    return DispatchOutcome{DispatchResult::Accepted, std::move(controller), std::move(note)};
  }
  [[nodiscard]] static DispatchOutcome refused(RefToken controller, std::string note) {
    return DispatchOutcome{DispatchResult::Refused, std::move(controller), std::move(note)};
  }
  [[nodiscard]] static DispatchOutcome indeterminate(std::string note) {
    return DispatchOutcome{DispatchResult::Indeterminate, RefToken{}, std::move(note)};
  }
};

// The adjacent-controller interface. PFM calls it, records the answer, and
// never treats the answer as proof of an electrical effect.
class ResponseTransport {
 public:
  ResponseTransport() = default;
  virtual ~ResponseTransport() = default;
  ResponseTransport(const ResponseTransport&) = delete;
  ResponseTransport& operator=(const ResponseTransport&) = delete;
  ResponseTransport(ResponseTransport&&) = delete;
  ResponseTransport& operator=(ResponseTransport&&) = delete;

  [[nodiscard]] virtual Result<DispatchOutcome> dispatch(const ResponseRequest& request) = 0;

  // Called after a plan is durably published, outside every runtime lock.
  virtual void on_plan_published(const ResponsePlan& /*plan*/) {}
};

// A transport that refuses everything. It exists so that a runtime can be
// opened without an adjacent controller, and it never implies an effect.
class RefusingTransport final : public ResponseTransport {
 public:
  [[nodiscard]] Result<DispatchOutcome> dispatch(const ResponseRequest& request) override;
};

// A deterministic, scripted stand-in for an adjacent controller. Every answer
// it gives is synthetic and labelled as such; it is the transport used by the
// scenario engine, the example, and the tests.
class ScriptedTransport final : public ResponseTransport {
 public:
  struct Rule {
    RequestKind kind{RequestKind::Unknown};
    bool any_kind{false};
    DispatchResult result{DispatchResult::Accepted};
    RefToken controller{};
    std::string note{};
    // When set, the first N matching dispatches answer Indeterminate, which is
    // how a crash around dispatch is modelled without a crash.
    std::uint32_t indeterminate_first{0};
  };

  ScriptedTransport() = default;

  void add_rule(Rule rule);
  void clear_rules();
  // Answer used when no rule matches.
  void set_default(DispatchResult result, std::string note);

  [[nodiscard]] Result<DispatchOutcome> dispatch(const ResponseRequest& request) override;

  [[nodiscard]] std::vector<ResponseRequest> dispatched() const;
  [[nodiscard]] std::size_t dispatch_count() const;
  [[nodiscard]] std::size_t count_of(RequestKind kind) const;

 private:
  mutable std::mutex mutex_{};
  std::vector<Rule> rules_{};
  std::vector<ResponseRequest> dispatched_{};
  std::vector<std::uint32_t> rule_hits_{};
  DispatchResult default_result_{DispatchResult::Accepted};
  std::string default_note_{};
};

// Detects a transport that calls back into the runtime, on the dispatching
// thread, while a dispatch is in flight. Such a call is refused with
// ReentrancyRefused instead of deadlocking.
//
// The gate is thread-aware on purpose: an unrelated thread that mutates the
// runtime while another thread is dispatching is not re-entering anything. It
// waits for the runtime mutex exactly as it would at any other time, and the
// write-ahead intent already committed keeps the two orderings safe.
class DispatchGate {
 public:
  [[nodiscard]] bool enter() noexcept {
    std::thread::id expected{};
    return owner_.compare_exchange_strong(expected, std::this_thread::get_id());
  }
  void leave() noexcept { owner_.store(std::thread::id{}); }
  [[nodiscard]] bool in_dispatch() const noexcept { return owner_.load() != std::thread::id{}; }
  [[nodiscard]] bool called_from_dispatch_thread() const noexcept {
    return owner_.load() == std::this_thread::get_id();
  }

 private:
  std::atomic<std::thread::id> owner_{};
};

}  // namespace summon::pfm
