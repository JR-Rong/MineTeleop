#include "mine_teleop/gstreamer_media.hpp"

#include <iostream>
#include <stdexcept>
#include <string>

namespace {
void expect(bool value, const char* message) {
  if (!value) throw std::runtime_error(message);
}

void check_transport() {
  GError* error = nullptr;
  auto* transport = mine_teleop::parse_initialized_gstreamer_pipeline(
      "webrtcbin name=webrtc bundle-policy=max-bundle", &error);
  const std::string message = error == nullptr ? "" : error->message;
  if (error != nullptr) g_error_free(error);
  if (transport == nullptr || !message.empty()) {
    if (transport != nullptr) gst_object_unref(transport);
    throw std::runtime_error("cold transport construction failed: " + message);
  }
  const bool correct_factory = std::string(gst_plugin_feature_get_name(
      GST_PLUGIN_FEATURE(gst_element_get_factory(transport)))) == "webrtcbin";
  const bool no_media_pads = GST_ELEMENT(transport)->numsinkpads == 0 &&
      GST_ELEMENT(transport)->numsrcpads == 0;
  gst_object_unref(transport);
  expect(correct_factory, "wrong transport factory");
  expect(no_media_pads, "transport-only construction introduced media pads");
}
}  // namespace

int main() {
  try {
    // Separate CTest process: no gst_init, encoder probe, camera or prior fixture.
    // Elements remain in NULL: no ICE gathering, network or capture is started.
    expect(!gst_is_initialized(), "fixture was already initialized");
    check_transport();
    expect(gst_is_initialized(), "production parser did not initialize GStreamer");
    check_transport();  // Repeated attempts share the existing once-only initializer.
    GError* error = nullptr;
    auto* invalid = mine_teleop::parse_initialized_gstreamer_pipeline(
        "mine-teleop-nonexistent-plugin", &error);
    const bool rejected = invalid == nullptr && error != nullptr;
    if (invalid != nullptr) gst_object_unref(invalid);
    if (error != nullptr) g_error_free(error);
    expect(rejected, "missing plugin did not preserve parser failure");
    std::cout << "cold transport, repeated construction and missing-plugin rejection passed\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
