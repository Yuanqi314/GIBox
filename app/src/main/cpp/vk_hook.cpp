// The Vulkan side of the index-buffer hash diagnostic (vk_capture.hpp), off by default. Two exported entry points of
// the system loader libvulkan.so are hooked, vkGetInstanceProcAddr and vkGetDeviceProcAddr: when the game asks for one
// of the buffer functions below, it gets a wrapper that tells the tracker what happened and then calls the loader's
// exported function of that name with the same arguments. The default path is observational. The opt-in tint test
// temporarily binds a private descriptor set around a confirmed draw, without changing the game's descriptor sets.
// Installed at the module's start, before the game creates its Vulkan device; a function the game resolved earlier is
// simply not seen.
//
// No inline hook (Dobby) in libvulkan.so: on the 0.7.56 device (Android 16, BTI hardware) the system libraries are
// mapped with branch target checks, so an indirect branch may only land on a "bti" / "paciasp" instruction. Dobby
// relocated the exported stub's direct "b" into the loader's internal GetInstanceProcAddr as "br x17", which lands on a
// function only ever reached by direct branches: SIGILL ILL_ILLOPC at its first instruction, x17 = pc. Here exactly one
// instruction of each stub is rewritten: its direct "b" becomes a direct "b" into a page of ours within 128 MiB, which
// goes on to the wrapper; the original target is reached from that page by a direct "b" too. A stub of another shape is
// left alone and its first words are logged.
#include "vk_capture.hpp"
#include "dobby.h"
#include <atomic>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <initializer_list>
#include <dlfcn.h>
#include <string>
#include <sys/mman.h>
#include <type_traits>
#include <unistd.h>
#if (defined(__ANDROID__) && !defined(GIBOX_HOST_TEST)) || defined(GIBOX_VK_TINT_TEST)
#define GIBOX_VK_TINT 1
#include <vulkan/vulkan_core.h>
#include "model.hpp"
#include "pc_hashes.hpp"
#endif

namespace {
// The few Vulkan declarations used (vulkan_core.h; LP64 layout, VKAPI_CALL is empty on arm64).
using VkResult=int32_t;using PFN=void(*)();
struct VkBufferCreateInfo {int32_t sType;const void* pNext;uint32_t flags;uint64_t size;uint32_t usage;int32_t sharingMode;uint32_t queueFamilyIndexCount;const uint32_t* pQueueFamilyIndices;};
static_assert(offsetof(VkBufferCreateInfo,size)==24&&offsetof(VkBufferCreateInfo,usage)==32,"VkBufferCreateInfo layout");
struct VkBindBufferMemoryInfo {int32_t sType;const void* pNext;uint64_t buffer,memory,memoryOffset;};
static_assert(offsetof(VkBindBufferMemoryInfo,buffer)==16&&offsetof(VkBindBufferMemoryInfo,memoryOffset)==32,"VkBindBufferMemoryInfo layout");
static_assert(sizeof(ms::vkcap::Copy)==24,"VkBufferCopy layout");

using GetProc=PFN(*)(void*,const char*);
using CreateBuffer=VkResult(*)(void*,const VkBufferCreateInfo*,const void*,uint64_t*);
using DestroyBuffer=void(*)(void*,uint64_t,const void*);
using BindBufferMemory=VkResult(*)(void*,uint64_t,uint64_t,uint64_t);
using BindBufferMemory2=VkResult(*)(void*,uint32_t,const VkBindBufferMemoryInfo*);
using MapMemory=VkResult(*)(void*,uint64_t,uint64_t,uint64_t,uint32_t,void**);
using UnmapMemory=void(*)(void*,uint64_t);
using FreeMemory=void(*)(void*,uint64_t,const void*);
using CmdCopyBuffer=void(*)(void*,uint64_t,uint64_t,uint32_t,const ms::vkcap::Copy*);
using CmdBindIndexBuffer=void(*)(void*,uint64_t,uint64_t,int32_t);
using BeginCommandBuffer=VkResult(*)(void*,const void*);
using CmdDrawIndexed=void(*)(void*,uint32_t,uint32_t,uint32_t,int32_t,uint32_t);

GetProc instanceProcOriginal=nullptr,deviceProcOriginal=nullptr;
PFN onGetDeviceProcAddr(void* device,const char* name);
std::atomic<CreateBuffer> createBuffer{nullptr};std::atomic<DestroyBuffer> destroyBuffer{nullptr};
std::atomic<BindBufferMemory> bindBufferMemory{nullptr};std::atomic<BindBufferMemory2> bindBufferMemory2{nullptr},bindBufferMemory2KHR{nullptr};
std::atomic<MapMemory> mapMemory{nullptr};std::atomic<UnmapMemory> unmapMemory{nullptr};std::atomic<FreeMemory> freeMemory{nullptr};
std::atomic<CmdCopyBuffer> cmdCopyBuffer{nullptr};std::atomic<CmdBindIndexBuffer> cmdBindIndexBuffer{nullptr};
std::atomic<BeginCommandBuffer> beginCommandBuffer{nullptr};
std::atomic<CmdDrawIndexed> cmdDrawIndexed{nullptr};
ms::vkcap::Tracker& T(){return ms::vkcap::tracker();}

#ifdef GIBOX_VK_TINT
// The test replaces the sampled RGB channels, keeping alpha, with another view of the same image. No upload or
// queue operations are required. Only ordinary, single-image bindings of a fully observed set 0 are accepted.
#define TINT_FUNCTIONS(X) \
 X(CreateImageView) X(DestroyImageView) X(DestroyImage) X(CreateDescriptorSetLayout) X(DestroyDescriptorSetLayout) \
 X(CreatePipelineLayout) X(DestroyPipelineLayout) X(CreateShaderModule) X(DestroyShaderModule) X(CreateGraphicsPipelines) X(DestroyPipeline) \
 X(AllocateDescriptorSets) X(UpdateDescriptorSets) X(FreeDescriptorSets) X(ResetDescriptorPool) X(DestroyDescriptorPool) \
 X(AllocateCommandBuffers) X(FreeCommandBuffers) X(ResetCommandPool) X(DestroyCommandPool) X(DestroyDevice) \
 X(CmdBindPipeline) X(CmdBindDescriptorSets)
#define UPLOAD_HOOKS(X) X(CreateDevice) X(GetDeviceQueue) X(GetDeviceQueue2) X(CreateCommandPool) X(QueueSubmit)
#define UPLOAD_API(X) X(GetPhysicalDeviceMemoryProperties) X(GetPhysicalDeviceQueueFamilyProperties) X(GetPhysicalDeviceFormatProperties) \
 X(CreateImage) X(GetImageMemoryRequirements) X(BindImageMemory) X(AllocateMemory) X(FreeMemory) X(CreateBuffer) X(DestroyBuffer) \
 X(GetBufferMemoryRequirements) X(BindBufferMemory) X(MapMemory) X(UnmapMemory) X(BeginCommandBuffer) X(EndCommandBuffer) \
 X(CmdCopyBufferToImage) X(CmdPipelineBarrier) X(CreateFence) X(GetFenceStatus) X(DestroyFence)
#define SKIN_HOOKS(X) X(CreateComputePipelines) X(CmdDispatch) X(CmdBindVertexBuffers)
#define TINT_DECLARE(F) std::atomic<PFN_vk##F> tint##F{nullptr};
TINT_FUNCTIONS(TINT_DECLARE)
TINT_DECLARE(CreateDescriptorPool)
TINT_DECLARE(UpdateDescriptorSetWithTemplate)
std::atomic<PFN_vkUpdateDescriptorSetWithTemplate> tintUpdateDescriptorSetWithTemplateKHR{nullptr};
#undef TINT_DECLARE
#define UPLOAD_DECLARE(F) std::atomic<PFN_vk##F> up##F{nullptr};
UPLOAD_HOOKS(UPLOAD_DECLARE)
UPLOAD_API(UPLOAD_DECLARE)
SKIN_HOOKS(UPLOAD_DECLARE)
#undef UPLOAD_DECLARE

constexpr size_t tintBindings=12,tintSetLimit=256;
struct TintImg {VkImageViewCreateInfo info{};uint64_t generation=0;std::array<VkImageView,2> colored{};};
struct TintSet {
 VkDescriptorPool pool{};uint32_t shape=0,written=0;
 std::array<VkDescriptorImageInfo,tintBindings> images{};
 struct Cache {VkDescriptorSet set{};uint64_t generation=0;};std::array<Cache,2> cache{};
 std::map<VkImageView,Cache> textureCache{};
};
struct TintCmd {
 VkCommandPool pool{};VkPipeline pipeline{};VkPipelineLayout layout{};VkDescriptorSet set{};
 VkPipeline compute{};VkPipelineLayout computeLayout{};std::array<VkDescriptorSet,2> computeSets{};
 std::array<VkBuffer,3> vertex{};std::array<VkDeviceSize,3> vertexOffset{};
 VkBuffer index{};VkDeviceSize indexOffset=0;VkIndexType indexType=VK_INDEX_TYPE_MAX_ENUM;
 std::set<VkBuffer> skinProduced{};
 std::set<std::pair<VkBuffer,unsigned>> sharedProduced{};
};
struct TextureGpu {VkImage image{};VkDeviceMemory memory{};VkImageView view{};};
struct TextureUpload {
 VkPhysicalDeviceMemoryProperties memory{};std::set<uint32_t> families;
 bool attempted=false,queued=false;uint32_t usedFamily=0;
 std::vector<TextureGpu> images{TextureGpu{},TextureGpu{}};
 VkCommandPool pool{};VkBuffer staging{};VkDeviceMemory stagingMemory{};VkFence fence{};
};
struct SkinBuffer {VkBuffer buffer{};VkDeviceMemory memory{};VkDeviceSize size=0;};
enum SkinStream {Rest=0,Weights,Attributes,Indices,Count,SkinStreamCount};
struct SkinSet {VkDescriptorPool pool{};uint32_t shape=0,bits=0;std::array<VkDescriptorBufferInfo,4> buffers{};};
struct SkinOutput {SkinBuffer buffer;bool queued=false;};
struct SharedSkin {ms::VulkanSkinPayload mesh;unsigned originalVertices=0,bones=0;std::vector<unsigned> images;};
struct SharedGpuSkin {std::array<SkinBuffer,SkinStreamCount> input{};VkDescriptorSet count{};bool ready=false;};
struct SkinSource {VkBuffer rest{},weights{};unsigned vertices=0,bones=0;};
constexpr uint32_t skinStorage=0x40000000,skinUniform=0x40000001;
constexpr size_t maxSkinOutputs=40,maxSkinSets=128;
struct SkinDevice {
 std::array<SkinBuffer,SkinStreamCount> input{};bool ready=false;
 std::set<VkShaderModule> kernels;
 std::map<VkShaderModule,uint32_t> vertexCodes;
 std::map<VkShaderModule,std::array<uint8_t,32>> vertexSemantics;
 std::map<VkPipeline,unsigned> graphics;
 std::set<VkPipelineLayout> layouts;
 std::map<VkPipeline,VkPipelineLayout> compute;
 std::map<VkDescriptorSet,SkinSet> sets;
 std::map<VkBuffer,SkinOutput> output;
 std::vector<SkinBuffer> retired;
 std::map<std::pair<VkBuffer,VkBuffer>,VkDescriptorSet> remaps;
 std::vector<SharedGpuSkin> profiles;
 std::map<VkBuffer,SkinSource> seenOutputs;
 std::map<std::pair<VkBuffer,VkBuffer>,unsigned> sourceProfiles;
 std::map<std::pair<VkBuffer,unsigned>,SkinOutput> sharedOutput;
 std::map<std::tuple<VkBuffer,VkBuffer,unsigned>,VkDescriptorSet> sharedRemaps;
 VkDescriptorPool pool{};VkDescriptorSetLayout storage{},uniform{};VkDescriptorSet count{};size_t allocated=0;
};
struct TintDevice {
 struct Layout {uint32_t shape=0;};
 std::map<VkDescriptorSetLayout,uint32_t> layouts;
 std::map<VkPipelineLayout,Layout> pipelineLayouts;
 std::map<VkShaderModule,bool> mainTextures;
 std::map<VkPipeline,VkPipelineLayout> pipelines;
 std::map<VkDescriptorSet,TintSet> sets;
 std::map<VkImageView,TintImg> images;
 std::vector<std::pair<VkImage,VkImageView>> ownedViews;
 std::map<uint32_t,VkDescriptorSetLayout> ownedLayouts;
 VkDescriptorPool pool{};size_t allocated=0;uint64_t generation=0;
 TextureUpload upload;std::map<VkCommandPool,uint32_t> families;
 SkinDevice skin;
};
struct TintState {
 std::mutex mutex;std::map<VkDevice,TintDevice> devices;
 std::map<VkCommandBuffer,std::pair<VkDevice,TintCmd>> commands;
 std::map<VkQueue,std::pair<VkDevice,uint32_t>> queues;
};
TintState& tintState(){static TintState state;return state;}
std::array<std::shared_ptr<ms::Texture>,2> paimonPixels; // immutable after startup
ms::VulkanSkinPayload paimonSkin; // immutable after startup
std::vector<SharedSkin> sharedSkins;
std::vector<std::shared_ptr<ms::Texture>> sharedPixels;

// Shader access flags (in addition to descriptor types/count) are part of set compatibility. Here every
// layout has contiguous combined samplers; accept only the layouts seen for this Body in the capture.
uint32_t tintShape(const VkDescriptorSetLayoutCreateInfo& info){
 const auto n=info.bindingCount;if(info.flags||info.pNext||!info.pBindings||!n||n>tintBindings)return 0;
 uint32_t seen=0,vs=0,fs=0;
 for(uint32_t i=0;i<n;i++){const auto& b=info.pBindings[i];
  if(b.binding>=n||(seen&(1u<<b.binding))||b.descriptorCount!=1||b.pImmutableSamplers||b.descriptorType!=VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER||
    !b.stageFlags||(b.stageFlags&~(VK_SHADER_STAGE_VERTEX_BIT|VK_SHADER_STAGE_FRAGMENT_BIT)))return 0;
  seen|=1u<<b.binding;
  if(b.stageFlags&VK_SHADER_STAGE_VERTEX_BIT)vs|=1u<<b.binding;
  if(b.stageFlags&VK_SHADER_STAGE_FRAGMENT_BIT)fs|=1u<<b.binding;
 }
 const auto legacy=n>=4?1u<<(n-1):0u;
 return vs==legacy&&fs==(seen^legacy)?n:n|(vs<<4)|(fs<<16);
}
uint32_t tintSlotCount(uint32_t shape){return shape&15u;}
VkShaderStageFlags tintSlotStages(uint32_t shape,unsigned slot){
 if(shape<16)return shape>=4&&slot+1==shape?VK_SHADER_STAGE_VERTEX_BIT:VK_SHADER_STAGE_FRAGMENT_BIT;
 return (((shape>>4)&(1u<<slot))?unsigned(VK_SHADER_STAGE_VERTEX_BIT):0u)|(((shape>>16)&(1u<<slot))?unsigned(VK_SHADER_STAGE_FRAGMENT_BIT):0u);
}
uint32_t skinShape(const VkDescriptorSetLayoutCreateInfo& p){
 if(p.pNext||p.flags||!p.pBindings||(p.bindingCount!=1&&p.bindingCount!=4))return 0;
 const bool storage=p.bindingCount==4;uint32_t bits=0;
 for(uint32_t i=0;i<p.bindingCount;i++){const auto& b=p.pBindings[i];if(b.binding>=p.bindingCount||(bits&(1u<<b.binding))||b.descriptorCount!=1||b.pImmutableSamplers||b.stageFlags!=VK_SHADER_STAGE_COMPUTE_BIT||b.descriptorType!=(storage?VK_DESCRIPTOR_TYPE_STORAGE_BUFFER:VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER))return 0;bits|=1u<<b.binding;}
 return storage?skinStorage:skinUniform;
}
template<class F>void tintObserve(F&& f) noexcept{try{auto& s=tintState();std::lock_guard<std::mutex> lock(s.mutex);f(s);}catch(...){} }

void releaseUploadStaging(VkDevice d,TextureUpload& u){
 if(u.fence)upDestroyFence.load()(d,u.fence,nullptr);
 if(u.pool)tintDestroyCommandPool.load()(d,u.pool,nullptr);
 if(u.staging)upDestroyBuffer.load()(d,u.staging,nullptr);
 if(u.stagingMemory)upFreeMemory.load()(d,u.stagingMemory,nullptr);
 u.fence={};u.pool={};u.staging={};u.stagingMemory={};
}
void releaseUpload(VkDevice d,TextureUpload& u){
 releaseUploadStaging(d,u);
 for(auto& x:u.images){if(x.view)tintDestroyImageView.load()(d,x.view,nullptr);
  if(x.image)tintDestroyImage.load()(d,x.image,nullptr);
  if(x.memory)upFreeMemory.load()(d,x.memory,nullptr);
  x={};}
 u.queued=false;
}
bool uploadMemory(VkDevice d,const TextureUpload& u,const VkMemoryRequirements& req,VkMemoryPropertyFlags flags,VkDeviceMemory& memory){
 for(uint32_t i=0;i<u.memory.memoryTypeCount;i++)if((req.memoryTypeBits&(1u<<i))&&(u.memory.memoryTypes[i].propertyFlags&flags)==flags){
  const VkMemoryAllocateInfo a{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,nullptr,req.size,i};
  if(upAllocateMemory.load()(d,&a,nullptr,&memory)==VK_SUCCESS)return true;
  memory={};}
 return false;
}
void releaseSkinBuffer(VkDevice d,SkinBuffer& b){if(b.buffer)upDestroyBuffer.load()(d,b.buffer,nullptr);if(b.memory)upFreeMemory.load()(d,b.memory,nullptr);b={};}
bool makeSkinBuffer(VkDevice d,const TextureUpload& u,SkinBuffer& b,VkDeviceSize size,VkBufferUsageFlags flags,const void* data=nullptr){
 const ::VkBufferCreateInfo p{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,nullptr,0,size,flags,VK_SHARING_MODE_EXCLUSIVE,0,nullptr};
 if(upCreateBuffer.load()(d,&p,nullptr,&b.buffer)!=VK_SUCCESS){b.buffer={};return false;}
 VkMemoryRequirements req{};upGetBufferMemoryRequirements.load()(d,b.buffer,&req);
 if(!uploadMemory(d,u,req,data?VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT:VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,b.memory)||
  upBindBufferMemory.load()(d,b.buffer,b.memory,0)!=VK_SUCCESS){releaseSkinBuffer(d,b);return false;}
 if(data){void* dest=nullptr;if(upMapMemory.load()(d,b.memory,0,size,0,&dest)!=VK_SUCCESS){releaseSkinBuffer(d,b);return false;}
  std::memcpy(dest,data,size_t(size));upUnmapMemory.load()(d,b.memory);}
 b.size=size;return true;
}
bool prepareSkinInput(VkDevice d,const TextureUpload& upload,const ms::VulkanSkinPayload& p,std::array<SkinBuffer,SkinStreamCount>& dst){
 auto make=[&](SkinStream binding,const ms::Bytes& bytes,VkBufferUsageFlags flags){return !bytes.empty()&&makeSkinBuffer(d,upload,dst[binding],bytes.size(),flags,bytes.data());};
 bool ready=p.vertices&&make(Rest,p.rest,VK_BUFFER_USAGE_STORAGE_BUFFER_BIT)&&make(Weights,p.weights,VK_BUFFER_USAGE_STORAGE_BUFFER_BIT)&&
  make(Attributes,p.attributes,VK_BUFFER_USAGE_VERTEX_BUFFER_BIT)&&make(Indices,p.indices,VK_BUFFER_USAGE_INDEX_BUFFER_BIT)&&
  makeSkinBuffer(d,upload,dst[Count],sizeof p.vertices,VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,&p.vertices);
 if(!ready)for(auto& b:dst)releaseSkinBuffer(d,b);
 return ready;
}
bool prepareSkinBuffers(VkDevice d,TintDevice& dev){
 auto& skin=dev.skin;if(!T().allSkins)return skin.ready=prepareSkinInput(d,dev.upload,paimonSkin,skin.input);
 skin.profiles.resize(sharedSkins.size());
 for(size_t i=0;i<sharedSkins.size();i++)skin.profiles[i].ready=prepareSkinInput(d,dev.upload,sharedSkins[i].mesh,skin.profiles[i].input);
 return std::any_of(skin.profiles.begin(),skin.profiles.end(),[](const auto& p){return p.ready;});
}
void releaseSkin(VkDevice d,SkinDevice& s){
 if(s.pool)tintDestroyDescriptorPool.load()(d,s.pool,nullptr);
 if(s.storage)tintDestroyDescriptorSetLayout.load()(d,s.storage,nullptr);
 if(s.uniform)tintDestroyDescriptorSetLayout.load()(d,s.uniform,nullptr);
 for(auto& b:s.input)releaseSkinBuffer(d,b);
 for(auto& kv:s.output)releaseSkinBuffer(d,kv.second.buffer);
 for(auto& p:s.profiles)for(auto& b:p.input)releaseSkinBuffer(d,b);
 for(auto& kv:s.sharedOutput)releaseSkinBuffer(d,kv.second.buffer);
 for(auto& b:s.retired)releaseSkinBuffer(d,b);
 s.pool={};s.storage={};s.uniform={};s.ready=false;
}
bool stagePaimon(VkDevice d,VkQueue queue,uint32_t family,TextureUpload& u){
 const std::vector<std::shared_ptr<ms::Texture>> lookup=T().allSkins?sharedPixels:std::vector<std::shared_ptr<ms::Texture>>(paimonPixels.begin(),paimonPixels.end());
 if(T().allSkins&&lookup.empty()){u.images.clear();u.usedFamily=family;u.queued=true;return true;}
 if(lookup.empty()||std::any_of(lookup.begin(),lookup.end(),[](const auto& p){return !p;}))return false;
 u.images.resize(lookup.size());VkDeviceSize size=0;std::vector<VkBufferImageCopy> regions(lookup.size());
 for(size_t i=0;i<u.images.size();i++){
  const auto& p=*lookup[i];auto& x=u.images[i];
  const VkImageCreateInfo ci{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,nullptr,0,VK_IMAGE_TYPE_2D,VK_FORMAT_R8G8B8A8_SRGB,{p.w,p.h,1},1,1,
   VK_SAMPLE_COUNT_1_BIT,VK_IMAGE_TILING_OPTIMAL,VK_IMAGE_USAGE_TRANSFER_DST_BIT|VK_IMAGE_USAGE_SAMPLED_BIT,VK_SHARING_MODE_EXCLUSIVE,0,nullptr,VK_IMAGE_LAYOUT_UNDEFINED};
  if(upCreateImage.load()(d,&ci,nullptr,&x.image)!=VK_SUCCESS){x.image={};return false;}
  VkMemoryRequirements req{};upGetImageMemoryRequirements.load()(d,x.image,&req);
  if(!uploadMemory(d,u,req,VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,x.memory)||upBindImageMemory.load()(d,x.image,x.memory,0)!=VK_SUCCESS)return false;
  const VkImageViewCreateInfo v{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,nullptr,0,x.image,VK_IMAGE_VIEW_TYPE_2D,ci.format,{}, {VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1}};
  if(tintCreateImageView.load()(d,&v,nullptr,&x.view)!=VK_SUCCESS){x.view={};return false;}
  regions[i].bufferOffset=size;regions[i].imageSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1};regions[i].imageExtent=ci.extent;
  size+=p.pixels.size();
 }
 const ::VkBufferCreateInfo b{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,nullptr,0,size,VK_BUFFER_USAGE_TRANSFER_SRC_BIT,VK_SHARING_MODE_EXCLUSIVE,0,nullptr};
 if(upCreateBuffer.load()(d,&b,nullptr,&u.staging)!=VK_SUCCESS){u.staging={};return false;}
 VkMemoryRequirements req{};upGetBufferMemoryRequirements.load()(d,u.staging,&req);
 if(!uploadMemory(d,u,req,VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,u.stagingMemory)||upBindBufferMemory.load()(d,u.staging,u.stagingMemory,0)!=VK_SUCCESS)return false;
 void* mapped=nullptr;if(upMapMemory.load()(d,u.stagingMemory,0,size,0,&mapped)!=VK_SUCCESS)return false;
 for(size_t i=0;i<regions.size();i++)std::memcpy(static_cast<char*>(mapped)+regions[i].bufferOffset,lookup[i]->pixels.data(),lookup[i]->pixels.size());
 upUnmapMemory.load()(d,u.stagingMemory);
 const VkCommandPoolCreateInfo pc{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,nullptr,VK_COMMAND_POOL_CREATE_TRANSIENT_BIT,family};
 if(upCreateCommandPool.load()(d,&pc,nullptr,&u.pool)!=VK_SUCCESS){u.pool={};return false;}
 const VkCommandBufferAllocateInfo ac{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,nullptr,u.pool,VK_COMMAND_BUFFER_LEVEL_PRIMARY,1};VkCommandBuffer cmd{};
 if(tintAllocateCommandBuffers.load()(d,&ac,&cmd)!=VK_SUCCESS)return false;
 const VkCommandBufferBeginInfo bc{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,nullptr,VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,nullptr};
 if(upBeginCommandBuffer.load()(cmd,&bc)!=VK_SUCCESS)return false;
 std::vector<VkImageMemoryBarrier> barriers(regions.size());
 for(size_t i=0;i<barriers.size();i++)barriers[i]={VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,nullptr,0,VK_ACCESS_TRANSFER_WRITE_BIT,VK_IMAGE_LAYOUT_UNDEFINED,
  VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,VK_QUEUE_FAMILY_IGNORED,VK_QUEUE_FAMILY_IGNORED,u.images[i].image,{VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1}};
 auto barrier=upCmdPipelineBarrier.load();
 barrier(cmd,VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,0,0,nullptr,0,nullptr,uint32_t(barriers.size()),barriers.data());
 for(size_t i=0;i<barriers.size();i++){
  upCmdCopyBufferToImage.load()(cmd,u.staging,u.images[i].image,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,1,&regions[i]);
  auto& x=barriers[i];x.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;x.dstAccessMask=VK_ACCESS_SHADER_READ_BIT;
  x.oldLayout=VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;x.newLayout=VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
 }
 barrier(cmd,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,0,0,nullptr,0,nullptr,uint32_t(barriers.size()),barriers.data());
 if(upEndCommandBuffer.load()(cmd)!=VK_SUCCESS)return false;
 const VkFenceCreateInfo fc{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO,nullptr,0};
 if(upCreateFence.load()(d,&fc,nullptr,&u.fence)!=VK_SUCCESS){u.fence={};return false;}
 const VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO,nullptr,0,nullptr,nullptr,1,&cmd,0,nullptr};
 if(upQueueSubmit.load()(queue,1,&submit,u.fence)!=VK_SUCCESS)return false;
 u.usedFamily=family;u.queued=true;return true;
}

::VkResult uploadOnCreateDevice(VkPhysicalDevice p,const VkDeviceCreateInfo* c,const VkAllocationCallbacks* a,VkDevice* d){
 const auto r=upCreateDevice.load()(p,c,a,d);
 if(r==VK_SUCCESS&&c&&d)tintObserve([&](auto& s){auto& u=s.devices[*d].upload;
  VkFormatProperties fmt{};upGetPhysicalDeviceFormatProperties.load()(p,VK_FORMAT_R8G8B8A8_SRGB,&fmt);
  constexpr auto required=VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT|VK_FORMAT_FEATURE_TRANSFER_DST_BIT;
  if((fmt.optimalTilingFeatures&required)!=required)return;
  upGetPhysicalDeviceMemoryProperties.load()(p,&u.memory);
  uint32_t n=0;upGetPhysicalDeviceQueueFamilyProperties.load()(p,&n,nullptr);if(!n||n>128)return;
  std::vector<VkQueueFamilyProperties> props(n);upGetPhysicalDeviceQueueFamilyProperties.load()(p,&n,props.data());
  for(uint32_t i=0;i<c->queueCreateInfoCount;i++){
   const auto& q=c->pQueueCreateInfos[i];const uint32_t f=q.queueFamilyIndex;
   if(q.flags||q.queueCount!=1||f>=n||!(props[f].queueFlags&VK_QUEUE_GRAPHICS_BIT)||(T().skinnedModel&&!(props[f].queueFlags&VK_QUEUE_COMPUTE_BIT)))continue;
   unsigned same=0;for(uint32_t j=0;j<c->queueCreateInfoCount;j++)same+=c->pQueueCreateInfos[j].queueFamilyIndex==f;
   if(same==1)u.families.insert(f);
  }
 });
 return r;
}
void uploadQueue(VkDevice d,uint32_t family,uint32_t index,VkQueue* q){
 if(!index&&q&&*q)tintObserve([&](auto& s){auto it=s.devices.find(d);if(it!=s.devices.end()&&it->second.upload.families.count(family))s.queues[*q]={d,family};});
}
void uploadOnGetDeviceQueue(VkDevice d,uint32_t family,uint32_t index,VkQueue* q){upGetDeviceQueue.load()(d,family,index,q);uploadQueue(d,family,index,q);}
void uploadOnGetDeviceQueue2(VkDevice d,const VkDeviceQueueInfo2* i,VkQueue* q){upGetDeviceQueue2.load()(d,i,q);if(i&&!i->flags)uploadQueue(d,i->queueFamilyIndex,i->queueIndex,q);}
::VkResult uploadOnCreateCommandPool(VkDevice d,const VkCommandPoolCreateInfo* c,const VkAllocationCallbacks* a,VkCommandPool* pool){
 const auto r=upCreateCommandPool.load()(d,c,a,pool);
 if(r==VK_SUCCESS&&c&&pool)tintObserve([&](auto& s){s.devices[d].families[*pool]=c->queueFamilyIndex;});return r;
}
::VkResult uploadOnQueueSubmit(VkQueue queue,uint32_t n,const VkSubmitInfo* p,VkFence fence){
 // The application already serializes this queue. On its sole graphics queue, the copy/barrier submission
 // orders all later draws for this family. Retire staging only after our fence signals, without blocking.
 tintObserve([&](auto& s){auto q=s.queues.find(queue);if(q==s.queues.end())return;
  auto [d,family]=q->second;auto it=s.devices.find(d);if(it==s.devices.end())return;auto& u=it->second.upload;
  if(!u.attempted){u.attempted=true;if((!T().skinnedModel||prepareSkinBuffers(d,it->second))&&stagePaimon(d,queue,family,u))T().textureUpload.store(1,std::memory_order_relaxed);
   else{releaseUpload(d,u);if(T().skinnedModel)releaseSkin(d,it->second.skin);T().textureUpload.store(-1,std::memory_order_relaxed);}}
  else if(u.queued&&u.fence&&u.usedFamily==family&&upGetFenceStatus.load()(d,u.fence)==VK_SUCCESS)releaseUploadStaging(d,u);
 });
 const auto result=upQueueSubmit.load()(queue,n,p,fence);
 if(result==VK_SUCCESS&&T().skinnedModel)tintObserve([&](auto& s){auto q=s.queues.find(queue);if(q==s.queues.end())return;
  auto& dev=s.devices.at(q->second.first);if(!dev.upload.queued||dev.upload.usedFamily!=q->second.second)return;
  for(uint32_t i=0;p&&i<n;i++)for(uint32_t j=0;j<p[i].commandBufferCount;j++){
   auto it=s.commands.find(p[i].pCommandBuffers[j]);if(it==s.commands.end()||it->second.first!=q->second.first)continue;
   for(auto b:it->second.second.skinProduced){auto o=dev.skin.output.find(b);if(o!=dev.skin.output.end())o->second.queued=true;}
   for(auto b:it->second.second.sharedProduced){auto o=dev.skin.sharedOutput.find(b);if(o!=dev.skin.sharedOutput.end())o->second.queued=true;}
  }});
 return result;
}

::VkResult tintOnCreateImageView(VkDevice d,const VkImageViewCreateInfo* p,const VkAllocationCallbacks* a,VkImageView* out){
 const auto result=tintCreateImageView.load()(d,p,a,out);
 if(result==VK_SUCCESS&&p&&out)tintObserve([&](auto& s){auto& dev=s.devices[d];dev.images.erase(*out);
  const bool fmt=T().allSkins?(p->format==VK_FORMAT_R8G8B8A8_SRGB||(p->format>=VK_FORMAT_ASTC_4x4_SRGB_BLOCK&&p->format<=VK_FORMAT_ASTC_12x12_SRGB_BLOCK&&(int(p->format)-int(VK_FORMAT_ASTC_4x4_SRGB_BLOCK))%2==0)):p->format==VK_FORMAT_ASTC_6x6_SRGB_BLOCK;
  if(!p->pNext&&!p->flags&&fmt&&p->viewType==VK_IMAGE_VIEW_TYPE_2D&&p->subresourceRange.aspectMask==VK_IMAGE_ASPECT_COLOR_BIT&&p->subresourceRange.layerCount==1)
   dev.images.emplace(*out,TintImg{*p,++dev.generation,{}});
 });
 return result;
}
void tintOnDestroyImageView(VkDevice d,VkImageView v,const VkAllocationCallbacks* a){
 tintObserve([&](auto& s){s.devices[d].images.erase(v);});tintDestroyImageView.load()(d,v,a);
}
void tintOnDestroyImage(VkDevice d,VkImage image,const VkAllocationCallbacks* a){
 tintObserve([&](auto& s){auto& dev=s.devices[d];
  for(auto i=dev.images.begin();i!=dev.images.end();)if(i->second.info.image==image)i=dev.images.erase(i);else ++i;
  for(auto i=dev.ownedViews.begin();i!=dev.ownedViews.end();)if(i->first==image){tintDestroyImageView.load()(d,i->second,nullptr);i=dev.ownedViews.erase(i);}else ++i;
 });tintDestroyImage.load()(d,image,a);
}
::VkResult tintOnCreateDescriptorSetLayout(VkDevice d,const VkDescriptorSetLayoutCreateInfo* p,const VkAllocationCallbacks* a,VkDescriptorSetLayout* out){
 auto r=tintCreateDescriptorSetLayout.load()(d,p,a,out);
 if(r==VK_SUCCESS&&p&&out)tintObserve([&](auto& s){auto& l=s.devices[d].layouts;auto shape=tintShape(*p);
  if(!shape&&T().skinnedModel)shape=skinShape(*p);if(shape)l[*out]=shape;else l.erase(*out);});
 return r;
}
void tintOnDestroyDescriptorSetLayout(VkDevice d,VkDescriptorSetLayout layout,const VkAllocationCallbacks* a){
 tintObserve([&](auto& s){s.devices[d].layouts.erase(layout);});tintDestroyDescriptorSetLayout.load()(d,layout,a);
}
::VkResult tintOnCreatePipelineLayout(VkDevice d,const VkPipelineLayoutCreateInfo* p,const VkAllocationCallbacks* a,VkPipelineLayout* out){
 auto r=tintCreatePipelineLayout.load()(d,p,a,out);
 if(r==VK_SUCCESS&&p&&out)tintObserve([&](auto& s){auto& dev=s.devices[d];dev.pipelineLayouts.erase(*out);dev.skin.layouts.erase(*out);
  if(!p->flags&&!p->pNext&&p->setLayoutCount&&p->pSetLayouts){auto i=dev.layouts.find(p->pSetLayouts[0]);if(i==dev.layouts.end())return;
   if(i->second<skinStorage)dev.pipelineLayouts[*out]={i->second};
   else if(i->second==skinStorage&&p->setLayoutCount==2&&!p->pushConstantRangeCount){auto j=dev.layouts.find(p->pSetLayouts[1]);if(j!=dev.layouts.end()&&j->second==skinUniform)dev.skin.layouts.insert(*out);}
  }});
 return r;
}
void tintOnDestroyPipelineLayout(VkDevice d,VkPipelineLayout layout,const VkAllocationCallbacks* a){
 tintObserve([&](auto& s){auto& dev=s.devices[d];dev.pipelineLayouts.erase(layout);
  for(auto i=dev.pipelines.begin();i!=dev.pipelines.end();)if(i->second==layout)i=dev.pipelines.erase(i);else ++i;
  dev.skin.layouts.erase(layout);for(auto i=dev.skin.compute.begin();i!=dev.skin.compute.end();)if(i->second==layout)i=dev.skin.compute.erase(i);else ++i;
 });tintDestroyPipelineLayout.load()(d,layout,a);
}
std::array<uint8_t,32> skinSemantics(const uint32_t* code,size_t n){
 // SPIR-V input Location decorations and names in Unity's compiled shader, independent of the pipeline handle / hash.
 std::array<uint8_t,32> result{};if(!code||n<5||n>(1u<<18)||code[0]!=0x07230203)return result;
 struct Input {unsigned loc=~0u;bool input=false;uint8_t type=0;};std::map<uint32_t,Input> vars;
 static const char* labels[]={"in_POSITION0","in_NORMAL0","in_TANGENT0","in_COLOR0","in_TEXCOORD0","in_TEXCOORD1"};
 for(size_t i=5;i<n;){const size_t len=code[i]>>16;if(!len||len>n-i)return {};
  const auto op=code[i]&65535u;
  if(op==5&&len>=3){const char* name=reinterpret_cast<const char*>(&code[i+2]);for(unsigned j=0;j<6;j++){size_t lenName=std::strlen(labels[j])+1;if((len-2)*4>=lenName&&std::memcmp(name,labels[j],lenName)==0){vars[code[i+1]].type=uint8_t(j+1);break;}}}
  if(op==59&&len>=4&&code[i+3]==1)vars[code[i+2]].input=true;
  if(op==71&&len==4&&code[i+2]==30)vars[code[i+1]].loc=code[i+3];
  i+=len;}
 bool pos=false;for(const auto& kv:vars){const auto& x=kv.second;if(!x.input||x.loc==~0u)continue;if(x.loc>=result.size()||!x.type||result[x.loc])return {};result[x.loc]=x.type;pos=pos||x.type==1;}
 return pos?result:std::array<uint8_t,32>{};
}
::VkResult tintOnCreateShaderModule(VkDevice d,const VkShaderModuleCreateInfo* p,const VkAllocationCallbacks* a,VkShaderModule* out){
 auto r=tintCreateShaderModule.load()(d,p,a,out);
 if(r==VK_SUCCESS&&p&&out)tintObserve([&](auto& s){auto& dev=s.devices[d];auto& modules=dev.mainTextures;modules.erase(*out);
  if(p->codeSize%4==0&&ms::vkcap::mainTextureBinding(p->pCode,p->codeSize/4))modules[*out]=true;
  if(T().skinnedModel){auto& skin=dev.skin;skin.kernels.erase(*out);skin.vertexCodes.erase(*out);skin.vertexSemantics.erase(*out);
   if(T().allSkins&&p->codeSize%4==0){auto semantics=skinSemantics(p->pCode,p->codeSize/4);if(std::any_of(semantics.begin(),semantics.end(),[](auto s){return s==1;}))skin.vertexSemantics[*out]=semantics;}
   // Exact programs from the captured frame: compute and the five draw pipeline vertex variants.
   if(p->pCode&&(p->codeSize==22676||p->codeSize==7480||p->codeSize==36764||p->codeSize==36664||p->codeSize==33376)){
    auto crc=ms::crc32(reinterpret_cast<const uint8_t*>(p->pCode),p->codeSize);
    if(crc==0x0374e652u)skin.kernels.insert(*out);
    else if(crc==0xf4fcd9c0u||crc==0x931beaf8u||crc==0xad6ddb74u||crc==0xcda35682u||crc==0x91473acfu)skin.vertexCodes[*out]=crc;
   }}
 });
 return r;
}
void tintOnDestroyShaderModule(VkDevice d,VkShaderModule shader,const VkAllocationCallbacks* a){
 tintObserve([&](auto& s){auto& dev=s.devices[d];dev.mainTextures.erase(shader);dev.skin.kernels.erase(shader);dev.skin.vertexCodes.erase(shader);dev.skin.vertexSemantics.erase(shader);});tintDestroyShaderModule.load()(d,shader,a);
}
unsigned skinVertexLayout(const VkGraphicsPipelineCreateInfo& p,uint32_t code,const std::array<uint8_t,32>* semantics=nullptr){
 const auto* v=p.pVertexInputState;if(!v)return 0;
 const unsigned n=semantics?v->vertexBindingDescriptionCount:code==0xad6ddb74u?3:2;
 if(p.pNext||!p.pInputAssemblyState||p.pInputAssemblyState->topology!=VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST||p.pInputAssemblyState->primitiveRestartEnable||
  (n!=2&&n!=3)||v->pNext||v->vertexBindingDescriptionCount!=n||!v->pVertexBindingDescriptions||!v->pVertexAttributeDescriptions)return 0;
 if(const auto* dynamic=p.pDynamicState)for(uint32_t i=0;dynamic->pDynamicStates&&i<dynamic->dynamicStateCount;i++){
  auto state=dynamic->pDynamicStates[i];if(state==VK_DYNAMIC_STATE_VERTEX_INPUT_EXT||state==VK_DYNAMIC_STATE_VERTEX_INPUT_BINDING_STRIDE)return 0;}
 unsigned seen=0;for(uint32_t i=0;i<n;i++){const auto& b=v->pVertexBindingDescriptions[i];if(b.binding>=n||b.stride!=(b.binding==1?20u:40u)||b.inputRate!=VK_VERTEX_INPUT_RATE_VERTEX||(seen&(1u<<b.binding)))return 0;seen|=1u<<b.binding;}
 for(uint32_t i=0;i<v->vertexAttributeDescriptionCount;i++){const auto& a=v->pVertexAttributeDescriptions[i];
  if(a.binding>=n)return 0;
  if(semantics){if(a.location>=semantics->size())return 0;auto kind=(*semantics)[a.location];
   switch(kind){
   case 1:if(a.binding==0&&a.offset==0&&a.format==VK_FORMAT_R32G32B32_SFLOAT)continue;break;
   case 2:if(((a.binding==0&&a.offset==12)||(a.binding==2&&a.offset==0))&&a.format==VK_FORMAT_R32G32B32_SFLOAT)continue;break;
   case 3:if(a.binding==0&&a.offset==24&&a.format==VK_FORMAT_R32G32B32A32_SFLOAT)continue;break;
   case 4:if(a.binding==1&&a.offset==0&&a.format==VK_FORMAT_R8G8B8A8_UNORM)continue;break;
   case 5:case 6:if(a.binding==1&&a.offset==4+8u*(kind-5)&&a.format==VK_FORMAT_R32G32_SFLOAT)continue;break;
   default:break;}
   return 0;}
  if(a.binding==1){if(a.offset==0&&a.format==VK_FORMAT_R8G8B8A8_UNORM)continue;if((a.offset==4||a.offset==12)&&a.format==VK_FORMAT_R32G32_SFLOAT)continue;return 0;}
  if((a.offset==0||a.offset==12)&&a.format==VK_FORMAT_R32G32B32_SFLOAT)continue;
  if(a.binding==0&&a.offset==24&&a.format==VK_FORMAT_R32G32B32A32_SFLOAT)continue;
  return 0;
 }
 return n;
}
::VkResult tintOnCreateGraphicsPipelines(VkDevice d,VkPipelineCache c,uint32_t n,const VkGraphicsPipelineCreateInfo* p,const VkAllocationCallbacks* a,VkPipeline* out){
 auto r=tintCreateGraphicsPipelines.load()(d,c,n,p,a,out);
 if(r==VK_SUCCESS&&p&&out)tintObserve([&](auto& s){auto& dev=s.devices[d];for(uint32_t i=0;i<n;i++)if(out[i]){
  dev.pipelines.erase(out[i]);dev.skin.graphics.erase(out[i]);if(!dev.pipelineLayouts.count(p[i].layout))continue;
  for(uint32_t j=0;p[i].pStages&&j<p[i].stageCount;j++){const auto& stage=p[i].pStages[j];
   if(stage.stage==VK_SHADER_STAGE_FRAGMENT_BIT&&dev.mainTextures.count(stage.module))dev.pipelines[out[i]]=p[i].layout;
   if(stage.stage==VK_SHADER_STAGE_VERTEX_BIT&&!stage.pNext&&!stage.pSpecializationInfo&&stage.pName&&std::strcmp(stage.pName,"main")==0){
    auto v=dev.skin.vertexCodes.find(stage.module);auto semantics=dev.skin.vertexSemantics.find(stage.module);
    if(v!=dev.skin.vertexCodes.end()||semantics!=dev.skin.vertexSemantics.end())if(auto bindings=skinVertexLayout(p[i],v!=dev.skin.vertexCodes.end()?v->second:0,semantics!=dev.skin.vertexSemantics.end()?&semantics->second:nullptr))dev.skin.graphics[out[i]]=bindings;}}
 }});
 return r;
}
void tintOnDestroyPipeline(VkDevice d,VkPipeline pipeline,const VkAllocationCallbacks* a){
 tintObserve([&](auto& s){auto& dev=s.devices[d];dev.pipelines.erase(pipeline);dev.skin.graphics.erase(pipeline);dev.skin.compute.erase(pipeline);});tintDestroyPipeline.load()(d,pipeline,a);
}
::VkResult skinOnCreateComputePipelines(VkDevice d,VkPipelineCache c,uint32_t n,const VkComputePipelineCreateInfo* p,const VkAllocationCallbacks* a,VkPipeline* out){
 auto r=upCreateComputePipelines.load()(d,c,n,p,a,out);
 if(r==VK_SUCCESS&&p&&out)tintObserve([&](auto& s){auto& skin=s.devices[d].skin;for(uint32_t i=0;i<n;i++)if(out[i]){
  skin.compute.erase(out[i]);const auto& stage=p[i].stage;
  if(!p[i].pNext&&!stage.pNext&&!stage.pSpecializationInfo&&stage.pName&&std::strcmp(stage.pName,"main")==0&&skin.layouts.count(p[i].layout)&&skin.kernels.count(stage.module))skin.compute[out[i]]=p[i].layout;
 }});
 return r;
}
::VkResult tintOnAllocateDescriptorSets(VkDevice d,const VkDescriptorSetAllocateInfo* p,VkDescriptorSet* out){
 auto r=tintAllocateDescriptorSets.load()(d,p,out);
 if(r==VK_SUCCESS&&p&&p->pSetLayouts&&out)tintObserve([&](auto& s){auto& dev=s.devices[d];for(uint32_t i=0;i<p->descriptorSetCount;i++){
  dev.sets.erase(out[i]);dev.skin.sets.erase(out[i]);auto l=dev.layouts.find(p->pSetLayouts[i]);
  if(!p->pNext&&l!=dev.layouts.end()){
   if(l->second>=skinStorage)dev.skin.sets.emplace(out[i],SkinSet{p->descriptorPool,l->second});
   else dev.sets.emplace(out[i],TintSet{p->descriptorPool,l->second});}}});
 return r;
}
void tintOnUpdateDescriptorSets(VkDevice d,uint32_t n,const VkWriteDescriptorSet* p,uint32_t nc,const VkCopyDescriptorSet* copies){
 tintUpdateDescriptorSets.load()(d,n,p,nc,copies);
 tintObserve([&](auto& s){auto& dev=s.devices[d];for(uint32_t i=0;p&&i<n;i++){
  const auto& w=p[i];auto it=dev.sets.find(w.dstSet);
  if(it==dev.sets.end()){
   auto a=dev.skin.sets.find(w.dstSet);if(a==dev.skin.sets.end())continue;auto& x=a->second;const bool storage=x.shape==skinStorage;
   if(w.pNext||w.dstBinding>=(storage?4u:1u)||w.dstArrayElement||w.descriptorCount!=1||w.descriptorType!=(storage?VK_DESCRIPTOR_TYPE_STORAGE_BUFFER:VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER)||!w.pBufferInfo){x.bits=0;continue;}
   x.buffers[w.dstBinding]=*w.pBufferInfo;x.bits|=1u<<w.dstBinding;continue;
  }
  auto& set=it->second;
  set.cache={};set.textureCache.clear(); // a known handle value may now name a freshly allocated resource
  if(w.pNext||w.dstBinding>=tintSlotCount(set.shape)||w.dstArrayElement||w.descriptorCount!=1||w.descriptorType!=VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER||!w.pImageInfo){set.written=0;continue;}
  set.images[w.dstBinding]=*w.pImageInfo;set.written|=1u<<w.dstBinding;
 }for(uint32_t i=0;copies&&i<nc;i++){auto it=dev.sets.find(copies[i].dstSet);if(it!=dev.sets.end()){it->second.written=0;it->second.cache={};it->second.textureCache.clear();}
   auto a=dev.skin.sets.find(copies[i].dstSet);if(a!=dev.skin.sets.end())a->second.bits=0;}});
}
void tintTemplateWrite(PFN_vkUpdateDescriptorSetWithTemplate fn,VkDevice d,VkDescriptorSet set,VkDescriptorUpdateTemplate t,const void* p){
 fn(d,set,t,p);tintObserve([&](auto& s){auto& dev=s.devices[d];auto i=dev.sets.find(set);if(i!=dev.sets.end()){i->second.written=0;i->second.cache={};i->second.textureCache.clear();}
  auto a=dev.skin.sets.find(set);if(a!=dev.skin.sets.end())a->second.bits=0;});
}
void tintOnUpdateDescriptorSetWithTemplate(VkDevice d,VkDescriptorSet set,VkDescriptorUpdateTemplate t,const void* p){tintTemplateWrite(tintUpdateDescriptorSetWithTemplate.load(),d,set,t,p);}
void tintOnUpdateDescriptorSetWithTemplateKHR(VkDevice d,VkDescriptorSet set,VkDescriptorUpdateTemplate t,const void* p){tintTemplateWrite(tintUpdateDescriptorSetWithTemplateKHR.load(),d,set,t,p);}
::VkResult tintOnFreeDescriptorSets(VkDevice d,VkDescriptorPool pool,uint32_t n,const VkDescriptorSet* p){
 auto r=tintFreeDescriptorSets.load()(d,pool,n,p);
 if(r==VK_SUCCESS)tintObserve([&](auto& s){auto& dev=s.devices[d];for(uint32_t i=0;p&&i<n;i++){dev.sets.erase(p[i]);dev.skin.sets.erase(p[i]);}});
 return r;
}
void tintForgetPool(VkDevice d,VkDescriptorPool pool){tintObserve([&](auto& s){auto& dev=s.devices[d];
 for(auto i=dev.sets.begin();i!=dev.sets.end();)if(i->second.pool==pool)i=dev.sets.erase(i);else ++i;
 for(auto i=dev.skin.sets.begin();i!=dev.skin.sets.end();)if(i->second.pool==pool)i=dev.skin.sets.erase(i);else ++i;});}
::VkResult tintOnResetDescriptorPool(VkDevice d,VkDescriptorPool p,VkDescriptorPoolResetFlags f){auto r=tintResetDescriptorPool.load()(d,p,f);if(r==VK_SUCCESS)tintForgetPool(d,p);return r;}
void tintOnDestroyDescriptorPool(VkDevice d,VkDescriptorPool pool,const VkAllocationCallbacks* a){tintForgetPool(d,pool);tintDestroyDescriptorPool.load()(d,pool,a);}
::VkResult tintOnAllocateCommandBuffers(VkDevice d,const VkCommandBufferAllocateInfo* p,VkCommandBuffer* out){
 auto r=tintAllocateCommandBuffers.load()(d,p,out);
 if(r==VK_SUCCESS&&p&&out)tintObserve([&](auto& s){for(uint32_t i=0;i<p->commandBufferCount;i++)s.commands[out[i]]={d,TintCmd{p->commandPool}};});
 return r;
}
void tintOnFreeCommandBuffers(VkDevice d,VkCommandPool p,uint32_t n,const VkCommandBuffer* buffers){
 tintObserve([&](auto& s){for(uint32_t i=0;buffers&&i<n;i++)s.commands.erase(buffers[i]);});tintFreeCommandBuffers.load()(d,p,n,buffers);
}
::VkResult tintOnResetCommandPool(VkDevice d,VkCommandPool p,VkCommandPoolResetFlags f){
 auto r=tintResetCommandPool.load()(d,p,f);
 if(r==VK_SUCCESS)tintObserve([&](auto& s){for(auto& kv:s.commands)if(kv.second.first==d&&kv.second.second.pool==p)kv.second.second=TintCmd{p};});
 return r;
}
void tintOnDestroyCommandPool(VkDevice d,VkCommandPool p,const VkAllocationCallbacks* a){
 tintObserve([&](auto& s){s.devices[d].families.erase(p);for(auto i=s.commands.begin();i!=s.commands.end();)if(i->second.first==d&&i->second.second.pool==p)i=s.commands.erase(i);else ++i;});tintDestroyCommandPool.load()(d,p,a);
}
void tintOnDestroyDevice(VkDevice d,const VkAllocationCallbacks* a){
 tintObserve([&](auto& s){auto i=s.devices.find(d);if(i!=s.devices.end()){auto& dev=i->second;
  if(dev.pool)tintDestroyDescriptorPool.load()(d,dev.pool,nullptr);
  for(const auto& v:dev.ownedViews)tintDestroyImageView.load()(d,v.second,nullptr);
  for(const auto& l:dev.ownedLayouts)tintDestroyDescriptorSetLayout.load()(d,l.second,nullptr);
  releaseSkin(d,dev.skin);
  releaseUpload(d,dev.upload);
  s.devices.erase(i);}
  for(auto q=s.queues.begin();q!=s.queues.end();)if(q->second.first==d)q=s.queues.erase(q);else ++q;
  for(auto j=s.commands.begin();j!=s.commands.end();)if(j->second.first==d)j=s.commands.erase(j);else ++j;
 });tintDestroyDevice.load()(d,a);
}
void tintBegin(VkCommandBuffer c){tintObserve([&](auto& s){auto i=s.commands.find(c);if(i!=s.commands.end())i->second.second=TintCmd{i->second.second.pool};});}
void tintOnCmdBindPipeline(VkCommandBuffer c,VkPipelineBindPoint bp,VkPipeline pipe){
 tintCmdBindPipeline.load()(c,bp,pipe);if(bp==VK_PIPELINE_BIND_POINT_GRAPHICS||(bp==VK_PIPELINE_BIND_POINT_COMPUTE&&T().skinnedModel))tintObserve([&](auto& s){auto i=s.commands.find(c);if(i!=s.commands.end()){
  auto& cmd=i->second.second;if(bp==VK_PIPELINE_BIND_POINT_GRAPHICS)cmd.pipeline=pipe;else cmd.compute=pipe;}});
}
void tintOnCmdBindDescriptorSets(VkCommandBuffer c,VkPipelineBindPoint bp,VkPipelineLayout layout,uint32_t first,uint32_t n,const VkDescriptorSet* p,uint32_t offsets,const uint32_t* values){
 tintCmdBindDescriptorSets.load()(c,bp,layout,first,n,p,offsets,values);
 if(n&&(bp==VK_PIPELINE_BIND_POINT_GRAPHICS||(bp==VK_PIPELINE_BIND_POINT_COMPUTE&&T().skinnedModel)))tintObserve([&](auto& s){auto i=s.commands.find(c);if(i==s.commands.end())return;auto& cmd=i->second.second;
  if(bp==VK_PIPELINE_BIND_POINT_GRAPHICS){if(!first){cmd.layout=layout;cmd.set=p?p[0]:VkDescriptorSet{};}else if(cmd.layout!=layout){cmd.layout={};cmd.set={};}}
  else if(!first&&n==2&&!offsets&&p){cmd.computeLayout=layout;cmd.computeSets={p[0],p[1]};}
  else{cmd.computeLayout={};cmd.computeSets={};}});
}

void skinOnCmdBindVertexBuffers(VkCommandBuffer c,uint32_t first,uint32_t n,const VkBuffer* buffers,const VkDeviceSize* offsets){
 upCmdBindVertexBuffers.load()(c,first,n,buffers,offsets);
 tintObserve([&](auto& s){auto i=s.commands.find(c);if(i==s.commands.end()||!buffers||!offsets)return;auto& x=i->second.second;
  for(uint32_t j=0;j<n&&first<3&&j<3-first;j++){x.vertex[first+j]=buffers[j];x.vertexOffset[first+j]=offsets[j];}});
}
void skinBoundIndex(VkCommandBuffer c,VkBuffer b,VkDeviceSize offset,VkIndexType type){tintObserve([&](auto& s){auto i=s.commands.find(c);if(i!=s.commands.end()){
 auto& x=i->second.second;x.index=b;x.indexOffset=offset;x.indexType=type;}});}
void skinForgetBuffer(VkDevice d,VkBuffer buffer){tintObserve([&](auto& s){auto i=s.devices.find(d);if(i==s.devices.end())return;auto& x=i->second.skin;
 auto o=x.output.find(buffer);if(o!=x.output.end()){auto b=o->second.buffer;x.retired.push_back(b);x.output.erase(o);}
 for(auto a=x.sharedOutput.begin();a!=x.sharedOutput.end();)if(a->first.first==buffer){x.retired.push_back(a->second.buffer);a=x.sharedOutput.erase(a);}else ++a;
 for(auto a=x.seenOutputs.begin();a!=x.seenOutputs.end();)if(a->first==buffer||a->second.rest==buffer||a->second.weights==buffer)a=x.seenOutputs.erase(a);else ++a;
 for(auto a=x.sourceProfiles.begin();a!=x.sourceProfiles.end();)if(a->first.first==buffer||a->first.second==buffer)a=x.sourceProfiles.erase(a);else ++a;
 for(auto a=x.sharedRemaps.begin();a!=x.sharedRemaps.end();)if(std::get<0>(a->first)==buffer||std::get<1>(a->first)==buffer)a=x.sharedRemaps.erase(a);else ++a;
 for(auto q=x.remaps.begin();q!=x.remaps.end();)if(q->first.first==buffer||q->first.second==buffer)q=x.remaps.erase(q);else ++q;
 for(auto& kv:s.commands)if(kv.second.first==d){auto& cmd=kv.second.second;cmd.skinProduced.erase(buffer);
  for(auto a=cmd.sharedProduced.begin();a!=cmd.sharedProduced.end();)if(a->first==buffer)a=cmd.sharedProduced.erase(a);else ++a;}
 });}
bool skinLayouts(VkDevice d,SkinDevice& s,bool legacy=true){
 auto create=[&](VkDescriptorSetLayout& slot,uint32_t n,VkDescriptorType type){if(slot)return true;std::array<VkDescriptorSetLayoutBinding,4> bindings{};
  for(uint32_t i=0;i<n;i++)bindings[i]={i,type,1,VK_SHADER_STAGE_COMPUTE_BIT,nullptr};
  const VkDescriptorSetLayoutCreateInfo p{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,nullptr,0,n,bindings.data()};
  if(tintCreateDescriptorSetLayout.load()(d,&p,nullptr,&slot)==VK_SUCCESS)return true;
  slot={};return false;};
 if(!create(s.storage,4,VK_DESCRIPTOR_TYPE_STORAGE_BUFFER)||!create(s.uniform,1,VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER))return false;
 if(!s.pool){const uint32_t uniforms=legacy?1u:uint32_t(s.profiles.size());const VkDescriptorPoolSize sizes[]={{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,uint32_t(4*maxSkinSets)},{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,uniforms}};
  const VkDescriptorPoolCreateInfo p{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,nullptr,0,uint32_t(maxSkinSets+uniforms),2,sizes};
  if(tintCreateDescriptorPool.load()(d,&p,nullptr,&s.pool)!=VK_SUCCESS){s.pool={};return false;}}
 if(legacy&&!s.count){const VkDescriptorSetAllocateInfo a{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,nullptr,s.pool,1,&s.uniform};
  if(tintAllocateDescriptorSets.load()(d,&a,&s.count)!=VK_SUCCESS){s.count={};return false;}
  const VkDescriptorBufferInfo data{s.input[Count].buffer,0,s.input[Count].size};
  const VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,nullptr,s.count,0,0,1,VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,nullptr,&data,nullptr};
  tintUpdateDescriptorSets.load()(d,1,&w,0,nullptr);}
 return true;
}
VkDescriptorSet writeSkinSet(VkDevice device,SkinDevice& s,const std::array<SkinBuffer,SkinStreamCount>& input,const VkDescriptorBufferInfo& palette,const SkinBuffer& output){
 if(s.allocated>=maxSkinSets)return {};
 const VkDescriptorSetAllocateInfo p{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,nullptr,s.pool,1,&s.storage};VkDescriptorSet set{};
 if(tintAllocateDescriptorSets.load()(device,&p,&set)!=VK_SUCCESS)return {};
 ++s.allocated;
 const VkDescriptorBufferInfo info[]={{input[Rest].buffer,0,input[Rest].size},{input[Weights].buffer,0,input[Weights].size},palette,{output.buffer,0,output.size}};
 std::array<VkWriteDescriptorSet,4> writes{};for(uint32_t i=0;i<4;i++)writes[i]={VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,nullptr,set,i,0,1,VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,nullptr,info+i,nullptr};
 tintUpdateDescriptorSets.load()(device,4,writes.data(),0,nullptr);return set;
}
VkDescriptorSet skinSetFor(VkDevice device,SkinDevice& s,VkBuffer original,const VkDescriptorBufferInfo& palette,const SkinBuffer& output){
 const auto key=std::make_pair(original,palette.buffer);auto it=s.remaps.find(key);if(it!=s.remaps.end())return it->second;
 if(!skinLayouts(device,s))return {};
 auto set=writeSkinSet(device,s,s.input,palette,output);if(set)s.remaps.emplace(key,set);return set;
}
void issueSkin(VkCommandBuffer cmd,TintCmd& cstate,VkDescriptorSet set,VkDescriptorSet uniform,const SkinBuffer& out,uint32_t vertices){
 auto barrier=upCmdPipelineBarrier.load();VkBufferMemoryBarrier memory{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,nullptr,VK_ACCESS_VERTEX_ATTRIBUTE_READ_BIT|VK_ACCESS_SHADER_WRITE_BIT,
  VK_ACCESS_SHADER_WRITE_BIT,VK_QUEUE_FAMILY_IGNORED,VK_QUEUE_FAMILY_IGNORED,out.buffer,0,out.size};
 barrier(cmd,VK_PIPELINE_STAGE_VERTEX_INPUT_BIT|VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,0,0,nullptr,1,&memory,0,nullptr);
 auto bind=tintCmdBindDescriptorSets.load();const VkDescriptorSet own[]={set,uniform};
 bind(cmd,VK_PIPELINE_BIND_POINT_COMPUTE,cstate.computeLayout,0,2,own,0,nullptr);
 upCmdDispatch.load()(cmd,(vertices+63)/64,1,1);
 bind(cmd,VK_PIPELINE_BIND_POINT_COMPUTE,cstate.computeLayout,0,2,cstate.computeSets.data(),0,nullptr);
 memory.srcAccessMask=VK_ACCESS_SHADER_WRITE_BIT;memory.dstAccessMask=VK_ACCESS_VERTEX_ATTRIBUTE_READ_BIT;
 barrier(cmd,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,VK_PIPELINE_STAGE_VERTEX_INPUT_BIT,0,0,nullptr,1,&memory,0,nullptr);
 T().skinDispatches.fetch_add(1,std::memory_order_relaxed);
}
void sharedSkinDispatch(VkDevice device,VkCommandBuffer cmd,TintCmd& state,TintDevice& d,const std::array<VkDescriptorBufferInfo,4>& v,uint32_t groups){
 auto& s=d.skin;
 if(v[0].range%40||v[2].range%64||v[0].range>40ull*ms::model::maxVertices||v[2].range>64ull*ms::model::maxBones||v[3].range!=v[0].range||v[3].buffer==v[0].buffer)return;
 const unsigned n=unsigned(v[0].range/40),b=unsigned(v[2].range/64);
 if(!n||!b||groups!=(n+63)/64||v[1].range!=32ull*n)return;
 const auto k=std::make_pair(v[0].buffer,v[1].buffer);auto source=s.sourceProfiles.find(k);
 if(source==s.sourceProfiles.end()&&std::none_of(sharedSkins.begin(),sharedSkins.end(),[&](const auto& p){return p.originalVertices==n&&p.bones==b;}))return;
 if(s.seenOutputs.size()<4096||s.seenOutputs.count(v[3].buffer))s.seenOutputs[v[3].buffer]={v[0].buffer,v[1].buffer,n,b};
 if(source==s.sourceProfiles.end())return;
 const unsigned profile=source->second;if(profile>=s.profiles.size()||!T().skinActive(profile))return;
 const auto& skin=sharedSkins[profile];auto& p=s.profiles[profile];if(!p.ready||skin.originalVertices!=n||skin.bones!=b)return;
 const auto output=std::make_pair(v[3].buffer,profile);auto o=s.sharedOutput.find(output);
 if(o==s.sharedOutput.end()){
  if(s.sharedOutput.size()+s.retired.size()>=maxSkinOutputs)return;
  o=s.sharedOutput.try_emplace(output).first;
  if(!makeSkinBuffer(device,d.upload,o->second.buffer,skin.mesh.rest.size(),VK_BUFFER_USAGE_STORAGE_BUFFER_BIT|VK_BUFFER_USAGE_VERTEX_BUFFER_BIT)){s.sharedOutput.erase(o);return;}}
 if(!skinLayouts(device,s,false))return;
 if(!p.count){const VkDescriptorSetAllocateInfo a{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,nullptr,s.pool,1,&s.uniform};
  if(tintAllocateDescriptorSets.load()(device,&a,&p.count)!=VK_SUCCESS){p.count={};return;}
  const VkDescriptorBufferInfo data{p.input[Count].buffer,0,p.input[Count].size};
  const VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,nullptr,p.count,0,0,1,VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,nullptr,&data,nullptr};tintUpdateDescriptorSets.load()(device,1,&w,0,nullptr);}
 auto& set=s.sharedRemaps[{v[3].buffer,v[2].buffer,profile}];if(!set)set=writeSkinSet(device,s,p.input,v[2],o->second.buffer);
 if(!set||!T().skinActive(profile))return;
 state.sharedProduced.insert(output);issueSkin(cmd,state,set,p.count,o->second.buffer,skin.mesh.vertices);
}
void skinOnCmdDispatch(VkCommandBuffer cmd,uint32_t x,uint32_t y,uint32_t z){
 auto dispatch=upCmdDispatch.load();dispatch(cmd,x,y,z);
 if(y!=1||z!=1||(!T().allSkins&&(x!=132||!T().tintOn.load(std::memory_order_relaxed))))return;
 tintObserve([&](auto& state){auto c=state.commands.find(cmd);if(c==state.commands.end())return;auto& cstate=c->second.second;
  const auto device=c->second.first;auto& d=state.devices.at(device);auto& s=d.skin;auto f=d.families.find(cstate.pool);
  if(!d.upload.queued||(!T().allSkins&&!s.ready)||f==d.families.end()||f->second!=d.upload.usedFamily)return;
  auto pipeline=s.compute.find(cstate.compute);if(pipeline==s.compute.end()||pipeline->second!=cstate.computeLayout)return;
  const auto a=s.sets.find(cstate.computeSets[0]),b=s.sets.find(cstate.computeSets[1]);
  if(a==s.sets.end()||b==s.sets.end()||a->second.shape!=skinStorage||a->second.bits!=15||b->second.shape!=skinUniform||b->second.bits!=1||b->second.buffers[0].range!=4)return;
  const auto& v=a->second.buffers;for(const auto& k:v)if(!k.buffer||k.offset)return;
  if(T().allSkins){sharedSkinDispatch(device,cmd,cstate,d,v,x);return;}
  if(v[0].range!=335880||v[1].range!=268704||v[2].range!=4608||v[3].range!=335880||v[0].buffer==v[3].buffer)return;
  if(!T().skinSource(reinterpret_cast<uintptr_t>(v[0].buffer),v[0].range,0x72b1a301u)||!T().skinSource(reinterpret_cast<uintptr_t>(v[1].buffer),v[1].range,0x21911ac3u))return;
  auto o=s.output.find(v[3].buffer);if(o==s.output.end()){
   if(s.output.size()+s.retired.size()>=maxSkinOutputs)return;
   auto ins=s.output.try_emplace(v[3].buffer);o=ins.first;
   if(!makeSkinBuffer(device,d.upload,o->second.buffer,paimonSkin.rest.size(),VK_BUFFER_USAGE_STORAGE_BUFFER_BIT|VK_BUFFER_USAGE_VERTEX_BUFFER_BIT)){s.output.erase(o);return;}}
  const auto& out=o->second.buffer;auto set=skinSetFor(device,s,v[3].buffer,v[2],out);
  if(!set||!T().tintOn.load(std::memory_order_relaxed))return;
  cstate.skinProduced.insert(v[3].buffer);
  issueSkin(cmd,cstate,set,s.count,out,paimonSkin.vertices);
 });
}

VkDescriptorSet tintClone(VkDevice d,TintDevice& dev,TintSet& target,TintImg& image,unsigned part,VkImageView preset){
 auto& saved=preset?target.textureCache[preset]:target.cache.at(part);if(saved.set&&saved.generation==image.generation)return saved.set;
 const auto count=tintSlotCount(target.shape);
 if(dev.allocated>=tintSetLimit||target.images[0].imageLayout!=VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL)return {};
 for(uint32_t i=0;i<count;i++)if(!target.images[i].imageView||!target.images[i].sampler)return {};
 if(!dev.pool){VkDescriptorPoolSize size{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,uint32_t(tintSetLimit*tintBindings)};
  const VkDescriptorPoolCreateInfo p{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,nullptr,0,uint32_t(tintSetLimit),1,&size};
  if(tintCreateDescriptorPool.load()(d,&p,nullptr,&dev.pool)!=VK_SUCCESS){dev.pool={};return {};}}
 auto& layout=dev.ownedLayouts[target.shape];
 if(!layout){std::array<VkDescriptorSetLayoutBinding,tintBindings> b{};
  for(uint32_t i=0;i<count;i++)b[i]={i,VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,1,tintSlotStages(target.shape,i),nullptr};
  const VkDescriptorSetLayoutCreateInfo p{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,nullptr,0,count,b.data()};
  if(tintCreateDescriptorSetLayout.load()(d,&p,nullptr,&layout)!=VK_SUCCESS){layout={};return {};}}
 VkImageView unused{};auto& view=preset?unused:image.colored.at(part);
 if(!preset&&!view){if(dev.ownedViews.size()>=tintSetLimit)return {};auto ci=image.info;auto a=ci.components.a;
  ci.components=part?VkComponentMapping{VK_COMPONENT_SWIZZLE_ONE,VK_COMPONENT_SWIZZLE_ZERO,VK_COMPONENT_SWIZZLE_ONE,a}:
                      VkComponentMapping{VK_COMPONENT_SWIZZLE_ZERO,VK_COMPONENT_SWIZZLE_ONE,VK_COMPONENT_SWIZZLE_ZERO,a};
  // Reserve the bookkeeping slot before calling the driver so a failed allocation cannot leak a view.
  dev.ownedViews.emplace_back(ci.image,VkImageView{});
  if(tintCreateImageView.load()(d,&ci,nullptr,&view)!=VK_SUCCESS){view={};dev.ownedViews.pop_back();return {};}
  dev.ownedViews.back().second=view;
 }
 const VkDescriptorSetAllocateInfo a{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,nullptr,dev.pool,1,&layout};VkDescriptorSet set{};
 if(tintAllocateDescriptorSets.load()(d,&a,&set)!=VK_SUCCESS)return {};
 ++dev.allocated;
 auto images=target.images;images[0].imageView=preset?preset:view;std::array<VkWriteDescriptorSet,tintBindings> writes{};
 for(uint32_t i=0;i<count;i++)writes[i]={VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,nullptr,set,i,0,1,VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,&images[i],nullptr,nullptr};
 tintUpdateDescriptorSets.load()(d,count,writes.data(),0,nullptr);
 saved={set,image.generation};return set;
}
struct TintDraw {
 VkPipelineLayout layout{};VkDescriptorSet original{},replacement{};
 unsigned bindings=0;std::array<VkBuffer,3> oldVertex{},newVertex{};std::array<VkDeviceSize,3> oldOffsets{};
 VkBuffer oldIndex{},newIndex{};VkDeviceSize oldIndexOffset=0;VkIndexType oldIndexType=VK_INDEX_TYPE_UINT16,newIndexType=VK_INDEX_TYPE_UINT16;
 ms::Submesh slot{};bool skip=false;
};
TintDraw tintDraw(VkCommandBuffer c,unsigned part,ms::vkcap::SkinHit shared={}){
 TintDraw result;tintObserve([&](auto& s){auto i=s.commands.find(c);if(i==s.commands.end())return;const auto& state=i->second.second;
  auto j=s.devices.find(i->second.first);if(j==s.devices.end())return;auto& d=j->second;
  auto p=d.pipelines.find(state.pipeline);if(p==d.pipelines.end()||p->second!=state.layout)return;
  auto l=d.pipelineLayouts.find(state.layout);if(l==d.pipelineLayouts.end())return;
  auto x=d.sets.find(state.set);if(x==d.sets.end()||x->second.shape!=l->second.shape||x->second.written!=(1u<<tintSlotCount(x->second.shape))-1)return;
  auto view=d.images.find(x->second.images[0].imageView);if(view==d.images.end())return;
  TintDraw plan;plan.layout=state.layout;plan.original=state.set;
  VkImageView preset{};
  if(shared){
   if(shared.profile>=sharedSkins.size()||shared.profile>=d.skin.profiles.size())return;
   const auto& profile=sharedSkins[shared.profile];auto& s=d.skin;const auto& buffers=s.profiles[shared.profile];
   if(!buffers.ready||shared.slot>=profile.mesh.slots.size()||!state.index||!state.vertex[0])return;
   auto layout=s.graphics.find(state.pipeline);if(layout==s.graphics.end())return;
   const auto& u=d.upload;auto family=d.families.find(state.pool);if(!u.queued||family==d.families.end()||family->second!=u.usedFamily)return;
   plan.bindings=layout->second;
   for(unsigned z=0;z<plan.bindings;z++)if(!state.vertex[z]||state.vertexOffset[z])return;
   auto observed=s.seenOutputs.find(state.vertex[0]);if(observed==s.seenOutputs.end()||observed->second.vertices!=profile.originalVertices||observed->second.bones!=profile.bones)return;
   auto source=std::make_pair(observed->second.rest,observed->second.weights);
   auto assoc=s.sourceProfiles.try_emplace(source,shared.profile);if(assoc.second)T().skinMatches.fetch_add(1,std::memory_order_relaxed);
   if(assoc.first->second!=shared.profile){assoc.first->second=~0u;return;}
   if(!T().skinActive(shared.profile))return;
   const auto currentKey=std::make_pair(state.vertex[0],shared.profile);auto current=s.sharedOutput.find(currentKey);
   if(current==s.sharedOutput.end()||(!current->second.queued&&!state.sharedProduced.count(currentKey)))return;
   plan.slot=profile.mesh.slots[shared.slot];if(!plan.slot.count){plan.skip=true;result=plan;return;}
   unsigned tex=profile.images[shared.slot];if(tex!=~0u){if(tex>=u.images.size()||!u.images[tex].view)return;preset=u.images[tex].view;}
   plan.oldIndex=state.index;plan.oldIndexOffset=state.indexOffset;plan.oldIndexType=state.indexType;
   plan.newIndex=buffers.input[Indices].buffer;plan.newIndexType=profile.mesh.indexBytes==4?VK_INDEX_TYPE_UINT32:VK_INDEX_TYPE_UINT16;
   plan.oldVertex=state.vertex;plan.oldOffsets=state.vertexOffset;
   plan.newVertex={current->second.buffer.buffer,buffers.input[Attributes].buffer,current->second.buffer.buffer};
   if(plan.bindings==3){auto previous=s.sharedOutput.find({state.vertex[2],shared.profile});if(previous!=s.sharedOutput.end()&&previous->second.queued)plan.newVertex[2]=previous->second.buffer.buffer;}
  }else if(!T().paimonTextureMod.empty()){
   const auto& u=d.upload;auto family=d.families.find(state.pool);
   if(!u.queued||family==d.families.end()||family->second!=u.usedFamily)return;
   preset=u.images[part].view;if(!preset)return;
  }
  if(T().skinnedModel&&!shared){
   const auto& s=d.skin;auto layout=s.graphics.find(state.pipeline);
   if(layout==s.graphics.end()||!s.ready||!state.index||state.indexOffset||state.indexType!=VK_INDEX_TYPE_UINT16)return;
   plan.bindings=layout->second;
   for(unsigned n=0;n<plan.bindings;n++)if(!state.vertex[n]||state.vertexOffset[n])return;
   auto current=s.output.find(state.vertex[0]);if(current==s.output.end()||(!current->second.queued&&!state.skinProduced.count(state.vertex[0])))return;
   plan.oldVertex=state.vertex;plan.oldOffsets=state.vertexOffset;plan.oldIndex=state.index;plan.newIndex=s.input[Indices].buffer;
   plan.newVertex={current->second.buffer.buffer,s.input[Attributes].buffer,current->second.buffer.buffer};
   if(plan.bindings==3){auto previous=s.output.find(state.vertex[2]);if(previous!=s.output.end()&&previous->second.queued)plan.newVertex[2]=previous->second.buffer.buffer;}
   plan.slot=paimonSkin.slots[part];
  }
  plan.replacement=shared&&!preset?state.set:tintClone(i->second.first,d,x->second,view->second,part,preset);result=plan;
 });return result;
}
#endif

VkResult onCreateBuffer(void* d,const VkBufferCreateInfo* info,const void* a,uint64_t* out){VkResult r=createBuffer.load()(d,info,a,out);if(r==0&&info&&out)T().created(*out,info->size,info->usage);return r;}
void onDestroyBuffer(void* d,uint64_t b,const void* a){T().destroyed(b);
#ifdef GIBOX_VK_TINT
 if(T().skinnedModel)skinForgetBuffer(static_cast<VkDevice>(d),reinterpret_cast<VkBuffer>(b));
#endif
 destroyBuffer.load()(d,b,a);
}
VkResult onBindBufferMemory(void* d,uint64_t b,uint64_t m,uint64_t o){VkResult r=bindBufferMemory.load()(d,b,m,o);if(r==0)T().bound(b,m,o);return r;}
VkResult bound2(VkResult r,uint32_t n,const VkBindBufferMemoryInfo* p){if(r==0&&p)for(uint32_t i=0;i<n;i++)T().bound(p[i].buffer,p[i].memory,p[i].memoryOffset);return r;}
VkResult onBindBufferMemory2(void* d,uint32_t n,const VkBindBufferMemoryInfo* p){return bound2(bindBufferMemory2.load()(d,n,p),n,p);}
VkResult onBindBufferMemory2KHR(void* d,uint32_t n,const VkBindBufferMemoryInfo* p){return bound2(bindBufferMemory2KHR.load()(d,n,p),n,p);}
VkResult onMapMemory(void* d,uint64_t m,uint64_t o,uint64_t size,uint32_t flags,void** pp){VkResult r=mapMemory.load()(d,m,o,size,flags,pp);if(r==0&&pp&&*pp)T().mapped(m,o,size,*pp);return r;}
void onUnmapMemory(void* d,uint64_t m){T().unmapped(m);unmapMemory.load()(d,m);}
void onFreeMemory(void* d,uint64_t m,const void* a){T().unmapped(m);freeMemory.load()(d,m,a);}
void onCmdCopyBuffer(void* c,uint64_t src,uint64_t dst,uint32_t n,const ms::vkcap::Copy* r){if(r)T().copied(src,dst,r,n);cmdCopyBuffer.load()(c,src,dst,n,r);}
void onCmdBindIndexBuffer(void* c,uint64_t b,uint64_t o,int32_t type){T().indexBound(b,o,reinterpret_cast<uintptr_t>(c),type);
#ifdef GIBOX_VK_TINT
 if(T().skinnedModel)skinBoundIndex(static_cast<VkCommandBuffer>(c),reinterpret_cast<VkBuffer>(b),o,VkIndexType(type));
#endif
 cmdBindIndexBuffer.load()(c,b,o,type);
}
VkResult onBeginCommandBuffer(void* c,const void* info){
 auto result=beginCommandBuffer.load()(c,info);
 if(result==0){T().beginCommands(reinterpret_cast<uintptr_t>(c));
#ifdef GIBOX_VK_TINT
  if(T().tintHooks)tintBegin(static_cast<VkCommandBuffer>(c));
#endif
 }
 return result;
}
void onCmdDrawIndexed(void* c,uint32_t count,uint32_t instances,uint32_t first,int32_t baseVertex,uint32_t firstInstance){
 [[maybe_unused]] int part=-1;[[maybe_unused]] ms::vkcap::SkinHit shared;
 try{part=T().drawIndexed(reinterpret_cast<uintptr_t>(c),count,instances,first,baseVertex,firstInstance,&shared);}catch(...){}
 auto draw=cmdDrawIndexed.load();
#ifdef GIBOX_VK_TINT
 if((part>=0||shared)&&T().tintHooks){const auto command=static_cast<VkCommandBuffer>(c);auto plan=tintDraw(command,shared?shared.slot:unsigned(part),shared);
  const bool active=shared?T().skinActive(shared.profile):T().tintOn.load(std::memory_order_relaxed);
  if(active&&plan.skip){T().sharedDraws.fetch_add(1,std::memory_order_relaxed);T().profileDraws[shared.profile].fetch_add(1,std::memory_order_relaxed);return;}
  if(plan.replacement&&active){auto bind=tintCmdBindDescriptorSets.load();
   bind(command,VK_PIPELINE_BIND_POINT_GRAPHICS,plan.layout,0,1,&plan.replacement,0,nullptr);
   if(plan.bindings){
    constexpr std::array<VkDeviceSize,3> zero{};auto vb=upCmdBindVertexBuffers.load();auto ib=cmdBindIndexBuffer.load();
    vb(command,0,plan.bindings,plan.newVertex.data(),zero.data());ib(c,reinterpret_cast<uintptr_t>(plan.newIndex),0,plan.newIndexType);
    draw(c,plan.slot.count,instances,plan.slot.start,0,firstInstance);
    ib(c,reinterpret_cast<uintptr_t>(plan.oldIndex),plan.oldIndexOffset,plan.oldIndexType);vb(command,0,plan.bindings,plan.oldVertex.data(),plan.oldOffsets.data());
   }else draw(c,count,instances,first,baseVertex,firstInstance);
   bind(command,VK_PIPELINE_BIND_POINT_GRAPHICS,plan.layout,0,1,&plan.original,0,nullptr);
   if(shared){T().sharedDraws.fetch_add(1,std::memory_order_relaxed);T().profileDraws[shared.profile].fetch_add(1,std::memory_order_relaxed);}
   else T().tintDraws[part].fetch_add(1,std::memory_order_relaxed);
   return;}
  if(shared){if(active)T().sharedMisses.fetch_add(1,std::memory_order_relaxed);}else T().tintMisses[part].fetch_add(1,std::memory_order_relaxed);
 }
#endif
 draw(c,count,instances,first,baseVertex,firstInstance);
}

// Each wrapper calls the loader's exported function of the same name (its trampoline), which dispatches through the
// handle's own table: right for any device the game makes, a probe device included. A name the loader does not export
// (vkBindBufferMemory2KHR on some loaders) is handed back unwrapped and counted.
template<class F>PFN wrap(std::atomic<F>& slot,PFN real,F replacement){if(!slot.load()){T().resolvedName(false);return real;}T().resolvedName(true);return reinterpret_cast<PFN>(replacement);}
PFN wrapped(const char* name,PFN real){if(!real||!name||!T().enabled)return real;std::string n=name;
 if(n=="vkCreateBuffer")return wrap(createBuffer,real,&onCreateBuffer);
 if(n=="vkDestroyBuffer")return wrap(destroyBuffer,real,&onDestroyBuffer);
 if(n=="vkBindBufferMemory")return wrap(bindBufferMemory,real,&onBindBufferMemory);
 if(n=="vkBindBufferMemory2")return wrap(bindBufferMemory2,real,&onBindBufferMemory2);
 if(n=="vkBindBufferMemory2KHR")return wrap(bindBufferMemory2KHR,real,&onBindBufferMemory2KHR);
 if(n=="vkMapMemory")return wrap(mapMemory,real,&onMapMemory);
 if(n=="vkUnmapMemory")return wrap(unmapMemory,real,&onUnmapMemory);
 if(n=="vkFreeMemory")return wrap(freeMemory,real,&onFreeMemory);
 if(n=="vkCmdCopyBuffer")return wrap(cmdCopyBuffer,real,&onCmdCopyBuffer);
 if(n=="vkCmdBindIndexBuffer")return wrap(cmdBindIndexBuffer,real,&onCmdBindIndexBuffer);
 if(T().probing()){
  if(n=="vkBeginCommandBuffer")return wrap(beginCommandBuffer,real,&onBeginCommandBuffer);
  if(n=="vkCmdDrawIndexed")return wrap(cmdDrawIndexed,real,&onCmdDrawIndexed);
 }
#ifdef GIBOX_VK_TINT
 if(T().tintHooks){
#define TINT_WRAP(F) if(n=="vk" #F)return wrap(tint##F,real,&tintOn##F);
  TINT_FUNCTIONS(TINT_WRAP)
  TINT_WRAP(UpdateDescriptorSetWithTemplate)
  TINT_WRAP(UpdateDescriptorSetWithTemplateKHR)
#undef TINT_WRAP
  if(!T().paimonTextureMod.empty()||T().allSkins){
#define UPLOAD_WRAP(F) if(n=="vk" #F)return wrap(up##F,real,&uploadOn##F);
   UPLOAD_HOOKS(UPLOAD_WRAP)
#undef UPLOAD_WRAP
   if(T().skinnedModel){
#define SKIN_WRAP(F) if(n=="vk" #F)return wrap(up##F,real,&skinOn##F);
    SKIN_HOOKS(SKIN_WRAP)
#undef SKIN_WRAP
   }
  }
 }
#endif
 // vkGetDeviceProcAddr looked up through vkGetInstanceProcAddr is the loader's internal function, not the exported stub:
 // ours instead, which goes through the stub's original target (the same lookup).
 if(n=="vkGetDeviceProcAddr"&&deviceProcOriginal){T().resolvedName(true);return reinterpret_cast<PFN>(&onGetDeviceProcAddr);}
 return real;}
PFN onGetInstanceProcAddr(void* instance,const char* name){return wrapped(name,instanceProcOriginal(instance,name));}
PFN onGetDeviceProcAddr(void* device,const char* name){return wrapped(name,deviceProcOriginal(device,name));}
}

namespace {
// ---------------------------------------------------------------- one-instruction redirect
constexpr uint32_t kB=0x14000000u,kLdrX17=0x58000051u /* ldr x17, #8 */,kBrX17=0xD61F0220u;
bool isB(uint32_t w){return (w&0xFC000000u)==kB;}
bool isLanding(uint32_t w){return w==0xD503245Fu||w==0xD503249Fu||w==0xD50324DFu||w==0xD503233Fu;}   // bti c / j / jc, paciasp
uintptr_t bTarget(uint32_t w,uintptr_t at){int64_t imm=int64_t(w&0x03FFFFFFu);if(imm&0x02000000){imm-=0x04000000;}return uintptr_t(int64_t(at)+imm*4);}
bool bTo(uintptr_t from,uintptr_t to,uint32_t& out){int64_t d=int64_t(to)-int64_t(from);if(d%4||d<-(int64_t(1)<<27)||d>=(int64_t(1)<<27))return false;out=kB|(uint32_t(d/4)&0x03FFFFFFu);return true;}
std::string instructionWords(uintptr_t at){char b[64];uint32_t w[3];std::memcpy(w,reinterpret_cast<const void*>(at),12);std::snprintf(b,sizeof b,"%08x %08x %08x",w[0],w[1],w[2]);return b;}
// A page of ours within reach of a direct branch from near (no PROT_BTI: anything may land in it).
uint32_t* islandNear(uintptr_t near){const size_t page=size_t(sysconf(_SC_PAGESIZE));
 for(int64_t mb=2;mb<=96;mb+=2)for(int sign:{-1,1}){uintptr_t hint=(uintptr_t(int64_t(near)+sign*mb*(int64_t(1)<<20)))&~uintptr_t(page-1);
   void* p=mmap(reinterpret_cast<void*>(hint),page,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);if(p==MAP_FAILED)continue;
   int64_t d=int64_t(reinterpret_cast<uintptr_t>(p))-int64_t(near);if(d>-(int64_t(120)<<20)&&d<(int64_t(120)<<20))return static_cast<uint32_t*>(p);munmap(p,page);}
 return nullptr;}
// The stub at `at`: [landing pad] b target. Fills island[0..4]: ldr x17, #8; br x17; .quad hook (where the stub's "b"
// will go), then "b target" (the original, returned). The page must be writable; nothing in the stub is written here:
// `site` and `jump` are the stub's branch and its new encoding, written by patch() once every entry is ready.
struct Redirect {uintptr_t site=0;uint32_t jump=0;void* original=nullptr;std::string what;};
bool prepare(uintptr_t at,void* hook,uint32_t* island,Redirect& r,std::string& why){uint32_t w0,w1;std::memcpy(&w0,reinterpret_cast<const void*>(at),4);std::memcpy(&w1,reinterpret_cast<const void*>(at+4),4);
 uintptr_t site=0;if(isB(w0))site=at;else if(isLanding(w0)&&isB(w1))site=at+4;const std::string before=instructionWords(at);
 if(!site){why="不是 [bti] + b 形式的入口，开头为 "+before;return false;}uint32_t insn;std::memcpy(&insn,reinterpret_cast<const void*>(site),4);uintptr_t target=bTarget(insn,site);
 uint32_t back,jump;if(!bTo(reinterpret_cast<uintptr_t>(island+4),target,back)||!bTo(site,reinterpret_cast<uintptr_t>(island),jump)){why="GIBox 的页超出跳转范围";return false;}
 island[0]=kLdrX17;island[1]=kBrX17;uint64_t h=reinterpret_cast<uint64_t>(hook);std::memcpy(island+2,&h,8);island[4]=back;
 char b[128];std::snprintf(b,sizeof b,"入口 %s，+%u 处的 b 改经 0x%lx",before.c_str(),unsigned(site-at),static_cast<unsigned long>(reinterpret_cast<uintptr_t>(island)));
 r.site=site;r.jump=jump;r.original=island+4;r.what=b;return true;}
// Our page executable, then each stub's branch rewritten (4 bytes, one aligned store).
bool patch(uint32_t* island,std::initializer_list<Redirect*> rs,std::string& why){const size_t page=size_t(sysconf(_SC_PAGESIZE));void* p=reinterpret_cast<void*>(reinterpret_cast<uintptr_t>(island)&~uintptr_t(page-1));
 if(mprotect(p,page,PROT_READ|PROT_EXEC)!=0){why="GIBox 页的 mprotect 失败";return false;}__builtin___clear_cache(static_cast<char*>(p),static_cast<char*>(p)+page);
 for(auto* r:rs){if(DobbyCodePatch(reinterpret_cast<void*>(r->site),reinterpret_cast<uint8_t*>(&r->jump),4)!=0){why="写入入口跳转失败";return false;}}return true;}
}

namespace ms::vkhook {
std::vector<std::string> prepareVulkanSkins(const std::string& shared,const std::vector<std::string>& selected,std::vector<uint32_t>& probes){
 std::vector<std::string> notes;
#ifdef GIBOX_VK_TINT
 struct Offline {
  std::ifstream in;std::vector<std::streampos> offsets;
  std::map<std::string,std::vector<std::pair<std::string,size_t>>> owners;
  Offline(const std::string& path,const std::set<std::string>& wanted):in(path){
   std::string line;require(std::getline(in,line)&&line.find("\"format\":\"gibox-offline-skeletons\"")!=line.npos,"离线骨骼表不可读");
   auto at=in.tellg();while(std::getline(in,line)&&line!="],\"prefabs\":["){offsets.push_back(at);at=in.tellg();}
   require(!offsets.empty(),"离线骨骼表没有网格");
   while(std::getline(in,line)&&line!="]}"){
    if(line.empty())continue;
    if(line.back()==',')line.pop_back();
    auto q=JsonParser(line).parse();const auto owner=pcdb::lower(q.at("owner").string);if(!wanted.count(owner))continue;
    auto& out=owners[owner];for(const auto& r:q.at("renderers").array)if(r.type==Json::Array&&r.array.size()==2)out.emplace_back(r.array[0].string,size_t(jnum(r.array[1],0,offsets.size()-1,"离线骨骼索引")));
   }
  }
  Json read(size_t i){std::string line;in.clear();in.seekg(offsets.at(i));require(bool(std::getline(in,line)),"离线骨骼表无法读取网格");if(!line.empty()&&line.back()==',')line.pop_back();return JsonParser(line).parse();}
 };
 auto mapFile=[](const std::string& path,size_t limit){auto bytes=read(path,limit);return pcdb::parseMap(std::string(bytes.begin(),bytes.end()));};
 auto table=mapFile(shared+"/device-hashes.json",32u<<20);pcdb::DeviceMap local;
 if(exists(shared+"/hash-map.json"))try{local=mapFile(shared+"/hash-map.json",16u<<20);}catch(const std::exception&){}
 // Bundled hashes are preferred when both tables contain a record for the same hash.
 table.base=&local;
 struct Candidate {std::string name,dir;ModelSummary summary;std::vector<std::vector<pcdb::Mesh>> refs;};
 std::vector<Candidate> candidates;std::set<std::string> rigOwners;
 for(const auto& name:selected){
  if(name.empty()||name=="."||name==".."||name.find_first_of("/\\")!=name.npos)continue;
  const auto dir=shared+"/models/"+name;if(!exists(dir+"/model.json"))continue;
  try{Candidate x{name,dir,readSummary(dir),{}};if(x.summary.animated)throw std::runtime_error("自定义逐帧网格暂未接入");
   for(const auto& part:x.summary.parts){
    const auto& t=part.target;require(part.hasTarget&&t.bones&&t.submeshes<=model::maxSlot,"没有带骨骼的完整网格指纹");
    std::set<std::string> owners;
    if(x.summary.id==1){for(const auto& st:x.summary.sceneTargets)if(matches(st.target,t))owners.insert(st.owners.begin(),st.owners.end());}
    else if(!x.summary.outfit.empty())owners.insert(x.summary.outfit);
    std::vector<pcdb::Mesh> list;table.each([&](const auto& p){const auto owner=pcdb::lower(p.avatar.empty()?p.owner:p.avatar);
     if(owner.empty()||(!owners.empty()&&!owners.count(owner))||p.variant.rfind("as uploaded,",0)!=0||meshName(p.mesh)!=meshName(t.mesh)||
        p.vertices!=t.vertices||p.subs.size()!=t.submeshes||(p.bones&&p.bones!=t.bones))return;
     if(x.summary.id!=1&&p.avatar.empty())return;
     list.push_back(p);});
    require(!list.empty(),"设备哈希表没有 "+t.mesh+" 的布局");
    const auto& base=list.front();for(auto& p:list){require(p.subs==base.subs&&pcdb::lower(p.avatar.empty()?p.owner:p.avatar)==pcdb::lower(base.avatar.empty()?base.owner:base.avatar),"哈希对应多个原网格布局或归属");}
    rigOwners.insert(pcdb::lower(base.avatar.empty()?base.owner:base.avatar));x.refs.push_back(std::move(list));
   }
   candidates.push_back(std::move(x));
  }catch(const std::exception& e){notes.push_back(name+"："+e.what());}}
 if(candidates.empty())return notes;
 Offline rigs(shared+"/offline-skeletons.json",rigOwners);
 size_t bytes=0;constexpr size_t maxPreload=192u<<20;
 for(auto& c:candidates)try{
  auto m=loadModelFile(c.dir,false,true,-1,c.summary.generation);
  std::vector<SharedSkin> pending;std::vector<std::shared_ptr<Texture>> pixels;
  std::vector<vkcap::SkinRoute> routes;
  size_t extra=0;
  for(size_t i=0;i<m->parts.size();i++){
   const auto& part=m->parts[i];const auto& t=c.summary.parts[i].target;const auto& p=c.refs.at(i).front();
   const auto owner=pcdb::lower(p.avatar.empty()?p.owner:p.avatar);auto matchesOwner=rigs.owners.find(owner);
   require(matchesOwner!=rigs.owners.end(),"离线骨骼表没有 "+owner);
   std::vector<int> mapping;unsigned matchCount=0;
   for(const auto& row:matchesOwner->second){if(meshName(row.first)!=meshName(t.mesh)&&normalize(row.first)!=normalize(t.renderer))continue;
    const auto q=rigs.read(row.second);const auto& bones=q.at("bones").array;const auto& binds=q.at("bindposes").array;
    if(meshName(q.at("mesh").string)!=meshName(t.mesh)||q.at("vertices").number!=t.vertices||q.at("submeshes").number!=t.submeshes||bones.size()!=t.bones||binds.size()!=t.bones*16u)continue;
    try{std::vector<std::string> names;std::vector<int> parents;Bytes tb(binds.size()*4);
     for(const auto& b:bones)names.push_back(b.string);
     for(const auto& pa:q.at("parents").array){const int j=jint(pa,"离线骨骼父节点");require(j>=-1&&j<int(bones.size()),"离线骨骼父节点无效");parents.push_back(j);}
     for(size_t j=0;j<binds.size();j++){float v=float(binds[j].number);require(binds[j].type==Json::Number&&std::isfinite(v),"离线矩阵无效");std::memcpy(tb.data()+4*j,&v,4);}
     auto map=mapBones(part.sourceBones,names,part.sourceBinds,tb,part.used,part.sourceParents,parents).target;
     require(matchCount==0||map==mapping,"骨骼映射有歧义");mapping=std::move(map);++matchCount;
    }catch(const std::exception&){if(matchCount)throw;}}
   require(matchCount>0,"离线骨骼没有验证过 "+owner+" / "+t.mesh);
   SharedSkin skin;skin.originalVertices=t.vertices;skin.bones=t.bones;skin.mesh=packVulkanSkin(part,m->variants.defaults,t.submeshes,mapping);
   extra+=skin.mesh.rest.size()+skin.mesh.weights.size()+skin.mesh.attributes.size()+skin.mesh.indices.size();
   skin.images.resize(t.submeshes,~0u);std::vector<Submesh> active;std::vector<Material> materials;activeDraws(part.draws,m->variants.defaults,active,materials);
   std::map<unsigned,std::string> slots;for(size_t j=0;j<materials.size();j++)if(active[j].count){auto& n=slots[materials[j].slot];require(n.empty()||n==materials[j].name,"同一槽位存在不同材质");n=materials[j].name;}
   std::map<std::string,MaterialFile> request;std::set<std::string> names;
   for(const auto& slot:slots){const auto mt=m->materials.find(slot.second);require(slot.first<t.submeshes&&mt!=m->materials.end(),"材质槽无效");
    const auto& ref=mt->second.mainRef;if(!ref.present())continue;
    require(ref.w<=4096&&ref.h<=4096,"主贴图超出 Vulkan 预加载上限");MaterialFile f;f.original=false;f.mainRef=ref;request[slot.second]=f;names.insert(slot.second);}
   readTextures(c.dir,m->generation,m->blobSize,request,names);
   for(const auto& slot:slots){const auto entry=request.find(slot.second);if(entry==request.end())continue;
    const auto tex=entry->second.main;require(bool(tex),"无法读取主贴图");
    skin.images[slot.first]=unsigned(sharedPixels.size()+pixels.size());extra+=tex->pixels.size();pixels.push_back(tex);}
   for(const auto& v:c.refs[i]){auto hash=vkcap::drawHash(v.ib);require(bool(hash),"哈希无效");
    require(!T().skinRoutes.count(*hash)&&std::none_of(routes.begin(),routes.end(),[&](const auto& r){return r.hash==*hash;}),"同一原网格已有优先 MOD");
    const auto total=pcdb::indices(v.subs);require(total&&total<=model::maxIndices,"原网格索引超限");
    routes.push_back({*hash,uint32_t(total),unsigned(sharedSkins.size()+pending.size()),v.subs});}
   pending.push_back(std::move(skin));
  }
  require(!pending.empty()&&sharedSkins.size()+pending.size()<=64&&extra<=maxPreload-bytes,"超过 Vulkan 预加载预算");
  size_t newHashes=0;for(const auto& r:routes)newHashes+=std::find(probes.begin(),probes.end(),r.hash)==probes.end();
  require(probes.size()+newHashes<=vkcap::Tracker::maxDrawHashes,"超过 Vulkan 索引探针上限");
  bytes+=extra;for(auto& r:routes){if(std::find(probes.begin(),probes.end(),r.hash)==probes.end())probes.push_back(r.hash);T().skinRoutes.emplace(r.hash,std::move(r));}
  for(auto& p:pending){sharedSkins.push_back(std::move(p));T().skinMods.push_back(c.name);}
  sharedPixels.insert(sharedPixels.end(),pixels.begin(),pixels.end());
  notes.push_back(c.name+"：Vulkan 就绪，"+std::to_string(pending.size())+" 个部件，等待游戏绘制确认");
 }catch(const std::exception& e){notes.push_back(c.name+"："+e.what());}
#else
 (void)shared;(void)selected;(void)probes;
#endif
 return notes;
}
void preparePaimonTextures(const std::string& modelDir,bool skin){
#ifdef GIBOX_VK_TINT
 const auto summary=readSummary(modelDir);
 require(summary.id==1&&summary.parts.size()==1&&std::any_of(summary.sceneTargets.begin(),summary.sceneTargets.end(),[](const auto& target){
  return target.target.mesh=="Body"&&target.target.vertices==8397&&target.target.submeshes==2&&
   std::find(target.owners.begin(),target.owners.end(),"npc_kanban_paimon")!=target.owners.end();}),"需要与原版派蒙 Body 匹配的已转换场景 MOD");
 const auto m=loadModelFile(modelDir,false,true,-1,summary.generation);
 std::array<std::string,2> material;
 for(const auto& draw:m->parts[0].draws)if(draw.row.slot<material.size()){
  auto& name=material[draw.row.slot];require(name.empty()||name==draw.row.name,"派蒙槽位包含多种材质，无法预加载单一贴图");name=draw.row.name;
 }
 std::map<std::string,MaterialFile> inputs;std::set<std::string> wanted;
 for(const auto& name:material){const auto i=m->materials.find(name);require(i!=m->materials.end(),"派蒙缺少槽 0 或槽 1 的材质");
  const auto& ref=i->second.mainRef;require(ref.present()&&ref.w<=2048&&ref.h<=2048,"派蒙主贴图缺失或超过 2048×2048");
  MaterialFile x;x.original=false;x.mainRef=ref;inputs[name]=x;wanted.insert(name);
 }
 readTextures(modelDir,m->generation,m->blobSize,inputs,wanted);
 if(skin){require(!m->animation.present(),"Vulkan 派蒙：暂不接管 MOD 自定义逐帧网格");paimonSkin=packPaimonSkin(m->parts[0],m->variants.defaults);}
 for(size_t i=0;i<material.size();i++)paimonPixels[i]=inputs.at(material[i]).main;
#else
 (void)modelDir;(void)skin;
#endif
}
// Hooks the loader's two lookup functions; the result and the reason when it cannot are kept in the tracker's stats.
bool install(std::string& detail){
 void* lib=dlopen("libvulkan.so",RTLD_NOW);if(!lib){detail="无法加载 libvulkan.so";vkcap::tracker().installed(false,detail);return false;}
 void* gipa=dlsym(lib,"vkGetInstanceProcAddr");void* gdpa=dlsym(lib,"vkGetDeviceProcAddr");
 if(!gipa||!gdpa){detail="libvulkan.so 没有导出 vkGetInstanceProcAddr / vkGetDeviceProcAddr";vkcap::tracker().installed(false,detail);return false;}
 // The loader's trampolines the wrappers call (see wrap).
 auto get=[lib](auto& slot,const char* name){slot.store(reinterpret_cast<typename std::remove_reference_t<decltype(slot)>::value_type>(dlsym(lib,name)));};
 get(createBuffer,"vkCreateBuffer");get(destroyBuffer,"vkDestroyBuffer");get(bindBufferMemory,"vkBindBufferMemory");get(bindBufferMemory2,"vkBindBufferMemory2");get(bindBufferMemory2KHR,"vkBindBufferMemory2KHR");
 get(mapMemory,"vkMapMemory");get(unmapMemory,"vkUnmapMemory");get(freeMemory,"vkFreeMemory");get(cmdCopyBuffer,"vkCmdCopyBuffer");get(cmdBindIndexBuffer,"vkCmdBindIndexBuffer");
 get(beginCommandBuffer,"vkBeginCommandBuffer");get(cmdDrawIndexed,"vkCmdDrawIndexed");
 if(!createBuffer.load()||!bindBufferMemory.load()||!mapMemory.load()||!cmdCopyBuffer.load()||!cmdBindIndexBuffer.load()){detail="libvulkan.so 没有导出缓冲相关函数";vkcap::tracker().installed(false,detail);return false;}
 if(vkcap::tracker().probing()&&(!beginCommandBuffer.load()||!cmdDrawIndexed.load())){detail="libvulkan.so 没有导出绘制探针入口";vkcap::tracker().installed(false,detail);return false;}
#ifdef GIBOX_VK_TINT
 if(vkcap::tracker().tintHooks){
#define TINT_LOAD(F) get(tint##F,"vk" #F);if(!tint##F.load()){detail="派蒙试验缺少 vk" #F;vkcap::tracker().installed(false,detail);return false;}
  TINT_FUNCTIONS(TINT_LOAD)
  TINT_LOAD(CreateDescriptorPool)
#undef TINT_LOAD
  get(tintUpdateDescriptorSetWithTemplate,"vkUpdateDescriptorSetWithTemplate");
  get(tintUpdateDescriptorSetWithTemplateKHR,"vkUpdateDescriptorSetWithTemplateKHR");
  if(!vkcap::tracker().paimonTextureMod.empty()||vkcap::tracker().allSkins){
#define UPLOAD_LOAD(F) get(up##F,"vk" #F);if(!up##F.load()){detail="派蒙贴图上传缺少 vk" #F;vkcap::tracker().installed(false,detail);return false;}
   UPLOAD_API(UPLOAD_LOAD)
   UPLOAD_HOOKS(UPLOAD_LOAD)
   if(vkcap::tracker().skinnedModel){SKIN_HOOKS(UPLOAD_LOAD)}
#undef UPLOAD_LOAD
  }
 }
#undef TINT_FUNCTIONS
#undef UPLOAD_API
#undef UPLOAD_HOOKS
#undef SKIN_HOOKS
#endif
 // Both stubs are checked before either is written: one of another shape leaves libvulkan.so untouched.
 auto shape=[](void* at){uint32_t w0,w1;std::memcpy(&w0,at,4);std::memcpy(&w1,static_cast<const char*>(at)+4,4);return isB(w0)||(isLanding(w0)&&isB(w1));};
 if(!shape(gipa)||!shape(gdpa)){detail="libvulkan.so 的入口不是 [bti] + b 形式（vkGetInstanceProcAddr "+instructionWords(reinterpret_cast<uintptr_t>(gipa))+"，vkGetDeviceProcAddr "+instructionWords(reinterpret_cast<uintptr_t>(gdpa))+"），未写入";vkcap::tracker().installed(false,detail);return false;}
 uint32_t* island=islandNear(reinterpret_cast<uintptr_t>(gipa));if(!island){detail="在 libvulkan.so 的跳转范围内没有可用的 GIBox 页，未写入";vkcap::tracker().installed(false,detail);return false;}
 Redirect gi,gd;std::string why;
 if(!prepare(reinterpret_cast<uintptr_t>(gipa),reinterpret_cast<void*>(&onGetInstanceProcAddr),island,gi,why)||!prepare(reinterpret_cast<uintptr_t>(gdpa),reinterpret_cast<void*>(&onGetDeviceProcAddr),island+8,gd,why)){
  detail=why+"，未写入";vkcap::tracker().installed(false,detail);return false;}
 instanceProcOriginal=reinterpret_cast<GetProc>(gi.original);deviceProcOriginal=reinterpret_cast<GetProc>(gd.original);
 vkcap::tracker().installed(true,"");   // before the stubs change: a lookup through them already wraps
 if(!patch(island,{&gi,&gd},why)){detail=why;vkcap::tracker().installed(false,detail);return false;}
 detail="vkGetInstanceProcAddr："+gi.what+"；vkGetDeviceProcAddr："+gd.what+"；包装函数调用加载器自己的导出";return true;}
}
