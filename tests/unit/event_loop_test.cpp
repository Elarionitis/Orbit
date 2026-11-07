#include "net/event_loop.h"

#include <gtest/gtest.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <future>
#include <thread>
#include <vector>

#include "common/clock.h"

using namespace orbit::net;

namespace {

class LoopThread {
public:
    LoopThread() : m_thread([this] { loop.run(); }) {}
    ~LoopThread() {
        loop.stop();
        m_thread.join();
    }
    EventLoop loop;

private:
    std::thread m_thread;
};

class FlagHandler final : public IoHandler {
public:
    void on_io(bool readable, bool, bool) override {
        // Level-triggered: this fires repeatedly until the byte is read.
        if (readable && !m_seen.exchange(true)) {
            ready.set_value();
        }
    }
    std::promise<void> ready;

private:
    std::atomic<bool> m_seen{false};
};

}  // namespace

TEST(EventLoop, RunsPostedTasks) {
    LoopThread thread;
    std::promise<std::thread::id> ran_on;
    thread.loop.post([&] { ran_on.set_value(std::this_thread::get_id()); });
    EXPECT_NE(ran_on.get_future().get(), std::this_thread::get_id());
}

TEST(EventLoop, PreservesPerProducerOrder) {
    constexpr int kProducers = 4;
    constexpr int kPerProducer = 20'000;

    LoopThread thread;
    std::vector<int> last_seen(kProducers, -1);
    std::atomic<bool> in_order{true};
    std::atomic<int> total{0};

    std::vector<std::thread> producers;
    for (int producer = 0; producer < kProducers; ++producer) {
        producers.emplace_back([&, producer] {
            for (int i = 0; i < kPerProducer; ++i) {
                thread.loop.post([&, producer, i] {
                    if (i != last_seen[static_cast<std::size_t>(producer)] + 1) {
                        in_order = false;
                    }
                    last_seen[static_cast<std::size_t>(producer)] = i;
                    total.fetch_add(1);
                });
            }
        });
    }
    for (auto& producer : producers) {
        producer.join();
    }

    std::promise<void> drained;
    thread.loop.post([&] { drained.set_value(); });
    drained.get_future().wait();

    EXPECT_TRUE(in_order.load());
    EXPECT_EQ(total.load(), kProducers * kPerProducer);
}

TEST(EventLoop, TimerFiresAfterDelay) {
    LoopThread thread;
    std::promise<std::uint64_t> fired;
    const auto start = orbit::monotonic_ms();
    thread.loop.post([&] {
        thread.loop.run_after(50, [&] { fired.set_value(orbit::monotonic_ms()); });
    });
    EXPECT_GE(fired.get_future().get() - start, 40U);
}

TEST(EventLoop, CancelledTimerStaysSilent) {
    LoopThread thread;
    std::atomic<bool> fired{false};
    thread.loop.post([&] {
        const auto id = thread.loop.run_after(20, [&] { fired = true; });
        thread.loop.cancel_timer(id);
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(80));
    EXPECT_FALSE(fired.load());
}

TEST(EventLoop, DeferredWorkRunsAfterTasksAndMayDeferAgain) {
    LoopThread thread;
    std::promise<std::vector<int>> result;
    auto order = std::make_shared<std::vector<int>>();
    thread.loop.post([&, order] {
        thread.loop.defer([&, order] {
            order->push_back(2);
            thread.loop.defer([&, order] {
                order->push_back(3);
                result.set_value(*order);
            });
        });
        order->push_back(1);
    });
    EXPECT_EQ(result.get_future().get(), (std::vector<int>{1, 2, 3}));
}

TEST(EventLoop, DispatchesSocketReadiness) {
    std::array<int, 2> fds{};
    ASSERT_EQ(::pipe(fds.data()), 0);
    UniqueFd reader(fds[0]);
    UniqueFd writer(fds[1]);

    LoopThread thread;
    FlagHandler handler;
    auto ready = handler.ready.get_future();
    thread.loop.post([&] { thread.loop.add(reader.get(), kInterestRead, &handler); });

    ASSERT_EQ(::write(writer.get(), "x", 1), 1);
    EXPECT_EQ(ready.wait_for(std::chrono::seconds(5)), std::future_status::ready);

    std::promise<void> removed;
    thread.loop.post([&] {
        thread.loop.remove(reader.get());
        removed.set_value();
    });
    removed.get_future().wait();
}

TEST(EventLoop, StopFromAnotherThreadWakesASleepingLoop) {
    EventLoop loop;
    std::thread thread([&] { loop.run(); });
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    const auto start = orbit::monotonic_ms();
    loop.stop();
    thread.join();
    EXPECT_LT(orbit::monotonic_ms() - start, 1000U);
}

TEST(EventLoop, TaskPostedBeforeStopStillRuns) {
    EventLoop loop;
    std::atomic<bool> ran{false};
    loop.post([&] { ran = true; });
    loop.stop();
    loop.run();
    EXPECT_TRUE(ran.load());
}
