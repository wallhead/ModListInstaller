#include "extractor/VerifiedBlockScheduler.h"

#include <array>
#include <iostream>
#include <stdexcept>

using modlist::detail::VerifiedBlock;
using modlist::detail::VerifiedBlockScheduler;

namespace {

void Expect(bool condition, const char* message) {
  if (!condition) {
    throw std::runtime_error(message);
  }
}

struct Fixture {
  struct Buffers {
    size_t live{0};
    size_t peak{0};
  };

  std::shared_ptr<Buffers> buffers = std::make_shared<Buffers>();
  VerifiedBlockScheduler scheduler;
  std::array<size_t, 32> loads{};

  explicit Fixture(size_t count = 20) : scheduler(count) {}

  void Complete(size_t index) {
    Expect(index < loads.size(), "Unexpected chunk index");
    ++loads[index];
    const auto count = buffers;
    ++count->live;
    count->peak = std::max(count->peak, count->live);
    std::shared_ptr<VerifiedBlock> block(new VerifiedBlock(), [count](auto* value) {
      --count->live;
      delete value;
    });
    block->data.assign(17, static_cast<uint8_t>(index));
    scheduler.Complete(index, std::move(block));
    Expect(count->peak <= 6, "Cached, loading and borrowed buffers must fit the six-buffer budget");
  }

  void Drain() {
    size_t operations = 0;
    while (const auto index = scheduler.Next()) {
      Expect(++operations < 100, "Scheduler failed to become idle");
      Complete(*index);
    }
  }
};

void TestCacheHitsAdvanceReadAhead() {
  Fixture f;
  f.scheduler.Request(0);
  f.Drain();
  Expect(f.loads[4] == 1, "Initial read-ahead should include chunk 4");
  f.scheduler.Request(1);
  Expect(f.scheduler.Find(1) != nullptr, "Chunk 1 should be cached");
  f.Drain();
  Expect(f.loads[5] == 1,
         "Consuming a cache hit must prefetch chunk 5 before a demand miss");
  for (size_t index = 2; index < 20; ++index) {
    f.scheduler.Request(index);
    Expect(f.scheduler.Find(index) != nullptr,
           "A fast reader must keep the next sequential demand cached");
    f.Drain();
  }
  for (size_t index = 0; index < 20; ++index) {
    Expect(f.loads[index] == 1, "Sequential input must load each chunk exactly once");
  }
}

void TestUnreadForwardBlocksSurviveEviction() {
  Fixture f;
  for (size_t index = 0; index <= 3; ++index) {
    f.scheduler.Request(index);
    f.Drain();
  }
  Expect(f.loads[7] == 1, "Chunk 3 consumption must advance read-ahead to chunk 7");
  Expect(f.scheduler.Find(4) != nullptr,
         "Loading chunk 7 must not evict upcoming unread chunk 4");
  Expect(f.loads[4] == 1, "Upcoming chunk 4 must not require a reload");
}

void TestDemandPromotionAndInFlightJoin() {
  Fixture f;
  f.scheduler.Request(0);
  Expect(f.scheduler.Next() == 0, "Initial demand must load first");
  f.scheduler.Request(0);
  Expect(!f.scheduler.Next(), "An in-flight demand must join its existing load");
  f.Complete(0);
  f.scheduler.Request(3);
  Expect(f.scheduler.Next() == 3, "Queued speculative chunk must become urgent on demand");
  f.scheduler.Request(3);
  Expect(!f.scheduler.Next(), "Only one source load may be in flight");
  f.Complete(3);
  f.Drain();
  Expect(f.loads[0] == 1 && f.loads[3] == 1,
         "Demand joining must not issue duplicate loads");
}

void TestSeekDuringLoadRebasesSpeculation() {
  Fixture f;
  f.scheduler.Request(0);
  f.Drain();
  f.scheduler.Request(14);
  Expect(f.scheduler.Next() == 14, "Forward seek must prioritize the new demand");
  f.scheduler.Request(2);
  f.Complete(14);
  auto tail = f.scheduler.Find(14);
  Expect(tail != nullptr && tail->data.front() == 14,
         "A seek must not change the identity of an in-flight result");
  tail.reset();
  Expect(f.scheduler.Next() == 5,
         "Backward seek must discard obsolete speculation and retain useful cached input");
  f.Complete(5);
  f.scheduler.ResetWindow();
  Expect(!f.scheduler.Next(), "Seeking to EOF must clear speculative work");
  f.scheduler.Request(19);
  Expect(f.scheduler.Next() == 19, "Last chunk must remain readable after EOF");
  f.Complete(19);
  Expect(!f.scheduler.Next(), "Read-ahead must stop at the final chunk");
}

void TestBorrowedBuffersApplyBackpressure() {
  Fixture f;
  std::array<std::shared_ptr<VerifiedBlock>, 6> borrowed;
  for (size_t index = 0; index < borrowed.size(); ++index) {
    f.scheduler.Request(index);
    Expect(f.scheduler.Next() == index, "Demand should precede speculative work");
    f.Complete(index);
    borrowed[index] = f.scheduler.Find(index);
  }
  f.scheduler.Request(6);
  Expect(!f.scheduler.Next(),
         "Six borrowed buffers must prevent allocating a seventh loading buffer");
  borrowed[0].reset();
  Expect(f.scheduler.Next() == 6, "Released obsolete buffer must unblock demand");
  f.Complete(6);
  Expect(f.buffers->peak == 6, "Borrowed-buffer test must exercise the full memory budget");
  for (size_t index = 1; index < borrowed.size(); ++index) {
    Expect(borrowed[index]->data.front() == index,
           "Eviction must not invalidate a borrowed chunk");
  }
}

void TestFinalVerificationCoversUntouchedChunks() {
  Fixture f;
  f.scheduler.Request(0);
  f.Drain();
  std::vector<bool> verified(20, false);
  for (size_t index = 0; index < 5; ++index) {
    verified[index] = true;
  }
  f.scheduler.VerifyRemaining(verified);
  f.Drain();
  for (size_t index = 0; index < 20; ++index) {
    Expect(f.loads[index] == 1,
           "Final verification must cover all untouched chunks without rereading verified ones");
  }
}

void TestDemandDisplacesSpeculationUnderBorrowedPressure() {
  Fixture f;
  std::array<std::shared_ptr<VerifiedBlock>, 2> borrowed;
  for (const size_t index : {0, 1, 3, 4, 5, 6}) {
    f.scheduler.Request(index);
    Expect(f.scheduler.Next() == index, "Fixture must load each requested chunk");
    f.Complete(index);
    if (index < 2) {
      borrowed[index] = f.scheduler.Find(index);
    }
  }
  f.scheduler.Request(2);
  Expect(f.scheduler.Next() == 2,
         "Essential demand must displace speculation when obsolete buffers are borrowed");
  f.Complete(2);
  Expect(f.scheduler.Find(3) != nullptr && f.scheduler.Find(6) == nullptr,
         "Memory pressure must displace the farthest prefetched chunk, not the next one");
}

void TestFinalVerificationJoinsInFlightSpeculation() {
  Fixture f;
  f.scheduler.Request(0);
  f.Complete(*f.scheduler.Next());
  Expect(f.scheduler.Next() == 1, "Read-ahead must start the next speculative load");
  std::vector<bool> verified(20, false);
  verified[0] = true;
  f.scheduler.VerifyRemaining(verified);
  Expect(!f.scheduler.Next(), "Final verification must join the existing speculative load");
  f.Complete(1);
  f.Drain();
  for (size_t index = 0; index < 20; ++index) {
    Expect(f.loads[index] == 1, "Final verification must not reload its in-flight chunk");
  }
}

void TestEmptyInputAndShortTail() {
  Fixture empty(0);
  empty.scheduler.Request(0);
  Expect(!empty.scheduler.Next(), "Empty input must not queue an invalid chunk");
  Fixture single(1);
  single.scheduler.Request(0);
  Expect(single.scheduler.Next() == 0, "One-chunk input must queue its demand");
  single.Complete(0);
  Expect(single.scheduler.Find(0)->data.size() == 17,
         "Short final chunks must retain their actual length");
  single.scheduler.Request(0);
  single.Drain();
  Expect(single.loads[0] == 1, "Repeated reads within one chunk must not reload it");
}

}  // namespace

int main() {
  try {
    TestCacheHitsAdvanceReadAhead();
    TestUnreadForwardBlocksSurviveEviction();
    TestDemandPromotionAndInFlightJoin();
    TestSeekDuringLoadRebasesSpeculation();
    TestBorrowedBuffersApplyBackpressure();
    TestDemandDisplacesSpeculationUnderBorrowedPressure();
    TestFinalVerificationCoversUntouchedChunks();
    TestFinalVerificationJoinsInFlightSpeculation();
    TestEmptyInputAndShortTail();
    std::cout << "All pipeline scheduler tests passed\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
