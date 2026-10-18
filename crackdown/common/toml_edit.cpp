#include "toml_edit.h"
#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cstring>
#include <fstream>
#include <functional>
#include <set>
#include <sstream>
#include <stdexcept>
#include <vector>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif

namespace crackdown::config {
ConfigTemp::ConfigTemp(const fs::path& target) {
  static std::atomic<uint64_t> counter=0;
#ifdef _WIN32
  const auto process_id=GetCurrentProcessId();
#else
  const auto process_id=getpid();
#endif
  for(;;) {
    auto candidate=target;
    candidate+=".new."+std::to_string(process_id)+"."+std::to_string(counter.fetch_add(1));
#ifdef _WIN32
    auto handle=CreateFileW(candidate.c_str(),GENERIC_WRITE,0,nullptr,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,nullptr);
    if(handle==INVALID_HANDLE_VALUE) {
      auto err=GetLastError(); if(err==ERROR_FILE_EXISTS || err==ERROR_ALREADY_EXISTS) continue;
      throw std::runtime_error("Cannot create settings temporary file (Windows error "+std::to_string(err)+")");
    }
    handle_=handle;
#else
    auto fd=open(candidate.c_str(),O_WRONLY|O_CREAT|O_EXCL|O_CLOEXEC,0600);
    if(fd<0) {
      if(errno==EEXIST) continue;
      throw std::runtime_error("Cannot create settings temporary file: "+std::string(std::strerror(errno)));
    }
    fd_=fd;
#endif
    path_=std::move(candidate); return;
  }
}
ConfigTemp::~ConfigTemp() {
#ifdef _WIN32
  if(handle_) CloseHandle(static_cast<HANDLE>(handle_));
#else
  if(fd_>=0) close(fd_);
#endif
  if(!path_.empty()) { std::error_code ec; fs::remove(path_,ec); }
}
void ConfigTemp::replace(const fs::path& target, const std::string& text) {
  size_t done=0;
#ifdef _WIN32
  if(!handle_) throw std::runtime_error("Settings temporary file is closed");
  while(done<text.size()) {
    DWORD written=0, count=static_cast<DWORD>(std::min<size_t>(text.size()-done,1024*1024));
    if(!WriteFile(static_cast<HANDLE>(handle_),text.data()+done,count,&written,nullptr) || !written)
      throw std::runtime_error("Cannot write settings");
    done+=written;
  }
  if(!FlushFileBuffers(static_cast<HANDLE>(handle_))) throw std::runtime_error("Cannot flush settings");
  auto handle=static_cast<HANDLE>(handle_); handle_=nullptr;
  if(!CloseHandle(handle)) throw std::runtime_error("Cannot close settings");
  if(!MoveFileExW(path_.c_str(),target.c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH))
    throw std::runtime_error("Cannot replace settings");
#else
  if(fd_<0) throw std::runtime_error("Settings temporary file is closed");
  while(done<text.size()) {
    auto n=write(fd_,text.data()+done,text.size()-done);
    if(n<0 && errno==EINTR) continue;
    if(n<=0) throw std::runtime_error("Cannot write settings");
    done+=static_cast<size_t>(n);
  }
  if(fsync(fd_)<0) throw std::runtime_error("Cannot flush settings");
  auto fd=fd_; fd_=-1;
  if(close(fd)<0) throw std::runtime_error("Cannot close settings");
  fs::rename(path_,target);
#endif
  path_.clear();
}
toml::table read_config(const fs::path& path) {
  if (!fs::exists(path)) return {};
  std::ifstream in(path, std::ios::binary); if (!in) throw std::runtime_error("Cannot read settings");
  std::stringstream s; s << in.rdbuf(); if (in.bad() || s.bad()) throw std::runtime_error("Cannot read settings"); return toml::parse(s.str());
}
const toml::node* setting(const toml::table& t, const std::string& key) {
  if (auto node=t.get(key)) return node;
  for (const auto& [k,v] : t) if (v.is_table()) {
    std::string prefix=std::string(k.str())+"_";
    if (key.starts_with(prefix)) if (auto n=setting(*v.as_table(),key.substr(prefix.size()))) return n;
  }
  return nullptr;
}
std::string toml_string(const std::string& text) {
  toml::table t; t.insert("v",text); std::ostringstream out; out << toml::toml_formatter(t);
  auto s=out.str(); return s.substr(s.find('=')+2);
}
void update_config(const fs::path& path, const Changes& changes) {
  if (changes.empty()) return;
  std::string text;
  if (fs::exists(path)) { std::ifstream in(path, std::ios::binary); if (!in) throw std::runtime_error("Cannot read settings"); std::stringstream s; s<<in.rdbuf(); if (in.bad() || s.bad()) throw std::runtime_error("Cannot read settings"); text=s.str(); }
  auto table=toml::parse(text);
  auto offset=[&](toml::source_position p) {
    size_t i=0; for (unsigned line=1; line<p.line; ++line) { i=text.find('\n',i); if(i==text.npos) throw std::runtime_error("Invalid TOML source position"); ++i; }
    // toml++ counts Unicode code points, while string edits use byte offsets.
    for (unsigned col=1; col<p.column; ++col) {
      if (i>=text.size()) throw std::runtime_error("Invalid TOML source column");
      unsigned char c=static_cast<unsigned char>(text[i]);
      i += c<0x80?1:c<0xE0?2:c<0xF0?3:4;
    }
    return i;
  };
  struct Edit { size_t begin,end; std::string value; }; std::vector<Edit> edits;
  std::set<std::string> found;
  std::function<void(const toml::table&,std::string)> walk = [&](const toml::table& t,std::string prefix) {
    for (const auto& [key,value] : t) {
      auto name=prefix+std::string(key.str());
      if(value.is_table()) { walk(*value.as_table(),name+"_"); continue; }
      auto it=changes.find(name); if(it==changes.end()) continue;
      if(!found.insert(name).second) throw std::runtime_error("Ambiguous TOML setting: " + name);
      auto src=value.source();
      size_t begin=offset(it->second ? src.begin : key.source().begin), end=offset(src.end);
      if(!it->second) {
        // A removed setting that had its line to itself takes the line with it
        // (an inline comment keeps the line).
        size_t line=text.rfind('\n',begin==0?0:begin-1); line=line==text.npos||begin==0?0:line+1;
        size_t after=text.find_first_not_of(" \t\r",end);
        if(text.find_first_not_of(" \t",line)==begin&&(after==text.npos||text[after]=='\n')) { begin=line; end=after==text.npos?text.size():after+1; }
      }
      edits.push_back({begin,end,it->second.value_or("")});
    }
  };
  walk(table,"");
  std::sort(edits.begin(),edits.end(),[](const Edit& a,const Edit& b){return a.begin>b.begin;});
  for(auto& e:edits) text.replace(e.begin,e.end-e.begin,e.value);
  const std::string newline = text.find("\r\n") != text.npos ? "\r\n" : "\n";
  std::string added;
  for(const auto& [key,value]:changes) if(value && !found.contains(key)) added+=key+" = "+*value+newline;
  text=added+text;
  (void)toml::parse(text); // Never replace a valid config with an invalid edit.
  ConfigTemp temp(path);
  temp.replace(path,text);
}
}
