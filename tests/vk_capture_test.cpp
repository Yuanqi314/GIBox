// Index-buffer hashes as 3DMigoto computes them (vk_capture.hpp) and the tracker that finds index bytes in mapped memory.
#include "vk_capture.hpp"
#include <cassert>
#include <iostream>
using namespace ms::vkcap;
int main(){
 // CRC-32C check value, and crc32c_append: a CRC continued over a second part equals the CRC of both.
 assert(crc32c(0,"123456789",9)==0xE3069283u&&crc32c(0,"",0)==0);
 assert(crc32c(crc32c(0,"1234",4),"56789",5)==0xE3069283u);
 // HackerDevice::CreateBuffer: data hash, then the 24-byte desc appended (BindFlags INDEX_BUFFER, Usage per variant).
 uint16_t idx[6]={0,1,2,2,1,3};auto hs=ibHashes(reinterpret_cast<const uint8_t*>(idx),sizeof idx);
 uint32_t data=crc32c(0,idx,sizeof idx);D3D11BufferDesc d;d.byteWidth=sizeof idx;d.bindFlags=2;
 assert(hs[0].variant==0&&hs[0].value==data&&hs[1].variant==1&&hs[1].value==crc32c(data,&d,sizeof d));
 d.usage=1;assert(hs[2].value==crc32c(data,&d,sizeof d));d.usage=2;d.cpuAccessFlags=0x10000;assert(hs[3].value==crc32c(data,&d,sizeof d));
 // Widened to 32 bit (format 1), and the 12 bytes read as 3 32-bit values narrowed back (format 2, all fit in 16 bit).
 uint32_t wide[6]={0,1,2,2,1,3};D3D11BufferDesc w;w.byteWidth=sizeof wide;w.bindFlags=2;
 assert(hs[4].variant==4&&hs[4].value==crc32c(0,wide,sizeof wide)&&hs[5].value==crc32c(crc32c(0,wide,sizeof wide),&w,sizeof w));
 assert(hs.size()==8&&variantName(5)=="16-bit widened to 32-bit, desc Usage=DEFAULT");   // as 32-bit these values exceed 16 bit
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
 std::cout<<"Vulkan index-buffer hashes: PASS (CRC-32C append, 3DMigoto buffer desc variants, staging copy, mapped bind)\n";}
