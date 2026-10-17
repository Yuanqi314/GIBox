#pragma once
// GIBox model format, version 1 — GIBox's own model folder, written by the converter (ModelWriter.java) and read here.
//
//   models/<name>/model.json    manifest: character, parts, materials, variants, and where each array sits in model.bin
//   models/<name>/model.bin     every binary array, each starting on a 16-byte boundary; CRC-32 of the whole file in the manifest
//   models/<name>/learned.json  written by the runtime: meshes identified in the game, the character ID found for a named model
//
// Every count, range and value is checked before anything reaches the engine (limits below). The skeleton library
// (skeletons/<id>/skeleton.json) is written here (skeletonJson) and read by the converter.
#include "core.hpp"
#include "json.hpp"
#include <array>
#include <cstdio>
#include <memory>
namespace ms {
namespace model {
inline constexpr unsigned maxParts=64,maxBones=512,maxVertices=500000,maxIndices=15000000,maxDraws=64,maxSlot=64,maxMaterials=256,maxTexture=4096,maxCombos=32,maxVars=16;
inline constexpr uint64_t maxBlob=768ull*1024*1024;
}

inline uint32_t crc32(const uint8_t* p,size_t n,uint32_t crc=0){
 static const auto table=[]{std::array<uint32_t,256> t{};for(uint32_t i=0;i<256;i++){uint32_t c=i;for(int k=0;k<8;k++)c=(c&1)?0xedb88320u^(c>>1):c>>1;t[i]=c;}return t;}();
 crc=~crc;for(size_t i=0;i<n;i++)crc=table[(crc^p[i])&0xff]^(crc>>8);return ~crc;}

// ---------------------------------------------------------------- JSON field access with limits
// Keys are const char*: a std::string temporary in the call would make every returned reference look dangling to GCC.
inline const Json& jfield(const Json& o,const char* k){require(o.type==Json::Object,std::string("Expected object around \"")+k+"\"");const auto& v=o.at(k);require(v.type!=Json::Null,std::string("Missing \"")+k+"\"");return v;}
inline std::string jstr(const Json& o,const char* k,size_t maxLen){const auto& v=jfield(o,k);require(v.type==Json::String&&!v.string.empty()&&v.string.size()<=maxLen,std::string("Invalid \"")+k+"\"");return v.string;}
inline uint64_t jnum(const Json& v,uint64_t lo,uint64_t hi,const std::string& what){require(v.type==Json::Number&&v.number>=0&&std::floor(v.number)==v.number&&v.number<=9007199254740991.,"Invalid "+what);auto n=uint64_t(v.number);require(n>=lo&&n<=hi,what+" out of range");return n;}
inline uint64_t juint(const Json& o,const char* k,uint64_t lo,uint64_t hi){return jnum(jfield(o,k),lo,hi,std::string("\"")+k+"\"");}
inline int jint(const Json& v,const std::string& what){require(v.type==Json::Number&&std::floor(v.number)==v.number&&std::abs(v.number)<=1000000,"Invalid "+what);return int(v.number);}
inline bool jbool(const Json& o,const char* k,bool fallback){const auto& v=o.at(k);if(v.type==Json::Null)return fallback;require(v.type==Json::Bool,std::string("Invalid \"")+k+"\"");return v.boolean;}
inline const std::vector<Json>& jarr(const Json& o,const char* k,size_t lo,size_t hi){const auto& v=jfield(o,k);require(v.type==Json::Array&&v.array.size()>=lo&&v.array.size()<=hi,std::string("Invalid \"")+k+"\" list");return v.array;}
inline bool safeName(const std::string& s){return !s.empty()&&s.size()<=64&&s.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_-")==s.npos;}
inline bool nameToken(const std::string& s){return s.size()>=3&&s.size()<=40&&s.find_first_not_of("abcdefghijklmnopqrstuvwxyz0123456789")==s.npos;}
inline std::string jnumber(double v){char b[32];std::snprintf(b,sizeof(b),"%.9g",v);return b;}

// ---------------------------------------------------------------- mesh identity (targets)
inline Fingerprint fingerprintOf(const Json& j){Fingerprint f;require(j.type==Json::Object,"Target object");f.renderer=jstr(j,"renderer",256);
 const auto& m=j.at("mesh");if(m.type!=Json::Null){require(m.type==Json::String&&m.string.size()<=256,"Target mesh");f.mesh=m.string;}
 f.vertices=unsigned(juint(j,"vertices",1,model::maxVertices));f.submeshes=unsigned(juint(j,"submeshes",1,model::maxDraws));f.bones=unsigned(juint(j,"bones",1,model::maxBones));return f;}
inline std::string fingerprintJson(const Fingerprint& f){return "{\"renderer\":"+quote(f.renderer)+",\"mesh\":"+quote(f.mesh)+",\"vertices\":"+std::to_string(f.vertices)+",\"submeshes\":"+std::to_string(f.submeshes)+",\"bones\":"+std::to_string(f.bones)+"}";}

// ---------------------------------------------------------------- variants
inline Variants variantsOf(const Json& o){Variants v;
 if(o.type==Json::Null){v.combos.push_back({});return v;}
 require(o.type==Json::Object,"Variants object");
 for(auto& x:jarr(o,"vars",0,model::maxVars)){auto n=jstr(x,"name",64);require(n.size()>1&&n[0]=='$',"Variant name: "+n);for(auto& y:v.vars)require(y.first!=n,"Duplicate variant "+n);
  std::vector<int> values;for(auto& k:jarr(x,"values",1,64))values.push_back(jint(k,"variant value"));v.vars.push_back({n,values});}
 for(auto& c:jarr(o,"combos",1,model::maxCombos)){require(c.type==Json::Object,"Variant combination");std::map<std::string,int> m;
  for(auto& kv:c.object){bool known=false;for(auto& y:v.vars)known|=y.first==kv.first;require(known,"Variant undeclared: "+kv.first);m[kv.first]=jint(kv.second,"variant value");}
  require(m.size()==v.vars.size(),"Variant combination must set every variable");for(auto& y:v.combos)require(y!=m,"Duplicate variant combination");v.combos.push_back(m);}
 v.def=unsigned(juint(o,"default",0,v.combos.size()-1));return v;}

// ---------------------------------------------------------------- manifest summary (what the runtime scans every second)
struct PartSummary {std::string name,renderer;unsigned bones=0;bool hasTarget=false;Fingerprint target;};
struct ModelSummary {std::string name,generation;unsigned id=0;std::vector<std::string> names;std::vector<PartSummary> parts;};
inline void checkFormat(const Json& m){require(m.type==Json::Object,"model.json: not an object");require(m.at("format").type==Json::String&&m.at("format").string=="gibox-model","model.json: not a GIBox model");
 auto version=juint(m,"version",1,1000000);require(version==1,"model.json: unsupported version "+std::to_string(version)+" (made by a newer GIBox?)");}
inline void characterOf(const Json& m,unsigned& id,std::vector<std::string>& names){const auto& c=jfield(m,"character");id=unsigned(juint(c,"id",0,10001999));require(id==0||id>=10000000,"Character ID out of range");
 const auto& n=c.at("names");if(n.type!=Json::Null){require(n.type==Json::Array&&n.array.size()<=16,"Character names");for(auto& x:n.array){require(x.type==Json::String&&nameToken(x.string),"Character name");names.push_back(x.string);}}
 require(id!=0||!names.empty(),"A model needs a character ID or names");}
inline ModelSummary summaryOf(const Json& m){checkFormat(m);ModelSummary s;s.name=jstr(m,"name",512);s.generation=jstr(m,"generation",64);characterOf(m,s.id,s.names);std::set<std::string> seen;
 for(auto& p:jarr(m,"parts",1,model::maxParts)){PartSummary ps;ps.name=jstr(p,"name",64);require(safeName(ps.name)&&seen.insert(ps.name).second,"Part name: "+ps.name);ps.renderer=jstr(p,"renderer",256);
  ps.bones=unsigned(juint(p,"bones",1,model::maxBones));if(p.at("target").type!=Json::Null){ps.target=fingerprintOf(p.at("target"));ps.hasTarget=true;}s.parts.push_back(ps);}
 return s;}
inline std::string manifestText(const std::string& dir){auto b=read(dir+"/model.json",4*1024*1024);return std::string(b.begin(),b.end());}
inline ModelSummary readSummary(const std::string& dir){return summaryOf(JsonParser(manifestText(dir)).parse());}

// ---------------------------------------------------------------- full model
struct Geometry {unsigned n=0,ni=0;Bytes positions,normals,uvs,indices;
 const uint8_t* pos()const{return positions.data();}const uint8_t* norm()const{return normals.data();}const uint8_t* uv()const{return uvs.data();}const uint8_t* idx()const{return indices.data();}};
struct PartFile {Part spec;Geometry geo;Bytes weights;std::set<unsigned> used;std::vector<std::vector<Submesh>> subs;std::vector<std::vector<Material>> rows;std::vector<std::pair<std::string,Bytes>> channels;std::vector<std::string> sourceBones;Bytes sourceBinds;};
struct MaterialFile {bool original=true;std::shared_ptr<Texture> main,light,normal;};
struct ModelFile {std::string name,path,generation;unsigned id=0;std::vector<std::string> names;bool originalMaterials=false,diffuseOnly=false;Variants variants;std::vector<PartFile> parts;std::map<std::string,MaterialFile> materials;};

namespace detail {
// model.bin, checked whole (size, CRC-32 streamed in 1 MiB steps) before any view is read from it; views are then read
// one by one, so a model never sits in memory twice.
class Blob {std::ifstream f;uint64_t n=0;public:
 Blob(const std::string& path,uint64_t size,uint32_t crc):f(path,std::ios::binary|std::ios::ate){
  require(bool(f),"Cannot read: "+path);auto end=f.tellg();require(end>=0&&uint64_t(end)==size,"model.bin: size differs from model.json (copied incompletely?)");n=size;f.seekg(0);
  std::vector<uint8_t> buf(1<<20);uint32_t c=0;for(uint64_t left=n;left;){auto k=size_t(std::min<uint64_t>(left,buf.size()));f.read(reinterpret_cast<char*>(buf.data()),k);require(bool(f),"Short read: "+path);c=crc32(buf.data(),k,c);left-=k;}
  require(c==crc,"model.bin: checksum mismatch (damaged file)");}
 uint64_t size()const{return n;}
 Bytes read(uint64_t off,uint64_t len){Bytes b(size_t(len),0);if(len){f.seekg(std::streamoff(off));f.read(reinterpret_cast<char*>(b.data()),std::streamsize(len));require(bool(f),"Short read: model.bin");}return b;}
};
// [offset, length] into model.bin: 16-byte aligned, exact length, inside the file.
inline Bytes view(const Json& v,Blob& blob,uint64_t expect,const std::string& what){
 require(v.type==Json::Array&&v.array.size()==2,"View "+what);auto off=jnum(v.array[0],0,blob.size(),what+" offset"),len=jnum(v.array[1],0,blob.size(),what+" length");
 require(off%16==0,what+": misaligned");require(len==expect,what+": wrong length");require(off+len<=blob.size(),what+": outside model.bin");return blob.read(off,len);}
inline void finite(const Bytes& b,const std::string& what){for(size_t i=0;i+4<=b.size();i+=4)require(std::isfinite(f32(b.data()+i)),what+": not a finite number");}
inline std::shared_ptr<Texture> texture(const Json& t,Blob& blob,const std::string& what){
 auto w=unsigned(juint(t,"width",1,model::maxTexture)),h=unsigned(juint(t,"height",1,model::maxTexture));
 return std::make_shared<Texture>(Texture{w,h,view(jfield(t,"data"),blob,4ull*w*h,what)});}
}

/** Reads and checks a model folder. Nothing partially valid is returned: any problem throws with what and where. */
inline std::shared_ptr<ModelFile> loadModelFile(const std::string& dir){
 auto m=JsonParser(manifestText(dir)).parse();auto summary=summaryOf(m);
 auto out=std::make_shared<ModelFile>();out->name=summary.name;out->path=dir;out->generation=summary.generation;out->id=summary.id;out->names=summary.names;
 const auto& flags=m.at("flags");if(flags.type!=Json::Null){out->originalMaterials=jbool(flags,"originalMaterials",false);out->diffuseOnly=jbool(flags,"diffuseOnly",false);}
 out->variants=variantsOf(m.at("variants"));
 // The data file: exact size and CRC-32 before any view is taken from it.
 const auto& blobInfo=jfield(m,"blob");require(jstr(blobInfo,"file",64)=="model.bin","blob.file must be model.bin");
 auto size=juint(blobInfo,"size",1,model::maxBlob);auto crc=uint32_t(juint(blobInfo,"crc32",0,0xffffffffull));
 detail::Blob blob(dir+"/model.bin",size,crc);
 // Materials first: draws refer to them by name.
 for(auto& mt:jarr(m,"materials",1,model::maxMaterials)){auto name=jstr(mt,"name",64);require(safeName(name)&&!out->materials.count(name),"Material name: "+name);MaterialFile f;f.original=jbool(mt,"original",false);
  if(!f.original){f.main=detail::texture(jfield(mt,"main"),blob,name+".main");
   if(mt.at("lightmap").type!=Json::Null){f.light=detail::texture(mt.at("lightmap"),blob,name+".lightmap");}
   if(mt.at("normalmap").type!=Json::Null){f.normal=detail::texture(mt.at("normalmap"),blob,name+".normalmap");}
   if(out->diffuseOnly){f.light.reset();f.normal.reset();}}
  out->materials[name]=f;}
 const auto& parts=jarr(m,"parts",1,model::maxParts);
 for(size_t pi=0;pi<parts.size();pi++){const auto& p=parts[pi];const auto& ps=summary.parts[pi];PartFile f;
  unsigned n=unsigned(juint(p,"vertices",1,model::maxVertices)),ni=unsigned(juint(p,"indices",3,model::maxIndices));require(ni%3==0,ps.name+": index count not a multiple of 3");
  f.spec={ps.name,ps.renderer,0,ps.bones};f.geo.n=n;f.geo.ni=ni;const auto& d=jfield(p,"data");auto at=[&](const char* k,uint64_t len){return detail::view(jfield(d,k),blob,len,ps.name+"."+k);};
  f.geo.positions=at("positions",12ull*n);f.geo.normals=at("normals",12ull*n);f.geo.uvs=at("uv",8ull*n);f.geo.indices=at("indices",4ull*ni);f.weights=at("weights",32ull*n);
  detail::finite(f.geo.positions,ps.name+".positions");detail::finite(f.geo.normals,ps.name+".normals");detail::finite(f.geo.uvs,ps.name+".uv");
  for(unsigned i=0;i<ni;i++)require(u32(f.geo.idx()+4ull*i)<n,ps.name+".indices: vertex index out of range");
  // Skin: four weights (finite, non-negative, sum 1) then four bone indices (inside the part's skeleton) per vertex.
  for(unsigned i=0;i<n;i++){const uint8_t* w=f.weights.data()+32ull*i;float sum=0;
   for(int j=0;j<4;j++){float x=f32(w+4*j);unsigned b=u32(w+16+4*j);require(std::isfinite(x)&&x>=0,ps.name+".weights: invalid weight");require(b<ps.bones,ps.name+".weights: bone index out of range");sum+=x;if(x>0)f.used.insert(b);}
   require(sum>=.999f&&sum<=1.001f,ps.name+".weights: weights do not sum to 1");}
  for(auto ch:{std::pair<const char*,unsigned>{"uv2",8},{"uv3",8},{"uv4",8},{"colors",4},{"tangents",16}}){
   if(d.at(ch.first).type==Json::Null){continue;}auto b=at(ch.first,uint64_t(ch.second)*n);if(ch.second!=4){detail::finite(b,ps.name+"."+ch.first);}f.channels.push_back({std::string(".")+ch.first,std::move(b)});}
  const auto& sk=p.at("skeleton");
  if(sk.type!=Json::Null){for(auto& b:jarr(sk,"bones",ps.bones,ps.bones)){require(b.type==Json::String&&!b.string.empty()&&b.string.size()<=256&&b.string.find_first_of("\t\r\n")==b.string.npos,ps.name+": bone name");f.sourceBones.push_back(b.string);}
   f.sourceBinds=detail::view(jfield(sk,"bindposes"),blob,64ull*ps.bones,ps.name+".bindposes");detail::finite(f.sourceBinds,ps.name+".bindposes");}
  // Draws per variant combination: index ranges and the material each one uses.
  const auto& draws=jarr(p,"draws",out->variants.combos.size(),out->variants.combos.size());
  for(auto& combo:draws){require(combo.type==Json::Array&&!combo.array.empty()&&combo.array.size()<=model::maxDraws,ps.name+": draws");std::vector<Submesh> subs;std::vector<Material> rows;
   for(auto& dr:combo.array){auto start=unsigned(juint(dr,"start",0,ni));auto count=unsigned(juint(dr,"count",0,ni));require(count<=ni-start&&count%3==0,ps.name+": draw range");
    auto mat=jstr(dr,"material",64);require(out->materials.count(mat),ps.name+": unknown material "+mat);subs.push_back({start,count});rows.push_back({mat,unsigned(juint(dr,"slot",0,model::maxSlot-1))});}
   f.subs.push_back(subs);f.rows.push_back(rows);}
  out->parts.push_back(std::move(f));}
 return out;}

// ---------------------------------------------------------------- learned.json (runtime-written)
struct Learned {unsigned id=0;std::map<std::string,Fingerprint> targets;};
inline Learned learnedOf(const Json& j){Learned l;require(j.type==Json::Object&&j.at("format").type==Json::String&&j.at("format").string=="gibox-learned","learned.json: format");
 if(j.at("id").type!=Json::Null){l.id=unsigned(juint(j,"id",0,10001999));require(l.id==0||l.id>=10000000,"learned.json: id");}
 const auto& t=j.at("targets");if(t.type!=Json::Null){require(t.type==Json::Object&&t.object.size()<=model::maxParts,"learned.json: targets");for(auto& kv:t.object){require(safeName(kv.first),"learned.json: part");l.targets[kv.first]=fingerprintOf(kv.second);}}
 return l;}
inline Learned readLearned(const std::string& dir){auto f=dir+"/learned.json";if(!exists(f))return {};try{return learnedOf(JsonParser(text(f)).parse());}catch(const std::exception&){return {};}}
inline std::string learnedJson(const Learned& l){std::string s="{\"format\":\"gibox-learned\",\"version\":1,\"id\":"+std::to_string(l.id)+",\"targets\":{";bool first=true;
 for(auto& kv:l.targets){s+=(first?"":",")+quote(kv.first)+":"+fingerprintJson(kv.second);first=false;}return s+"}}\n";}

// ---------------------------------------------------------------- skeleton library (runtime-written, read by the converter)
struct SkeletonRenderer {std::string name;Fingerprint target;std::vector<std::string> bones;std::vector<int> parents;Bytes bindposes;};
inline std::string skeletonJson(unsigned id,const std::string& root,const std::vector<SkeletonRenderer>& renderers){
 std::string s="{\"format\":\"gibox-skeleton\",\"version\":1,\"id\":"+std::to_string(id)+",\"root\":"+quote(root)+",\"renderers\":[";
 for(size_t r=0;r<renderers.size();r++){auto& x=renderers[r];require(x.bindposes.size()==64*x.bones.size()&&x.parents.size()==x.bones.size(),"Skeleton record");
  s+=std::string(r?",":"")+"{\"name\":"+quote(x.name)+",\"target\":"+fingerprintJson(x.target)+",\"bones\":[";
  for(size_t i=0;i<x.bones.size();i++){s+=std::string(i?",":"")+quote(x.bones[i]);}s+="],\"parents\":[";
  for(size_t i=0;i<x.parents.size();i++){s+=std::string(i?",":"")+std::to_string(x.parents[i]);}s+="],\"bindposes\":[";
  for(size_t i=0;i<x.bindposes.size();i+=4){float f=f32(x.bindposes.data()+i);require(std::isfinite(f),"Skeleton bindpose");s+=std::string(i?",":"")+jnumber(f);}s+="]}";}
 return s+"]}\n";}
}
