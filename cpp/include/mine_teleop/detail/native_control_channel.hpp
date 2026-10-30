#pragma once

#include "mine_teleop/detail/native_control_mailbox.hpp"
#define GST_USE_UNSTABLE_API
#include <gst/webrtc/webrtc.h>
#include <memory>
#include <mutex>
#include <string>

namespace mine_teleop::detail {

// Experimental courier only. It never authorizes control: the caller must
// validate the selected SCTP route, input lease and video gate independently.
class NativeControlChannel {
  struct State {
    explicit State(NativeMediaScope value) : scope(std::move(value)), mailbox(scope) {}
    NativeMediaScope scope;
    NativeControlMailbox mailbox;
    std::mutex mutex;
    GstWebRTCDataChannel* channel{};
    gulong open_handler{}, close_handler{}, error_handler{}, message_handler{};
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
    gchar *label = nullptr, *protocol = nullptr;
    gboolean ordered = TRUE;
    gint retransmits = -1;
    g_object_get(channel, "label", &label, "protocol", &protocol, "ordered", &ordered,
                 "max-retransmits", &retransmits, nullptr);
    const std::string label_value = label ? label : "", protocol_value = protocol ? protocol : "";
    g_free(label); g_free(protocol);
    std::lock_guard lock(state->mutex);
    if (state->retired || !state->mailbox.attach(state->scope, label_value, protocol_value,
                                                 ordered != FALSE, retransmits)) return;
    state->channel = GST_WEBRTC_DATA_CHANNEL(g_object_ref(channel));
    state->open_handler = connect_owned(channel, "on-open", G_CALLBACK(opened), state);
    state->close_handler = connect_owned(channel, "on-close", G_CALLBACK(closed), state);
    state->error_handler = connect_owned(channel, "on-error", G_CALLBACK(error), state);
    state->message_handler = connect_owned(channel, "on-message-string", G_CALLBACK(message), state);
  }
  static void opened(GstWebRTCDataChannel*, gpointer data) {
    const auto state = *static_cast<Owner*>(data);
    std::lock_guard lock(state->mutex);
    if (!state->retired && state->mailbox.usable()) state->open = true;
  }
  static void closed(GstWebRTCDataChannel*, gpointer data) {
    const auto state = *static_cast<Owner*>(data);
    std::lock_guard lock(state->mutex);
    state->open = false;
    state->mailbox.retire();
  }
  static void error(GstWebRTCDataChannel*, GError*, gpointer data) { closed(nullptr, data); }
  static void message(GstWebRTCDataChannel*, gchar* value, gpointer data) {
    const auto state = *static_cast<Owner*>(data);
    if (value) {
      std::size_t length = 0;
      while (length <= 64 * 1024 && value[length]) ++length;
      static_cast<void>(state->mailbox.receive(state->scope, std::string_view(value, length)));
    }
    else state->mailbox.retire();
  }

 public:
  NativeControlChannel(GstElement* peer, NativeMediaScope scope)
      : state_(std::make_shared<State>(std::move(scope))), peer_(GST_ELEMENT(gst_object_ref(peer))) {
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
  std::optional<std::string> pop() { return state_->mailbox.pop(state_->scope); }

  // A successful return means queued locally, not delivered or authenticated.
  bool send(const std::string& payload) {
#if GST_CHECK_VERSION(1, 22, 0)
    if (payload.empty() || payload.size() > 64 * 1024 || payload.find('\0') != std::string::npos) return false;
    std::lock_guard lock(state_->mutex);
    if (state_->retired || !state_->open || !state_->mailbox.usable() || !state_->channel) return false;
    GstWebRTCDataChannelState ready = GST_WEBRTC_DATA_CHANNEL_STATE_CLOSED;
    guint64 buffered = 0;
    g_object_get(state_->channel, "ready-state", &ready, "buffered-amount", &buffered, nullptr);
    if (ready != GST_WEBRTC_DATA_CHANNEL_STATE_OPEN || buffered != 0) return false;
    GError* error = nullptr;
    const bool ok = gst_webrtc_data_channel_send_string_full(state_->channel, payload.c_str(), &error) != FALSE;
    g_clear_error(&error);
    return ok;
#else
    (void)payload;
    return false;
#endif
  }

  void retire() {
    GstWebRTCDataChannel* channel = nullptr;
    gulong handlers[4]{};
    {
      std::lock_guard lock(state_->mutex);
      if (state_->retired) return;
      state_->retired = true;
      state_->open = false;
      state_->mailbox.retire();
      channel = state_->channel;
      state_->channel = nullptr;
      handlers[0] = state_->open_handler; handlers[1] = state_->close_handler;
      handlers[2] = state_->error_handler; handlers[3] = state_->message_handler;
    }
    if (peer_) {
      g_signal_handler_disconnect(peer_, peer_handler_);
      gst_object_unref(peer_); peer_ = nullptr;
    }
    if (channel) {
      for (const auto handler : handlers) if (handler) g_signal_handler_disconnect(channel, handler);
      gst_webrtc_data_channel_close(channel);
      g_object_unref(channel);
    }
  }

 private:
  Owner state_;
  GstElement* peer_{};
  gulong peer_handler_{};
};

}  // namespace mine_teleop::detail
