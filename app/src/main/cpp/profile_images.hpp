#pragma once
#include "loading_images.hpp"
#include <array>

namespace ms {
inline constexpr std::array<const char*,2> profileRoles{"avatar","namecard"};
struct ProfilePrepared {
 std::array<Texture,2> images;
 std::array<std::string,2> names;
 std::string error;
};

struct ProfileReader {
 std::string key;
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
     out->images[i]={w,h,std::move(pixels)};out->names[i]=input.first;
    }
   }catch(const std::exception& ex){out->error=input.first+"："+ex.what();}
   if(!out->names[0].empty()&&!out->names[1].empty())break;
  }
  key=std::move(next);return out;
 }
};

class ProfileScreens {
 struct Rect {float x,y,w,h;};struct Pivot {float x,y;};
 struct Image {Ptr native=0,original=0,replacement=0;uint32_t root=0,saved=0;};
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
  if(alive(image)&&load<Ptr>(image+0x10)==row.native&&load<Ptr>(image+0xe0)==row.replacement)
   e.set(abi::imageSetOverride,image,alive(row.original)?row.original:0);
  release(row.saved);release(row.root);
 }
 void clear(){for(auto& entry:images)restore(entry.first,entry.second);images.clear();
  for(size_t i=0;i<sprites.size();i++){dispose(sprites[i],spriteRoots[i]);dispose(textures[i],textureRoots[i]);}}
 Ptr sprite(size_t i){
  if(alive(sprites[i])&&alive(textures[i]))return sprites[i];
  dispose(sprites[i],spriteRoots[i]);dispose(textures[i],textureRoots[i]);
  const auto& t=prepared->images[i];Ptr descriptor=e.call<Ptr>(abi::textureType);require(descriptor,"缺少个人资料纹理类型");
  Ptr& tex=textures[i];tex=e.call<Ptr>(abi::objectNew,load<Ptr>(descriptor));require(tex,"个人资料纹理分配失败");textureRoots[i]=pin(tex);
  e.call<void>(abi::textureInit,tex,int(t.w),int(t.h),4,0,0,Ptr(0));require(alive(tex),"个人资料纹理初始化失败");
  e.call<void>(abi::texturePixels,tex,reinterpret_cast<Ptr>(t.pixels.data()),int(t.pixels.size()),Ptr(0));e.op(abi::textureWrap,tex,1);e.op(abi::textureFilter,tex,1);e.action(abi::textureApply,tex);
  Ptr& s=sprites[i];s=e.call<Ptr>(abi::spriteCreate,tex,Rect{0,0,float(t.w),float(t.h)},Pivot{.5f,.5f},Ptr(0));require(alive(s),"个人资料 Sprite 创建失败");spriteRoots[i]=pin(s);return s;
 }
 void apply(Ptr image,size_t role){
  auto& row=images[image];Ptr native=load<Ptr>(image+0x10);
  if(row.native!=native){restore(image,row);row={};row.root=pin(image);row.native=native;}
  Ptr current=load<Ptr>(image+0xe0),replacement=sprite(role);
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
