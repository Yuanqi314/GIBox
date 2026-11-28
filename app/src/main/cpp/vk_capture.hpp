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
#include <array>
#include <atomic>
#include <charconv>
#include <cstdint>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
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
 explicit operator bool()const{return profile!=~0u;}
};
struct SkinRoute {uint32_t hash=0,total=0;unsigned profile=0;std::vector<std::pair<uint32_t,uint32_t>> ranges;bool pcRule=false;};
struct IniSkinRule {unsigned profile=0,slot=0;int64_t first=-1,count=-1;};
enum class SkinMiss : unsigned {DrawRange,Ambiguous,Graphics,SkinOutput,Bones,Resources,Pose,Total};
using SkinChoices=std::map<std::string,std::map<std::string,int>>;
inline constexpr uint32_t paimonBodyHash=0x5b4cdc9cu;
// A narrow test route. Do not allow a partial match (instances, index format, range or base vertex).
inline int paimonTintPart(uint32_t hash,uint64_t uploadBytes,uint32_t first,uint32_t count,uint32_t width,uint32_t instances,int32_t baseVertex,uint32_t firstInstance){
 if(hash!=paimonBodyHash||uploadBytes!=61224||width!=2||instances!=1||baseVertex||firstInstance)return -1;
 return first==0&&count==26226?0:first==26226&&count==4386?1:-1;
}
// SPIR-V OpName and OpDecorate suffice for this deliberately narrow prototype. A shader stripped of names,
// using decoration groups or containing an unexpected binding does not prove this target.
inline std::optional<uint32_t> namedTextureBinding(const uint32_t* code,size_t words,const char* name){
 if(!code||words<5||words>(1u<<18)||code[0]!=0x07230203u)return std::nullopt;
 const size_t length=std::strlen(name)+1;
 uint32_t id=0;
 for(size_t i=5;i<words;){size_t n=code[i]>>16;if(!n||n>words-i)return std::nullopt;
  if((code[i]&0xffff)==5&&n>=2&&(n-2)*4>=length&&!std::memcmp(code+i+2,name,length)){
   if(!code[i+1]||(id&&id!=code[i+1]))return std::nullopt;
   id=code[i+1];}
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
inline bool mainTextureBinding(const uint32_t* code,size_t words){return namedTextureBinding(code,words,"_MainTex")==0u;}

// The buffers, memory bindings and host mappings needed to find index data in CPU memory: a staging buffer copied
// into an index buffer (read when the copy is recorded), or an index buffer in mapped memory (read at its first bind).
class Tracker {
 struct UploadHash {uint32_t value=0;uint8_t widths=0;}; // bit 0: raw 16-bit input, bit 1: raw 32-bit input
 struct Upload {uint64_t seq,offset,bytes;std::array<UploadHash,12> hashes{};size_t count=0;};
 struct Probe {uint64_t uploads=0;size_t reported=0;std::vector<Draw> draws;};
 struct Buffer {uint64_t size=0,memory=0,memoryOffset=0,generation=0;uint32_t usage=0,skinCrc=0;bool bound=false,captured=false;std::vector<Upload> matches;};
 struct Mapping {const uint8_t* base=nullptr;uint64_t offset=0,size=0;};
 struct IndexBinding {uint64_t buffer,generation,offset;uint32_t width;};
 std::mutex m;std::unordered_map<uint64_t,Buffer> buffers;std::unordered_map<uint64_t,Mapping> maps;std::vector<Blob> out;Stats st;uint64_t seq=0;
 std::unordered_map<uintptr_t,IndexBinding> drawBindings;
 std::map<uint32_t,Probe> probes; // keys set once before the loader hooks are installed
 std::unordered_set<uint64_t> pcIndexBytes16,pcIndexBytes32;
 uint64_t bufferGeneration=0;
 static constexpr uint32_t indexUsage=0x40,transferSrc=0x1;static constexpr size_t maxBlob=64u<<20,maxQueue=65536;
 static bool skinSize(uint64_t n){return n==335880||n==268704;}
 // Host address of [memory offset, +size) or null when that range is not mapped.
 const uint8_t* host(const Buffer& b,uint64_t at,uint64_t size){auto x=maps.find(b.memory);if(x==maps.end()||!x->second.base)return nullptr;uint64_t from=b.memoryOffset+at;
  if(from<x->second.offset||(x->second.size!=~0ull&&from+size>x->second.offset+x->second.size)){return nullptr;}return x->second.base+(from-x->second.offset);}
 // Under the lock: an unmap waits until the bytes are read.
 void add(const uint8_t* p,uint64_t n,uint64_t buffer,Buffer& dst,uint64_t offset,const char* via){st.uploads++;st.bytes+=n;
  if(!probing()&&out.size()>=maxQueue){st.dropped++;return;}
  Blob b;b.seq=++seq;b.buffer=buffer;b.offset=offset;b.bytes=uint32_t(n);b.via=via;if(allForms)b.hashes=ibHashes(p,size_t(n));else hashFormat(p,size_t(n),0,b.hashes);
  if(probing()){
   Upload match{b.seq,offset,n};
   auto keep=[&](const Hash& h){auto probe=probes.find(h.value);if(probe==probes.end())return;
    const auto format=h.variant>>2;auto route=skinRoutes.find(h.value);
    if(format&&!iniSkinRules.count(h.value)&&(route==skinRoutes.end()||!route->second.pcRule||n!=uint64_t(route->second.total)*(format==1?2u:4u)))return;
    const uint8_t widths=format?uint8_t(format==1?1:2):uint8_t(3);
    for(size_t i=0;i<match.count;i++)if(match.hashes[i].value==h.value){match.hashes[i].widths|=widths;return;}
    ++probe->second.uploads;match.hashes[match.count++]={h.value,widths};};
   for(const auto& h:b.hashes)keep(h);
   // GIMI can hash 32-bit PC indices when the same mesh uses 16-bit indices on Android (or vice versa).
   // Try only widths with a verified PC route for the uploaded byte size.
   if(!allForms){
    std::vector<Hash> alternate;
    if(pcIndexBytes16.count(n))indexWidthHashes<uint16_t,uint32_t>(p,size_t(n),1,alternate);
    if(pcIndexBytes32.count(n))indexWidthHashes<uint32_t,uint16_t>(p,size_t(n),2,alternate);
    for(const auto& h:alternate)keep(h);
   }
   if(match.count){++st.drawProbeUploads;if(dst.matches.size()<16)dst.matches.push_back(match);}
  }
  if(probeOnly.load(std::memory_order_relaxed))return;
  if(out.size()>=maxQueue){st.dropped++;return;}
  for(size_t i=0;i<n/2;i++){uint16_t v;std::memcpy(&v,p+2*i,2);if(i<8)b.head16.push_back(v);if(v>b.max16)b.max16=v;}
  if(n%4==0)for(size_t i=0;i<n/4;i++){uint32_t v;std::memcpy(&v,p+4*i,4);if(v>b.max32)b.max32=v;}
  out.push_back(std::move(b));}
public:
 static constexpr size_t maxDrawHashes=64,maxDrawsPerHash=64;
 std::atomic<bool> enabled{false};
 std::atomic<bool> allForms{true};   // false (match / collect): the data as uploaded only, one pass over it
 std::atomic<bool> probeOnly{false};
 bool tintHooks=false; // startup only; hooks are never installed by a live setting change
 std::string paimonTextureMod; // optional startup preset, chosen before installing any wrappers
 bool skinnedModel=false; // this preset uses the captured Vulkan skinning pipeline instead of Unity mesh APIs
 bool allSkins=false;
 std::vector<std::string> skinMods; // profile index -> mod; published before install
 std::unordered_map<uint32_t,SkinRoute> skinRoutes;
 std::unordered_map<uint32_t,std::vector<IniSkinRule>> iniSkinRules;
 std::shared_ptr<const SkinChoices> skinChoices;
 std::atomic<uint64_t> activeSkins{0},skinMatches{0},sharedDraws{0},sharedMisses{0},originalSkinJobs{0};
 std::array<std::atomic<uint64_t>,64> profileDraws{};
 std::array<std::atomic<uint64_t>,unsigned(SkinMiss::Total)> skinMisses{};
 std::array<std::atomic<uint32_t>,64> skinBoneErrors{}; // required in high bits, live palette in low bits
 std::atomic<bool> tintOn{false};
 std::atomic<int> textureUpload{0}; // 0 waiting, 1 queued on the only graphics queue, -1 failed
 std::array<std::atomic<uint64_t>,2> tintDraws{},tintMisses{};
 std::atomic<uint64_t> skinDispatches{0};
 // Startup only. The wrappers and runtime may read the keys once installation succeeds.
 void setDrawHashes(const std::vector<uint32_t>& hashes){probes.clear();pcIndexBytes16.clear();pcIndexBytes32.clear();for(uint32_t hash:hashes){if(probes.size()==maxDrawHashes)break;probes.try_emplace(hash);
  auto route=skinRoutes.find(hash);if(route!=skinRoutes.end()&&route->second.pcRule){pcIndexBytes16.insert(2ull*route->second.total);pcIndexBytes32.insert(4ull*route->second.total);}}}
 bool probing()const{return !probes.empty();}
 void setPaimonReplacement(bool master,bool tint,const std::string& textureMod){
  if(tintHooks)tintOn.store(master&&(paimonTextureMod.empty()?tint:paimonTextureMod==textureMod),std::memory_order_relaxed);
 }
 void setSkinReplacement(bool master,bool request,const std::vector<std::string>& selected){
  uint64_t bits=0;if(master&&request)for(size_t i=0;i<skinMods.size()&&i<64;i++)if(std::find(selected.begin(),selected.end(),skinMods[i])!=selected.end())bits|=uint64_t(1)<<i;
  activeSkins.store(bits,std::memory_order_relaxed);
 }
 bool skinActive(unsigned profile)const{return profile<64&&(activeSkins.load(std::memory_order_relaxed)&(uint64_t(1)<<profile));}
 void noteSkinMiss(SkinMiss why){skinMisses[unsigned(why)].fetch_add(1,std::memory_order_relaxed);}
 void setSkinChoices(const SkinChoices& choices){std::atomic_store(&skinChoices,std::make_shared<const SkinChoices>(choices));}
 bool ownsSkin(const std::string& name)const{return enabled&&allSkins&&textureUpload.load(std::memory_order_relaxed)>=0&&
  std::find(skinMods.begin(),skinMods.end(),name)!=skinMods.end();}
 void created(uint64_t buffer,uint64_t size,uint32_t usage){if(!(usage&(indexUsage|transferSrc))&&!(skinnedModel&&(usage&0x20)&&skinSize(size)))return;std::lock_guard<std::mutex> l(m);Buffer b;b.size=size;b.usage=usage;if(probing())b.generation=++bufferGeneration;buffers[buffer]=std::move(b);}
 void destroyed(uint64_t buffer){std::lock_guard<std::mutex> l(m);buffers.erase(buffer);}
 void bound(uint64_t buffer,uint64_t memory,uint64_t offset){std::lock_guard<std::mutex> l(m);auto b=buffers.find(buffer);if(b==buffers.end())return;b->second.memory=memory;b->second.memoryOffset=offset;b->second.bound=true;b->second.skinCrc=0;}
 void mapped(uint64_t memory,uint64_t offset,uint64_t size,const void* p){std::lock_guard<std::mutex> l(m);maps[memory]={static_cast<const uint8_t*>(p),offset,size};}
 void unmapped(uint64_t memory){std::lock_guard<std::mutex> l(m);maps.erase(memory);}
 // A copy into an index buffer: each region's source bytes, as written into the staging memory before the copy.
 void copied(uint64_t src,uint64_t dst,const Copy* regions,uint32_t count){
  std::lock_guard<std::mutex> l(m);auto d=buffers.find(dst);if(d==buffers.end())return;auto s=buffers.find(src);
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
  if(command&&!dst.matches.empty()&&(indexType==0||indexType==1)&&drawBindings.size()<512)
   drawBindings.emplace(command,IndexBinding{buffer,dst.generation,offset,indexType==0?2u:4u});
 }
 void beginCommands(uintptr_t command){std::lock_guard<std::mutex> l(m);drawBindings.erase(command);}
 bool skinSource(uint64_t buffer,uint64_t n,uint32_t expected){
  std::lock_guard<std::mutex> l(m);auto it=buffers.find(buffer);if(it==buffers.end()||it->second.size!=n||!it->second.bound||!(it->second.usage&0x20))return false;
  auto& b=it->second;if(!b.skinCrc){if(const auto* p=host(b,0,n)){auto crc=crc32c(0,p,size_t(n));if(crc==expected)b.skinCrc=crc;}}
  return b.skinCrc==expected;
 }
 int drawIndexed(uintptr_t command,uint32_t count,uint32_t instances,uint32_t first,int32_t baseVertex,uint32_t firstInstance,SkinHit* skin=nullptr){
  if(!count||!instances)return -1;
  std::lock_guard<std::mutex> l(m);
  st.indexedDraws++;
  auto binding=drawBindings.find(command);if(binding==drawBindings.end())return -1;
  const auto& state=binding->second;
  auto buffer=buffers.find(state.buffer);if(buffer==buffers.end()||buffer->second.generation!=state.generation)return -1;
  uint64_t begin=uint64_t(first)*state.width,bytes=uint64_t(count)*state.width;
  if(begin+bytes>buffer->second.size-state.offset)return -1;
  begin+=state.offset;
  int part=-1;
  bool conflict=false,compatConflict=false,iniChecked=false;
  SkinHit compatibility;
  for(const auto& upload:buffer->second.matches){
   if(begin<upload.offset||begin+bytes>upload.offset+upload.bytes||(begin-upload.offset)%state.width)continue;
   for(size_t i=0;i<upload.count;i++){
    if(!(upload.hashes[i].widths&(state.width==2?1:2)))continue;
    const auto hash=upload.hashes[i].value;auto& draws=probes.at(hash).draws;
    if(skin&&allSkins){
     const auto relative=uint32_t((begin-upload.offset)/state.width);
     auto accept=[&](SkinHit& hit,unsigned profile,unsigned slot,bool& ambiguous){
      if(hit&&(hit.profile!=profile||hit.slot!=slot)){
       const bool previous=skinActive(hit.profile),next=skinActive(profile);
       if(previous&&!next)return;
       ambiguous=previous==next;
      }
      hit={profile,slot,relative,count};
     };
     const auto rules=iniSkinRules.find(hash);
     if(rules!=iniSkinRules.end()){
      iniChecked=true;
      for(const auto& rule:rules->second){
       if(baseVertex||instances!=1||count%3||upload.bytes%state.width)continue;
       if(rule.first>=0?relative!=rule.first:(relative||uint64_t(count)*state.width!=upload.bytes))continue;
       if(rule.count>=0&&count!=rule.count)continue;
       accept(*skin,rule.profile,rule.slot,conflict);
      }
     }
     auto route=skinRoutes.find(hash);
     if(!baseVertex&&instances==1&&route!=skinRoutes.end()&&upload.bytes==uint64_t(route->second.total)*state.width){
      const auto& r=route->second;const auto sub=std::find(r.ranges.begin(),r.ranges.end(),std::make_pair(relative,count));
      if(sub!=r.ranges.end())accept(compatibility,r.profile,unsigned(sub-r.ranges.begin()),compatConflict);
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
std::vector<std::string> prepareVulkanSkins(const std::string& shared,const std::vector<std::string>& selected,std::vector<uint32_t>& probes,
                                          const std::vector<std::string>& compatibility={});
}
