#include "mine_teleop/media.hpp"
#include "mine_teleop/detail/media_freshness.hpp"
#include "control_time_fixture.hpp"
#include <atomic>
#include <iostream>

namespace {
using namespace mine_teleop;
std::int64_t now=50;
std::int64_t steady_now_ms(){return now;}
struct Freshness {
  std::atomic<std::uint64_t> bad_sample_revision{0},brake_sample_revision{0},invalid_brake_revision{0},fresh_frames{0};
  std::atomic<bool> invalid_sample_blocked{false};
  std::atomic<unsigned> consecutive_anomalies{0};
  std::atomic<std::int64_t> last_fresh_frame_ms{0},max_age_ms{500};
};
struct Lane {
  struct {bool critical_for_control{false};std::string id{"optional-camera"};} camera;
  std::unique_ptr<Freshness> send_freshness=std::make_unique<Freshness>();
  std::atomic<bool> disabled{false},camera_faulted{false};
  std::atomic<std::uint64_t> video_wait_encoded_baseline{0};
};
struct Observer {
  VehicleConfig config;
  std::mutex control_recovery_phase_mutex,control_mutex;
  bool stop_requested{false},control_inhibited{false},control_service_started{true};
  std::string control_service_issue_code;
  std::vector<std::unique_ptr<Lane>> lanes;
  std::shared_ptr<SessionControlRecoveryLatch> control_recovery_latch=std::make_shared<SessionControlRecoveryLatch>();
  VehicleControlService* control_service;
  struct {std::string session_id()const{return "session-001";}std::int64_t now_ms()const{return now;}} signaling;
  struct Time {bool ready{true},latest_candidate_healthy{true};std::uint64_t bad_cohort_revision{0},brake_cohort_revision{0};};
  Time vehicle_time_readiness(){return {};}
  std::pair<int,int> recovery_timing_configured(){return {500,5000};}
  ControlRecoveryPhase phase{ControlRecoveryPhase::Ready};
  bool vehicle_video_hold_active{false};
  std::vector<std::string> diagnostics;
  void emit_diagnostic(std::string_view event,std::string_view,std::string_view,std::string_view,std::string_view,bool,Json){diagnostics.emplace_back(event);}
  void apply_control_recovery_phase(ControlRecoveryPhase value,const Lane*,bool){phase=value;}
#include "recovery_enforcer.inc"
};
void expect(bool condition,const char* message){if(!condition)throw std::runtime_error(message);}
void test_receive_wait_cause_survives_healthy_flags_and_optional_camera_faults() {
  for(auto cause:{ControlRecoveryCause::ControlLink,ControlRecoveryCause::InputLease,ControlRecoveryCause::ReadinessHistory}) {
    auto config=load_vehicle_config("configs/vehicle-agent.dev.yaml");
    test::LogicalTimeControlService service(config,"driver-001","session-001","token",std::make_unique<MockVehicleAdapter>());
    service.start(0);SessionControlProfileRequest profile;profile.vehicle_id="vehicle-001";profile.driver_id="driver-001";
    profile.session_id="session-001";profile.control_token="token";profile.seq=1;
    expect(service.receive_session_profile(profile,0).accepted,"profile failed");
    ControlCommand command;command.vehicle_id="vehicle-001";command.driver_id="driver-001";
    command.session_id="session-001";command.control_token="token";command.seq=1;command.gear="D";command.throttle=0.5;
    expect(service.receive_command(command,0).accepted,"initial motion failed");
    service.set_control_braking_wait(true,50,cause);
    Observer observer;observer.config=config;observer.config.runtime.control_enabled=true;
    observer.config.field_safety.require_time_sync=false;observer.control_service=&service;
    observer.lanes.push_back(std::make_unique<Lane>());observer.lanes.front()->camera_faulted=true;
    static_cast<void>(observer.control_recovery_latch->enter_session("session-001"));
    observer.enforce_control_recovery();
    expect(observer.phase==ControlRecoveryPhase::BrakingWait,"receive wait was not observed");
    expect(observer.control_recovery_latch->cause_for("session-001")==cause,
      "receive-path link/input/history wait was mislabeled as critical video");
    expect(service.control_braking_wait() && service.session_control_profile().at("active")==true,
      "diagnostic cause change altered physical brake/session");
    observer.lanes.front()->camera.critical_for_control=true;
    observer.control_recovery_latch=std::make_shared<SessionControlRecoveryLatch>();
    static_cast<void>(observer.control_recovery_latch->enter_session("session-001"));
    observer.enforce_control_recovery();
    expect(observer.control_recovery_latch->cause_for("session-001")==ControlRecoveryCause::CriticalVideo,
      "real failed critical lane lost its own cause");
    service.close();
  }
}

// A real service, latch and the unchanged extracted observer. `hold_allowed` is the
// vehicle config; `negotiated` is whether the driver's commands declare the capability.
struct Rig {
  VehicleConfig config=load_vehicle_config("configs/vehicle-agent.dev.yaml");
  std::unique_ptr<test::LogicalTimeControlService> service;
  Observer observer;
  std::uint64_t seq{0};
  Rig(bool hold_allowed,bool negotiated,bool past_startup_grace=true) {
    config.control.control_timeout_ms=1200;config.control.transient_hold.enabled=hold_allowed;
    service=std::make_unique<test::LogicalTimeControlService>(config,"driver-001","session-001","token",std::make_unique<MockVehicleAdapter>());
    service->start(0);
    SessionControlProfileRequest profile;profile.vehicle_id="vehicle-001";profile.driver_id="driver-001";
    profile.session_id="session-001";profile.control_token="token";profile.seq=1;
    expect(service->receive_session_profile(profile,0).accepted,"profile failed");
    expect(send(0,negotiated).accepted,"initial command failed");
    observer.config=config;observer.config.runtime.control_enabled=true;
    observer.config.field_safety.require_time_sync=false;observer.control_service=service.get();
    observer.lanes.push_back(std::make_unique<Lane>());
    observer.lanes.front()->camera.critical_for_control=true;observer.lanes.front()->camera.id="front";
    static_cast<void>(observer.control_recovery_latch->enter_session("session-001"));
    // The 15 s startup grace suppresses video-only waits. Start its clock, then move past it.
    static_cast<void>(observer.control_recovery_latch->startup_grace_active("session-001",0));
    if(past_startup_grace){now=20000;fresh().last_fresh_frame_ms=now;}
  }
  ReceiveResult send(std::int64_t time,bool capable,bool video_ready=true,bool transient=false) {
    ControlCommand command;command.vehicle_id="vehicle-001";command.driver_id="driver-001";command.session_id="session-001";
    command.control_token="token";command.seq=++seq;command.sent_at_utc_ms=time;command.gear="D";command.throttle=0.5;
    command.transient_hold_capable=capable;command.critical_video_ready=video_ready;command.critical_video_transient=transient;
    return service->receive_command(command,time);
  }
  Freshness& fresh(){return *observer.lanes.front()->send_freshness;}
  Json hold(std::int64_t time){return service->control_timing(time).at("transient_hold");}
  void enforce(){observer.enforce_control_recovery();}
  bool braking_wait()const{return observer.phase==ControlRecoveryPhase::BrakingWait;}
};
void hold_turns_a_vehicle_video_stall_into_a_hold_not_a_braking_wait() {
  Rig rig(true,true);
  rig.fresh().fresh_frames=10;  // Frames that arrived before the stall do not count toward recovery.
  now+=1000;  // The newest encoded frame is now 1000 ms old.
  rig.enforce();
  expect(!rig.braking_wait(),"a stale lane under a negotiated hold demanded a braking wait");
  expect(rig.service->transient_hold_active()&&!rig.service->control_braking_wait()&&rig.observer.vehicle_video_hold_active,
    "vehicle video stall did not become a hold");
  expect(rig.hold(5).at("causes").at(0)=="vehicle_video_stale","hold cause missing");
  expect(std::find(rig.observer.diagnostics.begin(),rig.observer.diagnostics.end(),"vehicle_transient_hold_entered")!=rig.observer.diagnostics.end(),
    "hold entry was not reported");
  // A fresh timestamp alone does not end the stall: three new encoded frames are required.
  rig.fresh().last_fresh_frame_ms=now;rig.enforce();
  expect(rig.observer.vehicle_video_hold_active,"stall cleared before three new encoded frames");
  rig.fresh().fresh_frames=12;rig.enforce();
  expect(rig.observer.vehicle_video_hold_active,"stall cleared after only two new encoded frames");
  rig.fresh().fresh_frames=13;rig.enforce();
  expect(!rig.observer.vehicle_video_hold_active&&rig.hold(5).at("causes").empty()&&!rig.braking_wait(),
    "stall did not clear after three new encoded frames");
}
void hold_events_are_reported_even_during_startup_grace() {
  Rig rig(true,true,false);  // Still inside the 15 s startup grace, which returns early.
  now=1000;rig.fresh().last_fresh_frame_ms=0;
  rig.enforce();
  expect(rig.service->transient_hold_active(),"hold did not start during startup grace");
  expect(std::find(rig.observer.diagnostics.begin(),rig.observer.diagnostics.end(),"vehicle_transient_hold_entered")!=rig.observer.diagnostics.end(),
    "hold entry was dropped by the startup-grace return");
}
void hold_still_brakes_for_fault_class_video_events() {
  for(const bool blocked_sample:{true,false}) {
    Rig rig(true,true);
    rig.enforce();expect(!rig.braking_wait(),"healthy lane braked");  // Establishes the revision baseline.
    if(blocked_sample)rig.fresh().invalid_sample_blocked=true;else rig.fresh().invalid_brake_revision=1;
    rig.enforce();
    expect(rig.braking_wait()&&rig.observer.control_recovery_latch->cause_for("session-001")==ControlRecoveryCause::CriticalVideo,
      "an invalid-sample fault was absorbed by the hold");
  }
  Rig disabled_camera(true,true);
  disabled_camera.observer.lanes.front()->disabled=true;disabled_camera.enforce();
  expect(disabled_camera.braking_wait(),"a disabled critical camera was absorbed by the hold");
}
void hold_ignores_stale_drop_revisions_but_legacy_still_brakes() {
  for(const bool negotiated:{true,false}) {
    Rig rig(true,negotiated);
    rig.enforce();
    rig.fresh().bad_sample_revision=4;rig.fresh().brake_sample_revision=1;  // Three consecutive stale drops between reads.
    rig.enforce();
    expect(rig.braking_wait()==!negotiated,negotiated?"a stale-drop brake revision braked despite the hold":
      "the legacy stale-drop brake revision was ignored");
  }
}
void two_dropped_pictures_are_tolerated_only_without_a_hold() {
  for(const bool negotiated:{false,true}) {
    Rig rig(true,negotiated);
    rig.fresh().consecutive_anomalies=2;rig.enforce();
    expect(!rig.braking_wait(),"two dropped pictures braked");
    expect(rig.service->transient_hold_active()==negotiated,"two dropped pictures: wrong hold state");
    rig.fresh().consecutive_anomalies=3;rig.enforce();  // The legacy braking threshold.
    expect(rig.braking_wait()==!negotiated,negotiated?"three dropped pictures braked despite the hold":"three dropped pictures did not brake");
  }
}
void stale_lane_brakes_without_a_negotiated_hold() {
  for(const bool hold_allowed:{false,true}) {
    Rig rig(hold_allowed,false);
    now+=1000;rig.enforce();
    expect(rig.braking_wait()&&!rig.service->transient_hold_active(),"legacy stale lane did not brake");
  }
}
void driver_video_transient_is_not_a_latch_fault_under_a_hold() {
  Rig rig(true,true);
  expect(rig.send(10,true,false,true).accepted,"transient video command rejected");
  rig.enforce();
  expect(!rig.braking_wait()&&!rig.service->control_braking_wait()&&rig.service->transient_hold_active(),
    "a stalled driver view braked instead of holding");
  static_cast<void>(rig.send(20,true,false,false));  // Not ready and not transient: a fault.
  rig.enforce();
  expect(rig.braking_wait(),"a driver video fault was absorbed by the hold");
}
}
int main(){try{test_receive_wait_cause_survives_healthy_flags_and_optional_camera_faults();
  hold_turns_a_vehicle_video_stall_into_a_hold_not_a_braking_wait();hold_events_are_reported_even_during_startup_grace();
  hold_still_brakes_for_fault_class_video_events();
  hold_ignores_stale_drop_revisions_but_legacy_still_brakes();two_dropped_pictures_are_tolerated_only_without_a_hold();
  stale_lane_brakes_without_a_negotiated_hold();driver_video_transient_is_not_a_latch_fault_under_a_hold();
  std::cout<<"actual recovery enforcer: receive causes, critical-camera contrasts and transient hold classification passed\n";return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
