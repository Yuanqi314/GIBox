#include "core.hpp"
#include "json.hpp"
#include "model.hpp"
#include "model_fixture.hpp"
#include "runtime.hpp"
#include <cassert>
#include <elf.h>
#include <iostream>
#include <limits>
#include <unistd.h>
using namespace ms;
using fixture::Spec;using fixture::replaceIn;
template<class F>void rejects(F f){bool rejected=false;try{f();}catch(const std::exception&){rejected=true;}assert(rejected);}
// The model folder is refused, and the message says why.
template<class F>void refused(const std::string& dir,F spec,const char* why){Spec s;spec(s);fixture::writeModel(dir,s);std::string message;
 try{loadModelFile(dir);}catch(const std::exception& ex){message=ex.what();}
 if(message.find(why)==std::string::npos){std::cerr<<"expected \""<<why<<"\", got \""<<message<<"\"\n";assert(false);}}
static void floatAt(Bytes& b,size_t p,float f){std::memcpy(b.data()+p,&f,4);}
// A version 2 model: an always-drawn draw plus one drawn only for $halo=3 with $legs=1; the skeleton carries parents.
Spec v2(){Spec s;s.version=2;s.variants="{\"vars\":[{\"name\":\"$halo\",\"values\":[0,1,3]},{\"name\":\"$legs\",\"values\":[0,1]}],\"default\":{\"$halo\":0,\"$legs\":1}}";
 s.draws={"[{\"start\":0,\"count\":3,\"material\":\"Material0\",\"slot\":0},{\"start\":0,\"count\":3,\"material\":\"Material0\",\"slot\":1,\"when\":{\"vars\":[\"$halo\",\"$legs\"],\"allow\":[[3,1]]}}]"};
 s.edit=[](std::string& j){replaceIn(j,"\"bones\":[\"BoneA\"]","\"bones\":[\"BoneA\"],\"parents\":[-1]");};return s;}
int main(int argc,char**argv){
 // Converter output check (called by the Java tests): the folder must load exactly as the runtime will load it.
 if(argc==2){std::string p=argv[1];auto m=loadModelFile(p);auto s=readSummary(p);assert(m->parts.size()==s.parts.size()&&!m->parts.empty()&&m->variants.combos.empty());
  for(auto& part:m->parts){assert(!part.draws.empty());for(auto& d:part.draws)assert(m->materials.count(d.row.name));std::vector<Submesh> subs;std::vector<Material> rows;activeDraws(part.draws,m->variants.defaults,subs,rows);assert(!subs.empty());}
  if(exists(p+"/learned.json"))learnedOf(JsonParser(text(p+"/learned.json")).parse());
  return 0;}
 char temp[]="/tmp/gibox-core-XXXXXX";assert(mkdtemp(temp));std::string root=temp;
 // ---- the GIBox model format: a good folder loads with everything in place
 {auto dir=root+"/good";Spec s;s.textures=true;s.withTarget(fixture::target("Body"));s.variants="{\"vars\":[{\"name\":\"$swap\",\"values\":[0,1]}],\"combos\":[{\"$swap\":0},{\"$swap\":1}],\"default\":1}";
  s.draws={"[{\"start\":0,\"count\":3,\"material\":\"Material0\",\"slot\":0}]","[{\"start\":0,\"count\":0,\"material\":\"Material0\",\"slot\":2}]"};fixture::writeModel(dir,s);
  auto m=loadModelFile(dir);auto& p=m->parts.at(0);
  assert(m->id==10000107&&m->generation=="1"&&p.spec.name=="Part0"&&p.spec.group=="Body"&&p.spec.bones==1&&p.geo.n==3&&p.geo.ni==3&&u32(p.geo.idx()+8)==2);
  assert(p.used==std::set<unsigned>{0}&&p.sourceBones==std::vector<std::string>{"BoneA"}&&p.sourceBinds.size()==64&&f32(p.sourceBinds.data())==1);
  // Version 1 combinations read as draws that know their combinations; an unlisted assignment is the default one.
  {std::vector<Submesh> subs;std::vector<Material> rows;assert(m->variants.combos.size()==2&&m->variants.defaults==(Assignment{{"$swap",1}})&&p.draws.size()==2&&pick(m->variants,{})==(Assignment{{"$swap",1}}));
   activeDraws(p.draws,pick(m->variants,{}),subs,rows);assert(subs.size()==1&&subs[0].count==0&&rows[0].slot==2);
   activeDraws(p.draws,pick(m->variants,Assignment{{"$swap",0}}),subs,rows);assert(subs.size()==1&&subs[0].count==3&&rows[0].slot==0&&pick(m->variants,Assignment{{"$swap",7}})==(Assignment{{"$swap",1}}));}
  auto& mat=m->materials.at("Material0");assert(!mat.original&&mat.main&&mat.main->w==1&&mat.light&&!mat.normal);
  auto sum=readSummary(dir);assert(sum.parts.size()==1&&sum.parts[0].hasTarget&&sum.parts[0].target.mesh=="Body"&&sum.parts[0].renderer=="Body");
  // diffuseOnly drops the extra maps; a model by name needs names in the right shape
  {auto j=text(dir+"/model.json");replaceIn(j,"\"blob\"","\"flags\":{\"diffuseOnly\":true},\"blob\"");write(dir+"/model.json",j);assert(!loadModelFile(dir)->materials.at("Material0").light);}
  {Spec n;n.character(0,{"citlali"}).noSkeleton();fixture::writeModel(root+"/named",n);auto mn=loadModelFile(root+"/named");assert(mn->id==0&&mn->names==std::vector<std::string>{"citlali"}&&mn->parts[0].sourceBones.empty());}}
 // ---- the checks: every one names what is wrong
 auto bad=root+"/bad";
 refused(bad,[](Spec& s){s.edit=[](std::string& j){replaceIn(j,"gibox-model","other-model");};},"not a GIBox model");
 refused(bad,[](Spec& s){s.edit=[](std::string& j){replaceIn(j,"\"version\":1","\"version\":3");};},"unsupported version");
 refused(bad,[](Spec& s){s.character(9999);},"Character ID out of range");
 refused(bad,[](Spec& s){s.character(0);},"needs a character ID or names");
 refused(bad,[](Spec& s){s.character(0,{"Citlali"});},"Character name");
 refused(bad,[](Spec& s){s.edit=[](std::string& j){auto p=j.find("\"crc32\":")+8;auto e=j.find('}',p);auto crc=std::stoull(j.substr(p,e-p));j.replace(p,e-p,std::to_string(crc^1));};},"checksum mismatch");
 refused(bad,[](Spec& s){s.edit=[](std::string& j){replaceIn(j,"\"size\":304","\"size\":320");};},"size differs");
 refused(bad,[](Spec& s){s.edit=[](std::string& j){replaceIn(j,"\"uv\":[96,24]","\"uv\":[100,24]");};},"misaligned");
 refused(bad,[](Spec& s){s.edit=[](std::string& j){replaceIn(j,"\"uv\":[96,24]","\"uv\":[96,16]");};},"wrong length");
 refused(bad,[](Spec& s){s.edit=[](std::string& j){replaceIn(j,"\"bindposes\":[240,64]","\"bindposes\":[288,64]");};},"outside model.bin");
 refused(bad,[](Spec& s){s.indices={0,1,3};},"vertex index out of range");
 refused(bad,[](Spec& s){s.indices={0,1,2,0};},"multiple of 3");
 refused(bad,[](Spec& s){s.positions[4]=std::numeric_limits<float>::quiet_NaN();},"not a finite number");
 refused(bad,[](Spec& s){floatAt(s.weights,0,.5f);},"do not sum to 1");
 refused(bad,[](Spec& s){floatAt(s.weights,0,-1);floatAt(s.weights,4,2);},"invalid weight");
 refused(bad,[](Spec& s){put32(s.weights.data()+16,1);},"bone index out of range");
 refused(bad,[](Spec& s){s.draws={"[{\"start\":0,\"count\":6,\"material\":\"Material0\",\"slot\":0}]"};},"out of range");
 refused(bad,[](Spec& s){s.draws={"[{\"start\":0,\"count\":3,\"material\":\"Nope\",\"slot\":0}]"};},"unknown material Nope");
 refused(bad,[](Spec& s){s.draws={"[{\"start\":0,\"count\":3,\"material\":\"Material0\",\"slot\":64}]"};},"\"slot\" out of range");
 refused(bad,[](Spec& s){s.draws.push_back(s.draws[0]);},"Invalid \"draws\" list");
 refused(bad,[](Spec& s){s.variants="{\"vars\":[{\"name\":\"$a\",\"values\":[0,1]}],\"combos\":[{}],\"default\":0}";},"must set every variable");
 refused(bad,[](Spec& s){s.variants="{\"vars\":[],\"combos\":[{\"$b\":1}],\"default\":0}";},"Variant undeclared: $b");
 refused(bad,[](Spec& s){s.variants="{\"vars\":[],\"combos\":[{}],\"default\":3}";},"\"default\" out of range");
 // ---- version 2: each draw says when it is drawn; no combination list, no limit of 32
 {Spec s=v2();fixture::writeModel(root+"/v2",s);auto m=loadModelFile(root+"/v2");auto& p=m->parts.at(0);std::vector<Submesh> subs;std::vector<Material> rows;
  assert(m->variants.combos.empty()&&m->variants.defaults==(Assignment{{"$halo",0},{"$legs",1}})&&p.draws.size()==2&&p.draws[0].when.vars.empty()&&p.sourceParents==std::vector<int>{-1});
  activeDraws(p.draws,pick(m->variants,{}),subs,rows);assert(subs.size()==1&&subs[0].count==3);
  activeDraws(p.draws,pick(m->variants,Assignment{{"$halo",3}}),subs,rows);assert(subs.size()==2&&rows[1].slot==1);
  activeDraws(p.draws,pick(m->variants,Assignment{{"$halo",3},{"$legs",0}}),subs,rows);assert(subs.size()==1);
  assert(pick(m->variants,Assignment{{"$halo",5},{"$nope",1}})==m->variants.defaults);}
 refused(bad,[](Spec& s){s=v2();replaceIn(s.draws[0],"[\"$halo\",\"$legs\"]","[\"$halo\",\"$wings\"]");},"undeclared variable");
 refused(bad,[](Spec& s){s=v2();replaceIn(s.draws[0],"[[3,1]]","[[4,1]]");},"when value not listed for $halo");
 refused(bad,[](Spec& s){s=v2();replaceIn(s.draws[0],"[[3,1]]","[[3]]");},"when tuple size");
 refused(bad,[](Spec& s){s=v2();replaceIn(s.variants,",\"$legs\":1}","}");},"Variant default must set every variable");
 refused(bad,[](Spec& s){s=v2();s.draws[0]="[]";},"Invalid \"draws\" list");
 refused(bad,[](Spec& s){s=v2();s.edit=[](std::string& j){replaceIn(j,"\"bones\":[\"BoneA\"]","\"bones\":[\"BoneA\"],\"parents\":[0]");};},"bone parent out of range");
 refused(bad,[](Spec& s){s.edit=[](std::string& j){replaceIn(j,"\"name\":\"Part0\"","\"name\":\"../x\"");};},"Part name");
 refused(bad,[](Spec& s){s.edit=[](std::string& j){replaceIn(j,"\"bones\":[\"BoneA\"]","\"bones\":[\"BoneA\",\"BoneB\"]");};},"Invalid \"bones\" list");
 refused(bad,[](Spec& s){s.withTarget("{\"renderer\":\"Body\",\"vertices\":0,\"submeshes\":1,\"bones\":1}");},"\"vertices\" out of range");
 {Spec s;fixture::writeModel(bad,s);write(bad+"/model.bin","short");rejects([&]{loadModelFile(bad);});std::filesystem::remove(bad+"/model.bin");rejects([&]{loadModelFile(bad);});}
 // ---- learned.json and the skeleton library round-trip through the native writers
 {Learned l;l.id=10000107;Fingerprint fp{"Body","Avatar_Body",23521,3,142};l.targets["Part0"]=fp;auto back=learnedOf(JsonParser(learnedJson(l)).parse());
  assert(back.id==10000107&&back.targets.at("Part0").vertices==23521&&back.targets.at("Part0").mesh=="Avatar_Body"&&matches(back.targets.at("Part0"),fp));
  rejects([]{learnedOf(JsonParser("{\"format\":\"gibox-learned\",\"targets\":{\"../x\":{}}}").parse());});rejects([]{learnedOf(JsonParser("{\"format\":\"x\"}").parse());});
  write(root+"/learned.json","{broken");assert(readLearned(root).targets.empty());}
 {SkeletonRenderer r;r.name="Body";r.target={"Body","Avatar_Body",3,1,2};r.bones={"Root","Spine\"1"};r.parents={-1,0};r.bindposes=Bytes(128);for(int b=0;b<2;b++)for(int i=0;i<4;i++)floatAt(r.bindposes,b*64+i*20,1);floatAt(r.bindposes,0,1.f/3);
  auto j=JsonParser(skeletonJson(10000107,"Scene/Avatar_Girl_Catalyst_Citlali",{r})).parse();auto& x=j.at("renderers").array.at(0);
  assert(j.at("format").string=="gibox-skeleton"&&j.at("id").number==10000107&&j.at("root").string.find("Citlali")!=std::string::npos&&x.at("bones").array[1].string=="Spine\"1"&&x.at("parents").array[0].number==-1);
  assert(float(x.at("bindposes").array[0].number)==1.f/3&&x.at("bindposes").array.size()==32&&fingerprintOf(x.at("target")).bones==2);
  auto good=r;r.parents={1,0};rejects([&]{skeletonJson(10000107,"",{r});});
  r=good;floatAt(r.bindposes,0,std::numeric_limits<float>::quiet_NaN());rejects([&]{skeletonJson(10000107,"",{r});});
  r=good;r.bindposes=Bytes(128);rejects([&]{skeletonJson(10000107,"",{r});});
  r=good;r.target.bones=3;rejects([&]{skeletonJson(10000107,"",{r});});
  r=good;r.parents.pop_back();rejects([&]{skeletonJson(10000107,"",{r});});}
 assert(crc32(reinterpret_cast<const uint8_t*>("123456789"),9)==0xcbf43926u);
 // ---- shared building blocks
 assert(config(" 10000107\n")==10000107);rejects([]{config("10000107 evil");});
 Bytes bind(64);for(int i=0;i<4;i++)floatAt(bind,i*20,1);auto mapping=mapBones({"Bone_A"},{"bonea"},bind,bind,{0}).target;assert(mapping[0]==0);rejects([&]{mapBones({"Bone_A"},{"bonea","Bone-A"},bind,Bytes(128),{0});});auto changed=bind;floatAt(changed,0,2);rejects([&]{mapBones({"Bone_A"},{"bonea"},bind,changed,{0});});
 // Bone matching in tiers: an exact name wins over a looser one; same names are told apart by their ancestors;
 // a helper bone the target lacks hands its weight to its nearest matching ancestor; a body bone never does.
 {auto binds=[&](size_t n){Bytes b;for(size_t i=0;i<n;i++)b.insert(b.end(),bind.begin(),bind.end());return b;};auto why=[&](auto f){std::string m;try{f();}catch(const std::exception& e){m=e.what();}return m;};
  assert(mapBones({"Bone A"},{"bone_a","Bone A"},bind,binds(2),{0}).target[0]==1);
  std::vector<std::string> src{"Root","L","Tip","R","Tip"},dst{"Root","R","Tip","L","Tip"};std::vector<int> sp{-1,0,1,0,3},dp{-1,0,1,0,3};
  auto m=mapBones(src,dst,binds(5),binds(5),{2,4},sp,dp);assert(m.target[2]==4&&m.target[4]==2&&m.moved.empty());
  assert(why([&]{mapBones(src,dst,binds(5),binds(5),{2});}).find("Bone name ambiguous: Tip")==0);
  std::vector<std::string> hs{"Bip001 Head","+HairBack01","+HairBack02"};std::vector<int> hp{-1,0,1};
  auto h=mapBones(hs,{"Bip001 Head"},binds(3),bind,{0,2},hp,{-1});assert(h.target[2]==0&&h.moved.size()==1&&h.moved[0].first==2);
  assert(why([&]{mapBones({"Bip001 Head","Bip001 L Hand"},{"Bip001 Head"},binds(2),bind,{1},{-1,0},{-1});}).find("Bone name missing: Bip001 L Hand")==0);
  assert(why([&]{mapBones(hs,{"Bip001 Head"},binds(3),bind,{2});}).find("Bone name missing: +HairBack02")==0);}
 // Weights after mapping: two source bones that became one target bone are added together; largest first; sum 1.
 {Skin s{Bytes(32),{0,1,2}};floatAt(s.data,0,.2f);floatAt(s.data,4,.5f);floatAt(s.data,8,.3f);put32(s.data.data()+16,0);put32(s.data.data()+20,1);put32(s.data.data()+24,2);
  remap(s,{3,5,3});assert(f32(s.data.data())==.5f&&u32(s.data.data()+16)==3&&f32(s.data.data()+4)==.5f&&u32(s.data.data()+20)==5&&f32(s.data.data()+8)==0&&f32(s.data.data()+12)==0);}
 {Skin s{Bytes(32),{0}};floatAt(s.data,0,1);remap(s,mapping);assert(u32(s.data.data()+16)==0);put32(s.data.data()+16,5);rejects([&]{remap(s,mapping);});}
 auto j=JsonParser("{\"enabled\":true,\"selected\":[\"\\u6a21\\u578b\",\"\\ud83d\\ude00\"],\"cycle\":2}").parse();assert(j.at("selected").array[0].string=="模型");assert(j.at("cycle").number==2);rejects([]{JsonParser("{\"a\":1,\"a\":2}").parse();});rejects([]{JsonParser("[01]").parse();});rejects([]{JsonParser("\"\\ud800\"").parse();});
 Bytes note(32);put32(note.data(),4);put32(note.data()+4,16);put32(note.data()+8,3);put32(note.data()+12,0x00554e47);for(int i=0;i<16;i++)note[16+i]=i;Elf64_Phdr ph{};ph.p_type=PT_NOTE;ph.p_memsz=note.size();auto id=loadedBuildId(reinterpret_cast<Ptr>(note.data()),&ph,1,sizeof(ph));assert(id=="000102030405060708090a0b0c0d0e0f"&&id!=abi::buildId);ph.p_memsz=16;rejects([&]{loadedBuildId(reinterpret_cast<Ptr>(note.data()),&ph,1,sizeof(ph));});
 Bytes dds(136);put32(dds.data(),0x20534444);put32(dds.data()+4,124);put32(dds.data()+12,3);put32(dds.data()+16,3);put32(dds.data()+76,32);put32(dds.data()+80,4);put32(dds.data()+84,0x31545844);dds[128]=0;dds[129]=0xf8;auto tex=decodeDds(dds);assert(tex.w==3&&tex.h==3&&tex.pixels.size()==36&&tex.pixels[0]==255&&tex.pixels[3]==255);dds.pop_back();rejects([&]{decodeDds(dds);});
 Fingerprint t{"Body","Avatar_Body_LOD1",23521,3,142},live{"Body","avatar_body (Clone)",23521,3,142};assert(matches(t,live)&&fingerprintOf(JsonParser(fingerprintJson(t)).parse()).mesh==t.mesh);
 assert(lowerLod("Body_LOD3")&&lowerLod("body_lod1 (Clone)")&&lowerLod("Body_LOD12 Instance")&&!lowerLod("Body")&&!lowerLod("Body_LOD0")&&!lowerLod("Body_LOD")&&!lowerLod("Body_LODx")&&!lowerLod("Lod1_Body"));
 live.mesh="avatar_body Instance";assert(matches(t,live));live.vertices++;assert(!matches(t,live));live.vertices--;live.mesh="Avatar_Face";assert(!matches(t,live));
 std::filesystem::remove_all(root);
 std::cout<<"Native core: PASS (GIBox model format: manifest, model.bin views, CRC-32, value/range/reference checks, variants, learned.json, skeleton.json; bones, JSON, ELF notes, DDS, targets)\n";
}
