// The Vulkan side of the index-buffer hash diagnostic (vk_capture.hpp), off by default. Two exported entry points of
// the system loader libvulkan.so are hooked, vkGetInstanceProcAddr and vkGetDeviceProcAddr: when the game asks for one
// of the buffer functions below, it gets a wrapper that tells the tracker what happened and then calls the loader's
// exported function of that name with the same arguments. No call is skipped or changed; the wrappers only read. Installed at the module's start, before
// the game creates its Vulkan device; a function the game resolved earlier is simply not seen.
#include "vk_capture.hpp"
#include "dobby.h"
#include <atomic>
#include <cstddef>
#include <dlfcn.h>
#include <string>
#include <type_traits>

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

GetProc instanceProcOriginal=nullptr,deviceProcOriginal=nullptr;
std::atomic<CreateBuffer> createBuffer{nullptr};std::atomic<DestroyBuffer> destroyBuffer{nullptr};
std::atomic<BindBufferMemory> bindBufferMemory{nullptr};std::atomic<BindBufferMemory2> bindBufferMemory2{nullptr},bindBufferMemory2KHR{nullptr};
std::atomic<MapMemory> mapMemory{nullptr};std::atomic<UnmapMemory> unmapMemory{nullptr};std::atomic<FreeMemory> freeMemory{nullptr};
std::atomic<CmdCopyBuffer> cmdCopyBuffer{nullptr};std::atomic<CmdBindIndexBuffer> cmdBindIndexBuffer{nullptr};
ms::vkcap::Tracker& T(){return ms::vkcap::tracker();}

VkResult onCreateBuffer(void* d,const VkBufferCreateInfo* info,const void* a,uint64_t* out){VkResult r=createBuffer.load()(d,info,a,out);if(r==0&&info&&out)T().created(*out,info->size,info->usage);return r;}
void onDestroyBuffer(void* d,uint64_t b,const void* a){T().destroyed(b);destroyBuffer.load()(d,b,a);}
VkResult onBindBufferMemory(void* d,uint64_t b,uint64_t m,uint64_t o){VkResult r=bindBufferMemory.load()(d,b,m,o);if(r==0)T().bound(b,m,o);return r;}
VkResult bound2(VkResult r,uint32_t n,const VkBindBufferMemoryInfo* p){if(r==0&&p)for(uint32_t i=0;i<n;i++)T().bound(p[i].buffer,p[i].memory,p[i].memoryOffset);return r;}
VkResult onBindBufferMemory2(void* d,uint32_t n,const VkBindBufferMemoryInfo* p){return bound2(bindBufferMemory2.load()(d,n,p),n,p);}
VkResult onBindBufferMemory2KHR(void* d,uint32_t n,const VkBindBufferMemoryInfo* p){return bound2(bindBufferMemory2KHR.load()(d,n,p),n,p);}
VkResult onMapMemory(void* d,uint64_t m,uint64_t o,uint64_t size,uint32_t flags,void** pp){VkResult r=mapMemory.load()(d,m,o,size,flags,pp);if(r==0&&pp&&*pp)T().mapped(m,o,size,*pp);return r;}
void onUnmapMemory(void* d,uint64_t m){T().unmapped(m);unmapMemory.load()(d,m);}
void onFreeMemory(void* d,uint64_t m,const void* a){T().unmapped(m);freeMemory.load()(d,m,a);}
void onCmdCopyBuffer(void* c,uint64_t src,uint64_t dst,uint32_t n,const ms::vkcap::Copy* r){if(r)T().copied(src,dst,r,n);cmdCopyBuffer.load()(c,src,dst,n,r);}
void onCmdBindIndexBuffer(void* c,uint64_t b,uint64_t o,int32_t type){T().indexBound(b,o);cmdBindIndexBuffer.load()(c,b,o,type);}

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
 return real;}
PFN onGetInstanceProcAddr(void* instance,const char* name){return wrapped(name,instanceProcOriginal(instance,name));}
PFN onGetDeviceProcAddr(void* device,const char* name){return wrapped(name,deviceProcOriginal(device,name));}
}

namespace ms::vkhook {
// Hooks the loader's two lookup functions; the result and the reason when it cannot are kept in the tracker's stats.
bool install(std::string& detail){
 void* lib=dlopen("libvulkan.so",RTLD_NOW);if(!lib){detail="libvulkan.so not loadable";vkcap::tracker().installed(false,detail);return false;}
 void* gipa=dlsym(lib,"vkGetInstanceProcAddr");void* gdpa=dlsym(lib,"vkGetDeviceProcAddr");
 if(!gipa||!gdpa){detail="libvulkan.so exports no vkGetInstanceProcAddr / vkGetDeviceProcAddr";vkcap::tracker().installed(false,detail);return false;}
 // The loader's trampolines the wrappers call (see wrap).
 auto get=[lib](auto& slot,const char* name){slot.store(reinterpret_cast<typename std::remove_reference_t<decltype(slot)>::value_type>(dlsym(lib,name)));};
 get(createBuffer,"vkCreateBuffer");get(destroyBuffer,"vkDestroyBuffer");get(bindBufferMemory,"vkBindBufferMemory");get(bindBufferMemory2,"vkBindBufferMemory2");get(bindBufferMemory2KHR,"vkBindBufferMemory2KHR");
 get(mapMemory,"vkMapMemory");get(unmapMemory,"vkUnmapMemory");get(freeMemory,"vkFreeMemory");get(cmdCopyBuffer,"vkCmdCopyBuffer");get(cmdBindIndexBuffer,"vkCmdBindIndexBuffer");
 if(!createBuffer.load()||!bindBufferMemory.load()||!mapMemory.load()||!cmdCopyBuffer.load()||!cmdBindIndexBuffer.load()){detail="libvulkan.so does not export the buffer functions";vkcap::tracker().installed(false,detail);return false;}
 vkcap::tracker().installed(true,"");   // before the hooks: a lookup through them already wraps
 if(DobbyHook(gipa,reinterpret_cast<dobby_dummy_func_t>(&onGetInstanceProcAddr),reinterpret_cast<dobby_dummy_func_t*>(&instanceProcOriginal))!=0||!instanceProcOriginal){detail="DobbyHook vkGetInstanceProcAddr failed";vkcap::tracker().installed(false,detail);return false;}
 if(DobbyHook(gdpa,reinterpret_cast<dobby_dummy_func_t>(&onGetDeviceProcAddr),reinterpret_cast<dobby_dummy_func_t*>(&deviceProcOriginal))!=0||!deviceProcOriginal){detail="DobbyHook vkGetDeviceProcAddr failed (vkGetInstanceProcAddr stays hooked and wraps)";return true;}
 detail="vkGetInstanceProcAddr and vkGetDeviceProcAddr in libvulkan.so; wrappers call the loader's own exports";return true;}
}
