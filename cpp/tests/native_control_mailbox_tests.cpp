#include "mine_teleop/detail/native_control_mailbox.hpp"
#include <iostream>
#include <atomic>
#include <stdexcept>
#include <string>
#include <thread>

using namespace mine_teleop::detail;
namespace {
void require(bool value, const char* why) { if (!value) throw std::runtime_error(why); }
}
int main() {
  try {
    const NativeMediaScope scope{"session-a", 7, "attempt-a"};
    const NativeMediaScope stale{"session-a", 7, "attempt-old"};
    NativeControlMailbox box(scope);
    require(!box.attach(stale, "control", "mine-teleop-control-v1", false, 0), "stale channel attached");
    require(!box.attach(scope, "other", "mine-teleop-control-v1", false, 0), "wrong label attached");
    require(!box.attach(scope, "control", "wrong", false, 0), "wrong protocol attached");
    require(!box.attach(scope, "control", "mine-teleop-control-v1", true, 0), "ordered channel attached");
    require(!box.attach(scope, "control", "mine-teleop-control-v1", false, -1), "reliable channel attached");
    require(box.attach(scope, "control", "mine-teleop-control-v1", false, 0), "valid channel rejected");
    require(!box.receive(stale, "old"), "stale message admitted");
    require(box.receive(scope, "first") && box.receive(scope, "second"), "valid messages rejected");
    require(box.pop(scope) == "first" && box.pop(scope) == "second", "messages reordered");
    require(!box.pop(scope).has_value(), "empty mailbox returned a message");
    require(!box.receive(scope, std::string(65537, 'x')) && !box.usable(), "oversized message did not fail closed");
    require(!box.receive(scope, "after-failure") && !box.pop(scope), "failed mailbox recovered silently");
    NativeControlMailbox overflow(scope);
    require(overflow.attach(scope, "control", "mine-teleop-control-v1", false, 0), "overflow fixture attach");
    for (int i = 0; i < 64; ++i) require(overflow.receive(scope, "x"), "queue capacity smaller than 64");
    require(!overflow.receive(scope, "overflow") && !overflow.usable() && !overflow.pop(scope),
        "overflow did not retire and clear the mailbox");
    NativeControlMailbox retired(scope);
    require(retired.attach(scope, "control", "mine-teleop-control-v1", false, 0), "retire fixture attach");
    require(retired.receive(scope, "pending"), "retire fixture enqueue");
    retired.retire();
    require(!retired.receive(scope, "late") && !retired.pop(scope) && !retired.usable(), "retired callback polluted state");
    NativeControlMailbox duplicate(scope);
    require(duplicate.attach(scope, "control", "mine-teleop-control-v1", false, 0), "duplicate fixture attach");
    require(!duplicate.attach(scope, "control", "mine-teleop-control-v1", false, 0) && !duplicate.usable(),
        "duplicate control channel did not fail closed");
    NativeControlMailbox racing(scope);
    require(racing.attach(scope, "control", "mine-teleop-control-v1", false, 0), "race fixture attach");
    std::atomic<bool> start{false};
    std::thread callback([&] { while (!start.load()) std::this_thread::yield();
      for (int i = 0; i < 32; ++i) static_cast<void>(racing.receive(scope, "callback")); });
    start = true;
    racing.retire();
    callback.join();
    require(!racing.usable() && !racing.pop(scope), "callback survived concurrent retirement");
    std::cout << "PASS native control mailbox scope, ordering, bounds and retirement\n";
    return 0;
  } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
