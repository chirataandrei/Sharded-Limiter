#pragma once

#include "shard.hpp"

class ShardedLimiter {
	private:
		static constexpr size_t NUM_SHARDS = 64;
		double default_capacity_;
		double default_refill_rate_;

		// using unique_ptr because Shard contains mutex
		std::vector<std::unique_ptr<Shard>> shards_;

		std::hash<std::string> hasher_;

		std::string get_shard(const std::string& key);

	public:
		explicit ShardedLimiter(double default_capacity, double default_refill_rate) {}

		// public API
		bool try_consume(const std::string& key, double tokens = 1.0, std::chrono::steady_clock::time_point now);
		bool try_consume_custom(const std::string& key, double tokens, double capacity, double refill_rate, 
			std::chrono::steady_clock::time_point now);
		void cleanup_all_stale(std::chrono::steady_clock::duration ttl_duration, std::chrono::steady_clock::time_point now);
		size_t total_size();

};

ShardedLimiter::ShardedLimiter(double default_capacity, double default_refill_rate) : 
	default_capacity_(default_capacity), default_refill_rate_(default_refill_rate) {
			
	shards_.reserve(NUM_SHARDS);
	for (size_t i = 0; i < NUM_SHARDS; i++) {
		shards_.push_back(std::make_unique<Shard>(default_capacity, default_refill_rate));
	}
}