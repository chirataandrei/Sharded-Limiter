#pragma once

#include <chrono>
#include <algorithm>

class TokenBucket {
	private:
		// upper limit
		double capacity_;
		// tokens generated per second
		double refill_rate_;
		// currently available tokens
		double tokens_;
		// last used	
		std::chrono::steady_clock::time_point last_seen_;

	public:
		explicit TokenBucket(double capacity, double refill_rate, std::chrono::steady_clock::time_point now) : 
			capacity_(capacity), refill_rate_(refill_rate), tokens_(capacity), last_seen_(now) {}
		
		// public APIs
		bool try_consume(double tokens, std::chrono::steady_clock::time_point now);
		double available_tokens() const { return tokens_; }
		std::chrono::steady_clock::time_point last_seen() const { return last_seen_; }
		bool is_full () const { return capacity_ <= tokens_; }
};

inline bool TokenBucket::try_consume(double tokens, std::chrono::steady_clock::time_point now)
{
	// update the currently available tokens
	std::chrono::duration<double> delta = now - last_seen_;
	double delta_seconds = std::max(0.0, delta.count());
	double new_tokens = delta_seconds * refill_rate_;
	tokens_ = std::min(tokens_ + new_tokens, capacity_);

	// update last seen
	if (now > last_seen_) {
		last_seen_ = now;
	}
	
	// verify if we have enough tokens
	if (tokens_ >= tokens) {
		tokens_ -= tokens;
		return true;
	}

	return false;
}
