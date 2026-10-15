#pragma once

#include "mine_teleop/detail/control_transport.hpp"
#include "mine_teleop/detail/native_control_acknowledgement_window.hpp"

#include <array>
#include <deque>

namespace mine_teleop::detail {
inline constexpr std::string_view kFeedbackTransportCapability = "native-feedback-transport-v1";
inline std::uint64_t feedback_transport_epoch(const Json& value) {
  return value.contains("feedback_transport") && value.at("feedback_transport").is_object() ?
      ControlTransportStamp::uint(value.at("feedback_transport"), "epoch") : std::uint64_t{0};
}

struct FeedbackTransportStamp {
  std::uint64_t epoch{}, from_epoch{}, authority_epoch{}, floor{};
  ControlPath path{ControlPath::Wss};
  std::string attempt, issuer;
  static FeedbackTransportStamp parse(const Json& value) {
    if (!value.is_object() || value.dump().size() > 1024 || value.value("protocol", "") != kFeedbackTransportCapability)
      throw std::invalid_argument("invalid feedback transport");
    FeedbackTransportStamp result;
    result.epoch = ControlTransportStamp::uint(value, "epoch");
    result.from_epoch = ControlTransportStamp::uint(value, "from_epoch");
    result.authority_epoch = ControlTransportStamp::uint(value, "authority_epoch");
    result.issuer = value.at("authority_issuer").get<std::string>();
    result.attempt = value.at("media_attempt").get<std::string>();
    const auto path = value.at("path").get<std::string>();
    if (!result.epoch || !result.authority_epoch || result.issuer.empty() || result.issuer.size() > 128 ||
        result.attempt.empty() || result.attempt.size() > 128 || (path != "dc" && path != "wss"))
      throw std::invalid_argument("invalid feedback identity");
    result.path = path == "dc" ? ControlPath::DataChannel : ControlPath::Wss;
    if (value.contains("status_floor")) result.floor = ControlTransportStamp::uint(value, "status_floor");
    return result;
  }
  Json json() const {
    return {{"protocol", kFeedbackTransportCapability}, {"epoch", epoch}, {"from_epoch", from_epoch},
        {"authority_epoch", authority_epoch}, {"authority_issuer", issuer}, {"media_attempt", attempt},
        {"path", control_path_name(path)}, {"status_floor", floor}};
  }
  bool same(const FeedbackTransportStamp& other) const {
    return epoch == other.epoch && from_epoch == other.from_epoch && authority_epoch == other.authority_epoch &&
        path == other.path && attempt == other.attempt && issuer == other.issuer;
  }
};

// Called only after MAC/session/ordinary-command admission, under the common
// vehicle mutex. It cannot admit commands, renew authority or refresh a watchdog.
class FeedbackTransportFence {
 public:
  void reset() { *this = {}; }
  bool select(const Json& request, const Json& authority, std::string_view attempt, std::uint64_t status_seq) {
    try {
      const auto proposed = FeedbackTransportStamp::parse(request);
      if (!authority.is_object() || proposed.issuer != authority.at("issuer").get<std::string>() || proposed.authority_epoch != ControlTransportStamp::uint(authority, "authority_epoch") ||
          proposed.attempt != attempt || !request.at("select").is_boolean() || !request.at("probe").is_boolean()) return false;
      if (stamp_.epoch && stamp_.same(proposed)) return true;
      if (!request.at("select").get<bool>() || stamp_.epoch == std::numeric_limits<std::uint64_t>::max() ||
          proposed.from_epoch != stamp_.epoch || proposed.epoch != stamp_.epoch + 1) return false;
      stamp_ = proposed; stamp_.floor = status_seq; return true;
    } catch (const std::exception&) { return false; }
  }
  bool negotiated() const { return stamp_.epoch != 0; }
  ControlPath path() const { return stamp_.path; }
  Json state() const { return negotiated() ? stamp_.json() : Json(nullptr); }
 private:
  FeedbackTransportStamp stamp_;
};

// Owns only the feedback axis. Serialized by the existing native transport
// mutex. One selected status route and at most one paired result probe; no
// duplicated ordinary commands or extra network/worker architecture.
class FeedbackTransportSender {
 public:
  struct Receipt { bool progress{}, boundary{}; };
  void reset() { *this = {}; }
  bool active() const { return stamp_.epoch != 0; }
  bool matches_event(const Json& value) const {
    try { const auto seq = ControlTransportStamp::uint(value, "command_seq");
      for (const auto& item : sent_) if (item.seq == seq) return item.intent == ControlTransportStamp::uint(value, "intent_seq") &&
          item.request == value.at("feedback_request") && item.command_stamp == value.value("control_transport", Json(nullptr)) &&
          value.value("command_received_path", "") == control_path_name(item.forward) && FeedbackTransportStamp::parse(item.request).same(stamp_);
    } catch (const std::exception&) {}
    return false;
  }
  void observe(bool ready, bool p2p, std::string_view attempt, const Json& authority,
      ControlPath forward, std::uint64_t forward_epoch, std::int64_t now, int retry_ms) {
    if (attempt.empty() || !authority.is_object()) return;
    const auto issuer = authority.at("issuer").get<std::string>();
    const auto epoch = ControlTransportStamp::uint(authority, "authority_epoch");
    const auto context = Json::array({issuer, epoch, attempt, control_path_name(forward), forward_epoch}).dump();
    if (context_ != context) {
      context_ = context; sent_.clear(); dc_ = {}; wss_ = {}; paired_dc_ = {}; paired_wss_ = {}; probe_seq_ = 0; selected_at_ = now;
    }
    if (!stamp_.epoch || stamp_.attempt != attempt || stamp_.issuer != issuer || stamp_.authority_epoch != epoch) {
      begin(ControlPath::Wss, std::string(attempt), issuer, epoch, now); next_trial_ = now;
    }
    if (probe_seq_ && now - probe_started_ > NativeControlAckRecovery::kFreshAckMs) probe_seq_ = 0;
    const bool dc_bad = dc_.anomalies >= NativeControlAckRecovery::kConsecutiveAnomalies ||
        now - std::max(dc_.progress, selected_at_) > NativeControlAckRecovery::kStallMs;
    if (stamp_.path == ControlPath::DataChannel && (!ready || dc_bad ||
        (!p2p && paired_dc_.mean() && paired_wss_.mean() && *paired_wss_.mean() < *paired_dc_.mean()))) {
      begin(ControlPath::Wss, std::string(attempt), issuer, epoch, now); next_trial_ = now + retry_ms;
    } else if (stamp_.path == ControlPath::Wss && confirmed_ && ready && now >= next_trial_ && dc_.healthy(now) &&
        (p2p || !wss_.healthy(now) || (paired_dc_.mean() && paired_wss_.mean() && *paired_dc_.mean() < *paired_wss_.mean()))) {
      begin(ControlPath::DataChannel, std::string(attempt), issuer, epoch, now);
    }
  }
  void decorate(Json& payload) const {
    if (!stamp_.epoch) return;
    auto request = stamp_.json(); request.erase("status_floor");
    request["select"] = !confirmed_; request["probe"] = probe_seq_ == 0;
    payload["feedback_request"] = std::move(request);
  }
  void sent(const Json& payload, ControlPath forward, std::int64_t now) {
    if (!payload.contains("feedback_request")) return;
    if (sent_.size() == 128) sent_.pop_front();
    sent_.push_back({payload.at("seq").get<std::uint64_t>(), payload.at("intent_seq").get<std::uint64_t>(),
        now, forward, payload.at("feedback_request"), payload.value("control_transport", Json(nullptr)), {}, {}, {}, false, false});
    if (payload.at("feedback_request").at("probe").get<bool>()) { probe_seq_ = sent_.back().seq; probe_started_ = now; }
  }
  Receipt result(const Json& value, ControlPath returned, std::int64_t now) {
    if (!value.contains("feedback_request") || !value.contains("feedback_transport")) return {};
    const auto seq = ControlTransportStamp::uint(value, "command_seq");
    auto found = std::find_if(sent_.begin(), sent_.end(), [&](const auto& item) { return item.seq == seq; });
    if (found == sent_.end() || found->intent != ControlTransportStamp::uint(value, "intent_seq") || found->request != value.at("feedback_request") ||
        found->command_stamp != value.value("control_transport", Json(nullptr)) || now < found->started ||
        value.value("command_received_path", "") != control_path_name(found->forward)) return {};
    const auto requested = FeedbackTransportStamp::parse(found->request);
    if (!requested.same(stamp_)) return {}; // Before any health, authority or result watermark mutation.
    if (!value.at("feedback_transport").is_object()) return {};
    const auto actual = FeedbackTransportStamp::parse(value.at("feedback_transport"));
    if (!actual.same(stamp_)) {
      if (!confirmed_ && found->request.at("select").get<bool>() && seq > reconciled_seq_) {
        reconciled_seq_ = seq; stamp_.epoch = actual.epoch;
        begin(ControlPath::Wss, stamp_.attempt, stamp_.issuer, stamp_.authority_epoch, now);
      }
      return {};
    }
    bool boundary = false;
    if (!confirmed_) { confirmed_ = true; stamp_.floor = actual.floor; ++revision_; boundary = true; }
    if (actual.floor != stamp_.floor) return {};
    auto& arrival = returned == ControlPath::DataChannel ? found->dc : found->wss;
    if (arrival) return {}; // Duplicate cannot refresh health or progress.
    const auto digest = value.at("vehicle_mac").get<std::string>();
    if (!found->digest.empty() && found->digest != digest) return {};
    found->digest = digest;
    const auto age = now - found->started; arrival = age;
    auto& health = returned == ControlPath::DataChannel ? dc_ : wss_;
    health.note(seq, age, now, value.at("accepted").get<bool>());
    if (found->request.at("probe").get<bool>() && found->dc && found->wss && !found->paired) {
      found->paired = true;
      if (*found->dc <= NativeControlAckRecovery::kFreshAckMs && *found->wss <= NativeControlAckRecovery::kFreshAckMs) {
        paired_dc_.add(*found->dc); paired_wss_.add(*found->wss);
      }
      if (probe_seq_ == seq) probe_seq_ = 0;
    }
    const bool progress = !found->delivered && seq > delivered_seq_;
    found->delivered = true;
    if (progress) delivered_seq_ = seq;
    return {progress, boundary};
  }
  bool status(const Json& wrapper, ControlPath returned) {
    try {
      if (!wrapper.contains("feedback_transport")) return false;
      const auto stamp = FeedbackTransportStamp::parse(wrapper.at("feedback_transport"));
      const auto seq = ControlTransportStamp::uint(wrapper.at("status"), "control_status_seq");
      if (!stamp.same(stamp_) || returned != stamp_.path || seq <= stamp.floor || seq <= status_seq_) return false;
      if (!confirmed_) {
        // Caller has already authenticated identity and MAC. Retain only four
        // current-candidate kinds; neither UI watermark nor health advances.
        const auto kind = wrapper.at("status").value("event", "");
        if (kind != "vehicle_telemetry" && kind != "vcu_handshake_status" && kind != "session_control_profile_status" && kind != "control_command_rejected") return false;
        if (!candidate_status_.contains(kind) || seq > ControlTransportStamp::uint(candidate_status_.at(kind).at("status"), "control_status_seq"))
          candidate_status_[kind] = wrapper;
        return false;
      }
      if (stamp.floor != stamp_.floor) return false;
      status_seq_ = seq; return true;
    } catch (const std::exception&) { return false; }
  }
  // Drain once only after exact confirmation. The caller re-authenticates and
  // passes these back through status(), so floor/scope/order are checked again.
  Json take_pending_statuses() {
    Json result = Json::array();
    if (!confirmed_) return result;
    for (const auto& wrapper : candidate_status_) {
      const auto stamp = FeedbackTransportStamp::parse(wrapper.at("feedback_transport"));
      if (stamp.same(stamp_) && stamp.floor == stamp_.floor &&
          ControlTransportStamp::uint(wrapper.at("status"), "control_status_seq") > std::max(stamp_.floor, status_seq_)) result.push_back(wrapper);
    }
    candidate_status_ = Json::object();
    std::sort(result.begin(), result.end(), [](const Json& left, const Json& right) {
      return ControlTransportStamp::uint(left.at("status"), "control_status_seq") < ControlTransportStamp::uint(right.at("status"), "control_status_seq"); });
    return result;
  }
  bool eligible_status(const Json& message) const {
    try { const auto stamp = FeedbackTransportStamp::parse(message.at("feedback_transport"));
      return confirmed_ && stamp.same(stamp_) && stamp.floor == stamp_.floor &&
          ControlTransportStamp::uint(message, "control_status_seq") > stamp_.floor;
    } catch (const std::exception&) { return false; }
  }
  Json status() const {
    auto value = stamp_.epoch ? stamp_.json() : Json::object();
    value["enabled"] = stamp_.epoch != 0; value["selecting"] = !confirmed_; value["revision"] = revision_;
    value["retained_sends"] = sent_.size(); value["probe_pending"] = probe_seq_ != 0;
    value["dc_paired_round_trip_ms"] = paired_dc_.mean() ? Json(*paired_dc_.mean()) : Json(nullptr);
    value["wss_paired_round_trip_ms"] = paired_wss_.mean() ? Json(*paired_wss_.mean()) : Json(nullptr);
    value["measurement_scope"] = "same_vehicle_result_same_forward_cohort_not_one_way";
    return value;
  }
 private:
  struct Rolling {
    std::array<std::int64_t, NativeControlAckRecovery::kConsecutiveAnomalies> ages{};
    std::size_t count{}, next{};
    void add(std::int64_t age) { ages[next++ % ages.size()] = age; count = std::min(count + 1, ages.size()); }
    std::optional<double> mean() const { if (count < ages.size()) return {}; double sum = 0; for (auto age : ages) sum += age; return sum / ages.size(); }
  };
  struct Health : Rolling {
    std::uint64_t seq{}; std::int64_t progress{}; unsigned anomalies{};
    void note(std::uint64_t sequence, std::int64_t age, std::int64_t now, bool accepted) {
      if (sequence <= seq) return; seq = sequence;
      if (!accepted || age > NativeControlAckRecovery::kFreshAckMs) { count = 0; if (anomalies < NativeControlAckRecovery::kConsecutiveAnomalies) ++anomalies; return; }
      anomalies = 0; add(age); progress = now;
    }
    bool healthy(std::int64_t now) const { return mean().has_value() && now >= progress && now - progress <= NativeControlAckRecovery::kFreshAckMs; }
  };
  struct Sent {
    std::uint64_t seq, intent; std::int64_t started; ControlPath forward; Json request, command_stamp;
    std::optional<std::int64_t> dc, wss; std::string digest; bool delivered, paired;
  };
  void begin(ControlPath path, std::string attempt, std::string issuer, std::uint64_t authority, std::int64_t now) {
    if (stamp_.epoch == std::numeric_limits<std::uint64_t>::max()) throw std::runtime_error("feedback epoch exhausted");
    const auto previous = stamp_.epoch;
    stamp_ = {previous + 1, previous, authority, 0, path, std::move(attempt), std::move(issuer)};
    confirmed_ = false; selected_at_ = now; probe_seq_ = 0; candidate_status_ = Json::object(); ++revision_;
  }
  FeedbackTransportStamp stamp_;
  Json candidate_status_ = Json::object();
  std::deque<Sent> sent_;
  Health dc_, wss_; Rolling paired_dc_, paired_wss_;
  std::string context_;
  std::uint64_t revision_{}, probe_seq_{}, delivered_seq_{}, status_seq_{}, reconciled_seq_{};
  std::int64_t probe_started_{}, selected_at_{}, next_trial_{};
  bool confirmed_{};
};
} // namespace mine_teleop::detail
