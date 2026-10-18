#include "runtime.hpp"
#include "json.hpp"
#include "logfmt.hpp"
#include <atomic>
#include <chrono>
#include <jni.h>
#include <link.h>
#include <thread>
#include "dobby.h"
#include "handoff_diagnostics.hpp"

namespace {
std::atomic<bool> started{false};std::unique_ptr<ms::Runtime> runtime;
std::atomic<ms::Runtime*> published{nullptr};
// Process lifetime: partial hook installation still needs its passthroughs.
std::atomic<ms::handoff::Diagnostics*> handoffDiagnostics{nullptr};
uint64_t now(){return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();}
void onTick(void*,DobbyRegisterContext*){
 if(ms::handoff::probeDepth)return; // avoid Runtime reentry caused by a getter
 if(auto* d=handoffDiagnostics.load(std::memory_order_acquire))d->onTick();
 if(auto* p=published.load(std::memory_order_acquire))p->tick(now());
}
#if !GIBOX_HANDOFF_DIAGNOSTICS
// The game's async model hand-off (abi::receive): attach before its own callback, check after (Runtime::spawned).
using ReceiveFn=void(*)(uintptr_t,uint64_t,bool,uint32_t,uintptr_t,uintptr_t);
ReceiveFn receiveOriginal=nullptr;
void onReceive(uintptr_t self,uint64_t key,bool ok,uint32_t handle,uintptr_t asset,uintptr_t method){
 auto* p=published.load(std::memory_order_acquire);const bool handled=p&&ok&&asset;
 if(handled)p->spawned(asset);
 receiveOriginal(self,key,ok,handle,asset,method);
 if(handled)p->spawnedAfter();
}
// The game's own mesh + bone binding: the mesh argument is swapped before the original runs (Runtime::bindMesh). Up to
// eight integer arguments and x0 are passed through untouched; neither entry takes floating-point arguments.
using BindFn=uint64_t(*)(uintptr_t,uintptr_t,uintptr_t,uintptr_t,uintptr_t,uintptr_t,uintptr_t,uintptr_t);
BindFn lodBindOriginal=nullptr,meshBonesOriginal=nullptr;
uint64_t onLodBind(uintptr_t loader,uintptr_t mesh,uintptr_t renderer,uintptr_t a3,uintptr_t a4,uintptr_t a5,uintptr_t a6,uintptr_t a7){
 if(auto* p=published.load(std::memory_order_acquire))mesh=p->bindMesh(renderer,mesh);
 auto result=lodBindOriginal(loader,mesh,renderer,a3,a4,a5,a6,a7);
 if(auto* p=published.load(std::memory_order_acquire))p->meshBound(renderer);
 return result;}
uint64_t onMeshBones(uintptr_t renderer,uintptr_t mesh,uintptr_t hashes,uintptr_t bones,uintptr_t a4,uintptr_t a5,uintptr_t a6,uintptr_t a7){
 if(auto* p=published.load(std::memory_order_acquire))mesh=p->bindMesh(renderer,mesh);
 auto result=meshBonesOriginal(renderer,mesh,hashes,bones,a4,a5,a6,a7);
 if(auto* p=published.load(std::memory_order_acquire))p->meshBound(renderer);
 return result;}
bool codeMatches(uintptr_t base,const ms::abi::CodeCheck& c){auto* p=reinterpret_cast<const unsigned char*>(base+c.rva);auto digit=[](char x){return x<='9'?x-'0':x-'a'+10;};
 for(size_t i=0;i<16;i++){if(p[i]!=unsigned(digit(c.hex[2*i])*16+digit(c.hex[2*i+1])))return false;}return true;}
#endif
struct Image{uintptr_t base=0;const ElfW(Phdr)* ph=nullptr;size_t count=0;};
int findImage(dl_phdr_info* info,size_t,void* ptr){std::string name=info->dlpi_name?info->dlpi_name:"";auto p=name.find_last_of('/');name=name.substr(p==name.npos?0:p+1);if(name!="libyuanshen.so")return 0;auto& out=*static_cast<Image*>(ptr);out={info->dlpi_addr,info->dlpi_phdr,info->dlpi_phnum};return 1;}
std::string jstringValue(JNIEnv* env,jstring s){ms::require(s!=nullptr,"Null Java string");const char* p=env->GetStringUTFChars(s,nullptr);ms::require(p!=nullptr,"Cannot decode Java string");std::string out=p;env->ReleaseStringUTFChars(s,p);return out;}
void exception(JNIEnv* env,const std::exception& e){env->ThrowNew(env->FindClass("java/lang/IllegalArgumentException"),e.what());}
std::string sharedDir(const std::string& pkg){return "/storage/emulated/0/Android/media/"+pkg+"/GIBox";}
// Start-up lines go to the same "GIBox Log.txt" as the runtime (logger "bootstrap", XXMI line format).
void bootLog(const std::string& pkg,const std::string& state,const std::string& detail=""){try{auto dir=sharedDir(pkg);auto level=ms::levelOf(state);if(int(level)<ms::logThresholdOf(dir))return;
 std::filesystem::create_directories(dir);ms::appendLog(dir,ms::logLine("bootstrap",level,detail.empty()?state:state+' '+detail));}catch(const std::exception&){}}
void bootStatus(const std::string& pkg,const std::string& state,const std::string& detail){auto dir=sharedDir(pkg);try{std::filesystem::create_directories(dir);auto s="{\"state\":"+ms::quote(state)+",\"detail\":"+ms::quote(detail)+",\"characterId\":0,\"model\":\"\"}";ms::write(dir+"/status.json.tmp",s);std::filesystem::rename(dir+"/status.json.tmp",dir+"/status.json");}catch(const std::exception&){}bootLog(pkg,state,detail);}
}
extern "C" JNIEXPORT void JNICALL Java_com_gibox_tool_NativeBridge_start(JNIEnv* env,jclass,jstring jp,jstring jd){try{
 auto pkg=jstringValue(env,jp),priv=jstringValue(env,jd);ms::require(pkg=="com.miHoYo.Yuanshen"||pkg=="com.miHoYo.ys.bilibili","Unsupported package");if(started.exchange(true))return;
 std::thread([pkg,priv]{try{bootStatus(pkg,"waiting","等待 libyuanshen.so");Image image;for(unsigned n=0;n<480&&!image.base;n++){dl_iterate_phdr(findImage,&image);if(!image.base)std::this_thread::sleep_for(std::chrono::milliseconds(500));}ms::require(image.base!=0,"libyuanshen.so load timeout");
 auto actual=ms::loadedBuildId(image.base,image.ph,image.count,sizeof(ElfW(Phdr)));if(actual!=ms::abi::buildId){bootStatus(pkg,"buildid-mismatch","实际 Build ID："+actual);return;}
 auto rt=std::make_unique<ms::Runtime>(ms::Engine(image.base),pkg,priv);
 // Publish before Dobby makes Tick executable through its trampoline.
 runtime=std::move(rt);published.store(runtime.get(),std::memory_order_release);
 int result=DobbyInstrument(reinterpret_cast<void*>(image.base+ms::abi::tick),onTick);
 if(result!=0){published.store(nullptr,std::memory_order_release);runtime.reset();throw std::runtime_error("DobbyInstrument failed: "+std::to_string(result));}bootStatus(pkg,"ready","模型运行时已就绪");
 #if GIBOX_HANDOFF_DIAGNOSTICS
 // A diagnostic failure leaves the existing replacement runtime running.
 try{auto* d=new ms::handoff::Diagnostics(ms::Engine(image.base),sharedDir(pkg)+"/handoff-"+std::to_string(::getpid())+".jsonl");
  handoffDiagnostics.store(d,std::memory_order_release);std::string error;
  if(!d->start(error))bootStatus(pkg,"handoff-disabled",error);
 }catch(const std::exception& e){bootStatus(pkg,"handoff-disabled",e.what());}
 #else
 // Replacement at the hand-off; without it the scan still replaces, just not before activation.
 {std::string bad;for(auto& c:ms::abi::handoffChecks)if(!codeMatches(image.base,c)){char b[24];std::snprintf(b,sizeof(b),"0x%llx ",static_cast<unsigned long long>(c.rva));bad+=b;}
  if(!bad.empty())bootLog(pkg,"spawn-hook-disabled","code mismatch at "+bad);
  else if(DobbyHook(reinterpret_cast<void*>(image.base+ms::abi::receive),reinterpret_cast<dobby_dummy_func_t>(onReceive),reinterpret_cast<dobby_dummy_func_t*>(&receiveOriginal))!=0||!receiveOriginal)bootLog(pkg,"spawn-hook-disabled","DobbyHook failed");
  else bootLog(pkg,"spawn-hook","installed");}
 // Mesh + bone binding: each entry is hooked only when its own code bytes match.
 for(auto& c:ms::abi::bindChecks){const bool lod=c.rva==ms::abi::lodBind;const char* what=lod?"lod-bind-hook":"mesh-bones-hook";
  if(!codeMatches(image.base,c)){bootLog(pkg,std::string(what)+"-disabled","code mismatch");continue;}
  auto replacement=lod?reinterpret_cast<dobby_dummy_func_t>(onLodBind):reinterpret_cast<dobby_dummy_func_t>(onMeshBones);auto* original=reinterpret_cast<dobby_dummy_func_t*>(lod?&lodBindOriginal:&meshBonesOriginal);
  if(DobbyHook(reinterpret_cast<void*>(image.base+c.rva),replacement,original)!=0||!*original)bootLog(pkg,std::string(what)+"-disabled","DobbyHook failed");else bootLog(pkg,what,"installed");}
 #endif
 }catch(const std::exception& e){bootStatus(pkg,"error",e.what());}}).detach();
 }catch(const std::exception& e){exception(env,e);} }
extern "C" JNIEXPORT jbyteArray JNICALL Java_com_gibox_tool_NativeBridge_decodeDds(JNIEnv* env,jclass,jbyteArray input){try{ms::require(input!=nullptr,"Null DDS");auto n=env->GetArrayLength(input);ms::require(n>=128&&n<=256*1024*1024,"DDS input size");ms::Bytes b(n);env->GetByteArrayRegion(input,0,n,reinterpret_cast<jbyte*>(b.data()));if(env->ExceptionCheck())return nullptr;auto t=ms::decodeDds(b);ms::Bytes out(8+t.pixels.size());ms::put32(out.data(),t.w);ms::put32(out.data()+4,t.h);std::memcpy(out.data()+8,t.pixels.data(),t.pixels.size());auto a=env->NewByteArray(out.size());if(!a)return nullptr;env->SetByteArrayRegion(a,0,out.size(),reinterpret_cast<jbyte*>(out.data()));return a;}catch(const std::exception& e){exception(env,e);return nullptr;}}
