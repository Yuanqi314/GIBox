#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace ms {
using Bytes=std::vector<uint8_t>;
inline void require(bool b,const std::string& s){if(!b)throw std::runtime_error(s);}
inline uint32_t u32(const uint8_t* p){return uint32_t(p[0])|(uint32_t(p[1])<<8)|(uint32_t(p[2])<<16)|(uint32_t(p[3])<<24);}
inline float f32(const uint8_t* p){uint32_t v=u32(p);float f;std::memcpy(&f,&v,4);return f;}
inline void put32(uint8_t* p,uint32_t v){for(int i=0;i<4;i++)p[i]=v>>(8*i);}
inline Bytes read(const std::string& path,size_t max=256*1024*1024){
 std::ifstream f(path,std::ios::binary|std::ios::ate);require(bool(f),"Cannot read: "+path);
 auto n=f.tellg();require(n>=0&&uint64_t(n)<=max,"File too large: "+path);Bytes b(size_t(n),0);f.seekg(0);
 if(n){f.read(reinterpret_cast<char*>(b.data()),n);}
 require(bool(f),"Short read: "+path);return b;
}
inline std::string text(const std::string& p){auto b=read(p,1024*1024);return std::string(b.begin(),b.end());}
inline void write(const std::string& p,const void* b,size_t n){std::ofstream f(p,std::ios::binary|std::ios::trunc);require(bool(f),"Cannot write: "+p);if(n)f.write(static_cast<const char*>(b),n);require(bool(f),"Write failed: "+p);}
inline void write(const std::string& p,const std::string& b){write(p,b.data(),b.size());}
inline bool exists(const std::string& p){return std::filesystem::exists(p);}
inline std::string trim(std::string s){auto a=s.find_first_not_of(" \t\r\n");if(a==s.npos)return {};return s.substr(a,s.find_last_not_of(" \t\r\n")-a+1);}
inline std::vector<std::string> lines(std::string s){if(s.compare(0,3,"\xef\xbb\xbf")==0)s.erase(0,3);std::istringstream in(s);std::vector<std::string> v;while(std::getline(in,s)){s=trim(s);if(!s.empty())v.push_back(s);}return v;}
inline std::vector<std::string> words(const std::string& s){std::istringstream in(s);std::vector<std::string> v;std::string a;while(in>>a)v.push_back(a);return v;}
inline unsigned decimal(const std::string& s){require(!s.empty()&&s.find_first_not_of("0123456789")==s.npos,"Invalid integer: "+s);auto n=std::stoull(s);require(n<=UINT32_MAX,"Integer overflow");return unsigned(n);}
inline unsigned config(const std::string& s){auto n=decimal(trim(s));require(n>=10000000&&n<=10001999,"Character ID out of range");return n;}
inline void filename(const std::string& s){require(!s.empty()&&s.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_-")==s.npos,"Invalid filename: "+s);}
inline std::string normalize(std::string s,bool bone=false){std::string o;for(unsigned char c:s){if(c>='A'&&c<='Z')c+=32;if(!bone||(c>='a'&&c<='z')||(c>='0'&&c<='9'))o+=char(c);}if(!bone){auto p=o.rfind("_lod");if(p!=o.npos&&p+4<o.size()&&o.find_first_not_of("0123456789",p+4)==o.npos)o.resize(p);}return o;}
struct Part {std::string name,group;unsigned material,bones;};
struct Material {std::string name;unsigned slot;};
struct Skin {Bytes data;std::set<unsigned> used;};
struct Submesh {unsigned start,count;};
struct Texture {unsigned w,h;Bytes pixels;};
inline std::vector<int> mapBones(const std::vector<std::string>& src,const std::vector<std::string>& dst,const Bytes& sb,const Bytes& tb,const std::set<unsigned>& used){
 require(!src.empty()&&src.size()<=512&&!dst.empty()&&dst.size()<=512&&sb.size()==64*src.size()&&tb.size()==64*dst.size(),"Bone count/bindpose length");
 std::map<std::string,std::vector<int>> sm,tm;for(size_t i=0;i<src.size();i++)sm[normalize(src[i],true)].push_back(i);for(size_t i=0;i<dst.size();i++)tm[normalize(dst[i],true)].push_back(i);std::vector<int> out(src.size(),-1);
 for(unsigned i:used){require(i<src.size(),"Bone source index");auto k=normalize(src[i],true);require(!k.empty()&&sm[k].size()==1&&tm[k].size()==1,"Bone name missing/ambiguous: "+src[i]);int t=tm[k][0];for(int j=0;j<16;j++){float a=f32(sb.data()+64*i+4*j),b=f32(tb.data()+64*t+4*j);require(std::isfinite(a)&&std::isfinite(b)&&std::abs(a-b)<=.001f*(1+std::max(std::abs(a),std::abs(b))),"Bindpose mismatch: "+src[i]);}out[i]=t;}return out;
}
inline void remap(Skin& s,const std::vector<int>& map){for(size_t i=0;i<s.data.size();i+=32)for(int j=0;j<4;j++){auto bi=u32(s.data.data()+i+16+4*j);bool weighted=f32(s.data.data()+i+4*j)>0;require(!weighted||(bi<map.size()&&map[bi]>=0),"Unmapped bone");put32(s.data.data()+i+16+4*j,weighted?map[bi]:0);}}
Texture decodeDds(const Bytes& b);

// Identity of an original Mesh asset, readable without CPU vertex data:
// asset name, vertex count (shared+0x74), submesh count (shared+0xc0), bindpose count (shared+0xe0).
// The renderer name is informational. Same rule as PC importers matching buffers by hash.
struct Fingerprint {std::string renderer,mesh;unsigned vertices=0,submeshes=0,bones=0;};
inline std::string meshName(std::string s){std::string o;for(unsigned char c:s)o+=char(c>='A'&&c<='Z'?c+32:c);for(const char* junk:{"(clone)"," instance"}){size_t p;while((p=o.find(junk))!=o.npos)o.erase(p,std::strlen(junk));}return normalize(trim(o));}
inline bool matches(const Fingerprint& t,const Fingerprint& f){return t.vertices==f.vertices&&t.submeshes==f.submeshes&&t.bones==f.bones&&(t.mesh.empty()?normalize(t.renderer)==normalize(f.renderer):meshName(t.mesh)==meshName(f.mesh));}

// Switchable variants of a model (GIBox model format, "variants"): variables, their combinations, the default one.
struct Variants {std::vector<std::pair<std::string,std::vector<int>>> vars;std::vector<std::map<std::string,int>> combos;unsigned def=0;};
inline int integer(const std::string& s){require(!s.empty()&&s.size()<11&&s.find_first_not_of("-0123456789")==s.npos&&s.find('-',1)==s.npos&&s!="-","Invalid integer: "+s);return std::stoi(s);}
// Desired values start from the default combo; unknown names are ignored; no exact combo falls back to the default.
inline unsigned pick(const Variants& v,const std::map<std::string,int>& chosen){auto want=v.combos[v.def];for(auto& kv:chosen)if(want.count(kv.first))want[kv.first]=kv.second;for(unsigned i=0;i<v.combos.size();i++)if(v.combos[i]==want)return i;return v.def;}
}
