# Sharded Rate Limiter

A header-only, thread-safe token-bucket rate limiter for C++20. Keys (IPs,
API keys, user IDs, ...) are distributed across a fixed set of shards so
that concurrent requests for *different* keys never contend on the same
lock.

## Features

- **Token bucket algorithm** — smooth, burst-tolerant rate limiting with
  double-precision refill based on elapsed wall-clock time.
- **Per-key isolation** — every key gets its own independent bucket,
  created lazily on first use.
- **Sharded for concurrency** — keys are hashed into 64 shards, each
  guarded by its own mutex, so unrelated keys never block each other.
- **Per-client overrides** — `try_consume_custom` lets you give a specific
  key (e.g. a VIP account) its own capacity and refill rate on the fly.
- **Bounded memory** — `cleanup_all_stale` reclaims buckets that are both
  full (no outstanding debt) and idle past a TTL.
- **Header-only** — no build step, no dependencies beyond the C++
  standard library and `pthreads`.

## Requirements

- A C++20 compiler (tested with AppleClang/Clang)
- CMake ≥ 3.20
- POSIX threads

## Architecture

```
ShardedLimiter            — public entry point, hashes a key to a shard
  └── Shard[64]            — unordered_map<key, TokenBucket> + mutex
        └── TokenBucket    — capacity, refill rate, current tokens
```

| Layer           | Responsibility                                              | Thread-safety                          |
|------------------|---------------------------------------------------------------|-----------------------------------------|
| `TokenBucket`    | Refill math and `try_consume` for a single key               | Not synchronized on its own             |
| `Shard`          | Owns one slice of the key space, lazily creates buckets       | `std::mutex`-guarded                    |
| `ShardedLimiter`  | Routes a key to its shard via `std::hash`                     | Lock-free routing, contention confined to one shard |

Spreading keys across 64 shards means two threads hitting *different*
clients essentially never block each other, while two threads hitting the
*same* client are correctly serialized.

## Building

```sh
cmake -S . -B build
cmake --build build
```

Useful options:

```sh
# Build and run the unit/concurrency test suite
cmake --build build --target test_limiter
ctest --test-dir build

# Build with ThreadSanitizer to validate the concurrency guarantees
cmake -S . -B build-tsan -DENABLE_TSAN=ON
cmake --build build-tsan --target test_limiter
./build-tsan/test_limiter
```

## Usage

```cpp
#include "ratelimiter/sharded_limiter.hpp"

// Default policy: burst of 5 requests, refilling at 1 token/sec.
ShardedLimiter limiter(/*default_capacity=*/5.0, /*default_refill_rate=*/1.0);

// Anonymous/free-tier client, rate-limited by source IP.
if (limiter.try_consume(client_ip)) {
    // handle request (200 OK)
} else {
    // reject (429 Too Many Requests)
}

// VIP client: override capacity/refill rate per call via try_consume_custom.
// try_emplace semantics mean the FIRST call for a given key decides its
// bucket's capacity/refill rate; later calls on an existing bucket reuse it.
limiter.try_consume_custom(api_key, /*tokens=*/1.0,
                            /*capacity=*/50.0, /*refill_rate=*/10.0,
                            std::chrono::steady_clock::now());

// Periodic housekeeping (e.g. on a background timer) to bound memory:
limiter.cleanup_all_stale(/*ttl=*/std::chrono::minutes(10),
                           std::chrono::steady_clock::now());

limiter.total_size(); // number of distinct keys currently tracked
```

See [`examples/server_simulation.cpp`](examples/server_simulation.cpp) for
a fuller walkthrough — normal traffic, a flooding client hitting 429, a VIP
account configured via `try_consume_custom`, refill over time, concurrent
clients, and TTL-based cleanup. Build and run it with:

```sh
cmake --build build --target server_simulation
./build/server_simulation
```

## API reference

### `TokenBucket` (`include/ratelimiter/token_bucket.hpp`)

| Method | Description |
|---|---|
| `TokenBucket(capacity, refill_rate, now)` | Constructs a bucket starting full. |
| `bool try_consume(tokens, now)` | Refills based on elapsed time, then consumes `tokens` if available. |
| `double available_tokens() const` | Tokens currently available (as of the last `try_consume`). |
| `time_point last_seen() const` | Timestamp of the last `try_consume` call. |
| `bool is_full() const` | Whether the bucket is at capacity (no outstanding debt). |

### `Shard` (`include/ratelimiter/shard.hpp`)

| Method | Description |
|---|---|
| `Shard(default_capacity, default_refill_rate)` | Constructs an empty shard. |
| `bool try_consume(key, tokens, now)` | Lazily creates a bucket for `key` using the shard defaults, then consumes. |
| `bool try_consume_custom(key, tokens, capacity, refill_rate, now)` | Lazily creates a bucket for `key` using the given capacity/refill rate, then consumes. |
| `void cleanup_stale(ttl, now)` | Removes buckets that are both full and idle for at least `ttl`. |
| `size_t size() const` | Number of keys currently tracked in this shard. |

### `ShardedLimiter` (`include/ratelimiter/sharded_limiter.hpp`)

| Method | Description |
|---|---|
| `ShardedLimiter(default_capacity, default_refill_rate)` | Constructs 64 shards with the given defaults. |
| `bool try_consume(key, tokens = 1.0, now = steady_clock::now())` | Routes `key` to its shard and attempts to consume. |
| `bool try_consume_custom(key, tokens, capacity, refill_rate, now)` | Same, with a per-key capacity/refill rate override. |
| `void cleanup_all_stale(ttl, now)` | Runs `cleanup_stale` on every shard. |
| `size_t total_size() const` | Sum of `size()` across all shards. |

## Design notes

- **Lazy, first-write-wins buckets.** Buckets are created on first use via
  `try_emplace`, so the capacity/refill rate passed on the *first* call for
  a key sticks; subsequent calls (default or custom) reuse the existing
  bucket. Decide a key's plan before its first request.
- **`is_full()` as the cleanup heuristic.** A bucket is only eligible for
  cleanup once it has both fully recovered (no debt) *and* gone idle past
  the TTL — a client mid-burst is never evicted out from under itself.
- **Monotonic clock, clamped deltas.** `TokenBucket` uses
  `std::chrono::steady_clock` and clamps negative elapsed time to zero, so
  an out-of-order timestamp can never grant free tokens.

## Project layout

```
include/ratelimiter/   Header-only library (TokenBucket, Shard, ShardedLimiter)
tests/                 Unit + concurrency test suite (test_limiter.cpp)
examples/              Runnable usage demos (server_simulation.cpp)
benchmarks/            Reserved for throughput benchmarks (throughput_bench.cpp)
```
