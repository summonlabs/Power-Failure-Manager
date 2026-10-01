// Power Failure Manager -- cross-process crash, lock, and fencing proofs.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0 (see LICENSE).
//
// Every case in this file drives tests/test_child.cpp as a real, independent
// operating-system process: a separate address space, a separate durable store
// handle, and a separate kernel lock. Nothing here is simulated in-process:
// exclusion is proved against another process, and crash consistency is proved
// by force-killing another process while it writes.
//
// There is no timeout mechanism anywhere. A child that stops making progress is
// a defect to diagnose, so the parent waits on the child's own progress output
// and on process exit, never on a clock. A force-killed child is never counted
// as a pass: its exit code is asserted to be a kill, and the store it left
// behind is then interpreted by a third, fresh process.
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <system_error>
#include <thread>
#include <vector>

#include "framework.hpp"
#include "support.hpp"

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
#include <signal.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>
extern char** environ;
#endif

// The shared framework has PFM_CHECK_MSG but no requiring form. A failed
// require must stop the case, and the reason must be machine-visible.
#define PFM_REQUIRE_MSG(condition, message)                                       \
  do {                                                                            \
    if (!(condition)) {                                                           \
      ::pfmtest::report_failure(                                                   \
          __FILE__, __LINE__,                                                      \
          std::string{"required: " #condition " -- "} + std::string{message});     \
      return;                                                                     \
    }                                                                             \
  } while (false)

namespace {

using summon::pfm::ControlEpoch;
using summon::pfm::ControllerIncarnation;
using summon::pfm::ElectricalObservation;
using summon::pfm::ManualClock;
using summon::pfm::PowerFailureRuntime;
using summon::pfm::RequestState;
using summon::pfm::ResponseRequest;
using summon::pfm::ResponseRequestId;
using summon::pfm::ResponseTransport;
using summon::pfm::Result;
using summon::pfm::RuntimeOptions;
using summon::pfm::StatusCode;
using summon::pfm::SyntheticPlant;
using summon::pfm::SyntheticPlantConfig;

// The identity every child process uses unless a case asks for another one.
constexpr std::uint64_t kEpoch = 1;
constexpr std::uint64_t kIncarnation = 1;
constexpr std::int64_t kBaseUnixMillis = 1767225600000LL;

std::string child_executable() { return std::string{PFM_TEST_CHILD_PATH}; }

std::string join(const std::string& directory, const std::string& name) {
  return (std::filesystem::path{directory} / name).string();
}

// --- a disposable directory that also hands out unique capture files --------

// The scratch directory of one case. It is unique per process as well as per
// case, because a child killed by an aborted run keeps a deny-all handle on its
// store lock file: a fixed directory name would let a leaked process fence out
// the next run instead of the run failing for a real reason.
unsigned process_identity() {
#ifdef _WIN32
  return static_cast<unsigned>(::GetCurrentProcessId());
#else
  return static_cast<unsigned>(::getpid());
#endif
}

class Scratch {
 public:
  explicit Scratch(const std::string& tag)
      : directory_(tag + "-" + std::to_string(process_identity())) {}

  Scratch(const Scratch&) = delete;
  Scratch& operator=(const Scratch&) = delete;

  [[nodiscard]] const std::string& path() const { return directory_.path(); }
  [[nodiscard]] std::string child_output_path() {
    counter_ += 1;
    return directory_.file("child-" + std::to_string(counter_) + ".out");
  }
  [[nodiscard]] std::string file(const std::string& name) const {
    return directory_.file(name);
  }

 private:
  pfmtest::TempDirectory directory_;
  unsigned counter_{0};
};

// --- text helpers ----------------------------------------------------------

std::vector<std::string> split_lines(const std::string& text) {
  std::vector<std::string> lines;
  std::string current;
  for (const char character : text) {
    if (character == '\n') {
      if (!current.empty() && current.back() == '\r') {
        current.pop_back();
      }
      lines.push_back(current);
      current.clear();
      continue;
    }
    current.push_back(character);
  }
  if (!current.empty()) {
    lines.push_back(current);
  }
  return lines;
}

bool contains(const std::string& haystack, const std::string& needle) {
  return haystack.find(needle) != std::string::npos;
}

std::vector<std::string> lines_with_prefix(const std::string& text, const std::string& prefix) {
  std::vector<std::string> result;
  for (const auto& line : split_lines(text)) {
    if (line.size() >= prefix.size() && line.compare(0, prefix.size(), prefix) == 0) {
      result.push_back(line);
    }
  }
  return result;
}

// The value of the "key=value" token named by "key" (which includes the '=').
std::string field(const std::string& line, const std::string& key) {
  const auto position = line.find(key);
  if (position == std::string::npos) {
    return std::string{};
  }
  const auto start = position + key.size();
  auto end = line.find_first_of(" \t\r\n", start);
  if (end == std::string::npos) {
    end = line.size();
  }
  return line.substr(start, end - start);
}

bool number_field(const std::string& line, const std::string& key, std::uint64_t& out) {
  const std::string text = field(line, key);
  if (text.empty()) {
    return false;
  }
  std::uint64_t value = 0;
  for (const char character : text) {
    if (character < '0' || character > '9') {
      return false;
    }
    value = value * 10u + static_cast<std::uint64_t>(character - '0');
  }
  out = value;
  return true;
}

bool flag_field(const std::string& line, const std::string& key, bool& out) {
  const std::string text = field(line, key);
  if (text == "1") {
    out = true;
    return true;
  }
  if (text == "0") {
    out = false;
    return true;
  }
  return false;
}

std::string first_line_with_prefix(const std::string& text, const std::string& prefix) {
  const auto lines = lines_with_prefix(text, prefix);
  return lines.empty() ? std::string{} : lines.front();
}

// Every file in the store directory that a commit left staged but unpublished.
std::vector<std::string> staging_files(const std::string& directory) {
  std::vector<std::string> result;
  std::error_code code;
  const std::filesystem::directory_iterator end;
  for (std::filesystem::directory_iterator iterator{directory, code}; !code && iterator != end;
       iterator.increment(code)) {
    const std::string name = iterator->path().filename().string();
    const std::string suffix = ".staging";
    if (name.size() > suffix.size() &&
        name.compare(name.size() - suffix.size(), suffix.size(), suffix) == 0) {
      result.push_back(iterator->path().string());
    }
  }
  std::sort(result.begin(), result.end());
  return result;
}

std::uintmax_t file_size_of(const std::string& path) {
  std::error_code code;
  const auto size = std::filesystem::file_size(path, code);
  return code ? 0 : size;
}

// --- a real operating-system process ---------------------------------------

// Owns a child process whose stdout and stderr are captured to a file (never a
// pipe, so a chatty child can never block on a full pipe) and whose stdin is a
// pipe the parent owns, so "hold the store until I say stop" is deterministic.
class ChildProcess {
 public:
  ChildProcess() = default;
  ~ChildProcess() {
    if (running()) {
      terminate();
    }
    static_cast<void>(wait());
  }

  ChildProcess(const ChildProcess&) = delete;
  ChildProcess& operator=(const ChildProcess&) = delete;

  bool start(const std::vector<std::string>& arguments, const std::string& output_path);
  [[nodiscard]] bool running();
  bool write_stdin(const std::string& text);
  void close_stdin();
  // Force-kills without any chance for the child to unwind, flush, or release
  // anything: exactly the failure mode the lock and the store must survive.
  void terminate();
  // Waits for exit. Returns the exit code, or -1 when the child was killed.
  int wait();
  [[nodiscard]] std::string output() const;
  // Waits, without any bound, until the child's own captured output contains
  // "text". Returns false only once the child has exited without producing it.
  bool wait_for_text(const std::string& text);

 private:
  std::string output_path_{};
#ifdef _WIN32
  HANDLE process_{nullptr};
  HANDLE stdin_write_{nullptr};
#else
  int pid_{0};
  int stdin_write_{-1};
#endif
};

#ifdef _WIN32

std::wstring widen(const std::string& text) {
  if (text.empty()) {
    return std::wstring{};
  }
  const int size = ::MultiByteToWideChar(CP_UTF8, 0, text.data(),
                                         static_cast<int>(text.size()), nullptr, 0);
  if (size <= 0) {
    return std::wstring{};
  }
  std::wstring wide(static_cast<std::size_t>(size), L'\0');
  ::MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), wide.data(), size);
  return wide;
}

// The command-line quoting the C runtime expects for argv round-tripping.
std::wstring quote_argument(const std::wstring& argument) {
  std::wstring result;
  result.push_back(L'"');
  std::size_t backslashes = 0;
  for (const wchar_t character : argument) {
    if (character == L'\\') {
      backslashes += 1;
      continue;
    }
    if (character == L'"') {
      result.append(backslashes * 2 + 1, L'\\');
      result.push_back(L'"');
      backslashes = 0;
      continue;
    }
    result.append(backslashes, L'\\');
    backslashes = 0;
    result.push_back(character);
  }
  result.append(backslashes * 2, L'\\');
  result.push_back(L'"');
  return result;
}

bool ChildProcess::start(const std::vector<std::string>& arguments,
                         const std::string& output_path) {
  if (process_ != nullptr) {
    return false;
  }
  output_path_ = output_path;
  SECURITY_ATTRIBUTES attributes{};
  attributes.nLength = sizeof(attributes);
  attributes.bInheritHandle = TRUE;
  HANDLE read_end = nullptr;
  HANDLE write_end = nullptr;
  if (::CreatePipe(&read_end, &write_end, &attributes, 0) == 0) {
    return false;
  }
  // The child must never inherit the writing end, or the parent would be unable
  // to observe end-of-file on its own handle.
  ::SetHandleInformation(write_end, HANDLE_FLAG_INHERIT, 0);
  HANDLE sink = ::CreateFileW(widen(output_path).c_str(), GENERIC_WRITE,
                              FILE_SHARE_READ | FILE_SHARE_WRITE, &attributes, CREATE_ALWAYS,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
  if (sink == INVALID_HANDLE_VALUE) {
    ::CloseHandle(read_end);
    ::CloseHandle(write_end);
    return false;
  }
  std::wstring command = quote_argument(widen(child_executable()));
  for (const auto& argument : arguments) {
    command.push_back(L' ');
    command.append(quote_argument(widen(argument)));
  }
  STARTUPINFOW startup{};
  startup.cb = sizeof(startup);
  startup.dwFlags = STARTF_USESTDHANDLES;
  startup.hStdInput = read_end;
  startup.hStdOutput = sink;
  startup.hStdError = sink;
  PROCESS_INFORMATION information{};
  const BOOL created = ::CreateProcessW(nullptr, command.data(), nullptr, nullptr, TRUE,
                                        CREATE_NO_WINDOW, nullptr, nullptr, &startup, &information);
  ::CloseHandle(read_end);
  ::CloseHandle(sink);
  if (created == 0) {
    ::CloseHandle(write_end);
    return false;
  }
  ::CloseHandle(information.hThread);
  process_ = information.hProcess;
  stdin_write_ = write_end;
  return true;
}

bool ChildProcess::running() {
  if (process_ == nullptr) {
    return false;
  }
  return ::WaitForSingleObject(process_, 0) == WAIT_TIMEOUT;
}

bool ChildProcess::write_stdin(const std::string& text) {
  if (stdin_write_ == nullptr) {
    return false;
  }
  DWORD written = 0;
  const BOOL ok = ::WriteFile(stdin_write_, text.data(), static_cast<DWORD>(text.size()), &written,
                              nullptr);
  return ok != 0 && written == text.size();
}

void ChildProcess::close_stdin() {
  if (stdin_write_ != nullptr) {
    ::CloseHandle(stdin_write_);
    stdin_write_ = nullptr;
  }
}

void ChildProcess::terminate() {
  if (process_ != nullptr) {
    static_cast<void>(::TerminateProcess(process_, 3));
  }
}

int ChildProcess::wait() {
  close_stdin();
  if (process_ == nullptr) {
    return 0;
  }
  ::WaitForSingleObject(process_, INFINITE);
  DWORD code = 0;
  static_cast<void>(::GetExitCodeProcess(process_, &code));
  ::CloseHandle(process_);
  process_ = nullptr;
  return static_cast<int>(code);
}

#else  // !_WIN32

bool ChildProcess::start(const std::vector<std::string>& arguments,
                         const std::string& output_path) {
  if (pid_ != 0) {
    return false;
  }
  output_path_ = output_path;
  int pipe_descriptors[2] = {-1, -1};
  if (::pipe(pipe_descriptors) != 0) {
    return false;
  }
  const int sink = ::open(output_path.c_str(), O_CREAT | O_WRONLY | O_TRUNC, 0644);
  if (sink < 0) {
    ::close(pipe_descriptors[0]);
    ::close(pipe_descriptors[1]);
    return false;
  }
  posix_spawn_file_actions_t actions;
  ::posix_spawn_file_actions_init(&actions);
  ::posix_spawn_file_actions_adddup2(&actions, pipe_descriptors[0], STDIN_FILENO);
  ::posix_spawn_file_actions_adddup2(&actions, sink, STDOUT_FILENO);
  ::posix_spawn_file_actions_adddup2(&actions, sink, STDERR_FILENO);
  ::posix_spawn_file_actions_addclose(&actions, pipe_descriptors[1]);
  std::vector<std::string> owned;
  owned.push_back(child_executable());
  for (const auto& argument : arguments) {
    owned.push_back(argument);
  }
  std::vector<char*> argv;
  argv.reserve(owned.size() + 1);
  for (auto& item : owned) {
    argv.push_back(item.data());
  }
  argv.push_back(nullptr);
  pid_t pid = 0;
  const int spawned =
      ::posix_spawn(&pid, owned.front().c_str(), &actions, nullptr, argv.data(), environ);
  ::posix_spawn_file_actions_destroy(&actions);
  ::close(pipe_descriptors[0]);
  ::close(sink);
  if (spawned != 0) {
    ::close(pipe_descriptors[1]);
    return false;
  }
  pid_ = static_cast<int>(pid);
  stdin_write_ = pipe_descriptors[1];
  return true;
}

bool ChildProcess::running() {
  if (pid_ == 0) {
    return false;
  }
  int status = 0;
  const pid_t result = ::waitpid(static_cast<pid_t>(pid_), &status, WNOHANG);
  return result == 0;
}

bool ChildProcess::write_stdin(const std::string& text) {
  if (stdin_write_ < 0) {
    return false;
  }
  const ssize_t written = ::write(stdin_write_, text.data(), text.size());
  return written == static_cast<ssize_t>(text.size());
}

void ChildProcess::close_stdin() {
  if (stdin_write_ >= 0) {
    ::close(stdin_write_);
    stdin_write_ = -1;
  }
}

void ChildProcess::terminate() {
  if (pid_ != 0) {
    ::kill(static_cast<pid_t>(pid_), SIGKILL);
  }
}

int ChildProcess::wait() {
  close_stdin();
  if (pid_ == 0) {
    return 0;
  }
  int status = 0;
  static_cast<void>(::waitpid(static_cast<pid_t>(pid_), &status, 0));
  pid_ = 0;
  if (WIFSIGNALED(status)) {
    return -1;
  }
  return WEXITSTATUS(status);
}

#endif  // _WIN32

std::string ChildProcess::output() const {
  std::ifstream stream{output_path_, std::ios::binary};
  if (!stream) {
    return std::string{};
  }
  // The child's stdout is a file handle in text mode, so every line it writes
  // arrives with a carriage return. The captured text is normalised to plain
  // newlines here, once, so no caller has to know that.
  std::string text;
  std::string line;
  while (std::getline(stream, line)) {
    if (!line.empty() && line.back() == '\r') {
      line.pop_back();
    }
    text.append(line);
    text.push_back('\n');
  }
  return text;
}

bool ChildProcess::wait_for_text(const std::string& text) {
  for (;;) {
    const std::string captured = output();
    if (contains(captured, text)) {
      return true;
    }
    if (!running()) {
      return contains(output(), text);
    }
    std::this_thread::yield();
  }
}

struct ChildRun {
  int exit_code{-1};
  std::string output{};
};

ChildRun run_child(Scratch& scratch, const std::vector<std::string>& arguments) {
  ChildRun run;
  ChildProcess child;
  const std::string capture = scratch.child_output_path();
  if (!child.start(arguments, capture)) {
    run.exit_code = -1000;
    run.output = "the child process could not be started";
    return run;
  }
  child.close_stdin();
  run.exit_code = child.wait();
  run.output = child.output();
  return run;
}

// --- the synthetic scenario, reproduced across processes --------------------

SyntheticPlantConfig plant_config() {
  SyntheticPlantConfig config;
  config.site = "site-a";
  return config;
}

std::shared_ptr<ManualClock> make_clock() {
  return std::make_shared<ManualClock>(summon::pfm::timestamp_from_unix_millis(kBaseUnixMillis));
}

RuntimeOptions durable_options(const std::string& directory, std::uint64_t epoch,
                               std::uint64_t incarnation) {
  RuntimeOptions options;
  options.mode = summon::pfm::StoreMode::Durable;
  options.store_directory = directory;
  options.policy = summon::pfm::default_policy();
  options.policy.generation = summon::pfm::PolicyGeneration::from_value(1);
  options.epoch = ControlEpoch::from_value(epoch);
  options.incarnation = ControllerIncarnation::from_value(incarnation);
  return options;
}

// Loses the same utility feed the dying child lost and admits the same evidence
// at the same driven instant. The topology and the incident are already durable
// in the store, so they are not published again.
Result<void> inject_the_same_failure(PowerFailureRuntime& runtime, SyntheticPlant& plant,
                                     ManualClock& clock) {
  const auto feeds = plant.elements_of_kind(summon::pfm::ElementKind::UtilityFeed);
  if (feeds.empty()) {
    return summon::pfm::Status::error(StatusCode::Internal,
                                      "the synthetic plant has no utility feed");
  }
  plant.lose_utility(feeds.front());
  if (auto advanced = clock.advance(summon::pfm::duration_from_seconds(1)); !advanced.ok()) {
    return advanced.status();
  }
  auto instant = clock.now();
  if (!instant.ok()) {
    return instant.status();
  }
  auto token = runtime.current_authority();
  if (!token.ok()) {
    return token.status();
  }
  return runtime.admit_evidence(plant.observe(instant.value()), token.value());
}

// Records every dispatch and answers Accepted. It never answers Indeterminate,
// so a second dispatch of the same logical operation is visible as a duplicate
// rather than being hidden by a synthetic failure.
class RecordingTransport final : public ResponseTransport {
 public:
  Result<summon::pfm::DispatchOutcome> dispatch(const ResponseRequest& request) override {
    requests_.push_back(request);
    return summon::pfm::DispatchOutcome::accepted(request.target.controller, "recorded by the test");
  }

  [[nodiscard]] std::size_t count() const { return requests_.size(); }
  [[nodiscard]] std::size_t count_with_key(const std::string& key) const {
    std::size_t found = 0;
    for (const auto& request : requests_) {
      if (request.idempotency.to_hex() == key) {
        found += 1;
      }
    }
    return found;
  }
  [[nodiscard]] const std::vector<ResponseRequest>& requests() const { return requests_; }
  [[nodiscard]] const ResponseRequest* find_with_key(const std::string& key) const {
    for (const auto& request : requests_) {
      if (request.idempotency.to_hex() == key) {
        return &request;
      }
    }
    return nullptr;
  }

 private:
  std::vector<ResponseRequest> requests_{};
};

std::string describe_request(const ResponseRequest& request) {
  return "id=" + std::to_string(request.id.value()) +
         " attempt=" + std::to_string(request.attempt.value()) +
         " ordinal=" + std::to_string(request.attempt_ordinal) +
         " kind=" + std::string{summon::pfm::request_kind_name(request.kind)} +
         " element=" + request.target.element.to_string() +
         " state=" + std::string{summon::pfm::request_state_name(request.state)} +
         " key=" + request.idempotency.to_hex();
}

struct OpenedRuntime {
  std::unique_ptr<SyntheticPlant> plant{};
  std::shared_ptr<ManualClock> clock{};
  RecordingTransport transport{};
  std::unique_ptr<PowerFailureRuntime> runtime{};

  static Result<std::unique_ptr<OpenedRuntime>> open(const std::string& directory,
                                                     std::uint64_t epoch,
                                                     std::uint64_t incarnation) {
    auto holder = std::unique_ptr<OpenedRuntime>{new OpenedRuntime{}};
    holder->plant = std::make_unique<SyntheticPlant>(plant_config());
    holder->clock = make_clock();
    std::shared_ptr<ResponseTransport> transport{&holder->transport, [](ResponseTransport*) {}};
    auto opened = PowerFailureRuntime::open(durable_options(directory, epoch, incarnation),
                                            holder->clock, transport);
    if (!opened.ok()) {
      return opened.status();
    }
    holder->runtime = std::move(opened.value());
    return holder;
  }
};

// --- parsing what a child reported ------------------------------------------

struct SlotReport {
  bool valid{false};
  std::uint64_t commit{0};
  std::uint64_t generation{0};
};

std::vector<SlotReport> slots_of(const std::string& output) {
  std::vector<SlotReport> slots;
  for (const auto& line : lines_with_prefix(output, "slot ")) {
    SlotReport slot;
    bool valid = false;
    if (flag_field(line, "valid=", valid)) {
      slot.valid = valid;
    }
    static_cast<void>(number_field(line, "commit=", slot.commit));
    static_cast<void>(number_field(line, "generation=", slot.generation));
    slots.push_back(slot);
  }
  return slots;
}

struct Resolution {
  bool found{false};
  std::uint64_t commit{0};
  std::uint64_t generation{0};
  bool fell_back{false};
  bool has_state{false};
  std::string code{};
  std::string digest{};
  std::uint64_t revision{0};
  std::uint64_t requests{0};
};

Resolution resolution_of(const std::string& output) {
  Resolution resolution;
  const std::string line = first_line_with_prefix(output, "resolved ");
  if (line.empty()) {
    return resolution;
  }
  resolution.found = true;
  static_cast<void>(number_field(line, "commit=", resolution.commit));
  static_cast<void>(number_field(line, "generation=", resolution.generation));
  static_cast<void>(number_field(line, "revision=", resolution.revision));
  static_cast<void>(number_field(line, "requests=", resolution.requests));
  static_cast<void>(flag_field(line, "fell_back=", resolution.fell_back));
  static_cast<void>(flag_field(line, "has_state=", resolution.has_state));
  resolution.code = field(line, "code=");
  resolution.digest = field(line, "digest=");
  return resolution;
}

// The commit sequence the child reported on its last progress line before it
// was killed: the store can never resolve to an older generation than one it
// had already published.
struct Progress {
  bool found{false};
  std::uint64_t commit{0};
  std::uint64_t revision{0};
};

Progress last_progress_of(const std::string& output) {
  Progress progress;
  for (const auto& line : split_lines(output)) {
    if (line.compare(0, 6, "cycle ") != 0 && line.compare(0, 7, "primed ") != 0) {
      continue;
    }
    std::uint64_t commit = 0;
    if (!number_field(line, "commit=", commit)) {
      continue;
    }
    progress.found = true;
    progress.commit = commit;
    static_cast<void>(number_field(line, "revision=", progress.revision));
  }
  return progress;
}

// A store that was killed while writing must resolve to exactly one whole
// authoritative generation: the newest slot is valid and complete, its journal
// replays to its live state byte for byte (which is what makes load() succeed
// without falling back), and two independent processes agree on the digest.
void check_whole_generation(const std::string& context, const std::string& store, Scratch& scratch,
                            const Progress& progress) {
  const ChildRun first = run_child(scratch, {"verify", store});
  const ChildRun second = run_child(scratch, {"verify", store});
  const Resolution resolved = resolution_of(first.output);
  PFM_CHECK_MSG(first.exit_code == 0, (context + ": " + first.output).c_str());
  PFM_CHECK_MSG(resolved.found, (context + ": no resolution line: " + first.output).c_str());
  PFM_CHECK_MSG(resolved.has_state, (context + ": the store holds no generation").c_str());
  PFM_CHECK_MSG(resolved.code == "pfm.ok", (context + " code=" + resolved.code).c_str());
  PFM_CHECK_MSG(!resolved.fell_back,
                (context + ": the newest slot was unusable and an older generation was loaded")
                    .c_str());
  PFM_CHECK_MSG(resolved.commit > 0, (context + ": resolved commit is zero").c_str());

  const auto slots = slots_of(first.output);
  PFM_CHECK_EQ(slots.size(), std::size_t{2});
  std::uint64_t newest = 0;
  std::size_t winners = 0;
  std::uint64_t newest_generation = 0;
  for (const auto& slot : slots) {
    if (!slot.valid) {
      continue;
    }
    if (slot.commit > newest) {
      newest = slot.commit;
      newest_generation = slot.generation;
      winners = 1;
    } else if (slot.commit == newest) {
      winners += 1;
    }
  }
  PFM_CHECK_MSG(winners == 1, (context + ": no single whole authoritative slot").c_str());
  PFM_CHECK_MSG(resolved.commit == newest,
                (context + ": the resolved sequence is not the newest valid slot").c_str());
  PFM_CHECK_MSG(resolved.generation == newest_generation,
                (context + ": the resolved generation is not the newest valid slot").c_str());

  const Resolution again = resolution_of(second.output);
  PFM_CHECK_EQ(second.exit_code, 0);
  PFM_CHECK_MSG(again.digest == resolved.digest,
                (context + ": two fresh processes replayed different states").c_str());
  PFM_CHECK_MSG(again.commit == resolved.commit,
                (context + ": two fresh processes resolved different generations").c_str());

  if (progress.found) {
    PFM_CHECK_MSG(resolved.commit >= progress.commit,
                  (context + ": the store lost a generation the writer had already published")
                      .c_str());
    PFM_CHECK_MSG(resolved.revision >= progress.revision,
                  (context + ": the store lost a revision the writer had already published")
                      .c_str());
  }

  // A staging file is never authoritative: removing it changes nothing.
  const auto staged = staging_files(store);
  if (!staged.empty()) {
    for (const auto& path : staged) {
      std::error_code code;
      std::filesystem::remove(path, code);
    }
    const ChildRun third = run_child(scratch, {"verify", store});
    const Resolution without_staging = resolution_of(third.output);
    PFM_CHECK_EQ(third.exit_code, 0);
    PFM_CHECK_MSG(without_staging.commit == resolved.commit,
                  (context + ": a leftover staging file changed the resolved generation").c_str());
    PFM_CHECK_MSG(without_staging.digest == resolved.digest,
                  (context + ": a leftover staging file changed the resolved state").c_str());
  }
}

// --- 1. single-writer exclusion --------------------------------------------

PFM_TEST(single_writer_exclusion_between_processes) {
  Scratch scratch("pfm-lock");
  const std::string store = join(scratch.path(), "store");
  ChildProcess holder;
  PFM_REQUIRE(holder.start({"hold-lock", store}, scratch.child_output_path()));
  PFM_REQUIRE(holder.wait_for_text("locked\n"));

  // A second, independent process is refused with the exact machine code.
  const ChildRun second = run_child(scratch, {"verify", store});
  PFM_CHECK_MSG(second.exit_code == 3, second.output.c_str());
  PFM_CHECK_MSG(contains(second.output, "code=pfm.store_locked"), second.output.c_str());

  // An independent runtime in this process is refused the same way.
  auto clock = make_clock();
  auto refused = PowerFailureRuntime::open(durable_options(store, kEpoch, kIncarnation), clock,
                                           std::shared_ptr<ResponseTransport>{});
  PFM_CHECK_MSG(!refused.ok(), "a second writer opened a store another process holds");
  if (!refused.ok()) {
    PFM_CHECK_EQ(refused.status().code(), StatusCode::StoreLocked);
  }

  // The holder still owns the store: its exclusive lock was never disturbed.
  PFM_CHECK(holder.running());

  // Release the holder through its own stdin and let it exit normally.
  PFM_REQUIRE(holder.write_stdin("release\n"));
  PFM_CHECK_EQ(holder.wait(), 0);
  PFM_CHECK(contains(holder.output(), "released\n"));

  // The same directory now opens again, in a fresh process and in this one.
  const ChildRun third = run_child(scratch, {"verify", store});
  PFM_CHECK_MSG(third.exit_code == 0, third.output.c_str());
  PFM_CHECK_MSG(contains(third.output, "has_state=1"), third.output.c_str());
  auto reopened = PowerFailureRuntime::open(durable_options(store, kEpoch, kIncarnation), clock,
                                            std::shared_ptr<ResponseTransport>{});
  PFM_CHECK_MSG(reopened.ok(), "the store did not reopen after the holder exited normally");
  if (reopened.ok()) {
    PFM_CHECK_OK(reopened.value()->shutdown());
  }
}

// --- 2. abrupt holder death -------------------------------------------------

PFM_TEST(force_killed_holder_releases_the_kernel_lock) {
  Scratch scratch("pfm-kill");
  const std::string store = join(scratch.path(), "store");
  ChildProcess holder;
  PFM_REQUIRE(holder.start({"hold-lock", store}, scratch.child_output_path()));
  PFM_REQUIRE(holder.wait_for_text("locked\n"));
  PFM_REQUIRE(holder.running());

  // A process killed while holding the store gets no chance to release
  // anything: the lock is released by the kernel, which is the only reason a
  // stale lock file can never fence out the next controller.
  holder.terminate();
  const int killed_code = holder.wait();
  PFM_CHECK_MSG(killed_code != 0, "a force-killed holder must never look like a clean exit");

  const ChildRun fresh = run_child(scratch, {"verify", store});
  PFM_CHECK_MSG(fresh.exit_code == 0, fresh.output.c_str());
  PFM_CHECK_MSG(contains(fresh.output, "has_state=1 code=pfm.ok"), fresh.output.c_str());
  PFM_CHECK_MSG(contains(fresh.output, "fell_back=0"), fresh.output.c_str());
  const Resolution resolved = resolution_of(fresh.output);
  PFM_CHECK_MSG(resolved.commit > 0, "the store the killed holder created resolves to nothing");

  // The store is a working single-writer store again.
  auto clock = make_clock();
  auto reopened = PowerFailureRuntime::open(durable_options(store, kEpoch, kIncarnation), clock,
                                            std::shared_ptr<ResponseTransport>{});
  PFM_CHECK_MSG(reopened.ok(), "the store did not reopen after the holder was killed");
  if (reopened.ok()) {
    auto status = reopened.value()->status();
    PFM_CHECK_OK(status);
    if (status.ok()) {
      // Opening a store is itself a durable decision, so the sequence of the
      // reopened runtime is the resolved generation plus its own authority
      // entry: it can never be behind the generation the store resolved to.
      PFM_CHECK_MSG(status.value().commit_sequence.value() >= resolved.commit,
                    "the reopened runtime is behind the generation the store resolved to");
    }
    PFM_CHECK_OK(reopened.value()->shutdown());
  }
}

// --- 3. crash consistency ---------------------------------------------------

PFM_TEST(killed_writer_leaves_one_whole_authoritative_generation) {
  Scratch scratch("pfm-crash");
  // Different kill points land in different phases of a commit: before the
  // first cycle, and several cycles in.
  const std::vector<std::string> kill_points = {"primed", "cycle 1 ", "cycle 2 ", "cycle 3 ",
                                                "cycle 5 ", "cycle 8 "};
  std::vector<std::string> observed;
  std::size_t index = 0;
  for (const auto& kill_point : kill_points) {
    index += 1;
    const std::string store = join(scratch.path(), "store-" + std::to_string(index));
    ChildProcess writer;
    PFM_REQUIRE(writer.start({"commit-loop", store, "40"}, scratch.child_output_path()));
    // The kill instant is chosen from the writer's own progress output, so the
    // process is provably somewhere inside the cycle that follows.
    PFM_REQUIRE(writer.wait_for_text(kill_point));
    writer.terminate();
    const int code = writer.wait();
    PFM_CHECK_MSG(code != 0, "a killed writer must never look like a clean exit");
    const std::string captured = writer.output();
    const Progress progress = last_progress_of(captured);
    check_whole_generation("kill at '" + kill_point + "'", store, scratch, progress);
    const Resolution resolved = resolution_of(run_child(scratch, {"verify", store}).output);
    observed.push_back("kill after '" + kill_point + "' -> commit=" +
                       std::to_string(resolved.commit) + " generation=" +
                       std::to_string(resolved.generation) + " last published=" +
                       std::to_string(progress.commit));
  }
  for (const auto& line : observed) {
    std::printf("  kill point: %s\n", line.c_str());
  }
  std::fflush(stdout);
}

// --- 3b. a staged commit never becomes authoritative ------------------------

PFM_TEST(staging_file_never_becomes_authoritative) {
  Scratch scratch("pfm-stage");
  const std::string store = join(scratch.path(), "store");
  const ChildRun seeded = run_child(scratch, {"commit-loop", store, "1"});
  PFM_REQUIRE_MSG(seeded.exit_code == 0, seeded.output.c_str());
  const ChildRun before = run_child(scratch, {"verify", store});
  const Resolution published = resolution_of(before.output);
  PFM_REQUIRE(before.exit_code == 0);
  PFM_REQUIRE_MSG(published.has_state && published.code == "pfm.ok", before.output.c_str());
  PFM_CHECK(staging_files(store).empty());

  const ChildRun staged = run_child(scratch, {"stage-only", store, "5"});
  PFM_CHECK_MSG(staged.exit_code == 0, staged.output.c_str());
  PFM_CHECK_MSG(contains(staged.output, "\nstaged\n"), staged.output.c_str());
  PFM_CHECK_MSG(contains(staged.output, "stage-only commit=" + std::to_string(published.commit)),
                staged.output.c_str());

  // The commits wrote and flushed staging files and never published them.
  const auto leftovers = staging_files(store);
  PFM_CHECK_MSG(!leftovers.empty(), "no staging file was left behind");
  for (const auto& path : leftovers) {
    PFM_CHECK_MSG(file_size_of(path) > 0, ("the staging file is empty: " + path).c_str());
  }
  std::uint64_t intended = 0;
  bool saw_intent = false;
  for (const auto& line : lines_with_prefix(staged.output, "staged cycle=")) {
    std::uint64_t value = 0;
    if (number_field(line, "intended_commit=", value)) {
      intended = value;
      saw_intent = true;
    }
  }
  PFM_CHECK(saw_intent);
  PFM_CHECK_MSG(intended > published.commit,
                "the staged generation was not newer than the published one");

  // A fresh process resolves exactly the generation that was published, and the
  // staged, newer generation is nowhere to be seen.
  const ChildRun after = run_child(scratch, {"verify", store});
  const Resolution resolved = resolution_of(after.output);
  PFM_CHECK_EQ(after.exit_code, 0);
  PFM_CHECK_MSG(resolved.has_state && resolved.code == "pfm.ok", after.output.c_str());
  PFM_CHECK_EQ(resolved.commit, published.commit);
  PFM_CHECK_EQ(resolved.generation, published.generation);
  PFM_CHECK_EQ(resolved.digest, published.digest);
  PFM_CHECK_MSG(resolved.commit != intended, "a staged generation became authoritative");
  PFM_CHECK_MSG(!resolved.fell_back, "the published generation was not the one resolved");

  // A store whose only commits were staged has no authoritative generation at
  // all, even though the staged bytes are on disk.
  const std::string virgin = join(scratch.path(), "virgin");
  const ChildRun virgin_stage = run_child(scratch, {"stage-only", virgin, "3"});
  PFM_CHECK_MSG(virgin_stage.exit_code == 0, virgin_stage.output.c_str());
  PFM_CHECK(!staging_files(virgin).empty());
  const ChildRun virgin_verify = run_child(scratch, {"verify", virgin});
  const Resolution none = resolution_of(virgin_verify.output);
  PFM_CHECK_EQ(virgin_verify.exit_code, 0);
  PFM_CHECK_MSG(!none.has_state, virgin_verify.output.c_str());
  PFM_CHECK_MSG(none.code == "pfm.store_not_found", virgin_verify.output.c_str());
  PFM_CHECK_EQ(none.commit, std::uint64_t{0});
}

// --- 4. death around dispatch ----------------------------------------------

PFM_TEST(death_around_dispatch_never_produces_a_blind_duplicate) {
  Scratch scratch("pfm-dispatch");
  const std::string store = join(scratch.path(), "store");
  const std::string marker = scratch.file("dispatch.log");

  const ChildRun died = run_child(scratch, {"dispatch-then-die", store, marker});
  PFM_CHECK_MSG(died.exit_code == 0, died.output.c_str());

  // Exactly one dispatch reached a transport, under exactly one idempotency key.
  std::ifstream marker_stream{marker};
  PFM_REQUIRE(marker_stream.good());
  std::string marker_text;
  {
    std::string line;
    while (std::getline(marker_stream, line)) {
      marker_text.append(line);
      marker_text.push_back('\n');
    }
  }
  const auto dispatch_lines = lines_with_prefix(marker_text, "dispatch ");
  PFM_REQUIRE_MSG(dispatch_lines.size() == 1, marker_text.c_str());
  const std::string key = field(dispatch_lines.front(), "key=");
  const std::string fingerprint = field(dispatch_lines.front(), "fingerprint=");
  std::uint64_t first_attempt = 0;
  PFM_CHECK(!key.empty());
  PFM_CHECK(!fingerprint.empty());
  PFM_CHECK(number_field(dispatch_lines.front(), "attempt=", first_attempt));

  // A fresh process reports the in-flight operation as Indeterminate, under the
  // same idempotency key, and does not dispatch anything.
  const ChildRun report = run_child(scratch, {"reopen-and-report", store});
  PFM_CHECK_MSG(report.exit_code == 0, report.output.c_str());
  std::size_t matching = 0;
  std::size_t indeterminate = 0;
  for (const auto& line : lines_with_prefix(report.output, "request ")) {
    if (field(line, "idempotency=") != key) {
      continue;
    }
    matching += 1;
    if (field(line, "state=") == "indeterminate") {
      indeterminate += 1;
    }
  }
  PFM_CHECK_EQ(matching, std::size_t{1});
  PFM_CHECK_EQ(indeterminate, std::size_t{1});
  PFM_CHECK_MSG(contains(report.output, "recovered=1"),
                "restored durable evidence must not count as current evidence");

  // Reopening here cannot silently retry, and re-deriving the very same failure
  // cannot either: the outstanding operation is carried forward, not repeated.
  auto holder = OpenedRuntime::open(store, kEpoch, kIncarnation);
  PFM_REQUIRE(holder.ok());
  auto& runtime = *holder.value()->runtime;
  auto& plant = *holder.value()->plant;
  auto& clock = *holder.value()->clock;
  auto& transport = holder.value()->transport;
  PFM_CHECK_EQ(transport.count(), std::size_t{0});

  auto reopened = runtime.requests();
  PFM_REQUIRE(reopened.ok());
  auto injected = inject_the_same_failure(runtime, plant, clock);
  PFM_REQUIRE_MSG(injected.ok(), std::string{injected.status().code_name()}.c_str());
  auto token = runtime.current_authority();
  PFM_REQUIRE(token.ok());
  auto outcome = runtime.evaluate(token.value());
  PFM_REQUIRE_MSG(outcome.ok(), std::string{outcome.status().code_name()}.c_str());
  // The operation that was in flight when the process died is never repeated:
  // its idempotency key does not reach a transport again, and it stays
  // Indeterminate until a caller resolves it explicitly. A re-derivation may
  // legitimately plan a *different* operation, which carries its own key.
  PFM_CHECK_MSG(transport.count_with_key(key) == 0,
                "the in-flight operation was silently retried after a death around dispatch");
  for (const auto& request : transport.requests()) {
    PFM_CHECK_MSG(request.idempotency.to_hex() != key, describe_request(request).c_str());
  }
  if (transport.count() != 0) {
    std::string detail = "operations planned by the re-derivation (none is the in-flight one):";
    for (const auto& request : transport.requests()) {
      detail += " [" + describe_request(request) + "]";
    }
    std::printf("  note: %s\n", detail.c_str());
    std::fflush(stdout);
  }

  auto requests = runtime.requests();
  PFM_REQUIRE(requests.ok());
  ResponseRequestId inflight{};
  std::size_t open_operations = 0;
  for (const auto& request : requests.value()) {
    if (request.idempotency.to_hex() == key) {
      inflight = request.id;
      PFM_CHECK_MSG(request.state == RequestState::Indeterminate,
                    "the in-flight operation is no longer indeterminate");
    }
    if (summon::pfm::request_state_is_open(request.state)) {
      open_operations += 1;
    }
  }
  PFM_CHECK(inflight.is_set());
  PFM_REQUIRE_MSG(open_operations >= 2,
                  "the plan did not retain a second outstanding operation to retry");

  // An explicit retry mints a new attempt identity and reuses the same
  // idempotency key, because it is the same consequential operation.
  auto refreshed = runtime.current_authority();
  PFM_REQUIRE(refreshed.ok());
  PFM_CHECK_OK(runtime.retry_request(inflight, refreshed.value()));
  auto retried = runtime.request(inflight);
  PFM_REQUIRE(retried.ok());
  PFM_CHECK_EQ(retried.value().state, RequestState::Superseded);
  auto after_retry = runtime.requests();
  PFM_REQUIRE(after_retry.ok());
  ResponseRequest fresh{};
  bool found_fresh = false;
  for (const auto& request : after_retry.value()) {
    if (request.idempotency.to_hex() == key &&
        summon::pfm::request_state_is_open(request.state)) {
      fresh = request;
      found_fresh = true;
    }
  }
  PFM_REQUIRE_MSG(found_fresh, "the retry minted no open attempt");
  PFM_CHECK_MSG(fresh.id != inflight, "the retry reused the request identity");
  PFM_CHECK_MSG(fresh.attempt.value() != first_attempt, "the retry reused the attempt identity");
  PFM_CHECK_EQ(fresh.attempt_ordinal, 2u);
  PFM_CHECK_EQ(fresh.fingerprint.to_hex(), fingerprint);
  PFM_CHECK_EQ(fresh.idempotency.to_hex(), key);
  const ResponseRequestId retried_id = fresh.id;
  const std::uint64_t retried_attempt = fresh.attempt.value();

  // The re-attempt really reaches a transport, under the same key: one
  // consequential operation, one idempotency key, two attempt identities.
  auto again = runtime.current_authority();
  PFM_REQUIRE(again.ok());
  const std::size_t before_retry_dispatch = transport.count();
  auto second_outcome = runtime.evaluate(again.value());
  PFM_REQUIRE_MSG(second_outcome.ok(), std::string{second_outcome.status().code_name()}.c_str());
  PFM_CHECK_EQ(transport.count_with_key(key), std::size_t{1});
  PFM_CHECK_MSG(transport.count() == before_retry_dispatch + 1,
                "the re-attempt did not reach a transport exactly once");
  const ResponseRequest* redispatch = transport.find_with_key(key);
  PFM_REQUIRE(redispatch != nullptr);
  PFM_CHECK_EQ(redispatch->fingerprint.to_hex(), fingerprint);
  PFM_CHECK_EQ(redispatch->id, retried_id);
  PFM_CHECK_EQ(redispatch->attempt.value(), retried_attempt);
  PFM_CHECK_MSG(redispatch->attempt.value() != first_attempt,
                "the re-dispatch reused the attempt identity of the dead process");

  // Across the whole retained history there is exactly one distinct
  // idempotency key for this logical operation.
  auto final_requests = runtime.requests();
  PFM_REQUIRE(final_requests.ok());
  std::size_t attempts = 0;
  for (const auto& request : final_requests.value()) {
    if (request.fingerprint.to_hex() != fingerprint) {
      continue;
    }
    attempts += 1;
    PFM_CHECK_EQ(request.idempotency.to_hex(), key);
  }
  PFM_CHECK_EQ(attempts, std::size_t{2});

  // Resolving an indeterminate dispatch explicitly is the other legal exit: the
  // caller states, with an observation, that the effect did not happen.
  ResponseRequestId other{};
  for (const auto& request : final_requests.value()) {
    if (request.id != inflight && request.id != retried_id &&
        request.state == RequestState::Indeterminate) {
      other = request.id;
      break;
    }
  }
  PFM_REQUIRE(other.is_set());
  auto resolved_token = runtime.current_authority();
  PFM_REQUIRE(resolved_token.ok());
  PFM_CHECK_OK(runtime.resolve_indeterminate(other, false, ElectricalObservation{},
                                             resolved_token.value()));
  auto resolved = runtime.request(other);
  PFM_REQUIRE(resolved.ok());
  PFM_CHECK_EQ(resolved.value().state, RequestState::Failed);
  auto settled = runtime.requests();
  PFM_REQUIRE(settled.ok());
  PFM_CHECK_EQ(settled.value().size(), final_requests.value().size());
  PFM_CHECK_OK(runtime.shutdown());
}

// --- 5. epoch rollover across processes ------------------------------------

PFM_TEST(epoch_rollover_supersedes_requests_and_clears_recovery) {
  Scratch scratch("pfm-epoch");
  const std::string store = join(scratch.path(), "store");
  const std::string marker = scratch.file("dispatch.log");
  const ChildRun died = run_child(scratch, {"dispatch-then-die", store, marker});
  PFM_REQUIRE_MSG(died.exit_code == 0, died.output.c_str());

  // Same epoch, different incarnation: a new controller must roll the epoch,
  // and the refusal happens in an independent process.
  const ChildRun usurper = run_child(scratch, {"reopen-and-report", store, "--incarnation", "2"});
  PFM_CHECK_MSG(usurper.exit_code == 3, usurper.output.c_str());
  PFM_CHECK_MSG(contains(usurper.output, "code=pfm.stale_incarnation"), usurper.output.c_str());

  // The refused open published nothing: the original identity still reopens.
  const ChildRun unchanged = run_child(scratch, {"reopen-and-report", store});
  PFM_CHECK_MSG(unchanged.exit_code == 0, unchanged.output.c_str());
  PFM_CHECK_MSG(contains(unchanged.output, "authority epoch=1 incarnation=1"),
                unchanged.output.c_str());

  auto holder = OpenedRuntime::open(store, kEpoch, kIncarnation);
  PFM_REQUIRE(holder.ok());
  auto& runtime = *holder.value()->runtime;
  auto& plant = *holder.value()->plant;
  auto& clock = *holder.value()->clock;

  // Recovery progress exists before the rollover: a published plan, an explicit
  // operator authorization, and outstanding requests.
  auto injected = inject_the_same_failure(runtime, plant, clock);
  PFM_REQUIRE_MSG(injected.ok(), std::string{injected.status().code_name()}.c_str());
  auto token = runtime.current_authority();
  PFM_REQUIRE(token.ok());
  auto evaluated = runtime.evaluate(token.value());
  PFM_REQUIRE_MSG(evaluated.ok(), std::string{evaluated.status().code_name()}.c_str());
  auto authorizing = runtime.current_authority();
  PFM_REQUIRE(authorizing.ok());
  PFM_REQUIRE(runtime.authorize_recovery(authorizing.value()).ok());
  auto before = runtime.state();
  PFM_REQUIRE(before.ok());
  PFM_CHECK(before.value().has_plan);
  PFM_CHECK(before.value().incident.operator_authorized);
  PFM_CHECK(before.value().incident.authorized_generation.is_set());

  std::vector<ResponseRequestId> previously_open;
  auto requests = runtime.requests();
  PFM_REQUIRE(requests.ok());
  for (const auto& request : requests.value()) {
    if (summon::pfm::request_state_is_open(request.state)) {
      previously_open.push_back(request.id);
    }
  }
  PFM_REQUIRE(!previously_open.empty());

  // The explicit rollover.
  PFM_CHECK_OK(
      runtime.roll_epoch(ControlEpoch::from_value(2), ControllerIncarnation::from_value(2)));

  auto after = runtime.state();
  PFM_REQUIRE(after.ok());
  PFM_CHECK_EQ(after.value().epoch.value(), std::uint64_t{2});
  PFM_CHECK_EQ(after.value().incarnation.value(), std::uint64_t{2});
  for (const auto id : previously_open) {
    auto request = runtime.request(id);
    PFM_REQUIRE(request.ok());
    PFM_CHECK_EQ(request.value().state, RequestState::Superseded);
    PFM_CHECK_EQ(request.value().terminal_reason, summon::pfm::ReasonCode::JustificationWithdrawn);
  }
  std::size_t still_open = 0;
  for (const auto& slot : after.value().requests) {
    if (summon::pfm::request_state_is_open(slot.request.state)) {
      still_open += 1;
    }
  }
  PFM_CHECK_EQ(still_open, std::size_t{0});
  // Recovery progress is cleared, never inherited from the previous authority.
  PFM_CHECK(!after.value().incident.operator_authorized);
  PFM_CHECK(!after.value().incident.authorized_generation.is_set());
  PFM_CHECK(!after.value().incident.has_stable_since);
  PFM_CHECK_EQ(after.value().incident.recovery_steps, std::uint64_t{0});
  PFM_CHECK_MSG(!after.value().has_plan, "a rolled epoch inherited the previous plan");
  for (const auto& slot : after.value().observations) {
    PFM_CHECK_MSG(!slot.present || slot.recovered,
                  "a rolled epoch treated restored evidence as current");
  }
  PFM_CHECK_OK(runtime.shutdown());
  holder.value()->runtime.reset();

  // The rolled authority, the superseded requests, and the cleared recovery
  // progress are what a fresh process reads back from the store.
  const ChildRun durable =
      run_child(scratch, {"reopen-and-report", store, "--epoch", "2", "--incarnation", "2"});
  PFM_CHECK_MSG(durable.exit_code == 0, durable.output.c_str());
  PFM_CHECK_MSG(contains(durable.output, "authority epoch=2 incarnation=2"),
                durable.output.c_str());
  PFM_CHECK_MSG(contains(durable.output, "recovery authorized=0 stable=0 steps=0"),
                durable.output.c_str());
  const auto durable_requests = lines_with_prefix(durable.output, "request ");
  PFM_REQUIRE(!durable_requests.empty());
  for (const auto& line : durable_requests) {
    PFM_CHECK_MSG(field(line, "state=") == "superseded", line.c_str());
  }

  // An epoch behind the durable one is refused as well, in a fresh process.
  const ChildRun backwards = run_child(scratch, {"reopen-and-report", store, "--epoch", "1"});
  PFM_CHECK_MSG(backwards.exit_code == 3, backwards.output.c_str());
  PFM_CHECK_MSG(contains(backwards.output, "code=pfm.stale_epoch"), backwards.output.c_str());
}

}  // namespace

PFM_TEST_MAIN()




