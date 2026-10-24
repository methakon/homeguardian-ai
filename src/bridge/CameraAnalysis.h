#pragma once
#include "core/CameraActivitySession.h"
#include "core/AnomalyMonitor.h"
#include "core/DetectionFusionRule.h"
#include "core/Pipeline.h"
#include "core/AlertPolicy.h"
#include "core/PersistenceAudit.h"
#include "core/UUID.h"
#include "camera/RegistryAuthorizer.h"
#include "bridge/LiveCameraFeed.h"
#include <nlohmann/json.hpp>
#if defined(__ANDROID__)
#if !defined(__ANDROID_API__) || __ANDROID_API__ < 30
#error "Camera JPEG decoder requires Android API level 30 or newer"
#else
#include <android/imagedecoder.h>
#include <android/bitmap.h>
#endif
#else
#include <sys/socket.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <poll.h>
#include <fcntl.h>
#include <unistd.h>
#include <signal.h>
#include <cerrno>
#endif
#include <memory>
#include <cstring>
#include <mutex>

namespace homeguardian::portal {
// No shell, files, network protocols or caller-controlled command arguments. Both
// byte directions, runtime, dimensions, child resources and reap are bounded.
inline std::vector<uint8_t> decode_jpeg_luma(const std::vector<uint8_t>& jpeg) {
 int w=0,h=0;
 if(jpeg.size()>512*1024 || !detail::looks_like_jpeg(jpeg) ||
    !detail::jpeg_dimensions(jpeg,&w,&h) || w>4096 || h>4096) return {};
#if defined(__ANDROID__)
#if defined(__ANDROID_API__) && __ANDROID_API__ >= 30
 // Each call owns its decoder; no subprocesses, shared decoder or global setup.
 AImageDecoder* raw=nullptr;
 const int created=AImageDecoder_createFromBuffer(jpeg.data(),jpeg.size(),&raw);
 // On failure outDecoder is unspecified: only adopt it after success.
 if(created!=ANDROID_IMAGE_DECODER_SUCCESS) return {};
 std::unique_ptr<AImageDecoder,decltype(&AImageDecoder_delete)> decoder(raw,&AImageDecoder_delete);
 if(!decoder) return {};
 const auto* header=AImageDecoder_getHeaderInfo(decoder.get());
 if(!header) return {};
 const int32_t width=AImageDecoderHeaderInfo_getWidth(header);
 const int32_t height=AImageDecoderHeaderInfo_getHeight(header);
 const char* mime=AImageDecoderHeaderInfo_getMimeType(header);
 if(width<=0 || height<=0 || width>4096 || height>4096 ||
    !mime || std::strcmp(mime,"image/jpeg")!=0 ||
    AImageDecoderHeaderInfo_getAlphaFlags(header)!=ANDROID_BITMAP_FLAGS_ALPHA_OPAQUE) return {};
 if(AImageDecoder_setAndroidBitmapFormat(decoder.get(),ANDROID_BITMAP_FORMAT_RGBA_8888)!=ANDROID_IMAGE_DECODER_SUCCESS ||
    AImageDecoder_setTargetSize(decoder.get(),160,120)!=ANDROID_IMAGE_DECODER_SUCCESS) return {};
 constexpr size_t stride=160u*4u, pixel_count=160u*120u, rgba_size=stride*120u;
 // Never allocate from untrusted dimensions or a decoder-supplied stride.
 if(AImageDecoder_getMinimumStride(decoder.get())!=stride) return {};
 std::vector<uint8_t> rgba(rgba_size);
 if(AImageDecoder_decodeImage(decoder.get(),rgba.data(),stride,rgba.size())!=ANDROID_IMAGE_DECODER_SUCCESS) return {};
 std::vector<uint8_t> luma(pixel_count);
 for(size_t i=0;i<pixel_count;++i){
  const auto* p=rgba.data()+i*4u;
  // Full-range BT.601 luma, rounded; alpha is opaque for JPEG.
  luma[i]=static_cast<uint8_t>((299u*p[0]+587u*p[1]+114u*p[2]+500u)/1000u);
 }
 return luma;
#else
 return {}; // Unsupported Android targets also fail closed if preprocessing continues.
#endif
#else
 int in[2],out[2];
 if(socketpair(AF_UNIX,SOCK_STREAM|SOCK_CLOEXEC,0,in)) return {};
 if(pipe2(out,O_CLOEXEC)){close(in[0]);close(in[1]);return {};}
 pid_t pid=fork();
 if(pid==0){
  dup2(in[1],0);dup2(out[1],1);
  int nullfd=open("/dev/null",O_WRONLY);dup2(nullfd,2);
  close_range(3,~0U,0);
  rlimit cpu{2,2}, mem{512u*1024*1024,512u*1024*1024}, files{16,16}, core{0,0};
  setrlimit(RLIMIT_CPU,&cpu);setrlimit(RLIMIT_AS,&mem);setrlimit(RLIMIT_NOFILE,&files);setrlimit(RLIMIT_CORE,&core);
  execl("/usr/bin/ffmpeg","ffmpeg","-nostdin","-v","error","-threads","1","-protocol_whitelist","pipe","-f","mjpeg","-i","pipe:0","-frames:v","1","-vf","scale=160:120","-threads","1","-f","rawvideo","-pix_fmt","gray","pipe:1",(char*)nullptr);
  _exit(127);
 }
 close(in[1]);close(out[1]);
 if(pid<0){close(in[0]);close(out[0]);return {};}
 fcntl(in[0],F_SETFL,O_NONBLOCK);fcntl(out[0],F_SETFL,O_NONBLOCK);
 size_t sent=0;bool input_open=true, eof=false;std::vector<uint8_t> bytes;
 const auto deadline=std::chrono::steady_clock::now()+std::chrono::milliseconds(1500);
 while(std::chrono::steady_clock::now()<deadline && !eof && bytes.size()<=19200){
  pollfd fds[2]={{in[0],static_cast<short>(input_open?POLLOUT:0),0},{out[0],POLLIN,0}};
  poll(fds,2,20);
  if(input_open && fds[0].revents){
   auto n=send(in[0],jpeg.data()+sent,jpeg.size()-sent,MSG_NOSIGNAL);
   if(n>0)sent+=static_cast<size_t>(n);
   if(sent==jpeg.size() || (n<0 && errno!=EAGAIN && errno!=EINTR)){shutdown(in[0],SHUT_WR);input_open=false;}
  }
  uint8_t buf[4096];ssize_t n;
  while((n=read(out[0],buf,sizeof(buf)))>0){bytes.insert(bytes.end(),buf,buf+n);if(bytes.size()>19200)break;}
  if(n==0)eof=true;
 }
 close(in[0]);close(out[0]);int status=0;
 bool exited=false;
 while(std::chrono::steady_clock::now()<deadline){if(waitpid(pid,&status,WNOHANG)==pid){exited=true;break;}poll(nullptr,0,5);}
 if(!exited){kill(pid,SIGKILL);while(waitpid(pid,&status,0)<0 && errno==EINTR){};return {};}
 if(!eof || !WIFEXITED(status) || WEXITSTATUS(status)!=0 || bytes.size()!=19200)return {};
 return bytes;
#endif
}

class CameraAnalysis {
public:
 CameraAnalysis(std::string registry,std::string device,std::string credential,PersistenceAudit* audit)
 : path_(std::move(registry)), device_(std::move(device)),credential_(std::move(credential)),audit_(audit), pipeline_(rules()) { reset(); }
 bool authorize(){
  net::DeviceRegistry fresh;std::string reason;
  if(!fresh.load(path_,&reason)){withdraw();return false;}
  camera::RegistryAuthorizer auth(fresh,"home_security");
  camera::LogicalDeviceId id;id.site_id="portal";id.device_id=device_; // site identity is operator-owned, not browser input
  const auto* d=fresh.find(device_);
  if(!d || d->consent_purpose!="home_security" || !d->has_camera ||
     auth.authorize(id,credential_,camera::Capability::video)!=camera::AuthorizationOutcome::allowed){withdraw();return false;}
  if(authorized_ && synthetic_!=d->is_synthetic)reset();
  synthetic_=d->is_synthetic;provenance_known_=true;authorized_=true;return true;
 }
 void note_no_frame(){ std::lock_guard<std::mutex> g(process_mutex_); if(state_!="authorization_denied") state_="no_frame"; }
 void withdraw(){reset();authorized_=false;state_="authorization_denied";}
 void process(const std::vector<uint8_t>& jpeg){
  std::lock_guard<std::mutex> guard(process_mutex_);
  // The analysis clock is the WALL clock: the correlation window, the observation timestamp and
  // the persisted metadata must be comparable with the engine's own clock. The caller supplies
  // media only; it cannot pin or rewind time.
  const int64_t ms=std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
  if(!authorize())return;
  auto luma=decode_jpeg_luma(jpeg);
  if(luma.empty()){++decode_failures_;state_="decode_failed";return;}
  if(!authorize())return;
  const auto now=std::chrono::system_clock::time_point(std::chrono::milliseconds(ms));
  if(last_ms_ && ms<=last_ms_){state_="timestamp_refused";return;}
  last_ms_=ms;
  auto provenance=synthetic_?Provenance::synthetic:Provenance::real;
  auto f=session_->ingest(luma.data(),luma.size(),provenance,now);
  ++processed_;state_="analyzed";
  auto o=session_->observation_for(UUID::generate(),"portal-camera");
  o.observation_timestamp=now;
  current_={{"observation_id",o.observation_id},{"detector_id",o.detector_id},{"status",detector_status_to_string(o.status)},{"confidence",o.confidence.value_or(0)},{"timestamp_ms",ms},{"provenance",provenance_to_string(provenance)},{"motion_level",f.motion_level},{"finding",camera_finding_kind_to_string(f.kind)},{"ratio",f.ratio},{"baseline",f.baseline},{"change_character",f.change_character}};
  recent_.push_back(current_);
  if(recent_.size()>8) recent_.erase(recent_.begin());
  auto r=anomaly_.observe(f.motion_level,now);auto s=anomaly_.snapshot();
  statistical_={{"status",s.status},{"deviation",s.deviation},{"sample_count",s.sample_count},{"baseline_ready",s.baseline_ready},{"advisory",true}};
  auto candidate=pipeline_.process(o.to_event());
  if(!candidate)return;
  ++qualified_;
  if(!authorize())return;
  candidate->details["camera_observation_id"]=o.observation_id;
  candidate->details["acquired_at_ms"]=ms;
  bool suppressed=false;
  bool committed=policy_.admit_durable(*candidate,suppressed,[&]{return audit_ && audit_->record_alert(*candidate);});
  if(committed){++commits_;last_alert_=candidate->alert_id;state_="alert_committed";
   last_alert_json_={{"alert_id",candidate->alert_id},{"rule",candidate->rule_name},{"severity",candidate->severity==Severity::critical?"critical":candidate->severity==Severity::warning?"warning":"info"},{"confidence",candidate->confidence},{"contributing_event_ids",candidate->contributing_event_ids},{"camera_observation_id",o.observation_id}};}
  else if(suppressed){++suppressed_;state_="cooldown_suppressed";}
  else {++commit_failures_;if(authorized_)state_="commit_failed";}
 }
 nlohmann::json status() const {
  std::lock_guard<std::mutex> guard(process_mutex_);
  nlohmann::json categories={{"motion","available"},{"statistical_change","available"}};
  for(auto name:{"audio","speech","identity","visitors","falls","animals","emergencies"})categories[name]="unavailable";
  return {{"enabled",true},{"authorized",authorized_},{"state",state_},{"observations_processed",processed_},{"decode_failures",decode_failures_},{"qualified_alerts",qualified_},{"alerts_committed",commits_},{"commit_failures",commit_failures_},{"suppressed_alerts",suppressed_},{"last_alert_id",last_alert_},{"last_alert",last_alert_json_},{"recent_observations",recent_},{"last_observation",current_},{"statistical",statistical_},{"constraints",{{"bounded_decoder",true},{"no_raw_media_persisted",true},{"no_identity_inference",true},{"no_audio_inference",true}}},{"categories",categories},{"provenance",provenance_known_?(synthetic_?"synthetic":"real"):"none"}};
 }
private:
 static std::vector<std::shared_ptr<ICorrelationRule>> rules(){DetectionFusionRule::Config c;c.evidence_detector_ids={"camera-motion-activity"};c.use_episode_evidence=true;c.episode.require_motion_character=true;return {std::make_shared<DetectionFusionRule>("camera-detection-fusion",c)};}
 void reset(){CameraActivitySession::Config c;c.width=160;c.height=120;session_=std::make_unique<CameraActivitySession>(c);pipeline_.clear_history();policy_.reset();anomaly_.reset();last_ms_=0;current_=nullptr;statistical_=nullptr;last_alert_json_=nullptr;recent_.clear();}
 mutable std::mutex process_mutex_;
 std::string path_,device_,credential_;PersistenceAudit* audit_;
 Pipeline pipeline_;AlertPolicy policy_;AnomalyMonitor anomaly_;std::unique_ptr<CameraActivitySession> session_;
 bool authorized_=false,synthetic_=false,provenance_known_=false;int64_t last_ms_=0;
 uint64_t processed_=0,decode_failures_=0,qualified_=0,commits_=0,commit_failures_=0,suppressed_=0;
 std::string state_="awaiting_frame",last_alert_;nlohmann::json current_=nullptr,statistical_=nullptr,last_alert_json_=nullptr;
 std::vector<nlohmann::json> recent_;
};
}
