// SPDX-License-Identifier: GPL-3.0-only
// The game-side adapter of the 3DMigoto logic core (migoto/): an animation model's INI files run in the game, and
// what its draws select picks the prebuilt frame (experiments.logicCore; the old FrameLoop otherwise, and when this
// fails). No Unity or JNI here: runtime.cpp asks for a frame index and applies it the way it applies FrameLoop's.
//
// Order, as on PC: the draws of a frame, then its Present ([Present] pre, input, transitions, post). Each engine frame
// the game presents is one Present followed by the next frame's draws, so the selection read after a tick is what
// the coming frame draws. The draws replayed are the ones model.json "logic.passes" states (the converter's GIMI
// layout); their order of slot checks is an assumption about the host d3dx.ini, stated there.
#pragma once
#include "migoto/adapters.hpp"
#include "model.hpp"
#include <algorithm>
#include <map>
#include <string>
#include <utility>
#include <vector>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <stdexcept>

namespace ms {

class LogicDriver {
public:
 struct Stats {uint64_t presents=0,gaps=0,misses=0,changes=0,stepUs=0,maxStepUs=0;};
 // Refuses logic data it cannot match by (spec.unsupported); the runtime then keeps FrameLoop and logs why.
 explicit LogicDriver(const LogicSpec& s):spec(s){
  if(!spec.unsupported.empty())throw std::runtime_error(spec.unsupported);
  std::vector<migoto::IniFile> files;for(auto& f:spec.files)files.push_back({f.ns,f.text});
  engine.load(files);engine.start();
  for(auto& r:engine.resources)if(!spec.resources.count(r.first))unknown.push_back(r.second.name);
  for(auto& p:spec.passes){migoto::Session::Pass x;x.name=p.name;for(auto& b:p.bindings)x.draw.bindings[b.first]=migoto::Ref::game(b.second);x.draw.checks=p.checks;x.draw.call=p.call;passes.push_back(std::move(x));}
  draws();}
 // The prebuilt choice (animation frame or outfit) the current selection matches; -1 when it matches none.
 int frame()const{return matched;}
 // engineFrame: the game's frame counter (Time.frameCount); seconds: a monotonic clock. Returns true when the
 // matched frame changed. The first call only starts the clock; a jump of more than 240 frames is not replayed.
 bool tick(uint32_t engineFrame,double seconds){
  if(!clock){clock=true;last=engineFrame;return false;}
  uint32_t elapsed=engineFrame-last;if(!elapsed)return false;last=engineFrame;
  if(elapsed>240){stats.gaps++;return false;}
  auto start=std::chrono::steady_clock::now();int before=matched;
  for(uint32_t n=0;n<elapsed;n++){engine.frame(seconds);stats.presents++;draws();}
  auto us=uint64_t(std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now()-start).count());stats.stepUs+=us;stats.maxStepUs=std::max(stats.maxStepUs,us);
  if(matched!=before)stats.changes++;
  return matched!=before;}
 // A part's selection in the converter's signature form (ModelConverter.logicSignature): "hidden" (skipped, no draw
 // of its own), "original" (the game's draw, not replaced), or its issued draws in the order the core issued them,
 // each with its kind, range and the identity of every field; "original|" first when the game's draw is not skipped.
 std::string signature(const std::string& part)const{auto it=spec.parts.find(part);auto own=current.find(part);if(it==spec.parts.end()||own==current.end())return "?";
  const auto& r=own->second;if(r.draws.empty())return r.skip?"hidden":"original";
  std::string out=r.skip?"":"original|";
  for(auto& d:r.draws){out+="kind="+d.kind+";";
   if(d.kind=="drawindexed"&&d.automatic)out+="count=auto;start=0;base=0;";
   else if(d.kind=="drawindexed"&&d.args.size()==3)out+="count="+number(d.args[0])+";start="+number(d.args[1])+";base="+number(d.args[2])+";";
   else{out+="args=";for(size_t i=0;i<d.args.size();i++)out+=(i?",":"")+number(d.args[i]);out+=d.automatic?";auto":d.fromCaller?";from_caller":"";out+=";";}
   for(auto& f:it->second){const std::map<std::string,migoto::Ref>* b=&d.bindings;
    if(f.pass!=part){auto other=current.find(f.pass);b=nullptr;if(other!=current.end())b=other->second.draws.empty()?&other->second.bindings:&other->second.draws.front().bindings;}
    migoto::Ref ref;if(b){auto x=b->find(f.slot);if(x!=b->end())ref=x->second;}
    out+=f.name+"="+identity(ref)+";";}
   out+="|";}
  return out;}
 // An outside choice (the App's outfit selection, until keys reach the core): the variable as the main INI names it.
 // False when the logic has no such variable. The next tick's draws see it.
 bool set(const std::string& name,int value){std::string low=name;for(auto& c:low)if(c>='A'&&c<='Z')c=char(c-'A'+'a');migoto::Var* v=engine.global(low,spec.mainNs);if(!v)return false;
  if(v->value!=float(value)){v->value=float(value);if(v->flags&migoto::VarPersist)engine.persistDirty=true;}return true;}
 // Draws again now (after set), without a Present; true when the matched choice changed.
 bool reselect(){int before=matched;draws();if(matched!=before)stats.changes++;return matched!=before;}
 // The finite-state analysis of the loaded logic (it sets variables while it runs and puts them back).
 migoto::ImportScan scan(){return migoto::ImportScan(engine);}
 std::string miss;            // why the last miss matched nothing: per differing part, what the closest frame has and what is selected
 std::vector<std::string> unknown;   // [Resource] sections the core has that the converter did not list
 Stats stats;
 const migoto::Diagnostics& diagnostics()const{return engine.diag;}
 const migoto::Engine& core()const{return engine;}
 std::string summary()const{size_t lim=engine.diag.count(migoto::Diagnostic::Limitation),warn=engine.diag.count(migoto::Diagnostic::Warning);
  return std::to_string(spec.files.size())+" INI, "+std::to_string(engine.globals.size())+" globals, "+std::to_string(spec.resources.size())+" resources, "+std::to_string(passes.size())+" draws per frame, "+std::to_string(spec.choices.size())+" prebuilt choices; "
   +std::to_string(warn)+" warnings, "+std::to_string(lim)+" limitations";}
private:
 const LogicSpec& spec;migoto::Engine engine;std::vector<migoto::Session::Pass> passes;
 std::map<std::string,migoto::DrawResult> current;int matched=-1;bool clock=false;uint32_t last=0;
 static std::string number(float v){if(v==float(int64_t(v))&&std::fabs(v)<1e15f)return std::to_string(int64_t(v));char b[32];std::snprintf(b,sizeof b,"%.9g",double(v));return b;}
 // What the converter calls a ref: a [Resource] by its identity (file and layout; aliases of one file are one), a game
 // resource by its hash, "null", or "-" when nothing is bound.
 std::string identity(const migoto::Ref& r)const{switch(r.kind){case migoto::Ref::None:return "-";case migoto::Ref::Null:return "null";
   case migoto::Ref::Game:{char b[16];std::snprintf(b,sizeof b,"game:%08x",r.hash);return b;}
   case migoto::Ref::Mod:{auto it=spec.resources.find(r.resource);return it==spec.resources.end()?"unknown:"+r.resource:it->second;}}
  return "?";}
 void draws(){
  for(auto& p:passes)current[p.name]=engine.draw(p.draw);
  // The frame shown now stays while it still matches (identical frames are common); else the first that matches.
  std::map<std::string,std::string> now;for(auto& d:spec.parts)now[d.first]=signature(d.first);
  auto fits=[&](size_t f){for(auto& d:spec.choices[f])if(now[d.first]!=d.second)return false;return true;};
  if(matched>=0&&fits(size_t(matched)))return;
  for(size_t f=0;f<spec.choices.size();f++)if(fits(f)){matched=int(f);return;}
  // No frame has this selection: name the closest frame (most parts equal) and each part that differs.
  stats.misses++;size_t best=0,bestSame=0;for(size_t f=0;f<spec.choices.size();f++){size_t same=0;for(auto& d:spec.choices[f])same+=now[d.first]==d.second;if(same>bestSame||f==0){best=f;bestSame=same;}}
  miss="closest prebuilt frame index "+std::to_string(best)+" ("+std::to_string(bestSame)+" of "+std::to_string(now.size())+" parts equal)";
  if(!spec.choices.empty())for(auto& d:spec.choices[best])if(now[d.first]!=d.second){auto shorten=[](const std::string& t){return t.size()>400?t.substr(0,400)+"...":t;};miss+="; "+d.first+": prebuilt "+shorten(d.second)+" selected "+shorten(now[d.first]);}}
};

}  // namespace ms
