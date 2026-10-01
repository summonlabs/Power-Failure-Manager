// Power Failure Manager -- durable, integrity-checked state store.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0 (see LICENSE).
#include "pfm/store.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>
#include <vector>

#include "pfm/codec.hpp"
#include "pfm/version.hpp"

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>
#endif

namespace summon::pfm {
namespace {

constexpr std::uint32_t kSlotMagic = 0x50464D53u;   // "PFMS"
constexpr std::uint32_t kTrailerMagic = 0x50464D54u;  // "PFMT"
constexpr std::size_t kHeaderSize = 56;
constexpr std::size_t kTrailerSize = 8;
constexpr std::size_t kMaxSlotBytes = 64u * 1024u * 1024u;

const char* kSlotNames[2] = {"state.0", "state.1"};
constexpr const char* kLockName = "store.lock";

Status io_error(std::string message, std::string context) {
  return Status::error(StatusCode::StoreIoError, std::move(message)).with_context(std::move(context));
}

Status corrupt(std::string message, std::string context) {
  return Status::error(StatusCode::StoreCorrupt, std::move(message)).with_context(std::move(context));
}

void put_u32(std::vector<std::uint8_t>& out, std::uint32_t value) {
  for (int shift = 0; shift < 32; shift += 8) {
    out.push_back(static_cast<std::uint8_t>((value >> shift) & 0xFFu));
  }
}

void put_u64(std::vector<std::uint8_t>& out, std::uint64_t value) {
  for (int shift = 0; shift < 64; shift += 8) {
    out.push_back(static_cast<std::uint8_t>((value >> shift) & 0xFFu));
  }
}

std::uint32_t get_u32(std::span<const std::uint8_t> data, std::size_t offset) {
  std::uint32_t value = 0;
  for (int i = 0; i < 4; ++i) {
    value |= static_cast<std::uint32_t>(data[offset + static_cast<std::size_t>(i)]) << (8 * i);
  }
  return value;
}

std::uint64_t get_u64(std::span<const std::uint8_t> data, std::size_t offset) {
  std::uint64_t value = 0;
  for (int i = 0; i < 8; ++i) {
    value |= static_cast<std::uint64_t>(data[offset + static_cast<std::size_t>(i)]) << (8 * i);
  }
  return value;
}

// --- snapshot payload ------------------------------------------------------

std::vector<std::uint8_t> encode_snapshot(const StoreSnapshot& snapshot, const Bounds& bounds) {
  codec::Writer writer;
  writer.u64(snapshot.retired_journal_entries);
  codec::encode(writer, snapshot.checkpoint, bounds);
  codec::encode(writer, snapshot.live, bounds);
  writer.u32(static_cast<std::uint32_t>(snapshot.journal.size()));
  for (const auto& entry : snapshot.journal) {
    codec::encode(writer, entry, bounds);
  }
  if (!writer.ok()) {
    return {};
  }
  return writer.bytes();
}

Result<StoreSnapshot> decode_snapshot(std::span<const std::uint8_t> bytes, const Bounds& bounds) {
  codec::Reader reader{bytes};
  StoreSnapshot snapshot;
  snapshot.retired_journal_entries = reader.u64();
  codec::decode(reader, snapshot.checkpoint, bounds);
  codec::decode(reader, snapshot.live, bounds);
  const auto count = reader.u32();
  if (!reader.ok()) {
    return reader.error();
  }
  if (count > bounds.max_journal_entries) {
    return Status::error(StatusCode::BoundsExceeded,
                         "declared journal length exceeds the configured bound")
        .with_context("store.journal");
  }
  snapshot.journal.resize(count);
  for (std::uint32_t i = 0; i < count; ++i) {
    codec::decode(reader, snapshot.journal[i], bounds);
  }
  if (!reader.ok()) {
    return reader.error();
  }
  if (!reader.at_end()) {
    return Status::error(StatusCode::StoreTrailingBytes,
                         "snapshot payload has trailing bytes after the final field")
        .with_context("store.payload");
  }
  return snapshot;
}

// --- file helpers ----------------------------------------------------------

// Writes the complete byte range and flushes it to the device before
// returning. The staging file is never the authoritative slot; the atomic
// replacement that follows is the commit point.
Result<void> write_and_flush(const std::string& path, std::span<const std::uint8_t> data) {
#ifdef _WIN32
  HANDLE handle = ::CreateFileW(std::filesystem::path{path}.wstring().c_str(), GENERIC_WRITE, 0,
                                nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (handle == INVALID_HANDLE_VALUE) {
    return io_error("cannot create the staging file", path);
  }
  std::size_t written = 0;
  while (written < data.size()) {
    const auto remaining = data.size() - written;
    const DWORD chunk = static_cast<DWORD>(remaining > (1u << 20) ? (1u << 20) : remaining);
    DWORD chunk_written = 0;
    if (::WriteFile(handle, data.data() + written, chunk, &chunk_written, nullptr) == 0 ||
        chunk_written == 0) {
      ::CloseHandle(handle);
      return io_error("short write to the staging file", path);
    }
    written += chunk_written;
  }
  if (::FlushFileBuffers(handle) == 0) {
    ::CloseHandle(handle);
    return io_error("device flush failed for the staging file", path);
  }
  if (::CloseHandle(handle) == 0) {
    return io_error("closing the staging file failed", path);
  }
  return {};
#else
  const int descriptor = ::open(path.c_str(), O_CREAT | O_WRONLY | O_TRUNC, 0644);
  if (descriptor < 0) {
    return io_error("cannot create the staging file", path);
  }
  std::size_t written = 0;
  while (written < data.size()) {
    const auto chunk = ::write(descriptor, data.data() + written, data.size() - written);
    if (chunk <= 0) {
      ::close(descriptor);
      return io_error("short write to the staging file", path);
    }
    written += static_cast<std::size_t>(chunk);
  }
  if (::fsync(descriptor) != 0) {
    ::close(descriptor);
    return io_error("device flush failed for the staging file", path);
  }
  if (::close(descriptor) != 0) {
    return io_error("closing the staging file failed", path);
  }
  return {};
#endif
}

Result<std::vector<std::uint8_t>> read_whole_file(const std::string& path) {
  std::error_code code;
  const auto size = std::filesystem::file_size(path, code);
  if (code) {
    return io_error("cannot size the slot file", path);
  }
  if (size > kMaxSlotBytes) {
    return Status::error(StatusCode::BoundsExceeded, "slot file exceeds the maximum size")
        .with_context(path);
  }
  std::ifstream stream{path, std::ios::binary};
  if (!stream) {
    return io_error("cannot open the slot file", path);
  }
  std::vector<std::uint8_t> bytes(static_cast<std::size_t>(size));
  if (size > 0) {
    stream.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(size));
    if (!stream) {
      return io_error("cannot read the slot file", path);
    }
  }
  return bytes;
}

bool file_exists(const std::string& path) {
  std::error_code code;
  return std::filesystem::exists(path, code) && !code;
}

struct SlotHeader {
  std::uint32_t format{0};
  StoreGeneration generation{};
  CommitSequence commit_sequence{};
  Timestamp committed_at{};
  std::uint32_t payload_length{0};
  std::uint32_t payload_crc{0};
};

std::vector<std::uint8_t> build_slot_file(const StoreSnapshot& snapshot, const Bounds& bounds,
                                          SlotHeader& header_out) {
  const auto payload = encode_snapshot(snapshot, bounds);
  const auto payload_crc = codec::crc32c(payload);
  std::vector<std::uint8_t> header;
  header.reserve(kHeaderSize);
  put_u32(header, kSlotMagic);
  put_u32(header, kStoreFormatVersion);
  put_u32(header, 0u);  // reserved, must be zero
  put_u32(header, 0u);  // reserved, must be zero
  put_u64(header, header_out.generation.value());
  put_u64(header, header_out.commit_sequence.value());
  put_u64(header, static_cast<std::uint64_t>(header_out.committed_at.value()));
  put_u32(header, static_cast<std::uint32_t>(payload.size()));
  put_u32(header, payload_crc);
  // The header checksum covers the first 48 bytes, so the field is computed
  // before it is appended and verified after it is zeroed again on load.
  put_u32(header, codec::crc32c(header));
  put_u32(header, 0u);
  header_out.format = kStoreFormatVersion;
  header_out.payload_length = static_cast<std::uint32_t>(payload.size());
  header_out.payload_crc = payload_crc;

  std::vector<std::uint8_t> file;
  file.reserve(header.size() + payload.size() + kTrailerSize);
  file.insert(file.end(), header.begin(), header.end());
  file.insert(file.end(), payload.begin(), payload.end());
  put_u32(file, kTrailerMagic);
  put_u32(file, static_cast<std::uint32_t>(header.size() + payload.size() + kTrailerSize));
  return file;
}

Result<StoreSnapshot> parse_slot_file(std::span<const std::uint8_t> bytes, const Bounds& bounds,
                                      SlotInfo& info) {
  if (bytes.size() < kHeaderSize + kTrailerSize) {
    return Status::error(StatusCode::StoreTruncated, "slot file is smaller than its framing")
        .with_context("store.slot");
  }
  if (get_u32(bytes, 0) != kSlotMagic) {
    return corrupt("slot file magic is wrong", "store.magic");
  }
  const std::uint32_t format = get_u32(bytes, 4);
  if (format != kStoreFormatVersion) {
    return Status::error(StatusCode::StoreVersionUnsupported,
                         "slot file format version is not supported")
        .with_context("store.format");
  }
  if (get_u32(bytes, 8) != 0u || get_u32(bytes, 12) != 0u || get_u32(bytes, 52) != 0u) {
    return Status::error(StatusCode::ReservedFieldNotZero,
                         "slot header reserved fields are not zero")
        .with_context("store.reserved");
  }
  const auto header_crc = get_u32(bytes, 48);
  // The checksum covers the 48 bytes that precede the checksum field itself:
  // exactly the message the writer hashed before appending the field. Hashing
  // the whole header instead appends the trailing reserved word to the message
  // and produces a different value, so a slot this store wrote would never
  // load.
  if (codec::crc32c(bytes.first(48)) != header_crc) {
    return Status::error(StatusCode::StoreIntegrityFailure, "slot header checksum mismatch")
        .with_context("store.header_crc");
  }
  const auto payload_length = get_u32(bytes, 40);
  const auto payload_crc = get_u32(bytes, 44);
  if (payload_length > kMaxSlotBytes ||
      kHeaderSize + static_cast<std::size_t>(payload_length) + kTrailerSize != bytes.size()) {
    return Status::error(StatusCode::StoreTruncated,
                         "slot payload length does not match the file length")
        .with_context("store.payload_length");
  }
  if (get_u32(bytes, bytes.size() - kTrailerSize) != kTrailerMagic) {
    return corrupt("slot trailer magic is wrong", "store.trailer");
  }
  if (get_u32(bytes, bytes.size() - 4) != static_cast<std::uint32_t>(bytes.size())) {
    return corrupt("slot trailer length does not match the file length", "store.trailer");
  }
  const auto payload = bytes.subspan(kHeaderSize, payload_length);
  if (codec::crc32c(payload) != payload_crc) {
    return Status::error(StatusCode::StoreIntegrityFailure, "slot payload checksum mismatch")
        .with_context("store.payload_crc");
  }
  auto snapshot = decode_snapshot(payload, bounds);
  if (!snapshot.ok()) {
    return snapshot.status();
  }
  info.valid = true;
  info.commit_sequence = CommitSequence::from_value(get_u64(bytes, 24));
  info.generation = StoreGeneration::from_value(get_u64(bytes, 16));
  info.committed_at = Timestamp::from_value(static_cast<std::int64_t>(get_u64(bytes, 32)));
  info.detail = "valid";
  return snapshot.value();
}

std::string join(const std::string& directory, const char* name) {
  std::filesystem::path path{directory};
  path /= name;
  return path.string();
}

std::string staging_path(const std::string& directory, const char* name) {
  std::filesystem::path path{directory};
  path /= std::string{name} + ".staging";
  return path.string();
}

}  // namespace

// --- the OS-level single-writer lock --------------------------------------

StoreLock::~StoreLock() { release(); }

StoreLock::StoreLock(StoreLock&& other) noexcept
    : handle_(other.handle_), path_(std::move(other.path_)) {
  other.handle_ = nullptr;
  other.path_.clear();
}

StoreLock& StoreLock::operator=(StoreLock&& other) noexcept {
  if (this != &other) {
    release();
    handle_ = other.handle_;
    path_ = std::move(other.path_);
    other.handle_ = nullptr;
    other.path_.clear();
  }
  return *this;
}

bool StoreLock::held() const noexcept { return handle_ != nullptr; }

void StoreLock::release() noexcept {
#ifdef _WIN32
  if (handle_ != nullptr) {
    ::CloseHandle(static_cast<HANDLE>(handle_));
  }
#else
  if (handle_ != nullptr) {
    const int descriptor = static_cast<int>(reinterpret_cast<std::intptr_t>(handle_));
    ::flock(descriptor, LOCK_UN);
    ::close(descriptor);
  }
#endif
  handle_ = nullptr;
}

Result<StoreLock> StoreLock::acquire(const std::string& directory) {
  StoreLock lock;
  lock.path_ = join(directory, kLockName);
#ifdef _WIN32
  // Share mode zero is the exclusion: while this handle is open, no other
  // process can open the same file at all. The kernel releases it when the
  // process dies, so a killed writer never leaves a stale lock behind.
  HANDLE handle = ::CreateFileW(std::filesystem::path{lock.path_}.wstring().c_str(),
                                GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_ALWAYS,
                                FILE_ATTRIBUTE_NORMAL, nullptr);
  if (handle == INVALID_HANDLE_VALUE) {
    const DWORD error = ::GetLastError();
    if (error == ERROR_SHARING_VIOLATION || error == ERROR_LOCK_VIOLATION) {
      return Status::error(StatusCode::StoreLocked,
                           "the store is already held by another process")
          .with_context(lock.path_);
    }
    return io_error("cannot open the store lock file", lock.path_);
  }
  lock.handle_ = handle;
#else
  const int descriptor = ::open(lock.path_.c_str(), O_CREAT | O_RDWR, 0644);
  if (descriptor < 0) {
    return io_error("cannot open the store lock file", lock.path_);
  }
  if (::flock(descriptor, LOCK_EX | LOCK_NB) != 0) {
    ::close(descriptor);
    return Status::error(StatusCode::StoreLocked,
                         "the store is already held by another process")
        .with_context(lock.path_);
  }
  lock.handle_ = reinterpret_cast<void*>(static_cast<std::intptr_t>(descriptor));
#endif
  return lock;
}

// --- durable store ---------------------------------------------------------

Result<std::string> canonical_store_directory(const std::string& directory) {
  if (directory.empty()) {
    return Status::error(StatusCode::StorePathInvalid, "store directory is empty")
        .with_context("store.path");
  }
  std::error_code code;
  std::filesystem::path path{directory};
  if (!path.is_absolute()) {
    path = std::filesystem::absolute(path, code);
    if (code) {
      return Status::error(StatusCode::StorePathInvalid, "cannot resolve the store directory")
          .with_context(directory);
    }
  }
  path = path.lexically_normal();
  const auto text = path.string();
  if (text.empty() || text.size() > 32767) {
    return Status::error(StatusCode::StorePathInvalid, "store directory path is unusable")
        .with_context(directory);
  }
  return text;
}

DurableStore::~DurableStore() {
  try {
    static_cast<void>(close());
  } catch (...) {
    // A destructor never propagates a failure; the lock is released by the
    // kernel on process exit in any case.
  }
}

Result<std::unique_ptr<DurableStore>> DurableStore::open(const StoreOptions& options) {
  if (auto r = validate(options.bounds); !r.ok()) {
    return r.status();
  }
  auto store = std::unique_ptr<DurableStore>{new DurableStore{}};
  store->bounds_ = options.bounds;
  store->status_.mode = options.mode;

  if (options.mode == StoreMode::Volatile) {
    store->status_.directory.clear();
    return store;
  }

  auto directory = canonical_store_directory(options.directory);
  if (!directory.ok()) {
    return directory.status();
  }
  store->status_.directory = directory.value();

  std::error_code code;
  if (!std::filesystem::exists(store->status_.directory, code)) {
    std::filesystem::create_directories(store->status_.directory, code);
    if (code) {
      return io_error("cannot create the store directory", store->status_.directory);
    }
  }
  if (!std::filesystem::is_directory(store->status_.directory, code) || code) {
    return Status::error(StatusCode::StorePathInvalid, "store path is not a directory")
        .with_context(store->status_.directory);
  }

  auto lock = StoreLock::acquire(store->status_.directory);
  if (!lock.ok()) {
    return lock.status();
  }
  store->lock_ = std::move(lock.value());
  store->status_.slots.resize(2);
  return store;
}

Result<void> DurableStore::close() {
  if (status_.mode == StoreMode::Durable) {
    // A staging file is never authoritative: any leftover from an interrupted
    // commit is removed rather than published.
    for (const auto* name : kSlotNames) {
      const auto staging = staging_path(status_.directory, name);
      if (file_exists(staging)) {
        std::error_code code;
        std::filesystem::remove(staging, code);
      }
    }
    lock_.release();
  }
  return {};
}

Result<StoreSnapshot> DurableStore::load() {
  if (status_.mode == StoreMode::Volatile) {
    // The volatile store holds nothing: its caller keeps the state in memory.
    return Status::error(StatusCode::StoreNotFound,
                         "a volatile store has no durable content to load")
        .with_context("store.volatile");
  }
  struct Candidate {
    bool valid{false};
    bool exists{false};
    StoreSnapshot snapshot{};
    SlotInfo info{};
    std::size_t index{0};
    std::string failure{};
  };
  std::vector<Candidate> candidates;
  candidates.reserve(2);
  bool any_slot_file = false;
  for (std::size_t index = 0; index < 2; ++index) {
    Candidate candidate;
    candidate.index = index;
    const auto path = join(status_.directory, kSlotNames[index]);
    any_slot_file = any_slot_file || file_exists(path);
    if (!file_exists(path)) {
      candidate.failure = "slot file does not exist";
      candidates.push_back(std::move(candidate));
      continue;
    }
    candidate.exists = true;
    auto bytes = read_whole_file(path);
    if (!bytes.ok()) {
      candidate.failure = std::string{bytes.status().message()};
      candidates.push_back(std::move(candidate));
      continue;
    }
    SlotInfo info;
    auto snapshot = parse_slot_file(bytes.value(), bounds_, info);
    if (!snapshot.ok()) {
      // The exact machine-readable reason the slot was refused, so a caller can
      // tell a corrupt slot from a truncated one without parsing diagnostics.
      candidate.info.refusal = snapshot.status().code();
      candidate.info.detail = std::string{snapshot.status().code_name()};
      candidate.failure = std::string{snapshot.status().message()};
      candidates.push_back(std::move(candidate));
      continue;
    }
    candidate.valid = true;
    candidate.snapshot = std::move(snapshot.value());
    candidate.info = info;
    candidates.push_back(std::move(candidate));
  }

  Candidate* best = nullptr;
  for (auto& candidate : candidates) {
    if (!candidate.valid) {
      continue;
    }
    if (best == nullptr ||
        best->info.commit_sequence < candidate.info.commit_sequence) {
      best = &candidate;
    }
  }
  if (best == nullptr) {
    std::string detail;
    for (const auto& candidate : candidates) {
      if (!candidate.failure.empty()) {
        detail.append(candidate.failure);
        detail.push_back(';');
      }
    }
    // A store in which no slot file was ever written is empty, and starting a
    // first generation in it is legitimate. A store whose slot files exist but
    // are unusable is a different situation and is never silently restarted.
    if (!any_slot_file) {
      return Status::error(StatusCode::StoreNotFound, "the store holds no generation yet")
          .with_context("store.empty");
    }
    return Status::error(StatusCode::NoAuthoritativeGeneration,
                         "no slot holds a usable generation: " + detail)
        .with_context("store.slots");
  }

  // The chosen slot must replay: applying its journal to its checkpoint has to
  // reproduce its live state byte for byte. A mismatch is reported, not
  // repaired, and the other slot is tried before the load fails.
  std::vector<Candidate*> order;
  for (auto& candidate : candidates) {
    if (candidate.valid) {
      order.push_back(&candidate);
    }
  }
  std::sort(order.begin(), order.end(), [](const Candidate* a, const Candidate* b) {
    return b->info.commit_sequence < a->info.commit_sequence;
  });

  std::string replay_detail;
  for (auto* candidate : order) {
    DomainState folded = candidate->snapshot.checkpoint;
    bool ok = true;
    for (const auto& entry : candidate->snapshot.journal) {
      auto applied = apply_journal_entry(folded, entry);
      if (!applied.ok()) {
        replay_detail = std::string{applied.status().message()};
        ok = false;
        break;
      }
    }
    if (!ok) {
      continue;
    }
    if (!(encode_state(folded) == encode_state(candidate->snapshot.live))) {
      replay_detail = "journal replay does not reproduce the persisted live state";
      continue;
    }
    static_cast<void>(replay_detail);
    // A fallback happened when the loaded slot is not the newest parsable one,
    // and equally when a slot that exists on disk was unusable: in both cases
    // the previous complete generation is what was loaded.
    bool skipped_unusable = false;
    for (const auto& other : candidates) {
      if (&other != candidate && other.exists && !other.valid) {
        skipped_unusable = true;
      }
    }
    status_.slots = {candidates[0].info, candidates[1].info};
    status_.has_state = true;
    status_.commit_sequence = candidate->info.commit_sequence;
    status_.generation = candidate->info.generation;
    status_.committed_at = candidate->info.committed_at;
    status_.fell_back = !(candidate == best) || skipped_unusable;
    status_.fallback_detail =
        status_.fell_back ? std::string{"the newest slot was unusable; the previous generation was loaded"}
                          : std::string{};
    status_.retired_journal_entries = candidate->snapshot.retired_journal_entries;
    return candidate->snapshot;
  }

  status_.slots = {candidates[0].info, candidates[1].info};
  status_.fell_back = true;
  status_.fallback_detail = replay_detail.empty() ? "no slot replayed" : replay_detail;
  return Status::error(StatusCode::ReplayDivergence,
                       status_.fallback_detail.empty() ? "no usable generation"
                                                       : status_.fallback_detail)
      .with_context("store.replay");
}

Result<void> DurableStore::commit(const StoreSnapshot& snapshot) {
  if (status_.mode == StoreMode::Volatile) {
    return {};
  }
  if (publication_disabled_) {
    return io_error("publication is disabled by fault injection", "store.commit");
  }
  if (auto r = validate(snapshot.live, bounds_); !r.ok()) {
    return r.status();
  }
  if (auto r = validate(snapshot.checkpoint, bounds_); !r.ok()) {
    return r.status();
  }

  const CommitSequence next_sequence =
      CommitSequence::from_value(status_.commit_sequence.value() + 1);
  SlotHeader header;
  header.commit_sequence = next_sequence;
  header.committed_at = snapshot.live.incident.updated_at.value() != 0
                            ? snapshot.live.incident.updated_at
                            : snapshot.checkpoint.incident.updated_at;
  header.generation = StoreGeneration::from_value(status_.generation.value() + 1);

  const auto file_bytes = build_slot_file(snapshot, bounds_, header);
  if (file_bytes.empty()) {
    return Status::error(StatusCode::CheckpointUnavailable, "the snapshot could not be encoded")
        .with_context("store.encode");
  }

  // Choose the inactive slot: the one with the lower commit sequence, or a
  // fixed slot when neither is usable.
  std::size_t target = 0;
  if (status_.slots.size() == 2) {
    if (status_.slots[0].valid && status_.slots[1].valid) {
      target = status_.slots[0].commit_sequence < status_.slots[1].commit_sequence ? 0 : 1;
    } else if (status_.slots[0].valid) {
      target = 1;
    } else {
      target = 0;
    }
  }
  const auto path = join(status_.directory, kSlotNames[target]);
  const auto staging = staging_path(status_.directory, kSlotNames[target]);

  if (auto r = write_and_flush(staging, file_bytes); !r.ok()) {
    return r.status();
  }
  // Read back and compare the staged bytes before anything is published.
  auto readback = read_whole_file(staging);
  if (!readback.ok()) {
    return readback.status();
  }
  if (!(readback.value() == file_bytes)) {
    std::error_code remove_code;
    std::filesystem::remove(staging, remove_code);
    return Status::error(StatusCode::StoreReadbackMismatch,
                         "the staged slot did not read back identical")
        .with_context(staging);
  }
  if (stage_only_) {
    // Fault injection: the staged file exists and is flushed, but the commit
    // point is never reached. This is exactly what a death before publication
    // leaves behind.
    return {};
  }

  std::error_code code;
  std::filesystem::rename(staging, path, code);
  if (code) {
    std::error_code remove_code;
    std::filesystem::remove(staging, remove_code);
    return io_error("publishing the slot failed", path);
  }

  status_.has_state = true;
  status_.commit_sequence = next_sequence;
  status_.generation = header.generation;
  status_.committed_at = header.committed_at;
  status_.slots[target].valid = true;
  status_.slots[target].commit_sequence = next_sequence;
  status_.slots[target].generation = header.generation;
  status_.slots[target].committed_at = header.committed_at;
  status_.slots[target].detail = "valid";
  status_.retired_journal_entries = snapshot.retired_journal_entries;
  status_.fell_back = false;
  status_.fallback_detail.clear();
  status_.commits += 1;
  return {};
}

}  // namespace summon::pfm
