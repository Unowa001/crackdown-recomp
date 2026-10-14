#include "platform.h"
#include "sha256.h"
#include <algorithm>
#include <cwchar>
#include <cwctype>
#include <limits>
#include <fstream>
#include <sstream>
#include <cstdlib>
#include <stdexcept>
#include <vector>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <dxgi.h>
#include <shlobj.h>
#include <shellapi.h>
#else
#include <cerrno>
#include <cstring>
#include <spawn.h>
#include <fcntl.h>
#include <sys/file.h>
#include <signal.h>
#include <sys/utsname.h>
#include <sys/wait.h>
#include <unistd.h>
extern char** environ;
#endif
namespace launcher {
namespace {
#ifdef _WIN32
uint64_t process_start(HANDLE process) {
  FILETIME created{}, exited{}, kernel{}, user{};
  if(!GetProcessTimes(process,&created,&exited,&kernel,&user)) return 0;
  return (uint64_t(created.dwHighDateTime)<<32)|created.dwLowDateTime;
}
#else
uint64_t process_start(uint64_t pid) {
  std::ifstream in("/proc/"+std::to_string(pid)+"/stat");
  std::string line; std::getline(in,line);
  auto end=line.rfind(')'); if(end==line.npos) return 0;
  std::istringstream fields(line.substr(end+1)); std::string field;
  for(int i=3;i<=22;++i) if(!(fields>>field)) return 0;
  try { return std::stoull(field); } catch(...) { return 0; }
}
#endif
// Variables the launcher owns in the child: the two roots and the Archipelago connection.
bool root_environment(std::string name) {
  std::transform(name.begin(),name.end(),name.begin(),[](unsigned char c){return char(std::toupper(c));});
  return name=="REX_USER_DATA_ROOT" || name=="REX_CACHE_ROOT" || name.rfind("REX_AP_",0)==0;
}
}
ProcessOwner current_process_owner() {
#ifdef _WIN32
  return {GetCurrentProcessId(),process_start(GetCurrentProcess())};
#else
  auto pid=static_cast<uint64_t>(getpid());
  auto started=process_start(pid);
  // Non-Linux POSIX hosts use a timestamp; owner_alive still checks the PID conservatively.
  if(!started) started=static_cast<uint64_t>(std::chrono::system_clock::now().time_since_epoch().count());
  return {pid,started};
#endif
}
bool owner_alive(ProcessOwner owner) {
#ifdef _WIN32
  if(owner.pid>std::numeric_limits<DWORD>::max()) return true;
  auto process=OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION|SYNCHRONIZE,FALSE,static_cast<DWORD>(owner.pid));
  if(!process) return GetLastError()!=ERROR_INVALID_PARAMETER;
  auto start=process_start(process);
  bool alive=WaitForSingleObject(process,0)!=WAIT_OBJECT_0 && (!start || start==owner.started);
  CloseHandle(process); return alive;
#else
  if(owner.pid>static_cast<uint64_t>(std::numeric_limits<pid_t>::max())) return true;
  if(kill(static_cast<pid_t>(owner.pid),0)<0) return errno!=ESRCH;
  auto start=process_start(owner.pid);
  return !start || start==owner.started;
#endif
}
InstanceLock::InstanceLock(const fs::path& home) {
  auto canonical=fs::canonical(home);
#ifdef _WIN32
  auto native=canonical.wstring();
  std::transform(native.begin(),native.end(),native.begin(),[](wchar_t c){return std::towlower(c);});
  auto key=utf8(fs::path(native)); sha256::SHA256 hash; hash.add(key.data(),key.size());
  auto name=L"Local\\CrackdownLauncher-"+fs::u8path(hash.getHash()).wstring();
  auto mutex=CreateMutexW(nullptr,TRUE,name.c_str());
  if(!mutex) throw std::runtime_error("Cannot lock the launcher package");
  if(GetLastError()==ERROR_ALREADY_EXISTS) { CloseHandle(mutex); throw std::runtime_error("The launcher is already running"); }
  handle_=mutex;
#else
  fd_=open((canonical/".launcher.lock").c_str(),O_RDWR|O_CREAT|O_CLOEXEC,0600);
  if(fd_<0) throw std::runtime_error("Cannot lock the launcher package: "+std::string(std::strerror(errno)));
  if(flock(fd_,LOCK_EX|LOCK_NB)<0) {
    auto err=errno; close(fd_); fd_=-1;
    throw std::runtime_error(err==EWOULDBLOCK ? "The launcher is already running" : "Cannot lock the launcher package");
  }
#endif
}
InstanceLock::~InstanceLock() {
#ifdef _WIN32
  if(handle_) { ReleaseMutex(static_cast<HANDLE>(handle_)); CloseHandle(static_cast<HANDLE>(handle_)); }
#else
  if(fd_>=0) close(fd_);
#endif
}
std::string folder_url(const fs::path& path) {
  auto bytes=utf8(fs::absolute(path)); std::string uri="file://";
  constexpr char hex[]="0123456789ABCDEF";
  for(unsigned char c:bytes) {
    if((c>='a' && c<='z') || (c>='A' && c<='Z') || (c>='0' && c<='9') || c=='/' || c=='-' || c=='_' || c=='.' || c=='~') uri+=c;
    else { uri+='%'; uri+=hex[c>>4]; uri+=hex[c&15]; }
  }
  return uri;
}
void open_folder(const fs::path& path, const std::function<bool(const char*)>& open_url) {
  auto absolute=fs::absolute(path);
#ifdef _WIN32
  auto result=ShellExecuteW(nullptr,L"open",absolute.c_str(),nullptr,nullptr,SW_SHOWNORMAL);
  if(reinterpret_cast<INT_PTR>(result)<=32) throw std::runtime_error("Cannot open folder");
#else
  auto uri=folder_url(absolute);
  if(!open_url || !open_url(uri.c_str())) throw std::runtime_error("Cannot open folder");
#endif
}
GameProcess::~GameProcess() {
#ifdef _WIN32
  if(handle_) CloseHandle(static_cast<HANDLE>(handle_));
#endif
}
void GameProcess::start(const fs::path& home, const std::vector<std::string>& args) {
  if(active_) throw std::runtime_error("The game is already running");
  auto exe=home/
#ifdef _WIN32
      "crackdown.exe";
  if(!fs::is_regular_file(exe)) throw std::runtime_error("crackdown.exe is missing next to the launcher");
  if(handle_) { CloseHandle(static_cast<HANDLE>(handle_)); handle_=nullptr; }
  // Quote embedded quotes and double trailing backslashes for CommandLineToArgvW.
  std::wstring cmd=L"\""+exe.wstring()+L"\"";
  for(const auto& utf8_arg:args) {
    std::wstring arg=fs::u8path(utf8_arg).wstring(), quoted=L" \""; unsigned slashes=0;
    for(wchar_t c:arg) { if(c==L'\\') { ++slashes; continue; } if(c==L'\"') quoted.append(slashes*2+1,L'\\'); else quoted.append(slashes,L'\\'); slashes=0; quoted+=c; }
    quoted.append(slashes*2,L'\\'); quoted+=L'\"'; cmd+=quoted;
  }
  // The SDK ranks command line > REX_* environment > TOML (cvar.cpp Outranks), so
  // the explicit roots already win. Belt and braces: remove the variables this launcher
  // owns (roots, REX_AP_*: the profile decides AP) from the child environment only.
  auto inherited=GetEnvironmentStringsW();
  if(!inherited) throw std::runtime_error("Cannot read game environment");
  std::vector<wchar_t> environment;
  for(auto p=inherited; *p; p+=std::wcslen(p)+1) {
    std::wstring value=p; auto equal=value.find(L'=');
    auto name=value.substr(0,equal);
    std::transform(name.begin(),name.end(),name.begin(),[](wchar_t c){return std::towupper(c);});
    if(name==L"REX_USER_DATA_ROOT" || name==L"REX_CACHE_ROOT" || name.rfind(L"REX_AP_",0)==0) continue;
    environment.insert(environment.end(),value.begin(),value.end()); environment.push_back(0);
  }
  FreeEnvironmentStringsW(inherited); environment.push_back(0);
  if(environment.size()==1) environment.push_back(0);
  STARTUPINFOW si{}; si.cb=sizeof(si); PROCESS_INFORMATION pi{};
  if(!CreateProcessW(exe.c_str(),cmd.data(),nullptr,nullptr,FALSE,CREATE_UNICODE_ENVIRONMENT,environment.data(),home.c_str(),&si,&pi)) throw std::runtime_error("Cannot start crackdown.exe (Windows error "+std::to_string(GetLastError())+")");
  CloseHandle(pi.hThread); handle_=pi.hProcess;
#else
      "crackdown";
  std::string program=exe.string(); std::vector<std::string> copies=args;
  std::vector<char*> argv{program.data()}; for(auto& a:copies) argv.push_back(a.data()); argv.push_back(nullptr);
  // No shell; change only the child's working directory, preserving the launcher's.
  posix_spawn_file_actions_t actions;
  int action_err=posix_spawn_file_actions_init(&actions);
  if(action_err) throw std::runtime_error("Cannot initialize game process: "+std::string(std::strerror(action_err)));
  action_err=posix_spawn_file_actions_addchdir_np(&actions,home.c_str());
  if(action_err) {
    posix_spawn_file_actions_destroy(&actions);
    throw std::runtime_error("Cannot set game directory: "+std::string(std::strerror(action_err)));
  }
  std::vector<std::string> env_copies;
  for(auto p=environ; *p; ++p) {
    std::string value=*p;
    if(!root_environment(value.substr(0,value.find('=')))) env_copies.push_back(std::move(value));
  }  std::vector<char*> environment;
  for(auto& value:env_copies) environment.push_back(value.data()); environment.push_back(nullptr);
  int err=posix_spawn(&pid_,exe.c_str(),&actions,nullptr,argv.data(),environment.data());
  posix_spawn_file_actions_destroy(&actions);
  if(err) throw std::runtime_error("Cannot start game: "+std::string(std::strerror(err)));
#endif
  active_=true; started_=std::chrono::steady_clock::now();
}
std::optional<int> GameProcess::poll() {
  if(!active_) return {};
#ifdef _WIN32
  if(WaitForSingleObject(static_cast<HANDLE>(handle_),0)!=WAIT_OBJECT_0) return {};
  DWORD code=0; GetExitCodeProcess(static_cast<HANDLE>(handle_),&code); active_=false; return static_cast<int>(code);
#else
  int status=0; auto ret=waitpid(pid_,&status,WNOHANG);
  if(ret==0 || (ret<0 && errno==EINTR)) return {};
  active_=false; return ret<0 ? -1 : WIFEXITED(status) ? WEXITSTATUS(status) : 128+WTERMSIG(status);
#endif
}
double GameProcess::elapsed() const { return std::chrono::duration<double>(std::chrono::steady_clock::now()-started_).count(); }
std::string system_info() {
  std::ostringstream out;
#ifdef _WIN32
  HKEY key{};
  if(RegOpenKeyExW(HKEY_LOCAL_MACHINE,L"HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0",0,KEY_READ,&key)==ERROR_SUCCESS) {
    wchar_t name[256]{}; DWORD size=sizeof(name);
    if(RegQueryValueExW(key,L"ProcessorNameString",nullptr,nullptr,reinterpret_cast<BYTE*>(name),&size)==ERROR_SUCCESS) out<<"CPU: "<<utf8(fs::path(name))<<'\n';
    RegCloseKey(key);
  }
  using VersionFn=LONG(WINAPI*)(OSVERSIONINFOW*);
  auto version_fn=reinterpret_cast<VersionFn>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"),"RtlGetVersion"));
  OSVERSIONINFOW ver{}; ver.dwOSVersionInfoSize=sizeof(ver);
  if(version_fn && version_fn(&ver)==0) out<<"Windows: "<<ver.dwMajorVersion<<'.'<<ver.dwMinorVersion<<" build "<<ver.dwBuildNumber<<'\n';
  IDXGIFactory* factory=nullptr;
  if(SUCCEEDED(CreateDXGIFactory(__uuidof(IDXGIFactory),reinterpret_cast<void**>(&factory)))) {
    IDXGIAdapter* adapter=nullptr;
    for(UINT i=0;factory->EnumAdapters(i,&adapter)!=DXGI_ERROR_NOT_FOUND;++i) {
      DXGI_ADAPTER_DESC d{}; adapter->GetDesc(&d); out<<"GPU: "<<utf8(fs::path(d.Description));
      LARGE_INTEGER version{};
      if(SUCCEEDED(adapter->CheckInterfaceSupport(__uuidof(IDXGIDevice),&version))) out<<" driver "<<HIWORD(version.HighPart)<<'.'<<LOWORD(version.HighPart)<<'.'<<HIWORD(version.LowPart)<<'.'<<LOWORD(version.LowPart);
      out<<'\n'; adapter->Release();
    }
    factory->Release();
  }
#else
  utsname u{}; if(uname(&u)==0) out<<u.sysname<<' '<<u.release<<' '<<u.machine<<'\n';
  std::ifstream cpu("/proc/cpuinfo"); std::string line;
  while(std::getline(cpu,line)) if(line.starts_with("model name")) { out<<line<<'\n'; break; }
#endif
  return out.str();
}
fs::path default_user_folder() {
#ifdef _WIN32
  fs::path result; PWSTR path=nullptr;
  if(SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Documents,KF_FLAG_DEFAULT,nullptr,&path))) result=path;
  CoTaskMemFree(path);
  if(result.empty()) throw std::runtime_error("Cannot find the Documents folder");
  return result/"crackdown";
#else
  if(auto xdg=std::getenv("XDG_DATA_HOME")) return fs::path(xdg)/"crackdown";
  if(auto home=std::getenv("HOME")) return fs::path(home)/".local"/"share"/"crackdown";
  throw std::runtime_error("Cannot find the user data folder");
#endif
}
std::string newest_log_tail(const fs::path& home, fs::file_time_type since) {
  fs::path newest; auto time=since; std::error_code ec;
  if(!fs::is_directory(home/"logs",ec)) return "No new game log was created.";
  for(const auto& file:fs::directory_iterator(home/"logs")) {
    auto name=utf8(file.path().filename());
    if(!file.is_regular_file() || !name.starts_with("crackdown_") || file.path().extension()!=".log") continue;
    auto t=file.last_write_time(); if(t>=time) { time=t; newest=file.path(); }
  }
  if(newest.empty()) return "No new game log was created.";
  std::ifstream in(newest,std::ios::binary); if(!in) return "Cannot read the new game log.";
  auto size=fs::file_size(newest); if(size>16384) in.seekg(static_cast<std::streamoff>(size-16384));
  std::stringstream s; s<<in.rdbuf(); return utf8(newest.filename())+"\n"+s.str();
}
}
