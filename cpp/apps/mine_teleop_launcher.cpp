#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <vector>

#include <unistd.h>

namespace {

void set_environment(const char* name, const std::string& value) {
  if (setenv(name, value.c_str(), 1) != 0) {
    std::perror(name);
    std::exit(126);
  }
}

}  // namespace

int main(int argc, char** argv) {
  const auto executable = std::filesystem::read_symlink("/proc/self/exe");
  const auto root = executable.parent_path().parent_path();
  const auto library_path =
      (root / "lib").string() + ":" + (root / "lib/vendor/chassis").string() + ":" +
      (root / "lib/vendor/mvs").string();
  if (chdir(root.c_str()) != 0) {
    std::perror("mine-teleop bundle directory");
    return 126;
  }

  const auto current_path = std::getenv("PATH");
  set_environment(
      "PATH", (root / "bin").string() + ":" + (current_path == nullptr ? "/usr/bin:/bin" : current_path));
  set_environment("LD_LIBRARY_PATH", library_path);
  set_environment("GST_PLUGIN_SYSTEM_PATH_1_0", "");
  set_environment("GST_PLUGIN_PATH_1_0", (root / "lib/gstreamer-1.0").string());
  set_environment("GST_PLUGIN_SCANNER", (root / "bin/gst-plugin-scanner").string());
  set_environment("GST_REGISTRY_FORK", "no");
  set_environment(
      "GST_REGISTRY",
      "/tmp/mine-teleop-gstreamer-registry-" + std::to_string(static_cast<unsigned long>(getuid())) + ".bin");
  set_environment("LIBVA_DRIVERS_PATH", (root / "lib/dri").string());
  set_environment("SSL_CERT_FILE", (root / "config/ca-certificates.crt").string());

  const auto runtime = (root / "bin/mine-teleop").string();
  std::vector<std::string> arguments{runtime};
  if (argc == 1) {
    arguments.emplace_back("vehicle-runtime");
    arguments.emplace_back("--config");
    arguments.push_back((root / "config/vehicle-agent.yaml").string());
  } else {
    for (int index = 1; index < argc; ++index) arguments.emplace_back(argv[index]);
  }

  std::vector<char*> raw;
  raw.reserve(arguments.size() + 1);
  for (auto& argument : arguments) raw.push_back(argument.data());
  raw.push_back(nullptr);
  execv(runtime.c_str(), raw.data());
  const auto saved_errno = errno;
  std::perror("mine-teleop-run");
  return saved_errno == ENOENT ? 127 : 126;
}
