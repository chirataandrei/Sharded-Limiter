#pragma once

#include "token_bucket.hpp"
#include <mutex>
#include <unordered_map>
#include <string>

class Shard {
	private:
		// guards concurrent acces to buckets
		mutable std::mutex mtx_;

		// maps each key to its dedicated bucket
		std::unordered_map<std::string, TokenBucket> buckets_;

		double default_capacity_;
		double default_refill_rate_;

	public:
		explicit Shard(double default_capacity, double default_refill_rate) :
			default_capacity_(default_capacity), default_refill_rate_(default_refill_rate) {}
		
		// Thread-Safe API
		bool try_consume(const std::string& key, double tokens, std::chrono::steady_clock::time_point now);
		bool try_consume_custom(const std::string& key, double tokens, double capacity, double refill_rate, 
			std::chrono::steady_clock::time_point now);
		void cleanup_stale(std::chrono::steady_clock::duration ttl_duration, std::chrono::steady_clock::time_point now);
		size_t size() const;
};

inline bool Shard::try_consume(const std::string& key, double tokens, std::chrono::steady_clock::time_point now)
{
	std::lock_guard<std::mutex> lock(mtx_);
	
	// store in the map
	// using try_emplace for optimization
	auto it = buckets_.try_emplace(key, default_capacity_, default_refill_rate_, now).first;

	return it->second.try_consume(tokens, now);
}

// for premium clients
inline bool Shard::try_consume_custom(const std::string& key, double tokens, double capacity, double refill_rate,
	std::chrono::steady_clock::time_point now) 
{
	std::lock_guard<std::mutex> lock(mtx_);

	// store in the map
	// using try_emplace for optimization
	auto it = buckets_.try_emplace(key, capacity, refill_rate, now).first;

	return it->second.try_consume(tokens, now);
}

inline void Shard::cleanup_stale(std::chrono::steady_clock::duration ttl_duration, std::chrono::steady_clock::time_point now)
{
	std::lock_guard<std::mutex> lock(mtx_);

	// iterate over the map
	auto it = buckets_.begin();
	while (it != buckets_.end()) {
		// verify if it's inactive
		if (now - it->second.last_seen() >= ttl_duration && it->second.is_full() == true) {
			it = buckets_.erase(it);
		} else {
			it++;
		}
	}
}

inline size_t Shard::size() const {
	std::lock_guard<std::mutex> lock(mtx_);

	return buckets_.size();
}