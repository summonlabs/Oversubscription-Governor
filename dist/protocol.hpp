// Oversubscription Governor — coordinator/publisher wire protocol.
//
// Every message is a bounded frame on a real TCP stream. The protocol carries
// evidence, decisions, and governance commands; it never carries durable state
// implicitly, and the coordinator is the only authority for generations.
#ifndef OVERSUB_DIST_PROTOCOL_HPP
#define OVERSUB_DIST_PROTOCOL_HPP

#include <string>
#include <vector>

#include "oversub/bytes.hpp"
#include "oversub/governor.hpp"
#include "oversub/net.hpp"

namespace oversub {
namespace dist {

enum class MessageType : u32 {
  Hello = 1,
  HelloAck = 2,
  Reject = 3,
  Publish = 4,
  PublishAck = 5,
  Evaluate = 6,
  DecisionMessage = 7,
  Revalidate = 8,
  RevalidateAck = 9,
  FencePublisher = 10,
  Shutdown = 11,
  ShutdownAck = 12,
};

const char* to_string(MessageType type);

inline constexpr u32 kMaxDetailLength = 256;
inline constexpr u32 kMaxWireIntents = 16;
inline constexpr u32 kMaxWireResources = 64;

struct HelloRequest {
  u32 protocol_version{kProtocolVersion};
  PublisherId publisher;
  IncarnationId incarnation;
  OversubscriptionDomainId domain;   // evidence domain this publisher serves
  bool adopt_incarnation{false};
  u64 tick{0};
};

struct HelloResponse {
  u32 protocol_version{kProtocolVersion};
  FabricEpoch epoch;
  PolicyGeneration policy_generation;
  u64 capacity_generation{0};
  u64 reservation_generation{0};
  u64 admission_generation{0};
  u64 last_sequence{0};   // highest accepted sequence for this publisher
};

struct PublishAck {
  StatusCode code{StatusCode::Ok};
  ReasonCode reason{ReasonCode::None};
  u64 generation{0};
  bool duplicate{false};
  std::string detail;
};

struct RevalidateAck {
  bool valid{false};
  ReasonCode reason{ReasonCode::None};
  std::string detail;
};

void encode_hello(const HelloRequest& request, std::vector<u8>& out);
bool decode_hello(const std::vector<u8>& in, HelloRequest& out);
void encode_hello_response(const HelloResponse& response, std::vector<u8>& out);
bool decode_hello_response(const std::vector<u8>& in, HelloResponse& out);
void encode_status(const Status& status, std::vector<u8>& out);
bool decode_status(const std::vector<u8>& in, Status& out);
void encode_publish_ack(const PublishAck& ack, std::vector<u8>& out);
bool decode_publish_ack(const std::vector<u8>& in, PublishAck& out);
void encode_revalidate_ack(const RevalidateAck& ack, std::vector<u8>& out);
bool decode_revalidate_ack(const std::vector<u8>& in, RevalidateAck& out);

void encode_capacity_snapshot(const CapacitySnapshot& snapshot, std::vector<u8>& out);
bool decode_capacity_snapshot(const std::vector<u8>& in, CapacitySnapshot& out);
void encode_reservation_snapshot(const ReservationSnapshot& snapshot, std::vector<u8>& out);
bool decode_reservation_snapshot(const std::vector<u8>& in, ReservationSnapshot& out);
void encode_admission_snapshot(const AdmissionSnapshot& snapshot, std::vector<u8>& out);
bool decode_admission_snapshot(const std::vector<u8>& in, AdmissionSnapshot& out);

void encode_decision(const Decision& decision, std::vector<u8>& out);
bool decode_decision(const std::vector<u8>& in, Decision& out);
void encode_revalidate_request(const Decision& decision, std::vector<u8>& out);
bool decode_revalidate_request(const std::vector<u8>& in, Decision& out);

}  // namespace dist
}  // namespace oversub

#endif  // OVERSUB_DIST_PROTOCOL_HPP
