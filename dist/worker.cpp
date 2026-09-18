#include "worker.hpp"

namespace oversub {
namespace dist {
namespace {

void stamp(Provenance& provenance, PublisherId publisher, IncarnationId incarnation, FabricEpoch epoch,
           u64 sequence, u64 evidence_id, u64 observed_tick) {
  provenance.publisher = publisher;
  provenance.incarnation = incarnation;
  provenance.epoch = epoch;
  provenance.sequence = sequence;
  provenance.observed_tick = observed_tick;
  provenance.observed_wall_millis = wall_clock_millis();
  provenance.evidence_id = EvidenceId(evidence_id);
}

}  // namespace

Status WorkerClient::connect(const WorkerOptions& options) {
  if (!options.publisher.valid() || !options.incarnation.valid() || !options.domain.valid()) {
    return Status::failure(StatusCode::InvalidArgument, ReasonCode::InvalidIdentity,
                           "publisher, incarnation, and domain are required");
  }
  Socket socket;
  Status status = tcp_connect(options.address, options.port, socket);
  if (!status.ok()) return status;
  connection_ = FramedConnection(std::move(socket));
  connection_.set_peer(options.address + ":" + std::to_string(options.port));
  publisher_ = options.publisher;
  incarnation_ = options.incarnation;
  sequence_ = 0;
  evidence_counter_ = 0;
  sequence_valid_ = true;

  HelloRequest hello;
  hello.protocol_version = kProtocolVersion;
  hello.publisher = options.publisher;
  hello.incarnation = options.incarnation;
  hello.domain = options.domain;
  hello.adopt_incarnation = options.adopt_incarnation;
  hello.tick = options.tick;
  std::vector<u8> payload;
  encode_hello(hello, payload);
  std::vector<u8> response;
  status = exchange(static_cast<u32>(MessageType::Hello), payload, static_cast<u32>(MessageType::HelloAck),
                    response);
  if (!status.ok()) return status;
  if (consume_remote_rejection(status)) return status;
  if (!decode_hello_response(response, hello_)) {
    return Status::failure(StatusCode::CorruptData, ReasonCode::FrameMalformed, "HELLO_ACK is malformed");
  }
  if (hello_.protocol_version != kProtocolVersion) {
    return Status::failure(StatusCode::Unsupported, ReasonCode::PublisherProtocolMismatch,
                           "coordinator protocol version mismatch");
  }
  // Resume this publisher's sequence so retransmission stays idempotent and new
  // evidence is never mistaken for a replay.
  sequence_ = hello_.last_sequence;
  evidence_counter_ = hello_.last_sequence;
  return Status::success();
}

void WorkerClient::close() { connection_.close(); }

Status WorkerClient::exchange(u32 request_type, const std::vector<u8>& payload, u32 expected_type,
                              std::vector<u8>& response) {
  response.clear();
  remote_rejected_ = false;
  remote_status_ = Status{};
  Status status = connection_.send_frame(request_type, payload);
  if (!status.ok()) return status;
  u32 type = 0;
  std::vector<u8> received;
  status = connection_.recv_frame(type, received);
  if (!status.ok()) return status;
  if (type == static_cast<u32>(MessageType::Reject)) {
    Status remote;
    if (!decode_status(received, remote)) {
      return Status::failure(StatusCode::CorruptData, ReasonCode::FrameMalformed, "REJECT is malformed");
    }
    remote_rejected_ = true;
    remote_status_ = std::move(remote);
    return Status::success();
  }
  if (type != expected_type) {
    return Status::failure(StatusCode::Conflict, ReasonCode::ProtocolViolation,
                           "unexpected response type " + std::to_string(type));
  }
  response = std::move(received);
  return Status::success();
}

bool WorkerClient::consume_remote_rejection(Status& out) {
  if (!remote_rejected_) return false;
  out = remote_status_;
  remote_rejected_ = false;
  return true;
}

void WorkerClient::stamp_provenance(Provenance& provenance, u64 observed_tick) {
  ++sequence_;
  ++evidence_counter_;
  // An evidence id is an idempotency key for one publication. It is derived from
  // the publisher incarnation and the sequence so that a restarted publisher can
  // never collide with, or silently overwrite, evidence from a previous
  // incarnation of the same publisher.
  u64 evidence_id = 0;
  if (!mul_checked(incarnation_.value(), 1000003ULL, evidence_id)) evidence_id = 0;
  evidence_id = add_sat(evidence_id, evidence_counter_);
  if (evidence_id == 0) evidence_id = evidence_counter_;
  stamp(provenance, publisher_, incarnation_, epoch_override_.valid() ? epoch_override_ : hello_.epoch,
        sequence_, evidence_id, observed_tick);
}

FabricEpoch WorkerClient::claimed_epoch() const {
  return epoch_override_.valid() ? epoch_override_ : hello_.epoch;
}

Status WorkerClient::publish_capacity(CapacitySnapshot& snapshot, PublishAck& ack) {
  stamp_provenance(snapshot.provenance, snapshot.observed_tick);
  snapshot.epoch = claimed_epoch();
  snapshot.domain = snapshot.domain.valid() ? snapshot.domain : OversubscriptionDomainId(0);
  snapshot.provenance.payload_digest = capacity_snapshot_digest(snapshot);
  std::vector<u8> body;
  encode_capacity_snapshot(snapshot, body);
  ByteWriter w;
  w.u32(static_cast<u32>(EvidenceKind::Capacity));
  w.raw(body.data(), body.size());
  std::vector<u8> response;
  Status status = exchange(static_cast<u32>(MessageType::Publish), w.data(),
                           static_cast<u32>(MessageType::PublishAck), response);
  if (!status.ok()) return status;
  Status remote;
  if (consume_remote_rejection(remote)) {
    ack = PublishAck{};
    ack.code = remote.code;
    ack.reason = remote.reason;
    ack.detail = remote.detail;
    return Status::success();
  }
  if (!decode_publish_ack(response, ack)) {
    return Status::failure(StatusCode::CorruptData, ReasonCode::FrameMalformed, "PUBLISH_ACK is malformed");
  }
  if (ack.code == StatusCode::Ok) snapshot.generation = CapacitySnapshotGeneration(ack.generation);
  return Status::success();
}

Status WorkerClient::publish_reservations(ReservationSnapshot& snapshot, PublishAck& ack) {
  stamp_provenance(snapshot.provenance, snapshot.observed_tick);
  snapshot.epoch = claimed_epoch();
  snapshot.provenance.payload_digest = reservation_snapshot_digest(snapshot);
  std::vector<u8> body;
  encode_reservation_snapshot(snapshot, body);
  ByteWriter w;
  w.u32(static_cast<u32>(EvidenceKind::Reservation));
  w.raw(body.data(), body.size());
  std::vector<u8> response;
  Status status = exchange(static_cast<u32>(MessageType::Publish), w.data(),
                           static_cast<u32>(MessageType::PublishAck), response);
  if (!status.ok()) return status;
  Status remote;
  if (consume_remote_rejection(remote)) {
    ack = PublishAck{};
    ack.code = remote.code;
    ack.reason = remote.reason;
    ack.detail = remote.detail;
    return Status::success();
  }
  if (!decode_publish_ack(response, ack)) {
    return Status::failure(StatusCode::CorruptData, ReasonCode::FrameMalformed, "PUBLISH_ACK is malformed");
  }
  if (ack.code == StatusCode::Ok) snapshot.generation = ReservationSnapshotGeneration(ack.generation);
  return Status::success();
}

Status WorkerClient::publish_admissions(AdmissionSnapshot& snapshot, PublishAck& ack) {
  stamp_provenance(snapshot.provenance, snapshot.observed_tick);
  snapshot.epoch = claimed_epoch();
  snapshot.provenance.payload_digest = admission_snapshot_digest(snapshot);
  std::vector<u8> body;
  encode_admission_snapshot(snapshot, body);
  ByteWriter w;
  w.u32(static_cast<u32>(EvidenceKind::Admission));
  w.raw(body.data(), body.size());
  std::vector<u8> response;
  Status status = exchange(static_cast<u32>(MessageType::Publish), w.data(),
                           static_cast<u32>(MessageType::PublishAck), response);
  if (!status.ok()) return status;
  Status remote;
  if (consume_remote_rejection(remote)) {
    ack = PublishAck{};
    ack.code = remote.code;
    ack.reason = remote.reason;
    ack.detail = remote.detail;
    return Status::success();
  }
  if (!decode_publish_ack(response, ack)) {
    return Status::failure(StatusCode::CorruptData, ReasonCode::FrameMalformed, "PUBLISH_ACK is malformed");
  }
  if (ack.code == StatusCode::Ok) snapshot.generation = AdmissionSnapshotGeneration(ack.generation);
  return Status::success();
}

Status WorkerClient::evaluate(OversubscriptionDomainId domain, u64 tick, Decision& out) {
  ByteWriter w;
  w.u64(domain.value());
  w.u64(tick);
  std::vector<u8> response;
  Status status = exchange(static_cast<u32>(MessageType::Evaluate), w.data(),
                           static_cast<u32>(MessageType::DecisionMessage), response);
  if (!status.ok()) return status;
  if (consume_remote_rejection(status)) return status;
  if (!decode_decision(response, out)) {
    return Status::failure(StatusCode::CorruptData, ReasonCode::FrameMalformed, "DECISION is malformed");
  }
  return Status::success();
}

Status WorkerClient::revalidate(const Decision& decision, RevalidateAck& ack) {
  std::vector<u8> payload;
  encode_revalidate_request(decision, payload);
  std::vector<u8> response;
  Status status = exchange(static_cast<u32>(MessageType::Revalidate), payload,
                           static_cast<u32>(MessageType::RevalidateAck), response);
  if (!status.ok()) return status;
  if (consume_remote_rejection(status)) return status;
  if (!decode_revalidate_ack(response, ack)) {
    return Status::failure(StatusCode::CorruptData, ReasonCode::FrameMalformed, "REVALIDATE_ACK is malformed");
  }
  return Status::success();
}

Status WorkerClient::fence_publisher(PublisherId publisher, ReasonCode reason, Status& remote_status) {
  ByteWriter w;
  w.u64(publisher.value());
  w.u32(static_cast<u32>(reason));
  std::vector<u8> response;
  Status status = exchange(static_cast<u32>(MessageType::FencePublisher), w.data(),
                           static_cast<u32>(MessageType::PublishAck), response);
  if (!status.ok()) return status;
  if (consume_remote_rejection(remote_status)) return Status::success();
  PublishAck ack;
  if (!decode_publish_ack(response, ack)) {
    return Status::failure(StatusCode::CorruptData, ReasonCode::FrameMalformed, "FENCE ack is malformed");
  }
  remote_status = Status{ack.code, ack.reason, ack.detail};
  return Status::success();
}

Status WorkerClient::shutdown() {
  std::vector<u8> response;
  Status status = exchange(static_cast<u32>(MessageType::Shutdown), {}, static_cast<u32>(MessageType::ShutdownAck),
                           response);
  connection_.close();
  if (!status.ok()) return status;
  if (consume_remote_rejection(status)) return status;
  return Status::success();
}

}  // namespace dist
}  // namespace oversub
