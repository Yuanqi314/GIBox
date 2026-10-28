#include "runtime.hpp"
#include "model.hpp"
#include "logfmt.hpp"
#include "render_probe.hpp"
#include "glow_trace.hpp"
#include "logic_game.hpp"
#include <chrono>
#include <atomic>
#include <cctype>
#include <condition_variable>
#include <cstdio>
#include <deque>
#include <elf.h>
#include <mutex>
#include <optional>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>
#include <thread>
#include <tuple>
namespace ms {
namespace {
bool alive(Ptr p){return p&&load<Ptr>(p+0x10);}
uint64_t length(Ptr p,uint64_t max){require(p!=0,"Null managed array");auto n=load<uint64_t>(p+0x18);require(n<=max,"Managed array bound");return n;}
std::vector<Ptr> pointers(Ptr p,size_t max){auto n=length(p,max);std::vector<Ptr> out(n);if(n)std::memcpy(out.data(),reinterpret_cast<void*>(p+0x20),n*8);return out;}
Bytes arrayBytes(Ptr p,size_t stride,size_t max){auto n=length(p,max);Bytes b(n*stride);if(!b.empty())std::memcpy(b.data(),reinterpret_cast<void*>(p+0x20),b.size());return b;}
int threadId(){static thread_local int id=int(::syscall(SYS_gettid));return id;}
uint64_t clockMs(){return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();}
std::string hex(uint64_t v){char b[24];std::snprintf(b,sizeof(b),"0x%llx",static_cast<unsigned long long>(v));return b;}

// ---------------------------------------------------------------- files (worker side)
// Debug experiments for the switch-in glow (control.json "experiments"), each off by default:
//   gameMaterials  the game's own materials on our mesh (no clones): is the glow in our copied materials?
//   firstRefreshMs first material re-copy after attaching (default 1500): does the glow end with it?
//   neutralColors / noUv2  mesh data: vertex colours set to the original's usual value, UV2 zeroed (rebuilds the model)
//   glowTrace      record every write to the tracked materials and renderers (glow_trace.hpp); changes nothing itself
//   slotTextures   1/2: the game's own materials stay on, the mod's textures go into each draw slot's per-material
//                  property block (1) or per-material custom texture (2); no copies or refreshes
//   separateDraws  every draw in effect its own submesh, as before 0.7.23 (mergeDraws, model.hpp): for comparing the
//                  picture and the draw count (rebuilds the model)
//   logicCore      a model's own INI logic (3DMigoto logic core, logic_game.hpp) picks the animation frame or the
//                  outfit shown, by what its draws select (an outfit model's logic must be finite; the App's outfit
//                  values are its input; the App's key presses, control.json "keys", are its key events, and its
//                  persist variables are kept in logic-persist/); FrameLoop or the App's choice when there is no logic
//                  data or it fails
struct Experiments {bool gameMaterials=false,neutralColors=false,noUv2=false,glowTrace=false,separateDraws=false,logicCore=false;int firstRefreshMs=0,slotTextures=0,animationMode=0;
 std::string meshKey(bool animated=false)const{return std::string(neutralColors?"c":"-")+(noUv2?"u":"-")+(separateDraws?"s":"-")+(animated?"a"+std::to_string(animationMode):"");}
 bool operator==(const Experiments& o)const{return gameMaterials==o.gameMaterials&&neutralColors==o.neutralColors&&noUv2==o.noUv2&&glowTrace==o.glowTrace&&separateDraws==o.separateDraws&&firstRefreshMs==o.firstRefreshMs&&slotTextures==o.slotTextures&&animationMode==o.animationMode&&logicCore==o.logicCore;}
 bool any()const{return gameMaterials||neutralColors||noUv2||glowTrace||separateDraws||firstRefreshMs||slotTextures||animationMode||logicCore;}
 std::string text()const{return std::string("gameMaterials=")+(gameMaterials?"on":"off")+" firstRefreshMs="+(firstRefreshMs?std::to_string(firstRefreshMs):"default")+" neutralColors="+(neutralColors?"on":"off")+" noUv2="+(noUv2?"on":"off")+" glowTrace="+(glowTrace?"on":"off")+" slotTextures="+(slotTextures==1?"block":slotTextures==2?"custom":"off")+" separateDraws="+(separateDraws?"on":"off")+" animationMode="+std::to_string(animationMode)+" logicCore="+(logicCore?"on":"off");}};
// keys: the App's presses of a model's [Key] sections (model.json logic.keys), numbered; each is taken once, by number.
struct KeyPress {long long seq=0;size_t file=0;std::string section;bool back=false;bool operator==(const KeyPress& o)const{return seq==o.seq&&file==o.file&&section==o.section&&back==o.back;}};
struct ControlState {Experiments exp;bool enabled=false;int logLevel=int(Level::Info);std::vector<std::string> selected;long long exportBones=0,refreshBones=0,renderMark=0;unsigned refreshBonesCharacter=0;std::map<std::string,std::map<std::string,int>> variables;std::map<std::string,std::vector<KeyPress>> keys;};
ControlState parseControl(const std::string& raw){
 Json c=JsonParser(raw).parse();require(c.type==Json::Object,"Control JSON object");ControlState s;
 const auto& a=c.at("selected");require(a.type==Json::Array&&a.array.size()<=128,"Selected list");
 for(auto& v:a.array){require(v.type==Json::String&&!v.string.empty()&&v.string.find('/')==v.string.npos&&v.string!="."&&v.string!="..","Selected name");s.selected.push_back(v.string);}
 auto integerOf=[](const Json& j){if(j.type==Json::Null)return 0LL;require(j.type==Json::Number&&std::floor(j.number)==j.number&&std::abs(j.number)<=9007199254740991.,"Control counter");return static_cast<long long>(j.number);};
 s.enabled=c.at("enabled").type==Json::Bool&&c.at("enabled").boolean;s.exportBones=integerOf(c.at("exportBones"));
 s.renderMark=integerOf(c.at("renderMark"));require(s.renderMark>=0,"Render marker counter");
 s.refreshBones=integerOf(c.at("refreshBones"));auto refreshId=integerOf(c.at("refreshBonesCharacter"));
 require(s.refreshBones>=0&&(!s.refreshBones||(refreshId>=10000000&&refreshId<=10001999)),"Bone refresh character/request");
 if(s.refreshBones){s.refreshBonesCharacter=unsigned(refreshId);}
 const auto& vars=c.at("variables");require(vars.type==Json::Object||vars.type==Json::Null,"Variables object");
 for(auto& model:vars.object){require(model.second.type==Json::Object,"Variables object");for(auto& kv:model.second.object){require(kv.first.size()>1&&kv.first[0]=='$',"Variable name");auto n=integerOf(kv.second);require(std::abs(n)<=1000000,"Variable value");s.variables[model.first][kv.first]=int(n);}}
 const auto& keys=c.at("keys");require(keys.type==Json::Object||keys.type==Json::Null,"Keys object");
 for(auto& model:keys.object){require(model.second.type==Json::Array&&model.second.array.size()<=32,"Key presses of a model");
  for(auto& k:model.second.array){require(k.type==Json::Object,"Key press");KeyPress x;x.seq=integerOf(k.at("seq"));auto file=integerOf(k.at("file"));require(x.seq>0&&file>=0&&file<64,"Key press number / file");x.file=size_t(file);
   require(k.at("section").type==Json::String&&!k.at("section").string.empty()&&k.at("section").string.size()<=256,"Key press section");x.section=k.at("section").string;x.back=k.at("back").type==Json::Bool&&k.at("back").boolean;s.keys[model.first].push_back(x);}}
 s.logLevel=logThreshold(c);
 const auto& x=c.at("experiments");if(x.type==Json::Object){auto flag=[&](const char* k){return x.at(k).type==Json::Bool&&x.at(k).boolean;};
  s.exp.gameMaterials=flag("gameMaterials");s.exp.neutralColors=flag("neutralColors");s.exp.noUv2=flag("noUv2");s.exp.glowTrace=flag("glowTrace");s.exp.separateDraws=flag("separateDraws");s.exp.logicCore=flag("logicCore");
  auto ms=integerOf(x.at("firstRefreshMs"));require(ms>=0&&ms<=10000,"experiments.firstRefreshMs");s.exp.firstRefreshMs=int(ms);
  auto slots=integerOf(x.at("slotTextures"));require(slots>=0&&slots<=2,"experiments.slotTextures");s.exp.slotTextures=int(slots);auto animation=integerOf(x.at("animationMode"));require(animation>=0&&animation<=2,"experiments.animationMode");s.exp.animationMode=int(animation);}
 return s;
}
// lodMesh: the converter's target is a lower level of detail (a skeleton library recorded while the character was still
// loading, before 0.6.5); the part's bone indices follow that LOD's shorter bone list, so the model is not built.
struct PartInfo {std::string name,group,lodMesh;unsigned minBones=0;bool hasTarget=false;Fingerprint target;};
// A model folder as the scan sees it (model.json + learned.json; model.bin is only read when the model is loaded).
// id 0 + names: converted without the skeleton library; the character is found by the names in its skeleton chain.
struct ModelInfo {std::string name,path,generation,stamp;unsigned id=0;bool animated=false;std::vector<PartInfo> parts;std::vector<std::string> names;Learned learned;Variants variants;When replaceWhen;};
std::vector<std::string> nameTokens(const std::string& s){std::vector<std::string> out;std::string cur;for(size_t i=0;i<=s.size();i++){unsigned char c=i<s.size()?s[i]:' ';bool alnum=(c>='a'&&c<='z')||(c>='A'&&c<='Z')||(c>='0'&&c<='9');
 if(!alnum){if(cur.size()>=3)out.push_back(cur);cur.clear();continue;}cur+=char(c>='A'&&c<='Z'?c+32:c);}return out;}
bool namesMatch(const std::vector<std::string>& names,const std::vector<std::string>& tokens){for(auto& n:names)for(auto& t:tokens)if(t==n||(std::min(t.size(),n.size())>=4&&(t.compare(0,n.size(),n)==0||n.compare(0,t.size(),t)==0)))return true;return false;}
// model.json is written last and the converter swaps the whole folder in: a new conversion is a new file (inode), even
// where the storage keeps only coarse modification times. Inode, time and size decide whether the folder is read again.
std::string stampOf(const std::string& file){
 struct stat st{};require(::stat(file.c_str(),&st)==0,"Cannot stat: "+file);
 return std::to_string(static_cast<unsigned long long>(st.st_ino))+":"+std::to_string(static_cast<long long>(st.st_mtim.tv_sec))+"."+std::to_string(static_cast<long long>(st.st_mtim.tv_nsec))+":"+std::to_string(static_cast<long long>(st.st_size));
}
// A learned target only counts for the renderer the part was converted for (learned.json may be older than the conversion).
bool sameRenderer(const Fingerprint& f,const PartSummary& p){auto k=normalize(p.renderer,true);return normalize(f.renderer,true)==k||normalize(f.mesh,true)==k||(p.hasTarget&&normalize(f.renderer,true)==normalize(p.target.renderer,true));}
std::vector<ModelInfo> scanModels(const std::string& root,std::map<std::string,ModelInfo>& cache){
 std::vector<ModelInfo> out;std::map<std::string,ModelInfo> next;std::error_code ec;
 for(auto& d:std::filesystem::directory_iterator(root,ec)){
  if(!d.is_directory(ec)){continue;}auto name=d.path().filename().string();if(name.empty()||name[0]=='.')continue;auto path=d.path().string();
  try{auto manifest=path+"/model.json";if(!exists(manifest))continue;auto stamp=stampOf(manifest);auto it=cache.find(path);
   if(it!=cache.end()&&it->second.stamp==stamp){next[path]=it->second;out.push_back(it->second);continue;}
   auto summary=readSummary(path);ModelInfo m;m.name=name;m.path=path;m.stamp=stamp;m.generation="g:"+summary.generation;m.id=summary.id;m.animated=summary.animated;m.names=summary.names;m.learned=readLearned(path);m.variants=summary.variants;m.replaceWhen=summary.replaceWhen;
   if(!m.id&&m.learned.id)m.id=m.learned.id;
   // Targets the runtime learned (also after a game update moved a part) take precedence over the converter's.
   for(auto& p:summary.parts){PartInfo pi;pi.name=p.name;pi.group=p.renderer;pi.minBones=p.indexedBones?p.bones:0;pi.hasTarget=p.hasTarget&&p.target.bones>=pi.minBones;pi.target=p.target;if(p.hasTarget&&lowerLod(p.target.mesh))pi.lodMesh=p.target.mesh;
    // A target learned from a lower level of detail (before 0.6.5) is dropped and learned again from the full mesh.
    auto l=m.learned.targets.find(p.name);if(l!=m.learned.targets.end()&&sameRenderer(l->second,p)&&!lowerLod(l->second.mesh)&&l->second.bones>=pi.minBones){pi.target=l->second;pi.hasTarget=true;}m.parts.push_back(pi);}
   next[path]=m;out.push_back(m);
  }catch(const std::exception&){}
 }
 cache.swap(next);std::sort(out.begin(),out.end(),[](auto& a,auto& b){return a.name<b.name;});return out;
}
// Everything a build needs, read and checked off the game thread (model.hpp).
// Textures: only those the chosen variant values draw; the others stay in model.bin until a variant switch needs them.
std::shared_ptr<ModelFile> loadModel(const ModelInfo& info,const std::map<std::string,int>& chosen,bool verify=true){
 auto m=loadModelFile(info.path,false,verify);require("g:"+m->generation==info.generation,"model.json changed while loading");
 m->name=info.name;m->id=info.id;require(m->parts.size()==info.parts.size(),"model.json changed while loading");
 // The worker reads the old sidecar alongside the verified model. Hooks never open a bonemap file.
 if(!info.animated)for(size_t i=0;i<m->parts.size();i++){auto& part=m->parts[i];if(!info.parts[i].hasTarget||!part.sourceBones.empty())continue;
  try{part.legacyBoneNames=readLegacyBoneNames(info.path+"/bonemap-"+part.spec.name+".tsv",part.spec.bones);}catch(const std::exception& ex){part.legacyBoneError=ex.what();}}
 readTextures(info.path,m->generation,m->blobSize,m->materials,materialsNeeded(*m,pick(m->variants,chosen)));return m;
}
// Memory a loaded, not yet built model holds: geometry and the textures read for it.
uint64_t loadedBytes(const ModelFile& m){uint64_t n=0;for(auto& p:m.parts){n+=p.geo.positions.size()+p.geo.normals.size()+p.geo.uvs.size()+p.geo.indices.size()+p.weights.size();for(auto& c:p.channels)n+=c.second.size();}
 for(auto& kv:m.materials){for(auto* t:{kv.second.main.get(),kv.second.light.get(),kv.second.normal.get()})if(t)n+=t->pixels.size();}return n;}
// skeletons/<name>/skeleton.json, verified before an atomic file replacement; keep the last good record on failure.
void writeSkeleton(const std::string& root,const std::string& name,bool replace,const std::string& json){
 auto dest=root+"/"+name;if(!replace)require(!exists(dest),"Skeleton export directory exists");std::filesystem::create_directories(dest);
 auto file=dest+"/skeleton.json",tmp=file+".tmp";
 {std::ofstream out(tmp,std::ios::binary|std::ios::trunc);out.write(json.data(),std::streamsize(json.size()));out.flush();require(bool(out),"Skeleton write failed: "+tmp);}
 require(text(tmp)==json,"Skeleton temporary-file verification failed");std::filesystem::rename(tmp,file);
 require(text(file)==json,"Skeleton read-back verification failed");
}
void writeAtomic(const std::string& file,const std::string& body){write(file+".tmp",body);std::filesystem::rename(file+".tmp",file);}

// Background thread for all file work. With threaded=false every job runs inline (host tests).
class Worker {
 bool threaded;std::mutex m;std::condition_variable cv;std::deque<std::function<void()>> jobs;bool stopping=false;std::thread thread;std::function<void()> periodic;
 static void run(const std::function<void()>& f){try{if(f)f();}catch(...){}}
 void loop(){auto last=std::chrono::steady_clock::now()-std::chrono::seconds(1);
  while(true){std::function<void()> f;{std::unique_lock<std::mutex> l(m);cv.wait_for(l,std::chrono::milliseconds(50),[&]{return stopping||!jobs.empty();});
    if(!jobs.empty()){f=std::move(jobs.front());jobs.pop_front();}else if(stopping)return;}
   run(f);auto now=std::chrono::steady_clock::now();if(now-last>=std::chrono::milliseconds(50)){last=now;run(periodic);}}}
 public:
 explicit Worker(bool t):threaded(t){}
 bool background()const{return threaded;}
 void start(std::function<void()> p){periodic=std::move(p);if(threaded)thread=std::thread([this]{loop();});}
 void post(std::function<void()> f){if(!threaded){run(f);return;}{std::lock_guard<std::mutex> l(m);if(stopping)return;jobs.push_back(std::move(f));}cv.notify_one();}
 // Ahead of everything queued (the files of a character that is appearing now).
 void postFront(std::function<void()> f){if(!threaded){run(f);return;}{std::lock_guard<std::mutex> l(m);if(stopping)return;jobs.push_front(std::move(f));}cv.notify_one();}
 void stop(){if(!threaded)return;{std::lock_guard<std::mutex> l(m);stopping=true;}cv.notify_all();if(thread.joinable())thread.join();}
 ~Worker(){stop();}
};
}

std::string loadedBuildId(Ptr base,const void* headers,size_t count,size_t stride){
 require(count<=128&&stride==sizeof(Elf64_Phdr),"Invalid ELF program headers");auto ph=static_cast<const uint8_t*>(headers);
 for(size_t i=0;i<count;i++){Elf64_Phdr h;std::memcpy(&h,ph+i*stride,sizeof(h));if(h.p_type!=PT_NOTE||h.p_memsz>1024*1024)continue;auto note=reinterpret_cast<const uint8_t*>(base+h.p_vaddr);size_t p=0;
 while(p+12<=h.p_memsz){uint64_t ns=u32(note+p),ds=u32(note+p+4),type=u32(note+p+8),name=p+12,desc=name+((ns+3)&~3ull),end=desc+((ds+3)&~3ull);require(end<=h.p_memsz&&end>p,"Invalid ELF note");if(type==NT_GNU_BUILD_ID&&ns==4&&u32(note+name)==0x00554e47&&ds&&ds<=64){std::string o;const char*hex="0123456789abcdef";for(size_t j=0;j<ds;j++){o+=hex[note[desc+j]>>4];o+=hex[note[desc+j]&15];}return o;}p=end;}}
 throw std::runtime_error("GNU Build ID not found");
}

// A bounded cache that keeps what is in use: entries go into the current generation; when it is full it becomes the
// previous one (the one before is dropped), and a hit in the previous generation moves the entry back. Keys carry the
// object's identity (address plus native object or instance id), so a reused address is a different key.
template<class K,class V> struct Recent {size_t limit;std::map<K,V> now,before;explicit Recent(size_t l):limit(l){}
 V* find(const K& k){auto it=now.find(k);if(it!=now.end())return &it->second;auto b=before.find(k);if(b==before.end())return nullptr;V v=std::move(b->second);before.erase(b);return &put(k,std::move(v));}
 V& put(const K& k,V v){if(now.size()>=limit){before.swap(now);now.clear();}return now[k]=std::move(v);}
 bool has(const K& k)const{return now.count(k)||before.count(k);}
 void clear(){now.clear();before.clear();}
};
struct Runtime::Impl {
 Engine e;std::string pkg,priv,shared,modelRoot,boneRoot;
 // ---- shared with the worker (guarded by sm)
 std::mutex sm;std::optional<ControlState> pendingControl;std::string controlError;std::vector<ModelInfo> scanned;bool scannedFresh=false;
 struct LoadResult {std::string generation,error;std::shared_ptr<ModelFile> data;uint64_t bytes=0;};
 // Textures read later for a built set (variant switch): path|generation, and the materials now holding pixels.
 struct AnimationResult {std::string path,generation,error;uint64_t token=0,us=0;size_t index=0;std::shared_ptr<ModelFile> data;};std::deque<AnimationResult> animationResults;
 struct TextureResult {std::string path,generation,error;std::map<std::string,MaterialFile> materials;};std::deque<TextureResult> textureResults;std::set<std::string> textureInflight;
 std::map<std::string,LoadResult> loaded;std::set<std::string> inflight;
 struct BoneResult {unsigned id;bool library;long long request;std::string error;long long refresh=0;};std::deque<BoneResult> boneResults;
 // ---- worker only
 std::string controlRaw;uint64_t controlAt=0,scanAt=0;std::map<std::string,ModelInfo> scanCache;
 // ---- game thread only
 struct Owned {std::vector<Ptr> objects;std::vector<uint32_t> handles;};
 struct RestoreBackup:Owned {struct User{Ptr native;uint32_t handle;};Ptr mesh=0,materials=0;std::map<Ptr,User> users;std::set<Ptr> handed;};
 struct MatTex {bool original=true;Ptr main=0,light=0,normal=0;};
 struct BuiltPart {std::string name,avatar;Ptr mesh=0;Fingerprint target;std::vector<std::string> bones;Bytes indices;std::vector<DrawSpec> draws;std::vector<MergedDraw> subs;std::vector<Material> rows;  // all draws; the submeshes in effect, and the material row of each
  Ptr animationOwner=0;Ptr originMesh=0,originMaterials=0;RestoreBackup* backup=nullptr;
  Owned dynamicRoots;std::map<std::string,Ptr> dynamicArrays;std::string dynamicShape;std::vector<int> boneMapping,targetParents;Bytes targetBinds;
  int bindable=-1;};  // last incoming bone-name hash count compatibility, for log suppression; origin pair pinned by the set
 struct AnimationRun {std::string error;FrameLoop loop;int mode=0;uint64_t token=0,started=0,lastStats=0,lastMark=0,updates=0,applyUs=0,maxApplyUs=0;size_t preparing=0,buildPart=0,backPart=0,applied=0;bool loading=false,ready=false,stopped=false;
  std::vector<std::shared_ptr<ModelFile>> cpu;std::vector<std::vector<Ptr>> meshes;std::vector<std::vector<Ptr>> owned;std::vector<Ptr> back;
 };
 // experiments.logicCore: the set's own INI logic, picking its animation frame or its outfit (logic_game.hpp).
 struct LogicRun {std::unique_ptr<LogicDriver> driver;bool failed=false,fresh=false,noted=false;uint64_t misses=0,lastStats=0;unsigned missLines=0;std::string missLogged;std::map<std::string,int> pushed;Assignment want;bool persistDue=false;uint64_t persistAt=0,persistSaves=0;};   // want: the outfit it picked (applyVariant may wait for textures)
 uint64_t animationToken=0;
 struct Set:Owned {std::string name,path,generation;unsigned id=0;std::shared_ptr<ModelFile> data;std::vector<Fingerprint> targets;std::vector<BuiltPart> parts;std::map<std::string,MatTex> textures;std::map<std::string,MaterialFile> mats;std::string meshKey;AnimationRun animation;uint64_t blobSize=0;std::string fileGeneration,textureWait;Variants variants;bool originalMaterials=false,built=false,mergeDraws=true;std::vector<TextureSwap> swaps;LogicSpec logicSpec;LogicRun logic;Assignment assignment;uint64_t used=0,bytes=0;};  // bytes: estimated memory of the built set
 // source: for a renderer the game created from one of ours (adopted), the renderer whose material clones it shows.
 struct Attachment:Owned {Ptr renderer=0,native=0,original=0,originalMaterials=0,source=0;Set* set=nullptr;RestoreBackup* backup=nullptr;size_t part=0;bool adopted=false;std::map<std::string,Ptr> clones;std::string restoreError;
   uint64_t refreshAt=0,materialTraceStart=0,materialTraceAt=0,materialCheckAt=0,materialCheckUntil=0;unsigned refreshes=0,materialTraceSamples=0,materialRepairs=0;std::map<Ptr,std::string> retired;std::map<Ptr,Ptr> materialSources;  // clone -> canonical game material; retained across refreshes
   // slotTextures experiment: the mode written, (property ID, texture) per draw slot, AllowPerMaterialProp before (-1: untouched)
   int slotMode=0,slotAllowBefore=-1;unsigned slotLost=0,slotSwaps=0;std::vector<std::vector<std::pair<int32_t,Ptr>>> slotTex;std::vector<Ptr> slotMats;
   // Direct textures (directPlan): game material -> its native object, what each written property held before (put back on
   // every re-apply, restore and detach) and what we wrote (main texture first); copy: a game copy of such a material.
   struct Direct {Ptr native=0;std::vector<std::pair<const char*,Ptr>> before,after;bool copy=false;Ptr main()const{return after.empty()?0:after[0].second;}};std::map<Ptr,Direct> direct;std::set<Ptr> directPinned;std::string directReport;unsigned directLost=0;};
 struct Seen {Ptr renderer=0,mesh=0;Fingerprint fp;};
 struct Character {Ptr entity=0;unsigned runtime=0,config=0;} ch;
 ControlState ctl;std::vector<ModelInfo> models;long long exportSeen=0,boneRefreshDone=0;unsigned boneRefreshDoneId=0;
 std::vector<std::unique_ptr<Set>> sets;std::map<Ptr,std::unique_ptr<Attachment>> attachments;std::vector<Seen> seen;
 std::vector<std::unique_ptr<RestoreBackup>> restoreBackups;uint64_t restorePruneAt=0,bindAfterHits=0,bindDeferred=0;
 Recent<std::pair<Ptr,uint32_t>,Fingerprint> fpCache{8192};std::set<std::pair<Ptr,Ptr>> skipped;
 std::map<std::string,std::pair<std::string,std::string>> failed;            // path -> generation, message
 std::map<std::string,std::map<std::string,Fingerprint>> learned;            // path -> part -> target from the world character
 std::set<unsigned> library;std::map<std::string,unsigned> learnedId;Recent<std::pair<Ptr,Ptr>,std::vector<std::string>> chains{1024};Recent<std::pair<Ptr,Ptr>,std::string> rendererNames{2048};std::map<unsigned,uint64_t> libraryRetry;std::map<unsigned,std::string> libraryError;std::map<std::string,unsigned> attempts;  // path|generation -> failed builds
 std::map<std::string,std::string> staleNote,materialWait;
 std::set<unsigned> libraryPending;std::map<std::string,std::string> boneRefreshCause;
 std::string boneCandidate,manualWait,boneBadList;unsigned boneBadStreak=0;Ptr boneCandidateEntity=0;unsigned boneCandidateId=0;uint64_t boneCandidateAt=0,manualRetryAt=0;bool manualPending=false;
 std::map<unsigned,std::pair<std::string,uint64_t>> libraryWait;uint64_t boneFullSearchAt=0;Ptr boneFullSearchEntity=0;bool exportLodWait=false;std::string boneSource;
 // Async hand-off: wanted models of the last tick (valid until the next tick's consume), GameObject class, Tick thread.
 std::vector<const ModelInfo*> wantedNow;std::vector<Ptr> spawnAttached;Ptr goClass=0;std::atomic<int> tickTid{0};std::atomic<uint64_t> offThread{0};uint64_t spawnOffThread=0,reclaims=0,guardAt=0,binds=0,passes=0;
 // Prepared on Tick, never by the binding hot path. Unknown targets and same-name LODs remain candidates.
 struct BindingTarget {const ModelInfo* model;std::vector<Fingerprint> parts;bool known;};
 using BindingFingerprint=std::tuple<std::string,unsigned,unsigned,unsigned>;
 using BindingRejectKey=std::tuple<Ptr,uint32_t,Ptr,uint32_t,Ptr,unsigned,unsigned,unsigned>;
 std::vector<BindingTarget> bindingTargets;std::set<BindingFingerprint> bindingExact;std::set<std::string> bindingLods;
 bool bindingReady=false,bindingUncertain=true;std::string bindingStamp;
 Recent<BindingRejectKey,uint64_t> bindingRejected{2048};uint64_t bindingNow=0,bindingStatsAt=0,bindingChecks=0,bindingCacheHits=0,bindingFiltered=0,bindingDeep=0;bool bindingStatsDirty=false;
 std::atomic<int> logLevel{int(Level::Info)};std::map<std::string,std::string> learnedGen;  // log threshold (control.json "logLevel"); generation the learned targets belong to
 Ptr rendererTypeObject=0;bool typeDumped=false;bool findOff=false;unsigned findFails=0;uint64_t next=0,discoverAt=0,findAt=0;bool inTick=false;std::string lastStatus;std::vector<std::string> notes;std::map<std::string,std::string> missingLogged;std::set<std::string> extraSlots,lodLogged;
 std::map<unsigned,std::pair<std::string,std::vector<SkeletonRenderer>>> libraryRead;  // id -> file stamp, records  // model|part -> last target-missing reason
 Owned handoffTypeRoots;uint64_t handoffTypeRetry=0;
 // Last scan, for the wait reason: world character's renderers, FindObjectsOfType results (-1: not run), renderers left
 // out because an attach failed on them; scanCount numbers the scans. Per model: scan number, reason, details logged, time.
 int scanWorld=0,scanAll=-1;unsigned scanSkipped=0;uint64_t scanCount=0;unsigned shownCharacter=~0u;uint64_t idleMs=60000,idleBudget=256ull<<20;bool controlSeen=false;std::map<std::string,long long> keySeen;std::string exportWait;
 struct WaitWhy {uint64_t scan=~0ull;std::string reason,context;bool plain=false;std::string logged;uint64_t loggedAt=0;};std::map<std::string,WaitWhy> waitWhy,targetWaitWhy;
 struct LegacyLodProbe {std::string stamp,note;uint64_t at=0;};std::map<std::string,LegacyLodProbe> legacyLodProbes;
 std::unique_ptr<renderprobe::UnityReader> renderReader;std::unique_ptr<renderprobe::Recorder> renderRecorder;
 long long renderMarkSeen=0;bool renderControlSeen=false;uint64_t renderIdentityAt=0,renderFollowEndAt=0;
 struct RenderBones {std::vector<std::pair<Ptr,uint32_t>> bones;std::string error;};
 struct RenderBind {RenderBones before;unsigned depth=0;};std::map<Ptr,RenderBind> renderBinding;
 Worker worker;  // last member: destroyed (joined) first, while everything it touches still exists

 Impl(Engine x,std::string p,std::string d,std::string media,bool threaded):e(std::move(x)),pkg(std::move(p)),priv(std::move(d)),worker(threaded){
  require(pkg=="com.miHoYo.Yuanshen"||pkg=="com.miHoYo.ys.bilibili","Unsupported package");shared=media+"/"+pkg+"/GIBox";modelRoot=shared+"/models";boneRoot=shared+"/skeletons";
  for(auto& s:{priv,shared,modelRoot,boneRoot})std::filesystem::create_directories(s);
  logLevel.store(logThresholdOf(shared));
  // A completed request is acknowledged across game restarts; an uncompleted request in control.json is replayed.
  try{auto ack=JsonParser(text(shared+"/bone-refresh-result.json")).parse();auto n=ack.at("request"),id=ack.at("character");
   require(n.type==Json::Number&&n.number>0&&n.number<=9007199254740991.&&std::floor(n.number)==n.number&&id.type==Json::Number&&id.number>=10000000&&id.number<=10001999&&std::floor(id.number)==id.number,"Bone refresh acknowledgement");
   // This acknowledges a completed operation, not a particular revision of the library. Automatic captures may
   // replace skeleton.json later; that must never revive the old request still present in control.json.
   boneRefreshDone=static_cast<long long>(n.number);boneRefreshDoneId=unsigned(id.number);
  }catch(const std::exception&){}
  loadVerified();worker.start([this]{workerTick(clockMs());});
 }

 // ---------------------------------------------------------------- worker side
 void workerTick(uint64_t now){
  if(now>=controlAt){controlAt=now+100;try{auto file=shared+"/control.json";if(exists(file)){auto raw=text(file);if(raw!=controlRaw){controlRaw=raw;scanAt=0;
     try{auto c=parseControl(raw);logLevel.store(c.logLevel);std::lock_guard<std::mutex> l(sm);pendingControl=c;controlError.clear();}catch(const std::exception& ex){std::lock_guard<std::mutex> l(sm);controlError=ex.what();}}}}catch(const std::exception&){}}
  if(now>=scanAt){scanAt=now+1000;try{auto list=scanModels(modelRoot,scanCache);std::lock_guard<std::mutex> l(sm);scanned=std::move(list);scannedFresh=true;}catch(const std::exception&){}}
 }
 // urgent: the character is appearing now: its job goes ahead of the queue (once; a queued copy then finds it loaded).
 void requestLoad(const ModelInfo& m,bool urgent=false){
  auto key=m.path+"|"+m.generation;{std::lock_guard<std::mutex> l(sm);auto it=loaded.find(m.path);if(it!=loaded.end()&&it->second.generation==m.generation)return;
   if(inflight.count(key)&&(!urgent||urgentLoads.count(key))){return;}inflight.insert(key);if(urgent)urgentLoads.insert(key);}
  auto v=ctl.variables.find(m.name);std::map<std::string,int> chosen=v==ctl.variables.end()?std::map<std::string,int>{}:v->second;
  auto job=[this,m,key,chosen]{{std::lock_guard<std::mutex> l(sm);auto it=loaded.find(m.path);if(it!=loaded.end()&&it->second.generation==m.generation)return;}
   LoadResult r;r.generation=m.generation;
   try{auto bin=m.path+"/model.bin";auto stamp=stampOf(bin);bool known=false;{std::lock_guard<std::mutex> l(sm);auto c=verifiedBins.find(bin);known=c!=verifiedBins.end()&&c->second==stamp;}
    r.data=loadModel(m,chosen,!known);r.bytes=loadedBytes(*r.data);
    if(!known){std::lock_guard<std::mutex> l(sm);verifiedBins[bin]=stamp;verifiedDirty=true;}}
   catch(const std::exception& ex){r.error=ex.what();}
   std::lock_guard<std::mutex> l(sm);loaded[m.path]=std::move(r);inflight.erase(key);urgentLoads.erase(key);};
  if(urgent)worker.postFront(job);else worker.post(job);
 }
 // model.bin files already checked whole (CRC-32) in an earlier load or session: path -> inode:time:size. A changed file
 // has another stamp and is checked again. Kept in the private folder.
 std::map<std::string,std::string> verifiedBins;bool verifiedDirty=false;std::set<std::string> urgentLoads;
 void loadVerified(){try{auto f=priv+"/verified-models.txt";if(!exists(f))return;std::string t=text(f);size_t p=0;
   while(p<t.size()){auto e=t.find('\n',p);auto line=t.substr(p,e==std::string::npos?std::string::npos:e-p);p=e==std::string::npos?t.size():e+1;auto tab=line.find('\t');if(tab!=std::string::npos)verifiedBins[line.substr(0,tab)]=line.substr(tab+1);}}catch(const std::exception&){}}
 void saveVerified(){std::string body;{std::lock_guard<std::mutex> l(sm);if(!verifiedDirty)return;verifiedDirty=false;for(auto& kv:verifiedBins)body+=kv.first+"\t"+kv.second+"\n";}
  auto file=priv+"/verified-models.txt";worker.post([file,body]{try{writeAtomic(file,body);}catch(...){}});}
 // Every switched-on model that is not built yet has its files read in the background right away, one at a time and up
 // to preloadBudget held: the first appearance can then build in the binding/hand-off hook, before the game shows the
 // character and starts its switch-in effect. Loads for models switched off are dropped.
 static constexpr uint64_t preloadBudget=512ull<<20;bool preloadFull=false;
 void preload(const std::vector<const ModelInfo*>& wanted,const std::set<std::string>& paths){uint64_t held=0;bool busy=false;
  {std::lock_guard<std::mutex> l(sm);for(auto it=loaded.begin();it!=loaded.end();){if(!paths.count(it->first)){it=loaded.erase(it);}else{held+=it->second.bytes;++it;}}busy=!inflight.empty();}
  // The controlled character's models go first and ahead of anything queued; the others one at a time.
  std::vector<const ModelInfo*> order(wanted);auto current=[&](const ModelInfo* m){return m->id&&m->id==ch.config;};
  std::stable_sort(order.begin(),order.end(),[&](const ModelInfo* a,const ModelInfo* b){return current(a)>current(b);});
  for(auto* m:order){if(findSet(m->path,m->generation)||liveSet(*m)||!lodTarget(*m).empty())continue;auto f=failed.find(m->path);if(f!=failed.end()&&f->second.first==m->generation)continue;
   {std::lock_guard<std::mutex> l(sm);auto it=loaded.find(m->path);if(it!=loaded.end()&&it->second.generation==m->generation)continue;}
   bool now=current(m);if(!now&&busy)return;
   if(!now&&held>=preloadBudget){if(!preloadFull){preloadFull=true;log("preload-budget",m->name+": "+std::to_string(held>>20)+" MiB already preloaded; the rest loads when its character appears");}return;}
   preloadFull=false;if(preloadLogged.insert(m->path+"|"+m->generation).second)log("preload",m->name+" "+m->generation+(now?" (controlled character, first)":""));
   requestLoad(*m,now);if(!now)return;busy=true;}}
 std::set<std::string> preloadLogged;
 bool takeLoaded(const ModelInfo& m,LoadResult& out){std::lock_guard<std::mutex> l(sm);auto it=loaded.find(m.path);if(it==loaded.end()||it->second.generation!=m.generation)return false;out=std::move(it->second);loaded.erase(it);return true;}
 // "GIBox Log.txt" next to status.json (readable without root), XXMI Launcher line format (logfmt.hpp). The message
 // starts with the state key; the level comes from the key, lines below control.json "logLevel" are dropped here.
 void log(const std::string& state,const std::string& detail=""){auto level=levelOf(state);if(int(level)<logLevel.load(std::memory_order_relaxed))return;
  auto dir=shared,line=logLine("runtime",level,detail.empty()?state:state+' '+detail);bool rss=state=="animation-stats"||state=="animation-ready";worker.post([dir,line,rss]()mutable{if(rss){std::ifstream status("/proc/self/status");std::string row;while(std::getline(status,row))if(row.compare(0,6,"VmRSS:")==0){line.pop_back();line+=" process"+row+" (worker sample)\n";break;}}appendLog(dir,line);});}
 void writeLater(const std::string& file,const std::string& body){worker.post([file,body]{write(file,body);});}
 // learned.json: what the folder had when it was scanned, plus everything learned for it this session.
 void saveLearned(const ModelInfo& m){Learned l=m.learned;auto s=learned.find(m.path);if(s!=learned.end())for(auto& kv:s->second)l.targets[kv.first]=kv.second;
  auto id=learnedId.find(m.path);if(id!=learnedId.end())l.id=id->second;auto file=m.path+"/learned.json",body=learnedJson(l);worker.post([file,body]{writeAtomic(file,body);});}

 // ---------------------------------------------------------------- first-stage render readback (debug only)
 bool renderDebug()const{return logLevel.load(std::memory_order_relaxed)==int(Level::Debug);}
 uint64_t renderNow()const{return worker.background()?clockMs():bindingNow;}
 void renderSetup(){
  if(!renderDebug()){renderBinding.clear();renderRecorder.reset();renderReader.reset();renderIdentityAt=0;renderFollowEndAt=0;return;}
  if(!renderRecorder){renderReader=std::make_unique<renderprobe::UnityReader>(e.namedExport);
   renderRecorder=std::make_unique<renderprobe::Recorder>([this](const std::string& line){auto dir=shared;worker.post([dir,line]{appendLog(dir,line);});});}
 }
 std::string renderKey(const Attachment& a)const{return a.set->name+"/"+a.set->parts[a.part].name+"/renderer="+hex(a.native);}
 void renderSnapshot(Attachment& a,const std::string& phase){if(!renderDebug())return;
  try{renderSetup();if(!alive(a.renderer)||load<Ptr>(a.renderer+0x10)!=a.native)return;
   Ptr arr=e.get(abi::getMaterials,a.renderer);if(!arr)return;
   // Own all returned managed arrays across the named engine calls, which can trigger GC.
   auto started=std::chrono::steady_clock::now();Owned roots;try{pin(roots,arr);auto original=pointers(a.originalMaterials,64),current=pointers(arr,64);std::vector<unsigned> slots;
    for(auto& row:a.set->parts[a.part].rows){unsigned slot=row.slot;if(slot>=original.size())for(size_t k=original.size();k-->0;)if(alive(original[k])){slot=unsigned(k);break;}slots.push_back(slot);}
    auto data=renderReader->sample(a.renderer,original,current,slots);data["modelCharacter"]=std::to_string(a.set->id);data["controlledCharacter"]=std::to_string(ch.config);
    data["skinId"]=renderprobe::missing("no verified skin identity reader");data["visualStatus"]="unconfirmed";
    data["readbackDurationUs"]=std::to_string(std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now()-started).count());
    renderRecorder->schema(renderReader->descriptions());renderRecorder->capture(renderNow(),renderKey(a),phase,std::move(data),true);
   }catch(...){freeOwned(roots);throw;}freeOwned(roots);
  }catch(const std::exception& ex){if(renderRecorder)renderRecorder->capture(renderNow(),renderKey(a),phase,{{"status",renderprobe::missing(ex.what())}},true);}
 }
 void renderIdentities(uint64_t now){if(!renderDebug()||!renderRecorder||now<renderIdentityAt)return;renderIdentityAt=now+500;
  for(auto& kv:attachments){auto& a=*kv.second;try{if(!alive(a.renderer)||load<Ptr>(a.renderer+0x10)!=a.native)continue;renderprobe::State data;
    data["controlledCharacter"]=std::to_string(ch.config);data["modelCharacter"]=std::to_string(a.set->id);data["mesh"]=hex(e.get(abi::getMesh,a.renderer));
    Ptr arr=e.get(abi::getMaterials,a.renderer);std::string mats;if(arr)for(Ptr m:pointers(arr,64))mats+=(mats.empty()?"":" ")+hex(m);data["materials"]=mats;
    data["parameters"]="event-only readback; not polled";
    renderRecorder->capture(now,renderKey(a)+"/identity","identity",std::move(data));
   }catch(const std::exception& ex){renderRecorder->capture(now,renderKey(a)+"/identity","identity",{{"status",renderprobe::missing(ex.what())}});}}
 }
 RenderBones renderBones(Ptr r){RenderBones b;try{Ptr a=e.get(abi::getBones,r);if(a){for(Ptr bone:pointers(a,512)){Ptr native=bone?load<Ptr>(bone+0x10):0;b.bones.push_back({native,native?load<uint32_t>(native+8):0});}}else b.error="bone array unavailable";}catch(const std::exception& ex){b.error=ex.what();}return b;}
 void renderBindBefore(Ptr native)noexcept{if(!renderDebug()||!renderRecorder)return;
  try{for(auto& kv:attachments)if(kv.second->native==native){auto& b=renderBinding[native];if(!b.depth)b.before=renderBones(kv.first);b.depth++;return;}}catch(...){}}
 void renderBindAfter(Ptr native){if(!renderDebug()||!renderRecorder)return;auto before=renderBinding.find(native);if(before==renderBinding.end())return;
  if(--before->second.depth){return;}auto old=std::move(before->second.before);renderBinding.erase(before);
  for(auto& kv:attachments)if(kv.second->native==native){auto after=renderBones(kv.first);renderprobe::State data;
   data["arrayIdentity"]="get_bones returns snapshots; compare ordered native bone pointer + instance id, not the allocated array address";data["countBefore"]=std::to_string(old.bones.size());data["countAfter"]=std::to_string(after.bones.size());
   data["elementsChanged"]=old.bones!=after.bones?"true":"false";
   auto boneList=[](const auto& list){std::string s;for(auto b:list)s+=(s.empty()?"":" ")+hex(b.first)+"#"+std::to_string(b.second);return s;};
   data["bonesBefore"]=boneList(old.bones);data["bonesAfter"]=boneList(after.bones);
   std::string changed;for(size_t i=0;i<std::max(old.bones.size(),after.bones.size());i++){std::pair<Ptr,uint32_t> x=i<old.bones.size()?old.bones[i]:std::pair<Ptr,uint32_t>{},y=i<after.bones.size()?after.bones[i]:std::pair<Ptr,uint32_t>{};
    if(x!=y)changed+=(changed.empty()?"":"; ")+std::to_string(i)+":"+hex(x.first)+"#"+std::to_string(x.second)+"->"+hex(y.first)+"#"+std::to_string(y.second);}
   data["changedIndices"]=changed;if(!old.error.empty()||!after.error.empty())data["status"]=renderprobe::missing(old.error+" / "+after.error);
   data["interpretation"]="array/element changes alone do not prove a broken skeleton";
   renderRecorder->capture(renderNow(),renderKey(*kv.second)+"/bones","bind-before-after",std::move(data),true);return;}
 }
 void renderTick(uint64_t now){bool fresh=!renderRecorder;renderSetup();if(!renderRecorder)return;
  if(fresh)for(auto& kv:attachments)renderSnapshot(*kv.second,"debug-enabled");
  // Startup counters are historical, not new markers. Disabled-mode markers are acknowledged but never replayed.
  if(renderControlSeen&&ctl.renderMark>renderMarkSeen){renderMarkSeen=ctl.renderMark;renderFollowEndAt=now+5000;renderRecorder->mark(now,uint64_t(renderMarkSeen),renderReader->descriptions());
   if(glowOn()&&glow::trace().armed.load())glowOpen("user mark",{},true,30000,3000,{0});
   for(auto& kv:attachments)renderSnapshot(*kv.second,"user-mark-received");}
  if(renderFollowEndAt&&now>=renderFollowEndAt){for(auto& kv:attachments)renderSnapshot(*kv.second,"user-mark-follow-end");renderFollowEndAt=0;}
  renderIdentities(now);renderRecorder->tick(now);
 }

 // ---------------------------------------------------------------- switch-in glow trace (debug + experiments.glowTrace)
 // The hooks (glow_trace.hpp) keep every write to the objects registered here: each attachment's renderer, its
 // GameObject and avatar root, the game's original materials, our clones (and retired ones) and whatever else the
 // renderer currently shows. Windows: a switch to a character with a model on (1.5 s before to 4 s after), a new
 // attachment, and a user mark (30 s before). At the window's read-back points and around each material refresh the
 // parameters the game was seen writing are read on both the current material and its original, and differences logged.
 struct GlowWindow {unsigned number=0;std::string what;uint64_t t0=0,from=0,to=0;std::set<uint16_t> owners;bool all=false;std::vector<uint64_t> reads;size_t nextRead=0;uint32_t frame=0;};
 std::vector<GlowWindow> glowWindows;unsigned glowNumber=0,glowAuto=0,glowCharacter=0;bool glowWasOn=false,glowNoHook=false;uint64_t glowRegistryAt=0;
 Recent<std::pair<Ptr,Ptr>,std::string> glowNames{4096};Recent<std::pair<Ptr,Ptr>,Ptr> glowAvatars{256};
 bool glowOn()const{return renderDebug()&&ctl.exp.glowTrace;}
 uint64_t glowNow()const{return worker.background()?clockMs():bindingNow;}
 std::string glowName(Ptr p){auto key=std::make_pair(p,load<Ptr>(p+0x10));if(auto* hit=glowNames.find(key))return *hit;return glowNames.put(key,name(p));}
 // The GameObject of the nearest "Avatar_" transform above the renderer's first bone (what the game activates on a switch).
 Ptr glowAvatarOf(Ptr r){auto key=std::make_pair(r,load<Ptr>(r+0x10));if(auto* hit=glowAvatars.find(key))return *hit;Ptr found=0;Owned tmp;
  try{Ptr ba=e.get(abi::getBones,r);if(ba){pin(tmp,ba);auto bones=pointers(ba,512);Ptr t=bones.empty()?0:bones[0];
    for(unsigned i=0;i<24&&alive(t);i++){if(normalize(name(t)).compare(0,7,"avatar_")==0){found=e.get(abi::gameObject,t);break;}t=e.get(abi::getParent,t);}}}
  catch(...){freeOwned(tmp);throw;}freeOwned(tmp);return glowAvatars.put(key,found);}
 uint16_t glowOwner(const Attachment& a){return glow::trace().owner(a.set->name,a.set->parts[a.part].name,a.set->id);}
 void glowRegister(){auto& t=glow::trace();t.beginRefresh();
  for(auto& kv:attachments){auto& a=*kv.second;
   try{if(!alive(a.renderer)||load<Ptr>(a.renderer+0x10)!=a.native)continue;auto& bp=a.set->parts[a.part];uint16_t owner=glowOwner(a);std::string who=a.set->name+"/"+bp.name+" ";
    t.put(a.renderer,glow::Role::Renderer,owner,-1,who+"renderer "+rendererName(a.renderer));
    Ptr go=e.get(abi::gameObject,a.renderer);if(alive(go))t.put(go,glow::Role::GameObject,owner,-1,who+"gameobject "+glowName(go));
    Ptr avatar=glowAvatarOf(a.renderer);if(alive(avatar))t.put(avatar,glow::Role::Avatar,owner,-1,who+"avatar "+glowName(avatar));
    auto originals=pointers(a.originalMaterials,64);
    for(size_t i=0;i<originals.size();i++){if(alive(originals[i]))t.put(originals[i],glow::Role::Original,owner,int16_t(i),who+"original slot "+std::to_string(i)+" "+glowName(originals[i]));}
    for(size_t j=0;j<bp.rows.size();j++){auto it=a.clones.find(bp.rows[j].name+"#"+std::to_string(bp.rows[j].slot));
     if(it!=a.clones.end()&&alive(it->second))t.put(it->second,glow::Role::Clone,owner,int16_t(j),who+"clone row "+std::to_string(j)+" "+glowName(it->second));}
    for(auto& r:a.retired){if(alive(r.first))t.put(r.first,glow::Role::Retired,owner,-1,who+"retired clone "+r.second+" "+glowName(r.first));}
    Ptr arr=e.get(abi::getMaterials,a.renderer);
    if(arr){Owned tmp;try{pin(tmp,arr);auto current=pointers(arr,64);
      for(size_t i=0;i<current.size();i++){Ptr m=current[i];if(!alive(m)||t.keep(m))continue;t.put(m,glow::Role::Current,owner,int16_t(i),who+"current slot "+std::to_string(i)+" "+glowName(m));}}
     catch(...){freeOwned(tmp);throw;}freeOwned(tmp);}
   }catch(const std::exception& ex){log("glow-note","registry: "+std::string(ex.what()));}}
  t.endRefresh();}
 std::set<uint16_t> glowOwnersOf(unsigned character){std::set<uint16_t> out;std::set<Ptr> world;try{for(Ptr r:worldRenderers())world.insert(r);}catch(const std::exception&){}
  for(auto& kv:attachments){auto& a=*kv.second;if((character&&a.set->id==character)||world.count(a.renderer))out.insert(glowOwner(a));}return out;}
 std::string glowOwnerNames(const std::set<uint16_t>& owners){std::string out;for(auto o:owners){glow::Trace::Owner x;if(glow::trace().ownerOfTag(glow::makeTag(glow::Role::None,o,0,0),x))out+=(out.empty()?"":", ")+x.model+"/"+x.part;}return out;}
 void glowOpen(const std::string& what,const std::set<uint16_t>& owners,bool all,uint64_t before,uint64_t after,const std::vector<uint64_t>& reads,bool merge=false){
  uint64_t now=glowNow();
  // Attaching the same objects again within a second (several parts at once): the open attach window is extended.
  for(auto& w:glowWindows)if(merge&&w.what==what&&w.owners==owners&&now<w.t0+1000){w.to=std::max(w.to,now+after);return;}
  if(!all&&++glowAuto>40){if(glowAuto==41)log("glow-note","40 automatic windows this session; from now on only marks open one");return;}
  GlowWindow w;w.number=++glowNumber;w.what=what;w.t0=now;w.from=now>before?now-before:0;w.to=now+after;w.owners=owners;w.all=all;for(auto r:reads)w.reads.push_back(now+r);
  auto ff=glow::trace().frameFn.load();w.frame=ff?uint32_t(ff()):0;
  log("glow-window","#"+std::to_string(w.number)+" "+what+" at frame "+std::to_string(w.frame)+" ("+(all?std::string("all tracked objects"):glowOwnerNames(owners))+"): writes from "+std::to_string(before)+" ms before to "+std::to_string(after)+" ms after");
  glowWindows.push_back(std::move(w));glowProcess(now);}
 void glowProcess(uint64_t now){
  for(size_t i=0;i<glowWindows.size();){auto& w=glowWindows[i];
   while(w.nextRead<w.reads.size()&&now>=w.reads[w.nextRead]){glowReadback(w,"+"+std::to_string(w.reads[w.nextRead]-w.t0)+"ms");w.nextRead++;}
   if(now>=w.to&&w.nextRead>=w.reads.size()){glowSummary(w);glowWindows.erase(glowWindows.begin()+long(i));}else i++;}}
 void glowSummary(const GlowWindow& w){auto& t=glow::trace();bool truncated=false;auto events=t.collect(w.from*1000000ull,w.to*1000000ull,truncated);
  auto keep=[&](uint64_t tag){return w.all?tag!=0:w.owners.count(glow::ownerOf(tag))>0;};std::string counts;
  auto rows=glow::summarize(t,events,keep,w.t0*1000000ull,w.number,80,counts);
  log("glow-window","#"+std::to_string(w.number)+" end: "+counts+(truncated?"; the oldest part was overwritten (ring full)":"")+(rows.empty()?"; nothing was written to these objects":"")
   +(t.dropped.load()?"; "+std::to_string(t.dropped.load())+" records dropped this session":""));
  for(auto& r:rows)log("glow-write",r);}
 void glowReadback(const GlowWindow& w,const std::string& phase){
  for(auto& kv:attachments){auto& a=*kv.second;if(!w.all&&!w.owners.count(glowOwner(a)))continue;glowDiff(a,"#"+std::to_string(w.number)+" "+phase);}}
 struct GlowState {Ptr shader=0;int queue=0;std::set<std::string> keywords;std::map<int32_t,std::string> values;std::map<int32_t,Ptr> textures;std::string error;};
 GlowState glowRead(Ptr m,const std::map<int32_t,uint8_t>& ids,const std::vector<int32_t>& textureIds){GlowState s;Owned tmp;
  try{require(alive(m),"material destroyed");s.shader=e.get(glow::rvaGetShader,m);s.queue=e.call<int32_t>(glow::rvaGetRenderQueue,m,Ptr(0));
   Ptr kw=e.get(glow::rvaGetKeywords,m);if(kw){pin(tmp,kw);for(Ptr x:pointers(kw,256)){if(x)s.keywords.insert(glow::Trace::managedString(x,128));}}
   for(auto& kv:ids){int32_t id=kv.first;uint8_t k=kv.second;if(!(e.call<uint32_t>(glow::rvaHasProperty,m,id,Ptr(0))&1u))continue;std::string v;
    if(k&glow::KindFloat)v+=glow::number(e.call<float>(glow::rvaGetFloat,m,id,Ptr(0)));
    if(k&glow::KindInt)v+=(v.empty()?"":" ")+std::to_string(e.call<int32_t>(glow::rvaGetInt,m,id,Ptr(0)));
    if(k&(glow::KindColor|glow::KindVector)){float x[4]={0,0,0,0};e.call<void>((k&glow::KindColor)?glow::rvaGetColor:glow::rvaGetVector,m,id,x,Ptr(0));v+=(v.empty()?"":" ")+glow::vec(x,4);}
    if(k&glow::KindScaleOffset){float x[4]={0,0,0,0};e.call<void>(glow::rvaGetScaleOffset,m,id,x,Ptr(0));v+=std::string(v.empty()?"":" ")+"st"+glow::vec(x,4);}
    if(!v.empty()){s.values[id]=v;}if(k&glow::KindTexture)s.textures[id]=e.call<Ptr>(glow::rvaGetTexture,m,id,Ptr(0));}
   for(int32_t id:textureIds){if(!s.textures.count(id))s.textures[id]=e.call<Ptr>(glow::rvaGetTexture,m,id,Ptr(0));}}
  catch(const std::exception& ex){s.error=ex.what();}freeOwned(tmp);return s;}
 std::string glowProperty(int32_t id){auto n=glow::trace().property(id);return (n.empty()?std::string("#"):n+"#")+std::to_string(id);}
 std::string glowTexture(Ptr t){if(!t)return "none";try{if(alive(t))return glowName(t)+"@"+hex(t);}catch(const std::exception&){}return hex(t);}
 // The current material of each draw against the game material it was copied from: what differs besides our textures.
 void glowDiff(Attachment& a,const std::string& when){if(a.set->originalMaterials||!alive(a.renderer)||load<Ptr>(a.renderer+0x10)!=a.native)return;
  auto& t=glow::trace();auto& bp=a.set->parts[a.part];std::set<Ptr> ours;
  // Parameters the game set by name (SetFloat("_DissolveValue", ...)): their IDs come from the engine, read back by ID.
  for(auto& n:t.unresolvedNames()){try{Ptr str=e.call<Ptr>(abi::newString,Ptr(0),n.c_str());if(str)t.learnProperty(e.call<int32_t>(glow::rvaPropertyToId,str,Ptr(0)),n);}catch(const std::exception&){}}
  auto ids=t.materialIds();
  for(auto& kv:a.set->textures)for(Ptr x:{kv.second.main,kv.second.light,kv.second.normal})if(x)ours.insert(x);
  Owned tmp;
  try{Ptr arr=e.get(abi::getMaterials,a.renderer);if(!arr){freeOwned(tmp);return;}pin(tmp,arr);auto current=pointers(arr,64),originals=pointers(a.originalMaterials,64);
   for(size_t j=0;j<bp.rows.size()&&j<current.size();j++){Ptr cur=current[j];unsigned slot=bp.rows[j].slot;Ptr src=slot<originals.size()?originals[slot]:0;
    if(!alive(src))for(size_t k=originals.size();k-->0;)if(alive(originals[k])){src=originals[k];slot=unsigned(k);break;}
    if(!alive(cur)||!alive(src)){continue;}
    std::vector<int32_t> textureIds;Ptr idArr=e.get(glow::rvaGetTextureIds,src);
    if(idArr){pin(tmp,idArr);auto n=length(idArr,256);for(uint64_t i=0;i<n;i++)textureIds.push_back(load<int32_t>(idArr+0x20+4*i));}
    auto x=glowRead(cur,ids,textureIds),y=glowRead(src,ids,textureIds);std::string d;unsigned equal=0,replaced=0;auto add=[&](const std::string& v){d+=(d.empty()?"":"; ")+v;};
    if(!x.error.empty()||!y.error.empty())add("read error: "+x.error+" / "+y.error);
    if(x.shader!=y.shader)add("shader "+hex(x.shader)+" vs "+hex(y.shader));else equal++;
    if(x.queue!=y.queue)add("renderQueue "+std::to_string(x.queue)+" vs "+std::to_string(y.queue));else equal++;
    std::string only,missing;for(auto& k:x.keywords)if(!y.keywords.count(k))only+=(only.empty()?"":" ")+k;for(auto& k:y.keywords)if(!x.keywords.count(k))missing+=(missing.empty()?"":" ")+k;
    if(!only.empty()||!missing.empty())add("keywords only here ["+only+"], only on the original ["+missing+"]");else equal++;
    for(auto& kv:x.values){auto o=y.values.find(kv.first);auto other=o==y.values.end()?std::string("absent"):o->second;if(other!=kv.second)add(glowProperty(kv.first)+" "+kv.second+" vs "+other);else equal++;}
    for(auto& kv:y.values)if(!x.values.count(kv.first))add(glowProperty(kv.first)+" absent vs "+kv.second);
    for(auto& kv:x.textures){auto o=y.textures.find(kv.first);Ptr other=o==y.textures.end()?0:o->second;if(other==kv.second){equal++;continue;}if(ours.count(kv.second)){replaced++;continue;}
     add(glowProperty(kv.first)+" texture "+glowTexture(kv.second)+" vs "+glowTexture(other));}
    auto label=t.labelOfObject(cur);
    log("glow-diff",when+" "+a.set->name+"/"+bp.name+" row "+std::to_string(j)+" "+(label.empty()?glowName(cur)+"@"+hex(cur):label)+" vs original slot "+std::to_string(slot)+": "
     +(d.empty()?"no difference":d)+" ("+std::to_string(equal)+" equal, "+std::to_string(replaced)+" GIBox textures, "+std::to_string(ids.size())+" parameters the game wrote, "+std::to_string(x.keywords.size())+" keywords)");}
   std::string custom;for(auto& kv:t.customIds()){if(!(kv.second&glow::KindFloat))continue;custom+=(custom.empty()?"":", ")+glowProperty(kv.first)+"="+glow::number(e.call<float>(glow::rvaGetCustomFloat,a.renderer,kv.first,Ptr(0)));}
   if(!custom.empty())log("glow-diff",when+" "+a.set->name+"/"+bp.name+" renderer custom floats: "+custom);}
  catch(const std::exception& ex){log("glow-note","read-back "+a.set->name+": "+ex.what());}freeOwned(tmp);}
 void glowTick(){auto& t=glow::trace();if(!worker.background())t.testClockMs.store(bindingNow);
  if(!glowOn()){if(glowWasOn){glowWasOn=false;t.armed.store(false);glowWindows.clear();log("glow-note","trace off: nothing is recorded; the engine entries stay hooked until the game restarts");}return;}
  if(!t.everything&&!glowNoHook){if(!e.hook){glowNoHook=true;log("glow-note","no inline hook installer: nothing is recorded");}else log("glow-hooks",glow::install(e));}
  if(!glowWasOn){glowWasOn=true;glowRegistryAt=0;glowCharacter=ch.config;t.armed.store(true);
   log("glow-note","trace on: "+std::to_string(t.hooked)+" engine entries hooked; windows open on a switch to a character with a model, on attaching, and on 标记画面异常");}
  uint64_t now=glowNow();if(now>=glowRegistryAt){glowRegistryAt=now+250;glowRegister();}
  if(ch.config!=glowCharacter){glowCharacter=ch.config;if(ch.entity){auto owners=glowOwnersOf(ch.config);if(!owners.empty())glowOpen("switch-in "+std::to_string(ch.config),owners,false,1500,4000,{0,1500,4000});}}
  glowProcess(now);}

 // ---------------------------------------------------------------- slot textures (debug experiment, experiments.slotTextures)
 // The game's own materials stay on the renderer (as with gameMaterials), so its effects play on what is shown; the mod's
 // textures reach each draw slot i of ours as an override: mode 1 through the renderer's per-material property block i
 // (read back right away and every 2 s: slot-textures-lost when the game dropped it), mode 2 through its per-material
 // custom texture with matID i (no getter exists: not read back, and not removable: switching off writes the material's
 // own texture into it). AllowPerMaterialProp is switched on for the renderer if it was off, and back off afterwards.
 Owned slotRoots;Ptr slotBlock=0;bool slotBlockTried=false;std::map<std::string,int32_t> slotIds;uint64_t slotCheckAt=0;
 Ptr managedName(const char* n){Ptr str=e.call<Ptr>(abi::newString,Ptr(0),n);require(str!=0,"Managed string allocation failed");return str;}
 int32_t slotId(const char* n){auto it=slotIds.find(n);if(it!=slotIds.end())return it->second;int32_t id=e.call<int32_t>(glow::rvaPropertyToId,managedName(n),Ptr(0));slotIds[n]=id;return id;}
 // Our own MaterialPropertyBlock, made with the class of the game's shared one (constructor = native block).
 Ptr slotScratch(){if(slotBlock||slotBlockTried)return slotBlock;slotBlockTried=true;
  try{Ptr shared=e.call<Ptr>(abi::blockShared,Ptr(0));require(shared!=0&&load<Ptr>(shared)!=0,"the game's shared MaterialPropertyBlock is missing");
   Ptr b=e.call<Ptr>(abi::objectNew,load<Ptr>(shared));require(b!=0,"allocation failed");pin(slotRoots,b);e.action(abi::blockCtor,b);require(load<Ptr>(b+0x10)!=0,"native block missing");slotBlock=b;}
  catch(const std::exception& ex){log("slot-textures","no MaterialPropertyBlock of our own: "+std::string(ex.what()));}
  return slotBlock;}
 // Per draw slot: main texture always, light map and normal map under the first property name the game material has.
 std::vector<std::vector<std::pair<int32_t,Ptr>>> slotPlan(Attachment& a,const std::vector<Ptr>& mats){Set& s=*a.set;auto& bp=s.parts[a.part];std::vector<std::vector<std::pair<int32_t,Ptr>>> plan(bp.rows.size());
  for(size_t i=0;i<bp.rows.size()&&i<mats.size();i++){auto& t=s.textures.at(bp.rows[i].name);if(t.original)continue;plan[i].push_back({slotId("_MainTex"),t.main});
   if(t.light){for(auto* n:{"_LightMap","_LightMapTex","_Lightmap","_LightmapTex"})if(e.call<uint32_t>(abi::hasProperty,mats[i],managedName(n),Ptr(0))&1u){plan[i].push_back({slotId(n),t.light});break;}}
   if(t.normal){for(auto* n:{"_BumpMap","_NormalMap","_BaseNormalMap","_NormalTex"})if(e.call<uint32_t>(abi::hasProperty,mats[i],managedName(n),Ptr(0))&1u){plan[i].push_back({slotId(n),t.normal});break;}}}
  return plan;}
 void slotWrite(Attachment& a){Ptr r=a.renderer;
  if(a.slotAllowBefore<0){a.slotAllowBefore=int(e.call<uint32_t>(abi::allowSlotProps,r,Ptr(0))&1u);if(!a.slotAllowBefore)e.op(abi::setAllowSlotProps,r,1);}
  for(size_t i=0;i<a.slotTex.size();i++){if(a.slotTex[i].empty())continue;
   if(a.slotMode==1){Ptr b=slotScratch();require(b!=0,"no MaterialPropertyBlock");e.action(abi::blockClear,b);e.call<void>(abi::getSlotBlock,r,b,int32_t(i),Ptr(0));
    for(auto& x:a.slotTex[i]){e.call<void>(abi::blockSetTexture,b,x.first,x.second,Ptr(0));}e.call<void>(abi::setSlotBlock,r,b,int32_t(i),Ptr(0));}
   else{for(auto& x:a.slotTex[i])e.call<void>(abi::setSlotCustomTexture,r,int32_t(i),x.first,x.second,int32_t(1),Ptr(0));}}}
 // Mode 1: how many of the textures written are in the renderer's blocks now.
 unsigned slotPresent(Attachment& a,unsigned& total){unsigned found=0;total=0;Ptr b=slotScratch();if(!b)return 0;
  for(size_t i=0;i<a.slotTex.size();i++){if(a.slotTex[i].empty())continue;e.action(abi::blockClear,b);e.call<void>(abi::getSlotBlock,a.renderer,b,int32_t(i),Ptr(0));
   for(auto& x:a.slotTex[i]){total++;Ptr got=e.call<Ptr>(abi::blockGetTexture,b,x.first,Ptr(0));if(alive(got)&&load<Ptr>(got+0x10)==load<Ptr>(x.second+0x10))found++;}}
  return found;}
 void slotReport(Attachment& a){unsigned slots=0,textures=0;for(auto& l:a.slotTex){if(!l.empty())slots++;textures+=unsigned(l.size());}std::string check;
  unsigned total=0,found=slotPresent(a,total);
  if(a.slotMode==1)check=", read back "+std::to_string(found)+" of "+std::to_string(total);
  else check=", no getter of its own; the slots' property blocks show "+std::to_string(found)+" of "+std::to_string(total);
  log("slot-textures",a.set->name+"/"+a.set->parts[a.part].name+" on "+rendererName(a.renderer)+": "+(a.slotMode==1?"per-material property block":"per-material custom texture")+", "+std::to_string(slots)+" of "
   +std::to_string(a.slotTex.size())+" draw slots, "+std::to_string(textures)+" textures"+check+"; per-material properties were "+(a.slotAllowBefore==1?"on":"off, switched on"));}
 struct SlotState {int mode=0,allow=-1;std::vector<std::vector<std::pair<int32_t,Ptr>>> tex;};
 SlotState slotTake(Attachment& a){SlotState o{a.slotMode,a.slotAllowBefore,std::move(a.slotTex)};a.slotMode=0;a.slotAllowBefore=-1;a.slotTex.clear();a.slotMats.clear();return o;}
 // Removes overrides written earlier from the renderer as it is now: mode 1 empties the blocks, mode 2 (no remover
 // exists) writes the texture of the material now in each slot into the override.
 void slotRelease(Attachment& a,const SlotState& o){if(!o.mode||!alive(a.renderer)||load<Ptr>(a.renderer+0x10)!=a.native)return;Ptr r=a.renderer;
  try{if(o.mode==1){Ptr b=slotScratch();if(b){e.action(abi::blockClear,b);for(size_t i=0;i<o.tex.size();i++){if(!o.tex[i].empty())e.call<void>(abi::setSlotBlock,r,b,int32_t(i),Ptr(0));}}}
   else{Ptr mats=e.get(abi::getMaterials,r);auto current=mats?pointers(mats,64):std::vector<Ptr>{};
    for(size_t i=0;i<o.tex.size();i++)for(auto& x:o.tex[i]){Ptr own=i<current.size()&&alive(current[i])?e.call<Ptr>(glow::rvaGetTexture,current[i],x.first,Ptr(0)):0;e.call<void>(abi::setSlotCustomTexture,r,int32_t(i),x.first,own,int32_t(1),Ptr(0));}
    log("slot-textures",a.set->name+"/"+a.set->parts[a.part].name+": per-material custom textures cannot be removed; the textures of the materials now shown were written into them");}
   if(o.allow==0)e.op(abi::setAllowSlotProps,r,0);}
  catch(const std::exception& ex){log("slot-textures","clearing "+a.set->name+": "+ex.what());}}
 void slotClear(Attachment& a){slotRelease(a,slotTake(a));}
 // Every 2 s: mode 1 reads its blocks back and writes them again when the game dropped them; both modes report when the
 // game put other materials into the slots (kept: the overrides belong to the slot, not to the material).
 void slotCheck(uint64_t now){if(!ctl.exp.slotTextures||now<slotCheckAt)return;slotCheckAt=now+2000;
  for(auto& kv:attachments){auto& a=*kv.second;if(!a.slotMode||!alive(a.renderer)||load<Ptr>(a.renderer+0x10)!=a.native)continue;std::string where=a.set->name+"/"+a.set->parts[a.part].name;
   try{Ptr mats=e.get(abi::getMaterials,a.renderer);auto current=mats?pointers(mats,64):std::vector<Ptr>{};
    if(current!=a.slotMats){unsigned changed=0;std::string first;
     for(size_t i=0;i<current.size();i++){if(i<a.slotMats.size()&&current[i]==a.slotMats[i])continue;changed++;if(first.empty()&&alive(current[i]))first=" slot "+std::to_string(i)+": "+name(current[i]);}
     a.slotSwaps++;a.slotMats=current;
     if(a.slotSwaps<=10){log("slot-textures",where+": the game put other materials on the renderer, "+std::to_string(changed)+" of "+std::to_string(current.size())+" slots ("+std::to_string(a.slotTex.size())+" ours)"+(first.empty()?"":","+first));}}
    if(a.slotMode!=1){continue;}unsigned total=0,found=slotPresent(a,total);if(found==total){continue;}a.slotLost++;slotWrite(a);
    if(a.slotLost<=10||a.slotLost%30==0)log("slot-textures-lost",where+": "+std::to_string(total-found)+" of "+std::to_string(total)+" textures were gone from the blocks, written again ("+std::to_string(a.slotLost)+" times so far)");}
   catch(const std::exception& ex){log("slot-textures","check "+where+": "+ex.what());}}}

 // ---------------------------------------------------------------- direct textures
 // The mod's textures go into the game's own material of a slot, not into a copy of it, when that material is evidently
 // this renderer's alone: made at runtime (negative instance id; a loaded asset is shared by everything made from it),
 // not carrying a GIBox texture (a game copy of one of our materials), in no other attachment's original palette, and on
 // no other renderer found (FindObjectsOfType sees active renderers only: evidence, not proof); and every draw of ours on
 // that slot uses the same textures. Any other slot is copied as before. What each written property held goes back on
 // every re-apply, restore and detach; a game copy of such a material (same stem, our main texture) is tracked alike.
 static int32_t instanceId(Ptr o){Ptr n=load<Ptr>(o+0x10);return n?int32_t(load<uint32_t>(n+8)):0;}
 bool gibTexture(Ptr t){if(!t)return false;for(auto& s:sets)for(auto& kv:s->textures)if(t==kv.second.main||t==kv.second.light||t==kv.second.normal)return true;return false;}
 const char* firstProperty(Ptr mat,std::initializer_list<const char*> properties){for(auto prop:properties)if(e.call<uint32_t>(abi::hasProperty,mat,managedName(prop),Ptr(0))&1u)return prop;return nullptr;}
 Ptr textureOf(Ptr mat,const char* property){return e.call<Ptr>(glow::rvaGetTexture,mat,slotId(property),Ptr(0));}
 void putTexture(Ptr mat,const char* property,Ptr tx){if(!std::strcmp(property,"_MainTex"))e.set(abi::mainTexture,mat,tx);else e.call<void>(abi::setTexture,mat,managedName(property),tx,Ptr(0));}
 void directPin(Attachment& a,Ptr p){if(alive(p)&&a.directPinned.insert(p).second)pin(a,p);}
 // Slot -> the game material that takes the mod's textures; the decision for every slot of ours goes to `report`.
 std::map<unsigned,Ptr> directPlan(Attachment& a,const std::vector<Ptr>& originals,std::string& report){Set& s=*a.set;auto& bp=s.parts[a.part];
  std::map<unsigned,std::set<std::string>> names;for(auto& row:bp.rows)names[row.slot].insert(row.name);
  std::map<unsigned,std::string> why;std::map<unsigned,Ptr> want;
  for(auto& n:names){unsigned slot=n.first;Ptr m=slot<originals.size()?originals[slot]:0;
   if(!alive(m)){why[slot]="no game material of its own";continue;}
   if(n.second.size()>1){why[slot]="draws with different textures";continue;}
   if(s.textures.at(*n.second.begin()).original){why[slot]="the model keeps the original material";continue;}
   int32_t id=instanceId(m);if(id>=0){why[slot]="a loaded asset (instance id "+std::to_string(id)+")";continue;}
   if(gibTexture(textureOf(m,"_MainTex"))){why[slot]="already carries a GIBox texture";continue;}
   bool other=false;for(auto& kv:attachments){auto& o=*kv.second;if(&o==&a||o.source==a.renderer)continue;if(o.originalMaterials)for(Ptr x:pointers(o.originalMaterials,64))other=other||x==m;for(auto& c:o.materialSources)other=other||c.second==m;}
   if(other){why[slot]="in another attachment's original palette";continue;}
   want[slot]=m;}
  // Renderers the game made from ours (adopted, source == ours) show our mesh with our materials: they share them.
  if(!want.empty()){std::set<Ptr> mine{a.renderer};for(auto& kv:attachments)if(kv.second->source==a.renderer)mine.insert(kv.first);
   std::vector<Ptr> list=allRenderers(bindingNow);for(auto& kv:attachments)list.push_back(kv.first);std::set<Ptr> checked;
   for(Ptr r:list){if(mine.count(r)||!checked.insert(r).second||!alive(r))continue;Ptr mats=e.get(abi::getMaterials,r);if(!mats)continue;
    for(Ptr x:pointers(mats,64))for(auto it=want.begin();it!=want.end();)if(it->second==x){why[it->first]="also on "+rendererName(r);it=want.erase(it);}else ++it;}
   for(auto& w:want)why[w.first]="written (instance id "+std::to_string(instanceId(w.second))+"; "+std::to_string(checked.size())+" other renderers checked)";}
  for(auto& w:why){Ptr m=w.first<originals.size()?originals[w.first]:0;report+=(report.empty()?"":"; ")+std::string("slot ")+std::to_string(w.first)+(alive(m)?" "+name(m):"")+": "+w.second;}
  return want;}
 // Before anything is written its old value is kept (and pinned), so a failed write is put back as well.
 void directWrite(Attachment& a,Ptr m,const MatTex& t){Attachment::Direct d;d.native=load<Ptr>(m+0x10);d.after.push_back({"_MainTex",t.main});
  if(t.light)if(auto* n=firstProperty(m,{"_LightMap","_LightMapTex","_Lightmap","_LightmapTex"}))d.after.push_back({n,t.light});
  if(t.normal)if(auto* n=firstProperty(m,{"_BumpMap","_NormalMap","_BaseNormalMap","_NormalTex"}))d.after.push_back({n,t.normal});
  for(auto& x:d.after){Ptr was=textureOf(m,x.first);directPin(a,was);d.before.push_back({x.first,was});}
  directPin(a,m);a.direct[m]=d;for(auto& x:d.after)putTexture(m,x.first,x.second);}
 // Every material written gets its own textures back (a dead one is skipped); the ledger is emptied.
 void directRestore(Attachment& a,const char* why){if(a.direct.empty())return;unsigned n=0,copies=0;
  for(auto& kv:a.direct){Ptr m=kv.first;auto& d=kv.second;if(!alive(m)||load<Ptr>(m+0x10)!=d.native)continue;for(auto& b:d.before)putTexture(m,b.first,b.second);n++;if(d.copy)copies++;}
  a.direct.clear();if(why&&n)log("direct-textures",a.set->name+"/"+a.set->parts[a.part].name+" ("+why+"): own textures written back into "+std::to_string(n)+" game materials"+(copies?" ("+std::to_string(copies)+" of them game copies)":""));}
 // A material on the renderer that the game copied from one we wrote into (same stem, our main texture) joins that
 // material's ledger. rewrite (each scan): a written one whose main texture the game replaced gets ours again (logged).
 void directTrack(Attachment& a,bool rewrite){bool any=false;for(auto& kv:attachments)any=any||!kv.second->direct.empty();if(!any||!alive(a.renderer)||load<Ptr>(a.renderer+0x10)!=a.native)return;
  std::string where=a.set->name+"/"+a.set->parts[a.part].name;
  for(auto& kv:a.direct){Ptr m=kv.first;auto& d=kv.second;if(!rewrite||d.copy||!alive(m)||load<Ptr>(m+0x10)!=d.native||textureOf(m,"_MainTex")==d.main())continue;
   for(auto& x:d.after)putTexture(m,x.first,x.second);
   if(++a.directLost<=10||a.directLost%30==0)log("direct-textures-lost",where+": the game put its own main texture on "+name(m)+", ours written again ("+std::to_string(a.directLost)+" times so far)");}
  Ptr mats=e.get(abi::getMaterials,a.renderer);if(!mats)return;
  for(Ptr m:pointers(mats,64)){if(!alive(m))continue;bool known=false;for(auto& kv:attachments)known=known||kv.second->direct.count(m)>0||kv.second->materialSources.count(m)>0;if(known)continue;
   Ptr main=textureOf(m,"_MainTex");if(!main)continue;
   for(auto& kv:attachments){auto& o=*kv.second;Ptr from=0;for(auto& w:o.direct)if(!w.second.copy&&w.second.main()==main&&materialStem(name(w.first))==materialStem(name(m))){from=w.first;break;}
    if(!from){continue;}
    auto d=o.direct.at(from);d.native=load<Ptr>(m+0x10);d.copy=true;directPin(o,m);for(auto& b:d.before)directPin(o,b.second);o.direct[m]=d;
    log("direct-textures",where+": the game's copy "+name(m)+" of "+name(from)+" carries our textures; it gets its own back with "+name(from));break;}}}

 // ---------------------------------------------------------------- texture swaps (model.json "textures": a mod's face texture)
 // On PC a texture-only override (FaceHeadDiffuse) gives the game's own face the mod's texture. Here: the renderer of
 // that name under the same character root as an attached part of the set keeps its mesh, and every slot of it showing
 // the game's face texture (the main texture its first textured slot had) shows ours, and so does every other renderer
 // under that root showing the same texture (Brow, Face_Eye: on PC the texture is replaced by its hash wherever the game
 // binds it). A material made at runtime and on
 // no other renderer found gets the texture written in, its own kept; any other one is replaced by a copy holding ours.
 // Each scan repeats this, so a fresh material the game puts on is done again, and a game copy of ours (it carries our
 // texture) is tracked to get the original back. Everything goes back when the set's parts under that root are gone,
 // the model is switched off or released, or the renderer dies.
 struct Swap:Owned {Ptr renderer=0,native=0,root=0,original=0;Set* set=nullptr;size_t index=0;
  std::map<Ptr,std::pair<Ptr,Ptr>> before;std::map<Ptr,Ptr> copyOf;std::set<Ptr> pinned;};   // material -> (native, texture); our copy -> its source
 std::map<Ptr,std::unique_ptr<Swap>> swapped;
 void swapPin(Swap& w,Ptr p){if(alive(p)&&w.pinned.insert(p).second)pin(w,p);}
 std::string swapName(const Swap& w){return w.set->name+"/"+w.set->swaps[w.index].renderer;}
 void swapApply(Swap& w,const std::vector<Ptr>& others){Ptr ours=w.set->textures.at(w.set->swaps[w.index].material).main;
  Ptr mats=e.get(abi::getMaterials,w.renderer);if(!mats)return;auto current=pointers(mats,64);auto next=current;bool changed=false;std::string report;
  for(size_t i=0;i<current.size();i++){Ptr m=current[i];if(!alive(m))continue;Ptr tex=textureOf(m,"_MainTex");
   if(tex==ours){if(!w.before.count(m)&&!w.copyOf.count(m)){swapPin(w,m);w.before[m]={load<Ptr>(m+0x10),w.original};
     log("texture-swap",swapName(w)+": the game's copy "+name(m)+" carries our texture; it gets the original back with the rest");}continue;}
   if(!w.original){if(!alive(tex))continue;w.original=tex;swapPin(w,tex);}
   if(tex!=w.original)continue;
   std::string why;int32_t id=instanceId(m);if(id>=0)why="a loaded asset (instance id "+std::to_string(id)+")";
   else for(Ptr r:others){if(!why.empty())break;if(r==w.renderer||!alive(r))continue;Ptr om=e.get(abi::getMaterials,r);if(!om)continue;for(Ptr x:pointers(om,64))if(x==m){why="also on "+rendererName(r);break;}}
   if(why.empty()){swapPin(w,m);w.before[m]={load<Ptr>(m+0x10),tex};putTexture(m,"_MainTex",ours);report+=(report.empty()?"":"; ")+std::string("slot ")+std::to_string(i)+" "+name(m)+": written (instance id "+std::to_string(id)+")";}
   else{Ptr c=clone(w,m);putTexture(c,"_MainTex",ours);w.copyOf[c]=m;swapPin(w,m);next[i]=c;changed=true;report+=(report.empty()?"":"; ")+std::string("slot ")+std::to_string(i)+" "+name(m)+": copied ("+why+")";}}
  if(changed){Owned tmp;try{e.set(abi::setMaterials,w.renderer,array(tmp,mats,next.data(),next.size(),8));}catch(...){freeOwned(tmp);throw;}freeOwned(tmp);}
  if(!report.empty())log("texture-swap",swapName(w)+" on "+rendererName(w.renderer)+": "+report);}
 // The sources back in place of our copies (while the renderer lives), the original texture back in every game material.
 void swapRestore(Swap& w,const char* why){unsigned n=0,c=0;
  if(alive(w.renderer)&&load<Ptr>(w.renderer+0x10)==w.native){Ptr mats=e.get(abi::getMaterials,w.renderer);if(mats){auto list=pointers(mats,64);bool changed=false;
    for(auto& m:list){auto it=w.copyOf.find(m);if(it!=w.copyOf.end()&&alive(it->second)){m=it->second;changed=true;c++;}}
    if(changed){Owned tmp;try{e.set(abi::setMaterials,w.renderer,array(tmp,mats,list.data(),list.size(),8));}catch(...){freeOwned(tmp);throw;}freeOwned(tmp);}}}
  for(auto& b:w.before)if(alive(b.first)&&load<Ptr>(b.first+0x10)==b.second.first){putTexture(b.first,"_MainTex",b.second.second);n++;}
  w.before.clear();if(n||c)log("texture-swap",swapName(w)+" ("+why+"): original texture back in "+std::to_string(n)+" game materials, "+std::to_string(c)+" copies taken off");}
 // false: a restore failed; that swap stays (and keeps its set) for the next try.
 template<class F>bool swapsEnd(F pick,const char* why){bool done=true;
  for(auto it=swapped.begin();it!=swapped.end();){if(!pick(*it->second)){++it;continue;}
   try{swapRestore(*it->second,why);freeOwned(*it->second);it=swapped.erase(it);}catch(const std::exception& ex){log("texture-swap-error",swapName(*it->second)+": "+ex.what());done=false;++it;}}
  return done;}
 // Each scan: the swaps made are kept up (or end), then the renderers of a swapped name under a root holding a part of
 // a set that swaps textures are taken.
 void swapTick(const std::vector<Ptr>& list){
  std::map<Ptr,Set*> roots;for(auto& kv:attachments){auto& a=*kv.second;if(a.set->swaps.empty()||!alive(kv.first)||load<Ptr>(kv.first+0x10)!=a.native)continue;Ptr root=avatarRoot(kv.first);if(root)roots[root]=a.set;}
  swapsEnd([&](Swap& w){bool lives=alive(w.renderer)&&load<Ptr>(w.renderer+0x10)==w.native;auto r=roots.find(w.root);
   bool keep=lives&&!attachments.count(w.renderer)&&r!=roots.end()&&r->second==w.set;
   if(keep){try{swapApply(w,list);}catch(const std::exception& ex){log("texture-swap-error",swapName(w)+": "+ex.what());}}
   return !keep;},"its parts are gone");
  if(roots.empty())return;
  std::set<std::string> names;for(auto& kv:roots)for(auto& x:kv.second->swaps)names.insert(normalize(x.renderer));
  for(Ptr r:list){if(!alive(r)||swapped.count(r)||attachments.count(r))continue;auto rn=normalize(rendererName(r));if(!names.count(rn))continue;
   Ptr root=avatarRoot(r);auto hit=roots.find(root);if(!root||hit==roots.end())continue;Set& s=*hit->second;size_t k=s.swaps.size();
   for(size_t i=0;i<s.swaps.size();i++)if(normalize(s.swaps[i].renderer)==rn){k=i;break;}
   if(k==s.swaps.size())continue;
   auto w=std::make_unique<Swap>();w->renderer=r;w->native=load<Ptr>(r+0x10);w->root=root;w->set=&s;w->index=k;
   try{pin(*w,r);ensureTextures(s,{s.swaps[k].material},true);swapApply(*w,list);
    if(!w->original)log("texture-swap",swapName(*w)+" on "+rendererName(r)+": no main texture on its materials; left alone");}
   catch(const std::exception& ex){log("texture-swap-error",s.name+": "+ex.what());try{swapRestore(*w,"failed");}catch(const std::exception&){}freeOwned(*w);continue;}
   swapped[r]=std::move(w);}
  // On PC the face texture is replaced by its hash wherever the game binds it, and Brow and Face_Eye show the same
  // texture as Face: every renderer under the root whose material shows the game's face texture takes ours too.
  std::vector<Swap*> faces;for(auto& kv:swapped)if(kv.second->original&&normalize(rendererName(kv.second->renderer))==normalize(kv.second->set->swaps[kv.second->index].renderer))faces.push_back(kv.second.get());
  for(Swap* face:faces)for(Ptr r:list){if(!alive(r)||swapped.count(r)||attachments.count(r)||avatarRoot(r)!=face->root)continue;Ptr om=e.get(abi::getMaterials,r);if(!om)continue;
   bool uses=false;for(Ptr x:pointers(om,64))uses=uses||(alive(x)&&textureOf(x,"_MainTex")==face->original);if(!uses)continue;
   auto w=std::make_unique<Swap>();w->renderer=r;w->native=load<Ptr>(r+0x10);w->root=face->root;w->set=face->set;w->index=face->index;w->original=face->original;
   try{pin(*w,r);swapPin(*w,w->original);swapApply(*w,list);log("texture-swap",swapName(*w)+": "+rendererName(r)+" shows the game's face texture too; it takes ours, as on PC");}
   catch(const std::exception& ex){log("texture-swap-error",face->set->name+": "+ex.what());try{swapRestore(*w,"failed");}catch(const std::exception&){}freeOwned(*w);continue;}
   swapped[r]=std::move(w);}}

 // ---------------------------------------------------------------- engine helpers (game thread)
 std::string name(Ptr p){require(alive(p),"Destroyed Unity object");Ptr s=e.get(abi::getName,p);if(!s)return {};auto n=load<uint32_t>(s+0x10);require(n<=1024,"Unity name bound");std::string out;for(unsigned i=0;i<n;i++){unsigned a=load<uint16_t>(s+0x14+i*2);if(a>=0xd800&&a<=0xdbff){require(i+1<n,"UTF16 name");unsigned b=load<uint16_t>(s+0x14+(++i)*2);require(b>=0xdc00&&b<=0xdfff,"UTF16 name");a=0x10000+((a-0xd800)<<10)+(b-0xdc00);}utf8(out,a);}return out;}
 void pin(Owned& o,Ptr p){if(!p)return;auto h=e.call<uint32_t>(abi::pin,p,2,Ptr(0));require(h!=0,"GC handle allocation failed");o.handles.push_back(h);}
 Ptr own(Owned& o,Ptr p){require(alive(p),"Unity allocation failed");o.objects.push_back(p);pin(o,p);return p;}
 Ptr clone(Owned& o,Ptr p){return own(o,e.get(abi::clone,p));}
 Ptr arrayOf(Owned& o,Ptr cls,const void* data,size_t n,unsigned stride){require(cls&&load<uint16_t>(cls+0xba)==stride,"Managed element size mismatch");Ptr p=e.call<Ptr>(abi::arrayNew,cls,uint64_t(n));require(p!=0,"Managed array allocation failed");pin(o,p);if(n)std::memcpy(reinterpret_cast<void*>(p+0x20),data,n*stride);return p;}
 Ptr array(Owned& o,Ptr templ,const void* data,size_t n,unsigned stride){require(templ!=0,"Missing managed array template");return arrayOf(o,load<Ptr>(templ),data,n,stride);}
 // Managed array classes for the mesh setters (Vector3[], Vector2[], int[], Color32[], Vector4[]), taken once from the
 // first template mesh and kept (il2cpp classes live as long as the process): later builds no longer make the engine copy
 // an original mesh's whole vertex and index data only to obtain an array of the right type.
 struct ArrayClasses {Ptr v3=0,v2=0,i32=0,c32=0,v4=0;bool ready()const{return v3&&v2&&i32&&c32&&v4;}} arrayClasses;
 void learnArrayClasses(Ptr mesh){if(arrayClasses.ready())return;Owned tmp;ArrayClasses c;
  try{auto cls=[&](abi::Offset getter,unsigned stride){Ptr a=e.get(getter,mesh);require(a!=0,"Missing managed array template");pin(tmp,a);Ptr k=load<Ptr>(a);require(k&&load<uint16_t>(k+0xba)==stride,"Managed element size mismatch");return k;};
   c.v3=cls(abi::getPositions,12);c.v2=cls(abi::getUV,8);c.i32=cls(abi::getTriangles,4);c.c32=cls(abi::getColors,4);c.v4=cls(abi::getTangents,16);}
  catch(...){freeOwned(tmp);throw;}freeOwned(tmp);arrayClasses=c;log("array-classes","learned from "+name(mesh));}
 void freeOwned(Owned& o){for(auto i=o.objects.rbegin();i!=o.objects.rend();++i)if(alive(*i))e.action(abi::destroy,*i);for(auto h:o.handles)e.call<void>(abi::unpin,h);o.objects.clear();o.handles.clear();}
 Ptr makeTexture(Owned& o,const Texture& t,bool linear){Ptr descriptor=e.call<Ptr>(abi::textureType);require(descriptor!=0,"Texture type missing");
  // objectNew only allocates the managed shell; its native texture (+0x10) exists once the constructor (textureInit) has run.
  Ptr x=e.call<Ptr>(abi::objectNew,load<Ptr>(descriptor));require(x!=0,"Texture allocation failed");o.objects.push_back(x);pin(o,x);
  e.call<void>(abi::textureInit,x,int(t.w),int(t.h),4,0,linear?1:0,Ptr(0));require(alive(x),"Texture init failed");e.call<void>(abi::texturePixels,x,reinterpret_cast<Ptr>(t.pixels.data()),int(t.pixels.size()),Ptr(0));e.action(abi::textureApply,x);return x;}
 void extraTexture(Ptr mat,Ptr tx,std::initializer_list<const char*> properties){if(auto* prop=firstProperty(mat,properties))e.call<void>(abi::setTexture,mat,managedName(prop),tx,Ptr(0));}
 Character current(){Ptr root=load<Ptr>(e.base+abi::manager);if(!root)return {};Ptr m=load<Ptr>(root+0x8c00);if(!m)return {};unsigned id=load<uint32_t>(m+0x288);Ptr list=load<Ptr>(m+0x10);if(!id||!list)return {};Ptr a=load<Ptr>(list+0x10);unsigned n=load<uint32_t>(list+0x18);if(!a||n>1024||length(a,1024)<n)return {};for(unsigned i=0;i<n;i++){Ptr ent=load<Ptr>(a+0x20+i*8);if(ent&&load<uint32_t>(ent+0x430)==id){unsigned config=load<uint32_t>(ent+0x3a4);if(config>=10000000&&config<=10001999)return {ent,id,config};}}return {};}
 // The renderer owner is null while the character is still loading: no renderers yet, not an error.
 std::vector<Ptr> worldRenderers(){if(!ch.entity)return {};Ptr o=load<Ptr>(ch.entity+0xf8);if(!o)return {};Ptr list=load<Ptr>(o+0x68);if(!list)return {};return pointers(list,64);}
 // Original mesh asset under a renderer, even while one of our meshes is attached to it.
 Ptr originalMesh(Ptr r){auto a=attachments.find(r);if(a!=attachments.end()&&a->second->native==load<Ptr>(r+0x10))return a->second->original;return e.get(abi::getMesh,r);}
 Fingerprint fingerprint(Ptr mesh,bool fresh=false){Ptr native=load<Ptr>(mesh+0x10);auto key=std::make_pair(native,load<uint32_t>(native+8));if(!fresh)if(auto* hit=fpCache.find(key))return *hit;
  Ptr data=load<Ptr>(native+0x48);Fingerprint f;f.mesh=name(mesh);f.vertices=load<uint32_t>(data+0x74);f.submeshes=load<uint32_t>(data+0xc0);f.bones=uint32_t(load<uint64_t>(data+0xe0));
  fpCache.put(key,f);return f;}
 // System.Type of SkinnedMeshRenderer, for FindObjectsOfType. Il2CppClass is laid out differently in this game, so the
 // offset B of byval_arg is found from two independent signs, read from the renderer class and from the element
 // classes of the renderer's own Material[] / Transform[] arrays:
 //  pair:  every class also keeps this_arg, a copy of byval_arg with only the byref bit set, so each class has two
 //         Il2CppTypes at the same offsets (B, T) with equal data/attrs/kind and flags differing by that one bit,
 //         and the data differs between classes (each class has its own type index);
 //  array: an array class's byval_arg points at the element class's byval_arg, i.e. at element+B.
 // A candidate is confirmed with Class::FromIl2CppType on every class before il2cpp creates (and caches) the Type object.
 // Nothing is assumed about the type enum values, the byref bit position or whether B and T are adjacent.
 static constexpr unsigned typeSpan=0x180;
 static bool typePair(Ptr c,unsigned b,unsigned t){Ptr x=c+b,y=c+t;if(load<uint64_t>(x)!=load<uint64_t>(y)||load<uint16_t>(x+8)!=load<uint16_t>(y+8))return false;
  uint8_t k=load<uint8_t>(x+0xa);if(k==0||k>0x1e||k!=load<uint8_t>(y+0xa))return false;uint8_t fb=load<uint8_t>(x+0xb),ft=load<uint8_t>(y+0xb),d=uint8_t(fb^ft);return d&&!(d&(d-1))&&(ft&d);}
 std::string classDump(const char* label,Ptr c){std::string out=std::string(label)+" "+hex(c)+"\n";for(unsigned off=0;off<typeSpan;off+=8)out+=hex(off)+"="+hex(load<uint64_t>(c+off))+(off%32==24?"\n":" ");return out;}
 Ptr rendererType(Ptr renderer){if(rendererTypeObject)return rendererTypeObject;Ptr klass=load<Ptr>(renderer);require(klass!=0,"Renderer class missing");
  std::vector<Ptr> elements,arrays;
  for(abi::Offset getter:{abi::getMaterials,abi::getBones}){Owned tmp;
   try{Ptr arr=e.get(getter,renderer);if(arr){pin(tmp,arr);Ptr ak=load<Ptr>(arr),ek=0;for(Ptr el:pointers(arr,512)){if(el&&(ek=load<Ptr>(el)))break;}
     if(ak&&ek&&ek!=klass&&std::find(elements.begin(),elements.end(),ek)==elements.end()){elements.push_back(ek);arrays.push_back(ak);}}}
   catch(...){freeOwned(tmp);throw;}freeOwned(tmp);}
  std::map<unsigned,unsigned> votes;for(size_t i=0;i<elements.size();i++)for(unsigned b=0;b+16<=typeSpan;b+=8){if(load<Ptr>(arrays[i]+b)==elements[i]+b)votes[b]++;}
  std::map<unsigned,unsigned> pairs;  // B -> T, present in the renderer class and every element class
  if(!elements.empty())for(unsigned b=0;b+16<=typeSpan;b+=8)for(unsigned t=0;t+16<=typeSpan;t+=8){
   if((b<t?t-b:b-t)<16||!typePair(klass,b,t)){continue;}uint8_t kind=load<uint8_t>(klass+b+0xa),bit=uint8_t(load<uint8_t>(klass+t+0xb)^load<uint8_t>(klass+b+0xb));std::set<uint64_t> data{load<uint64_t>(klass+b)};bool ok=true;
   for(Ptr c:elements){if(!typePair(c,b,t)||load<uint8_t>(c+b+0xa)!=kind||uint8_t(load<uint8_t>(c+t+0xb)^load<uint8_t>(c+b+0xb))!=bit||!data.insert(load<uint64_t>(c+b)).second){ok=false;break;}}
   if(ok&&!pairs.count(b))pairs[b]=t;}
  // Both signs first, then a pair, then array votes alone (which still need a plausible, shared kind byte).
  std::vector<std::pair<unsigned,unsigned>> order;std::set<unsigned> all;for(auto& kv:votes)all.insert(kv.first);for(auto& kv:pairs)all.insert(kv.first);
  for(unsigned b:all){unsigned score=(pairs.count(b)?2000u:0u)+(votes.count(b)?1000u+votes[b]:0u);order.push_back({score,b});}std::sort(order.rbegin(),order.rend());
  std::string rejected;
  for(auto& o:order){unsigned b=o.second;uint8_t kind=load<uint8_t>(klass+b+0xa);if(kind==0||kind>0x1e){continue;}bool same=true;for(Ptr c:elements){same=same&&load<uint8_t>(c+b+0xa)==kind;}if(!same){continue;}
   if(e.call<Ptr>(abi::classFromType,klass+b,1)!=klass){rejected+=hex(b)+" ";continue;}bool all_ok=true;for(Ptr c:elements){all_ok=all_ok&&e.call<Ptr>(abi::classFromType,c+b,1)==c;}if(!all_ok){rejected+=hex(b)+"(elements) ";continue;}
   Ptr type=e.call<Ptr>(abi::typeObject,klass+b);require(type!=0&&load<Ptr>(type+0x10)!=0,"Type object missing");
   log("renderer-type","byval_arg at class+"+hex(b)+(pairs.count(b)?" this_arg +"+hex(pairs[b]):std::string(" no this_arg pair"))+", array votes "+std::to_string(votes.count(b)?votes[b]:0)+", "+std::to_string(elements.size()+1)+" classes");
   return rendererTypeObject=type;}
  // Not found: record the classes once so the layout can be read from the log.
  if(!typeDumped){typeDumped=true;std::string v,pr;for(auto& kv:votes)v+=hex(kv.first)+":"+std::to_string(kv.second)+" ";for(auto& kv:pairs)pr+=hex(kv.first)+"/"+hex(kv.second)+" ";
   std::string dump="votes "+v+"\npairs "+pr+"\nrejected "+rejected+"\n"+classDump("renderer",klass);for(size_t i=0;i<elements.size();i++)dump+=classDump(i?"element2":"element1",elements[i])+classDump(i?"array2":"array1",arrays[i]);log("renderer-type-dump",dump);}
  throw std::runtime_error("SkinnedMeshRenderer Il2CppType not found ("+std::to_string(elements.size()+1)+" classes, "+std::to_string(pairs.size())+" pairs, "+std::to_string(votes.size())+" array votes)");}
 // Every active SkinnedMeshRenderer (FindObjectsOfType), plus the world character's own list. Since 0.3.5 this is the
 // fallback (world character, renderers that did not come through the async hand-off), so it runs every 500 ms.
 std::vector<Ptr> allRenderers(uint64_t now){std::vector<Ptr> out;try{out=worldRenderers();}catch(const std::exception& ex){log("world-renderers",ex.what());}
  scanWorld=int(out.size());scanAll=-1;
  if(!findOff&&now>=findAt){Ptr sample=0;for(Ptr r:out)if(alive(r)){sample=r;break;}if(!sample)for(auto& a:attachments)if(alive(a.first)){sample=a.first;break;}
   if(sample||rendererTypeObject){Owned tmp;try{Ptr found=e.call<Ptr>(abi::findObjectsOfType,rendererType(sample),Ptr(0));scanAll=0;if(found){pin(tmp,found);for(Ptr r:pointers(found,8192)){out.push_back(r);scanAll++;}}findFails=0;}
    catch(const std::exception& ex){log("find-error",ex.what());findAt=now+5000;if(++findFails>=3){findOff=true;log("find-disabled","falling back to the world character only");}}freeOwned(tmp);}}
  // World character first: it is the preferred template for builds.
  std::vector<Ptr> unique;std::set<Ptr> once;for(Ptr r:out)if(once.insert(r).second)unique.push_back(r);return unique;}

 // ---------------------------------------------------------------- models
 // Every selected model, in the order it was switched on (control.json "selected"). When two of them replace the same
 // mesh (two mods of one character), the one switched on first shows, also if it finished building later (promote) or
 // the order changed (discover); parts only one of them replaces are shown from each.
 std::vector<const ModelInfo*> chooseModels(){
  std::vector<const ModelInfo*> out;std::set<std::string> taken;
  for(auto& n:ctl.selected)for(auto& m:models)if(m.name==n&&taken.insert(m.path).second){auto v=ctl.variables.find(m.name);
   if(drawn(m.replaceWhen,pick(m.variants,v==ctl.variables.end()?std::map<std::string,int>{}:v->second)))out.push_back(&m);}
  return out;}
 // A selected model left out only because its own switch (replaceWhen) is off.
 bool switchedOff(){for(auto& n:ctl.selected)for(auto& m:models)if(m.name==n){auto v=ctl.variables.find(m.name);if(!drawn(m.replaceWhen,pick(m.variants,v==ctl.variables.end()?std::map<std::string,int>{}:v->second)))return true;}return false;}
 size_t rank(const std::string& path){for(size_t i=0;i<wantedNow.size();i++)if(wantedNow[i]->path==path)return i;return wantedNow.size();}
 bool claims(const Set& s,Ptr original){if(!alive(original))return false;auto fp=fingerprint(original);for(auto& p:s.parts)if(matches(p.target,fp))return true;return false;}
 // Every part of the model is also replaced by a model switched on earlier: building it would show nothing, so it waits
 // (and is built when that one is switched off). Unknown targets count as not covered.
 bool shadowed(const ModelInfo& m,const std::vector<const ModelInfo*>& wanted){std::vector<Fingerprint> mine;if(!targetsFor(m,mine))return false;
  for(auto& t:mine){bool covered=false;for(auto* h:wanted){if(h==&m)break;std::vector<Fingerprint> theirs;if(!targetsFor(*h,theirs))continue;for(auto& x:theirs)covered=covered||matches(x,t);if(covered)break;}if(!covered)return false;}
  return true;}
 static std::string lodTarget(const ModelInfo& m){for(auto& p:m.parts)if(!p.lodMesh.empty())return p.lodMesh;return {};}
 bool outranked(const Attachment& a,const std::vector<Set*>& live){size_t mine=rank(a.set->path);for(Set* s:live)if(s->path!=a.set->path&&rank(s->path)<mine&&claims(*s,a.original))return true;return false;}
 // Lower-case name tokens of the transforms above a renderer's skeleton, e.g. avatar girl catalyst citlali.
 std::vector<std::string> chain(Ptr r,bool fresh=false){auto key=std::make_pair(r,load<Ptr>(r+0x10));if(!fresh)if(auto* hit=chains.find(key))return *hit;std::vector<std::string> out;Owned tmp;
  try{Ptr ba=e.get(abi::getBones,r);if(ba){pin(tmp,ba);auto bones=pointers(ba,512);Ptr t=bones.empty()?0:bones[0];for(int i=0;i<24&&t&&alive(t);i++){for(auto& x:nameTokens(name(t)))out.push_back(x);t=e.get(abi::getParent,t);}}}catch(...){freeOwned(tmp);throw;}
  freeOwned(tmp);chains.put(key,out);return out;}
 // Targets learned this session (also after a game update moved a part) take precedence over the folder's.
 Ptr avatarRoot(Ptr renderer){Owned tmp;Ptr root=0;
  try{Ptr ba=e.get(abi::getBones,renderer);pin(tmp,ba);auto bones=pointers(ba,512);Ptr t=bones.empty()?0:bones[0];
   for(unsigned i=0;i<24&&alive(t);i++){auto n=meshName(name(t));if(n.compare(0,7,"avatar_")==0){root=t;break;}t=e.get(abi::getParent,t);}
  }catch(...){freeOwned(tmp);throw;}freeOwned(tmp);return root;}
 std::string avatarIdentity(Ptr renderer){Ptr root=avatarRoot(renderer);if(!root)return {};auto n=meshName(name(root));return n.substr(0,n.find("_manekin"));}
 bool targetsFor(const ModelInfo& m,std::vector<Fingerprint>& out){out.clear();auto l=learned.find(m.path);
  for(auto& p:m.parts){if(l!=learned.end()&&l->second.count(p.name)&&l->second.at(p.name).bones>=p.minBones)out.push_back(l->second.at(p.name));else if(p.hasTarget&&p.target.bones>=p.minBones)out.push_back(p.target);else return false;}return true;}
 std::string rendererName(Ptr r){auto key=std::make_pair(r,load<Ptr>(r+0x10));if(auto* hit=rendererNames.find(key))return *hit;return rendererNames.put(key,name(r));}
 // Models imported without a bone library: the first renderer whose skeleton chain carries the model's
 // character name and whose renderer name is the part's group defines the part's target (world character first).
 // A model imported for a chosen character ID (model.json "character" holds that character's model name) learns the same way
 // while the character is not on the field (party, detail page), but only from playable-character skeletons
 // (Avatar_...), and keeps its ID.
 void learnByName(const ModelInfo& m){
  std::vector<Fingerprint> t;if(targetsFor(m,t))return;std::vector<Ptr> pool;try{pool=worldRenderers();}catch(const std::exception&){}for(auto& s:seen)pool.push_back(s.renderer);
  unsigned budget=8;bool changed=false;std::set<Ptr> world0;try{for(Ptr r:worldRenderers())world0.insert(r);}catch(const std::exception&){}
  for(auto& p:m.parts){if(p.hasTarget||learned[m.path].count(p.name))continue;
   for(Ptr r:pool){if(!alive(r))continue;if(normalize(rendererName(r))!=normalize(p.group))continue;auto key=std::make_pair(r,load<Ptr>(r+0x10));if(!chains.has(key)){if(!budget)break;budget--;}{auto c=chain(r);if(!namesMatch(m.names,c)||(m.id&&std::find(c.begin(),c.end(),"avatar")==c.end()))continue;}
    Ptr mesh=originalMesh(r);if(!alive(mesh))continue;auto fp=fingerprint(mesh,true);if(lowerLod(fp.mesh)||fp.bones<p.minBones||!usableTemplate({r,mesh,fp},p.minBones))continue;fp.renderer=rendererName(r);learned[m.path][p.name]=fp;changed=true;log("target-learned",m.name+"/"+p.name+" by name "+m.names[0]+" ("+fp.mesh+", "+std::to_string(fp.vertices)+" vertices)");
    if(!m.id&&world0.count(r)&&ch.entity&&!learnedId.count(m.path)){learnedId[m.path]=ch.config;}
    break;}}
  if(changed)saveLearned(m);
 }
 // Folders without usable targets: keep identifying missing parts on the world character until all are ready.
 void learnTargets(const ModelInfo& m){
  if(m.id==0){learnByName(m);return;}
  if(!ch.entity||m.id!=ch.config){if(!m.names.empty())learnByName(m);return;}std::vector<Fingerprint> t;if(targetsFor(m,t)){checkStale(m,t);return;}
  // Only from the full meshes: right after loading the character shows a lower level of detail for a moment.
  struct World {Ptr rendererObject,mesh;std::string renderer,meshName;};std::vector<World> world;bool changed=false;for(Ptr r:worldRenderers()){if(!alive(r))continue;Ptr mesh=originalMesh(r);if(alive(mesh))world.push_back({r,mesh,name(r),name(mesh)});}
  for(auto& w:world){if(lowerLod(w.meshName)){staleNote[m.path]=w.renderer+" 正在使用低精度网格 "+w.meshName+"，等待该角色的完整模型后重新检测骨骼";
    // A detail/party preview can already have the full mesh while the world avatar stays at a lower LOD.
    // Use the existing avatar-name and minimum-palette checks, never the lower-LOD mesh itself.
    if(!m.names.empty()){learnByName(m);if(targetsFor(m,t))staleNote.erase(m.path);}return;}}
  for(auto& p:m.parts){if(p.hasTarget||learned[m.path].count(p.name))continue;std::vector<size_t> hit;for(size_t i=0;i<world.size();i++)if(normalize(p.group)==normalize(world[i].renderer)||normalize(p.group)==normalize(world[i].meshName))hit.push_back(i);
   if(hit.size()!=1){auto why=std::string(hit.empty()?"missing":"ambiguous")+": "+p.group;auto& last=missingLogged[m.path+"|"+p.name];if(last!=why){last=why;log("target-missing",m.name+"/"+p.name+": renderer match "+why);}continue;}
   auto fp=fingerprint(world[hit[0]].mesh,true);if(fp.bones<p.minBones){staleNote[m.path]=p.group+"：MOD 的骨骼索引要求至少 "+std::to_string(p.minBones)+" 根骨骼，当前完整网格只有 "+std::to_string(fp.bones)+" 根；等待该角色的兼容模型，或用对应骨骼记录重新转换";continue;}
   auto& w=world[hit[0]];if(!usableTemplate({w.rendererObject,w.mesh,fp},p.minBones)){staleNote[m.path]=p.group+" 的骨骼或绑定矩阵尚未完整，持续等待并重新检测";continue;}
   fp.renderer=w.renderer;learned[m.path][p.name]=fp;changed=true;log("target-learned",m.name+"/"+p.name);}
  if(changed){saveLearned(m);if(targetsFor(m,t))staleNote.erase(m.path);}
  if(!targetsFor(m,t)&&!m.names.empty()){learnByName(m);if(targetsFor(m,t))staleNote.erase(m.path);}
 }
 // The game changed this character's model (version update): no target matches the world character any more. A part
 // follows the new mesh only with the same renderer and mesh name. Named skeletons are verified and mapped by name;
 // index-only skeletons also need the unchanged palette size. Other outfits are reported instead of guessed.
 static bool lodName(const std::string& n){auto l=n;for(auto& c:l)c=char(std::tolower(static_cast<unsigned char>(c)));return l.find("_lod")!=l.npos;}
 void checkStale(const ModelInfo& m,const std::vector<Fingerprint>& targets){
  struct World {Fingerprint fp;};std::vector<World> world;
  for(Ptr r:worldRenderers()){if(!alive(r))continue;Ptr mesh=originalMesh(r);if(!alive(mesh))continue;auto fp=fingerprint(mesh,true);fp.renderer=rendererName(r);world.push_back({fp});}
  if(world.empty())return;
  std::vector<std::string> changed;bool moved=false;
  for(size_t i=0;i<m.parts.size()&&i<targets.size();i++){auto& t=targets[i];std::vector<size_t> hit;bool present=false;
   bool matched=false;for(auto& w:world)matched=matched||matches(t,w.fp);if(matched)continue;
   bool lod=false;for(auto& w:world){if(normalize(w.fp.renderer)!=normalize(t.renderer))continue;present=true;lod=lod||(meshName(w.fp.mesh)==meshName(t.mesh)&&lodName(w.fp.mesh)!=lodName(t.mesh));}
   if(!present||lod){continue;}  // still loading, or showing another level of detail of the same mesh
   for(size_t j=0;j<world.size();j++)if(normalize(world[j].fp.renderer)==normalize(t.renderer)&&meshName(world[j].fp.mesh)==meshName(t.mesh)&&!lodName(world[j].fp.mesh)&&!lodName(t.mesh))hit.push_back(j);
   if(hit.size()!=1){std::string now;for(auto& w:world)if(normalize(w.fp.renderer)==normalize(t.renderer)){now=w.fp.mesh;break;}changed.push_back(t.renderer+" "+t.mesh+"→"+now);continue;}auto fp=world[hit[0]].fp;
   if(fp.bones!=t.bones){if(m.id==ch.config){auto cause=fingerprintJson(fp),key=m.path+"|"+m.parts[i].name;if(boneRefreshCause[key]!=cause){boneRefreshCause[key]=cause;library.erase(ch.config);libraryRetry[ch.config]=0;boneCandidate.clear();}}
    if(m.parts[i].minBones){changed.push_back(t.renderer+" 骨骼数 "+std::to_string(t.bones)+"→"+std::to_string(fp.bones));continue;}}
   learned[m.path][m.parts[i].name]=fp;moved=true;
   log("target-updated",m.name+"/"+m.parts[i].name+": "+t.mesh+" vertices "+std::to_string(t.vertices)+"->"+std::to_string(fp.vertices)+", submeshes "+std::to_string(t.submeshes)+"->"+std::to_string(fp.submeshes));}
  if(moved){saveLearned(m);Set* old=nullptr;for(auto& s:sets)if(s->path==m.path){old=s.get();break;}if(old)releaseSet(old);failed.erase(m.path);}
  if(changed.empty())staleNote.erase(m.path);
  else{std::string list;for(auto& c:changed)list+=(list.empty()?"":"、")+c;staleNote[m.path]="这个角色当前的模型和导入时不同（换了皮肤，或版本更新改了模型："+list+"），需要对应的 MOD 重新导入";}}
 Set* findSet(const std::string& path,const std::string& gen){for(auto& s:sets)if(s->path==path&&s->generation==gen)return s.get();return nullptr;}
 // Newest built set for a wanted model: the current generation, or the previous one while a reload builds.
 Set* liveSet(const ModelInfo& m){Set* best=nullptr;for(auto& s:sets)if(s->path==m.path&&s->built){if(s->generation==m.generation)return s.get();best=s.get();}return best;}
 bool wantedSet(const Set& s){for(auto* m:wantedNow)if(liveSet(*m)==&s)return true;return false;}
 // One renderer per part, still showing the original asset: the source of bones, bindposes and the mesh to clone.
 bool usableTemplate(const Seen& s,unsigned minBones,bool lodChecked=false){Owned tmp;bool ok=false;
  try{auto fp=fingerprint(s.mesh,true);require((lodChecked||!lowerLod(fp.mesh))&&fp.bones>=minBones,"Template LOD/bone count");
   Ptr bones=e.get(abi::getBones,s.renderer);require(bones!=0,"Template bones missing");pin(tmp,bones);
   Ptr binds=e.get(abi::getBindposes,s.mesh);require(binds!=0,"Template bindposes missing");pin(tmp,binds);
   auto n=length(bones,512);require(n>0&&n>=minBones&&n==fp.bones&&n==length(binds,512),"Template bone/bindpose count");
   for(Ptr b:pointers(bones,512))require(alive(b)&&!name(b).empty(),"Template bone missing/unnamed");
   detail::finite(arrayBytes(binds,64,512),"Template bindposes");ok=true;}
  catch(const std::exception&){}freeOwned(tmp);return ok;}
 // The bone library's record of a character (skeletons/<id>/skeleton.json), re-read when the file changes.
 const std::vector<SkeletonRenderer>* libraryRecord(unsigned id){auto file=boneRoot+"/"+std::to_string(id)+"/skeleton.json";std::error_code ec;
  if(!std::filesystem::exists(file,ec)){libraryRead.erase(id);return nullptr;}auto stamp=stampOf(file);auto it=libraryRead.find(id);
  if(it==libraryRead.end()||it->second.first!=stamp){std::vector<SkeletonRenderer> r;std::string root;try{r=readSkeleton(file,&root);}catch(const std::exception& ex){log("bone-library-read-error",std::to_string(id)+": "+ex.what());}
   libraryRoots[id]=root;it=libraryRead.insert_or_assign(id,std::make_pair(stamp,std::move(r))).first;}
  return it->second.second.empty()?nullptr:&it->second.second;}
 // The avatar a character's bone library was recorded from, in avatarIdentity() form ("avatar_girl_catalyst_varesa"):
 // proves a hidden party member's renderer belongs to the model's character before any set of it is built. "root" is
 // the transform chain from the scene down to the first bone, so like avatarIdentity() the nearest "Avatar_" node above
 // the bone is the one that counts.
 std::map<unsigned,std::string> libraryRoots;
 std::string libraryAvatar(unsigned id){if(!libraryRecord(id))return "";const auto& root=libraryRoots[id];size_t end=root.size();
  while(end>0){auto slash=root.rfind('/',end-1);size_t start=slash==std::string::npos?0:slash+1;auto n=meshName(root.substr(start,end-start));
   if(n.compare(0,7,"avatar_")==0){return n.substr(0,n.find("_manekin"));}if(slash==std::string::npos){break;}end=slash;}
  return "";}
 // Old index-only models may have no embedded skeleton and no separate library, but still carry a mapping recorded
 // during an earlier build. Check EVERY currently used index against it. Unused -1 rows prove nothing and are
 // never filled in. This path does not claim to recover historical bindposes: it validates the live ones and waits
 // for two identical captures, then uses that renderer's own palette, just like an index-only full-mesh build.
 std::optional<Seen> legacyLodTemplate(const ModelInfo& m,const Fingerprint& target,const ModelFile& saved){
  if(m.animated||!ch.entity||m.id!=ch.config){return std::nullopt;}std::set<Ptr> world;for(Ptr r:worldRenderers())world.insert(r);
  for(size_t i=0;i<m.parts.size()&&i<saved.parts.size();i++){const auto& info=m.parts[i];const auto& part=saved.parts[i];
   if(!info.hasTarget||!matches(target,info.target)||!part.sourceBones.empty()){continue;}auto key=m.path+"|"+info.name;auto& probe=legacyLodProbes[key];
   auto report=[&](const std::string& why){if(probe.note!=why){probe.note=why;log("legacy-lod-wait",m.name+"/"+info.name+": "+why);}};
   if(part.legacyBoneNames.size()!=part.spec.bones||part.used.empty()){report(part.legacyBoneError.empty()?"no complete historical index bonemap; waiting for a full mesh or skeleton library":part.legacyBoneError);continue;}
   bool covered=true;for(unsigned index:part.used)covered=covered&&index<part.legacyBoneNames.size()&&!part.legacyBoneNames[index].empty();
   if(!covered){probe.stamp.clear();report("the historical bonemap does not name every bone used by this conversion");continue;}
   for(const auto& candidate:seen){if(!world.count(candidate.renderer)||!alive(candidate.renderer)||!alive(candidate.mesh)||!lowerLod(candidate.fp.mesh)||meshName(candidate.fp.mesh)!=meshName(target.mesh)||candidate.fp.bones!=target.bones||candidate.fp.submeshes!=target.submeshes||normalize(rendererName(candidate.renderer))!=normalize(target.renderer))continue;
    if(attachments.count(candidate.renderer)||skipped.count({candidate.renderer,load<Ptr>(candidate.renderer+0x10)})||e.get(abi::getMesh,candidate.renderer)!=candidate.mesh)continue;
    Owned roots;try{auto current=fingerprint(candidate.mesh,true);require(lowerLod(current.mesh)&&meshName(current.mesh)==meshName(target.mesh)&&current.bones==target.bones&&current.submeshes==target.submeshes,"candidate mesh layout changed");
     if(!m.names.empty()){auto tokens=chain(candidate.renderer,true);require(namesMatch(m.names,tokens)&&std::find(tokens.begin(),tokens.end(),"avatar")!=tokens.end(),"candidate avatar identity differs");}
     Ptr array=e.get(abi::getBones,candidate.renderer);pin(roots,array);auto bones=pointers(array,512);require(bones.size()==target.bones&&bones.size()>=info.minBones,"live bone count differs from the learned full target");
     std::string stamp=m.generation+":"+hex(candidate.renderer)+":"+std::to_string(instanceId(candidate.renderer))+":"+hex(candidate.mesh)+":"+std::to_string(instanceId(candidate.mesh))+":"+describe(current);
     for(size_t n=0;n<bones.size();n++){require(alive(bones[n]),"live bone is missing");auto label=name(bones[n]);require(!label.empty()&&label.size()<=256&&label.find_first_of("\t\r\n")==label.npos,"live bone name is invalid");
      if(part.used.count(unsigned(n)))require(label==part.legacyBoneNames[n],"used bone "+std::to_string(n)+" differs: saved "+part.legacyBoneNames[n]+", live "+label);
      stamp+='\n'+hex(load<Ptr>(bones[n]+0x10))+":"+std::to_string(instanceId(bones[n]))+":"+label;}
     Ptr binds=e.get(abi::getBindposes,candidate.mesh);pin(roots,binds);require(length(binds,512)==bones.size(),"live bone/bindpose count differs");auto matrices=arrayBytes(binds,64,512);detail::finite(matrices,"Legacy LOD live bindposes");stamp.append(reinterpret_cast<const char*>(matrices.data()),matrices.size());freeOwned(roots);
     if(probe.stamp!=stamp){probe.stamp=std::move(stamp);probe.at=bindingNow;report("used bone names match; waiting for a second stable live palette capture");return std::nullopt;}
     if(bindingNow<probe.at+500){return std::nullopt;}auto logKey="legacy|"+m.path+"|"+m.generation+"|"+info.name;
     if(lodLogged.insert(logKey).second){log("legacy-lod",m.name+"/"+info.name+": "+current.mesh+", palette="+std::to_string(bones.size())+", matched-used="+std::to_string(part.used.size())+", source=historical-bonemap, live-bindposes=stable (historical bindposes not recorded)");}probe.note.clear();return Seen{candidate.renderer,candidate.mesh,current};
    }catch(const std::exception& ex){freeOwned(roots);probe.stamp.clear();report(ex.what());}}
  }return std::nullopt;}
 // No full mesh in sight, but the controlled character shows a lower LOD of it whose bones (names, order) and bindposes
 // are exactly those the bone library recorded from the full mesh: the skinning is the same, so it can be the template.
 std::optional<Seen> lodTemplate(const ModelInfo& m,const Fingerprint& t,unsigned minBones){
  if(!m.id||!ch.entity||m.id!=ch.config){return std::nullopt;}auto* lib=libraryRecord(m.id);const SkeletonRenderer* rec=nullptr;SkeletonRenderer embedded;
  if(lib)for(auto& r:*lib)if(matches(t,r.target)||(normalize(r.name)==normalize(t.renderer)&&r.target.mesh==t.mesh&&r.target.vertices==t.vertices&&r.target.bones==t.bones)){rec=&r;break;}
  // The converter also saved the full palette in this model. A missing library file must not discard that proof.
  // Copy only preloaded, CRC-checked data; no file read is added to a renderer hook. Still require exact names,
  // order and bindposes below, as on the main branch's library-backed path.
  std::shared_ptr<ModelFile> saved;
  if(!lib){std::lock_guard<std::mutex> lock(sm);auto it=loaded.find(m.path);if(it!=loaded.end()&&it->second.generation==m.generation)saved=it->second.data;}
  if(!lib&&!saved)if(auto* building=findSet(m.path,m.generation))saved=building->data;
  if(!rec&&saved)for(size_t i=0;i<m.parts.size()&&i<saved->parts.size();i++){auto& part=m.parts[i];auto& data=saved->parts[i];
   if(!part.hasTarget||!matches(t,part.target)||data.sourceBones.size()!=t.bones||data.sourceBinds.size()!=64ull*t.bones)continue;
   embedded.name=t.renderer;embedded.target=t;embedded.bones=data.sourceBones;embedded.parents=data.sourceParents;embedded.bindposes=data.sourceBinds;rec=&embedded;break;}
  if(!rec){return !lib&&saved?legacyLodTemplate(m,t,*saved):std::nullopt;}std::set<Ptr> world;try{for(Ptr r:worldRenderers())world.insert(r);}catch(const std::exception&){return std::nullopt;}
  for(auto& s:seen){if(!world.count(s.renderer)||!alive(s.renderer)||!alive(s.mesh)||!lowerLod(s.fp.mesh)||meshName(s.fp.mesh)!=meshName(t.mesh)||s.fp.bones!=rec->bones.size()||normalize(rendererName(s.renderer))!=normalize(t.renderer))continue;
   if(attachments.count(s.renderer)||skipped.count({s.renderer,load<Ptr>(s.renderer+0x10)})||e.get(abi::getMesh,s.renderer)!=s.mesh||!sameBones(s.renderer,rec->bones))continue;
   Owned tmp;bool same=false;try{Ptr binds=e.get(abi::getBindposes,s.mesh);if(binds){pin(tmp,binds);auto b=arrayBytes(binds,64,512);same=b.size()==rec->bindposes.size()&&std::memcmp(b.data(),rec->bindposes.data(),b.size())==0;}}catch(const std::exception&){same=false;}freeOwned(tmp);
   if(!same||!usableTemplate(s,minBones,true))continue;
   auto key=m.path+"|"+t.renderer;if(lodLogged.insert(key).second)log("template-lod",m.name+": "+t.renderer+" only shows "+s.fp.mesh+"; its bones and bindposes equal "+(rec==&embedded?"the model's saved ":"the library's ")+t.mesh+", building on it");
   return s;}
  return std::nullopt;}
 std::optional<std::vector<Seen>> templatesFor(const ModelInfo& model,const std::vector<Fingerprint>& targets){std::vector<Seen> out;
  for(size_t i=0;i<targets.size();i++){auto& t=targets[i];bool found=false;unsigned min=model.parts[i].minBones;
   // Not a renderer that already failed to attach (another skeleton on the same asset): it would fail the build too.
   for(auto& s:seen)if(matches(t,s.fp)&&alive(s.renderer)&&alive(s.mesh)&&!attachments.count(s.renderer)&&!skipped.count({s.renderer,load<Ptr>(s.renderer+0x10)})&&e.get(abi::getMesh,s.renderer)==s.mesh&&usableTemplate(s,min)){out.push_back(s);found=true;break;}
   if(!found)for(auto& a:attachments)if(alive(a.first)&&alive(a.second->original)&&matches(t,fingerprint(a.second->original))){Seen s{a.first,a.second->original,fingerprint(a.second->original)};if(usableTemplate(s,min)){out.push_back(s);found=true;break;}}
   if(!found)if(auto lod=lodTemplate(model,t,min)){out.push_back(*lod);found=true;}
   if(!found){for(auto& s:seen)if(matches(t,s.fp)&&alive(s.renderer)&&alive(s.mesh)&&!usableTemplate(s,min)){staleNote[model.path]=t.renderer+" 的骨骼或绑定矩阵尚未完整，持续等待并重新检测";break;}return std::nullopt;}}
  staleNote.erase(model.path);
  return out;}

 // ---------------------------------------------------------------- build (one part per tick)
 // Textures are made only for the materials the current variant values draw, when first needed. Pixels already read
 // (preload, or a background read for a variant switch) are used and then dropped; sync=true reads missing ones here.
 // false: the missing ones are requested from the worker and the caller waits for a later tick.
 bool ensureTextures(Set& s,const std::set<std::string>& names,bool sync){std::set<std::string> missing;
  for(auto& n:names){if(s.textures.count(n))continue;auto it=s.mats.find(n);if(it==s.mats.end()){continue;}if(!it->second.loaded())missing.insert(n);}
  if(!missing.empty()){
   if(!sync){requestTextures(s,missing);return false;}
   readTextures(s.path,s.fileGeneration,s.blobSize,s.mats,missing);log("texture-read",s.name+": "+std::to_string(missing.size())+" material(s) read on demand");}
  for(auto& n:names){if(s.textures.count(n))continue;auto it=s.mats.find(n);if(it==s.mats.end())continue;auto& mf=it->second;MatTex t;t.original=mf.original;
   // A swap's texture goes on the game's own mesh, whose UVs the converter did not flip: its rows go back to PC order.
   bool swap=false;for(auto& w:s.swaps)swap|=w.material==n;
   if(!t.original){require(mf.main!=nullptr,"Texture data missing: "+n);if(swap)flipRows(*mf.main);t.main=makeTexture(s,*mf.main,false);if(mf.light)t.light=makeTexture(s,*mf.light,true);if(mf.normal)t.normal=makeTexture(s,*mf.normal,true);
    for(auto* x:{mf.main.get(),mf.light.get(),mf.normal.get()}){if(x)s.bytes+=uint64_t(x->w)*x->h*4;}mf.main.reset();mf.light.reset();mf.normal.reset();}
   s.textures[n]=t;}
  return true;}
 void requestTextures(Set& s,const std::set<std::string>& names){auto key=s.path+"|"+s.generation;{std::lock_guard<std::mutex> l(sm);if(textureInflight.count(key))return;textureInflight.insert(key);}
  std::map<std::string,MaterialFile> want;for(auto& n:names)want[n]=s.mats.at(n);
  auto path=s.path,generation=s.generation,file=s.fileGeneration;auto size=s.blobSize;
  worker.post([this,path,generation,file,size,want,key]()mutable{TextureResult r;r.path=path;r.generation=generation;std::set<std::string> names;for(auto& kv:want)names.insert(kv.first);
   try{readTextures(path,file,size,want,names);r.materials=std::move(want);}catch(const std::exception& ex){r.error=ex.what();}
   std::lock_guard<std::mutex> l(sm);textureResults.push_back(std::move(r));textureInflight.erase(key);});}
 // A set's draws for an assignment, as material names.
 std::set<std::string> drawnBy(const Set& s,const Assignment& a){if(s.data)return materialsDrawn(s.data->parts,a);std::set<std::string> out;std::vector<Submesh> subs;std::vector<Material> rows;
  for(auto& bp:s.parts){activeDraws(bp.draws,a,subs,rows);for(auto& r:rows)out.insert(r.name);}return out;}
 // The draws in effect become the mesh's submeshes (merged per mergeDraws) and bp.rows the material row of each, which
 // applyMaterials puts on the renderer slot by slot. bp.indices stays the model's buffer for the next assignment. Not
 // merged with flags.originalMaterials: the game's palette goes on as it is, submesh i drawing with its material i.
 void setSubmeshes(Set& s,BuiltPart& bp){std::vector<Submesh> drawn;std::vector<Material> rows;activeDraws(bp.draws,s.assignment,drawn,rows);
  auto subs=mergeDraws(drawn,rows,s.mergeDraws);Owned tmp;
  try{e.op(abi::setSubmeshCount,bp.mesh,int(subs.size()));
   for(size_t i=0;i<subs.size();i++){Bytes joined;const uint8_t* src=bp.indices.data()+4ull*subs[i].ranges[0].start;if(subs[i].ranges.size()>1){joined=joinedIndices(bp.indices,subs[i]);src=joined.data();}
    Ptr a=arrayOf(tmp,arrayClasses.i32,src,subs[i].count,4);e.call<void>(abi::setSubmeshTriangles,bp.mesh,a,int(i),Ptr(0));}}catch(...){freeOwned(tmp);throw;}freeOwned(tmp);
  bp.rows.clear();for(auto& d:subs)bp.rows.push_back(d.row);bp.subs=std::move(subs);
  log("submeshes",s.name+"/"+bp.name+": "+std::to_string(drawn.size())+" draws in effect, "+std::to_string(bp.subs.size())+" submeshes"+(s.mergeDraws?"":s.originalMaterials?" (not merged: original materials by position)":" (not merged: experiments.separateDraws)"));}
 // The most common vertex colour of the game's mesh (per channel); the mobile default seen on readable meshes when the
 // game's mesh cannot be read.
 std::array<uint8_t,4> usualColor(Ptr mesh,const std::string& model){std::array<uint8_t,4> out{255,128,128,51};Owned tmp;
  try{Ptr arr=e.get(abi::getColors,mesh);size_t n=arr?length(arr,1u<<24):0;
   if(n){pin(tmp,arr);std::array<std::array<unsigned,256>,4> hist{};const uint8_t* p=reinterpret_cast<const uint8_t*>(arr+0x20);for(size_t v=0;v<n;v++)for(int c=0;c<4;c++)hist[c][p[4*v+c]]++;
    for(int c=0;c<4;c++)out[c]=uint8_t(std::max_element(hist[c].begin(),hist[c].end())-hist[c].begin());}
   else log("experiment-mesh",model+": the game's vertex colours cannot be read, using (255,128,128,51)");}
  catch(const std::exception&){}freeOwned(tmp);return out;}
 BuiltPart makePart(Set& s,const PartFile& pf,const Seen& tpl,size_t partIndex,bool animationFrame=false){Ptr owner=tpl.renderer,ownerMesh=tpl.mesh;require(alive(owner)&&alive(ownerMesh),"Template renderer expired");
  Owned tmp;
  try{
   Ptr boneArray=e.get(abi::getBones,owner);pin(tmp,boneArray);auto bones=pointers(boneArray,512);Ptr binds=e.get(abi::getBindposes,ownerMesh);pin(tmp,binds);require(length(binds,512)==bones.size(),"Target bone/bindpose count");
   std::vector<std::string> target;for(Ptr b:bones)target.push_back(name(b));Skin weights{pf.weights,pf.used};std::vector<int> mapping;
   if(pf.sourceBones.empty()){mapping.assign(pf.spec.bones,-1);for(size_t i=0;i<mapping.size()&&i<bones.size();i++)mapping[i]=int(i);for(unsigned i:weights.used){require(i<bones.size(),"Model uses bone index "+std::to_string(i)+" (needs at least "+std::to_string(i+1)+" bones), but this template has "+std::to_string(bones.size())+"; a matching skeleton is required");mapping[i]=int(i);}}
   else{std::vector<int> parents;for(Ptr b:bones){Ptr up=alive(b)?e.get(abi::getParent,b):0;auto at=std::find(bones.begin(),bones.end(),up);parents.push_back(up&&at!=bones.end()?int(at-bones.begin()):-1);}
    auto bm=mapBones(pf.sourceBones,target,pf.sourceBinds,arrayBytes(binds,64,512),weights.used,pf.sourceParents,parents);mapping=bm.target;
    for(auto& mv:bm.moved){unsigned n=0;for(size_t v=0;v+32<=weights.data.size();v+=32)for(int j=0;j<4;j++)if(f32(weights.data.data()+v+4*j)>0&&u32(weights.data.data()+v+16+4*j)==mv.first){n++;break;}
     log("bone-fallback",s.name+"/"+pf.spec.name+": "+pf.sourceBones[mv.first]+" -> "+target[size_t(mv.second)]+" ("+std::to_string(n)+" vertices; the game's skeleton has no such bone)");}}
   remap(weights,mapping);BuiltPart bp;bp.animationOwner=owner;bp.boneMapping=mapping;bp.targetBinds=arrayBytes(binds,64,512);
   if(!pf.sourceBones.empty())for(Ptr bone:bones){Ptr up=e.get(abi::getParent,bone);auto at=std::find(bones.begin(),bones.end(),up);bp.targetParents.push_back(at==bones.end()?-1:int(at-bones.begin()));}
   if(s.parts.empty())ensureTextures(s,drawnBy(s,s.assignment),true);
   if(!animationFrame){std::string report="source_index\ttarget_index\tsource_name\ttarget_name\n";for(size_t i=0;i<mapping.size();i++){
     int observed=pf.sourceBones.empty()&&!pf.used.count(unsigned(i))?-1:mapping[i];report+=std::to_string(i)+'\t'+std::to_string(observed)+'\t'+(pf.sourceBones.empty()?"#"+std::to_string(i):pf.sourceBones[i])+'\t'+(observed>=0?target[size_t(observed)]:"")+"\n";}
    writeLater(s.path+"/bonemap-"+pf.spec.name+".tsv",report);}
   bp.name=pf.spec.name;bp.avatar=avatarIdentity(owner);bp.target=s.targets[partIndex];bp.bones=target;bp.draws=pf.draws;bp.indices.assign(pf.geo.idx(),pf.geo.idx()+4ull*pf.geo.ni);
   // A GC pin protects the managed wrapper, not the native Mesh against Unity Destroy. Keep an independent,
   // unmodified engine clone before replacing anything. It is owned separately from the mod set so a restored
   // renderer can continue using it after the mod is disabled/released.
   if(!animationFrame){auto backup=std::make_unique<RestoreBackup>();try{backup->mesh=clone(*backup,ownerMesh);}catch(...){freeOwned(*backup);throw;}
   bp.backup=backup.get();restoreBackups.push_back(std::move(backup));
   s.bytes+=uint64_t(fingerprint(ownerMesh).vertices)*(12+12+8+32+32);}
   const auto& geo=pf.geo;bp.mesh=clone(s,ownerMesh);
   Ptr native=load<Ptr>(bp.mesh+0x10),data=load<Ptr>(native+0x48);require(data!=0&&load<uint64_t>(data+0xe0)==bones.size()&&load<uint32_t>(data+0xf8)==15,"Native Mesh layout");
   learnArrayClasses(ownerMesh);const auto& k=arrayClasses;
   e.op(abi::clear,bp.mesh,1);e.op(abi::setIndexFormat,bp.mesh,geo.n>65535?1:0);e.set(abi::setTriangles,bp.mesh,arrayOf(tmp,k.i32,nullptr,0,4));
   e.set(abi::setPositions,bp.mesh,arrayOf(tmp,k.v3,geo.pos(),geo.n,12));e.set(abi::setNormals,bp.mesh,arrayOf(tmp,k.v3,geo.norm(),geo.n,12));e.set(abi::setUV,bp.mesh,arrayOf(tmp,k.v2,geo.uv(),geo.n,8));
   std::array<uint8_t,4> neutral{255,128,128,51};if(ctl.exp.neutralColors)neutral=usualColor(ownerMesh,s.name);
   bool colorsSet=false,uv2Set=false;
   for(auto& c:pf.channels){abi::Offset setter;Ptr cls;unsigned stride;const uint8_t* src=c.second.data();Bytes replaced;
    if(c.first==".uv2"){setter=abi::setUV2;cls=k.v2;stride=8;uv2Set=true;if(ctl.exp.noUv2){replaced.assign(size_t(geo.n)*8,0);src=replaced.data();}}
    else if(c.first==".uv3"){setter=abi::setUV3;cls=k.v2;stride=8;}else if(c.first==".uv4"){setter=abi::setUV4;cls=k.v2;stride=8;}
    else if(c.first==".colors"){setter=abi::setColors;cls=k.c32;stride=4;colorsSet=true;if(ctl.exp.neutralColors){replaced.resize(size_t(geo.n)*4);for(size_t v=0;v<geo.n;v++)std::memcpy(replaced.data()+4*v,neutral.data(),4);src=replaced.data();}}
    else{setter=abi::setTangents;cls=k.v4;stride=16;}
    e.set(setter,bp.mesh,arrayOf(tmp,cls,src,geo.n,stride));}
   if(ctl.exp.neutralColors&&!colorsSet){Bytes c(size_t(geo.n)*4);for(size_t v=0;v<geo.n;v++)std::memcpy(c.data()+4*v,neutral.data(),4);e.set(abi::setColors,bp.mesh,arrayOf(tmp,k.c32,c.data(),geo.n,4));}
   if(ctl.exp.noUv2&&!uv2Set){Bytes z(size_t(geo.n)*8,0);e.set(abi::setUV2,bp.mesh,arrayOf(tmp,k.v2,z.data(),geo.n,8));}
   if(ctl.exp.neutralColors||ctl.exp.noUv2)log("experiment-mesh",s.name+"/"+pf.spec.name+(ctl.exp.neutralColors?" colours=("+std::to_string(neutral[0])+","+std::to_string(neutral[1])+","+std::to_string(neutral[2])+","+std::to_string(neutral[3])+")":"")+(ctl.exp.noUv2?" uv2=0":""));
   e.set(abi::setTriangles,bp.mesh,arrayOf(tmp,k.i32,geo.idx(),geo.ni,4));setSubmeshes(s,bp);
   data=load<Ptr>(load<Ptr>(bp.mesh+0x10)+0x48);require(data&&load<uint32_t>(data+0x74)==geo.n&&load<uint64_t>(data+0xe0)==bones.size()&&load<uint32_t>(data+0xf8)==15,"Mesh layout changed");
   require(data!=load<Ptr>(load<Ptr>(ownerMesh+0x10)+0x48),"Cloned Mesh still shares native data with original");
   // Skin weights (32 bytes per vertex): a new block from the engine allocator with the skin label replaces the clone's
   // skin array {data +0xf0, label +0xf8, size +0x100, capacity +0x108}. The clone's previous block is not returned
   // (its deallocator is unconfirmed); the engine frees ours with the mesh.
   Ptr allocator=load<Ptr>(e.base+abi::nativeAllocator);require(allocator!=0,"Engine allocator missing");
   Ptr weight=e.call<Ptr>(abi::nativeAlloc,allocator,Ptr(0),uint64_t(weights.data.size()),16,15,0,"gibox-weights",1);require(weight!=0,"Weight buffer allocation failed");
   std::memcpy(reinterpret_cast<void*>(weight),weights.data.data(),weights.data.size());store<Ptr>(data+0xf0,weight);store<uint64_t>(data+0x100,geo.n);store<uint64_t>(data+0x108,geo.n);
   e.action(abi::meshFinish,bp.mesh);e.op(abi::meshUpload,bp.mesh,0);
   freeOwned(tmp);return bp;
  }catch(...){freeOwned(tmp);throw;}
 }
 void buildStep(Set& s,const Seen& tpl){size_t part=s.parts.size();s.parts.push_back(makePart(s,s.data->parts[part],tpl,part));log("direct-mesh-prepared",s.name+"/"+s.parts.back().name);
  if(s.parts.size()==s.data->parts.size()){s.built=true;s.data.reset();log("model-built",s.name+" "+s.generation);}
 }

 const char* animationMode(const Set& s)const{return s.animation.mode==0?"cached":s.animation.mode==1?"dynamic":"paused";}
 bool animationMesh(const Set& s,size_t part,Ptr mesh)const{const auto& a=s.animation;if(part>=a.owned.size())return false;const auto& list=a.owned[part];return std::find(list.begin(),list.end(),mesh)!=list.end();}
 Seen animationTemplate(Set& s,size_t part){auto& bp=s.parts.at(part);Seen tpl;tpl.renderer=bp.animationOwner;
  if(!alive(tpl.renderer))for(auto& kv:attachments)if(kv.second->set==&s&&kv.second->part==part&&alive(kv.first)){tpl.renderer=kv.first;break;}
  require(alive(tpl.renderer)&&bp.backup&&alive(bp.backup->mesh),"Animation template not available; wait for the character");tpl.mesh=bp.backup->mesh;return tpl;}
 void remapAnimation(Set& s,ModelFile& data){require(data.parts.size()==s.parts.size(),"Animation part count changed");
  for(size_t i=0;i<data.parts.size();i++){auto& p=data.parts[i];auto& bp=s.parts[i];std::vector<int> mapping;
   if(p.sourceBones.empty()){mapping.assign(p.spec.bones,-1);for(size_t n=0;n<mapping.size()&&n<bp.bones.size();n++)mapping[n]=int(n);}
   else mapping=mapBones(p.sourceBones,bp.bones,p.sourceBinds,bp.targetBinds,p.used,p.sourceParents,bp.targetParents).target;
   Skin skin{std::move(p.weights),p.used};remap(skin,mapping);p.weights=std::move(skin.data);}}
 void requestAnimation(Set& s,size_t index){auto& a=s.animation;require(!a.loading&&index<a.cpu.size(),"Animation request state");a.loading=true;
  auto path=s.path,generation=s.generation,file=s.fileGeneration;auto token=a.token;
  worker.post([this,path,generation,file,index,token]{AnimationResult result;result.path=path;result.generation=generation;result.index=index;result.token=token;auto started=std::chrono::steady_clock::now();
   try{result.data=loadModelFile(path,false,false,int(index),file);}catch(const std::exception& ex){result.error=ex.what();}
   result.us=uint64_t(std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now()-started).count());std::lock_guard<std::mutex> lock(sm);animationResults.push_back(std::move(result));});}
 void animationPrepare(uint64_t now){bool meshBudget=true;
  for(auto& ptr:sets){auto& s=*ptr;auto& a=s.animation;if(!s.built||!a.loop.spec.present()||a.ready||a.stopped||a.mode==2||!wantedSet(s))continue;
   try{
    require(a.mode!=1||a.loop.spec.stableWeights,"Dynamic mode needs identical frame weights; use cached mode for this animation");
    if(a.meshes[0].empty()){for(size_t i=0;i<s.parts.size();i++){a.meshes[0].push_back(s.parts[i].mesh);a.owned[i].push_back(s.parts[i].mesh);}a.preparing=1;
     log("animation-load",s.name+" mode="+animationMode(s)+" frames="+std::to_string(a.cpu.size())+" geometryMiB="+std::to_string(a.loop.spec.bytes>>20)+" stableWeights="+(a.loop.spec.stableWeights?"yes":"no")+" frameCountRva="+hex(abi::frameCount));}
    if(a.mode==1&&a.backPart<s.parts.size()){
     if(!meshBudget){continue;}meshBudget=false;size_t i=a.backPart;auto tpl=animationTemplate(s,i);
     auto part=makePart(s,a.cpu[0]->parts[i],tpl,i,true);a.back[i]=part.mesh;a.owned[i].push_back(part.mesh);
     e.action(abi::meshDynamic,part.mesh);e.action(abi::meshDynamic,s.parts[i].mesh);a.backPart++;if(a.backPart==s.parts.size())remapAnimation(s,*a.cpu[0]);continue;}
    if(a.preparing>=a.cpu.size()){if(a.mode==0){for(auto& frame:a.meshes)for(Ptr mesh:frame)e.op(abi::meshUpload,mesh,1);for(auto& data:a.cpu)data.reset();}
     a.ready=true;a.loop.clock=false;a.lastStats=now;s.bytes+=a.loop.spec.bytes;
     log("animation-ready",s.name+" mode="+animationMode(s)+" frames="+std::to_string(a.cpu.size())+" warmupMs="+std::to_string(now-a.started)+" estimatedFrameGeometryMiB="+std::to_string(a.loop.spec.bytes>>20)+animationMemory(s)+" (driver/native overhead not measured)");continue;}
    size_t frame=a.preparing;
    if(!a.cpu[frame]){if(!a.loading)requestAnimation(s,frame);continue;}
    if(a.mode==1){remapAnimation(s,*a.cpu[frame]);for(size_t i=0;i<s.parts.size();i++){auto& weights=a.cpu[frame]->parts[i].weights;require(weights==a.cpu[0]->parts[i].weights,"Animation target weights differ after permutation/remap; use cached mode");Bytes().swap(weights);}a.preparing++;continue;}
    if(!meshBudget){continue;}meshBudget=false;size_t i=a.buildPart;auto tpl=animationTemplate(s,i);auto before=std::chrono::steady_clock::now();auto part=makePart(s,a.cpu[frame]->parts[i],tpl,i,true);
    require(part.rows.size()==s.parts[i].rows.size(),"Animation material slot count changed");for(size_t n=0;n<part.rows.size();n++)require(part.rows[n].name==s.parts[i].rows[n].name&&part.rows[n].slot==s.parts[i].rows[n].slot,"Animation material layout changed");
    a.meshes[frame].push_back(part.mesh);a.owned[i].push_back(part.mesh);a.buildPart++;
    auto us=std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now()-before).count();log("animation-build",s.name+" frame="+std::to_string(a.loop.spec.frames[frame].value)+" part="+std::to_string(i)+" us="+std::to_string(us));
    if(a.buildPart==s.parts.size()){a.buildPart=0;a.cpu[frame].reset();a.preparing++;}
   }catch(const std::exception& ex){a.stopped=true;a.error=ex.what();log("animation-error",s.name+" mode="+animationMode(s)+" prepare: "+ex.what());}
  }}
 Ptr animationArray(BuiltPart& bp,const std::string& key,Ptr cls,const void* data,unsigned count,unsigned stride){auto it=bp.dynamicArrays.find(key);Ptr array=it==bp.dynamicArrays.end()?0:it->second;
  require(!array||length(array,model::maxIndices)==count,"Animation managed array shape");if(!array){if(data)array=arrayOf(bp.dynamicRoots,cls,data,count,stride);else{require(cls&&load<uint16_t>(cls+0xba)==stride,"Animation array element size");array=e.call<Ptr>(abi::arrayNew,cls,uint64_t(count));require(array!=0,"Animation array allocation failed");pin(bp.dynamicRoots,array);}bp.dynamicArrays[key]=array;}
  else if(count&&data){std::memcpy(reinterpret_cast<void*>(array+0x20),data,size_t(count)*stride);}
  return array;}
 void updateDynamic(Set& s,size_t i,Ptr mesh,const PartFile& p){auto& bp=s.parts[i];const auto& geo=p.geo;require(alive(mesh),"Animation staging mesh expired");
  Ptr data=load<Ptr>(load<Ptr>(mesh+0x10)+0x48);require(data&&load<uint32_t>(data+0x74)==geo.n&&load<uint64_t>(data+0xe0)==bp.bones.size()&&load<uint32_t>(data+0xf8)==15,"Animation native Mesh layout");
  Ptr weights=load<Ptr>(data+0xf0);require(weights&&load<uint64_t>(data+0x108)>=geo.n&&s.animation.cpu[0]->parts[i].weights.size()==size_t(geo.n)*32,"Animation weight capacity");
  std::vector<Submesh> draws;std::vector<Material> rows;activeDraws(p.draws,s.assignment,draws,rows);auto subs=mergeDraws(draws,rows,s.mergeDraws);
  require(subs.size()==bp.rows.size(),"Animation material slot count");std::string shape=std::to_string(geo.n);for(auto& ch:p.channels)shape+=ch.first;for(size_t n=0;n<subs.size();n++){require(subs[n].row.name==bp.rows[n].name&&subs[n].row.slot==bp.rows[n].slot,"Animation material layout");shape+='|'+std::to_string(subs[n].count);}
  if(bp.dynamicShape!=shape){freeOwned(bp.dynamicRoots);bp.dynamicArrays.clear();bp.dynamicShape=shape;}
  const auto& cls=arrayClasses;e.set(abi::setPositions,mesh,animationArray(bp,"pos",cls.v3,geo.pos(),geo.n,12));e.set(abi::setNormals,mesh,animationArray(bp,"norm",cls.v3,geo.norm(),geo.n,12));e.set(abi::setUV,mesh,animationArray(bp,"uv",cls.v2,geo.uv(),geo.n,8));
  for(auto& channel:p.channels){abi::Offset setter;Ptr type;unsigned stride;const uint8_t* source=channel.second.data();Bytes changed;
   if(channel.first==".uv2"){setter=abi::setUV2;type=cls.v2;stride=8;if(ctl.exp.noUv2){changed.assign(size_t(geo.n)*8,0);source=changed.data();}}
   else if(channel.first==".uv3"){setter=abi::setUV3;type=cls.v2;stride=8;}else if(channel.first==".uv4"){setter=abi::setUV4;type=cls.v2;stride=8;}
   else if(channel.first==".colors"){setter=abi::setColors;type=cls.c32;stride=4;if(ctl.exp.neutralColors){auto color=usualColor(bp.backup->mesh,s.name);changed.resize(size_t(geo.n)*4);for(size_t v=0;v<geo.n;v++)std::memcpy(changed.data()+v*4,color.data(),4);source=changed.data();}}
   else{setter=abi::setTangents;type=cls.v4;stride=16;}
   e.set(setter,mesh,animationArray(bp,channel.first,type,source,geo.n,stride));}
  e.op(abi::setSubmeshCount,mesh,int(subs.size()));for(size_t n=0;n<subs.size();n++){Ptr array=animationArray(bp,"indices"+std::to_string(n),cls.i32,nullptr,subs[n].count,4);auto* dst=reinterpret_cast<uint8_t*>(array+0x20);size_t written=0;for(auto& range:subs[n].ranges){require(4ull*(uint64_t(range.start)+range.count)<=geo.indices.size(),"Animation draw range");std::memcpy(dst+written,geo.idx()+4ull*range.start,4ull*range.count);written+=4ull*range.count;}e.call<void>(abi::setSubmeshTriangles,mesh,array,int(n),0,Ptr(0));}
  Ptr after=load<Ptr>(load<Ptr>(mesh+0x10)+0x48);require(after==data&&load<Ptr>(after+0xf0)==weights&&load<uint64_t>(after+0x108)>=geo.n,"Animation weight storage changed during mesh update");
  // Weight order is verified once for every frame during warm-up. The two native weight buffers remain immutable.
  e.action(abi::meshFinish,mesh);e.op(abi::meshUpload,mesh,0);
 }
 void applyAnimation(Set& s,size_t frame){auto& a=s.animation;std::vector<Ptr> next=a.mode==0?a.meshes.at(frame):a.back;require(next.size()==s.parts.size(),"Animation mesh cache incomplete");
  if(a.mode==1){require(a.cpu[frame]!=nullptr,"Animation CPU frame missing");for(size_t i=0;i<s.parts.size();i++)updateDynamic(s,i,next[i],a.cpu[frame]->parts[i]);}
  std::vector<std::pair<Ptr,Ptr>> changed;
  try{for(auto& kv:attachments){auto& attachment=*kv.second;if(attachment.set!=&s||!alive(attachment.renderer)||load<Ptr>(attachment.renderer+0x10)!=attachment.native)continue;Ptr old=e.get(abi::getMesh,attachment.renderer);
    if(!animationMesh(s,attachment.part,old)){continue;}changed.push_back({attachment.renderer,old});e.set(abi::setMesh,attachment.renderer,next[attachment.part]);}}
  catch(...){for(auto& old:changed)try{e.set(abi::setMesh,old.first,old.second);}catch(const std::exception&){}throw;}
  for(size_t i=0;i<s.parts.size();i++){Ptr old=s.parts[i].mesh;s.parts[i].mesh=next[i];if(a.mode==1)a.back[i]=old;}a.applied=frame;
 }
 std::string animationMemory(const Set& s)const{uint64_t cpu=0;size_t meshes=0;for(auto& frame:s.animation.cpu)if(frame)cpu+=loadedBytes(*frame);for(auto& part:s.animation.owned)meshes+=part.size();return " cpuFrameMiB="+std::to_string(cpu>>20)+" meshInstances="+std::to_string(meshes);}
 // experiments.logicCore: the logic of a set that has logic data starts with the switch and stops with it. An
 // animation's first selection must match a prebuilt frame; an outfit model's logic must also be finite (ImportScan)
 // and take the App's outfit values. Any failure drops it for good: FrameLoop carries on from the frame shown, an
 // outfit model goes back to the App's variable table.
 bool animated(const Set& s)const{return s.animation.loop.spec.present();}
 const char* fallback(const Set& s)const{return animated(s)?"FrameLoop":"the App's outfit choice";}
 std::string choiceText(const Set& s,int i)const{if(animated(s))return "frame value "+std::to_string(s.animation.loop.spec.frames[size_t(i)].value);std::string t;for(auto& kv:s.logicSpec.assignments[size_t(i)])t+=(t.empty()?"":" ")+kv.first+"="+std::to_string(kv.second);return "outfit "+t;}
 std::string shownText(const Set& s)const{if(animated(s))return "frame value "+std::to_string(s.animation.loop.spec.frames[s.animation.applied].value);std::string t;for(auto& kv:s.assignment)t+=(t.empty()?"":" ")+kv.first+"="+std::to_string(kv.second);return "outfit "+t;}
 const std::map<std::string,int>& appChoice(const Set& s)const{static const std::map<std::string,int> none;auto v=ctl.variables.find(s.name);return v==ctl.variables.end()?none:v->second;}
 // The App's outfit values the logic has not seen yet; false when the logic has no such variable.
 bool pushChoice(Set& s,std::string& missing){auto& L=s.logic;bool ok=true;
  for(auto& var:s.variants.vars){auto c=appChoice(s).find(var.first);int value=c!=appChoice(s).end()?c->second:s.variants.defaults.at(var.first);
   auto seen=L.pushed.find(var.first);if(seen!=L.pushed.end()&&seen->second==value)continue;
   if(!L.driver->set(var.first,value)){ok=false;missing=var.first;continue;}L.pushed[var.first]=value;}
  return ok;}
 void stopLogic(Set& s,const std::string& state,const std::string& why){auto& L=s.logic;if(L.driver){try{if(L.driver->persistChanged()||L.persistDue)savePersist(s);}catch(const std::exception& ex){log("logic-persist-error",s.name+": "+ex.what());}}
  L.driver.reset();L.pushed.clear();L.want.clear();L.persistDue=false;
  if(animated(s)){s.animation.loop.showFrom(s.animation.applied);s.animation.loop.clock=false;}log(state,s.name+": "+why+"; "+fallback(s)+" from here ("+shownText(s)+")");}
 // persist: logic-persist/<model>.json beside control.json, the model's persist variables by full name (namespace
 // included, as 3DMigoto writes d3dx_user.ini) and the main INI's namespace they belong to; another INI under the same
 // model name does not get them. Written whole to a temporary file and renamed; at most once a second while they
 // change, and when the logic stops.
 std::string persistFile(const Set& s)const{return shared+"/logic-persist/"+s.name+".json";}
 std::map<std::string,float> loadPersist(const Set& s){std::map<std::string,float> out;auto file=persistFile(s);if(!exists(file))return out;
  try{Json j=JsonParser(text(file)).parse();require(j.type==Json::Object&&j.at("format").type==Json::String&&j.at("format").string=="gibox-logic-persist","not a persist file");
   const auto& main=j.at("main");if(main.type!=Json::String||main.string!=s.logicSpec.mainNs){log("logic-persist",s.name+": the saved values belong to another INI ("+(main.type==Json::String?main.string:std::string("?"))+"); not restored");return out;}
   const auto& v=j.at("values");require(v.type==Json::Object&&v.object.size()<=4096,"values");
   for(auto& kv:v.object){require(kv.first.size()>1&&kv.first[0]=='$'&&kv.second.type==Json::Number&&std::isfinite(kv.second.number),"value "+kv.first);out[kv.first]=float(kv.second.number);}}
  catch(const std::exception& ex){out.clear();log("logic-persist-error",s.name+": "+file+": "+ex.what()+"; not restored");}
  return out;}
 void savePersist(Set& s){auto& L=s.logic;auto values=L.driver->persistent();L.persistDue=false;if(values.empty())return;
  std::string body="{\"format\":\"gibox-logic-persist\",\"version\":1,\"main\":"+quote(s.logicSpec.mainNs)+",\"values\":{";bool first=true;
  for(auto& kv:values){if(!std::isfinite(kv.second))continue;char b[32];std::snprintf(b,sizeof b,"%.9g",double(kv.second));body+=(first?"":",")+quote(kv.first)+":"+b;first=false;}
  body+="}}\n";auto dir=shared+"/logic-persist",file=persistFile(s);
  worker.post([dir,file,body]{try{std::filesystem::create_directories(dir);writeAtomic(file,body);}catch(...){}});
  log(L.persistSaves++?"logic-persist-saved":"logic-persist",s.name+": "+std::to_string(values.size())+" persist values saved");}
 // The App's key presses for this model since the last ones taken: each goes to the core once, as a key event of the
 // next Present; a press while the core does not run the model is dropped (and said so), not kept for later.
 void logicKeys(Set& s){auto it=ctl.keys.find(s.name);if(it==ctl.keys.end())return;long long& seen=keySeen[s.name];
  for(auto& k:it->second){if(k.seq<=seen)continue;seen=k.seq;
   if(!s.logic.driver){const char* why=!ctl.exp.logicCore?"logicCore is off":!s.logicSpec.present()?"the model has no logic data (convert it again)":s.logic.failed?"it stopped or did not start; the logic-error / logic-unavailable line above says why":"it has not started yet (the model is not shown)";
    log("logic-key",s.name+": "+k.section+" not pressed: the logic core does not run this model ("+why+")");continue;}
   const LogicSpec::Key* key=nullptr;for(auto& x:s.logicSpec.keys)if(x.file==k.file&&x.section==k.section)key=&x;
   if(!key||!s.logic.driver->press(*key,k.back)){log("logic-key",s.name+": "+k.section+(k.back?" back":"")+" is not a cycle or toggle key of this model"+(k.back?" with back =":"")+"; not pressed");continue;}
   log("logic-key",s.name+": "+k.section+" ("+key->type+(k.back?", back":"")+") pressed, press "+std::to_string(k.seq));}}
 void logicSwitch(Set& s){auto& L=s.logic;
  if(ctl.exp.logicCore&&!s.logicSpec.present()&&!L.noted){L.noted=true;log("logic-unavailable",s.name+": converted without logic data (before 0.7.34 for animations, 0.7.36 for outfits, or the converter could not state it: see its conversion log); "+fallback(s)+" plays it");}
  if(!ctl.exp.logicCore||!s.logicSpec.present()){if(L.driver)stopLogic(s,"logic-off","switched off");return;}
  if(L.driver||L.failed)return;
  if(!s.logicSpec.unsupported.empty()){L.failed=true;log("logic-unavailable",s.name+": "+s.logicSpec.unsupported+"; "+fallback(s)+" plays it");return;}
  try{auto saved=loadPersist(s);L.driver=std::make_unique<LogicDriver>(s.logicSpec,saved);auto& d=*L.driver;
   if(!saved.empty())log("logic-persist",s.name+": "+std::to_string(d.restoredCount())+" of "+std::to_string(saved.size())+" saved values restored (persist variables of this INI)");
   auto scan=d.scan();std::string scanText=scan.finiteStates?"finite, "+std::to_string(scan.states)+" states":"not finite";
   // A driven outfit model (the converter: its Present program follows the game) is not finite by design: its
   // program picks among the converted outfits by the clock and the keys, and the App's table is not its input.
   if(!animated(s)&&s.logicSpec.driven){for(auto& var:s.variants.vars)if(!d.has(var.first)){L.driver.reset();L.failed=true;log("logic-error",s.name+": the outfit variable "+var.first+" is not in the INI logic; the App's outfit choice stays");return;}
    scanText="driven by its own program (clock, keys): the App's outfit table is not pushed while the core runs";}
   else if(!animated(s)){std::string why;if(!scan.finiteStates){for(auto& r:scan.reasons)why+=(why.empty()?"":"; ")+r;L.driver.reset();L.failed=true;log("logic-unavailable",s.name+": the outfit logic is not shown to be finite ("+why.substr(0,600)+"); the App's outfit choice stays");return;}
    // A restored persist value stands over the App's table at the start; the App's next change of it goes in.
    for(auto& var:s.variants.vars)if(d.restoredVar(var.first)){auto c=appChoice(s).find(var.first);L.pushed[var.first]=c!=appChoice(s).end()?c->second:s.variants.defaults.at(var.first);}
    std::string missing;if(!pushChoice(s,missing)){L.driver.reset();L.failed=true;log("logic-error",s.name+": the outfit variable "+missing+" is not in the INI logic; the App's outfit choice stays");return;}d.reselect();}
   if(d.frame()<0){std::string miss=d.miss;L.driver.reset();L.failed=true;log("logic-error",s.name+": what the draws select at the start matches no prebuilt "+(animated(s)?"frame":"outfit")+"; "+fallback(s)+" kept. "+miss);return;}
   size_t notes=0;if(!d.unknown.empty()){std::string list;for(auto& u:d.unknown)list+=(list.empty()?"":", ")+u;log("logic-note",s.name+": resources the converter did not list (bindings to them match nothing): "+list.substr(0,600));}
   for(auto& n:d.diagnostics().list){if(n.level==migoto::Diagnostic::Notice)continue;if(++notes>24){log("logic-note",s.name+": ...");break;}
    log("logic-note",s.name+": "+(n.level==migoto::Diagnostic::Limitation?"limitation":n.level==migoto::Diagnostic::Warning?"warning":"error")+" "+n.where+": "+n.message);}
   log("logic-start",s.name+": "+d.summary()+"; draw variables "+scanText+"; first "+choiceText(s,d.frame()));L.fresh=true;L.misses=0;L.missLines=0;L.missLogged.clear();}
  catch(const std::exception& ex){L.driver.reset();L.failed=true;log("logic-error",s.name+": "+ex.what()+"; "+fallback(s)+" kept");}}
 // One engine frame of a set's logic: the App's outfit values, the Presents since the last frame, the misses.
 // Returns the choice to show, or -1 for no change.
 int logicStep(Set& s,uint32_t frame,uint64_t now){auto& L=s.logic;auto& d=*L.driver;
  bool changed=false;
  if(!animated(s)&&!s.logicSpec.driven){std::string missing;size_t before=L.pushed.size();auto old=L.pushed;if(!pushChoice(s,missing))throw std::runtime_error("the outfit variable "+missing+" is not in the INI logic");
   if(old!=L.pushed||before!=L.pushed.size())changed|=d.reselect();}
  changed|=d.tick(frame,double(now)/1000.0);changed|=L.fresh;L.fresh=false;
  if(d.persistChanged())L.persistDue=true;
  if(L.persistDue&&now>=L.persistAt){L.persistAt=now+1000;savePersist(s);}
  // A miss is logged when what is selected differs from the last one logged; eight per set, then only counted.
  if(d.stats.misses>L.misses){L.misses=d.stats.misses;if(d.miss!=L.missLogged&&L.missLines<=8){L.missLogged=d.miss;
    log("logic-miss",s.name+(++L.missLines>8?": more selections match nothing prebuilt; not logged further (counted in the stats)":": the draws select what nothing prebuilt has; "+shownText(s)+" stays. "+d.miss));}}
  return changed?d.frame():-1;}
 void animationTick(uint64_t now){if(inTick||!ctl.enabled)return;bool any=false;
  for(auto& s:sets)any|=s->built&&wantedSet(*s)&&((animated(*s)&&s->animation.ready&&!s->animation.stopped)||(!animated(*s)&&(s->logic.driver||(ctl.exp.logicCore&&s->logicSpec.present()&&!s->logic.failed))));
  if(!any)return;
  inTick=true;
  try{uint32_t frame=uint32_t(e.call<int32_t>(abi::frameCount,Ptr(0)));bool debug=logLevel.load(std::memory_order_relaxed)==int(Level::Debug);
   for(auto& ptr:sets){auto& s=*ptr;auto& a=s.animation;if(!s.built||!wantedSet(s))continue;
    // An outfit model: the logic picks the converted outfit; nothing else runs per frame.
    if(!animated(s)){logicSwitch(s);if(!s.logic.driver)continue;
     try{int next=logicStep(s,frame,now);if(next>=0){s.logic.want=pick(s.variants,s.logicSpec.assignments[size_t(next)]);if(s.logic.want!=s.assignment){applyVariant(s,s.logic.want);if(debug)log("logic-outfit",s.name+" engineFrame="+std::to_string(frame)+" "+choiceText(s,next));}}
      if(debug&&now>=s.logic.lastStats+5000){auto& st=s.logic.driver->stats;log("logic-stats",s.name+" "+shownText(s)+" presents="+std::to_string(st.presents)+" changes="+std::to_string(st.changes)+" misses="+std::to_string(st.misses)+" avgLogicUs="+std::to_string(st.presents?st.stepUs/st.presents:0)+" maxLogicUs="+std::to_string(st.maxStepUs));s.logic.lastStats=now;}}
     catch(const std::exception& ex){s.logic.failed=true;stopLogic(s,"logic-error",ex.what());}
     continue;}
    if(!a.ready||a.stopped)continue;
    try{logicSwitch(s);bool changed=false;size_t next=a.loop.current;
     if(s.logic.driver){try{int f=logicStep(s,frame,now);if(f>=0){changed=true;next=size_t(f);}}
      catch(const std::exception& ex){s.logic.failed=true;stopLogic(s,"logic-error",ex.what());}}
     else{changed=a.loop.tick(frame);next=a.loop.current;}
     if(changed&&next!=a.applied){auto start=debug?std::chrono::steady_clock::now():std::chrono::steady_clock::time_point{};applyAnimation(s,next);a.updates++;
      if(debug){auto us=uint64_t(std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now()-start).count());a.applyUs+=us;a.maxApplyUs=std::max(a.maxApplyUs,us);if(a.updates<=32)log("animation-frame",s.name+" mode="+animationMode(s)+" driver="+(s.logic.driver?"logic":"frameloop")+" engineFrame="+std::to_string(frame)+" value="+std::to_string(a.loop.spec.frames[a.applied].value)+" us="+std::to_string(us));}}
     if(debug&&(now>=a.lastStats+5000||ctl.renderMark!=static_cast<long long>(a.lastMark))){auto* d=s.logic.driver.get();std::string driver=d?" driver=logic presents="+std::to_string(d->stats.presents)+" misses="+std::to_string(d->stats.misses)+" avgLogicUs="+std::to_string(d->stats.presents?d->stats.stepUs/d->stats.presents:0)+" maxLogicUs="+std::to_string(d->stats.maxStepUs):" driver=frameloop advances="+std::to_string(a.loop.advances)+" clockGaps="+std::to_string(a.loop.gaps);
      log(ctl.renderMark!=static_cast<long long>(a.lastMark)?"animation-mark":"animation-stats",s.name+" mode="+animationMode(s)+" engineFrame="+std::to_string(frame)+" value="+std::to_string(a.loop.spec.frames[a.applied].value)+" updates="+std::to_string(a.updates)+driver+" avgApplyUs="+std::to_string(a.updates?a.applyUs/a.updates:0)+" maxApplyUs="+std::to_string(a.maxApplyUs)+animationMemory(s));a.lastStats=now;a.lastMark=uint64_t(ctl.renderMark);}
    }catch(const std::exception& ex){a.stopped=true;a.error=ex.what();log("animation-error",s.name+" mode="+animationMode(s)+" playback: "+ex.what());}}
  }catch(const std::exception& ex){for(auto& s:sets)if(s->animation.ready&&!s->animation.stopped){s->animation.stopped=true;s->animation.error=ex.what();log("animation-error",s->name+": engine frame clock: "+ex.what());}}
  inTick=false;
 }
 void freeAnimation(Set& s){for(auto& part:s.parts)freeOwned(part.dynamicRoots);}

 Set* startSet(const ModelInfo& m,LoadResult& res,const std::vector<Fingerprint>& targets,uint64_t now){
  auto s=std::make_unique<Set>();s->name=m.name;s->path=m.path;s->generation=m.generation;s->id=m.id;s->data=res.data;s->targets=targets;s->variants=res.data->variants;s->originalMaterials=res.data->originalMaterials;
  // Estimated memory once built: vertex arrays and the clone's skin, indices (ours and the engine's), RGBA textures.
  for(auto& p:res.data->parts){s->bytes+=uint64_t(p.geo.n)*(12+12+8+32+32);for(auto& c:p.channels)s->bytes+=c.second.size();s->bytes+=uint64_t(p.geo.ni)*8;}
  if(res.data->animation.present()){auto& a=s->animation;a.loop=FrameLoop(res.data->animation);a.mode=ctl.exp.animationMode;a.token=++animationToken;a.started=now;a.cpu.resize(a.loop.spec.frames.size());a.meshes.resize(a.cpu.size());a.owned.resize(res.data->parts.size());a.back.resize(res.data->parts.size());a.cpu[0]=res.data;}
  s->meshKey=ctl.exp.meshKey(res.data->animation.present());s->mergeDraws=!ctl.exp.separateDraws&&!s->originalMaterials;s->swaps=res.data->swaps;s->logicSpec=res.data->logic;s->mats=std::move(res.data->materials);res.data->materials.clear();s->blobSize=res.data->blobSize;s->fileGeneration=res.data->generation;
  auto v=ctl.variables.find(s->name);s->assignment=pick(s->variants,v==ctl.variables.end()?std::map<std::string,int>{}:v->second);s->used=now;Set* cur=s.get();sets.push_back(std::move(s));log("loading",m.name+" "+m.generation);return cur;}

 // ---------------------------------------------------------------- attachments
 void snapshotMaterials(RestoreBackup& backup,Ptr originals){if(backup.materials)return;Owned tmp;
  try{std::vector<Ptr> palette;for(Ptr m:pointers(originals,64)){require(alive(m),"Original palette not ready for snapshot");palette.push_back(clone(tmp,m));}
   Ptr array0=array(tmp,originals,palette.data(),palette.size(),8);
   backup.objects.reserve(backup.objects.size()+tmp.objects.size());backup.handles.reserve(backup.handles.size()+tmp.handles.size());
   backup.objects.insert(backup.objects.end(),tmp.objects.begin(),tmp.objects.end());backup.handles.insert(backup.handles.end(),tmp.handles.begin(),tmp.handles.end());
   backup.materials=array0;tmp.objects.clear();tmp.handles.clear();
  }catch(...){freeOwned(tmp);throw;}}
 void keepRestored(RestoreBackup& backup,Ptr renderer){if(backup.users.count(renderer))return;
  auto h=e.call<uint32_t>(abi::pin,renderer,2,Ptr(0));require(h!=0,"GC handle allocation failed");
  try{backup.users.emplace(renderer,RestoreBackup::User{load<Ptr>(renderer+0x10),h});}catch(...){e.call<void>(abi::unpin,h);throw;}}
 bool backupMaterial(RestoreBackup& backup,Ptr palette){if(!palette||!backup.materials)return false;auto originals=pointers(backup.materials,64);
  for(Ptr m:pointers(palette,64)){
   if(std::find(originals.begin(),originals.end(),m)!=originals.end()){return true;}
  }
  return false;}
 void keepRestoredResources(Ptr renderer,Ptr mesh,Ptr palette){for(auto& b:restoreBackups)if((mesh&&mesh==b->mesh)||backupMaterial(*b,palette)){
   keepRestored(*b,renderer);if(mesh==b->mesh)b->handed.insert(mesh);
   if(palette)for(Ptr m:pointers(palette,64))if(std::find(b->objects.begin(),b->objects.end(),m)!=b->objects.end())b->handed.insert(m);}}
 void pruneRestoreBackups(bool shutdown=false){for(auto it=restoreBackups.begin();it!=restoreBackups.end();){auto& b=**it;bool held=false;
   try{
    for(auto u=b.users.begin();u!=b.users.end();){Ptr r=u->first;bool uses=false;if(alive(r)&&load<Ptr>(r+0x10)==u->second.native){Ptr mesh=e.get(abi::getMesh,r),mats=e.get(abi::getMaterials,r);
      if(mesh==b.mesh){uses=true;}
      if(mats){for(Ptr m:pointers(mats,64)){
       if(std::find(b.objects.begin(),b.objects.end(),m)!=b.objects.end()){uses=true;}
      }}
     }
     if(uses){++u;}else{e.call<void>(abi::unpin,u->second.handle);u=b.users.erase(u);}}
    for(auto& a:attachments)held=held||a.second->backup==&b||a.second->original==b.mesh||backupMaterial(b,a.second->originalMaterials);
    for(auto& s:sets)for(auto& bp:s->parts)held=held||bp.backup==&b||bp.originMesh==b.mesh||backupMaterial(b,bp.originMaterials);
   }catch(const std::exception&){++it;continue;} // An unreadable live reference must not cause premature destruction.
   if(held){++it;continue;}
   if(!b.users.empty()&&!shutdown){++it;continue;}
   // The game may instantiate a restored renderer without another binding callback. Never Destroy resources once
   // handed back: unseen game copies may still reference them. Drop our pins/tracking and leave those to Unity's
   // normal unused-asset lifetime; private snapshots that were never handed back can still be destroyed here.
   b.objects.erase(std::remove_if(b.objects.begin(),b.objects.end(),[&](Ptr p){return b.handed.count(p)>0;}),b.objects.end());
   for(auto& u:b.users){e.call<void>(abi::unpin,u.second.handle);}
   b.users.clear();
   freeOwned(b);it=restoreBackups.erase(it);
  }}
 // A fresh preview can have the original mesh but materials copied from a replaced renderer. Recover the WHOLE
 // original palette (including slot order and count), not just each material: a mod may have four draws over three
 // game slots, or several draws that all use slot 0. Saving the mod's array shape would itself break restoration.
 void captureMaterials(Attachment& a,Ptr mats){pin(a,mats);auto incoming=pointers(mats,64);Attachment* source=nullptr;bool known=false;
  for(auto& kv:attachments){auto& owner=*kv.second;for(Ptr p:incoming)if(owner.materialSources.count(p)){known=true;
    if(matches(a.set->parts[a.part].target,owner.set->parts[owner.part].target)){source=&owner;break;}}if(source)break;}
  require(!known||source,"Inherited GIBox materials belong to a different mesh");
  a.originalMaterials=source?source->originalMaterials:mats;pin(a,a.originalMaterials);
  if(source)log("material-backup-cleaned",a.set->name+"/"+a.set->parts[a.part].name+": inherited "+std::to_string(incoming.size())+" mod material slots replaced with the original "+std::to_string(length(a.originalMaterials,64))+"-slot palette");}
 // Leaving a slot mode: property blocks (mode 1) are emptied before anything new is written, because writing a slot's
 // block may drop what else that slot carries (0.7.21 on device: block -> custom texture showed the game's textures
 // until the character was loaded again); custom textures (mode 2) once the new materials are on, since the shown
 // material's own textures are written into them. AllowPerMaterialProp passes on to the next slot mode.
 void applyMaterials(Attachment& a){int want=a.set->originalMaterials?0:ctl.exp.slotTextures;
  if(!a.slotMode||a.slotMode==want){applyMaterialsAs(a,want);return;}
  SlotState old=slotTake(a);if(want){a.slotAllowBefore=old.allow;old.allow=-1;}
  if(old.mode==1){slotRelease(a,old);applyMaterialsAs(a,want);return;}
  try{applyMaterialsAs(a,want);}catch(...){slotRelease(a,old);throw;}slotRelease(a,old);}
 void applyMaterialsAs(Attachment& a,int slotMode){Set& s=*a.set;auto& bp=s.parts[a.part];directRestore(a,nullptr);
  if(slotMode){auto originals=pointers(a.originalMaterials,64);std::vector<Ptr> mats;
   for(auto& row:bp.rows){Ptr src=row.slot<originals.size()?originals[row.slot]:0;if(!alive(src))for(size_t k=originals.size();k-->0;)if(alive(originals[k])){src=originals[k];break;}require(alive(src),"Original material missing");mats.push_back(src);}
   std::set<std::string> names;for(auto& r:bp.rows)names.insert(r.name);ensureTextures(s,names,true);
   Owned tmp;try{e.set(abi::setMaterials,a.renderer,array(tmp,a.originalMaterials,mats.data(),mats.size(),8));}catch(...){freeOwned(tmp);throw;}freeOwned(tmp);
   bool first=a.slotMode!=slotMode;a.slotTex=slotPlan(a,mats);a.slotMats=mats;a.slotMode=slotMode;slotWrite(a);if(first)slotReport(a);return;}
  if(ctl.exp.gameMaterials&&!s.originalMaterials){auto originals=pointers(a.originalMaterials,64);std::vector<Ptr> mats;
   for(auto& row:bp.rows){Ptr src=row.slot<originals.size()?originals[row.slot]:0;if(!alive(src))for(size_t k=originals.size();k-->0;)if(alive(originals[k])){src=originals[k];break;}require(alive(src),"Original material missing");mats.push_back(src);}
   Owned tmp;try{e.set(abi::setMaterials,a.renderer,array(tmp,a.originalMaterials,mats.data(),mats.size(),8));}catch(...){freeOwned(tmp);throw;}freeOwned(tmp);return;}if(!s.originalMaterials){std::set<std::string> names;for(auto& r:bp.rows)names.insert(r.name);ensureTextures(s,names,true);}if(s.originalMaterials){e.set(abi::setMaterials,a.renderer,a.originalMaterials);return;}
  auto originals=pointers(a.originalMaterials,64);std::vector<Ptr> mats;std::map<unsigned,Ptr> direct;
  // A decision that writes nothing (every slot copied, as for the party screen's loaded assets) is a debug line.
  {std::string report;direct=directPlan(a,originals,report);if(report!=a.directReport){a.directReport=report;log(direct.empty()?"direct-textures-copied":"direct-textures",s.name+"/"+bp.name+" on "+rendererName(a.renderer)+": "+report);}}
  for(auto& row:bp.rows){auto key=row.name+"#"+std::to_string(row.slot);auto it=a.clones.find(key);
   // A slot that takes the mod's textures in the game's material: its old copy is retired (an adopted renderer may show it).
   auto own=direct.find(row.slot);if(own!=direct.end()){if(it!=a.clones.end()){a.retired[it->second]=it->first;a.clones.erase(it);}
    if(!a.direct.count(own->second)){directWrite(a,own->second,s.textures.at(row.name));}
    mats.push_back(own->second);continue;}
   Ptr m=0;if(it!=a.clones.end()&&alive(it->second))m=it->second;
   else{Ptr src=row.slot<originals.size()?originals[row.slot]:0;
    // The PC mod can have more parts than the phone's mesh has materials (Shenhe: 4 parts, 3 submeshes): such a slot
    // takes the shader of the renderer's last material, with the mod's own textures.
     if(row.slot>=originals.size()){
      for(size_t k=originals.size();k-->0;){
       if(alive(originals[k])){
        src=originals[k];std::string layout;
        for(size_t j=0;j<originals.size();j++)layout+=(layout.empty()?"":"; ")+std::to_string(j)+":"+(alive(originals[j])?name(originals[j]):"(missing)");
        if(extraSlots.insert(s.path+"|"+s.generation+"|"+bp.name+"#"+std::to_string(row.slot)+"|"+layout).second){
         log("material-slot-borrowed",s.name+"/"+bp.name+": slot "+std::to_string(row.slot)+" has no game material on "+rendererName(a.renderer)+" ("+std::to_string(originals.size())+"), using slot "+std::to_string(k)+"'s; originals ["+layout+"]");
        }
        break;
       }
      }
     }
    require(alive(src),"Original material missing (slot "+std::to_string(row.slot)+")");m=clone(a,src);a.materialSources[m]=src;pin(a,src);auto& t=s.textures.at(row.name);
    if(!t.original){e.set(abi::mainTexture,m,t.main);if(t.light)extraTexture(m,t.light,{"_LightMap","_LightMapTex","_Lightmap","_LightmapTex"});if(t.normal)extraTexture(m,t.normal,{"_BumpMap","_NormalMap","_BaseNormalMap","_NormalTex"});}a.clones[key]=m;}
   mats.push_back(m);}
  Owned tmp;try{e.set(abi::setMaterials,a.renderer,array(tmp,a.originalMaterials,mats.data(),mats.size(),8));}catch(...){freeOwned(tmp);throw;}freeOwned(tmp);}
 bool attach(Ptr r,Ptr mesh,Set& s,size_t part,uint64_t now){
  auto& bp=s.parts[part];auto a=std::make_unique<Attachment>();a->renderer=r;a->native=load<Ptr>(r+0x10);a->original=mesh;a->set=&s;a->part=part;a->backup=bp.backup;bool meshSet=false;
  auto readyKey=s.path+"|attach|"+std::to_string(a->native);auto waiting=[&](const std::string& reason){if(missingLogged[readyKey]!=reason){missingLogged[readyKey]=reason;log("attach-wait",s.name+"/"+bp.name+": "+reason);}freeOwned(*a);return false;};
  try{Ptr ba=e.get(abi::getBones,r);pin(*a,ba);auto bones=ba?pointers(ba,512):std::vector<Ptr>{};
   if(bones.size()!=bp.bones.size())return waiting("bone array is not complete; waiting for a later binding/discovery");
   bool same=true;for(size_t i=0;i<bones.size();i++){if(!alive(bones[i]))return waiting("a bone is not ready");auto bn=name(bones[i]);if(bn.empty())return waiting("a bone has no name yet");same=same&&bn==bp.bones[i];}
   if(!same){skipped.insert({r,a->native});freeOwned(*a);log("attach-skip",name(r)+": bone order differs from "+s.name+"/"+bp.name);return false;}
   Ptr mats=e.get(abi::getMaterials,r);if(!mats)return waiting("material array is not ready");length(mats,64);missingLogged.erase(readyKey);pin(*a,r);pin(*a,mesh);captureMaterials(*a,mats);
   // An existing but unavailable material can become ready on the same renderer. Do not borrow another shader for
   // it, or mark that renderer permanently skipped. The next discovery will retry before any mesh is changed.
   auto originals=pointers(a->originalMaterials,64);bool any=false;for(Ptr m:originals)any=any||alive(m);int missing=-1;
   if(!s.originalMaterials)for(auto& row:bp.rows)if(!any||(row.slot<originals.size()&&!alive(originals[row.slot]))){missing=int(row.slot);break;}
   for(size_t slot=0;missing<0&&slot<originals.size();slot++)if(!alive(originals[slot]))missing=int(slot);
   auto waitKey=s.path+"|material|"+std::to_string(a->native);
   if(missing>=0){auto reason=bp.name+" 的原材质槽 "+std::to_string(missing)+" 尚未就绪，等待后重试";materialWait[s.path]=reason;
    if(missingLogged[waitKey]!=reason){missingLogged[waitKey]=reason;log("material-wait",s.name+": "+reason);}freeOwned(*a);return false;}
   missingLogged.erase(waitKey);require(a->backup&&alive(a->backup->mesh),"Restoration mesh snapshot unavailable");snapshotMaterials(*a->backup,a->originalMaterials);
   e.set(abi::setMesh,r,bp.mesh);meshSet=true;applyMaterials(*a);if(!s.originalMaterials)a->refreshAt=now+(ctl.exp.firstRefreshMs?uint64_t(ctl.exp.firstRefreshMs):1500);
   if(!alive(bp.originMesh)){bp.originMesh=mesh;bp.originMaterials=a->originalMaterials;pin(s,mesh);pin(s,bp.originMaterials);}}
  catch(...){try{directRestore(*a,"attach failed");}catch(const std::exception&){}if(meshSet&&alive(r))e.set(abi::setMesh,r,mesh);freeOwned(*a);throw;}
   log("attached",s.name+"/"+bp.name+" -> "+name(r));attachments[r]=std::move(a);beginMaterialTrace(*attachments.at(r),now);s.used=now;return true;}
 // Bounded observations, not an effect-parameter fix: identify palette replacements during party hand-off versus
 // unchanged material instances. Existing INFO palette logs stay intact; debug readback uses named managed methods.
 void materialState(Attachment& a,const std::string& phase,uint64_t now){
  renderSnapshot(a,phase);
  if(a.set->originalMaterials||logLevel.load(std::memory_order_relaxed)>int(Level::Info)){return;}
  try{
   if(!alive(a.renderer)||load<Ptr>(a.renderer+0x10)!=a.native)return;
   auto saved=pointers(a.originalMaterials,64);Ptr array=e.get(abi::getMaterials,a.renderer);auto current=array?pointers(array,64):std::vector<Ptr>{};
   auto describe=[&](Ptr m){
    if(!alive(m)){return std::string("dead@")+hex(m);}
    std::string kind="untracked";
    if(std::find(saved.begin(),saved.end(),m)!=saved.end()){kind="original";}
    for(auto& kv:attachments){auto& owner=*kv.second;for(auto& c:owner.clones){if(c.second==m){kind="mod";}}if(owner.retired.count(m)){kind="retired";}}
    return kind+":"+name(m)+"@"+hex(load<Ptr>(m+0x10));};
   auto palette=[&](const std::vector<Ptr>& list){std::string out="[";for(size_t i=0;i<list.size();i++){out+=(i?"; ":"")+std::to_string(i)+"="+describe(list[i]);}return out+"]";};
   log("material-state",a.set->name+"/"+a.set->parts[a.part].name+" "+phase+" age-ms="+std::to_string(now>=a.materialTraceStart?now-a.materialTraceStart:0)+" renderer="+hex(a.native)+" current="+palette(current)+" saved="+palette(saved));
  }catch(const std::exception& ex){log("material-state-error",a.set->name+": "+ex.what());}
 }
 void beginMaterialTrace(Attachment& a,uint64_t now){if(a.set->originalMaterials){return;}
  if(glowOn()&&glow::trace().armed.load()){glowRegister();glow::trace().marker(a.renderer,a.adopted?"adopted":"attached");if(!a.adopted)glowOpen("attach "+a.set->name+"/"+a.set->parts[a.part].name,{glowOwner(a)},false,500,3000,{0,3000},true);}if(worker.background()){now=clockMs();}a.materialTraceStart=now;a.materialTraceAt=now+500;a.materialTraceSamples=0;
  a.materialCheckAt=now+50;a.materialCheckUntil=now+1500;a.materialRepairs=0;materialState(a,"attached",now);}
 static std::string materialStem(const std::string& raw){auto s=normalize(raw);auto clone=s.find("(clone)"),instance=s.find("(instance)");auto end=std::min(clone,instance);return trim(s.substr(0,end));}
 // The log confirms copies named "...(Clone) (Instance) #n" appearing after the hand-off. Only accept that layout
 // on our exact mesh, with the expected row count and original material stem in EVERY slot. Names alone are not proof.
 bool copiedPalette(Attachment& a,const std::vector<Ptr>& current,Ptr materials){auto& bp=a.set->parts[a.part];
  if(e.get(abi::getMesh,a.renderer)!=bp.mesh||current.size()!=bp.rows.size()){return false;}
  Owned roots;bool result=false;
  try{pin(roots,materials);result=[&]{auto original=pointers(a.originalMaterials,64);bool copied=false;
  for(size_t i=0;i<current.size();i++){
   Ptr m=current[i];if(!alive(m)){return false;}auto slot=bp.rows[i].slot;Ptr src=slot<original.size()?original[slot]:0;
   if(slot>=original.size()){for(size_t k=original.size();k-->0;){if(alive(original[k])){src=original[k];break;}}}
   if(!alive(src)||materialStem(name(src))!=materialStem(name(m))){return false;}
   bool known=false;for(auto& kv:attachments){if(kv.second->materialSources.count(m)){known=true;break;}}
   if(!known){if(normalize(name(m)).find("(clone)")==std::string::npos){return false;}copied=true;}
  }
  return copied;}();}catch(...){freeOwned(roots);throw;}
  freeOwned(roots);return result;
 }
 void retakeClones(Attachment& a){
  auto old=a.clones;a.clones.clear();try{applyMaterials(a);}catch(...){a.clones=old;throw;}for(auto& kv:old){a.retired[kv.second]=kv.first;}
  std::map<Ptr,Ptr> swap;for(auto& kv:a.retired){auto it=a.clones.find(kv.second);if(it!=a.clones.end())swap[kv.first]=it->second;}
  for(auto& kv:attachments){auto& o=*kv.second;if(o.source!=a.renderer||!alive(o.renderer))continue;Ptr om=e.get(abi::getMaterials,o.renderer);if(!om)continue;
   auto list=pointers(om,64);bool changed=false;for(auto& p:list){auto f=swap.find(p);if(f!=swap.end()){p=f->second;changed=true;}}
    if(changed){Owned tmp;try{e.set(abi::setMaterials,o.renderer,array(tmp,om,list.data(),list.size(),8));}catch(...){freeOwned(tmp);throw;}freeOwned(tmp);}}
 }
 // Keep the two delayed refreshes, including when the game has instantiated our material palette.
 void refreshClones(Attachment& a,uint64_t now){a.refreshAt=++a.refreshes<2?now+2500:0;Set& s=*a.set;
  if(s.originalMaterials||a.adopted||a.slotMode||a.clones.empty()||!alive(a.renderer)||load<Ptr>(a.renderer+0x10)!=a.native){return;}
  materialState(a,"refresh-before",now);Ptr mats=e.get(abi::getMaterials,a.renderer);
  if(!mats){log("material-refresh",s.name+": attempt "+std::to_string(a.refreshes)+" skipped (no material array)");return;}
  auto current=pointers(mats,64);bool shows=false;for(auto& c:a.clones){shows=shows||std::find(current.begin(),current.end(),c.second)!=current.end();}
  if(!shows&&!copiedPalette(a,current,mats)){log("material-refresh",s.name+": attempt "+std::to_string(a.refreshes)+" skipped (current materials are not the owned clones)");return;}
  bool glow=glowOn()&&glow::trace().armed.load();if(glow){glow::trace().marker(a.renderer,"refresh "+std::to_string(a.refreshes));glowDiff(a,"refresh-before "+std::to_string(a.refreshes));}
  retakeClones(a);log("material-refresh",s.name+": attempt "+std::to_string(a.refreshes)+" applied");materialState(a,"refresh-after",now);
  if(glow){glowRegister();glowDiff(a,"refresh-after "+std::to_string(a.refreshes));}}
 // A bounded early repair: first 1.5 s, at most 20 checks/s and two rebuild attempts. Do not perpetually fight the
 // game's effects, alter shader parameters, replace unrelated meshes, or destroy game-owned material instances.
 void guardMaterials(Attachment& a,uint64_t now){
  if(!a.materialCheckAt||now<a.materialCheckAt){return;}
  if(now>a.materialCheckUntil||a.materialRepairs>=2){a.materialCheckAt=0;return;}
  a.materialCheckAt=now+50;
  try{
   if(a.set->originalMaterials||a.slotMode||!alive(a.renderer)||load<Ptr>(a.renderer+0x10)!=a.native){return;}
   Ptr mats=e.get(abi::getMaterials,a.renderer);if(!mats){return;}auto current=pointers(mats,64);
   bool known=true;for(Ptr m:current){bool found=false;for(auto& kv:attachments){found=found||kv.second->materialSources.count(m)>0||kv.second->direct.count(m)>0;}known=known&&found;}
   if(known||!copiedPalette(a,current,mats)){return;}
   a.materialRepairs++;materialState(a,"copy-before",now);retakeClones(a);a.adopted=false;a.source=0;
   if(!a.refreshAt){a.refreshes=0;a.refreshAt=now+(ctl.exp.firstRefreshMs?uint64_t(ctl.exp.firstRefreshMs):1500);}
   log("material-reclaimed",a.set->name+"/"+a.set->parts[a.part].name+": copied palette rebuilt at "+std::to_string(now-a.materialTraceStart)+" ms (attempt "+std::to_string(a.materialRepairs)+")");materialState(a,"copy-after",now);
  }catch(const std::exception& ex){log("material-reclaim-error",a.set->name+": "+ex.what());}
 }
 // Attach failures stay visible in status until the next control change.
 void note(const std::string& m){log("attach-error",m);if(std::find(notes.begin(),notes.end(),m)==notes.end()&&notes.size()<8)notes.push_back(m);}
 // Does the renderer still show material clones of ours (its own, or the source's for an adopted copy), or a game
 // material holding our textures (direct textures)?
 bool showsClones(Ptr r){Ptr mats=e.get(abi::getMaterials,r);if(!mats)return false;std::set<Ptr> current;for(Ptr m:pointers(mats,64))current.insert(m);
  for(auto& kv:attachments){for(auto& c:kv.second->clones)if(current.count(c.second))return true;for(auto& c:kv.second->retired)if(current.count(c.first))return true;
   for(auto& d:kv.second->direct)if(current.count(d.first))return true;}return false;}
 // Original materials go back only while the renderer still shows our clones: once they are destroyed, a renderer still
 // pointing at them draws magenta (0.3.5: the game swaps the mesh, we dropped the attachment and destroyed its clones).
 // A renderer in the slotTextures experiment shows the game's materials, one per draw slot of ours.
 // The game may put its own instances into those slots (the overrides stay with the slot), so the slot count decides.
 bool showsOurs(Attachment& a){if(showsClones(a.renderer))return true;if(!a.slotMode)return false;Ptr mats=e.get(abi::getMaterials,a.renderer);if(!mats)return false;
  auto current=pointers(mats,64);if(current.size()!=a.set->parts[a.part].rows.size())return false;for(Ptr m:current)if(!alive(m))return false;return true;}
 // Game materials holding our textures get theirs back before the palette goes back (so original UVs never meet our
 // textures). One in the palette of another attachment's ledger cannot be: it is still on show there (logged).
 void restoreMaterials(Attachment& a,bool knownPair=false){if(!alive(a.renderer)||load<Ptr>(a.renderer+0x10)!=a.native)return;
  directTrack(a,false);bool ours=knownPair||showsOurs(a);directRestore(a,"restore");
  if(a.originalMaterials)for(Ptr m:pointers(a.originalMaterials,64))for(auto& kv:attachments)if(kv.second.get()!=&a&&kv.second->direct.count(m))
   log("direct-textures-shared",a.set->name+"/"+a.set->parts[a.part].name+": "+name(m)+" goes back to "+rendererName(a.renderer)+" still holding our textures (in use by "+rendererName(kv.first)+")");
  if(!ours){slotClear(a);return;}SlotState slots=slotTake(a);try{restorePalette(a);}catch(...){slotRelease(a,slots);throw;}slotRelease(a,slots);}
 void restorePalette(Attachment& a){
  unsigned dead=0;for(Ptr m:pointers(a.originalMaterials,64))if(!alive(m))dead++;Ptr palette=a.originalMaterials;
  if(dead){require(a.backup&&a.backup->materials,"Restoration material snapshot unavailable");palette=a.backup->materials;
   for(Ptr m:pointers(palette,64))require(alive(m),"Restoration material snapshot expired");
   log("restore-backup",a.set->name+": "+std::to_string(dead)+" original materials expired, using the clean snapshot");}
  keepRestoredResources(a.renderer,0,palette);
  e.set(abi::setMaterials,a.renderer,palette);}
 void restoreFailure(Attachment& a,const std::string& error){if(a.restoreError!=error){a.restoreError=error;log("restore-error",a.set->name+"/"+a.set->parts[a.part].name+": "+error+"; keeping its resources and retrying this renderer only");}}
 bool restore(Attachment& a){if(!alive(a.renderer)||load<Ptr>(a.renderer+0x10)!=a.native)return true;
  try{Ptr current=e.get(abi::getMesh,a.renderer);bool ours=current==a.set->parts[a.part].mesh||animationMesh(*a.set,a.part,current),paired=ours||current==a.original;
  // The game can instantiate our materials after hand-off, giving them unknown pointers. Owning the mesh is enough
  // evidence to restore the whole saved pair. Restore materials first so a setter failure cannot leave original UVs
  // using mod textures; keep unrelated meshes under the narrower material-identity guard.
   Ptr original=a.original;bool fallback=ours&&!alive(original);
   if(fallback){require(a.backup&&alive(a.backup->mesh),"Restoration mesh snapshot unavailable");original=a.backup->mesh;}
   if(ours)keepRestoredResources(a.renderer,original,0);
   restoreMaterials(a,paired);if(ours)e.set(abi::setMesh,a.renderer,original);
   if(fallback)log("restore-backup",a.set->name+"/"+a.set->parts[a.part].name+": original mesh expired, restored the independent original snapshot");
   if(paired){log("restored",a.set->name+"/"+a.set->parts[a.part].name+": original mesh and materials restored");}
   a.restoreError.clear();return true;
  }catch(const std::exception& ex){restoreFailure(a,ex.what());return false;}}
 // The game puts its own mesh back on renderers we attached: inside the hand-off callback, and whenever it changes the
 // level of detail (Body, Body_LOD1..3). When the new mesh is the same asset (mesh name without the LOD suffix, same
 // skeleton), ours goes straight back and the game's mesh becomes the one restored later. Materials are re-applied only
 // if the game replaced them too.
 bool reclaim(Attachment& a,Ptr mesh){auto& bp=a.set->parts[a.part];if(!alive(mesh)||!alive(a.renderer)||load<Ptr>(a.renderer+0x10)!=a.native)return false;
  if(animationMesh(*a.set,a.part,mesh)){e.set(abi::setMesh,a.renderer,bp.mesh);return true;}
  auto fp=fingerprint(mesh);if(meshName(fp.mesh)!=meshName(bp.target.mesh))return false;
  Owned tmp;bool ok=false;
  try{Ptr ba=e.get(abi::getBones,a.renderer);pin(tmp,ba);auto bones=pointers(ba,512);bool same=bones.size()==bp.bones.size();for(size_t i=0;same&&i<bones.size();i++)same=alive(bones[i])&&name(bones[i])==bp.bones[i];
   if(same){
    // Restoring goes back to the LOD0 original: the renderer's bones stay bound for that layout (ours is built on it).
    if(matches(bp.target,fp)||!alive(a.original)){pin(a,mesh);a.original=mesh;}
    // The backup belongs to the first clean attachment. A later material array may be the game's own copy of our
    // textured clones, so it is not a trustworthy replacement for the backup even when its pointers are unfamiliar.
    if(!showsOurs(a)){e.set(abi::setMesh,a.renderer,bp.mesh);applyMaterials(a);}
    else e.set(abi::setMesh,a.renderer,bp.mesh);
    ok=true;if(++reclaims<=20)log("reclaimed",a.set->name+"/"+bp.name+" <- "+fp.mesh+(reclaims==20?" (further reclaims not logged)":""));}}
  catch(...){freeOwned(tmp);throw;}freeOwned(tmp);return ok;}
 // Between ticks (the Tick hook runs every frame; at most every 5 ms here): put ours back the moment the game has swapped
 // the mesh, so a swap shows for at most one frame.
 void guard(uint64_t now){if(inTick||!ctl.enabled||attachments.empty()||now<guardAt)return;guardAt=now+5;inTick=true;
  try{for(auto& kv:attachments){auto& a=*kv.second;if(!wantedSet(*a.set)||!alive(a.renderer)||load<Ptr>(a.renderer+0x10)!=a.native)continue;Ptr m=e.get(abi::getMesh,a.renderer);if(m&&m!=a.set->parts[a.part].mesh)reclaim(a,m);guardMaterials(a,now);}}
  catch(const std::exception& ex){log("guard-error",ex.what());}inTick=false;}
 // A single attachment going away (its renderer died, or the game replaced its mesh): renderers adopted from it still show
 // its material clones, so those move to the first of them instead of being destroyed.
 bool detach(Attachment& a,bool restoreFirst){if(restoreFirst&&!restore(a))return false;Attachment* heir=nullptr;
  for(auto& kv:attachments){auto& o=*kv.second;if(&o==&a||o.source!=a.renderer)continue;if(!heir){heir=&o;}else{o.source=heir->renderer;}}
  if(heir){heir->source=0;for(auto& c:a.clones){if(!alive(c.second))continue;auto it=std::find(a.objects.begin(),a.objects.end(),c.second);if(it==a.objects.end())continue;a.objects.erase(it);pin(*heir,c.second);heir->objects.push_back(c.second);heir->clones[c.first]=c.second;}
   for(auto& c:a.retired){if(!alive(c.first))continue;auto it=std::find(a.objects.begin(),a.objects.end(),c.first);if(it==a.objects.end())continue;a.objects.erase(it);pin(*heir,c.first);heir->objects.push_back(c.first);heir->retired[c.first]=c.second;}
   for(auto& source:a.materialSources)if(alive(source.first)){heir->materialSources[source.first]=source.second;pin(*heir,source.second);}
   // The owner may disappear during the switch-in effect. Its heir now owns these clones and must finish the same
   // refresh schedule; adopted followers otherwise have no timer and are deliberately skipped by refreshClones().
   heir->adopted=false;heir->refreshAt=a.refreshAt;heir->refreshes=a.refreshes;}
  // Game materials holding our textures: the heir shows them now; without one they get their own textures back.
  if(heir){for(auto& d:a.direct){directPin(*heir,d.first);for(auto& b:d.second.before)directPin(*heir,b.second);heir->direct[d.first]=d.second;}a.direct.clear();}
  else{try{directRestore(a,"detach");}catch(const std::exception& ex){log("direct-textures-error",a.set->name+": "+ex.what());}}
  freeOwned(a);return true;}
 // Whole groups go away together: every renderer gets its original back before any clone is destroyed.
 template<class F>bool detachWhere(F pick){std::vector<Ptr> keys,done;for(auto& kv:attachments)if(pick(*kv.second))keys.push_back(kv.first);
  for(Ptr k:keys)if(restore(*attachments.at(k)))done.push_back(k);
  // A failed follower may still display its successful source's clones. Transfer ownership through detach(),
  // rather than destroying a source's materials while that follower is waiting to restore.
  for(Ptr k:done){detach(*attachments.at(k),false);attachments.erase(k);}return done.size()==keys.size();}
 bool detachSet(Set& s){bool faces=swapsEnd([&](Swap& w){return w.set==&s;},"model off");return detachWhere([&](Attachment& a){return a.set==&s;})&&faces;}
 void releaseSet(Set* s){if(!detachSet(*s))return;freeAnimation(*s);freeOwned(*s);log("model-released",s->name+" "+s->generation);sets.erase(std::remove_if(sets.begin(),sets.end(),[&](auto& x){return x.get()==s;}),sets.end());pruneRestoreBackups();}
 bool releaseAll(){bool done=swapsEnd([](Swap&){return true;},"released");done=detachWhere([](Attachment&){return true;})&&done;for(auto it=sets.begin();it!=sets.end();){bool used=false;for(auto& a:attachments)used=used||a.second->set==it->get();
    if(used){++it;}else{freeAnimation(**it);freeOwned(**it);it=sets.erase(it);}}
  seen.clear();skipped.clear();pruneRestoreBackups();std::lock_guard<std::mutex> l(sm);loaded.clear();return done;}
 // The bone names of a renderer, in order, equal to a list.
 bool sameBones(Ptr r,const std::vector<std::string>& want){Owned tmp;bool same=false;
  try{Ptr ba=e.get(abi::getBones,r);if(ba){pin(tmp,ba);auto bones=pointers(ba,512);same=bones.size()==want.size();for(size_t i=0;same&&i<bones.size();i++)same=alive(bones[i])&&name(bones[i])==want[i];}}
  catch(const std::exception&){same=false;}freeOwned(tmp);return same;}
 // The controlled character's renderer showing a lower level of detail of a part's mesh, on the very bones ours was built
 // for: the game keeps some characters (the world Traveler on some devices) on Body_LOD1. Ours goes on, and the LOD mesh
 // is what comes back on restore.
 bool lodOf(const Set& s,size_t i,const Seen& sn,const std::set<Ptr>& world){auto& t=s.parts[i].target;
  return world.count(sn.renderer)&&s.id&&s.id==ch.config&&lowerLod(sn.fp.mesh)&&meshName(sn.fp.mesh)==meshName(t.mesh)&&normalize(rendererName(sn.renderer))==normalize(t.renderer)&&sn.fp.bones==t.bones&&sameBones(sn.renderer,s.parts[i].bones);}
 // Attach every seen renderer whose original mesh matches a part of a live set.
 void attachSeen(const std::vector<Set*>& live,uint64_t now){std::set<Ptr> world;bool worldRead=false;
  for(auto& sn:seen){if(attachments.count(sn.renderer)||!alive(sn.renderer))continue;
   for(Set* s:live){bool done=false;for(size_t i=0;i<s->parts.size()&&!done;i++){bool lod=false;
     if(!matches(s->parts[i].target,sn.fp)){if(!lowerLod(sn.fp.mesh))continue;if(!worldRead){worldRead=true;try{for(Ptr r:worldRenderers())world.insert(r);}catch(const std::exception&){}}if(!lodOf(*s,i,sn,world))continue;lod=true;}
     done=true;
      try{if(e.get(abi::getMesh,sn.renderer)==sn.mesh){if(attach(sn.renderer,sn.mesh,*s,i,now)&&lod)log("attached-lod",s->name+"/"+s->parts[i].name+" over "+sn.fp.mesh);}}catch(const std::exception& ex){skipped.insert({sn.renderer,load<Ptr>(sn.renderer+0x10)});note(s->name+": "+ex.what());}}
    if(done)break;}}}
 // A renderer the game made from one of ours (instantiated from a changed prefab, or taken from a pool) already shows our
 // mesh: track it so it is restored with the others. It usually shares the source's material clones.
 void adopt(Ptr r,Set& s,size_t part,uint64_t now){auto& bp=s.parts[part];auto a=std::make_unique<Attachment>();a->renderer=r;a->native=load<Ptr>(r+0x10);a->set=&s;a->part=part;a->backup=bp.backup;a->adopted=true;
  try{Ptr mats=e.get(abi::getMaterials,r);std::set<Ptr> current;if(mats){for(Ptr m:pointers(mats,64))current.insert(m);}Attachment* src=nullptr;
   for(auto& kv:attachments){auto& o=*kv.second;if(o.set!=&s||o.part!=part)continue;for(auto& c:o.clones)if(current.count(c.second)){src=&o;break;}for(auto& c:o.retired)if(!src&&current.count(c.first))src=&o;if(src)break;}
   Ptr original=src?src->original:bp.originMesh,originalMats=src?src->originalMaterials:bp.originMaterials;
   if(src)a->backup=src->backup;
   if(!alive(original)&&a->backup)original=a->backup->mesh;
   if(!originalMats&&a->backup)originalMats=a->backup->materials;
   require(alive(original)&&originalMats!=0,"Original of adopted renderer unknown");
   pin(*a,r);pin(*a,original);pin(*a,originalMats);a->original=original;a->originalMaterials=originalMats;a->source=src?src->renderer:0;}
  catch(...){freeOwned(*a);throw;}
   log("adopted",s.name+"/"+bp.name+" -> "+name(r));attachments[r]=std::move(a);beginMaterialTrace(*attachments.at(r),now);s.used=now;
  // A copy keeps the material references but not necessarily the per-slot overrides of its source.
  if(ctl.exp.slotTextures&&!s.originalMaterials){try{applyMaterials(*attachments.at(r));}catch(const std::exception& ex){log("slot-textures","adopted copy "+s.name+": "+ex.what());}}}
 void discover(const std::vector<Set*>& live,uint64_t now){materialWait.clear();
  std::map<Ptr,std::pair<Set*,size_t>> ours;for(auto& s:sets)for(size_t i=0;i<s->parts.size();i++){ours[s->parts[i].mesh]={s.get(),i};if(i<s->animation.owned.size())for(Ptr mesh:s->animation.owned[i])ours[mesh]={s.get(),i};}
  auto list=allRenderers(now);
  for(auto it=attachments.begin();it!=attachments.end();){auto& a=*it->second;bool keep=alive(a.renderer)&&load<Ptr>(a.renderer+0x10)==a.native;
   if(keep){Ptr m=e.get(abi::getMesh,a.renderer);if(m&&m!=a.set->parts[a.part].mesh&&!reclaim(a,m)){keep=false;log("attachment-dropped","game put another mesh on "+rendererName(a.renderer)+(alive(m)?": "+name(m):""));try{restoreMaterials(a);}catch(const std::exception& ex){restoreFailure(a,ex.what());keep=true;}}}
   bool newer=false;for(Set* s:live)newer=newer||(s!=a.set&&s->path==a.set->path);
   bool giveWay=keep&&(newer||outranked(a,live));if(giveWay){keep=false;log("outranked",a.set->name+"/"+a.set->parts[a.part].name+" on "+rendererName(a.renderer)+": a newer or earlier-priority model replaces this mesh");}
    if(keep&&a.materialTraceAt&&now>=a.materialTraceAt){materialState(a,"early-sample",now);a.materialTraceAt=++a.materialTraceSamples<2?a.materialTraceStart+1000:0;}
   if(keep&&a.refreshAt&&now>=a.refreshAt){try{refreshClones(a,now);}catch(const std::exception& ex){log("material-refresh-error",a.set->name+": "+ex.what());}}
   if(keep){try{directTrack(a,true);}catch(const std::exception& ex){log("direct-textures-error",a.set->name+": "+ex.what());}}
   if(!keep){if(detach(a,giveWay))it=attachments.erase(it);else ++it;}else{a.set->used=now;++it;}}
  seen.clear();scanSkipped=0;scanCount++;
  for(Ptr r:list){try{if(!alive(r)||attachments.count(r))continue;Ptr native=load<Ptr>(r+0x10);if(skipped.count({r,native})){scanSkipped++;continue;}Ptr m=e.get(abi::getMesh,r);if(!alive(m))continue;
    auto o=ours.find(m);if(o!=ours.end()){adopt(r,*o->second.first,o->second.second,now);continue;}seen.push_back({r,m,fingerprint(m)});}
   catch(const std::exception& ex){log("discover-skip",ex.what());}}
  attachSeen(live,now);swapTick(list);}
 // A finished build takes over in the same tick: its predecessor's renderers, and those of models switched on after it
 // that replace the same meshes.
 void promote(Set& s,uint64_t now){std::vector<std::pair<Ptr,Ptr>> moved;std::set<Ptr> taken;size_t mine=rank(s.path);
  for(auto& kv:attachments){auto& a=*kv.second;if(a.set==&s)continue;if(a.set->path==s.path||(rank(a.set->path)>mine&&claims(s,a.original))){taken.insert(kv.first);moved.push_back({a.renderer,a.original});}}
  detachWhere([&](Attachment& a){return taken.count(a.renderer)>0;});
  std::vector<Set*> old;for(auto& x:sets)if(x.get()!=&s&&x->path==s.path)old.push_back(x.get());for(Set* o:old)releaseSet(o);
  for(auto& m:moved){if(attachments.count(m.first)||!alive(m.first)||!alive(m.second))continue;auto fp=fingerprint(m.second);for(size_t i=0;i<s.parts.size();i++)if(matches(s.parts[i].target,fp)){try{attach(m.first,m.second,s,i,now);}catch(const std::exception& ex){note(s.name+": "+ex.what());}break;}}
  attachSeen({&s},now);}
 // All or nothing: if a part or a renderer cannot take the new values, every part and renderer goes back to the old ones.
 void applyVariant(Set& s,const Assignment& a){if(a==s.assignment)return;
  // New variant values that draw materials whose textures are not read yet: read them in the background and switch
  // on a later tick, so the game thread does not stall on file reads.
  if(!s.originalMaterials&&!ensureTextures(s,drawnBy(s,a),false)){std::string text;for(auto& kv:a)text+=(text.empty()?"":" ")+kv.first+"="+std::to_string(kv.second);
   if(s.textureWait!=text){s.textureWait=text;log("variant-textures-loading",s.name+" "+text);}return;}
  s.textureWait.clear();Assignment old=s.assignment;
  auto apply=[&]{for(auto& bp:s.parts){setSubmeshes(s,bp);e.op(abi::meshUpload,bp.mesh,0);}for(auto& kv:attachments){if(kv.second->set==&s)applyMaterials(*kv.second);}};
  s.assignment=a;try{apply();}catch(...){s.assignment=old;try{apply();}catch(const std::exception& ex){log("variant-error",s.name+": going back failed too: "+ex.what());}throw;}std::string text;for(auto& kv:a)text+=(text.empty()?"":" ")+kv.first+"="+std::to_string(kv.second);log("variant",s.name+" "+text);}

 // ---------------------------------------------------------------- async hand-off (JDIIHHFJKBM.AAKKKEMMMCA)
 // The game hands a freshly made, still inactive model GameObject to its entity here (party screen, detail page). Its
 // renderers already carry the original Mesh, so attaching before the game's own callback runs sets the model up with
 // ours before the game activates it, instead of swapping it on the next scan. Runs between ticks on the Tick thread
 // only; inTick doubles as the re-entrancy guard. A selected model that is not built yet is built right here from these
 // renderers when its files are already loaded (the tick preloads them); otherwise the scan picks it up later.
 void seedGameObjectClass(){for(Ptr r:worldRenderers())if(alive(r)){Ptr go=e.get(abi::gameObject,r);if(alive(go)&&load<Ptr>(go)){goClass=load<Ptr>(go);log("spawn-ready","GameObject class known, hand-off replacement on");}return;}}
 // Called from hooks: nothing in Impl is touched off the Tick thread.
 bool onTickThread(const char* what){int t=tickTid.load();if(t&&threadId()==t)return true;if(t&&offThread.fetch_add(1)==0)log("hook-skip",std::string(what)+" called on another thread than Tick; left to the game");return false;}
 // ---- appearance timeline (debug log level only, at most 300 lines a session): when the game hands over models and
 // binds meshes, also before GIBox is ready to replace anything (loading screen), to find where party members are made.
 unsigned timelineLines=0,timelineOthers=0;std::set<std::string> timelineMeshes;  // base mesh names of every model's parts ("body")
 static std::string baseMesh(const std::string& n){auto m=meshName(n);auto p=m.rfind("_lod");if(p!=std::string::npos&&p+4<m.size()&&std::all_of(m.begin()+long(p)+4,m.end(),[](char c){return c>='0'&&c<='9';}))m.erase(p);return m;}
 bool timelineOn(){return logLevel.load(std::memory_order_relaxed)<=int(Level::Debug)&&timelineLines<300;}
 void timeline(const std::string& what,const std::string& detail){if(!timelineOn())return;timelineLines++;log("appearance",what+" "+detail+" | character "+(ch.entity?std::to_string(ch.config):std::string("none"))+", replacement "+(ctl.enabled&&!wantedNow.empty()&&bindingReady?"ready":"not ready"));}
 // A bound renderer whose mesh is one a model replaces (any LOD): which avatar, which mesh, on the controlled character?
 void timelineBind(const char* phase,Ptr native){if(!timelineOn()||timelineMeshes.empty())return;
  try{Ptr r=e.call<Ptr>(abi::wrapperFor,native);if(!alive(r)||load<Ptr>(r+0x10)!=native)return;Ptr mesh=e.get(abi::getMesh,r);if(!alive(mesh))return;auto fp=fingerprint(mesh);
   if(!timelineMeshes.count(baseMesh(fp.mesh))){return;}bool world=false;for(Ptr w:worldRenderers()){world=world||w==r;}
   timeline(std::string("bind-")+phase,"renderer "+hex(native)+" avatar "+avatarIdentity(r)+" mesh "+fp.mesh+" ("+std::to_string(fp.vertices)+" vertices)"+(world?" controlled":" not controlled")+(attachments.count(r)?" attached":""));}
  catch(const std::exception&){}}
 void spawned(Ptr go){if(!onTickThread("hand-off"))return;spawnAttached.clear();
  // Only character models: scene objects, monsters and effects filled the 300 lines before (0.7.18).
  if(timelineOn()&&!inTick){try{if(alive(go)){auto n=name(go);if(n.compare(0,7,"Avatar_")!=0){timelineOthers++;}
     else{timeline("handoff","gameobject "+n+(goClass&&load<Ptr>(go)==goClass?"":" (GameObject class not learned yet)")+(timelineOthers?" ("+std::to_string(timelineOthers)+" other objects handed over since the last line)":""));timelineOthers=0;}}}
   catch(const std::exception&){}}
  if(inTick||!ctl.enabled||!rendererTypeObject||!goClass||wantedNow.empty())return;
  if(!alive(go)||load<Ptr>(go)!=goClass)return;
  inTick=true;Owned tmp;uint64_t now=clockMs();unsigned adopted=0,built=0;
  try{Ptr arr=e.call<Ptr>(abi::componentsInChildren,go,rendererTypeObject,uint32_t(1),Ptr(0));
   if(arr){pin(tmp,arr);std::map<Ptr,std::pair<Set*,size_t>> ours;for(auto& s:sets)for(size_t i=0;i<s->parts.size();i++){ours[s->parts[i].mesh]={s.get(),i};if(i<s->animation.owned.size())for(Ptr mesh:s->animation.owned[i])ours[mesh]={s.get(),i};}
    std::vector<Seen> found;
    for(Ptr r:pointers(arr,256)){if(!alive(r)||attachments.count(r)||skipped.count({r,load<Ptr>(r+0x10)}))continue;Ptr m=e.get(abi::getMesh,r);if(!alive(m))continue;
     auto o=ours.find(m);if(o!=ours.end()){adopt(r,*o->second.first,o->second.second,now);adopted++;continue;}found.push_back({r,m,fingerprint(m)});}
    if(!found.empty())for(auto* m:wantedNow){if(liveSet(*m)||shadowed(*m,wantedNow)||!lodTarget(*m).empty())continue;auto f=failed.find(m->path);if(f!=failed.end()&&f->second.first==m->generation)continue;
     std::vector<Fingerprint> targets;if(!targetsFor(*m,targets))continue;std::vector<Seen> tpl;
     for(size_t i=0;i<targets.size();i++){for(auto& sn:found)if(matches(targets[i],sn.fp)&&usableTemplate(sn,m->parts[i].minBones)){tpl.push_back(sn);break;}}
     if(tpl.size()!=targets.size())continue;
     LoadResult res;if(!takeLoaded(*m,res)){requestLoad(*m);continue;}
     if(!res.error.empty()){failed[m->path]={m->generation,res.error};log("model-error",m->name+": "+res.error);continue;}
     Set* cur=findSet(m->path,m->generation);if(cur&&!cur->built)releaseSet(cur);cur=startSet(*m,res,targets,now);
     try{while(!cur->built)buildStep(*cur,tpl[cur->parts.size()]);}catch(const std::exception& ex){log("spawn-build-error",m->name+": "+ex.what());releaseSet(cur);continue;}
     promote(*cur,now);built++;}
    for(auto& sn:found){if(attachments.count(sn.renderer))continue;bool done=false;
     for(auto* m:wantedNow){Set* s=liveSet(*m);if(!s)continue;
      for(size_t i=0;i<s->parts.size()&&!done;i++)if(matches(s->parts[i].target,sn.fp)){done=true;
       try{if(attach(sn.renderer,sn.mesh,*s,i,now))spawnAttached.push_back(sn.renderer);}catch(const std::exception& ex){skipped.insert({sn.renderer,load<Ptr>(sn.renderer+0x10)});note(s->name+": "+ex.what());}}
      if(done)break;}}}
   if(!spawnAttached.empty()||adopted||built)log("spawn",name(go)+": attached "+std::to_string(spawnAttached.size())+", adopted "+std::to_string(adopted)+", built "+std::to_string(built));}
  catch(const std::exception& ex){log("spawn-error",ex.what());}
  freeOwned(tmp);inTick=false;}
 // After the game's own callback: is our mesh still on the renderers attached above?
 void spawnedAfter(){if(!onTickThread("hand-off")||spawnAttached.empty()||inTick)return;inTick=true;unsigned kept=0,back=0,lost=0;
  try{for(Ptr r:spawnAttached){auto it=attachments.find(r);if(it==attachments.end()||!alive(r)){lost++;continue;}auto& a=*it->second;Ptr m=e.get(abi::getMesh,r);
     if(m==a.set->parts[a.part].mesh){kept++;}else if(reclaim(a,m)){back++;}else{lost++;}
     materialState(a,"handoff-after",clockMs());}}
  catch(const std::exception& ex){log("spawn-error",ex.what());}
  if(back||lost)log("spawn-after","after the game's hand-off callback: kept "+std::to_string(kept)+", put back "+std::to_string(back)+", lost "+std::to_string(lost));
  spawnAttached.clear();inTick=false;}

 // ---------------------------------------------------------------- the game's own mesh + bone binding
 // The game rebinds renderers itself: its native LOD switch (abi::lodBind) and SkinnedMeshRenderer.BindMeshBones
 // (abi::meshBones) set the mesh through the engine's internal setter and then bind bones from that mesh's bone-name
 // hashes. On a renderer we attached, a request for the original mesh or one of its LODs is answered with ours, so the
 // setter and the bone binding both use our mesh (whose hashes are the LOD0 template's): nothing to put back afterwards.
 // Everything else passes unchanged: no attachment, another mesh, another thread, or a call made inside our own work.
 bool knownMesh(Ptr native,Fingerprint& fp,Ptr& managed){managed=e.call<Ptr>(abi::wrapperFor,native);if(!alive(managed)||load<Ptr>(managed+0x10)!=native)return false;fp=fingerprint(managed);return true;}
 bool bindable(Attachment& a,Ptr candidate){auto& bp=a.set->parts[a.part];if(!alive(candidate)||!alive(bp.mesh))return false;
  uint64_t ours=load<uint64_t>(load<Ptr>(bp.mesh+0x10)+0x90),theirs=load<uint64_t>(load<Ptr>(candidate+0x10)+0x90);int compatible=ours&&ours==theirs?1:0;
  if(!compatible&&bp.bindable!=0)log("bind-incompatible",a.set->name+"/"+bp.name+": bone-name hash count ours "+std::to_string(ours)+", incoming "+std::to_string(theirs)+"; the game's rebinding is left alone");
  bp.bindable=compatible;return compatible==1;}
 Ptr bindMesh(Ptr rendererNative,Ptr meshNative){
  if(!meshNative||!rendererNative||!onTickThread("mesh binding")||!ctl.enabled||inTick||attachments.empty())return meshNative;
  Attachment* a=nullptr;for(auto& kv:attachments)if(kv.second->native==rendererNative){a=kv.second.get();break;}
  if(!a||!wantedSet(*a->set)){return meshNative;}auto& bp=a->set->parts[a->part];if(!alive(bp.mesh)){return meshNative;}Ptr ours=load<Ptr>(bp.mesh+0x10);if(a->part<a->set->animation.owned.size())for(Ptr mesh:a->set->animation.owned[a->part])if(alive(mesh)&&load<Ptr>(mesh+0x10)==meshNative)return ours;if(meshNative==ours){inTick=true;renderBindBefore(rendererNative);inTick=false;return meshNative;}
  inTick=true;renderBindBefore(rendererNative);Ptr out=meshNative;
  try{if(!alive(a->renderer)||load<Ptr>(a->renderer+0x10)!=rendererNative){inTick=false;return meshNative;}Fingerprint fp;Ptr managed=0;bool known=knownMesh(meshNative,fp,managed);
   if(known&&meshName(fp.mesh)==meshName(bp.target.mesh)&&bindable(*a,managed)){
    // A successful interception must still remember the new game asset. Otherwise Destroy(old asset) leaves
    // restoration pointing at the expired predecessor even though a live original just passed this hook.
    if((matches(bp.target,fp)||!alive(a->original))&&a->original!=managed){pin(*a,managed);a->original=managed;}
    out=ours;if(glowOn())glow::trace().marker(a->renderer,"bind-substituted "+fp.mesh);if(++binds<=20)log("bind-substituted",a->set->name+"/"+bp.name+": game asked for "+fp.mesh+" ("+std::to_string(fp.vertices)+" vertices)"+(binds==20?" (further binds not logged)":""));}
   else if(++passes<=20)log("bind-passed",rendererName(a->renderer)+" <- "+(known?fp.mesh:std::string("unknown mesh")));}
  catch(const std::exception& ex){log("bind-error",ex.what());out=meshNative;}
  inTick=false;return out;}
 BindingFingerprint bindingFingerprint(const Fingerprint& f){return {meshName(f.mesh),f.vertices,f.submeshes,f.bones};}
 void prepareBindingTargets(){bindingTargets.clear();bindingExact.clear();bindingLods.clear();bindingUncertain=false;bindingReady=false;
  std::string stamp=quote(std::to_string(ch.entity))+":"+std::to_string(ch.config);
  for(auto* m:wantedNow){if(!lodTarget(*m).empty())continue;BindingTarget row{m,{},false};row.known=targetsFor(*m,row.parts);
   stamp+="|"+quote(m->path)+":"+quote(m->generation)+":"+std::to_string(m->id)+(row.known?":known":":unknown");
   for(auto& n:m->names)stamp+=quote(n);
   if(!row.known)bindingUncertain=true;
   for(auto& t:row.parts){stamp+=fingerprintJson(t);if(t.mesh.empty()){bindingUncertain=true;continue;}
    bindingExact.insert(bindingFingerprint(t));bindingLods.insert(meshName(t.mesh));}
   bindingTargets.push_back(std::move(row));}
  if(stamp!=bindingStamp){bindingRejected.clear();bindingStamp=std::move(stamp);}bindingReady=true;}
 bool bindingPossible(const Fingerprint& fp){return bindingUncertain||bindingExact.count(bindingFingerprint(fp))||(lowerLod(fp.mesh)&&bindingLods.count(meshName(fp.mesh)));}
 void bindingStats(){if(!bindingStatsDirty||bindingNow<bindingStatsAt)return;bindingStatsAt=bindingNow+5000;bindingStatsDirty=false;
  log("bind-filter-stats","checks "+std::to_string(bindingChecks)+", cache-hits "+std::to_string(bindingCacheHits)+", rejected "+std::to_string(bindingFiltered)+", candidates "+std::to_string(bindingDeep)+" (session totals)");}
 void bindingDeferred(const std::string& model,const std::string& reason){if(bindDeferred++<20)log("bind-deferred",model+": "+reason+"; waiting for a later binding/hand-off or the discovery fallback");}
 // These same checked hook entries also see first-time renderers. Their pre-call phase must let the game bind the
 // original bones; immediately after the original returns, attach/build from that now-complete renderer.
 // Build (if needed) and attach one bound renderer for model m. false: not this model's (try the next one); true: the
 // renderer is settled, attached or deferred. Files not read yet: an urgent load, and the renderer is kept in
 // pendingBinds so it is taken over as soon as they are, also while it is still hidden (a party member not switched in).
 struct PendingBind {Ptr native;std::string path;uint64_t since;};std::map<Ptr,PendingBind> pendingBinds;
 bool bindBuild(Ptr r,Ptr mesh,const Fingerprint& fp,const ModelInfo* m,const std::vector<Fingerprint>& targets){
    seen.erase(std::remove_if(seen.begin(),seen.end(),[&](const Seen& sn){return sn.renderer==r;}),seen.end());seen.push_back({r,mesh,fp});
    Set* s=liveSet(*m);if(!s){auto failure=failed.find(m->path);if(failure!=failed.end()&&failure->second.first==m->generation)return false;
     auto tpl=templatesFor(*m,targets);if(!tpl){bindingDeferred(m->name,"template/bones are not complete");return true;}
     LoadResult res;if(!takeLoaded(*m,res)){requestLoad(*m,true);pendingBinds[r]={load<Ptr>(r+0x10),m->path,clockMs()};bindingDeferred(m->name,"model data is not preloaded; taken over once it is read");return true;}
     if(!res.error.empty()){failed[m->path]={m->generation,res.error};log("model-error",m->name+": "+res.error);return true;}
     Set* cur=findSet(m->path,m->generation);if(cur&&!cur->built)releaseSet(cur);cur=startSet(*m,res,targets,clockMs());
     Ptr buildingRenderer=0;try{while(!cur->built){const auto& used=(*tpl)[cur->parts.size()];buildingRenderer=used.renderer;buildStep(*cur,used);}}
     catch(const std::exception& ex){releaseSet(cur);if(buildingRenderer)skipped.insert({buildingRenderer,alive(buildingRenderer)?load<Ptr>(buildingRenderer+0x10):0});
      if(++attempts[m->path+"|"+m->generation]>=3){failed[m->path]={m->generation,ex.what()};log("model-error",m->name+": "+ex.what());}
      else{log("build-retry",m->name+": "+ex.what());}
      bindingDeferred(m->name,ex.what());return true;}
     promote(*cur,clockMs());s=cur;}
    if(!attachments.count(r))attachSeen({s},clockMs());
    if(attachments.count(r)){pendingBinds.erase(r);if(++bindAfterHits<=20)log("bind-attached",m->name+": renderer taken over immediately after the game's mesh/bone binding");}
    else bindingDeferred(m->name,"renderer skeleton/materials are not ready");
    return true;}
 // Renderers whose model files were not read when the game bound them: taken over on the first tick after the read.
 void processPendingBinds(uint64_t now){
  for(auto it=pendingBinds.begin();it!=pendingBinds.end();){Ptr r=it->first;auto p=it->second;const ModelInfo* m=nullptr;for(auto* w:wantedNow)if(w->path==p.path)m=w;
   if(!m||!alive(r)||load<Ptr>(r+0x10)!=p.native||attachments.count(r)||now>p.since+30000){it=pendingBinds.erase(it);continue;}
   bool ready=liveSet(*m)!=nullptr;if(!ready){std::lock_guard<std::mutex> l(sm);auto x=loaded.find(m->path);ready=x!=loaded.end()&&x->second.generation==m->generation;}
   if(!ready){++it;continue;}
   it=pendingBinds.erase(it);
   try{Ptr mesh=e.get(abi::getMesh,r);std::vector<Fingerprint> targets;if(!alive(mesh)||!targetsFor(*m,targets))continue;
    bindBuild(r,mesh,fingerprint(mesh,true),m,targets);if(attachments.count(r))log("bind-late-attached",m->name+": taken over right after its files were read");}
   catch(const std::exception& ex){log("bind-after-error",ex.what());}}}
 void meshBound(Ptr native){if(!native||!onTickThread("mesh-bound")||inTick)return;
  if(!ctl.enabled||wantedNow.empty()||!bindingReady){if(timelineOn()){inTick=true;timelineBind("early",native);inTick=false;}return;}
  inTick=true;
  try{renderBindAfter(native);Ptr r=e.call<Ptr>(abi::wrapperFor,native);if(!alive(r)||load<Ptr>(r+0x10)!=native){inTick=false;return;}
   Ptr mesh=e.get(abi::getMesh,r);if(!alive(mesh)){inTick=false;return;}
   auto attached=attachments.find(r);if(attached!=attachments.end()){auto& a=*attached->second;
    if(!wantedSet(*a.set)){inTick=false;return;}
    if(mesh!=a.set->parts[a.part].mesh){reclaim(a,mesh);}else if(!showsOurs(a))applyMaterials(a);
    inTick=false;return;}
   if(skipped.count({r,native})){inTick=false;return;}
   // A game-instantiated copy may already carry our mesh. Register it before the set can be disabled/released.
   for(auto& s:sets)for(size_t i=0;i<s->parts.size();i++)if(mesh==s->parts[i].mesh||animationMesh(*s,i,mesh)){adopt(r,*s,i,clockMs());
    if(++bindAfterHits<=20){log("bind-attached",s->name+": adopted a game copy after mesh/bone binding");}
    inTick=false;return;}
   Ptr meshNative=load<Ptr>(mesh+0x10),data=load<Ptr>(meshNative+0x48);if(!data){inTick=false;return;}
   unsigned vertices=load<uint32_t>(data+0x74),submeshes=load<uint32_t>(data+0xc0),bones=uint32_t(load<uint64_t>(data+0xe0));
   // Identity includes instance ids and the cheap native geometry header: reused addresses or a mesh finishing
   // loading must miss the old rejection. Values contain only expiry times, never managed references/GC roots.
   BindingRejectKey key{native,load<uint32_t>(native+8),meshNative,load<uint32_t>(meshNative+8),data,vertices,submeshes,bones};
   bindingStatsDirty=true;if(auto* until=bindingRejected.find(key))if(bindingNow<*until){bindingCacheHits++;inTick=false;return;}
   bindingChecks++;auto fp=fingerprint(mesh,true); // refresh on a miss/expiry, including an in-place layout/name update
   if(!bindingPossible(fp)){
    // A zero/unnamed header is not a stable mismatch. Never cache missing bones, materials, identity or templates.
    if(vertices&&submeshes&&submeshes<=64&&bones&&bones<=512&&!fp.mesh.empty())bindingRejected.put(key,bindingNow+2000);
    bindingFiltered++;inTick=false;return;}
   bindingDeep++;std::set<Ptr> world;try{for(Ptr w:worldRenderers())world.insert(w);}catch(const std::exception&){}
   // Timeline: only binds that passed the mesh filter (a part's mesh), so rejected NPC/monster binds cost nothing extra.
   if(timelineOn())timeline("bind","renderer "+hex(native)+" avatar "+avatarIdentity(r)+" mesh "+fp.mesh+" ("+std::to_string(fp.vertices)+" vertices)"+(world.count(r)?" controlled":" not controlled"));
   std::vector<std::string> tokens;bool tokensRead=false;
   for(auto& row:bindingTargets){auto* m=row.model;const auto& targets=row.parts;if(!row.known){if(world.count(r)&&m->id==ch.config)bindingDeferred(m->name,"the target mesh has not been learned");continue;}bool candidate=false;
    for(auto& t:targets)candidate=candidate||matches(t,fp)||(world.count(r)&&lowerLod(fp.mesh)&&meshName(fp.mesh)==meshName(t.mesh));
    if(!candidate)continue;
    if(world.count(r)){if(m->id&&m->id!=ch.config)continue;}
    else{if(!tokensRead){tokens=chain(r,true);tokensRead=true;} // a pooled/loading renderer's parent chain can change
     if(std::find(tokens.begin(),tokens.end(),"avatar")==tokens.end()||(!m->names.empty()&&!namesMatch(m->names,tokens)))continue;}
    if(!world.count(r)&&m->names.empty()){auto* built=liveSet(*m);bool identity=false;auto root=avatarIdentity(r);
     if(built&&!root.empty())for(auto& bp:built->parts)identity=identity||(bp.avatar==root&&(matches(bp.target,fp)||meshName(bp.target.mesh)==meshName(fp.mesh)));
     // Not built yet (a party member loaded but not switched in): the avatar its bone library was recorded from.
     std::string recorded;if(!identity&&!built&&m->id&&!root.empty()){recorded=libraryAvatar(m->id);identity=root==recorded;}
     if(!identity){bindingDeferred(m->name,"a non-world renderer's avatar identity is not verified (renderer "+(root.empty()?std::string("?"):root)+", bone library "+(built?std::string("not needed: built"):recorded.empty()?std::string("none"):recorded)+")");continue;}}
    if(bindBuild(r,mesh,fp,m,targets))break;
   }
  }catch(const std::exception& ex){log("bind-after-error",ex.what());}inTick=false;}

 // ---------------------------------------------------------------- bone library
 // A phone may keep the world model at LOD1 indefinitely. Look for an ACTIVE full-model instance of that same
 // avatar (e.g. its detail/party preview), rather than exporting the LOD palette or waiting on the world forever.
 // A complete replacement group must come from ONE avatar root. Never join parts of different pooled instances,
 // infer a full mesh from its bone count, or change the game's LOD setting. No loaded-asset/prefab search is used.
 std::vector<Ptr> boneRenderers(uint64_t now){
  auto world=worldRenderers();exportWait.clear();exportLodWait=false;std::string low;
  for(Ptr r:world)if(alive(r)){Ptr m=originalMesh(r);if(alive(m)){auto fp=fingerprint(m,true);if(lowerLod(fp.mesh)&&low.empty())low=rendererName(r)+" / "+fp.mesh;}}
  if(low.empty())return world;
  exportLodWait=true;exportWait="大世界的 "+low+" 是低精度网格；尚未找到同角色的完整模型。请先在大世界切到该角色，再打开该角色详情或队伍预览并停留；完整模型出现后会自动采集。原骨骼库保留";
  if(boneFullSearchEntity!=ch.entity){boneFullSearchEntity=ch.entity;boneFullSearchAt=0;boneCandidate.clear();}
  if(now<boneFullSearchAt){return {};}boneFullSearchAt=now+2000;
  Owned roots;std::vector<Ptr> result;
  try{
   std::string identity;std::vector<std::pair<std::string,std::string>> required;std::set<std::string> names;
   for(Ptr r:world){if(!alive(r)){boneCandidate.clear();return {};}Ptr m=originalMesh(r);if(!alive(m)){boneCandidate.clear();return {};}
    auto avatar=avatarIdentity(r);if(avatar.empty()||(!identity.empty()&&identity!=avatar)){boneCandidate.clear();return {};}identity=avatar;
    auto key=std::make_pair(normalize(rendererName(r)),meshName(fingerprint(m,true).mesh));
    if(std::find(required.begin(),required.end(),key)!=required.end()){boneCandidate.clear();return {};}required.push_back(key);names.insert(key.first);}
   if(required.empty())return {};
   Ptr found=e.call<Ptr>(abi::findObjectsOfType,rendererType(world.front()),Ptr(0));if(!found){boneCandidate.clear();return {};}pin(roots,found);
   std::map<Ptr,std::vector<std::vector<Ptr>>> groups;
   for(Ptr r:pointers(found,8192)){if(!alive(r))continue;auto rn=normalize(rendererName(r));if(!names.count(rn))continue;Ptr m=originalMesh(r);if(!alive(m))continue;auto fp=fingerprint(m,true);if(lowerLod(fp.mesh))continue;
    auto key=std::make_pair(rn,meshName(fp.mesh));auto at=std::find(required.begin(),required.end(),key);if(at==required.end())continue;
    Ptr root=avatarRoot(r);if(!root)continue;auto n=meshName(name(root));if(n.substr(0,n.find("_manekin"))!=identity)continue;
    auto& group=groups[root];if(group.empty())group.resize(required.size());group[size_t(at-required.begin())].push_back(r);}
   for(auto& kv:groups){bool complete=true;std::vector<Ptr> candidate;for(auto& part:kv.second){if(part.size()!=1){complete=false;break;}candidate.push_back(part[0]);}
    if(complete){if(!result.empty()){result.clear();exportWait+="；发现多组完整实例，暂不选取";break;}result=std::move(candidate);}}
   freeOwned(roots);
  }catch(...){freeOwned(roots);boneCandidate.clear();throw;}
  if(result.empty()){boneCandidate.clear();return {};}
  std::string source=std::to_string(ch.config)+": active same-avatar full model instead of "+low;
  for(Ptr r:result){auto fp=fingerprint(originalMesh(r),true);source+="; "+rendererName(r)+" / "+fp.mesh+" ("+std::to_string(fp.vertices)+" vertices, "+std::to_string(fp.bones)+" bones)";}
  if(boneSource!=source){boneSource=source;log("bone-library-source",source);}exportLodWait=false;exportWait.clear();return result;
 }
 // One complete capture of the world character. Every listed renderer must be usable; a partial capture is retried.
 // Two identical healthy captures are required before writing. Completion is acknowledged from the worker only.
 bool exportBones(uint64_t now,bool libraryDir){
  auto renderers=boneRenderers(now);if(exportLodWait){library.erase(ch.config);return false;}
  std::vector<SkeletonRenderer> records;std::set<std::string> used;std::string chain;Owned roots;unsigned seenCount=0,loading=0,mismatch=0,unnamed=0,badNames=0;std::string example,badList=std::to_string(ch.config)+":";
  try{for(Ptr r:renderers){seenCount++;if(!alive(r)){loading++;continue;}Ptr mesh=originalMesh(r);if(!alive(mesh)){loading++;continue;}Ptr ba=e.get(abi::getBones,r);if(!ba){loading++;continue;}pin(roots,ba);auto bones=pointers(ba,512);
    Ptr binds=e.get(abi::getBindposes,mesh);if(!binds||bones.empty()){loading++;continue;}pin(roots,binds);
    auto rn=name(r);if(length(binds,512)!=bones.size()){mismatch++;badList+=rn+" ";if(example.empty())example=rn+" "+std::to_string(bones.size())+" bones / "+std::to_string(length(binds,512))+" bindposes";continue;}
    std::string n;for(char c:rn)if((c>='a'&&c<='z')||(c>='A'&&c<='Z')||(c>='0'&&c<='9')||c=='_'||c=='-')n+=c;if(n.empty()){unnamed++;badList+="(unnamed) ";continue;}if(n.size()>60)n.resize(60);auto base=n;for(unsigned duplicate=1;!used.insert(n).second;duplicate++)n=base+"_"+std::to_string(duplicate);
    SkeletonRenderer rec;rec.name=n;bool bad=false;
    for(size_t i=0;i<bones.size()&&!bad;i++){if(!alive(bones[i])){bad=true;break;}auto bn=name(bones[i]);if(bn.empty()||bn.size()>256||bn.find_first_of("\t\r\n")!=bn.npos){bad=true;break;}rec.bones.push_back(bn);Ptr parent=e.get(abi::getParent,bones[i]);auto at=std::find(bones.begin(),bones.end(),parent);rec.parents.push_back(at==bones.end()?-1:int(at-bones.begin()));}
    if(bad){badNames++;badList+=rn+" ";if(example.empty())example=rn+": a bone is missing, unnamed, overlong or has a tab/newline";continue;}
    // Transform chain from the skeleton up to the scene root: carries the character's internal name for the import guess.
    if(chain.empty()){std::vector<std::string> up;Ptr t=bones[0];for(int i=0;i<24&&t&&alive(t);i++){auto tn=name(t);for(auto& c:tn)if(c=='/'||c=='\n'||c=='\r')c='_';up.push_back(tn);t=e.get(abi::getParent,t);}std::reverse(up.begin(),up.end());for(auto& x:up)chain+=(chain.empty()?"":"/")+x;}
    rec.target=fingerprint(mesh,true);rec.target.renderer=rn;rec.bindposes=arrayBytes(binds,64,512);records.push_back(std::move(rec));}}
  catch(...){freeOwned(roots);throw;}
  freeOwned(roots);
  if(records.empty()||records.size()!=seenCount){
   // A renderer that stays structurally unusable (nothing loading, yet its bone and bindpose counts differ, or it or a
   // bone has no usable name) while all the others are complete would hold the library back for ever and keep this full
   // capture running every 500 ms. After 30 s of exactly the same result, the complete renderers are recorded without
   // it (the two-capture check below still applies) and the log names what was left out.
   bool structural=!records.empty()&&loading==0&&(mismatch||unnamed||badNames);
   if(structural&&badList==boneBadList){boneBadStreak++;}else{boneBadList=structural?badList:std::string();boneBadStreak=structural?1:0;}
   exportWait="Incomplete skeleton: "+std::to_string(seenCount)+" renderers, "+std::to_string(loading)+" still loading, "+std::to_string(mismatch)+" with bone/bindpose counts that differ, "+std::to_string(unnamed)+" unnamed, "+std::to_string(badNames)+" with unusable bone names"+(example.empty()?"":" (e.g. "+example+")");
   if(!(structural&&boneBadStreak>=60)){boneCandidate.clear();library.erase(ch.config);if(!mismatch&&!unnamed&&!badNames){return false;}throw std::runtime_error(exportWait);}
   if(boneBadStreak==60)log("bone-library-partial",std::to_string(ch.config)+": recording the complete renderers without "+badList.substr(badList.find(':')+1)+"(unchanged for 30 s: "+exportWait+")");}
  try{validateSkeleton(ch.config,records);}catch(...){boneCandidate.clear();library.erase(ch.config);throw;}
  auto stamp=skeletonStamp(chain,records);
  if(boneCandidateEntity!=ch.entity||boneCandidateId!=ch.config||boneCandidate!=stamp){boneCandidateEntity=ch.entity;boneCandidateId=ch.config;boneCandidate=stamp;boneCandidateAt=now;library.erase(ch.config);exportWait="waiting for a second complete, stable skeleton capture";return false;}
  if(now<boneCandidateAt+500){exportWait="waiting for a second complete, stable skeleton capture";return false;}
  // Only the engine reads happen here; the JSON is built on the worker, and a library record that has not changed since
  // the last session is left as it is (the skeleton library is checked every session, rewritten only after a game update).
  auto dir=std::to_string(ch.config)+(libraryDir?"":"-"+std::to_string(now));auto root=boneRoot;unsigned id=ch.config;auto request=ctl.exportBones;
  auto refresh=libraryDir&&boneRefreshPending()&&id==ctl.refreshBonesCharacter?ctl.refreshBones:0;auto ack=shared+"/bone-refresh-result.json";
  if(libraryDir)libraryPending.insert(id);else manualPending=true;
  worker.post([this,root,dir,libraryDir,id,chain,records,request,refresh,ack]{BoneResult result{id,libraryDir,request,{}};result.refresh=refresh;
   try{auto json=skeletonJson(id,chain,records);auto file=root+"/"+dir+"/skeleton.json";
    bool unchanged=libraryDir&&exists(file)&&text(file)==json;
    if(!unchanged)writeSkeleton(root,dir,libraryDir,json);
    require(text(file)==json,"Skeleton verification failed");log(unchanged?"bone-library-unchanged":libraryDir?"bone-library":"bones-exported",root+"/"+dir);
    if(refresh){auto body="{\"request\":"+std::to_string(refresh)+",\"character\":"+std::to_string(id)+"}";writeAtomic(ack,body);require(text(ack)==body,"Bone refresh acknowledgement verification failed");}
   }catch(const std::exception& ex){result.error=ex.what();}
   std::lock_guard<std::mutex> lock(sm);boneResults.push_back(std::move(result));});return true;}

 // ---------------------------------------------------------------- why a model waits
 // Its targets are known, but no renderer of the last scan shows that mesh. The status line says why in words; the log
 // ("wait-reason") gets the scan summary, each missing target and the renderers that come closest: the same mesh name
 // (another level of detail, an update) or, failing that, the same renderer name. Logged when that changes, and every
 // 30 s while it lasts, so one log shows whether the character was scanned at all and which number differs.
 static std::string describe(const Fingerprint& f){return (f.mesh.empty()?std::string("(无 Mesh 名)"):f.mesh)+"（顶点 "+std::to_string(f.vertices)+"，子网格 "+std::to_string(f.submeshes)+"，骨骼 "+std::to_string(f.bones)+"）";}
 static std::string differences(const Fingerprint& t,const Fingerprint& f){std::string d;
  auto add=[&](const char* what,uint32_t a,uint32_t b){if(a!=b)d+=std::string(d.empty()?"":"，")+what+" "+std::to_string(a)+"→"+std::to_string(b);};
  add("顶点",t.vertices,f.vertices);add("子网格",t.submeshes,f.submeshes);add("骨骼",t.bones,f.bones);return d;}
 // A preload is only a file read. Unknown targets never reach waitReason(), so report this earlier gate as well.
 // Only the controlled character's renderers are evidence here; another avatar's Body cannot establish compatibility.
 const std::string& targetWait(const ModelInfo& m,uint64_t now){auto& w=targetWaitWhy[m.path];
  auto context=m.generation+":"+std::to_string(ch.config)+":"+std::to_string(ctl.exp.animationMode)+":"+std::to_string(ctl.renderMark)+":"+std::to_string(logLevel.load());
  if(w.scan==scanCount&&w.context==context)return w.reason;
  w.scan=scanCount;w.context=context;auto why=staleNote.find(m.path);
  w.reason=why==staleNote.end()?"等待"+(m.id?std::string("该角色"):"名字含 "+m.names[0]+" 的角色")+"出现以识别部件":why->second;
  std::string details;bool animationPaletteMismatch=false;
  if(ch.entity&&m.id==ch.config){auto pool=worldRenderers();
   for(auto& p:m.parts){auto learnedModel=learned.find(m.path);if(p.hasTarget||(learnedModel!=learned.end()&&learnedModel->second.count(p.name)&&learnedModel->second.at(p.name).bones>=p.minBones))continue;
    std::string candidates;unsigned count=0;
    for(Ptr r:pool){if(!alive(r))continue;Ptr mesh=originalMesh(r);if(!alive(mesh))continue;auto fp=fingerprint(mesh);auto rn=rendererName(r);
     if(normalize(rn)!=normalize(p.group)&&normalize(fp.mesh)!=normalize(p.group))continue;
     animationPaletteMismatch=animationPaletteMismatch||(m.animated&&p.minBones>fp.bones);candidates+=(candidates.empty()?"":"；")+rn+" / "+describe(fp);if(++count==4)break;}
    details+="\n"+p.name+" 要 "+p.group+(p.minBones?"，按索引绑定至少 "+std::to_string(p.minBones)+" 根骨骼":"，按骨骼名映射")+(candidates.empty()?"；当前角色没有同名部件":"；当前 "+candidates);
    unsigned full=0;for(auto& s:seen){if(std::find(pool.begin(),pool.end(),s.renderer)!=pool.end()||!alive(s.renderer)||lowerLod(s.fp.mesh)||normalize(rendererName(s.renderer))!=normalize(p.group))continue;
     auto* tokens=chains.find({s.renderer,load<Ptr>(s.renderer+0x10)});if(!tokens||!namesMatch(m.names,*tokens)||std::find(tokens->begin(),tokens->end(),"avatar")==tokens->end())continue;
     details+="；场景同角色完整候选 "+describe(s.fp);if(++full==4)break;}}
   if(!details.empty())w.reason+=details;
   if(animationPaletteMismatch)w.reason="当前动画文件按 Unity 骨骼索引解释的范围超过现场骨骼数；PC 新增几何、额外顶点组和权重语义尚需适配，不能据此认定游戏必须有同样多的骨骼。"+w.reason;
   else if(w.reason.find("低精度")!=std::string::npos)w.reason+="；请靠近镜头或打开角色详情，让完整模型出现后再试；当前 LOD 的骨骼数不能证明完整模型也相同";
  }
  auto detail=context+"|"+w.reason;
  if(w.logged!=detail||now>=w.loggedAt+30000){auto mode=ctl.exp.animationMode==0?"cached":ctl.exp.animationMode==1?"dynamic":"paused";
   log(m.animated?"animation-wait":"target-wait",m.name+(m.animated?" mode="+std::string(mode)+" stage=target-not-learned playback=not-started":"")+": "+w.reason);w.logged=std::move(detail);w.loggedAt=now;}
  return w.reason;}
 const WaitWhy& waitReason(const ModelInfo& m,const std::vector<Fingerprint>& targets,uint64_t now){
  auto& w=waitWhy[m.path];if(w.scan==scanCount)return w;w.scan=scanCount;w.reason.clear();w.plain=false;
  std::string scan="当前角色 "+(ch.entity?std::to_string(ch.config):std::string("无"))+"，它的渲染器 "+std::to_string(scanWorld)+" 个，全场景 "
   +(findOff?std::string("扫描不可用"):scanAll<0?std::string("未扫描"):std::to_string(scanAll)+" 个")+(scanSkipped?"，跳过挂载失败过的 "+std::to_string(scanSkipped)+" 个":"");
  std::string details;
  if(!ch.entity&&!rendererTypeObject){w.reason="还没认出当前操控的角色：进入大世界后才开始扫描";w.plain=true;}
  else for(size_t i=0;i<targets.size()&&i<m.parts.size();i++){auto& t=targets[i];bool present=false;std::vector<std::string> sameMesh,sameRenderer;std::string diff;
   for(auto& s:seen){if(!alive(s.renderer))continue;if(matches(t,s.fp)){present=true;break;}auto rn=rendererName(s.renderer);
    if(!t.mesh.empty()&&meshName(s.fp.mesh)==meshName(t.mesh)){if(sameMesh.size()<4)sameMesh.push_back(rn+" / "+describe(s.fp));if(diff.empty())diff=differences(t,s.fp);}
    else if(normalize(rn)==normalize(t.renderer)&&sameRenderer.size()<4)sameRenderer.push_back(rn+" / "+describe(s.fp));}
   if(present)continue;
   auto& near=sameMesh.empty()?sameRenderer:sameMesh;std::string list;for(auto& x:near)list+=(list.empty()?"":"；")+x;
   details+="\n"+m.parts[i].name+" 要 "+t.renderer+" / "+describe(t)+(near.empty()?"：没有同名的渲染器或 Mesh":(sameMesh.empty()?"：没有这个 Mesh，同名渲染器有 ":"：同名 Mesh 数字不同 ")+list);
   if(w.reason.empty()&&!diff.empty()&&(!m.id||(ch.entity&&ch.config==m.id))){w.reason="场景里有 "+t.mesh+"，但和导入时不同（"+diff+"）：可能是远处的低精度模型，或换了皮肤、版本更新";w.plain=true;}}
  // Counts change from scan to scan; the targets and candidates are what matters for "changed".
  if(details!=w.logged||now>=w.loggedAt+30000){if(!details.empty()||w.plain){log("wait-reason",m.name+": "+scan+(w.plain&&details.empty()?"："+w.reason:"")+details);}w.logged=details;w.loggedAt=now;}
  return w;}

 // ---------------------------------------------------------------- status
 void status(const std::string& state,const std::string& detail,const std::vector<std::string>& active,const std::vector<std::string>& errors){
  std::string model;if(ch.entity)try{for(Ptr r:worldRenderers()){auto a=attachments.find(r);if(a!=attachments.end()){model=a->second->set->name;break;}}}catch(const std::exception&){}
  if(model.empty()&&!active.empty())model=active[0];
  auto list=[](const std::vector<std::string>& v){std::string s="[";for(size_t i=0;i<v.size();i++)s+=(i?",":"")+quote(v[i]);return s+"]";};
  std::string s="{\"state\":"+quote(state)+",\"detail\":"+quote(detail)+",\"characterId\":"+std::to_string(ch.config)+",\"model\":"+quote(model)+",\"models\":"+list(active)+",\"errors\":"+list(errors)+",\"scan\":"+quote(findOff?"off":rendererTypeObject?"ok":"pending")+",\"buildId\":"+quote(abi::buildId)+"}";
  std::string refreshState,refreshDetail;
  if(ctl.refreshBones){
   if(!boneRefreshPending()){refreshState="complete";refreshDetail="骨骼已重新采集";}
   else if(!ch.entity||ch.config!=ctl.refreshBonesCharacter){refreshState="waiting-character";refreshDetail="等待在大世界切换到角色 "+std::to_string(ctl.refreshBonesCharacter);}
   else if(libraryPending.count(ch.config)){refreshState="saving";refreshDetail="骨骼已通过校验，正在保存并回读检查";}
   else{refreshState=exportLodWait?"waiting-full-mesh":libraryError.count(ch.config)?"retrying":"capturing";refreshDetail=exportWait;}
  }
  s.pop_back();s+=",\"boneRefreshRequest\":"+std::to_string(ctl.refreshBones)+",\"boneRefreshCharacter\":"+std::to_string(ctl.refreshBonesCharacter)+",\"boneRefreshState\":"+quote(refreshState)+",\"boneRefreshDetail\":"+quote(refreshDetail)+"}";
  if(s==lastStatus){return;}lastStatus=s;auto dir=shared;worker.post([dir,s]{write(dir+"/status.json.tmp",s);std::filesystem::rename(dir+"/status.json.tmp",dir+"/status.json");});log(state,detail);}

 // ---------------------------------------------------------------- tick
 bool boneRefreshPending()const{return ctl.refreshBones&&(boneRefreshDone!=ctl.refreshBones||boneRefreshDoneId!=ctl.refreshBonesCharacter);}
 void consume(){std::optional<ControlState> c;std::string cerr;std::deque<BoneResult> bones;std::deque<TextureResult> textures;std::deque<AnimationResult> frames;{std::lock_guard<std::mutex> l(sm);c.swap(pendingControl);cerr.swap(controlError);bones.swap(boneResults);textures.swap(textureResults);frames.swap(animationResults);if(scannedFresh){models=scanned;scannedFresh=false;}}
  for(auto& frame:frames){Set* found=nullptr;for(auto& candidate:sets)if(candidate->path==frame.path&&candidate->generation==frame.generation&&candidate->animation.token==frame.token){found=candidate.get();break;}if(!found)continue;auto& a=found->animation;a.loading=false;
   if(!frame.error.empty()){a.stopped=true;a.error=frame.error;log("animation-error",found->name+": frame read: "+frame.error);continue;}a.cpu.at(frame.index)=std::move(frame.data);log("animation-read",found->name+" frame="+std::to_string(a.loop.spec.frames[frame.index].value)+" us="+std::to_string(frame.us));}
  if(timelineOn()){timelineMeshes.clear();for(auto& m:models)for(auto& p:m.parts)if(p.hasTarget)timelineMeshes.insert(baseMesh(p.target.mesh));}
  // Background texture reads for variant switches: hand the pixels to their set (if it is still there).
  for(auto& t:textures){Set* s=nullptr;for(auto& x:sets)if(x->path==t.path&&x->generation==t.generation){s=x.get();break;}if(!s)continue;
   if(!t.error.empty()){log("texture-read-error",s->name+": "+t.error);continue;}
   for(auto& kv:t.materials){auto it=s->mats.find(kv.first);if(it!=s->mats.end()&&!it->second.loaded()&&kv.second.loaded()){it->second.main=kv.second.main;it->second.light=kv.second.light;it->second.normal=kv.second.normal;}}}
  for(auto& b:bones){if(b.library)libraryPending.erase(b.id);else manualPending=false;
   if(b.error.empty()){if(b.library){
     if(b.refresh&&b.refresh==ctl.refreshBones&&b.id==ctl.refreshBonesCharacter){boneRefreshDone=b.refresh;boneRefreshDoneId=b.id;libraryRead.erase(b.id);log("bone-refresh-complete",std::to_string(b.id)+": request "+std::to_string(b.refresh));}
     // An old in-flight capture must not satisfy a newer request for the same character.
     if(boneRefreshPending()&&b.id==ctl.refreshBonesCharacter){library.erase(b.id);}else{library.insert(b.id);libraryError.erase(b.id);}
    }else{exportSeen=b.request;manualWait.clear();}}
   else{if(b.library)library.erase(b.id);auto& last=b.library?libraryError[b.id]:manualWait;if(last!=b.error){last=b.error;log(b.library?"bone-library-error":"bone-export-error",std::to_string(b.id)+": "+b.error+"; retrying automatically");}}}
  // A re-imported folder (new generation, maybe another character) forgets what was learned for the old one.
  for(auto& m:models){auto g=learnedGen.find(m.path);if(g==learnedGen.end()||g->second!=m.generation){learned.erase(m.path);learnedId.erase(m.path);staleNote.erase(m.path);learnedGen[m.path]=m.generation;for(auto it=missingLogged.lower_bound(m.path+"|");it!=missingLogged.end()&&it->first.compare(0,m.path.size()+1,m.path+"|")==0;)it=missingLogged.erase(it);}
   auto l=learned.find(m.path);if(l!=learned.end())for(auto& p:m.parts){auto t=l->second.find(p.name);if(t!=l->second.end()&&t->second.bones<p.minBones)l->second.erase(t);}}
  // A bone export is a request made while the game runs: the count found at start-up is not one.
  if(c&&!controlSeen){controlSeen=true;exportSeen=c->exportBones;
   // Key presses already in control.json were made before this game start: taken as done, never replayed.
   for(auto& kv:c->keys)for(auto& k:kv.second)keySeen[kv.first]=std::max(keySeen[kv.first],k.seq);}
  if(c&&c->exportBones!=exportSeen&&c->exportBones!=ctl.exportBones){boneCandidate.clear();manualRetryAt=0;manualWait.clear();}
  if(c&&c->refreshBones&&(c->refreshBones!=ctl.refreshBones||c->refreshBonesCharacter!=ctl.refreshBonesCharacter)&&
     (c->refreshBones!=boneRefreshDone||c->refreshBonesCharacter!=boneRefreshDoneId)){
   auto id=c->refreshBonesCharacter;library.erase(id);libraryRead.erase(id);libraryRetry[id]=0;libraryError.erase(id);
   boneCandidate.clear();boneBadList.clear();boneBadStreak=0;exportWait.clear();boneFullSearchAt=0;libraryWait.erase(id);log("bone-refresh-request",std::to_string(id)+": request "+std::to_string(c->refreshBones));
  }
  if(c){bool diagnosticOnly=c->enabled==ctl.enabled&&c->selected==ctl.selected&&c->variables==ctl.variables&&c->exportBones==ctl.exportBones&&c->refreshBones==ctl.refreshBones&&c->refreshBonesCharacter==ctl.refreshBonesCharacter&&(c->renderMark!=ctl.renderMark||c->logLevel!=ctl.logLevel||c->keys!=ctl.keys);
   if(!renderControlSeen){renderControlSeen=true;renderMarkSeen=c->renderMark;}
   if(c->logLevel!=int(Level::Debug))renderMarkSeen=c->renderMark;
   bool expMaterials=c->exp.gameMaterials!=ctl.exp.gameMaterials||c->exp.slotTextures!=ctl.exp.slotTextures;if(!(c->exp==ctl.exp))log("experiments",c->exp.text());
   ctl=*c;handoff::publishControl(ctl.enabled);
   // gameMaterials or slotTextures switched: every attached renderer gets the other kind of materials now (adopted copies
   // only for slotTextures, whose overrides are their own).
   if(expMaterials)for(auto& kv:attachments){auto& a=*kv.second;try{if(!a.adopted||a.slotMode||(ctl.exp.slotTextures&&!a.set->originalMaterials))applyMaterials(a);}catch(const std::exception& ex){log("experiment-error",a.set->name+": "+ex.what());}}
   if(!diagnosticOnly){failed.clear();attempts.clear();skipped.clear();notes.clear();findOff=false;findFails=0;findAt=0;discoverAt=0;staleNote.clear();bindingRejected.clear();bindingStamp.clear();}}

  if(!cerr.empty())log("control-error",cerr);}
 void tick(uint64_t now){if(inTick)return;bindingNow=now;if(now<next){guard(now);animationTick(now);return;}inTick=true;next=now+100;
  try{
   if(!tickTid.load())tickTid.store(threadId());
   // consume() may replace `models`: hand-off pointers into it are dropped first and set again below.
   wantedNow.clear();bindingTargets.clear();bindingReady=false;if(!worker.background()){workerTick(now);}consume();ch=current();renderTick(now);bindingStats();
   if(ch.config!=shownCharacter){shownCharacter=ch.config;log("character",ch.entity?std::to_string(ch.config):std::string("none"));}
   glowTick();
   // Reuse the verified Type discovery only here, on the existing serial Tick.
   // Diagnostic hooks never call Runtime::Impl and never scan all objects.
   if(handoff::canSeed()&&now>=handoffTypeRetry){handoffTypeRetry=now+1000;
    try{handoff::ProbeScope probe;for(Ptr r:worldRenderers())if(alive(r)){Ptr type=rendererType(r);Ptr go=e.get(static_cast<abi::Offset>(handoff::componentGameObjectRva),r);require(alive(go)&&load<Ptr>(go)!=0,"Renderer GameObject missing");
      pin(handoffTypeRoots,type);handoff::gameObjectClass.store(load<Ptr>(go),std::memory_order_release);handoff::rendererType.store(type,std::memory_order_release);log("handoff-type","SkinnedMeshRenderer Type ready");break;}}
    catch(const std::exception& ex){log("handoff-type-wait",ex.what());}}
   if(ch.entity&&ctl.exportBones!=exportSeen&&!manualPending&&now>=manualRetryAt){manualRetryAt=now+500;
    try{if(!exportBones(now,false)&&manualWait!=exportWait){manualWait=exportWait;log("bone-export-wait",exportWait+"; retrying automatically");}}
    catch(const std::exception& ex){if(manualWait!=ex.what()){manualWait=ex.what();log("bone-export-error",manualWait+"; retrying automatically");}}}
   // The bone library feeds one-click import, so it is recorded even while replacement is switched off:
   // First capture or an explicitly invalidated capture only. Retry every 500 ms without a limit until validated;
   // after the worker confirms the write/read-back, stop scanning this character until a new capture is requested.
   if(ch.entity&&!library.count(ch.config)&&!libraryPending.count(ch.config)&&now>=libraryRetry[ch.config]){
    libraryRetry[ch.config]=now+500;
    try{if(!exportBones(now,true)){libraryRetry[ch.config]=now+500;auto& last=libraryWait[ch.config];if(last.first!=exportWait||now>=last.second+30000){last={exportWait,now};log("bone-library-wait",std::to_string(ch.config)+(exportWait.empty()?"":": "+exportWait));}}}
    catch(const std::exception& ex){libraryRetry[ch.config]=now+500;auto& last=libraryError[ch.config];if(last!=ex.what()){last=ex.what();log("bone-library-error",std::to_string(ch.config)+": "+ex.what()+"; retrying automatically");}}}
   if(now>=restorePruneAt){restorePruneAt=now+500;pruneRestoreBackups();}
   if(!ctl.enabled){bool done=true;if(!sets.empty()||!attachments.empty())done=releaseAll();status(done?"disabled":"waiting",done?"":"正在恢复原模型；暂时失败的渲染器会单独重试",{},{});inTick=false;return;}
   auto wanted=chooseModels();wantedNow=wanted;std::set<std::string> wantedPaths;for(auto* m:wanted)wantedPaths.insert(m->path);
   preload(wanted,wantedPaths);processPendingBinds(now);saveVerified();
   for(auto& kv:attachments)if(wantedSet(*kv.second->set))guardMaterials(*kv.second,now);
   slotCheck(now);
   // Sets no longer wanted give their renderers back and stay built for quick switching back: released after a minute
   // unused, or sooner, oldest first, while they hold more than 256 MiB together. Wanted sets stay built while unused,
   // so the hand-off can attach them the moment the character appears.
   // Mesh experiments switched: models built with the other vertex data are built again.
   {std::vector<Set*> stale;for(auto& s:sets)if(s->meshKey!=ctl.exp.meshKey(s->animation.loop.spec.present()))stale.push_back(s.get());for(Set* x:stale){log("experiment-rebuild",x->name);releaseSet(x);}}
   bool restoring=false;for(auto& s:sets)if(!wantedPaths.count(s->path)&&!detachSet(*s))restoring=true;
   {std::vector<Set*> idle;for(auto& s:sets){bool attached=false;for(auto& a:attachments)attached|=a.second->set==s.get();if(attached)s->used=now;else if(!wantedPaths.count(s->path))idle.push_back(s.get());}
    std::sort(idle.begin(),idle.end(),[](Set* a,Set* b){return a->used>b->used;});uint64_t kept=0;
    for(Set* x:idle){if(now>x->used+idleMs||kept+x->bytes>idleBudget){releaseSet(x);}else{kept+=x->bytes;}}}
   std::vector<Set*> live;for(auto* m:wanted)if(Set* s=liveSet(*m))live.push_back(s);
   if(now>=discoverAt){discoverAt=now+500;discover(live,now);if(rendererTypeObject&&!goClass)try{seedGameObjectClass();}catch(const std::exception& ex){log("spawn-ready-error",ex.what());}for(auto* m:wanted)try{learnTargets(*m);}catch(const std::exception& ex){log("target-error",ex.what());}
    live.clear();for(auto* m:wanted)if(Set* s=liveSet(*m))live.push_back(s);}  // a moved target releases its set
   prepareBindingTargets();
   // Variant choice from control.json, applied in place on built sets.
   for(Set* s:live)logicKeys(*s);
   for(Set* s:live)try{if(s->logic.driver&&!animated(*s)){if(!s->logic.want.empty())applyVariant(*s,s->logic.want);continue;}auto v=ctl.variables.find(s->name);applyVariant(*s,pick(s->variants,v==ctl.variables.end()?std::map<std::string,int>{}:v->second));}catch(const std::exception& ex){log("variant-error",s->name+": "+ex.what());}
   bool budget=true;std::vector<std::string> active,errors,building;std::string waiting,animationWaiting;bool stale=false;
   for(auto* m:wanted){
    if(!findSet(m->path,m->generation)&&!liveSet(*m)&&shadowed(*m,wanted))continue;
    auto f=failed.find(m->path);if(f!=failed.end()&&f->second.first==m->generation){errors.push_back(m->name+": "+f->second.second);continue;}
    {auto lod=lodTarget(*m);if(!lod.empty()){errors.push_back(m->name+": 转换时骨骼库记录的是低精度模型 "+lod+"，骨骼对不上；在 App 里重新转换这个 MOD");continue;}}
    Set* cur=findSet(m->path,m->generation);if(cur&&cur->built){auto mw=materialWait.find(m->path),st=staleNote.find(m->path);if(mw!=materialWait.end()){waiting=m->name+": "+mw->second;stale=true;}else if(st!=staleNote.end()){waiting=m->name+": "+st->second;stale=true;}continue;}
    std::vector<Fingerprint> targets;if(!targetsFor(*m,targets)){waiting=m->name+": "+targetWait(*m,now);stale=staleNote.count(m->path);if(m->animated&&ch.entity&&m->id==ch.config)animationWaiting=waiting;continue;}
    auto tpl=templatesFor(*m,targets);
    if(!cur){if(!tpl){auto st=staleNote.find(m->path);if(st!=staleNote.end()){waiting=m->name+": "+st->second;stale=true;}
      else{auto& why=waitReason(*m,targets,now);if(waiting.empty()){waiting=m->name+": "+(why.reason.empty()?std::string("等待该角色出现"):why.reason);stale=why.plain;}}
      requestLoad(*m);continue;}LoadResult res;if(!takeLoaded(*m,res)){requestLoad(*m);if(!takeLoaded(*m,res)){building.push_back(m->name);continue;}}
     if(!res.error.empty()){failed[m->path]={m->generation,res.error};log("model-error",m->name+": "+res.error);errors.push_back(m->name+": "+res.error);continue;}
     cur=startSet(*m,res,targets,now);}
    if(!budget||!tpl){building.push_back(m->name);continue;}budget=false;
    const Seen& used=(*tpl)[cur->parts.size()];
    try{buildStep(*cur,used);}catch(const std::exception& ex){
     // A template with an unusual skeleton can fail where another renderer of the same asset works: try up to three.
     releaseSet(cur);skipped.insert({used.renderer,alive(used.renderer)?load<Ptr>(used.renderer+0x10):0});
     if(++attempts[m->path+"|"+m->generation]<3){log("build-retry",m->name+": "+ex.what());building.push_back(m->name);continue;}
     failed[m->path]={m->generation,ex.what()};log("model-error",m->name+": "+ex.what());errors.push_back(m->name+": "+ex.what());continue;}
    if(cur->built)promote(*cur,now);else building.push_back(m->name);
   }
   animationPrepare(now);for(auto& s:sets)if(wantedSet(*s)&&s->animation.stopped)errors.push_back(s->name+"：动画已暂停："+s->animation.error);
   for(auto& n:notes)errors.push_back(n);
   for(auto& kv:attachments){auto& n=kv.second->set->name;if(std::find(active.begin(),active.end(),n)==active.end())active.push_back(n);}
   if(restoring)status("waiting","正在恢复已停用 MOD 的原模型；暂时失败的渲染器会单独重试",active,errors);
   else if(!animationWaiting.empty()&&errors.empty())status("waiting",animationWaiting,active,errors);
   else if(!active.empty())status("active",errors.empty()?"":errors[0],active,errors);
   else if(!building.empty())status("loading",building[0],active,errors);
   else if(!errors.empty())status("error",errors[0],active,errors);
   else status("waiting",wanted.empty()?(switchedOff()?"显示替换已关闭，保留游戏原模型（在 App 的款式里打开）":"没有勾选的模型"):(waiting.empty()?"等待角色出现":waiting)+(stale?"":findOff?"（全场景扫描不可用，目前只认当前操控的角色；GIBox Log.txt 里有 renderer-type-dump）":"（大世界、队伍界面、角色详情页任一处出现即可）"),active,errors);
  }catch(const std::exception& ex){log("tick-error",ex.what());status("error",ex.what(),{},{ex.what()});}
  inTick=false;animationTick(now);}
};
Runtime::Runtime(Engine e,std::string pkg,std::string priv,std::string media,bool threaded):p(std::make_unique<Impl>(std::move(e),std::move(pkg),std::move(priv),std::move(media),threaded)){}
Runtime::~Runtime()=default; // Engine cleanup requires shutdown() on the game thread; the worker is joined here.
// Every engine call below these entry points is GIBox's own (glow trace: "GIBox" rather than "game").
void Runtime::tick(uint64_t now){glow::OwnScope own;p->tick(now);}
void Runtime::spawned(Ptr go)noexcept{glow::OwnScope own;try{p->spawned(go);}catch(...){}}
void Runtime::spawnedAfter()noexcept{glow::OwnScope own;try{p->spawnedAfter();}catch(...){}}
Ptr Runtime::bindMesh(Ptr renderer,Ptr mesh)noexcept{glow::OwnScope own;try{return p->bindMesh(renderer,mesh);}catch(...){return mesh;}}
void Runtime::meshBound(Ptr renderer)noexcept{glow::OwnScope own;try{p->meshBound(renderer);}catch(...){}}
void Runtime::shutdown(){glow::OwnScope own;glow::trace().armed.store(false);handoff::disarm();p->renderBinding.clear();p->renderRecorder.reset();p->renderReader.reset();if(p->releaseAll()){p->pruneRestoreBackups(true);p->freeOwned(p->handoffTypeRoots);p->freeOwned(p->slotRoots);p->slotBlock=0;}}
}
