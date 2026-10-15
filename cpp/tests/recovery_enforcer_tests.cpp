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
  std::atomic<std::uint64_t> bad_sample_revision{0},brake_sample_revision{0},fresh_frames{0};
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
}
int main(){try{test_receive_wait_cause_survives_healthy_flags_and_optional_camera_faults();
  std::cout<<"actual recovery enforcer: three receive causes and real critical-camera contrasts passed\n";return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
