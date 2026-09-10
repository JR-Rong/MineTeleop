#include "mine_teleop/detail/console_assets.hpp"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>

namespace {
void expect(bool valid, const char* message) { if (!valid) throw std::runtime_error(message); }
}

int main() {
  const auto root = std::filesystem::temp_directory_path() / ("mine-console-assets-" + mine_teleop::random_token(8));
  std::filesystem::create_directories(root / "assets/models");
  try {
    std::ofstream(root / "assets/main.js") << "export const ready=true;";
    std::ofstream(root / "assets/models/truck.glb", std::ios::binary).write("glTF\0data", 9);
    std::ofstream(root / "secret.json") << "not served";
    std::ofstream(root / "assets/secret.yaml") << "not served";
    mine_teleop::DriverConfig config; config.visual_assets_root = root / "assets";
    const auto script = mine_teleop::serve_console_asset(config, "/assets/main.js");
    expect(script.status == 200 && script.content_type.starts_with("text/javascript"), "JavaScript asset unavailable");
    const auto model = mine_teleop::serve_console_asset(config, "/assets/models/truck.glb");
    expect(model.status == 200 && model.body.size() == 9 && model.body[4] == '\0', "binary model was truncated");
    for (const auto* path : {"/assets/../secret.json", "/assets/models/../../secret.json", "/assets/C:/secret.json", "/assets/secret.yaml", "/assets/missing.glb"}) {
      expect(mine_teleop::serve_console_asset(config, path).status == 404, "asset root or allowlist escaped");
    }
    std::error_code error;
    std::filesystem::create_symlink(root / "secret.json", root / "assets/link.json", error);
    if (!error) expect(mine_teleop::serve_console_asset(config, "/assets/link.json").status == 404, "symlink escaped asset root");
    std::filesystem::create_directories(root / "config");
    const auto yaml = root / "config/driver.yaml";
    std::ofstream(yaml) << "driver:\n  id: test\ncloud:\n  signaling_url: ws://127.0.0.1:8765/signaling\nui:\n  scene_manifest: models/custom/model.json\n";
    const auto loaded = mine_teleop::load_driver_config(yaml.string());
    expect(loaded.visual_assets_root == (root / "assets"), "relocated bundle root did not resolve");
    expect(loaded.scene_manifest == "models/custom/model.json", "replacement model config lost");
    std::ofstream(yaml) << "driver:\n  id: test\ncloud:\n  signaling_url: ws://127.0.0.1:8765/signaling\nui:\n  scene_manifest: ../secret.json\n";
    bool rejected = false;
    try { static_cast<void>(mine_teleop::load_driver_config(yaml.string())); } catch (const std::exception&) { rejected = true; }
    expect(rejected, "invalid manifest path accepted");
    std::filesystem::remove_all(root);
    std::cout << "console_assets_tests=passed\n";
  } catch (const std::exception& error) {
    std::filesystem::remove_all(root); std::cerr << error.what() << '\n'; return 1;
  }
}
