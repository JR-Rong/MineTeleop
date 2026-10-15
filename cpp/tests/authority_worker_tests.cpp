// Review harness: exact frozen direct_authority_loop; injected RPC denial and mock adapter failure.
#include "mine_teleop/detail/direct_authority.hpp"
#include "mine_teleop/detail/emergency_stop.hpp"
#include "control_time_fixture.hpp"
#include <thread>
#include <mutex>
#include <iostream>
#include <cstdlib>
using namespace mine_teleop; using namespace mine_teleop::detail;
std::int64_t fixture_mono=100;
std::int64_t steady_now_ms(){return fixture_mono;}
std::string random_token(int){return "nonce";}
const std::chrono::milliseconds kNativeControlWatchdogInterval{50};
struct ThrowAdapter : VehicleAdapter {
 MockVehicleAdapter inner; inline static bool throw_stop=false;
 void open() override {inner.open();} void close() override {inner.close();}
 std::uint64_t configure_runtime_control_profile(const SessionControlProfile& p,std::uint64_t n) override{return inner.configure_runtime_control_profile(p,n);}
 void clear_runtime_control_profile() override{inner.clear_runtime_control_profile();}
 double session_motor_torque_limit_nm() const override{return inner.session_motor_torque_limit_nm();}
 double session_brake_pressure_limit_bar() const override{return inner.session_brake_pressure_limit_bar();}
 void apply_control(const ControlCommand& c) override{inner.apply_control(c);}
 void apply_safe_stop(const ControlOutput& o,VehicleStopContext c) override{if(throw_stop) throw std::runtime_error("fixture adapter safe-stop failure"); inner.apply_safe_stop(o,c);}
 bool poll_feedback() override{return inner.poll_feedback();}
 bool request_vcu_handshake() override{return inner.request_vcu_handshake();}
 bool disconnect_vcu_handshake() override{return inner.disconnect_vcu_handshake();}
 bool feedback_ready() const override{return inner.feedback_ready();}
 VcuHandshakeStatus vcu_handshake_status() const override{return inner.vcu_handshake_status();}
 VehicleTelemetry read_telemetry() override{return inner.read_telemetry();}
 VehicleAdapterStatus status() const override{return inner.status();}
};
struct FixtureSignaling {
 bool malformed=false;std::atomic<unsigned> calls{0};
 Json direct_authority_configuration(){return malformed?Json::object():Json{{"maximum_duration_ms",600}};}
 Json renew_direct_authority(std::string,int,std::uint64_t){++calls;throw HttpStatusError(401,"fixture authenticated revocation");}
 struct Clock{int uncertainty_ms=0; bool acceptable(int)const{return true;}};
 Clock time_sync_status(){return {};}
 std::int64_t now_ms(){return 0;}
};
struct Worker {
 FixtureSignaling signaling;
 struct {struct{int experimental_direct_authority_max_ms=600;} runtime;struct{int max_time_sync_uncertainty_ms=25;}field_safety;}config;
 std::atomic<bool> stop_requested{false};std::mutex control_mutex;DirectAuthorityLease direct_authority;
 bool control_service_started=true;std::unique_ptr<test::LogicalTimeControlService> control_service;
 void invalidate_native_control_trusted_gear_locked(){}
 std::atomic<bool> control_inhibited{false}; bool control_link_open=false, direct_authority_stop_failed=false;
 EmergencyStopReceiver emergency_stop_receiver; ControlTransportFence control_transport_fence;
 std::optional<std::int64_t> last_vcu_status_ms; std::string control_service_issue_code;
 unsigned diagnostics=0;
 struct GstWebRTCDataChannel{}; GstWebRTCDataChannel* control_channel=nullptr;
 static GstWebRTCDataChannel* g_object_ref(GstWebRTCDataChannel* p){return p;}
 static void g_object_unref(GstWebRTCDataChannel*){}
 static void gst_webrtc_data_channel_close(GstWebRTCDataChannel*){}
 void observe_control_hard_stop_locked(std::int64_t){}
 void send_vcu_handshake_status_locked(std::string_view){}
 void send_latest_vehicle_telemetry_locked(){}
 void emit_diagnostic(std::string_view,std::string_view,std::string_view,std::string_view,std::string_view,bool,const Json&){++diagnostics;}
 #define GST_WEBRTC_DATA_CHANNEL(x) (x)
 #include "authority_worker_methods.inc"


};
void expect(bool value,const char* message){if(!value)throw std::runtime_error(message);}
void initialize(Worker& worker){
 worker.direct_authority.configure("issuer","vehicle-001","driver-001","session-001",1,600);
 auto config=load_vehicle_config("configs/vehicle-agent.dev.yaml");auto adapter=std::make_unique<ThrowAdapter>();
 worker.control_service=std::make_unique<test::LogicalTimeControlService>(config,"driver-001","session-001","fixture",std::move(adapter));
 worker.control_service->start(0);
 SessionControlProfileRequest profile;profile.vehicle_id="vehicle-001";profile.driver_id="driver-001";profile.session_id="session-001";profile.control_token="fixture";profile.seq=1;
 expect(worker.control_service->receive_session_profile(profile,0).accepted,"profile rejected");
 ControlCommand command;command.vehicle_id="vehicle-001";command.driver_id="driver-001";command.session_id="session-001";command.control_token="fixture";command.seq=1;command.gear="D";command.throttle=.5;
 expect(worker.control_service->receive_command(command,0).accepted,"initial command rejected");
}
int main(){
 std::set_terminate([]{std::cerr<<"COUNTEREXAMPLE R2: adapter exception escaped actual authority worker; terminate exit86\n"<<std::flush;std::_Exit(86);});
 try{
  Worker worker;initialize(worker);
  // The real service remains active; swap only the injected adapter's safe-stop behavior through its shared fixture flag.
  ThrowAdapter::throw_stop=true;
  std::jthread thread([&](std::stop_token token){worker.direct_authority_loop(token);});
  const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(1);
  while(!worker.signaling.calls.load()&&std::chrono::steady_clock::now()<deadline)std::this_thread::yield();
  std::this_thread::sleep_for(std::chrono::milliseconds(10));thread.request_stop();thread.join();
  expect(worker.direct_authority.status(100).at("revoked")==true,"failed physical stop left software authority live");
  worker.tick_control_service();
  expect(!worker.control_service_started&&!worker.control_service&&worker.diagnostics>=2,"adapter failure did not enter existing diagnostic/close path");
  ThrowAdapter::throw_stop=false;Worker expiry;initialize(expiry);
  expiry.direct_authority.begin_request("expiry",0);
  DirectAuthorityGrant grant{"issuer","vehicle-001","driver-001","session-001","expiry",1,1,1,0,600,600};
  expect(expiry.direct_authority.accept_authenticated_cloud(grant.json(),0,0,0),"expiry grant rejected");
  ThrowAdapter::throw_stop=true;fixture_mono=601;
  expiry.tick_control_service();
  expect(!expiry.control_service_started&&!expiry.control_service&&expiry.diagnostics>=2,"expiry stop failure escaped or was silently swallowed");
  ThrowAdapter::throw_stop=false;fixture_mono=100;Worker fatal;initialize(fatal);fatal.signaling.malformed=true;
  std::jthread fatal_thread([&](std::stop_token token){fatal.direct_authority_loop(token);});fatal_thread.join();fatal.tick_control_service();
  expect(!fatal.control_service_started&&!fatal.control_service,"worker entry failure did not fail closed through common shutdown");
  std::cout<<"3 actual worker/tick fault groups passed; real common service, throwing adapter, fake signaling/Gst, no devices\n";
 }catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}
}
