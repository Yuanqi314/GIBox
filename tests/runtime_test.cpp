// Fake managed objects exercise runtime state transitions without a game process.
#include "runtime.hpp"
#include "json.hpp"
#include "model_fixture.hpp"
#include <cassert>
#include <cstdlib>
#include <iostream>
#include <sys/mman.h>
#include <thread>
using namespace ms;
using fixture::Spec;using fixture::writeModel;using fixture::learnedAt;
namespace {
struct Obj {std::string kind,name;Ptr mesh=0,materials=0,bones=0,native=0,shared=0,parent=0;std::map<abi::Offset,Ptr> arrays;};
struct Fake {
 Ptr stringBase=0x7020000000ull,base=0,manager=0,owner=0,klass=0,typeObj=0;std::map<Ptr,Obj> obj;std::vector<void*> allocated;std::map<unsigned,Ptr> handles;unsigned nextHandle=1,nextId=1;
 unsigned clones=0,meshClones=0,textures=0,weightAllocs=0,typeLookups=0,findCalls=0,findAllCalls=0,childCalls=0,bindposeReads=0;std::map<Ptr,Ptr> goOfRenderer;std::map<Ptr,std::vector<Ptr>> goChildren;Ptr lastWeights=0;std::map<Ptr,std::string> strings;std::string lightProperty;bool failMaterials=false,failFind=false;
 std::map<Ptr,Ptr> diffuse;std::map<Ptr,float> glow; // material properties copied by Instantiate, including copies made by the game
 std::vector<Ptr> scene,assets;/* assets: only in the everything-loaded search (prefabs, inactive pooled instances) */std::vector<std::pair<Ptr,unsigned>> triangles;Ptr skeletonRoot=0,kavehRoot=0;std::map<std::string,Ptr> kinds;
 // Il2CppClass per object kind, laid out unlike stock il2cpp: byval_arg at +0x98 with a type enum that is not the stock
 // 0x12, this_arg (byref bit 0x20) at +0x40, not next to it. Decoys: a pair whose data is the same in every class, and
 // a pair present only in the renderer class.
 Ptr kindClass(const std::string& k){auto it=kinds.find(k);if(it!=kinds.end())return it->second;Ptr c=alloc(0x200);uint64_t id=0x1234+kinds.size();
  for(unsigned off:{0x98u,0x40u}){store<uint64_t>(c+off,id);store<uint16_t>(c+off+8,0x0006);store<uint8_t>(c+off+0xa,0x1c);store<uint8_t>(c+off+0xb,off==0x40?0x21:0x01);}
  for(unsigned off:{0x150u,0x160u}){store<uint64_t>(c+off,0x77);store<uint8_t>(c+off+0xa,0x1c);store<uint8_t>(c+off+0xb,off==0x160?0x20:0);}
  if(k=="renderer")for(unsigned off:{0x110u,0x120u}){store<uint64_t>(c+off,0x99);store<uint8_t>(c+off+0xa,0x05);store<uint8_t>(c+off+0xb,off==0x120?0x40:0);}
  return kinds[k]=c;}
 // Managed array of one element. Its class's byval_arg points either at the element class's byval_arg (stock il2cpp)
 // or at a separate metadata copy of it (so only the this_arg pair can locate byval_arg); plus a decoy pointer.
 std::vector<std::pair<Ptr,std::string>> typedClasses;bool pointerArrays=false;
 void linkArray(Ptr cls,const std::string& kind){Ptr target=kindClass(kind)+0x98;if(!pointerArrays){Ptr copy=alloc(16);std::memcpy(reinterpret_cast<void*>(copy),reinterpret_cast<void*>(target),16);target=copy;}
  store<Ptr>(cls+0x98,target);store<uint8_t>(cls+0xa2,0x1d);store<Ptr>(cls+0x48,kindClass(kind)+0x30);}
 void linkArrays(bool pointers){pointerArrays=pointers;for(auto& t:typedClasses)linkArray(t.first,t.second);}
 void breakPairs(){for(auto& kv:kinds)std::memset(reinterpret_cast<void*>(kv.second+0x40),0,16);}
 Ptr typed(const std::string& kind,Ptr element){Ptr a=array(8,1,&element),cls=load<Ptr>(a);typedClasses.push_back({cls,kind});linkArray(cls,kind);return a;}
 Ptr alloc(size_t n){auto p=std::calloc(1,n);assert(p);allocated.push_back(p);return reinterpret_cast<Ptr>(p);}
 Ptr array(unsigned stride,unsigned count,const void* data=nullptr){Ptr cls=alloc(0x200),p=alloc(0x20+stride*count);store<uint16_t>(cls+0xba,stride);store<Ptr>(p,cls);store<uint64_t>(p+0x18,count);if(data&&count)std::memcpy(reinterpret_cast<void*>(p+0x20),data,stride*count);return p;}
 Ptr object(std::string kind,std::string name){Ptr p=alloc(0x20),n=alloc(0x100);store<Ptr>(p,kindClass(kind));store<Ptr>(p+0x10,n);store<uint32_t>(n+8,nextId++);obj[p]=Obj{kind,name,0,0,0,n,0,0,{}};return p;}
 Ptr mesh(std::string name="Body",unsigned vertices=3){Ptr p=object("mesh",name),shared=alloc(0x120);auto&o=obj[p];o.shared=shared;store<Ptr>(o.native+0x48,shared);store<uint32_t>(shared+0x74,vertices);store<uint32_t>(shared+0xc0,1);store<uint64_t>(shared+0xe0,1);store<uint64_t>(o.native+0x90,1);store<uint32_t>(shared+0xf8,15);store<Ptr>(shared+0xf0,alloc(32*6));store<uint64_t>(shared+0x100,3);store<uint64_t>(shared+0x108,6);
  for(auto c:{std::pair{abi::getPositions,12},std::pair{abi::getNormals,12},std::pair{abi::getUV,8},std::pair{abi::getTriangles,4},std::pair{abi::getColors,4},std::pair{abi::getTangents,16}})o.arrays[c.first]=array(c.second,vertices);
  Bytes binds(64);for(int i=0;i<4;i++){float f=1;std::memcpy(binds.data()+i*20,&f,4);}o.arrays[abi::getBindposes]=array(64,1,binds.data());return p;}
 Ptr renderer(Ptr mesh,const char* bone="BoneA",const char* name="Body",Ptr root=0){Ptr r=object("renderer",name);auto&o=obj[r];o.mesh=mesh;Ptr m=object("material","Material");o.materials=typed("material",m);Ptr b=object("bone",bone);obj[b].parent=root?root:skeletonRoot;o.bones=typed("bone",b);return r;}
 // Like Object.Instantiate of a renderer's GameObject: same mesh and material references, new bone transforms.
 Ptr instantiate(Ptr src){Ptr r=object("renderer",obj.at(src).name);Ptr b=object("bone","BoneA");obj[b].parent=skeletonRoot;auto& o=obj[r];o.mesh=obj.at(src).mesh;o.materials=obj.at(src).materials;o.bones=typed("bone",b);return r;}
 // A model GameObject as the game hands it over: inactive, renderers already set up.
 Ptr gameObjectOf(std::vector<Ptr> renderers){Ptr g=object("gameobject","Avatar_Girl_Catalyst_Citlali_Manekin");for(Ptr r:renderers)goOfRenderer[r]=g;goChildren[g]=renderers;return g;}
 void world(Ptr r){store<Ptr>(owner+0x68,r?array(8,1,&r):array(8,0));}
 Fake(){void* p=mmap(nullptr,0x16000000,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS|MAP_NORESERVE,-1,0);assert(p!=MAP_FAILED);base=reinterpret_cast<Ptr>(p);Ptr root=alloc(0x8c10);manager=alloc(0x300);store<Ptr>(base+abi::manager,root);store<Ptr>(root+0x8c00,manager);store<uint32_t>(manager+0x288,7);
  Ptr entity=alloc(0x440),list=alloc(0x20);store<Ptr>(manager+0x10,list);store<Ptr>(list+0x10,array(8,1,&entity));store<uint32_t>(list+0x18,1);store<uint32_t>(entity+0x430,7);store<uint32_t>(entity+0x3a4,10000107);owner=alloc(0x70);store<Ptr>(entity+0xf8,owner);
  klass=kindClass("renderer");Ptr scenes=object("transform","Scene");skeletonRoot=object("transform","Avatar_Girl_Catalyst_Citlali");obj[skeletonRoot].parent=scenes;kavehRoot=object("transform","Avatar_Boy_Sword_Kaveh");obj[kavehRoot].parent=scenes;}
 ~Fake(){for(void* p:allocated)std::free(p);munmap(reinterpret_cast<void*>(base),0x16000000);}
}*f=nullptr;
Ptr getMesh(Ptr p,Ptr){return f->obj.at(p).mesh;}Ptr getMaterials(Ptr p,Ptr){return f->obj.at(p).materials;}Ptr getBones(Ptr p,Ptr){return f->obj.at(p).bones;}
Ptr getName(Ptr p,Ptr){auto n=f->obj.at(p).name;Ptr s=f->alloc(0x14+2*n.size());store<uint32_t>(s+0x10,n.size());for(size_t i=0;i<n.size();i++)store<uint16_t>(s+0x14+i*2,n[i]);return s;}
Ptr clone(Ptr p,Ptr){f->clones++;auto original=f->obj.at(p);if(original.kind!="mesh"){Ptr out=f->object(original.kind,original.name);if(original.kind=="material"){f->diffuse[out]=f->diffuse[p];f->glow[out]=f->glow[p];}return out;}f->meshClones++;Ptr out=f->mesh(original.name,load<uint32_t>(original.shared+0x74));auto& o=f->obj[out];o.arrays=original.arrays;store<uint64_t>(o.shared+0xe0,load<uint64_t>(original.shared+0xe0));return out;}
void destroy(Ptr p,Ptr){store<Ptr>(p+0x10,0);}Ptr parent(Ptr p,Ptr){return f->obj.at(p).parent;}
uint32_t pin(Ptr p,int,Ptr){auto h=f->nextHandle++;f->handles[h]=p;return h;}void unpin(uint32_t h){assert(f->handles.erase(h)==1);}
void setMesh(Ptr p,Ptr m,Ptr){f->obj.at(p).mesh=m;}void setMaterials(Ptr p,Ptr m,Ptr){if(f->failMaterials){f->failMaterials=false;throw std::runtime_error("fake-material-failure");}f->obj.at(p).materials=m;}
Ptr newArray(Ptr cls,uint64_t count){return f->array(load<uint16_t>(cls+0xba),count);}void op(Ptr,int,Ptr){}void action(Ptr,Ptr){}
void submeshCount(Ptr m,int n,Ptr){assert(n>=1);store<uint32_t>(f->obj.at(m).shared+0xc0,n);}
void sub(Ptr m,Ptr a,int i,Ptr){assert(i>=0&&unsigned(i)<load<uint32_t>(f->obj.at(m).shared+0xc0));f->triangles.push_back({m,unsigned(load<uint64_t>(a+0x18))});}
Ptr getPos(Ptr p,Ptr){return f->obj.at(p).arrays.at(abi::getPositions);}Ptr getNorm(Ptr p,Ptr){return f->obj.at(p).arrays.at(abi::getNormals);}Ptr getUV(Ptr p,Ptr){return f->obj.at(p).arrays.at(abi::getUV);}Ptr getIdx(Ptr p,Ptr){return f->obj.at(p).arrays.at(abi::getTriangles);}Ptr getBinds(Ptr p,Ptr){f->bindposeReads++;return f->obj.at(p).arrays.at(abi::getBindposes);}Ptr getColors(Ptr p,Ptr){return f->obj.at(p).arrays.at(abi::getColors);}Ptr getTangents(Ptr p,Ptr){return f->obj.at(p).arrays.at(abi::getTangents);}
// Like il2cpp object_new: the managed shell has no native object until the constructor runs.
Ptr texType(){Ptr d=f->alloc(8);store<Ptr>(d,f->alloc(0x100));return d;}Ptr objectNew(Ptr cls){assert(cls);Ptr p=f->alloc(0x20);f->obj[p]=Obj{"texture","",0,0,0,0,0,0,{}};return p;}
void texInit(Ptr o,int w,int h,int fmt,int mip,int linear,Ptr){assert(w>0&&h>0&&fmt==4&&mip==0&&(linear==0||linear==1)&&load<Ptr>(o+0x10)==0);Ptr n=f->alloc(0x80);store<Ptr>(o+0x10,n);f->obj[o].native=n;f->textures++;}
void texPixels(Ptr o,Ptr data,int n,Ptr){assert(load<Ptr>(o+0x10)&&data&&n==4);}
// Managed strings live above 4 GiB in the game; a 32-bit truncation must not survive the round trip.
Ptr newString(Ptr self,const char* s){assert(self==0&&s);Ptr p=(f->stringBase+=0x40);f->strings[p]=s;return p;}
uint32_t hasProp(Ptr mat,Ptr name,Ptr){assert(mat&&f->strings.count(name));return f->strings[name]=="_LightMapTex"?0xffffff01u:0xffffff00u;}
Ptr nativeAlloc(Ptr allocator,Ptr old,uint64_t size,int align,int label,int options,const char* name,int line){assert(allocator==load<Ptr>(f->base+abi::nativeAllocator)&&old==0&&size==32*3&&align==16&&label==15&&options==0&&name&&line==1);f->weightAllocs++;return f->lastWeights=f->alloc(size);}
void setTex(Ptr mat,Ptr name,Ptr tex,Ptr){assert(mat&&tex&&load<Ptr>(tex+0x10)&&f->strings.count(name));f->lightProperty=f->strings[name];}
void setMainTexture(Ptr mat,Ptr tex,Ptr){assert(mat&&tex);f->diffuse[mat]=tex;}
unsigned channelSets=0;void setChannel(Ptr,Ptr a,Ptr){assert(load<uint64_t>(a+0x18)==3);channelSets++;}void setPos(Ptr p,Ptr a,Ptr){store<uint32_t>(f->obj.at(p).shared+0x74,load<uint64_t>(a+0x18));}void setAny(Ptr,Ptr,Ptr){}
Ptr classFromType(Ptr t,int init){assert(init==1&&load<uint8_t>(t+0xa)!=0);f->typeLookups++;for(auto& kv:f->kinds)if(t==kv.second+0x98)return kv.second;return f->alloc(0x10);}
Ptr typeObject(Ptr t){assert(t==f->klass+0x98);if(!f->typeObj){f->typeObj=f->alloc(0x20);store<Ptr>(f->typeObj+0x10,t);}return f->typeObj;}
Ptr goOf(Ptr r,Ptr){assert(f->obj.at(r).kind=="renderer");auto it=f->goOfRenderer.find(r);if(it!=f->goOfRenderer.end())return it->second;Ptr g=f->object("gameobject","GO");f->goOfRenderer[r]=g;return g;}
Ptr children(Ptr go,Ptr type,uint32_t inactive,Ptr){assert(f->obj.at(go).kind=="gameobject"&&type&&type==f->typeObj&&inactive==1);f->childCalls++;auto& v=f->goChildren[go];return f->array(8,v.size(),v.data());}
// Scripting::ScriptingWrapperFor: the managed object of a native one.
Ptr wrapperFor(Ptr native){for(auto& kv:f->obj)if(kv.second.native==native)return kv.first;return 0;}
Ptr findNative(Ptr type,int mode){assert(type&&type==f->typeObj&&mode==2);f->findAllCalls++;if(f->failFind)throw std::runtime_error("fake-find-failure");std::vector<Ptr> all=f->scene;all.insert(all.end(),f->assets.begin(),f->assets.end());return f->array(8,all.size(),all.data());}
Ptr findObjects(Ptr type,Ptr){assert(type&&type==f->typeObj);f->findCalls++;if(f->failFind)throw std::runtime_error("fake-find-failure");return f->array(8,f->scene.size(),f->scene.data());}
Ptr resolve(abi::Offset o){switch(o){
#define FN(x,y) case abi::x:return reinterpret_cast<Ptr>(&y)
 FN(getMesh,getMesh);FN(getMaterials,getMaterials);FN(getBones,getBones);FN(getName,getName);FN(getParent,parent);FN(clone,clone);FN(destroy,destroy);FN(pin,pin);FN(unpin,unpin);FN(setMesh,setMesh);FN(setMaterials,setMaterials);FN(arrayNew,newArray);FN(getBindposes,getBinds);FN(getPositions,getPos);FN(getNormals,getNorm);FN(getUV,getUV);FN(getTriangles,getIdx);FN(getColors,getColors);FN(getTangents,getTangents);FN(setColors,setChannel);FN(setTangents,setChannel);FN(setUV2,setChannel);FN(setPositions,setPos);FN(setNormals,setAny);FN(setUV,setAny);FN(setTriangles,setAny);FN(clear,op);FN(setIndexFormat,op);FN(setSubmeshCount,submeshCount);FN(meshUpload,op);FN(meshFinish,action);FN(setSubmeshTriangles,sub);FN(textureType,texType);FN(objectNew,objectNew);FN(textureInit,texInit);FN(texturePixels,texPixels);FN(textureApply,action);FN(mainTexture,setMainTexture);FN(newString,newString);FN(nativeAlloc,nativeAlloc);FN(hasProperty,hasProp);FN(setTexture,setTex);FN(classFromType,classFromType);FN(typeObject,typeObject);FN(findObjectsOfType,findObjects);FN(findObjectsNative,findNative);FN(componentsInChildren,children);FN(gameObject,goOf);FN(wrapperFor,wrapperFor);
#undef FN
 default:throw std::runtime_error("Unexpected fake engine call: "+std::to_string(o));}}
bool alive(Ptr p){return p&&load<Ptr>(p+0x10);}
}
int main(){Fake fake;f=&fake;char temp[]="/tmp/model-runtime-XXXXXX";assert(mkdtemp(temp));std::string root=temp,pkg="com.miHoYo.Yuanshen",media=root+"/media",shared=media+"/"+pkg+"/GIBox",models=shared+"/models",boneRoot=shared+"/skeletons";
 writeModel(models+"/A");writeModel(models+"/B");
 // Model B carries a real 1x1 diffuse and a LightMap, so its material builds two textures.
 {Spec b;b.textures=true;b.channels=true;writeModel(models+"/B",b);}
 // C only knows its target (another character, never controlled): it must still follow that mesh wherever it appears.
 writeModel(models+"/C",Spec().character(10000200).withTarget(fixture::target("OtherBody")));
 // V has two variants: $swap=1 hides the only submesh.
 {Spec v;v.variants="{\"vars\":[{\"name\":\"$swap\",\"values\":[0,1]}],\"combos\":[{\"$swap\":0},{\"$swap\":1}],\"default\":0}";v.draws.push_back("[{\"start\":0,\"count\":0,\"material\":\"Material0\",\"slot\":0}]");writeModel(models+"/V",v);}
 Ptr original=f->mesh();Ptr world=f->renderer(original);f->world(world);
 Ptr ui=f->renderer(original);Ptr uiMaterials=f->obj[ui].materials;                  // detail page: same asset
 Ptr npc=f->renderer(f->mesh("Body",4));                                                // same name, different mesh
 Ptr odd=f->renderer(original,"Other");                                                 // same mesh, other skeleton
 Ptr other=f->renderer(f->mesh("OtherBody"),"BoneA","Body",f->kavehRoot);Ptr otherMesh=f->obj[other].mesh;  // another character (model C, U)
 f->scene={world,ui,npc,odd,other};
 Engine e(f->base);e.resolver=resolve;Runtime runtime(e,pkg,root+"/private",media,false);store<Ptr>(f->base+abi::nativeAllocator,f->alloc(0x10));
 // rev only makes control.json differ (a control change clears latched failures)
 auto control=[&](bool enabled,int rev,int exportBones,std::vector<std::string> selected,std::string vars="{}"){std::string list="[";for(size_t i=0;i<selected.size();i++)list+=(i?",":"")+quote(selected[i]);list+="]";
  write(shared+"/control.json.tmp","{\"enabled\":"+std::string(enabled?"true":"false")+",\"selected\":"+list+",\"revision\":"+std::to_string(rev)+",\"exportBones\":"+std::to_string(exportBones)+",\"variables\":"+vars+"}");std::filesystem::rename(shared+"/control.json.tmp",shared+"/control.json");};
 auto status=[&]{return JsonParser(text(shared+"/status.json")).parse();};auto state=[&]{return status().at("state").string;};
 auto meshOf=[&](Ptr r){return f->obj[r].mesh;};

 // 0. The bone library is recorded while replacement is still off (one-click import depends on it).
 control(false,0,0,{});runtime.tick(9000);assert(!exists(boneRoot+"/10000107/skeleton.json"));runtime.tick(9500);assert(state()=="disabled"&&exists(boneRoot+"/10000107/skeleton.json")&&meshOf(world)==original&&exists(shared+"/GIBox Log.txt"));
 // 1. Legacy model A: target learned from the world character, then every renderer showing that asset follows.
 control(true,0,0,{"A","B"});runtime.tick(10000);Ptr a1=meshOf(world);
 assert(state()=="active"&&a1!=original&&meshOf(ui)==a1&&f->obj[ui].materials!=uiMaterials&&f->obj[ui].materials!=f->obj[world].materials);
 assert(meshOf(npc)!=a1&&meshOf(odd)==original&&meshOf(other)==otherMesh);
 assert(f->findCalls==1&&f->typeLookups>0&&text(shared+"/GIBox Log.txt").find("byval_arg at class+0x98 this_arg +0x40")!=std::string::npos&&status().at("scan").string=="ok"&&learnedAt(models+"/A").targets.at("Part0").mesh=="Body");
 // Skin weights: a new engine block of exactly the model's size, written and installed in the clone's skin array.
 {Ptr d=f->obj[a1].shared;float w;std::memcpy(&w,reinterpret_cast<void*>(load<Ptr>(d+0xf0)),4);assert(f->weightAllocs==1&&load<Ptr>(d+0xf0)==f->lastWeights&&w==1&&load<uint64_t>(d+0x100)==3&&load<uint64_t>(d+0x108)==3);}
 // Skeleton library for the import: per-renderer bones, bindposes and mesh identity, plus the skeleton's transform chain.
 {auto sk=JsonParser(text(boneRoot+"/10000107/skeleton.json")).parse();auto& r=sk.at("renderers").array.at(0);
  assert(sk.at("id").number==10000107&&sk.at("root").string.find("Avatar_Girl_Catalyst_Citlali")!=std::string::npos&&r.at("name").string=="Body"&&r.at("bones").array.at(0).string=="BoneA");
  assert(r.at("bindposes").array.size()==16&&r.at("bindposes").array[0].number==1&&fingerprintOf(r.at("target")).mesh=="Body"&&r.at("parents").array[0].number==-1);}
 // 2. Asset reloaded: a new original mesh with the same fingerprint (name suffix normalised) reuses the built mesh.
 //    B (selected after A, same mesh) is not built: everything it replaces is shown from A.
 Ptr ui2=f->renderer(f->mesh("Body(Clone)"));f->scene.push_back(ui2);unsigned lookups=f->typeLookups;runtime.tick(10100);assert(meshOf(ui2)!=a1);runtime.tick(10500);
 assert(meshOf(ui2)==a1&&f->meshClones==1&&f->typeLookups==lookups&&f->findCalls==2);
 // 3. Two models of one character: the one switched on first shows. B first: B is built and takes the renderers from A
 //    in the same tick (two textures, LightMap property chosen by name). A first again: A, still built, takes them back.
 control(true,1,0,{"B","A"});runtime.tick(10600);Ptr b1=meshOf(world);
 assert(state()=="active"&&b1!=a1&&meshOf(ui)==b1&&meshOf(ui2)==b1&&f->textures==2&&f->lightProperty=="_LightMapTex"&&f->meshClones==2&&channelSets==3);
 assert(status().at("models").array.size()==1&&status().at("models").array[0].string=="B");
 control(true,2,0,{"A","B"});runtime.tick(10700);assert(meshOf(world)==a1&&meshOf(ui)==a1&&meshOf(ui2)==a1&&f->meshClones==2&&text(shared+"/GIBox Log.txt").find("outranked B/Part0 on Body")!=std::string::npos);
 // 4. Character leaves (renderers destroyed) and returns as new objects: no rebuild.
 for(Ptr r:{world,ui,ui2}){store<Ptr>(r+0x10,0);}f->scene={npc,odd,other};f->world(0);store<uint32_t>(f->manager+0x288,0);runtime.tick(11200);assert(state()=="waiting");
 Ptr world2=f->renderer(original);f->world(world2);f->scene={world2,npc,odd,other};store<uint32_t>(f->manager+0x288,7);runtime.tick(11700);assert(state()=="active"&&meshOf(world2)==a1&&f->meshClones==2);
 // 5. Manual bone export keeps working; renderer names are filtered.
 f->obj[world2].name="Body (1)";control(true,2,1,{"A","B"});runtime.tick(11800);runtime.tick(12300);{bool found=false;for(auto& d:std::filesystem::directory_iterator(boneRoot))if(d.path().filename().string().rfind("10000107-",0)==0&&text(d.path().string()+"/skeleton.json").find("\"name\":\"Body1\"")!=std::string::npos)found=true;assert(found&&state()=="active");}f->obj[world2].name="Body";
 // 6. Hot reload: a new generation is built and takes over in one tick; the old mesh is destroyed.
 writeModel(models+"/A",Spec().gen("2"));runtime.tick(13000);Ptr a2=meshOf(world2);assert(a2!=a1&&!alive(a1)&&f->meshClones==3&&state()=="active");
 // A broken edit keeps the last good generation on screen and reports the error.
 writeModel(models+"/A",Spec().gen("3"));write(models+"/A/model.bin","bad");runtime.tick(14100);assert(meshOf(world2)==a2&&state()=="active"&&!status().at("errors").array.empty());
 writeModel(models+"/A",Spec().gen("4"));runtime.tick(15200);Ptr a4=meshOf(world2);assert(a4!=a2&&status().at("errors").array.empty());
 // 7. A target-only model follows its own character although another one is controlled.
 control(true,2,1,{"A","B","C"});runtime.tick(15300);assert(meshOf(other)!=otherMesh&&meshOf(world2)==a4&&status().at("models").array.size()==2);
 // 8. Variants switch in place: same mesh object, new submesh ranges.
 control(true,2,1,{"V"});runtime.tick(15400);Ptr v=meshOf(world2);assert(v!=a4&&meshOf(other)==otherMesh&&f->triangles.back()==std::make_pair(v,3u));
 control(true,2,1,{"V"},"{\"V\":{\"$swap\":1}}");runtime.tick(15500);assert(meshOf(world2)==v&&f->triangles.back()==std::make_pair(v,0u));
 control(true,2,1,{"V"},"{\"V\":{\"$swap\":0}}");runtime.tick(15600);assert(meshOf(world2)==v&&f->triangles.back()==std::make_pair(v,3u));
 // 8b. Imported without a bone library: found by the character name in the skeleton chain, bones taken from the game,
 //     the ID written back once the character is seen in the world; a name seen only outside the world keeps ID 0.
 for(auto [dir,token]:{std::pair<const char*,const char*>{"T","citlali"},{"U","kaveh"}})writeModel(models+"/"+dir,Spec().character(0,{token}).noSkeleton());
 control(true,2,1,{"T","U"});runtime.tick(15700);runtime.tick(15800);
 assert(meshOf(world2)!=original&&meshOf(world2)!=v&&meshOf(other)!=otherMesh&&learnedAt(models+"/T").targets.count("Part0")&&learnedAt(models+"/T").id==10000107&&learnedAt(models+"/U").id==0&&learnedAt(models+"/U").targets.at("Part0").mesh=="OtherBody"&&status().at("models").array.size()==2);
 // 9. Disable restores every renderer and releases every handle.
 control(false,2,1,{"V"});runtime.tick(16000);assert(state()=="disabled"&&meshOf(world2)==original&&meshOf(other)==otherMesh&&f->handles.empty());
 // 10. Load failure latches per generation; a control change retries.
 write(models+"/A/model.bin","bad");control(true,2,1,{"A"});runtime.tick(16100);assert(state()=="error"&&meshOf(world2)==original&&f->handles.empty());
 unsigned built=f->meshClones;runtime.tick(16700);assert(f->meshClones==built);
 writeModel(models+"/A",Spec().gen("5"));control(true,3,1,{"A"});runtime.tick(16800);assert(state()=="active");
 // 11. Attaching fails after the mesh was set: the renderer gets its original mesh back and the error is reported.
 control(false,3,1,{"A"});runtime.tick(16900);f->failMaterials=true;control(true,3,1,{"A"});runtime.tick(17000);
 assert(meshOf(world2)==original&&!status().at("errors").array.empty());control(true,4,1,{"A"});runtime.tick(17100);assert(state()=="active"&&meshOf(world2)!=original);
 // 12. A failing renderer search only backs off; the world character keeps its model.
 {f->failFind=true;Ptr w=meshOf(world2);runtime.tick(17700);assert(state()=="active"&&meshOf(world2)==w);unsigned calls=f->findCalls;runtime.tick(18300);assert(f->findCalls==calls);f->failFind=false;runtime.tick(22800);assert(f->findCalls==calls+1&&state()=="active");}
 // 12b. A renderer the game copies from one of ours shows our mesh from the start and is adopted; when the source goes
 //      away, its material clones stay alive for the copy.
 Ptr model=meshOf(world2),source=f->renderer(original),sourceMats=f->obj[source].materials;f->scene.push_back(source);runtime.tick(25000);
 assert(model!=original&&meshOf(source)==model&&f->obj[source].materials!=sourceMats);
 Ptr copy=f->instantiate(source);assert(meshOf(copy)==model);f->scene.push_back(copy);runtime.tick(25600);
 assert(text(shared+"/GIBox Log.txt").find("adopted A/Part0 -> Body")!=std::string::npos&&meshOf(copy)==model&&state()=="active");
 Ptr shared0=load<Ptr>(f->obj[copy].materials+0x20);store<Ptr>(source+0x10,0);runtime.tick(26200);assert(alive(shared0)&&meshOf(copy)==model);
 // 12c. Async hand-off: the game passes a new, still inactive model GameObject before activating it. A built model is
 //      attached right there, with no tick in between. D was never in the scene: the tick preloads its files and the
 //      hand-off builds it from the handed renderer. Anything that is not a GameObject is ignored.
 //      The game's own callback then puts its mesh back on the renderer (seen on device): ours goes back right after it.
 {Ptr handed=f->renderer(original),go=f->gameObjectOf({handed}),handedMats=f->obj[handed].materials;unsigned calls=f->childCalls;runtime.spawned(go);
  assert(meshOf(handed)==model);f->obj[handed].mesh=original;runtime.spawnedAfter();
  assert(f->childCalls==calls+1&&meshOf(handed)==model&&text(shared+"/GIBox Log.txt").find("spawn Avatar_Girl_Catalyst_Citlali_Manekin: attached 1, adopted 0, built 0")!=std::string::npos);
  assert(text(shared+"/GIBox Log.txt").find("spawn-after after the game's hand-off callback: kept 0, put back 1, lost 0")!=std::string::npos);
  writeModel(models+"/D",Spec().character(10000300).withTarget(fixture::target("SpawnBody")));
  control(true,4,1,{"A","D"});runtime.tick(26300);
  Ptr spawnMesh=f->mesh("SpawnBody"),dr=f->renderer(spawnMesh,"BoneA","Body",f->kavehRoot),dgo=f->gameObjectOf({dr});unsigned before=f->meshClones;
  runtime.spawned(dr);assert(meshOf(dr)==spawnMesh);
  runtime.spawned(dgo);runtime.spawnedAfter();assert(meshOf(dr)!=spawnMesh&&f->meshClones==before+1&&text(shared+"/GIBox Log.txt").find("attached 1, adopted 0, built 1")!=std::string::npos);
  // 12d. Level-of-detail swap by the game between ticks: ours is back on the next frame; disabling later restores the
  //      LOD0 original (the renderer's bones stay bound for that layout).
  Ptr lod=f->mesh("Body_LOD3",2);f->obj[world2].mesh=lod;runtime.tick(26310);assert(meshOf(world2)==model&&text(shared+"/GIBox Log.txt").find("reclaimed A/Part0 <- Body_LOD3")!=std::string::npos);
  // 12e. The game's own rebinding (native LOD switch / BindMeshBones) asks for its mesh on a renderer we attached: it gets
  //      ours, LODs included (an LOD never seen before is resolved through its managed wrapper). Other meshes, renderers
  //      we did not attach, and a mesh whose bone-name hashes differ from the original's pass unchanged.
  {Ptr wn=load<Ptr>(world2+0x10),ourN=load<Ptr>(model+0x10),origN=load<Ptr>(original+0x10),lod2=f->mesh("Body_LOD2",2),capeN=load<Ptr>(f->mesh("Cape",3)+0x10);
   assert(runtime.bindMesh(wn,origN)==ourN&&runtime.bindMesh(wn,load<Ptr>(lod2+0x10))==ourN&&runtime.bindMesh(wn,capeN)==capeN&&runtime.bindMesh(wn,ourN)==ourN);
   assert(runtime.bindMesh(load<Ptr>(npc+0x10),origN)==origN&&text(shared+"/GIBox Log.txt").find("bind-substituted A/Part0: game asked for Body_LOD2 (2 vertices)")!=std::string::npos);
   // Off the Tick thread nothing is touched: the game's request passes as it is.
   {Ptr got=0;std::thread other([&]{got=runtime.bindMesh(wn,origN);});other.join();assert(got==origN&&text(shared+"/GIBox Log.txt").find("hook-skip mesh binding called on another thread than Tick")!=std::string::npos);}
   Ptr dN=load<Ptr>(dr+0x10),spawnN=load<Ptr>(spawnMesh+0x10);store<uint64_t>(load<Ptr>(meshOf(dr)+0x10)+0x90,0);
   assert(runtime.bindMesh(dN,spawnN)==spawnN&&text(shared+"/GIBox Log.txt").find("bind-incompatible D/Part0")!=std::string::npos);}
  // A different mesh is left to the game, and the renderer gets its original materials back before our clones are
  // destroyed (0.3.5 left destroyed clones on it, which draws magenta).
  Ptr cape=f->mesh("Cape",3),handedClone=load<Ptr>(f->obj[handed].materials+0x20);f->obj[handed].mesh=cape;runtime.tick(26320);assert(meshOf(handed)==cape);
  runtime.tick(26800);assert(meshOf(handed)==cape&&f->obj[handed].materials==handedMats&&!alive(handedClone)&&alive(load<Ptr>(handedMats+0x20)));
  control(false,4,1,{"A","D"});runtime.tick(26900);
  assert(state()=="disabled"&&meshOf(copy)==original&&f->obj[copy].materials==sourceMats&&meshOf(handed)==cape&&meshOf(dr)==spawnMesh&&meshOf(world2)==original&&!alive(shared0)&&f->handles.empty());
  assert(runtime.bindMesh(load<Ptr>(world2+0x10),load<Ptr>(original+0x10))==load<Ptr>(original+0x10));  // disabled: untouched
  for(Ptr r:{copy,handed,dr,world2})assert(alive(load<Ptr>(f->obj[r].materials+0x20)));

  Ptr later=f->renderer(original),lgo=f->gameObjectOf({later});runtime.spawned(lgo);assert(meshOf(later)==original&&f->handles.empty());}
 runtime.shutdown();assert(meshOf(world2)==original&&f->handles.empty());
 // A new session finds the skeleton library unchanged and leaves the file alone.
 {auto file=boneRoot+"/10000107/skeleton.json";auto before=text(file);auto stamp=std::filesystem::last_write_time(file);std::this_thread::sleep_for(std::chrono::milliseconds(30));
  control(false,0,0,{});Runtime again(e,pkg,root+"/private",media,false);again.tick(30000);again.tick(30100);
  assert(text(file)==before&&std::filesystem::last_write_time(file)==stamp&&text(shared+"/GIBox Log.txt").find("bone-library "+boneRoot)!=std::string::npos);again.shutdown();assert(f->handles.empty());}
 // 13. Threaded mode (as in the game): file work on the worker, engine calls only from tick().
 //     The classes lose their this_arg pair here, so the Type comes from the array-class sign alone.
 f->breakPairs();f->linkArrays(true);
 {std::string media2=root+"/media2",shared2=media2+"/"+pkg+"/GIBox",models2=shared2+"/models";writeModel(models2+"/A");std::filesystem::create_directories(shared2);
  write(shared2+"/control.json","{\"enabled\":true,\"selected\":[\"A\"],\"cycle\":0,\"exportBones\":0}");
  {Runtime threaded(e,pkg,root+"/private2",media2,true);bool ok=false;
   for(int i=0;i<300&&!ok;i++){threaded.tick(100000+uint64_t(i)*100);std::this_thread::sleep_for(std::chrono::milliseconds(10));try{ok=JsonParser(text(shared2+"/status.json")).parse().at("state").string=="active";}catch(const std::exception&){}}
   assert(ok&&meshOf(world2)!=original);threaded.tick(200000);threaded.shutdown();assert(meshOf(world2)==original&&f->handles.empty());}
  assert(exists(shared2+"/skeletons/10000107/skeleton.json")&&text(shared2+"/GIBox Log.txt").find("byval_arg at class+0x98 no this_arg pair, array votes 2")!=std::string::npos);}
 // 14. Neither sign: three failed searches turn the scan off, the classes are dumped once, the world character keeps
 //     working and a model of another character says why it waits.
 f->linkArrays(false);
 {std::string media3=root+"/media3",shared3=media3+"/"+pkg+"/GIBox",models3=shared3+"/models";writeModel(models3+"/C",Spec().character(10000200).withTarget(fixture::target("OtherBody")));
  std::filesystem::create_directories(shared3);write(shared3+"/control.json","{\"enabled\":true,\"selected\":[\"C\"],\"cycle\":0,\"exportBones\":0}");
  Runtime blind(e,pkg,root+"/private3",media3,false);unsigned calls=f->findCalls;for(uint64_t t:{300000ull,305000ull,310000ull,315000ull})blind.tick(t);
  auto st=JsonParser(text(shared3+"/status.json")).parse();auto log=text(shared3+"/GIBox Log.txt");
  assert(f->findCalls==calls&&st.at("state").string=="waiting"&&st.at("scan").string=="off"&&st.at("detail").string.find("全场景扫描不可用")!=std::string::npos&&meshOf(other)==otherMesh);
  assert(log.find("renderer-type-dump votes")!=std::string::npos&&log.find("renderer-type-dump votes")==log.rfind("renderer-type-dump votes")&&log.find("find-disabled")!=std::string::npos&&log.find("element2 ")!=std::string::npos);
  blind.shutdown();assert(f->handles.empty());}
 // 15. A game update gave the character's mesh more vertices (same renderer, mesh name and bones): the part follows the new
 //     mesh by itself. Another mesh name (an outfit) is reported instead of guessed.
 {std::string media4=root+"/media4",shared4=media4+"/"+pkg+"/GIBox",models4=shared4+"/models";writeModel(models4+"/A",Spec().withTarget(fixture::target("Body")));
  std::filesystem::create_directories(shared4);write(shared4+"/control.json","{\"enabled\":true,\"selected\":[\"A\"],\"cycle\":0,\"exportBones\":0}");
  Ptr updated=f->mesh("Body",5);Ptr world5=f->renderer(updated);f->world(world5);
  Runtime upd(e,pkg,root+"/private4",media4,false);upd.tick(400000);upd.tick(400300);auto st=JsonParser(text(shared4+"/status.json")).parse();
  assert(st.at("state").string=="active"&&meshOf(world5)!=updated&&learnedAt(models4+"/A").targets.at("Part0").vertices==5&&text(shared4+"/GIBox Log.txt").find("target-updated A/Part0: Body vertices 3->5")!=std::string::npos);
  store<Ptr>(world5+0x10,0);Ptr outfit=f->renderer(f->mesh("BodyCostume",4));f->world(outfit);upd.tick(400600);st=JsonParser(text(shared4+"/status.json")).parse();
  assert(st.at("state").string=="waiting"&&st.at("detail").string.find("和导入时不同")!=std::string::npos&&meshOf(outfit)==f->obj[outfit].mesh&&learnedAt(models4+"/A").targets.at("Part0").vertices==5);
  upd.shutdown();assert(f->handles.empty());}
 // 16. Manual import for a chosen character without its skeleton library: ID plus model name (model.json "character"). The part is learned
 //     from that character's skeleton while another one is on the field (party / detail page) and the ID stays. Imported
 //     again for another character (new generation), what was learned for the old one is forgotten. Log lines follow the
 //     XXMI Launcher format, and control.json "logLevel" filters them.
 f->linkArrays(true);
 {std::string media5=root+"/media5",shared5=media5+"/"+pkg+"/GIBox",models5=shared5+"/models",logFile=shared5+"/GIBox Log.txt";
  writeModel(models5+"/K",Spec().character(10000081,{"kaveh"}).noSkeleton());
  std::filesystem::create_directories(shared5);write(shared5+"/control.json","{\"enabled\":true,\"selected\":[\"K\"],\"cycle\":0,\"exportBones\":0}");
  Ptr world6=f->renderer(original);f->world(world6);Ptr kaveh=f->renderer(f->mesh("KavehBody"),"BoneA","Body",f->kavehRoot),kavehMesh=f->obj[kaveh].mesh;
  // An NPC whose skeleton chain carries the same name but is not a playable character is never learned from.
  Ptr npcRoot=f->object("transform","NPC_Kaveh_Statue");Ptr statue=f->renderer(f->mesh("StatueBody"),"BoneA","Body",npcRoot),statueMesh=f->obj[statue].mesh;
  f->scene={statue,world6,kaveh};
  Runtime idm(e,pkg,root+"/private5",media5,false);idm.tick(500000);idm.tick(500600);
  auto log=text(logFile);
  assert(meshOf(kaveh)!=kavehMesh&&meshOf(statue)==statueMesh&&meshOf(world6)==original&&readSummary(models5+"/K").id==10000081&&learnedAt(models5+"/K").id==0&&learnedAt(models5+"/K").targets.at("Part0").mesh=="KavehBody");
  assert(log.find("runtime INFO target-learned K/Part0 by name kaveh (KavehBody, 3 vertices)")!=std::string::npos);
  {auto first=log.substr(0,log.find('\n'));auto digits=[&](size_t at,size_t n){for(size_t i=at;i<at+n;i++)if(first.size()<=i||!std::isdigit(static_cast<unsigned char>(first[i])))return false;return true;};
   assert(digits(0,4)&&first[4]=='-'&&digits(5,2)&&first[7]=='-'&&digits(8,2)&&first[10]==' '&&digits(11,2)&&first[13]==':'&&digits(14,2)&&first[16]==':'&&digits(17,2)&&first[19]==','&&digits(20,3)&&first[23]==' '&&digits(24,6)&&first.compare(30,9," runtime ")==0);
   for(size_t p=0;(p=log.find('\n',p))!=std::string::npos&&p+1<log.size();p++)if(log[p+1]!=' ')assert(log.compare(p+1+24,6,first,24,6)==0);}  // one instance id per process
  // (the converter drops learned.json when a folder is converted for another character)
  std::filesystem::remove(models5+"/K/learned.json");writeModel(models5+"/K",Spec().character(10000107,{"citlali"}).noSkeleton().gen("2"));
  idm.tick(501700);idm.tick(502300);
  assert(meshOf(kaveh)==kavehMesh&&meshOf(world6)!=original&&learnedAt(models5+"/K").targets.at("Part0").mesh=="Body");
  // logLevel warning: INFO lines (disabled, model-released) are dropped, warnings still pass.
  write(shared5+"/control.json","{\"enabled\":false,\"selected\":[\"K\"],\"cycle\":0,\"exportBones\":0,\"logLevel\":\"warning\"}");auto size=text(logFile).size();idm.tick(503400);
  assert(meshOf(world6)==original&&text(logFile).size()==size);
  {std::thread other([&]{idm.bindMesh(load<Ptr>(world6+0x10),load<Ptr>(original+0x10));});other.join();}
  assert(text(logFile).find("runtime WARNING hook-skip",size)!=std::string::npos);
  idm.shutdown();assert(f->handles.empty());}
 // 17. The skeleton library while the character is still loading (no renderers yet): no error, recorded once they are
 //     there. A renderer whose bindposes do not match its bones is reported with the reason, once.
 {std::string media6=root+"/media6",shared6=media6+"/"+pkg+"/GIBox",logFile=shared6+"/GIBox Log.txt";std::filesystem::create_directories(shared6);
  write(shared6+"/control.json","{\"enabled\":false,\"selected\":[]}");f->world(0);
  Runtime lib(e,pkg,root+"/private6",media6,false);lib.tick(600000);lib.tick(600500);
  assert(!exists(shared6+"/skeletons/10000107/skeleton.json")&&(!exists(logFile)||text(logFile).find("bone-library-error")==std::string::npos));
  Ptr broken=f->renderer(f->mesh());store<uint64_t>(f->obj[f->obj[broken].mesh].arrays[abi::getBindposes]+0x18,2);f->world(broken);
  lib.tick(601200);lib.tick(612000);lib.tick(623000);auto log=text(logFile);auto at=log.find("bone-library-error 10000107: Incomplete skeleton: 1 renderers, 0 still loading, 1 with bone/bindpose counts that differ");
  assert(at!=std::string::npos&&log.find("bone-library-error",at+1)==std::string::npos&&log.find("Body 1 bones / 2 bindposes")!=std::string::npos);
  Ptr good=f->renderer(original);f->world(good);lib.tick(634000);lib.tick(634500);assert(exists(shared6+"/skeletons/10000107/skeleton.json"));lib.shutdown();assert(f->handles.empty());}
 // 18. Why a model waits: before the character is known the scan has not started; then the same mesh at another level of
 //     detail is named with the number that differs (status and log); once LOD0 shows, it attaches.
 {std::string media7=root+"/media7",shared7=media7+"/"+pkg+"/GIBox",models7=shared7+"/models",logFile=shared7+"/GIBox Log.txt";writeModel(models7+"/A",Spec().withTarget(fixture::target("Body")));
  std::filesystem::create_directories(shared7);write(shared7+"/control.json","{\"enabled\":true,\"selected\":[\"A\"]}");
  Ptr lod=f->renderer(f->mesh("Body_LOD1",2));f->world(lod);f->scene={lod};store<uint32_t>(f->manager+0x288,0);
  Runtime why(e,pkg,root+"/private7",media7,false);why.tick(700000);auto st=JsonParser(text(shared7+"/status.json")).parse();
  assert(st.at("state").string=="waiting"&&st.at("detail").string.find("还没认出当前操控的角色")!=std::string::npos&&st.at("detail").string.find("任一处")==std::string::npos);
  store<uint32_t>(f->manager+0x288,7);why.tick(700600);st=JsonParser(text(shared7+"/status.json")).parse();auto log=text(logFile);
  assert(st.at("state").string=="waiting"&&st.at("detail").string.find("和导入时不同（顶点 3→2）")!=std::string::npos&&meshOf(lod)==f->obj[lod].mesh);
  assert(log.find("runtime INFO character none")!=std::string::npos&&log.find("runtime INFO character 10000107")!=std::string::npos);
  assert(log.find("wait-reason A: 当前角色 10000107，它的渲染器 1 个，全场景 1 个")!=std::string::npos&&log.find("Part0 要 Body / Body（顶点 3，子网格 1，骨骼 1）：同名 Mesh 数字不同 Body / Body_LOD1（顶点 2，子网格 1，骨骼 1）")!=std::string::npos);
  Ptr near=f->renderer(original);f->world(near);f->scene={lod,near};why.tick(701200);why.tick(701800);
  assert(JsonParser(text(shared7+"/status.json")).parse().at("state").string=="active"&&meshOf(near)!=original);why.shutdown();assert(f->handles.empty());}
 // 19. Lower levels of detail. Right after loading the character shows one (Body_LOD3): the skeleton library waits and
 //     records the full mesh. A model converted against a lower LOD is not built, also when that LOD is on screen, and
 //     says it needs re-converting. A bone-export count already in control.json at start-up is not a request.
 {std::string media8=root+"/media8",shared8=media8+"/"+pkg+"/GIBox",models8=shared8+"/models",bones8=shared8+"/skeletons";writeModel(models8+"/L",Spec().withTarget(fixture::target("Body_LOD3")));
  std::filesystem::create_directories(shared8);write(shared8+"/control.json","{\"enabled\":true,\"selected\":[\"L\"],\"exportBones\":3}");
  Ptr low=f->mesh("Body_LOD3");Ptr w=f->renderer(low);f->world(w);f->scene={w};
  {Runtime lod(e,pkg,root+"/private8",media8,false);lod.tick(800000);lod.tick(800600);auto st=JsonParser(text(shared8+"/status.json")).parse();
   assert(!exists(bones8+"/10000107/skeleton.json")&&meshOf(w)==low&&st.at("state").string=="error"&&st.at("detail").string.find("低精度模型 Body_LOD3")!=std::string::npos);
   f->obj[w].mesh=original;lod.tick(802000);lod.tick(802600);auto lib=text(bones8+"/10000107/skeleton.json");
   assert(lib.find("\"mesh\":\"Body\"")!=std::string::npos&&lib.find("LOD")==std::string::npos);lod.shutdown();}
  {Runtime again(e,pkg,root+"/private8b",media8,false);again.tick(803000);again.tick(803600);again.shutdown();}
  for(auto& d:std::filesystem::directory_iterator(bones8))assert(d.path().filename()=="10000107");
  assert(f->handles.empty());}
 // 20. A version 2 model: draws carry their conditions. The default shows one draw; $halo=3 (with $legs at its default 1)
 //     adds the second in place; a value the model does not list keeps the default.
 {std::string media9=root+"/media9",shared9=media9+"/"+pkg+"/GIBox",models9=shared9+"/models";
  {Spec w;w.version=2;w.withTarget(fixture::target("Body"));w.variants="{\"vars\":[{\"name\":\"$halo\",\"values\":[0,1,3]},{\"name\":\"$legs\",\"values\":[0,1]}],\"default\":{\"$halo\":0,\"$legs\":1}}";
   w.draws={"[{\"start\":0,\"count\":3,\"material\":\"Material0\",\"slot\":0},{\"start\":0,\"count\":3,\"material\":\"Material0\",\"slot\":0,\"when\":{\"vars\":[\"$halo\",\"$legs\"],\"allow\":[[3,1]]}}]"};writeModel(models9+"/W",w);}
  std::filesystem::create_directories(shared9);auto ctl9=[&](const char* vars){write(shared9+"/control.json.tmp",std::string("{\"enabled\":true,\"selected\":[\"W\"],\"variables\":")+vars+"}");std::filesystem::rename(shared9+"/control.json.tmp",shared9+"/control.json");};
  ctl9("{}");Ptr w9=f->renderer(original);f->world(w9);f->scene={w9};
  Runtime v2(e,pkg,root+"/private9",media9,false);v2.tick(900000);v2.tick(900600);Ptr ours=meshOf(w9);auto subs=[&]{return load<uint32_t>(f->obj[ours].shared+0xc0);};
  assert(ours!=original&&subs()==1);
  ctl9("{\"W\":{\"$halo\":3}}");v2.tick(901200);v2.tick(901800);assert(meshOf(w9)==ours&&subs()==2);
  ctl9("{\"W\":{\"$halo\":7}}");v2.tick(902400);v2.tick(903000);assert(meshOf(w9)==ours&&subs()==1);
  // A renderer refusing the new materials puts the whole switch back (the next try goes through).
  f->failMaterials=true;ctl9("{\"W\":{\"$halo\":3}}");for(uint64_t t=903600;f->failMaterials&&t<910000;t+=600)v2.tick(t);
  assert(!f->failMaterials&&subs()==1&&meshOf(w9)==ours);v2.tick(911000);assert(subs()==2);
  v2.shutdown();assert(meshOf(w9)==original&&f->handles.empty());}
 // 21. A set nobody wants any more stays built for a quick switch back, and is released after a minute unused.
 {std::string media10=root+"/media10",shared10=media10+"/"+pkg+"/GIBox",models10=shared10+"/models",logFile=shared10+"/GIBox Log.txt";writeModel(models10+"/A",Spec().withTarget(fixture::target("Body")));
  std::filesystem::create_directories(shared10);auto ctl10=[&](const char* sel){write(shared10+"/control.json.tmp",std::string("{\"enabled\":true,\"selected\":")+sel+"}");std::filesystem::rename(shared10+"/control.json.tmp",shared10+"/control.json");};
  ctl10("[\"A\"]");Ptr w10=f->renderer(original);f->world(w10);f->scene={w10};
  Runtime idle(e,pkg,root+"/private10",media10,false);idle.tick(1000000);idle.tick(1000600);assert(meshOf(w10)!=original);unsigned built=f->meshClones;
  ctl10("[]");idle.tick(1001200);idle.tick(1001800);assert(meshOf(w10)==original&&text(logFile).find("model-released")==std::string::npos);
  ctl10("[\"A\"]");idle.tick(1002400);idle.tick(1003000);assert(meshOf(w10)!=original&&f->meshClones==built);
  ctl10("[]");idle.tick(1003600);idle.tick(1030000);assert(text(logFile).find("model-released")==std::string::npos);idle.tick(1070000);assert(text(logFile).find("model-released A")!=std::string::npos);
  idle.shutdown();assert(f->handles.empty());}
 // 22. A partially loaded character never produces a partial library/export. A verified library is rechecked after
 //     a new request, and both bad captures and failed writes retry until healthy, then stop all capture reads.
 {std::string media11=root+"/media11",shared11=media11+"/"+pkg+"/GIBox",bones11=shared11+"/skeletons",file=bones11+"/10000107/skeleton.json";
  std::filesystem::create_directories(shared11);auto ctl11=[&](int request){write(shared11+"/control.json","{\"enabled\":false,\"selected\":[],\"exportBones\":"+std::to_string(request)+"}");};ctl11(0);
  Ptr bm=f->mesh(),fm=f->mesh("Face"),body=f->renderer(bm),face=f->renderer(fm,"FaceBone","Face");Ptr rs[]={body,face};store<Ptr>(f->owner+0x68,f->array(8,2,rs));
  Ptr faceBinds=f->obj[fm].arrays[abi::getBindposes];store<uint64_t>(faceBinds+0x18,2);
  Runtime complete(e,pkg,root+"/private11",media11,false);
  for(uint64_t t=1100000;t<=1103000;t+=500)complete.tick(t);
  assert(!exists(file)); // Body is good, but Face is not: no successful subset is published.
  ctl11(1);complete.tick(1103500);assert(!exists(file));
  store<uint64_t>(faceBinds+0x18,1);complete.tick(1104000);assert(!exists(file));complete.tick(1104500);complete.tick(1105000);
  auto healthy=text(file);assert(JsonParser(healthy).parse().at("renderers").array.size()==2);
  auto reads=f->bindposeReads;complete.tick(1106000);complete.tick(1107000);assert(f->bindposeReads==reads);
  bool exported=false;for(auto& d:std::filesystem::directory_iterator(bones11))if(d.path().filename().string().rfind("10000107-",0)==0){assert(JsonParser(text(d.path().string()+"/skeleton.json")).parse().at("renderers").array.size()==2);exported=true;}assert(exported);
  // Same renderer objects, a changed skeleton: two bones instead of one; an invalid matrix must keep the good record.
  Ptr ba=f->obj[body].bones,b1=load<Ptr>(ba+0x20),b2=f->object("bone","BoneB");f->obj[b2].parent=b1;Ptr bs[]={b1,b2};f->obj[body].bones=f->array(8,2,bs);
  Bytes bind(128);for(int n=0;n<2;n++)for(int j=0;j<4;j++){float one=1;std::memcpy(bind.data()+n*64+j*20,&one,4);}
  Ptr bindArray=f->array(64,2,bind.data());f->obj[bm].arrays[abi::getBindposes]=bindArray;store<uint64_t>(f->obj[bm].shared+0xe0,2);
  ctl11(2);store<float>(bindArray+0x20,std::numeric_limits<float>::quiet_NaN());complete.tick(1108000);complete.tick(1108500);assert(text(file)==healthy);
  store<float>(bindArray+0x20,1);complete.tick(1109000);assert(text(file)==healthy);complete.tick(1109500);complete.tick(1110000);
  auto updated=text(file);assert(updated!=healthy&&JsonParser(updated).parse().at("renderers").array[0].at("bones").array.size()==2);
  // A failed write is not marked complete; preserve the old record and recover automatically when storage works.
  ctl11(3);std::filesystem::create_directory(file+".tmp");f->obj[b2].name="UpdatedBoneB";complete.tick(1113000);complete.tick(1113500);complete.tick(1114000);assert(text(file)==updated);
  std::filesystem::remove_all(file+".tmp");complete.tick(1114500);complete.tick(1115000);assert(text(file).find("UpdatedBoneB")!=std::string::npos);
  reads=f->bindposeReads;complete.tick(1116000);complete.tick(1120000);assert(f->bindposeReads==reads);
  complete.shutdown();assert(f->handles.empty());}
 // 23. Preserve the 0.7.5 exit: a persistent structural defect may be excluded after 30 s, with one partial warning.
 {std::string mediaPartial=root+"/mediaPartial",sharedPartial=mediaPartial+"/"+pkg+"/GIBox",bonesPartial=sharedPartial+"/skeletons",logFile=sharedPartial+"/GIBox Log.txt";std::filesystem::create_directories(sharedPartial);
  write(sharedPartial+"/control.json","{\"enabled\":false,\"selected\":[],\"exportBones\":0}");
  Ptr good=f->renderer(original),broken=f->renderer(f->mesh(),"BoneA","Cloak");store<uint64_t>(f->obj[f->obj[broken].mesh].arrays[abi::getBindposes]+0x18,2);
  store<Ptr>(f->owner+0x68,f->array(8,2,std::vector<Ptr>{good,broken}.data()));
  Runtime part(e,pkg,root+"/privatePartial",mediaPartial,false);uint64_t t=1100000;
  for(;t<1100000+20000;t+=500){part.tick(t);}assert(!exists(bonesPartial+"/10000107/skeleton.json"));
  for(;t<1100000+40000;t+=500){part.tick(t);}auto lib=text(bonesPartial+"/10000107/skeleton.json");auto log=text(logFile);
  assert(lib.find("\"name\":\"Body\"")!=std::string::npos&&lib.find("Cloak")==std::string::npos&&log.find("runtime WARNING bone-library-partial 10000107: recording the complete renderers without Cloak")!=std::string::npos);
  assert(log.find("bone-library-partial")==log.rfind("bone-library-partial"));auto reads=f->bindposeReads;part.tick(t+1000);part.tick(t+10000);assert(f->bindposeReads==reads);
  part.shutdown();assert(f->handles.empty());}
 // 24. Default-off geometry leaves the original visible. Enabling builds/attaches it; disabling restores both mesh and
 //     materials. The inactive model never takes precedence over a second enabled model on the same part.
 {std::string mediaGate=root+"/mediaGate",sharedGate=mediaGate+"/"+pkg+"/GIBox",modelsGate=sharedGate+"/models";
  Spec g;g.version=3;g.withTarget(fixture::target("Body"));g.variants="{\"vars\":[{\"name\":\"$vision\",\"values\":[0,1]}],\"default\":{\"$vision\":1}}";
  g.edit=[](std::string& j){fixture::replaceIn(j,"\"blob\"","\"replaceWhen\":{\"vars\":[\"$vision\"],\"allow\":[[0]]},\"blob\"");};writeModel(modelsGate+"/G",g);writeModel(modelsGate+"/Other");
  Ptr w=f->renderer(original),mat=f->obj[w].materials;f->world(w);f->scene={w};auto built=f->meshClones;
  auto gateControl=[&](std::string selected,std::string vars){write(sharedGate+"/control.json","{\"enabled\":true,\"selected\":"+selected+",\"variables\":"+vars+"}");};
  gateControl("[\"G\"]","{}");Runtime gate(e,pkg,root+"/privateGate",mediaGate,false);gate.tick(1300000);gate.tick(1300600);
  assert(meshOf(w)==original&&f->obj[w].materials==mat&&f->meshClones==built);
  assert(JsonParser(text(sharedGate+"/status.json")).parse().at("detail").string.find("显示替换已关闭")!=std::string::npos);
  gateControl("[\"G\"]","{\"G\":{\"$vision\":0}}");gate.tick(1301200);gate.tick(1301800);Ptr replaced=meshOf(w);assert(replaced!=original&&f->obj[w].materials!=mat&&f->meshClones==built+1);
  gateControl("[\"G\"]","{\"G\":{\"$vision\":1}}");gate.tick(1302400);assert(meshOf(w)==original&&f->obj[w].materials==mat);
  gateControl("[\"G\",\"Other\"]","{}");gate.tick(1303000);gate.tick(1303600);assert(meshOf(w)!=original&&meshOf(w)!=replaced&&JsonParser(text(sharedGate+"/status.json")).parse().at("models").array[0].string=="Other");
  gate.shutdown();assert(f->handles.empty());}
 // 25. Index-only MOD needs two bones, but cached targets and this character's template have one. Do not build, latch a
 //     failure, or learn another character's larger Body mesh. The SAME entity can finish loading later and be retried.
 {std::string mediaMin=root+"/mediaMin",sharedMin=mediaMin+"/"+pkg+"/GIBox",modelsMin=sharedMin+"/models",dir=modelsMin+"/Indexed";
  Spec s;s.noSkeleton().withTarget(fixture::target("Body"));s.edit=[](std::string& j){fixture::replaceIn(j,"\"bones\":1,\"vertices\"","\"bones\":2,\"vertices\"");};for(int i=0;i<3;i++){put32(s.weights.data()+32*i+16,1);}writeModel(dir,s);
  Learned stale;stale.targets["Part0"]={"Body","Body",3,1,1};write(dir+"/learned.json",learnedJson(stale));
  auto twoBones=[&](Ptr r){auto mesh=f->obj[r].mesh;Ptr b1=load<Ptr>(f->obj[r].bones+0x20),b2=f->object("bone","BoneB");f->obj[b2].parent=b1;Ptr bs[]={b1,b2};f->obj[r].bones=f->array(8,2,bs);Bytes binds(128);for(int n=0;n<2;n++)for(int j=0;j<4;j++)store<float>(reinterpret_cast<Ptr>(binds.data())+n*64+j*20,1);f->obj[mesh].arrays[abi::getBindposes]=f->array(64,2,binds.data());store<uint64_t>(f->obj[mesh].shared+0xe0,2);};
  Ptr mesh=f->mesh(),w=f->renderer(mesh),foreign=f->renderer(f->mesh("Body",4),"BoneA","Body",f->kavehRoot);twoBones(foreign);f->world(w);f->scene={w,foreign};auto foreignMesh=meshOf(foreign);auto built=f->meshClones;
  write(sharedMin+"/control.json","{\"enabled\":true,\"selected\":[\"Indexed\"]}");Runtime min(e,pkg,root+"/privateMin",mediaMin,false);
  for(uint64_t t=1400000;t<=1403000;t+=500){min.tick(t);}auto st=JsonParser(text(sharedMin+"/status.json")).parse();
  assert(st.at("state").string=="waiting"&&st.at("detail").string.find("至少 2 根骨骼")!=std::string::npos&&meshOf(w)==mesh&&meshOf(foreign)==foreignMesh&&f->meshClones==built);
  auto log=text(sharedMin+"/GIBox Log.txt");assert(log.find("build-retry")==std::string::npos&&log.find("spawn-build-error")==std::string::npos);
  twoBones(w);for(uint64_t t=1403500;t<=1406000;t+=500){min.tick(t);}
  assert(meshOf(w)!=mesh&&meshOf(foreign)==foreignMesh&&learnedAt(dir).targets.at("Part0").bones==2&&f->meshClones==built+1);
  min.shutdown();assert(f->handles.empty());}
 // 26. A part whose renderer is not on the character is looked for again every scan, but logged once.
 {std::string mediaMiss=root+"/mediaMiss",sharedMiss=mediaMiss+"/"+pkg+"/GIBox";Spec s;s.edit=[](std::string& j){fixture::replaceIn(j,"\"renderer\":\"Body\"","\"renderer\":\"Cloak\"");};writeModel(sharedMiss+"/models/Miss",s);
  Ptr w=f->renderer(original);f->world(w);f->scene={w};write(sharedMiss+"/control.json","{\"enabled\":true,\"selected\":[\"Miss\"]}");
  Runtime miss(e,pkg,root+"/privateMiss",mediaMiss,false);for(uint64_t t=1500000;t<=1505000;t+=500){miss.tick(t);}
  auto log=text(sharedMiss+"/GIBox Log.txt");auto at=log.find("target-missing Miss/Part0: renderer match missing: Cloak");assert(at!=std::string::npos&&log.find("target-missing",at+1)==std::string::npos&&meshOf(w)==original);
  miss.shutdown();assert(f->handles.empty());}
 // 27. Phone mesh with fewer materials than the mod's parts (Shenhe: 4 parts, 3 submeshes): the extra slot borrows the
 //     last material's shader. Clones are taken again after the switch-in effect (twice), adopted copies follow.
 //     A built model goes onto the controlled character's lower-LOD mesh of the same asset when the bones are the same.
 {std::string mediaSlot=root+"/mediaSlot",sharedSlot=mediaSlot+"/"+pkg+"/GIBox";Spec s;s.withTarget(fixture::target("Body"));s.textures=true;
  s.draws={"[{\"start\":0,\"count\":3,\"material\":\"Material0\",\"slot\":0},{\"start\":0,\"count\":3,\"material\":\"Material0\",\"slot\":1}]"};writeModel(sharedSlot+"/models/Slot",s);
  Ptr full=f->mesh(),w=f->renderer(full);f->world(w);f->scene={w};write(sharedSlot+"/control.json","{\"enabled\":true,\"selected\":[\"Slot\"]}");
  Runtime slot(e,pkg,root+"/privateSlot",mediaSlot,false);slot.tick(1600000);slot.tick(1600600);slot.tick(1601200);
  assert(meshOf(w)!=full);Ptr first=f->obj[w].materials;assert(load<uint64_t>(first+0x18)==2);Ptr c0=load<Ptr>(first+0x20),c1=load<Ptr>(first+0x28);assert(c0!=c1&&alive(c0)&&alive(c1));
  Ptr copy=f->instantiate(w);f->scene={w,copy};slot.tick(1601800);assert(meshOf(copy)==meshOf(w)&&f->obj[copy].materials==first);
  slot.tick(1602800);Ptr second=f->obj[w].materials;assert(second!=first&&load<Ptr>(second+0x20)!=c0&&load<Ptr>(second+0x28)!=c1&&alive(c0));
  slot.tick(1605400);assert(f->obj[w].materials!=second);auto third=f->obj[w].materials;assert(load<Ptr>(f->obj[copy].materials+0x20)==load<Ptr>(third+0x20));
  slot.tick(1609000);assert(f->obj[w].materials==third);
  auto log=text(sharedSlot+"/GIBox Log.txt");assert(log.find("material-slot-borrowed Slot/Part0: slot 1 has no game material")!=std::string::npos&&log.find("material-slot-borrowed")==log.rfind("material-slot-borrowed"));
  // The same character now shows only Body_LOD1 (same bone names): ours goes on, and the LOD comes back on restore.
  Ptr lodMesh=f->mesh("Body_LOD1",2),lod=f->renderer(lodMesh);f->world(lod);f->scene={w,copy,lod};slot.tick(1609600);slot.tick(1610200);
  assert(meshOf(lod)==meshOf(w)&&text(sharedSlot+"/GIBox Log.txt").find("attached-lod Slot/Part0 over Body_LOD1")!=std::string::npos);
  Ptr other=f->renderer(f->mesh("Body_LOD1",2),"BoneZ");f->scene={w,copy,lod,other};f->world(other);slot.tick(1610800);slot.tick(1611400);assert(meshOf(other)!=meshOf(w));
  write(sharedSlot+"/control.json","{\"enabled\":false,\"selected\":[\"Slot\"]}");slot.tick(1612000);assert(meshOf(lod)==lodMesh&&meshOf(w)==full);
  slot.shutdown();assert(f->handles.empty());}
 // 28. Only Body_LOD1 in sight from the start: built on it when its bones and bindposes are exactly the bone library's
 //     record of the full Body; without that record it waits as before.
 {std::string mediaLod=root+"/mediaLod",sharedLod=mediaLod+"/"+pkg+"/GIBox";Spec s;s.withTarget(fixture::target("Body"));writeModel(sharedLod+"/models/Lod",s);
  Ptr lodMesh=f->mesh("Body_LOD1",2),lod=f->renderer(lodMesh);f->world(lod);f->scene={lod};write(sharedLod+"/control.json","{\"enabled\":true,\"selected\":[\"Lod\"]}");
  Runtime first(e,pkg,root+"/privateLod",mediaLod,false);for(uint64_t t=1700000;t<=1702000;t+=500){first.tick(t);}assert(meshOf(lod)==lodMesh);first.shutdown();
  SkeletonRenderer rec;rec.name="Body";rec.target=fingerprintOf(JsonParser(fixture::target("Body")).parse());rec.bones={"BoneA"};rec.parents={-1};
  rec.bindposes.assign(reinterpret_cast<const uint8_t*>(f->obj[lodMesh].arrays[abi::getBindposes]+0x20),reinterpret_cast<const uint8_t*>(f->obj[lodMesh].arrays[abi::getBindposes]+0x20+64));
  std::filesystem::create_directories(sharedLod+"/skeletons/10000107");write(sharedLod+"/skeletons/10000107/skeleton.json",skeletonJson(10000107,"Scene/Avatar_Girl_Catalyst_Citlali",{rec}));
  Runtime second(e,pkg,root+"/privateLod2",mediaLod,false);for(uint64_t t=1710000;t<=1713000;t+=500){second.tick(t);}
  auto log=text(sharedLod+"/GIBox Log.txt");assert(meshOf(lod)!=lodMesh&&log.find("template-lod Lod: Body only shows Body_LOD1")!=std::string::npos&&log.find("attached-lod Lod/Part0 over Body_LOD1")!=std::string::npos);
  second.shutdown();assert(meshOf(lod)==lodMesh&&f->handles.empty());}
 // 29. A preview's game code instantiates our materials (new pointers, same mod texture). Every kind of off switch
 //     must restore the saved original mesh AND texture. A reclaim must not overwrite the clean material backup.
 for(int mode=0;mode<3;mode++){
  std::string mediaRestore=root+"/mediaRestore"+std::to_string(mode),sharedRestore=mediaRestore+"/"+pkg+"/GIBox";Spec s;s.version=3;s.textures=true;s.withTarget(fixture::target("Body"));
  s.variants="{\"vars\":[{\"name\":\"$show\",\"values\":[0,1]}],\"default\":{\"$show\":1}}";s.edit=[](std::string& j){fixture::replaceIn(j,"\"blob\"","\"replaceWhen\":{\"vars\":[\"$show\"],\"allow\":[[1]]},\"blob\"");};writeModel(sharedRestore+"/models/R",s);
  Ptr originalMesh=f->mesh(),w=f->renderer(originalMesh),preview=f->renderer(originalMesh),saved=f->obj[preview].materials,baseMat=load<Ptr>(saved+0x20),gameTexture=f->object("texture","Game diffuse");f->diffuse[baseMat]=gameTexture;f->world(w);f->scene={w,preview};
  auto ctlRestore=[&](bool enabled,bool selected,int show){write(sharedRestore+"/control.json","{\"enabled\":"+std::string(enabled?"true":"false")+",\"selected\":"+(selected?"[\"R\"]":"[]")+",\"variables\":{\"R\":{\"$show\":"+std::to_string(show)+"}}}");};
  ctlRestore(true,true,1);Runtime paired(e,pkg,root+"/privateRestore"+std::to_string(mode),mediaRestore,false);paired.tick(1800000);paired.tick(1800600);assert(meshOf(preview)!=originalMesh);
  Ptr modMat=load<Ptr>(f->obj[preview].materials+0x20),gameCopy=clone(modMat,0);assert(f->diffuse[gameCopy]!=gameTexture);f->obj[preview].materials=f->typed("material",gameCopy);
  if(mode>0)f->obj[preview].mesh=originalMesh; // game has already put the original geometry back
  if(mode==2){paired.tick(1801200);assert(meshOf(preview)!=originalMesh);} // reclaim sees unfamiliar mod-textured materials
  ctlRestore(mode!=0,mode!=1,mode==2?0:1);paired.tick(1801800);
  assert(meshOf(preview)==originalMesh&&f->obj[preview].materials==saved&&f->diffuse[load<Ptr>(f->obj[preview].materials+0x20)]==gameTexture);
  assert(text(sharedRestore+"/GIBox Log.txt").find("restored R/Part0")!=std::string::npos);paired.shutdown();assert(f->handles.empty());}
 // 30. The game copies our material array while supplying an original mesh. Resolve known clone sources before
 //     taking the backup, so switching off cannot restore mod textures or a clone that its old owner will destroy.
 {std::string mediaAlias=root+"/mediaAlias",sharedAlias=mediaAlias+"/"+pkg+"/GIBox";Spec s;s.textures=true;s.withTarget(fixture::target("Body"));s.draws={"[{\"start\":0,\"count\":3,\"material\":\"Material0\",\"slot\":0},{\"start\":0,\"count\":3,\"material\":\"Material0\",\"slot\":1}]"};writeModel(sharedAlias+"/models/Alias",s);
  Ptr originalMesh=f->mesh(),w=f->renderer(originalMesh),saved=f->obj[w].materials,baseMat=load<Ptr>(saved+0x20),gameTexture=f->object("texture","Original diffuse");f->diffuse[baseMat]=gameTexture;f->world(w);f->scene={w};write(sharedAlias+"/control.json","{\"enabled\":true,\"selected\":[\"Alias\"]}");
  Runtime alias(e,pkg,root+"/privateAlias",mediaAlias,false);alias.tick(1900000);alias.tick(1900600);Ptr preview=f->renderer(originalMesh);f->obj[preview].materials=f->obj[w].materials;alias.tick(1901800);f->scene.push_back(preview);Ptr go=f->gameObjectOf({preview});alias.spawned(go);alias.spawnedAfter();
  assert(meshOf(preview)==meshOf(w)&&text(sharedAlias+"/GIBox Log.txt").find("material-backup-cleaned Alias/Part0")!=std::string::npos);
  write(sharedAlias+"/control.json","{\"enabled\":false,\"selected\":[\"Alias\"]}");alias.tick(1902400);assert(meshOf(preview)==originalMesh&&load<uint64_t>(f->obj[preview].materials+0x18)==1&&load<Ptr>(f->obj[preview].materials+0x20)==baseMat&&f->diffuse[baseMat]==gameTexture);alias.shutdown();assert(f->handles.empty());}
 // 31. Destroy the source before the first delayed refresh. Its material heir (and a second descendant) must finish
 //     both refreshes and shed a simulated temporary material parameter, not only keep the old clones alive.
 {std::string mediaHeir=root+"/mediaHeir",sharedHeir=mediaHeir+"/"+pkg+"/GIBox";Spec s;s.textures=true;s.withTarget(fixture::target("Body"));writeModel(sharedHeir+"/models/Heir",s);
  Ptr originalMesh=f->mesh(),source=f->renderer(originalMesh),saved=f->obj[source].materials,baseMat=load<Ptr>(saved+0x20);f->glow[baseMat]=1;f->world(source);f->scene={source};write(sharedHeir+"/control.json","{\"enabled\":true,\"selected\":[\"Heir\"]}");
  Runtime heir(e,pkg,root+"/privateHeir",mediaHeir,false);heir.tick(2000000);Ptr firstMat=load<Ptr>(f->obj[source].materials+0x20);assert(f->glow[firstMat]==1);
  Ptr child=f->instantiate(source);f->world(child);f->scene={source,child};heir.tick(2000600);store<Ptr>(source+0x10,0);f->scene={child};f->glow[baseMat]=0;heir.tick(2001000);
  Ptr grandchild=f->instantiate(child);f->scene.push_back(grandchild);heir.tick(2001200);assert(load<Ptr>(f->obj[child].materials+0x20)==firstMat);
  heir.tick(2001800);Ptr secondMat=load<Ptr>(f->obj[child].materials+0x20);assert(secondMat!=firstMat&&f->glow[secondMat]==0&&load<Ptr>(f->obj[grandchild].materials+0x20)==secondMat);
  heir.tick(2004400);Ptr thirdMat=load<Ptr>(f->obj[child].materials+0x20);assert(thirdMat!=secondMat&&f->glow[thirdMat]==0&&load<Ptr>(f->obj[grandchild].materials+0x20)==thirdMat);heir.tick(2009000);assert(load<Ptr>(f->obj[child].materials+0x20)==thirdMat);
  auto heirLog=text(sharedHeir+"/GIBox Log.txt");assert(heirLog.find("material-state Heir/Part0 early-sample")!=std::string::npos&&heirLog.find("material-refresh Heir: attempt 1 applied")!=std::string::npos&&heirLog.find("material-refresh Heir: attempt 2 applied")!=std::string::npos);
  heir.shutdown();assert(meshOf(child)==originalMesh&&meshOf(grandchild)==originalMesh&&f->handles.empty());}
 // 32. A present but destroyed game material is a loading/lifetime problem, not an extra PC part. Do not borrow a
 //     different slot's shader for it; wait without changing the original pair, then retry on the SAME renderer.
 {std::string mediaDead=root+"/mediaDead",sharedDead=mediaDead+"/"+pkg+"/GIBox";Spec s;s.withTarget(fixture::target("Body"));s.draws={"[{\"start\":0,\"count\":3,\"material\":\"Material0\",\"slot\":0},{\"start\":0,\"count\":3,\"material\":\"Material0\",\"slot\":1}]"};writeModel(sharedDead+"/models/Dead",s);
  Ptr originalMesh=f->mesh(),w=f->renderer(originalMesh),first=load<Ptr>(f->obj[w].materials+0x20),dead=f->object("material","Not ready");store<Ptr>(dead+0x10,0);Ptr ms[]={first,dead};Ptr saved=f->array(8,2,ms);f->obj[w].materials=saved;f->world(w);f->scene={w};write(sharedDead+"/control.json","{\"enabled\":true,\"selected\":[\"Dead\"]}");
  Runtime missing(e,pkg,root+"/privateDead",mediaDead,false);missing.tick(2100000);missing.tick(2100600);auto log=text(sharedDead+"/GIBox Log.txt");assert(meshOf(w)==originalMesh&&f->obj[w].materials==saved&&log.find("material-wait Dead:")!=std::string::npos&&log.find("material-wait")==log.rfind("material-wait")&&log.find("material-slot-borrowed")==std::string::npos);
  assert(JsonParser(text(sharedDead+"/status.json")).parse().at("detail").string.find("原材质槽 1 尚未就绪")!=std::string::npos);
  Ptr ready=f->object("material","Ready");ms[1]=ready;Ptr complete=f->array(8,2,ms);f->obj[w].materials=complete;missing.tick(2101200);assert(meshOf(w)!=originalMesh);
  missing.shutdown();assert(meshOf(w)==originalMesh&&f->obj[w].materials==complete&&f->handles.empty());}
 // 33. A party-effect material copy has a new identity. Bounded observations must report that identity and a skipped
 //     refresh without altering it; the trace cannot claim to observe (or fix) shader parameters/property blocks.
 {std::string mediaTrace=root+"/mediaTrace",sharedTrace=mediaTrace+"/"+pkg+"/GIBox";Spec s;s.textures=true;s.withTarget(fixture::target("Body"));writeModel(sharedTrace+"/models/Trace",s);
  Ptr originalMesh=f->mesh(),w=f->renderer(originalMesh),saved=f->obj[w].materials;f->world(w);f->scene={w};write(sharedTrace+"/control.json","{\"enabled\":true,\"selected\":[\"Trace\"]}");
  Runtime trace(e,pkg,root+"/privateTrace",mediaTrace,false);trace.tick(2200000);Ptr copy=clone(load<Ptr>(f->obj[w].materials+0x20),0);Ptr copied=f->array(8,1,&copy);f->obj[w].materials=copied;
  trace.tick(2200600);assert(f->obj[w].materials==copied);trace.tick(2201800);assert(f->obj[w].materials==copied);
  auto traceLog=text(sharedTrace+"/GIBox Log.txt");assert(traceLog.find("material-state Trace/Part0 early-sample")!=std::string::npos&&traceLog.find("current=[0=untracked:")!=std::string::npos&&traceLog.find("attempt 1 skipped (current materials are not the owned clones)")!=std::string::npos);
  trace.shutdown();assert(meshOf(w)==originalMesh&&f->obj[w].materials==saved&&f->handles.empty());}
 // 34. Party hand-off instantiates each draw's material (two draws sharing one base slot become two instances).
 //     Repair that recognized palette early, cap early attempts, retain the delayed recovery and original palette.
 {std::string mediaCopy=root+"/mediaCopy",sharedCopy=mediaCopy+"/"+pkg+"/GIBox";Spec s;s.textures=true;s.withTarget(fixture::target("Body"));s.indices={0,1,2,2,1,0};s.draws={"[{\"start\":0,\"count\":3,\"material\":\"Material0\",\"slot\":0},{\"start\":3,\"count\":3,\"material\":\"Material0\",\"slot\":0}]"};writeModel(sharedCopy+"/models/Copy",s);
  Ptr originalMesh=f->mesh(),w=f->renderer(originalMesh),saved=f->obj[w].materials,baseMat=load<Ptr>(saved+0x20);f->obj[baseMat].name="Avatar_Mat_Body";f->glow[baseMat]=0;f->world(w);f->scene={w};write(sharedCopy+"/control.json","{\"enabled\":true,\"selected\":[\"Copy\"]}");
  Runtime copy(e,pkg,root+"/privateCopy",mediaCopy,false);copy.tick(2300000);assert(load<uint64_t>(f->obj[w].materials+0x18)==2);Ptr ourMesh=meshOf(w),texture=f->diffuse[load<Ptr>(f->obj[w].materials+0x20)];std::vector<Ptr> gameCopies;
  auto instantiatePalette=[&]{Ptr list[2];for(unsigned i=0;i<2;i++){list[i]=clone(load<Ptr>(f->obj[w].materials+0x20+8*i),0);f->obj[list[i]].name="Avatar_Mat_Body(Clone) (Instance) #"+std::to_string(i);f->glow[list[i]]=1;gameCopies.push_back(list[i]);}Ptr arr=f->array(8,2,list);f->obj[w].materials=arr;return arr;};
  for(uint64_t now:{2300050ull,2300100ull}){Ptr gameArray=instantiatePalette();copy.tick(now);assert(f->obj[w].materials!=gameArray&&meshOf(w)==ourMesh);for(Ptr m:{load<Ptr>(f->obj[w].materials+0x20),load<Ptr>(f->obj[w].materials+0x28)}){assert(f->glow[m]==0&&f->diffuse[m]==texture);}}
  Ptr third=instantiatePalette();copy.tick(2300150);assert(f->obj[w].materials==third);copy.tick(2301800);assert(f->obj[w].materials!=third&&f->glow[load<Ptr>(f->obj[w].materials+0x20)]==0);
  auto copyLog=text(sharedCopy+"/GIBox Log.txt");assert(copyLog.find("material-reclaimed Copy/Part0")!=std::string::npos&&copyLog.find("attempt 1 applied")!=std::string::npos&&copyLog.find("skipped (current materials are not the owned clones)")==std::string::npos);
  copy.shutdown();assert(meshOf(w)==originalMesh&&f->obj[w].materials==saved&&f->handles.empty());for(Ptr m:gameCopies){assert(alive(m));}}
 // 35. Similar names by themselves are insufficient: wrong row count, another material stem and an unrelated mesh
 //     must all remain untouched by the early guard.
 for(int mode=0;mode<3;mode++){
  std::string mediaGuard=root+"/mediaGuard"+std::to_string(mode),sharedGuard=mediaGuard+"/"+pkg+"/GIBox";Spec s;s.textures=true;s.withTarget(fixture::target("Body"));writeModel(sharedGuard+"/models/Guard",s);
  Ptr originalMesh=f->mesh(),w=f->renderer(originalMesh),baseMat=load<Ptr>(f->obj[w].materials+0x20);f->obj[baseMat].name="Avatar_Mat_Body";f->world(w);f->scene={w};write(sharedGuard+"/control.json","{\"enabled\":true,\"selected\":[\"Guard\"]}");
  Runtime guard(e,pkg,root+"/privateGuard"+std::to_string(mode),mediaGuard,false);guard.tick(2400000);Ptr gameCopy=clone(load<Ptr>(f->obj[w].materials+0x20),0);f->obj[gameCopy].name=(mode==1?"Different_Mat":"Avatar_Mat_Body")+std::string("(Clone) (Instance) #0");Ptr pair[]={gameCopy,gameCopy};Ptr array=f->array(8,mode==0?2:1,pair);f->obj[w].materials=array;
  Ptr current=mode==2?f->mesh("UnrelatedBody"):meshOf(w);f->obj[w].mesh=current;guard.tick(2400050);assert(f->obj[w].materials==array&&meshOf(w)==current);guard.shutdown();assert(alive(gameCopy)&&f->handles.empty());}
 // 36. A character-scoped request survives waiting, retries incomplete captures without overwriting the last good
 //     record, completes only after worker read-back, rejects stale completions and is acknowledged across restart.
 {std::string mediaRefresh=root+"/mediaRefresh",sharedRefresh=mediaRefresh+"/"+pkg+"/GIBox",bonesRefresh=sharedRefresh+"/skeletons";Ptr originalMesh=f->mesh(),w=f->renderer(originalMesh),completeBones=f->obj[w].bones;f->world(w);f->scene={w};
  Ptr entity=load<Ptr>(load<Ptr>(load<Ptr>(f->manager+0x10)+0x10)+0x20);store<uint32_t>(entity+0x3a4,10000200);
  SkeletonRenderer rec;rec.name="Body";rec.target={"Body","Body",3,1,1};rec.bones={"BoneA"};rec.parents={-1};rec.bindposes.resize(64);for(int i=0;i<4;i++){float one=1;std::memcpy(rec.bindposes.data()+i*20,&one,4);}
  auto old=skeletonJson(10000107,"OldCapture",{rec});std::string file=bonesRefresh+"/10000107/skeleton.json";std::filesystem::create_directories(bonesRefresh+"/10000107");write(file,old);
  auto request=[&](long long n){write(sharedRefresh+"/control.json","{\"enabled\":false,\"selected\":[],\"refreshBones\":"+std::to_string(n)+",\"refreshBonesCharacter\":10000107}");};
  request(901);
  {Runtime refresh(e,pkg,root+"/privateRefresh",mediaRefresh,false);auto st=[&]{return JsonParser(text(sharedRefresh+"/status.json")).parse();};
   refresh.tick(2500000);assert(st().at("boneRefreshState").string=="waiting-character"&&text(file)==old);
   store<uint32_t>(entity+0x3a4,10000107);f->obj[w].bones=f->array(8,0);refresh.tick(2500600);refresh.tick(2501200);assert(st().at("boneRefreshState").string!="complete"&&text(file)==old);
   f->obj[w].bones=completeBones;refresh.tick(2501800);refresh.tick(2502400);assert(st().at("boneRefreshState").string=="saving");refresh.tick(2502500);assert(st().at("boneRefreshState").string=="complete"&&text(file)!=old);
   auto reads=f->bindposeReads;refresh.tick(2503100);assert(f->bindposeReads==reads);
   request(902);refresh.tick(2503700);refresh.tick(2504300);request(903);refresh.tick(2504400);assert(st().at("boneRefreshRequest").number==903&&st().at("boneRefreshState").string!="complete");
   refresh.tick(2505000);refresh.tick(2505100);assert(st().at("boneRefreshState").string=="complete");refresh.shutdown();}
  {Runtime restarted(e,pkg,root+"/privateRefreshRestart",mediaRefresh,false);restarted.tick(2510000);auto st=JsonParser(text(sharedRefresh+"/status.json")).parse();assert(st.at("boneRefreshRequest").number==903&&st.at("boneRefreshState").string=="complete");restarted.shutdown();}
  // A request queued while the game is not running is NOT discarded on first control load.
  request(904);{Runtime queued(e,pkg,root+"/privateRefreshQueued",mediaRefresh,false);queued.tick(2520000);queued.tick(2520600);queued.tick(2520700);auto st=JsonParser(text(sharedRefresh+"/status.json")).parse();assert(st.at("boneRefreshRequest").number==904&&st.at("boneRefreshState").string=="complete");queued.shutdown();}
  // Rewriting the library later (including automatic captures after an update) must not replay a completed request.
  // Also accept a legacy 0.7.10 acknowledgement carrying an obsolete CRC: the operation token remains authoritative.
  write(file,old);write(sharedRefresh+"/bone-refresh-result.json","{\"request\":904,\"character\":10000107,\"crc32\":0}");
  {Runtime rewritten(e,pkg,root+"/privateRefreshRewritten",mediaRefresh,false);rewritten.tick(2530000);auto done=JsonParser(text(sharedRefresh+"/status.json")).parse();assert(done.at("boneRefreshRequest").number==904&&done.at("boneRefreshState").string=="complete");rewritten.shutdown();}
  // An acknowledgement belonging to another character must not suppress a real request with the same token.
  write(sharedRefresh+"/bone-refresh-result.json","{\"request\":904,\"character\":10000200}");
  {Runtime wrongCharacter(e,pkg,root+"/privateRefreshWrongCharacter",mediaRefresh,false);wrongCharacter.tick(2540000);auto pending=JsonParser(text(sharedRefresh+"/status.json")).parse();assert(pending.at("boneRefreshState").string!="complete");wrongCharacter.tick(2540600);wrongCharacter.tick(2540700);auto done=JsonParser(text(sharedRefresh+"/status.json")).parse();assert(done.at("boneRefreshState").string=="complete");wrongCharacter.shutdown();}
  assert(f->handles.empty());}
 std::filesystem::remove_all(root);
 std::cout<<"Native runtime: PASS (structural capture exit after 30 s preserved, replacement-off restores originals and says so, missing renderer logged once, extra material slots borrow a shader, paired restoration after game material copies, clean palette backups, inherited refresh schedule, temporary material readiness, clones retaken after switch-in, lower-LOD world mesh with the same bones (attach, and build checked against the bone library), indexed skeleton bounds and retry, idle sets released after a minute, version 2 conditional draws switched in place, unverified lower LODs never recorded or built against, bone export only on request, wait reasons in status and log, GIBox model folders and learned.json, skeleton.json library (quiet while loading, reasons once), first-switched-on precedence for two mods of one character, manual import by ID + model name, re-import for another character, XXMI log lines and levels,fingerprint matching, detail-page/asset-reload followers, NPC/skeleton guards, multi-model, idle cache, hot reload, broken-edit fallback, variants, name-matched models, async hand-off attach/build, game mesh put back (hand-off, LOD), game rebinding answered with ours, no magenta after drops, adoption, clone hand-over, update retargeting, bone library, Type from this_arg pairs or array classes, scan-off fallback, failure latch, rollback, GC release, threaded worker)\n";
}
