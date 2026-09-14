#include <cassert>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "../../net/native_exit_gate.h"
#include "application/profiler/viewer_update_worker.h"

namespace spark::detail {
struct DeadlineThreadTestAccess {
    static bool reaping(DeadlineThread &worker)
    {
        std::shared_ptr<DeadlineThread::Run> run;
        {
            std::scoped_lock lock(worker.mutex_);
            run = worker.run_;
        }
        if (!run) {
            return false;
        }
        std::scoped_lock lock(run->mutex);
        return run->reaping;
    }
};
}  // namespace spark::detail

namespace {

using namespace std::chrono_literals;

struct Probe {
    std::mutex mutex;
    std::condition_variable cv;
    std::vector<spark::ViewerUpdateWorker::WorkType> executed;
    std::optional<spark::ViewerUpdateWorker::Completion> completion;
    bool block_execution = false;
    bool execution_entered = false;
    bool release_execution = false;
    bool fail_next = false;
    spark::CancellationToken cancellation;
};

template <typename Predicate>
bool waitFor(Probe &probe, Predicate predicate)
{
    std::unique_lock lock(probe.mutex);
    return probe.cv.wait_for(lock, 2s, std::move(predicate));
}

bool waitUntilAvailable(spark::ViewerUpdateWorker &worker)
{
    const auto deadline = std::chrono::steady_clock::now() + 2s;
    while (!worker.available() && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::yield();
    }
    return worker.available();
}

void resetExecution(Probe &probe, bool block)
{
    std::scoped_lock lock(probe.mutex);
    probe.block_execution = block;
    probe.execution_entered = false;
    probe.release_execution = false;
}

}  // namespace

int main()
{
    {
        spark::test::NativeExitGate gate;
        spark::ViewerUpdateWorker worker(
            [&](const auto &) -> std::string {
                spark::test::holdNativeThreadExit(gate);
                throw std::runtime_error("exit gate");
            },
            [](const auto &) {});
        assert(worker.start());
        assert(worker.enqueueOpen({}, {}, "Exit"));
        gate.waitEntered();
        const auto begin = std::chrono::steady_clock::now();
        assert(!worker.stopUntil(begin + 20ms));
        assert(std::chrono::steady_clock::now() - begin < 250ms);
        assert(!worker.start());
        gate.unblock();
        assert(worker.stopUntil(std::chrono::steady_clock::now() + 2s));
        assert(worker.start());
        worker.stop();
    }
    {
        spark::detail::DeadlineThread worker;
        spark::test::NativeExitGate gate;
        std::atomic<bool> self_done{false};
        struct SelfExit {
            spark::detail::DeadlineThread &worker;
            spark::test::NativeExitGate &gate;
            std::atomic<bool> &done;
            ~SelfExit()
            {
                gate.block();
                const auto begin = std::chrono::steady_clock::now();
                assert(worker.isCurrentThread());
                assert(!worker.reapUntil(begin + 2s));
                assert(std::chrono::steady_clock::now() - begin < 100ms);
                done.store(true);
            }
        };
        std::atomic<bool> reaper_ready{false};
        std::atomic<bool> allow_reap{false};
        std::thread reaper([&] {
            reaper_ready.store(true);
            while (!allow_reap.load()) {
                std::this_thread::yield();
            }
            assert(worker.reapUntil(std::chrono::steady_clock::now() + 2s));
        });
        while (!reaper_ready.load()) {
            std::this_thread::yield();
        }
        assert(worker.start([&] { static thread_local SelfExit exit{worker, gate, self_done}; }));
        gate.waitEntered();
        allow_reap.store(true);
        const auto claim_deadline = std::chrono::steady_clock::now() + 2s;
        while (!spark::detail::DeadlineThreadTestAccess::reaping(worker) &&
               std::chrono::steady_clock::now() < claim_deadline) {
            std::this_thread::yield();
        }
        assert(spark::detail::DeadlineThreadTestAccess::reaping(worker));
        gate.unblock();
        reaper.join();
        assert(self_done.load());
        assert(!worker.joinable());
    }
    auto probe = std::make_shared<Probe>();
    spark::ViewerUpdateWorker worker(
        [probe](const spark::ViewerUpdateWorker::WorkItem &work) {
            std::unique_lock lock(probe->mutex);
            probe->executed.push_back(work.type);
            probe->cancellation = work.cancellation;
            probe->execution_entered = true;
            probe->cv.notify_all();
            if (probe->block_execution) {
                probe->cv.wait(lock, [&probe] { return probe->release_execution; });
            }
            const bool fail = probe->fail_next;
            probe->fail_next = false;
            lock.unlock();
            if (fail) {
                throw std::runtime_error("injected viewer worker failure");
            }
            return work.type == spark::ViewerUpdateWorker::WorkType::Open ? std::string("viewer-url")
                                                                          : std::string("update-key");
        },
        [probe](spark::ViewerUpdateWorker::Completion completion) {
            std::scoped_lock lock(probe->mutex);
            probe->completion = std::move(completion);
            probe->cv.notify_all();
        });

    assert(worker.start());
    assert(worker.start());

    resetExecution(*probe, true);
    const auto first_generation = worker.enqueueOpen({}, {}, "Console");
    assert(first_generation.has_value());
    assert(waitFor(*probe, [&] { return probe->execution_entered; }));
    assert(!worker.enqueueOpen({}, {}, "Second"));
    assert(!worker.enqueueCombined({}, {}, *first_generation));

    {
        std::scoped_lock lock(probe->mutex);
        probe->release_execution = true;
    }
    probe->cv.notify_all();
    assert(waitFor(*probe, [&] { return probe->completion.has_value(); }));
    {
        std::scoped_lock lock(probe->mutex);
        const auto completion = probe->completion;
        if (!completion) {
            return 1;
        }
        assert(completion->type == spark::ViewerUpdateWorker::WorkType::Open);
        assert(completion->generation == *first_generation);
        assert(completion->url == "viewer-url");
        assert(completion->sender_name == "Console");
    }
    assert(worker.openPending());
    assert(!worker.enqueueOpen({}, {}, "Still pending"));
    assert(worker.completeOpen(*first_generation));
    assert(!worker.openPending());

    resetExecution(*probe, true);
    const auto second_generation = worker.generation();
    assert(waitUntilAvailable(worker));
    assert(worker.enqueueCombined({}, {}, second_generation));
    assert(waitFor(*probe, [&] { return probe->execution_entered; }));
    worker.invalidate();
    {
        std::scoped_lock lock(probe->mutex);
        assert(probe->cancellation.stopRequested());
    }
    const auto stop_begin = std::chrono::steady_clock::now();
    assert(!worker.stopUntil(stop_begin + 20ms));
    assert(std::chrono::steady_clock::now() - stop_begin < 250ms);
    assert(!worker.start());
    const auto invalidated_generation = worker.generation();
    assert(invalidated_generation == second_generation + 1);
    assert(!worker.current(second_generation));
    assert(!worker.current(invalidated_generation));
    assert(!worker.completeOpen(second_generation));
    assert(!worker.enqueueCombined({}, {}, second_generation));
    {
        std::scoped_lock lock(probe->mutex);
        probe->release_execution = true;
    }
    probe->cv.notify_all();
    worker.stop();
    assert(worker.available());

    assert(worker.start());
    resetExecution(*probe, false);
    std::size_t executed_before_update = 0;
    {
        std::scoped_lock lock(probe->mutex);
        executed_before_update = probe->executed.size();
    }
    assert(worker.enqueueCombined({}, {}, invalidated_generation));
    assert(waitFor(*probe, [&] { return probe->executed.size() > executed_before_update; }));
    {
        std::scoped_lock lock(probe->mutex);
        assert(probe->executed.back() == spark::ViewerUpdateWorker::WorkType::Combined);
    }
    assert(waitUntilAvailable(worker));
    assert(worker.enqueueStatistics({}, {}, invalidated_generation));
    assert(waitFor(*probe, [&] { return probe->executed.size() > executed_before_update + 1; }));
    {
        std::scoped_lock lock(probe->mutex);
        assert(probe->executed.back() == spark::ViewerUpdateWorker::WorkType::Statistics);
    }
    assert(waitUntilAvailable(worker));
    assert(worker.enqueueSampler({}, {}, invalidated_generation));
    assert(waitFor(*probe, [&] { return probe->executed.size() > executed_before_update + 2; }));
    {
        std::scoped_lock lock(probe->mutex);
        assert(probe->executed.back() == spark::ViewerUpdateWorker::WorkType::Sampler);
    }
    worker.stop();
    assert(worker.available());

    assert(worker.start());
    {
        std::scoped_lock lock(probe->mutex);
        probe->fail_next = true;
    }
    assert(worker.enqueueCombined({}, {}, invalidated_generation));
    assert(waitFor(*probe, [&] { return probe->executed.size() > executed_before_update + 3; }));
    worker.stop();
    assert(!worker.current(invalidated_generation));
    assert(worker.consumeFailure());
    assert(!worker.consumeFailure());

    assert(worker.start());
    assert(worker.current(invalidated_generation));
    resetExecution(*probe, false);
    assert(worker.enqueueCombined({}, {}, invalidated_generation));
    assert(waitFor(*probe, [&] { return probe->executed.size() > executed_before_update + 4; }));
    worker.stop();
    assert(worker.available());
    assert(!worker.current(invalidated_generation));
    assert(!worker.enqueueCombined({}, {}, invalidated_generation));
    return 0;
}
