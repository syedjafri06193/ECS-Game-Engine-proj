#pragma once
// Work-stealing job scheduler (design doc §5).
//
// One worker per hardware thread; the constructing thread is worker 0 and
// participates whenever it waits. Jobs are small type-erased callables stored
// inline (no heap allocation per job). Waiting threads help: they execute
// jobs until their counter reaches zero, so nested waits never deadlock.
//
// Quiesce() (§3.4) waits for EVERY in-flight job - including ones sitting in
// worker deques that will run after submission stops - before a module can
// be unloaded. External submissions block while quiesced; submissions from
// inside running jobs are allowed so in-flight work can finish.
#include <atomic>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <type_traits>
#include <vector>
#include "core/base.h"
#include "jobs/deque.h"

namespace engine {

class Scheduler;

// Counts outstanding jobs. Padded: counters are hammered by many workers.
struct alignas(kCacheLine) JobCounter {
    std::atomic<int64_t> pending{0};
    bool Done() const { return pending.load(std::memory_order_acquire) == 0; }
};

struct alignas(kCacheLine) Job {
    static constexpr size_t kStorage = 96;
    void (*invoke)(Job&) = nullptr;
    JobCounter* counter = nullptr;
    std::atomic<bool> in_use{false};
    alignas(16) std::byte storage[kStorage];
};

class Scheduler {
public:
    explicit Scheduler(unsigned worker_count = std::thread::hardware_concurrency());
    ~Scheduler();
    Scheduler(const Scheduler&) = delete;
    Scheduler& operator=(const Scheduler&) = delete;

    unsigned WorkerCount() const { return unsigned(workers_.size()); }

    // Submit a callable. `counter` (optional) is incremented now and
    // decremented when the job finishes.
    template <typename F>
    void Submit(F&& fn, JobCounter* counter = nullptr)
    {
        using Fn = std::decay_t<F>;
        static_assert(sizeof(Fn) <= Job::kStorage, "job capture too large; capture a pointer instead");
        static_assert(alignof(Fn) <= 16, "job capture over-aligned");
        static_assert(std::is_nothrow_move_constructible_v<Fn> || std::is_trivially_copyable_v<Fn>);
        Job* job = AllocateJob();
        new (job->storage) Fn(std::forward<F>(fn));
        job->invoke = [](Job& j) {
            Fn* f = std::launder(reinterpret_cast<Fn*>(j.storage));
            (*f)();
            f->~Fn();
        };
        job->counter = counter;
        Enqueue(job);
    }

    // Blocks until `counter` reaches zero, executing jobs meanwhile.
    void Wait(JobCounter& counter);

    // fn(begin, end) over [0, count) in batches of `batch`.
    void ParallelFor(size_t count, size_t batch, const std::function<void(size_t, size_t)>& fn);

    // Stop accepting external work and wait for every in-flight job (§3.4).
    void Quiesce();
    void Resume();
    bool Quiesced() const { return quiesced_.load(std::memory_order_acquire); }
    int64_t InFlight() const { return inflight_.load(std::memory_order_acquire); }

    // Index of the calling thread in this scheduler, or -1.
    int CurrentWorker() const;

    struct Stats { uint64_t executed = 0, stolen = 0, inline_overflow = 0; };
    Stats GetStats() const;

private:
    struct alignas(kCacheLine) Worker {
        WorkStealingDeque<Job> deque{8192};
        std::vector<std::unique_ptr<Job>> ring;       // job storage owned by this thread's submissions
        size_t ring_next = 0;
        std::atomic<uint64_t> executed{0}, stolen{0}, overflow{0};
        std::thread thread;
    };

    Job* AllocateJob();
    void Enqueue(Job* job);
    void Execute(Job* job);
    Job* FindWork(int self);
    void WorkerLoop(int index);
    void NotifyWork();

    std::vector<std::unique_ptr<Worker>> workers_;
    std::mutex inject_mutex_;
    std::deque<Job*> injected_;                       // from threads that aren't workers
    std::vector<std::unique_ptr<Job>> external_ring_; // job storage for external submitters
    size_t external_next_ = 0;
    std::mutex external_alloc_mutex_;

    alignas(kCacheLine) std::atomic<int64_t> inflight_{0};
    alignas(kCacheLine) std::atomic<uint32_t> work_epoch_{0};
    std::atomic<int> sleepers_{0};
    std::atomic<bool> stop_{false};
    std::atomic<bool> quiesced_{false};
    std::mutex quiesce_mutex_;
    std::condition_variable quiesce_cv_;
};

// Increments a counter for a group of jobs and waits on it at scope exit.
class JobGroup {
public:
    explicit JobGroup(Scheduler& s) : s_(s) {}
    ~JobGroup() { Wait(); }
    JobGroup(const JobGroup&) = delete;
    JobGroup& operator=(const JobGroup&) = delete;
    template <typename F> void Run(F&& f) { s_.Submit(std::forward<F>(f), &counter_); }
    void Wait() { s_.Wait(counter_); }
private:
    Scheduler& s_;
    JobCounter counter_;
};

}  // namespace engine
