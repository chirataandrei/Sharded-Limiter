// Throughput micro-benchmarks for ShardedLimiter.
//
// Not a rigorous statistical benchmark (single run per scenario, no
// warm-up discarding) -- just enough to see the shape of the numbers:
// raw per-call overhead, and how sharding behaves under contention
// versus a single, non-sharded Shard.

#include "ratelimiter/sharded_limiter.hpp"
#include "ratelimiter/shard.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <iomanip>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

using Clock = std::chrono::steady_clock;

namespace {

// Large enough that every call succeeds, so every scenario does the same
// amount of work (refill math + lookup + mutex) regardless of outcome.
constexpr double kCapacity = 1'000'000'000.0;
constexpr double kRefillRate = 1'000'000'000.0;

struct BenchResult {
    std::string name;
    long long total_ops;
    double seconds;

    double ops_per_sec() const { return static_cast<double>(total_ops) / seconds; }
    double ns_per_op() const { return seconds * 1e9 / static_cast<double>(total_ops); }
};

void report(const BenchResult& r) {
    std::cout << std::left << std::setw(52) << r.name << std::right << std::fixed
              << std::setw(14) << std::setprecision(0) << r.ops_per_sec() << " ops/s"
              << std::setw(10) << std::setprecision(1) << r.ns_per_op() << " ns/op\n";
}

// ---------------------------------------------------------------------
// Single-threaded scenarios
// ---------------------------------------------------------------------

BenchResult bench_single_key_single_thread(long long iterations) {
    ShardedLimiter limiter(kCapacity, kRefillRate);
    const std::string key = "single-key";
    const Clock::time_point now = Clock::now(); // frozen: isolates limiter overhead from clock syscalls

    Clock::time_point start = Clock::now();
    for (long long i = 0; i < iterations; ++i) {
        limiter.try_consume(key, 1.0, now);
    }
    Clock::time_point end = Clock::now();

    return {"1 key, 1 thread", iterations, std::chrono::duration<double>(end - start).count()};
}

BenchResult bench_many_keys_single_thread(long long iterations, long long key_space) {
    ShardedLimiter limiter(kCapacity, kRefillRate);
    std::vector<std::string> keys;
    keys.reserve(static_cast<size_t>(key_space));
    for (long long i = 0; i < key_space; ++i) {
        keys.push_back("key_" + std::to_string(i));
    }
    const Clock::time_point now = Clock::now();

    Clock::time_point start = Clock::now();
    for (long long i = 0; i < iterations; ++i) {
        limiter.try_consume(keys[static_cast<size_t>(i % key_space)], 1.0, now);
    }
    Clock::time_point end = Clock::now();

    return {std::to_string(key_space) + " keys, 1 thread", iterations,
             std::chrono::duration<double>(end - start).count()};
}

// ---------------------------------------------------------------------
// Multi-threaded scenarios
// ---------------------------------------------------------------------

template <typename ConsumeFn>
BenchResult bench_threaded(const std::string& name, int num_threads, long long iterations_per_thread,
                            ConsumeFn consume) {
    std::atomic<bool> go{false};
    std::vector<std::thread> workers;
    workers.reserve(static_cast<size_t>(num_threads));

    for (int t = 0; t < num_threads; ++t) {
        workers.emplace_back([&, t]() {
            while (!go.load(std::memory_order_acquire)) {
                // spin until the main thread releases every worker at once
            }
            for (long long i = 0; i < iterations_per_thread; ++i) {
                consume(t);
            }
        });
    }

    Clock::time_point start = Clock::now();
    go.store(true, std::memory_order_release);
    for (std::thread& w : workers) {
        w.join();
    }
    Clock::time_point end = Clock::now();

    long long total_ops = static_cast<long long>(num_threads) * iterations_per_thread;
    return {name, total_ops, std::chrono::duration<double>(end - start).count()};
}

std::vector<std::string> make_keys(int count, const std::string& prefix) {
    std::vector<std::string> keys;
    keys.reserve(static_cast<size_t>(count));
    for (int i = 0; i < count; ++i) {
        keys.push_back(prefix + std::to_string(i));
    }
    return keys;
}

} // namespace

int main() {
    const int threads = static_cast<int>(std::max(2u, std::thread::hardware_concurrency()));
    std::cout << "Hardware concurrency: " << threads << " threads\n\n";

    std::cout << "-- Single-threaded --\n";
    report(bench_single_key_single_thread(5'000'000));
    report(bench_many_keys_single_thread(5'000'000, 10'000));

    std::cout << "\n-- Multi-threaded (" << threads << " threads) --\n";
    constexpr long long kItersPerThread = 1'000'000;

    // Worst case: every thread hammers the SAME key, fully serialized by
    // that one key's shard mutex.
    {
        ShardedLimiter limiter(kCapacity, kRefillRate);
        const std::string key = "hot-key";
        report(bench_threaded("ShardedLimiter: 1 shared key (worst case)", threads, kItersPerThread,
                               [&](int /*tid*/) { limiter.try_consume(key); }));
    }

    // Best case: each thread owns a distinct key, spread across (mostly)
    // distinct shards -- lock contention is rare.
    {
        ShardedLimiter limiter(kCapacity, kRefillRate);
        std::vector<std::string> keys = make_keys(threads, "thread-key-");
        report(bench_threaded("ShardedLimiter: " + std::to_string(threads) + " distinct keys (best case)",
                               threads, kItersPerThread,
                               [&](int tid) { limiter.try_consume(keys[static_cast<size_t>(tid)]); }));
    }

    // Baseline for comparison: a single, non-sharded Shard (what you'd get
    // with NUM_SHARDS == 1) under the same distinct-key workload. The gap
    // between this and the "best case" above is the value of sharding.
    {
        Shard shard(kCapacity, kRefillRate);
        std::vector<std::string> keys = make_keys(threads, "thread-key-");
        report(bench_threaded("Single Shard (no sharding), " + std::to_string(threads) + " distinct keys",
                               threads, kItersPerThread,
                               [&](int tid) { shard.try_consume(keys[static_cast<size_t>(tid)], 1.0, Clock::now()); }));
    }

    return 0;
}
