// Oversubscription Governor — strongly typed identities, generations, epochs.
#ifndef OVERSUB_IDS_HPP
#define OVERSUB_IDS_HPP

#include <functional>
#include <string>

#include "oversub/checked.hpp"

namespace oversub {

// Strongly typed identity. Value 0 means "unset"; identities issued by the
// runtime start at 1.
template <class Tag>
class StrongId {
 public:
  constexpr StrongId() = default;
  constexpr explicit StrongId(u64 value) : value_(value) {}
  constexpr u64 value() const { return value_; }
  constexpr bool valid() const { return value_ != 0; }
  friend constexpr bool operator==(const StrongId& a, const StrongId& b) { return a.value_ == b.value_; }
  friend constexpr bool operator!=(const StrongId& a, const StrongId& b) { return a.value_ != b.value_; }
  friend constexpr bool operator<(const StrongId& a, const StrongId& b) { return a.value_ < b.value_; }
  friend constexpr bool operator>(const StrongId& a, const StrongId& b) { return a.value_ > b.value_; }
  std::string to_string() const { return std::to_string(value_); }

 private:
  u64 value_{0};
};

template <class Tag>
struct StrongIdHash {
  std::size_t operator()(const StrongId<Tag>& id) const noexcept {
    u64 x = id.value();
    x ^= x >> 33;
    x *= 0xFF51AFD7ED558CCDULL;
    x ^= x >> 33;
    x *= 0xC4CEB9FE1A85EC53ULL;
    x ^= x >> 33;
    return static_cast<std::size_t>(x);
  }
};

// Monotonic generation. Generation 0 means "never issued". Generations only ever
// advance; the runtime binds every authoritative decision to the exact
// generations that justified it.
template <class Tag>
class Gen {
 public:
  constexpr Gen() = default;
  constexpr explicit Gen(u64 value) : value_(value) {}
  constexpr u64 value() const { return value_; }
  constexpr bool valid() const { return value_ != 0; }
  friend constexpr bool operator==(const Gen& a, const Gen& b) { return a.value_ == b.value_; }
  friend constexpr bool operator!=(const Gen& a, const Gen& b) { return a.value_ != b.value_; }
  friend constexpr bool operator<(const Gen& a, const Gen& b) { return a.value_ < b.value_; }
  friend constexpr bool operator>(const Gen& a, const Gen& b) { return a.value_ > b.value_; }
  friend constexpr bool operator<=(const Gen& a, const Gen& b) { return a.value_ <= b.value_; }
  friend constexpr bool operator>=(const Gen& a, const Gen& b) { return a.value_ >= b.value_; }
  std::string to_string() const { return std::to_string(value_); }

 private:
  u64 value_{0};
};

template <class Tag>
struct GenHash {
  std::size_t operator()(const Gen<Tag>& g) const noexcept {
    return StrongIdHash<Tag>{}(StrongId<Tag>(g.value()));
  }
};

using OversubscriptionDomainId = StrongId<struct DomainIdTag>;
using ResourceId = StrongId<struct ResourceIdTag>;
using PoolId = StrongId<struct PoolIdTag>;
using OversubscriptionPolicyId = StrongId<struct PolicyIdTag>;
using RiskBudgetId = StrongId<struct RiskBudgetIdTag>;
using ServiceClassId = StrongId<struct ServiceClassIdTag>;
using ReservationId = StrongId<struct ReservationIdTag>;
using AdmissionId = StrongId<struct AdmissionIdTag>;
using EvidenceId = StrongId<struct EvidenceIdTag>;
using PublisherId = StrongId<struct PublisherIdTag>;
using DecisionId = StrongId<struct DecisionIdTag>;
using IncarnationId = StrongId<struct IncarnationIdTag>;
using AttemptId = StrongId<struct AttemptIdTag>;
using AuditRecordId = StrongId<struct AuditRecordIdTag>;

using DomainGeneration = Gen<struct DomainGenTag>;
using PolicyGeneration = Gen<struct PolicyGenTag>;
using ResourceGeneration = Gen<struct ResourceGenTag>;
using CapacitySnapshotGeneration = Gen<struct CapacitySnapshotGenTag>;
using ReservationSnapshotGeneration = Gen<struct ReservationSnapshotGenTag>;
using AdmissionSnapshotGeneration = Gen<struct AdmissionSnapshotGenTag>;
using ReservationGeneration = Gen<struct ReservationGenTag>;
using AdmissionGeneration = Gen<struct AdmissionGenTag>;
using RiskBudgetGeneration = Gen<struct RiskBudgetGenTag>;
using StateRevision = Gen<struct StateRevisionTag>;

template <class Tag>
StrongId<Tag> next_id(StrongId<Tag> current) {
  return StrongId<Tag>(current.value() + 1);
}

template <class Tag>
Gen<Tag> next_gen(Gen<Tag> current) {
  return Gen<Tag>(current.value() + 1);
}

// Fabric epoch: the fabric-wide authority epoch. Evidence, decisions, and
// publisher registrations are all fenced by it. Advancing the epoch invalidates
// every prior binding.
class FabricEpoch {
 public:
  constexpr FabricEpoch() = default;
  constexpr explicit FabricEpoch(u64 value) : value_(value) {}
  constexpr u64 value() const { return value_; }
  constexpr bool valid() const { return value_ != 0; }
  friend constexpr bool operator==(const FabricEpoch& a, const FabricEpoch& b) { return a.value_ == b.value_; }
  friend constexpr bool operator!=(const FabricEpoch& a, const FabricEpoch& b) { return a.value_ != b.value_; }
  friend constexpr bool operator<(const FabricEpoch& a, const FabricEpoch& b) { return a.value_ < b.value_; }
  std::string to_string() const { return std::to_string(value_); }

 private:
  u64 value_{0};
};

// Reference to an external priority/QoS system. The governor never assigns
// priorities; it only carries the reference so decisions can be explained in the
// terms of the system that owns them.
struct PriorityRef {
  u32 value{0};
  friend constexpr bool operator==(const PriorityRef& a, const PriorityRef& b) { return a.value == b.value; }
  friend constexpr bool operator!=(const PriorityRef& a, const PriorityRef& b) { return a.value != b.value; }
  friend constexpr bool operator<(const PriorityRef& a, const PriorityRef& b) { return a.value < b.value; }
  std::string to_string() const { return std::to_string(value); }
};

}  // namespace oversub

#endif  // OVERSUB_IDS_HPP
