#include "mine_teleop/media.hpp"
#include "mine_teleop/media_health.hpp"
#include "mine_teleop/relay.hpp"
#include "mine_teleop/server.hpp"
#include "mine_teleop/surround_math.hpp"
#include "mine_teleop/video.hpp"
#include <atomic>
#include <cmath>
#include <fstream>
#include <iostream>
#include <thread>
using namespace mine_teleop;
void require(bool ok, const char *reason) {
  if (!ok)
    throw std::runtime_error(reason);
}
template <class F> void rejects(F f) {
  bool rejected = false;
  try {
    f();
  } catch (const std::exception &) {
    rejected = true;
  }
  require(rejected, "invalid state accepted");
}
void math_test() {
  surround::Camera c;
  c.width = 100;
  c.height = 100;
  c.K = {40, 0, 50, 0, 40, 50, 0, 0, 1};
  c.xi = 1;
  c.vehicle_from_camera = {1, 0, 0, 0, 0, -1, 0, 0, 0, 0, -1, 2, 0, 0, 0, 1};
  c.runtime_from_calibration = {1, 0, 0, 0, 1, 0, 0, 0, 1};
  for (double x : {-2., -.5, 0., .7, 2.})
    for (double y : {-2., -.3, 0., .4, 2.}) {
      const auto pixel = surround::project(c, {x, y, 0});
      require(pixel.valid, "ground projection invalid");
      const auto restored = surround::ground_point(c, pixel);
      require(std::hypot(restored.x - x, restored.y - y) < 1e-7,
              "projection direction/xi inverse incorrect");
    }
  // Independently generated with OpenCV omnidir.projectPoints, including
  // distortion.
  auto golden = c;
  golden.width = 1280;
  golden.height = 720;
  golden.K = {300, 0, 640, 0, 305, 360, 0, 0, 1};
  golden.xi = .7;
  golden.D = {.01, -.002, .003, -.004};
  const std::array<std::array<double, 4>, 4> points = {
      {{-2, -1, 492.1069725046482, 435.2696754531076},
       {0, 0, 640, 360},
       {2, .5, 788.952256369506, 322.3024776390804},
       {1, -2, 713.0898807911024, 509.6140088479166}}};
  for (const auto &p : points) {
    auto pixel = surround::project(golden, {p[0], p[1], 0});
    require(std::hypot(pixel.u - p[2], pixel.v - p[3]) < 1e-6,
            "native omnidir differs from OpenCV");
  }
  const auto transform = surround::image_mapping(
      1280, 720, {100, 50, 1000, 600}, 90, true, false, 300, 500);
  require(std::abs(transform[0]) < 1e-9 && std::abs(transform[1] - .5) < 1e-9,
          "crop/rotation/mirror coordinate order differs");
  const std::string header =
      "v=0\r\no=- 0 0 IN IP4 127.0.0.1\r\ns=-\r\nt=0 0\r\n";
  auto answer = [&](std::string level) {
    return header +
           "m=video 9 UDP/TLS/RTP/SAVPF 96\r\na=rtpmap:96 "
           "H264/90000\r\na=fmtp:96 profile-level-id=42c0" +
           level + ";packetization-mode=1\r\n";
  };
  MediaProfile profile;
  profile.width = 1280;
  profile.height = 1440;
  profile.fps = 30;
  require(h264_answer_supports(answer("28"), {profile}),
          "valid Level 4 answer rejected");
  require(!h264_answer_supports(answer("1f"), {profile}),
          "720p-level answer admitted oversized stream");
  require(!h264_answer_supports(answer("zz"), {profile}),
          "malformed H264 level accepted");
  require(!h264_answer_supports(answer("28"), {profile}, {50}),
          "actual Level 5 encoder incorrectly admitted Level 4 receiver");
  require(h264_profile_level_id("Z0LAKM4=") == "42c028",
          "actual SPS profile/level extraction differs");
  require(h264_profile_level_id("bad").empty(), "invalid SPS accepted");
  surround::Renderer renderer({c, c, c, c}, {-2, 2, -2, 2}, .5, .5, 64, 64);
  std::vector<std::uint8_t> frame(100 * 100 * 4, 200), output(64 * 64 * 4);
  surround::Image image{frame.data(), 100, 100, 400, surround::Format::Rgba};
  renderer.render({image, image, image, image}, 15, output.data());
  require(output[(10 * 64 + 10) * 4] > 0, "ground unexpectedly blank");
  renderer.render({image, image, image, image}, 0, output.data());
  require(output[(10 * 64 + 10) * 4] < 32,
          "missing cameras left old pixels visible");
  std::uint8_t uyvy[] = {128, 16, 128, 235, 128, 16, 128, 235}, rgba[16]{};
  surround::resize_into({uyvy, 2, 2, 4, surround::Format::Uyvy}, rgba, 2, 2, 8);
  require(rgba[0] == 0 && rgba[4] >= 254, "UYVY colour conversion incorrect");
  require(minimum_h264_level_idc(1280, 720, 30) == 31, "720p level incorrect");
  require(minimum_h264_level_idc(1280, 1440, 30) == 40,
          "drive mosaic must require level 4");
  require(minimum_h264_level_idc(2560, 1440, 30) == 50,
          "four-up fish requires level 5");
  require(minimum_h264_level_idc(960, 1080, 30) == 32,
          "540p mosaic requires level 3.2");
}
void health_test() {
  MediaHealth h;
  h.reset({"a", "b"}, 100);
  SourceFrameIdentity a{"a", 1, 120, true, 1}, b{"b", 1, 120, true, 1};
  require(h.captured(a, 120) && h.captured(b, 120), "first sources rejected");
  require(h.composed(1, {a, b}, 130, 100, 20) &&
              h.encoded(1, {a, b}, 135, true),
          "healthy composite rejected");
  require(!h.captured(a, 140), "duplicate sequence credited");
  b.sequence = 2;
  b.captured_steady_ms = 150;
  h.captured(b, 150);
  require(!h.encoded(1, {a, b}, 160, true),
          "live output concealed frozen source");
  require(!h.stale(3200, 3000, false).empty(),
          "frozen source did not time out");
  a.sequence = 0;
  a.source_generation = 2;
  a.captured_steady_ms = 3210;
  require(h.captured(a, 3210), "reopen sequence reset rejected");
  auto stale = a;
  stale.source_generation = 1;
  require(!h.captured(stale, 3211), "old camera incarnation accepted");
  b.sequence = 3;
  b.captured_steady_ms = 3240;
  h.captured(b, 3240);
  require(!h.composed(1, {a, b}, 3240, 100, 20), "20ms hard limit ignored");
  require(h.composed(1, {a, b}, 3240, 100, 35),
          "qualified 35ms hard limit rejected");
  require(!h.encoded(1, {a, b}, 3250, false),
          "masked output credited as healthy");
}
Json ledger(const std::filesystem::path &root) {
  std::ifstream in(root / "ledger.json");
  Json j;
  in >> j;
  return j;
}
void agent(const std::filesystem::path &root, std::int64_t now,
           Json reports = Json::object(), bool healthy = true) {
  std::ofstream(root / "status.json")
      << Json{{"instance", ledger(root).at("instance")},
              {"at_ms", now},
              {"healthy", healthy},
              {"leases", reports}};
}
void relay_test() {
  const auto root = std::filesystem::temp_directory_path() /
                    ("mine-relay-" + random_token(8));
  std::filesystem::create_directories(root);
  try {
    RelayBudget budget(root);
    agent(root, 100000);
    std::atomic<int> approved{};
    std::vector<std::thread> threads;
    for (int i = 0; i < 16; ++i)
      threads.emplace_back([&, i] {
        if (budget
                .request("s" + std::to_string(i), "a" + std::to_string(i),
                         "two-540p", 100000)
                .value("approved", false))
          ++approved;
      });
    for (auto &thread : threads)
      thread.join();
    require(approved == 1, "concurrent relay reservations overcommitted");
    const auto leases = ledger(root).at("leases");
    const auto lease = leases.begin().value();
    const auto id = lease.at("lease_id").get<std::string>(),
               session = lease.at("session_id").get<std::string>(),
               attempt = lease.at("media_attempt_id").get<std::string>();
    require(budget.credentials(session, attempt, 100001).empty(),
            "reserved lease issued credentials");
    Json confirm = {{"lease_id", id},
                    {"media_attempt_id", attempt},
                    {"policy_version", 1},
                    {"applied_video_bps", 2200000.0},
                    {"healthy_encoded_frames", true}};
    auto bad = confirm;
    bad["applied_video_bps"] = 9000000;
    rejects([&] { budget.confirm(session, bad, 100001); });
    budget.confirm(session, confirm, 100001);
    agent(root, 100002, {{id, {{"activated", true}}}});
    require(!budget.credentials(session, attempt, 100002).empty(),
            "confirmed activated lease not issued");
    require(budget.credentials(session, "old", 100002).empty(),
            "credentials crossed media attempts");
    budget.revoke(session, attempt, 100003);
    rejects([&] {
      budget.renew(session, {{"lease_id", id}, {"media_attempt_id", attempt}},
                   100004);
    });
    require(!budget.request(session, attempt, "two-540p", 100004)
                 .value("approved", false),
            "revoked attempt resurrected");
    agent(root, 100005,
          {{id,
            {{"secret_deleted", true},
             {"credential_rejected", true},
             {"allocations", 0},
             {"empty_for_ms", 4999}}}});
    require(!budget.request("new", "new", "two-540p", 100005)
                 .value("approved", false),
            "quota released before five empty seconds");
    agent(root, 100006,
          {{id,
            {{"secret_deleted", true},
             {"credential_rejected", true},
             {"allocations", 0},
             {"empty_for_ms", 5000}}}});
    require(budget.request("new", "new", "two-540p", 100006)
                .value("approved", false),
            "reclaimed quota never released");
    {
      RelayBudget restarted(root);
      require(!restarted.request("third", "third", "two-540p", 100007)
                   .value("approved", false),
              "restart admitted before reconciliation");
    }
    NativeControlIntentStore store(200);
    store.set_epoch(10);
    NativeControlIntent intent{"ui", 1, "N", 0, 0, 0, false, 10};
    require(store.update(intent, 1).accepted, "new neutral rejected");
    intent.intent_seq++;
    intent.throttle = .5;
    require(store.update(intent, 2).accepted, "fresh input rejected");
    store.suspend();
    require(!store.sample(3).active,
            "suspended sender sampled retained throttle");
    store.set_epoch(10, false);
    require(!store.sample(4).active,
            "same-epoch status resumed a pending parking request");
    store.set_epoch(11, false);
    require(!store.sample(4).active,
            "unacknowledged new epoch resumed parking input");
    store.set_epoch(11, true);
    require(!store.sample(4).active,
            "epoch change relabelled retained throttle");
    require(!store.update(intent, 5).accepted, "old epoch input accepted");
    intent.control_epoch = 11;
    intent.intent_seq++;
    require(!store.update(intent, 6).accepted,
            "new epoch bypassed neutral interlock");
    intent.throttle = 0;
    require(store.update(intent, 7).accepted, "new neutral cannot rearm");
    CriticalCameraControlLatch latch;
    require(!latch.enter_session("same"), "new session unexpectedly inhibited");
    require(latch.inhibit("same"), "camera fault did not latch");
    latch.revoke_input();
    latch.confirm_parked_rebuild();
    require(latch.enter_session("same"),
            "media recovery cleared same-session camera fault");
    CriticalCameraControlLatch restarted_vehicle;
    require(restarted_vehicle.control_epoch() != latch.control_epoch(),
            "vehicle restart reused control epoch");
  } catch (...) {
    std::filesystem::remove_all(root);
    throw;
  }
  std::filesystem::remove_all(root);
}
void relay_api_test() {
  const auto root = std::filesystem::temp_directory_path() /
                    ("mine-relay-api-" + random_token(8));
  std::filesystem::create_directories(root);
  try {
    SignalingServerConfig config;
    config.relay_state_dir = root;
    config.audit_log_path = (root / "audit.jsonl").string();
    config.turn_realm = "fixture.test";
    config.turn_urls = {"turn:127.0.0.1:3478?transport=udp"};
    {
      SignalingService service(config);
      auto post = [&](const std::string &path, const Json &body) {
        HttpRequest r;
        r.method = "POST";
        r.path = path;
        r.target = path;
        r.peer_address = "127.0.0.1";
        r.body = body.dump();
        return service.handle(r);
      };
      auto get =
          [&](const std::string &path,
              const std::unordered_map<std::string, std::string> &query) {
            HttpRequest r;
            r.method = "GET";
            r.path = path;
            r.target = path;
            r.peer_address = "127.0.0.1";
            r.query = query;
            return Json::parse(service.handle(r).body);
          };
      const auto online = Json::parse(
          post("/vehicles/online", {{"vehicle_id", "vehicle-001"},
                                    {"device_token", "dev-device-secret"},
                                    {"connection_id", "fixture"}})
              .body);
      const auto login = Json::parse(
          post("/auth/driver_login", {{"driver_id", "driver-console-001"},
                                      {"password", "dev-password"}})
              .body);
      const auto token = login.at("token").get<std::string>();
      const auto session =
          Json::parse(post("/sessions", {{"driver_id", "driver-console-001"},
                                         {"vehicle_id", "vehicle-001"},
                                         {"token", token}})
                          .body);
      const auto id = session.at("session_id").get<std::string>(),
                 base = "/sessions/" + id;
      Json vehicle = {
          {"actor", "vehicle-001"},
          {"device_token", "dev-device-secret"},
          {"connection_generation", online.at("connection_generation")},
          {"media_attempt_id", "attempt"},
          {"profile", "two-540p"}};
      Json driver = {{"actor", "driver-console-001"},
                     {"token", token},
                     {"media_attempt_id", "attempt"},
                     {"profile", "two-540p"}};
      agent(root, now_ms());
      require(post(base + "/relay/request", driver).status == 401,
              "browser issued relay reservation as encoding attestation");
      const auto reserved =
          Json::parse(post(base + "/relay/request", vehicle).body);
      require(reserved.value("approved", false) && !reserved.contains("secret"),
              "relay reservation invalid or exposed secret");
      auto ice_query = std::unordered_map<std::string, std::string>{
          {"actor", "driver-console-001"},
          {"token", token},
          {"media_attempt_id", "attempt"}};
      require(get(base + "/ice_servers", ice_query).at("ice_servers").size() ==
                  1,
              "reservation alone issued TURN");
      Json policy = {{"lease_id", reserved.at("lease_id")},
                     {"policy_version", 1},
                     {"applied_video_bps", 2200000.0},
                     {"healthy_encoded_frames", true}};
      driver.update(policy);
      vehicle.update(policy);
      require(post(base + "/relay/confirm", driver).status == 401,
              "driver replaced device encoding confirmation");
      require(post(base + "/relay/confirm", vehicle).status == 200,
              "trusted device confirmation failed");
      agent(root, now_ms(),
            {{reserved.at("lease_id").get<std::string>(),
              {{"activated", true}}}});
      const auto issued = get(base + "/ice_servers", ice_query);
      require(issued.at("ice_servers").size() == 2,
              "confirmed relay credentials missing");
      ice_query["media_attempt_id"] = "retired";
      require(get(base + "/ice_servers", ice_query).at("ice_servers").size() ==
                  1,
              "retired media attempt obtained credentials");
      require(post(base + "/relay/release", driver).status == 200,
              "driver could not revoke relay qualification");
      require(post(base + "/relay/renew", vehicle).status == 400,
              "late device renewal resurrected revocation");
      require(post(base + "/relay/confirm", vehicle).status == 400,
              "late confirmation resurrected revocation");
      ice_query["media_attempt_id"] = "attempt";
      require(get(base + "/ice_servers", ice_query).at("ice_servers").size() ==
                  1,
              "revoked lease still issued credentials");
    }
  } catch (...) {
    std::filesystem::remove_all(root);
    throw;
  }
  std::filesystem::remove_all(root);
}
int main() {
  try {
    math_test();
    health_test();
    relay_test();
    relay_api_test();
    std::cout << "PASS projection, masking, H264 levels, three-layer health, "
                 "epochs and relay lifecycle\n";
    return 0;
  } catch (const std::exception &e) {
    std::cerr << e.what() << '\n';
    return 1;
  }
}
