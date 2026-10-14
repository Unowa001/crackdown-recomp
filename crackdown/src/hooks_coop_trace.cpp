// Opt-in transparent wrapper tracing. Generated disassembly references are
// recorded in docs/coop-evidence.md. The stock guest implementation runs once.
// Raw keys and packet payloads are never written. net_trace lives in the SDK.
#include "generated/default/crackdown_init.h"
#include <rex/hook.h>
#include <rex/system/xthread.h>
#include <rex/system/coop_network_context.h>
#include <chrono>
#include <fstream>
#include <map>
#include <mutex>
#include <string_view>
#include <thread>
REXCVAR_DEFINE_STRING(coop_trace_file,"","Crackdown","Optional network JSONL evidence path");
REXCVAR_DEFINE_BOOL(coop_trace_verbose,false,"Crackdown","Also trace enter lines, empty polls and ProfileGate");
namespace {
std::mutex trace_mutex;
const uint8_t* ReadGuest(uint32_t address,uint32_t length) {
  if (!address || uint64_t(address)+length>0x100000000ull) return nullptr;
  auto heap=REX_KERNEL_MEMORY()->LookupHeap(address);
  rex::memory::HeapAllocationInfo region{};
  if (!heap || !heap->QueryRegionInfo(address,&region) ||
      !(region.state&rex::memory::kMemoryAllocationCommit) ||
      !(region.protect&rex::memory::kMemoryProtectRead) ||
      uint64_t(address)+length>uint64_t(region.base_address)+region.region_size) return nullptr;
  return REX_KERNEL_MEMORY()->TranslateVirtual<const uint8_t*>(address);
}
std::string Decode(std::string_view operation,const uint32_t* args,uint32_t result) {
  std::string decoded;
  const bool returning=operation.ends_with(" return");
  const bool send=operation.starts_with("NetDll_send");
  const bool recv=operation.starts_with("NetDll_recv");
  if ((send && !returning) || (recv && returning && int32_t(result)>0)) {
    const uint32_t length=send?args[2]:result;
    const auto bounded=std::min(length,4096u);
    if (const auto bytes=ReadGuest(args[1],bounded)) {
      uint32_t hash=2166136261u;
      for (uint32_t i=0;i<bounded;i++) hash=(hash^bytes[i])*16777619u;
      decoded+=fmt::format(",\"payload_length\":{},\"hashed_bytes\":{},\"fnv1a\":{}",length,bounded,hash);
    }
  }
  uint32_t address=0;
  if (operation.starts_with("NetDll_bind") || operation.starts_with("NetDll_connect")) address=args[1];
  if (operation.starts_with("NetDll_sendto") || (operation.starts_with("NetDll_recvfrom") && returning && int32_t(result)>=0)) address=args[4];
  if (const auto bytes=ReadGuest(address,16)) {
    decoded+=fmt::format(",\"family\":{},\"port\":{},\"address\":{}",
        rex::memory::load_and_swap<uint16_t>(bytes),rex::memory::load_and_swap<uint16_t>(bytes+2),rex::memory::load_and_swap<uint32_t>(bytes+4));
  }
  if (operation.starts_with("NetDll_XNetRegisterKey") || operation.starts_with("NetDll_XNetUnregisterKey") ||
      (operation.starts_with("NetDll_XNetCreateKey") && returning && result==0)) {
    if (const auto bytes=ReadGuest(args[0],8))
      decoded+=fmt::format(",\"session_id\":\"{:016X}\"",rex::memory::load_and_swap<uint64_t>(bytes));
  }
  if (operation.starts_with("SessionCreate"))
    decoded+=fmt::format(",\"xgi_message\":{},\"flags\":{},\"user\":{},\"public_slots\":{},\"private_slots\":{},\"info_ptr\":{},\"overlapped_ptr\":{}",0xb0010,args[0],args[1],args[2],args[3],args[5],args[6]);
  if (operation.starts_with("SessionJoin"))
    decoded+=fmt::format(",\"xgi_message\":{},\"member_count\":{}",0xb0012,args[1]);
  if (operation.starts_with("PrivilegeWrapper")) {
    decoded+=fmt::format(",\"privilege\":{}",args[1]);
    if (returning) if (const auto bytes=ReadGuest(args[2],4))
      decoded+=fmt::format(",\"allowed\":{}",rex::memory::load_and_swap<uint32_t>(bytes));
  }
  if (operation.starts_with("ProfileGate") && returning) {
    if (const auto bytes=ReadGuest(args[0],200))
      decoded+=fmt::format(",\"user\":{},\"profile_flags\":{}",rex::memory::load_and_swap<uint32_t>(bytes),rex::memory::load_and_swap<uint32_t>(bytes+196));
  }
  return decoded;
}
void Trace(const char* operation,uint32_t pc,uint32_t caller,const uint32_t* args,
           uint32_t result,uint32_t last_error,
           uint32_t thread_id=rex::system::XThread::GetCurrentThreadId()) {
  const auto now=std::chrono::duration_cast<std::chrono::microseconds>(
      std::chrono::steady_clock::now().time_since_epoch()).count();
  auto line=fmt::format("{{\"us\":{},\"op\":\"{}\",\"pc\":{},\"lr\":{},\"thread\":{},"
      "\"args\":[{},{},{},{},{},{},{},{}],\"result\":{},\"last_error\":{}{}}}",
      now,operation,pc,caller,thread_id,
      args[0],args[1],args[2],args[3],args[4],args[5],args[6],args[7],result,last_error,Decode(operation,args,result));
  std::lock_guard lock(trace_mutex);
  const auto path=REXCVAR_GET(coop_trace_file);
  if (path.empty()) { REXLOG_INFO("coop_trace {}",line); return; }
  static std::ofstream output(path,std::ios::app); output<<line<<'\n'; output.flush();
}
// The game polls nonblocking UDP and select() in a tight loop (2.8 GB of trace in
// minutes on 2026-10-13); keep only calls that did something.
bool Quiet(std::string_view operation,uint32_t result,uint32_t last_error) {
  if (operation.ends_with(" enter"))
    return !(operation.starts_with("NetDll_connect") || operation.starts_with("NetDll_accept") ||
             operation.starts_with("Session"));
  if (operation.starts_with("ProfileGate") || operation.starts_with("NetDll___WSAFDIsSet") ||
      operation.starts_with("NetDll_WSAGetLastError")) return true;
  if (operation.starts_with("NetDll_select")) return result==0;
  // Re-armed and waited on every poll (millions of calls per minute in game).
  if (operation.starts_with("NetDll_WSAEventSelect")) return result==0;
  if (operation.starts_with("NetDll_WSAWaitForMultipleEvents")) return result!=~0u;
  if (operation.starts_with("NetDll_recv")) return result==uint32_t(-1) && last_error==10035;
  return false;
}
void MaybeTrace(const char* operation,uint32_t pc,uint32_t caller,const uint32_t* args,
                uint32_t result,uint32_t last_error) {
  if (!REXCVAR_GET(coop_trace_verbose) && Quiet(operation,result,last_error)) return;
  Trace(operation,pc,caller,args,result,last_error);
}
// Calls still inside the SDK after 1.5 s are logged once as "<op> stuck":
// both instances fell silent together in the co-op load (2026-10-13) with no
// failing return, which looks like a blocked call rather than an error.
struct InFlight { const char* operation; uint32_t thread,pc,caller; uint32_t args[8]; int64_t start; bool reported; };
std::mutex inflight_mutex;
std::map<uint32_t,InFlight> inflight;
int64_t NowUs() {
  return std::chrono::duration_cast<std::chrono::microseconds>(
      std::chrono::steady_clock::now().time_since_epoch()).count();
}
void Watchdog() {
  for (;;) {
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    std::vector<InFlight> stuck;
    {
      std::lock_guard lock(inflight_mutex);
      for (auto& [thread,call]:inflight)
        if (!call.reported && NowUs()-call.start>1'500'000) { call.reported=true; stuck.push_back(call); }
    }
    for (const auto& call:stuck)
      Trace(fmt::format("{} stuck",call.operation).c_str(),call.pc,call.caller,call.args,0,0,call.thread);
  }
}
void Begin(const char* operation,uint32_t pc,uint32_t caller,const uint32_t* args) {
  static std::once_flag started;
  std::call_once(started,[] { std::thread(Watchdog).detach(); });
  const uint32_t thread=rex::system::XThread::GetCurrentThreadId();
  InFlight call{operation,thread,pc,caller,{},NowUs(),false};
  std::copy(args,args+8,call.args);
  std::lock_guard lock(inflight_mutex);
  inflight[thread]=call;
}
void End() {
  std::lock_guard lock(inflight_mutex);
  inflight.erase(rex::system::XThread::GetCurrentThreadId());
}
}
#define COOP_TRACE_WRAPPER(function,operation,pc) \
REX_HOOK_RAW(function) { \
  if (!REXCVAR_GET(net_trace)) { __imp__##function(ctx,base); return; } \
  const uint32_t caller=uint32_t(ctx.lr); \
  const uint32_t args[]={ctx.r3.u32,ctx.r4.u32,ctx.r5.u32,ctx.r6.u32,ctx.r7.u32,ctx.r8.u32,ctx.r9.u32,ctx.r10.u32}; \
  MaybeTrace(operation " enter",pc,caller,args,0,0); \
  Begin(operation,pc,caller,args); \
  __imp__##function(ctx,base); \
  End(); \
  auto thread=rex::system::XThread::GetCurrentThread(); \
  MaybeTrace(operation " return",pc,caller,args,ctx.r3.u32,thread?thread->last_error():0); \
}
COOP_TRACE_WRAPPER(sub_82929670,"NetDll_getsockname",0x82929670)
COOP_TRACE_WRAPPER(sub_829297C0,"NetDll_WSAWaitForMultipleEvents",0x829297C0)
COOP_TRACE_WRAPPER(sub_82929448,"NetDll_XNetInAddrToString",0x82929448)
COOP_TRACE_WRAPPER(sub_82929630,"NetDll_setsockopt",0x82929630)
COOP_TRACE_WRAPPER(sub_829293D0,"NetDll_XNetCreateKey",0x829293D0)
COOP_TRACE_WRAPPER(sub_82929508,"NetDll_XNetQosRelease",0x82929508)
COOP_TRACE_WRAPPER(sub_829296A0,"NetDll_bind",0x829296A0)
COOP_TRACE_WRAPPER(sub_82929470,"NetDll_XNetGetConnectStatus",0x82929470)
COOP_TRACE_WRAPPER(sub_82929650,"NetDll_getsockopt",0x82929650)
COOP_TRACE_WRAPPER(sub_829296D0,"NetDll_listen",0x829296D0)
COOP_TRACE_WRAPPER(sub_829297B8,"NetDll_WSACloseEvent",0x829297B8)
COOP_TRACE_WRAPPER(sub_829292F8,"NetDll_XNetStartup",0x829292F8)
COOP_TRACE_WRAPPER(sub_829293E0,"NetDll_XNetRegisterKey",0x829293E0)
COOP_TRACE_WRAPPER(sub_82929528,"NetDll_WSAStartup",0x82929528)
COOP_TRACE_WRAPPER(sub_82929418,"NetDll_XNetServerToInAddr",0x82929418)
COOP_TRACE_WRAPPER(sub_82929608,"NetDll_shutdown",0x82929608)
COOP_TRACE_WRAPPER(sub_82929788,"NetDll_WSAEventSelect",0x82929788)
COOP_TRACE_WRAPPER(sub_82929430,"NetDll_XNetInAddrToXnAddr",0x82929430)
COOP_TRACE_WRAPPER(sub_82929618,"NetDll_ioctlsocket",0x82929618)
COOP_TRACE_WRAPPER(sub_829297B0,"NetDll_WSACreateEvent",0x829297B0)
COOP_TRACE_WRAPPER(sub_82929400,"NetDll_XNetXnAddrToInAddr",0x82929400)
COOP_TRACE_WRAPPER(sub_829295E0,"NetDll_socket",0x829295E0)
COOP_TRACE_WRAPPER(sub_829297A0,"NetDll_inet_addr",0x829297A0)
COOP_TRACE_WRAPPER(sub_82929518,"NetDll_XNetGetTitleXnAddr",0x82929518)
COOP_TRACE_WRAPPER(sub_829296B8,"NetDll_connect",0x829296B8)
COOP_TRACE_WRAPPER(sub_829296E0,"NetDll_accept",0x829296E0)
COOP_TRACE_WRAPPER(sub_829293F0,"NetDll_XNetUnregisterKey",0x829293F0)
COOP_TRACE_WRAPPER(sub_82929480,"NetDll_XNetQosListen",0x82929480)
COOP_TRACE_WRAPPER(sub_82929750,"NetDll_send",0x82929750)
COOP_TRACE_WRAPPER(sub_82929688,"NetDll_getpeername",0x82929688)
COOP_TRACE_WRAPPER(sub_829297C8,"NetDll___WSAFDIsSet",0x829297C8)
COOP_TRACE_WRAPPER(sub_829295F8,"NetDll_closesocket",0x829295F8)
COOP_TRACE_WRAPPER(sub_82929730,"NetDll_recvfrom",0x82929730)
COOP_TRACE_WRAPPER(sub_82929718,"NetDll_recv",0x82929718)
COOP_TRACE_WRAPPER(sub_829296F8,"NetDll_select",0x829296F8)
COOP_TRACE_WRAPPER(sub_829293C0,"NetDll_XNetRandom",0x829293C0)
COOP_TRACE_WRAPPER(sub_829294A0,"NetDll_XNetQosLookup",0x829294A0)
COOP_TRACE_WRAPPER(sub_82929460,"NetDll_XNetConnect",0x82929460)
COOP_TRACE_WRAPPER(sub_82929768,"NetDll_sendto",0x82929768)
COOP_TRACE_WRAPPER(sub_829297A8,"NetDll_WSAGetLastError",0x829297A8)
COOP_TRACE_WRAPPER(sub_8292A9B8,"SessionCreate",0x8292A9B8)
COOP_TRACE_WRAPPER(sub_8292ABF8,"SessionJoinLocal",0x8292ABF8)
COOP_TRACE_WRAPPER(sub_8292ACA0,"SessionJoinRemote",0x8292ACA0)
COOP_TRACE_WRAPPER(sub_8292AF70,"SessionStart",0x8292AF70)
COOP_TRACE_WRAPPER(sub_8292B008,"SessionEnd",0x8292B008)
COOP_TRACE_WRAPPER(sub_8292B0B8,"SessionSearchEx",0x8292B0B8)
// .7.cpp:21652/21679 checks local sign-in before the privilege import.
COOP_TRACE_WRAPPER(sub_82744760,"PrivilegeWrapper",0x82744760)
// .140.cpp:32988 sign-in; privilege 252/251 at 0x82A8F034/0x82A8F058.
COOP_TRACE_WRAPPER(sub_82A8ED90,"ProfileGate",0x82A8ED90)
