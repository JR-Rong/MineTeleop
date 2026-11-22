#include "mine_teleop/platform.hpp"

#include <bit>
#include <cerrno>
#include <csignal>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <new>
#include <stdexcept>
#include <string>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <shellapi.h>
#include <windows.h>
#include <wlanapi.h>
#else
#include <spawn.h>
#include <sys/wait.h>

#include <thread>

extern char** environ;
#endif

namespace mine_teleop {

struct ScopedWifiStreaming::Impl {
#if defined(_WIN32)
  HMODULE library{nullptr};
  HANDLE client{nullptr};
  decltype(&WlanCloseHandle) close_client{nullptr};
  ~Impl() {
    if (client != nullptr && close_client != nullptr) static_cast<void>(close_client(client, nullptr));
    if (library != nullptr) static_cast<void>(FreeLibrary(library));
  }
#endif
};

ScopedWifiStreaming::ScopedWifiStreaming() noexcept {
#if defined(_WIN32)
  try {
    impl_ = std::make_unique<Impl>();
  } catch (const std::bad_alloc&) {
    status_.error_code = ERROR_NOT_ENOUGH_MEMORY;
    return;
  }
  // Resolve the optional service dynamically so Windows Server installations
  // without WLAN support can still start the client normally.
  impl_->library = LoadLibraryExW(L"wlanapi.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
  if (impl_->library == nullptr) {
    status_.error_code = GetLastError();
    return;
  }
  const auto open_client = std::bit_cast<decltype(&WlanOpenHandle)>(GetProcAddress(impl_->library, "WlanOpenHandle"));
  impl_->close_client = std::bit_cast<decltype(&WlanCloseHandle)>(GetProcAddress(impl_->library, "WlanCloseHandle"));
  const auto enumerate = std::bit_cast<decltype(&WlanEnumInterfaces)>(GetProcAddress(impl_->library, "WlanEnumInterfaces"));
  const auto release = std::bit_cast<decltype(&WlanFreeMemory)>(GetProcAddress(impl_->library, "WlanFreeMemory"));
  const auto set_interface = std::bit_cast<decltype(&WlanSetInterface)>(GetProcAddress(impl_->library, "WlanSetInterface"));
  if (open_client == nullptr || impl_->close_client == nullptr || enumerate == nullptr ||
      release == nullptr || set_interface == nullptr) {
    status_.error_code = ERROR_PROC_NOT_FOUND;
    return;
  }
  status_.api_available = true;
  DWORD negotiated_version = 0;
  status_.error_code = open_client(2, nullptr, &negotiated_version, &impl_->client);
  if (status_.error_code != ERROR_SUCCESS) return;
  PWLAN_INTERFACE_INFO_LIST interfaces = nullptr;
  status_.error_code = enumerate(impl_->client, nullptr, &interfaces);
  if (status_.error_code != ERROR_SUCCESS || interfaces == nullptr) {
    if (interfaces != nullptr) release(interfaces);
    if (status_.error_code == ERROR_SUCCESS) status_.error_code = ERROR_INVALID_DATA;
    return;
  }
  for (DWORD index = 0; index < interfaces->dwNumberOfItems; ++index) {
    const auto& adapter = interfaces->InterfaceInfo[index];
    if (adapter.isState != wlan_interface_state_connected) continue;
    ++status_.connected_interfaces;
    BOOL enabled = TRUE;
    const DWORD result = set_interface(impl_->client, &adapter.InterfaceGuid,
        wlan_intf_opcode_media_streaming_mode, sizeof(enabled), &enabled, nullptr);
    if (result == ERROR_SUCCESS) ++status_.enabled_interfaces;
    else if (status_.error_code == ERROR_SUCCESS) status_.error_code = result;
    // The streaming hint need not disable background scans on a given driver.
    // Request this separately, using the same automatically released client.
    BOOL background_scan = FALSE;
    const DWORD scan_result = set_interface(impl_->client, &adapter.InterfaceGuid,
        wlan_intf_opcode_background_scan_enabled, sizeof(background_scan), &background_scan, nullptr);
    if (scan_result == ERROR_SUCCESS) ++status_.background_scan_disabled_interfaces;
    else if (status_.error_code == ERROR_SUCCESS) status_.error_code = scan_result;
  }
  release(interfaces);
#endif
}

ScopedWifiStreaming::~ScopedWifiStreaming() = default;

void initialize_network_process() {
  static std::once_flag initialized;
  std::call_once(initialized, [] {
#if defined(_WIN32)
    WSADATA data{};
    const int result = WSAStartup(MAKEWORD(2, 2), &data);
    if (result != 0) {
      throw std::runtime_error("WSAStartup failed with code " + std::to_string(result));
    }
#else
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
