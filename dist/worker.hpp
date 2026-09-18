// Oversubscription Governor — publisher (client) role.
#ifndef OVERSUB_DIST_WORKER_HPP
#define OVERSUB_DIST_WORKER_HPP

#include <string>

#include "oversub/net.hpp"
#include "protocol.hpp"

namespace oversub {
namespace dist {

struct WorkerOptions {
  std::string address{"127.0.0.1"};
  u16 port{0};
  PublisherId publisher;
  IncarnationId incarnation;
  OversubscriptionDomainId domain;
  bool adopt_incarnation{false};
  u64 tick{0};
};

// A publisher client. Every evidence publication carries explicit provenance:
// publisher, incarnation, epoch, a monotonically increasing sequence, and a
// stable evidence id used for idempotent retransmission.
class WorkerClient {
 public:
  WorkerClient() = default;
  WorkerClient(const WorkerClient&) = delete;
  WorkerClient& operator=(const WorkerClient&) = delete;

  Status connect(const WorkerOptions& options);
  void close();
  const HelloResponse& hello() const { return hello_; }
  bool connected() const { return !connection_.closed() && sequence_valid_; }

  Status publish_capacity(CapacitySnapshot& snapshot, PublishAck& ack);
  Status publish_reservations(ReservationSnapshot& snapshot, PublishAck& ack);
  Status publish_admissions(AdmissionSnapshot& snapshot, PublishAck& ack);
  Status evaluate(OversubscriptionDomainId domain, u64 tick, Decision& out);
  Status revalidate(const Decision& decision, RevalidateAck& ack);
  Status fence_publisher(PublisherId publisher, ReasonCode reason, Status& remote_status);
  Status shutdown();

  u64 sequence() const { return sequence_; }
  void set_sequence(u64 sequence) { sequence_ = sequence; }
  void set_evidence_counter(u64 value) { evidence_counter_ = value; }
  // Publisher-side epoch claim. Unset means "use the epoch the coordinator
  // reported"; an explicit value is published verbatim so a stale or future
  // epoch is rejected by the coordinator rather than silently corrected.
  void set_epoch_override(FabricEpoch epoch) { epoch_override_ = epoch; }

 private:
  Status exchange(u32 request_type, const std::vector<u8>& payload, u32 expected_type,
                  std::vector<u8>& response);
  bool consume_remote_rejection(Status& out);
  void stamp_provenance(Provenance& provenance, u64 observed_tick);
  FabricEpoch claimed_epoch() const;

  FramedConnection connection_;
  HelloResponse hello_;
  PublisherId publisher_;
  IncarnationId incarnation_;
  u64 sequence_{0};
  u64 evidence_counter_{0};
  bool sequence_valid_{false};
  bool remote_rejected_{false};
  Status remote_status_{};
  FabricEpoch epoch_override_;
};

}  // namespace dist
}  // namespace oversub

#endif  // OVERSUB_DIST_WORKER_HPP
