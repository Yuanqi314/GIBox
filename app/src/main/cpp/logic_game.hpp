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

namespace ms {

class LogicDriver {
public:
 struct Stats {uint64_t presents=0,gaps=0,misses=0,changes=0,stepUs=0,maxStepUs=0;};
 explicit LogicDriver(const LogicSpec& s):spec(s){
  std::vector<migoto::IniFile> files;for(auto& f:spec.files)files.push_back({f.ns,f.text});
  engine.load(files);engine.start();
  for(auto& r:engine.resources){std::string n=r.second.shortName;for(auto& c:n)if(c>='A'&&c<='Z')c=char(c-'A'+'a');names[r.first]=n;}
  for(auto& p:spec.passes){migoto::Session::Pass x;x.name=p.name;for(auto& b:p.bindings)x.draw.bindings[b.first]=migoto::Ref::game(b.second);x.draw.checks=p.checks;x.draw.call=p.call;passes.push_back(std::move(x));}
  draws();}
 // The prebuilt frame the current selection matches; -1 when it matches none.
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
 // What a draw uses now, in the converter's signature form.
 std::string signature(const std::string& draw)const{std::string out;auto it=spec.reads.find(draw);if(it==spec.reads.end())return out;
  for(auto& f:it->second){auto sel=current.find(f.second.first);std::string v="-";
   if(sel!=current.end()){const auto* b=&sel->second.bindings;if(!sel->second.draws.empty())b=&sel->second.draws.front().bindings;auto x=b->find(f.second.second);if(x!=b->end())v=name(x->second);}
   out+=f.first+"="+v+";";}
  return out;}
 std::string miss;   // the selection of the last miss, for the log
 Stats stats;
 const migoto::Diagnostics& diagnostics()const{return engine.diag;}
 const migoto::Engine& core()const{return engine;}
 std::string summary()const{size_t lim=engine.diag.count(migoto::Diagnostic::Limitation),warn=engine.diag.count(migoto::Diagnostic::Warning);
  return std::to_string(spec.files.size())+" INI, "+std::to_string(engine.globals.size())+" globals, "+std::to_string(passes.size())+" draws per frame, "+std::to_string(spec.frames.size())+" prebuilt frames; "
   +std::to_string(warn)+" warnings, "+std::to_string(lim)+" limitations";}
private:
 using Selection=migoto::DrawResult;
 const LogicSpec& spec;migoto::Engine engine;std::vector<migoto::Session::Pass> passes;
 std::map<std::string,Selection> current;std::map<std::string,std::string> names;int matched=-1;bool clock=false;uint32_t last=0;
 // A [Resource] by its section name, lowercase, as the converter records it; other refs as migoto::describe has them.
 std::string name(const migoto::Ref& r)const{if(r.kind==migoto::Ref::Mod){auto it=names.find(r.resource);if(it!=names.end())return it->second;}return migoto::describe(engine,r);}
 void draws(){
  for(auto& p:passes)current[p.name]=engine.draw(p.draw);
  // The frame shown now stays while it still matches (identical frames are common); else the first that matches.
  std::map<std::string,std::string> now;for(auto& d:spec.reads)now[d.first]=signature(d.first);
  auto fits=[&](size_t f){for(auto& d:spec.frames[f])if(now[d.first]!=d.second)return false;return true;};
  if(matched>=0&&fits(size_t(matched)))return;
  for(size_t f=0;f<spec.frames.size();f++)if(fits(f)){matched=int(f);return;}
  stats.misses++;miss.clear();for(auto& d:now)miss+=d.first+": "+d.second+" ";}
};

}  // namespace ms
