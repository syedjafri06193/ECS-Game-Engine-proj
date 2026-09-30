#include "jobs/scheduler.h"
#include <chrono>
#include <random>
#include "core/platform/platform.h"

#if defined(__x86_64__) || defined(_M_X64)
#include <immintrin.h>
#define ENGINE_PAUSE() _mm_pause()
#else
#define ENGINE_PAUSE() std::this_thread::yield()
#endif

namespace engine {

namespace {
thread_local const Scheduler* t_scheduler = nullptr;
thread_local int t_worker = -1;
constexpr size_t kRingSize = 4096;
}  // namespace

Scheduler::Scheduler(unsigned worker_count)
{
    const unsigned n = worker_count == 0 ? 1u : worker_count;
    for (unsigned i = 0; i < n; ++i) workers_.push_back(std::make_unique<Worker>());

    // The constructing thread is worker 0.
    ENGINE_ASSERT(t_scheduler == nullptr, "a thread can own only one scheduler at a time");
    t_scheduler = this;
    t_worker = 0;

    for (unsigned i = 1; i < n; ++i)
        workers_[i]->thread = std::thread([this, i] { WorkerLoop(int(i)); });
}

Scheduler::~Scheduler()
{
    stop_.store(true, std::memory_order_release);
    work_epoch_.fetch_add(1, std::memory_order_acq_rel);
    work_epoch_.notify_all();
    for (auto& w : workers_)
        if (w->thread.joinable()) w->thread.join();
    if (t_scheduler == this) { t_scheduler = nullptr; t_worker = -1; }
}

int Scheduler::CurrentWorker() const { return t_scheduler == this ? t_worker : -1; }

Job* Scheduler::AllocateJob()
{
    const int self = CurrentWorker();
    Job* job = nullptr;
    if (self >= 0) {
        Worker& w = *workers_[size_t(self)];
        if (w.ring.size() < kRingSize) {
            w.ring.push_back(std::make_unique<Job>());
            job = w.ring.back().get();
        } else {
            job = w.ring[w.ring_next++ % kRingSize].get();
        }
    } else {
        std::lock_guard lock(external_alloc_mutex_);
        if (external_ring_.size() < kRingSize) {
            external_ring_.push_back(std::make_unique<Job>());
            job = external_ring_.back().get();
        } else {
            job = external_ring_[external_next_++ % kRingSize].get();
        }
    }
    // The ring wrapped onto a job that hasn't finished: help until it has.
    while (job->in_use.load(std::memory_order_acquire)) {
        if (Job* other = FindWork(self)) Execute(other);
        else std::this_thread::yield();
    }
    job->in_use.store(true, std::memory_order_relaxed);
    return job;
}

void Scheduler::Enqueue(Job* job)
{
    const int self = CurrentWorker();
    if (self >= 0) {
        // Submissions from workers (including jobs running during a quiesce)
        // are always accepted: in-flight work must be able to finish.
        inflight_.fetch_add(1, std::memory_order_acq_rel);
        if (job->counter) job->counter->pending.fetch_add(1, std::memory_order_acq_rel);
        if (!workers_[size_t(self)]->deque.Push(job)) {
            workers_[size_t(self)]->overflow.fetch_add(1, std::memory_order_relaxed);
            Execute(job);                                   // full: run inline, always safe
            return;
        }
    } else {
        // External thread: blocks while quiesced. The check and the in-flight
        // increment happen under the same mutex Quiesce() sets its flag under,
        // so no job can slip in after Quiesce() observed zero.
        std::unique_lock lock(quiesce_mutex_);
        quiesce_cv_.wait(lock, [&] { return !quiesced_.load(std::memory_order_acquire); });
        inflight_.fetch_add(1, std::memory_order_acq_rel);
        if (job->counter) job->counter->pending.fetch_add(1, std::memory_order_acq_rel);
        lock.unlock();
        std::lock_guard ilock(inject_mutex_);
        injected_.push_back(job);
    }
    NotifyWork();
}

void Scheduler::NotifyWork()
{
    work_epoch_.fetch_add(1, std::memory_order_acq_rel);
    if (sleepers_.load(std::memory_order_acquire) > 0) work_epoch_.notify_one();
}

void Scheduler::Execute(Job* job)
{
    ENGINE_ZONE("job");
    JobCounter* counter = job->counter;
    job->invoke(*job);
    job->in_use.store(false, std::memory_order_release);
    if (counter) counter->pending.fetch_sub(1, std::memory_order_acq_rel);
    if (inflight_.fetch_sub(1, std::memory_order_acq_rel) == 1) {
        work_epoch_.fetch_add(1, std::memory_order_acq_rel);   // wake anyone waiting on quiesce
    }
    const int self = CurrentWorker();
    if (self >= 0) workers_[size_t(self)]->executed.fetch_add(1, std::memory_order_relaxed);
}

Job* Scheduler::FindWork(int self)
{
    if (self >= 0)
        if (Job* j = workers_[size_t(self)]->deque.Pop()) return j;

    {
        std::unique_lock lock(inject_mutex_, std::try_to_lock);
        if (lock.owns_lock() && !injected_.empty()) {
            Job* j = injected_.front();
            injected_.pop_front();
            return j;
        }
    }

    const size_t n = workers_.size();
    if (n <= 1 && self >= 0) return nullptr;
    thread_local std::minstd_rand rng(std::random_device{}());
    const size_t start = rng() % n;
    for (size_t k = 0; k < n; ++k) {
        const size_t victim = (start + k) % n;
        if (int(victim) == self) continue;
        if (Job* j = workers_[victim]->deque.Steal()) {
            if (self >= 0) workers_[size_t(self)]->stolen.fetch_add(1, std::memory_order_relaxed);
            return j;
        }
    }
    return nullptr;
}

void Scheduler::WorkerLoop(int index)
{
    t_scheduler = this;
    t_worker = index;
    const std::string name = "worker-" + std::to_string(index);
    platform::SetCurrentThreadName(name.c_str());

    while (!stop_.load(std::memory_order_acquire)) {
        if (Job* j = FindWork(index)) { Execute(j); continue; }

        // Spin briefly - work often arrives in bursts - then sleep.
        bool found = false;
        for (int spin = 0; spin < 256 && !found; ++spin) {
            ENGINE_PAUSE();
            if (Job* j = FindWork(index)) { Execute(j); found = true; }
        }
        if (found) continue;

        const uint32_t epoch = work_epoch_.load(std::memory_order_acquire);
        if (Job* j = FindWork(index)) { Execute(j); continue; }
        if (stop_.load(std::memory_order_acquire)) break;
        sleepers_.fetch_add(1, std::memory_order_acq_rel);
        work_epoch_.wait(epoch, std::memory_order_acquire);   // returns if a submit bumped the epoch
        sleepers_.fetch_sub(1, std::memory_order_acq_rel);
    }
    t_scheduler = nullptr;
    t_worker = -1;
}

void Scheduler::Wait(JobCounter& counter)
{
    const int self = CurrentWorker();
    int idle = 0;
    while (!counter.Done()) {
        if (Job* j = FindWork(self)) { Execute(j); idle = 0; continue; }
        if (++idle < 64) ENGINE_PAUSE();
        else std::this_thread::yield();
    }
}

void Scheduler::ParallelFor(size_t count, size_t batch, const std::function<void(size_t, size_t)>& fn)
{
    if (count == 0) return;
    if (batch == 0) batch = 1;
    JobCounter counter;
    for (size_t b = 0; b < count; b += batch) {
        const size_t e = b + batch < count ? b + batch : count;
        Submit([&fn, b, e] { fn(b, e); }, &counter);
    }
    Wait(counter);
}

void Scheduler::Quiesce()
{
    ENGINE_ZONE("quiesce");
    {
        std::lock_guard lock(quiesce_mutex_);
        quiesced_.store(true, std::memory_order_release);
    }
    const int self = CurrentWorker();
    while (inflight_.load(std::memory_order_acquire) > 0) {
        if (Job* j = FindWork(self)) Execute(j);
        else std::this_thread::yield();
    }
}

void Scheduler::Resume()
{
    {
        std::lock_guard lock(quiesce_mutex_);
        quiesced_.store(false, std::memory_order_release);
    }
    quiesce_cv_.notify_all();
}

Scheduler::Stats Scheduler::GetStats() const
{
    Stats s;
    for (const auto& w : workers_) {
        s.executed += w->executed.load(std::memory_order_relaxed);
        s.stolen += w->stolen.load(std::memory_order_relaxed);
        s.inline_overflow += w->overflow.load(std::memory_order_relaxed);
    }
    return s;
}

}  // namespace engine
