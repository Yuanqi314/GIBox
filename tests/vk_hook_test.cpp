// The one-instruction redirect of vk_hook.cpp, checked as bytes on the host (the code is ARM64 and is not run here):
// a [bti c] + b stub keeps its landing pad, its b goes to our page, our page goes on to the hook and back to the target;
// both entries are ready before either stub is written.
#include <cstdint>
#include <cstring>
#include <sys/mman.h>
#include <unistd.h>
static int patches=0;
extern "C" int DobbyCodePatch(void* address,uint8_t* buffer,uint32_t size){std::memcpy(address,buffer,size);patches++;return 0;}
#include "../app/src/main/cpp/vk_hook.cpp"
#include <cassert>
#include <iostream>
static uintptr_t target(uint32_t w,uintptr_t at){int64_t imm=int64_t(w&0x03FFFFFFu);if(imm&0x02000000)imm-=0x04000000;return uintptr_t(int64_t(at)+imm*4);}
#ifdef GIBOX_VK_TINT
template<class H>static H fake(uintptr_t id){return reinterpret_cast<H>(id);}
static void tintTrial(){
 const auto d=fake<VkDevice>(1);const auto c=fake<VkCommandBuffer>(2);const auto p=fake<VkPipeline>(3);
 const auto layout=fake<VkPipelineLayout>(4);const auto original=fake<VkDescriptorSet>(5);const auto a=fake<VkImageView>(6),b=fake<VkImageView>(7);
 auto& dev=tintState().devices[d];dev.pipelineLayouts[layout]={2};dev.pipelines[p]=layout;
 tintState().commands[c]={d,TintCmd{}};
 TintSet& game=dev.sets[original];game.shape=2;game.written=3;
 game.images[0]={fake<VkSampler>(8),a,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
 game.images[1]={fake<VkSampler>(9),b,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
 dev.images[a].info.image=fake<VkImage>(10);dev.images[a].info.components.a=VK_COMPONENT_SWIZZLE_A;dev.images[a].generation=1;
 static std::vector<uintptr_t> call;static unsigned creates=0,updates=0;
 tintCreateDescriptorPool=+[](VkDevice,const VkDescriptorPoolCreateInfo*,const VkAllocationCallbacks*,VkDescriptorPool* out){*out=fake<VkDescriptorPool>(11);return VK_SUCCESS;};
 tintCreateDescriptorSetLayout=+[](VkDevice,const VkDescriptorSetLayoutCreateInfo* info,const VkAllocationCallbacks*,VkDescriptorSetLayout* out){assert(tintShape(*info)==2);*out=fake<VkDescriptorSetLayout>(12);return VK_SUCCESS;};
 tintCreateImageView=+[](VkDevice,const VkImageViewCreateInfo* info,const VkAllocationCallbacks*,VkImageView* out){
  assert(info->image==fake<VkImage>(10)&&info->components.a==VK_COMPONENT_SWIZZLE_A);
  assert((creates==0&&info->components.r==VK_COMPONENT_SWIZZLE_ZERO&&info->components.g==VK_COMPONENT_SWIZZLE_ONE&&info->components.b==VK_COMPONENT_SWIZZLE_ZERO)||
         (creates==1&&info->components.r==VK_COMPONENT_SWIZZLE_ONE&&info->components.g==VK_COMPONENT_SWIZZLE_ZERO&&info->components.b==VK_COMPONENT_SWIZZLE_ONE));
  *out=fake<VkImageView>(20+creates++);return VK_SUCCESS;};
 tintAllocateDescriptorSets=+[](VkDevice,const VkDescriptorSetAllocateInfo*,VkDescriptorSet* out){*out=fake<VkDescriptorSet>(30+updates);return VK_SUCCESS;};
 tintUpdateDescriptorSets=+[](VkDevice,uint32_t n,const VkWriteDescriptorSet* writes,uint32_t copies,const VkCopyDescriptorSet*){
  assert(n==2&&!copies&&writes[0].dstSet==fake<VkDescriptorSet>(30+updates));
  assert(writes[0].pImageInfo->imageView==fake<VkImageView>(20+updates)&&writes[0].pImageInfo->sampler==fake<VkSampler>(8));
  assert(writes[1].pImageInfo->imageView==fake<VkImageView>(7)&&writes[1].pImageInfo->sampler==fake<VkSampler>(9));++updates;};
 tintCmdBindPipeline=+[](VkCommandBuffer,VkPipelineBindPoint,VkPipeline){};
 tintCmdBindDescriptorSets=+[](VkCommandBuffer,VkPipelineBindPoint,VkPipelineLayout l,uint32_t first,uint32_t n,const VkDescriptorSet* sets,uint32_t offsets,const uint32_t*){
  assert(l==fake<VkPipelineLayout>(4)&&first==0&&n==1&&!offsets);call.push_back(reinterpret_cast<uintptr_t>(*sets));};
 cmdDrawIndexed=+[](void* cm,uint32_t n,uint32_t inst,uint32_t first,int32_t vertex,uint32_t fi){
  assert(cm==fake<VkCommandBuffer>(2)&&!vertex&&!fi&&inst==1&&((n==26226&&!first)||(n==4386&&first==26226)));call.push_back(100+first);};
 tintOnCmdBindPipeline(c,VK_PIPELINE_BIND_POINT_GRAPHICS,p);
 tintOnCmdBindDescriptorSets(c,VK_PIPELINE_BIND_POINT_GRAPHICS,layout,0,1,&original,0,nullptr);call.clear();
 // Synthetic buffer with the known CRC-32C variant. It is never sent to a driver or interpreted as geometry.
 std::vector<uint8_t> data(61224);const uint32_t tail=0x86c821d5;std::memcpy(data.data()+data.size()-4,&tail,4);
 T().allForms=false;T().probeOnly=true;T().setDrawHashes({ms::vkcap::paimonBodyHash});T().tintHooks=true;T().tintOn=true;
 T().created(40,data.size(),0x40);T().bound(40,41,0);T().mapped(41,0,data.size(),data.data());T().indexBound(40,0,reinterpret_cast<uintptr_t>(c),0);
 onCmdDrawIndexed(c,26226,1,0,0,0);onCmdDrawIndexed(c,4386,1,26226,0,0);onCmdDrawIndexed(c,26226,1,0,0,0);
 assert((call==std::vector<uintptr_t>{30,100,5,31,26326,5,30,100,5})&&creates==2&&updates==2&&T().tintDraws[0]==2&&T().tintDraws[1]==1);
 assert(game.images[0].imageView==a&&game.images[1].imageView==b);
 call.clear();T().setPaimonReplacement(false,true,"");onCmdDrawIndexed(c,26226,1,0,0,0);assert((call==std::vector<uintptr_t>{100}));
 call.clear();T().setPaimonReplacement(true,true,"");tintBegin(c);onCmdDrawIndexed(c,26226,1,0,0,0);assert((call==std::vector<uintptr_t>{100})&&T().tintMisses[0]==1);
 // Real views are substituted only after upload and only for the queue family that owns the images.
 tintOnCmdBindPipeline(c,VK_PIPELINE_BIND_POINT_GRAPHICS,p);
 tintOnCmdBindDescriptorSets(c,VK_PIPELINE_BIND_POINT_GRAPHICS,layout,0,1,&original,0,nullptr);call.clear();
 game.cache={};T().paimonTextureMod="sample";T().setPaimonReplacement(true,false,"sample");
 onCmdDrawIndexed(c,26226,1,0,0,0);assert((call==std::vector<uintptr_t>{100}));
 dev.upload.queued=true;dev.upload.usedFamily=8;dev.upload.images[0].view=fake<VkImageView>(22);dev.upload.images[1].view=fake<VkImageView>(23);
 dev.families[VkCommandPool{}]=9;call.clear();onCmdDrawIndexed(c,26226,1,0,0,0);assert((call==std::vector<uintptr_t>{100}));
 dev.families[VkCommandPool{}]=8;call.clear();onCmdDrawIndexed(c,26226,1,0,0,0);onCmdDrawIndexed(c,4386,1,26226,0,0);
 assert((call==std::vector<uintptr_t>{32,100,5,33,26326,5})&&creates==2&&updates==4);
 T().setPaimonReplacement(true,true,"other");call.clear();onCmdDrawIndexed(c,26226,1,0,0,0);assert((call==std::vector<uintptr_t>{100}));
 // A skin draw changes all geometry bindings but restores them before returning to the game's recorder.
 T().skinnedModel=true;T().setPaimonReplacement(true,false,"sample");
 auto& state=tintState().commands[c].second;state.index=fake<VkBuffer>(101);state.indexType=VK_INDEX_TYPE_UINT16;
 state.vertex={fake<VkBuffer>(102),fake<VkBuffer>(103),fake<VkBuffer>(104)};
 auto& skin=dev.skin;skin.ready=true;skin.graphics[p]=3;
 skin.input[Attributes].buffer=fake<VkBuffer>(105);skin.input[Indices].buffer=fake<VkBuffer>(106);
 skin.output[state.vertex[0]]={{fake<VkBuffer>(107),{},10000},true};
 skin.output[state.vertex[2]]={{fake<VkBuffer>(108),{},10000},true};
 paimonSkin.slots[0]={6,120};
 upCmdBindVertexBuffers=+[](VkCommandBuffer,uint32_t first,uint32_t n,const VkBuffer* buffers,const VkDeviceSize*){
  assert(first==0&&n==3);auto id=reinterpret_cast<uintptr_t>(buffers[0]);
  assert((id==107&&buffers[1]==fake<VkBuffer>(105)&&buffers[2]==fake<VkBuffer>(108))||
    (id==102&&buffers[1]==fake<VkBuffer>(103)&&buffers[2]==fake<VkBuffer>(104)));call.push_back(id);};
 cmdBindIndexBuffer=+[](void*,uint64_t buffer,uint64_t offset,int32_t type){assert(!offset&&type==VK_INDEX_TYPE_UINT16);call.push_back(buffer);};
 cmdDrawIndexed=+[](void*,uint32_t n,uint32_t i,uint32_t start,int32_t v,uint32_t inst){assert(n==120&&i==1&&start==6&&!v&&!inst);call.push_back(120);};
 call.clear();onCmdDrawIndexed(c,26226,1,0,0,0);
 assert((call==std::vector<uintptr_t>{32,107,106,120,101,102,5}));
 // Different mesh profiles learn independent input buffers from the confirmed graphics draw, not from a name or shape alone.
 T().allSkins=true;T().skinMods={"Alpha","Beta"};T().setSkinReplacement(true,true,{"Alpha","Beta"});
 sharedSkins.resize(2);skin.profiles.resize(2);
 for(unsigned i=0;i<2;i++){
  auto& profile=sharedSkins[i];profile.originalVertices=123+i;profile.bones=10+i;profile.mesh.slots[0]={6+i,120+i*3};
  profile.images={~0u,~0u};auto& gpu=skin.profiles[i];gpu.ready=true;
  gpu.input[Indices].buffer=fake<VkBuffer>((i+1)*1000);gpu.input[Attributes].buffer=fake<VkBuffer>((i+1)*1000+1);
  state.vertex[0]=fake<VkBuffer>((i+1)*100);state.vertex[2]=fake<VkBuffer>((i+1)*100+1);
  const auto rest=fake<VkBuffer>((i+1)*100+2),weights=fake<VkBuffer>((i+1)*100+3);
  skin.seenOutputs[state.vertex[0]]={rest,weights,profile.originalVertices,profile.bones};
  assert(!tintDraw(c,0,{i,0}).replacement&&skin.sourceProfiles.at({rest,weights})==i);
  skin.sharedOutput[{state.vertex[0],i}]={{fake<VkBuffer>((i+1)*100+4),{},120},true};
  auto plan=tintDraw(c,0,{i,0});assert(plan.replacement==original&&plan.slot.count==120+i*3&&plan.newIndex==gpu.input[Indices].buffer);
 }
 std::cout<<"Vulkan draw replacement: PASS (scoped textures, skinned vertex and index rebind, queue ownership, restoration and master switch)\n";
}
#endif
int main(){
 const size_t page=size_t(sysconf(_SC_PAGESIZE));auto* code=static_cast<uint32_t*>(mmap(nullptr,page*4,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0));assert(code!=MAP_FAILED);
 // bti c; b +0x1000 (the loader's internal function, reached only by this direct branch); a plain "b" stub next to it.
 code[0]=0xD503245Fu;code[1]=0x14000000u|(0x1000/4);uintptr_t stub=reinterpret_cast<uintptr_t>(code),internal=stub+4+0x1000;
 code[64]=0x14000000u|8;uintptr_t plain=reinterpret_cast<uintptr_t>(code+64);
 uint32_t* island=islandNear(stub);assert(island);std::string why;int hook=0,hook2=0;Redirect a,b;
 assert(prepare(stub,&hook,island,a,why)&&prepare(plain,&hook2,island+8,b,why)&&patches==0&&code[1]==(0x14000000u|(0x1000/4)));   // nothing in the stubs yet
 assert(patch(island,{&a,&b},why)&&patches==2&&a.original==island+4&&b.original==island+12);
 assert(code[0]==0xD503245Fu&&(code[1]&0xFC000000u)==0x14000000u&&target(code[1],stub+4)==reinterpret_cast<uintptr_t>(island));   // landing pad kept, b -> island
 uint64_t h;std::memcpy(&h,island+2,8);assert(island[0]==0x58000051u&&island[1]==0xD61F0220u&&h==reinterpret_cast<uint64_t>(&hook));   // ldr x17,#8; br x17; .quad hook
 assert((island[4]&0xFC000000u)==0x14000000u&&target(island[4],reinterpret_cast<uintptr_t>(island+4))==internal);   // the original: b internal
 assert(a.what.find("入口 d503245f 14000400")==0&&a.what.find("+4 处的 b")!=std::string::npos);
 assert(target(code[64],plain)==reinterpret_cast<uintptr_t>(island+8)&&target(island[12],reinterpret_cast<uintptr_t>(island+12))==plain+32&&b.what.find("+0 处的 b")!=std::string::npos);
 // Anything else is refused and left as it was.
 code[128]=0xA9BF7BFDu;code[129]=0x910003FDu;Redirect c;assert(!prepare(reinterpret_cast<uintptr_t>(code+128),&hook,island+16,c,why)&&why.find("不是 [bti] + b 形式的入口，开头为 a9bf7bfd 910003fd")==0&&code[128]==0xA9BF7BFDu);
 std::cout<<"Vulkan stub redirect: PASS (landing pad kept, direct b to our page and back, both ready before writing, other shapes refused)\n";
#ifdef GIBOX_VK_TINT
 tintTrial();
#endif
}
