// SPDX-License-Identifier: GPL-3.0-only
// Host-only, side by side: a GIMI Present frame-loop MOD run by the new logic core, and the same MOD as the old path
// handles it (the old converter's per-frame draws from LegacyAnimationDump, advanced by the old runtime FrameLoop).
// Per presented frame and per part it compares the IB, the three vertex streams, ps-t0..2 and the face texture.
//
// The game's draws are described by this test adapter, for the GIMI layout of a Genshin character: the skinning pass
// has the Position buffer in vb0 and the Blend buffer in vb1; each part draw has the Texcoord buffer in vb1 and the
// IB (with its first index); the face draw has the face texture in ps-t0. The hashes come from the MOD's own
// TextureOverride sections (by GIMI's section names). The checktextureoverride order (vertex buffers before ib) is
// what the d3dx.ini host lists must do for drawindexed = auto to see the replaced buffers; it is an assumption of
// this adapter, not something the fixed 3DMigoto commit contains.
//
// usage: migoto_parity <mod folder> <main ini> <dump from LegacyAnimationDump> [frames]
#include "migoto/adapters.hpp"
#include "model.hpp"
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
using namespace migoto;
namespace fs=std::filesystem;

static std::string slurp(const fs::path& p){std::ifstream f(p,std::ios::binary);std::ostringstream o;o<<f.rdbuf();return o.str();}
static std::string norm(std::string s){std::replace(s.begin(),s.end(),'\\','/');s=lower(trim(s));while(startsWith(s,"./"))s=s.substr(2);return s;}
// XXMI's include_recursive = Mods with exclude_recursive = DISABLED*: files of a folder (case-insensitive order)
// before its subfolders; the namespace is the path from the 3DMigoto folder.
static void collect(const fs::path& dir,const std::string& rel,std::vector<IniFile>& out){
 std::vector<fs::path> files,dirs;for(auto& e:fs::directory_iterator(dir)){std::string n=e.path().filename().string();if(istarts(n,"disabled")||lower(n)=="desktop.ini")continue;
  if(e.is_directory())dirs.push_back(e.path());else if(lower(e.path().extension().string())==".ini")files.push_back(e.path());}
 auto by=[](const fs::path& a,const fs::path& b){return lower(a.filename().string())<lower(b.filename().string());};std::sort(files.begin(),files.end(),by);std::sort(dirs.begin(),dirs.end(),by);
 for(auto& f:files)out.push_back({rel+"\\"+f.filename().string(),slurp(f)});
 for(auto& d:dirs)collect(d,rel+"\\"+d.filename().string(),out);}

struct LegacyDraw {std::string part,ib,position,blend,texcoord,t0,t1,t2;};

int main(int argc,char** argv){
 if(argc<4){std::cerr<<"usage: migoto_parity <mod folder> <main ini> <legacy dump> [frames]\n";return 2;}
 fs::path mod=argv[1];std::string mainIni=argv[2];int frames=argc>4?std::atoi(argv[4]):300;
 // ---- the old path
 std::ifstream dump(argv[3]);std::string line;ms::AnimationSpec spec;std::map<int,std::vector<LegacyDraw>> legacy;std::map<int,std::string> legacyFace;
 while(std::getline(dump,line)){std::istringstream in(line);std::string kind;in>>kind;
  if(kind=="animation"){in>>spec.variable>>spec.first>>spec.last>>spec.step>>spec.initial>>spec.counter>>spec.period>>spec.counterStep;int v;while(in>>v){ms::AnimationFrame f;f.value=v;spec.frames.push_back(f);}}
  else if(kind=="draw"){int v,start,count;LegacyDraw d;in>>v>>d.part>>start>>count;std::string kv;
   while(in>>kv){auto eq=kv.find('=');std::string k=kv.substr(0,eq),x=lower(kv.substr(eq+1));
    if(k=="ib")d.ib=x;else if(k=="position")d.position=x;else if(k=="blend")d.blend=x;else if(k=="texcoord")d.texcoord=x;else if(k=="ps-t0")d.t0=x;else if(k=="ps-t1")d.t1=x;else if(k=="ps-t2")d.t2=x;}
   legacy[v].push_back(d);}
  else if(kind=="face"){int v;std::string f;in>>v>>f;legacyFace[v]=f=="-"?"-":norm(f);}}
 if(spec.frames.empty()){std::cerr<<"no animation in the dump\n";return 2;}
 ms::FrameLoop old(spec);old.tick(0);
 // ---- the new core
 std::vector<IniFile> files;collect(mod,"Mods\\"+mod.filename().string(),files);
 Engine e;e.load(files);e.start();
 auto hashOf=[&](const std::string& suffix){for(auto& kv:e.overrides)for(auto& t:kv.second){std::string n=lower(t.name);if(!t.drawMatch&&n.size()>=suffix.size()&&n.compare(n.size()-suffix.size(),suffix.size(),suffix)==0)return t.hash;}
  std::cerr<<"no TextureOverride named *"<<suffix<<"\n";std::exit(2);return 0u;};
 uint32_t position=hashOf("position"),blend=hashOf("blend"),texcoord=hashOf("texcoord"),face=hashOf("faceheaddiffuse");
 std::vector<Session::Pass> passes;DrawInfo skin;skin.bindings={{"vb0",Ref::game(position)},{"vb1",Ref::game(blend)}};skin.checks={"vb0","vb1"};passes.push_back({"skin",skin});
 std::map<std::string,std::string> partPass;   // legacy part name -> pass
 for(auto& kv:e.overrides)for(auto& t:kv.second){if(!t.match.count("first_index"))continue;std::string full=t.name,shortName=full.substr(full.rfind('\\')+1);
  DrawInfo d;d.bindings={{"vb1",Ref::game(texcoord)},{"ib",Ref::game(t.hash)}};d.checks={"vb0","vb1","vb2","ib"};d.call={{"first_index",float(t.match["first_index"].value)}};
  passes.push_back({shortName,d});partPass[lower(shortName)]=shortName;}
 DrawInfo faceDraw;faceDraw.bindings={{"ps-t0",Ref::game(face)}};faceDraw.checks={"ps-t0"};passes.push_back({"face",faceDraw});
 Session session(e);
 auto fileOf=[&](const std::string& shortName){for(auto& r:e.resources)if(lower(r.second.shortName)==lower(shortName)){auto f=r.second.values.find("filename");return f==r.second.values.end()?std::string("-"):norm(f->second);}return std::string("?");};
 auto bound=[](const std::string& draw,const std::string& slot){auto p=draw.find(" "+slot+"=");if(p==std::string::npos)return std::string("-");p+=slot.size()+2;return lower(draw.substr(p,draw.find(' ',p)-p));};
 int mismatches=0,compared=0;std::set<int> seen;std::vector<int> firstFrames;
 for(int tick=1;tick<=frames;tick++){std::map<std::string,Session::Selection> sel;session.tick(passes,tick/60.0,&sel);
  int value=old.spec.frames[old.current].value;seen.insert(value);if(firstFrames.size()<12)firstFrames.push_back(value);
  auto& skinSel=sel["skin"];std::string pos=skinSel.draws.empty()?"-":bound(skinSel.draws[0],"vb0"),bl=skinSel.draws.empty()?"-":bound(skinSel.draws[0],"vb1");
  for(auto& d:legacy[value]){auto pp=partPass.find(lower(d.part));if(pp==partPass.end()){std::cerr<<"tick "<<tick<<": the core has no draw for legacy part "<<d.part<<"\n";mismatches++;continue;}
   auto& ps=sel[pp->second];compared++;
   if(ps.draws.size()!=1){std::cerr<<"tick "<<tick<<" "<<d.part<<": core issued "<<ps.draws.size()<<" draws\n";mismatches++;continue;}
   const std::string& dr=ps.draws[0];
   std::vector<std::pair<std::string,std::pair<std::string,std::string>>> fields={{"ib",{d.ib,bound(dr,"ib")}},{"position",{d.position,pos}},{"blend",{d.blend,bl}},
    {"texcoord",{d.texcoord,bound(dr,"vb1")}},{"ps-t0",{d.t0,bound(dr,"ps-t0")}},{"ps-t1",{d.t1,bound(dr,"ps-t1")}},{"ps-t2",{d.t2,bound(dr,"ps-t2")}}};
   for(auto& f:fields)if(f.second.first!=f.second.second){if(mismatches<10)std::cerr<<"tick "<<tick<<" frame "<<value<<" "<<d.part<<" "<<f.first<<": old "<<f.second.first<<", core "<<f.second.second<<"\n";mismatches++;}}
  std::string coreFace=fileOf(sel["face"].bindings["ps-t0"]);if(legacyFace[value]!=coreFace){if(mismatches<10)std::cerr<<"tick "<<tick<<" face: old "<<legacyFace[value]<<", core "<<coreFace<<"\n";mismatches++;}
  old.tick(uint32_t(tick));}
 ImportScan scan(e);
 std::cout<<"parity "<<mod.filename().string()<<"/"<<mainIni<<": "<<frames<<" presented frames, "<<compared<<" part draws compared, "<<mismatches<<" mismatches; "
  <<seen.size()<<" of "<<spec.frames.size()<<" frames shown; first frames:";for(int f:firstFrames)std::cout<<' '<<f;
 std::cout<<"\n  core: "<<files.size()<<" INI loaded, "<<e.resources.size()<<" resources ("<<scan.resources.size()<<" bindable), "<<session.changes<<" selection changes over "<<passes.size()<<" draws (";{bool first=true;for(auto& c:session.changesOf){std::cout<<(first?"":", ")<<c.first<<" "<<c.second;first=false;}}std::cout<<"), finite-state: "<<(scan.finiteStates?"yes":"no");
 for(auto& v:scan.vars)if(v.second.readByDraws)std::cout<<"; "<<v.first<<(v.second.finite?" finite":" runtime ("+(v.second.why.empty()?std::string():v.second.why[0])+")");
 std::cout<<"\n";for(auto& d:e.diag.list)std::cout<<"  "<<(d.level==Diagnostic::Limitation?"limitation":d.level==Diagnostic::Warning?"warning":d.level==Diagnostic::Error?"error":"notice")<<": "<<d.where<<": "<<d.message<<"\n";
 return mismatches?1:0;}
