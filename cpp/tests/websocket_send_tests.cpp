#include "mine_teleop/detail/websocket_send_deadline.hpp"
#include "mine_teleop/websocket.hpp"
#include <atomic>
#include <condition_variable>
#include <iostream>
#include <mutex>
#include <stop_token>
#include <thread>
#ifndef _WIN32
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace {
using namespace mine_teleop;
using namespace mine_teleop::detail;
using Clock = std::chrono::steady_clock;
using namespace std::chrono_literals;
void expect(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
template<class Run> void throws(Run run) {
  try { run(); } catch (const std::runtime_error&) { return; }
  throw std::runtime_error("expected send failure");
}
void test_all_progress_paths_obey_one_deadline() {
  for (bool complete_late : {false,true}) {
    Clock::time_point now{};
    WebSocketWriteObservation observation;
    throws([&] { send_websocket_bytes("1234",now+20ms,{},
        [&](auto bytes) { now+=30ms; return WebSocketWriteProgress{complete_late?bytes.size():1,false}; },
        [](auto) { return true; },[&] { return now; },[] { return false; },observation); });
    expect(observation.calls==1 && observation.longest_call_ms==30,
        "successful short/complete late write escaped deadline or lost call evidence");
  }
  Clock::time_point now{}; WebSocketWriteObservation observation;
  throws([&] { send_websocket_bytes("1234",now+20ms,{},
      [&](auto) { return WebSocketWriteProgress{0,true}; },
      [&](auto remaining) { expect(remaining==20ms,"poll renewed the budget");now+=remaining;return true; },
      [&] { return now; },[] { return false; },observation); });
  expect(observation.polls==1 && observation.poll_ms==20,"AGAIN did not share deadline");
  now={};observation={};
  send_websocket_bytes("1234",now+20ms,{},[&](auto) {now+=1ms;return WebSocketWriteProgress{1,false};},
      [](auto) {return true;},[&] {return now;},[] {return false;},observation);
  expect(observation.calls==4 && observation.bytes==4,"normal partial writes did not complete");
  observation={};
  throws([&] {send_websocket_bytes("x",now,{},[](auto) {return WebSocketWriteProgress{1,false};},
    [](auto) {return true;},[&] {return now;},[] {return false;},observation);});
  expect(observation.calls==0,"expired frame reached the backend");
}
void test_blocking_backend_is_interrupted_and_cancel_can_join() {
  for (bool cancel : {false,true}) {
    std::mutex mutex; std::condition_variable wake;
    bool interrupted=false, entered=false;
    WebSocketSendDeadline deadline([&] {
      {std::lock_guard lock(mutex);interrupted=true;} wake.notify_all();
    });
    std::stop_source stop;
    deadline.arm(Clock::now()+(cancel?5s:20ms));
    std::stop_callback cancellation(stop.get_token(),[&] {deadline.interrupt();});
    std::thread cancelling;
    if(cancel) cancelling=std::thread([&] {
      std::unique_lock lock(mutex);wake.wait(lock,[&]{return entered;});lock.unlock();stop.request_stop();
    });
    WebSocketWriteObservation observation;
    const auto started=Clock::now();
    throws([&] {send_websocket_bytes("frame",started+(cancel?5s:20ms),stop.get_token(),
      [&](auto bytes) {
        std::unique_lock lock(mutex);entered=true;wake.notify_all();
        if(!wake.wait_for(lock,1s,[&]{return interrupted;})) throw std::runtime_error("fixture interrupt did not arrive");
        return WebSocketWriteProgress{bytes.size(),false};
      },[](auto){return true;},[]{return Clock::now();},[&]{return deadline.interrupted();},observation);});
    if(cancelling.joinable())cancelling.join();
    expect(interrupted && deadline.interrupted() && Clock::now()-started<1s,
      "blocked TLS-like call or cancellation failed to return within the fixture bound");
    expect(!deadline.finish(),"interrupted stream became reusable");
    throws([&]{deadline.arm(Clock::now()+20ms);});
  }
}
void test_completed_operation_disarms_old_deadlines() {
  std::atomic<int> interrupts{0};
  {
    WebSocketSendDeadline deadline([&]{++interrupts;});
    // This checks timer ownership, not the20ms transport SLA tested above.
    // Wait past the retired timer, with a distinct live deadline that survives
    // ordinary runner preemption; never mistake an expired new timer for an
    // old timer firing on its replacement.
    const auto old_deadline=Clock::now()+1s;
    deadline.arm(old_deadline);expect(deadline.finish(),"normal operation did not finish");
    deadline.arm(Clock::now()+5s);
    std::mutex mutex;std::condition_variable wake;std::unique_lock lock(mutex);
    wake.wait_until(lock,old_deadline+10ms,[]{return false;});
    expect(interrupts==0,"old timer interrupted a newer operation");
    expect(deadline.finish(),"new operation lost its own deadline");
  }
  expect(interrupts==0,"observer survived retirement and touched replacement ownership");
}
void test_original_descriptor_reuse_cannot_redirect_interruption() {
#ifndef _WIN32
  int original[2],replacement[2];
  expect(::socketpair(AF_UNIX,SOCK_STREAM,0,original)==0,"original socketpair failed");
  const auto stale=original[0];
  auto interrupt=websocket_socket_interrupter(stale);
  ::close(original[0]);
  expect(::socketpair(AF_UNIX,SOCK_STREAM,0,replacement)==0,"replacement socketpair failed");
  expect(replacement[0]==stale,"fixture did not force descriptor reuse");
  interrupt();
  char byte=0;
  expect(::recv(original[1],&byte,1,MSG_DONTWAIT)==0,"pinned original endpoint was not interrupted");
  expect(::send(replacement[0],"x",1,0)==1 && ::recv(replacement[1],&byte,1,0)==1 && byte=='x',
    "deadline shut down a replacement connection using the original descriptor number");
  ::close(original[1]);::close(replacement[0]);::close(replacement[1]);
#endif
}
int tls_client(int argc,char** argv) {
  expect(argc==4,"TLS fixture needs URL and temporary CA");
  WebSocketClient client(2s,{},argv[3]);client.connect(argv[2]);
  if(std::string_view(argv[1])=="--tls-client-drain") {
    for(int index=0;index<8;++index)
      client.send_json({{"probe",std::string(16*1024,'x')},{"seq",index}},2s);
    const auto reply=client.receive_json(2s);
    expect(reply.status==WebSocketReceiveStatus::Message && reply.message.value("received",0)==8,
        "TLS retry corrupted, duplicated or lost a record");
    const auto started=Clock::now();client.close();
    const auto close_ms=std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now()-started).count();
    expect(close_ms<2000,"healthy TLS owner cleanup exceeded isolated bound");
    std::cout<<Json{{"received",8},{"close_ms",close_ms}}.dump()<<std::endl;
    return 0;
  }
  const bool cancel=std::string_view(argv[1])=="--tls-client-cancel";
  std::stop_source stop;
  WebSocketSendDeadline cancellation([&]{stop.request_stop();});
  const Json payload={{"probe",std::string(16*1024,'x')}};
  bool failed=false; std::int64_t failed_ms=0;
  for(int index=0;index<4096;++index) {
    const auto started=Clock::now();
    if(cancel)cancellation.arm(started+20ms);
    try { client.send_json(payload,cancel?5000ms:20ms,stop.get_token()); }
    catch(const std::exception&) {
      failed=true;failed_ms=std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now()-started).count();break;
    }
    if(cancel)cancellation.disarm();
  }
  cancellation.disarm();
  const auto diagnostics=client.last_send_diagnostics();
  const auto transport=client.transport_diagnostics();
  expect(failed && !client.connected(),"no-read TLS stream remained writable/reusable indefinitely");
  expect(diagnostics.value("curl_send_calls",0)>0,"test expired only during frame preparation");
  expect(!cancel || diagnostics.value("cancel_requested",false),"TLS fixture failed without observing cancellation");
#ifdef _WIN32
  expect(transport.value("curl_tls_backend",std::string{}).starts_with("Schannel"),"Windows TLS fixture did not exercise Schannel");
  expect(diagnostics.value("progress_callback_calls",0)>0,"Schannel blocked send did not poll its caller deadline");
#endif
  throws([&]{client.send_json({{"estop",true}},20ms);});
  const auto close_started=Clock::now();client.close();
  const auto close_ms=std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now()-close_started).count();
  // Retain phase evidence even when the backend/cleanup bound fails on CI.
  std::cout<<Json{{"failed_send_ms",failed_ms},{"close_ms",close_ms},{"send",diagnostics},{"transport",transport}}.dump()<<std::endl;
  expect(failed_ms<2000 && close_ms<2000,"blocked TLS send or owner cleanup exceeded isolated bound");
  return 0;
}
}
int main(int argc,char** argv) {
  try {
    if(argc>1 && (std::string_view(argv[1])=="--tls-client" ||
        std::string_view(argv[1])=="--tls-client-cancel" ||
        std::string_view(argv[1])=="--tls-client-drain"))return tls_client(argc,argv);
    test_all_progress_paths_obey_one_deadline();
    test_blocking_backend_is_interrupted_and_cancel_can_join();
    test_completed_operation_disarms_old_deadlines();
    test_original_descriptor_reuse_cannot_redirect_interruption();
    std::cout<<"4 websocket send deadline/ownership groups passed\n";return 0;
  }catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}
}
