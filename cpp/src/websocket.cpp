#include "mine_teleop/websocket.hpp"
#include "mine_teleop/platform.hpp"

#include <curl/curl.h>

#if defined(__APPLE__)
#include <CommonCrypto/CommonDigest.h>
#include <Security/Security.h>
#else
#include <openssl/evp.h>
#include <openssl/rand.h>
#endif

#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cerrno>
#include <cstring>
#include <cstdlib>
#include <limits>
#include <mutex>
#include <stdexcept>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace mine_teleop {
namespace {

constexpr std::string_view kWebSocketGuid = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";

void ensure_curl_global() {
  static std::once_flag initialized;
  std::call_once(initialized, [] {
    initialize_network_process();
    if (curl_global_init(CURL_GLOBAL_DEFAULT) != CURLE_OK) {
      throw std::runtime_error("curl_global_init failed");
    }
  });
}

std::string lower(std::string value) {
  std::transform(value.begin(), value.end(), value.begin(), [](unsigned char character) {
    return static_cast<char>(std::tolower(character));
  });
  return value;
}

std::string trim(std::string value) {
  const auto first = value.find_first_not_of(" \t\r\n");
  if (first == std::string::npos) return {};
  const auto last = value.find_last_not_of(" \t\r\n");
  return value.substr(first, last - first + 1);
}

bool valid_utf8(std::string_view value) {
  std::size_t index = 0;
  const auto continuation = [](unsigned char byte) { return byte >= 0x80U && byte <= 0xbfU; };
  while (index < value.size()) {
    const auto first = static_cast<unsigned char>(value[index]);
    if (first <= 0x7fU) {
      ++index;
      continue;
    }
    if (first >= 0xc2U && first <= 0xdfU) {
      if (index + 1 >= value.size() || !continuation(static_cast<unsigned char>(value[index + 1]))) return false;
      index += 2;
      continue;
    }
    if (first >= 0xe0U && first <= 0xefU) {
      if (index + 2 >= value.size()) return false;
      const auto second = static_cast<unsigned char>(value[index + 1]);
      const auto third = static_cast<unsigned char>(value[index + 2]);
      if (!continuation(third) ||
          (first == 0xe0U && (second < 0xa0U || second > 0xbfU)) ||
          (first == 0xedU && (second < 0x80U || second > 0x9fU)) ||
          ((first != 0xe0U && first != 0xedU) && !continuation(second))) {
        return false;
      }
      index += 3;
      continue;
    }
    if (first >= 0xf0U && first <= 0xf4U) {
      if (index + 3 >= value.size()) return false;
      const auto second = static_cast<unsigned char>(value[index + 1]);
      const auto third = static_cast<unsigned char>(value[index + 2]);
      const auto fourth = static_cast<unsigned char>(value[index + 3]);
      if (!continuation(third) || !continuation(fourth) ||
          (first == 0xf0U && (second < 0x90U || second > 0xbfU)) ||
          (first == 0xf4U && (second < 0x80U || second > 0x8fU)) ||
          ((first != 0xf0U && first != 0xf4U) && !continuation(second))) {
        return false;
      }
      index += 4;
      continue;
    }
    return false;
  }
  return true;
}

bool valid_websocket_close_code(std::uint16_t code) {
  if (code >= 3000U && code <= 4999U) return true;
  if (code < 1000U || code > 1014U) return false;
  return code != 1004U && code != 1005U && code != 1006U;
}

void validate_websocket_close_payload(std::string_view payload) {
  if (payload.size() == 1) throw std::invalid_argument("websocket close payload must be empty or include a status code");
  if (payload.size() < 2) return;
  const auto code = static_cast<std::uint16_t>(
      (static_cast<std::uint16_t>(static_cast<unsigned char>(payload[0])) << 8U) |
      static_cast<unsigned char>(payload[1]));
  if (!valid_websocket_close_code(code)) throw std::invalid_argument("invalid websocket close status code");
  if (!valid_utf8(payload.substr(2))) throw std::invalid_argument("websocket close reason must be valid UTF-8");
}

bool supported_websocket_opcode(std::uint8_t opcode) {
  return opcode == 0x1U || opcode == 0x8U || opcode == 0x9U || opcode == 0xaU;
}

std::string base64_encode(const unsigned char* data, std::size_t size) {
  static constexpr std::string_view alphabet =
      "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  std::string encoded;
  encoded.reserve(4 * ((size + 2) / 3));
  for (std::size_t index = 0; index < size; index += 3) {
    const auto remaining = size - index;
    const std::uint32_t block =
        (static_cast<std::uint32_t>(data[index]) << 16U) |
        (remaining > 1 ? static_cast<std::uint32_t>(data[index + 1]) << 8U : 0U) |
        (remaining > 2 ? static_cast<std::uint32_t>(data[index + 2]) : 0U);
    encoded.push_back(alphabet[(block >> 18U) & 0x3fU]);
    encoded.push_back(alphabet[(block >> 12U) & 0x3fU]);
    encoded.push_back(remaining > 1 ? alphabet[(block >> 6U) & 0x3fU] : '=');
    encoded.push_back(remaining > 2 ? alphabet[block & 0x3fU] : '=');
  }
  return encoded;
}

int base64_digit(unsigned char value) {
  if (value >= 'A' && value <= 'Z') return value - 'A';
  if (value >= 'a' && value <= 'z') return value - 'a' + 26;
  if (value >= '0' && value <= '9') return value - '0' + 52;
  if (value == '+') return 62;
  if (value == '/') return 63;
  return -1;
}

std::vector<unsigned char> base64_decode(std::string_view encoded) {
  if (encoded.empty() || encoded.size() % 4 != 0) throw std::invalid_argument("invalid base64");
  std::vector<unsigned char> decoded;
  decoded.reserve(encoded.size() / 4 * 3);
  for (std::size_t index = 0; index < encoded.size(); index += 4) {
    const bool last = index + 4 == encoded.size();
    const bool third_padding = encoded[index + 2] == '=';
    const bool fourth_padding = encoded[index + 3] == '=';
    if (encoded[index] == '=' || encoded[index + 1] == '=' || (third_padding && !fourth_padding) ||
        (!last && (third_padding || fourth_padding))) {
      throw std::invalid_argument("invalid base64");
    }
    const int first = base64_digit(static_cast<unsigned char>(encoded[index]));
    const int second = base64_digit(static_cast<unsigned char>(encoded[index + 1]));
    const int third = third_padding ? 0 : base64_digit(static_cast<unsigned char>(encoded[index + 2]));
    const int fourth = fourth_padding ? 0 : base64_digit(static_cast<unsigned char>(encoded[index + 3]));
    if (first < 0 || second < 0 || third < 0 || fourth < 0) throw std::invalid_argument("invalid base64");
    const auto block = static_cast<std::uint32_t>(
        (first << 18U) | (second << 12U) | (third << 6U) | fourth);
    decoded.push_back(static_cast<unsigned char>((block >> 16U) & 0xffU));
    if (!third_padding) decoded.push_back(static_cast<unsigned char>((block >> 8U) & 0xffU));
    if (!fourth_padding) decoded.push_back(static_cast<unsigned char>(block & 0xffU));
  }
  return decoded;
}

std::array<unsigned char, 20> sha1(std::string_view value) {
  std::array<unsigned char, 20> digest{};
#if defined(__APPLE__)
  if (CC_SHA1(value.data(), static_cast<CC_LONG>(value.size()), digest.data()) == nullptr) {
    throw std::runtime_error("CommonCrypto SHA-1 failed");
  }
#else
  unsigned int digest_size = 0;
  if (EVP_Digest(
          value.data(),
          value.size(),
          digest.data(),
          &digest_size,
          EVP_sha1(),
          nullptr) != 1 ||
      digest_size != digest.size()) {
    throw std::runtime_error("OpenSSL SHA-1 failed");
  }
#endif
  return digest;
}

void random_bytes(unsigned char* output, std::size_t size) {
#if defined(__APPLE__)
  if (SecRandomCopyBytes(kSecRandomDefault, size, output) != errSecSuccess) {
    throw std::runtime_error("Security SecRandomCopyBytes failed");
  }
#else
  if (size > static_cast<std::size_t>(std::numeric_limits<int>::max()) ||
      RAND_bytes(output, static_cast<int>(size)) != 1) {
    throw std::runtime_error("OpenSSL RAND_bytes failed");
  }
#endif
}

bool wait_socket(int socket, short events, std::chrono::milliseconds timeout) {
  pollfd descriptor{socket, events, 0};
  const auto bounded = std::clamp<std::int64_t>(timeout.count(), 0, std::numeric_limits<int>::max());
  while (true) {
    const int result = ::poll(&descriptor, 1, static_cast<int>(bounded));
    if (result > 0) return (descriptor.revents & (events | POLLERR | POLLHUP)) != 0;
    if (result == 0) return false;
    if (errno != EINTR) throw std::runtime_error(std::string("poll failed: ") + std::strerror(errno));
  }
}

void socket_send_all(int socket, std::string_view value) {
  std::size_t sent = 0;
  while (sent < value.size()) {
    const auto result = ::send(socket, value.data() + sent, value.size() - sent, MSG_NOSIGNAL);
    if (result < 0) {
      if (errno == EINTR) continue;
      throw std::runtime_error(std::string("websocket send failed: ") + std::strerror(errno));
    }
    if (result == 0) throw std::runtime_error("websocket closed while sending");
    sent += static_cast<std::size_t>(result);
  }
}

bool socket_receive_exact(
    int socket,
    char* output,
    std::size_t size,
    std::chrono::steady_clock::time_point deadline) {
  std::size_t received = 0;
  while (received < size) {
    const auto now = std::chrono::steady_clock::now();
    if (now >= deadline || !wait_socket(
            socket,
            POLLIN,
            std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now))) {
      throw std::runtime_error("websocket frame timed out");
    }
    const auto result = ::recv(socket, output + received, size - received, 0);
    if (result < 0) {
      if (errno == EINTR) continue;
      throw std::runtime_error(std::string("websocket receive failed: ") + std::strerror(errno));
    }
    if (result == 0) return false;
    received += static_cast<std::size_t>(result);
  }
  return true;
}

std::string url_encode(std::string_view value) {
  static constexpr char hex[] = "0123456789ABCDEF";
  std::string encoded;
  for (const unsigned char character : value) {
    if (std::isalnum(character) || character == '-' || character == '_' || character == '.' || character == '~') {
      encoded.push_back(static_cast<char>(character));
    } else {
      encoded.push_back('%');
      encoded.push_back(hex[character >> 4U]);
      encoded.push_back(hex[character & 0x0fU]);
    }
  }
  return encoded;
}

std::string curl_url_part(CURLU* url, CURLUPart part, unsigned int flags = 0) {
  char* value = nullptr;
  const auto result = curl_url_get(url, part, &value, flags);
  if (result != CURLUE_OK || value == nullptr) return {};
  std::string output(value);
  curl_free(value);
  return output;
}

std::unordered_map<std::string, std::string> parse_http_headers(std::string_view response) {
  std::unordered_map<std::string, std::string> headers;
  const auto first_end = response.find("\r\n");
  std::size_t start = first_end == std::string_view::npos ? response.size() : first_end + 2;
  while (start < response.size()) {
    const auto end = response.find("\r\n", start);
    if (end == std::string_view::npos || end == start) break;
    const auto separator = response.find(':', start);
    if (separator != std::string_view::npos && separator < end) {
      headers[lower(trim(std::string(response.substr(start, separator - start))))] =
          trim(std::string(response.substr(separator + 1, end - separator - 1)));
    }
    start = end + 2;
  }
  return headers;
}

}  // namespace

std::string websocket_accept_key(std::string_view client_key) {
  try {
    if (base64_decode(client_key).size() != 16) throw std::invalid_argument("invalid key length");
  } catch (const std::exception&) {
    throw std::invalid_argument("Sec-WebSocket-Key must be a base64-encoded 16-byte value");
  }
  const auto digest = sha1(std::string(client_key) + std::string(kWebSocketGuid));
  return base64_encode(digest.data(), digest.size());
}

std::string signaling_websocket_url(
    std::string_view signaling_url,
    std::string_view session_id,
    std::string_view participant,
    std::string_view connection_generation) {
  if (session_id.empty() || participant.empty()) {
    throw std::invalid_argument("websocket signaling identity is required");
  }
  std::string origin(signaling_url);
  if (origin.starts_with("http://")) origin.replace(0, 7, "ws://");
  if (origin.starts_with("https://")) origin.replace(0, 8, "wss://");
  if (!origin.starts_with("ws://") && !origin.starts_with("wss://")) {
    throw std::invalid_argument("websocket signaling URL must use ws, wss, http, or https");
  }
  if (origin.ends_with("/signaling")) origin.resize(origin.size() - std::string_view("/signaling").size());
  while (!origin.empty() && origin.back() == '/') origin.pop_back();
  auto url = origin + "/signaling/" + url_encode(session_id) + "/ws?participant=" +
      url_encode(participant);
  if (!connection_generation.empty()) {
    url += "&connection_generation=" + url_encode(connection_generation);
  }
  return url;
}

ServerWebSocketConnection::ServerWebSocketConnection(int socket, std::size_t max_message_bytes)
    : socket_(socket), max_message_bytes_(max_message_bytes) {
  if (socket_ < 0 || max_message_bytes_ == 0) throw std::invalid_argument("invalid server websocket connection");
}

void ServerWebSocketConnection::send_frame(std::uint8_t opcode, std::string_view payload) const {
  std::string frame;
  frame.push_back(static_cast<char>(0x80U | (opcode & 0x0fU)));
  if (payload.size() < 126) {
    frame.push_back(static_cast<char>(payload.size()));
  } else if (payload.size() <= 0xffffU) {
    frame.push_back(126);
    frame.push_back(static_cast<char>((payload.size() >> 8U) & 0xffU));
    frame.push_back(static_cast<char>(payload.size() & 0xffU));
  } else {
    frame.push_back(127);
    for (int shift = 56; shift >= 0; shift -= 8) {
      frame.push_back(static_cast<char>((static_cast<std::uint64_t>(payload.size()) >> shift) & 0xffU));
    }
  }
  frame.append(payload);
  socket_send_all(socket_, frame);
}

void ServerWebSocketConnection::send_json(const Json& value) const { send_frame(0x1, value.dump()); }

void ServerWebSocketConnection::send_close(std::uint16_t code, std::string_view reason) const {
  if (!valid_websocket_close_code(code)) throw std::invalid_argument("invalid websocket close status code");
  if (reason.size() > 123) throw std::invalid_argument("websocket close reason is too long");
  if (!valid_utf8(reason)) throw std::invalid_argument("websocket close reason must be valid UTF-8");
  std::string payload;
  payload.push_back(static_cast<char>((code >> 8U) & 0xffU));
  payload.push_back(static_cast<char>(code & 0xffU));
  payload.append(reason);
  send_frame(0x8, payload);
}

WebSocketReceiveResult ServerWebSocketConnection::receive_json(std::chrono::milliseconds timeout) {
  const auto first_deadline = std::chrono::steady_clock::now() + timeout;
  while (true) {
    if (!wait_socket(socket_, POLLIN, timeout)) return {WebSocketReceiveStatus::Timeout, Json::object()};
    std::array<char, 2> header{};
    if (!socket_receive_exact(socket_, header.data(), header.size(), first_deadline)) {
      return {WebSocketReceiveStatus::Closed, Json::object()};
    }
    const auto first = static_cast<unsigned char>(header[0]);
    const auto second = static_cast<unsigned char>(header[1]);
    const bool final = (first & 0x80U) != 0;
    const auto opcode = static_cast<std::uint8_t>(first & 0x0fU);
    if ((first & 0x70U) != 0) throw std::invalid_argument("websocket reserved bits are not supported");
    if ((second & 0x80U) == 0) throw std::invalid_argument("client websocket frames must be masked");
    if (!supported_websocket_opcode(opcode)) throw std::invalid_argument("unsupported websocket opcode");
    std::uint64_t length = second & 0x7fU;
    if (length == 126) {
      std::array<unsigned char, 2> extended{};
      if (!socket_receive_exact(socket_, reinterpret_cast<char*>(extended.data()), extended.size(), first_deadline)) {
        return {WebSocketReceiveStatus::Closed, Json::object()};
      }
      length = (static_cast<std::uint64_t>(extended[0]) << 8U) | extended[1];
      if (length < 126U) throw std::invalid_argument("websocket frame length is not minimally encoded");
    } else if (length == 127) {
      std::array<unsigned char, 8> extended{};
      if (!socket_receive_exact(socket_, reinterpret_cast<char*>(extended.data()), extended.size(), first_deadline)) {
        return {WebSocketReceiveStatus::Closed, Json::object()};
      }
      if ((extended[0] & 0x80U) != 0) throw std::invalid_argument("invalid websocket frame length");
      length = 0;
      for (const auto byte : extended) length = (length << 8U) | byte;
      if (length <= 0xffffU) throw std::invalid_argument("websocket frame length is not minimally encoded");
    }
    const bool control = opcode == 0x8 || opcode == 0x9 || opcode == 0xa;
    if (control && (!final || length > 125)) {
      throw std::invalid_argument("websocket control frames must be complete and no longer than 125 bytes");
    }
    if (length > max_message_bytes_) throw std::invalid_argument("websocket message too large");
    std::array<unsigned char, 4> mask{};
    if (!socket_receive_exact(socket_, reinterpret_cast<char*>(mask.data()), mask.size(), first_deadline)) {
      return {WebSocketReceiveStatus::Closed, Json::object()};
    }
    std::string payload(static_cast<std::size_t>(length), '\0');
    if (length > 0 && !socket_receive_exact(socket_, payload.data(), payload.size(), first_deadline)) {
      return {WebSocketReceiveStatus::Closed, Json::object()};
    }
    for (std::size_t index = 0; index < payload.size(); ++index) {
      payload[index] = static_cast<char>(static_cast<unsigned char>(payload[index]) ^ mask[index % mask.size()]);
    }
    if (opcode == 0x8) {
      validate_websocket_close_payload(payload);
      send_frame(0x8, payload);
      return {WebSocketReceiveStatus::Closed, Json::object()};
    }
    if (opcode == 0x9) {
      send_frame(0xa, payload);
      continue;
    }
    if (opcode == 0xa) continue;
    if (!final) throw std::invalid_argument("fragmented websocket messages are not supported");
    if (opcode != 0x1) throw std::invalid_argument("only text websocket frames are supported");
    try {
      auto value = Json::parse(payload);
      if (!value.is_object()) throw std::invalid_argument("websocket message must be a JSON object");
      return {WebSocketReceiveStatus::Message, std::move(value)};
    } catch (const Json::exception& error) {
      throw std::invalid_argument(std::string("invalid websocket JSON: ") + error.what());
    }
  }
}

struct WebSocketClient::Impl {
  CURL* curl{nullptr};
  curl_slist* resolve_entries{nullptr};
  std::filesystem::path ca_bundle;
  curl_socket_t socket{CURL_SOCKET_BAD};
  std::string buffered;
  std::size_t max_message_bytes{8 * 1024 * 1024};
  bool peer_closed{false};

  Impl(const std::vector<std::string>& entries, std::filesystem::path next_ca_bundle)
      : ca_bundle(std::move(next_ca_bundle)) {
    try {
      for (const auto& entry : entries) {
        if (entry.empty() || entry.find_first_of("\r\n") != std::string::npos) {
          throw std::invalid_argument("websocket resolve entry is invalid");
        }
        auto* next = curl_slist_append(resolve_entries, entry.c_str());
        if (next == nullptr) throw std::runtime_error("cannot allocate websocket resolve entries");
        resolve_entries = next;
      }
    } catch (...) {
      if (resolve_entries != nullptr) curl_slist_free_all(resolve_entries);
      resolve_entries = nullptr;
      throw;
    }
  }

  ~Impl() {
    if (curl != nullptr) curl_easy_cleanup(curl);
    if (resolve_entries != nullptr) curl_slist_free_all(resolve_entries);
  }

  void send_all(std::string_view value, std::chrono::milliseconds timeout) {
    std::size_t sent = 0;
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (sent < value.size()) {
      std::size_t count = 0;
      const auto result = curl_easy_send(curl, value.data() + sent, value.size() - sent, &count);
      if (result == CURLE_OK) {
        if (count == 0) throw std::runtime_error("websocket closed while sending");
        sent += count;
        continue;
      }
      if (result != CURLE_AGAIN) throw std::runtime_error(std::string("websocket send failed: ") + curl_easy_strerror(result));
      const auto now = std::chrono::steady_clock::now();
      if (now >= deadline || !wait_socket(
              static_cast<int>(socket),
              POLLOUT,
              std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now))) {
        throw std::runtime_error("websocket send timed out");
      }
    }
  }

  bool read_more(std::chrono::steady_clock::time_point deadline) {
    while (true) {
      std::array<char, 16 * 1024> buffer{};
      std::size_t received = 0;
      const auto result = curl_easy_recv(curl, buffer.data(), buffer.size(), &received);
      if (result == CURLE_OK) {
        if (received == 0) {
          peer_closed = true;
          return false;
        }
        buffered.append(buffer.data(), received);
        return true;
      }
      if (result != CURLE_AGAIN) throw std::runtime_error(std::string("websocket receive failed: ") + curl_easy_strerror(result));
      const auto now = std::chrono::steady_clock::now();
      if (now >= deadline || !wait_socket(
              static_cast<int>(socket),
              POLLIN,
              std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now))) {
        return false;
      }
    }
  }

  bool ensure_bytes(std::size_t size, std::chrono::steady_clock::time_point deadline) {
    while (buffered.size() < size) {
      if (!read_more(deadline)) return false;
    }
    return true;
  }

  void send_frame(std::uint8_t opcode, std::string_view payload, std::chrono::milliseconds timeout) {
    std::array<unsigned char, 4> mask{};
    random_bytes(mask.data(), mask.size());
    std::string frame;
    frame.push_back(static_cast<char>(0x80U | (opcode & 0x0fU)));
    if (payload.size() < 126) {
      frame.push_back(static_cast<char>(0x80U | payload.size()));
    } else if (payload.size() <= 0xffffU) {
      frame.push_back(static_cast<char>(0x80U | 126U));
      frame.push_back(static_cast<char>((payload.size() >> 8U) & 0xffU));
      frame.push_back(static_cast<char>(payload.size() & 0xffU));
    } else {
      frame.push_back(static_cast<char>(0x80U | 127U));
      for (int shift = 56; shift >= 0; shift -= 8) {
        frame.push_back(static_cast<char>((static_cast<std::uint64_t>(payload.size()) >> shift) & 0xffU));
      }
    }
    frame.append(reinterpret_cast<const char*>(mask.data()), mask.size());
    for (std::size_t index = 0; index < payload.size(); ++index) {
      frame.push_back(static_cast<char>(static_cast<unsigned char>(payload[index]) ^ mask[index % mask.size()]));
    }
    send_all(frame, timeout);
  }
};

WebSocketClient::WebSocketClient(std::chrono::milliseconds timeout)
    : WebSocketClient(timeout, {}, {}) {}

WebSocketClient::WebSocketClient(
    std::chrono::milliseconds timeout,
    std::vector<std::string> resolve_entries,
    std::filesystem::path ca_bundle)
    : resolve_entries_(std::move(resolve_entries)),
      ca_bundle_(std::move(ca_bundle)),
      impl_(nullptr),
      timeout_(timeout) {
  if (timeout_.count() <= 0) throw std::invalid_argument("websocket timeout must be positive");
  ensure_curl_global();
  impl_ = std::make_unique<Impl>(resolve_entries_, ca_bundle_);
}

WebSocketClient::~WebSocketClient() { close(); }

void WebSocketClient::connect(std::string_view url) { connect(url, {}); }

void WebSocketClient::connect(std::string_view url, const HttpHeaders& request_headers) {
  close();
  if (!url.starts_with("ws://") && !url.starts_with("wss://")) {
    throw std::invalid_argument("websocket URL must use ws or wss");
  }
  url_ = std::string(url);
  std::string transport_url(url);
  if (transport_url.starts_with("ws://")) transport_url.replace(0, 5, "http://");
  if (transport_url.starts_with("wss://")) transport_url.replace(0, 6, "https://");

  CURLU* parsed = curl_url();
  if (parsed == nullptr) throw std::runtime_error("curl_url failed");
  if (curl_url_set(parsed, CURLUPART_URL, transport_url.c_str(), 0) != CURLUE_OK) {
    curl_url_cleanup(parsed);
    throw std::invalid_argument("invalid websocket URL");
  }
  const auto host = curl_url_part(parsed, CURLUPART_HOST);
  const auto port = curl_url_part(parsed, CURLUPART_PORT);
  auto path = curl_url_part(parsed, CURLUPART_PATH);
  const auto query = curl_url_part(parsed, CURLUPART_QUERY);
  if (path.empty()) path = "/";
  curl_url_cleanup(parsed);
  if (host.empty()) throw std::invalid_argument("websocket URL host is required");
  const auto authority_host = host.find(':') == std::string::npos ? host : "[" + host + "]";
  const auto authority = port.empty() ? authority_host : authority_host + ":" + port;

  impl_ = std::make_unique<Impl>(resolve_entries_, ca_bundle_);
  impl_->curl = curl_easy_init();
  if (impl_->curl == nullptr) throw std::runtime_error("curl_easy_init failed");
  curl_easy_setopt(impl_->curl, CURLOPT_URL, transport_url.c_str());
  // Keep WSS on the same direct path as HTTPS even when the host environment
  // has HTTP(S)/ALL_PROXY configured.
  curl_easy_setopt(impl_->curl, CURLOPT_PROXY, "");
  curl_easy_setopt(impl_->curl, CURLOPT_CONNECT_ONLY, 1L);
  curl_easy_setopt(impl_->curl, CURLOPT_HTTP_VERSION, CURL_HTTP_VERSION_1_1);
  curl_easy_setopt(impl_->curl, CURLOPT_CONNECTTIMEOUT_MS, static_cast<long>(timeout_.count()));
  curl_easy_setopt(impl_->curl, CURLOPT_TIMEOUT_MS, static_cast<long>(timeout_.count()));
  curl_easy_setopt(impl_->curl, CURLOPT_NOSIGNAL, 1L);
  curl_easy_setopt(impl_->curl, CURLOPT_USERAGENT, "mine-teleop-websocket/0.2");
  const auto ca_bundle = impl_->ca_bundle.string();
  if (!ca_bundle.empty()) {
    curl_easy_setopt(impl_->curl, CURLOPT_CAINFO, ca_bundle.c_str());
  } else if (const auto* ca_bundle = std::getenv("CURL_CA_BUNDLE"); ca_bundle != nullptr && *ca_bundle != '\0') {
    curl_easy_setopt(impl_->curl, CURLOPT_CAINFO, ca_bundle);
  } else if (const auto* ca_file = std::getenv("SSL_CERT_FILE"); ca_file != nullptr && *ca_file != '\0') {
    curl_easy_setopt(impl_->curl, CURLOPT_CAINFO, ca_file);
  }
  if (impl_->resolve_entries != nullptr) curl_easy_setopt(impl_->curl, CURLOPT_RESOLVE, impl_->resolve_entries);
  const auto result = curl_easy_perform(impl_->curl);
  if (result != CURLE_OK) {
    close();
    throw std::runtime_error(std::string("websocket connect failed: ") + curl_easy_strerror(result));
  }
  if (curl_easy_getinfo(impl_->curl, CURLINFO_ACTIVESOCKET, &impl_->socket) != CURLE_OK ||
      impl_->socket == CURL_SOCKET_BAD) {
    close();
    throw std::runtime_error("websocket connection has no active socket");
  }

  std::array<unsigned char, 16> nonce{};
  random_bytes(nonce.data(), nonce.size());
  const auto key = base64_encode(nonce.data(), nonce.size());
  const auto target = path + (query.empty() ? "" : "?" + query);
  std::string request =
      "GET " + target + " HTTP/1.1\r\nHost: " + authority +
      "\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Key: " + key +
      "\r\nSec-WebSocket-Version: 13\r\nUser-Agent: mine-teleop-websocket/0.2\r\n";
  std::unordered_set<std::string> header_names;
  static const std::unordered_set<std::string> reserved_headers{
      "host",
      "upgrade",
      "connection",
      "sec-websocket-key",
      "sec-websocket-version",
      "user-agent",
  };
  for (const auto& [name, value] : request_headers) {
    const auto canonical_name = lower(name);
    if (name.empty() || name.find_first_of(":\r\n") != std::string::npos ||
        value.find_first_of("\r\n") != std::string::npos ||
        reserved_headers.contains(canonical_name) ||
        !header_names.insert(canonical_name).second) {
      close();
      throw std::invalid_argument("websocket header is invalid or reserved");
    }
    request += name + ": " + value + "\r\n";
  }
  request += "\r\n";
  impl_->send_all(request, timeout_);
  const auto deadline = std::chrono::steady_clock::now() + timeout_;
  while (impl_->buffered.find("\r\n\r\n") == std::string::npos) {
    if (impl_->buffered.size() > 64 * 1024) {
      close();
      throw std::runtime_error("websocket handshake headers are too large");
    }
    if (!impl_->read_more(deadline)) {
      close();
      throw std::runtime_error("websocket handshake timed out");
    }
  }
  const auto end = impl_->buffered.find("\r\n\r\n") + 4;
  const auto response = impl_->buffered.substr(0, end);
  impl_->buffered.erase(0, end);
  const auto line_end = response.find("\r\n");
  if (line_end == std::string::npos || response.substr(0, line_end).find(" 101 ") == std::string::npos) {
    const auto status_line = response.substr(0, line_end);
    close();
    throw std::runtime_error("websocket upgrade failed: " + status_line);
  }
  const auto headers = parse_http_headers(response);
  const auto upgrade = headers.find("upgrade");
  const auto connection = headers.find("connection");
  const auto accept = headers.find("sec-websocket-accept");
  if (upgrade == headers.end() || lower(upgrade->second) != "websocket" || connection == headers.end() ||
      lower(connection->second).find("upgrade") == std::string::npos || accept == headers.end() ||
      accept->second != websocket_accept_key(key)) {
    close();
    throw std::runtime_error("websocket upgrade response is invalid");
  }
}

void WebSocketClient::close() {
  if (impl_ && impl_->curl != nullptr && impl_->socket != CURL_SOCKET_BAD) {
    try {
      impl_->send_frame(0x8, std::string("\x03\xe8", 2), std::chrono::milliseconds(250));
    } catch (const std::exception&) {
    }
  }
  impl_.reset();
  url_.clear();
}

bool WebSocketClient::connected() const {
  return impl_ && impl_->curl != nullptr && impl_->socket != CURL_SOCKET_BAD;
}

void WebSocketClient::send_json(const Json& value) {
  if (!connected()) throw std::runtime_error("websocket is not connected");
  if (!value.is_object()) throw std::invalid_argument("websocket JSON message must be an object");
  impl_->send_frame(0x1, value.dump(), timeout_);
}

WebSocketReceiveResult WebSocketClient::receive_json(std::chrono::milliseconds timeout) {
  if (!connected()) throw std::runtime_error("websocket is not connected");
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  while (true) {
    if (!impl_->ensure_bytes(2, deadline)) {
      if (impl_->peer_closed) {
        close();
        return {WebSocketReceiveStatus::Closed, Json::object()};
      }
      return {WebSocketReceiveStatus::Timeout, Json::object()};
    }
    const auto first = static_cast<unsigned char>(impl_->buffered[0]);
    const auto second = static_cast<unsigned char>(impl_->buffered[1]);
    const bool final = (first & 0x80U) != 0;
    const auto opcode = static_cast<std::uint8_t>(first & 0x0fU);
    if ((first & 0x70U) != 0) throw std::runtime_error("websocket server used unsupported reserved bits");
    if ((second & 0x80U) != 0) throw std::runtime_error("websocket server frame must not be masked");
    if (!supported_websocket_opcode(opcode)) throw std::runtime_error("websocket server used an unsupported opcode");
    std::uint64_t length = second & 0x7fU;
    std::size_t header_size = 2;
    if (length == 126) {
      if (!impl_->ensure_bytes(4, deadline)) throw std::runtime_error("incomplete websocket frame");
      length = (static_cast<std::uint64_t>(static_cast<unsigned char>(impl_->buffered[2])) << 8U) |
          static_cast<unsigned char>(impl_->buffered[3]);
      if (length < 126U) throw std::runtime_error("websocket frame length is not minimally encoded");
      header_size = 4;
    } else if (length == 127) {
      if (!impl_->ensure_bytes(10, deadline)) throw std::runtime_error("incomplete websocket frame");
      if ((static_cast<unsigned char>(impl_->buffered[2]) & 0x80U) != 0) {
        throw std::runtime_error("invalid websocket frame length");
      }
      length = 0;
      for (std::size_t index = 2; index < 10; ++index) {
        length = (length << 8U) | static_cast<unsigned char>(impl_->buffered[index]);
      }
      if (length <= 0xffffU) throw std::runtime_error("websocket frame length is not minimally encoded");
      header_size = 10;
    }
    const bool control = opcode == 0x8 || opcode == 0x9 || opcode == 0xa;
    if (control && (!final || length > 125)) throw std::runtime_error("invalid websocket control frame");
    if (length > impl_->max_message_bytes) throw std::runtime_error("websocket message too large");
    if (length > std::numeric_limits<std::size_t>::max() - header_size) {
      throw std::runtime_error("websocket frame length is unsupported");
    }
    if (!impl_->ensure_bytes(header_size + static_cast<std::size_t>(length), deadline)) {
      throw std::runtime_error("incomplete websocket frame");
    }
    const auto payload = impl_->buffered.substr(header_size, static_cast<std::size_t>(length));
    impl_->buffered.erase(0, header_size + static_cast<std::size_t>(length));
    if (opcode == 0x8) {
      try {
        validate_websocket_close_payload(payload);
      } catch (const std::invalid_argument& error) {
        throw std::runtime_error(error.what());
      }
      close();
      return {WebSocketReceiveStatus::Closed, Json::object()};
    }
    if (opcode == 0x9) {
      impl_->send_frame(0xa, payload, timeout_);
      continue;
    }
    if (opcode == 0xa) continue;
    if (!final) throw std::runtime_error("fragmented websocket messages are not supported");
    if (opcode != 0x1) throw std::runtime_error("only text websocket frames are supported");
    try {
      auto value = Json::parse(payload);
      if (!value.is_object()) throw std::runtime_error("websocket message must be a JSON object");
      return {WebSocketReceiveStatus::Message, std::move(value)};
    } catch (const Json::exception& error) {
      throw std::runtime_error(std::string("invalid websocket JSON: ") + error.what());
    }
  }
}

}  // namespace mine_teleop
