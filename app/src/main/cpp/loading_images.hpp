#pragma once
#include "runtime.hpp"
#include "model.hpp"
#include <array>
#include <atomic>
#include <chrono>
#include <random>
namespace ms {
struct LoadingPrepared {std::string name,key,error;unsigned cycle=0,index=0;bool hide=false;Texture image;};
// Worker-only reader: one decoded image is resident, not a whole slideshow.
struct LoadingReader {
 std::string key,selectionKey;unsigned selectedImage=0;std::mt19937 random{unsigned(std::chrono::steady_clock::now().time_since_epoch().count())};
 std::shared_ptr<LoadingPrepared> update(const std::string& root,const std::vector<std::string>& selected,bool enabled,const std::map<std::string,std::map<std::string,int>>& vars,unsigned cycle){
  std::string folder,name;if(enabled)for(const auto& n:selected){if(n.empty()||n==".."||n.find_first_of("/\\")!=n.npos)continue;if(exists(root+"/"+n+"/loading.json")){name=n;folder=root+"/"+n;break;}}
  std::string next=name.empty()?"none":folder+":"+std::to_string(static_cast<long long>(std::filesystem::last_write_time(folder+"/loading.json").time_since_epoch().count()))+":"+std::to_string(cycle);
  std::string selection=next;auto v=vars.find(name);int hide=-1;if(v!=vars.end()){auto it=v->second.find("$skip_ui");if(it!=v->second.end())hide=it->second;}next+=':'+std::to_string(hide);
  if(next==key){return nullptr;}key=next;auto out=std::make_shared<LoadingPrepared>();out->key=key;out->name=name;out->cycle=cycle;if(name.empty())return out;
  try{auto j=JsonParser(text(folder+"/loading.json")).parse();require(jstr(j,"format",64)=="gibox-loading-images"&&juint(j,"version",1,1)==1,"加载图片格式无效");
   const auto& images=jarr(j,"images",1,64);for(const auto& row:images){auto f=jstr(row,"file",64);require(f.compare(0,7,"images/")==0&&f.size()>12&&f.compare(f.size()-5,5,".rgba")==0&&f.find_first_not_of("0123456789",7)==f.size()-5,"加载图片路径无效");juint(row,"width",1,2048);juint(row,"height",1,2048);}
   if(selection!=selectionKey||selectedImage>=images.size()){selectedImage=std::uniform_int_distribution<unsigned>(0,unsigned(images.size()-1))(random);selectionKey=selection;}out->index=selectedImage;const auto& image=images[out->index];auto file=jstr(image,"file",64);
   require(file.compare(0,7,"images/")==0&&file.size()>12&&file.compare(file.size()-5,5,".rgba")==0&&file.find_first_not_of("0123456789",7)==file.size()-5,"加载图片路径无效");
   out->image.w=unsigned(juint(image,"width",1,2048));out->image.h=unsigned(juint(image,"height",1,2048));out->image.pixels=read(folder+"/"+file,16u<<20);require(out->image.pixels.size()==4ull*out->image.w*out->image.h,"加载图片数据长度不符");
   out->hide=hide>=0?hide!=0:j.at("hideUi").type==Json::Bool&&j.at("hideUi").boolean;
  }catch(const std::exception& ex){out->error=ex.what();}return out;}
};
class LoadingScreens {
 struct Color {float r,g,b,a;};struct Rect {float x,y,w,h;};struct Pivot {float x,y;};
 struct Hidden {Ptr go,native;bool active;};Engine& e;std::vector<uint32_t> refs,assetRefs,hiddenRefs,originalRefs;std::vector<Ptr> assets;std::vector<Hidden> hidden;
 Ptr owner=0,ownerNative=0,image=0,imageNative=0,original=0,sprite=0;int originalType=0;bool originalAspect=false,visible=false,applied=false,busy=false;Color originalColor{1,1,1,1};std::string appliedKey;
 std::shared_ptr<LoadingPrepared> prepared;
 static bool alive(Ptr p){return p&&load<Ptr>(p+0x10)!=0;}
 void pin(std::vector<uint32_t>& roots,Ptr p){if(p){auto h=e.call<uint32_t>(abi::pin,p,2,Ptr(0));require(h!=0,"加载界面 GC 引用失败");roots.push_back(h);}}
 void release(std::vector<uint32_t>& roots){for(auto h:roots){e.call<void>(abi::unpin,h);}
  roots.clear();}
 bool live()const{return alive(owner)&&load<Ptr>(owner+0x10)==ownerNative&&alive(image)&&load<Ptr>(image+0x10)==imageNative;}
 void cleanAssets(){for(auto it=assets.rbegin();it!=assets.rend();++it){if(alive(*it)){e.action(abi::destroy,*it);}}
  assets.clear();release(assetRefs);sprite=0;}
 void restore(){if(applied&&live()){if(load<Ptr>(image+0xe0)==sprite)e.set(abi::imageSetOverride,image,original);e.op(abi::imageSetType,image,originalType);e.op(abi::imageSetAspect,image,originalAspect?1:0);Color restored=originalColor;restored.a=e.call<Color>(abi::graphicColor,image,Ptr(0)).a;e.call<void>(abi::graphicSetColor,image,restored,Ptr(0));}
  for(const auto& h:hidden){if(alive(h.go)&&load<Ptr>(h.go+0x10)==h.native){e.op(abi::goSetActive,h.go,h.active?1:0);}}
  hidden.clear();release(hiddenRefs);applied=false;appliedKey.clear();cleanAssets();}
 void hideUi(){Ptr root=e.get(abi::gameObject,owner);for(unsigned offset:{0x18u,0x20u,0x38u,0x40u}){Ptr component=load<Ptr>(owner+offset);if(!alive(component))continue;Ptr go=e.get(abi::gameObject,component);if(!alive(go)||go==root)continue;
   bool duplicate=false;for(const auto& h:hidden){duplicate=duplicate||h.go==go;}
   if(duplicate){continue;}pin(hiddenRefs,go);bool active=(e.call<uint32_t>(abi::goActiveSelf,go,Ptr(0))&1)!=0;hidden.push_back({go,load<Ptr>(go+0x10),active});if(active)e.op(abi::goSetActive,go,0);}}
 void capture(){original=load<Ptr>(image+0xe0);release(originalRefs);pin(originalRefs,original);originalType=e.call<int>(abi::imageType,image,Ptr(0));originalAspect=(e.call<uint32_t>(abi::imageAspect,image,Ptr(0))&1)!=0;originalColor=e.call<Color>(abi::graphicColor,image,Ptr(0));}
 void apply(const std::string& fit){if(!live()||!prepared||prepared->name.empty()||!prepared->error.empty())return;const auto& t=prepared->image;Rect crop{0,0,float(t.w),float(t.h)};std::string key=prepared->key+":"+fit;
  if(fit=="cover"){Ptr transform=e.get(abi::graphicRect,image);require(alive(transform),"加载背景尺寸尚未就绪");auto bounds=e.call<Rect>(abi::rectBounds,transform,Ptr(0));require(std::isfinite(bounds.w)&&std::isfinite(bounds.h)&&bounds.w>0&&bounds.h>0,"加载背景尺寸无效");
   float ratio=bounds.w/bounds.h;if(float(t.w)/t.h>ratio){crop.w=t.h*ratio;crop.x=(t.w-crop.w)*.5f;}else{crop.h=t.w/ratio;crop.y=(t.h-crop.h)*.5f;}key+=':'+std::to_string(crop.w)+':'+std::to_string(crop.h);}
  if(appliedKey==key&&alive(sprite)&&assets.size()==2&&alive(assets[0])){
   Ptr current=load<Ptr>(image+0xe0);if(current!=sprite){release(originalRefs);original=current;pin(originalRefs,original);e.set(abi::imageSetOverride,image,sprite);}
   if(e.call<int>(abi::imageType,image,Ptr(0))!=0){originalType=e.call<int>(abi::imageType,image,Ptr(0));e.op(abi::imageSetType,image,0);}
   e.op(abi::imageSetAspect,image,fit=="original"?1:0);Color color=e.call<Color>(abi::graphicColor,image,Ptr(0));if(color.r!=1||color.g!=1||color.b!=1){originalColor=color;e.call<void>(abi::graphicSetColor,image,Color{1,1,1,color.a},Ptr(0));}return;
  }restore();capture();
  Ptr descriptor=e.call<Ptr>(abi::textureType);require(descriptor!=0,"缺少加载图片纹理类型");Ptr texture=e.call<Ptr>(abi::objectNew,load<Ptr>(descriptor));require(texture!=0,"加载图片分配失败");assets.push_back(texture);pin(assetRefs,texture);
  e.call<void>(abi::textureInit,texture,int(t.w),int(t.h),4,0,0,Ptr(0));require(alive(texture),"加载图片初始化失败");e.call<void>(abi::texturePixels,texture,reinterpret_cast<Ptr>(t.pixels.data()),int(t.pixels.size()),Ptr(0));e.op(abi::textureWrap,texture,1);e.op(abi::textureFilter,texture,1);e.action(abi::textureApply,texture);
  sprite=e.call<Ptr>(abi::spriteCreate,texture,crop,Pivot{.5f,.5f},Ptr(0));require(alive(sprite),"加载图片 Sprite 创建失败");assets.push_back(sprite);pin(assetRefs,sprite);
  applied=true;e.op(abi::imageSetType,image,0);e.op(abi::imageSetAspect,image,fit=="original"?1:0);e.call<void>(abi::graphicSetColor,image,Color{1,1,1,originalColor.a},Ptr(0));e.set(abi::imageSetOverride,image,sprite);if(prepared->hide)hideUi();appliedKey=key;
 }
 public:
 std::atomic<unsigned> cycle{0};explicit LoadingScreens(Engine& engine):e(engine){}
 bool updating()const{return busy;}
 bool showing()const{return live()&&load<Ptr>(owner+0x28)==image&&(e.call<uint32_t>(abi::imageActive,image,Ptr(0))&1)!=0;}
 bool discover(Ptr type){if(busy||!type||showing()){return false;}std::vector<uint32_t> roots;bool changed=false;Ptr pendingCanvas=0,pendingBg=0;unsigned pendingCount=0;
  try{Ptr found=e.call<Ptr>(abi::findObjectsOfType,type,Ptr(0));if(found){pin(roots,found);uint64_t n=load<uint64_t>(found+0x18);require(n<=64,"加载界面实例数无效");
   for(uint64_t i=0;i<n;i++){Ptr canvas=load<Ptr>(found+0x20+i*8);if(!alive(canvas)){continue;}Ptr go=e.get(abi::gameObject,canvas);if(!alive(go)||!(e.call<uint32_t>(abi::goActive,go,Ptr(0))&1)){continue;}Ptr bg=load<Ptr>(canvas+0x28);if(!alive(bg)){continue;}if(!(e.call<uint32_t>(abi::imageActive,bg,Ptr(0))&1)){pendingCanvas=canvas;pendingBg=bg;pendingCount++;continue;}
    pendingCount=0;changed=owner!=canvas||image!=bg||!live();observe(canvas,bg);break;}}if(!showing()&&pendingCount==1){changed=owner!=pendingCanvas||image!=pendingBg||!live();observe(pendingCanvas,pendingBg);}}
  catch(...){release(roots);throw;}release(roots);return changed;}
 bool hasCanvas()const{return owner!=0;}
 bool ready()const{return prepared&&!prepared->name.empty();}bool active()const{return applied&&visible;}std::string name()const{return prepared?prepared->name:"";}std::string error()const{return prepared?prepared->error:"";}
 void receive(std::shared_ptr<LoadingPrepared> p){prepared=std::move(p);}
 bool matches(Ptr p)const{return p&&p==image;}
 void observe(Ptr canvas,Ptr background){if(busy||!alive(canvas)||!alive(background))return;if(owner==canvas&&image==background&&live())return;
  bool previous=visible;shutdown();if(previous){cycle.fetch_add(1);}owner=canvas;ownerNative=load<Ptr>(canvas+0x10);image=background;imageNative=load<Ptr>(background+0x10);pin(refs,owner);pin(refs,image);
  capture();}
 void event(Ptr background,bool shown){if(busy||!matches(background))return;if(!shown&&visible){busy=true;try{restore();visible=false;cycle.fetch_add(1);}catch(...){busy=false;throw;}busy=false;}}
 void tick(bool enabled,const std::vector<std::string>& selected,const std::string& fit="original"){if(busy)return;busy=true;try{bool selectedNow=false;if(prepared){for(const auto& n:selected){selectedNow=selectedNow||n==prepared->name;}}
   if(!live()||load<Ptr>(owner+0x28)!=image){restore();if(visible){cycle.fetch_add(1);}visible=false;busy=false;return;}bool shown=(e.call<uint32_t>(abi::imageActive,image,Ptr(0))&1)!=0;
   if(!shown&&visible){restore();visible=false;cycle.fetch_add(1);}else if(shown){visible=true;if(enabled&&selectedNow)apply(fit);else restore();}
  }catch(...){try{restore();}catch(...){}busy=false;throw;}busy=false;}
 void destroyed(Ptr canvas){if(canvas==owner){if(visible)cycle.fetch_add(1);shutdown();}}
 void shutdown(){busy=true;try{restore();release(refs);release(originalRefs);owner=0;image=0;visible=false;}catch(...){busy=false;throw;}busy=false;}
};

inline constexpr std::array<const char*,2> profileRoles{"avatar","namecard"};
struct ProfilePrepared {
 std::array<Texture,2> images;
 std::array<bool,2> linear{};
 std::array<std::string,2> names;
 std::string error;
};

struct ProfileReader {
 std::string key;
 static Texture circleAvatar(unsigned w,unsigned h,Bytes pixels){
  unsigned edge=std::min(w,h),left=(w-edge)/2,bottom=(h-edge)/2;
  Texture result{edge,edge,Bytes(size_t(edge)*edge*4)};
  float radius=float(edge)*.5f;
  for(unsigned y=0;y<edge;y++){
   auto* row=result.pixels.data()+size_t(y)*edge*4;
   std::memcpy(row,pixels.data()+(size_t(bottom+y)*w+left)*4,size_t(edge)*4);
   float dy=float(y)+.5f-radius;
   for(unsigned x=0;x<edge;x++){
    float dx=float(x)+.5f-radius;
    float coverage=std::clamp(radius+.5f-std::sqrt(dx*dx+dy*dy),0.f,1.f);
    auto& alpha=row[4*x+3];alpha=uint8_t(std::lround(alpha*coverage));
   }
  }
  return result;
 }
 std::shared_ptr<ProfilePrepared> update(const std::string& root,const std::vector<std::string>& selected,bool enabled){
  std::string next;std::vector<std::pair<std::string,std::string>> inputs;
  if(enabled)for(const auto& name:selected){
   if(name.empty()||name=="."||name==".."||name.find_first_of("/\\")!=name.npos)continue;
   auto file=root+"/"+name+"/profile.json";std::error_code error;auto stamp=std::filesystem::last_write_time(file,error);
   if(error)continue;
   next+=file+":"+std::to_string(static_cast<long long>(stamp.time_since_epoch().count()))+'\n';inputs.push_back({name,std::move(file)});
  }
  if(next==key)return nullptr;
  auto out=std::make_shared<ProfilePrepared>();
  for(const auto& input:inputs){
   try{
    auto doc=JsonParser(text(input.second)).parse();require(jstr(doc,"format",64)=="gibox-profile-images"&&juint(doc,"version",1,1)==1,"头像和名片格式无效");
    for(size_t i=0;i<profileRoles.size();i++){
     const auto& row=doc.at(profileRoles[i]);if(row.type==Json::Null||!out->names[i].empty())continue;
     std::string path=std::string("images/")+profileRoles[i]+".rgba";
     require(jstr(row,"file",64)==path,"头像或名片路径无效");
     unsigned w=unsigned(juint(row,"width",1,2048)),h=unsigned(juint(row,"height",1,2048));
     auto pixels=read(root+"/"+input.first+"/"+path,16u<<20);require(pixels.size()==4ull*w*h,"头像或名片数据长度不符");
     out->images[i]=i==0?circleAvatar(w,h,std::move(pixels)):Texture{w,h,std::move(pixels)};
     out->linear[i]=jbool(row,"linear",false);out->names[i]=input.first;
    }
   }catch(const std::exception& ex){out->error=input.first+"："+ex.what();}
   if(!out->names[0].empty()&&!out->names[1].empty())break;
  }
  key=std::move(next);return out;
 }
};

class ProfileScreens {
 struct Rect {float x,y,w,h;};struct Pivot {float x,y;};
 struct Image {Ptr native=0,original=0,replacement=0;uint32_t root=0,saved=0;bool changedAspect=false;};
 Engine& e;
 std::array<std::atomic<Ptr>,2> classes{};
 std::array<Ptr,2> types{},sprites{},textures{};
 std::array<uint32_t,2> typeRoots{},spriteRoots{},textureRoots{};
 std::map<Ptr,Image> images;
 std::shared_ptr<ProfilePrepared> prepared;
 uint64_t scanAt=0;
 static bool alive(Ptr p){return p&&load<Ptr>(p+0x10)!=0;}
 uint32_t pin(Ptr p){if(!p)return 0;auto h=e.call<uint32_t>(abi::pin,p,2,Ptr(0));require(h!=0,"个人资料图片引用失败");return h;}
 void release(uint32_t& h){if(h)e.call<void>(abi::unpin,h);h=0;}
 void dispose(Ptr& p,uint32_t& h){if(alive(p))e.action(abi::destroy,p);p=0;release(h);}
 void restore(Ptr image,Image& row){
  if(alive(image)&&load<Ptr>(image+0x10)==row.native){
   if(load<Ptr>(image+0xe0)==row.replacement)e.set(abi::imageSetOverride,image,alive(row.original)?row.original:0);
   if(row.changedAspect)e.op(abi::imageSetAspect,image,0);
  }
  release(row.saved);release(row.root);
 }
 void clear(){for(auto& entry:images)restore(entry.first,entry.second);images.clear();
  for(size_t i=0;i<sprites.size();i++){dispose(sprites[i],spriteRoots[i]);dispose(textures[i],textureRoots[i]);}}
 Ptr sprite(size_t i){
  if(alive(sprites[i])&&alive(textures[i]))return sprites[i];
  dispose(sprites[i],spriteRoots[i]);dispose(textures[i],textureRoots[i]);
  const auto& t=prepared->images[i];Ptr descriptor=e.call<Ptr>(abi::textureType);require(descriptor,"缺少个人资料纹理类型");
  Ptr& tex=textures[i];tex=e.call<Ptr>(abi::objectNew,load<Ptr>(descriptor));require(tex,"个人资料纹理分配失败");textureRoots[i]=pin(tex);
  e.call<void>(abi::textureInit,tex,int(t.w),int(t.h),4,0,int(prepared->linear[i]),Ptr(0));require(alive(tex),"个人资料纹理初始化失败");
  e.call<void>(abi::texturePixels,tex,reinterpret_cast<Ptr>(t.pixels.data()),int(t.pixels.size()),Ptr(0));e.op(abi::textureWrap,tex,1);e.op(abi::textureFilter,tex,1);e.action(abi::textureApply,tex);
  Ptr& s=sprites[i];s=e.call<Ptr>(abi::spriteCreate,tex,Rect{0,0,float(t.w),float(t.h)},Pivot{.5f,.5f},Ptr(0));require(alive(s),"个人资料 Sprite 创建失败");spriteRoots[i]=pin(s);return s;
 }
 void apply(Ptr image,size_t role){
  auto& row=images[image];Ptr native=load<Ptr>(image+0x10);
  if(row.native!=native){restore(image,row);row={};row.root=pin(image);row.native=native;}
  Ptr current=load<Ptr>(image+0xe0),replacement=sprite(role);
  if(role==0&&!(e.call<uint32_t>(abi::imageAspect,image,Ptr(0))&1)){
   e.op(abi::imageSetAspect,image,1);row.changedAspect=true;
  }
  if(current==replacement)return;
  if(current!=row.replacement||!row.replacement){auto saved=pin(current);release(row.saved);row.saved=saved;row.original=current;}
  row.replacement=replacement;e.set(abi::imageSetOverride,image,replacement);
 }
 public:
 explicit ProfileScreens(Engine& engine):e(engine){}
 void observe(unsigned page,Ptr klass){if(page<classes.size()&&klass)classes[page].store(klass);}
 void receive(std::shared_ptr<ProfilePrepared> next){clear();prepared=std::move(next);scanAt=0;}
 bool ready()const{return prepared&&(!prepared->names[0].empty()||!prepared->names[1].empty());}
 void appendActive(std::vector<std::string>& active)const{
  if(!prepared)return;
  for(size_t i=0;i<sprites.size();i++)if(sprites[i]&&!prepared->names[i].empty()){
   const auto& mod=prepared->names[i];bool showing=std::any_of(images.begin(),images.end(),[&](const auto& row){return row.second.replacement==sprites[i];});
   if(showing&&std::find(active.begin(),active.end(),mod)==active.end())active.push_back(mod);
  }
 }
 void shutdown(){clear();for(auto& handle:typeRoots)release(handle);types.fill(0);}
 void tick(bool enabled,const std::vector<std::string>& selected,unsigned byval,uint64_t now){
  std::array<bool,2> wanted{};
  if(enabled&&prepared)for(size_t i=0;i<wanted.size();i++)if(!prepared->names[i].empty())
   wanted[i]=std::find(selected.begin(),selected.end(),prepared->names[i])!=selected.end();
  if(!wanted[0]&&!wanted[1]){if(!images.empty()||textures[0]||textures[1])clear();return;}
  if(!byval||now<scanAt)return;
  scanAt=now+300;std::set<Ptr> seen;
  for(size_t page=0;page<classes.size();page++){
   Ptr klass=classes[page].load();if(!klass)continue;
   if(!types[page]){
    require(e.call<Ptr>(abi::classFromType,klass+byval,1)==klass,"个人资料页 Type 校验失败");
    Ptr type=e.call<Ptr>(abi::typeObject,klass+byval);require(alive(type),"个人资料页 Type 未就绪");typeRoots[page]=pin(type);types[page]=type;
   }
   Ptr array=e.call<Ptr>(abi::findObjectsOfType,types[page],Ptr(0));if(!array)continue;
   uint32_t root=pin(array);
   try{
    auto count=load<uint64_t>(array+0x18);require(count<=64,"个人资料页数量异常");
    for(uint64_t n=0;n<count;n++){
     Ptr view=load<Ptr>(array+0x20+8*n);if(!alive(view)||load<Ptr>(view)!=klass)continue;
     for(size_t i=0;i<wanted.size();i++)if(wanted[i]){
      // playerIconImage and _nameCardPic in both profile classes, CN and global 7.2.
      Ptr image=load<Ptr>(view+(i==0?0x88:0xb8));
      if(alive(image)&&(e.call<uint32_t>(abi::imageActive,image,Ptr(0))&1)&&seen.insert(image).second)apply(image,i);
     }
    }
   }catch(...){release(root);throw;}release(root);
  }
  for(auto it=images.begin();it!=images.end();){
   if(seen.count(it->first)){++it;continue;}
   restore(it->first,it->second);it=images.erase(it);
  }
 }
};
}
