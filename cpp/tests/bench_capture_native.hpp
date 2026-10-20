#pragma once
// Linux bench-only actual ioctl adapter and durable journal; not production policy.
#include "bench_capture_containment.hpp"
#include <linux/videodev2.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <sys/file.h>
#include <fcntl.h>
#include <unistd.h>
#include <cerrno>
#include <cstring>
#include <filesystem>
#include <chrono>
#include <set>
namespace mine_teleop::bench {
inline std::int64_t utc_ms(){return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();}
inline std::int64_t monotonic_ms(){return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();}
inline std::string hex(const void* ptr,std::size_t bytes){static constexpr char digits[]="0123456789abcdef";std::string result;result.reserve(bytes*2);const auto* p=static_cast<const unsigned char*>(ptr);for(std::size_t i=0;i<bytes;++i){result.push_back(digits[p[i]>>4]);result.push_back(digits[p[i]&15]);}return result;}
inline void unhex(const std::string& text,void* ptr,std::size_t bytes){if(text.size()!=bytes*2)throw std::runtime_error("invalid ioctl snapshot ABI size");auto* p=static_cast<unsigned char*>(ptr);auto digit=[](char c){if(c>='0'&&c<='9')return c-'0';if(c>='a'&&c<='f')return c-'a'+10;throw std::runtime_error("invalid snapshot hex");};for(std::size_t i=0;i<bytes;++i)p[i]=static_cast<unsigned char>(digit(text[i*2])*16+digit(text[i*2+1]));}
class NativeDeviceOps final:public DeviceOps {
 public:
  int open(const std::string& path,bool read_only=false)override{const auto fd=::open(path.c_str(),(read_only?O_RDONLY:O_RDWR)|O_NONBLOCK|O_CLOEXEC);if(fd<0)return fd;if(::flock(fd,LOCK_EX|LOCK_NB)<0){::close(fd);return -1;}return fd;}
  int close(int fd)noexcept override{return ::close(fd);}
  Json identity(int fd,const std::string& path)override {
    struct stat held{},node{},sys{};if(::fstat(fd,&held)<0||!S_ISCHR(held.st_mode)||::stat(path.c_str(),&node)<0||held.st_rdev!=node.st_rdev||held.st_dev!=node.st_dev||held.st_ino!=node.st_ino)throw std::runtime_error("selected node identity changed or is not a character device");
    const auto resolved=std::filesystem::canonical(path);const auto number=std::to_string(::major(held.st_rdev))+":"+std::to_string(::minor(held.st_rdev));const auto sysfs=std::filesystem::canonical("/sys/dev/char/"+number);if(::stat(sysfs.c_str(),&sys)<0)throw std::runtime_error("sysfs identity unavailable");
    v4l2_capability capability{};if(retry(fd,VIDIOC_QUERYCAP,&capability)<0)throw std::runtime_error("QUERYCAP identity failed");
    const auto caps=(capability.capabilities&V4L2_CAP_DEVICE_CAPS)?capability.device_caps:capability.capabilities;if(!(caps&V4L2_CAP_VIDEO_CAPTURE)||!(caps&V4L2_CAP_STREAMING))throw std::runtime_error("selected node is not a capture/streaming node");
    auto text=[](const auto& array){return std::string(reinterpret_cast<const char*>(array),strnlen(reinterpret_cast<const char*>(array),sizeof(array)));};
    return {{"requested_path",path},{"resolved_path",resolved.string()},{"major_minor",number},{"node_dev",held.st_dev},{"node_inode",held.st_ino},{"sysfs_path",sysfs.string()},{"sysfs_inode",sys.st_ino},{"driver",text(capability.driver)},{"card",text(capability.card)},{"bus_info",text(capability.bus_info)},{"version",capability.version},{"capabilities",capability.capabilities},{"device_caps",capability.device_caps}};
  }
  Json occupancy(const std::array<Json,2>& identities,int allowed_child=-1)override {
    Json owners=Json::array(),errors=Json::array();std::set<std::string> selected;for(const auto& id:identities)if(id.contains("major_minor"))selected.insert(id["major_minor"].get<std::string>());
    std::error_code error;std::filesystem::directory_iterator processes("/proc",error);if(error)return {{"known",false},{"foreign_owners",owners},{"error",error.message()}};
    for(const auto& process:processes){const auto name=process.path().filename().string();if(name.empty()||!std::all_of(name.begin(),name.end(),[](unsigned char c){return c>='0'&&c<='9';}))continue;int pid;try{pid=std::stoi(name);}catch(...){continue;}if(pid==::getpid()||pid==allowed_child)continue;
      std::error_code fd_error;std::filesystem::directory_iterator descriptors(process.path()/"fd",fd_error);if(fd_error){if(fd_error.value()!=ENOENT&&fd_error.value()!=ESRCH)errors.push_back({{"pid",pid},{"errno",fd_error.value()}});continue;}
      for(const auto& entry:descriptors){struct stat info{};if(::stat(entry.path().c_str(),&info)<0){if(errno!=ENOENT&&errno!=ESRCH)errors.push_back({{"pid",pid},{"errno",errno}});continue;}if(S_ISCHR(info.st_mode)){const auto number=std::to_string(::major(info.st_rdev))+":"+std::to_string(::minor(info.st_rdev));if(selected.count(number))owners.push_back({{"pid",pid},{"fd",entry.path().filename().string()},{"major_minor",number}});}}
    }
    return {{"known",errors.empty()},{"foreign_owners",owners},{"scan_errors",errors},{"scope","selected_device_fd_scan_advisory_lock_not_global_atomic_exclusion"}};
  }
  Json call(int fd,Operation operation,const Json& request=Json::object())override {
    const auto at=utc_ms(),steady=monotonic_ms();int rc=-1,err=0;Json fields,raw;
    if(operation==Operation::GetFormat||operation==Operation::SetFormat){v4l2_format value{};value.type=V4L2_BUF_TYPE_VIDEO_CAPTURE;if(operation==Operation::SetFormat)unhex(request.at("raw_hex").get<std::string>(),&value,sizeof(value));rc=retry(fd,operation==Operation::GetFormat?VIDIOC_G_FMT:VIDIOC_S_FMT,&value);err=rc<0?errno:0;const auto& p=value.fmt.pix;fields={{"width",p.width},{"height",p.height},{"pixelformat",p.pixelformat},{"field",p.field},{"bytesperline",p.bytesperline},{"sizeimage",p.sizeimage},{"colorspace",p.colorspace},{"priv",p.priv},{"flags",p.flags},{"ycbcr_enc",p.ycbcr_enc},{"quantization",p.quantization},{"xfer_func",p.xfer_func}};raw=hex(&value,sizeof(value));}
    else if(operation==Operation::GetParameters||operation==Operation::SetParameters){v4l2_streamparm value{};value.type=V4L2_BUF_TYPE_VIDEO_CAPTURE;if(operation==Operation::SetParameters)unhex(request.at("raw_hex").get<std::string>(),&value,sizeof(value));rc=retry(fd,operation==Operation::GetParameters?VIDIOC_G_PARM:VIDIOC_S_PARM,&value);err=rc<0?errno:0;const auto& p=value.parm.capture;fields={{"capability",p.capability},{"capturemode",p.capturemode},{"readbuffers",p.readbuffers},{"extendedmode",p.extendedmode},{"timeperframe",{{"numerator",p.timeperframe.numerator},{"denominator",p.timeperframe.denominator}}}};raw=hex(&value,sizeof(value));}
    else if(operation==Operation::StreamOff){auto type=V4L2_BUF_TYPE_VIDEO_CAPTURE;rc=retry(fd,VIDIOC_STREAMOFF,&type);err=rc<0?errno:0;}
    else{v4l2_requestbuffers value{};value.type=V4L2_BUF_TYPE_VIDEO_CAPTURE;value.memory=V4L2_MEMORY_MMAP;value.count=0;rc=retry(fd,VIDIOC_REQBUFS,&value);err=rc<0?errno:0;fields={{"returned_count",value.count}};}
    return {{"rc",rc},{"errno",err},{"recorded_at_utc_ms",at},{"recorded_at_monotonic_ms",steady},{"fields",fields},{"raw_hex",raw},{"uapi_scope","local_linux_video_capture_struct"}};
  }
 private:static int retry(int fd,unsigned long request,void* value){int rc;do{rc=::ioctl(fd,request,value);}while(rc<0&&errno==EINTR);return rc;}
};
class DurableJournal {
 public:
  explicit DurableJournal(const std::filesystem::path& directory){if(directory.empty()||!directory.is_absolute()||::mkdir(directory.c_str(),0700)<0)throw std::runtime_error("evidence-dir must be new absolute directory with existing parent");path_=directory/"capture-containment.jsonl";fd_=::open(path_.c_str(),O_WRONLY|O_CREAT|O_EXCL|O_CLOEXEC|O_NOFOLLOW,0600);if(fd_<0)throw std::runtime_error("cannot create private capture evidence");const auto dir=::open(directory.c_str(),O_RDONLY|O_DIRECTORY|O_CLOEXEC);if(dir<0){::close(fd_);fd_=-1;throw std::runtime_error("cannot open evidence directory for sync");}const auto rc=::fsync(dir);::close(dir);const auto parent=::open(directory.parent_path().c_str(),O_RDONLY|O_DIRECTORY|O_CLOEXEC);const auto parent_rc=parent<0 ? -1 : ::fsync(parent);if(parent>=0)::close(parent);if(rc<0||parent_rc<0){::close(fd_);fd_=-1;throw std::runtime_error("cannot persist evidence directory entry");}}
  ~DurableJournal(){if(fd_>=0)::close(fd_);}
  void record(Json value){value["supervisor_recorded_at_utc_ms"]=utc_ms();value["supervisor_recorded_at_monotonic_ms"]=monotonic_ms();const auto text=value.dump()+"\n";for(std::size_t offset=0;offset<text.size();){const auto n=::write(fd_,text.data()+offset,text.size()-offset);if(n<0&&errno==EINTR)continue;if(n<=0)throw std::runtime_error("capture evidence write failed");offset+=static_cast<std::size_t>(n);}if(::fsync(fd_)<0)throw std::runtime_error("capture evidence fsync failed");}
  const std::filesystem::path& path()const{return path_;}
  void close_in_child()const{::close(fd_);}
 private:int fd_{-1};std::filesystem::path path_;
};
} // namespace mine_teleop::bench
