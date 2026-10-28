// macOS-only Node-API adapter for the opt-in hybrid bench. The receiver runs
// off the Cocoa/Node thread; only view creation/layout/removal run on it.
#import <Cocoa/Cocoa.h>
#import <QuartzCore/QuartzCore.h>
#include <node_api.h>
#include "native_receiver_core.hpp"
#include <cstring>

using namespace mine_teleop::bench;

@interface BenchVideoView : NSView
@end
@implementation BenchVideoView
- (NSView*)hitTest:(NSPoint)point { (void)point; return nil; }
@end

namespace {
struct Layer {
  std::mutex mutex;
  std::deque<Json> commands, events;
  std::jthread worker;
  std::atomic<bool> finished{false};
  NSView* parent{};
  std::vector<NSView*> views;
  bool started{};
  void emit(Json event) {
    std::lock_guard lock(mutex);
    if (events.size() >= 256) throw std::runtime_error("native event queue exceeded");
    events.push_back(std::move(event));
  }
  void run(Options opts, std::stop_token stop) {
    Json report{{"schema", "native-receiver-remote-v1"}, {"runtime", gst_version_string()},
        {"expected_transport", opts.ice_transport}, {"measurement_start_clock_ms", bench_now_ms()}, {"samples", Json::array()}, {"passed", false}};
    try {
      Receiver receiver(opts);
      receiver.start();
      bool have_offer = false;
      auto next_sample = Clock::now();
      const auto deadline = Clock::now() + 1900s;
      try {
        while (!stop.stop_requested()) {
          std::deque<Json> pending;
          { std::lock_guard lock(mutex); pending.swap(commands); }
          for (const auto& command : pending) {
            if (command.at("op") == "offer") {
              const auto sdp = receiver.answer(command.at("payload"));
              have_offer = true;
              emit({{"type", "answer"}, {"sdp", sdp}});
            } else receiver.candidate(command.at("payload"));
          }
          if (Clock::now() >= deadline) throw std::runtime_error("native bench hard deadline exceeded");
          receiver.check();
          for (const auto& candidate : receiver.candidates()) emit({{"type", "candidate"}, {"candidate", candidate}});
          if (have_offer && Clock::now() >= next_sample) {
            const auto pair = receiver.selected_pair();
            auto sample = Json{{"clock_ms", bench_now_ms()}, {"tracks", receiver.counters()}, {"webrtc", receiver.stats()}, {"selected_pair", pair}};
            report["samples"].push_back(sample);
            emit({{"type", "sample"}, {"sample", sample}});
            next_sample = Clock::now() + 1s;
          }
          std::this_thread::sleep_for(5ms);
        }
      } catch (const std::exception& error) { report["error"] = error.what(); }
      receiver.stop();
      report["receiver"] = receiver.report();
      report["measurement_end_clock_ms"] = bench_now_ms();
      report["passed"] = have_offer && !report.contains("error") &&
          report["receiver"]["tracks"].size() == opts.window_handles.size() &&
          report["receiver"]["selected_pair"].value("local_transport", "") == opts.ice_transport &&
          report["receiver"]["selected_pair"].value("remote_transport", "") == opts.ice_transport;
      for (const auto& track : report["receiver"]["tracks"])
        if (track.at("draw_events_pts_ns_clock_ms").size() < 30 || track.at("identity_observation_overflow") != 0)
          report["passed"] = false;
    } catch (const std::exception& error) { report["error"] = error.what(); }
    { std::lock_guard lock(mutex); events.push_back({{"type", "complete"}, {"report", report}}); finished = true; }
  }
};
std::unique_ptr<Layer> layer;

void check(napi_status status) { if (status != napi_ok) throw std::invalid_argument("invalid native media argument"); }
std::vector<napi_value> arguments(napi_env env, napi_callback_info info, size_t count) {
  std::vector<napi_value> result(count);
  size_t actual = count;
  check(napi_get_cb_info(env, info, &actual, result.data(), nullptr, nullptr));
  if (actual != count) throw std::invalid_argument("wrong argument count");
  return result;
}
std::string string(napi_env env, napi_value value, size_t limit = 128 * 1024) {
  size_t size{}; check(napi_get_value_string_utf8(env, value, nullptr, 0, &size));
  if (size > limit) throw std::invalid_argument("native media argument too large");
  std::string text(size + 1, '\0');
  check(napi_get_value_string_utf8(env, value, text.data(), text.size(), &size)); text.resize(size); return text;
}
napi_value json(napi_env env, const Json& value) {
  const auto text = value.dump(); napi_value result;
  check(napi_create_string_utf8(env, text.c_str(), text.size(), &result)); return result;
}
template<typename F> napi_value guarded(napi_env env, F body) {
  try { if (![NSThread isMainThread]) throw std::runtime_error("native media API requires the Cocoa main thread"); return body(); }
  catch (const std::exception& e) { napi_throw_error(env, nullptr, e.what()); return nullptr; }
}
napi_value create(napi_env env, napi_callback_info info) {
  return guarded(env, [&] {
    if (layer) throw std::runtime_error("native media already initialized");
    const auto args = arguments(env, info, 4);
    const auto config = load_vehicle_config(string(env, args[1], 4096));
    validate(config);
    void* data{}; size_t size{}; check(napi_get_buffer_info(env, args[0], &data, &size));
    if (size != sizeof(NSView*)) throw std::invalid_argument("invalid native window handle");
    NSView* parent{}; std::memcpy(&parent, data, sizeof(parent));
    if (![parent isKindOfClass:[NSView class]] || !parent.window) throw std::runtime_error("window handle is not an attached NSView");
    gst_init(nullptr, nullptr);
    auto next = std::make_unique<Layer>(); next->parent = parent;
    Options opts; opts.decoder = "vtdec_hw"; opts.display = true;
    opts.ice_transport = string(env, args[2], 8);
    if (opts.ice_transport != "udp" && opts.ice_transport != "tcp") throw std::invalid_argument("invalid bench transport");
    const auto network = Json::parse(string(env, args[3], 1024));
    if (!network.is_null()) {
      opts.local_address = network.at("localIp").get<std::string>();
      opts.local_public_address = network.at("localPublicIp").get<std::string>();
      opts.remote_address = network.at("remotePublicIp").get<std::string>();
      const auto stun_url = network.at("stunUrl").get<std::string>();
      if (opts.ice_transport != "udp" || network.at("kind") != "wan-ipv4-udp" || !stun_url.starts_with("stun:"))
        throw std::invalid_argument("WAN bench requires UDP and STUN");
      // The host validates the narrow STUN input before creating this addon.
      opts.stun_uri = "stun://" + stun_url.substr(5);
    }
    for (std::size_t index = 0; index < config.enabled_cameras().size(); ++index) {
      NSView* view = [[BenchVideoView alloc] initWithFrame:NSMakeRect(0, 0, 1, 1)];
      [view setWantsLayer:YES];
      view.layer.masksToBounds = YES;
      [view setHidden:YES]; [parent addSubview:view positioned:NSWindowAbove relativeTo:nil];
      next->views.push_back(view); opts.window_handles.push_back(reinterpret_cast<std::uintptr_t>(view));
    }
    layer = std::move(next);
    // The worker is created only after its owner and stop source exist.
    layer->worker = std::jthread([opts](std::stop_token token) { layer->run(opts, token); });
    return json(env, {{"decoder", "vtdec_hw"}, {"views", layer->views.size()}, {"clock_ms", bench_now_ms()}});
  });
}
napi_value command(napi_env env, napi_callback_info info) {
  return guarded(env, [&] {
    if (!layer || layer->finished) throw std::runtime_error("native media is not running");
    const auto args = arguments(env, info, 1);
    const auto value = Json::parse(string(env, args[0]));
    const auto op = value.at("op").get<std::string>();
    if (op != "offer" && op != "candidate") throw std::invalid_argument("unsupported native media operation");
    std::lock_guard lock(layer->mutex);
    if (layer->commands.size() >= 128) throw std::runtime_error("native command queue exceeded");
    layer->commands.push_back(value); return json(env, true);
  });
}
napi_value layout(napi_env env, napi_callback_info info) {
  return guarded(env, [&] {
    if (!layer) throw std::runtime_error("native media is not initialized");
    auto rects = Json::parse(string(env, arguments(env, info, 1)[0], 8192));
    if (!rects.is_array() || rects.size() != layer->views.size()) throw std::invalid_argument("native layout track mismatch");
    for (size_t i = 0; i < rects.size(); ++i) {
      auto r = rects[i];
      double x = r.at("x"), y = r.at("y"), width = r.at("width"), height = r.at("height");
      for (double v : {x, y, width, height}) if (!std::isfinite(v) || std::abs(v) > 20000) throw std::invalid_argument("invalid video rectangle");
      const auto bounds = layer->parent.bounds;
      NSRect target = NSMakeRect(x, layer->parent.isFlipped ? y : NSHeight(bounds) - y - height, width, height);
      target = NSIntersectionRect(target, bounds);
      if (!NSEqualRects(layer->views[i].frame, target)) [layer->views[i] setFrame:target];
      const BOOL hidden = !r.value("visible", false) || NSIsEmptyRect(target);
      if (layer->views[i].isHidden != hidden) [layer->views[i] setHidden:hidden];
    }
    Json details = Json::array();
    for (auto* view : layer->views) {
      Json children = Json::array();
      for (NSView* child in view.subviews) children.push_back({{"class", std::string(NSStringFromClass(child.class).UTF8String)},
          {"x", child.frame.origin.x}, {"y", child.frame.origin.y}, {"w", child.frame.size.width}, {"h", child.frame.size.height}, {"hidden", static_cast<bool>(child.isHidden)}});
      details.push_back({{"x", view.frame.origin.x}, {"y", view.frame.origin.y}, {"w", view.frame.size.width}, {"h", view.frame.size.height},
          {"hidden", static_cast<bool>(view.isHidden)}, {"children", children}});
    }
    return json(env, details);
  });
}
napi_value poll(napi_env env, napi_callback_info) {
  return guarded(env, [&] {
    Json result = Json::array();
    if (!layer) return json(env, result);
    { std::lock_guard lock(layer->mutex); for (auto& event : layer->events) result.push_back(std::move(event)); layer->events.clear(); }
    if (layer->finished) {
      layer->worker.join();
      for (auto* view : layer->views) { [view removeFromSuperview]; [view release]; }
      layer.reset();
    }
    return json(env, result);
  });
}
napi_value stop(napi_env env, napi_callback_info) {
  return guarded(env, [&] { if (layer) layer->worker.request_stop(); return json(env, true); });
}
napi_value clock_value(napi_env env, napi_callback_info) { return guarded(env, [&] { return json(env, bench_now_ms()); }); }
napi_value init(napi_env env, napi_value exports) {
  for (const auto& [name, callback] : std::initializer_list<std::pair<const char*, napi_callback>>{
       {"clock", clock_value}, {"create", create}, {"command", command}, {"layout", layout}, {"poll", poll}, {"stop", stop}}) {
    napi_value fn; check(napi_create_function(env, name, NAPI_AUTO_LENGTH, callback, nullptr, &fn));
    check(napi_set_named_property(env, exports, name, fn));
  }
  return exports;
}
}  // namespace
NAPI_MODULE(NODE_GYP_MODULE_NAME, init)
