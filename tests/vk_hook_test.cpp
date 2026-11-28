// The one-instruction redirect of vk_hook.cpp, checked as bytes on the host (the code is ARM64 and is not run here):
// a [bti c] + b stub keeps its landing pad, its b goes to our page, our page goes on to the hook and back to the target;
// both entries are ready before either stub is written.
#include <array>
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
static void livePixelBindingTrial(){
 ms::LogicSpec spec;
 spec.files.push_back({"","",R"ini(
[Constants]
global $material = 0
[ResourceIB]
[ResourceEffect]
filename = effect.dds
[ResourceColor]
filename = alt_diffuse.dds
[TextureOverrideBody]
hash = 11223344
handling = skip
ib = ResourceIB
if $material
 ps-t8 = ResourceColor
else
 ps-t8 = ResourceEffect
 ps-t69 = null
endif
drawindexed = 6, 0, 0
)ini"});
 spec.resources={{"resourceib","mesh"},{"resourceeffect","file:effect.dds"},{"resourcecolor","file:alt_diffuse.dds"}};
 spec.parts["Body"]={{"ib","Body","ib"}};
 ms::LogicDriver driver(spec,{},ms::LogicDriver::Source::Live);
 migoto::DrawInfo d;d.live=true;d.checks={"ib"};d.bindings["ib"]=migoto::Ref::game(0x11223344);
 assert(driver.drawLive("Body",d).total==1);
 assert(driver.set("$material",1));
 assert(driver.drawLive("Body",d).signature.empty()); // A real material texture needs a recorded slot.
}
static void pcRouteTrial(){
 using namespace ms;
 pcdb::Mesh mesh;mesh.mesh="Body";mesh.avatar="avatar_test";mesh.vertices=100;mesh.subs={{0,120},{120,240}};
 pcdb::DeviceMap device;pcdb::Db db;pcdb::Component component;
 component.avatar=10000107;component.indexes={0,120};component.counts={120,240};
 db.byIb["11223344"]={0};db.comps.push_back(component);
 PartSummary part;part.sourceDraws={{"11223344",0,0,120},{"11223344",1,120,240}};
 assert(ms::vkhook::pcSkinHashes(part,{},mesh,device,db,10000107)==std::set<uint32_t>{0x11223344});
 part.sourceDraws[1].count=239;
 assert(ms::vkhook::pcSkinHashes(part,{},mesh,device,db,10000107).empty());
 part.sourceDraws[1].count=240;
 device.byIb["11223344"]=mesh;device.byIb["11223344"].avatar="avatar_other";
 assert(ms::vkhook::pcSkinHashes(part,{},mesh,device,db,10000107).empty());
 device.byIb["11223344"]=mesh;part.sourceDraws.clear();
 LogicSpec legacy;legacy.passes={{"BodyA",{{"ib",0x11223344}},{},{{"first_index",0}}},
                                 {"BodyB",{{"ib",0x11223344}},{},{{"first_index",120}}}};
 assert(ms::vkhook::pcSkinHashes(part,legacy,mesh,device,db,10000107)==std::set<uint32_t>{0x11223344});
 assert(ms::vkhook::pcSkinHashes(part,legacy,mesh,pcdb::DeviceMap{},db,10000107).empty());
 legacy.passes.pop_back();
 assert(ms::vkhook::pcSkinHashes(part,legacy,mesh,device,db,10000107).empty());
}
static void vertexLayoutTrial(){
 std::array<VkVertexInputBindingDescription,3> bindings{{{0,40,VK_VERTEX_INPUT_RATE_VERTEX},
  {1,12,VK_VERTEX_INPUT_RATE_VERTEX},{2,0,VK_VERTEX_INPUT_RATE_VERTEX}}};
 std::array<VkVertexInputAttributeDescription,5> attributes{{{0,0,VK_FORMAT_R32G32B32_SFLOAT,0},
  {6,1,VK_FORMAT_R8G8B8A8_UNORM,0},{2,1,VK_FORMAT_R32G32_SFLOAT,4},
  {3,1,VK_FORMAT_R32G32_SFLOAT,4},{4,2,VK_FORMAT_R8G8B8A8_UNORM,12}}};
 VkPipelineVertexInputStateCreateInfo vertex{};
 vertex.vertexBindingDescriptionCount=bindings.size();vertex.pVertexBindingDescriptions=bindings.data();
 vertex.vertexAttributeDescriptionCount=attributes.size();vertex.pVertexAttributeDescriptions=attributes.data();
 VkPipelineInputAssemblyStateCreateInfo assembly{};assembly.topology=VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
 VkGraphicsPipelineCreateInfo pipeline{};pipeline.pVertexInputState=&vertex;pipeline.pInputAssemblyState=&assembly;
 auto packed=skinDrawLayout(pipeline,nullptr);
 assert(packed.bindings==3&&packed.attributes.stride==12&&!packed.attributes.packed());
 ms::VulkanSkinPayload mesh;mesh.vertices=2;
 for(unsigned b=0;b<40;b++)mesh.attributes.push_back(uint8_t(b+1));
 auto repacked=packSkinAttributes(mesh,packed.attributes);
 assert(repacked.size()==24&&repacked[0]==1&&repacked[11]==12&&repacked[12]==21&&repacked[23]==32);
 bindings[1].stride=24;
 auto padded=skinDrawLayout(pipeline,nullptr);
 assert(padded.bindings==3&&padded.attributes.stride==24&&packSkinAttributes(mesh,padded.attributes)[23]==0);
 bindings[1].stride=8;attributes[1]={2,1,VK_FORMAT_R32G32_SFLOAT,0};vertex.vertexAttributeDescriptionCount=2;
 std::array<uint8_t,32> semantics{};semantics[2]=5;
 auto uvOnly=skinDrawLayout(pipeline,&semantics);
 assert(uvOnly.bindings==3&&uvOnly.attributes.stride==8&&packSkinAttributes(mesh,uvOnly.attributes)[8]==25);
 assert(!skinDrawLayout(pipeline,nullptr).bindings);
 bindings[1].stride=12;attributes[1]={6,1,VK_FORMAT_R8G8B8A8_UNORM,0};vertex.vertexAttributeDescriptionCount=attributes.size();
 bindings[0].stride=32;assert(!skinDrawLayout(pipeline,nullptr).bindings);bindings[0].stride=40;
 attributes[0].offset=12;assert(!skinDrawLayout(pipeline,nullptr).bindings);attributes[0].offset=0;
 const VkDynamicState topology=VK_DYNAMIC_STATE_PRIMITIVE_TOPOLOGY;
 VkPipelineDynamicStateCreateInfo dynamic{};dynamic.dynamicStateCount=1;dynamic.pDynamicStates=&topology;
 pipeline.pDynamicState=&dynamic;assert(!skinDrawLayout(pipeline,nullptr).bindings);pipeline.pDynamicState=nullptr;
 VkPipelineShaderStageCreateInfo stage{};stage.stage=VK_SHADER_STAGE_VERTEX_BIT;stage.module=fake<VkShaderModule>(71);stage.pName="main";
 pipeline.stageCount=1;pipeline.pStages=&stage;
 const auto device=fake<VkDevice>(72);VkPipeline handle{};
 tintCreateGraphicsPipelines=+[](VkDevice,VkPipelineCache,uint32_t,const VkGraphicsPipelineCreateInfo*,const VkAllocationCallbacks*,VkPipeline* out){
  *out=fake<VkPipeline>(73);return VK_SUCCESS;
 };
 T().allSkins=true;
 assert(tintOnCreateGraphicsPipelines(device,{},1,&pipeline,nullptr,&handle)==VK_SUCCESS);
 auto& tracked=tintState().devices.at(device);
 assert(tracked.skin.drawLayouts.at(handle).bindings==3&&tracked.pipelines.empty());
 tintState().devices.erase(device);T().allSkins=false;
}
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
 skin.drawLayouts[p]={7,{20,{}}};
 sharedSkins.resize(2);skin.profiles.resize(2);
 for(unsigned i=0;i<2;i++){
  auto& profile=sharedSkins[i];profile.originalVertices=123+i;profile.bones=10+i;profile.mesh->slots[0]={6+i,120+i*3};
  profile.images={~0u,~0u};auto& gpu=skin.profiles[i];gpu.ready=true;
  gpu.input[Indices].buffer=fake<VkBuffer>((i+1)*1000);gpu.input[Attributes].buffer=fake<VkBuffer>((i+1)*1000+1);
  state.vertex[0]=fake<VkBuffer>((i+1)*100);state.vertex[2]=fake<VkBuffer>((i+1)*100+1);
  const auto rest=fake<VkBuffer>((i+1)*100+2),weights=fake<VkBuffer>((i+1)*100+3);
  skin.seenOutputs[state.vertex[0]]={rest,weights,profile.originalVertices,profile.bones};
  assert(!tintDraw(c,0,{i,0}).replacement&&skin.sourceProfiles.at({rest,weights})==Profiles{i});
  skin.sharedOutput[{state.vertex[0],i}]={{fake<VkBuffer>((i+1)*100+4),{},120},true};
  auto plan=tintDraw(c,0,{i,0});assert(plan.replacement==original&&plan.slot.count==120+i*3&&plan.newIndex==gpu.input[Indices].buffer);
 }
 auto& profile=sharedSkins[1];profile.originalVertices=0;profile.bones=0;profile.mesh->requiredBones=21;
 profile.iniDraws={{{{8,6},{},{}},{{14,9},{},{}}}};
 for(auto& row:profile.iniDraws[0])row.images={~0u,~0u,~0u};
 profile.variants.vars={{"$vision",{0,1}}};profile.variants.defaults={{"$vision",1}};
 profile.replaceWhen.vars={"$vision"};profile.replaceWhen.allow={{0}};
 assert(!tintDraw(c,0,{1,0}).replacement);
 assert(T().skinBoneErrors[1]==(21u<<16|11u));
 skin.seenOutputs[state.vertex[0]].bones=21;
 assert(!tintDraw(c,0,{1,0}).replacement); // Default keeps the game's draw.
 T().setSkinChoices({{"Beta",{{"$vision",0}}}});
 auto generic=tintDraw(c,0,{1,0});assert(generic.replacement==original&&generic.draws.size()==2&&generic.draws[0].first.start==8&&generic.draws[1].first.count==9);
 const auto output=state.vertex[0],bones=fake<VkBuffer>(901);
 const auto seen=skin.seenOutputs.at(output);
 const std::array<VkDescriptorBufferInfo,4> job{{{seen.rest,0,40ull*seen.vertices},{seen.weights,0,32ull*seen.vertices},
                                                {bones,0,64ull*seen.bones},{output,0,40ull*seen.vertices}}};
 skin.storage=fake<VkDescriptorSetLayout>(906);skin.uniform=fake<VkDescriptorSetLayout>(907);skin.pool=fake<VkDescriptorPool>(908);
 auto& gpu=skin.profiles[1];gpu.count=fake<VkDescriptorSet>(902);
 skin.sharedRemaps[{output,bones,1}]=fake<VkDescriptorSet>(909);
 upCmdPipelineBarrier=+[](VkCommandBuffer,VkPipelineStageFlags,VkPipelineStageFlags,VkDependencyFlags,uint32_t,const VkMemoryBarrier*,uint32_t,const VkBufferMemoryBarrier*,uint32_t,const VkImageMemoryBarrier*){};
 static unsigned dispatches=0;
 upCmdDispatch=+[](VkCommandBuffer,uint32_t x,uint32_t y,uint32_t z){assert(x==1&&y==1&&z==1);++dispatches;};
 tintCmdBindDescriptorSets=+[](VkCommandBuffer,VkPipelineBindPoint,VkPipelineLayout,uint32_t,uint32_t,const VkDescriptorSet*,uint32_t,const uint32_t*){};
 profile.mesh->vertices=30;
 sharedSkinDispatch(d,c,state,dev,job,(seen.vertices+63)/64);
 assert(dispatches==1&&skin.sharedOutput.at({output,1}).revision==skin.seenOutputs.at(output).revision);
 T().iniSkinRules[ms::vkcap::paimonBodyHash]={{1,0,0,26226}};
 upCmdBindVertexBuffers=+[](VkCommandBuffer,uint32_t,uint32_t n,const VkBuffer* buffers,const VkDeviceSize*){
  assert(n==3);call.push_back(reinterpret_cast<uintptr_t>(buffers[0]));};
 cmdBindIndexBuffer=+[](void*,uint64_t b,uint64_t o,int32_t){assert(!o);call.push_back(b);};
 cmdDrawIndexed=+[](void*,uint32_t count,uint32_t,uint32_t first,int32_t,uint32_t){call.push_back(first+count);};
 call.clear();onCmdDrawIndexed(c,26226,1,0,0,0);
 assert((call==std::vector<uintptr_t>{204,2000,14,23,101,200})&&dispatches==1);
 auto shortJob=job;shortJob[2].range=20*64;
 sharedSkinDispatch(d,c,state,dev,shortJob,(seen.vertices+63)/64);
 call.clear();onCmdDrawIndexed(c,26226,1,0,0,0);
 assert((call==std::vector<uintptr_t>{26226})&&dispatches==1);
 profile.rigid=true;skin.drawLayouts[p].bindings=3;state.vertex[0]=fake<VkBuffer>(800);
 gpu.input[Rest].buffer=fake<VkBuffer>(801);
 upCmdBindVertexBuffers=+[](VkCommandBuffer,uint32_t,uint32_t n,const VkBuffer* buffers,const VkDeviceSize*){
  assert(n==2);call.push_back(reinterpret_cast<uintptr_t>(buffers[0]));};
 call.clear();onCmdDrawIndexed(c,26226,1,0,0,0);
 assert((call==std::vector<uintptr_t>{801,2000,14,23,101,800})&&dispatches==1);
 // An original depth or material pass without a supported texture descriptor can still use the skinned streams.
 profile.rigid=false;skin.drawLayouts[p].bindings=5;state.vertex[0]=output;state.set={};dev.pipelines.erase(p);
 sharedSkinDispatch(d,c,state,dev,job,(seen.vertices+63)/64);
 upCmdBindVertexBuffers=+[](VkCommandBuffer,uint32_t first,uint32_t n,const VkBuffer* buffers,const VkDeviceSize*){
  assert(n==1&&(first==0||first==2));call.push_back(reinterpret_cast<uintptr_t>(buffers[0]));};
 call.clear();onCmdDrawIndexed(c,26226,1,0,0,0);
 assert((call==std::vector<uintptr_t>{204,204,2000,14,23,101,200,201})&&dispatches==2);
 // A pass can sample the lightmap alone. Use its named slot and preserve the other game sampler and the next draw.
 state.set=original;dev.pipelines[p]=layout;dev.textureSlots[p]={~0u,1,~0u,~0u};profile.iniDraws[0][0].images={0,1,~0u};
 tintUpdateDescriptorSets=+[](VkDevice,uint32_t n,const VkWriteDescriptorSet* writes,uint32_t copies,const VkCopyDescriptorSet*){
  assert(n==2&&!copies&&writes[0].dstSet==fake<VkDescriptorSet>(34));
  assert(writes[0].pImageInfo->imageView==fake<VkImageView>(6)&&writes[0].pImageInfo->sampler==fake<VkSampler>(8));
  assert(writes[1].pImageInfo->imageView==fake<VkImageView>(23)&&writes[1].pImageInfo->sampler==fake<VkSampler>(9));++updates;
 };
 tintCmdBindDescriptorSets=+[](VkCommandBuffer,VkPipelineBindPoint point,VkPipelineLayout l,uint32_t first,uint32_t n,const VkDescriptorSet* sets,uint32_t,const uint32_t*){
  assert(point==VK_PIPELINE_BIND_POINT_GRAPHICS&&l==fake<VkPipelineLayout>(4)&&!first&&n==1);
  call.push_back(reinterpret_cast<uintptr_t>(*sets));
 };
 const auto expected=std::vector<uintptr_t>{34,204,204,2000,14,5,23,101,200,201};
 for(unsigned i=0;i<2;i++){
  call.clear();onCmdDrawIndexed(c,26226,1,0,0,0);
  assert(call==expected&&updates==5&&dispatches==2&&T().lightMapDraws==i+1);
 }
 // A texture absent from the upload requests only the sampled role and leaves the game draw intact.
 sharedImages.resize(2);auto lightView=dev.upload.images[1].view;dev.upload.images[1].view={};
 warmSkinImages(dev,{1},true);
 assert((dev.upload.warm==std::set<unsigned>{0,1}));
 call.clear();onCmdDrawIndexed(c,26226,1,0,0,0);
 assert((call==std::vector<uintptr_t>{26226})&&dev.upload.requested==std::set<unsigned>{1});
 dev.upload.images[1].view=lightView;dev.upload.requested.clear();dev.upload.warm.clear();sharedImages.clear();
 // The same draw executor can select its model by a Vulkan shader and vertex upload without a PC IB rule.
 T().iniSkinRules.clear();
 const uint32_t code[]={0x07230203u,0x10000u,0u,8u,0u};const unsigned usedVertex=203;
 const auto hash=ms::vkcap::crc32c(0,code,sizeof code);const auto command=reinterpret_cast<uintptr_t>(c);
 T().resourceRules.push_back({{1,0,0,26226},{{{ms::vkcap::ResourceKind::Vertex,0,1},hash},{{ms::vkcap::ResourceKind::Shader,0,16},hash}}});
 T().created(usedVertex,sizeof code,0x80);T().bound(usedVertex,111,0);T().mapped(111,0,sizeof code,code);state.vertex[1]=fake<VkBuffer>(usedVertex);
 T().bindVertex(command,1,usedVertex,0);T().shaderCreated(210,code,sizeof code);
 T().pipelineCreated(gpuHandle(p),gpuHandle(layout),{{16,210}});T().bindPipeline(command,false,gpuHandle(p));
 call.clear();onCmdDrawIndexed(c,26226,1,0,0,0);assert(call==expected&&T().lightMapDraws==3);
 T().pipelineDestroyed(gpuHandle(p));call.clear();onCmdDrawIndexed(c,26226,1,0,0,0);assert((call==std::vector<uintptr_t>{26226}));
 // A live draw runs the INI with the tracked IB, VB and call arguments. Present advances it once, then the
 // replacement draws bracket the game's own draw and restore its actual bindings before it runs.
 T().resourceRules.clear();T().iniSkinRules[ms::vkcap::paimonBodyHash]={{1,0,0,26226}};
 auto logic=std::make_shared<ms::LogicSpec>();logic->ordered=true;
 char hashes[80];std::snprintf(hashes,sizeof hashes,"hash=%08x\n",ms::vkcap::paimonBodyHash);
 std::string ini=R"ini(
[Constants]
global $ready = 0
global persist $mode = 0
[Present]
$ready = 1
[KeyMode]
key = k
type = cycle
$mode = 1
[ResourceMod]
[ResourceGame]
[ResourcePosition]
[ResourceBlend]
[CommandListReplacement]
ResourceGame = ref ib
ib = ResourceMod
drawindexed = 6, 0, 0
ib = ResourceGame
[CommandListAfter]
ib = ResourceMod
if $mode == 1
 drawindexed = 3, 15, 0
else
 drawindexed = 9, 6, 0
endif
[TextureOverrideBody]
)ini";
 ini+=hashes;
 ini+=R"ini(match_first_index=0
if $ready && index_count == 26226 && vertex_count == 124 && vb1 == 42
run = CommandListReplacement
post run = CommandListAfter
endif
[TextureOverrideVertex]
)ini";
 std::snprintf(hashes,sizeof hashes,"hash=%08x\nfilter_index=42\n",hash);ini+=hashes;
 ini+="[TextureOverridePosition]\nhash=11223344\nvb0=ResourcePosition\n"
      "[TextureOverrideBlend]\nhash=55667788\nvb1=ResourceBlend\nhandling=skip\ndraw=100000,0\nvb1=null\n";
 ini+="[TextureOverrideOtherGpu]\n";
 std::snprintf(hashes,sizeof hashes,"hash=%08x\n",ms::vkcap::paimonBodyHash);
 ini+=hashes;ini+="match_vulkan = vb0:12345678\nhandling = skip\n";
 logic->files.push_back({"","",ini});logic->resources["resourcemod"]="modib";
 logic->resources["resourceposition"]="rest";logic->resources["resourceblend"]="weights";
 logic->keys={{0,"KeyMode","cycle",false}};
 logic->passes={{"skin",{{"vb0",0x11223344},{"vb1",0x55667788}},{"vb0","vb1"},{}},
                {"Body",{{"ib",ms::vkcap::paimonBodyHash}},{"ib"},{{"first_index",0}}}};
 logic->parts["Body"]={{"ib","Body","ib"},{"position","skin","vb0"},{"blend","skin","vb1"}};
 logic->assignments={{{"$vision",0}}};
 logic->choices={{{"Body","original|kind=drawindexed;count=6;start=0;base=0;ib=modib;position=rest;blend=weights;|"
                          "kind=drawindexed;count=9;start=6;base=0;ib=modib;position=rest;blend=weights;|"}}};
 profile.logic=logic;profile.logicPasses={{&logic->passes.back(),{}}};
 profile.persistFile="fixture/logic-persist/Beta.json";
 profile.iniDraws[0][0].images={~0u,~0u,~0u};
 std::swap(profile.iniDraws[0][0],profile.iniDraws[0][1]); // Row order inherited from another outfit.
 indexLogicRows(profile.logicPasses.front(),*logic,profile.iniDraws.front());
 T().logicResources=true;T().skinLogic=true;
 const auto queue=fake<VkQueue>(999);
 tintState().queues[queue]={d,8};
 logicQueuePresent=+[](VkQueue,const VkPresentInfoKHR*){return VK_SUCCESS;};
 call.clear();onCmdDrawIndexed(c,26226,1,0,0,0);assert((call==std::vector<uintptr_t>{26226}));
 logicOnQueuePresent(queue,nullptr);
 call.clear();onCmdDrawIndexed(c,26226,1,0,0,0);
 assert((call==std::vector<uintptr_t>{204,204,2000,14,101,200,201,26226,204,204,2000,23,101,200,201}));
 // The key's next result was never converted. Retain the complete original draw.
 T().queueSkinKeys({{"Beta","KeyMode",0}});
 logicOnQueuePresent(queue,nullptr);
 call.clear();onCmdDrawIndexed(c,26226,1,0,0,0);assert((call==std::vector<uintptr_t>{26226}));
 auto saves=T().logicSaves("fixture/logic-persist/");
 assert(saves.size()==1&&saves.at(profile.persistFile)->values.at("$mode")==1);
 T().logicWritten(profile.persistFile,saves.at(profile.persistFile));
 T().skinLogic=false;logicOnQueuePresent(queue,nullptr);
 assert(dev.logic.empty());
 T().skinLogic=true;onCmdDrawIndexed(c,26226,1,0,0,0);logicOnQueuePresent(queue,nullptr);
 assert(dev.logic.at(logic.get()).driver.restoredCount()==1);
 call.clear();onCmdDrawIndexed(c,26226,1,0,0,0);assert((call==std::vector<uintptr_t>{26226}));
 std::cout<<"Vulkan draw replacement: PASS (scoped textures, skinned vertex and index rebind, queue ownership, restoration and master switch)\n";
}
static void ordinaryTextureTrial(){
 using namespace ms::vkcap;
 const auto device=fake<VkDevice>(501);
 const auto command=fake<VkCommandBuffer>(502);
 const auto ui=fake<VkPipeline>(503),other=fake<VkPipeline>(504);
 const auto layout=fake<VkPipelineLayout>(505);
 const auto original=fake<VkDescriptorSet>(506),clone=fake<VkDescriptorSet>(507);
 const auto gameView=fake<VkImageView>(508),modView=fake<VkImageView>(509);
 const auto sampler=fake<VkSampler>(510);
 const auto cmd=gpuHandle(command);
 auto& tracker=T();
 tracker.enabled=true;tracker.allSkins=false;tracker.skinnedModel=false;tracker.renderTextures=true;
 tracker.renderResourceContexts=true;tracker.tintOn=false;
 tracker.renderMods={"texture-trial"};
 auto& dev=tintState().devices[device];
 const VkDescriptorSetLayoutBinding mixed[]={{3,VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,1,VK_SHADER_STAGE_FRAGMENT_BIT,nullptr},
                                            {5,VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,1,VK_SHADER_STAGE_VERTEX_BIT,nullptr}};
 const VkDescriptorSetLayoutCreateInfo mixedLayout{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,nullptr,0,2,mixed};
 const auto shape=mixedShape(dev,mixedLayout);assert(shape&&mixedShape(dev,mixedLayout)==shape);
 dev.pipelineLayouts[layout].sets[1]=shape;
 dev.pipelines[ui]=dev.pipelines[other]=layout;
 dev.upload.queued=true;dev.upload.images[0].view=modView;
 dev.families[{}]=0;
 tintState().commands[command]={device,TintCmd{}};
 auto& set=dev.sets[original];set.shape=shape;set.mixed=dev.mixedLayouts.at(shape);set.written=(1u<<3)|(1u<<5);
 set.images[3]={sampler,gameView,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
 set.buffers[5]={fake<VkBuffer>(521),0,16};
 std::array<uint8_t,20> bytes{};
 bytes[0]=1;bytes[16]=2;bytes[17]=3;
 const auto hash=crc32c(0,bytes.data(),16),shader=crc32c(0,bytes.data()+16,4);
 tracker.created(511,bytes.size(),1);tracker.bound(511,512,0);tracker.mapped(512,0,bytes.size(),bytes.data());
 tracker.imageCreated(513,2,2,1,1,1,1,4);tracker.imageView(gpuHandle(gameView),513,0,0);
 tracker.copiedImage(0,511,513,{0,0,0,0,2,2,true});
 tracker.shaderCreated(514,bytes.data()+16,4);tracker.shaderCreated(515,bytes.data(),4);
 tracker.pipelineCreated(gpuHandle(ui),gpuHandle(layout),{{16,514}});
 tracker.pipelineCreated(gpuHandle(other),gpuHandle(layout),{{16,515}});
 tracker.descriptorLayout(516,{},{{3,1}});tracker.descriptorAllocated(gpuHandle(original),517,516);
 tracker.descriptorWrite(gpuHandle(original),3,0,{ResourceKind::Texture,gpuHandle(gameView)});
 TextureProgram program;
 program.ini="[Constants]\nglobal persist $enabled = 1\n[ResourceColor]\n";
 program.defaults["$enabled"]=1;program.vars["$enabled"]={0,1};
 program.persistFile="texture-trial";
 program.images["resourcecolor"].push_back({0,0,0});
 TextureRoute sampled;sampled.thisSet=1;sampled.thisBinding=3;sampled.vertices=6;
 sampled.terms={{{ResourceKind::Texture,1,3},hash},{{ResourceKind::Shader,0,16},shader}};
 ms::vkhook::appendTextureRule(program,sampled,"if $enabled\nthis = ResourceColor\nendif\n");
 TextureRoute indexed=sampled;indexed.vertices=-1;indexed.ib=paimonBodyHash;indexed.first=0;indexed.count=26226;
 ms::vkhook::appendTextureRule(program,indexed,"this = ResourceColor\n");
 texturePrograms={std::move(program)};explicitTexturePrograms={0};textureIBPrograms[paimonBodyHash]={0};
 tracker.setSkinChoices({});tracker.setRenderReplacement(true,{"texture-trial"},"original");
 assert(tracker.ownsRenderMod("texture-trial"));
 static std::vector<uintptr_t> calls;
 tintCmdBindDescriptorSets=+[](VkCommandBuffer,VkPipelineBindPoint point,VkPipelineLayout,uint32_t first,uint32_t n,const VkDescriptorSet* bound,uint32_t offsets,const uint32_t*){
  assert(point==VK_PIPELINE_BIND_POINT_GRAPHICS&&first==1&&n==1&&!offsets);calls.push_back(gpuHandle(*bound));};
 tintCreateDescriptorSetLayout=+[](VkDevice,const VkDescriptorSetLayoutCreateInfo* info,const VkAllocationCallbacks*,VkDescriptorSetLayout* out){
  assert(info->bindingCount==2&&info->pBindings[0].binding==3&&info->pBindings[1].binding==5);*out=fake<VkDescriptorSetLayout>(518);return VK_SUCCESS;};
 tintAllocateDescriptorSets=+[](VkDevice,const VkDescriptorSetAllocateInfo*,VkDescriptorSet* out){*out=fake<VkDescriptorSet>(507);return VK_SUCCESS;};
 tintUpdateDescriptorSets=+[](VkDevice,uint32_t n,const VkWriteDescriptorSet* writes,uint32_t copies,const VkCopyDescriptorSet*){
  assert(n==2&&!copies&&writes[0].dstBinding==3&&writes[0].pImageInfo->imageView==fake<VkImageView>(509)&&writes[0].pImageInfo->sampler==fake<VkSampler>(510));
  assert(writes[1].dstBinding==5&&writes[1].descriptorType==VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER&&writes[1].pBufferInfo->buffer==fake<VkBuffer>(521));};
 gpuCmdDraw=+[](VkCommandBuffer,uint32_t n,uint32_t,uint32_t,uint32_t){calls.push_back(n);};
 cmdDrawIndexed=+[](void*,uint32_t n,uint32_t,uint32_t,int32_t,uint32_t){calls.push_back(n);};
 tintOnCmdBindDescriptorSets(command,VK_PIPELINE_BIND_POINT_GRAPHICS,layout,1,1,&original,0,nullptr);
 tintOnCmdBindPipeline(command,VK_PIPELINE_BIND_POINT_GRAPHICS,ui);
 calls.clear();resourceOnCmdDraw(command,6,1,0,0);
 assert((calls==std::vector<uintptr_t>{gpuHandle(clone),6,gpuHandle(original)}));
 calls.clear();resourceOnCmdDraw(command,12,1,0,0);
 tintOnCmdBindPipeline(command,VK_PIPELINE_BIND_POINT_GRAPHICS,other);
 resourceOnCmdDraw(command,6,1,0,0);assert((calls==std::vector<uintptr_t>{12,6}));
 tracker.setSkinChoices({{"texture-trial",{{"$enabled",0}}}});
 tintOnCmdBindPipeline(command,VK_PIPELINE_BIND_POINT_GRAPHICS,ui);
 calls.clear();resourceOnCmdDraw(command,6,1,0,0);assert((calls==std::vector<uintptr_t>{6}));
 assert(tracker.savedLogic("texture-trial","",true)->at("$enabled")==0);
 std::vector<uint8_t> index(61224);const uint32_t tail=0x86c821d5;std::memcpy(index.data()+index.size()-4,&tail,4);
 tracker.setDrawHashes({paimonBodyHash});
 tracker.created(519,index.size(),0x40);tracker.bound(519,520,0);tracker.mapped(520,0,index.size(),index.data());
 tracker.indexBound(519,0,cmd,0);
 calls.clear();onCmdDrawIndexed(command,26226,1,0,0,0);onCmdDrawIndexed(command,4386,1,26226,0,0);
 assert((calls==std::vector<uintptr_t>{gpuHandle(clone),26226,gpuHandle(original),4386}));
 tracker.setRenderReplacement(false,{"texture-trial"},"original");
 calls.clear();onCmdDrawIndexed(command,26226,1,0,0,0);assert((calls==std::vector<uintptr_t>{26226}&&set.images[3].imageView==gameView));
 std::cout<<"Vulkan ordinary textures: PASS (both draw calls, atlas context, INI choices and persistence, original descriptor restoration, no model)\n";
}
static void registryReloadTrial(){
 auto temp=(std::filesystem::temp_directory_path()/"gibox-reload-XXXXXX").string();
 const auto* root=mkdtemp(temp.data());
 assert(root);
 const std::string shared(root);
 auto& tracker=T();tracker.hotReload=true;tracker.enabled=true;tracker.probeOnly=true;
 std::vector<uint8_t> data(61224);const uint32_t tail=0x86c821d5;std::memcpy(data.data()+data.size()-4,&tail,4);
 tracker.created(7001,data.size(),0x40);tracker.bound(7001,7002,0);tracker.mapped(7002,0,data.size(),data.data());
 tracker.indexBound(7001,0,7003,0);tracker.unmapped(7002);
 std::vector<uint32_t> probes;
 for(uint32_t i=0;i<90;i++){probes.push_back(1000+i);}
 ms::vkhook::configureVulkanProbes(probes);
 std::vector<std::string> selected;
 auto write=[&](const std::string& name,const std::string& generation){
  const auto dir=shared+"/models/"+name;
  std::filesystem::create_directories(dir+"/textures");
  ms::write(dir+"/textures/0.rgba",std::string(4,'c'));
  const std::string doc=R"json({"format":"gibox-texture-mod","version":1,"name":")json"+name+
    R"json(","generation":")json"+generation+R"json(","character":{"id":1},"variants":{},"textures":[
    {"resource":"color","file":"textures/0.rgba","width":1,"height":1}],
    "diag":{"format":"gibox-texture-diag","version":1,"rules":[{"section":"Color","kind":"segment",
    "hash":"5b4cdc9c","first":0,"textures":[{"slot":"ps-t0","resource":"color","role":"diffuse"}]}]}})json";
  ms::write(dir+"/texture.json",doc);
 };
 for(unsigned i=0;i<70;i++){
  selected.push_back("Texture"+std::to_string(i));write(selected.back(),"first");
 }
 ms::vkhook::stageVulkanRegistry(shared,selected,{});
 assert(!tracker.ownsRenderMod("Texture69"));
 const auto restore=[](const auto&){return true;};
 assert(!ms::vkhook::publishVulkanRegistry(true,false,selected,{},"cover",restore).empty());
 assert(tracker.ownsRenderMod("Texture69")&&std::atomic_load(&tracker.activeRenderMods)->count(69));
 assert(tracker.drawStats().size()==91&&tracker.takeIndexUploads().second.count(69));
 std::vector<ms::vkcap::IndexIdentity> identities;
 tracker.drawIndexed(7003,26226,1,0,0,0,nullptr,&identities);
 assert(std::any_of(identities.begin(),identities.end(),[](const auto& id){return id.hash==ms::vkcap::paimonBodyHash;}));
 ms::vkcap::Tracker meshTracker;meshTracker.hotReload=true;meshTracker.allSkins=true;
 meshTracker.created(7101,data.size(),0x40);meshTracker.bound(7101,7102,0);meshTracker.mapped(7102,0,data.size(),data.data());
 meshTracker.indexBound(7101,0,7103,0);meshTracker.unmapped(7102);
 ms::vkcap::Registration modelRules;
 for(unsigned i=0;i<72;i++){modelRules.skinMods.push_back("Part"+std::to_string(i));}
 modelRules.iniSkinRules[ms::vkcap::paimonBodyHash]={{71,0,0,26226}};
 meshTracker.publishRegistration(std::move(modelRules),{ms::vkcap::paimonBodyHash});
 meshTracker.setSkinReplacement(true,true,{"Part71"});
 ms::vkcap::SkinHit hit;
 meshTracker.drawIndexed(7103,26226,1,0,0,0,&hit);
 assert(hit&&hit.profile==71&&meshTracker.profileDraws.size()==72);
 const auto version=tracker.registryVersion;
 const auto unchanged=sharedImages.front().file;
 write(selected.back(),"second-version");
 ms::vkhook::stageVulkanRegistry(shared,selected,{});
 assert(ms::vkhook::publishVulkanRegistry(true,false,selected,{},"original",[](const auto&){return false;}).empty());
 assert(tracker.registryVersion==version&&sharedImages.back().file->generation=="first");
 assert(!ms::vkhook::publishVulkanRegistry(true,false,selected,{},"original",restore).empty());
 assert(tracker.registryVersion==version+1&&sharedImages.front().file==unchanged&&sharedImages.back().file->generation=="second-version");
 std::filesystem::remove_all(shared+"/models/"+selected.back());
 ms::vkhook::stageVulkanRegistry(shared,selected,{});
 assert(!ms::vkhook::publishVulkanRegistry(true,false,selected,{},"original",restore).empty());
 assert(!tracker.ownsRenderMod("Texture69")&&tracker.ownsRenderMod("Texture68"));
 ms::vkhook::stageVulkanRegistry(shared,{},{});
 ms::vkhook::publishVulkanRegistry(false,false,{},{}, "original",restore);
 assert(std::atomic_load(&tracker.activeRenderMods)->empty()&&sharedImages.empty());
 std::filesystem::remove_all(shared);
 std::cout<<"Vulkan live registry: PASS (preexisting uploads, more than 64 rules and mods, refusable handoff, reconvert and delete)\n";
}
static void gpuLifetimeTrial(){
 const auto device=fake<VkDevice>(8001);
 const auto parent=fake<VkCommandBuffer>(8002),child=fake<VkCommandBuffer>(8003);
 auto& state=tintState();
 auto& dev=state.devices[device];auto& upload=dev.upload;
 const auto held=fake<VkImageView>(8004),idle=fake<VkImageView>(8005);
 const auto descriptor=fake<VkDescriptorSet>(8006);
 const ImageKey heldKey{"file","v1","texture.json","1",0,4,1,1,false,false,0,0};
 const ImageKey idleKey{"file","v2","texture.json","1",0,4,1,1,false,false,0,0};
 upload.images={{fake<VkImage>(8007),{},held,64},{fake<VkImage>(8008),{},idle,64}};
 upload.imageKeys={heldKey,idleKey};upload.imageBytes=128;
 upload.resident.emplace(heldKey,ResidentImage{upload.images[0],20});
 upload.resident.emplace(idleKey,ResidentImage{upload.images[1],10});
 dev.pool=fake<VkDescriptorPool>(8009);dev.allocated=1;dev.clones[descriptor]={{held},1,20};
 state.commands[parent]={device,TintCmd{}};state.commands[child]={device,TintCmd{}};
 state.commands[child].second.gpuSets.insert(descriptor);state.commands[child].second.skinCatalogs.insert(8);
 trackExecutedResources(parent,1,&child);
 tintBegin(child);
 const auto& pinsOfParent=state.commands[parent].second;
 assert(pinsOfParent.gpuSets.count(descriptor)&&pinsOfParent.skinCatalogs.count(8));
 static std::vector<VkImageView> destroyedViews;
 static unsigned freed=0,destroyedBuffers=0;
 tintDestroyImageView=+[](VkDevice,VkImageView view,const VkAllocationCallbacks*){destroyedViews.push_back(view);};
 tintDestroyImage=+[](VkDevice,VkImage,const VkAllocationCallbacks*){};
 tintFreeDescriptorSets=+[](VkDevice,VkDescriptorPool,uint32_t n,const VkDescriptorSet*){freed+=n;return VK_SUCCESS;};
 upDestroyBuffer=+[](VkDevice,VkBuffer,const VkAllocationCallbacks*){++destroyedBuffers;};
 dev.retiredSkins[8].retired.push_back({fake<VkBuffer>(8011),{},32,32});
 const CommandPins referenced{pinsOfParent.gpuSets,pinsOfParent.gpuBuffers,pinsOfParent.skinCatalogs};
 upload.fence=fake<VkFence>(8012);
 evictImages(device,dev,referenced,64,128);assert(destroyedViews.empty());
 upload.fence={};
 evictImages(device,dev,referenced,64,128);
 collectGpuResources(device,dev,referenced);
 assert((destroyedViews==std::vector<VkImageView>{idle})&&!destroyedBuffers&&upload.imageBytes==64&&!upload.images[1].view);
 tintBegin(parent);
 collectGpuResources(device,dev,{});
 evictImages(device,dev,{},64,64);
 assert((destroyedViews==std::vector<VkImageView>{idle,held})&&freed==1&&destroyedBuffers==1&&upload.imageBytes==0);
 state.commands.erase(parent);state.commands.erase(child);state.devices.erase(device);
 std::cout<<"Vulkan lifetimes: PASS (upload fence, secondary command pins, LRU, clone and geometry retirement)\n";
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
 livePixelBindingTrial();
 pcRouteTrial();
 vertexLayoutTrial();
 tintTrial();
 ordinaryTextureTrial();
 registryReloadTrial();
 gpuLifetimeTrial();
#endif
}
