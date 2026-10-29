// Texture-only mods, phase 1: the App's diagnostic document as the game reads it, and how its rules are matched
// against renderers (no engine: the renderers are written out as the Runtime would read them).
#include "texture_diag.hpp"
#include <cassert>
#include <cstdlib>
#include <iostream>
using ms::Ptr;
using namespace ms::texdiag;
namespace {
const char* doc=R"J({"format":"gibox-texture-diag","version":1,"name":"Mod","ini":"Mod.ini","character":10000107,"characterName":"x","rules":[
 {"section":"TextureOverrideModBangsPosition","part":"ModBangsPosition","hint":"bangsposition","slotLetter":-1,"hash":"0e33fe05","first":-1,"priority":1,"kind":"control","textures":[]},
 {"section":"TextureOverrideModBodyA","part":"ModBodyA","hint":"body","slotLetter":0,"hash":"d5ae71a3","first":0,"kind":"segment","textures":[
  {"slot":"resource\\gimi\\diffuse","resource":"rbody1","role":"diffuse","condition":"$colour == 0","via":""},{"slot":"resource\\gimi\\diffuse","resource":"rbody2","role":"diffuse","condition":"$colour == 1","via":""},
  {"slot":"ps-t3","resource":"rramp","role":"ramp","condition":"","via":""},{"slot":"ps-t7","resource":"rodd","role":"unknown","condition":"","via":""}]},
 {"section":"TextureOverrideModBodyB","part":"ModBodyB","hint":"body","slotLetter":1,"hash":"d5ae71a3","first":25002,"kind":"segment","textures":[{"slot":"resource\\gimi\\diffuse","resource":"rbody1","role":"diffuse","condition":"","via":""}]},
 {"section":"TextureOverrideModBodyAll","part":"ModBodyAll","hint":"bodyall","hash":"d5ae71a3","first":-1,"kind":"whole-ib","textures":[{"slot":"ps-t0","resource":"rbody1","role":"diffuse","condition":"","via":""}]},
 {"section":"TextureOverrideModDressA","part":"ModDressA","hint":"dress","slotLetter":0,"hash":"ede196e7","first":0,"kind":"segment","textures":[{"slot":"resource\\gimi\\diffuse","resource":"rdressa","role":"diffuse","condition":"","via":""}]},
 {"section":"TextureOverrideModDressB","part":"ModDressB","hint":"dress","slotLetter":1,"hash":"ede196e7","first":10764,"kind":"segment","textures":[{"slot":"resource\\gimi\\diffuse","resource":"rdressa","role":"diffuse","condition":"","via":""}]},
 {"section":"TextureOverrideModDressC","part":"ModDressC","hint":"dress","slotLetter":2,"hash":"ede196e7","first":22887,"kind":"segment","textures":[{"slot":"resource\\gimi\\diffuse","resource":"rdressc","role":"diffuse","condition":"","via":""}]},
 {"section":"TextureOverrideModTailA","part":"ModTailA","hint":"tail","slotLetter":0,"hash":"335e2828","first":0,"kind":"segment","textures":[{"slot":"resource\\gimi\\diffuse","resource":"rdressa","role":"diffuse","condition":"","via":""}]},
 {"section":"TextureOverrideModHairA","part":"ModHairA","hint":"hair","slotLetter":0,"hash":"64d28bf2","first":0,"kind":"segment","textures":[{"slot":"resource\\gimi\\diffuse","resource":"rdressa","role":"diffuse","condition":"","via":""}]},
 {"section":"TextureOverrideModHairB","part":"ModHairB","hint":"hair","slotLetter":1,"hash":"64d28bf2","first":900,"kind":"segment","textures":[{"slot":"resource\\gimi\\diffuse","resource":"rdressa","role":"diffuse","condition":"","via":""}]},
 {"section":"TextureOverrideModFaceEyesADiffuse","part":"ModFaceEyesADiffuse","hint":"faceeyesadiffuse","hash":"60512e64","first":-1,"kind":"hash-unknown","textureName":true,"textures":[{"slot":"resource\\gimi\\diffuse","resource":"reyes","role":"diffuse","condition":"","via":""}]}],
 "resources":[{"name":"rbody1","file":"b1.dds","exists":true,"kind":"dds","width":2048,"height":2048,"format":"DXGI 98 (BC7_UNORM)","mips":12},{"name":"reyes","file":"e.png","exists":true,"kind":"png","width":1024,"height":1024,"format":"PNG","mips":1}],
 "variables":[{"name":"$colour","initial":0,"values":[0,1],"persist":true,"keys":["KeySwap"]}],"otherInis":[{"ini":"DISABLED/Other.ini","sharedHashes":["d5ae71a3"]}],"notes":[]})J";
Texture tex(const char* prop,const char* name,const char* w,const char* h){Texture t;t.property=prop;t.texture=0x1000;t.name=name;t.width=w;t.height=h;t.format="4 (RGBA32)";return t;}
Renderer renderer(const char* name,Ptr native,std::vector<std::pair<uint32_t,uint32_t>> subs,unsigned materials,bool readable=true){Renderer r;r.name=name;r.native=native;r.renderer=native+1;r.meshName=std::string(name)+"Mesh";
 r.submeshes=unsigned(subs.size());r.layout.submeshes=int(subs.size());r.layout.submeshCount=std::to_string(subs.size());r.layout.status=readable?"read":"未采集: missing export il2cpp_runtime_invoke";
 if(readable)for(auto& s:subs){ms::renderprobe::UnityReader::Submesh x;x.known=true;x.start=s.first;x.count=s.second;x.base="0";x.topology="0 (Triangles)";r.layout.subs.push_back(x);}
 for(unsigned i=0;i<materials;i++){Material m;m.material=native*16+i;m.name=std::string(name)+"Mat"+std::to_string(i);m.shader="miHoYo/Character";m.textures.push_back(tex("_MainTex","Tex","2048","2048"));m.textures.push_back(tex("_LightMapTex","Light","2048","2048"));r.materials.push_back(m);}
 return r;}
const Match& of(const std::vector<Match>& all,const Doc& d,const char* section){for(auto& m:all)if(d.rules[m.rule].section==section)return m;assert(false);return all[0];}
bool has(const std::string& s,const char* x){if(s.find(x)!=std::string::npos)return true;std::cerr<<"missing \""<<x<<"\" in: "<<s<<"\n";return false;}
}
int main(){
 Doc d=parse(doc,"/x/Mod.json");assert(d.rules.size()==11&&d.character==10000107&&d.resources.at("rbody1").width==2048&&d.variables.size()==1&&d.variables[0]=="$colour=0 of [0,1] persist"&&d.others[0]=="DISABLED/Other.ini shares d5ae71a3");
 {bool bad=false;try{parse(R"({"format":"gibox-texture-diag","version":2,"rules":[]})","v2");}catch(const std::exception& e){bad=std::string(e.what()).find("version 2")!=std::string::npos;}assert(bad);}
 // Body: both of its hash's starts are submesh starts, as many materials as submeshes. Dress: 22887 lies inside its
 // second submesh (PC draws merged) with other textures. Hair: two submeshes but a third material. Face: layout unreadable.
 std::vector<Renderer> rs{renderer("Body",0x10,{{0,25002},{25002,3000}},2),renderer("Dress",0x20,{{0,10764},{10764,20000}},2),renderer("Hair",0x30,{{0,900},{900,300}},3),renderer("Face",0x40,{{0,600}},1,false)};
 rs[3].materials[0].textures[0]=tex("_MainTex","Avatar_Face_Tex","1024","1024");
 auto all=match(d,rs);assert(all.size()==d.rules.size());
 auto& bodyA=of(all,d,"TextureOverrideModBodyA");assert(bodyA.verdict==Verdict::Confirmed&&bodyA.renderer==0&&bodyA.slot==0);
 auto lineA=matchLine(d,bodyA,rs);assert(has(lineA,"确定匹配")&&has(lineA,"only Body#0x10 has all 2")&&has(lineA,"index count 25002 = distance to the next match_first_index")&&has(lineA,"-> _MainTex (material has it")&&has(lineA,"ps-t3 rramp [ramp] -> 待诊断 (no candidate property on this material)")&&has(lineA,"ps-t7 rodd [unknown] -> 待诊断 (slot role unknown)")&&has(lineA,"if $colour == 1"));
 assert(of(all,d,"TextureOverrideModBodyB").verdict==Verdict::Confirmed&&of(all,d,"TextureOverrideModBodyB").slot==1);
 auto& whole=of(all,d,"TextureOverrideModBodyAll");assert(whole.verdict==Verdict::Likely&&whole.renderer==0&&whole.slot==-1);
 auto& dressA=of(all,d,"TextureOverrideModDressA");assert(dressA.verdict==Verdict::Likely&&dressA.renderer==1&&has(matchLine(d,dressA,rs),"2 of the hash's 3"));
 auto& dressB=of(all,d,"TextureOverrideModDressB");assert(dressB.verdict==Verdict::Ambiguous&&has(matchLine(d,dressB,rs),"PC segment 22887 lies inside this submesh too")&&has(matchLine(d,dressB,rs),"different textures"));
 auto& dressC=of(all,d,"TextureOverrideModDressC");assert(dressC.verdict==Verdict::Ambiguous&&dressC.renderer==1&&dressC.submesh==1&&has(matchLine(d,dressC,rs),"most likely merged into its submesh 1")&&has(matchLine(d,dressC,rs),"lies inside an Android submesh"));
 // A single segment at index 0 is a submesh start everywhere: no name, no decision.
 auto& tail=of(all,d,"TextureOverrideModTailA");assert(tail.verdict==Verdict::Ambiguous&&tail.candidates.size()==3);
 auto& hair=of(all,d,"TextureOverrideModHairA");assert(hair.verdict==Verdict::Likely&&hair.renderer==2&&has(matchLine(d,hair,rs),"3 materials, 2 submeshes"));
 auto& eyes=of(all,d,"TextureOverrideModFaceEyesADiffuse");assert(eyes.verdict==Verdict::Ambiguous&&has(matchLine(d,eyes,rs),"Face#0x40 slot 0 _MainTex=Avatar_Face_Tex (same size 1024x1024)")&&has(matchLine(d,eyes,rs),"cannot be recomputed"));
 assert(of(all,d,"TextureOverrideModBangsPosition").verdict==Verdict::Skipped);
 // Without any readable layout: name + counts + order make a rule likely at most; never confirmed.
 std::vector<Renderer> blind{renderer("Body",0x10,{{0,25002},{25002,3000}},2,false),renderer("Dress",0x20,{{0,1},{1,1}},2,false)};
 auto b=match(d,blind);for(auto& m:b)assert(m.verdict!=Verdict::Confirmed);
 auto& bb=of(b,d,"TextureOverrideModBodyB");assert(bb.verdict==Verdict::Likely&&bb.slot==1&&has(matchLine(d,bb,blind),"by order only"));
 assert(of(b,d,"TextureOverrideModDressA").verdict==Verdict::Ambiguous);   // 3 segments, 2 submeshes
 // Two rules on one slot are not both confirmed.
 Doc twice=d;twice.rules[2].first=0;twice.rules[2].section="TextureOverrideModBodyA2";auto t=match(twice,rs);assert(of(t,twice,"TextureOverrideModBodyA").verdict!=Verdict::Confirmed&&has(matchLine(twice,of(t,twice,"TextureOverrideModBodyA"),rs),"rules on the same slot"));
 // The lines the Runtime writes.
 assert(has(rendererLine(rs[3],10000107),"layout=未采集: missing export il2cpp_runtime_invoke")&&has(submeshLines(rs[0])[1],"submesh=1 indexStart=25002 indexCount=3000 baseVertex=0 topology=0 (Triangles) material-slot-by-position=1 (BodyMat1)"));
 assert(has(materialLines(rs[0])[0],"slot=0 material=BodyMat0#0x100 shader=miHoYo/Character textures=[_MainTex=Tex 2048x2048 4 (RGBA32)"));
 assert(has(ruleLine(d,d.rules[1]),"kind=segment part=body letter=A textures=[resource\\gimi\\diffuse=rbody1[diffuse] if $colour == 0"));
 if(std::getenv("TEXDIAG_PRINT"))for(auto& m:all)std::cout<<matchLine(d,m,rs)<<"\n";
 std::cout<<"Texture-only diagnostics: PASS (document, index-start / group / name / count / order evidence, merged PC segments, unreadable layouts never confirmed, texture-hash candidates by size, shared slots)\n";
}
