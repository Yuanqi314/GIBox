// SPDX-License-Identifier: GPL-3.0-only
#pragma once
// Texture-only mods, phase 1: read-only diagnostics (nothing here or in its Runtime wiring writes to the game).
//
// The App writes GIBox/texture-diag/<model>.json for a mod that only changes textures (TextureDiag.java): its
// TextureOverride rules as the INI states them. The Runtime reads the controlled character's renderers (their game
// mesh and materials, also under a GIBox model) and this file matches the two and words the log lines.
//
// What a rule is matched by, strongest first; the hash itself never takes part: a 3DMigoto hash is taken from the PC
// game's D3D11 buffer or texture, which Android does not have, and nothing here recomputes one.
//  * index start: match_first_index is where the PC draw starts in its index buffer; a submesh whose indexStart
//    (Mesh.GetIndexStart, read by name through IL2CPP) equals it, on exactly one renderer, with every other
//    match_first_index of the same hash also a submesh start of that renderer, is "confirmed" when the renderer has as
//    many materials as submeshes (slot = submesh). It holds only where the Android mesh keeps the PC index order,
//    which these logs are there to show.
//  * the part name the INI gives the section (DressA -> dress) against the renderer's name, counts (segments of the
//    hash, submeshes, materials) and the segment's place among them: "likely" at best, never "confirmed".
//  * the size of the mod's texture against the game's (texture-hash overrides): candidates only.
// A PC segment that starts inside an Android submesh (not at its start) means PC draws were merged into one submesh
// and one material slot: reported as such, ambiguous when the merged segments ask for different textures.
#include "core.hpp"
#include "json.hpp"
#include "render_probe.hpp"
#include <algorithm>
#include <map>
#include <set>
#include <string>
#include <tuple>
#include <vector>

namespace ms::texdiag {

struct Binding {std::string slot,resource,role,condition,via;};
struct Rule {std::string section,part,hint,hash,kind,evidence;int slotLetter=-1;long long first=-1;int priority=0;bool textureName=false;
 std::vector<Binding> textures;std::vector<std::string> external,conditions,draws;};
struct Resource {std::string name,file,kind,format,error;bool exists=false;long long width=-1,height=-1,mips=-1;};
struct Doc {std::string path,stamp,name,ini,characterName;unsigned character=0;std::vector<Rule> rules;std::map<std::string,Resource> resources;std::vector<std::string> variables,others,notes;};

inline std::string str(const Json& j,const char* k){const auto& v=j.at(k);return v.type==Json::String?v.string:std::string();}
inline long long num(const Json& j,const char* k,long long fallback){const auto& v=j.at(k);return v.type==Json::Number&&std::isfinite(v.number)&&std::floor(v.number)==v.number&&std::fabs(v.number)<9e15?(long long)v.number:fallback;}
// The App's document (version 1); anything it cannot use is an error naming the field, never a silent skip.
inline Doc parse(const std::string& text,const std::string& path){Doc d;d.path=path;Json j=JsonParser(text).parse();
 require(j.type==Json::Object&&str(j,"format")=="gibox-texture-diag","not a GIBox texture diagnostic");require(num(j,"version",0)==1,"texture diagnostic version "+std::to_string(num(j,"version",0))+" is not supported");
 d.name=str(j,"name");d.ini=str(j,"ini");d.characterName=str(j,"characterName");auto c=num(j,"character",0);require(c==0||(c>=10000000&&c<=10001999),"texture diagnostic character");d.character=unsigned(c);
 const auto& rules=j.at("rules");require(rules.type==Json::Array&&rules.array.size()<=4096,"texture diagnostic rules");
 for(auto& r:rules.array){require(r.type==Json::Object,"texture diagnostic rule");Rule x;x.section=str(r,"section");x.part=str(r,"part");x.hint=str(r,"hint");x.hash=str(r,"hash");x.kind=str(r,"kind");x.evidence=str(r,"hashEvidence");
  x.slotLetter=int(num(r,"slotLetter",-1));x.first=num(r,"first",-1);x.priority=int(num(r,"priority",0));x.textureName=r.at("textureName").type==Json::Bool&&r.at("textureName").boolean;
  require(!x.section.empty()&&!x.kind.empty(),"texture diagnostic rule without section or kind");
  const auto& t=r.at("textures");if(t.type==Json::Array)for(auto& b:t.array){require(b.type==Json::Object,"texture binding");x.textures.push_back({str(b,"slot"),str(b,"resource"),str(b,"role"),str(b,"condition"),str(b,"via")});}
  for(auto* k:{"external","conditions"}){const auto& a=r.at(k);if(a.type==Json::Array)for(auto& v:a.array)if(v.type==Json::String)(std::string(k)=="external"?x.external:x.conditions).push_back(v.string);}
  const auto& di=r.at("drawIndexed");if(di.type==Json::Array)for(auto& v:di.array)if(v.type==Json::Object)x.draws.push_back(str(v,"args")+(str(v,"condition").empty()?"":" if "+str(v,"condition")));
  d.rules.push_back(std::move(x));}
 const auto& res=j.at("resources");if(res.type==Json::Array)for(auto& r:res.array){if(r.type!=Json::Object)continue;Resource x;x.name=str(r,"name");x.file=str(r,"file");x.kind=str(r,"kind");x.format=str(r,"format");x.error=str(r,"error");
  x.exists=r.at("exists").type==Json::Bool&&r.at("exists").boolean;x.width=num(r,"width",-1);x.height=num(r,"height",-1);x.mips=num(r,"mips",-1);d.resources[x.name]=x;}
 const auto& vars=j.at("variables");if(vars.type==Json::Array)for(auto& v:vars.array)if(v.type==Json::Object){std::string vals;const auto& a=v.at("values");if(a.type==Json::Array)for(auto& n:a.array)if(n.type==Json::Number)vals+=(vals.empty()?"":",")+std::to_string((long long)n.number);
  d.variables.push_back(str(v,"name")+"="+std::to_string(num(v,"initial",0))+" of ["+vals+"]"+(v.at("persist").boolean?" persist":""));}
 const auto& others=j.at("otherInis");if(others.type==Json::Array)for(auto& o:others.array)if(o.type==Json::Object){std::string h;const auto& a=o.at("sharedHashes");if(a.type==Json::Array)for(auto& n:a.array)if(n.type==Json::String)h+=(h.empty()?"":",")+n.string;d.others.push_back(str(o,"ini")+(h.empty()?"":" shares "+h));}
 const auto& notes=j.at("notes");if(notes.type==Json::Array)for(auto& n:notes.array)if(n.type==Json::String)d.notes.push_back(n.string);
 return d;}

// What the Runtime read of one renderer. A string field starting with "未采集:" was not available, with the reason.
struct Texture {std::string property;int id=0;Ptr texture=0;std::string name,cls,width,height,mips,format,status;};
struct Material {Ptr material=0;std::string name,shader,error;std::vector<Texture> textures;std::vector<std::string> sharedWith;};
struct Renderer {Ptr renderer=0,native=0,mesh=0;std::string name,meshName,avatar,takenBy,swap,error;bool lod=false;unsigned vertices=0,submeshes=0,bones=0;
 renderprobe::UnityReader::MeshLayout layout;std::vector<Material> materials;
 bool known()const{if(layout.subs.empty()||int(layout.subs.size())!=layout.submeshes)return false;for(auto& s:layout.subs)if(!s.known)return false;return true;}};

enum class Verdict{Confirmed,Likely,Ambiguous,None,Skipped};
inline const char* verdictName(Verdict v){switch(v){case Verdict::Confirmed:return "确定匹配";case Verdict::Likely:return "高可信候选";case Verdict::Ambiguous:return "存在歧义";case Verdict::None:return "无法匹配";default:return "不需匹配";}}
struct Match {size_t rule=0;Verdict verdict=Verdict::None;int renderer=-1,submesh=-1,slot=-1;std::vector<std::string> reasons,candidates;};

inline std::string letters(const std::string& s){std::string o;for(unsigned char c:s)if(std::isalnum(c))o+=char(std::tolower(c));return o;}
// How well a part hint names a renderer: the length of the renderer's name when the hint ends or starts with it.
inline size_t nameScore(const std::string& hint,const std::string& renderer){auto h=letters(hint),r=letters(normalize(renderer));if(r.empty()||h.empty())return 0;
 bool end=h.size()>=r.size()&&h.compare(h.size()-r.size(),r.size(),r)==0,start=h.compare(0,r.size(),r)==0;return end||start?r.size():0;}
inline std::string hexOf(Ptr p){char b[24];std::snprintf(b,sizeof b,"0x%llx",(unsigned long long)p);return b;}
inline std::string label(const Renderer& r){return r.name+"#"+hexOf(r.native);}
// The game properties phase 2 would put a role on, among those the material has; "待诊断" when none or unknown.
inline const std::vector<const char*>& candidates(const std::string& role){
 static const std::vector<const char*> diffuse{"_MainTex"},light{"_LightMapTex","_LightMap","_LightmapTex","_Lightmap"},normal{"_BumpMap","_NormalMap","_BaseNormalMap","_NormalTex"},ramp{"_ShadowRampTex","_RampTex"},none{};
 if(role=="diffuse"){return diffuse;}if(role=="lightmap"){return light;}if(role=="normal"){return normal;}if(role=="ramp"){return ramp;}return none;}
inline std::string orDash(const std::string& s){return s.empty()?std::string("-"):s;}
inline std::string textureText(const Texture& t){return t.property+(t.texture?"="+(t.name.empty()?std::string("(unnamed)"):t.name)+(t.width.empty()?"":" "+t.width+"x"+t.height)+(t.format.empty()?"":" "+t.format)+(t.mips.empty()?"":" mips "+t.mips):std::string("=null"))+(t.status.empty()?"":" ["+t.status+"]");}
inline std::string plan(const Doc& d,const Rule& r,const Material* m){std::string out;
 for(auto& b:r.textures){std::string target="待诊断";
  if(m)for(auto* p:candidates(b.role)){for(auto& t:m->textures)if(t.property==p){target=std::string(p)+" (material has it; current "+(t.texture?t.name+" "+t.width+"x"+t.height:std::string("null"))+")";break;}if(target!="待诊断")break;}
  if(target=="待诊断")target+=b.role=="unknown"?" (slot role unknown)":m?" (no candidate property on this material)":" (no material)";
  auto res=d.resources.find(b.resource);std::string size=res==d.resources.end()?"":res->second.width>=0?" "+std::to_string(res->second.width)+"x"+std::to_string(res->second.height)+" "+res->second.format:res->second.error.empty()?"":" ("+res->second.error+")";
  out+=(out.empty()?"":"; ")+b.slot+" "+b.resource+size+" ["+b.role+"]"+(b.condition.empty()?"":" if "+b.condition)+" -> "+target;}
 return out.empty()?"no texture bindings":out;}

inline std::vector<Match> match(const Doc& d,const std::vector<Renderer>& rs){
 std::vector<Match> out;std::map<std::string,std::vector<long long>> firsts;
 for(auto& r:d.rules){if(r.first>=0)firsts[r.hash].push_back(r.first);}for(auto& f:firsts)std::sort(f.second.begin(),f.second.end());
 auto sameTextures=[&](long long a,long long b,const std::string& hash){const Rule* x=nullptr;const Rule* y=nullptr;for(auto& r:d.rules)if(r.hash==hash){if(r.first==a)x=&r;if(r.first==b)y=&r;}
  if(!x||!y||x->textures.size()!=y->textures.size()){return false;}for(size_t i=0;i<x->textures.size();i++)if(x->textures[i].slot!=y->textures[i].slot||x->textures[i].resource!=y->textures[i].resource||x->textures[i].condition!=y->textures[i].condition)return false;return true;};
 auto nameHits=[&](const Rule& r){std::vector<int> hits;size_t best=0;for(size_t i=0;i<rs.size();i++){auto s=nameScore(r.hint,rs[i].name);if(!s)continue;if(s>best){best=s;hits.clear();}if(s==best)hits.push_back(int(i));}return hits;};
 auto slotOf=[&](const Renderer& r,int sub,Match& m){size_t mats=r.materials.size(),subs=size_t(r.layout.submeshes>=0?r.layout.submeshes:int(r.submeshes));
  if(mats==subs){m.reasons.push_back("materials = submeshes ("+std::to_string(mats)+"): submesh "+std::to_string(sub)+" draws with material slot "+std::to_string(sub));return true;}
  if(size_t(sub)<mats&&size_t(sub)+1<subs){m.reasons.push_back(std::to_string(mats)+" materials, "+std::to_string(subs)+" submeshes: slot "+std::to_string(sub)+" by position (counts differ)");return false;}
  if(size_t(sub)>=mats){m.reasons.push_back("submesh "+std::to_string(sub)+" has no material slot ("+std::to_string(mats)+" materials, "+std::to_string(subs)+" submeshes)");m.slot=-1;return false;}
  m.reasons.push_back("last submesh with "+std::to_string(mats)+" materials for "+std::to_string(subs)+" submeshes: standard Unity draws the extra materials over it too; unverified on this engine");return false;};
 // Rules without match_first_index for a hash others split go last: they follow what the segments matched.
 std::vector<size_t> order;for(size_t k=0;k<d.rules.size();k++)if(d.rules[k].kind!="whole-ib")order.push_back(k);for(size_t k=0;k<d.rules.size();k++)if(d.rules[k].kind=="whole-ib")order.push_back(k);
 for(size_t k:order){const Rule& r=d.rules[k];Match m;m.rule=k;
  if(r.kind=="control"){m.verdict=Verdict::Skipped;m.reasons.push_back("binds no texture (sets variables only)");out.push_back(m);continue;}
  auto names=nameHits(r);std::string nameText;for(int i:names)nameText+=(nameText.empty()?"":", ")+label(rs[size_t(i)]);
  if(r.kind=="segment"){const auto& group=firsts[r.hash];
   std::vector<std::pair<int,int>> exact,inside;bool unknown=false;
   for(size_t i=0;i<rs.size();i++){if(!rs[i].known()){unknown=true;continue;}const auto& subs=rs[i].layout.subs;
    for(size_t j=0;j<subs.size();j++){if(subs[j].start==uint64_t(r.first))exact.push_back({int(i),int(j)});else if(uint64_t(r.first)>subs[j].start&&uint64_t(r.first)<uint64_t(subs[j].start)+subs[j].count)inside.push_back({int(i),int(j)});}}
   auto groupExact=[&](int i){unsigned n=0;for(long long f:group)for(auto& s:rs[size_t(i)].layout.subs)if(s.start==uint64_t(f)){n++;break;}return n;};
   // Index 0 starts a submesh on every renderer: the other segments of the hash decide, when they all start
   // submeshes of only one of them (index starts, not order); otherwise the part name, which only makes it likely.
   if(exact.size()>1&&group.size()>1){std::vector<std::pair<int,int>> whole;for(auto& h:exact)if(groupExact(h.first)==group.size())whole.push_back(h);
    if(whole.size()==1){m.reasons.push_back(std::to_string(exact.size())+" renderers have a submesh starting at "+std::to_string(r.first)+"; only "+label(rs[size_t(whole[0].first)])+" has all "+std::to_string(group.size())+" of the hash's match_first_index values as submesh starts");exact=whole;}}
   if(exact.size()>1&&!names.empty()){std::vector<std::pair<int,int>> named;for(auto& h:exact)if(std::find(names.begin(),names.end(),h.first)!=names.end())named.push_back(h);
    if(named.size()==1){m.reasons.push_back(std::to_string(exact.size())+" renderers have a submesh starting at "+std::to_string(r.first)+"; the part name picks "+label(rs[size_t(named[0].first)]));exact=named;m.verdict=Verdict::Likely;}}
   if(exact.size()==1){auto [i,j]=exact[0];const auto& R=rs[size_t(i)];const auto& sm=R.layout.subs[size_t(j)];m.renderer=i;m.submesh=j;m.slot=j;
    m.reasons.push_back("submesh "+std::to_string(j)+" indexStart "+std::to_string(sm.start)+" = match_first_index");
    unsigned all=groupExact(i);bool complete=all==group.size();m.reasons.push_back(std::to_string(all)+" of the hash's "+std::to_string(group.size())+" match_first_index values are submesh starts of this renderer");
    auto next=std::upper_bound(group.begin(),group.end(),r.first);bool countOk=true;
    if(next!=group.end()){long long pc=*next-r.first;if(pc==(long long)sm.count)m.reasons.push_back("index count "+std::to_string(sm.count)+" = distance to the next match_first_index");
     else{countOk=false;m.reasons.push_back("index count "+std::to_string(sm.count)+" vs "+std::to_string(pc)+" to the next match_first_index");}}
    bool merged=false;for(long long f:group)if(f>r.first&&uint64_t(f)<uint64_t(sm.start)+sm.count){merged=true;bool same=sameTextures(r.first,f,r.hash);
     m.reasons.push_back("PC segment "+std::to_string(f)+" lies inside this submesh too (PC draws merged into one slot): "+(same?"same textures, one material covers both":"different textures, one material cannot carry both"));if(!same)countOk=false;}
    bool slotSure=slotOf(R,j,m);
    if(!names.empty()&&std::find(names.begin(),names.end(),i)==names.end()){m.reasons.push_back("the part name '"+r.hint+"' points at "+nameText+" instead");slotSure=false;}
    else if(!names.empty())m.reasons.push_back("the part name '"+r.hint+"' agrees");
    if(m.verdict!=Verdict::Likely)m.verdict=complete&&countOk&&slotSure?Verdict::Confirmed:(merged&&!countOk?Verdict::Ambiguous:Verdict::Likely);
    out.push_back(m);continue;}
   if(exact.size()>1){m.verdict=Verdict::Ambiguous;for(auto& h:exact)m.candidates.push_back(label(rs[size_t(h.first)])+" submesh "+std::to_string(h.second));m.reasons.push_back("several renderers have a submesh starting at "+std::to_string(r.first)+" and the part name does not decide");out.push_back(m);continue;}
   if(!inside.empty()){m.verdict=Verdict::Ambiguous;for(auto& h:inside){m.candidates.push_back(label(rs[size_t(h.first)])+" submesh "+std::to_string(h.second));}
    m.reasons.push_back("match_first_index "+std::to_string(r.first)+" lies inside an Android submesh: the PC segment was merged with others into one submesh / material slot");
    // The renderer where the hash's other segments start submeshes is the one it most likely went into.
    std::vector<std::pair<int,int>> fit;unsigned most=0;for(auto& h:inside){unsigned n=groupExact(h.first);if(n>most){most=n;fit.clear();}if(n&&n==most)fit.push_back(h);}
    if(fit.size()==1){m.renderer=fit[0].first;m.submesh=fit[0].second;m.slot=fit[0].second;m.reasons.push_back("most of the hash's other segments ("+std::to_string(most)+") start submeshes of "+label(rs[size_t(m.renderer)])+": most likely merged into its submesh "+std::to_string(m.submesh));}
    out.push_back(m);continue;}
   // No index layout to compare: the name, the counts and the order; never more than likely.
   std::vector<int> unread;for(int i:names)if(!rs[size_t(i)].known())unread.push_back(i);
   if(names.size()==1&&!unread.empty()){const auto& R=rs[size_t(names[0])];size_t pos=size_t(std::find(group.begin(),group.end(),r.first)-group.begin());
    size_t subs=R.layout.submeshes>=0?size_t(R.layout.submeshes):R.submeshes;m.renderer=names[0];m.reasons.push_back("indexStart not readable on "+label(R)+(R.layout.status.empty()?"":" ("+R.layout.status+")"));
    m.reasons.push_back("the part name '"+r.hint+"' names it; segment "+std::to_string(pos)+" of "+std::to_string(group.size())+", "+std::to_string(subs)+" submeshes, "+std::to_string(R.materials.size())+" materials");
    if(group.size()==subs&&subs==R.materials.size()){m.submesh=int(pos);m.slot=int(pos);m.verdict=Verdict::Likely;m.reasons.push_back("counts agree: slot "+std::to_string(pos)+" by order only (order alone is not proof)");}
    else{m.verdict=Verdict::Ambiguous;m.reasons.push_back("counts differ: no slot by order");}
    out.push_back(m);continue;}
   if(names.size()>1){m.verdict=Verdict::Ambiguous;for(int i:names)m.candidates.push_back(label(rs[size_t(i)]));m.reasons.push_back("no submesh starts at "+std::to_string(r.first)+" where the layout is readable, and the part name fits several renderers");out.push_back(m);continue;}
   m.verdict=Verdict::None;m.reasons.push_back(std::string("no renderer has a submesh starting at ")+std::to_string(r.first)+(unknown?" among those whose layout is readable":"")+(names.empty()?"; the part name '"+r.hint+"' names no renderer":"; "+nameText+" (by name) starts none there"));
   m.reasons.push_back("so either Android's index order differs from the PC one, or the hash belongs to another character / outfit");out.push_back(m);continue;}
  if(r.kind=="whole-ib"){m.reasons.push_back("no match_first_index: 3DMigoto applies it to every draw of hash "+r.hash);
   std::set<int> hit;for(auto& x:out)if(x.renderer>=0&&d.rules[x.rule].hash==r.hash&&(x.verdict==Verdict::Confirmed||x.verdict==Verdict::Likely))hit.insert(x.renderer);
   if(hit.size()==1){m.renderer=*hit.begin();m.verdict=Verdict::Likely;m.reasons.push_back("the hash's segments match "+label(rs[size_t(m.renderer)])+": every slot of it");}
   else{m.verdict=hit.empty()?Verdict::None:Verdict::Ambiguous;for(int i:hit)m.candidates.push_back(label(rs[size_t(i)]));m.reasons.push_back(hit.empty()?"the hash's segments match no renderer":"the hash's segments match several renderers");}
   out.push_back(m);continue;}
  // hash-unknown: an IB, VB or texture hash; neither can be computed here. Name and texture size give candidates.
  m.reasons.push_back(r.evidence.empty()?"hash kind unknown":r.evidence);m.reasons.push_back("a 3DMigoto hash cannot be recomputed on Android; no confirmation possible");
  for(int i:names)m.candidates.push_back(label(rs[size_t(i)])+" (by part name)");
  if(r.textureName||!names.empty()){const Resource* mine=nullptr;for(auto& b:r.textures){auto it=d.resources.find(b.resource);if(it!=d.resources.end()&&it->second.width>0){mine=&it->second;break;}}
   if(mine){unsigned n=0;for(size_t i=0;i<rs.size()&&n<8;i++)for(size_t s=0;s<rs[i].materials.size()&&n<8;s++)for(auto& t:rs[i].materials[s].textures)
     if(t.texture&&t.width==std::to_string(mine->width)&&t.height==std::to_string(mine->height)&&n<8){m.candidates.push_back(label(rs[i])+" slot "+std::to_string(s)+" "+t.property+"="+t.name+" (same size "+t.width+"x"+t.height+")");n++;}
    if(!n)m.reasons.push_back("no game texture of the mod texture's size "+std::to_string(mine->width)+"x"+std::to_string(mine->height)+" on this character (Android textures may be smaller)");}}
  m.verdict=m.candidates.empty()?Verdict::None:Verdict::Ambiguous;out.push_back(m);}
 // Several rules on one slot: phase 2 cannot put both on one material unless their conditions keep them apart.
 std::map<std::tuple<int,int>,std::vector<size_t>> bySlot;for(size_t i=0;i<out.size();i++)if(out[i].renderer>=0&&out[i].slot>=0)bySlot[{out[i].renderer,out[i].slot}].push_back(i);
 for(auto& kv:bySlot)if(kv.second.size()>1){std::string others;for(size_t i:kv.second)others+=(others.empty()?"":", ")+d.rules[out[i].rule].section;
  for(size_t i:kv.second){out[i].reasons.push_back("rules on the same slot: "+others);if(out[i].verdict==Verdict::Confirmed)out[i].verdict=Verdict::Likely;}}
 std::sort(out.begin(),out.end(),[](const Match& a,const Match& b){return a.rule<b.rule;});
 return out;}

inline std::string matchLine(const Doc& d,const Match& m,const std::vector<Renderer>& rs){const Rule& r=d.rules[m.rule];
 std::string s=d.name+": "+r.section+" hash="+(r.hash.empty()?"-":r.hash)+" match_first_index="+(r.first>=0?std::to_string(r.first):"-")+" -> "+verdictName(m.verdict);
 const Material* mat=nullptr;
 if(m.renderer>=0){const auto& R=rs[size_t(m.renderer)];s+=" renderer="+label(R)+" mesh="+R.meshName;if(m.submesh>=0)s+=" submesh="+std::to_string(m.submesh);
  if(m.slot>=0){s+=" slot="+std::to_string(m.slot);if(size_t(m.slot)<R.materials.size()){mat=&R.materials[size_t(m.slot)];s+=" material="+mat->name;}}}
 std::string why;for(auto& x:m.reasons)why+=(why.empty()?"":"; ")+x;s+=" | why: "+why;
 if(!m.candidates.empty()){std::string c;for(auto& x:m.candidates)c+=(c.empty()?"":"; ")+x;s+=" | candidates: "+c;}
 if(m.verdict!=Verdict::Skipped)s+=" | textures (phase-2 plan, not applied): "+plan(d,r,mat);
 return s;}
inline std::string ruleLine(const Doc& d,const Rule& r){std::string t;for(auto& b:r.textures)t+=(t.empty()?"":", ")+b.slot+"="+(b.resource.empty()?"null":b.resource)+"["+b.role+"]"+(b.condition.empty()?"":" if "+b.condition)+(b.via.empty()?"":" via "+b.via);
 std::string x;for(auto& e:r.external)x+=(x.empty()?"":", ")+e;std::string dr;for(auto& e:r.draws)dr+=(dr.empty()?"":", ")+e;
 return d.name+": "+r.section+" hash="+(r.hash.empty()?"-":r.hash)+" match_first_index="+(r.first>=0?std::to_string(r.first):"-")+" priority="+std::to_string(r.priority)+" kind="+r.kind+" part="+r.hint+(r.slotLetter>=0?std::string(" letter=")+char('A'+r.slotLetter):"")
  +" textures=["+t+"]"+(dr.empty()?"":" drawindexed=["+dr+"]")+(x.empty()?"":" external=["+x+"]");}
inline std::string rendererLine(const Renderer& r,unsigned character){
 return "character="+std::to_string(character)+" renderer="+label(r)+" mesh="+r.meshName+"#"+hexOf(r.mesh)+" lod="+(r.lod?"lower (by name)":"full (by name)")+" vertices="+std::to_string(r.vertices)
  +" submeshes(native)="+std::to_string(r.submeshes)+" subMeshCount="+orDash(r.layout.submeshCount)+" vertexCount="+orDash(r.layout.vertexCount)+" bones="+std::to_string(r.bones)+" readable="+orDash(r.layout.readable)+" indexFormat="+orDash(r.layout.indexFormat)
  +" materials="+std::to_string(r.materials.size())+" avatar="+(r.avatar.empty()?"-":r.avatar)+(r.takenBy.empty()?"":" taken-over-by="+r.takenBy+" (game mesh/materials read)")+(r.swap.empty()?"":" texture-swap="+r.swap)
  +(r.layout.status=="read"?"":" layout="+r.layout.status)+(r.error.empty()?"":" error="+r.error);}
inline std::vector<std::string> submeshLines(const Renderer& r){std::vector<std::string> out;
 for(size_t i=0;i<r.layout.subs.size();i++){auto& s=r.layout.subs[i];std::string slot=i<r.materials.size()?std::to_string(i)+" ("+r.materials[i].name+")":"none";
  out.push_back(label(r)+" submesh="+std::to_string(i)+(s.known?" indexStart="+std::to_string(s.start)+" indexCount="+std::to_string(s.count):" range="+s.error)+" baseVertex="+s.base+" topology="+s.topology+" material-slot-by-position="+slot);}
 return out;}
inline std::vector<std::string> materialLines(const Renderer& r){std::vector<std::string> out;
 for(size_t i=0;i<r.materials.size();i++){auto& m=r.materials[i];std::string t;for(auto& x:m.textures)t+=(t.empty()?"":", ")+textureText(x);std::string shared;for(auto& x:m.sharedWith)shared+=(shared.empty()?"":",")+x;
  out.push_back(label(r)+" slot="+std::to_string(i)+" material="+m.name+"#"+hexOf(m.material)+" shader="+m.shader+(shared.empty()?"":" shared-with="+shared)+" textures=["+t+"]"+(m.error.empty()?"":" error="+m.error));}
 return out;}
}  // namespace ms::texdiag
