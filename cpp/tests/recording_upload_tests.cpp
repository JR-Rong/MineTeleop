#include "mine_teleop/upload.hpp"
#include <fstream>
#include <iostream>
#include <chrono>
#include <unistd.h>
using namespace mine_teleop;
namespace {void check(bool value,const char* why){if(!value)throw std::runtime_error(why);} }
int main(){
  const auto root=std::filesystem::temp_directory_path()/("mine_teleop_upload_closed_"+std::to_string(::getpid()));
  try{std::filesystem::remove_all(root);const auto recording=root/"recording", archive=root/"archive";std::filesystem::create_directories(recording);
    auto metadata=[&](const char* filename,const Json& value){std::ofstream out(recording/filename);out<<value.dump();};
    auto video=[&](const char* filename){std::ofstream out(recording/filename);out<<"offline real bytes to verify SHA and atomic copy";};
    video("00-open.mp4");metadata("00-open.json",{{"schema_version",1},{"kind","recording_segment"},{"upload_state","pending"},{"finalized",false},{"incomplete",true},{"recording_status","open"}});
    metadata("01-summary.json",{{"schema_version",1},{"kind","recording_session_summary"}});
    video("02-closed.mp4");metadata("02-closed.json",{{"schema_version",1},{"kind","recording_segment"},{"upload_state","pending"},{"finalized",true},{"incomplete",false},{"recording_status","complete"},{"hash_state","pending"},{"segment_id","closed"}});
    video("03-legacy.mp4");metadata("03-legacy.json",{{"segment_id","legacy"}});
    LocalArchiveUploader uploader(recording,archive);check(uploader.backlog()["pending_segments"]==2,"open/summary must not be pending");const auto first=uploader.process_once();check(first.action=="uploaded"&&first.segment_id=="closed","closed advances despite prior incomplete metadata");
    std::ifstream input(archive/"02-closed.json");Json result;input>>result;check(result["hash_state"]=="verified"&&result["video_sha256"]==sha256_file(recording/"02-closed.mp4"),"deferred hash matches actual archived bytes");
    check(uploader.process_once().segment_id=="legacy","explicit legacy compatibility");check(uploader.process_once().action=="idle"&&uploader.backlog()["pending_segments"]==0,"no rearchive/incomplete pending");check(!std::filesystem::exists(archive/"00-open.mp4")&&!std::filesystem::exists(archive/"01-summary.json"),"not complete artifacts remain unarchived");
    std::filesystem::remove_all(root);std::cout<<"PASS actual uploader closed evidence/deferred SHA/open-summary guard/legacy/idempotence\n";return 0;
  }catch(const std::exception& e){std::filesystem::remove_all(root);std::cerr<<"FAIL "<<e.what()<<'\n';return 1;}
}
