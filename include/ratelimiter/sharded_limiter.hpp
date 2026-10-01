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

		Shard& get_shard(const std::string& key);
		const Shard& get_shard(const std::string& key) const;

	public:
		explicit ShardedLimiter(double default_capacity, double default_refill_rate);

		// public API
		bool try_consume(const std::string& key, double tokens = 1.0, 
			std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now());
		bool try_consume_custom(const std::string& key, double tokens, double capacity, double refill_rate, 
			std::chrono::steady_clock::time_point now);
		void cleanup_all_stale(std::chrono::steady_clock::duration ttl_duration, std::chrono::steady_clock::time_point now);
		size_t total_size() const;

};

inline ShardedLimiter::ShardedLimiter(double default_capacity, double default_refill_rate) : 
	default_capacity_(default_capacity), default_refill_rate_(default_refill_rate) {
			
	shards_.reserve(NUM_SHARDS);
	for (size_t i = 0; i < NUM_SHARDS; i++) {
		shards_.push_back(std::make_unique<Shard>(default_capacity, default_refill_rate));
	}
}

inline Shard& ShardedLimiter::get_shard(const std::string& key) {
	size_t idx = hasher_(key) & (NUM_SHARDS - 1);
	return *shards_[idx];
}

inline const Shard& ShardedLimiter::get_shard(const std::string& key) const {
	size_t idx = hasher_(key) & (NUM_SHARDS - 1);
	return *shards_[idx];
}

inline bool ShardedLimiter::try_consume(const std::string& key, double tokens, std::chrono::steady_clock::time_point now)
{
	Shard& curr = get_shard(key);
	return curr.try_consume(key, tokens, now);
}

inline bool ShardedLimiter::try_consume_custom(const std::string& key, double tokens, double capacity, double refill_rate, 
	std::chrono::steady_clock::time_point now)
{
	Shard& curr = get_shard(key);
	return curr.try_consume_custom(key, tokens, capacity, refill_rate, now);
}

inline void ShardedLimiter::cleanup_all_stale(std::chrono::steady_clock::duration ttl_duration, std::chrono::steady_clock::time_point now)
{
	for (size_t i = 0; i < NUM_SHARDS; i++) {
		shards_[i]->cleanup_stale(ttl_duration, now);
	}
}

inline size_t ShardedLimiter::total_size() const
{
	size_t curr = 0;
	for (size_t i = 0; i < NUM_SHARDS; i++){
		curr += shards_[i]->size();
	}
	return curr;
}