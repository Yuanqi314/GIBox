// Index-buffer hashes as 3DMigoto computes them (vk_capture.hpp) and the tracker that finds index bytes in mapped memory.
#include "vk_capture.hpp"
#include <cassert>
#include <array>
#include <cstdio>
#include <iostream>
using namespace ms::vkcap;
int main(){
 {Tracker backend;backend.enabled=true;backend.allSkins=true;backend.skinMods={"Model"};
  assert(backend.hasSkin("Model")&&!backend.hasSkin("Other")&&!backend.skinBackendReady());
  backend.drawIndexed(1,0,1,0,0,0);backend.textureUpload=1;
  assert(!backend.skinBackendReady());
  backend.textureUpload=0;backend.drawIndexed(1,3,1,0,0,0);
  assert(!backend.skinBackendReady());
  backend.textureUpload=1;
  assert(backend.skinBackendReady());
  backend.textureUpload=-1;
  assert(!backend.skinBackendReady());
 }
 // CRC-32C check value, and crc32c_append: a CRC continued over a second part equals the CRC of both.
 assert(crc32c(0,"123456789",9)==0xE3069283u&&crc32c(0,"",0)==0);
 assert(crc32c(crc32c(0,"1234",4),"56789",5)==0xE3069283u);
 // HackerDevice::CreateBuffer: data hash, then the 24-byte desc appended (DEFAULT + IB, DEFAULT + IB|SRV|UAV, IMMUTABLE + IB).
 uint16_t idx[6]={0,1,2,2,1,3};auto hs=ibHashes(reinterpret_cast<const uint8_t*>(idx),sizeof idx);
 {Tracker early;early.allSkins=true;early.allForms=false;early.probeOnly=true;early.routesReady=false;
  early.created(19,sizeof idx,0x40);early.bound(19,19,0);early.mapped(19,0,sizeof idx,idx);early.indexBound(19,0,1,0);
  SkinHit before,after;
  early.drawIndexed(1,3,1,3,0,0,&before);assert(!before);
  early.iniSkinRules[hs[2].value]={{0,1,3,3}};early.setDrawHashes({hs[2].value});
  assert(early.skinIndexUploads.exchange(0)==1);
  early.drawIndexed(1,3,1,3,0,0,&after);
  assert(after&&after.profile==0&&after.slot==1&&early.drawStats().front().uploads==1);
 }
 uint32_t data=crc32c(0,idx,sizeof idx);D3D11BufferDesc d;d.byteWidth=sizeof idx;d.bindFlags=2;
 assert(hs[0].variant==0&&hs[0].value==data&&hs[1].variant==1&&hs[1].value==crc32c(data,&d,sizeof d));
 d.bindFlags=0x8A;assert(hs[2].value==crc32c(data,&d,sizeof d));d.bindFlags=2;d.usage=1;assert(hs[3].value==crc32c(data,&d,sizeof d));
 // Device evidence (0.7.57 capture, ib-hashes.txt): Linnea's Body index data (204924 bytes, data hash 8ee07611) gives the
 // GIMI rule hash e282f691 with Bind IB|SRV|UAV; the Traveler's Body (87822 bytes, 72b6b481) gives dfb54407 with Bind IB.
 {D3D11BufferDesc l;l.byteWidth=204924;l.bindFlags=0x8A;D3D11BufferDesc t;t.byteWidth=87822;t.bindFlags=0x2;
  assert(crc32c(0x8ee07611u,&l,sizeof l)==0xe282f691u&&crc32c(0x72b6b481u,&t,sizeof t)==0xdfb54407u);}
 // Widened to 32 bit (format 1), and the 12 bytes read as 3 32-bit values narrowed back (format 2, all fit in 16 bit).
 uint32_t wide[6]={0,1,2,2,1,3};D3D11BufferDesc w;w.byteWidth=sizeof wide;w.bindFlags=2;
 assert(hs[4].variant==4&&hs[4].value==crc32c(0,wide,sizeof wide)&&hs[5].value==crc32c(crc32c(0,wide,sizeof wide),&w,sizeof w));
 assert(hs.size()==8&&variantName(5)=="16-bit widened to 32-bit, desc DEFAULT, Bind=IB"&&variantName(2)=="as uploaded, desc DEFAULT, Bind=IB|SRV|UAV");   // as 32-bit these values exceed 16 bit
 auto hw=ibHashes(reinterpret_cast<const uint8_t*>(wide),sizeof wide);   // 32-bit data: as uploaded, widened, narrowed back
 assert(hw.size()==12&&hw[8].variant==8&&hw[8].value==hs[0].value&&hw[9].value==hs[1].value&&hw[0].value==hs[4].value);
 uint32_t big[2]={70000,1};assert(ibHashes(reinterpret_cast<const uint8_t*>(big),8).size()==8);   // a value over 16 bit: no narrowing
 // Staging copy: the source bytes read when the copy is recorded; one region beyond the source is not read.
 Tracker t;uint8_t staging[32]={};std::memcpy(staging+8,idx,sizeof idx);
 t.created(1,12,0x40);t.created(2,32,0x1);t.bound(2,9,0);t.mapped(9,0,32,staging);
 Copy c[2]={{8,0,12},{24,0,12}};t.copied(2,1,c,2);auto blobs=t.take();auto st=t.stats();
 assert(blobs.size()==1&&blobs[0].bytes==12&&blobs[0].via=="staging copy"&&blobs[0].hashes[1].value==hs[1].value&&blobs[0].max16==3&&blobs[0].head16.size()==6&&st.copies==2&&st.outOfRange==1);
 // A copy whose source memory is not mapped is counted, not read; a copy into a buffer that is not an index buffer is ignored.
 t.created(3,32,0x1);t.bound(3,10,0);t.copied(3,1,c,1);t.created(4,12,0x80);t.copied(2,4,c,1);assert(t.take().empty()&&t.stats().notMapped==1&&t.stats().copies==3);
 // An index buffer in mapped memory: read once at its first bind, from the bind offset to its end; the memory offset counts.
 uint8_t host[64]={};std::memcpy(host+16+4,idx,sizeof idx);t.created(5,16,0x40);t.bound(5,11,16);t.mapped(11,0,64,host);
 t.indexBound(5,4);t.indexBound(5,4);blobs=t.take();assert(blobs.size()==1&&blobs[0].bytes==12&&blobs[0].hashes[1].value==hs[1].value&&t.stats().mappedBinds==1);
 // A filled-by-copy index buffer is not read again at its bind; an unmapped memory is no longer read.
 t.indexBound(1,0);t.unmapped(11);t.created(6,16,0x40);t.bound(6,11,16);t.indexBound(6,0);assert(t.take().empty()&&t.stats().notMapped==2);
 // Modes (1.0.3): hashMode first, else the old switches; match / collect hash the data as uploaded only (4 values, the
 // same as the full set's first four).
 assert(modeFrom("full",false,false)==Mode::Full&&modeFrom("off",true,true)==Mode::Off&&modeFrom("",false,true)==Mode::Collect&&modeFrom("",true,false)==Mode::Match&&modeFrom("x",false,false)==Mode::Off);
 {Tracker one;one.allForms=false;uint8_t h2[12];std::memcpy(h2,idx,12);one.created(7,12,0x40);one.bound(7,12,0);one.mapped(12,0,12,h2);one.indexBound(7,0);auto b=one.take();
  assert(b.size()==1&&b[0].hashes.size()==4);for(size_t i=0;i<4;i++)assert(b[0].hashes[i].value==hs[i].value&&b[0].hashes[i].variant==hs[i].variant);}
 assert(drawHash("5B4cDC9c")==0x5b4cdc9cu&&drawHash("00000000")==0u&&!drawHash("0xfedcba")&&!drawHash("-1234567"));
 assert(paimonTintPart(paimonBodyHash,61224,0,26226,2,1,0,0)==0);
 assert(paimonTintPart(paimonBodyHash,61224,26226,4386,2,1,0,0)==1);
 assert(paimonTintPart(paimonBodyHash,61224,0,4386,2,1,0,0)<0&&paimonTintPart(paimonBodyHash,61224,26226,4386,4,1,0,0)<0);
 assert(paimonTintPart(paimonBodyHash,61224,26226,4386,2,2,0,0)<0&&paimonTintPart(paimonBodyHash,61224,26226,4386,2,1,1,0)<0);
 {uint32_t spirv[]={0x07230203,0x10000,0,8,0,(5u<<16)|5u,2,0x69614d5f,0x7865546e,0,(4u<<16)|71u,2,34,0,(4u<<16)|71u,2,33,0};
  constexpr size_t words=sizeof spirv/sizeof spirv[0];
  assert(mainTextureBinding(spirv,words)&&!mainTextureBinding(spirv,words-1));
  spirv[words-1]=1;assert(!mainTextureBinding(spirv,words));
  spirv[words-1]=0;spirv[15]=3;assert(!mainTextureBinding(spirv,words));}
 {std::vector<uint32_t> shader{0x07230203u,0x10000,0,8,0};
  auto texture=[&](const char* name,uint32_t id,uint32_t set,uint32_t slot){
   const size_t length=std::strlen(name)+1,words=(length+3)/4;
   shader.insert(shader.end(),{uint32_t((words+2)<<16)|5u,id});
   auto at=shader.size();shader.resize(at+words);std::memcpy(shader.data()+at,name,length);
   shader.insert(shader.end(),{(4u<<16)|71u,id,34,set,(4u<<16)|71u,id,33,slot});
  };
  texture("_LightMapTex",2,0,1);
  assert((materialTextureBindings(shader.data(),shader.size())==std::array<unsigned,3>{~0u,1,~0u}));
  texture("_MainTex",3,0,3);texture("_BumpMap",4,0,2);texture("_PackedShadowRampTex",5,0,4);
  assert((materialTextureBindings(shader.data(),shader.size())==std::array<unsigned,3>{3,1,2}));
  texture("_LightMap",6,0,0);
  assert((materialTextureBindings(shader.data(),shader.size())==std::array<unsigned,3>{3,~0u,2}));
 }
 {Tracker probe;probe.allForms=false;probe.probeOnly=true;probe.setDrawHashes({hs[1].value});
  probe.created(1,64,0x40);probe.created(2,32,0x1);probe.bound(1,10,0);probe.bound(2,9,0);probe.mapped(9,0,32,staging);
  Copy upload{8,16,12};probe.copied(2,1,&upload,1);
  probe.indexBound(1,4,11,0); // 16-bit indices, bind twelve bytes before this upload
  probe.drawIndexed(12,3,1,7,0,0); // a different command buffer
  probe.drawIndexed(11,3,1,5,0,0); // begins before the upload
  probe.drawIndexed(11,3,1,10,0,0); // ends after the upload
  assert(probe.takeDraws().empty());
  probe.drawIndexed(11,3,2,7,-4,1);probe.drawIndexed(11,3,2,7,-4,1);
  auto draws=probe.takeDraws();assert(draws.size()==1&&draws[0].hash==hs[1].value&&draws[0].buffer==1&&draws[0].uploadOffset==16&&draws[0].bindOffset==4&&draws[0].first==7&&draws[0].relativeFirst==1&&
    draws[0].indexBits==16&&draws[0].count==3&&draws[0].baseVertex==-4&&draws[0].instances==2&&draws[0].firstInstance==1);
  assert(probe.take().empty()&&probe.takeDraws().empty()&&probe.stats().indexedDraws==5);
  probe.beginCommands(11);probe.drawIndexed(11,2,1,7,0,0);assert(probe.takeDraws().empty());
  probe.indexBound(1,16,11,0);probe.indexBound(0,0,11,0);probe.drawIndexed(11,2,1,0,0,0);assert(probe.takeDraws().empty());
  probe.indexBound(1,16,11,0);
  Copy overwrite{0,18,2};probe.copied(2,1,&overwrite,1);
  probe.drawIndexed(11,2,1,0,0,0);assert(probe.takeDraws().empty());
  probe.destroyed(1);probe.created(1,64,0x40);probe.bound(1,10,0);probe.copied(2,1,&upload,1);
  probe.drawIndexed(11,2,1,0,0,0);assert(probe.takeDraws().empty());
  probe.indexBound(1,16,11,1);probe.drawIndexed(11,2,1,1,0,0);
  draws=probe.takeDraws();assert(draws.size()==1&&draws[0].indexBits==32&&draws[0].relativeFirst==1);
 }
 {Tracker probe;probe.allForms=false;probe.probeOnly=true;
  const std::array<uint16_t,6> elsewhere{10,20,30,40,50,60};
  auto other=ibHashes(reinterpret_cast<const uint8_t*>(elsewhere.data()),sizeof elsewhere)[1].value;
  probe.setDrawHashes({hs[1].value,hs[2].value,hs[2].value,other,0});
  probe.created(11,12,0x40);probe.bound(11,11,0);probe.mapped(11,0,12,idx);
  probe.created(22,12,0x40);probe.bound(22,22,0);probe.mapped(22,0,12,elsewhere.data());
  probe.indexBound(11,0,1,0);probe.indexBound(22,0,2,0);
  // An instanced draw family for one object reaches the cap; the other object still reports independently.
  for(uint32_t i=0;i<=Tracker::maxDrawsPerHash;i++)probe.drawIndexed(1,6,1,0,0,i);
  probe.drawIndexed(2,3,1,3,0,0);
  auto draws=probe.takeDraws();assert(draws.size()==2*Tracker::maxDrawsPerHash+1&&probe.takeDraws().empty());
  for(const auto& d:draws){if(d.hash==other){assert(d.buffer==22&&d.relativeFirst==3);}else{assert((d.hash==hs[1].value||d.hash==hs[2].value)&&d.buffer==11&&d.relativeFirst==0);}}
  auto stats=probe.drawStats();assert(stats.size()==4&&stats.front().hash==0&&stats.front().uploads==0&&stats.front().draws==0);
  for(size_t i=1;i<stats.size();++i){assert(stats[i].uploads==1&&stats[i].draws==(stats[i].hash==other?1:Tracker::maxDrawsPerHash));}
  assert(probe.stats().uploads==2&&probe.stats().drawProbeUploads==2&&probe.take().empty());
  probe.allSkins=true;probe.skinMods={"First","Second"};probe.setSkinReplacement(true,true,{"Second"});
  probe.skinRoutes[hs[1].value]={hs[1].value,6,0,{{0,3},{3,3}}};
  probe.skinRoutes[other]={other,6,1,{{0,3},{3,3}}};
  SkinHit a,b,shortDraw;
  probe.drawIndexed(1,3,1,0,0,0,&a);probe.drawIndexed(2,3,1,3,0,0,&b);probe.drawIndexed(2,2,1,3,0,0,&shortDraw);
  assert(a&&a.profile==0&&a.slot==0&&!probe.skinActive(a.profile));
  assert(b&&b.profile==1&&b.slot==1&&probe.skinActive(b.profile)&&!shortDraw);
  probe.skinRoutes[hs[2].value]={hs[2].value,6,1,{{0,3},{3,3}}};
  probe.setSkinReplacement(true,true,{"First","Second"});
  SkinHit conflict;probe.drawIndexed(1,3,1,0,0,0,&conflict);assert(!conflict);
 }
 {Tracker probe;probe.allForms=false;probe.probeOnly=true;probe.allSkins=true;
  const auto pc32=hs[5].value,pc16=hs[1].value;
  probe.skinRoutes[pc32]={pc32,6,0,{{0,3},{3,3}},true};
  probe.skinRoutes[pc16]={pc16,6,0,{{0,3},{3,3}},true};
  probe.setDrawHashes({pc32,pc16});
  probe.created(1,sizeof idx,0x40);probe.bound(1,1,0);probe.mapped(1,0,sizeof idx,idx);
  probe.created(2,sizeof wide,0x40);probe.bound(2,2,0);probe.mapped(2,0,sizeof wide,wide);
  probe.indexBound(1,0,1,0);probe.indexBound(1,0,2,1);probe.indexBound(2,0,3,1);
  SkinHit a,b,wrongWidth,wrongCount;
  probe.drawIndexed(1,3,1,3,0,0,&a);probe.drawIndexed(3,3,1,0,0,0,&b);
  probe.drawIndexed(2,3,1,0,0,0,&wrongWidth);probe.drawIndexed(1,2,1,3,0,0,&wrongCount);
  assert(a&&a.slot==1&&b&&b.slot==0&&!wrongWidth&&!wrongCount);
  auto draws=probe.takeDraws();
  assert(std::any_of(draws.begin(),draws.end(),[&](const auto& d){return d.hash==pc32&&d.buffer==1&&d.indexBits==16;}));
  assert(std::any_of(draws.begin(),draws.end(),[&](const auto& d){return d.hash==pc16&&d.buffer==2&&d.indexBits==32;}));
 }
 {Tracker probe;probe.allForms=false;probe.probeOnly=true;probe.allSkins=true;
  const auto pc=hs[2].value;probe.iniSkinRules[pc]={{0,2,3,3}};probe.setDrawHashes({pc});
  probe.created(1,sizeof idx,0x1);probe.bound(1,1,0);probe.mapped(1,0,sizeof idx,idx);
  probe.created(2,52,0x40);const Copy region{0,16,sizeof idx};probe.copied(1,2,&region,1);probe.bound(2,2,0);
  probe.indexBound(2,22,1,0);
  SkinHit hit;probe.drawIndexed(1,3,1,0,0,0,&hit);
  assert(hit&&hit.slot==2&&hit.first==3&&hit.count==3);
  SkinHit narrow,shifted;
  probe.drawIndexed(1,2,1,0,0,0,&narrow);probe.drawIndexed(1,3,1,1,0,0,&shifted);
  assert(!narrow&&!shifted);
  probe.iniSkinRules[pc].push_back({1,0,3,-1});
  SkinHit ambiguous;probe.drawIndexed(1,3,1,0,0,0,&ambiguous);
  assert(!ambiguous&&probe.skinMisses[unsigned(SkinMiss::Ambiguous)]==1);
 }
 {Tracker gpu;gpu.allSkins=true;gpu.inspectResources=true;gpu.probeOnly=true;gpu.skinMods={"GPU"};gpu.setSkinReplacement(true,true,{"GPU"});
  const std::array<uint8_t,64> stagingBytes=[](){std::array<uint8_t,64> b{};for(unsigned i=0;i<b.size();i++)b[i]=uint8_t(i+19);return b;}();
  const auto hash=[&](size_t off,size_t n){return crc32c(0,stagingBytes.data()+off,n);};
  const auto texture=crc32c(hash(24,8),stagingBytes.data()+36,8);
  auto hex=[](uint32_t value){char s[9];std::snprintf(s,sizeof s,"%08x",value);return std::string(s);};
  const auto rules=resourceMatches("vb0:"+hex(hash(6,8))+",ps:"+hex(hash(52,8))+",tex0.0:"+hex(texture)+
    ",cb0.1:"+hex(hash(8,4))+",sb0.2.1:"+hex(hash(14,8))+",ib:"+hex(hash(0,6)));
  assert(rules&&rules->size()==6&&!resourceMatches("vb0:ffffffff,vb0:12345678")&&!resourceMatches("vb32:12345678")&&
   !resourceMatches("tex0.1:12345678,")&&!resourceMatches("sb2.9.1024:12345678"));
  gpu.resourceRules.push_back({{0,3,0,3},*rules});
  gpu.created(1,64,1);gpu.bound(1,11,0);gpu.mapped(11,0,64,stagingBytes.data());
  gpu.created(2,6,0x40);gpu.bound(2,12,0);gpu.created(3,8,0x80);gpu.bound(3,13,0);
  gpu.created(4,16,0x10);gpu.bound(4,11,0);gpu.created(5,8,0x20);gpu.bound(5,15,0);
  const Copy index{0,0,6},vertex{6,0,8},storage{14,0,8};
  gpu.copied(1,2,&index,1,7);gpu.copied(1,3,&vertex,1,7);gpu.copied(1,5,&storage,1,7);
  gpu.imageCreated(20,2,2,1,1,1,1,4);gpu.imageView(21,20,0,0);
  gpu.copiedImage(7,1,20,{24,3,0,0,2,2,true});
  gpu.shaderCreated(30,stagingBytes.data()+52,8);gpu.pipelineCreated(31,40,{{16,30}});gpu.pipelineCreated(32,40,{{32,30}});gpu.shaderDestroyed(30);
  gpu.descriptorLayout(41,{{1,0}},{{0,1},{1,1},{2,2}});gpu.descriptorAllocated(42,43,41);gpu.descriptorLayoutDestroyed(41);
  gpu.descriptorWrite(42,0,0,{ResourceKind::Texture,21});gpu.descriptorWrite(42,1,0,{ResourceKind::Constant,4,4,4});
  gpu.descriptorWrite(42,2,1,{ResourceKind::Storage,5,0,8});
  const uint64_t sets[]={42};const uint32_t offset=4,wrong=0;
  auto bind=[&](uintptr_t c,uint32_t dynamicOffset){gpu.indexBound(2,0,c,0);gpu.bindVertex(c,0,3,0);gpu.bindPipeline(c,false,31);
   gpu.bindDescriptors(c,false,40,0,sets,1,&dynamicOffset,1);};
  auto matched=[&](uintptr_t c){SkinHit hit;gpu.drawIndexed(c,3,1,0,0,0,&hit);return hit;};
  bind(7,offset);bind(8,offset);assert(matched(7).slot==3&&!matched(8));
  auto identities=gpu.takeResourceSamples();assert(identities.size()==6);
  gpu.submitted(7);assert(matched(8).slot==3);
  gpu.bindDescriptors(8,false,40,0,sets,1,&wrong,1);assert(!matched(8));bind(8,offset);
  gpu.imageChanged(8,20,0,1,0,1);assert(!matched(8));
  gpu.beginCommands(8);bind(8,offset);assert(matched(8).slot==3); // discarded writes never reached the resource table
  gpu.updated(9,3,0,4,stagingBytes.data());gpu.submitted(9);assert(!matched(8));
  gpu.copied(1,3,&vertex,1,10);gpu.submitted(10);assert(matched(8).slot==3);
  gpu.bindPipeline(8,true,32);gpu.bindDescriptors(8,true,40,0,sets,1,&offset,1);gpu.observeResources(8,true);
  auto dispatch=gpu.takeResourceSamples();assert(std::any_of(dispatch.begin(),dispatch.end(),[](auto& s){return s.compute&&s.identity.slot.kind==ResourceKind::Storage;}));
  gpu.imageDestroyed(20);gpu.imageCreated(20,2,2,1,1,1,1,4);gpu.imageView(21,20,0,0);
  gpu.copiedImage(0,1,20,{24,3,0,0,2,2,true});assert(!matched(8));
  gpu.descriptorWrite(42,0,0,{ResourceKind::Texture,21});assert(matched(8).slot==3);
  gpu.descriptorPoolReset(43);assert(!matched(8));
 }
 std::cout<<"Vulkan resource and draw tracking: PASS (PC IB aliases, all six Vulkan resource kinds, dynamic offsets, submissions, and lifetime)\n";}
