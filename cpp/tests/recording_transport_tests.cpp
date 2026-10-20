#include "mine_teleop/recording.hpp"
#include <algorithm>
#include <array>
#include <chrono>
#include <cstring>
#include <fcntl.h>
#include <iostream>
#include <thread>
#include <unistd.h>
using namespace mine_teleop;
using namespace mine_teleop::detail;
namespace {
void check(bool value, const char* reason) { if (!value) throw std::runtime_error(reason); }
void exact(int fd, void* out, std::size_t bytes) {
  auto* p=static_cast<char*>(out);
  while(bytes) { const auto n=::read(fd,p,bytes); if(n<=0) throw std::runtime_error("mock IPC EOF"); p+=n;bytes-=static_cast<std::size_t>(n); }
}
void status(int fd,const Json& value) {
  const auto line=value.dump()+"\n"; std::size_t sent=0;
  while(sent<line.size()) { auto n=::write(fd,line.data()+sent,line.size()-sent); if(n<=0) throw std::runtime_error("mock status failed"); sent+=static_cast<std::size_t>(n); }
}
int mock_worker() {
  RecordingWireHeader header; exact(3,&header,sizeof(header));
  check(header.type==RecordingPacketType::Start && header.bytes<=kRecordingConfigByteLimit,"startup framing");
  std::string text(header.bytes,'\0');exact(3,text.data(),text.size());const auto config=Json::parse(text);
  const auto attempt=config.at("recording_attempt_id").get<std::string>();
  if(attempt=="stall") { std::this_thread::sleep_for(std::chrono::seconds(30)); return 0; }
  if(attempt=="dead") return 1;
  std::vector<std::uint64_t> counts(config.at("cameras").size(),0);std::size_t ended=0;
  while(ended<counts.size()) {
    exact(3,&header,sizeof(header));check(header.lane<counts.size(),"lane bound");
    if(header.type==RecordingPacketType::AccessUnit) {
      std::vector<std::uint8_t> bytes(header.bytes);exact(3,bytes.data(),bytes.size());
      check(bytes.size()==4096 && std::all_of(bytes.begin(),bytes.end(),[](auto v){return v==0xa5;}),"actual complete AU transfer");++counts[header.lane];
    } else {check(header.type==RecordingPacketType::End && header.bytes==0 && header.pts==counts[header.lane],"actual EOS covers admitted count");++ended;}
  }
  if(attempt=="malformed") {status(4,{{"recording_attempt_id",attempt},{"state",Json::array()}});return 1;}
  if(attempt=="unknown") {status(4,{{"recording_attempt_id",attempt},{"camera_id","alien"},{"recording",Json::object()}});return 1;}
  if(attempt=="unconfirmed") return 0;
  for(std::size_t i=0;i<counts.size();++i) {
    Json record={{"schema_version",1},{"configured_enabled",true},{"active",false},{"state",attempt=="eos_stall"?"finalizing":"finalized"},{"incomplete",false},{"issue_code",""},{"next_action","attempt done"},{"recording_attempt_id",attempt},{"status_revision",std::uint64_t{3}},{"accepted_access_units",counts[i]},{"worker_input_access_units",counts[i]},{"finalized_segments",std::uint64_t{attempt=="eos_stall"?0U:1U}},{"unfinished_segments",std::uint64_t{attempt=="eos_stall"?1U:0U}},{"final_eos_received",attempt!="eos_stall"},{"cleanup_pending",false},{"summary_persisted",true},{"summary_revision",std::uint64_t{3}},{"reported_at_utc_ms",std::int64_t{123456}}};
    status(4,{{"recording_attempt_id",attempt},{"camera_id",config["cameras"][i]["camera_id"]},{"recording",record}});
  }
  if(attempt=="eos_stall"){status(4,{{"recording_attempt_id",attempt},{"state","finalizing"}});std::this_thread::sleep_for(std::chrono::seconds(30));return 0;}
  // 65th complete message must survive the bounded 64-message pump plus EOF.
  if(attempt=="sixtyfive")for(int i=0;i<63;++i)status(4,{{"recording_attempt_id",attempt},{"state","recording"}});
  status(4,{{"recording_attempt_id",attempt},{"state","finalized"}});return 0;
}
int fd_count() {int count=0;for(int fd=0;fd<1024;++fd)if(::fcntl(fd,F_GETFD)>=0)++count;return count;}
RecordingSessionSettings settings(const std::filesystem::path& self,const std::string& mode,int lanes=1) {
  RecordingSessionSettings s;s.vehicle_id="test";s.session_id="local";s.attempt_id=mode;s.root="/tmp/offline-not-used-by-mock";s.worker_executable=self;
  for(int i=0;i<lanes;++i)s.lanes.push_back({"cam"+std::to_string(i),"h264","mock",20,1});return s;
}
void transfer(const std::filesystem::path& self,const std::string& mode,int lane_count=1,bool inherited=false) {
  auto s=settings(self,mode,lane_count);s.incomplete_before=inherited;IsolatedRecordingSession recorder(s);
  std::array<std::uint8_t,4096> payload;payload.fill(0xa5);
  for(int i=0;i<lane_count;++i){recorder.admit_bytes(static_cast<std::size_t>(i),{},payload);recorder.end_lane(static_cast<std::size_t>(i));}
  recorder.finish();
  for(int i=0;i<lane_count;++i) {
    const auto value=recorder.lane_status(static_cast<std::size_t>(i));
    if(mode=="malformed"||mode=="unknown"||mode=="dead"||mode=="unconfirmed") {
      check(value["incomplete"]==true && value["active"]==false,"fault remains record-only inactive/incomplete");
      if(mode=="unconfirmed")check(value["issue_code"]=="recording_status_unconfirmed","missing terminal distinguished");
    } else {check(value["state"]=="finalized" && value["final_eos_received"]==true,"terminal completion proof");check(value["accepted_access_units"]==1 && value["worker_input_access_units"]==1,"exact accepted coverage");check(value["reported_at_utc_ms"]==123456,"worker progress time preserved");check(value["incomplete"]==inherited,"same SID sticky incomplete preserved");}
  }
}
void eos_stall(const std::filesystem::path& self) {
  const auto before=fd_count();
  const auto start=std::chrono::steady_clock::now();
  Json observed;
  {
    IsolatedRecordingSession recorder(settings(self,"eos_stall"));
    std::array<std::uint8_t,4096> bytes;bytes.fill(0xa5);
    recorder.admit_bytes(0,{},bytes);recorder.end_lane(0);recorder.finish();
    const auto elapsed=std::chrono::steady_clock::now()-start;
    const auto value=recorder.lane_status(0);
    check(elapsed>=std::chrono::milliseconds(7900)&&elapsed<std::chrono::seconds(9),"actual EOS/drain stops within independent 8s parent budget");
    check(value["accepted_access_units"]==1&&value["worker_input_access_units"]==1,"stalled worker actually received complete AU and matching End");
    check(value["state"]=="failed"&&value["issue_code"]=="recording_finalize_timeout"&&value["incomplete"]==true&&value["final_eos_received"]==false&&value["cleanup_pending"]==false,"EOS stall cannot be finalized or leave child pending");
    check(value["summary_persisted"]==false&&value["summary_revision"]==3&&value["status_revision"]==4,
          "parent timeout must not reuse the previous worker snapshot's persistence proof");
    observed={{"event","recording_eos_stall_observed"},{"elapsed_ms",std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count()},{"issue_code",value["issue_code"]},{"final_eos_received",value["final_eos_received"]},{"cleanup_pending",value["cleanup_pending"]},{"fd_before",before}};
  }
  check(fd_count()==before,"EOS-timeout resource cleanup");
  observed["fd_after"]=fd_count();std::cout<<observed.dump()<<'\n';
  std::cout<<"PASS actual exact-AU/End stall reaches 8s finalize deadline and reaps child\n";
}

void ring_tests() {
  RecordingAuRing ring(2,7);std::array<std::uint8_t,4> a{1,2,3,4};std::array<std::uint8_t,3>b{5,6,7};
  check(ring.push({},a)&&ring.push({},b)&&!ring.push({},b),"slots bound rejects whole AU");ring.pop();std::array<std::uint8_t,5> large{};check(!ring.push({},large),"bytes bound independent of slots");ring.pop();
  check(ring.push({},a),"wrap admitted");auto v=ring.front();std::vector<std::uint8_t> got(v->first.begin(),v->first.end());got.insert(got.end(),v->second.begin(),v->second.end());check(std::equal(got.begin(),got.end(),a.begin()),"wrap preserves whole payload");ring.pop();
  RecordingAuRing stress(7,4096);std::atomic<bool> failed{false};constexpr std::uint64_t count=100000;
  std::jthread consumer([&]{for(std::uint64_t i=0;i<count;++i){std::optional<RecordingAuRing::ReadView> view;while(!(view=stress.front()))std::this_thread::yield();if(view->header->pts!=i||view->first[0]!=static_cast<std::uint8_t>(i))failed=true;stress.pop();}});
  for(std::uint64_t i=0;i<count;++i){std::array<std::uint8_t,31> bytes{};bytes.fill(static_cast<std::uint8_t>(i));RecordingWireHeader h;h.pts=i;while(!stress.push(h,bytes))std::this_thread::yield();check(stress.queued()<=7,"queued diagnostic bounded");}consumer.join();check(!failed,"concurrent payload/header order");
}
}
int main(int argc,char** argv) {
  try {
    if(argc==5)return mock_worker();
    const auto self=std::filesystem::absolute(argv[0]);ring_tests();std::cout<<"PASS ring whole-AU/slots/bytes/wrap/concurrency\n";
    transfer(self,"normal");transfer(self,"sixtyfive");transfer(self,"normal",16);transfer(self,"normal",1,true);std::cout<<"PASS parent exact transfer/EOS/65th terminal/16 lanes/sticky failure\n";
    for(const auto* mode:{"malformed","unknown","dead","unconfirmed"})transfer(self,mode);std::cout<<"PASS malformed/unknown/dead/unconfirmed record-only failure\n";
    const auto before=fd_count();for(int i=0;i<20;++i){auto s=settings("/nonexistent/recorder","missing");IsolatedRecordingSession recorder(s);recorder.finish();check(recorder.lane_status(0)["incomplete"]==true,"spawn failure status readable");}check(fd_count()==before,"repeated spawn failures leak no FDs");
    {IsolatedRecordingSession recorder(settings(self,"stall"));std::array<std::uint8_t,4096> bytes{};bytes.fill(0xa5);const auto begin=std::chrono::steady_clock::now();for(int i=0;i<500;++i)recorder.admit_bytes(0,{},bytes);check(std::chrono::steady_clock::now()-begin<std::chrono::seconds(1),"stalled worker cannot wait in admission");recorder.finish();check(recorder.lane_status(0)["incomplete"]==true,"overflow explicit incomplete");check(std::chrono::steady_clock::now()-begin<std::chrono::seconds(2),"overflow cleanup bounded");}
    eos_stall(self);for(int i=0;i<10;++i)transfer(self,"normal");check(fd_count()==before,"repeated reconnect no FD leaks");std::cout<<"PASS bounded stalled IPC/20 spawn failures/10 reconnects\n";return 0;
  }catch(const std::exception& e){std::cerr<<"FAIL "<<e.what()<<'\n';return 1;}
}
