#include "ratelimiter/sharded_limiter.hpp"

#include <atomic>
#include <cmath>
#include <cstdlib>
#include <functional>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

namespace {

int g_checks = 0;
int g_failures = 0;

void record(bool ok, const char* expr, const char* file, int line) {
    ++g_checks;
    if (!ok) {
        ++g_failures;
        std::cerr << file << ":" << line << ": CHECK FAILED: " << expr << "\n";
    }
}

} // namespace

#define CHECK(expr) record((expr), #expr, __FILE__, __LINE__)
#define CHECK_NEAR(a, b, eps) \
    record(std::fabs((a) - (b)) <= (eps), #a " ~= " #b, __FILE__, __LINE__)

using Clock = std::chrono::steady_clock;
using Seconds = std::chrono::duration<double>;

namespace {

Clock::time_point offset(Clock::time_point base, double seconds) {
    return base + std::chrono::duration_cast<Clock::duration>(Seconds(seconds));
}

void run(const char* name, const std::function<void()>& test) {
    int before = g_failures;
    test();
    std::cout << "[" << (g_failures == before ? "PASS" : "FAIL") << "] " << name << "\n";
}

// ---------------------------------------------------------------------
// TokenBucket
// ---------------------------------------------------------------------

void test_token_bucket_starts_full() {
    Clock::time_point t0 = Clock::now();
    TokenBucket b(10.0, 1.0, t0);

    CHECK(b.is_full());
    CHECK_NEAR(b.available_tokens(), 10.0, 1e-9);
    CHECK(b.last_seen() == t0);
}

void test_token_bucket_consume_success_decrements() {
    Clock::time_point t0 = Clock::now();
    TokenBucket b(10.0, 1.0, t0);

    CHECK(b.try_consume(4.0, t0));
    CHECK_NEAR(b.available_tokens(), 6.0, 1e-9);
    CHECK(!b.is_full());
}

void test_token_bucket_consume_failure_insufficient_leaves_state_unchanged() {
    Clock::time_point t0 = Clock::now();
    TokenBucket b(5.0, 1.0, t0);

    CHECK(!b.try_consume(10.0, t0));
    CHECK_NEAR(b.available_tokens(), 5.0, 1e-9);
    CHECK(b.is_full());
}

void test_token_bucket_exact_consume_drains_to_zero() {
    Clock::time_point t0 = Clock::now();
    TokenBucket b(5.0, 1.0, t0);

    CHECK(b.try_consume(5.0, t0));
    CHECK_NEAR(b.available_tokens(), 0.0, 1e-9);
    CHECK(!b.is_full());
}

void test_token_bucket_refill_over_time() {
    Clock::time_point t0 = Clock::now();
    TokenBucket b(10.0, 2.0, t0); // 2 tokens/sec

    CHECK(b.try_consume(8.0, t0));
    CHECK_NEAR(b.available_tokens(), 2.0, 1e-9);

    // 3 seconds later: +6 tokens -> 8 total, consume 0 just to force refresh
    Clock::time_point t1 = offset(t0, 3.0);
    CHECK(b.try_consume(0.0, t1));
    CHECK_NEAR(b.available_tokens(), 8.0, 1e-6);
    CHECK(b.last_seen() == t1);
}

void test_token_bucket_refill_caps_at_capacity() {
    Clock::time_point t0 = Clock::now();
    TokenBucket b(10.0, 2.0, t0);

    CHECK(b.try_consume(5.0, t0));
    Clock::time_point t1 = offset(t0, 1000.0); // plenty of time to overflow
    CHECK(b.try_consume(0.0, t1));
    CHECK_NEAR(b.available_tokens(), 10.0, 1e-6);
    CHECK(b.is_full());
}

void test_token_bucket_same_timestamp_no_refill() {
    Clock::time_point t0 = Clock::now();
    TokenBucket b(5.0, 100.0, t0);

    CHECK(b.try_consume(5.0, t0));
    CHECK_NEAR(b.available_tokens(), 0.0, 1e-9);

    // repeated calls at the exact same instant must not grant free tokens
    CHECK(!b.try_consume(1.0, t0));
    CHECK_NEAR(b.available_tokens(), 0.0, 1e-9);
}

void test_token_bucket_clock_regression_is_safe() {
    Clock::time_point t0 = Clock::now();
    TokenBucket b(5.0, 10.0, t0);

    Clock::time_point past = offset(t0, -5.0);
    CHECK(b.try_consume(0.0, past));

    // no negative refill from a time point earlier than last_seen_
    CHECK_NEAR(b.available_tokens(), 5.0, 1e-9);
    // last_seen_ must not move backwards
    CHECK(b.last_seen() == t0);
}

// ---------------------------------------------------------------------
// Shard
// ---------------------------------------------------------------------

void test_shard_lazy_bucket_creation_and_size() {
    Clock::time_point t0 = Clock::now();
    Shard s(10.0, 1.0);

    CHECK(s.size() == 0);
    s.try_consume("a", 1.0, t0);
    CHECK(s.size() == 1);
    s.try_consume("a", 1.0, t0); // same key, no growth
    CHECK(s.size() == 1);
    s.try_consume("b", 1.0, t0);
    CHECK(s.size() == 2);
}

void test_shard_default_capacity_enforced() {
    Clock::time_point t0 = Clock::now();
    Shard s(3.0, 1.0);

    CHECK(s.try_consume("k", 1.0, t0));
    CHECK(s.try_consume("k", 1.0, t0));
    CHECK(s.try_consume("k", 1.0, t0));
    CHECK(!s.try_consume("k", 1.0, t0)); // exhausted
}

void test_shard_try_consume_custom_first_write_wins() {
    Clock::time_point t0 = Clock::now();
    Shard s(5.0, 1.0);

    // first touch creates the bucket with a custom, larger capacity
    CHECK(s.try_consume_custom("k", 1.0, 100.0, 1.0, t0));

    // subsequent calls via the default-config API must reuse the existing
    // bucket (try_emplace does not overwrite), so 50 tokens should still
    // be available even though the shard's default capacity is only 5.
    CHECK(s.try_consume("k", 50.0, t0));
}

void test_shard_cleanup_removes_full_and_stale() {
    Clock::time_point t0 = Clock::now();
    Shard s(5.0, 1.0);

    s.try_consume("k", 0.0, t0); // bucket created, remains full
    CHECK(s.size() == 1);

    s.cleanup_stale(std::chrono::seconds(1), offset(t0, 2.0));
    CHECK(s.size() == 0);
}

void test_shard_cleanup_keeps_non_full_bucket() {
    Clock::time_point t0 = Clock::now();
    Shard s(5.0, 0.0); // no refill

    s.try_consume("k", 3.0, t0); // tokens left: 2, not full
    CHECK(s.size() == 1);

    s.cleanup_stale(std::chrono::seconds(1), offset(t0, 1000.0));
    CHECK(s.size() == 1);
}

void test_shard_cleanup_keeps_bucket_within_ttl() {
    Clock::time_point t0 = Clock::now();
    Shard s(5.0, 1.0);

    s.try_consume("k", 0.0, t0); // full
    CHECK(s.size() == 1);

    s.cleanup_stale(std::chrono::seconds(10), offset(t0, 1.0));
    CHECK(s.size() == 1);
}

// ---------------------------------------------------------------------
// ShardedLimiter
// ---------------------------------------------------------------------

void test_sharded_limiter_distributes_keys_and_counts_total_size() {
    Clock::time_point t0 = Clock::now();
    ShardedLimiter limiter(10.0, 1.0);

    constexpr int kKeys = 1000;
    for (int i = 0; i < kKeys; ++i) {
        limiter.try_consume("key_" + std::to_string(i), 1.0, t0);
    }
    CHECK(limiter.total_size() == static_cast<size_t>(kKeys));
}

void test_sharded_limiter_try_consume_respects_capacity() {
    Clock::time_point t0 = Clock::now();
    ShardedLimiter limiter(3.0, 1.0);

    CHECK(limiter.try_consume("x", 1.0, t0));
    CHECK(limiter.try_consume("x", 1.0, t0));
    CHECK(limiter.try_consume("x", 1.0, t0));
    CHECK(!limiter.try_consume("x", 1.0, t0));
}

void test_sharded_limiter_try_consume_custom_overrides_default() {
    Clock::time_point t0 = Clock::now();
    ShardedLimiter limiter(1.0, 1.0);

    CHECK(limiter.try_consume_custom("premium", 20.0, 20.0, 1.0, t0));
    CHECK(!limiter.try_consume_custom("premium", 1.0, 20.0, 1.0, t0)); // drained
}

void test_sharded_limiter_cleanup_all_stale() {
    Clock::time_point t0 = Clock::now();
    ShardedLimiter limiter(5.0, 1.0);

    // many distinct keys left full (never drained) -> stale after TTL
    for (int i = 0; i < 200; ++i) {
        limiter.try_consume("stale_" + std::to_string(i), 0.0, t0);
    }
    // one key drained below capacity -> must survive cleanup
    limiter.try_consume("alive", 5.0, t0);

    CHECK(limiter.total_size() == 201);
    limiter.cleanup_all_stale(std::chrono::seconds(1), offset(t0, 10.0));
    CHECK(limiter.total_size() == 1);
}

void test_sharded_limiter_concurrent_same_key_never_exceeds_capacity() {
    Clock::time_point t0 = Clock::now();
    constexpr double kCapacity = 100.0;
    // refill_rate = 0 removes any timing dependence from the result
    ShardedLimiter limiter(kCapacity, 0.0);

    constexpr int kThreads = 16;
    constexpr int kAttemptsPerThread = 200;
    std::atomic<int> successes{0};

    std::vector<std::thread> workers;
    workers.reserve(kThreads);
    for (int t = 0; t < kThreads; ++t) {
        workers.emplace_back([&]() {
            for (int i = 0; i < kAttemptsPerThread; ++i) {
                if (limiter.try_consume("shared", 1.0, t0)) {
                    successes.fetch_add(1, std::memory_order_relaxed);
                }
            }
        });
    }
    for (auto& w : workers) {
        w.join();
    }

    CHECK(successes.load() == static_cast<int>(kCapacity));
}

void test_sharded_limiter_concurrent_distinct_keys_no_data_races() {
    constexpr int kThreads = 16;
    constexpr int kKeysPerThread = 100;
    ShardedLimiter limiter(5.0, 1.0);

    // each thread owns its own key namespace, so no two threads ever touch
    // the same bucket concurrently -- this exercises shard selection and
    // the per-shard mutex without introducing contention on a single key.
    std::vector<std::thread> workers;
    workers.reserve(kThreads);
    for (int t = 0; t < kThreads; ++t) {
        workers.emplace_back([&limiter, t]() {
            for (int i = 0; i < kKeysPerThread; ++i) {
                limiter.try_consume("t" + std::to_string(t) + "_k" + std::to_string(i), 1.0);
            }
        });
    }
    for (auto& w : workers) {
        w.join();
    }

    CHECK(limiter.total_size() == static_cast<size_t>(kThreads * kKeysPerThread));
}

} // namespace

int main() {
    run("TokenBucket starts full", test_token_bucket_starts_full);
    run("TokenBucket consume success decrements", test_token_bucket_consume_success_decrements);
    run("TokenBucket consume failure leaves state unchanged",
        test_token_bucket_consume_failure_insufficient_leaves_state_unchanged);
    run("TokenBucket exact consume drains to zero", test_token_bucket_exact_consume_drains_to_zero);
    run("TokenBucket refill over time", test_token_bucket_refill_over_time);
    run("TokenBucket refill caps at capacity", test_token_bucket_refill_caps_at_capacity);
    run("TokenBucket same timestamp grants no refill", test_token_bucket_same_timestamp_no_refill);
    run("TokenBucket clock regression is safe", test_token_bucket_clock_regression_is_safe);

    run("Shard lazy bucket creation and size", test_shard_lazy_bucket_creation_and_size);
    run("Shard default capacity enforced", test_shard_default_capacity_enforced);
    run("Shard try_consume_custom first write wins", test_shard_try_consume_custom_first_write_wins);
    run("Shard cleanup removes full and stale bucket", test_shard_cleanup_removes_full_and_stale);
    run("Shard cleanup keeps non-full bucket", test_shard_cleanup_keeps_non_full_bucket);
    run("Shard cleanup keeps bucket within ttl", test_shard_cleanup_keeps_bucket_within_ttl);

    run("ShardedLimiter distributes keys and counts total size",
        test_sharded_limiter_distributes_keys_and_counts_total_size);
    run("ShardedLimiter try_consume respects capacity",
        test_sharded_limiter_try_consume_respects_capacity);
    run("ShardedLimiter try_consume_custom overrides default",
        test_sharded_limiter_try_consume_custom_overrides_default);
    run("ShardedLimiter cleanup_all_stale", test_sharded_limiter_cleanup_all_stale);
    run("ShardedLimiter concurrent same key never exceeds capacity",
        test_sharded_limiter_concurrent_same_key_never_exceeds_capacity);
    run("ShardedLimiter concurrent distinct keys no data races",
        test_sharded_limiter_concurrent_distinct_keys_no_data_races);

    std::cout << g_checks - g_failures << "/" << g_checks << " checks passed\n";
    return g_failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
