#include "mine_teleop/recording.hpp"
#include <iostream>
#include <string_view>

int main(int argc, char** argv) {
  if (argc == 2 && std::string_view(argv[1]) == "--help") {
    std::cout << "mine-teleop-recorder: isolated local encoded-AU recorder worker\nInternal IPC only; no vehicle configuration, camera, server or CAN entry point.\n";
    return 0;
  }
  if (argc != 5 || std::string_view(argv[1]) != "--ipc-in-fd" || std::string_view(argv[2]) != "3" ||
      std::string_view(argv[3]) != "--ipc-status-fd" || std::string_view(argv[4]) != "4") return 2;
  return mine_teleop::run_recording_worker(3, 4);
}
