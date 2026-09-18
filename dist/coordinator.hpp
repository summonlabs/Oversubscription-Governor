// Oversubscription Governor — coordinator (server) role.
#ifndef OVERSUB_DIST_COORDINATOR_HPP
#define OVERSUB_DIST_COORDINATOR_HPP

#include <atomic>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "oversub/governor.hpp"
#include "oversub/net.hpp"
#include "protocol.hpp"

namespace oversub {
namespace dist {

struct CoordinatorOptions {
  std::string address{"127.0.0.1"};
  u16 port{0};
  GovernorOptions governor;
  u32 max_connections{8};
  u64 max_frames_per_connection{65536};
};

class Coordinator {
 public:
  Coordinator();
  ~Coordinator();
  Coordinator(const Coordinator&) = delete;
  Coordinator& operator=(const Coordinator&) = delete;

  // Opens durable state and binds the listener, then serves connections on a
  // background thread.
  Status start(const CoordinatorOptions& options, OversubscriptionPolicy policy);
  // Blocks until a client requests shutdown (or stop() is called).
  Status wait_until_stopped();
  // Blocking single call: start + wait + stop. Used by the coordinator process.
  Status serve(const CoordinatorOptions& options, OversubscriptionPolicy policy);
  // Requests shutdown, wakes the accept loop, and joins every worker thread.
  Status stop();

  u16 port() const { return port_; }
  Governor& governor() { return governor_; }
  u64 connections() const { return connections_.load(); }
  u64 frames_handled() const { return frames_.load(); }

 private:
  friend class CoordinatorTestPeer;
  void accept_loop();
  void handle_connection(FramedConnection connection);
  void reap_handlers();
  Status wake_listener();
  void request_stop();

  struct Handler {
    std::thread thread;
    std::shared_ptr<std::atomic<bool>> finished;
  };

  CoordinatorOptions options_;
  Governor governor_;
  Socket listener_;
  u16 port_{0};
  std::atomic<bool> stop_requested_{false};
  std::atomic<bool> stopped_{false};
  std::atomic<u64> connections_{0};
  std::atomic<u64> frames_{0};
  std::thread accept_thread_;
  std::vector<Handler> handlers_;
  std::vector<std::string> peers_;
  std::mutex mutex_;
  std::condition_variable cv_;
  bool running_{false};
};

}  // namespace dist
}  // namespace oversub

#endif  // OVERSUB_DIST_COORDINATOR_HPP
