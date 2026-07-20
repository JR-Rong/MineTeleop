#include "mine_teleop/server.hpp"

#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <openssl/rand.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cerrno>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <stdexcept>
#include <utility>

#include <yaml-cpp/yaml.h>

namespace mine_teleop {
namespace {

class Unauthorized final : public std::runtime_error {
 public:
  using std::runtime_error::runtime_error;
};

class Conflict final : public std::runtime_error {
 public:
  using std::runtime_error::runtime_error;
};

class NotFound final : public std::runtime_error {
 public:
  using std::runtime_error::runtime_error;
};

std::string lower(std::string value) {
  std::transform(value.begin(), value.end(), value.begin(), [](unsigned char value) {
    return static_cast<char>(std::tolower(value));
  });
  return value;
}

std::string trim(std::string value) {
  const auto first = value.find_first_not_of(" \t\r\n");
  if (first == std::string::npos) return {};
  const auto last = value.find_last_not_of(" \t\r\n");
  return value.substr(first, last - first + 1);
}

int hex_digit(char value) {
  if (value >= '0' && value <= '9') return value - '0';
  if (value >= 'a' && value <= 'f') return value - 'a' + 10;
  if (value >= 'A' && value <= 'F') return value - 'A' + 10;
  return -1;
}

std::string url_decode(std::string_view value) {
  std::string result;
  result.reserve(value.size());
  for (std::size_t index = 0; index < value.size(); ++index) {
    if (value[index] == '+') {
      result.push_back(' ');
    } else if (value[index] == '%' && index + 2 < value.size()) {
      const int high = hex_digit(value[index + 1]);
      const int low = hex_digit(value[index + 2]);
      if (high < 0 || low < 0) throw std::invalid_argument("invalid URL encoding");
      result.push_back(static_cast<char>((high << 4) | low));
      index += 2;
    } else {
      result.push_back(value[index]);
    }
  }
  return result;
}

std::vector<std::string> path_parts(std::string_view path) {
  std::vector<std::string> result;
  std::size_t start = 0;
  while (start < path.size()) {
    while (start < path.size() && path[start] == '/') ++start;
    if (start >= path.size()) break;
    const auto end = path.find('/', start);
    result.push_back(url_decode(path.substr(start, end == std::string_view::npos ? path.size() - start : end - start)));
    if (end == std::string_view::npos) break;
    start = end + 1;
  }
  return result;
}

std::string query_value(const HttpRequest& request, std::string_view key) {
  const auto found = request.query.find(std::string(key));
  return found == request.query.end() ? "" : found->second;
}

std::string required_string(const Json& value, std::string_view key) {
  const std::string name(key);
  if (!value.contains(name) || !value.at(name).is_string() || value.at(name).get_ref<const std::string&>().empty()) {
    throw std::invalid_argument(std::string(key) + " must be a non-empty string");
  }
  return value.at(name).get<std::string>();
}

std::string optional_string(const Json& value, std::string_view key) {
  const std::string name(key);
  if (!value.contains(name) || value.at(name).is_null()) return {};
  if (!value.at(name).is_string()) throw std::invalid_argument(std::string(key) + " must be a string");
  return value.at(name).get<std::string>();
}

std::string message_key(std::string_view session_id, std::string_view recipient) {
  return std::string(session_id) + "\x1f" + std::string(recipient);
}

std::string status_reason(int status) {
  switch (status) {
    case 200: return "OK";
    case 201: return "Created";
    case 400: return "Bad Request";
    case 401: return "Unauthorized";
    case 404: return "Not Found";
    case 405: return "Method Not Allowed";
    case 409: return "Conflict";
    case 413: return "Payload Too Large";
    case 500: return "Internal Server Error";
    default: return "Response";
  }
}

void send_all(int socket, std::string_view value) {
  std::size_t sent = 0;
  while (sent < value.size()) {
    const auto result = ::send(socket, value.data() + sent, value.size() - sent, MSG_NOSIGNAL);
    if (result < 0) {
      if (errno == EINTR) continue;
      throw std::runtime_error(std::string("send failed: ") + std::strerror(errno));
    }
    if (result == 0) throw std::runtime_error("connection closed while sending response");
    sent += static_cast<std::size_t>(result);
  }
}

HttpRequest parse_request(int socket, std::size_t max_body_bytes) {
  constexpr std::size_t max_headers = 64 * 1024;
  std::string wire;
  std::array<char, 16 * 1024> buffer{};
  std::size_t header_end = std::string::npos;
  while ((header_end = wire.find("\r\n\r\n")) == std::string::npos) {
    const auto received = ::recv(socket, buffer.data(), buffer.size(), 0);
    if (received < 0) {
      if (errno == EINTR) continue;
      throw std::runtime_error(std::string("recv failed: ") + std::strerror(errno));
    }
    if (received == 0) throw std::invalid_argument("client closed before sending HTTP headers");
    wire.append(buffer.data(), static_cast<std::size_t>(received));
    if (wire.size() > max_headers) throw std::invalid_argument("HTTP headers too large");
  }

  std::istringstream headers(wire.substr(0, header_end));
  HttpRequest request;
  std::string request_line;
  if (!std::getline(headers, request_line)) throw std::invalid_argument("missing HTTP request line");
  request_line = trim(std::move(request_line));
  std::istringstream line(request_line);
  std::string version;
  if (!(line >> request.method >> request.target >> version) || !version.starts_with("HTTP/1.")) {
    throw std::invalid_argument("invalid HTTP request line");
  }
  std::string header;
  while (std::getline(headers, header)) {
    header = trim(std::move(header));
    if (header.empty()) continue;
    const auto separator = header.find(':');
    if (separator == std::string::npos) throw std::invalid_argument("invalid HTTP header");
    request.headers[lower(trim(header.substr(0, separator)))] = trim(header.substr(separator + 1));
  }

  std::size_t content_length = 0;
  if (const auto found = request.headers.find("content-length"); found != request.headers.end()) {
    std::size_t consumed = 0;
    try {
      content_length = std::stoull(found->second, &consumed);
    } catch (const std::exception&) {
      throw std::invalid_argument("invalid Content-Length header");
    }
    if (consumed != found->second.size()) throw std::invalid_argument("invalid Content-Length header");
  }
  if (content_length > max_body_bytes) throw std::length_error("request body too large");
  const auto body_start = header_end + 4;
  while (wire.size() - body_start < content_length) {
    const auto received = ::recv(socket, buffer.data(), buffer.size(), 0);
    if (received < 0) {
      if (errno == EINTR) continue;
      throw std::runtime_error(std::string("recv failed: ") + std::strerror(errno));
    }
    if (received == 0) throw std::invalid_argument("client closed before sending HTTP body");
    wire.append(buffer.data(), static_cast<std::size_t>(received));
  }
  request.body = wire.substr(body_start, content_length);

  const auto question = request.target.find('?');
  request.path = url_decode(request.target.substr(0, question));
  if (question != std::string::npos) {
    const std::string_view query(request.target.data() + question + 1, request.target.size() - question - 1);
    std::size_t start = 0;
    while (start <= query.size()) {
      const auto end = query.find('&', start);
      const auto item = query.substr(start, end == std::string_view::npos ? query.size() - start : end - start);
      if (!item.empty()) {
        const auto equal = item.find('=');
        request.query[url_decode(item.substr(0, equal))] = equal == std::string_view::npos ? "" : url_decode(item.substr(equal + 1));
      }
      if (end == std::string_view::npos) break;
      start = end + 1;
    }
  }
  return request;
}

std::string console_html() {
  return R"HTML(<!doctype html>
<html lang="zh-CN"><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>Mine Teleop WebRTC Console</title><style>
body{background:#0b1220;color:#e5e7eb;font-family:system-ui;margin:0;padding:20px}button{font-size:17px;margin:4px;padding:10px 16px}
.danger{background:#dc2626;color:#fff}.panel{max-width:1400px;margin:auto}.grid{display:grid;grid-template-columns:repeat(2,minmax(0,1fr));gap:10px}
.camera{background:#000;min-height:240px;border-radius:8px;overflow:hidden;position:relative}.camera video{width:100%;height:100%;min-height:240px;object-fit:contain}
.label{position:absolute;left:8px;top:8px;background:#000a;padding:4px 8px;border-radius:4px;z-index:2}pre{background:#030712;padding:12px;overflow:auto}.keys{color:#9ca3af}
@media(max-width:800px){.grid{grid-template-columns:1fr}}</style></head><body><main class="panel">
<h1>Mine Teleop WebRTC 控制台</h1><p class="keys">H.265 优先、H.264 回退；方向键控制，空格制动，E 急停。</p>
<button id="connect">连接</button><button id="estop" class="danger">急停</button><strong id="webrtc">未连接</strong>
<section id="cameras" class="grid"></section><pre id="status">等待连接</pre></main><script>
const state={left:false,right:false,up:false,down:false,brake:false};
const webrtcLabel=document.getElementById('webrtc'),cameraGrid=document.getElementById('cameras'),statusPanel=document.getElementById('status');
let peer=null,pendingIce=[],remoteCameraIds=[],polling=false,mediaStatus={lanes:[]},h265FailureSamples=0,h265FallbackSent=false;const previousStats=new Map(),cameraByMid=new Map();
async function post(path,body={}){const r=await fetch(path,{method:'POST',headers:{'content-type':'application/json'},body:JSON.stringify(body)});const j=await r.json();if(!r.ok)throw Error(j.error||r.status);return j}
async function send(extra={}){return post('/api/control/keyboard',{...state,...extra})}
function advertisedCodecs(){const caps=RTCRtpReceiver.getCapabilities&&RTCRtpReceiver.getCapabilities('video');const found=new Set(['h264']);for(const c of (caps&&caps.codecs)||[]){const m=(c.mimeType||'').toLowerCase();if(m.includes('h265')||m.includes('hevc'))found.add('h265');if(m.includes('h264')||m.includes('avc'))found.add('h264')}return [...found]}
async function connect(){if(polling)return;await post('/api/connect');await post('/api/webrtc/capabilities',{codecs:advertisedCodecs()});polling=true;pollSignaling()}
document.querySelector('#connect').onclick=()=>connect().catch(alert);
document.querySelector('#estop').onclick=()=>send({estop:true}).catch(alert);
const keys={ArrowLeft:'left',ArrowRight:'right',ArrowUp:'up',ArrowDown:'down',' ':'brake'};
addEventListener('keydown',e=>{if(e.key==='e'||e.key==='E'){send({estop:true});return}if(keys[e.key]&&!state[keys[e.key]]){state[keys[e.key]]=true;send();e.preventDefault()}});
addEventListener('keyup',e=>{if(keys[e.key]){state[keys[e.key]]=false;send();e.preventDefault()}});
async function pollSignaling(){while(polling){try{const data=await post('/api/poll-signaling');for(const message of data.messages||[]){if(message.type==='webrtc_offer')await startFromOffer(message.payload||{});if(message.type==='ice_candidate')await addIce(message.payload||{});if(message.type==='media_status')mediaStatus=message.payload||{lanes:[]}}}catch(e){webrtcLabel.textContent='信令错误: '+e.message}await new Promise(r=>setTimeout(r,100))}}
async function addIce(candidate){if(!candidate.candidate)return;if(!peer||!peer.remoteDescription){pendingIce.push(candidate);return}await peer.addIceCandidate(candidate)}
function attach(cameraId,stream){let box=document.getElementById('camera-'+cameraId);if(!box){box=document.createElement('article');box.id='camera-'+cameraId;box.className='camera';box.innerHTML='<span class="label"></span><video autoplay playsinline muted></video>';box.querySelector('.label').textContent=cameraId;cameraGrid.appendChild(box)}box.querySelector('video').srcObject=stream}
async function startFromOffer(offer){if(peer)peer.close();pendingIce=[];cameraByMid.clear();previousStats.clear();h265FailureSamples=0;h265FallbackSent=false;remoteCameraIds=(offer.media_tracks||[]).map(t=>t.camera_id);peer=new RTCPeerConnection({bundlePolicy:'max-bundle'});webrtcLabel.textContent=`协商 ${offer.codec||''}/${offer.backend||''}`;peer.onconnectionstatechange=()=>webrtcLabel.textContent=peer.connectionState;peer.onicecandidate=e=>{if(e.candidate)post('/api/webrtc/ice-candidate',{candidate:e.candidate.toJSON()}).catch(console.error)};peer.ontrack=e=>{const id=remoteCameraIds.shift()||e.transceiver.mid||e.track.id;cameraByMid.set(e.transceiver.mid||'',id);attach(id,e.streams[0]||new MediaStream([e.track]))};await peer.setRemoteDescription({type:'offer',sdp:offer.sdp});while(pendingIce.length)await addIce(pendingIce.shift());const answer=await peer.createAnswer();await peer.setLocalDescription(answer);await post('/api/webrtc/answer',{type:'answer',sdp:peer.localDescription.sdp})}
async function collectMetrics(){if(!peer)return;const report=await peer.getStats();let rtt=0;for(const s of report.values())if(s.type==='candidate-pair'&&s.state==='succeeded'&&s.nominated)rtt=Number(s.currentRoundTripTime||0);const streams=[];for(const s of report.values()){if(s.type!=='inbound-rtp'||(s.kind||s.mediaType)!=='video')continue;const prior=previousStats.get(s.id);let fps=Number(s.framesPerSecond||0);if(!fps&&prior){const seconds=(s.timestamp-prior.timestamp)/1000;if(seconds>0)fps=(Number(s.framesDecoded||0)-prior.framesDecoded)/seconds}previousStats.set(s.id,{timestamp:s.timestamp,framesDecoded:Number(s.framesDecoded||0)});const jitterMs=Number(s.jitterBufferEmittedCount||0)>0?Number(s.jitterBufferDelay||0)*1000/Number(s.jitterBufferEmittedCount):0;const processingMs=Number(s.framesDecoded||0)>0?Number(s.totalProcessingDelay||0)*1000/Number(s.framesDecoded):0;const cameraId=cameraByMid.get(s.mid||'')||'';const lane=(mediaStatus.lanes||[]).find(l=>l.camera_id===cameraId)||{};const captureEncodeMs=Number(lane.capture_to_encoded_ms||0);const latencyMs=captureEncodeMs+rtt*500+jitterMs+processingMs;streams.push({camera_id:cameraId,mid:s.mid||'',codec_id:s.codecId||'',fps,frames_decoded:Number(s.framesDecoded||0),frames_dropped:Number(s.framesDropped||0),packets_lost:Number(s.packetsLost||0),jitter_ms:Number(s.jitter||0)*1000,capture_to_encoded_ms:captureEncodeMs,jitter_buffer_ms:jitterMs,processing_ms:processingMs,round_trip_ms:rtt*1000,estimated_end_to_end_latency_ms:latencyMs,passed:fps>=20&&latencyMs<=200})}const metrics={sampled_at_ms:Date.now(),connection_state:peer.connectionState,codec:mediaStatus.codec||'',backend:mediaStatus.backend||'',latency_method:'capture-to-encoded + rtt/2 + jitter-buffer + browser-processing',streams,passed:streams.length>0&&streams.every(s=>s.passed)};await post('/api/webrtc/metrics',metrics);if(metrics.codec==='h265'&&metrics.connection_state==='connected'&&streams.length){h265FailureSamples=streams.some(s=>s.fps<20)?h265FailureSamples+1:0;if(h265FailureSamples>=3&&!h265FallbackSent){h265FallbackSent=true;await post('/api/webrtc/fallback',{codec:'h264',reason:'h265_decode_fps_below_20'})}}else h265FailureSamples=0;statusPanel.textContent=JSON.stringify(metrics,null,2)}
setInterval(()=>collectMetrics().catch(console.error),1000);
</script></body></html>)HTML";
}

Json keyboard_to_control(const Json& payload) {
  const bool left = payload.value("left", false);
  const bool right = payload.value("right", false);
  const bool up = payload.value("up", false);
  const bool down = payload.value("down", false);
  const bool brake_key = payload.value("brake", false);
  return {
      {"gear", up ? "D" : (down ? "R" : "N")},
      {"steering", left == right ? 0.0 : (left ? -1.0 : 1.0)},
      {"throttle", (up || down) && !brake_key ? 0.35 : 0.0},
      {"brake", brake_key ? 1.0 : 0.0},
      {"estop", payload.value("estop", false)},
  };
}

}  // namespace

Json HttpRequest::json_body() const {
  if (body.empty()) return Json::object();
  try {
    auto value = Json::parse(body);
    if (!value.is_object()) throw std::invalid_argument("JSON body must be an object");
    return value;
  } catch (const Json::exception& error) {
    throw std::invalid_argument(std::string("invalid JSON body: ") + error.what());
  }
}

ServerResponse ServerResponse::json(int status, const Json& value) {
  return ServerResponse{status, "application/json; charset=utf-8", value.dump(), {}};
}

ServerResponse ServerResponse::text(int status, std::string body, std::string content_type) {
  return ServerResponse{status, std::move(content_type), std::move(body), {}};
}

SimpleHttpServer::SimpleHttpServer(
    std::string host, std::uint16_t port, Handler handler, std::size_t max_body_bytes)
    : host_(std::move(host)),
      requested_port_(port),
      handler_(std::move(handler)),
      max_body_bytes_(max_body_bytes) {
  if (host_.empty()) throw std::invalid_argument("HTTP host must not be empty");
  if (!handler_) throw std::invalid_argument("HTTP handler is required");
  if (max_body_bytes_ == 0) throw std::invalid_argument("HTTP max body size must be positive");
}

SimpleHttpServer::~SimpleHttpServer() { stop(); }

void SimpleHttpServer::open_listener() {
  if (listener_fd_ >= 0) return;
  addrinfo hints{};
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;
  hints.ai_flags = AI_PASSIVE;
  addrinfo* addresses = nullptr;
  const auto service = std::to_string(requested_port_);
  const int resolve = ::getaddrinfo(host_.c_str(), service.c_str(), &hints, &addresses);
  if (resolve != 0) throw std::runtime_error(std::string("cannot resolve HTTP bind address: ") + gai_strerror(resolve));
  int saved_errno = 0;
  for (auto* address = addresses; address != nullptr; address = address->ai_next) {
    listener_fd_ = ::socket(address->ai_family, address->ai_socktype, address->ai_protocol);
    if (listener_fd_ < 0) {
      saved_errno = errno;
      continue;
    }
    int reuse = 1;
    ::setsockopt(listener_fd_, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
    if (::bind(listener_fd_, address->ai_addr, address->ai_addrlen) == 0 && ::listen(listener_fd_, 64) == 0) break;
    saved_errno = errno;
    ::close(listener_fd_);
    listener_fd_ = -1;
  }
  ::freeaddrinfo(addresses);
  if (listener_fd_ < 0) throw std::runtime_error(std::string("cannot bind HTTP listener: ") + std::strerror(saved_errno));

  sockaddr_storage bound{};
  socklen_t length = sizeof(bound);
  if (::getsockname(listener_fd_, reinterpret_cast<sockaddr*>(&bound), &length) != 0) {
    throw std::runtime_error(std::string("getsockname failed: ") + std::strerror(errno));
  }
  if (bound.ss_family == AF_INET) bound_port_ = ntohs(reinterpret_cast<sockaddr_in*>(&bound)->sin_port);
  if (bound.ss_family == AF_INET6) bound_port_ = ntohs(reinterpret_cast<sockaddr_in6*>(&bound)->sin6_port);
}

void SimpleHttpServer::serve_client(int client_fd) const {
  ServerResponse response;
  try {
    response = handler_(parse_request(client_fd, max_body_bytes_));
  } catch (const std::length_error& error) {
    response = ServerResponse::json(413, {{"error", error.what()}});
  } catch (const std::invalid_argument& error) {
    response = ServerResponse::json(400, {{"error", error.what()}});
  } catch (const std::exception& error) {
    response = ServerResponse::json(500, {{"error", error.what()}});
  }
  std::ostringstream header;
  header << "HTTP/1.1 " << response.status << ' ' << status_reason(response.status) << "\r\n"
         << "Content-Type: " << response.content_type << "\r\n"
         << "Content-Length: " << response.body.size() << "\r\n"
         << "Connection: close\r\n";
  for (const auto& [name, value] : response.headers) header << name << ": " << value << "\r\n";
  header << "\r\n";
  try {
    send_all(client_fd, header.str());
    send_all(client_fd, response.body);
  } catch (const std::exception&) {
  }
}

void SimpleHttpServer::serve_forever() {
  open_listener();
  stopping_ = false;
  while (!stopping_) {
    const int client = ::accept(listener_fd_, nullptr, nullptr);
    if (client < 0) {
      if (errno == EINTR) continue;
      if (stopping_ || errno == EBADF || errno == EINVAL) break;
      continue;
    }
    std::thread([this, client] {
      serve_client(client);
      ::shutdown(client, SHUT_RDWR);
      ::close(client);
    }).detach();
  }
}

void SimpleHttpServer::start() {
  if (thread_.joinable()) throw std::runtime_error("HTTP server is already running");
  open_listener();
  thread_ = std::thread([this] { serve_forever(); });
}

void SimpleHttpServer::stop() {
  stopping_ = true;
  if (listener_fd_ >= 0) {
    ::shutdown(listener_fd_, SHUT_RDWR);
    ::close(listener_fd_);
    listener_fd_ = -1;
  }
  if (thread_.joinable() && thread_.get_id() != std::this_thread::get_id()) thread_.join();
}

std::string random_token(std::size_t bytes) {
  if (bytes == 0 || bytes > 1024) throw std::invalid_argument("random token byte count is invalid");
  std::vector<unsigned char> value(bytes);
  if (RAND_bytes(value.data(), static_cast<int>(value.size())) != 1) throw std::runtime_error("OpenSSL RAND_bytes failed");
  std::ostringstream output;
  output << std::hex << std::setfill('0');
  for (const auto byte : value) output << std::setw(2) << static_cast<int>(byte);
  return output.str();
}

Json SignalingService::Session::to_json() const {
  return {{"session_id", session_id}, {"vehicle_id", vehicle_id}, {"driver_id", driver_id}, {"state", state}, {"control_token", control_token}};
}

Json SignalingService::Message::to_json() const {
  return {{"session_id", session_id}, {"sender", sender}, {"recipient", recipient}, {"type", type}, {"payload", payload}};
}

SignalingService::SignalingService(SignalingServerConfig config) : config_(std::move(config)) {
  if (config_.token_ttl_ms <= 0) throw std::invalid_argument("driver token TTL must be positive");
  if (config_.driver_passwords.empty()) throw std::invalid_argument("at least one driver credential is required");
  if (config_.device_tokens.empty()) throw std::invalid_argument("at least one device credential is required");
  for (const auto& [id, password] : config_.driver_passwords) {
    if (id.empty() || password.empty()) throw std::invalid_argument("driver credentials must not be empty");
  }
  for (const auto& [id, token] : config_.device_tokens) {
    if (id.empty() || token.empty()) throw std::invalid_argument("device credentials must not be empty");
  }
}

Json SignalingService::health() const {
  std::lock_guard lock(mutex_);
  return {{"status", "ok"}, {"runtime", "cpp"}, {"online_vehicles", online_vehicles_.size()}, {"sessions", sessions_.size()}};
}

const SignalingService::Session& SignalingService::require_active_session(std::string_view session_id) const {
  const auto found = sessions_.find(std::string(session_id));
  if (found == sessions_.end()) throw NotFound("unknown session");
  if (found->second.state != "SESSION_ACTIVE") throw Conflict("session is not active");
  return found->second;
}

const SignalingService::Session& SignalingService::require_participant(
    std::string_view session_id, std::string_view participant) const {
  const auto& session = require_active_session(session_id);
  if (participant != session.driver_id && participant != session.vehicle_id) {
    throw Unauthorized("actor is not current session participant");
  }
  return session;
}

void SignalingService::validate_driver_token(std::string_view driver_id, std::string_view token) const {
  const auto found = driver_tokens_.find(std::string(token));
  if (token.empty() || found == driver_tokens_.end() || found->second.driver_id != driver_id) {
    throw Unauthorized("invalid driver token");
  }
  if (now_ms() >= found->second.expires_at_ms) throw Unauthorized("driver token expired");
}

void SignalingService::validate_device_token(std::string_view vehicle_id, std::string_view token) const {
  const auto found = config_.device_tokens.find(std::string(vehicle_id));
  if (token.empty() || found == config_.device_tokens.end() || found->second != token) {
    throw Unauthorized("invalid device token");
  }
}

void SignalingService::validate_actor_credential(const Session& session, std::string_view actor, const Json& value) const {
  if (actor == session.driver_id) {
    validate_driver_token(actor, optional_string(value, "token"));
  } else if (actor == session.vehicle_id) {
    validate_device_token(actor, optional_string(value, "device_token"));
  } else {
    throw Unauthorized("actor is not current session participant");
  }
}

void SignalingService::audit(std::string_view event, const Json& details) const {
  if (config_.audit_log_path.empty()) return;
  std::ofstream output(config_.audit_log_path, std::ios::app);
  if (!output) throw std::runtime_error("cannot append signaling audit log");
  output << Json({{"event", event}, {"ts_ms", now_ms()}, {"details", details}}).dump() << '\n';
}

ServerResponse SignalingService::handle(const HttpRequest& request) {
  try {
    if (request.method == "GET") return handle_get(request);
    if (request.method == "POST") return handle_post(request);
    return ServerResponse::json(405, {{"error", "method not allowed"}});
  } catch (const Unauthorized& error) {
    return ServerResponse::json(401, {{"error", error.what()}});
  } catch (const NotFound& error) {
    return ServerResponse::json(404, {{"error", error.what()}});
  } catch (const Conflict& error) {
    return ServerResponse::json(409, {{"error", error.what()}});
  } catch (const std::invalid_argument& error) {
    return ServerResponse::json(400, {{"error", error.what()}});
  } catch (const Json::exception& error) {
    return ServerResponse::json(400, {{"error", error.what()}});
  }
}

ServerResponse SignalingService::handle_get(const HttpRequest& request) {
  if (request.path == "/health") return ServerResponse::json(200, health());
  const auto parts = path_parts(request.path);
  std::lock_guard lock(mutex_);
  if (parts.size() == 3 && parts[0] == "signaling" && parts[2] == "messages") {
    const auto recipient = query_value(request, "recipient");
    if (recipient.empty()) throw std::invalid_argument("recipient is required");
    const auto& session = require_participant(parts[1], recipient);
    if (recipient == session.driver_id) {
      validate_driver_token(recipient, query_value(request, "token"));
    } else {
      validate_device_token(recipient, query_value(request, "device_token"));
    }
    Json values = Json::array();
    auto found = messages_.find(message_key(parts[1], recipient));
    if (found != messages_.end()) {
      const auto requested = query_value(request, "types");
      if (requested.empty()) {
        for (const auto& message : found->second) values.push_back(message.to_json());
        messages_.erase(found);
      } else {
        std::vector<std::string> types;
        std::size_t start = 0;
        while (start <= requested.size()) {
          const auto end = requested.find(',', start);
          const auto value = trim(requested.substr(start, end == std::string::npos ? requested.size() - start : end - start));
          if (!value.empty()) types.push_back(value);
          if (end == std::string::npos) break;
          start = end + 1;
        }
        std::vector<Message> remaining;
        for (const auto& message : found->second) {
          if (std::find(types.begin(), types.end(), message.type) != types.end()) {
            values.push_back(message.to_json());
          } else {
            remaining.push_back(message);
          }
        }
        if (remaining.empty()) {
          messages_.erase(found);
        } else {
          found->second = std::move(remaining);
        }
      }
    }
    return ServerResponse::json(200, {{"messages", std::move(values)}});
  }
  if (parts.size() == 3 && parts[0] == "vehicles" && parts[2] == "session") {
    const auto& vehicle_id = parts[1];
    validate_device_token(vehicle_id, query_value(request, "device_token"));
    for (const auto& [id, session] : sessions_) {
      static_cast<void>(id);
      if (session.vehicle_id == vehicle_id && session.state == "SESSION_ACTIVE") {
        return ServerResponse::json(200, {{"vehicle_id", vehicle_id}, {"session_id", session.session_id}, {"driver_id", session.driver_id}, {"state", session.state}});
      }
    }
    return ServerResponse::json(200, {{"vehicle_id", vehicle_id}, {"session_id", ""}, {"state", "none"}});
  }
  if (parts.size() == 3 && parts[0] == "sessions" && parts[2] == "ice_servers") {
    const auto actor = query_value(request, "actor");
    const auto& session = require_participant(parts[1], actor);
    Json credentials = {{"token", query_value(request, "token")}, {"device_token", query_value(request, "device_token")}};
    validate_actor_credential(session, actor, credentials);
    return ServerResponse::json(200, {{"session_id", parts[1]}, {"ice_servers", Json::array()}});
  }
  return ServerResponse::json(404, {{"error", "not found"}});
}

ServerResponse SignalingService::handle_post(const HttpRequest& request) {
  const auto value = request.json_body();
  const auto parts = path_parts(request.path);
  std::lock_guard lock(mutex_);
  if (request.path == "/auth/driver_login") {
    const auto driver_id = required_string(value, "driver_id");
    const auto password = optional_string(value, "password");
    const auto found = config_.driver_passwords.find(driver_id);
    if (found == config_.driver_passwords.end() || found->second != password) throw Unauthorized("invalid driver credentials");
    const std::string token = "driver-token-" + random_token();
    driver_tokens_[token] = DriverToken{driver_id, now_ms() + config_.token_ttl_ms};
    audit("driver_login", {{"driver_id", driver_id}});
    return ServerResponse::json(200, {{"token_type", "bearer"}, {"token", token}, {"expires_at_ms", driver_tokens_.at(token).expires_at_ms}});
  }
  if (request.path == "/vehicles/online") {
    const auto vehicle_id = required_string(value, "vehicle_id");
    validate_device_token(vehicle_id, optional_string(value, "device_token"));
    online_vehicles_[vehicle_id] = true;
    audit("vehicle_online", {{"vehicle_id", vehicle_id}});
    return ServerResponse::json(200, {{"vehicle_id", vehicle_id}, {"state", "online"}});
  }
  if (request.path == "/vehicles/offline") {
    const auto vehicle_id = required_string(value, "vehicle_id");
    validate_device_token(vehicle_id, optional_string(value, "device_token"));
    online_vehicles_.erase(vehicle_id);
    for (auto& [id, session] : sessions_) {
      static_cast<void>(id);
      if (session.vehicle_id == vehicle_id && session.state == "SESSION_ACTIVE") session.state = "FAILED";
    }
    audit("vehicle_offline", {{"vehicle_id", vehicle_id}, {"reason", optional_string(value, "reason")}});
    return ServerResponse::json(200, {{"vehicle_id", vehicle_id}, {"state", "offline"}});
  }
  if (request.path == "/sessions") {
    const auto driver_id = required_string(value, "driver_id");
    const auto vehicle_id = required_string(value, "vehicle_id");
    validate_driver_token(driver_id, optional_string(value, "token"));
    if (!online_vehicles_.contains(vehicle_id)) throw Conflict("vehicle is not online");
    for (const auto& [id, session] : sessions_) {
      static_cast<void>(id);
      if (session.vehicle_id == vehicle_id && session.state == "SESSION_ACTIVE") throw Conflict("control authority already granted");
    }
    ++session_counter_;
    std::ostringstream id;
    id << "session-" << std::setw(6) << std::setfill('0') << session_counter_;
    Session session{id.str(), vehicle_id, driver_id, "SESSION_ACTIVE", "control-token-" + random_token()};
    sessions_[session.session_id] = session;
    audit("session_started", session.to_json());
    return ServerResponse::json(200, session.to_json());
  }
  if (parts.size() == 3 && parts[0] == "sessions" && parts[2] == "end") {
    const auto actor = required_string(value, "actor");
    auto& session = const_cast<Session&>(require_participant(parts[1], actor));
    validate_actor_credential(session, actor, value);
    session.state = "ENDED";
    audit("session_ended", session.to_json());
    return ServerResponse::json(200, session.to_json());
  }
  if (parts.size() == 4 && parts[0] == "sessions" && parts[2] == "control_authority" && parts[3] == "revoke") {
    const auto actor = required_string(value, "actor");
    auto& session = const_cast<Session&>(require_participant(parts[1], actor));
    validate_actor_credential(session, actor, value);
    session.state = "ENDED";
    audit("control_authority_revoked", {{"session_id", session.session_id}, {"reason", optional_string(value, "reason")}});
    return ServerResponse::json(200, session.to_json());
  }
  if (parts.size() == 3 && parts[0] == "sessions" &&
      (parts[2] == "abnormal_disconnect" || parts[2] == "diagnostics" || parts[2] == "control_timeout" ||
       parts[2] == "estop" || parts[2] == "turn_relay" || parts[2] == "turn_usage")) {
    const auto actor = required_string(value, "actor");
    const auto& session = require_participant(parts[1], actor);
    validate_actor_credential(session, actor, value);
    audit(parts[2], value);
    return ServerResponse::json(200, {{"event", parts[2]}, {"session_id", parts[1]}});
  }
  if (parts.size() == 3 && parts[0] == "signaling" && parts[2] == "messages") {
    const auto sender = required_string(value, "sender");
    const auto recipient = required_string(value, "recipient");
    const auto type = required_string(value, "type");
    static const std::vector<std::string> allowed{
        "webrtc_offer", "webrtc_answer", "ice_candidate", "media_capabilities", "media_fallback",
        "control_command", "telemetry", "media_status", "session_event"};
    if (std::find(allowed.begin(), allowed.end(), type) == allowed.end()) throw std::invalid_argument("unsupported signaling message type");
    const auto& session = require_participant(parts[1], sender);
    validate_actor_credential(session, sender, value);
    if (recipient != session.driver_id && recipient != session.vehicle_id) throw Unauthorized("recipient is not current session participant");
    const bool driver_to_vehicle = sender == session.driver_id && recipient == session.vehicle_id;
    const bool vehicle_to_driver = sender == session.vehicle_id && recipient == session.driver_id;
    if ((type == "media_capabilities" || type == "media_fallback" || type == "webrtc_answer") && !driver_to_vehicle) {
      throw Unauthorized(type + " route is invalid");
    }
    if (type == "webrtc_offer" && !vehicle_to_driver) throw Unauthorized("webrtc_offer route is invalid");
    if (type == "ice_candidate" && !driver_to_vehicle && !vehicle_to_driver) {
      throw Unauthorized("ice_candidate route is invalid");
    }
    const auto payload = value.value("payload", Json::object());
    if (!payload.is_object()) throw std::invalid_argument("payload must be an object");
    if (type == "control_command") {
      if (sender != session.driver_id || recipient != session.vehicle_id) throw Unauthorized("control_command route is invalid");
      const auto command = ControlCommand::from_json(payload);
      if (command.vehicle_id != session.vehicle_id || command.session_id != session.session_id) {
        throw Unauthorized("control_command does not match current session");
      }
      if (command.authority_token != session.control_token) throw Unauthorized("control authority token is invalid");
    }
    auto& queue = messages_[message_key(parts[1], recipient)];
    queue.push_back(Message{parts[1], sender, recipient, type, payload});
    audit(type, {{"session_id", parts[1]}, {"sender", sender}, {"recipient", recipient}});
    return ServerResponse::json(200, {{"queued", queue.size()}});
  }
  return ServerResponse::json(404, {{"error", "not found"}});
}

DriverConfig load_driver_config(const std::string& path) {
  const auto root = YAML::LoadFile(path);
  DriverConfig config;
  if (!root["driver"] || !root["driver"]["id"]) throw std::invalid_argument("driver.id is required");
  if (!root["cloud"] || !root["cloud"]["signaling_url"]) throw std::invalid_argument("cloud.signaling_url is required");
  config.driver_id = root["driver"]["id"].as<std::string>();
  config.signaling_url = root["cloud"]["signaling_url"].as<std::string>();
  if (root["control"] && root["control"]["rate_hz"]) config.rate_hz = root["control"]["rate_hz"].as<int>();
  if (root["control"] && root["control"]["estop_hold_ms"]) config.estop_hold_ms = root["control"]["estop_hold_ms"].as<int>();
  if (config.driver_id.empty() || config.signaling_url.empty() || config.rate_hz <= 0 || config.estop_hold_ms < 0) {
    throw std::invalid_argument("driver configuration is invalid");
  }
  return config;
}

DriverConsoleRuntime::DriverConsoleRuntime(DriverConfig config, std::string vehicle_id, std::string password)
    : config_(std::move(config)),
      vehicle_id_(std::move(vehicle_id)),
      password_(std::move(password)),
      signaling_http_url_(normalize_signaling_http_url(config_.signaling_url)) {
  if (vehicle_id_.empty() || password_.empty()) throw std::invalid_argument("vehicle id and driver password are required");
}

Json DriverConsoleRuntime::connect() {
  {
    std::lock_guard lock(mutex_);
    if (!session_id_.empty()) {
      return {
          {"runtime", "cpp"},
          {"driver_id", config_.driver_id},
          {"vehicle_id", vehicle_id_},
          {"connected", true},
          {"session_id", session_id_},
          {"connected_at_ms", connected_at_ms_},
      };
    }
  }
  const auto login = http_.post_json_response(
      signaling_http_url_ + "/auth/driver_login", {{"driver_id", config_.driver_id}, {"password", password_}});
  const auto token = required_string(login, "token");
  const auto session = http_.post_json_response(
      signaling_http_url_ + "/sessions", {{"driver_id", config_.driver_id}, {"vehicle_id", vehicle_id_}, {"token", token}});
  std::lock_guard lock(mutex_);
  driver_token_ = token;
  session_id_ = required_string(session, "session_id");
  control_token_ = required_string(session, "control_token");
  sequence_ = 0;
  connected_at_ms_ = now_ms();
  return {
      {"runtime", "cpp"},
      {"driver_id", config_.driver_id},
      {"vehicle_id", vehicle_id_},
      {"connected", true},
      {"session_id", session_id_},
      {"connected_at_ms", connected_at_ms_},
  };
}

Json DriverConsoleRuntime::poll_signaling() {
  std::string token;
  std::string session;
  {
    std::lock_guard lock(mutex_);
    token = driver_token_;
    session = session_id_;
  }
  if (token.empty() || session.empty()) throw std::runtime_error("driver console is not connected");
  const auto response = http_.get_json(
      signaling_http_url_ + "/signaling/" + http_.url_encode(session) + "/messages?recipient=" +
      http_.url_encode(config_.driver_id) + "&token=" + http_.url_encode(token) +
      "&types=webrtc_offer,ice_candidate,media_status");
  std::lock_guard lock(mutex_);
  signaling_messages_ = response.value("messages", Json::array());
  return {{"session_id", session_id_}, {"messages", signaling_messages_}};
}

Json DriverConsoleRuntime::send_signaling_message(std::string_view type, const Json& payload) {
  std::string token;
  std::string session;
  {
    std::lock_guard lock(mutex_);
    if (session_id_.empty() || driver_token_.empty()) throw std::runtime_error("driver console is not connected");
    token = driver_token_;
    session = session_id_;
  }
  const auto response = http_.post_json_response(
      signaling_http_url_ + "/signaling/" + http_.url_encode(session) + "/messages",
      {{"sender", config_.driver_id},
       {"recipient", vehicle_id_},
       {"token", token},
       {"type", type},
       {"payload", payload}});
  return {{"queued", response.value("queued", 0)}, {"type", type}, {"session_id", session}};
}

Json DriverConsoleRuntime::send_media_capabilities(const Json& input) {
  if (!input.is_object() || !input.contains("codecs") || !input.at("codecs").is_array()) {
    throw std::invalid_argument("media capabilities must contain a codecs array");
  }
  Json codecs = Json::array();
  for (const auto& value : input.at("codecs")) {
    if (!value.is_string()) throw std::invalid_argument("media codec capability must be a string");
    auto codec = lower(value.get<std::string>());
    if (codec == "h265" || codec == "hevc" || codec == "h264" || codec == "avc") codecs.push_back(codec);
  }
  if (codecs.empty()) codecs.push_back("h264");
  return send_signaling_message("media_capabilities", {{"codecs", std::move(codecs)}});
}

Json DriverConsoleRuntime::send_media_fallback(const Json& input) {
  if (!input.is_object() || lower(input.value("codec", "")) != "h264") {
    throw std::invalid_argument("media fallback must request H.264");
  }
  return send_signaling_message(
      "media_fallback", {{"codec", "h264"}, {"reason", input.value("reason", "browser_decode_failure")}});
}

Json DriverConsoleRuntime::send_webrtc_answer(const Json& input) {
  if (!input.is_object() || input.value("type", "") != "answer" || input.value("sdp", "").empty()) {
    throw std::invalid_argument("WebRTC answer must contain type=answer and SDP");
  }
  return send_signaling_message("webrtc_answer", {{"type", "answer"}, {"sdp", input.at("sdp")}});
}

Json DriverConsoleRuntime::send_webrtc_ice_candidate(const Json& input) {
  const auto candidate = input.contains("candidate") && input.at("candidate").is_object() ? input.at("candidate") : input;
  if (!candidate.is_object() || candidate.value("candidate", "").empty()) {
    throw std::invalid_argument("WebRTC ICE candidate is required");
  }
  return send_signaling_message("ice_candidate", candidate);
}

Json DriverConsoleRuntime::ingest_webrtc_metrics(const Json& input) {
  if (!input.is_object()) throw std::invalid_argument("WebRTC metrics must be an object");
  std::lock_guard lock(mutex_);
  webrtc_metrics_ = input;
  webrtc_metrics_["received_at_ms"] = now_ms();
  return {{"accepted", true}, {"received_at_ms", webrtc_metrics_.at("received_at_ms")}};
}

Json DriverConsoleRuntime::send_control(const Json& input) {
  std::string token;
  std::string session;
  std::string control_token;
  std::uint64_t sequence = 0;
  {
    std::lock_guard lock(mutex_);
    if (session_id_.empty() || driver_token_.empty() || control_token_.empty()) throw std::runtime_error("driver console is not connected");
    token = driver_token_;
    session = session_id_;
    control_token = control_token_;
    sequence = ++sequence_;
  }
  ControlCommand command;
  command.vehicle_id = vehicle_id_;
  command.session_id = session;
  command.seq = sequence;
  command.ts_ms = now_ms();
  command.gear = input.value("gear", "N");
  command.steering = input.value("steering", 0.0);
  command.throttle = input.value("throttle", 0.0);
  command.brake = input.value("brake", 0.0);
  command.estop = input.value("estop", false);
  command.authority_token = control_token;
  command.validate();
  const auto response = http_.post_json_response(
      signaling_http_url_ + "/signaling/" + http_.url_encode(session) + "/messages",
      {{"sender", config_.driver_id},
       {"recipient", vehicle_id_},
       {"token", token},
       {"type", "control_command"},
       {"payload", command.to_json()}});
  {
    std::lock_guard lock(mutex_);
    last_control_sent_ms_ = command.ts_ms;
  }
  return {{"queued", response.value("queued", 0)}, {"command", command.to_json()}};
}

Json DriverConsoleRuntime::status() const {
  std::lock_guard lock(mutex_);
  return {
      {"runtime", "cpp"},
      {"driver_id", config_.driver_id},
      {"vehicle_id", vehicle_id_},
      {"connected", !session_id_.empty()},
      {"session_id", session_id_},
      {"sequence", sequence_},
      {"connected_at_ms", connected_at_ms_},
      {"last_control_sent_ms", last_control_sent_ms_},
      {"webrtc_metrics", webrtc_metrics_},
      {"last_signaling_messages", signaling_messages_},
  };
}

DriverConsoleHttpApp::DriverConsoleHttpApp(std::shared_ptr<DriverConsoleRuntime> runtime) : runtime_(std::move(runtime)) {
  if (!runtime_) throw std::invalid_argument("driver console runtime is required");
}

ServerResponse DriverConsoleHttpApp::handle(const HttpRequest& request) const {
  try {
    if (request.method == "GET" && request.path == "/health") return ServerResponse::json(200, {{"status", "ok"}, {"runtime", "cpp"}});
    if (request.method == "GET" && request.path == "/api/time") return ServerResponse::json(200, {{"now_ms", now_ms()}});
    if (request.method == "GET" && request.path == "/api/status") return ServerResponse::json(200, runtime_->status());
    if (request.method == "GET" && request.path == "/") return ServerResponse::text(200, console_html(), "text/html; charset=utf-8");
    if (request.method == "POST" && request.path == "/api/connect") return ServerResponse::json(200, runtime_->connect());
    if (request.method == "POST" && request.path == "/api/poll-signaling") return ServerResponse::json(200, runtime_->poll_signaling());
    if (request.method == "POST" && request.path == "/api/webrtc/capabilities") return ServerResponse::json(200, runtime_->send_media_capabilities(request.json_body()));
    if (request.method == "POST" && request.path == "/api/webrtc/fallback") return ServerResponse::json(200, runtime_->send_media_fallback(request.json_body()));
    if (request.method == "POST" && request.path == "/api/webrtc/answer") return ServerResponse::json(200, runtime_->send_webrtc_answer(request.json_body()));
    if (request.method == "POST" && request.path == "/api/webrtc/ice-candidate") return ServerResponse::json(200, runtime_->send_webrtc_ice_candidate(request.json_body()));
    if (request.method == "POST" && request.path == "/api/webrtc/metrics") return ServerResponse::json(200, runtime_->ingest_webrtc_metrics(request.json_body()));
    if (request.method == "POST" && request.path == "/api/control") return ServerResponse::json(200, runtime_->send_control(request.json_body()));
    if (request.method == "POST" && request.path == "/api/control/keyboard") {
      return ServerResponse::json(200, runtime_->send_control(keyboard_to_control(request.json_body())));
    }
    if (request.method == "POST" && request.path == "/api/control/gamepad") {
      const auto input = request.json_body();
      Json control = {
          {"gear", input.value("gear", "D")},
          {"steering", input.value("steering", 0.0)},
          {"throttle", input.value("throttle", 0.0)},
          {"brake", input.value("brake", 0.0)},
          {"estop", input.value("estop", false)},
      };
      return ServerResponse::json(200, runtime_->send_control(control));
    }
    return ServerResponse::json(404, {{"error", "not found"}});
  } catch (const std::invalid_argument& error) {
    return ServerResponse::json(400, {{"error", error.what()}});
  } catch (const std::exception& error) {
    return ServerResponse::json(409, {{"error", error.what()}});
  }
}

}  // namespace mine_teleop
