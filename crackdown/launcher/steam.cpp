#include "v2.h"
#include "platform.h"
#include <algorithm>
#include <set>
#include <stdexcept>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <tlhelp32.h>
#endif
namespace launcher {
namespace {
std::string zstring(std::span<const unsigned char> b,size_t& p) {
  size_t start=p; while(p<b.size() && b[p]) ++p;
  if(p==b.size()) throw std::runtime_error("Truncated Steam VDF string");
  std::string s(reinterpret_cast<const char*>(b.data()+start),p-start); ++p; return s;
}
std::vector<VdfNode> parse(std::span<const unsigned char> b,size_t& p,unsigned depth) {
  if(depth>32) throw std::runtime_error("Steam VDF nesting limit");
  std::vector<VdfNode> nodes;
  while(p<b.size()) {
    auto type=b[p++]; if(type==8) return nodes;
    VdfNode n; n.type=type; n.key=zstring(b,p);
    if(type==0) n.children=parse(b,p,depth+1);
    else if(type==1) n.value=zstring(b,p);
    else {
      size_t length=type==2||type==3||type==4||type==6?4:type==7||type==10?8:0;
      if(!length || length>b.size()-p) throw std::runtime_error("Unsupported or truncated Steam VDF field");
      n.value.assign(reinterpret_cast<const char*>(b.data()+p),length); p+=length;
    }
    nodes.push_back(std::move(n));
  }
  throw std::runtime_error("Unterminated Steam VDF object");
}
void emit(Bytes& b,const std::vector<VdfNode>& nodes,unsigned depth=0) {
  if(depth>32) throw std::runtime_error("Steam VDF nesting limit");
  auto z=[&](const std::string& s) { if(s.find('\0')!=std::string::npos) throw std::runtime_error("NUL in VDF string"); b.insert(b.end(),s.begin(),s.end()); b.push_back(0); };
  for(const auto& n:nodes) {
    b.push_back(n.type); z(n.key);
    if(n.type==0) emit(b,n.children,depth+1);
    else if(n.type==1) z(n.value);
    else { size_t count=n.type==2||n.type==3||n.type==4||n.type==6?4:n.type==7||n.type==10?8:0;
      if(!count || n.value.size()!=count) throw std::runtime_error("Invalid VDF value"); b.insert(b.end(),n.value.begin(),n.value.end()); }
  }
  b.push_back(8);
}
std::string field(const VdfNode& n,const char* key) { for(const auto& f:n.children) if(f.key==key && f.type==1) return f.value; return {}; }
std::string integer(uint32_t v) { std::string s(4,'\0'); for(unsigned i=0;i<4;++i) s[i]=char(v>>(i*8)); return s; }
uint32_t crc32(const std::string& s) { uint32_t c=0xffffffff; for(unsigned char b:s) { c^=b; for(int i=0;i<8;++i) c=(c>>1)^(0xedb88320u & (0u-(c&1))); } return ~c; }
}
std::vector<VdfNode> read_vdf(std::span<const unsigned char> bytes) {
  if(bytes.empty()) return {{0,"shortcuts",{},{}}};
  size_t p=0; auto result=parse(bytes,p,0); if(p!=bytes.size()) throw std::runtime_error("Trailing Steam VDF bytes"); return result;
}
Bytes write_vdf(const std::vector<VdfNode>& nodes) { Bytes bytes; emit(bytes,nodes); return bytes; }
bool append_steam_shortcut(std::vector<VdfNode>& nodes,const fs::path& executable) {
  auto root=std::find_if(nodes.begin(),nodes.end(),[](const auto& n){return n.type==0 && n.key=="shortcuts";});
  if(root==nodes.end() || std::count_if(nodes.begin(),nodes.end(),[](const auto& n){return n.key=="shortcuts";})!=1) throw std::runtime_error("Steam VDF has no unique shortcuts object");
  auto exe="\""+utf8(fs::absolute(executable))+"\""; auto dir="\""+utf8(fs::absolute(executable).parent_path())+"\"";
  std::set<unsigned> keys;
  for(const auto& n:root->children) {
    if(field(n,"Exe")==exe) return false;
    if(n.type!=0 || n.key.empty() || n.key.size()>8 || !std::all_of(n.key.begin(),n.key.end(),[](unsigned char c){return c>='0'&&c<='9';})) throw std::runtime_error("Invalid Steam shortcut index");
    unsigned k=static_cast<unsigned>(std::stoul(n.key)); if(!keys.insert(k).second) throw std::runtime_error("Duplicate Steam shortcut index");
  }
  unsigned index=0; if(!keys.empty()) index=*keys.rbegin()+1;
  VdfNode n{0,std::to_string(index),{}, {}};
  auto str=[&](const char* k,const std::string& v) { n.children.push_back({1,k,v,{}}); };
  auto num=[&](const char* k,uint32_t v) { n.children.push_back({2,k,integer(v),{}}); };
  num("appid",crc32(exe+"Crackdown")|0x80000000u); str("AppName","Crackdown"); str("Exe",exe); str("StartDir",dir);
  str("icon",""); str("ShortcutPath",""); str("LaunchOptions",""); num("IsHidden",0); num("AllowDesktopConfig",1); num("AllowOverlay",1);
  num("OpenVR",0); num("Devkit",0); str("DevkitGameID",""); num("DevkitOverrideAppID",0); num("LastPlayTime",0);
  n.children.push_back({0,"tags",{},{{1,"0","Crackdown",{}}}}); root->children.push_back(std::move(n)); return true;
}
std::string add_to_steam(const fs::path& executable) {
#ifdef _WIN32
  HANDLE snapshot=CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS,0);
  if(snapshot==INVALID_HANDLE_VALUE) throw std::runtime_error("Cannot check whether Steam is running");
  PROCESSENTRY32W p{}; p.dwSize=sizeof(p); bool running=false;
  if(!Process32FirstW(snapshot,&p)) { CloseHandle(snapshot); throw std::runtime_error("Cannot inspect Steam processes"); }
  do { if(_wcsicmp(p.szExeFile,L"steam.exe")==0) running=true; } while(Process32NextW(snapshot,&p)); CloseHandle(snapshot);
  if(running) throw std::runtime_error("Close Steam before adding the shortcut, then try again.");
  wchar_t buffer[32768]; DWORD bytes=sizeof(buffer);
  if(RegGetValueW(HKEY_CURRENT_USER,L"Software\\Valve\\Steam",L"SteamPath",RRF_RT_REG_SZ,nullptr,buffer,&bytes)!=ERROR_SUCCESS) throw std::runtime_error("Steam installation was not found");
  fs::path userdata=fs::path(buffer)/"userdata";
  if(!fs::is_directory(userdata)) throw std::runtime_error("Steam has no local users. Sign in to Steam first.");
  size_t added=0,existing=0;
  for(const auto& u:fs::directory_iterator(userdata)) {
    auto id=utf8(u.path().filename());
    if(!u.is_directory() || id.empty() || !std::all_of(id.begin(),id.end(),[](unsigned char c){return c>='0'&&c<='9';})) continue;
    auto config=u.path()/"config"; fs::create_directories(config); auto path=config/"shortcuts.vdf";
    Bytes original=fs::exists(path)?read_bytes(path,16*1024*1024):Bytes{}; auto nodes=read_vdf(original);
    if(!append_steam_shortcut(nodes,executable)) { ++existing; continue; }
    auto output=write_vdf(nodes); (void)read_vdf(output);
    if(fs::exists(path)) {
      auto backup=path; backup+=".launcher-backup."+std::to_string(GetTickCount64());
      if(!fs::copy_file(path,backup)) throw std::runtime_error("Cannot back up Steam shortcuts");
      if(read_bytes(path,16*1024*1024)!=original) throw std::runtime_error("Steam shortcuts changed; retry with Steam closed");
    }
    ConfigTemp temporary(path); temporary.replace(path,std::string(output.begin(),output.end())); ++added;
  }
  if(!added && !existing) throw std::runtime_error("Steam has no local users. Sign in to Steam first.");
  return "Steam shortcuts added: "+std::to_string(added)+"; already present: "+std::to_string(existing)+". Open Steam to play.";
#else
  (void)executable; throw std::runtime_error("Add to Steam is available on Windows.");
#endif
}
}
