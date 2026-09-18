#include "coordinator.hpp"

namespace oversub {
namespace dist {
namespace {

Status reject_with(FramedConnection& connection, const Status& status) {
  std::vector<u8> payload;
  encode_status(status, payload);
  return connection.send_frame(static_cast<u32>(MessageType::Reject), payload);
}

}  // namespace

Coordinator::Coordinator() = default;

Coordinator::~Coordinator() { (void)stop(); }

Status Coordinator::start(const CoordinatorOptions& options, OversubscriptionPolicy policy) {
  options_ = options;
  if (options_.max_connections == 0) {
    return Status::failure(StatusCode::InvalidArgument, ReasonCode::LimitExceeded,
                           "max_connections must be non-zero");
  }
  if (options_.max_frames_per_connection == 0) {
    return Status::failure(StatusCode::InvalidArgument, ReasonCode::LimitExceeded,
                           "max_frames_per_connection must be non-zero");
  }
  Status status = governor_.open(options_.governor, std::move(policy));
  if (!status.ok()) return status;
  status = tcp_listen(options_.address, options_.port, listener_, port_);
  if (!status.ok()) {
    (void)governor_.close();
    return status;
  }
  {
    std::lock_guard<std::mutex> lock(mutex_);
    running_ = true;
  }
  accept_thread_ = std::thread([this] { accept_loop(); });
  return Status::success();
}

Status Coordinator::wait_until_stopped() {
  std::unique_lock<std::mutex> lock(mutex_);
  cv_.wait(lock, [this] { return !running_; });
  return Status::success();
}

Status Coordinator::serve(const CoordinatorOptions& options, OversubscriptionPolicy policy) {
  Status status = start(options, std::move(policy));
  if (!status.ok()) return status;
  status = wait_until_stopped();
  Status stop_status = stop();
  if (!status.ok()) return status;
  return stop_status;
}

Status Coordinator::stop() {
  request_stop();
  if (accept_thread_.joinable()) accept_thread_.join();
  for (auto& handler : handlers_) {
    if (handler.thread.joinable()) handler.thread.join();
  }
  handlers_.clear();
  listener_.reset();
  (void)governor_.close();
  return Status::success();
}

void Coordinator::request_stop() {
  const bool was_stopped = stop_requested_.exchange(true);
  if (!was_stopped) (void)wake_listener();
}

// A self-connection makes the blocking accept() return deterministically without
// relying on close-while-accepting semantics.
Status Coordinator::wake_listener() {
  if (!listener_.valid()) return Status::success();
  Socket socket;
  Status status = tcp_connect(options_.address, port_, socket);
  if (!status.ok()) return status;
  socket.reset();
  return Status::success();
}

void Coordinator::reap_handlers() {
  for (auto it = handlers_.begin(); it != handlers_.end();) {
    if (it->finished->load()) {
      if (it->thread.joinable()) it->thread.join();
      it = handlers_.erase(it);
    } else {
      ++it;
    }
  }
}

void Coordinator::accept_loop() {
  u32 consecutive_failures = 0;
  while (!stop_requested_.load()) {
    Socket client;
    std::string peer;
    Status status = tcp_accept(listener_, client, peer);
    if (!status.ok()) {
      if (stop_requested_.load()) break;
      if (++consecutive_failures > 16) break;
      continue;
    }
    consecutive_failures = 0;
    if (stop_requested_.load()) break;   // the wake connection
    reap_handlers();
    if (handlers_.size() >= options_.max_connections) {
      FramedConnection rejected(std::move(client));
      rejected.set_peer(peer);
      (void)reject_with(rejected, Status::failure(StatusCode::Busy, ReasonCode::QueueLimitExceeded,
                                                  "connection limit reached"));
      rejected.close();
      continue;
    }
    auto finished = std::make_shared<std::atomic<bool>>(false);
    auto* finished_ptr = finished.get();
    handlers_.push_back(Handler{
        std::thread([this, client = std::move(client), peer, finished_ptr]() mutable {
          FramedConnection connection(std::move(client));
          connection.set_peer(peer);
          {
            std::lock_guard<std::mutex> lock(mutex_);
            peers_.push_back(peer);
          }
          // A connection handler must never let an exception escape the thread:
          // it would terminate the whole coordinator process.
          try {
            handle_connection(std::move(connection));
          } catch (...) {
            connection.close();
          }
          finished_ptr->store(true);
        }),
        std::move(finished)});
    connections_.fetch_add(1);
  }
  {
    std::lock_guard<std::mutex> lock(mutex_);
    running_ = false;
  }
  cv_.notify_all();
}

void Coordinator::handle_connection(FramedConnection connection) {
  u32 type = 0;
  std::vector<u8> payload;
  Status status = connection.recv_frame(type, payload);
  if (!status.ok()) return;
  if (type != static_cast<u32>(MessageType::Hello)) {
    (void)reject_with(connection,
                      Status::failure(StatusCode::InvalidArgument, ReasonCode::ProtocolViolation,
                                      "the first frame must be HELLO"));
    return;
  }
  HelloRequest hello;
  if (!decode_hello(payload, hello)) {
    (void)reject_with(connection, Status::failure(StatusCode::CorruptData, ReasonCode::FrameMalformed,
                                                  "HELLO payload is malformed"));
    return;
  }
  if (hello.protocol_version != kProtocolVersion) {
    (void)reject_with(connection, Status::failure(StatusCode::Unsupported, ReasonCode::PublisherProtocolMismatch,
                                                  "protocol version mismatch"));
    return;
  }
  Status registration = hello.adopt_incarnation
                            ? governor_.adopt_incarnation(hello.publisher, hello.incarnation,
                                                          governor_.epoch(), hello.tick)
                            : governor_.register_publisher(hello.publisher, hello.incarnation,
                                                           governor_.epoch(), hello.tick);
  if (!registration.ok() &&
      hello.adopt_incarnation &&
      registration.reason == ReasonCode::PublisherNotRegistered) {
    registration = governor_.register_publisher(hello.publisher, hello.incarnation, governor_.epoch(), hello.tick);
  }
  if (!registration.ok()) {
    (void)reject_with(connection, registration);
    return;
  }
  HelloResponse response;
  response.protocol_version = kProtocolVersion;
  response.epoch = governor_.epoch();
  response.policy_generation = governor_.policy_generation();
  status = governor_.evidence_generations(hello.domain, response.capacity_generation,
                                          response.reservation_generation, response.admission_generation);
  (void)status;
  if (hello.publisher.valid()) (void)governor_.publisher_sequence(hello.publisher, response.last_sequence);
  {
    std::vector<u8> encoded;
    encode_hello_response(response, encoded);
    status = connection.send_frame(static_cast<u32>(MessageType::HelloAck), encoded);
    if (!status.ok()) return;
  }

  u64 frames = 0;
  while (!stop_requested_.load()) {
    status = connection.recv_frame(type, payload);
    if (!status.ok()) return;
    frames_.fetch_add(1);
    if (++frames > options_.max_frames_per_connection) {
      (void)reject_with(connection, Status::failure(StatusCode::OutOfRange, ReasonCode::QueueLimitExceeded,
                                                    "frame budget for this connection is exhausted"));
      return;
    }
    switch (static_cast<MessageType>(type)) {
      case MessageType::Publish: {
        ByteReader reader(payload);
        u32 kind = 0;
        if (!reader.u32(kind)) {
          (void)reject_with(connection, Status::failure(StatusCode::CorruptData, ReasonCode::FrameMalformed,
                                                        "PUBLISH header is malformed"));
          return;
        }
        std::vector<u8> body(payload.begin() + static_cast<std::ptrdiff_t>(reader.offset()), payload.end());
        PublishAck ack;
        if (kind == static_cast<u32>(EvidenceKind::Capacity)) {
          CapacitySnapshot snapshot;
          if (!decode_capacity_snapshot(body, snapshot)) {
            (void)reject_with(connection, Status::failure(StatusCode::CorruptData, ReasonCode::FrameMalformed,
                                                          "capacity snapshot is malformed"));
            return;
          }
          const IngestOutcome outcome = governor_.ingest_capacity(snapshot);
          ack.code = outcome.status.code;
          ack.reason = outcome.status.reason;
          ack.detail = outcome.status.detail;
          ack.generation = outcome.generation;
          ack.duplicate = outcome.duplicate;
        } else if (kind == static_cast<u32>(EvidenceKind::Reservation)) {
          ReservationSnapshot snapshot;
          if (!decode_reservation_snapshot(body, snapshot)) {
            (void)reject_with(connection, Status::failure(StatusCode::CorruptData, ReasonCode::FrameMalformed,
                                                          "reservation snapshot is malformed"));
            return;
          }
          const IngestOutcome outcome = governor_.ingest_reservations(snapshot);
          ack.code = outcome.status.code;
          ack.reason = outcome.status.reason;
          ack.detail = outcome.status.detail;
          ack.generation = outcome.generation;
          ack.duplicate = outcome.duplicate;
        } else if (kind == static_cast<u32>(EvidenceKind::Admission)) {
          AdmissionSnapshot snapshot;
          if (!decode_admission_snapshot(body, snapshot)) {
            (void)reject_with(connection, Status::failure(StatusCode::CorruptData, ReasonCode::FrameMalformed,
                                                          "admission snapshot is malformed"));
            return;
          }
          const IngestOutcome outcome = governor_.ingest_admissions(snapshot);
          ack.code = outcome.status.code;
          ack.reason = outcome.status.reason;
          ack.detail = outcome.status.detail;
          ack.generation = outcome.generation;
          ack.duplicate = outcome.duplicate;
        } else {
          (void)reject_with(connection, Status::failure(StatusCode::InvalidArgument, ReasonCode::EvidenceMalformed,
                                                        "unknown evidence kind"));
          return;
        }
        std::vector<u8> encoded;
        encode_publish_ack(ack, encoded);
        if (!connection.send_frame(static_cast<u32>(MessageType::PublishAck), encoded).ok()) return;
        break;
      }
      case MessageType::Evaluate: {
        ByteReader reader(payload);
        u64 domain = 0;
        u64 tick = 0;
        if (!reader.u64(domain) || !reader.u64(tick) || !reader.at_end()) {
          (void)reject_with(connection, Status::failure(StatusCode::CorruptData, ReasonCode::FrameMalformed,
                                                        "EVALUATE payload is malformed"));
          return;
        }
        EvaluationRequest request;
        request.domain = OversubscriptionDomainId(domain);
        request.tick = tick;
        Decision decision;
        const Status evaluated = governor_.evaluate(request, decision);
        if (!evaluated.ok()) {
          (void)reject_with(connection, evaluated);
          return;
        }
        std::vector<u8> encoded;
        encode_decision(decision, encoded);
        if (!connection.send_frame(static_cast<u32>(MessageType::DecisionMessage), encoded).ok()) return;
        break;
      }
      case MessageType::Revalidate: {
        Decision decision;
        if (!decode_revalidate_request(payload, decision)) {
          (void)reject_with(connection, Status::failure(StatusCode::CorruptData, ReasonCode::FrameMalformed,
                                                        "REVALIDATE payload is malformed"));
          return;
        }
        RevalidationReport report;
        const Status revalidated = governor_.revalidate(decision, report);
        RevalidateAck ack;
        if (!revalidated.ok()) {
          ack.valid = false;
          ack.reason = revalidated.reason;
          ack.detail = revalidated.detail;
        } else {
          ack.valid = report.valid;
          ack.reason = report.reason;
          ack.detail = report.detail;
        }
        std::vector<u8> encoded;
        encode_revalidate_ack(ack, encoded);
        if (!connection.send_frame(static_cast<u32>(MessageType::RevalidateAck), encoded).ok()) return;
        break;
      }
      case MessageType::FencePublisher: {
        ByteReader reader(payload);
        u64 publisher = 0;
        u32 reason = 0;
        if (!reader.u64(publisher) || !reader.u32(reason) || !reader.at_end()) {
          (void)reject_with(connection, Status::failure(StatusCode::CorruptData, ReasonCode::FrameMalformed,
                                                        "FENCE_PUBLISHER payload is malformed"));
          return;
        }
        const Status fenced = governor_.fence_publisher(PublisherId(publisher), static_cast<ReasonCode>(reason));
        std::vector<u8> encoded;
        encode_status(fenced, encoded);
        if (!connection.send_frame(static_cast<u32>(MessageType::PublishAck), encoded).ok()) return;
        break;
      }
      case MessageType::Shutdown: {
        std::vector<u8> encoded;
        encode_status(Status::success(), encoded);
        (void)connection.send_frame(static_cast<u32>(MessageType::ShutdownAck), encoded);
        connection.close();
        request_stop();
        return;
      }
      default:
        (void)reject_with(connection, Status::failure(StatusCode::InvalidArgument, ReasonCode::ProtocolViolation,
                                                      "unexpected message type"));
        return;
    }
  }
}

}  // namespace dist
}  // namespace oversub
