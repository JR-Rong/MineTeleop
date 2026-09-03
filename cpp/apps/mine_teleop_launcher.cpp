#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <fcntl.h>
#include <poll.h>
#include <sys/file.h>
#include <sys/prctl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

namespace {

constexpr std::uintmax_t kDefaultRuntimeLogMaxBytes = 64U * 1024U * 1024U;
constexpr int kDefaultRuntimeLogRotations = 5;
constexpr std::size_t kRelayBufferBytes = 16U * 1024U;
constexpr int kRelayReadsPerStream = 16;
// Bound the partial-line buffer kept while splitting relayed output into
// lines; a writer that never emits a newline must not grow memory without
// limit, so an over-long pending fragment is persisted as-is.
constexpr std::size_t kMaxPendingLogLineBytes = 64U * 1024U;

volatile std::sig_atomic_t relay_signal = 0;

void set_environment(const char* name, const std::string& value) {
  if (setenv(name, value.c_str(), 1) != 0) {
    std::perror(name);
    std::exit(126);
  }
}

void handle_relay_signal(int signal) {
  relay_signal = signal;
}

bool write_all(
    int descriptor,
    const char* data,
    std::size_t size,
    bool interrupt_for_relay_signal = false) {
  while (size > 0) {
    if (interrupt_for_relay_signal && relay_signal != 0) {
      errno = EINTR;
      return false;
    }
    const auto written = ::write(descriptor, data, size);
    if (written > 0) {
      data += written;
      size -= static_cast<std::size_t>(written);
      continue;
    }
    if (written < 0 && errno == EINTR) {
      if (interrupt_for_relay_signal && relay_signal != 0) return false;
      continue;
    }
    return false;
  }
  return true;
}

std::string json_escape(std::string_view value) {
  std::string escaped;
  escaped.reserve(value.size());
  for (const unsigned char character : value) {
    switch (character) {
      case '\"': escaped += "\\\""; break;
      case '\\': escaped += "\\\\"; break;
      case '\b': escaped += "\\b"; break;
      case '\f': escaped += "\\f"; break;
      case '\n': escaped += "\\n"; break;
      case '\r': escaped += "\\r"; break;
      case '\t': escaped += "\\t"; break;
      default:
        if (character < 0x20U) {
          constexpr char hex[] = "0123456789abcdef";
          escaped += "\\u00";
          escaped += hex[(character >> 4U) & 0x0fU];
          escaped += hex[character & 0x0fU];
        } else {
          escaped += static_cast<char>(character);
        }
    }
  }
  return escaped;
}

std::string runtime_log_event(
    std::string_view event,
    std::string_view issue_code,
    std::string_view severity,
    const std::filesystem::path& path,
    std::string_view error,
    std::string_view operator_action,
    std::uintmax_t max_bytes = 0,
    int rotations = 0) {
  const auto event_at_utc_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::system_clock::now().time_since_epoch()).count();
  std::string line =
      "{\"event_at_utc_ms\":" + std::to_string(event_at_utc_ms) +
      ",\"event\":\"" + json_escape(event) +
      "\",\"subsystem\":\"vehicle_runtime\",\"severity\":\"" +
      json_escape(severity) + "\",\"issue_code\":\"" +
      json_escape(issue_code) + "\",\"stage\":\"runtime_log_relay\",\"log_path\":\"" +
      json_escape(path.string()) + "\"";
  if (!error.empty()) line += ",\"error\":\"" + json_escape(error) + "\"";
  if (max_bytes > 0) {
    line += ",\"log_max_bytes\":" + std::to_string(max_bytes);
    line += ",\"log_rotations\":" + std::to_string(rotations);
  }
  line += ",\"operator_action\":\"" + json_escape(operator_action) + "\"}\n";
  return line;
}

std::uintmax_t positive_environment_integer(
    const char* name,
    std::uintmax_t fallback) {
  const char* value = std::getenv(name);
  if (value == nullptr || value[0] == '\0' || value[0] == '-') return fallback;
  char* end = nullptr;
  errno = 0;
  const auto parsed = std::strtoull(value, &end, 10);
  if (errno != 0 || end == value || *end != '\0' || parsed == 0) return fallback;
  return static_cast<std::uintmax_t>(parsed);
}

class RotatingRuntimeLog {
 public:
  RotatingRuntimeLog(std::filesystem::path path, std::uintmax_t max_bytes, int rotations)
      : path_(std::move(path)), max_bytes_(max_bytes), rotations_(rotations) {}

  ~RotatingRuntimeLog() {
    if (descriptor_ >= 0) ::close(descriptor_);
    if (lock_descriptor_ >= 0) ::close(lock_descriptor_);
  }

  RotatingRuntimeLog(const RotatingRuntimeLog&) = delete;
  RotatingRuntimeLog& operator=(const RotatingRuntimeLog&) = delete;

  bool open(std::string& error) {
    std::error_code filesystem_error;
    const auto parent = path_.parent_path();
    if (!parent.empty()) {
      const bool parent_existed = std::filesystem::exists(parent, filesystem_error);
      if (filesystem_error) {
        error = filesystem_error.message();
        return false;
      }
      std::filesystem::create_directories(parent, filesystem_error);
      if (filesystem_error) {
        error = filesystem_error.message();
        return false;
      }
      if (!parent_existed && ::chmod(parent.c_str(), 0750) != 0) {
        error = std::strerror(errno);
        return false;
      }
    }

    const auto lock_path = path_.string() + ".lock";
    // Keep the lock descriptor across exec so the supervised runtime (and its
    // children) continue to exclude a second launcher if this relay is killed.
    lock_descriptor_ = ::open(
        lock_path.c_str(),
        O_RDWR | O_CREAT | O_NOFOLLOW,
        0640);
    if (lock_descriptor_ < 0) {
      error = std::strerror(errno);
      return false;
    }
    struct stat lock_status {};
    if (::fstat(lock_descriptor_, &lock_status) != 0) {
      error = std::strerror(errno);
      return false;
    }
    if (!S_ISREG(lock_status.st_mode) || lock_status.st_nlink != 1) {
      error = "runtime log lock is not a regular file";
      return false;
    }
    if (::fchmod(lock_descriptor_, 0640) != 0) {
      error = std::strerror(errno);
      return false;
    }
    if (::flock(lock_descriptor_, LOCK_EX | LOCK_NB) != 0) {
      error = errno == EWOULDBLOCK
          ? "another vehicle runtime already owns this log"
          : std::strerror(errno);
      return false;
    }

    if (!open_active(O_APPEND, error)) return false;
    if (bytes_ >= max_bytes_ && !rotate(error)) return false;
    return true;
  }

  bool append(std::string_view value, std::string& error) {
    if (descriptor_ < 0) {
      error = "runtime log is unavailable";
      return false;
    }
    const char* cursor = value.data();
    std::size_t remaining = value.size();
    while (remaining > 0) {
      if (bytes_ >= max_bytes_ && !rotate(error)) return false;
      const auto available = max_bytes_ - bytes_;
      const auto chunk = static_cast<std::size_t>(std::min<std::uintmax_t>(available, remaining));
      if (chunk == 0) continue;
      if (!write_all(descriptor_, cursor, chunk)) {
        error = std::strerror(errno);
        ::close(descriptor_);
        descriptor_ = -1;
        return false;
      }
      bytes_ += chunk;
      cursor += chunk;
      remaining -= chunk;
    }
    return true;
  }

  [[nodiscard]] const std::filesystem::path& path() const { return path_; }
  [[nodiscard]] std::uintmax_t max_bytes() const { return max_bytes_; }
  [[nodiscard]] int rotations() const { return rotations_; }

 private:
  bool open_active(int append_flag, std::string& error) {
    descriptor_ = ::open(
        path_.c_str(),
        O_WRONLY | O_CREAT | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK | append_flag,
        0640);
    if (descriptor_ < 0) {
      error = std::strerror(errno);
      return false;
    }
    struct stat status {};
    if (::fstat(descriptor_, &status) != 0) {
      error = std::strerror(errno);
      ::close(descriptor_);
      descriptor_ = -1;
      return false;
    }
    if (!S_ISREG(status.st_mode) || status.st_nlink != 1) {
      error = "runtime log is not a regular file";
      ::close(descriptor_);
      descriptor_ = -1;
      return false;
    }
    if (::fchmod(descriptor_, 0640) != 0) {
      error = std::strerror(errno);
      ::close(descriptor_);
      descriptor_ = -1;
      return false;
    }
    const int flags = ::fcntl(descriptor_, F_GETFL);
    if (flags < 0 || ::fcntl(descriptor_, F_SETFL, flags & ~O_NONBLOCK) != 0) {
      error = std::strerror(errno);
      ::close(descriptor_);
      descriptor_ = -1;
      return false;
    }
    bytes_ = append_flag == O_APPEND && status.st_size > 0
        ? static_cast<std::uintmax_t>(status.st_size)
        : 0;
    return true;
  }

  bool rotate(std::string& error) {
    if (descriptor_ >= 0) {
      ::close(descriptor_);
      descriptor_ = -1;
    }
    const auto fail = [&](const std::string& operation) {
      error = operation + ": " + std::strerror(errno);
      std::string reopen_error;
      open_active(O_APPEND, reopen_error);
      if (descriptor_ < 0 && !reopen_error.empty()) error += "; reopen: " + reopen_error;
      return false;
    };

    if (rotations_ > 0) {
      const auto oldest = path_.string() + "." + std::to_string(rotations_);
      if (::unlink(oldest.c_str()) != 0 && errno != ENOENT) {
        return fail("remove oldest runtime log");
      }
      for (int index = rotations_ - 1; index >= 1; --index) {
        const auto source = path_.string() + "." + std::to_string(index);
        const auto target = path_.string() + "." + std::to_string(index + 1);
        if (::rename(source.c_str(), target.c_str()) != 0 && errno != ENOENT) {
          return fail("rotate runtime log backup");
        }
      }
      const auto first = path_.string() + ".1";
      if (::rename(path_.c_str(), first.c_str()) != 0 && errno != ENOENT) {
        return fail("rotate active runtime log");
      }
    } else if (::unlink(path_.c_str()) != 0 && errno != ENOENT) {
      return fail("truncate active runtime log");
    }
    return open_active(O_TRUNC, error);
  }

  std::filesystem::path path_;
  std::uintmax_t max_bytes_{0};
  int rotations_{0};
  int descriptor_{-1};
  int lock_descriptor_{-1};
  std::uintmax_t bytes_{0};
};

void close_pipe(std::array<int, 2>& descriptors);

bool make_pipe(std::array<int, 2>& descriptors) {
  if (::pipe(descriptors.data()) != 0) return false;
  for (int& descriptor : descriptors) {
    if (descriptor < STDERR_FILENO + 1) {
      const int replacement = ::fcntl(descriptor, F_DUPFD_CLOEXEC, STDERR_FILENO + 1);
      if (replacement < 0) {
        const int saved_errno = errno;
        close_pipe(descriptors);
        errno = saved_errno;
        return false;
      }
      ::close(descriptor);
      descriptor = replacement;
    }
    const int descriptor_flags = ::fcntl(descriptor, F_GETFD);
    if (descriptor_flags < 0 ||
        ::fcntl(descriptor, F_SETFD, descriptor_flags | FD_CLOEXEC) != 0) {
      const int saved_errno = errno;
      close_pipe(descriptors);
      errno = saved_errno;
      return false;
    }
  }
  const int read_flags = ::fcntl(descriptors[0], F_GETFL);
  if (read_flags < 0 ||
      ::fcntl(descriptors[0], F_SETFL, read_flags | O_NONBLOCK) != 0) {
    const int saved_errno = errno;
    close_pipe(descriptors);
    errno = saved_errno;
    return false;
  }
  return true;
}

void close_pipe(std::array<int, 2>& descriptors) {
  for (int& descriptor : descriptors) {
    if (descriptor >= 0) ::close(descriptor);
    descriptor = -1;
  }
}

int exec_runtime(const std::string& runtime, std::vector<char*>& raw) {
  execv(runtime.c_str(), raw.data());
  const auto saved_errno = errno;
  std::perror("mine-teleop-run");
  return saved_errno == ENOENT ? 127 : 126;
}

struct RelayStream {
  int descriptor{-1};
  int terminal_descriptor{-1};
  bool terminal_enabled{true};
  std::string log_pending;
};

bool ascii_digit(char character) {
  return character >= '0' && character <= '9';
}

// The vendor ChassisControl library dumps its full global state through its
// log_printf helper once per control cycle (see UpdateVehicleState ->
// CalculateWheelControlData -> SendCanMessage). Those lines keep value on a
// live operator terminal but bury the persisted runtime log, so lines in the
// vendor format at debug/info level are relayed to the terminal only.
// Vendor format, optionally wrapped in ANSI colour escapes:
//   [2026-09-03 07:34:12.345] [I] [1234] [TAG] message
std::string_view strip_leading_ansi_escapes(std::string_view line) {
  while (line.size() >= 2 && line[0] == '\x1b' && line[1] == '[') {
    std::size_t index = 2;
    while (index < line.size() &&
           (line[index] < 0x40 || line[index] > 0x7e)) {
      ++index;
    }
    if (index >= line.size()) return line;
    line.remove_prefix(index + 1);
  }
  return line;
}

// Returns the vendor log level character when the line matches the vendor
// log_printf prefix, or '\0' when it does not.
char vendor_log_line_level(std::string_view raw_line) {
  const std::string_view line = strip_leading_ansi_escapes(raw_line);
  // "[YYYY-MM-DD HH:MM:SS.mmm] [L] [" is 31 characters.
  if (line.size() < 32) return '\0';
  if (line[0] != '[') return '\0';
  for (const std::size_t index :
       {1, 2, 3, 4, 6, 7, 9, 10, 12, 13, 15, 16, 18, 19, 21, 22, 23}) {
    if (!ascii_digit(line[index])) return '\0';
  }
  if (line[5] != '-' || line[8] != '-' || line[11] != ' ' ||
      line[14] != ':' || line[17] != ':' || line[20] != '.' ||
      line[24] != ']' || line[25] != ' ' || line[26] != '[' ||
      line[28] != ']' || line[29] != ' ' || line[30] != '[') {
    return '\0';
  }
  std::size_t index = 31;
  if (!ascii_digit(line[index])) return '\0';
  while (index < line.size() && ascii_digit(line[index])) ++index;
  if (index + 2 >= line.size() || line[index] != ']' ||
      line[index + 1] != ' ' || line[index + 2] != '[') {
    return '\0';
  }
  return line[27];
}

bool vendor_chatter_line(std::string_view line) {
  const char level = vendor_log_line_level(line);
  return level == 'D' || level == 'I';
}

// Buffers relayed output per stream, splits it into lines, and persists
// everything except vendor debug/info chatter (see vendor_chatter_line).
// Terminal output is unaffected: it is written raw before this helper runs.
void persist_stream_log(
    RelayStream& stream,
    RotatingRuntimeLog& log,
    bool& log_enabled,
    bool& log_failure_reported,
    std::string_view chunk,
    bool flush_partial) {
  if (!log_enabled) return;
  stream.log_pending.append(chunk.data(), chunk.size());

  const auto report_failure = [&](std::string_view error) {
    log_enabled = false;
    if (log_failure_reported) return;
    const auto diagnostic = runtime_log_event(
        "vehicle_runtime_log_write_failed",
        "runtime_log_write_failed",
        "critical",
        log.path(),
        error,
        "Keep the vehicle in a safe state, free disk space or repair the log filesystem, then restart the runtime in a controlled maintenance window.");
    write_all(STDERR_FILENO, diagnostic.data(), diagnostic.size());
    log_failure_reported = true;
  };

  std::size_t consumed = 0;
  while (log_enabled) {
    const auto newline = stream.log_pending.find('\n', consumed);
    if (newline == std::string::npos) break;
    const auto line = std::string_view(stream.log_pending)
                          .substr(consumed, newline - consumed + 1);
    consumed = newline + 1;
    if (vendor_chatter_line(line)) continue;
    std::string error;
    if (!log.append(line, error)) report_failure(error);
  }
  stream.log_pending.erase(0, consumed);

  if (!log_enabled) {
    stream.log_pending.clear();
    return;
  }
  if ((flush_partial || stream.log_pending.size() > kMaxPendingLogLineBytes) &&
      !stream.log_pending.empty()) {
    std::string error;
    if (!vendor_chatter_line(stream.log_pending) &&
        !log.append(stream.log_pending, error)) {
      report_failure(error);
    }
    stream.log_pending.clear();
  }
}

bool drain_stream(
    RelayStream& stream,
    RotatingRuntimeLog& log,
    bool& log_enabled,
    bool& log_failure_reported) {
  std::array<char, kRelayBufferBytes> buffer{};
  bool read_any = false;
  for (int reads = 0; reads < kRelayReadsPerStream; ++reads) {
    const auto count = ::read(stream.descriptor, buffer.data(), buffer.size());
    if (count > 0) {
      read_any = true;
      const auto size = static_cast<std::size_t>(count);
      if (stream.terminal_enabled &&
          !write_all(
              stream.terminal_descriptor,
              buffer.data(),
              size,
              true)) {
        stream.terminal_enabled = false;
      }
      persist_stream_log(
          stream,
          log,
          log_enabled,
          log_failure_reported,
          std::string_view(buffer.data(), size),
          false);
      continue;
    }
    if (count == 0) {
      persist_stream_log(stream, log, log_enabled, log_failure_reported, {}, true);
      ::close(stream.descriptor);
      stream.descriptor = -1;
      return read_any;
    }
    if (errno == EINTR) continue;
    if (errno == EAGAIN || errno == EWOULDBLOCK) return read_any;
    persist_stream_log(stream, log, log_enabled, log_failure_reported, {}, true);
    ::close(stream.descriptor);
    stream.descriptor = -1;
    return read_any;
  }
  return read_any;
}

int relay_runtime(
    const std::string& runtime,
    std::vector<char*>& raw,
    RotatingRuntimeLog& log) {
  std::array<int, 2> stdout_pipe{-1, -1};
  std::array<int, 2> stderr_pipe{-1, -1};
  if (!make_pipe(stdout_pipe) || !make_pipe(stderr_pipe)) {
    const auto saved_errno = errno;
    close_pipe(stdout_pipe);
    close_pipe(stderr_pipe);
    const auto diagnostic = runtime_log_event(
        "vehicle_runtime_log_start_failed",
        "runtime_log_pipe_failed",
        "critical",
        log.path(),
        std::strerror(saved_errno),
        "Repair the process file-descriptor limit before starting the vehicle runtime.");
    std::string ignored;
    log.append(diagnostic, ignored);
    write_all(STDERR_FILENO, diagnostic.data(), diagnostic.size());
    return 126;
  }

  const auto ready = runtime_log_event(
      "vehicle_runtime_log_ready",
      "runtime_log_ready",
      "info",
      log.path(),
      "",
      "No action is required.",
      log.max_bytes(),
      log.rotations());
  std::string ready_error;
  if (!log.append(ready, ready_error)) {
    close_pipe(stdout_pipe);
    close_pipe(stderr_pipe);
    const auto diagnostic = runtime_log_event(
        "vehicle_runtime_log_start_failed",
        "runtime_log_initial_write_failed",
        "critical",
        log.path(),
        ready_error,
        "Repair filesystem permissions or free space before starting the vehicle runtime.");
    write_all(STDERR_FILENO, diagnostic.data(), diagnostic.size());
    return 126;
  }
  write_all(STDERR_FILENO, ready.data(), ready.size());

  sigset_t relay_signals;
  sigset_t prior_signal_mask;
  ::sigemptyset(&relay_signals);
  ::sigaddset(&relay_signals, SIGINT);
  ::sigaddset(&relay_signals, SIGTERM);
  if (::sigprocmask(SIG_BLOCK, &relay_signals, &prior_signal_mask) != 0) {
    close_pipe(stdout_pipe);
    close_pipe(stderr_pipe);
    const auto diagnostic = runtime_log_event(
        "vehicle_runtime_log_start_failed",
        "runtime_log_signal_setup_failed",
        "critical",
        log.path(),
        std::strerror(errno),
        "Repair the process signal environment before starting the vehicle runtime.");
    std::string ignored;
    log.append(diagnostic, ignored);
    write_all(STDERR_FILENO, diagnostic.data(), diagnostic.size());
    return 126;
  }
  struct sigaction relay_action {};
  relay_action.sa_handler = handle_relay_signal;
  ::sigemptyset(&relay_action.sa_mask);
  relay_action.sa_flags = 0;
  if (::sigaction(SIGINT, &relay_action, nullptr) != 0 ||
      ::sigaction(SIGTERM, &relay_action, nullptr) != 0) {
    const auto saved_errno = errno;
    ::sigprocmask(SIG_SETMASK, &prior_signal_mask, nullptr);
    close_pipe(stdout_pipe);
    close_pipe(stderr_pipe);
    const auto diagnostic = runtime_log_event(
        "vehicle_runtime_log_start_failed",
        "runtime_log_signal_setup_failed",
        "critical",
        log.path(),
        std::strerror(saved_errno),
        "Repair the process signal environment before starting the vehicle runtime.");
    std::string ignored;
    log.append(diagnostic, ignored);
    write_all(STDERR_FILENO, diagnostic.data(), diagnostic.size());
    return 126;
  }

  std::fflush(nullptr);
  const auto launcher_pid = ::getpid();
  const auto child = ::fork();
  if (child < 0) {
    const auto saved_errno = errno;
    ::sigprocmask(SIG_SETMASK, &prior_signal_mask, nullptr);
    close_pipe(stdout_pipe);
    close_pipe(stderr_pipe);
    const auto diagnostic = runtime_log_event(
        "vehicle_runtime_log_start_failed",
        "runtime_log_fork_failed",
        "critical",
        log.path(),
        std::strerror(saved_errno),
        "Repair the process limit before starting the vehicle runtime.");
    std::string ignored;
    log.append(diagnostic, ignored);
    write_all(STDERR_FILENO, diagnostic.data(), diagnostic.size());
    return 126;
  }
  if (child == 0) {
    struct sigaction default_action {};
    default_action.sa_handler = SIG_DFL;
    ::sigemptyset(&default_action.sa_mask);
    ::sigaction(SIGINT, &default_action, nullptr);
    ::sigaction(SIGTERM, &default_action, nullptr);
    std::signal(SIGPIPE, SIG_DFL);
    ::sigprocmask(SIG_SETMASK, &prior_signal_mask, nullptr);
    if (::prctl(PR_SET_PDEATHSIG, SIGTERM) != 0) _exit(126);
    if (::getppid() != launcher_pid) _exit(126);
    ::close(stdout_pipe[0]);
    ::close(stderr_pipe[0]);
    if (::dup2(stdout_pipe[1], STDOUT_FILENO) < 0 ||
        ::dup2(stderr_pipe[1], STDERR_FILENO) < 0) {
      _exit(126);
    }
    ::close(stdout_pipe[1]);
    ::close(stderr_pipe[1]);
    _exit(exec_runtime(runtime, raw));
  }

  ::close(stdout_pipe[1]);
  stdout_pipe[1] = -1;
  ::close(stderr_pipe[1]);
  stderr_pipe[1] = -1;

  ::sigprocmask(SIG_SETMASK, &prior_signal_mask, nullptr);

  RelayStream stdout_stream{stdout_pipe[0], STDOUT_FILENO, true, {}};
  RelayStream stderr_stream{stderr_pipe[0], STDERR_FILENO, true, {}};
  bool log_enabled = true;
  bool log_failure_reported = false;
  bool child_reaped = false;
  bool wait_failed = false;
  int child_status = 0;
  int forwarded_signal = 0;
  auto drain_deadline = std::chrono::steady_clock::time_point::max();

  while (!child_reaped || stdout_stream.descriptor >= 0 || stderr_stream.descriptor >= 0) {
    const int signal = relay_signal;
    if (signal != 0) {
      relay_signal = 0;
      forwarded_signal = signal;
      if (!child_reaped) ::kill(child, signal);
    }

    std::array<pollfd, 2> polling{{
        {stdout_stream.descriptor, POLLIN | POLLHUP, 0},
        {stderr_stream.descriptor, POLLIN | POLLHUP, 0},
    }};
    const auto result = ::poll(polling.data(), polling.size(), 100);
    bool read_any = false;
    if (result > 0) {
      if (stdout_stream.descriptor >= 0 && polling[0].revents != 0) {
        read_any = drain_stream(stdout_stream, log, log_enabled, log_failure_reported) || read_any;
      }
      if (stderr_stream.descriptor >= 0 && polling[1].revents != 0) {
        read_any = drain_stream(stderr_stream, log, log_enabled, log_failure_reported) || read_any;
      }
    }

    if (!child_reaped) {
      const auto waited = ::waitpid(child, &child_status, WNOHANG);
      if (waited == child) {
        child_reaped = true;
        drain_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
      }
      if (waited < 0 && errno != EINTR) {
        child_reaped = true;
        wait_failed = true;
        drain_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
      }
    } else if (stdout_stream.descriptor >= 0 || stderr_stream.descriptor >= 0) {
      static_cast<void>(read_any);
      if (std::chrono::steady_clock::now() >= drain_deadline) {
        if (stdout_stream.descriptor >= 0) ::close(stdout_stream.descriptor);
        if (stderr_stream.descriptor >= 0) ::close(stderr_stream.descriptor);
        stdout_stream.descriptor = -1;
        stderr_stream.descriptor = -1;
      }
    }
  }

  if (wait_failed) return 2;
  if (WIFEXITED(child_status)) return WEXITSTATUS(child_status);
  if (WIFSIGNALED(child_status)) return 128 + WTERMSIG(child_status);
  return forwarded_signal == 0 ? 2 : 128 + forwarded_signal;
}

}  // namespace

int main(int argc, char** argv) {
  const auto executable = std::filesystem::read_symlink("/proc/self/exe");
  const auto root = executable.parent_path().parent_path();
  const auto library_path =
      (root / "lib").string() + ":" + (root / "lib/vendor/chassis").string() + ":" +
      (root / "lib/vendor/mvs").string();
  if (chdir(root.c_str()) != 0) {
    std::perror("mine-teleop bundle directory");
    return 126;
  }

  const auto current_path = std::getenv("PATH");
  set_environment(
      "PATH", (root / "bin").string() + ":" + (current_path == nullptr ? "/usr/bin:/bin" : current_path));
  set_environment("LD_LIBRARY_PATH", library_path);
  set_environment("GST_PLUGIN_SYSTEM_PATH_1_0", "");
  set_environment("GST_PLUGIN_PATH_1_0", (root / "lib/gstreamer-1.0").string());
  set_environment("GST_PLUGIN_SCANNER", (root / "bin/gst-plugin-scanner").string());
  set_environment("GST_REGISTRY_FORK", "no");
  set_environment(
      "GST_REGISTRY",
      "/tmp/mine-teleop-gstreamer-registry-" + std::to_string(static_cast<unsigned long>(getuid())) + ".bin");
  set_environment("LIBVA_DRIVERS_PATH", (root / "lib/dri").string());
  set_environment("SSL_CERT_FILE", (root / "config/ca-certificates.crt").string());

  const auto runtime = (root / "bin/mine-teleop").string();
  std::vector<std::string> arguments{runtime};
  if (argc == 1) {
    arguments.emplace_back("vehicle-runtime");
    arguments.emplace_back("--config");
    arguments.push_back((root / "config/vehicle-agent.yaml").string());
  } else {
    for (int index = 1; index < argc; ++index) arguments.emplace_back(argv[index]);
  }

  std::vector<char*> raw;
  raw.reserve(arguments.size() + 1);
  for (auto& argument : arguments) raw.push_back(argument.data());
  raw.push_back(nullptr);

  if (arguments.size() < 2 || arguments[1] != "vehicle-runtime") {
    return exec_runtime(runtime, raw);
  }

  std::signal(SIGPIPE, SIG_IGN);

  const char* configured_path = std::getenv("MINE_TELEOP_VEHICLE_RUNTIME_LOG_PATH");
  const auto log_path = configured_path != nullptr && configured_path[0] != '\0'
      ? std::filesystem::path(configured_path)
      : std::filesystem::path("/var/log/mine-teleop/vehicle-runtime.log");
  const auto max_bytes = positive_environment_integer(
      "MINE_TELEOP_VEHICLE_RUNTIME_LOG_MAX_BYTES",
      kDefaultRuntimeLogMaxBytes);
  const auto rotations = static_cast<int>(std::min<std::uintmax_t>(
      positive_environment_integer(
          "MINE_TELEOP_VEHICLE_RUNTIME_LOG_ROTATIONS",
          kDefaultRuntimeLogRotations),
      100));
  RotatingRuntimeLog log(log_path, max_bytes, rotations);
  std::string error;
  if (!log.open(error)) {
    const auto diagnostic = runtime_log_event(
        "vehicle_runtime_log_start_failed",
        "runtime_log_open_failed",
        "critical",
        log_path,
        error,
        "Create /var/log/mine-teleop with mode 0750 for the vehicle runtime user, verify free space, and retry.");
    write_all(STDERR_FILENO, diagnostic.data(), diagnostic.size());
    return 126;
  }
  set_environment("MINE_TELEOP_RUNTIME_LOG_RELAY", "1");
  return relay_runtime(runtime, raw, log);
}
