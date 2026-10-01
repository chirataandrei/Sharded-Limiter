// Demonstrates wiring ShardedLimiter into a simulated HTTP server:
// clients are identified by IP (anonymous traffic) or API key
// (authenticated traffic), a flooding client gets throttled with
// HTTP 429, and a VIP account is given a bigger, faster-refilling
// budget via try_consume_custom.

#include "ratelimiter/sharded_limiter.hpp"

#include <iomanip>
#include <iostream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

using Clock = std::chrono::steady_clock;

namespace {

struct HttpResponse {
    int status;
    std::string body;
};

// One inbound HTTP request for `client_id` (a source IP for anonymous
// traffic, or an API key for authenticated traffic). VIP accounts are
// rate-limited on their own plan instead of the server-wide default.
HttpResponse handle_request(ShardedLimiter& limiter, const std::string& client_id,
                             bool is_vip, Clock::time_point now) {
    bool allowed = is_vip
        ? limiter.try_consume_custom(client_id, /*tokens=*/1.0, /*capacity=*/50.0,
                                      /*refill_rate=*/10.0, now)
        : limiter.try_consume(client_id, /*tokens=*/1.0, now);

    return allowed ? HttpResponse{200, "OK"} : HttpResponse{429, "Too Many Requests"};
}

void log_response(const std::string& label, const HttpResponse& resp) {
    std::cout << "  " << std::left << std::setw(28) << label << " -> " << resp.status
              << " " << resp.body << "\n";
}

} // namespace

int main() {
    // Server-wide default policy for anonymous/free-tier clients:
    // burst of 5 requests, refilling at 1 request/sec.
    ShardedLimiter limiter(/*default_capacity=*/5.0, /*default_refill_rate=*/1.0);

    // A manually-advanced clock keeps the demo deterministic and lets us
    // fast-forward through "time passing" without actually sleeping.
    Clock::time_point clock = Clock::now();

    std::cout << "=== Scenario 1: normal traffic from a few distinct IPs ===\n";
    for (const std::string& ip : {"203.0.113.10", "203.0.113.11", "203.0.113.12"}) {
        log_response(ip, handle_request(limiter, ip, /*is_vip=*/false, clock));
    }

    std::cout << "\n=== Scenario 2: a free-tier IP floods the server ===\n";
    const std::string flooder = "203.0.113.99";
    for (int i = 1; i <= 8; ++i) {
        HttpResponse resp = handle_request(limiter, flooder, /*is_vip=*/false, clock);
        log_response(flooder + " (request #" + std::to_string(i) + ")", resp);
    }
    std::cout << "  -> first 5 requests exhaust the burst capacity, the rest are throttled\n";

    std::cout << "\n=== Scenario 3: a VIP account, configured via try_consume_custom ===\n";
    const std::string vip_key = "api-key-vip-acme-corp";
    int vip_allowed = 0;
    int vip_throttled = 0;
    for (int i = 0; i < 60; ++i) {
        HttpResponse resp = handle_request(limiter, vip_key, /*is_vip=*/true, clock);
        (resp.status == 200 ? vip_allowed : vip_throttled)++;
    }
    std::cout << "  " << vip_key << " -> " << vip_allowed << " allowed, " << vip_throttled
              << " throttled out of 60 rapid requests (vs. only 5 for a free-tier client)\n";

    std::cout << "\n=== Scenario 4: tokens refill as (simulated) time passes ===\n";
    std::cout << "  " << flooder << " was throttled above; fast-forwarding the clock by 5s...\n";
    clock += std::chrono::seconds(5);
    log_response(flooder + " (after 5s)", handle_request(limiter, flooder, /*is_vip=*/false, clock));

    std::cout << "\n=== Scenario 5: concurrent clients hitting the server at once ===\n";
    constexpr int kWorkers = 8;
    constexpr int kRequestsPerWorker = 3;
    std::vector<std::thread> workers;
    std::mutex log_mtx;
    workers.reserve(kWorkers);
    for (int w = 0; w < kWorkers; ++w) {
        workers.emplace_back([&, w]() {
            std::string client = "worker-" + std::to_string(w);
            for (int i = 0; i < kRequestsPerWorker; ++i) {
                // Real server handlers run on arbitrary threads; ShardedLimiter's
                // per-shard mutex makes this safe without any extra locking here.
                HttpResponse resp = handle_request(limiter, client, /*is_vip=*/false, Clock::now());
                std::lock_guard<std::mutex> lock(log_mtx);
                log_response(client, resp);
            }
        });
    }
    for (std::thread& t : workers) {
        t.join();
    }

    std::cout << "\n=== Scenario 6: housekeeping reclaims idle clients ===\n";
    // A liveness probe that only ever "peeks" (tokens=0) never runs its
    // bucket down, so it stays full and is a prime cleanup candidate once
    // it goes idle. Every other client above still owes some debt (their
    // buckets aren't full yet), so this is the only one cleanup can sweep.
    const std::string idle_client = "health-check-bot";
    limiter.try_consume(idle_client, /*tokens=*/0.0, clock);
    std::cout << "  " << idle_client << " polled once and then went idle\n";

    std::cout << "\nActive tracked clients: " << limiter.total_size() << "\n";

    // Periodic housekeeping a real server would run on a background timer:
    // drop buckets that are both full (no outstanding debt) and idle past
    // the TTL, so memory doesn't grow unbounded with one-off clients.
    limiter.cleanup_all_stale(std::chrono::seconds(30), clock + std::chrono::seconds(60));
    std::cout << "Active clients after cleanup_all_stale: " << limiter.total_size()
              << " (" << idle_client << " was reclaimed; everyone else still owes debt)\n";

    return 0;
}
