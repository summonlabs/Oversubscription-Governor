// Oversubscription Governor — corrective intent adapters.
//
// The governor never performs adjacent-system actions. It produces bounded
// intent, and the operator wires an adapter that forwards that intent to the
// system that owns the action (admission, capacity borrowing, burst pools,
// headroom). The reference adapters here record or count intent; they do not
// touch any external system.
#ifndef OVERSUB_ADAPTERS_HPP
#define OVERSUB_ADAPTERS_HPP

#include <string>
#include <vector>

#include "oversub/governor.hpp"

namespace oversub {

class CorrectiveIntentSink {
 public:
  virtual ~CorrectiveIntentSink() = default;
  // Called outside every governor lock, in the order the intents were produced.
  virtual Status deliver(const Decision& decision, const CorrectiveIntent& intent) = 0;
};

// Records delivered intent for inspection, replay, or hand-off. The buffer is
// bounded; once full, further deliveries are counted and dropped.
class RecordingIntentSink : public CorrectiveIntentSink {
 public:
  explicit RecordingIntentSink(std::size_t capacity = 256) : capacity_(capacity == 0 ? 1 : capacity) {}

  Status deliver(const Decision& decision, const CorrectiveIntent& intent) override {
    ++delivered_;
    if (records_.size() >= capacity_) {
      ++dropped_;
      return Status::failure(StatusCode::OutOfRange, ReasonCode::LimitExceeded,
                             "intent record buffer is full");
    }
    Record record;
    record.decision = decision.id;
    record.kind = intent.kind;
    record.target = intent.target;
    record.domain = intent.domain;
    record.service_class = intent.service_class;
    record.resource = intent.resource;
    record.amount_units = intent.amount_units;
    record.reason = intent.reason;
    records_.push_back(record);
    return Status::success();
  }

  struct Record {
    DecisionId decision;
    CorrectiveKind kind{CorrectiveKind::None};
    CorrectiveTarget target{CorrectiveTarget::Domain};
    OversubscriptionDomainId domain;
    ServiceClassId service_class;
    ResourceId resource;
    u64 amount_units{0};
    ReasonCode reason{ReasonCode::None};
  };

  const std::vector<Record>& records() const { return records_; }
  u64 delivered() const { return delivered_; }
  u64 dropped() const { return dropped_; }
  void clear() {
    records_.clear();
    delivered_ = 0;
    dropped_ = 0;
  }

 private:
  std::size_t capacity_;
  std::vector<Record> records_;
  u64 delivered_{0};
  u64 dropped_{0};
};

// Delivers every intent of one decision, in order. Stops at the first delivery
// failure and reports it; already-delivered intent is not rolled back because
// delivery belongs to the adjacent system.
inline Status dispatch_corrective_intent(const Decision& decision, CorrectiveIntentSink& sink,
                                         u32 max_intents = kHardMaxCorrectiveIntents) {
  u32 delivered = 0;
  for (const auto& intent : decision.evaluation.intents) {
    if (intent.kind == CorrectiveKind::None) continue;
    if (delivered >= max_intents) {
      return Status::failure(StatusCode::OutOfRange, ReasonCode::LimitExceeded,
                             "corrective intent dispatch budget exhausted");
    }
    Status status = sink.deliver(decision, intent);
    if (!status.ok()) return status;
    ++delivered;
  }
  return Status::success();
}

}  // namespace oversub

#endif  // OVERSUB_ADAPTERS_HPP
