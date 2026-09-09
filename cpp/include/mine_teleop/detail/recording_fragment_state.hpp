#pragma once

#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <utility>

#include <gst/gst.h>

namespace mine_teleop::detail {

// An owner is shared by the splitmux internal sink and muxer through their
// GObject qdata.  It deliberately contains no GstObject or Lane pointer: a
// late bus message retains its source object, and qdata retains this owner
// until that source is destroyed.  This prevents object-address reuse from
// assigning a retired async-finalize error to a later pipeline generation.
enum class RecordingFragmentState {
  Started,
  Finalizing,
  Completed,
  Failed,
};

struct RecordingFragmentSnapshot {
  std::uint64_t pipeline_generation{0};
  std::uint64_t revision{0};
  std::string session_id;
  std::string camera_id;
  std::filesystem::path path;
  std::optional<std::int64_t> started_at_ms;
  std::optional<std::int64_t> ended_at_ms;
  RecordingFragmentState state{RecordingFragmentState::Started};
  std::string failure;
};

class RecordingFragmentOwner {
 public:
  RecordingFragmentOwner(std::uint64_t pipeline_generation, std::string session_id,
                         std::string camera_id)
      : pipeline_generation_(pipeline_generation),
        session_id_(std::move(session_id)),
        camera_id_(std::move(camera_id)) {}

  // The location comes from splitmuxsink-fragment-opened, after the internal
  // filesink has received its final path.  Binding is idempotent only for the
  // same fragment, so a delayed message cannot retarget an existing owner.
  [[nodiscard]] bool bind_path(const std::filesystem::path& path) {
    if (path.empty())
      return false;
    const auto normalized = path.lexically_normal();
    std::lock_guard lock(mutex_);
    if (path_.empty()) {
      path_ = normalized;
      ++revision_;
      return true;
    }
    return path_ == normalized;
  }

  void set_started_at_ms(std::int64_t started_at_ms) {
    std::lock_guard lock(mutex_);
    if (!started_at_ms_.has_value())
      started_at_ms_ = started_at_ms;
  }

  // A close message is a necessary but not sufficient completion signal.  A
  // previously latched failure always wins, including error -> closed and a
  // duplicate/later closed message.
  [[nodiscard]] bool mark_closed(std::int64_t ended_at_ms) {
    std::lock_guard lock(mutex_);
    if (path_.empty() || state_ == RecordingFragmentState::Failed)
      return false;
    if (!ended_at_ms_.has_value())
      ended_at_ms_ = ended_at_ms;
    if (state_ == RecordingFragmentState::Started) {
      state_ = RecordingFragmentState::Finalizing;
      ++revision_;
      return true;
    }
    return false;
  }

  // Failure is intentionally dominant over Completed.  A caller that already
  // published pending metadata must therefore invalidate it after this method
  // succeeds.
  [[nodiscard]] bool mark_failed(std::string failure) {
    std::lock_guard lock(mutex_);
    const bool changed = state_ != RecordingFragmentState::Failed;
    state_ = RecordingFragmentState::Failed;
    if (changed) {
      ++revision_;
      failure_ = std::move(failure);
    }
    return changed;
  }

  // Claim a sidecar publication only after container validation.  The caller
  // must recheck the owner immediately before writing because an async error
  // may arrive while validation is in progress.
  [[nodiscard]] bool claim_completed() {
    std::lock_guard lock(mutex_);
    if (state_ != RecordingFragmentState::Finalizing)
      return false;
    state_ = RecordingFragmentState::Completed;
    ++revision_;
    return true;
  }

  [[nodiscard]] RecordingFragmentSnapshot snapshot() const {
    std::lock_guard lock(mutex_);
    return {
        pipeline_generation_, revision_,    session_id_, camera_id_, path_,
        started_at_ms_,       ended_at_ms_, state_,      failure_,
    };
  }

 private:
  const std::uint64_t pipeline_generation_;
  std::uint64_t revision_{0};
  const std::string session_id_;
  const std::string camera_id_;
  mutable std::mutex mutex_;
  std::filesystem::path path_;
  std::optional<std::int64_t> started_at_ms_;
  std::optional<std::int64_t> ended_at_ms_;
  RecordingFragmentState state_{RecordingFragmentState::Started};
  std::string failure_;
};

// Qdata follows the real splitmux internal object until that object is
// destroyed.  It is therefore safe to classify a bus message from an old sink
// or muxer after async-finalize has detached it from the main pipeline.  The
// record itself contains no raw GstObject pointers, avoiding address reuse
// between pipeline generations.
inline GQuark recording_fragment_owner_quark() {
  static const auto quark = g_quark_from_static_string("mine-teleop-recording-fragment-owner");
  return quark;
}

inline void destroy_recording_fragment_owner(gpointer value) {
  delete static_cast<std::shared_ptr<RecordingFragmentOwner>*>(value);
}

inline void tag_recording_fragment_owner(GstObject* object,
                                         std::shared_ptr<RecordingFragmentOwner> owner) {
  if (object == nullptr || !owner)
    return;
  auto* holder = new std::shared_ptr<RecordingFragmentOwner>(std::move(owner));
  g_object_set_qdata_full(G_OBJECT(object), recording_fragment_owner_quark(), holder,
                          destroy_recording_fragment_owner);
}

[[nodiscard]] inline std::shared_ptr<RecordingFragmentOwner> recording_fragment_owner(
    GstObject* source) {
  GstObject* current = source;
  while (current != nullptr) {
    const auto* holder = static_cast<const std::shared_ptr<RecordingFragmentOwner>*>(
        g_object_get_qdata(G_OBJECT(current), recording_fragment_owner_quark()));
    if (holder != nullptr) {
      const auto result = *holder;
      if (current != source)
        gst_object_unref(current);
      return result;
    }
    GstObject* parent = gst_object_get_parent(current);
    if (current != source)
      gst_object_unref(current);
    current = parent;
  }
  return {};
}

struct FinalizedMp4Validation {
  bool valid{false};
  std::string reason;
};

struct Mp4ProbePadLink {
  GstElement* sink{nullptr};
  bool linked{false};
  bool failed{false};
};

inline void on_mp4_probe_pad_added(GstElement*, GstPad* source_pad, gpointer user_data) {
  auto* link = static_cast<Mp4ProbePadLink*>(user_data);
  if (link == nullptr || link->sink == nullptr || link->linked)
    return;
  GstPad* sink_pad = gst_element_get_static_pad(link->sink, "sink");
  if (sink_pad == nullptr) {
    link->failed = true;
    return;
  }
  const auto result = gst_pad_link(source_pad, sink_pad);
  gst_object_unref(sink_pad);
  if (result == GST_PAD_LINK_OK) {
    link->linked = true;
  } else {
    link->failed = true;
  }
}

// splitmuxsink is configured with mp4mux.  This bounded top-level ISO-BMFF
// check catches the observed truncated mdat/no-moov artifact without scanning
// a large recording on the media/control loop.  A future non-MP4 recorder must
// supply its own validator instead of inheriting this rule.
inline FinalizedMp4Validation validate_finalized_mp4(const std::filesystem::path& path) {
  std::error_code size_error;
  const auto size = std::filesystem::file_size(path, size_error);
  if (size_error || size < 16)
    return {false, "MP4 fragment is missing or too small"};
  if (size > static_cast<std::uintmax_t>(std::numeric_limits<std::streamoff>::max())) {
    return {false, "MP4 fragment is too large for bounded validation"};
  }
  std::ifstream input(path, std::ios::binary);
  if (!input)
    return {false, "cannot open MP4 fragment for validation"};

  bool saw_ftyp = false;
  bool saw_moov = false;
  bool saw_mdat = false;
  std::uintmax_t offset = 0;
  constexpr std::size_t kMaxTopLevelBoxes = 128;
  for (std::size_t index = 0; offset < size && index < kMaxTopLevelBoxes; ++index) {
    std::array<unsigned char, 16> header{};
    input.seekg(static_cast<std::streamoff>(offset), std::ios::beg);
    input.read(reinterpret_cast<char*>(header.data()), 8);
    if (input.gcount() != 8)
      return {false, "truncated MP4 box header"};
    const auto read_u32 = [&](std::size_t start) {
      return (static_cast<std::uint64_t>(header[start]) << 24U) |
             (static_cast<std::uint64_t>(header[start + 1]) << 16U) |
             (static_cast<std::uint64_t>(header[start + 2]) << 8U) |
             static_cast<std::uint64_t>(header[start + 3]);
    };
    std::uintmax_t box_size = read_u32(0);
    std::size_t header_size = 8;
    if (box_size == 1) {
      input.read(reinterpret_cast<char*>(header.data() + 8), 8);
      if (input.gcount() != 8)
        return {false, "truncated extended MP4 box header"};
      box_size = (read_u32(8) << 32U) | read_u32(12);
      header_size = 16;
    } else if (box_size == 0) {
      box_size = size - offset;
    }
    if (box_size < header_size || box_size > size - offset) {
      return {false, "MP4 box extends beyond fragment boundary"};
    }
    const std::string type{static_cast<char>(header[4]), static_cast<char>(header[5]),
                           static_cast<char>(header[6]), static_cast<char>(header[7])};
    saw_ftyp = saw_ftyp || type == "ftyp";
    saw_moov = saw_moov || type == "moov";
    saw_mdat = saw_mdat || type == "mdat";
    offset += box_size;
  }
  if (offset != size)
    return {false, "MP4 top-level box count exceeded validation bound"};
  if (!saw_ftyp || !saw_moov || !saw_mdat) {
    return {false, "MP4 fragment lacks ftyp, moov, or mdat"};
  }
  return {true, {}};
}

// The box check above catches malformed boundaries cheaply.  The worker then
// runs this bounded qtdemux probe off the media/control loop so a hashable but
// unplayable MP4 cannot become a pending upload.  It intentionally links the
// demuxed elementary stream to fakesink rather than requiring a hardware
// decoder on every vehicle build.
inline FinalizedMp4Validation probe_finalized_mp4(const std::filesystem::path& path,
                                                  GstClockTime timeout = 5 * GST_SECOND) {
  GstElement* pipeline = gst_pipeline_new(nullptr);
  GstElement* source = gst_element_factory_make("filesrc", nullptr);
  GstElement* demux = gst_element_factory_make("qtdemux", nullptr);
  GstElement* sink = gst_element_factory_make("fakesink", nullptr);
  if (pipeline == nullptr || source == nullptr || demux == nullptr || sink == nullptr) {
    if (pipeline != nullptr)
      gst_object_unref(pipeline);
    if (source != nullptr)
      gst_object_unref(source);
    if (demux != nullptr)
      gst_object_unref(demux);
    if (sink != nullptr)
      gst_object_unref(sink);
    return {false, "MP4 probe requires filesrc, qtdemux, and fakesink"};
  }
  g_object_set(source, "location", path.string().c_str(), nullptr);
  g_object_set(sink, "sync", FALSE, nullptr);
  gst_bin_add_many(GST_BIN(pipeline), source, demux, sink, nullptr);
  if (!gst_element_link(source, demux)) {
    gst_object_unref(pipeline);
    return {false, "MP4 probe could not link filesrc to qtdemux"};
  }
  Mp4ProbePadLink link{sink};
  g_signal_connect(demux, "pad-added", G_CALLBACK(on_mp4_probe_pad_added), &link);
  const auto state = gst_element_set_state(pipeline, GST_STATE_PLAYING);
  if (state == GST_STATE_CHANGE_FAILURE) {
    gst_element_set_state(pipeline, GST_STATE_NULL);
    gst_object_unref(pipeline);
    return {false, "MP4 probe could not enter PLAYING"};
  }
  GstBus* bus = gst_element_get_bus(pipeline);
  if (bus == nullptr) {
    gst_element_set_state(pipeline, GST_STATE_NULL);
    gst_object_unref(pipeline);
    return {false, "MP4 probe has no bus"};
  }
  GstMessage* message = gst_bus_timed_pop_filtered(
      bus, timeout, static_cast<GstMessageType>(GST_MESSAGE_ERROR | GST_MESSAGE_EOS));
  FinalizedMp4Validation result;
  if (message == nullptr) {
    result = {false, "MP4 probe timed out before EOS"};
  } else if (GST_MESSAGE_TYPE(message) == GST_MESSAGE_EOS && link.linked && !link.failed) {
    result = {true, {}};
  } else if (GST_MESSAGE_TYPE(message) == GST_MESSAGE_ERROR) {
    GError* error = nullptr;
    gchar* debug = nullptr;
    gst_message_parse_error(message, &error, &debug);
    const std::string reason = error == nullptr ? "unknown qtdemux error" : error->message;
    if (error != nullptr)
      g_error_free(error);
    g_free(debug);
    result = {false, "MP4 qtdemux probe failed: " + reason};
  } else {
    result = {false, "MP4 probe reached EOS without a demuxed elementary stream"};
  }
  if (message != nullptr)
    gst_message_unref(message);
  gst_object_unref(bus);
  gst_element_set_state(pipeline, GST_STATE_NULL);
  gst_object_unref(pipeline);
  return result;
}

}  // namespace mine_teleop::detail
