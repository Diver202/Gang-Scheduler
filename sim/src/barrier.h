#pragma once
#include <atomic>
#include <memory>
#include <pthread.h>
#include <string>

inline void cpuRelax() {
#if defined(__aarch64__)
    asm volatile("yield");
#elif defined(__x86_64__)
    __builtin_ia32_pause();
#endif
}

class Barrier {
public:
    virtual ~Barrier() = default;
    virtual void wait() = 0;
};

// Busy-waits: never gives up the CPU.
class SpinBarrier : public Barrier {
public:
    explicit SpinBarrier(int n) : n(n) {}

    void wait() override {
        int gen = generation.load(std::memory_order_acquire);
        if (arrived.fetch_add(1, std::memory_order_acq_rel) == n - 1) {
            arrived.store(0, std::memory_order_relaxed);
            generation.store(gen + 1, std::memory_order_release);
        } else {
            while (generation.load(std::memory_order_acquire) == gen) cpuRelax();
        }
    }

private:
    int n;
    alignas(64) std::atomic<int> arrived{0};
    alignas(64) std::atomic<int> generation{0};
};

// Sleeps in the kernel (futex) until the last thread arrives.
class BlockingBarrier : public Barrier {
public:
    explicit BlockingBarrier(int n) { pthread_barrier_init(&barrier, nullptr, n); }
    ~BlockingBarrier() override { pthread_barrier_destroy(&barrier); }
    void wait() override { pthread_barrier_wait(&barrier); }

private:
    pthread_barrier_t barrier;
};

inline std::unique_ptr<Barrier> makeBarrier(const std::string& kind, int n) {
    if (kind == "spin") return std::make_unique<SpinBarrier>(n);
    if (kind == "block") return std::make_unique<BlockingBarrier>(n);
    return nullptr;
}
