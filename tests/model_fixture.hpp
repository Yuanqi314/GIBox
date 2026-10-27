#pragma once
// GIBox model folders for the native tests, laid out the way ModelConverter writes them (model.hpp reads them):
// one part "Part0" on renderer Body, 3 vertices, 1 triangle, one bone, material Material0.
#include "model.hpp"
#include <functional>
namespace fixture {
using namespace ms;
inline Bytes floats(const std::vector<float>& v){Bytes b(4*v.size());if(!v.empty())std::memcpy(b.data(),v.data(),b.size());return b;}
inline Bytes words32(const std::vector<uint32_t>& v){Bytes b(4*v.size());for(size_t i=0;i<v.size();i++)put32(b.data()+4*i,v[i]);return b;}
inline std::string target(const std::string& mesh,unsigned vertices=3){return "{\"renderer\":\"Body\",\"mesh\":"+quote(mesh)+",\"vertices\":"+std::to_string(vertices)+",\"submeshes\":1,\"bones\":1}";}
struct Spec {
 unsigned id=10000107;std::vector<std::string> names;std::string generation="1";
 unsigned version=1;           // 2/3: "draws" holds one flat list whose entries may carry "when"
 bool skeleton=true;          // bone names + bindposes; without them the game's own bone order is used
 std::string target;          // Part0's mesh identity (fingerprint JSON); empty: learned in game
 bool textures=false;         // Material0 with a 1x1 main texture and a lightmap; otherwise the original material
 unsigned extraMaterials=0;   // with textures: Material1..N as well, each with its own 1x1 main texture
 bool channels=false;         // colors, tangents and uv2, as the converter writes them
 std::string variants;        // "variants" JSON; empty: none
 std::vector<std::string> draws={"[{\"start\":0,\"count\":3,\"material\":\"Material0\",\"slot\":0}]"};  // one list per combination
 std::vector<float> positions=std::vector<float>(9,0.f);std::vector<uint32_t> indices={0,1,2};
 Bytes weights=[]{Bytes w(96);for(int i=0;i<3;i++){float one=1;std::memcpy(w.data()+32*i,&one,4);}return w;}();
 std::function<void(std::string&)> edit;  // last change to model.json (tests of the checks)
 Spec& gen(const std::string& g){generation=g;return *this;}
 Spec& character(unsigned i,std::vector<std::string> n={}){id=i;names=std::move(n);return *this;}
 Spec& noSkeleton(){skeleton=false;return *this;}
 Spec& withTarget(const std::string& t){target=t;return *this;}
};
inline std::string view(Bytes& blob,const Bytes& data){while(blob.size()%16)blob.push_back(0);auto at=blob.size();blob.insert(blob.end(),data.begin(),data.end());return "["+std::to_string(at)+","+std::to_string(data.size())+"]";}
inline void writeModel(const std::string& dir,const Spec& s=Spec()){
 std::filesystem::create_directories(dir);Bytes blob;
 auto pos=floats(s.positions),uv=floats(std::vector<float>(6,0.f));
 // One statement per view: the order of operands in a + chain is unspecified, the order in model.bin is not.
 auto vp=view(blob,pos);auto vn=view(blob,pos);auto vu=view(blob,uv);auto vi=view(blob,words32(s.indices));auto vw=view(blob,s.weights);
 std::string data="{\"positions\":"+vp+",\"normals\":"+vn+",\"uv\":"+vu+",\"indices\":"+vi+",\"weights\":"+vw;
 if(s.channels){auto vc=view(blob,Bytes(12,255));auto vt=view(blob,floats(std::vector<float>(12,0.f)));auto v2=view(blob,uv);data+=",\"colors\":"+vc+",\"tangents\":"+vt+",\"uv2\":"+v2;}
 data+="}";
 std::string part="{\"name\":\"Part0\",\"renderer\":\"Body\",\"bones\":1,\"vertices\":3,\"indices\":"+std::to_string(s.indices.size())+",\"data\":"+data;
 if(s.skeleton)part+=",\"skeleton\":{\"bones\":[\"BoneA\"],\"bindposes\":"+view(blob,floats({1,0,0,0,0,1,0,0,0,0,1,0,0,0,0,1}))+"}";
 if(!s.target.empty())part+=",\"target\":"+s.target;
 if(s.version>=2)part+=",\"draws\":"+s.draws[0]+"}";else{part+=",\"draws\":[";for(size_t i=0;i<s.draws.size();i++)part+=(i?",":"")+s.draws[i];part+="]}";}
 std::string material="{\"name\":\"Material0\",\"original\":true}";
 if(s.textures){Bytes px(4,255);auto main=view(blob,px);auto light=view(blob,px);material="{\"name\":\"Material0\",\"main\":{\"width\":1,\"height\":1,\"data\":"+main+"},\"lightmap\":{\"width\":1,\"height\":1,\"data\":"+light+"}}";
  for(unsigned i=1;i<=s.extraMaterials;i++){Bytes c(4,uint8_t(i));auto v=view(blob,c);material+=",{\"name\":\"Material"+std::to_string(i)+"\",\"main\":{\"width\":1,\"height\":1,\"data\":"+v+"}}";}}
 std::string names="[";for(size_t i=0;i<s.names.size();i++)names+=(i?",":"")+quote(s.names[i]);names+="]";
 std::string json="{\"format\":\"gibox-model\",\"version\":"+std::to_string(s.version)+",\"name\":"+quote(std::filesystem::path(dir).filename().string())+",\"generation\":"+quote(s.generation)
  +",\"character\":{\"id\":"+std::to_string(s.id)+",\"names\":"+names+"}"+(s.variants.empty()?"":",\"variants\":"+s.variants)
  +",\"blob\":{\"file\":\"model.bin\",\"size\":"+std::to_string(blob.size())+",\"crc32\":"+std::to_string(crc32(blob.data(),blob.size()))+"},\"parts\":["+part+"],\"materials\":["+material+"]}\n";
 if(s.edit)s.edit(json);
 // model.json last and as a new file, like the converter (which swaps in a whole folder): the runtime notices it by inode.
 write(dir+"/model.bin",blob.data(),blob.size());write(dir+"/model.json.tmp",json);std::filesystem::rename(dir+"/model.json.tmp",dir+"/model.json");
}
inline void replaceIn(std::string& s,const std::string& from,const std::string& to){auto p=s.find(from);if(p==std::string::npos)throw std::runtime_error("fixture: no "+from);s.replace(p,from.size(),to);}
inline Learned learnedAt(const std::string& dir){return learnedOf(JsonParser(text(dir+"/learned.json")).parse());}
// Test animation frames use real blob views; frame 1 deliberately reads positions from the normals view.
inline std::string jsonText(const Json& j){switch(j.type){case Json::Null:return "null";case Json::Bool:return j.boolean?"true":"false";case Json::Number:{char number[64];std::snprintf(number,sizeof(number),"%.17g",j.number);return number;}case Json::String:return quote(j.string);case Json::Array:{std::string out="[";for(size_t i=0;i<j.array.size();i++)out+=(i?",":"")+jsonText(j.array[i]);return out+"]";}case Json::Object:{std::string out="{";bool first=true;for(auto& p:j.object){out+=(first?"":",")+quote(p.first)+":"+jsonText(p.second);first=false;}return out+"}";}}throw std::runtime_error("JSON type");}
inline Spec animated(Spec s=Spec()){
 s.version=4;s.variants="";auto previous=s.edit;s.edit=[previous](std::string& text){if(previous)previous(text);auto m=JsonParser(text).parse();auto animation=JsonParser("{\"kind\":\"present-loop\",\"stableWeights\":true,\"variable\":\"$frame\",\"first\":0,\"last\":2,\"step\":1,\"initial\":0,\"counter\":0,\"period\":0.5,\"counterStep\":1,\"frames\":[]}").parse();
  for(int i=0;i<3;i++){Json frame;frame.type=Json::Object;Json value;value.type=Json::Number;value.number=i;frame.object["value"]=value;frame.object["parts"]=m.at("parts");if(i==1){auto& data=frame.object["parts"].array[0].object["data"];data.object["positions"]=data.at("weights");data.object["positions"].array[1].number=36;}animation.object["frames"].array.push_back(std::move(frame));}
  m.object["animation"]=std::move(animation);text=jsonText(m);};return s;
}

// model.json "logic" (signature 2) for an animated fixture: one part "Part" (the game's IB hash aaaa0001), every
// field read from its own draws, and frame i recorded as drawing the [Resource] of identity ids[i] with
// drawindexed = auto under handling = skip; logicIni drives $frame like the fixture's FrameLoop and binds ResourceA /
// B / C (identities file:a.ib / b.ib / c.ib) for frames 0 / 1 / 2.
inline const char* logicIni="[Constants]\nglobal $frame = 0\nglobal $counter = 0\n[Present]\nif $counter % 0.5 == 0\n if $frame < 2\n  $frame = $frame + 1\n else\n  $frame = 0\n endif\nendif\npost $counter = $counter + 1\n"
 "[TextureOverridePart]\nhash = aaaa0001\nif $frame == 0\n ib = ResourceA\nelif $frame == 1\n ib = ResourceB\nelse\n ib = ResourceC\nendif\nhandling = skip\ndrawindexed = auto\n"
 "[ResourceA]\nfilename = a.ib\n[ResourceB]\nfilename = b.ib\n[ResourceC]\nfilename = c.ib\n";
inline std::string logicDraw(const std::string& ib,const std::string& range="count=auto;start=0;base=0;"){return "kind=drawindexed;"+range+"ib="+ib+";position=-;blend=-;texcoord=-;ps-t0=-;ps-t1=-;ps-t2=-;|";}
inline std::string logicSignature(const std::string& ib){return logicDraw(ib);}
inline std::string logicResources(){return "{\"resource\\\\mods\\\\logic\\\\a.ini\\\\a\":\"file:a.ib\",\"resource\\\\mods\\\\logic\\\\a.ini\\\\b\":\"file:b.ib\",\"resource\\\\mods\\\\logic\\\\a.ini\\\\c\":\"file:c.ib\"}";}
// sigs: each frame's whole signature for Part; logicJson(ids) records logicSignature(ids[i]).
inline std::string logicJsonOf(const std::vector<std::string>& sigs,const std::string& file="logic/0.ini",const std::string& readPass="Part",const std::string& resources=logicResources()){std::string frames,fields;
 for(size_t i=0;i<sigs.size();i++)frames+=(i?",":"")+std::string("{\"value\":")+std::to_string(i)+",\"draws\":{\"Part\":"+quote(sigs[i])+"}}";
 for(std::string f:{"ib","position","blend","texcoord","ps-t0","ps-t1","ps-t2"})fields+=(fields.empty()?"":",")+("\""+f+"\":[\""+readPass+"\",\""+(f=="ib"||f.rfind("ps-",0)==0?f:std::string("vb0"))+"\"]");
 return "{\"signature\":2,\"main\":\"Mods\\\\Logic\\\\a.ini\",\"files\":[{\"namespace\":\"Mods\\\\Logic\\\\a.ini\",\"file\":\""+file+"\"}],\"resources\":"+resources+",\"passes\":[{\"name\":\"Part\",\"bindings\":{\"ib\":\"aaaa0001\"},\"checks\":[\"ib\"]}],"
  "\"fields\":{\"Part\":{"+fields+"}},\"frames\":["+frames+"]}";}
inline std::string logicJson(const std::vector<std::string>& ids,const std::string& file="logic/0.ini",const std::string& readPass="Part"){std::vector<std::string> sigs;for(auto& i:ids)sigs.push_back(logicSignature(i));return logicJsonOf(sigs,file,readPass);}
inline Spec withLogic(Spec a,const std::string& logic){auto inner=a.edit;a.edit=[inner,logic](std::string& t){if(inner)inner(t);t.insert(t.rfind('}'),",\"logic\":"+logic);};return a;}
inline void writeLogicModel(const std::string& dir,const Spec& a,const std::string& ini=logicIni){std::filesystem::create_directories(dir+"/logic");write(dir+"/logic/0.ini",ini);writeModel(dir,a);}

}
