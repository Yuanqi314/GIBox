#pragma once
#include "runtime.hpp"
#include "logfmt.hpp"
#include <array>
#include <deque>
#include <optional>

namespace ms::renderprobe {
using State=std::map<std::string,std::string>;
inline std::string missing(const std::string& why){return "未采集: "+why;}
inline std::string pointer(Ptr p){std::ostringstream s;s<<"0x"<<std::hex<<p;return s.str();}
inline std::string number(float f){if(!std::isfinite(f))return "invalid-nonfinite";std::ostringstream s;s.precision(9);s<<f;return s.str();}
inline std::string fields(const State& values){std::string s="{";for(auto& x:values){if(s.size()>1)s+=",";s+=quote(x.first)+":"+quote(x.second);}return s+"}";}

// Read via typed managed methods, rather than calling a resolved icall with a guessed native ABI.
// Every new engine entry is found by exported name, every overload by parameter AND return type.
// No offsets, memory-pattern guesses, il2cpp class-layout guesses, or setter fallbacks here.
class UnityReader {
 std::function<Ptr(const char*)> lookup;
 bool attempted=false;std::string unavailable;
 using Domain=Ptr(*)();using Assemblies=const Ptr*(*)(Ptr,size_t*);using Image=Ptr(*)(Ptr);
 using Class=Ptr(*)(Ptr,const char*,const char*);using Methods=Ptr(*)(Ptr,void**);
 using Name=const char*(*)(Ptr);using Count=uint32_t(*)(Ptr);using Param=Ptr(*)(Ptr,uint32_t);
 using TypeName=char*(*)(Ptr);using Free=void(*)(void*);using Invoke=Ptr(*)(Ptr,Ptr,void**,Ptr*);
 using New=Ptr(*)(Ptr);using Unbox=void*(*)(Ptr);using Pin=uint32_t(*)(Ptr,bool);using Unpin=void(*)(uint32_t);
 using StringLength=int32_t(*)(Ptr);using StringChars=const uint16_t*(*)(Ptr);using ArrayLength=uintptr_t(*)(Ptr);
 using ObjectClass=Ptr(*)(Ptr);using ClassParent=Ptr(*)(Ptr);using ClassText=const char*(*)(Ptr);
 Domain domain{};Assemblies assemblies{};Image image{};Class klass{};Methods methods{};Name methodName{};
 Count paramCount{};Param param{};Image returnType{};TypeName typeName{};Free release{};Invoke invoke{};
 New objectNew{};Unbox unbox{};Pin pin{};Unpin unpin{};StringLength stringLength{};StringChars stringChars{};ArrayLength arrayLength{};
 ObjectClass objectClass{};ClassParent classParent{};ClassText className{},classNamespace{};std::map<std::string,Ptr> inheritedCache;
 std::map<std::string,Ptr> classes;std::map<std::string,Ptr> methodCache;
 struct Property {int id,type;std::string name;};
 struct Schema {Ptr shader=0;uint32_t root=0;std::string key;std::vector<Property> properties;};
 std::map<Ptr,Schema> schemas;std::map<std::string,std::string> schemaText;
 template<class T>void symbol(T& p,const char* n,bool required=true){p=reinterpret_cast<T>(lookup?lookup(n):0);if(required&&!p&&unavailable.empty())unavailable=std::string("missing export ")+n;}
 bool init(){if(attempted)return unavailable.empty();attempted=true;
  symbol(domain,"il2cpp_domain_get");symbol(assemblies,"il2cpp_domain_get_assemblies");symbol(image,"il2cpp_assembly_get_image");
  symbol(klass,"il2cpp_class_from_name");symbol(methods,"il2cpp_class_get_methods");symbol(methodName,"il2cpp_method_get_name");
  symbol(paramCount,"il2cpp_method_get_param_count");symbol(param,"il2cpp_method_get_param");symbol(returnType,"il2cpp_method_get_return_type");
  symbol(typeName,"il2cpp_type_get_name");symbol(release,"il2cpp_free");symbol(invoke,"il2cpp_runtime_invoke");
  symbol(objectNew,"il2cpp_object_new",false);symbol(unbox,"il2cpp_object_unbox");symbol(pin,"il2cpp_gchandle_new");symbol(unpin,"il2cpp_gchandle_free");
  symbol(stringLength,"il2cpp_string_length");symbol(stringChars,"il2cpp_string_chars");symbol(arrayLength,"il2cpp_array_length",false);
  // Optional: the object's own class and its parents, so a method is looked up on the class the object really has.
  symbol(objectClass,"il2cpp_object_get_class",false);symbol(classParent,"il2cpp_class_get_parent",false);
  symbol(className,"il2cpp_class_get_name",false);symbol(classNamespace,"il2cpp_class_get_namespace",false);
  return unavailable.empty();}
 struct Roots {UnityReader& r;std::vector<uint32_t> handles;
  explicit Roots(UnityReader& x):r(x){}~Roots(){for(auto h:handles)r.unpin(h);}
  Ptr add(Ptr p){if(p){auto h=r.pin(p,true);require(h!=0,"readback GC root failed");try{handles.push_back(h);}catch(...){r.unpin(h);throw;}}return p;}};
 std::string type(Ptr p){char* v=typeName(p);require(v,"type name unavailable");std::string s(v);release(v);return s;}
 Ptr classOf(const std::string& name){auto old=classes.find(name);if(old!=classes.end())return old->second;
  size_t n=0;Ptr d=domain();require(d!=0,"IL2CPP domain unavailable");const Ptr* list=assemblies(d,&n);require(list&&n<=1024,"assembly list unavailable");
  Ptr found=0;for(size_t i=0;i<n;i++){Ptr c=klass(image(list[i]),"UnityEngine",name.c_str());if(c){require(!found||found==c,"ambiguous Unity class "+name);found=c;}}
  classes[name]=found;return found;}
 Ptr method(const std::string& cls,const std::string& name,const std::vector<std::string>& args,const std::string& result){
  auto key=cls+"::"+name+"->"+result;for(auto& a:args)key+="|"+a;
  auto old=methodCache.find(key);if(old!=methodCache.end())return old->second;
  Ptr c=classOf(cls),found=0;if(c){void* it=nullptr;unsigned bounded=0;
   while(Ptr m=methods(c,&it)){require(++bounded<=4096,"Unity method enumeration bound");const char* n=methodName(m);
    if(!n||name!=n||paramCount(m)!=args.size()||type(returnType(m))!=result)continue;
    bool same=true;for(size_t i=0;i<args.size();i++)same=same&&type(param(m,uint32_t(i)))==args[i];
    if(same){require(!found,"ambiguous Unity method "+key);found=m;}}}
  methodCache[key]=found;return found;}
 Ptr call(Roots& roots,const std::string& cls,const std::string& name,Ptr self,
          const std::vector<std::string>& types,const std::string& result,std::vector<void*> args={}){
  Ptr m=method(cls,name,types,result);require(m!=0,"missing named method "+cls+"::"+name);Ptr exception=0;
  Ptr out=invoke(m,self,args.empty()?nullptr:args.data(),&exception);require(exception==0,"managed exception in "+cls+"::"+name);
  return roots.add(out);}
 template<class T>T value(Ptr p){require(p!=0,"missing boxed return");auto b=unbox(p);require(b!=nullptr,"missing boxed data");T v;std::memcpy(&v,b,sizeof(v));return v;}
 std::string string(Ptr p){if(!p)return "";auto n=stringLength(p);require(n>=0&&n<=8192,"string length bound");auto chars=stringChars(p);require(chars||!n,"string unavailable");
  std::string s;for(int32_t i=0;i<n;i++){unsigned c=chars[i];if(c>=0xd800&&c<=0xdbff&&i+1<n){unsigned low=chars[++i];require(low>=0xdc00&&low<=0xdfff,"UTF16");c=0x10000+((c-0xd800)<<10)+(low-0xdc00);}
   if(c<0x80)s+=char(c);else if(c<0x800){s+=char(0xc0|(c>>6));s+=char(0x80|(c&63));}else if(c<0x10000){s+=char(0xe0|(c>>12));s+=char(0x80|((c>>6)&63));s+=char(0x80|(c&63));}else{s+=char(0xf0|(c>>18));s+=char(0x80|((c>>12)&63));s+=char(0x80|((c>>6)&63));s+=char(0x80|(c&63));}}
  return s;}
 std::string identity(Ptr p){if(!p)return "null";return pointer(p)+"/native="+pointer(load<Ptr>(p+0x10));}
 template<class F>std::string read(F fn){try{return fn();}catch(const std::exception& ex){return missing(ex.what());}}
 Schema& schema(Roots& roots,Ptr shader){Ptr native=load<Ptr>(shader+0x10);require(native!=0,"shader expired");auto old=schemas.find(native);if(old!=schemas.end())return old->second;
  require(schemas.size()<32,"shader schema cache limit (32)");Schema s;s.shader=shader;s.key=identity(shader);
  int n=value<int>(call(roots,"Shader","GetPropertyCount",shader,{},"System.Int32"));require(n>=0&&n<=1024,"shader property limit (1024)");
  State names;for(int i=0;i<n;i++){auto name=string(call(roots,"Shader","GetPropertyName",shader,{"System.Int32"},"System.String",{&i}));
   int id=value<int>(call(roots,"Shader","GetPropertyNameId",shader,{"System.Int32"},"System.Int32",{&i}));
   int kind=value<int>(call(roots,"Shader","GetPropertyType",shader,{"System.Int32"},"UnityEngine.Rendering.ShaderPropertyType",{&i}));
   s.properties.push_back({id,kind,name});names[std::to_string(id)]=name+"/type="+std::to_string(kind);}
  s.root=pin(shader,true);require(s.root!=0,"shader GC root failed");
  schemaText[s.key]=fields(names);return schemas.emplace(native,std::move(s)).first->second;}
 std::string property(Roots& roots,const std::string& cls,Ptr obj,const Property& p){return read([&]{int id=p.id;
  if(p.type==0||p.type==1){auto v=value<std::array<float,4>>(call(roots,cls,p.type==0?"GetColor":"GetVector",obj,{"System.Int32"},p.type==0?"UnityEngine.Color":"UnityEngine.Vector4",{&id}));
   return "["+number(v[0])+","+number(v[1])+","+number(v[2])+","+number(v[3])+"]";}
  if(p.type==2||p.type==3)return number(value<float>(call(roots,cls,"GetFloat",obj,{"System.Int32"},"System.Single",{&id})));
  if(p.type==4)return identity(call(roots,cls,"GetTexture",obj,{"System.Int32"},"UnityEngine.Texture",{&id}));
  if(p.type==5)return std::to_string(value<int>(call(roots,cls,"GetInteger",obj,{"System.Int32"},"System.Int32",{&id})));
  return missing("unsupported shader property type "+std::to_string(p.type));});}
 void block(Roots& roots,Ptr renderer,int slot,const std::vector<Property>& properties,State& out){const auto prefix=slot<0?std::string("block/renderer/"):"block/slot"+std::to_string(slot)+"/";
  out[prefix+"coverage"]="shader-declared properties only; arbitrary keys/arrays/buffers not enumerated";
  try{require(objectNew!=nullptr,"missing export il2cpp_object_new");Ptr c=classOf("MaterialPropertyBlock");require(c!=0,"missing MaterialPropertyBlock class");
   // Class allocation and the managed constructor avoid inventing the native MPB wrapper layout/ABI.
   Ptr b=roots.add(objectNew(c));require(b!=0,"block allocation failed");call(roots,"MaterialPropertyBlock",".ctor",b,{},"System.Void");
   if(slot<0)call(roots,"Renderer","GetPropertyBlock",renderer,{"UnityEngine.MaterialPropertyBlock"},"System.Void",{reinterpret_cast<void*>(b)});
   else call(roots,"Renderer","GetPropertyBlock",renderer,{"UnityEngine.MaterialPropertyBlock","System.Int32"},"System.Void",{reinterpret_cast<void*>(b),&slot});
   bool empty=value<bool>(call(roots,"MaterialPropertyBlock","get_isEmpty",b,{},"System.Boolean"));out[prefix+"empty"]=empty?"true":"false";
   for(auto& p:properties){auto key=prefix+std::to_string(p.id);if(empty){out[key]="absent";continue;}
    bool has=false;try{int id=p.id;has=value<bool>(call(roots,"MaterialPropertyBlock","HasProperty",b,{"System.Int32"},"System.Boolean",{&id}));}
    catch(const std::exception& ex){out[key]=missing(ex.what());continue;}
    out[key]=has?property(roots,"MaterialPropertyBlock",b,p):"absent";}
   // MPB is managed; its own finalizer releases its native copy after roots are dropped. Never write it to Renderer.
  }catch(const std::exception& ex){out[prefix+"status"]=missing(ex.what());}}
 // The class chain of a live object, most derived first ("UnityEngine.Texture2D", "UnityEngine.Texture", ...).
 std::vector<std::pair<Ptr,std::string>> classChain(Ptr obj){require(objectClass&&classParent&&className&&classNamespace,"missing export il2cpp_object_get_class / il2cpp_class_get_parent / il2cpp_class_get_name / il2cpp_class_get_namespace");
  require(obj&&load<Ptr>(obj+0x10),"object expired");std::vector<std::pair<Ptr,std::string>> out;Ptr c=objectClass(obj);require(c!=0,"object class unavailable");
  for(unsigned depth=0;c&&depth<16;depth++,c=classParent(c)){const char* n=className(c);const char* ns=classNamespace(c);out.push_back({c,std::string(ns&&*ns?std::string(ns)+".":"")+(n?n:"?")});}return out;}
 static bool is(const std::vector<std::pair<Ptr,std::string>>& chain,const char* cls){for(auto& c:chain)if(c.second==cls)return true;return false;}
 // A method by name, parameter and return types, on the object's class or the nearest parent that declares it
 // (an override is found before the method it overrides). The same signature checks as method().
 Ptr inherited(const std::vector<std::pair<Ptr,std::string>>& chain,const std::string& name,const std::vector<std::string>& args,const std::string& result){
  require(!chain.empty(),"class chain");auto key=pointer(chain[0].first)+"::"+name+"->"+result;for(auto& a:args)key+="|"+a;
  auto old=inheritedCache.find(key);if(old!=inheritedCache.end())return old->second;require(inheritedCache.size()<4096,"method cache bound");Ptr found=0;
  for(auto& c:chain){void* it=nullptr;unsigned bounded=0;
   while(Ptr m=methods(c.first,&it)){require(++bounded<=4096,"Unity method enumeration bound");const char* n=methodName(m);
    if(!n||name!=n||paramCount(m)!=args.size()||type(returnType(m))!=result)continue;
    bool same=true;for(size_t i=0;i<args.size();i++)same=same&&type(param(m,uint32_t(i)))==args[i];
    if(same){require(!found,"ambiguous Unity method "+c.second+"::"+name);found=m;}}
   if(found)break;}
  inheritedCache[key]=found;return found;}
 Ptr invokeOn(Roots& roots,const std::vector<std::pair<Ptr,std::string>>& chain,Ptr self,const std::string& name,const std::vector<std::string>& types,const std::string& result,std::vector<void*> args={}){
  Ptr m=inherited(chain,name,types,result);require(m!=0,"no method "+name+"("+[&]{std::string t;for(auto& x:types)t+=(t.empty()?"":",")+x;return t;}()+") -> "+result+" on "+chain[0].second+" or its parents (stripped or not in this engine)");
  Ptr exception=0;Ptr out=invoke(m,self,args.empty()?nullptr:args.data(),&exception);require(exception==0,"managed exception in "+chain[0].second+"::"+name);return roots.add(out);}
 // A boxed enum as the engine names it (System.Enum.ToString on the boxed value) plus its number.
 std::string enumText(Roots& roots,Ptr boxed){int v=value<int>(boxed);std::string n=read([&]{auto chain=classChain(boxed);return string(invokeOn(roots,chain,boxed,"ToString",{},"System.String"));});
  return std::to_string(v)+(n.find("未采集:")==0?"":" ("+n+")");}
 public:
 // Per-submesh layout of a Mesh, through the engine's own managed Mesh methods (by name and signature; no RVA, no
 // native layout). Each value is read on its own: one the engine lacks is reported, the others are still read.
 struct Submesh {bool known=false;uint32_t start=0,count=0;std::string base,topology,error;};
 struct MeshLayout {std::string status,cls,source,submeshCount,vertexCount,readable,indexFormat;int submeshes=-1;std::vector<Submesh> subs;};
 MeshLayout meshLayout(Ptr mesh,unsigned limit=64){MeshLayout out;
  if(!init()){out.status=missing(unavailable);return out;}
  try{Roots roots(*this);roots.add(mesh);auto chain=classChain(mesh);out.cls=chain[0].second;require(is(chain,"UnityEngine.Mesh"),"not a UnityEngine.Mesh: "+out.cls);
   auto i32=[&](const char* n){return read([&]{return std::to_string(value<int>(invokeOn(roots,chain,mesh,n,{},"System.Int32")));});};
   out.submeshCount=i32("get_subMeshCount");out.vertexCount=i32("get_vertexCount");
   out.readable=read([&]{return value<bool>(invokeOn(roots,chain,mesh,"get_isReadable",{},"System.Boolean"))?std::string("true"):std::string("false");});
   out.indexFormat=read([&]{return enumText(roots,invokeOn(roots,chain,mesh,"get_indexFormat",{},"UnityEngine.Rendering.IndexFormat"));});
   if(out.submeshCount.find("未采集:")==0){out.status="submesh layout unavailable: "+out.submeshCount;return out;}
   int n=std::stoi(out.submeshCount);out.submeshes=n;if(n<0||unsigned(n)>limit){out.status="submesh count "+std::to_string(n)+" outside 0.."+std::to_string(limit)+"; not read";return out;}
   for(int i=0;i<n;i++){Submesh sm;int k=i;
    try{sm.start=value<uint32_t>(invokeOn(roots,chain,mesh,"GetIndexStart",{"System.Int32"},"System.UInt32",{&k}));
     sm.count=value<uint32_t>(invokeOn(roots,chain,mesh,"GetIndexCount",{"System.Int32"},"System.UInt32",{&k}));sm.known=true;}
    catch(const std::exception& ex){sm.error=ex.what();}
    sm.base=read([&]{return std::to_string(value<uint32_t>(invokeOn(roots,chain,mesh,"GetBaseVertex",{"System.Int32"},"System.UInt32",{&k})));});
    sm.topology=read([&]{return enumText(roots,invokeOn(roots,chain,mesh,"GetTopology",{"System.Int32"},"UnityEngine.MeshTopology",{&k}));});
    out.subs.push_back(std::move(sm));}
   out.status="read";out.source="IL2CPP named methods";}
  catch(const std::exception& ex){out.status=missing(ex.what());}
  return out;}
 // Size and format of a texture, the same way; format only through the class the object really is.
 struct TextureLayout {std::string status,cls,width,height,mips,format;};
 TextureLayout textureLayout(Ptr tex){TextureLayout out;
  if(!init()){out.status=missing(unavailable);return out;}
  try{Roots roots(*this);roots.add(tex);auto chain=classChain(tex);out.cls=chain[0].second;require(is(chain,"UnityEngine.Texture"),"not a UnityEngine.Texture: "+out.cls);
   auto i32=[&](const char* n){return read([&]{return std::to_string(value<int>(invokeOn(roots,chain,tex,n,{},"System.Int32")));});};
   out.width=i32("get_width");out.height=i32("get_height");
   if(is(chain,"UnityEngine.Texture2D")){out.mips=i32("get_mipmapCount");out.format=read([&]{return enumText(roots,invokeOn(roots,chain,tex,"get_format",{},"UnityEngine.TextureFormat"));});}
   else if(is(chain,"UnityEngine.RenderTexture")){out.mips=missing("render texture");out.format=read([&]{return enumText(roots,invokeOn(roots,chain,tex,"get_format",{},"UnityEngine.RenderTextureFormat"));});}
   else{out.mips=missing("not a Texture2D");out.format=missing("format is read only on Texture2D / RenderTexture ("+out.cls+")");}
   out.status="read";}
  catch(const std::exception& ex){out.status=missing(ex.what());}
  return out;}
 explicit UnityReader(std::function<Ptr(const char*)> f):lookup(std::move(f)){}
 ~UnityReader(){if(unpin)for(auto& x:schemas)unpin(x.second.root);}
 const std::map<std::string,std::string>& descriptions()const{return schemaText;}
 State sample(Ptr renderer,const std::vector<Ptr>& originals,const std::vector<Ptr>& current,const std::vector<unsigned>& sourceSlots){
  State out;out["coverage"]="shader-declared scalar/vector/texture values; no arbitrary runtime-only properties";
  if(!init()){out["engine"]=missing(unavailable);out["block/has"]=missing(unavailable);return out;}
  out["engine"]="il2cpp named managed methods (signature checked; runtime_invoke)";Roots roots(*this);
  out["block/has"]=read([&]{return value<bool>(call(roots,"Renderer","HasPropertyBlock",renderer,{},"System.Boolean"))?"true":"false";});
  std::map<Ptr,State> values;std::map<Ptr,Schema*> materialSchemas;std::vector<Property> unionProperties;std::set<int> unionIds;
  auto material=[&](Ptr m)->State&{auto prior=values.find(m);if(prior!=values.end())return prior->second;State data;data["identity"]=identity(m);
   try{require(m&&load<Ptr>(m+0x10),"material expired");roots.add(m);Ptr shader=call(roots,"Material","get_shader",m,{},"UnityEngine.Shader");require(shader!=0,"shader unavailable");
    auto& sc=schema(roots,shader);materialSchemas[m]=&sc;data["shader"]=sc.key;
    data["keywords"]=read([&]{Ptr a=call(roots,"Material","get_shaderKeywords",m,{},"System.String[]");require(a!=0,"keyword array unavailable");require(arrayLength!=nullptr,"missing export il2cpp_array_length");auto n=arrayLength(a);require(n<=512,"keyword array bound");std::vector<std::string> keys;
     for(size_t i=0;i<n;i++){keys.push_back(string(roots.add(load<Ptr>(a+0x20+sizeof(Ptr)*i))));}std::sort(keys.begin(),keys.end());std::string text;for(auto& k:keys)text+=(text.empty()?"":" ")+k;return text;});
    data["renderQueue"]=read([&]{return std::to_string(value<int>(call(roots,"Material","get_renderQueue",m,{},"System.Int32")));});
    for(auto& p:sc.properties){data[std::to_string(p.id)]=property(roots,"Material",m,p);if(unionIds.insert(p.id).second)unionProperties.push_back(p);
     if(p.type==4)for(auto name:{"GetTextureScale","GetTextureOffset"}){int id=p.id;data[std::to_string(id)+"/"+name]=read([&]{auto v=value<std::array<float,2>>(call(roots,"Material",name,m,{"System.Int32"},"UnityEngine.Vector2",{&id}));return "["+number(v[0])+","+number(v[1])+"]";});}}
   }catch(const std::exception& ex){data["status"]=missing(ex.what());}
   return values.emplace(m,std::move(data)).first->second;};
  for(Ptr m:originals){material(m);}for(Ptr m:current){material(m);}
  for(auto& entry:values)for(auto& v:entry.second)out["material/"+identity(entry.first)+"/"+v.first]=v.second;
  for(size_t i=0;i<current.size();i++){unsigned slot=i<sourceSlots.size()?sourceSlots[i]:unsigned(i);auto prefix="pair/slot"+std::to_string(i)+"/";
   out[prefix+"originalSlot"]=std::to_string(slot);if(slot>=originals.size()){out[prefix+"status"]=missing("source material slot");continue;}
   auto& a=material(originals[slot]);auto& b=material(current[i]);out[prefix+"source"]=identity(originals[slot]);out[prefix+"mod"]=identity(current[i]);
   bool different=false;for(auto& v:b){if(v.first=="identity")continue;auto old=a.find(v.first);if(old==a.end()||old->second!=v.second){out[prefix+"difference/"+v.first]=(old==a.end()?missing("original property"):old->second)+" -> "+v.second;different=true;}}
   for(auto& v:a)if(v.first!="identity"&&!b.count(v.first)){out[prefix+"difference/"+v.first]=v.second+" -> "+missing("mod property");different=true;}
   bool incomplete=a.count("status")||b.count("status");for(auto& v:a)incomplete=incomplete||v.second.find("未采集:")==0;for(auto& v:b)incomplete=incomplete||v.second.find("未采集:")==0;
   out[prefix+"comparison"]=incomplete?"incomplete":"complete";out[prefix+"differences"]=different?"true":"false";}
  block(roots,renderer,-1,unionProperties,out);
  for(size_t i=0;i<current.size();i++){auto sc=materialSchemas.find(current[i]);block(roots,renderer,int(i),sc==materialSchemas.end()?unionProperties:sc->second->properties,out);}
  return out;
 }
};

// Continuous 30-second history of cheap identity samples + complete EVENT snapshots. No polling of all shader
// values at 100 ms in this first diagnostic stage. Full states stay in memory so replay has a baseline after pruning.
class Recorder {
 struct Entry {uint64_t at;std::chrono::system_clock::time_point wall;std::string key,phase;State state;size_t bytes;};
 std::deque<Entry> ring;size_t bytes=0;uint64_t overwritten=0,followUntil=0,request=0;
 std::map<std::string,State> lastWritten;std::map<std::string,size_t> retained;std::set<std::string> described;
 std::function<void(const std::string&)> emit;
 static constexpr size_t byteLimit=1024*1024,entryLimit=1024;
 std::string line(const Entry& e,const State& data,const char* kind){return logLine("runtime",Level::Debug,std::string(kind)+" mono-ms="+std::to_string(e.at)+" key="+quote(e.key)+" phase="+quote(e.phase)+" "+fields(data),e.wall);}
 void send(const Entry& e,bool baseline,const char* kind){State changes;if(lastWritten.size()>=64&&!lastWritten.count(e.key))lastWritten.clear();auto& old=lastWritten[e.key];
  for(auto& v:e.state){auto p=old.find(v.first);if(baseline||p==old.end()||p->second!=v.second)changes[v.first]=v.second;}
  for(auto& v:old)if(!e.state.count(v.first))changes[v.first]="removed";
  if(baseline||!changes.empty()){emit(line(e,changes,kind));}old=e.state;}
 public:
 explicit Recorder(std::function<void(const std::string&)> f):emit(std::move(f)){}
 void clear(){ring.clear();bytes=0;followUntil=0;request=0;overwritten=0;lastWritten.clear();retained.clear();described.clear();}
 void prune(uint64_t now){while(!ring.empty()&&(now-ring.front().at>30000||bytes>byteLimit||ring.size()>entryLimit)){if(now-ring.front().at<=30000)overwritten=now;bytes-=ring.front().bytes;auto key=ring.front().key;ring.pop_front();if(--retained[key]==0){retained.erase(key);lastWritten.erase(key);}}}
 void schema(const std::map<std::string,std::string>& descriptions){for(auto& s:descriptions)if(described.insert(s.first).second)emit(logLine("runtime",Level::Debug,"render-schema shader="+quote(s.first)+" "+s.second));}
 void capture(uint64_t now,const std::string& key,const std::string& phase,State state,bool writeNow=false){
  prune(now);size_t n=key.size()+phase.size()+128;for(auto& v:state)n+=v.first.size()+v.second.size()+64;
  if(n>byteLimit/2){state={{"status",missing("snapshot exceeds 512 KiB bound")}};n=256;}
  ring.push_back({now,std::chrono::system_clock::now(),key,phase,std::move(state),n});retained[key]++;bytes+=n;prune(now);
  if(writeNow||request)send(ring.back(),false,"render-state");}
 void mark(uint64_t now,uint64_t token,const std::map<std::string,std::string>& descriptions){prune(now);if(request)emit(logLine("runtime",Level::Debug,"render-mark-end request="+std::to_string(request)+" reason=superseded next-request="+std::to_string(token)));request=token;followUntil=now+5000;lastWritten.clear();
  emit(logLine("runtime",Level::Debug,"render-mark-begin request="+std::to_string(token)+" received-mono-ms="+std::to_string(now)+" earliest-mono-ms="+std::to_string(ring.empty()?now:ring.front().at)+" requested-history-ms=30000 capacity-truncated="+std::string(overwritten&&now-overwritten<=30000?"true":"false")+" visual-status=user-marked; parameter changes are not a visual diagnosis"));
  // Replay schemas too: ordinary log rotation may have removed the one-time descriptions.
  for(auto& s:descriptions)emit(logLine("runtime",Level::Debug,"render-schema-replay shader="+quote(s.first)+" "+s.second));
  for(auto& entry:ring)send(entry,lastWritten.count(entry.key)==0,"render-history");}
 void tick(uint64_t now){prune(now);if(request&&now>=followUntil){emit(logLine("runtime",Level::Debug,"render-mark-end request="+std::to_string(request)+" mono-ms="+std::to_string(now)));request=0;followUntil=0;}}
};
}
