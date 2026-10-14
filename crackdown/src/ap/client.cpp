#include "client.h"

#include <rex/logging.h>

#include <chrono>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winhttp.h>
#pragma comment(lib, "winhttp.lib")
#endif

namespace crackdown::ap {
namespace {
constexpr size_t kMaxMessage = 64 * 1024 * 1024;
// Bounds resolve/connect/send and the upgrade response. An upgraded socket's
// receive ignores it (measured, run net-silent); WinHTTP's own 30 s keepalive
// pings detect a dead link instead.
constexpr int kHandshakeTimeoutMs = 10000;

struct Endpoint {
  bool secure = false;
  std::wstring host;
  uint16_t port = 38281;
};

// Parses "[ws://|wss://]host[:port][/]". Returns the candidates to try in order.
std::vector<Endpoint> Endpoints(const std::string& address) {
  std::string rest = address;
  std::vector<bool> schemes{true, false};
  if (rest.rfind("wss://", 0) == 0) { rest = rest.substr(6); schemes = {true}; }
  else if (rest.rfind("ws://", 0) == 0) { rest = rest.substr(5); schemes = {false}; }
  while (!rest.empty() && rest.back() == '/') rest.pop_back();
  uint16_t port = 38281;
  if (auto colon = rest.rfind(':'); colon != std::string::npos) {
    try { port = static_cast<uint16_t>(std::stoi(rest.substr(colon + 1))); } catch (...) { return {}; }
    rest = rest.substr(0, colon);
  }
  if (rest.empty()) return {};
  std::vector<Endpoint> out;
  for (bool secure : schemes) out.push_back({secure, std::wstring(rest.begin(), rest.end()), port});
  return out;
}
}  // namespace

#ifdef _WIN32
// Opened, received on and closed by the receive thread only. The sender thread
// calls SendText/Shutdown, both send-side, under send_mutex; Close takes the same
// mutex, so no send or shutdown can overlap the handle being closed.
struct Client::Socket {
  HINTERNET session = nullptr, connect = nullptr, ws = nullptr;
  std::mutex send_mutex;
  bool closed = false, shutdown_sent = false;  // guarded by send_mutex
  ~Socket() {
    Close();
    if (connect) WinHttpCloseHandle(connect);
    if (session) WinHttpCloseHandle(session);
  }
  bool Open(const Endpoint& e, std::string& error) {
    session = WinHttpOpen(L"CrackdownRecomp-AP/1", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                          WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session) return Fail("WinHttpOpen", error);
    WinHttpSetTimeouts(session, kHandshakeTimeoutMs, kHandshakeTimeoutMs, kHandshakeTimeoutMs,
                       kHandshakeTimeoutMs);
    connect = WinHttpConnect(session, e.host.c_str(), e.port, 0);
    if (!connect) return Fail("WinHttpConnect", error);
    HINTERNET request = WinHttpOpenRequest(connect, L"GET", L"/", nullptr, WINHTTP_NO_REFERER,
                                           WINHTTP_DEFAULT_ACCEPT_TYPES,
                                           e.secure ? WINHTTP_FLAG_SECURE : 0);
    if (!request) return Fail("WinHttpOpenRequest", error);
    bool ok = WinHttpSetOption(request, WINHTTP_OPTION_UPGRADE_TO_WEB_SOCKET, nullptr, 0) &&
              WinHttpSendRequest(request, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA,
                                 0, 0, 0) &&
              WinHttpReceiveResponse(request, nullptr);
    DWORD status = 0, size = sizeof(status);
    if (ok) {
      WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                          WINHTTP_HEADER_NAME_BY_INDEX, &status, &size, WINHTTP_NO_HEADER_INDEX);
      ok = status == 101;
    }
    if (ok) ws = WinHttpWebSocketCompleteUpgrade(request, 0);
    const DWORD last = GetLastError();
    WinHttpCloseHandle(request);
    if (!ws) {
      error = "WebSocket upgrade failed (status " + std::to_string(status) + ", error " +
              std::to_string(last) + ")";
      return false;
    }
    return true;
  }
  bool Fail(const char* what, std::string& error) {
    error = std::string(what) + " failed (error " + std::to_string(GetLastError()) + ")";
    return false;
  }
  // Blocks for one whole text message. False on close/error. Receive thread only.
  bool Receive(std::string& message, std::string& error) {
    message.clear();
    char buffer[16384];
    for (;;) {
      DWORD read = 0;
      WINHTTP_WEB_SOCKET_BUFFER_TYPE type{};
      const DWORD rc = WinHttpWebSocketReceive(ws, buffer, sizeof(buffer), &read, &type);
      if (rc != NO_ERROR) { error = "receive error " + std::to_string(rc); return false; }
      if (type == WINHTTP_WEB_SOCKET_CLOSE_BUFFER_TYPE) { error = "server closed"; return false; }
      message.append(buffer, read);
      if (message.size() > kMaxMessage) { error = "message too large"; return false; }
      if (type == WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE ||
          type == WINHTTP_WEB_SOCKET_BINARY_MESSAGE_BUFFER_TYPE)
        return true;
    }
  }
  bool SendText(const std::string& text) {
    std::lock_guard lock(send_mutex);
    if (closed || shutdown_sent) return false;
    return WinHttpWebSocketSend(ws, WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE,
                                const_cast<char*>(text.data()), DWORD(text.size())) == NO_ERROR;
  }
  // Send-side close frame; the server's reply ends the pending receive.
  void Shutdown() {
    std::lock_guard lock(send_mutex);
    if (closed || shutdown_sent) return;
    shutdown_sent = true;
    WinHttpWebSocketShutdown(ws, WINHTTP_WEB_SOCKET_SUCCESS_CLOSE_STATUS, nullptr, 0);
  }
  // Receive thread only, after its last receive returned.
  void Close() {
    std::lock_guard lock(send_mutex);
    if (closed) return;
    closed = true;
    if (ws) WinHttpCloseHandle(ws);
    ws = nullptr;
  }
};
#else
// Linux transport is not implemented yet; the bridge reports it and stays offline
// (offline checks still persist and are sent from a Windows run).
struct Client::Socket {
  bool Open(const Endpoint&, std::string& error) { error = "AP transport is Windows-only"; return false; }
  bool Receive(std::string&, std::string& error) { error = "unsupported"; return false; }
  bool SendText(const std::string&) { return false; }
  void Shutdown() {}
  void Close() {}
};
#endif

struct Client::State {
  std::string address;
  std::atomic<bool> stop{false};
  std::atomic<int> running{0};
  std::atomic<uint64_t> generation{0};
  std::mutex in_mutex, out_mutex;
  std::condition_variable out_cv;
  std::deque<NetEvent> inbound;
  std::deque<std::pair<uint64_t, std::string>> outbound;
  std::shared_ptr<Socket> socket;  // guarded by out_mutex
  void Push(NetEvent event) {
    std::lock_guard lock(in_mutex);
    inbound.push_back(std::move(event));
  }
};

Client::~Client() { Stop(); }

void Client::Start(std::string address) {
  std::lock_guard life(lifecycle_);
  if (state_) state_->stop = true;
  JoinLocked();
  state_ = std::make_shared<State>();
  state_->address = std::move(address);
  state_->running = 2;
  thread_ = std::thread(Run, state_);
  sender_ = std::thread(Sender, state_);
}

void Client::RequestStop() {
  std::lock_guard life(lifecycle_);
  if (!state_) return;
  state_->stop = true;
  std::lock_guard lock(state_->out_mutex);
  state_->out_cv.notify_all();
}

void Client::Stop() {
  std::lock_guard life(lifecycle_);
  JoinLocked();
}

// lifecycle_ held. The sender sends the close frame and the server's reply ends
// the receive. A server that never answers (or a handshake still inside its 10 s
// timeouts) would block forever, so threads still running after 3 s are detached;
// they hold their own State reference.
void Client::JoinLocked() {
  if (!state_) return;
  state_->stop = true;
  {
    std::lock_guard lock(state_->out_mutex);
    state_->out_cv.notify_all();
  }
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
  while (state_->running > 0 && std::chrono::steady_clock::now() < deadline)
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  for (auto* t : {&thread_, &sender_}) {
    if (!t->joinable()) continue;
    if (state_->running > 0) t->detach();
    else t->join();
  }
  if (state_->running > 0) REXLOG_WARN("ap: network thread still closing; detached");
  state_.reset();
}

std::vector<NetEvent> Client::Drain() {
  std::shared_ptr<State> s;
  {
    std::lock_guard life(lifecycle_);
    s = state_;
  }
  if (!s) return {};
  std::lock_guard lock(s->in_mutex);
  std::vector<NetEvent> out(std::make_move_iterator(s->inbound.begin()),
                            std::make_move_iterator(s->inbound.end()));
  s->inbound.clear();
  return out;
}

void Client::Send(uint64_t generation, nlohmann::json commands) {
  std::lock_guard life(lifecycle_);
  if (!state_) return;
  std::lock_guard lock(state_->out_mutex);
  state_->outbound.emplace_back(generation, commands.dump());
  state_->out_cv.notify_all();
}

void Client::Sender(std::shared_ptr<State> s) {
  std::unique_lock lock(s->out_mutex);
  while (!s->stop) {
    s->out_cv.wait(lock, [&] { return s->stop || !s->outbound.empty(); });
    while (!s->stop && !s->outbound.empty()) {
      auto [generation, text] = std::move(s->outbound.front());
      s->outbound.pop_front();
      auto socket = s->socket;
      if (!socket || generation != s->generation.load()) continue;  // stale: drop
      lock.unlock();
      if (!socket->SendText(text)) socket->Shutdown();
      lock.lock();
    }
  }
  if (auto socket = s->socket) {
    lock.unlock();
    socket->Shutdown();
  }
  --s->running;
}

void Client::Run(std::shared_ptr<State> s) {
  auto endpoints = Endpoints(s->address);
  if (endpoints.empty()) {
    s->Push({NetEvent::Kind::disconnected, 0, {}, {}, "invalid server address"});
    --s->running;
    return;
  }
  int backoff_s = 1;
  while (!s->stop) {
    std::shared_ptr<Socket> socket;
    std::string error;
    for (const auto& endpoint : endpoints) {
      auto attempt = std::make_shared<Socket>();
      if (attempt->Open(endpoint, error)) { socket = attempt; break; }
      REXLOG_INFO("ap: connect {}{}:{} failed: {}", endpoint.secure ? "wss://" : "ws://",
                  std::string(endpoint.host.begin(), endpoint.host.end()), endpoint.port, error);
      if (s->stop) break;
    }
    if (socket) {
      const uint64_t generation = ++s->generation;
      {
        std::lock_guard lock(s->out_mutex);
        s->socket = socket;
        s->outbound.clear();  // nothing queued for an older socket survives
      }
      // A stop requested before the socket was published gets no close frame
      // from the sender; it is caught here before the first receive.
      s->Push({NetEvent::Kind::connected_socket, generation, {}, {}, {}});
      backoff_s = 1;
      std::string message;
      while (!s->stop && socket->Receive(message, error)) {
        auto parsed = nlohmann::json::parse(message, nullptr, false);
        if (parsed.is_discarded() || !parsed.is_array()) {
          error = "malformed packet";
          break;
        }
        for (auto& packet : parsed) {
          if (!packet.is_object() || !packet.contains("cmd") || !packet["cmd"].is_string()) continue;
          NetEvent event{NetEvent::Kind::packet, generation, packet["cmd"].get<std::string>(),
                         std::move(packet), {}};
          s->Push(std::move(event));
        }
      }
      {
        std::lock_guard lock(s->out_mutex);
        s->socket.reset();
      }
      ++s->generation;  // invalidate sends queued for the closed socket
      socket->Close();
      s->Push({NetEvent::Kind::disconnected, generation, {}, {}, s->stop ? "stopped" : error});
    }
    for (int i = 0; i < backoff_s * 10 && !s->stop; ++i)
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
    backoff_s = std::min(backoff_s * 2, 15);
  }
  --s->running;
}
}  // namespace crackdown::ap
