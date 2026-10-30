#pragma once

#include "mine_teleop/detail/native_control_mailbox.hpp"
#define GST_USE_UNSTABLE_API
#include <gst/webrtc/webrtc.h>
#include <gio/gio.h>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace mine_teleop::detail {

inline bool native_direct_udp_candidate(std::string_view type, std::string_view protocol) {
  return (type == "host" || type == "srflx" || type == "prflx") &&
      (protocol == "udp" || protocol == "UDP");
}

// Experimental courier only. It never authorizes control: the caller must
// validate the selected SCTP route, input lease and video gate independently.
class NativeControlChannel {
  struct Candidate {
    GstWebRTCDataChannel* channel{};
    gulong open_handler{}, close_handler{}, error_handler{}, message_handler{};
  };
  struct State {
    explicit State(NativeMediaScope value) : scope(std::move(value)), mailbox(scope) {}
    NativeMediaScope scope;
    NativeControlMailbox mailbox;
    std::mutex mutex;
    std::mutex send_mutex;
    GstWebRTCDataChannel* channel{};  // non-owning; held by candidates
    std::vector<Candidate> candidates;
    bool open{}, retired{};
  };
  using Owner = std::shared_ptr<State>;
  static void release_owner(gpointer data, GClosure*) { delete static_cast<Owner*>(data); }
  static gulong connect_owned(gpointer object, const char* signal, GCallback callback, const Owner& state) {
    return g_signal_connect_data(object, signal, callback, new Owner(state), release_owner, GConnectFlags(0));
  }
  static void prepared(GstElement*, GstWebRTCDataChannel* channel, gboolean local, gpointer data) {
    const auto state = *static_cast<Owner*>(data);
    if (local || !channel) return;
    std::lock_guard lock(state->mutex);
    if (state->retired) return;
    if (state->candidates.size() >= 8) {
      state->open = false;
      state->mailbox.retire();
      return;
    }
    // For a remote in-band channel, the prepare notification may precede
    // reception of its label and protocol. Register now; validate at open.
    Candidate candidate;
    candidate.channel = GST_WEBRTC_DATA_CHANNEL(g_object_ref(channel));
    candidate.open_handler = connect_owned(channel, "on-open", G_CALLBACK(opened), state);
    candidate.close_handler = connect_owned(channel, "on-close", G_CALLBACK(closed), state);
    candidate.error_handler = connect_owned(channel, "on-error", G_CALLBACK(error), state);
    candidate.message_handler = connect_owned(channel, "on-message-string", G_CALLBACK(message), state);
    state->candidates.push_back(candidate);
  }
  static void opened(GstWebRTCDataChannel* channel, gpointer data) {
    const auto state = *static_cast<Owner*>(data);
    gchar *label = nullptr, *protocol = nullptr;
    gboolean ordered = TRUE;
    gint retransmits = -1;
    g_object_get(channel, "label", &label, "protocol", &protocol, "ordered", &ordered,
                 "max-retransmits", &retransmits, nullptr);
    const std::string label_value = label ? label : "", protocol_value = protocol ? protocol : "";
    g_free(label); g_free(protocol);
    std::lock_guard lock(state->mutex);
    if (state->retired) return;
    if (state->mailbox.attach(state->scope, label_value, protocol_value, ordered != FALSE, retransmits)) {
      state->channel = channel;
      state->open = true;
    } else if (!state->mailbox.usable()) state->open = false;
  }
  static void closed(GstWebRTCDataChannel* channel, gpointer data) {
    const auto state = *static_cast<Owner*>(data);
    std::lock_guard lock(state->mutex);
    if (state->channel != channel) return;
    state->open = false;
    state->mailbox.retire();
  }
  static void error(GstWebRTCDataChannel* channel, GError*, gpointer data) { closed(channel, data); }
  static void message(GstWebRTCDataChannel* channel, gchar* value, gpointer data) {
    const auto state = *static_cast<Owner*>(data);
    std::lock_guard lock(state->mutex);
    if (state->retired || !state->open || state->channel != channel) return;
    if (value) {
      std::size_t length = 0;
      while (length <= 64 * 1024 && value[length]) ++length;
      static_cast<void>(state->mailbox.receive(state->scope, std::string_view(value, length)));
    }
    else state->mailbox.retire();
  }

 public:
  NativeControlChannel(GstElement* peer, NativeMediaScope scope)
      : state_(std::make_shared<State>(std::move(scope))) {
    if (!peer || !GST_IS_ELEMENT(peer)) throw std::invalid_argument("invalid WebRTC peer");
    peer_ = GST_ELEMENT(gst_object_ref(peer));
    // on-data-channel is too late to guarantee observation of early messages.
    if (!g_signal_lookup("prepare-data-channel", G_OBJECT_TYPE(peer_))) {
      gst_object_unref(peer_); peer_ = nullptr;
      throw std::runtime_error("early DataChannel observation is unavailable");
    }
    peer_handler_ = connect_owned(peer_, "prepare-data-channel", G_CALLBACK(prepared), state_);
  }
  NativeControlChannel(const NativeControlChannel&) = delete;
  NativeControlChannel& operator=(const NativeControlChannel&) = delete;
  ~NativeControlChannel() { retire(); }

  [[nodiscard]] bool ready() const {
    std::lock_guard lock(state_->mutex);
    return !state_->retired && state_->open && state_->mailbox.usable();
  }
  // Observe the ICE transport actually carrying this SCTP association, not a
  // media-track pair. Unknown, relay, TCP and older runtimes fail closed.
  // This is route classification only, not peer identity or WAN evidence.
  [[nodiscard]] bool direct_udp(const std::string& expected_local = {}, const std::string& expected_remote = {}) const {
#if GST_CHECK_VERSION(1, 28, 0)
    std::lock_guard send_lock(state_->send_mutex);
    {
      std::lock_guard lock(state_->mutex);
      if (state_->retired || !state_->open || !state_->mailbox.usable() || !peer_) return false;
    }
    GstWebRTCSCTPTransport* sctp = nullptr;
    GstWebRTCDTLSTransport* dtls = nullptr;
    GstWebRTCICETransport* ice = nullptr;
    GstWebRTCPeerConnectionState peer_state = GST_WEBRTC_PEER_CONNECTION_STATE_NEW;
    GstWebRTCSCTPTransportState sctp_state = GST_WEBRTC_SCTP_TRANSPORT_STATE_NEW;
    GstWebRTCDTLSTransportState dtls_state = GST_WEBRTC_DTLS_TRANSPORT_STATE_NEW;
    GstWebRTCICEConnectionState ice_state = GST_WEBRTC_ICE_CONNECTION_STATE_NEW;
    g_object_get(peer_, "connection-state", &peer_state, "sctp-transport", &sctp, nullptr);
    if (sctp) g_object_get(sctp, "state", &sctp_state, "transport", &dtls, nullptr);
    if (dtls) g_object_get(dtls, "state", &dtls_state, "transport", &ice, nullptr);
    if (ice) g_object_get(ice, "state", &ice_state, nullptr);
    auto* pair = ice ? gst_webrtc_ice_transport_get_selected_candidate_pair(ice) : nullptr;
    const auto candidate_direct_udp = [](const GstWebRTCICECandidate* candidate) {
      if (!candidate || !candidate->stats || !candidate->stats->type || !candidate->stats->proto) return false;
      return native_direct_udp_candidate(candidate->stats->type, candidate->stats->proto);
    };
    const auto address_matches = [](const GstWebRTCICECandidate* candidate, const std::string& expected) {
      if (expected.empty()) return true;
      if (!candidate || !candidate->stats || !candidate->stats->ipaddr) return false;
      auto* actual = g_inet_address_new_from_string(candidate->stats->ipaddr);
      auto* wanted = g_inet_address_new_from_string(expected.c_str());
      const bool match = actual && wanted && g_inet_address_equal(actual, wanted);
      if (actual) g_object_unref(actual);
      if (wanted) g_object_unref(wanted);
      return match;
    };
    const bool result = peer_state == GST_WEBRTC_PEER_CONNECTION_STATE_CONNECTED &&
        sctp_state == GST_WEBRTC_SCTP_TRANSPORT_STATE_CONNECTED &&
        dtls_state == GST_WEBRTC_DTLS_TRANSPORT_STATE_CONNECTED &&
        (ice_state == GST_WEBRTC_ICE_CONNECTION_STATE_CONNECTED || ice_state == GST_WEBRTC_ICE_CONNECTION_STATE_COMPLETED) &&
        pair && candidate_direct_udp(pair->local) && candidate_direct_udp(pair->remote) &&
        address_matches(pair->local, expected_local) && address_matches(pair->remote, expected_remote);
    if (pair) gst_webrtc_ice_candidate_pair_free(pair);
    if (ice) gst_object_unref(ice);
    if (dtls) gst_object_unref(dtls);
    if (sctp) gst_object_unref(sctp);
    return result;
#else
    (void)expected_local; (void)expected_remote;
    return false;
#endif
  }
  std::optional<std::string> pop() { return state_->mailbox.pop(state_->scope); }

  // A successful return means queued locally, not delivered or authenticated.
  bool send(const std::string& payload) {
#if GST_CHECK_VERSION(1, 22, 0)
    if (payload.empty() || payload.size() > 64 * 1024 || payload.find('\0') != std::string::npos) return false;
    std::lock_guard send_lock(state_->send_mutex);
    GstWebRTCDataChannel* channel = nullptr;
    {
      std::lock_guard lock(state_->mutex);
      if (state_->retired || !state_->open || !state_->mailbox.usable() || !state_->channel) return false;
      channel = GST_WEBRTC_DATA_CHANNEL(g_object_ref(state_->channel));
    }
    GstWebRTCDataChannelState ready = GST_WEBRTC_DATA_CHANNEL_STATE_CLOSED;
    guint64 buffered = 0;
    g_object_get(channel, "ready-state", &ready, "buffered-amount", &buffered, nullptr);
    if (ready != GST_WEBRTC_DATA_CHANNEL_STATE_OPEN || buffered != 0) { g_object_unref(channel); return false; }
    GError* error = nullptr;
    const bool ok = gst_webrtc_data_channel_send_string_full(channel, payload.c_str(), &error) != FALSE;
    g_clear_error(&error);
    g_object_unref(channel);
    return ok;
#else
    (void)payload;
    return false;
#endif
  }

  void retire() {
    std::lock_guard send_lock(state_->send_mutex);
    std::vector<Candidate> candidates;
    {
      std::lock_guard lock(state_->mutex);
      if (state_->retired) return;
      state_->retired = true;
      state_->open = false;
      state_->mailbox.retire();
      candidates.swap(state_->candidates);
      state_->channel = nullptr;
    }
    if (peer_) {
      g_signal_handler_disconnect(peer_, peer_handler_);
      gst_object_unref(peer_); peer_ = nullptr;
    }
    for (const auto& candidate : candidates) {
      for (const auto handler : {candidate.open_handler, candidate.close_handler,
                                 candidate.error_handler, candidate.message_handler})
        if (handler) g_signal_handler_disconnect(candidate.channel, handler);
      g_object_unref(candidate.channel);
    }
  }

 private:
  Owner state_;
  GstElement* peer_{};
  gulong peer_handler_{};
};

}  // namespace mine_teleop::detail
