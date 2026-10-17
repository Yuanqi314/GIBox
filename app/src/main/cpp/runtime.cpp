#include "runtime.hpp"
#include "model.hpp"
#include "logfmt.hpp"
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
struct ControlState {bool enabled=false;int logLevel=int(Level::Info);std::vector<std::string> selected;long long exportBones=0;std::map<std::string,std::map<std::string,int>> variables;};
ControlState parseControl(const std::string& raw){
 Json c=JsonParser(raw).parse();require(c.type==Json::Object,"Control JSON object");ControlState s;
 const auto& a=c.at("selected");require(a.type==Json::Array&&a.array.size()<=128,"Selected list");
 for(auto& v:a.array){require(v.type==Json::String&&!v.string.empty()&&v.string.find('/')==v.string.npos&&v.string!="."&&v.string!="..","Selected name");s.selected.push_back(v.string);}
 auto integerOf=[](const Json& j){if(j.type==Json::Null)return 0LL;require(j.type==Json::Number&&std::floor(j.number)==j.number&&std::abs(j.number)<=9007199254740991.,"Control counter");return static_cast<long long>(j.number);};
 s.enabled=c.at("enabled").type==Json::Bool&&c.at("enabled").boolean;s.exportBones=integerOf(c.at("exportBones"));
 const auto& vars=c.at("variables");require(vars.type==Json::Object||vars.type==Json::Null,"Variables object");
 for(auto& model:vars.object){require(model.second.type==Json::Object,"Variables object");for(auto& kv:model.second.object){require(kv.first.size()>1&&kv.first[0]=='$',"Variable name");auto n=integerOf(kv.second);require(std::abs(n)<=1000000,"Variable value");s.variables[model.first][kv.first]=int(n);}}
 s.logLevel=logThreshold(c);
 return s;
}
// lodMesh: the converter's target is a lower level of detail (a skeleton library recorded while the character was still
// loading, before 0.6.5); the part's bone indices follow that LOD's shorter bone list, so the model is not built.
struct PartInfo {std::string name,group,lodMesh;bool hasTarget=false;Fingerprint target;};
// A model folder as the scan sees it (model.json + learned.json; model.bin is only read when the model is loaded).
// id 0 + names: converted without the skeleton library; the character is found by the names in its skeleton chain.
struct ModelInfo {std::string name,path,generation,stamp;unsigned id=0;std::vector<PartInfo> parts;std::vector<std::string> names;Learned learned;};
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
   auto summary=readSummary(path);ModelInfo m;m.name=name;m.path=path;m.stamp=stamp;m.generation="g:"+summary.generation;m.id=summary.id;m.names=summary.names;m.learned=readLearned(path);
   if(!m.id&&m.learned.id)m.id=m.learned.id;
   // Targets the runtime learned (also after a game update moved a part) take precedence over the converter's.
   for(auto& p:summary.parts){PartInfo pi;pi.name=p.name;pi.group=p.renderer;pi.hasTarget=p.hasTarget;pi.target=p.target;if(p.hasTarget&&lowerLod(p.target.mesh))pi.lodMesh=p.target.mesh;
    // A target learned from a lower level of detail (before 0.6.5) is dropped and learned again from the full mesh.
    auto l=m.learned.targets.find(p.name);if(l!=m.learned.targets.end()&&sameRenderer(l->second,p)&&!lowerLod(l->second.mesh)){pi.target=l->second;pi.hasTarget=true;}m.parts.push_back(pi);}
   next[path]=m;out.push_back(m);
  }catch(const std::exception&){}
 }
 cache.swap(next);std::sort(out.begin(),out.end(),[](auto& a,auto& b){return a.name<b.name;});return out;
}
// Everything a build needs, read and checked off the game thread (model.hpp).
std::shared_ptr<ModelFile> loadModel(const ModelInfo& info){
 auto m=loadModelFile(info.path);require("g:"+m->generation==info.generation,"model.json changed while loading");
 m->name=info.name;m->id=info.id;require(m->parts.size()==info.parts.size(),"model.json changed while loading");return m;
}
// skeletons/<name>/skeleton.json, swapped in whole.
void writeSkeleton(const std::string& root,const std::string& name,bool replace,const std::string& json){
 auto dest=root+"/"+name,tmp=root+"/."+name+".tmp";std::filesystem::remove_all(tmp);
 if(!replace){require(!exists(dest),"Skeleton export directory exists");}std::filesystem::create_directories(tmp);
 write(tmp+"/skeleton.json",json);if(replace)std::filesystem::remove_all(dest);std::filesystem::rename(tmp,dest);
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

struct Runtime::Impl {
 Engine e;std::string pkg,priv,shared,modelRoot,boneRoot;
 // ---- shared with the worker (guarded by sm)
 std::mutex sm;std::optional<ControlState> pendingControl;std::string controlError;std::vector<ModelInfo> scanned;bool scannedFresh=false;
 struct LoadResult {std::string generation,error;std::shared_ptr<ModelFile> data;};
 std::map<std::string,LoadResult> loaded;std::set<std::string> inflight;
 // ---- worker only
 std::string controlRaw;uint64_t controlAt=0,scanAt=0;std::map<std::string,ModelInfo> scanCache;
 // ---- game thread only
 struct Owned {std::vector<Ptr> objects;std::vector<uint32_t> handles;};
 struct MatTex {bool original=true;Ptr main=0,light=0,normal=0;};
 struct BuiltPart {std::string name;Ptr mesh=0;Fingerprint target;std::vector<std::string> bones;Bytes indices;std::vector<std::vector<Submesh>> subs;std::vector<std::vector<Material>> rows;
  Ptr originMesh=0,originMaterials=0;
  int bindable=-1;};  // our mesh carries the same number of bone-name hashes as the original (checked once)  // the asset's own mesh and materials, from the first attachment (pinned by the set)
 struct Set:Owned {std::string name,path,generation;unsigned id=0;std::shared_ptr<ModelFile> data;std::vector<Fingerprint> targets;std::vector<BuiltPart> parts;std::map<std::string,MatTex> textures;Variants variants;bool originalMaterials=false,built=false;unsigned combo=0;uint64_t used=0;};
 // source: for a renderer the game created from one of ours (adopted), the renderer whose material clones it shows.
 struct Attachment:Owned {Ptr renderer=0,native=0,original=0,originalMaterials=0,source=0;Set* set=nullptr;size_t part=0;bool adopted=false;std::map<std::string,Ptr> clones;};
 struct Seen {Ptr renderer=0,mesh=0;Fingerprint fp;};
 struct Character {Ptr entity=0;unsigned runtime=0,config=0;} ch;
 ControlState ctl;std::vector<ModelInfo> models;long long exportSeen=0;
 std::vector<std::unique_ptr<Set>> sets;std::map<Ptr,std::unique_ptr<Attachment>> attachments;std::vector<Seen> seen;
 std::map<std::pair<Ptr,uint32_t>,Fingerprint> fpCache;std::set<std::pair<Ptr,Ptr>> skipped;
 std::map<std::string,std::pair<std::string,std::string>> failed;            // path -> generation, message
 std::map<std::string,std::map<std::string,Fingerprint>> learned;            // path -> part -> target from the world character
 std::map<std::string,Ptr> learnTried;std::set<unsigned> library;std::map<std::string,unsigned> learnedId;std::map<std::pair<Ptr,Ptr>,std::vector<std::string>> chains;std::map<std::pair<Ptr,Ptr>,std::string> rendererNames;std::map<unsigned,uint64_t> libraryRetry;std::map<unsigned,std::string> libraryError;std::map<std::string,unsigned> attempts;  // path|generation -> failed builds
 std::map<std::string,std::string> staleNote;
 // Async hand-off: wanted models of the last tick (valid until the next tick's consume), GameObject class, Tick thread.
 std::vector<const ModelInfo*> wantedNow;std::vector<Ptr> spawnAttached;Ptr goClass=0;std::atomic<int> tickTid{0};std::atomic<uint64_t> offThread{0};uint64_t spawnOffThread=0,reclaims=0,guardAt=0,binds=0,passes=0;
 std::atomic<int> logLevel{int(Level::Info)};std::map<std::string,std::string> learnedGen;  // log threshold (control.json "logLevel"); generation the learned targets belong to
 Ptr rendererTypeObject=0;bool typeDumped=false;bool findOff=false;unsigned findFails=0;uint64_t next=0,discoverAt=0,findAt=0;bool inTick=false;std::string lastStatus;std::vector<std::string> notes;
 Owned handoffTypeRoots;uint64_t handoffTypeRetry=0;
 // Last scan, for the wait reason: world character's renderers, FindObjectsOfType results (-1: not run), renderers left
 // out because an attach failed on them; scanCount numbers the scans. Per model: scan number, reason, details logged, time.
 int scanWorld=0,scanAll=-1;unsigned scanSkipped=0;uint64_t scanCount=0;unsigned shownCharacter=~0u;bool controlSeen=false;std::string exportWait;
 struct WaitWhy {uint64_t scan=~0ull;std::string reason;bool plain=false;std::string logged;uint64_t loggedAt=0;};std::map<std::string,WaitWhy> waitWhy;
 Worker worker;  // last member: destroyed (joined) first, while everything it touches still exists

 Impl(Engine x,std::string p,std::string d,std::string media,bool threaded):e(std::move(x)),pkg(std::move(p)),priv(std::move(d)),worker(threaded){
  require(pkg=="com.miHoYo.Yuanshen"||pkg=="com.miHoYo.ys.bilibili","Unsupported package");shared=media+"/"+pkg+"/GIBox";modelRoot=shared+"/models";boneRoot=shared+"/skeletons";
  for(auto& s:{priv,shared,modelRoot,boneRoot})std::filesystem::create_directories(s);
  logLevel.store(logThresholdOf(shared));
  worker.start([this]{workerTick(clockMs());});
 }

 // ---------------------------------------------------------------- worker side
 void workerTick(uint64_t now){
  if(now>=controlAt){controlAt=now+100;try{auto file=shared+"/control.json";if(exists(file)){auto raw=text(file);if(raw!=controlRaw){controlRaw=raw;scanAt=0;
     try{auto c=parseControl(raw);logLevel.store(c.logLevel);std::lock_guard<std::mutex> l(sm);pendingControl=c;controlError.clear();}catch(const std::exception& ex){std::lock_guard<std::mutex> l(sm);controlError=ex.what();}}}}catch(const std::exception&){}}
  if(now>=scanAt){scanAt=now+1000;try{auto list=scanModels(modelRoot,scanCache);std::lock_guard<std::mutex> l(sm);scanned=std::move(list);scannedFresh=true;}catch(const std::exception&){}}
 }
 void requestLoad(const ModelInfo& m){
  auto key=m.path+"|"+m.generation;{std::lock_guard<std::mutex> l(sm);if(inflight.count(key))return;auto it=loaded.find(m.path);if(it!=loaded.end()&&it->second.generation==m.generation)return;inflight.insert(key);}
  worker.post([this,m,key]{LoadResult r;r.generation=m.generation;try{r.data=loadModel(m);}catch(const std::exception& ex){r.error=ex.what();}std::lock_guard<std::mutex> l(sm);loaded[m.path]=std::move(r);inflight.erase(key);});
 }
 bool takeLoaded(const ModelInfo& m,LoadResult& out){std::lock_guard<std::mutex> l(sm);auto it=loaded.find(m.path);if(it==loaded.end()||it->second.generation!=m.generation)return false;out=std::move(it->second);loaded.erase(it);return true;}
 // "GIBox Log.txt" next to status.json (readable without root), XXMI Launcher line format (logfmt.hpp). The message
 // starts with the state key; the level comes from the key, lines below control.json "logLevel" are dropped here.
 void log(const std::string& state,const std::string& detail=""){auto level=levelOf(state);if(int(level)<logLevel.load(std::memory_order_relaxed))return;
  auto dir=shared,line=logLine("runtime",level,detail.empty()?state:state+' '+detail);worker.post([dir,line]{appendLog(dir,line);});}
 void writeLater(const std::string& file,const std::string& body){worker.post([file,body]{write(file,body);});}
 // learned.json: what the folder had when it was scanned, plus everything learned for it this session.
 void saveLearned(const ModelInfo& m){Learned l=m.learned;auto s=learned.find(m.path);if(s!=learned.end())for(auto& kv:s->second)l.targets[kv.first]=kv.second;
  auto id=learnedId.find(m.path);if(id!=learnedId.end())l.id=id->second;auto file=m.path+"/learned.json",body=learnedJson(l);worker.post([file,body]{writeAtomic(file,body);});}

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
 void extraTexture(Ptr mat,Ptr tx,std::initializer_list<const char*> properties){for(auto prop:properties){Ptr property=e.call<Ptr>(abi::newString,Ptr(0),prop);require(property!=0,"Managed string allocation failed");if(e.call<uint32_t>(abi::hasProperty,mat,property,Ptr(0))&1u){e.call<void>(abi::setTexture,mat,property,tx,Ptr(0));return;}}}
 Character current(){Ptr root=load<Ptr>(e.base+abi::manager);if(!root)return {};Ptr m=load<Ptr>(root+0x8c00);if(!m)return {};unsigned id=load<uint32_t>(m+0x288);Ptr list=load<Ptr>(m+0x10);if(!id||!list)return {};Ptr a=load<Ptr>(list+0x10);unsigned n=load<uint32_t>(list+0x18);if(!a||n>1024||length(a,1024)<n)return {};for(unsigned i=0;i<n;i++){Ptr ent=load<Ptr>(a+0x20+i*8);if(ent&&load<uint32_t>(ent+0x430)==id){unsigned config=load<uint32_t>(ent+0x3a4);if(config>=10000000&&config<=10001999)return {ent,id,config};}}return {};}
 // The renderer owner is null while the character is still loading: no renderers yet, not an error.
 std::vector<Ptr> worldRenderers(){if(!ch.entity)return {};Ptr o=load<Ptr>(ch.entity+0xf8);if(!o)return {};Ptr list=load<Ptr>(o+0x68);if(!list)return {};return pointers(list,64);}
 // Original mesh asset under a renderer, even while one of our meshes is attached to it.
 Ptr originalMesh(Ptr r){auto a=attachments.find(r);if(a!=attachments.end()&&a->second->native==load<Ptr>(r+0x10))return a->second->original;return e.get(abi::getMesh,r);}
 Fingerprint fingerprint(Ptr mesh){Ptr native=load<Ptr>(mesh+0x10);auto key=std::make_pair(native,load<uint32_t>(native+8));auto it=fpCache.find(key);if(it!=fpCache.end())return it->second;
  Ptr data=load<Ptr>(native+0x48);Fingerprint f;f.mesh=name(mesh);f.vertices=load<uint32_t>(data+0x74);f.submeshes=load<uint32_t>(data+0xc0);f.bones=uint32_t(load<uint64_t>(data+0xe0));
  if(fpCache.size()>16384){fpCache.clear();}fpCache[key]=f;return f;}
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
  for(auto& n:ctl.selected)for(auto& m:models)if(m.name==n&&taken.insert(m.path).second)out.push_back(&m);
  return out;}
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
 std::vector<std::string> chain(Ptr r){auto key=std::make_pair(r,load<Ptr>(r+0x10));auto it=chains.find(key);if(it!=chains.end())return it->second;std::vector<std::string> out;Owned tmp;
  try{Ptr ba=e.get(abi::getBones,r);if(ba){pin(tmp,ba);auto bones=pointers(ba,512);Ptr t=bones.empty()?0:bones[0];for(int i=0;i<24&&t&&alive(t);i++){for(auto& x:nameTokens(name(t)))out.push_back(x);t=e.get(abi::getParent,t);}}}catch(...){freeOwned(tmp);throw;}
  freeOwned(tmp);if(chains.size()>2048)chains.clear();chains[key]=out;return out;}
 // Targets learned this session (also after a game update moved a part) take precedence over the folder's.
 bool targetsFor(const ModelInfo& m,std::vector<Fingerprint>& out){out.clear();auto l=learned.find(m.path);
  for(auto& p:m.parts){if(l!=learned.end()&&l->second.count(p.name))out.push_back(l->second.at(p.name));else if(p.hasTarget)out.push_back(p.target);else return false;}return true;}
 std::string rendererName(Ptr r){auto key=std::make_pair(r,load<Ptr>(r+0x10));auto it=rendererNames.find(key);if(it!=rendererNames.end())return it->second;if(rendererNames.size()>4096)rendererNames.clear();return rendererNames[key]=name(r);}
 // Models imported without a bone library: the first renderer whose skeleton chain carries the model's
 // character name and whose renderer name is the part's group defines the part's target (world character first).
 // A model imported for a chosen character ID (model.json "character" holds that character's model name) learns the same way
 // while the character is not on the field (party, detail page), but only from playable-character skeletons
 // (Avatar_...), and keeps its ID.
 void learnByName(const ModelInfo& m){
  std::vector<Fingerprint> t;if(targetsFor(m,t))return;std::vector<Ptr> pool;try{pool=worldRenderers();}catch(const std::exception&){}for(auto& s:seen)pool.push_back(s.renderer);
  unsigned budget=8;bool changed=false;std::set<Ptr> world0;try{for(Ptr r:worldRenderers())world0.insert(r);}catch(const std::exception&){}
  for(auto& p:m.parts){if(p.hasTarget||learned[m.path].count(p.name))continue;
   for(Ptr r:pool){if(!alive(r))continue;if(normalize(rendererName(r))!=normalize(p.group))continue;auto key=std::make_pair(r,load<Ptr>(r+0x10));if(!chains.count(key)){if(!budget)break;budget--;}{auto c=chain(r);if(!namesMatch(m.names,c)||(m.id&&std::find(c.begin(),c.end(),"avatar")==c.end()))continue;}
    Ptr mesh=originalMesh(r);if(!alive(mesh))continue;auto fp=fingerprint(mesh);if(lowerLod(fp.mesh))continue;fp.renderer=rendererName(r);learned[m.path][p.name]=fp;changed=true;log("target-learned",m.name+"/"+p.name+" by name "+m.names[0]+" ("+fp.mesh+", "+std::to_string(fp.vertices)+" vertices)");
    if(!m.id&&world0.count(r)&&ch.entity&&!learnedId.count(m.path)){learnedId[m.path]=ch.config;}
    break;}}
  if(changed)saveLearned(m);
 }
 // Folders converted before targets existed: identify each part on the world character by renderer name, once per entity.
 void learnTargets(const ModelInfo& m){
  if(m.id==0){learnByName(m);return;}
  if(!ch.entity||m.id!=ch.config){if(!m.names.empty())learnByName(m);return;}std::vector<Fingerprint> t;if(targetsFor(m,t)){checkStale(m,t);return;}auto tried=learnTried.find(m.path);if(tried!=learnTried.end()&&tried->second==ch.entity)return;
  // Only from the full meshes: right after loading the character shows a lower level of detail for a moment.
  struct World {Ptr mesh;std::string renderer,meshName;};std::vector<World> world;bool changed=false;for(Ptr r:worldRenderers()){if(!alive(r))continue;Ptr mesh=originalMesh(r);if(alive(mesh))world.push_back({mesh,name(r),name(mesh)});}
  for(auto& w:world){if(lowerLod(w.meshName))return;}learnTried[m.path]=ch.entity;
  for(auto& p:m.parts){if(p.hasTarget||learned[m.path].count(p.name))continue;std::vector<size_t> hit;for(size_t i=0;i<world.size();i++)if(normalize(p.group)==normalize(world[i].renderer)||normalize(p.group)==normalize(world[i].meshName))hit.push_back(i);
   if(hit.size()!=1){log("target-missing",m.name+"/"+p.name+": renderer match missing/ambiguous: "+p.group);continue;}
   auto fp=fingerprint(world[hit[0]].mesh);fp.renderer=world[hit[0]].renderer;learned[m.path][p.name]=fp;changed=true;log("target-learned",m.name+"/"+p.name);}
  if(changed)saveLearned(m);
 }
 // The game changed this character's model (version update): no target matches the world character any more. A part
 // follows the new mesh only when it is plainly the same asset (same renderer, same mesh name, same bone count; bones
 // are then mapped by name, or by the unchanged palette for name-imported models). Anything else (another outfit, a
 // re-rigged model) is reported instead of guessed.
 static bool lodName(const std::string& n){auto l=n;for(auto& c:l)c=char(std::tolower(static_cast<unsigned char>(c)));return l.find("_lod")!=l.npos;}
 void checkStale(const ModelInfo& m,const std::vector<Fingerprint>& targets){
  struct World {Fingerprint fp;};std::vector<World> world;
  for(Ptr r:worldRenderers()){if(!alive(r))continue;Ptr mesh=originalMesh(r);if(!alive(mesh))continue;auto fp=fingerprint(mesh);fp.renderer=rendererName(r);world.push_back({fp});}
  if(world.empty()){return;}for(auto& t:targets)for(auto& w:world)if(matches(t,w.fp)){staleNote.erase(m.path);return;}
  std::vector<std::string> changed;bool moved=false;
  for(size_t i=0;i<m.parts.size()&&i<targets.size();i++){auto& t=targets[i];std::vector<size_t> hit;bool present=false;
   bool lod=false;for(auto& w:world){if(normalize(w.fp.renderer)!=normalize(t.renderer))continue;present=true;lod=lod||(meshName(w.fp.mesh)==meshName(t.mesh)&&lodName(w.fp.mesh)!=lodName(t.mesh));}
   if(!present||lod){continue;}  // still loading, or showing another level of detail of the same mesh
   for(size_t j=0;j<world.size();j++)if(normalize(world[j].fp.renderer)==normalize(t.renderer)&&meshName(world[j].fp.mesh)==meshName(t.mesh)&&!lodName(world[j].fp.mesh)&&!lodName(t.mesh))hit.push_back(j);
   if(hit.size()!=1){std::string now;for(auto& w:world)if(normalize(w.fp.renderer)==normalize(t.renderer)){now=w.fp.mesh;break;}changed.push_back(t.renderer+" "+t.mesh+"→"+now);continue;}auto fp=world[hit[0]].fp;
   if(fp.bones!=t.bones){changed.push_back(t.renderer+" 骨骼数 "+std::to_string(t.bones)+"→"+std::to_string(fp.bones));continue;}
   learned[m.path][m.parts[i].name]=fp;moved=true;
   log("target-updated",m.name+"/"+m.parts[i].name+": "+t.mesh+" vertices "+std::to_string(t.vertices)+"->"+std::to_string(fp.vertices)+", submeshes "+std::to_string(t.submeshes)+"->"+std::to_string(fp.submeshes));}
  if(moved){saveLearned(m);Set* old=nullptr;for(auto& s:sets)if(s->path==m.path){old=s.get();break;}if(old)releaseSet(old);failed.erase(m.path);}
  if(changed.empty())staleNote.erase(m.path);
  else{std::string list;for(auto& c:changed)list+=(list.empty()?"":"、")+c;staleNote[m.path]="这个角色当前的模型和导入时不同（换了皮肤，或版本更新改了模型："+list+"），需要对应的 MOD 重新导入";}}
 Set* findSet(const std::string& path,const std::string& gen){for(auto& s:sets)if(s->path==path&&s->generation==gen)return s.get();return nullptr;}
 // Newest built set for a wanted model: the current generation, or the previous one while a reload builds.
 Set* liveSet(const ModelInfo& m){Set* best=nullptr;for(auto& s:sets)if(s->path==m.path&&s->built){if(s->generation==m.generation)return s.get();best=s.get();}return best;}
 // One renderer per part, still showing the original asset: the source of bones, bindposes and the mesh to clone.
 std::optional<std::vector<Seen>> templatesFor(const std::vector<Fingerprint>& targets){std::vector<Seen> out;
  for(auto& t:targets){bool found=false;
   // Not a renderer that already failed to attach (another skeleton on the same asset): it would fail the build too.
   for(auto& s:seen)if(matches(t,s.fp)&&alive(s.renderer)&&alive(s.mesh)&&!attachments.count(s.renderer)&&!skipped.count({s.renderer,load<Ptr>(s.renderer+0x10)})&&e.get(abi::getMesh,s.renderer)==s.mesh){out.push_back(s);found=true;break;}
   if(!found)for(auto& a:attachments)if(alive(a.first)&&alive(a.second->original)&&matches(t,fingerprint(a.second->original))){out.push_back({a.first,a.second->original,fingerprint(a.second->original)});found=true;break;}
   if(!found)return std::nullopt;}
  return out;}

 // ---------------------------------------------------------------- build (one part per tick)
 void createTextures(Set& s){for(auto& kv:s.data->materials){MatTex t;t.original=kv.second.original;if(!t.original){t.main=makeTexture(s,*kv.second.main,false);if(kv.second.light)t.light=makeTexture(s,*kv.second.light,true);if(kv.second.normal)t.normal=makeTexture(s,*kv.second.normal,true);}s.textures[kv.first]=t;}}
 void setSubmeshes(Set& s,BuiltPart& bp){Owned tmp;try{auto& subs=bp.subs[s.combo];e.op(abi::setSubmeshCount,bp.mesh,int(subs.size()));
   for(size_t i=0;i<subs.size();i++){Ptr a=arrayOf(tmp,arrayClasses.i32,bp.indices.data()+subs[i].start*4ull,subs[i].count,4);e.call<void>(abi::setSubmeshTriangles,bp.mesh,a,int(i),Ptr(0));}}catch(...){freeOwned(tmp);throw;}freeOwned(tmp);}
 void buildStep(Set& s,const Seen& tpl){
  auto& pf=s.data->parts[s.parts.size()];Ptr owner=tpl.renderer,ownerMesh=tpl.mesh;require(alive(owner)&&alive(ownerMesh),"Template renderer expired");
  if(s.parts.empty()){createTextures(s);}Owned tmp;
  try{
   Ptr boneArray=e.get(abi::getBones,owner);pin(tmp,boneArray);auto bones=pointers(boneArray,512);Ptr binds=e.get(abi::getBindposes,ownerMesh);pin(tmp,binds);require(length(binds,512)==bones.size(),"Target bone/bindpose count");
   std::vector<std::string> target;for(Ptr b:bones)target.push_back(name(b));Skin weights{pf.weights,pf.used};std::vector<int> mapping;
   if(pf.sourceBones.empty()){mapping.assign(pf.spec.bones,-1);for(unsigned i:weights.used){require(i<bones.size(),"Model uses bone "+std::to_string(i)+" but the target skeleton has "+std::to_string(bones.size()));mapping[i]=int(i);}}
   else mapping=mapBones(pf.sourceBones,target,pf.sourceBinds,arrayBytes(binds,64,512),weights.used);
   remap(weights,mapping);
   std::string report="source_index\ttarget_index\tsource_name\ttarget_name\n";for(size_t i=0;i<mapping.size();i++)report+=std::to_string(i)+'\t'+std::to_string(mapping[i])+'\t'+(pf.sourceBones.empty()?"#"+std::to_string(i):pf.sourceBones[i])+'\t'+(mapping[i]>=0?target[mapping[i]]:"")+"\n";writeLater(s.path+"/bonemap-"+pf.spec.name+".tsv",report);
   BuiltPart bp;bp.name=pf.spec.name;bp.target=s.targets[s.parts.size()];bp.bones=target;bp.subs=pf.subs;bp.rows=pf.rows;bp.indices.assign(pf.geo.idx(),pf.geo.idx()+4ull*pf.geo.ni);
   const auto& geo=pf.geo;bp.mesh=clone(s,ownerMesh);
   Ptr native=load<Ptr>(bp.mesh+0x10),data=load<Ptr>(native+0x48);require(data!=0&&load<uint64_t>(data+0xe0)==bones.size()&&load<uint32_t>(data+0xf8)==15,"Native Mesh layout");
   learnArrayClasses(ownerMesh);const auto& k=arrayClasses;
   e.op(abi::clear,bp.mesh,1);e.op(abi::setIndexFormat,bp.mesh,geo.n>65535?1:0);e.set(abi::setTriangles,bp.mesh,arrayOf(tmp,k.i32,nullptr,0,4));
   e.set(abi::setPositions,bp.mesh,arrayOf(tmp,k.v3,geo.pos(),geo.n,12));e.set(abi::setNormals,bp.mesh,arrayOf(tmp,k.v3,geo.norm(),geo.n,12));e.set(abi::setUV,bp.mesh,arrayOf(tmp,k.v2,geo.uv(),geo.n,8));
   for(auto& c:pf.channels){abi::Offset setter;Ptr cls;unsigned stride;
    if(c.first==".uv2"){setter=abi::setUV2;cls=k.v2;stride=8;}else if(c.first==".uv3"){setter=abi::setUV3;cls=k.v2;stride=8;}else if(c.first==".uv4"){setter=abi::setUV4;cls=k.v2;stride=8;}else if(c.first==".colors"){setter=abi::setColors;cls=k.c32;stride=4;}else{setter=abi::setTangents;cls=k.v4;stride=16;}
    e.set(setter,bp.mesh,arrayOf(tmp,cls,c.second.data(),geo.n,stride));}
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
   s.parts.push_back(std::move(bp));
  }catch(...){freeOwned(tmp);throw;}
  freeOwned(tmp);log("direct-mesh-prepared",s.name+"/"+pf.spec.name);
  if(s.parts.size()==s.data->parts.size()){s.built=true;s.data.reset();log("model-built",s.name+" "+s.generation);}
 }

 Set* startSet(const ModelInfo& m,LoadResult& res,const std::vector<Fingerprint>& targets,uint64_t now){
  auto s=std::make_unique<Set>();s->name=m.name;s->path=m.path;s->generation=m.generation;s->id=m.id;s->data=res.data;s->targets=targets;s->variants=res.data->variants;s->originalMaterials=res.data->originalMaterials;
  auto v=ctl.variables.find(s->name);s->combo=pick(s->variants,v==ctl.variables.end()?std::map<std::string,int>{}:v->second);s->used=now;Set* cur=s.get();sets.push_back(std::move(s));log("loading",m.name+" "+m.generation);return cur;}

 // ---------------------------------------------------------------- attachments
 void applyMaterials(Attachment& a){Set& s=*a.set;auto& bp=s.parts[a.part];if(s.originalMaterials){e.set(abi::setMaterials,a.renderer,a.originalMaterials);return;}
  auto originals=pointers(a.originalMaterials,64);std::vector<Ptr> mats;
  for(auto& row:bp.rows[s.combo]){auto key=row.name+"#"+std::to_string(row.slot);auto it=a.clones.find(key);Ptr m=0;if(it!=a.clones.end()&&alive(it->second))m=it->second;
   else{require(row.slot<originals.size()&&alive(originals[row.slot]),"Original material missing");m=clone(a,originals[row.slot]);auto& t=s.textures.at(row.name);
    if(!t.original){e.set(abi::mainTexture,m,t.main);if(t.light)extraTexture(m,t.light,{"_LightMap","_LightMapTex","_Lightmap","_LightmapTex"});if(t.normal)extraTexture(m,t.normal,{"_BumpMap","_NormalMap","_BaseNormalMap","_NormalTex"});}a.clones[key]=m;}
   mats.push_back(m);}
  Owned tmp;try{e.set(abi::setMaterials,a.renderer,array(tmp,a.originalMaterials,mats.data(),mats.size(),8));}catch(...){freeOwned(tmp);throw;}freeOwned(tmp);}
 bool attach(Ptr r,Ptr mesh,Set& s,size_t part,uint64_t now){
  auto& bp=s.parts[part];auto a=std::make_unique<Attachment>();a->renderer=r;a->native=load<Ptr>(r+0x10);a->original=mesh;a->set=&s;a->part=part;bool meshSet=false;
  try{Ptr ba=e.get(abi::getBones,r);pin(*a,ba);auto bones=pointers(ba,512);bool same=bones.size()==bp.bones.size();for(size_t i=0;same&&i<bones.size();i++)same=alive(bones[i])&&name(bones[i])==bp.bones[i];
   if(!same){skipped.insert({r,a->native});freeOwned(*a);log("attach-skip",name(r)+": bone order differs from "+s.name+"/"+bp.name);return false;}
   Ptr mats=e.get(abi::getMaterials,r);length(mats,64);pin(*a,r);pin(*a,mesh);pin(*a,mats);a->originalMaterials=mats;
   e.set(abi::setMesh,r,bp.mesh);meshSet=true;applyMaterials(*a);
   if(!alive(bp.originMesh)){bp.originMesh=mesh;bp.originMaterials=mats;pin(s,mesh);pin(s,mats);}}
  catch(...){if(meshSet&&alive(r))e.set(abi::setMesh,r,mesh);freeOwned(*a);throw;}
  log("attached",s.name+"/"+bp.name+" -> "+name(r));attachments[r]=std::move(a);s.used=now;return true;}
 // Attach failures stay visible in status until the next control change.
 void note(const std::string& m){log("attach-error",m);if(std::find(notes.begin(),notes.end(),m)==notes.end()&&notes.size()<8)notes.push_back(m);}
 // Does the renderer still show material clones of ours (its own, or the source's for an adopted copy)?
 bool showsClones(Ptr r){Ptr mats=e.get(abi::getMaterials,r);if(!mats)return false;std::set<Ptr> current;for(Ptr m:pointers(mats,64))current.insert(m);
  for(auto& kv:attachments){for(auto& c:kv.second->clones)if(current.count(c.second))return true;}return false;}
 // Original materials go back only while the renderer still shows our clones: once they are destroyed, a renderer still
 // pointing at them draws magenta (0.3.5: the game swaps the mesh, we dropped the attachment and destroyed its clones).
 void restoreMaterials(Attachment& a){if(!alive(a.renderer)||load<Ptr>(a.renderer+0x10)!=a.native||!showsClones(a.renderer))return;
  unsigned dead=0;for(Ptr m:pointers(a.originalMaterials,64))if(!alive(m))dead++;if(dead)log("restore-materials-dead",std::to_string(dead)+" original material(s) already destroyed by the game");
  e.set(abi::setMaterials,a.renderer,a.originalMaterials);}
 void restore(Attachment& a){if(!alive(a.renderer)||load<Ptr>(a.renderer+0x10)!=a.native)return;
  if(alive(a.original)&&e.get(abi::getMesh,a.renderer)==a.set->parts[a.part].mesh){e.set(abi::setMesh,a.renderer,a.original);}restoreMaterials(a);}
 // The game puts its own mesh back on renderers we attached: inside the hand-off callback, and whenever it changes the
 // level of detail (Body, Body_LOD1..3). When the new mesh is the same asset (mesh name without the LOD suffix, same
 // skeleton), ours goes straight back and the game's mesh becomes the one restored later. Materials are re-applied only
 // if the game replaced them too.
 bool reclaim(Attachment& a,Ptr mesh){auto& bp=a.set->parts[a.part];if(!alive(mesh)||!alive(a.renderer)||load<Ptr>(a.renderer+0x10)!=a.native)return false;
  auto fp=fingerprint(mesh);if(meshName(fp.mesh)!=meshName(bp.target.mesh))return false;
  Owned tmp;bool ok=false;
  try{Ptr ba=e.get(abi::getBones,a.renderer);pin(tmp,ba);auto bones=pointers(ba,512);bool same=bones.size()==bp.bones.size();for(size_t i=0;same&&i<bones.size();i++)same=alive(bones[i])&&name(bones[i])==bp.bones[i];
   if(same){
    // Restoring goes back to the LOD0 original: the renderer's bones stay bound for that layout (ours is built on it).
    if(matches(bp.target,fp)||!alive(a.original)){pin(a,mesh);a.original=mesh;}
    if(!showsClones(a.renderer)){Ptr mats=e.get(abi::getMaterials,a.renderer);pin(a,mats);a.originalMaterials=mats;e.set(abi::setMesh,a.renderer,bp.mesh);applyMaterials(a);}
    else e.set(abi::setMesh,a.renderer,bp.mesh);
    ok=true;if(++reclaims<=20)log("reclaimed",a.set->name+"/"+bp.name+" <- "+fp.mesh+(reclaims==20?" (further reclaims not logged)":""));}}
  catch(...){freeOwned(tmp);throw;}freeOwned(tmp);return ok;}
 // Between ticks (the Tick hook runs every frame; at most every 5 ms here): put ours back the moment the game has swapped
 // the mesh, so a swap shows for at most one frame.
 void guard(uint64_t now){if(inTick||!ctl.enabled||attachments.empty()||now<guardAt)return;guardAt=now+5;inTick=true;
  try{for(auto& kv:attachments){auto& a=*kv.second;if(!alive(a.renderer)||load<Ptr>(a.renderer+0x10)!=a.native)continue;Ptr m=e.get(abi::getMesh,a.renderer);if(m&&m!=a.set->parts[a.part].mesh)reclaim(a,m);}}
  catch(const std::exception& ex){log("guard-error",ex.what());}inTick=false;}
 // A single attachment going away (its renderer died, or the game replaced its mesh): renderers adopted from it still show
 // its material clones, so those move to the first of them instead of being destroyed.
 void detach(Attachment& a,bool restoreFirst){if(restoreFirst)restore(a);Attachment* heir=nullptr;
  for(auto& kv:attachments){auto& o=*kv.second;if(&o==&a||o.source!=a.renderer)continue;if(!heir){heir=&o;}else{o.source=heir->renderer;}}
  if(heir){heir->source=0;for(auto& c:a.clones){if(!alive(c.second))continue;auto it=std::find(a.objects.begin(),a.objects.end(),c.second);if(it==a.objects.end())continue;a.objects.erase(it);pin(*heir,c.second);heir->objects.push_back(c.second);heir->clones[c.first]=c.second;}}
  freeOwned(a);}
 // Whole groups go away together: every renderer gets its original back before any clone is destroyed.
 template<class F>void detachWhere(F pick){std::vector<Ptr> keys;for(auto& kv:attachments)if(pick(*kv.second))keys.push_back(kv.first);for(Ptr k:keys)restore(*attachments.at(k));for(Ptr k:keys){freeOwned(*attachments.at(k));attachments.erase(k);}}
 void detachSet(Set& s){detachWhere([&](Attachment& a){return a.set==&s;});}
 void releaseSet(Set* s){detachSet(*s);freeOwned(*s);log("model-released",s->name+" "+s->generation);sets.erase(std::remove_if(sets.begin(),sets.end(),[&](auto& x){return x.get()==s;}),sets.end());}
 void releaseAll(){detachWhere([](Attachment&){return true;});for(auto& s:sets)freeOwned(*s);sets.clear();seen.clear();skipped.clear();std::lock_guard<std::mutex> l(sm);loaded.clear();}
 // Attach every seen renderer whose original mesh matches a part of a live set.
 void attachSeen(const std::vector<Set*>& live,uint64_t now){
  for(auto& sn:seen){if(attachments.count(sn.renderer)||!alive(sn.renderer))continue;
   for(Set* s:live){bool done=false;for(size_t i=0;i<s->parts.size()&&!done;i++)if(matches(s->parts[i].target,sn.fp)){done=true;
      try{if(e.get(abi::getMesh,sn.renderer)==sn.mesh)attach(sn.renderer,sn.mesh,*s,i,now);}catch(const std::exception& ex){skipped.insert({sn.renderer,load<Ptr>(sn.renderer+0x10)});note(s->name+": "+ex.what());}}
    if(done)break;}}}
 // A renderer the game made from one of ours (instantiated from a changed prefab, or taken from a pool) already shows our
 // mesh: track it so it is restored with the others. It usually shares the source's material clones.
 void adopt(Ptr r,Set& s,size_t part,uint64_t now){auto& bp=s.parts[part];auto a=std::make_unique<Attachment>();a->renderer=r;a->native=load<Ptr>(r+0x10);a->set=&s;a->part=part;a->adopted=true;
  try{Ptr mats=e.get(abi::getMaterials,r);std::set<Ptr> current;if(mats){for(Ptr m:pointers(mats,64))current.insert(m);}Attachment* src=nullptr;
   for(auto& kv:attachments){auto& o=*kv.second;if(o.set!=&s||o.part!=part)continue;for(auto& c:o.clones)if(current.count(c.second)){src=&o;break;}if(src)break;}
   Ptr original=src?src->original:bp.originMesh,originalMats=src?src->originalMaterials:bp.originMaterials;require(alive(original)&&originalMats!=0,"Original of adopted renderer unknown");
   pin(*a,r);pin(*a,original);pin(*a,originalMats);a->original=original;a->originalMaterials=originalMats;a->source=src?src->renderer:0;}
  catch(...){freeOwned(*a);throw;}
  log("adopted",s.name+"/"+bp.name+" -> "+name(r));attachments[r]=std::move(a);s.used=now;}
 void discover(const std::vector<Set*>& live,uint64_t now){
  std::map<Ptr,std::pair<Set*,size_t>> ours;for(auto& s:sets)for(size_t i=0;i<s->parts.size();i++)ours[s->parts[i].mesh]={s.get(),i};
  auto list=allRenderers(now);
  for(auto it=attachments.begin();it!=attachments.end();){auto& a=*it->second;bool keep=alive(a.renderer)&&load<Ptr>(a.renderer+0x10)==a.native;
   if(keep){Ptr m=e.get(abi::getMesh,a.renderer);if(m&&m!=a.set->parts[a.part].mesh&&!reclaim(a,m)){keep=false;log("attachment-dropped","game put another mesh on "+rendererName(a.renderer)+(alive(m)?": "+name(m):""));restoreMaterials(a);}}
   bool giveWay=keep&&outranked(a,live);if(giveWay){keep=false;log("outranked",a.set->name+"/"+a.set->parts[a.part].name+" on "+rendererName(a.renderer)+": a model switched on earlier replaces this mesh");}
   if(!keep){detach(a,giveWay);it=attachments.erase(it);}else{a.set->used=now;++it;}}
  seen.clear();scanSkipped=0;scanCount++;
  for(Ptr r:list){try{if(!alive(r)||attachments.count(r))continue;Ptr native=load<Ptr>(r+0x10);if(skipped.count({r,native})){scanSkipped++;continue;}Ptr m=e.get(abi::getMesh,r);if(!alive(m))continue;
    auto o=ours.find(m);if(o!=ours.end()){adopt(r,*o->second.first,o->second.second,now);continue;}seen.push_back({r,m,fingerprint(m)});}
   catch(const std::exception& ex){log("discover-skip",ex.what());}}
  attachSeen(live,now);}
 // A finished build takes over in the same tick: its predecessor's renderers, and those of models switched on after it
 // that replace the same meshes.
 void promote(Set& s,uint64_t now){std::vector<std::pair<Ptr,Ptr>> moved;std::set<Ptr> taken;size_t mine=rank(s.path);
  for(auto& kv:attachments){auto& a=*kv.second;if(a.set==&s)continue;if(a.set->path==s.path||(rank(a.set->path)>mine&&claims(s,a.original))){taken.insert(kv.first);moved.push_back({a.renderer,a.original});}}
  detachWhere([&](Attachment& a){return taken.count(a.renderer)>0;});
  std::vector<Set*> old;for(auto& x:sets)if(x.get()!=&s&&x->path==s.path)old.push_back(x.get());for(Set* o:old)releaseSet(o);
  for(auto& m:moved){if(!alive(m.first)||!alive(m.second))continue;auto fp=fingerprint(m.second);for(size_t i=0;i<s.parts.size();i++)if(matches(s.parts[i].target,fp)){try{attach(m.first,m.second,s,i,now);}catch(const std::exception& ex){note(s.name+": "+ex.what());}break;}}
  attachSeen({&s},now);}
 void applyVariant(Set& s,unsigned combo){if(combo==s.combo||combo>=s.variants.combos.size())return;s.combo=combo;for(auto& bp:s.parts){setSubmeshes(s,bp);e.op(abi::meshUpload,bp.mesh,0);}
  for(auto& kv:attachments){if(kv.second->set==&s)applyMaterials(*kv.second);}log("variant",s.name+" combo "+std::to_string(combo));}

 // ---------------------------------------------------------------- async hand-off (JDIIHHFJKBM.AAKKKEMMMCA)
 // The game hands a freshly made, still inactive model GameObject to its entity here (party screen, detail page). Its
 // renderers already carry the original Mesh, so attaching before the game's own callback runs sets the model up with
 // ours before the game activates it, instead of swapping it on the next scan. Runs between ticks on the Tick thread
 // only; inTick doubles as the re-entrancy guard. A selected model that is not built yet is built right here from these
 // renderers when its files are already loaded (the tick preloads them); otherwise the scan picks it up later.
 void seedGameObjectClass(){for(Ptr r:worldRenderers())if(alive(r)){Ptr go=e.get(abi::gameObject,r);if(alive(go)&&load<Ptr>(go)){goClass=load<Ptr>(go);log("spawn-ready","GameObject class known, hand-off replacement on");}return;}}
 // Called from hooks: nothing in Impl is touched off the Tick thread.
 bool onTickThread(const char* what){int t=tickTid.load();if(t&&threadId()==t)return true;if(t&&offThread.fetch_add(1)==0)log("hook-skip",std::string(what)+" called on another thread than Tick; left to the game");return false;}
 void spawned(Ptr go){if(!onTickThread("hand-off"))return;spawnAttached.clear();
  if(inTick||!ctl.enabled||!rendererTypeObject||!goClass||wantedNow.empty())return;
  if(!alive(go)||load<Ptr>(go)!=goClass)return;
  inTick=true;Owned tmp;uint64_t now=clockMs();unsigned adopted=0,built=0;
  try{Ptr arr=e.call<Ptr>(abi::componentsInChildren,go,rendererTypeObject,uint32_t(1),Ptr(0));
   if(arr){pin(tmp,arr);std::map<Ptr,std::pair<Set*,size_t>> ours;for(auto& s:sets)for(size_t i=0;i<s->parts.size();i++)ours[s->parts[i].mesh]={s.get(),i};
    std::vector<Seen> found;
    for(Ptr r:pointers(arr,256)){if(!alive(r)||attachments.count(r)||skipped.count({r,load<Ptr>(r+0x10)}))continue;Ptr m=e.get(abi::getMesh,r);if(!alive(m))continue;
     auto o=ours.find(m);if(o!=ours.end()){adopt(r,*o->second.first,o->second.second,now);adopted++;continue;}found.push_back({r,m,fingerprint(m)});}
    if(!found.empty())for(auto* m:wantedNow){if(liveSet(*m)||shadowed(*m,wantedNow)||!lodTarget(*m).empty())continue;auto f=failed.find(m->path);if(f!=failed.end()&&f->second.first==m->generation)continue;
     std::vector<Fingerprint> targets;if(!targetsFor(*m,targets))continue;std::vector<Seen> tpl;
     for(auto& t:targets){for(auto& sn:found)if(matches(t,sn.fp)){tpl.push_back(sn);break;}}
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
    if(m==a.set->parts[a.part].mesh)kept++;else if(reclaim(a,m))back++;else lost++;}}
  catch(const std::exception& ex){log("spawn-error",ex.what());}
  if(back||lost)log("spawn-after","after the game's hand-off callback: kept "+std::to_string(kept)+", put back "+std::to_string(back)+", lost "+std::to_string(lost));
  spawnAttached.clear();inTick=false;}

 // ---------------------------------------------------------------- the game's own mesh + bone binding
 // The game rebinds renderers itself: its native LOD switch (abi::lodBind) and SkinnedMeshRenderer.BindMeshBones
 // (abi::meshBones) set the mesh through the engine's internal setter and then bind bones from that mesh's bone-name
 // hashes. On a renderer we attached, a request for the original mesh or one of its LODs is answered with ours, so the
 // setter and the bone binding both use our mesh (whose hashes are the LOD0 template's): nothing to put back afterwards.
 // Everything else passes unchanged: no attachment, another mesh, another thread, or a call made inside our own work.
 bool knownMesh(Ptr native,Fingerprint& fp){auto it=fpCache.find({native,load<uint32_t>(native+8)});if(it!=fpCache.end()){fp=it->second;return true;}
  Ptr managed=e.call<Ptr>(abi::wrapperFor,native);if(!alive(managed)||load<Ptr>(managed+0x10)!=native)return false;fp=fingerprint(managed);return true;}
 bool bindable(Attachment& a){auto& bp=a.set->parts[a.part];if(bp.bindable>=0)return bp.bindable==1;if(!alive(a.original))return false;
  uint64_t ours=load<uint64_t>(load<Ptr>(bp.mesh+0x10)+0x90),theirs=load<uint64_t>(load<Ptr>(a.original+0x10)+0x90);bp.bindable=ours&&ours==theirs?1:0;
  if(!bp.bindable)log("bind-incompatible",a.set->name+"/"+bp.name+": bone-name hashes ours "+std::to_string(ours)+", original "+std::to_string(theirs)+"; the game's rebinding is left alone");
  return bp.bindable==1;}
 Ptr bindMesh(Ptr rendererNative,Ptr meshNative){
  if(!meshNative||!rendererNative||!onTickThread("mesh binding")||!ctl.enabled||inTick||attachments.empty())return meshNative;
  Attachment* a=nullptr;for(auto& kv:attachments)if(kv.second->native==rendererNative){a=kv.second.get();break;}
  if(!a){return meshNative;}auto& bp=a->set->parts[a->part];if(!alive(bp.mesh)){return meshNative;}Ptr ours=load<Ptr>(bp.mesh+0x10);if(meshNative==ours)return meshNative;
  inTick=true;Ptr out=meshNative;
  try{Fingerprint fp;bool known=knownMesh(meshNative,fp);
   if(known&&meshName(fp.mesh)==meshName(bp.target.mesh)&&bindable(*a)){out=ours;if(++binds<=20)log("bind-substituted",a->set->name+"/"+bp.name+": game asked for "+fp.mesh+" ("+std::to_string(fp.vertices)+" vertices)"+(binds==20?" (further binds not logged)":""));}
   else if(++passes<=20)log("bind-passed",rendererName(a->renderer)+" <- "+(known?fp.mesh:std::string("unknown mesh")));}
  catch(const std::exception& ex){log("bind-error",ex.what());out=meshNative;}
  inTick=false;return out;}

 // ---------------------------------------------------------------- bone library
 // Records the world character's skeleton. Returns false while the character is still loading (no renderer with a mesh
 // and bones yet): the caller tries again shortly, nothing is logged as an error. Renderers that are there but cannot be
 // recorded are skipped; if none is left, the error says why for each kind.
 bool exportBones(uint64_t now,bool libraryDir){
  // Right after loading, the character shows a lower level of detail (Body_LOD3: fewer vertices and a shorter bone
  // list) for a moment. A record of that would not fit mods made for the full model: wait for the full meshes.
  exportWait.clear();for(Ptr r:worldRenderers()){if(!alive(r))continue;Ptr mesh=originalMesh(r);if(!alive(mesh))continue;auto fp=fingerprint(mesh);if(lowerLod(fp.mesh)){exportWait=rendererName(r)+" shows "+fp.mesh+" (a lower level of detail)";return false;}}
  std::vector<SkeletonRenderer> records;std::set<std::string> used;std::string chain;Owned roots;unsigned seenCount=0,loading=0,mismatch=0,unnamed=0,badNames=0;std::string example;
  try{for(Ptr r:worldRenderers()){seenCount++;if(!alive(r)){loading++;continue;}Ptr mesh=originalMesh(r);if(!alive(mesh)){loading++;continue;}Ptr ba=e.get(abi::getBones,r);if(!ba){loading++;continue;}pin(roots,ba);auto bones=pointers(ba,512);
    Ptr binds=e.get(abi::getBindposes,mesh);if(!binds||bones.empty()){loading++;continue;}pin(roots,binds);
    auto rn=name(r);if(length(binds,512)!=bones.size()){mismatch++;if(example.empty())example=rn+" "+std::to_string(bones.size())+" bones / "+std::to_string(length(binds,512))+" bindposes";continue;}
    std::string n;for(char c:rn)if((c>='a'&&c<='z')||(c>='A'&&c<='Z')||(c>='0'&&c<='9')||c=='_'||c=='-')n+=c;if(n.empty()){unnamed++;log("bone-export-skip","unnamed renderer");continue;}if(n.size()>60)n.resize(60);while(!used.insert(n).second)n+="_";
    SkeletonRenderer rec;rec.name=n;bool bad=false;
    for(size_t i=0;i<bones.size()&&!bad;i++){if(!alive(bones[i])){bad=true;break;}auto bn=name(bones[i]);if(bn.empty()||bn.size()>256||bn.find_first_of("\t\r\n")!=bn.npos){bad=true;break;}rec.bones.push_back(bn);Ptr parent=e.get(abi::getParent,bones[i]);auto at=std::find(bones.begin(),bones.end(),parent);rec.parents.push_back(at==bones.end()?-1:int(at-bones.begin()));}
    if(bad){badNames++;if(example.empty())example=rn+": a bone is missing, unnamed, overlong or has a tab/newline";log("bone-export-skip",n+": a bone is missing, unnamed, overlong or has a tab/newline");continue;}
    // Transform chain from the skeleton up to the scene root: carries the character's internal name for the import guess.
    if(chain.empty()){std::vector<std::string> up;Ptr t=bones[0];for(int i=0;i<24&&t&&alive(t);i++){auto tn=name(t);for(auto& c:tn)if(c=='/'||c=='\n'||c=='\r')c='_';up.push_back(tn);t=e.get(abi::getParent,t);}std::reverse(up.begin(),up.end());for(auto& x:up)chain+=(chain.empty()?"":"/")+x;}
    rec.target=fingerprint(mesh);rec.target.renderer=rn;rec.bindposes=arrayBytes(binds,64,512);records.push_back(std::move(rec));}}
  catch(...){freeOwned(roots);throw;}
  freeOwned(roots);
  if(records.empty()){if(seenCount==loading)return false;
   throw std::runtime_error("No exportable renderer: "+std::to_string(seenCount)+" renderers, "+std::to_string(loading)+" still loading, "+std::to_string(mismatch)+" with bone/bindpose counts that differ, "+std::to_string(unnamed)+" unnamed, "+std::to_string(badNames)+" with unusable bone names"+(example.empty()?"":" (e.g. "+example+")"));}
  // Only the engine reads happen here; the JSON is built on the worker, and a library record that has not changed since
  // the last session is left as it is (the skeleton library is checked every session, rewritten only after a game update).
  auto dir=std::to_string(ch.config)+(libraryDir?"":"-"+std::to_string(now));auto root=boneRoot;unsigned id=ch.config;
  worker.post([this,root,dir,libraryDir,id,chain,records]{try{auto json=skeletonJson(id,chain,records);auto file=root+"/"+dir+"/skeleton.json";
    if(libraryDir&&exists(file)){auto old=read(file,64*1024*1024);if(old.size()==json.size()&&std::equal(old.begin(),old.end(),json.begin())){log("bone-library-unchanged",root+"/"+dir);return;}}
    writeSkeleton(root,dir,libraryDir,json);log(libraryDir?"bone-library":"bones-exported",root+"/"+dir);}catch(const std::exception& ex){log("bone-export-error",ex.what());}});return true;}

 // ---------------------------------------------------------------- why a model waits
 // Its targets are known, but no renderer of the last scan shows that mesh. The status line says why in words; the log
 // ("wait-reason") gets the scan summary, each missing target and the renderers that come closest: the same mesh name
 // (another level of detail, an update) or, failing that, the same renderer name. Logged when that changes, and every
 // 30 s while it lasts, so one log shows whether the character was scanned at all and which number differs.
 static std::string describe(const Fingerprint& f){return (f.mesh.empty()?std::string("(无 Mesh 名)"):f.mesh)+"（顶点 "+std::to_string(f.vertices)+"，子网格 "+std::to_string(f.submeshes)+"，骨骼 "+std::to_string(f.bones)+"）";}
 static std::string differences(const Fingerprint& t,const Fingerprint& f){std::string d;
  auto add=[&](const char* what,uint32_t a,uint32_t b){if(a!=b)d+=std::string(d.empty()?"":"，")+what+" "+std::to_string(a)+"→"+std::to_string(b);};
  add("顶点",t.vertices,f.vertices);add("子网格",t.submeshes,f.submeshes);add("骨骼",t.bones,f.bones);return d;}
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
   if(w.reason.empty()&&!diff.empty()){w.reason="场景里有 "+t.mesh+"，但和导入时不同（"+diff+"）：可能是远处的低精度模型，或换了皮肤、版本更新";w.plain=true;}}
  // Counts change from scan to scan; the targets and candidates are what matters for "changed".
  if(details!=w.logged||now>=w.loggedAt+30000){if(!details.empty()||w.plain){log("wait-reason",m.name+": "+scan+(w.plain&&details.empty()?"："+w.reason:"")+details);}w.logged=details;w.loggedAt=now;}
  return w;}

 // ---------------------------------------------------------------- status
 void status(const std::string& state,const std::string& detail,const std::vector<std::string>& active,const std::vector<std::string>& errors){
  std::string model;if(ch.entity)try{for(Ptr r:worldRenderers()){auto a=attachments.find(r);if(a!=attachments.end()){model=a->second->set->name;break;}}}catch(const std::exception&){}
  if(model.empty()&&!active.empty())model=active[0];
  auto list=[](const std::vector<std::string>& v){std::string s="[";for(size_t i=0;i<v.size();i++)s+=(i?",":"")+quote(v[i]);return s+"]";};
  std::string s="{\"state\":"+quote(state)+",\"detail\":"+quote(detail)+",\"characterId\":"+std::to_string(ch.config)+",\"model\":"+quote(model)+",\"models\":"+list(active)+",\"errors\":"+list(errors)+",\"scan\":"+quote(findOff?"off":rendererTypeObject?"ok":"pending")+",\"buildId\":"+quote(abi::buildId)+"}";
  if(s==lastStatus){return;}lastStatus=s;auto dir=shared;worker.post([dir,s]{write(dir+"/status.json.tmp",s);std::filesystem::rename(dir+"/status.json.tmp",dir+"/status.json");});log(state,detail);}

 // ---------------------------------------------------------------- tick
 void consume(){std::optional<ControlState> c;std::string cerr;{std::lock_guard<std::mutex> l(sm);c.swap(pendingControl);cerr.swap(controlError);if(scannedFresh){models=scanned;scannedFresh=false;}}
  // A re-imported folder (new generation, maybe another character) forgets what was learned for the old one.
  for(auto& m:models){auto g=learnedGen.find(m.path);if(g!=learnedGen.end()&&g->second==m.generation)continue;learned.erase(m.path);learnedId.erase(m.path);learnTried.erase(m.path);staleNote.erase(m.path);learnedGen[m.path]=m.generation;}
  // A bone export is a request made while the game runs: the count found at start-up is not one.
  if(c&&!controlSeen){controlSeen=true;exportSeen=c->exportBones;}
  if(c){ctl=*c;handoff::publishControl(ctl.enabled);failed.clear();attempts.clear();skipped.clear();learnTried.clear();notes.clear();findOff=false;findFails=0;findAt=0;discoverAt=0;staleNote.clear();}
  if(!cerr.empty())log("control-error",cerr);}
 void tick(uint64_t now){if(inTick)return;if(now<next){guard(now);return;}inTick=true;next=now+100;
  try{
   if(!tickTid.load())tickTid.store(threadId());
   // consume() may replace `models`: hand-off pointers into it are dropped first and set again below.
   wantedNow.clear();if(!worker.background()){workerTick(now);}consume();ch=current();
   if(ch.config!=shownCharacter){shownCharacter=ch.config;log("character",ch.entity?std::to_string(ch.config):std::string("none"));}
   // Reuse the verified Type discovery only here, on the existing serial Tick.
   // Diagnostic hooks never call Runtime::Impl and never scan all objects.
   if(handoff::canSeed()&&now>=handoffTypeRetry){handoffTypeRetry=now+1000;
    try{handoff::ProbeScope probe;for(Ptr r:worldRenderers())if(alive(r)){Ptr type=rendererType(r);Ptr go=e.get(static_cast<abi::Offset>(handoff::componentGameObjectRva),r);require(alive(go)&&load<Ptr>(go)!=0,"Renderer GameObject missing");
      pin(handoffTypeRoots,type);handoff::gameObjectClass.store(load<Ptr>(go),std::memory_order_release);handoff::rendererType.store(type,std::memory_order_release);log("handoff-type","SkinnedMeshRenderer Type ready");break;}}
    catch(const std::exception& ex){log("handoff-type-wait",ex.what());}}
   if(ch.entity&&ctl.exportBones!=exportSeen){exportSeen=ctl.exportBones;try{if(!exportBones(now,false))log("bone-export-error",exportWait.empty()?std::string("the character is still loading; export again once it is on the field"):exportWait+"; export again close to the character");}catch(const std::exception& ex){log("bone-export-error",ex.what());}}
   // The bone library feeds one-click import, so it is recorded even while replacement is switched off:
   // once per character per session; renderers that are still loading are retried ten seconds later.
   // The same failure is reported once per character and session, then retried quietly every ten seconds.
   if(ch.entity&&!library.count(ch.config)&&now>=libraryRetry[ch.config]){
    try{if(exportBones(now,true))library.insert(ch.config);else{libraryRetry[ch.config]=now+1000;log("bone-library-wait",std::to_string(ch.config)+(exportWait.empty()?"":": "+exportWait));}}
    catch(const std::exception& ex){libraryRetry[ch.config]=now+10000;auto& last=libraryError[ch.config];if(last!=ex.what()){last=ex.what();log("bone-library-error",std::to_string(ch.config)+": "+ex.what());}}}
   if(!ctl.enabled){if(!sets.empty()||!attachments.empty())releaseAll();status("disabled","",{},{});inTick=false;return;}
   auto wanted=chooseModels();wantedNow=wanted;std::set<std::string> wantedPaths;for(auto* m:wanted)wantedPaths.insert(m->path);
   // Sets no longer wanted give their renderers back; at most three of them stay built for quick switching. Wanted sets
   // stay built while unused, so the hand-off can attach them the moment the character appears.
   for(auto& s:sets)if(!wantedPaths.count(s->path))detachSet(*s);
   {std::vector<Set*> idle;for(auto& s:sets){bool attached=false;for(auto& a:attachments)attached|=a.second->set==s.get();if(attached)s->used=now;else if(!wantedPaths.count(s->path))idle.push_back(s.get());}
    std::sort(idle.begin(),idle.end(),[](Set* a,Set* b){return a->used>b->used;});for(size_t i=3;i<idle.size();i++)releaseSet(idle[i]);}
   std::vector<Set*> live;for(auto* m:wanted)if(Set* s=liveSet(*m))live.push_back(s);
   if(now>=discoverAt){discoverAt=now+500;discover(live,now);if(rendererTypeObject&&!goClass)try{seedGameObjectClass();}catch(const std::exception& ex){log("spawn-ready-error",ex.what());}for(auto* m:wanted)try{learnTargets(*m);}catch(const std::exception& ex){log("target-error",ex.what());}
    live.clear();for(auto* m:wanted)if(Set* s=liveSet(*m))live.push_back(s);}  // a moved target releases its set
   // Variant choice from control.json, applied in place on built sets.
   for(Set* s:live)try{auto v=ctl.variables.find(s->name);applyVariant(*s,pick(s->variants,v==ctl.variables.end()?std::map<std::string,int>{}:v->second));}catch(const std::exception& ex){log("variant-error",s->name+": "+ex.what());}
   bool budget=true;std::vector<std::string> active,errors,building;std::string waiting;bool stale=false;
   for(auto* m:wanted){
    if(!findSet(m->path,m->generation)&&!liveSet(*m)&&shadowed(*m,wanted))continue;
    auto f=failed.find(m->path);if(f!=failed.end()&&f->second.first==m->generation){errors.push_back(m->name+": "+f->second.second);continue;}
    {auto lod=lodTarget(*m);if(!lod.empty()){errors.push_back(m->name+": 转换时骨骼库记录的是低精度模型 "+lod+"，骨骼对不上；在 App 里重新转换这个 MOD");continue;}}
    Set* cur=findSet(m->path,m->generation);if(cur&&cur->built){auto st=staleNote.find(m->path);if(st!=staleNote.end()){waiting=m->name+": "+st->second;stale=true;}continue;}
    std::vector<Fingerprint> targets;if(!targetsFor(*m,targets)){waiting=m->name+": 等待"+(m->id?"该角色":"名字含 "+m->names[0]+" 的角色")+"出现以识别部件";continue;}
    auto tpl=templatesFor(targets);
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
   for(auto& n:notes)errors.push_back(n);
   for(auto& kv:attachments){auto& n=kv.second->set->name;if(std::find(active.begin(),active.end(),n)==active.end())active.push_back(n);}
   if(!active.empty())status("active",errors.empty()?"":errors[0],active,errors);
   else if(!building.empty())status("loading",building[0],active,errors);
   else if(!errors.empty())status("error",errors[0],active,errors);
   else status("waiting",wanted.empty()?"没有勾选的模型":(waiting.empty()?"等待角色出现":waiting)+(stale?"":findOff?"（全场景扫描不可用，目前只认当前操控的角色；GIBox Log.txt 里有 renderer-type-dump）":"（大世界、队伍界面、角色详情页任一处出现即可）"),active,errors);
  }catch(const std::exception& ex){log("tick-error",ex.what());status("error",ex.what(),{},{ex.what()});}
  inTick=false;}
};
Runtime::Runtime(Engine e,std::string pkg,std::string priv,std::string media,bool threaded):p(std::make_unique<Impl>(std::move(e),std::move(pkg),std::move(priv),std::move(media),threaded)){}
Runtime::~Runtime()=default; // Engine cleanup requires shutdown() on the game thread; the worker is joined here.
void Runtime::tick(uint64_t now){p->tick(now);}
void Runtime::spawned(Ptr go)noexcept{try{p->spawned(go);}catch(...){}}
void Runtime::spawnedAfter()noexcept{try{p->spawnedAfter();}catch(...){}}
Ptr Runtime::bindMesh(Ptr renderer,Ptr mesh)noexcept{try{return p->bindMesh(renderer,mesh);}catch(...){return mesh;}}
void Runtime::shutdown(){handoff::disarm();p->releaseAll();p->freeOwned(p->handoffTypeRoots);}
}
