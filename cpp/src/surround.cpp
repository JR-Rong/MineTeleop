#include "mine_teleop/surround.hpp"
#include <algorithm>
#include <cmath>
#include <fstream>
#include <iomanip>
#include <sstream>
#ifdef __APPLE__
#include <CommonCrypto/CommonDigest.h>
#else
#include <openssl/evp.h>
#endif
namespace mine_teleop {
std::string sha256_text(std::string_view text) {
  std::array<unsigned char, 32> bytes{};
#ifdef __APPLE__
  CC_SHA256(text.data(), static_cast<CC_LONG>(text.size()), bytes.data());
#else
  unsigned size = 0;
  if (EVP_Digest(text.data(), text.size(), bytes.data(), &size, EVP_sha256(),
                 nullptr) != 1 ||
      size != 32)
    throw std::runtime_error("SHA256 failed");
#endif
  std::ostringstream out;
  out << std::hex << std::setfill('0');
  for (auto b : bytes)
    out << std::setw(2) << unsigned(b);
  return out.str();
}
namespace {
template <std::size_t N> std::array<double, N> matrix(const Json &j) {
  if (!j.is_array() || j.size() != N)
    throw std::runtime_error("invalid calibration matrix dimensions");
  std::array<double, N> out{};
  for (std::size_t i = 0; i < N; ++i) {
    out[i] = j[i].get<double>();
    if (!std::isfinite(out[i]))
      throw std::runtime_error("non-finite calibration");
  }
  return out;
}
} // namespace
SurroundCalibration load_surround_calibration(const std::filesystem::path &path,
                                              const VehicleConfig &config,
                                              bool acceptance) {
  std::ifstream file(path);
  if (!file)
    throw std::runtime_error("cannot open surround calibration");
  Json envelope;
  file >> envelope;
  const auto payload = envelope.at("payload").get<std::string>();
  if (payload.size() > 4 * 1024 * 1024 ||
      sha256_text(payload) != envelope.at("sha256").get<std::string>())
    throw std::runtime_error("surround calibration hash mismatch");
  const auto doc = Json::parse(payload);
  SurroundCalibration result;
  result.hash = envelope.at("sha256");
  result.document = doc;
  if (doc.at("schema") != 1 || doc.at("vehicle_id") != config.vehicle_id ||
      doc.at("model") != "opencv_omnidir" ||
      doc.at("transform") != "T_vehicle_from_camera_m" ||
      doc.at("pixel_mapping") != "crop_orientation_resize_pixel_centres")
    throw std::runtime_error(
        "surround calibration convention/identity mismatch");
  if (acceptance) {
    const auto &a = doc.at("acceptance");
    if (!a.value("static_pass", false) || !a.value("dynamic_pass", false) ||
        a.value("check_points", 0) < 32 || a.value("p95_m", 1.0) > .05 ||
        a.value("max_m", 1.0) > .10 ||
        a.value("measurement_accuracy_m", 1.0) > .005 ||
        a.value("ground_flatness_m", 1.0) > .005 ||
        !a.at("profiles")
             .value(config.surround.mode == "full" ? "full"
                                                   : config.surround.profile,
                    false))
      throw std::runtime_error(
          "surround calibration/profile has not passed field acceptance");
  }
  const std::array<std::string, 4> ids{"fish_front", "fish_rear", "fish_left",
                                       "fish_right"};
  for (unsigned i = 0; i < 4; ++i) {
    const auto &j = doc.at("cameras").at(ids[i]);
    auto &c = result.cameras[i];
    auto found = std::find_if(
        config.cameras.begin(), config.cameras.end(),
        [&](const auto &x) { return x.id == ids[i] && x.enabled; });
    if (found == config.cameras.end() || !found->critical_for_control ||
        found->installation_id.empty() || j.at("device") != found->device)
      throw std::runtime_error(
          "surround requires four matching critical fisheye inputs");
    const auto fingerprint = sha256_text(
        found->id + ":" + found->device + ":" + found->installation_id + ":" +
        std::to_string(found->capture_width) + "x" +
        std::to_string(found->capture_height));
    if (j.at("installation_fingerprint") != fingerprint)
      throw std::runtime_error("surround installation fingerprint mismatch");
    c.K = matrix<9>(j.at("K"));
    c.xi = j.at("xi");
    c.D = matrix<4>(j.at("D"));
    c.vehicle_from_camera = matrix<16>(j.at("T_vehicle_from_camera"));
    c.runtime_from_calibration = matrix<9>(j.at("A_runtime_from_calibration"));
    c.width = j.at("runtime_size")[0];
    c.height = j.at("runtime_size")[1];
    const auto sensor = j.at("sensor_size").get<std::array<int, 2>>();
    if (j.at("calibration_size") != j.at("sensor_size"))
      throw std::runtime_error("K must use raw calibration sensor pixels");
    const auto &transform = j.at("image_transform");
    // The present native capture path delivers raw sensor pixels. Any future
    // crop/orientation stage must declare and perform that transformation here.
    if (transform.at("crop") != Json::array({0, 0, sensor[0], sensor[1]}) ||
        transform.at("rotation_deg") != 0 ||
        transform.at("mirror_x") != false ||
        transform.at("mirror_y") != false ||
        sensor != std::array<int, 2>{c.width, c.height})
      throw std::runtime_error(
          "native capture does not perform the declared image transformation");
    const auto resize = transform.at("resize").get<std::array<int, 2>>();
    if (resize != std::array<int, 2>{c.width, c.height})
      throw std::runtime_error("runtime resize mismatch");
    const auto expected = surround::image_mapping(
        sensor[0], sensor[1], transform.at("crop").get<std::array<int, 4>>(),
        transform.at("rotation_deg"), transform.at("mirror_x"),
        transform.at("mirror_y"), c.width, c.height);
    for (unsigned k = 0; k < 9; ++k)
      if (std::abs(expected[k] - c.runtime_from_calibration[k]) > 1e-8)
        throw std::runtime_error("crop/orientation/resize matrix mismatch; K "
                                 "must not be scaled twice");
    const auto &profile = config.realtime_profile(found->realtime_profile);
    const bool raw = found->backend == "ccg2";
    if (c.width != (raw ? found->capture_width : profile.width) ||
        c.height != (raw ? found->capture_height : profile.height) ||
        c.width <= 0 || c.height <= 0 || c.width > 8192 || c.height > 8192 ||
        !std::isfinite(c.xi) || c.xi < 0 || c.K[0] <= 0 || c.K[4] <= 0 ||
        c.K[8] != 1)
      throw std::runtime_error(
          "surround capture dimensions or intrinsics mismatch");
    const auto &t = c.vehicle_from_camera;
    if (t[12] != 0 || t[13] != 0 || t[14] != 0 || t[15] != 1 || t[11] <= 0)
      throw std::runtime_error("surround camera pose invalid");
    for (int a = 0; a < 3; ++a)
      for (int b = 0; b < 3; ++b) {
        double dot = 0;
        for (int k = 0; k < 3; ++k)
          dot += t[k * 4 + a] * t[k * 4 + b];
        if (std::abs(dot - (a == b ? 1 : 0)) > 1e-5)
          throw std::runtime_error("surround rotation is not orthonormal");
      }
    const auto det = t[0] * (t[5] * t[10] - t[6] * t[9]) -
                     t[1] * (t[4] * t[10] - t[6] * t[8]) +
                     t[2] * (t[4] * t[9] - t[5] * t[8]);
    if (std::abs(det - 1) > 1e-5)
      throw std::runtime_error("surround pose is not right-handed");
    if (j.at("intrinsic_samples").get<int>() < 20 ||
        j.at("intrinsic_rms_px").get<double>() > 1.5 ||
        j.at("ground_board_count").get<int>() < 3)
      throw std::runtime_error(
          "surround calibration sample coverage insufficient");
  }
  const auto roi = matrix<4>(doc.at("ground_region"));
  result.region = {roi[0], roi[1], roi[2], roi[3]};
  result.vehicle_length = doc.at("vehicle_length_m");
  result.vehicle_width = doc.at("vehicle_width_m");
  if (!std::isfinite(result.vehicle_length) ||
      !std::isfinite(result.vehicle_width) || result.vehicle_length <= 0 ||
      result.vehicle_width <= 0 || roi[0] >= roi[1] || roi[2] >= roi[3])
    throw std::runtime_error("invalid surround physical dimensions");
  return result;
}
} // namespace mine_teleop
