#include "v2.h"
#include <fstream>
#include <stdexcept>
#include <algorithm>
namespace launcher {
Bytes read_bytes(const fs::path& path,uint64_t limit) {
  auto size=fs::file_size(path); if(size>limit) throw std::runtime_error("File exceeds size limit");
  Bytes b(static_cast<size_t>(size)); std::ifstream in(path,std::ios::binary);
  if(!in.read(reinterpret_cast<char*>(b.data()),static_cast<std::streamsize>(b.size()))) throw std::runtime_error("Cannot read "+utf8(path));
  return b;
}
void write_bytes(const fs::path& path,std::span<const unsigned char> b) {
  std::ofstream out(path,std::ios::binary|std::ios::trunc);
  out.write(reinterpret_cast<const char*>(b.data()),static_cast<std::streamsize>(b.size())); out.close();
  if(!out) throw std::runtime_error("Cannot write "+utf8(path));
}
std::vector<XzpEntry> read_xzp(std::span<const unsigned char> b) {
  auto bad=[] { throw std::runtime_error("Invalid XUIZ v1 package"); };
  auto number=[&](size_t p,size_t n)->uint32_t { if(p>b.size()||n>b.size()-p) bad(); uint32_t v=0; for(size_t i=0;i<n;++i) v=(v<<8)|b[p+i]; return v; };
  if(b.size()<22 || !std::equal(b.begin(),b.begin()+4,"XUIZ") || number(4,4)!=1 || number(8,4)!=b.size() || number(12,4)!=0) bad();
  size_t payload=22+uint64_t(number(16,4)),pos=22,end=0; if(payload>b.size()) bad();
  std::vector<XzpEntry> entries;
  for(size_t i=0,count=number(20,2);i<count;++i) {
    if(pos>payload || payload-pos<9) bad();
    size_t length=number(pos,4),offset=number(pos+4,4),units=b[pos+8]; pos+=9;
    if(units*2>payload-pos) bad(); std::string name;
    auto append=[&](uint32_t cp) {
      if(cp==0) bad();
      if(cp<128) name+=char(cp);
      else if(cp<2048) { name+=char(0xc0|(cp>>6)); name+=char(0x80|(cp&63)); }
      else if(cp<65536) { name+=char(0xe0|(cp>>12)); name+=char(0x80|((cp>>6)&63)); name+=char(0x80|(cp&63)); }
      else { name+=char(0xf0|(cp>>18)); name+=char(0x80|((cp>>12)&63)); name+=char(0x80|((cp>>6)&63)); name+=char(0x80|(cp&63)); }
    };
    for(size_t u=0;u<units;++u) {
      uint32_t cp=number(pos+u*2,2);
      if(cp>=0xd800 && cp<=0xdbff) { if(++u>=units) bad(); auto low=number(pos+u*2,2); if(low<0xdc00||low>0xdfff) bad(); cp=0x10000+((cp-0xd800)<<10)+(low-0xdc00); }
      else if(cp>=0xdc00&&cp<=0xdfff) bad(); append(cp);
    }
    pos+=units*2;
    if(offset!=end || offset>b.size()-payload || length>b.size()-payload-offset) bad();
    entries.push_back({std::move(name),Bytes(b.begin()+payload+offset,b.begin()+payload+offset+length)}); end=offset+length;
  }
  if(pos!=payload || end!=b.size()-payload) bad(); return entries;
}
const Bytes* xzp_entry(const std::vector<XzpEntry>& entries,const std::string& name) {
  const Bytes* found=nullptr;
  for(const auto& e:entries) if(e.name==name) { if(found) throw std::runtime_error("Duplicate XZP entry: "+name); found=&e.data; }
  return found;
}
}
