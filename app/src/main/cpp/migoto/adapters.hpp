// SPDX-License-Identifier: GPL-3.0-only
// GIBox 3DMigoto logic core: the two adapters over one engine.
//  - ImportScan (converter side): what can be known without running the mod: every [Resource] a command list can
//    bind (so meshes and textures can be prepared ahead), and, per variable, whether its values are a fixed finite
//    set. Only a variable every write of which is a constant (and no transition) is finite; a name, a file name
//    pattern or a count of draws never makes one.
//  - Session (game side): runs the frame's draws and Present through the engine and reports, per draw, whether
//    what it selects (skip, issued draws, bound resources) changed since the previous frame, so the renderer is
//    asked to switch only then, never rebuilt per frame.
// GPLv3 because it builds on the port in engine.hpp; it contains no upstream code of its own.
#pragma once
#include "engine.hpp"
#include <algorithm>
#include <sstream>

namespace migoto {

inline std::string describe(const Engine& e,const Ref& r){
 switch(r.kind){case Ref::None:return "-";case Ref::Null:return "null";
  case Ref::Game:{char b[16];std::snprintf(b,sizeof b,"game:%08x",r.hash);return b;}
  case Ref::Mod:{auto it=e.resources.find(r.resource);return it==e.resources.end()?r.resource:it->second.shortName;}}
 return "?";}

// What a renderer has to load for a ref: the file and its layout. Sections that name the same file the same way
// (GIMI animation frames often repeat one texture) are one resource to the renderer.
inline std::string identity(const Engine& e,const Ref& r){if(r.kind!=Ref::Mod)return describe(e,r);auto it=e.resources.find(r.resource);if(it==e.resources.end())return r.resource;
 auto get=[&](const char* k){auto v=it->second.values.find(k);return v==it->second.values.end()?std::string():v->second;};std::string file=get("filename");
 if(file.empty())return it->second.shortName;
 std::replace(file.begin(),file.end(),'\\','/');
 return lower(it->second.path)+"|"+lower(file)+"|"+lower(get("type"))+"|"+get("stride")+"|"+lower(get("format"));}

struct ImportScan {
 struct Binding {std::string slot,resource;};
 struct VarInfo {std::string name;bool finite=true,readByDraws=false;std::set<float> values;std::vector<std::string> why;};
 std::vector<Binding> bindings;          // every slot <- [Resource] a command list can execute, all branches
 std::set<std::string> resources;        // their section keys
 std::map<std::string,VarInfo> vars;
 std::vector<std::string> customShaders,unported;
 bool finiteStates=false;uint64_t states=1;   // true: the draw variables are all finite and their product is small

 static bool constant(const Node& n){if(n.kind==Node::Value)return true;if(n.kind==Node::Unary)return n.op!=Op::Random&&constant(*n.rhs);
  if(n.kind==Node::Binary)return constant(*n.lhs)&&constant(*n.rhs);
  return false;}
 static void reads(const Node& n,std::set<Var*>& out){if(n.kind==Node::Variable)out.insert(n.var);if(n.lhs)reads(*n.lhs,out);if(n.rhs)reads(*n.rhs,out);}

 void walk(Engine& e,CommandList& l,bool draws,std::set<const CommandList*>& seen,const std::string& where){
  if(!seen.insert(&l).second)return;
  for(auto& c:l.commands){Command* cmd=c.get();
   if(auto* a=cmd->kind()==Kind::Assign?static_cast<Assign*>(cmd):nullptr){if(a->param)continue;auto& v=vars[a->var->name];v.name=a->var->name;
    if(constant(*a->value.root)){StaticContext sc;float f=0;staticEvaluate(a->value,sc,f);v.values.insert(f);}
    else{v.finite=false;v.why.push_back(where+": "+a->line);}}
   else if(auto* i=cmd->kind()==Kind::If?static_cast<If*>(cmd):nullptr){if(draws){std::set<Var*> r;reads(*i->cond.root,r);for(auto* x:r){vars[x->name].name=x->name;vars[x->name].readByDraws=true;}}
    for(auto* sub:{&i->truePre,&i->truePost,&i->falsePre,&i->falsePost})walk(e,*sub,draws,seen,where);}
   else if(auto* r=cmd->kind()==Kind::Run?static_cast<RunList*>(cmd):nullptr){walk(e,r->target->pre,draws,seen,r->target->name);walk(e,r->target->post,draws,seen,r->target->name);}
   else if(auto* b=cmd->kind()==Kind::Bind?static_cast<Bind*>(cmd):nullptr){std::string src=b->src;if(e.resources.count(src)){bindings.push_back({b->dst,src});resources.insert(src);}
}
   else if(auto* s=cmd->kind()==Kind::CustomShader?static_cast<CustomShaderRun*>(cmd):nullptr)customShaders.push_back(s->where);
   else if(auto* u=cmd->kind()==Kind::Unported?static_cast<Unported*>(cmd):nullptr)unported.push_back(u->where);}}

 explicit ImportScan(Engine& e,uint64_t limit=4096){
  std::set<const CommandList*> seen;
  for(auto& kv:e.overrides)for(auto& t:kv.second){walk(e,t.list->pre,true,seen,t.name);walk(e,t.list->post,true,seen,t.name);}
  std::set<const CommandList*> other;
  for(auto& kv:e.lists){walk(e,kv.second->pre,false,other,kv.second->name);walk(e,kv.second->post,false,other,kv.second->name);}
  for(auto& k:e.keys)for(auto& p:k.presets){for(auto& v:p.vars){auto& info=vars[v.first->name];info.name=v.first->name;info.values.insert(v.second);
    if(p.transition||p.releaseTransition){info.finite=false;info.why.push_back("["+k.name+"] transition");}}
   if(p.run){walk(e,p.run->pre,false,other,k.name);walk(e,p.run->post,false,other,k.name);}}
  for(auto& kv:vars){auto g=e.globals.find(kv.first);if(g!=e.globals.end())kv.second.values.insert(e.initial[kv.first]);}
  finiteStates=true;
  for(auto& kv:vars){if(!kv.second.readByDraws)continue;if(!kv.second.finite){finiteStates=false;continue;}
   states*=std::max<uint64_t>(1,kv.second.values.size());if(states>limit)finiteStates=false;}}
};

class Session {
public:
 struct Pass {std::string name;DrawInfo draw;};
 // draws / bindings: section names, for people; key: the same by resource identity, which decides "changed".
 struct Selection {bool skip=false;std::vector<std::string> draws;std::map<std::string,std::string> bindings;std::string key;
  bool operator==(const Selection& o)const{return key==o.key;}};
 Engine& e;std::map<std::string,Selection> last;std::map<std::string,uint64_t> changesOf;uint64_t frames=0,changes=0;
 explicit Session(Engine& engine):e(engine){}
 Selection select(const DrawResult& r)const{Selection s;s.skip=r.skip;
  for(auto& d:r.draws){std::ostringstream o;o<<d.kind<<(d.automatic?"(auto)":d.fromCaller?"(from_caller)":"");for(auto a:d.args)o<<' '<<a;
   std::ostringstream k;k<<o.str();for(auto& b:d.bindings){o<<' '<<b.first<<'='<<describe(e,b.second);k<<' '<<b.first<<'='<<identity(e,b.second);}
   s.draws.push_back(o.str());s.key+=k.str()+";";}
  s.key+=s.skip?"skip;":"draw;";for(auto& b:r.bindings){s.bindings[b.first]=describe(e,b.second);s.key+=b.first+"="+identity(e,b.second)+" ";}
  return s;}
 // The frame's draws in the order the game issues them, then its Present. Returns the passes whose selection
 // changed (all of them on the first frame).
 std::vector<std::string> tick(const std::vector<Pass>& passes,double seconds,std::map<std::string,Selection>* out=nullptr){
  std::vector<std::string> changed;
  for(auto& p:passes){Selection s=select(e.draw(p.draw));auto it=last.find(p.name);if(it==last.end()||!(it->second==s)){changed.push_back(p.name);changesOf[p.name]++;}last[p.name]=s;if(out)(*out)[p.name]=s;}
  e.frame(seconds);frames++;changes+=changed.size();return changed;}
};

}  // namespace migoto
