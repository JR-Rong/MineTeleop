#include "mine_teleop/platform.hpp"

#include <cerrno>
#include <csignal>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <stdexcept>
#include <string>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <shellapi.h>
#include <windows.h>
#else
#include <spawn.h>
#include <sys/wait.h>

#include <thread>

extern char** environ;
#endif

namespace mine_teleop {

void initialize_network_process() {
  static std::once_flag initialized;
  std::call_once(initialized, [] {
#if !defined(_WIN32)
    if (std::signal(SIGPIPE, SIG_IGN) == SIG_ERR) {
      throw std::runtime_error("failed to ignore SIGPIPE for network transports");
    }
#endif
  });
}

bool is_loopback_bind_address(std::string_view host) {
  return host == "127.0.0.1" || host == "::1";
}

std::string platform_name() {
#if defined(_WIN32)
  return "windows";
#elif defined(__APPLE__)
#if defined(__aarch64__) || defined(__arm64__)
  return "macos-arm64";
#else
  return "macos-x64";
#endif
#elif defined(__linux__)
#if defined(__aarch64__)
  return "linux-arm64";
#else
  return "linux-x64";
#endif
#else
  return "unknown";
#endif
}

bool open_default_browser(std::string_view url, std::string& error) {
  if (!url.starts_with("http://127.0.0.1:") && !url.starts_with("http://[::1]:")) {
    error = "refusing to open a non-loopback control URL";
    return false;
  }
#if defined(_WIN32)
  const auto result = reinterpret_cast<std::intptr_t>(
      ShellExecuteA(nullptr, "open", std::string(url).c_str(), nullptr, nullptr, SW_SHOWNORMAL));
  if (result <= 32) {
    error = "ShellExecute failed with code " + std::to_string(result);
    return false;
  }
  return true;
#else
#if defined(__APPLE__)
  constexpr const char* command = "open";
#else
  constexpr const char* command = "xdg-open";
#endif
  std::string target(url);
  char* arguments[]{const_cast<char*>(command), target.data(), nullptr};
  pid_t child = -1;
  const int result = posix_spawnp(&child, command, nullptr, nullptr, arguments, environ);
  if (result != 0) {
    error = std::string("cannot launch ") + command + ": " + std::strerror(result);
    return false;
  }
  std::thread([child] {
    int status = 0;
    while (waitpid(child, &status, 0) < 0 && errno == EINTR) {
    }
  }).detach();
  return true;
#endif
}

}  // namespace mine_teleop
