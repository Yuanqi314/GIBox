#pragma once
// Index buffers as the game uploads them through Vulkan, hashed the way 3DMigoto hashes a D3D11 buffer (0.7.56): a
// read-only diagnostic, off by default (control.json textureMods.ibHash, read once at start). It answers one question:
// are this build's index buffers byte for byte the PC ones, so that a GIMI hash such as "hash = 773fbe7b" can be
// recomputed on Android? Nothing here changes what the game draws.
//
// 3DMigoto (XXMI-Libs-Package a7847eae, DirectX11/HackerDevice.cpp CreateBuffer):
//   hash = data_hash = crc32c_hw(0, pInitialData->pSysMem, pDesc->ByteWidth);
//   hash = crc32c_hw(hash, pDesc, sizeof(D3D11_BUFFER_DESC));
// crc32c_hw (util.h) is crc32c_append (crc32c-hw-1.0.5): CRC-32C, the seed and the result inverted, so appending
// continues the same CRC. The PC desc is not stored anywhere on Android; the 0.7.57 device capture solved it from the
// rule hashes of two mods: ByteWidth = the data's size, Usage DEFAULT, BindFlags INDEX_BUFFER | SHADER_RESOURCE |
// UNORDERED_ACCESS (0x8A, Linnea: head/body/dress, bangs, eyes, book, crystal) or INDEX_BUFFER alone (0x2, the
// Traveler), CPU access, misc flags and stride 0. Both are hashed, IMMUTABLE too, for the data as uploaded, widened from
// 16 to 32 bit, or narrowed from 32 to 16 bit.
//
// Modes (1.0.3, control.json textureMods.hashMode, read at the game's start), after XXMI's normal play and its hunting /
// frame analysis: "match" hashes each upload once, as uploaded (every hash this device ever tied to a PC one was that
// form), and checks it against the mods' rules; "collect" also ties every shown mesh (characters, NPCs, monsters) to
// its upload for hash-map.json; "full" is the old diagnostic: every form, ib-hashes.txt, per-mesh candidate lines.
// The optional draw probe watches a set of hashes against the same uploads without enabling mesh collection.
#include <algorithm>
#include <initializer_list>
#include <iterator>
#include <array>
#include <chrono>
#include <atomic>
#include <charconv>
#include <cstdint>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <deque>
#include <functional>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <tuple>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>
#if defined(__aarch64__)
#include <arm_acle.h>
#include <sys/auxv.h>
#endif

namespace ms::vkcap {
enum class Mode {Off=0,Match=1,Collect=2,Full=3};
// hashMode when given; else the switches of 1.0.2 and earlier: capture -> collect, ibHash -> match.
inline Mode modeFrom(const std::string& mode,bool ibHash,bool capture){
 if(mode=="off"){return Mode::Off;}if(mode=="match"){return Mode::Match;}if(mode=="collect"){return Mode::Collect;}if(mode=="full"){return Mode::Full;}
 return capture?Mode::Collect:ibHash?Mode::Match:Mode::Off;}
inline const char* modeName(Mode m){return m==Mode::Match?"匹配":m==Mode::Collect?"采集":m==Mode::Full?"完整诊断":"关闭";}
inline std::optional<uint32_t> drawHash(const std::string& value){
 if(value.size()!=8)return std::nullopt;
 uint32_t hash=0;auto r=std::from_chars(value.data(),value.data()+value.size(),hash,16);
 if(r.ec!=std::errc{}||r.ptr!=value.data()+value.size())return std::nullopt;
 return hash;
}
inline const std::array<uint32_t,256>& crcTable(){static const auto table=[]{std::array<uint32_t,256> t{};
  for(uint32_t i=0;i<256;i++){uint32_t c=i;for(int k=0;k<8;k++)c=(c&1)?(c>>1)^0x82F63B78u:c>>1;t[i]=c;}return t;}();return table;}
#if defined(__aarch64__)
__attribute__((target("crc"))) inline uint32_t crcArm(uint32_t c,const uint8_t* p,size_t n){
 for(;n&&(reinterpret_cast<uintptr_t>(p)&7);n--)c=__crc32cb(c,*p++);
 for(;n>=8;n-=8,p+=8){uint64_t v;std::memcpy(&v,p,8);c=__crc32cd(c,v);}
 for(;n;n--)c=__crc32cb(c,*p++);return c;}
inline bool crcHardware(){static const bool on=(getauxval(AT_HWCAP)&(1ul<<7))!=0;return on;}   // HWCAP_CRC32
#endif
// crc32c_append: crc32c(crc32c(0,a),b) is the CRC of a followed by b.
inline uint32_t crc32c(uint32_t seed,const void* data,size_t n){auto* p=static_cast<const uint8_t*>(data);uint32_t c=~seed;
#if defined(__aarch64__)
 if(crcHardware())return ~crcArm(c,p,n);
#endif
 auto& t=crcTable();for(size_t i=0;i<n;i++)c=t[(c^p[i])&0xff]^(c>>8);return ~c;}

struct D3D11BufferDesc {uint32_t byteWidth=0,usage=0,bindFlags=0,cpuAccessFlags=0,miscFlags=0,structureByteStride=0;};
static_assert(sizeof(D3D11BufferDesc)==24,"D3D11_BUFFER_DESC is 24 bytes");
// variant = format * 4 + kind; format: 0 as uploaded, 1 widened 16->32 bit, 2 narrowed 32->16 bit; kind: 0 the data
// alone (3DMigoto's data_hash), 1 DEFAULT + INDEX_BUFFER, 2 DEFAULT + INDEX_BUFFER | SHADER_RESOURCE | UNORDERED_ACCESS,
// 3 IMMUTABLE + INDEX_BUFFER.
struct Hash {uint32_t value=0;uint8_t variant=0;};
inline std::string variantName(uint8_t v){static const char* formats[]={"as uploaded","16-bit widened to 32-bit","32-bit narrowed to 16-bit"};
 static const char* kinds[]={"data only","desc DEFAULT, Bind=IB","desc DEFAULT, Bind=IB|SRV|UAV","desc IMMUTABLE, Bind=IB"};
 return std::string(formats[(v>>2)%3])+", "+kinds[v&3];}
inline void hashVariants(uint32_t data,size_t n,uint8_t format,std::vector<Hash>& out){out.push_back({data,uint8_t(format*4)});
 const uint32_t kinds[3][2]={{0,0x2},{0,0x8A},{1,0x2}};   // Usage, BindFlags
 for(uint8_t k=0;k<3;k++){D3D11BufferDesc d;d.byteWidth=uint32_t(n);d.usage=kinds[k][0];d.bindFlags=kinds[k][1];out.push_back({crc32c(data,&d,sizeof d),uint8_t(format*4+k+1)});}}
inline void hashFormat(const uint8_t* p,size_t n,uint8_t format,std::vector<Hash>& out){hashVariants(crc32c(0,p,n),n,format,out);}
template<class From,class To>
inline void indexWidthHashes(const uint8_t* p,size_t n,uint8_t format,std::vector<Hash>& out){
 std::array<To,1024> converted;uint32_t crc=0;const size_t count=n/sizeof(From);
 for(size_t at=0;at<count;at+=converted.size()){
  const auto batch=std::min(converted.size(),count-at);
  for(size_t i=0;i<batch;i++){
   From v;std::memcpy(&v,p+(at+i)*sizeof(From),sizeof v);
   if constexpr(sizeof(From)>sizeof(To)){if(v>0xffff)return;}
   converted[i]=To(v);
  }
  crc=crc32c(crc,converted.data(),batch*sizeof(To));
 }
 hashVariants(crc,count*sizeof(To),format,out);
}
inline std::vector<Hash> ibHashes(const uint8_t* p,size_t n){std::vector<Hash> out;hashFormat(p,n,0,out);
 if(n%2==0)indexWidthHashes<uint16_t,uint32_t>(p,n,1,out);
 if(n%4==0)indexWidthHashes<uint32_t,uint16_t>(p,n,2,out);
 return out;}

// One index upload: where it came from, its hashes and enough of its content to compare by hand.
struct Blob {uint64_t seq=0,buffer=0,offset=0;uint32_t bytes=0;std::string via;std::vector<Hash> hashes;std::vector<uint16_t> head16;uint32_t max16=0,max32=0;};
struct Stats {uint64_t uploads=0,bytes=0,copies=0,mappedBinds=0,notMapped=0,outOfRange=0,tooLarge=0,dropped=0,resolved=0,wrapped=0,indexedDraws=0,drawProbeUploads=0;bool installed=false;std::string error;};
struct Copy {uint64_t srcOffset=0,dstOffset=0,size=0;};   // VkBufferCopy
struct Draw {
 uint64_t upload=0,buffer=0,uploadOffset=0,bindOffset=0;
 uint32_t first=0,relativeFirst=0,count=0,instances=0,firstInstance=0;
 int32_t baseVertex=0;uint32_t indexBits=0,hash=0;
};
struct DrawStats {uint32_t hash=0;uint64_t uploads=0;size_t draws=0;};
struct SkinHit {
 unsigned profile=~0u,slot=0;uint32_t first=0,count=0;
 std::optional<uint32_t> iniHash{};
 explicit operator bool()const{return profile!=~0u;}
};
struct SkinRoute {uint32_t hash=0,total=0;unsigned profile=0;std::vector<std::pair<uint32_t,uint32_t>> ranges;bool pcRule=false;};
struct IniSkinRule {unsigned profile=0,slot=0;int64_t first=-1,count=-1;};
enum class ResourceKind : uint8_t {Index,Vertex,Texture,Shader,Constant,Storage,Total};
struct ResourceSlot {
 ResourceKind kind;uint32_t set=0,binding=0,element=0;
 bool operator<(const ResourceSlot& other)const{return std::tie(kind,set,binding,element)<std::tie(other.kind,other.set,other.binding,other.element);}
};
struct ResourceMatch {ResourceSlot slot;uint32_t hash;};
struct IndexIdentity {uint32_t hash,first,total;};
struct RenderDraw {
 bool indexed=false;
 uint32_t count=0,instances=0,first=0,firstInstance=0;
 int32_t baseVertex=0;
 std::vector<IndexIdentity> indices{};
};
struct ResourceRule {IniSkinRule draw;std::vector<ResourceMatch> all;};
using Profiles=std::set<unsigned>;
struct Registration {
 std::vector<std::string> skinMods,renderMods;
 std::unordered_map<uint32_t,SkinRoute> skinRoutes;
 std::unordered_map<uint32_t,std::vector<IniSkinRule>> iniSkinRules;
 std::map<uint32_t,Profiles> renderIndexPrograms;
 std::map<uint32_t,uint32_t> renderIndexLengths;
 std::vector<ResourceRule> resourceRules;
 bool renderResourceContexts=false,logicResources=false;
};
struct ResourceSample {ResourceMatch identity;bool compute=false;uint64_t pass=0;uint32_t first=0,count=0;};
struct ResourceBinding {ResourceKind kind;uint64_t handle=0,offset=0,size=~0ull,generation=0;};
struct ImageCopy {uint64_t offset;uint32_t rowLength,mip,layer,width,height;bool whole;};
inline std::string resourceSlotName(const ResourceSlot& s){
 switch(s.kind){
  case ResourceKind::Index:return "ib";
  case ResourceKind::Vertex:return "vb"+std::to_string(s.binding);
  case ResourceKind::Shader:switch(s.binding){case 1:return "vs";case 2:return "hs";case 4:return "ds";case 8:return "gs";case 16:return "ps";case 32:return "cs";default:return "shader";}
  default:return std::string(s.kind==ResourceKind::Texture?"tex":s.kind==ResourceKind::Constant?"cb":"sb")+
                 std::to_string(s.set)+"."+std::to_string(s.binding)+(s.element?"."+std::to_string(s.element):"");
 }
}
inline std::optional<std::vector<ResourceMatch>> resourceMatches(std::string_view text){
 auto trim=[](std::string_view s){auto a=s.find_first_not_of(" \t");return a==s.npos?std::string_view{}:s.substr(a,s.find_last_not_of(" \t")-a+1);};
 auto number=[](std::string_view s,unsigned& n){if(s.empty())return false;auto p=std::from_chars(s.data(),s.data()+s.size(),n);return p.ec==std::errc{}&&p.ptr==s.data()+s.size();};
 std::vector<ResourceMatch> out;std::set<ResourceSlot> seen;
 while(!text.empty()&&out.size()<8){
  auto comma=text.find(',');auto item=trim(text.substr(0,comma));auto colon=item.find(':');
  if(colon==item.npos)return std::nullopt;
  auto key=trim(item.substr(0,colon));auto hash=drawHash(std::string(trim(item.substr(colon+1))));if(!hash)return std::nullopt;
  ResourceSlot s{ResourceKind::Index};
  if(key!="ib"){
   constexpr std::string_view names[]={"vs","hs","ds","gs","ps","cs"};
   const auto stage=std::find(std::begin(names),std::end(names),key);
   if(stage!=std::end(names)){s.kind=ResourceKind::Shader;s.binding=1u<<(stage-names);}
   else if(key.substr(0,2)=="vb"){
    s.kind=ResourceKind::Vertex;if(!number(key.substr(2),s.binding)||s.binding>=32)return std::nullopt;
   }else{
    auto prefix=key.substr(0,3);size_t width=2;
    if(prefix=="tex"){s.kind=ResourceKind::Texture;width=3;}
    else if(key.substr(0,2)=="cb")s.kind=ResourceKind::Constant;
    else if(key.substr(0,2)=="sb")s.kind=ResourceKind::Storage;
    else return std::nullopt;
    auto fields=key.substr(width);auto dot=fields.find('.');if(dot==fields.npos||!number(fields.substr(0,dot),s.set)||s.set>=8)return std::nullopt;
    fields.remove_prefix(dot+1);dot=fields.find('.');
    if(!number(fields.substr(0,dot),s.binding)||s.binding>=4096)return std::nullopt;
    if(dot!=fields.npos&&(!number(fields.substr(dot+1),s.element)||s.element>=1024))return std::nullopt;
   }
  }
  if(!seen.insert(s).second)return std::nullopt;
  out.push_back({s,*hash});
  if(comma==text.npos)return out;
  text.remove_prefix(comma+1);
 }
 return std::nullopt;
}
struct SkinLayoutMiss {unsigned profile;uintptr_t pipeline;std::string details;};
enum class SkinMiss : unsigned {DrawRange,Ambiguous,Graphics,SkinOutput,Bones,Resources,Pose,VertexBindings,Material,ComputeDescriptors,Logic,Total};
enum class SkinPhase : unsigned {Index,Textures,Association,Pose,FirstDraw,Total};
using SkinChoices=std::map<std::string,std::map<std::string,int>>;
struct LogicPersist {std::string main;std::map<std::string,float> values;};
struct SkinKey {std::string mod,section;size_t file=0;int select=-1,hold=0;bool back=false;};
inline constexpr uint32_t paimonBodyHash=0x5b4cdc9cu;
// A narrow test route. Do not allow a partial match (instances, index format, range or base vertex).
inline int paimonTintPart(uint32_t hash,uint64_t uploadBytes,uint32_t first,uint32_t count,uint32_t width,uint32_t instances,int32_t baseVertex,uint32_t firstInstance){
 if(hash!=paimonBodyHash||uploadBytes!=61224||width!=2||instances!=1||baseVertex||firstInstance)return -1;
 return first==0&&count==26226?0:first==26226&&count==4386?1:-1;
}
// SPIR-V OpName and OpDecorate suffice for this deliberately narrow prototype. A shader stripped of names,
// using decoration groups or containing an unexpected binding does not prove this target.
inline std::optional<uint32_t> namedTextureBinding(const uint32_t* code,size_t words,std::initializer_list<const char*> names){
 if(!code||words<5||words>(1u<<18)||code[0]!=0x07230203u)return std::nullopt;
 uint32_t id=0;
 for(size_t i=5;i<words;){size_t n=code[i]>>16;if(!n||n>words-i)return std::nullopt;
  if((code[i]&0xffff)==5&&n>=3)for(const auto* name:names){
   const size_t length=std::strlen(name)+1;
   if((n-2)*4>=length&&!std::memcmp(code+i+2,name,length)){
    if(!code[i+1]||(id&&id!=code[i+1]))return std::nullopt;
    id=code[i+1];break;
   }
  }
  i+=n;}
 if(!id)return std::nullopt;
 bool set=false;std::optional<uint32_t> binding;
 for(size_t i=5;i<words;){size_t n=code[i]>>16;
  if((code[i]&0xffff)==71&&n==4&&code[i+1]==id){
   if(code[i+2]==34){if(code[i+3])return std::nullopt;set=true;}
   if(code[i+2]==33){if(binding&&*binding!=code[i+3])return std::nullopt;binding=code[i+3];}}
  i+=n;}
 return set?binding:std::nullopt;
}
inline bool mainTextureBinding(const uint32_t* code,size_t words){return namedTextureBinding(code,words,{"_MainTex"})==0u;}
inline std::array<unsigned,4> materialTextureBindings(const uint32_t* code,size_t words){
 return {namedTextureBinding(code,words,{"_MainTex"}).value_or(~0u),
         namedTextureBinding(code,words,{"_LightMapTex","_LightMap","_LightmapTex","_Lightmap"}).value_or(~0u),
         namedTextureBinding(code,words,{"_BumpMap","_NormalMap","_BaseNormalMap","_NormalTex"}).value_or(~0u),
         namedTextureBinding(code,words,{"_ShadowRamp","_ShadowRampTex","_RampTex"}).value_or(~0u)};
}

// The buffers, memory bindings and host mappings needed to find index data in CPU memory: a staging buffer copied
// into an index buffer (read when the copy is recorded), or an index buffer in mapped memory (read at its first bind).
class Tracker : public Registration {
 struct UploadHash {uint32_t value=0;uint8_t widths=0;}; // bit 0: raw 16-bit input, bit 1: raw 32-bit input
 struct Upload {uint64_t seq,offset,bytes;std::array<UploadHash,12> hashes{};size_t count=0;};
 struct Probe {uint64_t uploads=0;size_t reported=0;std::vector<Draw> draws;};
 struct ResourceSpan {uint64_t offset,size;uint32_t hash;};
 struct Buffer {uint64_t size=0,memory=0,memoryOffset=0,generation=0;uint32_t usage=0,skinCrc=0;bool bound=false,captured=false;
  std::vector<Upload> matches;std::vector<ResourceSpan> signatures;};
 struct Mapping {const uint8_t* base=nullptr;uint64_t offset=0,size=0;};
 struct IndexBinding {uint64_t buffer,generation,offset;uint32_t width;};
 struct Image {unsigned width,height,mips,layers,blockWidth,blockHeight,blockBytes;uint64_t generation;
  std::map<std::pair<unsigned,unsigned>,uint32_t> hashes{};};
 struct View {uint64_t image,generation,id;unsigned mip,layer;};
 struct Pipeline {uint64_t layout=0;std::map<unsigned,uint32_t> stages;};
 using DescriptorKey=std::pair<unsigned,unsigned>;
 struct DescriptorLayout {std::vector<DescriptorKey> dynamic;std::map<unsigned,unsigned> counts;};
 struct DescriptorSet {uint64_t generation,pool;std::shared_ptr<const DescriptorLayout> layout{};std::map<DescriptorKey,ResourceBinding> slots{};};
 struct BoundSet {uint64_t handle=0,generation=0;std::map<DescriptorKey,uint32_t> offsets{};};
 struct Bindings {uint64_t pipeline=0,layout=0;std::array<BoundSet,8> sets{};};
 struct Command {std::array<ResourceBinding,32> vertices{};Bindings graphics,compute;};
 struct PendingWrite {uint64_t handle,generation,offset,size;unsigned mip=0,layer=0;bool image=false;std::optional<uint32_t> hash;};
 std::mutex m;std::unordered_map<uint64_t,Buffer> buffers;std::unordered_map<uint64_t,Mapping> maps;std::vector<Blob> out;Stats st;uint64_t seq=0;
 std::unordered_map<uintptr_t,IndexBinding> drawBindings;
 std::unordered_map<uint64_t,Image> images;
 std::unordered_map<uint64_t,View> views;
 std::unordered_map<uint64_t,uint32_t> shaders;
 std::unordered_map<uint64_t,Pipeline> pipelines;
 std::unordered_map<uint64_t,std::shared_ptr<const DescriptorLayout>> descriptorLayouts;
 std::unordered_map<uint64_t,DescriptorSet> descriptors;
 std::unordered_map<uintptr_t,Command> commands;
 std::unordered_map<uintptr_t,std::vector<PendingWrite>> pending;
 std::unordered_set<uintptr_t> pendingOverflow;
 std::vector<ResourceSample> resourceSamples;
 std::array<uint64_t,2> resourcePasses{};
 std::array<std::set<std::tuple<bool,ResourceSlot,uint32_t>>,size_t(ResourceKind::Total)> sampled;
 std::map<uint32_t,Probe> probes; // keys set once before the loader hooks are installed
 std::vector<SkinLayoutMiss> skinLayoutMisses;
 std::unordered_set<uint64_t> pcIndexBytes16,pcIndexBytes32;
 uint64_t bufferGeneration=0;
 static constexpr uint32_t indexUsage=0x40,transferSrc=0x1,resourceUsages=0xf0;
 static constexpr size_t maxBlob=64u<<20,maxQueue=65536,maxResourceWrites=2048;
 static bool skinSize(uint64_t n){return n==335880||n==268704;}
 // Host address of [memory offset, +size) or null when that range is not mapped.
 const uint8_t* host(const Buffer& b,uint64_t at,uint64_t size){auto x=maps.find(b.memory);if(!b.bound||x==maps.end()||!x->second.base||at>~0ull-b.memoryOffset)return nullptr;
  const auto from=b.memoryOffset+at;if(from<x->second.offset)return nullptr;
  const auto rel=from-x->second.offset;if(rel>x->second.size||size>x->second.size-rel)return nullptr;
  return x->second.base+rel;}
 static bool overlaps(uint64_t a,uint64_t n,uint64_t b,uint64_t s){return a<b+s&&b<a+n;}
 void remember(const PendingWrite& w){
  if(w.image){auto i=images.find(w.handle);if(i==images.end()||i->second.generation!=w.generation)return;
   const auto key=std::make_pair(w.mip,w.layer);if(w.hash)i->second.hashes[key]=*w.hash;else i->second.hashes.erase(key);return;}
  auto it=buffers.find(w.handle);if(it==buffers.end()||it->second.generation!=w.generation)return;
  auto& a=it->second.signatures;a.erase(std::remove_if(a.begin(),a.end(),[&](const auto& r){return overlaps(w.offset,w.size,r.offset,r.size);}),a.end());
  if(w.hash){if(a.size()>=16)a.erase(a.begin());a.push_back({w.offset,w.size,*w.hash});}
 }
 void saveWrite(uintptr_t command,const PendingWrite& w){
  if(!command){remember(w);return;}
  auto& writes=pending[command];
  if(writes.size()<maxResourceWrites)writes.push_back(w);
  else pendingOverflow.insert(command);
 }
 std::optional<uint32_t> bufferResource(uintptr_t command,const ResourceBinding& ref,bool staging=false){
  if(pendingOverflow.count(command))return {};
  auto lookup=buffers.find(ref.handle);if(lookup==buffers.end()||ref.offset>=lookup->second.size)return {};
  auto& b=lookup->second;
  if(ref.generation&&ref.generation!=b.generation)return {};
  const auto n=ref.size==~0ull?b.size-ref.offset:ref.size;
  if(!n||n>b.size-ref.offset)return {};
  const auto use=[&](uint64_t offset,uint64_t size){return offset==ref.offset&&(ref.size==~0ull||size==n);};
  if(auto p=pending.find(command);p!=pending.end())for(auto w=p->second.rbegin();w!=p->second.rend();++w)
   if(!w->image&&w->handle==ref.handle&&w->generation==b.generation&&overlaps(ref.offset,n,w->offset,w->size))
    return use(w->offset,w->size)?w->hash:std::nullopt;
  const auto limit=(ref.kind==ResourceKind::Constant||ref.kind==ResourceKind::Storage)&&!staging?65536:maxBlob;
  if(n<=limit)if(const auto* p=host(b,ref.offset,n))return crc32c(0,p,size_t(n));
  for(auto it=b.signatures.rbegin();it!=b.signatures.rend();++it)if(use(it->offset,it->size))return it->hash;
  return {};
 }
 std::optional<uint32_t> textureResource(uintptr_t command,uint64_t handle,uint64_t generation){
  const auto view=views.find(handle);if(view==views.end())return {};
  if(view->second.id!=generation)return {};
  const auto& v=view->second;const auto image=images.find(v.image);
  if(image==images.end()||image->second.generation!=v.generation)return {};
  if(auto p=pending.find(command);p!=pending.end())for(auto w=p->second.rbegin();w!=p->second.rend();++w)
   if(w->image&&w->handle==v.image&&w->generation==v.generation&&w->mip==v.mip&&w->layer==v.layer)return w->hash;
  auto hash=image->second.hashes.find({v.mip,v.layer});if(hash!=image->second.hashes.end())return hash->second;return {};
 }
 std::optional<uint32_t> resourceHash(uintptr_t command,bool compute,const ResourceSlot& slot){
  if(pendingOverflow.count(command))return {};
  if(slot.kind==ResourceKind::Index){auto i=drawBindings.find(command);if(compute||i==drawBindings.end())return {};
   auto b=buffers.find(i->second.buffer);if(b==buffers.end()||b->second.generation!=i->second.generation)return {};
   return bufferResource(command,{slot.kind,i->second.buffer,i->second.offset});}
  auto cmd=commands.find(command);if(cmd==commands.end())return {};
  const auto& record=cmd->second;
  if(slot.kind==ResourceKind::Vertex)return !compute&&slot.binding<record.vertices.size()&&record.vertices[slot.binding].generation?bufferResource(command,record.vertices[slot.binding]):std::nullopt;
  const auto& bound=compute?record.compute:record.graphics;
  auto p=pipelines.find(bound.pipeline);if(p==pipelines.end())return {};
  if(slot.kind==ResourceKind::Shader){const auto stage=p->second.stages.find(slot.binding);
   return stage!=p->second.stages.end()?std::optional<uint32_t>(stage->second):std::nullopt;}
  if(p->second.layout!=bound.layout)return {};
  if(slot.set>=bound.sets.size())return {};
  const auto& set=bound.sets[slot.set];auto d=descriptors.find(set.handle);if(d==descriptors.end()||d->second.generation!=set.generation)return {};
  const auto key=std::make_pair(slot.binding,slot.element);auto it=d->second.slots.find(key);
  if(it==d->second.slots.end()||it->second.kind!=slot.kind||!it->second.generation)return {};
  if(slot.kind==ResourceKind::Texture)return textureResource(command,it->second.handle,it->second.generation);
  auto ref=it->second;if(auto off=set.offsets.find(key);off!=set.offsets.end()){if(off->second>~0ull-ref.offset)return {};ref.offset+=off->second;}
  return bufferResource(command,ref);
 }
 void sampleResources(uintptr_t command,bool compute,uint32_t first=0,uint32_t count=0){
  if(!inspectResources||(resourcePasses[compute]++&31u))return;
  auto sample=[&](const ResourceSlot& slot){auto& seen=sampled[size_t(slot.kind)];if(seen.size()>=48)return;
   if(auto hash=resourceHash(command,compute,slot);hash&&seen.emplace(compute,slot,*hash).second)
    resourceSamples.push_back({{slot,*hash},compute,resourcePasses[compute],first,count});};
  auto cmd=commands.find(command);if(cmd==commands.end())return;
  if(!compute){sample({ResourceKind::Index});for(unsigned i=0;i<cmd->second.vertices.size();i++)
   if(cmd->second.vertices[i].handle)sample({ResourceKind::Vertex,0,i});}
  const auto& state=compute?cmd->second.compute:cmd->second.graphics;
  if(auto p=pipelines.find(state.pipeline);p!=pipelines.end())for(auto& s:p->second.stages)sample({ResourceKind::Shader,0,s.first});
  for(unsigned i=0;i<state.sets.size();i++){
   const auto& set=state.sets[i];auto d=descriptors.find(set.handle);if(d==descriptors.end()||d->second.generation!=set.generation)continue;
   for(const auto& slot:d->second.slots)sample({slot.second.kind,i,slot.first.first,slot.first.second});
  }
 }
 // Under the lock: an unmap waits until the bytes are read.
 void noteSkinIndexUpload(uint32_t hash){
  if(auto entry=renderIndexPrograms.find(hash);entry!=renderIndexPrograms.end()){
   renderIndexUploads.insert(entry->second.begin(),entry->second.end());
  }
  if(auto rules=iniSkinRules.find(hash);rules!=iniSkinRules.end()){
   for(const auto& rule:rules->second){skinIndexUploads.insert(rule.profile);}
  }
  if(auto route=skinRoutes.find(hash);route!=skinRoutes.end()){skinIndexUploads.insert(route->second.profile);}
 }
 Profiles skinIndexUploads,renderIndexUploads;
 void add(const uint8_t* p,uint64_t n,uint64_t buffer,Buffer& dst,uint64_t offset,const char* via){st.uploads++;st.bytes+=n;
  if(!probing()&&out.size()>=maxQueue){st.dropped++;return;}
  Blob b;b.seq=++seq;b.buffer=buffer;b.offset=offset;b.bytes=uint32_t(n);b.via=via;
  if(allForms||hotReload)b.hashes=ibHashes(p,size_t(n));else hashFormat(p,size_t(n),0,b.hashes);
  if(probing()){
   Upload match{b.seq,offset,n};
   const bool ready=routesReady.load(std::memory_order_acquire);
   auto keep=[&](const Hash& h){auto probe=probes.find(h.value);if(probe==probes.end())return;
    const auto format=h.variant>>2;auto route=skinRoutes.find(h.value);
    if(format&&!iniSkinRules.count(h.value)&&!renderIndexPrograms.count(h.value)&&
       (route==skinRoutes.end()||!route->second.pcRule||n!=uint64_t(route->second.total)*(format==1?2u:4u)))return;
    const uint8_t widths=format?uint8_t(format==1?1:2):uint8_t(3);
    for(size_t i=0;i<match.count;i++)if(match.hashes[i].value==h.value){match.hashes[i].widths|=widths;return;}
    ++probe->second.uploads;match.hashes[match.count++]={h.value,widths};
    if(allSkins||renderTextures){noteSkinIndexUpload(h.value);}
   };
   for(const auto& h:b.hashes){
    if(hotReload){
     const auto width=h.variant>>2;
     const uint8_t widths=width?uint8_t(width==1?1:2):uint8_t(3);
     match.hashes[match.count++]={h.value,widths};
     if(ready){
      if(auto probe=probes.find(h.value);probe!=probes.end()){++probe->second.uploads;noteSkinIndexUpload(h.value);}
     }
    }else if(ready){keep(h);}
    else if(h.variant<4){match.hashes[match.count++]={h.value,3};}
   }
   // GIMI can hash 32-bit PC indices when the same mesh uses 16-bit indices on Android (or vice versa).
   // Try only widths with a verified PC route for the uploaded byte size.
   if(ready&&!allForms&&!hotReload){
    std::vector<Hash> alternate;
    if(pcIndexBytes16.count(n))indexWidthHashes<uint16_t,uint32_t>(p,size_t(n),1,alternate);
    if(pcIndexBytes32.count(n))indexWidthHashes<uint32_t,uint16_t>(p,size_t(n),2,alternate);
    for(const auto& h:alternate)keep(h);
   }
   if(match.count){if(ready)++st.drawProbeUploads;if(dst.matches.size()<16)dst.matches.push_back(match);}
  }
  if(probeOnly.load(std::memory_order_relaxed))return;
  if(out.size()>=maxQueue){st.dropped++;return;}
  for(size_t i=0;i<n/2;i++){uint16_t v;std::memcpy(&v,p+2*i,2);if(i<8)b.head16.push_back(v);if(v>b.max16)b.max16=v;}
  if(n%4==0)for(size_t i=0;i<n/4;i++){uint32_t v;std::memcpy(&v,p+4*i,4);if(v>b.max32)b.max32=v;}
  out.push_back(std::move(b));}
public:
 static constexpr size_t maxDrawsPerHash=64;
 std::atomic<bool> enabled{false};
 std::atomic<bool> routesReady{true};   // early hooks can record raw uploads while startup model data is loaded
 std::atomic<bool> allForms{true};   // false (match / collect): the data as uploaded only, one pass over it
 std::atomic<bool> probeOnly{false};
 bool tintHooks=false; // startup only; hooks are never installed by a live setting change
 std::string paimonTextureMod; // optional startup preset, chosen before installing any wrappers
 bool skinnedModel=false; // this preset uses the captured Vulkan skinning pipeline instead of Unity mesh APIs
 bool allSkins=false;
 bool renderTextures=false;
 bool hotReload=false;
 mutable std::shared_mutex registryMutex;
 std::shared_ptr<const Profiles> activeRenderMods=std::make_shared<const Profiles>();
 std::deque<std::atomic<uint64_t>> renderDraws;
 std::atomic<int> renderLoadingFit{0};
 bool inspectResources=false; // existing hash collection mode, set before installing the loader hooks
 std::atomic<bool> skinLogic{false};
 std::atomic<bool> skinSharedChoices{false};
 bool gpuEnabled()const{
  return hotReload||inspectResources||((renderTextures||allSkins)&&(!routesReady.load(std::memory_order_acquire)||
         (renderTextures&&renderResourceContexts)||(allSkins&&!resourceRules.empty())));
 }
 bool gpuBuffersEnabled()const{return gpuEnabled()||(allSkins&&logicResources);}
 std::pair<Profiles,Profiles> takeIndexUploads(){
  std::lock_guard<std::mutex> lock(m);
  std::pair<Profiles,Profiles> result;
  result.first.swap(skinIndexUploads);result.second.swap(renderIndexUploads);return result;
 }
 std::optional<uint32_t> boundResource(uintptr_t command,const ResourceSlot& slot){
  std::lock_guard<std::mutex> lock(m);return resourceHash(command,false,slot);
 }
 bool renderMatches(uintptr_t command,const RenderDraw& draw,const std::vector<ResourceMatch>& terms){
  std::lock_guard<std::mutex> lock(m);
  for(const auto& term:terms){
   if(term.slot.kind==ResourceKind::Index){
    if(!draw.indexed)return false;
    if(std::any_of(draw.indices.begin(),draw.indices.end(),[&](const auto& index){return index.hash==term.hash;}))continue;
   }
   if(resourceHash(command,false,term.slot)!=term.hash)return false;
  }
  return true;
 }
 std::optional<uint32_t> liveStorage(uintptr_t command,uint64_t handle,uint64_t bytes){
  std::lock_guard<std::mutex> lock(m);return bufferResource(command,{ResourceKind::Storage,handle,0,bytes});
 }
 void queueSkinKeys(std::vector<SkinKey> keys){
  std::lock_guard<std::mutex> lock(m);
  for(auto& key:keys){if(skinKeys.size()<256){skinKeys.push_back(std::move(key));}}
 }
 std::vector<SkinKey> takeSkinKeys(){
  std::lock_guard<std::mutex> lock(m);std::vector<SkinKey> out;out.swap(skinKeys);return out;
 }
 std::optional<std::map<std::string,float>> savedLogic(const std::string& file,const std::string& main,bool pendingOnly=false){
  std::lock_guard<std::mutex> lock(m);
  const auto& values=pendingOnly?pendingLogicValues:savedLogicValues;
  const auto at=values.find(file);if(at==values.end())return {};
  const auto& value=*at->second;
  return value.main==main?value.values:std::map<std::string,float>{};
 }
 void rememberLogic(const std::string& file,LogicPersist value,bool dirty){
  if(file.empty())return;
  std::lock_guard<std::mutex> lock(m);
  auto snapshot=std::make_shared<const LogicPersist>(std::move(value));
  if(dirty){savedLogicValues.insert_or_assign(file,snapshot);pendingLogicValues.insert_or_assign(file,std::move(snapshot));}
  else{savedLogicValues.try_emplace(file,std::move(snapshot));}
 }
 std::map<std::string,std::shared_ptr<const LogicPersist>> logicSaves(const std::string& dir){
  std::lock_guard<std::mutex> lock(m);
  std::map<std::string,std::shared_ptr<const LogicPersist>> out;
  for(auto it=pendingLogicValues.lower_bound(dir);it!=pendingLogicValues.end()&&it->first.rfind(dir,0)==0;++it){
   out.insert(*it);
  }
  return out;
 }
 void logicWritten(const std::string& file,const std::shared_ptr<const LogicPersist>& value){
  std::lock_guard<std::mutex> lock(m);
  auto at=pendingLogicValues.find(file);
  if(at!=pendingLogicValues.end()&&at->second==value){pendingLogicValues.erase(at);}
 }
private:
 std::vector<SkinKey> skinKeys;
 std::map<std::string,std::shared_ptr<const LogicPersist>> savedLogicValues,pendingLogicValues;
public:
 std::shared_ptr<const SkinChoices> skinChoices;
 std::shared_ptr<const Profiles> activeSkins=std::make_shared<const Profiles>();
 std::atomic<uint64_t> skinMatches{0},sharedDraws{0},sharedMisses{0},originalSkinJobs{0};
 std::atomic<uint64_t> lightMapDraws{0},normalMapDraws{0};
 std::deque<std::atomic<uint64_t>> profileDraws;
 std::deque<std::array<std::atomic<uint64_t>,unsigned(SkinPhase::Total)>> skinTimes;
 void noteSkinTime(unsigned profile,SkinPhase phase){
  if(profile>=skinTimes.size())return;
  auto& slot=skinTimes[profile][unsigned(phase)];if(slot.load(std::memory_order_relaxed))return;
  const auto now=std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
  uint64_t absent=0;slot.compare_exchange_strong(absent,uint64_t(now),std::memory_order_relaxed);
 }
 std::array<std::atomic<uint64_t>,unsigned(SkinMiss::Total)> skinMisses{};
 std::deque<std::atomic<uint32_t>> skinBoneErrors; // required in high bits, live palette in low bits
 std::atomic<bool> tintOn{false};
 std::atomic<int> textureUpload{0}; // 0 waiting, 1 queued on the only graphics queue, -1 failed
 std::atomic<bool> indexedDrawSeen{false};
 std::atomic<uint64_t> deviceCreates{0},queueSubmits{0};
 std::atomic<uint64_t> gpuTextureBytes{0},gpuGeometryBytes{0},gpuEvictions{0},gpuDescriptorSets{0};
 std::atomic<uint64_t> computeDispatches{0},skinKernelDispatches{0},skinOutputCaptures{0};
 std::array<std::atomic<uint64_t>,2> tintDraws{},tintMisses{};
 std::atomic<uint64_t> skinDispatches{0};
private:
 void replaceDrawHashes(const std::vector<uint32_t>& hashes){
  auto previous=std::move(probes);probes.clear();pcIndexBytes16.clear();pcIndexBytes32.clear();
  skinIndexUploads.clear();renderIndexUploads.clear();
  for(uint32_t hash:hashes){
   if(auto found=previous.find(hash);found!=previous.end()){
    probes.try_emplace(hash,std::move(found->second));previous.erase(found);
   }else{probes.try_emplace(hash);}
   auto route=skinRoutes.find(hash);if(route!=skinRoutes.end()&&route->second.pcRule){pcIndexBytes16.insert(2ull*route->second.total);pcIndexBytes32.insert(4ull*route->second.total);}
   if(auto texture=renderIndexLengths.find(hash);texture!=renderIndexLengths.end()){
    pcIndexBytes16.insert(2ull*texture->second);pcIndexBytes32.insert(4ull*texture->second);
   }}
  if(hotReload){
   for(const auto& buffer:buffers)for(const auto& upload:buffer.second.matches){
    for(size_t i=0;i<upload.count;i++){
     if(auto probe=probes.find(upload.hashes[i].value);probe!=probes.end()){
      if(!probe->second.uploads){probe->second.uploads=1;}
      noteSkinIndexUpload(probe->first);
     }
    }
   }
   routesReady.store(true,std::memory_order_release);
  }else if(!routesReady.load(std::memory_order_relaxed)){
   for(auto& b:buffers){auto& uploads=b.second.matches;
    for(auto& u:uploads){
     size_t count=0;
     for(size_t i=0;i<u.count;i++)if(auto p=probes.find(u.hashes[i].value);p!=probes.end()){
      if(std::none_of(u.hashes.begin(),u.hashes.begin()+count,[&](const auto& h){return h.value==p->first;})){
       ++p->second.uploads;u.hashes[count++]=u.hashes[i];
       if(allSkins||renderTextures){noteSkinIndexUpload(p->first);}
      }
     }
     u.count=count;if(count)++st.drawProbeUploads;
    }
    uploads.erase(std::remove_if(uploads.begin(),uploads.end(),[](const auto& u){return !u.count;}),uploads.end());
   }
   if(!inspectResources&&!renderTextures&&resourceRules.empty()){
    images.clear();views.clear();shaders.clear();pipelines.clear();descriptorLayouts.clear();descriptors.clear();
    if(!logicResources){
     for(auto& resource:buffers){resource.second.signatures.clear();}
     commands.clear();pending.clear();pendingOverflow.clear();
    }
   }
   routesReady.store(true,std::memory_order_release);
  }
 }
public:
 void setDrawHashes(const std::vector<uint32_t>& hashes){
  std::lock_guard<std::mutex> l(m);replaceDrawHashes(hashes);
 }
 void publishRegistration(Registration next,const std::vector<uint32_t>& hashes){
  std::lock_guard<std::mutex> l(m);
  static_cast<Registration&>(*this)=std::move(next);
  renderDraws.clear();profileDraws.clear();skinTimes.clear();skinBoneErrors.clear();skinLayoutMisses.clear();
  resizeProfileStats();
  replaceDrawHashes(hashes);
 }
 void resizeProfileStats(){
  while(renderDraws.size()<renderMods.size()){renderDraws.emplace_back(0);}
  while(profileDraws.size()<skinMods.size()){
   profileDraws.emplace_back(0);skinBoneErrors.emplace_back(0);skinTimes.emplace_back();
   for(auto& time:skinTimes.back()){time.store(0,std::memory_order_relaxed);}
  }
 }
 uint64_t registryVersion=0,skinVersion=0; // guarded by registryMutex, including readers of the associated arrays
 bool probing()const{return hotReload||allSkins||renderTextures||inspectResources||!probes.empty();}
 void setRenderReplacement(bool master,const std::vector<std::string>& selected,const std::string& fit){
  if(!routesReady.load(std::memory_order_acquire))return;
  resizeProfileStats();
  auto profiles=std::make_shared<Profiles>();
  if(master&&enabled){
   for(size_t i=0;i<renderMods.size();i++){
    if(std::find(selected.begin(),selected.end(),renderMods[i])!=selected.end()){profiles->insert(unsigned(i));}
   }
  }
  std::atomic_store(&activeRenderMods,std::shared_ptr<const Profiles>(std::move(profiles)));
  renderLoadingFit.store(fit=="cover"?1:fit=="stretch"?2:0,std::memory_order_relaxed);
 }
 bool ownsRenderMod(const std::string& name)const{
  std::shared_lock<std::shared_mutex> lock(registryMutex);
  return enabled&&routesReady.load(std::memory_order_acquire)&&std::find(renderMods.begin(),renderMods.end(),name)!=renderMods.end();
 }
 void setPaimonReplacement(bool master,bool tint,const std::string& textureMod){
  if(tintHooks)tintOn.store(master&&(paimonTextureMod.empty()?tint:paimonTextureMod==textureMod),std::memory_order_relaxed);
 }
 void setSkinReplacement(bool master,bool request,const std::vector<std::string>& selected){
  resizeProfileStats();
  auto profiles=std::make_shared<Profiles>();
  if(master&&request)for(size_t i=0;i<skinMods.size();i++){
   if(std::find(selected.begin(),selected.end(),skinMods[i])!=selected.end()){profiles->insert(unsigned(i));}
  }
  std::atomic_store(&activeSkins,std::shared_ptr<const Profiles>(std::move(profiles)));
 }
 bool skinActive(unsigned profile)const{return std::atomic_load(&activeSkins)->count(profile)!=0;}
 void noteSkinMiss(SkinMiss why){skinMisses[unsigned(why)].fetch_add(1,std::memory_order_relaxed);}
 void noteSkinLayout(unsigned profile,uintptr_t pipeline,const char* details){
  std::lock_guard<std::mutex> lock(m);
  if(skinLayoutMisses.size()<48)skinLayoutMisses.push_back({profile,pipeline,details});
 }
 std::vector<SkinLayoutMiss> takeSkinLayouts(){
  std::lock_guard<std::mutex> lock(m);
  std::vector<SkinLayoutMiss> out;out.swap(skinLayoutMisses);return out;
 }
 void setSkinChoices(const SkinChoices& choices){std::atomic_store(&skinChoices,std::make_shared<const SkinChoices>(choices));}
 bool skinBackendReady()const{return enabled&&allSkins&&textureUpload.load(std::memory_order_relaxed)==1&&indexedDrawSeen.load(std::memory_order_relaxed);}
 bool hasSkin(const std::string& name)const{
  std::shared_lock<std::shared_mutex> lock(registryMutex);
  return std::find(skinMods.begin(),skinMods.end(),name)!=skinMods.end();
 }
 void created(uint64_t buffer,uint64_t size,uint32_t usage){if(!(usage&(indexUsage|transferSrc))&&!(gpuBuffersEnabled()&&(usage&resourceUsages))&&!(skinnedModel&&(usage&0x20)&&skinSize(size)))return;std::lock_guard<std::mutex> l(m);Buffer b;b.size=size;b.usage=usage;if(probing())b.generation=++bufferGeneration;buffers[buffer]=std::move(b);}
 void destroyed(uint64_t buffer){std::lock_guard<std::mutex> l(m);buffers.erase(buffer);}
 void bound(uint64_t buffer,uint64_t memory,uint64_t offset){std::lock_guard<std::mutex> l(m);auto b=buffers.find(buffer);if(b==buffers.end())return;b->second.memory=memory;b->second.memoryOffset=offset;b->second.bound=true;b->second.skinCrc=0;b->second.signatures.clear();}
 void mapped(uint64_t memory,uint64_t offset,uint64_t size,const void* p){std::lock_guard<std::mutex> l(m);maps[memory]={static_cast<const uint8_t*>(p),offset,size};}
 void unmapped(uint64_t memory){std::lock_guard<std::mutex> l(m);maps.erase(memory);}
 // A copy into an index buffer: each region's source bytes, as written into the staging memory before the copy.
 void copied(uint64_t src,uint64_t dst,const Copy* regions,uint32_t count,uintptr_t command=0){
  std::lock_guard<std::mutex> l(m);auto d=buffers.find(dst);if(d==buffers.end())return;auto s=buffers.find(src);
  if(gpuBuffersEnabled()&&(d->second.usage&resourceUsages))for(uint32_t i=0;i<count;i++){
   const auto& r=regions[i];if(!r.size||r.dstOffset>d->second.size||r.size>d->second.size-r.dstOffset)continue;
   std::optional<uint32_t> hash;
   if(r.size<=maxBlob)hash=bufferResource(command,{ResourceKind::Vertex,src,r.srcOffset,r.size},true);
   saveWrite(command,{dst,d->second.generation,r.dstOffset,r.size,0,0,false,hash});
  }
  if(skinnedModel&&(d->second.usage&0x20)&&skinSize(d->second.size)){
   for(uint32_t i=0;i<count;i++){const auto& r=regions[i];d->second.skinCrc=0;if(r.dstOffset||r.size!=d->second.size||s==buffers.end()||!s->second.bound||r.srcOffset>s->second.size||r.size>s->second.size-r.srcOffset)continue;
    if(const auto* p=host(s->second,r.srcOffset,r.size))d->second.skinCrc=crc32c(0,p,size_t(r.size));}
  }
  if(!(d->second.usage&indexUsage))return;
  d->second.captured=true;
   for(uint32_t i=0;i<count;i++){const Copy& r=regions[i];st.copies++;
    auto& matches=d->second.matches;
    if(r.dstOffset>d->second.size||r.size>d->second.size-r.dstOffset){st.outOfRange++;continue;}
    if(!matches.empty())matches.erase(std::remove_if(matches.begin(),matches.end(),[&](const Upload& u){return r.dstOffset<u.offset+u.bytes&&u.offset<r.dstOffset+r.size;}),matches.end());
    if(s==buffers.end()||!s->second.bound){st.notMapped++;continue;}if(r.size>maxBlob){st.tooLarge++;continue;}
    if(r.srcOffset>s->second.size||r.size>s->second.size-r.srcOffset){st.outOfRange++;continue;}
    const uint8_t* p=host(s->second,r.srcOffset,r.size);if(!p){st.notMapped++;continue;}add(p,r.size,dst,d->second,r.dstOffset,"staging copy");}}
 // An index buffer bound for drawing that no copy filled: read from its mapped memory once, offset to its end.
 void indexBound(uint64_t buffer,uint64_t offset,uintptr_t command=0,int32_t indexType=-1){
  std::lock_guard<std::mutex> l(m);auto b=buffers.find(buffer);
  if(command&&probing())drawBindings.erase(command);
  if(b==buffers.end()||!b->second.bound||!(b->second.usage&indexUsage)||offset>=b->second.size)return;
  auto& dst=b->second;
  if(!dst.captured){
   dst.captured=true;uint64_t n=dst.size-offset;
   if(n>maxBlob)st.tooLarge++;
   else if(const uint8_t* p=host(dst,offset,n)){st.mappedBinds++;add(p,n,buffer,dst,offset,"mapped memory at its first bind (offset to the buffer's end)");}
   else st.notMapped++;
  }
  if(command&&(!dst.matches.empty()||gpuEnabled())&&(indexType==0||indexType==1)&&drawBindings.size()<512)
   drawBindings.emplace(command,IndexBinding{buffer,dst.generation,offset,indexType==0?2u:4u});
 }
 void beginCommands(uintptr_t command){std::lock_guard<std::mutex> l(m);drawBindings.erase(command);commands.erase(command);pending.erase(command);pendingOverflow.erase(command);}
 void executeCommands(uintptr_t primary,uintptr_t secondary){
  std::lock_guard<std::mutex> l(m);auto p=pending.find(secondary);
  if(pendingOverflow.count(secondary))pendingOverflow.insert(primary);
  if(p!=pending.end()&&primary!=secondary)for(const auto& w:p->second)saveWrite(primary,w);
 }
 void submitted(uintptr_t command){std::lock_guard<std::mutex> l(m);
  if(pendingOverflow.count(command)){for(auto& b:buffers)b.second.signatures.clear();for(auto& i:images)i.second.hashes.clear();return;}
  auto p=pending.find(command);if(p==pending.end())return;
  for(const auto& w:p->second)remember(w);}
 void updated(uintptr_t command,uint64_t dst,uint64_t offset,uint64_t size,const void* data){
  if(!gpuBuffersEnabled())return;
  std::lock_guard<std::mutex> l(m);auto d=buffers.find(dst);if(d==buffers.end()||offset>=d->second.size)return;
  if(size==~0ull&&!data)size=d->second.size-offset;
  if(!size||size>d->second.size-offset)return;
  std::optional<uint32_t> hash;if(data&&size<=65536)hash=crc32c(0,data,size_t(size));
  saveWrite(command,{dst,d->second.generation,offset,size,0,0,false,hash});
 }
 void imageCreated(uint64_t handle,unsigned width,unsigned height,unsigned mips,unsigned layers,unsigned bw,unsigned bh,unsigned bytes){
  std::lock_guard<std::mutex> l(m);images.insert_or_assign(handle,Image{width,height,mips,layers,bw,bh,bytes,++bufferGeneration});}
 void imageDestroyed(uint64_t handle){std::lock_guard<std::mutex> l(m);images.erase(handle);}
 void imageView(uint64_t handle,uint64_t image,unsigned mip,unsigned layer){std::lock_guard<std::mutex> l(m);views.erase(handle);
  auto i=images.find(image);if(i!=images.end()&&mip<i->second.mips&&layer<i->second.layers)views.emplace(handle,View{image,i->second.generation,++bufferGeneration,mip,layer});}
 void viewDestroyed(uint64_t handle){std::lock_guard<std::mutex> l(m);views.erase(handle);}
 void imageChanged(uintptr_t command,uint64_t dst,unsigned mip,unsigned levels,unsigned layer,unsigned layers){
  if(!gpuEnabled())return;
  std::lock_guard<std::mutex> l(m);auto target=images.find(dst);if(target==images.end())return;const auto& image=target->second;
  if(mip>=image.mips||layer>=image.layers)return;
  for(unsigned i=0;i<std::min(levels,image.mips-mip);i++)for(unsigned j=0;j<std::min(layers,image.layers-layer);j++)
   saveWrite(command,{dst,image.generation,0,0,mip+i,layer+j,true,{}});
 }
 void copiedImage(uintptr_t command,uint64_t src,uint64_t dst,const ImageCopy& copy){
  if(!gpuEnabled())return;
  std::lock_guard<std::mutex> l(m);const auto target=images.find(dst);if(target==images.end())return;
  auto& image=target->second;if(copy.mip>=image.mips||copy.layer>=image.layers)return;
  std::optional<uint32_t> hash;
  const unsigned w=std::max(1u,image.width>>copy.mip),h=std::max(1u,image.height>>copy.mip);
  const auto source=buffers.find(src);
  if(copy.whole&&w==copy.width&&h==copy.height&&source!=buffers.end()&&image.blockBytes){
   const auto cols=[&](unsigned n){return (uint64_t(n)+image.blockWidth-1)/image.blockWidth*image.blockBytes;};
   const auto bytes=cols(w),pitch=cols(copy.rowLength?copy.rowLength:w),rows=(uint64_t(h)+image.blockHeight-1)/image.blockHeight;
   const auto n=(rows-1)*pitch+bytes;
   if(pitch>=bytes&&n<=maxBlob&&copy.offset<=source->second.size&&n<=source->second.size-copy.offset){
    if(auto* hostData=host(source->second,copy.offset,n)){
     uint32_t digest=0;
     for(uint64_t row=0;row<rows;row++){
      digest=crc32c(digest,hostData+row*pitch,size_t(bytes));
     }
     hash=digest;
    }
   }
  }
  saveWrite(command,{dst,image.generation,0,0,copy.mip,copy.layer,true,hash});
 }
 void shaderCreated(uint64_t handle,const void* data,size_t size){std::lock_guard<std::mutex> l(m);shaders.erase(handle);
  if(data&&size&&size<=maxBlob)shaders.emplace(handle,crc32c(0,data,size));}
 void shaderDestroyed(uint64_t handle){std::lock_guard<std::mutex> l(m);shaders.erase(handle);}
 void pipelineCreated(uint64_t handle,uint64_t layout,const std::vector<std::pair<unsigned,uint64_t>>& stages){std::lock_guard<std::mutex> l(m);
  auto& pipeline=pipelines[handle];pipeline={layout,{}};
  for(const auto& stage:stages)if(auto s=shaders.find(stage.second);s!=shaders.end())pipeline.stages[stage.first]=s->second;}
 void pipelineDestroyed(uint64_t handle){std::lock_guard<std::mutex> l(m);pipelines.erase(handle);}
 void bindPipeline(uintptr_t command,bool compute,uint64_t handle){std::lock_guard<std::mutex> l(m);auto& c=commands[command];(compute?c.compute:c.graphics).pipeline=handle;}
 void bindVertex(uintptr_t command,unsigned slot,uint64_t handle,uint64_t offset,uint64_t size=~0ull){
  if(slot>=32)return;
  std::lock_guard<std::mutex> l(m);auto b=buffers.find(handle);
  commands[command].vertices[slot]={ResourceKind::Vertex,handle,offset,size,b==buffers.end()?0:b->second.generation};
 }
 void descriptorLayout(uint64_t layout,std::vector<DescriptorKey> entries,std::map<unsigned,unsigned> counts){std::sort(entries.begin(),entries.end());
  auto definition=std::make_shared<const DescriptorLayout>(DescriptorLayout{std::move(entries),std::move(counts)});
  std::lock_guard<std::mutex> l(m);descriptorLayouts[layout]=std::move(definition);}
 void descriptorLayoutDestroyed(uint64_t layout){std::lock_guard<std::mutex> l(m);descriptorLayouts.erase(layout);}
 void descriptorAllocated(uint64_t handle,uint64_t pool,uint64_t layout){std::lock_guard<std::mutex> l(m);DescriptorSet set{++bufferGeneration,pool};
  if(auto i=descriptorLayouts.find(layout);i!=descriptorLayouts.end())set.layout=i->second;
  descriptors.insert_or_assign(handle,std::move(set));}
 void descriptorReleased(uint64_t handle){std::lock_guard<std::mutex> l(m);descriptors.erase(handle);}
 void descriptorPoolReset(uint64_t pool){std::lock_guard<std::mutex> l(m);for(auto d=descriptors.begin();d!=descriptors.end();)if(d->second.pool==pool)d=descriptors.erase(d);else ++d;}
 void descriptorWrite(uint64_t handle,unsigned binding,unsigned element,ResourceBinding resource){std::lock_guard<std::mutex> l(m);
  auto d=descriptors.find(handle);if(d==descriptors.end())return;const auto key=std::make_pair(binding,element);
  if(!d->second.layout)return;
  const auto& counts=d->second.layout->counts;auto n=counts.find(binding);
  if(n==counts.end()||element>=n->second){d->second.slots.clear();return;}
  if(resource.kind==ResourceKind::Texture){auto v=views.find(resource.handle);if(v!=views.end())resource.generation=v->second.id;}
  else{auto b=buffers.find(resource.handle);if(b!=buffers.end())resource.generation=b->second.generation;}
  if(resource.generation&&(d->second.slots.size()<256||d->second.slots.count(key)))d->second.slots[key]=resource;else d->second.slots.erase(key);}
 void descriptorUnknown(uint64_t handle){std::lock_guard<std::mutex> l(m);auto d=descriptors.find(handle);if(d!=descriptors.end())d->second.slots.clear();}
 void bindDescriptors(uintptr_t command,bool compute,uint64_t layout,unsigned first,const uint64_t* sets,unsigned count,const uint32_t* offsets,unsigned offsetCount){
  std::lock_guard<std::mutex> l(m);auto& c=commands[command];auto& b=compute?c.compute:c.graphics;
  if(b.layout!=layout){b.sets={};b.layout=layout;}
  unsigned consumed=0;
  for(unsigned i=0;i<count&&first+i<b.sets.size();i++){
   auto& to=b.sets[first+i];to={};auto set=descriptors.find(sets[i]);if(set==descriptors.end()){for(unsigned j=first+i;j<b.sets.size();j++)b.sets[j]={};return;}
   if(!set->second.layout){for(unsigned j=first+i;j<b.sets.size();j++)b.sets[j]={};return;}
   const auto& dynamic=set->second.layout->dynamic;
   if(dynamic.size()>offsetCount-consumed||(!offsets&&!dynamic.empty())){for(unsigned j=first+i;j<b.sets.size();j++)b.sets[j]={};return;}
   to.handle=sets[i];to.generation=set->second.generation;
   for(const auto& key:dynamic)to.offsets.emplace(key,offsets[consumed++]);
  }
 }
 void observeResources(uintptr_t command,bool compute){std::lock_guard<std::mutex> l(m);sampleResources(command,compute);}
 std::vector<ResourceSample> takeResourceSamples(){std::lock_guard<std::mutex> l(m);std::vector<ResourceSample> result;result.swap(resourceSamples);return result;}
 bool skinSource(uint64_t buffer,uint64_t n,uint32_t expected){
  std::lock_guard<std::mutex> l(m);auto it=buffers.find(buffer);if(it==buffers.end()||it->second.size!=n||!it->second.bound||!(it->second.usage&0x20))return false;
  auto& b=it->second;if(!b.skinCrc){if(const auto* p=host(b,0,n)){auto crc=crc32c(0,p,size_t(n));if(crc==expected)b.skinCrc=crc;}}
  return b.skinCrc==expected;
 }
 int drawIndexed(uintptr_t command,uint32_t count,uint32_t instances,uint32_t first,int32_t baseVertex,uint32_t firstInstance,SkinHit* skin=nullptr,std::vector<IndexIdentity>* identities=nullptr){
  if(!count||!instances)return -1;
  std::lock_guard<std::mutex> l(m);
  if(!st.indexedDraws)indexedDrawSeen.store(true,std::memory_order_relaxed);
  st.indexedDraws++;
  if(!routesReady.load(std::memory_order_acquire))return -1;
  sampleResources(command,false,first,count);
  auto binding=drawBindings.find(command);if(binding==drawBindings.end())return -1;
  const auto& state=binding->second;
  auto buffer=buffers.find(state.buffer);if(buffer==buffers.end()||buffer->second.generation!=state.generation)return -1;
  uint64_t begin=uint64_t(first)*state.width,bytes=uint64_t(count)*state.width;
  if(begin+bytes>buffer->second.size-state.offset)return -1;
  begin+=state.offset;
  int part=-1;
  bool conflict=false,compatConflict=false,iniChecked=false;
  SkinHit compatibility;
  auto accept=[&](SkinHit& hit,unsigned profile,unsigned slot,uint32_t relative,bool& ambiguous,std::optional<uint32_t> hash={}){
   if(hit&&(hit.profile!=profile||hit.slot!=slot)){
    const bool previous=skinActive(hit.profile),next=skinActive(profile);
    if(previous&&!next)return;
    if(hotReload&&previous&&next&&skinMods[hit.profile]!=skinMods[profile]){
     if(hit.profile<profile)return;
     ambiguous=false;
     hit={profile,slot,relative,count,hash};return;
    }
    ambiguous=previous==next;
   }
   hit={profile,slot,relative,count,hash};
  };
  if(skin&&allSkins&&!baseVertex&&instances==1&&count%3==0)for(const auto& rule:resourceRules){
   const auto& d=rule.draw;
   if(!skinActive(d.profile)||(d.first>=0&&first!=d.first)||(d.count>=0&&count!=d.count))continue;
   if(std::all_of(rule.all.begin(),rule.all.end(),[&](const auto& term){return resourceHash(command,false,term.slot)==term.hash;}))
    accept(*skin,d.profile,d.slot,first,conflict);
  }
  for(const auto& upload:buffer->second.matches){
   if(begin<upload.offset||begin+bytes>upload.offset+upload.bytes||(begin-upload.offset)%state.width)continue;
   for(size_t i=0;i<upload.count;i++){
    if(!(upload.hashes[i].widths&(state.width==2?1:2)))continue;
    const auto hash=upload.hashes[i].value;auto probe=probes.find(hash);if(probe==probes.end())continue;
    if(identities){identities->push_back({hash,uint32_t((begin-upload.offset)/state.width),uint32_t(upload.bytes/state.width)});}
    auto& draws=probe->second.draws;
    if(skin&&allSkins){
     const auto relative=uint32_t((begin-upload.offset)/state.width);
     const auto rules=iniSkinRules.find(hash);
     if(rules!=iniSkinRules.end()){
      iniChecked=true;
      for(const auto& rule:rules->second){
       if(baseVertex||instances!=1||count%3||upload.bytes%state.width)continue;
       if(rule.first>=0?relative!=rule.first:(relative||uint64_t(count)*state.width!=upload.bytes))continue;
       if(rule.count>=0&&count!=rule.count)continue;
       accept(*skin,rule.profile,rule.slot,relative,conflict,hash);
      }
     }
     auto route=skinRoutes.find(hash);
     if(!baseVertex&&instances==1&&route!=skinRoutes.end()&&upload.bytes==uint64_t(route->second.total)*state.width){
      const auto& r=route->second;const auto sub=std::find(r.ranges.begin(),r.ranges.end(),std::make_pair(relative,count));
      if(sub!=r.ranges.end())accept(compatibility,r.profile,unsigned(sub-r.ranges.begin()),relative,compatConflict);
     }
    }
    if(tintOn.load(std::memory_order_relaxed)){
     const int hit=paimonTintPart(hash,upload.bytes,uint32_t((begin-upload.offset)/state.width),count,state.width,instances,baseVertex,firstInstance);
     if(hit>=0)part=hit;
    }
    if(draws.size()==maxDrawsPerHash||std::any_of(draws.begin(),draws.end(),[&](const Draw& d){return d.upload==upload.seq&&d.bindOffset==state.offset&&d.indexBits==8*state.width&&d.first==first&&d.count==count&&d.instances==instances&&d.baseVertex==baseVertex&&d.firstInstance==firstInstance;}))continue;
    draws.push_back({upload.seq,state.buffer,upload.offset,state.offset,first,uint32_t((begin-upload.offset)/state.width),count,instances,firstInstance,baseVertex,8*state.width,hash});
   }
   break;
  }
  if(skin){
   if(conflict){*skin={};noteSkinMiss(SkinMiss::Ambiguous);}
   else if(!*skin){
    if(iniChecked)noteSkinMiss(SkinMiss::DrawRange);
    else if(compatConflict)noteSkinMiss(SkinMiss::Ambiguous);
    else *skin=compatibility;
   }
  }
  return part;
 }
 std::vector<Draw> takeDraws(){std::lock_guard<std::mutex> l(m);std::vector<Draw> draws;for(auto& kv:probes){auto& p=kv.second;draws.insert(draws.end(),p.draws.begin()+p.reported,p.draws.end());p.reported=p.draws.size();}return draws;}
 std::vector<DrawStats> drawStats(){std::lock_guard<std::mutex> l(m);std::vector<DrawStats> out;for(const auto& kv:probes)out.push_back({kv.first,kv.second.uploads,kv.second.draws.size()});return out;}
 void resolvedName(bool wrapped){std::lock_guard<std::mutex> l(m);st.resolved++;if(wrapped)st.wrapped++;}
 void installed(bool ok,const std::string& error){std::lock_guard<std::mutex> l(m);st.installed=ok;st.error=error;enabled=ok;}
 std::vector<Blob> take(){std::lock_guard<std::mutex> l(m);std::vector<Blob> r;r.swap(out);return r;}
 Stats stats(){std::lock_guard<std::mutex> l(m);return st;}
};
inline Tracker& tracker(){static Tracker t;return t;}
}
// vk_hook.cpp (Android only): hooks the Vulkan loader's lookups; detail says what was hooked or why not.
namespace ms::vkhook {
bool install(std::string& detail);
void preparePaimonTextures(const std::string& modelDir,bool skin=false);
void configureVulkanProbes(std::vector<uint32_t> probes);
void stageVulkanRegistry(const std::string& shared,const std::vector<std::string>& selected,const std::vector<std::string>& compatibility);
std::vector<std::string> publishVulkanRegistry(bool enabled,bool unity,const std::vector<std::string>& selected,
                                             const std::vector<std::string>& compatibility,const std::string& fit,
                                             const std::function<bool(const std::vector<std::string>&)>& restoreUnity);
}
