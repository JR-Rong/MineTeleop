#pragma once
// Bench-only policy/model. No device code executes in the portable fake suite.
#include "mine_teleop/core.hpp"
#include <array>
#include <algorithm>
#include <cstdint>
#include <cerrno>
#include <memory>
#include <stdexcept>
#include <string>
namespace mine_teleop::bench {
using Json = mine_teleop::Json;
enum class Operation { GetFormat, GetParameters, StreamOff, ReleaseBuffers, SetFormat, SetParameters };
class DeviceOps {
 public:
  virtual ~DeviceOps() = default;
  virtual int open(const std::string& path, bool read_only = false) = 0;
  virtual int close(int fd) noexcept = 0;
  virtual Json identity(int fd, const std::string& path) = 0;
  virtual Json occupancy(const std::array<Json,2>& identities, int allowed_child = -1) = 0;
  virtual Json call(int fd, Operation operation, const Json& request = Json::object()) = 0;
};
inline bool fraction_equal(const Json& a,const Json& b) {
  for(const auto* key:{"numerator","denominator"}) if(!a.contains(key)||!b.contains(key)||!a[key].is_number_unsigned()||!b[key].is_number_unsigned())return false;
  const auto an=a["numerator"].get<std::uint64_t>(),ad=a["denominator"].get<std::uint64_t>(),bn=b["numerator"].get<std::uint64_t>(),bd=b["denominator"].get<std::uint64_t>();
  return an>0&&ad>0&&bn>0&&bd>0&&an<=UINT32_MAX&&ad<=UINT32_MAX&&bn<=UINT32_MAX&&bd<=UINT32_MAX&&an*bd==bn*ad;
}
inline Json compare_state(const Json& expected_format,const Json& actual_format,const Json& expected_parm,const Json& actual_parm) {
  Json differences=Json::array();
  for(const auto* field:{"width","height","pixelformat","field","bytesperline","sizeimage","colorspace","priv","flags","ycbcr_enc","quantization","xfer_func"})
    if(!expected_format.contains(field)||!actual_format.contains(field)||expected_format[field]!=actual_format[field])differences.push_back(std::string("format.")+field);
  for(const auto* field:{"capability","capturemode","readbuffers","extendedmode"})
    if(!expected_parm.contains(field)||!actual_parm.contains(field)||expected_parm[field]!=actual_parm[field])differences.push_back(std::string("parameters.")+field);
  if(!expected_parm.contains("timeperframe")||!actual_parm.contains("timeperframe")||!fraction_equal(expected_parm["timeperframe"],actual_parm["timeperframe"]))differences.push_back("parameters.timeperframe");
  return {{"match",differences.empty()},{"differences",differences}};
}
class CaptureContainment {
 public:
  explicit CaptureContainment(DeviceOps& ops):ops_(ops){}
  ~CaptureContainment(){for(auto fd:fds_)if(fd>=0)ops_.close(fd);}
  CaptureContainment(const CaptureContainment&)=delete;
  void snapshot(const std::array<std::string,2>& paths) {
    for(std::size_t i=0;i<2;++i){paths_[i]=paths[i];fds_[i]=ops_.open(paths[i]);if(fds_[i]<0)throw std::runtime_error("bench device open failed");identities_[i]=ops_.identity(fds_[i],paths[i]);formats_[i]=ops_.call(fds_[i],Operation::GetFormat);parameters_[i]=ops_.call(fds_[i],Operation::GetParameters);if(formats_[i].value("rc",-1)!=0||parameters_[i].value("rc",-1)!=0)throw std::runtime_error("both G_FMT/G_PARM baselines are required before any mutation");if(!parameters_[i].at("fields").contains("timeperframe")||!fraction_equal(parameters_[i]["fields"]["timeperframe"],parameters_[i]["fields"]["timeperframe"]))throw std::runtime_error("baseline timeperframe invalid; no mutation allowed");}
    if(identities_[0].at("major_minor")==identities_[1].at("major_minor"))throw std::runtime_error("bench aliases must resolve to two distinct device nodes");
    check_occupancy();prepared_=true;
  }
  Json baselines() const {Json lanes=Json::array();for(std::size_t i=0;i<2;++i)lanes.push_back({{"device",paths_[i]},{"identity",identities_[i]},{"G_FMT",formats_[i]},{"G_PARM",parameters_[i]}});return {{"scope","same_open_file_description"},{"lanes",lanes}};}
  void permit_mutation_after_durable_snapshot(){if(!prepared_)throw std::runtime_error("missing two-device baselines");for(std::size_t i=0;i<2;++i)if(ops_.identity(fds_[i],paths_[i])!=identities_[i])throw std::runtime_error("device identity changed before mutation");check_occupancy();permitted_=true;}
  int fd(std::size_t lane)const {if(!permitted_||lane>=2)throw std::runtime_error("mutation not permitted");return fds_[lane];}
  Json occupancy(int child=-1){return ops_.occupancy(identities_,child);}
  Json restore(bool sources_released) {
    Json result={{"sources_released",sources_released},{"success",false},{"lanes",Json::array()}};
    if(!permitted_){result["reason"]="mutation_not_permitted";return result;}
    if(!sources_released){result["reason"]="child_or_source_cleanup_unconfirmed";close_all();return result;}
    const auto occupied=ops_.occupancy(identities_);result["occupancy"]=occupied;
    if(!occupied.value("known",false)||!occupied.at("foreign_owners").empty()){result["reason"]="new_or_unknown_device_owner_no_restore_writes";close_all();return result;}
    bool success=true;
    for(std::size_t i=0;i<2;++i){Json lane={{"device",paths_[i]},{"success",false},{"scope","same_open_file_description"}};
      try{
        const auto owners_now=ops_.occupancy(identities_);lane["occupancy_before_cleanup"]=owners_now;
        if(!owners_now.value("known",false)||!owners_now.at("foreign_owners").empty())throw std::runtime_error("ownership_changed_no_restore_writes");
        if(ops_.identity(fds_[i],paths_[i])!=identities_[i])throw std::runtime_error("identity_changed_no_restore_writes");
        lane["observed_before_cleanup_G_FMT"]=ops_.call(fds_[i],Operation::GetFormat);lane["observed_before_cleanup_G_PARM"]=ops_.call(fds_[i],Operation::GetParameters);
        lane["STREAMOFF"]=ops_.call(fds_[i],Operation::StreamOff);lane["REQBUFS_zero"]=ops_.call(fds_[i],Operation::ReleaseBuffers);
        // A driver may reject STREAMOFF when the source already stopped. Only
        // zero-buffer release and subsequent real S/G results justify proceeding.
        if(lane["STREAMOFF"].value("rc",-1)!=0&&lane["STREAMOFF"].value("errno",0)!=EINVAL)throw std::runtime_error("streamoff_failed_no_restore_writes");
        if(lane["REQBUFS_zero"].value("rc",-1)!=0)throw std::runtime_error("buffer_release_failed_no_restore_writes");
        if(lane["REQBUFS_zero"].contains("fields")&&lane["REQBUFS_zero"]["fields"].contains("returned_count")&&lane["REQBUFS_zero"]["fields"]["returned_count"]!=0)throw std::runtime_error("buffer_release_normalized_nonzero_no_restore_writes");
        const auto owners_before_set=ops_.occupancy(identities_);if(!owners_before_set.value("known",false)||!owners_before_set.at("foreign_owners").empty()||ops_.identity(fds_[i],paths_[i])!=identities_[i])throw std::runtime_error("identity_or_ownership_changed_before_restore_writes");
        lane["S_FMT"]=ops_.call(fds_[i],Operation::SetFormat,formats_[i]);
        lane["S_PARM"]=ops_.call(fds_[i],Operation::SetParameters,parameters_[i]);
        lane["G_FMT"]=ops_.call(fds_[i],Operation::GetFormat);lane["G_PARM"]=ops_.call(fds_[i],Operation::GetParameters);
        bool matched=lane["S_FMT"].value("rc",-1)==0&&lane["S_PARM"].value("rc",-1)==0&&lane["G_FMT"].value("rc",-1)==0&&lane["G_PARM"].value("rc",-1)==0;
        if(matched){lane["comparison"]=compare_state(formats_[i].at("fields"),lane["G_FMT"].at("fields"),parameters_[i].at("fields"),lane["G_PARM"].at("fields"));matched=lane["comparison"].at("match").get<bool>();}
        const auto close_result=ops_.close(fds_[i]);const auto close_errno=close_result<0?errno:0;fds_[i]=-1;lane["guard_fd_close_errno"]=close_errno;lane["guard_fd_close_rc"]=close_result;matched=matched&&close_result==0;
        // Reopen only for public readback after the original owned handle closes.
        const auto fresh=ops_.open(paths_[i],true);if(fresh<0)throw std::runtime_error("new_fd_verify_open_failed");
        try{if(ops_.identity(fresh,paths_[i])!=identities_[i])throw std::runtime_error("new_fd_identity_changed");lane["new_fd_G_FMT"]=ops_.call(fresh,Operation::GetFormat);lane["new_fd_G_PARM"]=ops_.call(fresh,Operation::GetParameters);if(lane["new_fd_G_FMT"].value("rc",-1)!=0||lane["new_fd_G_PARM"].value("rc",-1)!=0)matched=false;else{lane["new_fd_comparison"]=compare_state(formats_[i].at("fields"),lane["new_fd_G_FMT"].at("fields"),parameters_[i].at("fields"),lane["new_fd_G_PARM"].at("fields"));matched=matched&&lane["new_fd_comparison"].at("match").get<bool>();}const auto close_rc=ops_.close(fresh);const auto close_errno=close_rc<0?errno:0;lane["new_readonly_fd_close_errno"]=close_errno;lane["new_readonly_fd_close_rc"]=close_rc;matched=matched&&close_rc==0;}catch(...){ops_.close(fresh);throw;}
        lane["success"]=matched;
      }catch(const std::exception& error){lane["error"]=error.what();}
      if(fds_[i]>=0){const auto close_result=ops_.close(fds_[i]);const auto close_errno=close_result<0?errno:0;fds_[i]=-1;lane["guard_fd_close_errno"]=close_errno;lane["guard_fd_close_rc"]=close_result;lane["success"]=lane["success"].get<bool>()&&close_result==0;}
      success=success&&lane["success"].get<bool>();result["lanes"].push_back(lane);
    }
    result["success"]=success;return result;
  }
 private:
  void close_all()noexcept{for(auto& fd:fds_)if(fd>=0){ops_.close(fd);fd=-1;}}
  void check_occupancy(){const auto state=ops_.occupancy(identities_);if(!state.value("known",false)||!state.at("foreign_owners").empty())throw std::runtime_error("selected devices occupied or occupancy cannot be established");}
  DeviceOps& ops_;std::array<int,2>fds_{-1,-1};std::array<std::string,2>paths_{};std::array<Json,2>identities_,formats_,parameters_;bool prepared_{false},permitted_{false};
};
// Bench-only exception safety: lifecycle remains owned by the outer supervisor.
// An ordinary throw after dispatch still reaps before any restoration writes.
// OOM/SIGKILL/kernel failure cannot be promised by a C++ scope guard.
template <typename Lifecycle, typename RecordFallback> class ScopedRestore {
 public:
  ScopedRestore(CaptureContainment& guard,Lifecycle& child,RecordFallback record):guard_(guard),child_(child),record_(std::move(record)){}
  ~ScopedRestore()noexcept{if(!attempted_&&child_.pid>0){try{const auto result=finish();record_(result);}catch(...){}}}
  Json finish(){attempted_=true;if(child_.pid<=0)return {{"success",false},{"reason","no_child_mutation_dispatched"}};if(!child_.reaped)child_.terminate();return guard_.restore(child_.reaped);}
 private:CaptureContainment& guard_;Lifecycle& child_;RecordFallback record_;bool attempted_{false};
};

} // namespace mine_teleop::bench
