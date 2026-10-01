// Power Failure Manager -- durable, integrity-checked state store.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0 (see LICENSE).
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "pfm/ids.hpp"
#include "pfm/journal.hpp"
#include "pfm/policy.hpp"
#include "pfm/state.hpp"
#include "pfm/status.hpp"

namespace summon::pfm {

enum class StoreMode : std::uint8_t {
  // Two slots, staged write, device flush, read-back verification, atomic
  // replacement, OS-level single-writer lock.
  Durable = 0,
  // No files, no lock. Used by pure decision paths and by tests that do not
  // claim durability.
  Volatile = 1,
};

struct StoreOptions {
  std::string directory{};
  StoreMode mode{StoreMode::Durable};
  Bounds bounds{};
};

struct SlotInfo {
  bool valid{false};
  // Why the slot was refused when valid is false. Ok for a slot that was not
  // examined because the file does not exist, and for one that loaded.
  StatusCode refusal{StatusCode::Ok};
  CommitSequence commit_sequence{};
  StoreGeneration generation{};
  Timestamp committed_at{};
  std::string detail{};
};

struct StoreStatus {
  StoreMode mode{StoreMode::Volatile};
  std::string directory{};
  bool has_state{false};
  CommitSequence commit_sequence{};
  StoreGeneration generation{};
  Timestamp committed_at{};
  // True when the newest slot was unusable and the previous complete
  // generation was loaded instead.
  bool fell_back{false};
  std::string fallback_detail{};
  std::vector<SlotInfo> slots{};
  std::uint64_t retired_journal_entries{0};
  std::uint64_t commits{0};
};

// The complete durable content: a checkpoint, the live state the checkpoint
// plus the retained journal must reproduce byte for byte, and the entries.
struct StoreSnapshot {
  DomainState checkpoint{};
  DomainState live{};
  std::vector<JournalEntry> journal{};
  std::uint64_t retired_journal_entries{0};
};

// An OS-level exclusive lock on a canonicalised lock file inside the store
// directory. It is held by the operating system, so a killed process cannot
// leave a stale lock behind and a second process is refused deterministically.
class StoreLock {
 public:
  StoreLock() noexcept = default;
  ~StoreLock();
  StoreLock(const StoreLock&) = delete;
  StoreLock& operator=(const StoreLock&) = delete;
  StoreLock(StoreLock&& other) noexcept;
  StoreLock& operator=(StoreLock&& other) noexcept;

  [[nodiscard]] static Result<StoreLock> acquire(const std::string& directory);
  [[nodiscard]] bool held() const noexcept;
  void release() noexcept;

 private:
  void* handle_{nullptr};
  std::string path_{};
};

// The store owns exactly one writer. Opening an existing durable store is
// itself a fenced operation performed by the runtime, not here.
class DurableStore {
 public:
  ~DurableStore();
  DurableStore(const DurableStore&) = delete;
  DurableStore& operator=(const DurableStore&) = delete;

  [[nodiscard]] static Result<std::unique_ptr<DurableStore>> open(const StoreOptions& options);

  [[nodiscard]] const StoreStatus& status() const noexcept { return status_; }
  [[nodiscard]] Result<StoreSnapshot> load();
  [[nodiscard]] Result<void> commit(const StoreSnapshot& snapshot);
  [[nodiscard]] Result<void> close();
  [[nodiscard]] bool is_durable() const noexcept { return status_.mode == StoreMode::Durable; }

  // Fault-injection hooks used by the crash-consistency proofs. They are part
  // of the public surface so that a real killed process can be stopped at an
  // exact point of the publication sequence; they never alter a normal commit.
  void set_publication_disabled(bool disabled) noexcept { publication_disabled_ = disabled; }
  void set_stage_only(bool stage_only) noexcept { stage_only_ = stage_only; }
  [[nodiscard]] bool publication_disabled() const noexcept { return publication_disabled_; }

 private:
  DurableStore() = default;

  StoreStatus status_{};
  Bounds bounds_{};
  StoreLock lock_{};
  bool publication_disabled_{false};
  bool stage_only_{false};
};

// Canonical path handling: resolves a store directory to an absolute path and
// refuses one that cannot be a directory on this host.
[[nodiscard]] Result<std::string> canonical_store_directory(const std::string& directory);

}  // namespace summon::pfm
