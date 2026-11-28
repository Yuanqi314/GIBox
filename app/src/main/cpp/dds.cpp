#include "core.hpp"
#include <array>
#define BCDEC_IMPLEMENTATION
#include "bcdec.h"
namespace ms {
namespace {
enum class DdsFormat {RGB,BC1,BC2,BC3,BC4,BC5,BC6U,BC6S,BC7};
struct DdsInfo {
 unsigned w,h,levels,bits,offset,pitch;
 std::array<unsigned,4> masks;
 DdsFormat format=DdsFormat::RGB;
 TextureColor color=TextureColor::Role;
};
DdsInfo ddsInfo(const Bytes& b){
 require(b.size()>=128&&u32(b.data())==0x20534444&&u32(b.data()+4)==124&&u32(b.data()+76)==32,"DDS 文件头无效");
 DdsInfo d{u32(b.data()+16),u32(b.data()+12),std::max(1u,u32(b.data()+28)),u32(b.data()+88),128,0,
           {u32(b.data()+92),u32(b.data()+96),u32(b.data()+100),u32(b.data()+104)}};
 require(d.w&&d.h&&d.w<=8192&&d.h<=8192&&u32(b.data()+24)<=1&&(u32(b.data()+112)&0xfe00)==0,"不支持的 DDS 尺寸、立方体贴图或体积贴图");
 if(u32(b.data()+80)&4){switch(u32(b.data()+84)){
  case 0x31545844:d.format=DdsFormat::BC1;break;
  case 0x33545844:d.format=DdsFormat::BC2;break;
  case 0x35545844:d.format=DdsFormat::BC3;break;
  case 0x31495441:case 0x55344342:d.format=DdsFormat::BC4;d.color=TextureColor::Linear;break;
  case 0x32495441:case 0x55354342:d.format=DdsFormat::BC5;d.color=TextureColor::Linear;break;
  case 0x30315844:{
   require(b.size()>=148&&u32(b.data()+132)==3&&!(u32(b.data()+136)&4)&&u32(b.data()+140)==1,"DDS DX10 必须是单张二维贴图");
   d.offset=148;const auto dx=u32(b.data()+128);
   d.color=TextureColor::Linear;
   switch(dx){
    case 72:d.color=TextureColor::Srgb;[[fallthrough]];case 71:d.format=DdsFormat::BC1;break;
    case 75:d.color=TextureColor::Srgb;[[fallthrough]];case 74:d.format=DdsFormat::BC2;break;
    case 78:d.color=TextureColor::Srgb;[[fallthrough]];case 77:d.format=DdsFormat::BC3;break;
    case 80:d.format=DdsFormat::BC4;break;
    case 83:d.format=DdsFormat::BC5;break;
    case 95:d.format=DdsFormat::BC6U;break;
    case 96:d.format=DdsFormat::BC6S;break;
    case 99:d.color=TextureColor::Srgb;[[fallthrough]];case 98:d.format=DdsFormat::BC7;break;
    case 29:d.color=TextureColor::Srgb;[[fallthrough]];case 28:d.bits=32;d.masks={0xff,0xff00,0xff0000,0xff000000};break;
    case 91:d.color=TextureColor::Srgb;[[fallthrough]];case 87:d.bits=32;d.masks={0xff0000,0xff00,0xff,0xff000000};break;
    default:require(false,"不支持的 DDS DXGI 格式 "+std::to_string(dx));
   }
   break;
  }
  default:require(false,"不支持的 DDS FourCC");
 }}
 if(d.format==DdsFormat::RGB){
  require(d.bits==24||d.bits==32,"DDS RGB bits");
  d.pitch=(u32(b.data()+8)&8)?u32(b.data()+20):d.w*(d.bits/8);
  require(d.pitch>=d.w*(d.bits/8)&&d.masks[0]&&d.masks[1]&&d.masks[2],"DDS RGB pitch/masks 无效");
 }
 return d;
}
struct PixelChannel {unsigned mask=0,shift=0;};
PixelChannel pixelChannel(unsigned mask){
 PixelChannel c;
 if(mask){while(!(mask&1)){mask>>=1;c.shift++;}require((mask&(mask+1))==0,"DDS 颜色掩码不连续");c.mask=mask;}
 return c;
}
unsigned component(unsigned pixel,PixelChannel c,unsigned fallback){return c.mask?((uint64_t((pixel>>c.shift)&c.mask)*255)+c.mask/2)/c.mask:fallback;}
unsigned ddsBlock(DdsFormat fmt){return fmt==DdsFormat::BC1||fmt==DdsFormat::BC4?8:16;}
uint64_t ddsLevelSize(const DdsInfo& d,unsigned w,unsigned h,unsigned level){
 if(d.format!=DdsFormat::RGB)return uint64_t((w+3)/4)*((h+3)/4)*ddsBlock(d.format);
 // Legacy 24-bit DDS files also use DWORD-aligned rows. Infer this from the declared top-level pitch.
 const unsigned row=w*(d.bits/8),original=d.w*(d.bits/8);
 const unsigned stride=level==0?d.pitch:(d.pitch>original&&d.pitch==(original+3u)/4*4?(row+3u)/4*4:row);
 return uint64_t(stride)*h;
}
Bytes ddsLevel(const Bytes& b,const DdsInfo& d,unsigned w,unsigned h,unsigned level,size_t off){
 const auto len=ddsLevelSize(d,w,h,level);require(off<=b.size()&&len<=b.size()-off,"DDS mipmap 数据不完整");
 Bytes pixels(4ull*w*h);
 if(d.format==DdsFormat::RGB){
  const auto stride=len/h;
  std::array<PixelChannel,4> masks{};for(unsigned c=0;c<4;c++)masks[c]=pixelChannel(d.masks[c]);
  for(unsigned y=0;y<h;y++)for(unsigned x=0;x<w;x++){
   const auto* p=b.data()+off+stride*y+(d.bits/8)*x;
   unsigned v=unsigned(p[0])|(unsigned(p[1])<<8)|(unsigned(p[2])<<16);if(d.bits==32)v|=unsigned(p[3])<<24;
   auto* dst=pixels.data()+4ull*(y*w+x);for(unsigned c=0;c<4;c++)dst[c]=component(v,masks[c],c==3?255:0);
  }
  return pixels;
 }
 const auto block=ddsBlock(d.format),bx=(w+3)/4,by=(h+3)/4;
 for(unsigned y=0;y<by;y++)for(unsigned x=0;x<bx;x++){
  // DDS payloads need not be aligned to the loads used by bcdec.
  alignas(8) uint8_t input[16];std::memcpy(input,b.data()+off+(uint64_t(y)*bx+x)*block,block);
  alignas(8) uint8_t rgba[64]={};uint8_t rg[32]={};float rgb[48]={};
  switch(d.format){
   case DdsFormat::RGB:break;
   case DdsFormat::BC1:bcdec_bc1(input,rgba,16);break;
   case DdsFormat::BC2:bcdec_bc2(input,rgba,16);break;
   case DdsFormat::BC3:bcdec_bc3(input,rgba,16);break;
   case DdsFormat::BC4:bcdec_bc4(input,rg,4);for(int k=0;k<16;k++){rgba[k*4]=rg[k];rgba[k*4+3]=255;}break;
   case DdsFormat::BC5:bcdec_bc5(input,rg,8);for(int k=0;k<16;k++){rgba[k*4]=rg[k*2];rgba[k*4+1]=rg[k*2+1];rgba[k*4+3]=255;}break;
   case DdsFormat::BC6U:case DdsFormat::BC6S:bcdec_bc6h_float(input,rgb,12,d.format==DdsFormat::BC6S);for(int k=0;k<16;k++){for(int c=0;c<3;c++)rgba[k*4+c]=uint8_t(std::round(std::clamp(rgb[k*3+c],0.f,1.f)*255));rgba[k*4+3]=255;}break;
   case DdsFormat::BC7:bcdec_bc7(input,rgba,16);break;
  }
  for(unsigned j=0;j<4&&y*4+j<h;j++)for(unsigned i=0;i<4&&x*4+i<w;i++)std::memcpy(pixels.data()+4ull*((y*4+j)*w+x*4+i),rgba+4*(j*4+i),4);
 }
 return pixels;
}
Bytes filterMip(const uint8_t* src,unsigned w,unsigned h,bool srgb){
 const unsigned nw=std::max(1u,w/2),nh=std::max(1u,h/2);
 static const auto linear=[](){std::array<float,256> values{};for(unsigned i=0;i<256;i++){
  const float v=i/255.f;values[i]=v<=.04045f?v/12.92f:std::pow((v+.055f)/1.055f,2.4f);
 }return values;}();
 static const auto encoded=[](){std::array<uint8_t,65536> values{};for(unsigned i=0;i<values.size();i++){
  const float v=i/65535.f;values[i]=uint8_t(std::lround(255*(v<=.0031308f?12.92f*v:1.055f*std::pow(v,1/2.4f)-.055f)));
 }return values;}();
 Bytes next(4ull*nw*nh);
 for(unsigned y=0;y<nh;y++)for(unsigned x=0;x<nw;x++){
  const unsigned x0=x*w/nw,x1=(x+1)*w/nw,y0=y*h/nh,y1=(y+1)*h/nh,count=(x1-x0)*(y1-y0);
  float samples[4]{};
  for(unsigned j=y0;j<y1;j++)for(unsigned i=x0;i<x1;i++)for(unsigned c=0;c<4;c++){
   const auto v=src[4ull*(j*w+i)+c];samples[c]+=srgb&&c!=3?linear[v]:v;
  }
  for(unsigned c=0;c<4;c++)next[4ull*(y*nw+x)+c]=srgb&&c!=3?
   encoded[size_t(std::lround(samples[c]/count*65535.f))]:uint8_t(std::lround(samples[c]/count));
 }
 return next;
}
}
Texture decodeDds(const Bytes& b){const auto d=ddsInfo(b);return {d.w,d.h,ddsLevel(b,d,d.w,d.h,0,d.offset)};}
void buildTextureMips(Texture& texture,bool roleLinear){
 require(texture.w&&texture.h&&texture.mips==1&&texture.pixels.size()==4ull*texture.w*texture.h,"RGBA 贴图数据无效");
 const bool srgb=!texture.linear(roleLinear);
 unsigned w=texture.w,h=texture.h;size_t last=0;
 texture.pixels.reserve(textureBytes(w,h,textureMipCount(w,h)));
 while(w>1||h>1){
  auto mip=filterMip(texture.pixels.data()+last,w,h,srgb);
  last=texture.pixels.size();
  texture.pixels.insert(texture.pixels.end(),mip.begin(),mip.end());
  ++texture.mips;
  w=std::max(1u,w/2);h=std::max(1u,h/2);
 }
}
Texture decodeModelDds(const Bytes& b,bool roleLinear,TextureColor overrideColor){
 const auto d=ddsInfo(b);require(d.levels<=textureMipCount(d.w,d.h),"DDS mipmap 数超出尺寸允许的范围");
 const bool canOverride=d.format<=DdsFormat::BC3||d.format==DdsFormat::BC7;
 Texture out{0,0,{},0,canOverride&&overrideColor!=TextureColor::Role?overrideColor:d.color};
 const bool srgb=!out.linear(roleLinear);
 unsigned w=d.w,h=d.h;size_t offset=d.offset,last=0;
 for(unsigned level=0;level<d.levels;level++){
  const auto size=ddsLevelSize(d,w,h,level);require(offset<=b.size()&&size<=b.size()-offset,"DDS mipmap 数据不完整");
  if((w<=1024&&h<=1024)||level+1==d.levels){
   auto pixels=ddsLevel(b,d,w,h,level,offset);
   while(w>1024||h>1024){pixels=filterMip(pixels.data(),w,h,srgb);w=std::max(1u,w/2);h=std::max(1u,h/2);}
   if(!out.mips){out.w=w;out.h=h;out.pixels.reserve(textureBytes(w,h,textureMipCount(w,h)));}
   last=out.pixels.size();out.pixels.insert(out.pixels.end(),pixels.begin(),pixels.end());out.mips++;
  }
  offset+=size;
  if(level+1<d.levels){w=std::max(1u,w/2);h=std::max(1u,h/2);}
 }
 while(w>1||h>1){
  auto pixels=filterMip(out.pixels.data()+last,w,h,srgb);
  last=out.pixels.size();out.pixels.insert(out.pixels.end(),pixels.begin(),pixels.end());out.mips++;
  w=std::max(1u,w/2);h=std::max(1u,h/2);
 }
 return out;
}
}
