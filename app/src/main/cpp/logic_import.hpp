// SPDX-License-Identifier: GPL-3.0-only
// The converter's door to the logic core (migoto/rehearsal.hpp): a request with the mod's INI files, a reply with the
// frames its Present program shows and the order it shows them in. One JSON string each way, so the App (through
// NativeBridge.rehearse) and the host tests (core_test --rehearse) call the same code.
//
// Request: {"main": namespace of the main INI, "files": [{"namespace", "text"}], "limits": {"presents", "states",
// "frames", "ms"} (each optional)}. Reply: {"ok": true, "frames": [{"$var": value}], "sequence": [frame per Present
// from the start], "loop": index the sequence repeats from, "states", "presents", "keyFrames", "keys", "phases":
// [{"var", "modulus"}], "notes", "us"} or {"ok": false, "reason"}.
#pragma once
#include "json.hpp"
#include "migoto/rehearsal.hpp"
#include <cstdio>
#include <string>
#include <vector>

namespace ms {

inline std::string rehearseJson(const std::string& request){
 try{Json j=JsonParser(request).parse();require(j.type==Json::Object,"rehearsal request");const auto& main=j.at("main");require(main.type==Json::String&&!main.string.empty(),"rehearsal main");
  std::vector<migoto::IniFile> files;const auto& list=j.at("files");require(list.type==Json::Array&&!list.array.empty()&&list.array.size()<=64,"rehearsal files");size_t total=0;
  for(auto& f:list.array){require(f.type==Json::Object&&f.at("namespace").type==Json::String&&f.at("text").type==Json::String,"rehearsal file");total+=f.at("text").string.size();require(total<=8u*1024*1024,"rehearsal INI files over 8 MiB");files.push_back({f.at("namespace").string,f.at("text").string});}
  migoto::Rehearsal::Limits lim;const auto& l=j.at("limits");
  if(l.type==Json::Object){auto take=[&](const char* k,uint64_t& out,uint64_t hi){const auto& n=l.at(k);if(n.type==Json::Number){require(n.number>=1&&n.number<=double(hi)&&std::floor(n.number)==n.number,std::string("rehearsal limit ")+k);out=uint64_t(n.number);}};
   take("presents",lim.presents,1u<<20);take("states",lim.states,1u<<20);take("frames",lim.frames,256);take("ms",lim.ms,60000);}
  migoto::Engine e;e.load(files);e.start();migoto::Rehearsal r(e,main.string,lim);
  auto number=[](double v){char b[32];std::snprintf(b,sizeof b,"%.9g",v);return std::string(b);};
  if(!r.ok)return "{\"ok\":false,\"reason\":"+quote(r.reason)+",\"us\":"+std::to_string(r.us)+"}";
  std::string out="{\"ok\":true,\"frames\":[";
  for(size_t i=0;i<r.frames.size();i++){out+=i?",{":"{";bool first=true;for(auto& kv:r.frames[i]){out+=(first?"":",")+quote(kv.first)+":"+std::to_string(kv.second);first=false;}out+="}";}
  out+="],\"sequence\":[";for(size_t i=0;i<r.sequence.size();i++)out+=(i?",":"")+std::to_string(r.sequence[i]);
  out+="],\"loop\":"+std::to_string(r.loop)+",\"states\":"+std::to_string(r.states)+",\"presents\":"+std::to_string(r.presents)+",\"keyFrames\":"+std::to_string(r.keyFrames)+",\"keys\":[";
  for(size_t i=0;i<r.keys.size();i++)out+=(i?",":"")+quote(r.keys[i]);
  out+="],\"phases\":[";for(size_t i=0;i<r.phases.size();i++)out+=std::string(i?",":"")+"{\"var\":"+quote(r.phases[i].var)+",\"modulus\":"+number(r.phases[i].modulus)+"}";
  out+="],\"notes\":[";for(size_t i=0;i<r.notes.size();i++)out+=(i?",":"")+quote(r.notes[i]);
  return out+"],\"us\":"+std::to_string(r.us)+"}";}
 catch(const std::exception& ex){return "{\"ok\":false,\"reason\":"+quote(std::string("the INI could not be rehearsed: ")+ex.what())+"}";}}

}  // namespace ms
