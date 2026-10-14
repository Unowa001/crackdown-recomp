// Archipelago WebSocket transport. The network thread only parses packets and
// queues immutable copies; it never touches guest memory or SessionState.
// Each successful connection gets a new generation. Sends name the generation
// they were built for and are dropped once it is stale, so a reply prepared
// for an old socket can never reach a new session.
//
// Handle ownership (WinHTTP "Concurrency in WinHTTP"): the receive thread opens,
// receives on and closes the socket; the sender thread only sends and, to stop,
// shuts down the send side (WinHttpWebSocketShutdown may overlap a receive).
// The handshake has 10 s timeouts; an open socket's receive has none (WinHTTP's
// keepalive pings end it when the link dies). The threads share only a
// reference-counted State, so a thread that outlives Stop()'s deadline can be
// detached safely.
#pragma once
#include <nlohmann/json.hpp>

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace crackdown::ap {
struct NetEvent {
  enum class Kind { packet, connected_socket, disconnected };
  Kind kind = Kind::packet;
  uint64_t generation = 0;
  std::string cmd;         // packet command, e.g. "RoomInfo"
  nlohmann::json body;     // the whole packet object
  std::string detail;      // disconnect reason
};

class Client {
 public:
  ~Client();
  // address: "host[:port]", "ws://host[:port]" or "wss://host[:port]". Without
  // a scheme, wss is tried before ws (as the reference clients do).
  void Start(std::string address);
  // Blocks at most ~3 s (stragglers are detached). Safe from any thread, repeatedly.
  void Stop();
  // Never blocks: asks the threads to close and exit. Stop()/Start() join them later.
  void RequestStop();
  std::vector<NetEvent> Drain();
  // Queue one JSON command array for the given generation.
  void Send(uint64_t generation, nlohmann::json commands);

 private:
  struct Socket;
  struct State;
  static void Run(std::shared_ptr<State> s);
  static void Sender(std::shared_ptr<State> s);
  void JoinLocked();
  std::mutex lifecycle_;  // serializes Start/Stop and the thread objects
  std::shared_ptr<State> state_;
  std::thread thread_, sender_;
};
}  // namespace crackdown::ap
