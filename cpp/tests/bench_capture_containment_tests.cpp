#include "bench_capture_containment.hpp"
#include <iostream>
#include <map>
#include <set>
#include <vector>
using namespace mine_teleop::bench;
namespace {
void check(bool value,const char* why){if(!value)throw std::runtime_error(why);}
Json fmt(){Json x=Json::object();for(const auto* f:{"width","height","pixelformat","field","bytesperline","sizeimage","colorspace","priv","flags","ycbcr_enc","quantization","xfer_func"})x[f]=1U;x["width"]=1920U;x["height"]=1080U;return x;}
Json parm(){return {{"capability",1U},{"capturemode",2U},{"readbuffers",4U},{"extendedmode",5U},{"timeperframe",{{"numerator",1U},{"denominator",30U}}}};}
class Fake final:public DeviceOps {
 public:
  std::map<int,int> opened;std::array<Json,2> formats{fmt(),fmt()},parameters{parm(),parm()};std::vector<std::pair<int,Operation>> calls;std::vector<bool> read_only_opens;
  int nextfd=10,fail_snapshot=-1,fail_set=-1,fail_release=-1,changed_identity=-1,invalid_initial=-1;bool foreign=false,unknown=false,readback_mismatch=false,equivalent_fraction=false,foreign_after_first_restore=false,release_normalized=false;
  int open(const std::string& p,bool read_only=false)override{read_only_opens.push_back(read_only);const auto fd=nextfd++;opened[fd]=p=="6"?0:1;return fd;}
  int close(int fd)noexcept override{opened.erase(fd);return 0;}
  Json identity(int fd,const std::string&)override{const auto i=opened.at(fd);return {{"major_minor",std::to_string(81)+":"+std::to_string(i)},{"generation",changed_identity==i?2:1}};}
  Json occupancy(const std::array<Json,2>&,int=-1)override{return {{"known",!unknown},{"foreign_owners",foreign?Json::array({{{"pid",999}}}):Json::array()}};}
  Json call(int fd,Operation op,const Json& request=Json::object())override {
    const auto i=opened.at(fd);calls.push_back({i,op});const bool get=op==Operation::GetFormat||op==Operation::GetParameters;
    if((get&&fail_snapshot==i)||(op==Operation::SetParameters&&fail_set==i)||(op==Operation::ReleaseBuffers&&fail_release==i))return {{"rc",-1},{"errno",5}};
    if(op==Operation::SetFormat){formats[static_cast<std::size_t>(i)]=request.at("fields");parameters[static_cast<std::size_t>(i)]["timeperframe"]={{"numerator",1U},{"denominator",10U}};}
    if(op==Operation::SetParameters){parameters[static_cast<std::size_t>(i)]=request.at("fields");if(foreign_after_first_restore&&i==0)foreign=true;if(equivalent_fraction)parameters[static_cast<std::size_t>(i)]["timeperframe"]={{"numerator",2U},{"denominator",60U}};if(readback_mismatch)formats[static_cast<std::size_t>(i)]["colorspace"]=999U;}
    if(op==Operation::ReleaseBuffers)return {{"rc",0},{"errno",0},{"fields",{{"returned_count",release_normalized?4U:0U}}}};
    Json fields=op==Operation::GetFormat?formats[static_cast<std::size_t>(i)]:parameters[static_cast<std::size_t>(i)];if(op==Operation::GetParameters&&invalid_initial==i)fields["timeperframe"]["numerator"]=0U;
    return {{"rc",0},{"errno",0},{"fields",fields},{"raw_hex","fake ABI preserved"}};
  }
  int writes()const{int n=0;for(auto c:calls)if(c.second==Operation::SetFormat||c.second==Operation::SetParameters)++n;return n;}
};
void ready(CaptureContainment& g){g.snapshot({"6","7"});g.permit_mutation_after_durable_snapshot();}
}
int main(){try{
  for(int bad:{0,1}){Fake ops;ops.fail_snapshot=bad;{CaptureContainment g(ops);bool rejected=false;try{g.snapshot({"6","7"});}catch(...){rejected=true;}check(rejected&&ops.writes()==0,"one failed baseline means zero mutation on both nodes");}check(ops.opened.empty(),"snapshot exception closes both owned FDs");}
  {Fake ops;ops.invalid_initial=1;CaptureContainment g(ops);bool rejected=false;try{g.snapshot({"6","7"});}catch(...){rejected=true;}check(rejected&&ops.writes()==0,"zero/invalid frame period cannot establish restoration contract");}
  std::cout<<"PASS dual baseline failures/invalid fraction prevent all writes and close FDs\n";
  {Fake ops;CaptureContainment g(ops);ready(g);ops.formats[0]["width"]=640U;ops.parameters[0]["timeperframe"]["denominator"]=20U;ops.equivalent_fraction=true;const auto result=g.restore(true);check(result["success"]==true&&ops.opened.empty(),"same-handle restore and fresh-handle readback");check(ops.read_only_opens.size()==4&&!ops.read_only_opens[0]&&!ops.read_only_opens[1]&&ops.read_only_opens[2]&&ops.read_only_opens[3],"fresh verification opens are readonly");for(int i=0;i<2;++i){auto f=std::find(ops.calls.begin(),ops.calls.end(),std::pair{i,Operation::SetFormat});auto p=std::find(ops.calls.begin(),ops.calls.end(),std::pair{i,Operation::SetParameters});check(f<p,"S_FMT dependent timing is restored before S_PARM");}check(fraction_equal({{"numerator",1U},{"denominator",30U}},{{"numerator",2U},{"denominator",60U}}),"equivalent rational period");}
  std::cout<<"PASS mutated format/timing restore order/rational equivalence/new FD evidence\n";
  for(int mode=0;mode<4;++mode){Fake ops;CaptureContainment g(ops);ready(g);if(mode==0)ops.foreign=true;if(mode==1)ops.unknown=true;if(mode==2)ops.changed_identity=0;const auto result=g.restore(mode!=3);check(result["success"]==false,"unproven source/identity/ownership fails restore");if(mode!=2)check(ops.writes()==0,"unreleased/occupied source has no restore writes");else{for(auto c:ops.calls)check(c.first!=0||(c.second!=Operation::SetFormat&&c.second!=Operation::SetParameters),"changed node not overwritten");}check(ops.opened.empty(),"fault closes supervisor owned FDs");}
  std::cout<<"PASS live child/foreign or unknown owner/identity change block unsafe restoration\n";
  for(int mode=0;mode<3;++mode){Fake ops;CaptureContainment g(ops);ready(g);if(mode==0)ops.fail_set=0;if(mode==1)ops.fail_release=0;if(mode==2)ops.readback_mismatch=true;const auto result=g.restore(true);check(result["success"]==false&&ops.opened.empty(),"restore/cleanup/readback failure remains separate nonzero evidence");if(mode==1)for(auto c:ops.calls)check(c.first!=0||(c.second!=Operation::SetFormat&&c.second!=Operation::SetParameters),"no format writes with owned buffers unreleased");}
  std::cout<<"PASS set failure/buffer release failure/color mismatch independently fail containment\n";
  {Fake ops;CaptureContainment g(ops);ready(g);ops.release_normalized=true;check(g.restore(true)["success"]==false&&ops.writes()==0,"returned nonzero REQBUFS count cannot masquerade as release");}
  {Fake ops;CaptureContainment g(ops);ready(g);ops.foreign_after_first_restore=true;check(g.restore(true)["success"]==false,"new owner arriving between lane restores blocks next writes");for(auto c:ops.calls)check(c.first!=1||(c.second!=Operation::SetFormat&&c.second!=Operation::SetParameters),"second lane not overwritten using stale batch occupancy");}

  for(const auto* field:{"width","height","pixelformat","field","bytesperline","sizeimage","colorspace","priv","flags","ycbcr_enc","quantization","xfer_func"}){auto changed=fmt();changed[field]=9U;check(compare_state(fmt(),changed,parm(),parm())["match"]==false,"format field omitted from comparison");}
  for(const auto* field:{"capability","capturemode","readbuffers","extendedmode"}){auto changed=parm();changed[field]=9U;check(compare_state(fmt(),fmt(),parm(),changed)["match"]==false,"parm field omitted from comparison");}
  check(!fraction_equal({{"numerator",0U},{"denominator",0U}},{{"numerator",0U},{"denominator",0U}}),"unknown cadence never equals valid baseline");
  std::cout<<"PASS all public format/parameter fields and invalid cadence compared\n";
  struct FakeChild{int pid{42};bool reaped{false},stuck{false};int termination_calls{0};void terminate(){++termination_calls;if(!stuck)reaped=true;}};
  for(bool stuck:{false,true}){Fake ops;CaptureContainment g(ops);ready(g);ops.formats[0]["width"]=640U;FakeChild child;child.stuck=stuck;Json fallback;
    try{ScopedRestore finally(g,child,[&](const Json& state){fallback=state;});throw std::runtime_error("injected payload/result exception after source mutation");}catch(const std::exception&){}
    check(child.termination_calls==1,"exception finally must attempt exact child cleanup first");check(fallback["success"]==!stuck,"exception restoration requires exact reap proof");check(ops.opened.empty(),"post-dispatch exception cannot leak owned guard FDs");if(stuck)check(ops.writes()==0,"unreaped child exception never writes restore settings");else check(ops.formats[0]==fmt(),"ordinary exception restores actual modified model state");
  }
  std::cout<<"PASS post-dispatch exception finally restores only after exact child reap\n";return 0;
}catch(const std::exception& e){std::cerr<<"FAIL "<<e.what()<<'\n';return 1;}}
