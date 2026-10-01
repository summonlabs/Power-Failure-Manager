// Power Failure Manager -- the public runtime.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0 (see LICENSE).
#include "pfm/runtime.hpp"

#include <algorithm>
#include <map>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include "pfm/report.hpp"

namespace summon::pfm {
namespace {

Status closed_error() {
  return Status::error(StatusCode::RuntimeClosed, "the runtime has been shut down")
      .with_context("runtime");
}

Status fenced_error() {
  return Status::error(StatusCode::AuthorityFenced,
                       "the runtime is fenced after a failed durable commit; reopen it from the "
                       "store")
      .with_context("runtime");
}

bool contains_ref(const std::vector<RefToken>& values, const RefToken& ref) {
  return std::binary_search(values.begin(), values.end(), ref);
}

}  // namespace

RuntimeOptions RuntimeOptions::volatile_memory() {
  RuntimeOptions options;
  options.mode = StoreMode::Volatile;
  options.epoch = ControlEpoch::from_value(1);
  options.incarnation = ControllerIncarnation::from_value(1);
  return options;
}

struct PowerFailureRuntime::Impl {
  mutable std::mutex mutex{};
  RuntimeOptions options{};
  std::shared_ptr<Clock> clock{};
  std::shared_ptr<ResponseTransport> transport{};
  std::unique_ptr<DurableStore> store{};
  DispatchGate gate{};

  bool closed{false};
  bool poisoned{false};

  DomainState state{};
  DomainState checkpoint{};
  std::vector<JournalEntry> journal{};
  std::uint64_t retired_journal_entries{0};
  JournalSequence next_sequence{};
  // Revision reserved by entries that have been built but not yet applied.
  // Within one batch every entry must follow the revision the previous entry
  // will produce, not the revision the state still carries.
  StateRevision pending_revision{};

  // Derived, rebuilt on demand; never persisted.
  TopologyIndex topology{};
  bool has_topology_index{false};

};

namespace {

using Impl = PowerFailureRuntime::Impl;

Result<Timestamp> now_of(const Impl& impl) {
  if (!impl.clock) {
    return Status::error(StatusCode::Internal, "the runtime has no clock").with_context("runtime");
  }
  return impl.clock->now();
}

// Returns a plain Status so that every caller can return it from any Result
// type without an extra unwrap.
Status fence(const Impl& impl, const AuthorityToken& token, bool incident_bound) {
  AuthorityExpectation expected = expectation_of(impl.state);
  expected.require_incident = incident_bound;
  return fence_authority(token, expected).status_or_ok();
}

void rebuild_topology(Impl& impl) {
  impl.has_topology_index = false;
  if (!impl.state.has_topology) {
    return;
  }
  auto index = TopologyIndex::build(impl.state.topology, impl.state.bounds);
  if (index.ok()) {
    impl.topology = std::move(index.value());
    impl.has_topology_index = true;
  }
}

// Applies a batch of entries and publishes them with exactly one commit. The
// batch is atomic from the caller's point of view: a failure anywhere restores
// the previous in-memory state and leaves the durable generation untouched.
Result<void> apply_and_commit(Impl& impl, std::vector<JournalEntry>& entries) {
  // The batch is already built, so the reserved revision is retired here even
  // when the batch is empty; otherwise a later batch would inherit it.
  impl.pending_revision = StateRevision{};
  if (entries.empty()) {
    return {};
  }
  DomainState backup = impl.state;
  const std::size_t journal_size = impl.journal.size();
  for (auto& entry : entries) {
    auto applied = apply_journal_entry(impl.state, entry);
    if (!applied.ok()) {
      impl.state = std::move(backup);
      impl.journal.resize(journal_size);
      impl.pending_revision = StateRevision{};
      return applied.status();
    }
    impl.journal.push_back(entry);
  }
  // A store that has never published has no checkpoint yet, and a
  // default-constructed state carries no control epoch, no incarnation, and no
  // policy, so it cannot be validated and cannot carry a replay. The first
  // batch is therefore retired into the checkpoint of the new authority, which
  // makes the checkpoint a complete state from the very first commit. The
  // journal stays bounded the same way afterwards: the oldest entries are
  // folded into the checkpoint, so replay always starts from a complete
  // checkpoint.
  if (impl.checkpoint.epoch.is_absent() ||
      impl.journal.size() > impl.state.bounds.max_journal_entries) {
    impl.retired_journal_entries += impl.journal.size();
    impl.checkpoint = impl.state;
    impl.journal.clear();
  }
  StoreSnapshot snapshot;
  snapshot.checkpoint = impl.checkpoint;
  snapshot.live = impl.state;
  snapshot.journal = impl.journal;
  snapshot.retired_journal_entries = impl.retired_journal_entries;
  auto committed = impl.store->commit(snapshot);
  impl.pending_revision = StateRevision{};
  if (!committed.ok()) {
    impl.state = std::move(backup);
    impl.journal.resize(journal_size);
    impl.poisoned = true;
    return committed.status();
  }
  return {};
}

// Publishes the current state as the new checkpoint and retires the journal.
// A checkpoint is a whole authoritative generation, so it is never an
// empty placeholder.
Result<void> fold_journal(Impl& impl) {
  impl.pending_revision = StateRevision{};
  impl.retired_journal_entries += impl.journal.size();
  impl.checkpoint = impl.state;
  impl.journal.clear();
  StoreSnapshot snapshot;
  snapshot.checkpoint = impl.checkpoint;
  snapshot.live = impl.state;
  snapshot.journal = impl.journal;
  snapshot.retired_journal_entries = impl.retired_journal_entries;
  auto committed = impl.store->commit(snapshot);
  if (!committed.ok()) {
    impl.poisoned = true;
    return committed.status();
  }
  return {};
}

Result<JournalEntry> make_entry(Impl& impl, JournalKind kind, Timestamp recorded_at,
                                JournalPayload payload) {
  JournalEntry entry;
  entry.sequence = JournalSequence::from_value(impl.next_sequence.value() + 1);
  entry.kind = kind;
  entry.recorded_at = recorded_at;
  const StateRevision base =
      impl.pending_revision.is_set() ? impl.pending_revision : impl.state.revision;
  entry.revision = StateRevision::from_value(base.value() + 1);
  entry.payload = std::move(payload);
  entry.fingerprint = journal_entry_fingerprint(entry);
  if (!entry.fingerprint.is_set()) {
    return Status::error(StatusCode::Internal, "the journal entry could not be fingerprinted")
        .with_context("journal.fingerprint");
  }
  if (auto r = validate(entry, impl.state.bounds); !r.ok()) {
    return r.status();
  }
  // The sequence and the revision are reserved only once the entry is valid,
  // so a refused entry never advances the identity of the next one.
  impl.pending_revision = entry.revision;
  impl.next_sequence = entry.sequence;
  return entry;
}

Result<void> mutate(Impl& impl, JournalKind kind, Timestamp recorded_at, JournalPayload payload) {
  auto entry = make_entry(impl, kind, recorded_at, std::move(payload));
  if (!entry.ok()) {
    return entry.status();
  }
  std::vector<JournalEntry> entries;
  entries.push_back(std::move(entry.value()));
  return apply_and_commit(impl, entries);
}

std::vector<RefToken> unevidenced_elements(const DomainState& state, Timestamp now) {
  std::vector<RefToken> result;
  for (const auto& slot : state.observations) {
    if (!slot.present) {
      continue;
    }
    if (slot.recovered || slot.contradicted ||
        classify_freshness(slot.observation.observed_at, now, state.policy) != Freshness::Fresh) {
      result.push_back(slot.element);
    }
  }
  std::sort(result.begin(), result.end());
  result.erase(std::unique(result.begin(), result.end()), result.end());
  return result;
}

std::vector<RefToken> unhealthy_scope_elements(const DomainState& state, Timestamp now) {
  std::vector<RefToken> result;
  const auto current = current_observations(state, now, state.policy);
  for (const auto& element : state.incident.impacted_scope) {
    if (element.kind() == RefKind::FailureDomain) {
      continue;
    }
    const ElectricalObservation* observation = nullptr;
    for (const auto& candidate : current) {
      if (candidate.element == element) {
        observation = &candidate;
      }
    }
    if (observation == nullptr || !element_kind_is_supply(observation->kind)) {
      continue;
    }
    ReasonCode reason = ReasonCode::None;
    if (!element_is_healthy(*observation, observation->kind, state.policy, reason)) {
      result.push_back(element);
    }
  }
  return result;
}

std::vector<ResponseRequest> open_requests(const DomainState& state) {
  std::vector<ResponseRequest> result;
  for (const auto& slot : state.requests) {
    if (request_state_is_open(slot.request.state)) {
      result.push_back(slot.request);
    }
  }
  return result;
}

Result<RecoveryAssessment> assess(const Impl& impl, Timestamp now) {
  RecoveryInputs inputs;
  inputs.now = now;
  inputs.policy = impl.state.policy;
  inputs.bounds = impl.state.bounds;
  inputs.plan = impl.state.has_plan ? &impl.state.plan : nullptr;
  inputs.current_observations = current_observations(impl.state, now, impl.state.policy);
  for (const auto& slot : impl.state.obligations) {
    inputs.obligations.push_back(slot.obligation);
  }
  inputs.has_stable_since = impl.state.incident.has_stable_since;
  inputs.stable_since = impl.state.incident.stable_since;
  inputs.operator_authorized = impl.state.incident.operator_authorized;
  inputs.authorized_at = impl.state.incident.authorized_at;
  inputs.authorized_generation = impl.state.incident.authorized_generation;
  inputs.incident_generation = impl.state.incident.generation;
  inputs.incident_scope = impl.state.incident.impacted_scope;
  return assess_recovery(inputs);
}

const ResponseRequest* find_open_request_by_fingerprint(const DomainState& state,
                                                        Fingerprint fingerprint) {
  for (const auto& slot : state.requests) {
    if (request_state_is_open(slot.request.state) && slot.request.fingerprint == fingerprint) {
      return &slot.request;
    }
  }
  return nullptr;
}

// Adopts externally confirmed evidence for a request: the effect is recorded
// only from evidence that is current and carries an external origin.
Result<void> apply_effect_evidence(Impl& impl, const ResponseRequest& request,
                                   const ElectricalObservation& evidence, Timestamp now,
                                   bool verified) {
  std::vector<JournalEntry> entries;
  {
    JournalPayload payload;
    payload.has_observation = true;
    payload.observation = evidence;
    auto entry = make_entry(impl, JournalKind::EvidenceAdmitted, now, payload);
    if (!entry.ok()) {
      return entry.status();
    }
    entries.push_back(std::move(entry.value()));
  }
  {
    JournalPayload payload;
    payload.request = request.id;
    payload.has_request_state = true;
    payload.from_state = request.state;
    payload.to_state = verified ? RequestState::Verified : RequestState::Observed;
    payload.source = evidence.element;
    payload.evidence = evidence.evidence_fingerprint;
    payload.attempt = request.attempt;
    payload.reason = verified ? ReasonCode::IndeterminateDispatchResolved : ReasonCode::None;
    payload.note = verified ? "effect confirmed by current external evidence"
                            : "effect observed; awaiting the required proof";
    auto entry = make_entry(impl, verified ? JournalKind::RequestVerified : JournalKind::RequestObserved,
                            now, payload);
    if (!entry.ok()) {
      return entry.status();
    }
    entries.push_back(std::move(entry.value()));
  }
  return apply_and_commit(impl, entries);
}

}  // namespace

PowerFailureRuntime::~PowerFailureRuntime() {
  if (impl_) {
    static_cast<void>(shutdown());
  }
}

Result<std::unique_ptr<PowerFailureRuntime>> PowerFailureRuntime::open(
    const RuntimeOptions& options, std::shared_ptr<Clock> clock,
    std::shared_ptr<ResponseTransport> transport) {
  if (auto r = validate(options.bounds); !r.ok()) {
    return r.status();
  }
  if (auto r = validate(options.policy); !r.ok()) {
    return r.status();
  }
  if (!clock) {
    return Status::error(StatusCode::InvalidArgument, "the runtime requires a clock")
        .with_context("runtime.clock");
  }
  if (options.mode == StoreMode::Durable && options.store_directory.empty()) {
    return Status::error(StatusCode::StorePathInvalid,
                         "a durable runtime requires a store directory")
        .with_context("runtime.store");
  }
  if (options.epoch.is_absent() || options.incarnation.is_absent()) {
    return Status::error(StatusCode::MissingAuthority,
                         "opening a runtime establishes authority, so an epoch and an "
                         "incarnation are required")
        .with_context("runtime.authority");
  }

  auto runtime = std::unique_ptr<PowerFailureRuntime>{new PowerFailureRuntime{}};
  runtime->impl_ = std::make_unique<Impl>();
  Impl& impl = *runtime->impl_;
  impl.options = options;
  impl.clock = std::move(clock);
  impl.transport = std::move(transport);
  impl.state.bounds = options.bounds;
  impl.state.policy = options.policy;
  impl.state.policy_generation = options.policy.generation;

  StoreOptions store_options;
  store_options.directory = options.store_directory;
  store_options.mode = options.mode;
  store_options.bounds = options.bounds;
  auto store = DurableStore::open(store_options);
  if (!store.ok()) {
    return store.status();
  }
  impl.store = std::move(store.value());

  auto instant = impl.clock->now();
  if (!instant.ok()) {
    return instant.status();
  }
  const Timestamp now = instant.value();

  bool rollover = true;
  bool existing = false;
  if (options.mode == StoreMode::Durable) {
    auto loaded = impl.store->load();
    if (loaded.ok()) {
      existing = true;
      const auto durable_epoch = loaded.value().live.epoch;
      const auto durable_incarnation = loaded.value().live.incarnation;
      if (options.epoch < durable_epoch) {
        return Status::error(StatusCode::StaleEpoch,
                             "the requested control epoch is behind the durable epoch")
            .with_context("runtime.epoch");
      }
      if (options.epoch > durable_epoch) {
        rollover = true;
      } else {
        rollover = false;
        if (!(options.incarnation == durable_incarnation)) {
          return Status::error(StatusCode::StaleIncarnation,
                               "the durable store is held under a different incarnation; a new "
                               "controller must roll the control epoch explicitly")
              .with_context("runtime.incarnation");
        }
      }
      impl.state = loaded.value().live;
      impl.state.bounds = options.bounds;
      impl.checkpoint = loaded.value().checkpoint;
      impl.journal = loaded.value().journal;
      impl.retired_journal_entries = loaded.value().retired_journal_entries;
      impl.next_sequence =
          impl.journal.empty() ? JournalSequence::from_value(1)
                               : JournalSequence::from_value(impl.journal.back().sequence.value() + 1);
    } else if (loaded.status().code() != StatusCode::StoreNotFound) {
      // Only a store that holds no slot at all is a fresh store. A store whose
      // slots exist but cannot be used is refused: silently starting a new
      // generation over an unreadable one would discard authoritative history.
      return loaded.status();
    }
  }
  // Establishing authority is itself a durable decision. On a resume the same
  // controller keeps its epoch and incarnation, and every in-flight dispatch
  // becomes indeterminate rather than being repeated; on a rollover the new
  // authority supersedes them.
  std::vector<JournalEntry> bootstrap;
  {
    JournalPayload payload;
    payload.epoch = options.epoch;
    payload.incarnation = options.incarnation;
    payload.epoch_rollover = rollover;
    auto entry = make_entry(impl, JournalKind::EpochRolled, now, payload);
    if (!entry.ok()) {
      return entry.status();
    }
    bootstrap.push_back(std::move(entry.value()));
  }
  if (!impl.state.has_topology || !(impl.state.policy == options.policy)) {
    JournalPayload payload;
    payload.has_policy = true;
    payload.policy = options.policy;
    payload.policy.generation =
        options.policy.generation.is_set() ? options.policy.generation
                                           : PolicyGeneration::from_value(1);
    auto entry = make_entry(impl, JournalKind::PolicyPublished, now, payload);
    if (!entry.ok()) {
      return entry.status();
    }
    bootstrap.push_back(std::move(entry.value()));
  }
  if (existing) {
    if (auto r = apply_and_commit(impl, bootstrap); !r.ok()) {
      return r.status();
    }
  } else {
    // Nothing has ever been published in this store, so the bootstrap is
    // applied in memory and published as one complete generation: the first
    // durable generation is a whole authoritative state, never a partly built
    // one, and a checkpoint is never an empty placeholder.
    for (auto& entry : bootstrap) {
      if (auto r = apply_journal_entry(impl.state, entry); !r.ok()) {
        return r.status();
      }
      impl.journal.push_back(entry);
    }
    if (auto r = fold_journal(impl); !r.ok()) {
      return r.status();
    }
  }
  rebuild_topology(impl);
  refresh_currency(impl.state, now);
  return runtime;
}

Result<void> PowerFailureRuntime::shutdown() {
  if (!impl_) {
    return {};
  }
  Impl& impl = *impl_;
  std::lock_guard<std::mutex> guard{impl.mutex};
  if (impl.closed) {
    return {};
  }
  impl.closed = true;
  if (impl.store) {
    auto closed = impl.store->close();
    if (!closed.ok()) {
      return closed.status();
    }
  }
  return {};
}

// --- read-only ------------------------------------------------------------

Result<RuntimeStatus> PowerFailureRuntime::status() const {
  if (!impl_) {
    return closed_error();
  }
  Impl& impl = *impl_;
  std::lock_guard<std::mutex> guard{impl.mutex};
  RuntimeStatus status;
  status.mode = impl.options.mode;
  status.store_directory = impl.store->status().directory;
  status.epoch = impl.state.epoch;
  status.incarnation = impl.state.incarnation;
  status.revision = impl.state.revision;
  status.topology_generation = impl.state.topology_generation;
  status.policy_generation = impl.state.policy_generation;
  status.evidence_generation = impl.state.evidence_generation;
  status.incident_live = impl.state.incident.lifecycle != IncidentLifecycle::None &&
                         impl.state.incident.lifecycle != IncidentLifecycle::Closed;
  status.incident = impl.state.incident.incident;
  status.incident_generation = impl.state.incident.generation;
  status.lifecycle = impl.state.incident.lifecycle;
  status.primary_failure = impl.state.incident.primary_failure;
  status.primary_element = impl.state.incident.primary_element;
  status.has_plan = impl.state.has_plan;
  status.plan_generation = impl.state.incident.plan_generation;
  status.failures = impl.state.has_plan ? impl.state.plan.failures.size() : 0;
  status.impacted_elements = impl.state.has_plan ? impl.state.plan.scope.impacted_elements.size() : 0;
  status.observations = impl.state.observations.size();
  status.requests = impl.state.requests.size();
  for (const auto& slot : impl.state.requests) {
    if (request_state_is_open(slot.request.state)) {
      status.open_requests += 1;
    }
    if (slot.request.state == RequestState::Verified) {
      status.verified_requests += 1;
    }
  }
  status.journal_entries = impl.journal.size();
  status.retired_journal_entries = impl.retired_journal_entries;
  status.operator_authorized = impl.state.incident.operator_authorized;
  status.commit_sequence = impl.store->status().commit_sequence;
  status.commits = impl.store->status().commits;
  status.fell_back = impl.store->status().fell_back;
  if (impl.state.has_plan) {
    if (auto instant = impl.clock->now(); instant.ok()) {
      if (auto assessed = assess(impl, instant.value()); assessed.ok()) {
        status.recovery_eligible = assessed.value().eligible;
      }
    }
  }
  return status;
}

Result<DomainState> PowerFailureRuntime::state() const {
  if (!impl_) {
    return closed_error();
  }
  Impl& impl = *impl_;
  std::lock_guard<std::mutex> guard{impl.mutex};
  return impl.state;
}

Result<ResponsePlan> PowerFailureRuntime::plan() const {
  if (!impl_) {
    return closed_error();
  }
  Impl& impl = *impl_;
  std::lock_guard<std::mutex> guard{impl.mutex};
  if (!impl.state.has_plan) {
    return Status::error(StatusCode::PlanNotCurrent,
                         "no plan has been published for the current state")
        .with_context("runtime.plan");
  }
  return impl.state.plan;
}

Result<ClassificationResult> PowerFailureRuntime::classification() const {
  auto plan_result = plan();
  if (!plan_result.ok()) {
    return plan_result.status();
  }
  auto instant = impl_->clock->now();
  if (!instant.ok()) {
    return instant.status();
  }
  ClassificationResult result;
  result.failures = plan_result.value().failures;
  for (const auto& slot : impl_->state.observations) {
    if (!slot.present) {
      continue;
    }
    if (slot.recovered || slot.contradicted ||
        classify_freshness(slot.observation.observed_at, instant.value(), impl_->state.policy) !=
            Freshness::Fresh) {
      result.unresolved_elements.push_back(slot.element);
    } else {
      result.resolved_elements.push_back(slot.element);
    }
  }
  std::sort(result.unresolved_elements.begin(), result.unresolved_elements.end());
  std::sort(result.resolved_elements.begin(), result.resolved_elements.end());
  return result;
}

Result<AffectedScope> PowerFailureRuntime::scope() const {
  auto plan_result = plan();
  if (!plan_result.ok()) {
    return plan_result.status();
  }
  return plan_result.value().scope;
}

Result<RecoveryAssessment> PowerFailureRuntime::recovery() const {
  if (!impl_) {
    return closed_error();
  }
  Impl& impl = *impl_;
  std::lock_guard<std::mutex> guard{impl.mutex};
  auto instant = now_of(impl);
  if (!instant.ok()) {
    return instant.status();
  }
  if (!impl.state.has_plan) {
    return Status::error(StatusCode::PlanNotCurrent,
                         "recovery requires a current plan computed from current evidence")
        .with_context("runtime.recovery");
  }
  return assess(impl, instant.value());
}

Result<std::vector<ResponseRequest>> PowerFailureRuntime::requests() const {
  if (!impl_) {
    return closed_error();
  }
  Impl& impl = *impl_;
  std::lock_guard<std::mutex> guard{impl.mutex};
  std::vector<ResponseRequest> result;
  result.reserve(impl.state.requests.size());
  for (const auto& slot : impl.state.requests) {
    result.push_back(slot.request);
  }
  return result;
}

Result<ResponseRequest> PowerFailureRuntime::request(ResponseRequestId id) const {
  if (!impl_) {
    return closed_error();
  }
  Impl& impl = *impl_;
  std::lock_guard<std::mutex> guard{impl.mutex};
  const auto* slot = impl.state.find_request(id);
  if (slot == nullptr) {
    return Status::error(StatusCode::RequestNotFound, "no such response request")
        .with_context("request.id");
  }
  return slot->request;
}

Result<std::vector<TransitionRecord>> PowerFailureRuntime::transitions() const {
  if (!impl_) {
    return closed_error();
  }
  Impl& impl = *impl_;
  std::lock_guard<std::mutex> guard{impl.mutex};
  return impl.state.transitions;
}

Result<std::vector<JournalEntry>> PowerFailureRuntime::journal() const {
  if (!impl_) {
    return closed_error();
  }
  Impl& impl = *impl_;
  std::lock_guard<std::mutex> guard{impl.mutex};
  return impl.journal;
}

Result<StoreStatus> PowerFailureRuntime::store_status() const {
  if (!impl_) {
    return closed_error();
  }
  Impl& impl = *impl_;
  std::lock_guard<std::mutex> guard{impl.mutex};
  return impl.store->status();
}

Result<AuthorityToken> PowerFailureRuntime::current_authority() const {
  if (!impl_) {
    return closed_error();
  }
  Impl& impl = *impl_;
  std::lock_guard<std::mutex> guard{impl.mutex};
  AuthorityToken token;
  token.incident = impl.state.incident.incident;
  token.generation = impl.state.incident.generation;
  token.epoch = impl.state.epoch;
  token.incarnation = impl.state.incarnation;
  token.revision = impl.state.revision;
  return token;
}

// --- evidence, topology, policy, obligations -------------------------------

Result<void> PowerFailureRuntime::publish_topology(const TopologySnapshot& snapshot,
                                                   const AuthorityToken& token) {
  if (!impl_) {
    return closed_error();
  }
  Impl& impl = *impl_;
  if (impl.gate.called_from_dispatch_thread()) {
    return Status::error(StatusCode::ReentrancyRefused,
                         "a transport cannot call back into the runtime mid-dispatch")
        .with_context("runtime.reentrancy");
  }
  std::lock_guard<std::mutex> guard{impl.mutex};
  if (impl.closed) {
    return closed_error();
  }
  if (impl.poisoned) {
    return fenced_error();
  }
  if (auto r = validate(snapshot, impl.state.bounds); !r.ok()) {
    return r.status();
  }
  if (auto r = fence(impl, token, false); !r.ok()) {
    return r;
  }
  auto instant = now_of(impl);
  if (!instant.ok()) {
    return instant.status();
  }
  JournalPayload payload;
  payload.has_topology = true;
  payload.topology = snapshot;
  canonicalise(payload.topology);
  auto applied = mutate(impl, JournalKind::TopologyPublished, instant.value(), payload);
  if (!applied.ok()) {
    return applied.status();
  }
  rebuild_topology(impl);
  return {};
}

Result<void> PowerFailureRuntime::publish_policy(const ElectricalPolicy& policy,
                                                 const AuthorityToken& token) {
  if (!impl_) {
    return closed_error();
  }
  Impl& impl = *impl_;
  if (impl.gate.called_from_dispatch_thread()) {
    return Status::error(StatusCode::ReentrancyRefused,
                         "a transport cannot call back into the runtime mid-dispatch")
        .with_context("runtime.reentrancy");
  }
  std::lock_guard<std::mutex> guard{impl.mutex};
  if (impl.closed) {
    return closed_error();
  }
  if (impl.poisoned) {
    return fenced_error();
  }
  if (auto r = validate(policy); !r.ok()) {
    return r.status();
  }
  if (policy.generation.is_absent()) {
    return Status::error(StatusCode::InvalidArgument, "a published policy needs a generation")
        .with_context("policy.generation");
  }
  if (auto r = fence(impl, token, false); !r.ok()) {
    return r;
  }
  auto instant = now_of(impl);
  if (!instant.ok()) {
    return instant.status();
  }
  JournalPayload payload;
  payload.has_policy = true;
  payload.policy = policy;
  return mutate(impl, JournalKind::PolicyPublished, instant.value(), payload);
}

Result<void> PowerFailureRuntime::publish_obligations(
    const std::vector<ProtectedObligation>& obligations, const AuthorityToken& token) {
  if (!impl_) {
    return closed_error();
  }
  Impl& impl = *impl_;
  if (impl.gate.called_from_dispatch_thread()) {
    return Status::error(StatusCode::ReentrancyRefused,
                         "a transport cannot call back into the runtime mid-dispatch")
        .with_context("runtime.reentrancy");
  }
  std::lock_guard<std::mutex> guard{impl.mutex};
  if (impl.closed) {
    return closed_error();
  }
  if (impl.poisoned) {
    return fenced_error();
  }
  if (obligations.size() > impl.state.bounds.max_obligations) {
    return Status::error(StatusCode::BoundsExceeded, "obligation set exceeds the bound")
        .with_context("obligations");
  }
  for (const auto& obligation : obligations) {
    if (auto r = validate(obligation, impl.state.bounds); !r.ok()) {
      return r.status();
    }
  }
  if (auto r = fence(impl, token, true); !r.ok()) {
    return r;
  }
  auto instant = now_of(impl);
  if (!instant.ok()) {
    return instant.status();
  }
  JournalPayload payload;
  payload.has_obligations = true;
  payload.obligations = obligations;
  return mutate(impl, JournalKind::ObligationsPublished, instant.value(), payload);
}

Result<void> PowerFailureRuntime::admit_evidence(const ElectricalObservation& observation,
                                                 const AuthorityToken& token) {
  std::vector<ElectricalObservation> batch;
  batch.push_back(observation);
  return admit_evidence(batch, token);
}

Result<void> PowerFailureRuntime::admit_evidence(
    const std::vector<ElectricalObservation>& observations, const AuthorityToken& token) {
  if (!impl_) {
    return closed_error();
  }
  Impl& impl = *impl_;
  if (impl.gate.called_from_dispatch_thread()) {
    return Status::error(StatusCode::ReentrancyRefused,
                         "a transport cannot call back into the runtime mid-dispatch")
        .with_context("runtime.reentrancy");
  }
  std::lock_guard<std::mutex> guard{impl.mutex};
  if (impl.closed) {
    return closed_error();
  }
  if (impl.poisoned) {
    return fenced_error();
  }
  if (observations.empty()) {
    return Status::error(StatusCode::InvalidArgument, "no observation was supplied")
        .with_context("evidence");
  }
  if (observations.size() > impl.state.bounds.max_observations) {
    return Status::error(StatusCode::BoundsExceeded, "observation batch exceeds the bound")
        .with_context("evidence");
  }
  for (const auto& observation : observations) {
    if (auto r = validate(observation, impl.state.bounds); !r.ok()) {
      return r.status();
    }
    if (impl.state.has_topology && impl.topology.find(observation.element) == nullptr) {
      return Status::error(StatusCode::EvidenceUnknownTarget,
                           "the observation names an element the published topology does not hold")
          .with_context(observation.element.to_string());
    }
  }
  if (auto r = fence(impl, token, true); !r.ok()) {
    return r;
  }
  auto instant = now_of(impl);
  if (!instant.ok()) {
    return instant.status();
  }
  std::vector<JournalEntry> entries;
  for (const auto& observation : observations) {
    JournalPayload payload;
    payload.has_observation = true;
    payload.observation = observation;
    auto entry = make_entry(impl, JournalKind::EvidenceAdmitted, instant.value(), payload);
    if (!entry.ok()) {
      return entry.status();
    }
    entries.push_back(std::move(entry.value()));
  }
  auto applied = apply_and_commit(impl, entries);
  if (!applied.ok()) {
    return applied.status();
  }
  refresh_currency(impl.state, instant.value());
  return {};
}

// --- decision --------------------------------------------------------------

Result<EvaluationOutcome> PowerFailureRuntime::evaluate(const AuthorityToken& token) {
  if (!impl_) {
    return closed_error();
  }
  Impl& impl = *impl_;
  if (impl.gate.called_from_dispatch_thread()) {
    return Status::error(StatusCode::ReentrancyRefused,
                         "a transport cannot call back into the runtime mid-dispatch")
        .with_context("runtime.reentrancy");
  }

  EvaluationOutcome outcome;
  ResponsePlan published_plan;
  std::vector<ResponseRequest> to_dispatch;
  {
    std::lock_guard<std::mutex> guard{impl.mutex};
    if (impl.closed) {
      return closed_error();
    }
    if (impl.poisoned) {
      return fenced_error();
    }
    if (impl.state.incident.lifecycle == IncidentLifecycle::None ||
        impl.state.incident.lifecycle == IncidentLifecycle::Closed) {
      return Status::error(StatusCode::NoActiveIncident,
                           "a decision requires a live incident")
          .with_context("incident.lifecycle");
    }
    if (auto r = fence(impl, token, true); !r.ok()) {
      return r;
    }
    auto instant = now_of(impl);
    if (!instant.ok()) {
      return instant.status();
    }
    const Timestamp now = instant.value();
    refresh_currency(impl.state, now);

    if (!impl.has_topology_index) {
      return Status::error(StatusCode::PreconditionFailed,
                           "a decision requires a published electrical topology")
          .with_context("runtime.topology");
    }

    PlanInputs inputs;
    inputs.incident = impl.state.incident.incident;
    inputs.incident_generation = impl.state.incident.generation;
    inputs.epoch = impl.state.epoch;
    inputs.revision = impl.state.revision;
    inputs.next_plan_generation =
        impl.state.next_plan_generation.is_set() ? impl.state.next_plan_generation
                                                 : PlanGeneration::from_value(1);
    inputs.now = now;
    inputs.policy = impl.state.policy;
    inputs.bounds = impl.state.bounds;
    inputs.topology = &impl.topology;
    inputs.current_observations = current_observations(impl.state, now, impl.state.policy);
    for (const auto& slot : impl.state.obligations) {
      inputs.obligations.push_back(slot.obligation);
    }
    inputs.open_requests = open_requests(impl.state);
    inputs.unevidenced_elements = unevidenced_elements(impl.state, now);
    inputs.unhealthy_scope_elements = unhealthy_scope_elements(impl.state, now);
    // Identity allocation is derived from what is already retained rather than
    // from a remembered counter, so a plan can never hand out an identity that
    // is already in use and a replay or a restart reproduces the same choice.
    ResponseRequestId next_request = ResponseRequestId::from_value(1);
    AttemptId next_attempt = AttemptId::from_value(1);
    for (const auto& slot : impl.state.requests) {
      if (!(slot.request.id < next_request)) {
        next_request = ResponseRequestId::from_value(slot.request.id.value() + 1);
      }
      if (!(slot.request.attempt < next_attempt)) {
        next_attempt = AttemptId::from_value(slot.request.attempt.value() + 1);
      }
    }
    // The counters are deliberately not written back here: durable state may
    // only change through apply_journal_entry, which advances them from the
    // plan it folds. Writing the derived value into the live state would make
    // it differ from the replay of the checkpoint plus the journal, and the
    // store refuses a generation that does not replay.
    inputs.first_request_id = next_request;
    inputs.first_attempt_id = next_attempt;

    auto planned = plan_response(inputs);
    if (!planned.ok()) {
      return planned.status();
    }
    ResponsePlan plan = std::move(planned.value());

    // The plan is the write-ahead record: it is committed before any request
    // leaves the process, together with the identity and idempotency key of
    // every operation it justifies.
    {
      JournalPayload payload;
      payload.has_plan = true;
      payload.plan = plan;
      auto applied = mutate(impl, JournalKind::PlanComputed, now, payload);
      if (!applied.ok()) {
        return applied.status();
      }
    }

    const ResponsePlan& published = impl.state.plan;
    outcome.generation = published.generation;
    outcome.primary_failure = published.primary_failure;
    outcome.primary_element = published.primary_element;
    outcome.failures = published.failures.size();
    outcome.requests_planned = published.requests.size();

    std::vector<JournalEntry> entries;
    for (const auto& request : published.requests) {
      // The plan records the decision; the request's lifecycle lives in the
      // retained request state, which is what a transition must be based on.
      const auto* live = impl.state.find_request(request.id);
      if (live == nullptr || live->request.state != RequestState::Planned) {
        continue;
      }
      JournalPayload payload;
      payload.request = request.id;
      payload.has_request_state = true;
      payload.from_state = live->request.state;
      payload.to_state = RequestState::Issued;
      payload.attempt = live->request.attempt;
      payload.reason = live->request.justification;
      auto entry = make_entry(impl, JournalKind::RequestIssued, now, payload);
      if (!entry.ok()) {
        return entry.status();
      }
      entries.push_back(std::move(entry.value()));
      to_dispatch.push_back(live->request);
    }
    if (auto applied = apply_and_commit(impl, entries); !applied.ok()) {
      return applied.status();
    }
    outcome.requests_dispatched = to_dispatch.size();
    for (const auto& request : to_dispatch) {
      outcome.dispatched_requests.push_back(request.id);
    }
    published_plan = impl.state.plan;
  }

  // The published plan is announced outside every runtime lock: adapter code is
  // never called while the runtime mutex is held.
  if (impl.transport) {
    impl.transport->on_plan_published(published_plan);
  }

  // Dispatch happens outside every runtime lock. Adapter code is never called
  // with the mutex held, so a slow or re-entrant controller cannot deadlock the
  // runtime.
  struct Answer {
    ResponseRequestId id;
    AttemptId attempt;
    DispatchResult result;
    RefToken controller;
    std::string note;
  };
  std::vector<Answer> answers;
  answers.reserve(to_dispatch.size());
  if (impl.transport) {
    for (const auto& request : to_dispatch) {
      Answer answer;
      answer.id = request.id;
      answer.attempt = request.attempt;
      if (!impl.gate.enter()) {
        answer.result = DispatchResult::Indeterminate;
        answer.note = "a dispatch was already in flight on this runtime";
        answers.push_back(std::move(answer));
        continue;
      }
      Result<DispatchOutcome> dispatched = DispatchOutcome::indeterminate("no answer was recorded");
      try {
        dispatched = impl.transport->dispatch(request);
      } catch (...) {
        dispatched = Status::error(StatusCode::TransportFailure,
                                   "the transport threw while dispatching a request")
                         .with_context(request.target.element.to_string());
      }
      impl.gate.leave();
      if (!dispatched.ok()) {
        answer.result = DispatchResult::Indeterminate;
        answer.note = std::string{dispatched.status().message()};
        answers.push_back(std::move(answer));
        continue;
      }
      answer.result = dispatched.value().result;
      answer.controller = dispatched.value().controller;
      answer.note = dispatched.value().note;
      answers.push_back(std::move(answer));
    }
  }

  {
    std::lock_guard<std::mutex> guard{impl.mutex};
    if (impl.closed) {
      return closed_error();
    }
    auto instant = now_of(impl);
    if (!instant.ok()) {
      return instant.status();
    }
    const Timestamp now = instant.value();
    std::vector<JournalEntry> entries;
    for (const auto& answer : answers) {
      const auto* slot = impl.state.find_request(answer.id);
      if (slot == nullptr || slot->request.state != RequestState::Issued) {
        continue;
      }
      JournalPayload payload;
      payload.request = answer.id;
      payload.has_request_state = true;
      payload.from_state = RequestState::Issued;
      payload.attempt = answer.attempt;
      payload.source = answer.controller;
      payload.note = answer.note;
      JournalKind kind = JournalKind::RequestIndeterminate;
      switch (answer.result) {
        case DispatchResult::Accepted:
          kind = JournalKind::RequestAcknowledged;
          payload.to_state = RequestState::Acknowledged;
          break;
        case DispatchResult::Refused:
          kind = JournalKind::RequestRefused;
          payload.to_state = RequestState::Refused;
          payload.reason = ReasonCode::DispatchNotAccepted;
          break;
        case DispatchResult::Failed:
          kind = JournalKind::RequestFailed;
          payload.to_state = RequestState::Failed;
          payload.reason = ReasonCode::AttemptFailed;
          break;
        case DispatchResult::Unavailable:
          kind = JournalKind::RequestIndeterminate;
          payload.to_state = RequestState::Indeterminate;
          payload.reason = ReasonCode::DispatchOutcomeUnknown;
          break;
        case DispatchResult::Indeterminate:
          kind = JournalKind::RequestIndeterminate;
          payload.to_state = RequestState::Indeterminate;
          payload.reason = ReasonCode::DispatchOutcomeUnknown;
          break;
      }
      auto entry = make_entry(impl, kind, now, payload);
      if (!entry.ok()) {
        continue;
      }
      entries.push_back(std::move(entry.value()));
      switch (answer.result) {
        case DispatchResult::Accepted:
          outcome.accepted += 1;
          break;
        case DispatchResult::Refused:
          outcome.refused += 1;
          break;
        case DispatchResult::Failed:
          outcome.failed += 1;
          break;
        default:
          outcome.indeterminate += 1;
          break;
      }
    }
    // Stability bookkeeping is a decision like any other, so it is journaled:
    // the dwell timer starts at the first plan that carries no failure and no
    // unresolved element, and is cleared the moment one appears.
    if (impl.state.has_plan) {
      const bool clean = impl.state.plan.failures.empty() &&
                         impl.state.plan.scope.unevidenced_elements.empty();
      if (clean && !impl.state.incident.has_stable_since) {
        auto entry = make_entry(impl, JournalKind::StabilityObserved, now, JournalPayload{});
        if (entry.ok()) {
          entries.push_back(std::move(entry.value()));
        }
      } else if (!clean && impl.state.incident.has_stable_since) {
        auto entry = make_entry(impl, JournalKind::StabilityCleared, now, JournalPayload{});
        if (entry.ok()) {
          entries.push_back(std::move(entry.value()));
        }
      }
    }
    if (auto applied = apply_and_commit(impl, entries); !applied.ok()) {
      return applied.status();
    }
    if (auto assessed = assess(impl, now); assessed.ok()) {
      outcome.recovery_eligible = assessed.value().eligible;
    }
  }
  return outcome;
}

// --- request lifecycle -----------------------------------------------------

Result<void> PowerFailureRuntime::acknowledge_request(ResponseRequestId id, const RefToken& source,
                                                      std::string note,
                                                      const AuthorityToken& token) {
  if (!impl_) {
    return closed_error();
  }
  Impl& impl = *impl_;
  std::lock_guard<std::mutex> guard{impl.mutex};
  if (impl.closed) {
    return closed_error();
  }
  if (impl.poisoned) {
    return fenced_error();
  }
  const auto* slot = impl.state.find_request(id);
  if (slot == nullptr) {
    return Status::error(StatusCode::RequestNotFound, "no such response request")
        .with_context("request.id");
  }
  if (auto r = fence(impl, token, true); !r.ok()) {
    return r;
  }
  auto instant = now_of(impl);
  if (!instant.ok()) {
    return instant.status();
  }
  JournalPayload payload;
  payload.request = id;
  payload.has_request_state = true;
  payload.from_state = slot->request.state;
  payload.to_state = RequestState::Acknowledged;
  payload.attempt = slot->request.attempt;
  payload.source = source;
  payload.note = std::move(note);
  return mutate(impl, JournalKind::RequestAcknowledged, instant.value(), payload);
}

Result<void> PowerFailureRuntime::record_effect(ResponseRequestId id,
                                                const ElectricalObservation& evidence,
                                                const AuthorityToken& token) {
  if (!impl_) {
    return closed_error();
  }
  Impl& impl = *impl_;
  std::lock_guard<std::mutex> guard{impl.mutex};
  if (impl.closed) {
    return closed_error();
  }
  if (impl.poisoned) {
    return fenced_error();
  }
  const auto* slot = impl.state.find_request(id);
  if (slot == nullptr) {
    return Status::error(StatusCode::RequestNotFound, "no such response request")
        .with_context("request.id");
  }
  if (auto r = validate(evidence, impl.state.bounds); !r.ok()) {
    return r.status();
  }
  if (!origin_is_external(evidence.origin)) {
    return Status::error(StatusCode::EvidenceOriginUntrusted,
                         "only attributable external evidence can confirm an effect")
        .with_context(evidence.element.to_string());
  }
  if (!(evidence.element == slot->request.target.element) &&
      !(evidence.element == slot->request.target.controller)) {
    return Status::error(StatusCode::RequestTargetMismatch,
                         "the evidence does not describe the element the request targeted")
        .with_context(evidence.element.to_string());
  }
  if (auto r = fence(impl, token, true); !r.ok()) {
    return r;
  }
  auto instant = now_of(impl);
  if (!instant.ok()) {
    return instant.status();
  }
  const Timestamp now = instant.value();
  if (classify_freshness(evidence.observed_at, now, impl.state.policy) != Freshness::Fresh) {
    return Status::error(StatusCode::EvidenceNotCurrent,
                         "the supplied evidence is not current and cannot confirm an effect")
        .with_context(evidence.element.to_string());
  }
  // The proof is only accepted when the evidence carries the fact the request
  // asked to be proven.
  bool proven = false;
  switch (slot->request.required_proof) {
    case ProofKind::None:
      proven = true;
      break;
    case ProofKind::Isolation:
    case ProofKind::DeEnergization:
      proven = evidence.energization == EnergizationState::DeEnergized ||
                evidence.breaker == BreakerPosition::Open ||
                evidence.breaker == BreakerPosition::RackedOut;
      break;
    case ProofKind::BreakerOpen:
      proven = evidence.breaker == BreakerPosition::Open ||
                evidence.breaker == BreakerPosition::RackedOut;
      break;
    case ProofKind::Transfer:
      proven = evidence.transfer == TransferState::OnGenerator ||
                evidence.transfer == TransferState::OnUtility ||
                evidence.transfer == TransferState::Isolated;
      break;
    case ProofKind::Synchronization:
      proven = evidence.generator == GeneratorState::Synchronized;
      break;
    case ProofKind::Generation:
      proven = evidence.generator == GeneratorState::Running ||
                evidence.generator == GeneratorState::Synchronized ||
                evidence.energization == EnergizationState::Energized;
      break;
    case ProofKind::Reserve:
      proven = evidence.has_reserve && evidence.reserve >= impl.state.policy.reserve_floor;
      break;
    case ProofKind::LoadShed:
      proven = evidence.has_current && evidence.current.value() == 0;
      break;
    case ProofKind::StableSource:
      proven = evidence.energization == EnergizationState::Energized;
      break;
    case ProofKind::FeedSelected:
      proven = evidence.energization == EnergizationState::Energized;
      break;
  }
  if (!proven) {
    // The evidence is recorded, and the request only reaches Observed: an
    // observation is not a verification.
    return apply_effect_evidence(impl, slot->request, evidence, now, false);
  }
  return apply_effect_evidence(impl, slot->request, evidence, now, true);
}

Result<void> PowerFailureRuntime::fail_request(ResponseRequestId id, ReasonCode reason,
                                               std::string note, const AuthorityToken& token) {
  if (!impl_) {
    return closed_error();
  }
  Impl& impl = *impl_;
  std::lock_guard<std::mutex> guard{impl.mutex};
  if (impl.closed) {
    return closed_error();
  }
  if (impl.poisoned) {
    return fenced_error();
  }
  const auto* slot = impl.state.find_request(id);
  if (slot == nullptr) {
    return Status::error(StatusCode::RequestNotFound, "no such response request")
        .with_context("request.id");
  }
  if (auto r = fence(impl, token, true); !r.ok()) {
    return r;
  }
  auto instant = now_of(impl);
  if (!instant.ok()) {
    return instant.status();
  }
  JournalPayload payload;
  payload.request = id;
  payload.has_request_state = true;
  payload.from_state = slot->request.state;
  payload.to_state = RequestState::Failed;
  payload.attempt = slot->request.attempt;
  payload.reason = reason;
  payload.note = std::move(note);
  return mutate(impl, JournalKind::RequestFailed, instant.value(), payload);
}

Result<void> PowerFailureRuntime::cancel_request(ResponseRequestId id, ReasonCode reason,
                                                 const AuthorityToken& token) {
  if (!impl_) {
    return closed_error();
  }
  Impl& impl = *impl_;
  std::lock_guard<std::mutex> guard{impl.mutex};
  if (impl.closed) {
    return closed_error();
  }
  if (impl.poisoned) {
    return fenced_error();
  }
  const auto* slot = impl.state.find_request(id);
  if (slot == nullptr) {
    return Status::error(StatusCode::RequestNotFound, "no such response request")
        .with_context("request.id");
  }
  if (auto r = fence(impl, token, true); !r.ok()) {
    return r;
  }
  auto instant = now_of(impl);
  if (!instant.ok()) {
    return instant.status();
  }
  JournalPayload payload;
  payload.request = id;
  payload.has_request_state = true;
  payload.from_state = slot->request.state;
  payload.to_state = RequestState::Abandoned;
  payload.attempt = slot->request.attempt;
  payload.reason = reason;
  return mutate(impl, JournalKind::RequestAbandoned, instant.value(), payload);
}

Result<void> PowerFailureRuntime::resolve_indeterminate(ResponseRequestId id, bool effect_observed,
                                                        const ElectricalObservation& evidence,
                                                        const AuthorityToken& token) {
  if (!impl_) {
    return closed_error();
  }
  Impl& impl = *impl_;
  std::lock_guard<std::mutex> guard{impl.mutex};
  if (impl.closed) {
    return closed_error();
  }
  if (impl.poisoned) {
    return fenced_error();
  }
  const auto* slot = impl.state.find_request(id);
  if (slot == nullptr) {
    return Status::error(StatusCode::RequestNotFound, "no such response request")
        .with_context("request.id");
  }
  if (slot->request.state != RequestState::Indeterminate) {
    return Status::error(StatusCode::RequestStateConflict,
                         "only an indeterminate dispatch can be resolved this way")
        .with_context("request.state");
  }
  if (auto r = fence(impl, token, true); !r.ok()) {
    return r;
  }
  if (!effect_observed) {
    auto instant = now_of(impl);
    if (!instant.ok()) {
      return instant.status();
    }
    JournalPayload payload;
    payload.request = id;
    payload.has_request_state = true;
    payload.from_state = RequestState::Indeterminate;
    payload.to_state = RequestState::Failed;
    payload.attempt = slot->request.attempt;
    payload.reason = ReasonCode::IndeterminateDispatchResolved;
    payload.note = "the indeterminate dispatch is established not to have happened";
    return mutate(impl, JournalKind::RequestResolved, instant.value(), payload);
  }
  if (auto r = validate(evidence, impl.state.bounds); !r.ok()) {
    return r.status();
  }
  if (!origin_is_external(evidence.origin)) {
    return Status::error(StatusCode::EvidenceOriginUntrusted,
                         "only attributable external evidence can establish an effect")
        .with_context(evidence.element.to_string());
  }
  auto instant = now_of(impl);
  if (!instant.ok()) {
    return instant.status();
  }
  const Timestamp now = instant.value();
  if (classify_freshness(evidence.observed_at, now, impl.state.policy) != Freshness::Fresh) {
    return Status::error(StatusCode::EvidenceNotCurrent,
                         "the supplied evidence is not current")
        .with_context(evidence.element.to_string());
  }
  return apply_effect_evidence(impl, slot->request, evidence, now, true);
}

Result<void> PowerFailureRuntime::retry_request(ResponseRequestId id, const AuthorityToken& token) {
  if (!impl_) {
    return closed_error();
  }
  Impl& impl = *impl_;
  std::lock_guard<std::mutex> guard{impl.mutex};
  if (impl.closed) {
    return closed_error();
  }
  if (impl.poisoned) {
    return fenced_error();
  }
  const auto* slot = impl.state.find_request(id);
  if (slot == nullptr) {
    return Status::error(StatusCode::RequestNotFound, "no such response request")
        .with_context("request.id");
  }
  if (!request_state_is_open(slot->request.state)) {
    return Status::error(StatusCode::RequestStateConflict,
                         "only an open request can be retried")
        .with_context("request.state");
  }
  if (auto r = fence(impl, token, true); !r.ok()) {
    return r;
  }
  auto instant = now_of(impl);
  if (!instant.ok()) {
    return instant.status();
  }
  const Timestamp now = instant.value();
  const ResponseRequest previous = slot->request;
  if (previous.attempt_ordinal >= impl.state.policy.max_attempts_per_request) {
    return Status::error(StatusCode::AttemptExhausted,
                         "the bounded attempt count for this operation is exhausted")
        .with_context("request.attempts");
  }
  if (impl.state.requests.size() >= impl.state.bounds.max_attempts_retained) {
    return Status::error(StatusCode::ResourceExhausted, "the retained attempt history is full")
        .with_context("state.requests");
  }

  ResponseRequest fresh = previous;
  fresh.id = impl.state.next_request_id.is_set() ? impl.state.next_request_id
                                                 : ResponseRequestId::from_value(1);
  fresh.attempt = impl.state.next_attempt_id.is_set() ? impl.state.next_attempt_id
                                                      : AttemptId::from_value(1);
  fresh.attempt_ordinal = previous.attempt_ordinal + 1;
  fresh.state = RequestState::Planned;
  fresh.created_at = now;
  fresh.plan_generation = impl.state.incident.plan_generation;
  fresh.justification_subject = previous.justification_subject;
  fresh.evidence.clear();
  fresh.owner_note.clear();
  fresh.verification_source = RefToken{};
  fresh.verification_evidence = Fingerprint{};
  fresh.verification_observed_at = Timestamp{};
  fresh.superseded_by = AttemptId{};
  fresh.terminal_reason = ReasonCode::None;
  fresh.has_issued_at = false;
  fresh.has_acknowledged_at = false;
  fresh.has_observed_at = false;
  fresh.has_verified_at = false;
  // The idempotency key and fingerprint are deliberately unchanged: this is the
  // same consequential operation, re-attempted under a new attempt identity.
  fresh.idempotency = previous.idempotency;
  fresh.fingerprint = previous.fingerprint;
  auto deadline = checked_add(now, impl.state.policy.verification_window);
  if (!deadline.ok()) {
    return deadline.status();
  }
  fresh.deadline = deadline.value();
  if (auto r = validate(fresh, impl.state.bounds); !r.ok()) {
    return r.status();
  }

  std::vector<JournalEntry> entries;
  {
    JournalPayload payload;
    payload.request = id;
    payload.has_request_state = true;
    payload.from_state = previous.state;
    payload.to_state = RequestState::Superseded;
    payload.attempt = fresh.attempt;
    payload.reason = ReasonCode::JustificationWithdrawn;
    payload.note = "replaced by a new attempt of the same operation";
    auto entry = make_entry(impl, JournalKind::RequestSuperseded, now, payload);
    if (!entry.ok()) {
      return entry.status();
    }
    entries.push_back(std::move(entry.value()));
  }
  {
    // The replacement is published as a plan revision that carries the new
    // attempt, so the write-ahead record precedes any dispatch.
    ResponsePlan plan = impl.state.plan;
    plan.generation = impl.state.next_plan_generation.is_set()
                          ? impl.state.next_plan_generation
                          : PlanGeneration::from_value(1);
    plan.decided_at = now;
    bool replaced = false;
    for (auto& request : plan.requests) {
      if (request.id == id) {
        request = fresh;
        replaced = true;
      }
    }
    if (!replaced) {
      plan.requests.push_back(fresh);
    }
    sort_requests(plan.requests);
    JournalPayload payload;
    payload.has_plan = true;
    payload.plan = plan;
    auto entry = make_entry(impl, JournalKind::PlanComputed, now, payload);
    if (!entry.ok()) {
      return entry.status();
    }
    entries.push_back(std::move(entry.value()));
  }
  auto applied = apply_and_commit(impl, entries);
  if (!applied.ok()) {
    return applied.status();
  }
  return {};
}

// --- incident lifecycle and recovery --------------------------------------

Result<void> PowerFailureRuntime::open_incident(const AuthorityToken& token) {
  if (!impl_) {
    return closed_error();
  }
  Impl& impl = *impl_;
  std::lock_guard<std::mutex> guard{impl.mutex};
  if (impl.closed) {
    return closed_error();
  }
  if (impl.poisoned) {
    return fenced_error();
  }
  if (token.incident.is_absent() || token.generation.is_absent()) {
    return Status::error(StatusCode::InvalidArgument,
                         "opening an incident requires an incident identity and generation")
        .with_context("incident.id");
  }
  if (auto r = fence(impl, token, false); !r.ok()) {
    return r;
  }
  if (impl.state.incident.lifecycle != IncidentLifecycle::None &&
      impl.state.incident.lifecycle != IncidentLifecycle::Closed) {
    return Status::error(StatusCode::IncidentAlreadyOpen, "an incident is already live")
        .with_context("incident.id");
  }
  auto instant = now_of(impl);
  if (!instant.ok()) {
    return instant.status();
  }
  JournalPayload payload;
  payload.has_incident = true;
  payload.incident = token.incident;
  payload.incident_generation = token.generation;
  return mutate(impl, JournalKind::IncidentOpened, instant.value(), payload);
}

Result<void> PowerFailureRuntime::authorize_recovery(const AuthorityToken& token) {
  if (!impl_) {
    return closed_error();
  }
  Impl& impl = *impl_;
  std::lock_guard<std::mutex> guard{impl.mutex};
  if (impl.closed) {
    return closed_error();
  }
  if (impl.poisoned) {
    return fenced_error();
  }
  if (auto r = fence(impl, token, true); !r.ok()) {
    return r;
  }
  auto instant = now_of(impl);
  if (!instant.ok()) {
    return instant.status();
  }
  JournalPayload payload;
  payload.has_recovery_authorization = true;
  payload.authorized_at = instant.value();
  payload.incident = impl.state.incident.incident;
  payload.incident_generation = impl.state.incident.generation;
  return mutate(impl, JournalKind::RecoveryAuthorized, instant.value(), payload);
}

Result<void> PowerFailureRuntime::begin_recovery(const AuthorityToken& token) {
  if (!impl_) {
    return closed_error();
  }
  Impl& impl = *impl_;
  std::lock_guard<std::mutex> guard{impl.mutex};
  if (impl.closed) {
    return closed_error();
  }
  if (impl.poisoned) {
    return fenced_error();
  }
  if (auto r = fence(impl, token, true); !r.ok()) {
    return r;
  }
  auto instant = now_of(impl);
  if (!instant.ok()) {
    return instant.status();
  }
  if (!impl.state.has_plan) {
    return Status::error(StatusCode::PlanNotCurrent,
                         "recovery requires a plan computed from current evidence")
        .with_context("runtime.recovery");
  }
  auto assessment = assess(impl, instant.value());
  if (!assessment.ok()) {
    return assessment.status();
  }
  if (!assessment.value().eligible) {
    return Status::error(StatusCode::RecoveryNotEligible, assessment.value().summarize())
        .with_context("recovery.gates");
  }
  return mutate(impl, JournalKind::RecoveryStarted, instant.value(), JournalPayload{});
}

Result<void> PowerFailureRuntime::complete_recovery(const AuthorityToken& token) {
  if (!impl_) {
    return closed_error();
  }
  Impl& impl = *impl_;
  std::lock_guard<std::mutex> guard{impl.mutex};
  if (impl.closed) {
    return closed_error();
  }
  if (impl.poisoned) {
    return fenced_error();
  }
  if (auto r = fence(impl, token, true); !r.ok()) {
    return r;
  }
  auto instant = now_of(impl);
  if (!instant.ok()) {
    return instant.status();
  }
  if (impl.state.incident.lifecycle != IncidentLifecycle::Recovering) {
    return Status::error(StatusCode::IllegalTransition,
                         "recovery can only complete from the recovering state")
        .with_context("incident.lifecycle");
  }
  auto assessment = assess(impl, instant.value());
  if (!assessment.ok()) {
    return assessment.status();
  }
  if (!assessment.value().eligible) {
    return Status::error(StatusCode::RecoveryNotEligible, assessment.value().summarize())
        .with_context("recovery.gates");
  }
  return mutate(impl, JournalKind::RecoveryCompleted, instant.value(), JournalPayload{});
}

Result<void> PowerFailureRuntime::close_incident(const AuthorityToken& token) {
  if (!impl_) {
    return closed_error();
  }
  Impl& impl = *impl_;
  std::lock_guard<std::mutex> guard{impl.mutex};
  if (impl.closed) {
    return closed_error();
  }
  if (impl.poisoned) {
    return fenced_error();
  }
  if (auto r = fence(impl, token, true); !r.ok()) {
    return r;
  }
  auto instant = now_of(impl);
  if (!instant.ok()) {
    return instant.status();
  }
  return mutate(impl, JournalKind::IncidentClosed, instant.value(), JournalPayload{});
}

Result<void> PowerFailureRuntime::relax_obligation(ObligationId id, const RefToken& granted_by,
                                                   const AuthorityToken& token) {
  if (!impl_) {
    return closed_error();
  }
  Impl& impl = *impl_;
  std::lock_guard<std::mutex> guard{impl.mutex};
  if (impl.closed) {
    return closed_error();
  }
  if (impl.poisoned) {
    return fenced_error();
  }
  if (!granted_by.is_set()) {
    return Status::error(StatusCode::InvalidArgument,
                         "a relaxation must name the authority that granted it")
        .with_context("obligation.relaxation");
  }
  const auto* slot = impl.state.find_obligation(id);
  if (slot == nullptr) {
    return Status::error(StatusCode::ObligationUnknown, "no such protected obligation")
        .with_context("obligation.id");
  }
  if (auto r = fence(impl, token, true); !r.ok()) {
    return r;
  }
  auto instant = now_of(impl);
  if (!instant.ok()) {
    return instant.status();
  }
  JournalPayload payload;
  payload.has_obligation = true;
  payload.obligation = id;
  payload.granted_by = granted_by;
  payload.relaxation_class = slot->obligation.protection;
  return mutate(impl, JournalKind::ObligationRelaxed, instant.value(), payload);
}

// --- maintenance -----------------------------------------------------------

Result<void> PowerFailureRuntime::checkpoint(const AuthorityToken& token) {
  if (!impl_) {
    return closed_error();
  }
  Impl& impl = *impl_;
  std::lock_guard<std::mutex> guard{impl.mutex};
  if (impl.closed) {
    return closed_error();
  }
  if (impl.poisoned) {
    return fenced_error();
  }
  if (auto r = fence(impl, token, true); !r.ok()) {
    return r;
  }
  return fold_journal(impl);
}

Result<void> PowerFailureRuntime::roll_epoch(ControlEpoch epoch, ControllerIncarnation incarnation) {
  if (!impl_) {
    return closed_error();
  }
  Impl& impl = *impl_;
  if (impl.gate.called_from_dispatch_thread()) {
    return Status::error(StatusCode::ReentrancyRefused,
                         "a transport cannot roll the control epoch mid-dispatch")
        .with_context("runtime.reentrancy");
  }
  std::lock_guard<std::mutex> guard{impl.mutex};
  if (impl.closed) {
    return closed_error();
  }
  if (epoch.is_absent() || incarnation.is_absent()) {
    return Status::error(StatusCode::MissingAuthority,
                         "a roll requires a control epoch and a controller incarnation")
        .with_context("runtime.authority");
  }
  if (epoch < impl.state.epoch) {
    return Status::error(StatusCode::StaleEpoch, "the control epoch never moves backwards")
        .with_context("runtime.epoch");
  }
  if (epoch == impl.state.epoch) {
    return Status::error(StatusCode::StaleIncarnation,
                         "a new controller must roll the control epoch beyond the current one")
        .with_context("runtime.epoch");
  }
  auto instant = now_of(impl);
  if (!instant.ok()) {
    return instant.status();
  }
  JournalPayload payload;
  payload.epoch = epoch;
  payload.incarnation = incarnation;
  payload.epoch_rollover = true;
  auto applied = mutate(impl, JournalKind::EpochRolled, instant.value(), payload);
  if (!applied.ok()) {
    return applied.status();
  }
  impl.poisoned = false;
  impl.options.epoch = epoch;
  impl.options.incarnation = incarnation;
  return {};
}

}  // namespace summon::pfm
