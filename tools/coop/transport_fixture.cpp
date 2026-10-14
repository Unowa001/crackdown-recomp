// Two independent SDK runtimes exercise guest socket boundaries on loopback.
#include <rex/runtime.h>
#include <rex/system/xsocket.h>
#include <rex/system/xthread.h>
#include <rex/system/xevent.h>
#include <future>
#include <iostream>
#include <stdexcept>
#include <thread>
using namespace rex;
using namespace rex::system;
namespace rex::kernel::xam {
u32 NetDll_XNetCreateKey_entry(u32,ppc_ptr_t<XNKID>,ppc_ptr_t<XNKEY>);
u32 NetDll_XNetRegisterKey_entry(u32,ppc_ptr_t<XNKID>,ppc_ptr_t<XNKEY>);
u32 NetDll_XNetUnregisterKey_entry(u32,ppc_ptr_t<XNKID>);
u32 NetDll_XNetXnAddrToInAddr_entry(u32,ppc_ptr_t<XNADDR>,mapped_void,mapped_void);
u32 NetDll_XNetInAddrToXnAddr_entry(u32,u32,ppc_ptr_t<XNADDR>,mapped_void);
u32 NetDll_getsockname_entry(u32,u32,ppc_ptr_t<XSOCKADDR_IN>,mapped_u32);
u32 NetDll_getsockopt_entry(u32,u32,u32,u32,mapped_void,mapped_u32);
u32 NetDll_WSAWaitForMultipleEvents_entry(u32,mapped_u32,u32,u32,u32);
}
void Require(bool condition,const char* message) {
  if (!condition) throw std::runtime_error(message);
  std::cout<<"PASS "<<message<<std::endl;
}
int Run(KernelState* kernel,bool host) {
  auto& net=kernel->coop_network();
  using namespace rex::kernel::xam;
  XNKID id{}; XNKEY key{};
  auto id_ptr=ppc_ptr_t<XNKID>::from_host(&id); auto key_ptr=ppc_ptr_t<XNKEY>::from_host(&key);
  Require(NetDll_XNetCreateKey_entry(1,id_ptr,key_ptr)==0 && (id.ab[0]&0xf0)==0,"typed System Link key generation");
  Require(NetDll_XNetRegisterKey_entry(1,id_ptr,key_ptr)==0,"key registration");
  Require(NetDll_XNetRegisterKey_entry(1,id_ptr,key_ptr)==0,"duplicate identical key registration");
  XNKEY bad_key=key; bad_key.ab[0]^=1;
  Require(NetDll_XNetRegisterKey_entry(1,id_ptr,ppc_ptr_t<XNKEY>::from_host(&bad_key))==10022,"conflicting duplicate key rejected");
  XNADDR peer_address=net.peers.front().address, reverse{}; XNKID reverse_id{};
  rex::be<uint32_t> token=0;
  Require(NetDll_XNetXnAddrToInAddr_entry(1,ppc_ptr_t<XNADDR>::from_host(&peer_address),mapped_void::from_host(&id),mapped_void::from_host(&token))==0,"peer address conversion");
  Require(NetDll_XNetInAddrToXnAddr_entry(1,uint32_t(token),ppc_ptr_t<XNADDR>::from_host(&reverse),mapped_void::from_host(&reverse_id))==0 && !std::memcmp(&peer_address,&reverse,36) && !std::memcmp(&id,&reverse_id,8),"by-value token reverse conversion");
  XSOCKADDR_IN bytes{}; bytes.sin_family=2; bytes.sin_port=1000; bytes.sin_addr=0xc0a80102;
  auto wire=reinterpret_cast<uint8_t*>(&bytes);
  Require(wire[2]==3 && wire[3]==0xe8 && wire[4]==192 && wire[5]==168 && wire[6]==1 && wire[7]==2,"sockaddr network byte fixture");
  auto udp=object_ref<XSocket>(new XSocket(kernel));
  Require(XSUCCEEDED(udp->Initialize(XSocket::X_AF_INET,XSocket::X_SOCK_DGRAM,XSocket::X_IPPROTO_VDP)),"VDP maps to UDP");
  rex::be<uint32_t> one=1;
  Require(XSUCCEEDED(udp->SetOption(0xffff,0x20,&one,4)),"SO_BROADCAST guest scalar");
  Require(XSUCCEEDED(udp->IOControl(0x8004667e,reinterpret_cast<uint8_t*>(&one))),"FIONBIO guest scalar");
  N_XSOCKADDR_IN bind{}; bind.sin_family=2; bind.sin_addr=0; bind.sin_port=1001;
  Require(XSUCCEEDED(udp->Bind(&bind,16)),"guest discovery bind remaps");
  rex::be<uint32_t> size=16; XSOCKADDR_IN queried{};
  Require(NetDll_getsockname_entry(1,udp->handle(),ppc_ptr_t<XSOCKADDR_IN>::from_host(&queried),mapped_u32::from_host(&size))==0 && size==16 && queried.sin_port==1001,"getsockname guest port and size");
  size=4; rex::be<uint32_t> option=0;
  Require(NetDll_getsockopt_entry(1,udp->handle(),0xffff,0x1008,mapped_void::from_host(&option),mapped_u32::from_host(&size))==0 && size==4 && option==2,"getsockopt guest type and size");
  size=1;
  Require(NetDll_getsockname_entry(1,udp->handle(),ppc_ptr_t<XSOCKADDR_IN>::from_host(&queried),mapped_u32::from_host(&size))==uint32_t(-1) && XThread::GetLastError()==10014,"truncated query rejected");
  size=16;
  Require(NetDll_getsockname_entry(1,0xdeadc0de,ppc_ptr_t<XSOCKADDR_IN>::from_host(&queried),mapped_u32::from_host(&size))==uint32_t(-1) && XThread::GetLastError()==10038,"unknown socket rejected");
  uint8_t payload[140]{}; N_XSOCKADDR_IN from{}; uint32_t len=16;
  Require(udp->RecvFrom(payload,140,0,&from,&len)==-1 && XThread::GetLastError()==10035,"empty receive WSAEWOULDBLOCK");
  auto event=object_ref<XEvent>(new XEvent(kernel)); event->Initialize(true,false);
  Require(udp->EventSelect(event,1)==0,"WSAEventSelect read association");
  auto timeout_event=object_ref<XEvent>(new XEvent(kernel)); timeout_event->Initialize(true,false);
  rex::be<uint32_t> handles[1]={timeout_event->handle()};
  auto wait_start=std::chrono::steady_clock::now();
  Require(NetDll_WSAWaitForMultipleEvents_entry(1,mapped_u32::from_host(handles),0,30,0)==258,"WSA wait timeout status");
  Require(std::chrono::steady_clock::now()-wait_start>=std::chrono::milliseconds(20),"WSA wait milliseconds conversion");
  N_XSOCKADDR_IN broadcast{}; broadcast.sin_family=2; broadcast.sin_addr=0xffffffff; broadcast.sin_port=1001;
  payload[0]=host?0xa1:0xb1; payload[139]=0x5a;
  bool received=false;
  for (int tries=0;tries<100 && !received;tries++) {
    Require(udp->SendTo(payload,140,0,&broadcast,16)==140,"broadcast to explicit peer unicast");
    auto wait=WaitForSingleObject(event->socket_event_handle(),100);
    if (wait!=WAIT_OBJECT_0) continue;
    len=16; uint8_t incoming[140]{};
    int count=udp->RecvFrom(incoming,140,0,&from,&len);
    if (count<0) continue;
    Require(count==140 && incoming[0]==(host?0xb1:0xa1) && incoming[139]==0x5a,"unaltered advertisement payload");
    Require(from.sin_port==1001 && uint32_t(from.sin_addr)==0xf0000001,"reverse source endpoint translation");
    received=true;
    udp->SendTo(payload,140,0,&broadcast,16);
  }
  Require(received,"two-process discovery round trip");
  udp->EventSelect({},0); udp->Close();
  Require(XSUCCEEDED(udp->Close()),"close idempotent");
  auto tcp=object_ref<XSocket>(new XSocket(kernel));
  Require(XSUCCEEDED(tcp->Initialize(XSocket::X_AF_INET,XSocket::X_SOCK_STREAM,XSocket::X_IPPROTO_TCP)),"TCP socket");
  bind.sin_port=1002;
  if (host) {
    Require(XSUCCEEDED(tcp->Bind(&bind,16)) && XSUCCEEDED(tcp->Listen(1)),"TCP host bind/listen");
    N_XSOCKADDR address{}; int size=16;
    auto accepted=tcp->Accept(&address,&size);
    Require(bool(accepted),"TCP accept");
    uint8_t value=0;
    Require(accepted->Recv(&value,1,0)==1 && value==0x3c,"TCP client packet");
    value=0xc3; Require(accepted->Send(&value,1,0)==1,"TCP host packet");
    accepted->Close();
  } else {
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    N_XSOCKADDR_IN peer{}; peer.sin_family=2; peer.sin_addr=0xf0000001; peer.sin_port=1002;
    Require(XSUCCEEDED(tcp->Connect(reinterpret_cast<N_XSOCKADDR*>(&peer),16)),"TCP connect through remote mapping");
    uint8_t value=0x3c; Require(tcp->Send(&value,1,0)==1,"TCP client send");
    Require(tcp->Recv(&value,1,0)==1 && value==0xc3,"TCP host receive");
  }
  tcp->Close();
  Require(net.Destinations({0xf0000099,1000}).empty(),"unknown opaque token rejected");
  Require(NetDll_XNetUnregisterKey_entry(1,id_ptr)==0 && net.keys.empty(),"key unregister cleanup");
  return 0;
}
int main(int argc,char** argv) {
  bool host=argc>1 && std::string(argv[1])=="A";
  InitLogging(); WSADATA data{}; WSAStartup(MAKEWORD(2,2),&data);
  cvar::SetFlagByName("net_mode","direct");
  cvar::SetFlagByName("net_port_offset",host?"20000":"21000");
  cvar::SetFlagByName("net_peer_endpoints",host?"127.0.0.1:22001":"127.0.0.1:21001");
  cvar::SetFlagByName("net_trace","true");
  Runtime runtime(std::filesystem::current_path()); RuntimeConfig config{}; config.tool_mode=true;
  if (XFAILED(runtime.Setup(std::move(config)))) return 2;
  std::promise<int> result; auto future=result.get_future();
  auto thread=object_ref<XHostThread>(new XHostThread(runtime.kernel_state(),1024*1024,0,[&] {
    int status=0;
    try { status=Run(runtime.kernel_state(),host); }
    catch (const std::exception& error) { std::cerr<<"FAIL "<<error.what()<<std::endl; status=1; }
    result.set_value(status); return status;
  }));
  if (XFAILED(thread->Create())) return 3;
  auto status=future.get();
  // Drain the guest host-thread before destroying its runtime.
  thread->Wait(0,0,false,nullptr);
  runtime.Shutdown(); WSACleanup(); return status;
}
