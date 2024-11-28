#pragma once

#include <atomic>

namespace ez {

struct trigger {
	trigger()                 { flag_.clear(); flag_.test_and_set(std::memory_order_relaxed); }
	auto operator()() -> void { flag_.clear(std::memory_order_relaxed); }
	operator bool()           { return !(flag_.test_and_set(std::memory_order_relaxed)); }
private:
	std::atomic_flag flag_;
};

// Ball thrown between two players.
// Can be used to coordinate access to some memory between two threads.
// Only the player currently holding the ball is allowed to access the
// memory.
// Each player must poll by calling catch_ball(), to check
// if the ball has been thrown back to them yet.
// Calling throw_ball() when you don't have the ball is invalid.
struct beach_ball {
	beach_ball(int first_catcher) {
		assert(first_catcher == 0 || first_catcher == 1);
		thrown_to_.store(first_catcher, std::memory_order_relaxed);
	}
	// We're not allowed to call this unless we have the ball,
	// i.e. catch_ball() must have returned true since our
	// last call to throw_ball().
	template <int player>
	auto throw_ball() -> void {
		static_assert(player == 0 || player == 1);
		thrown_to_.store(1 - player, std::memory_order_release);
	}
	// Returns true if the ball is caught.
	// Returns false if the ball has not been thrown to this player.
	// May also return false spuriously because that's how
	// compare_exchange_weak() works, but will always return true
	// eventually if the ball has been thrown to us.
	template <int player>
	auto catch_ball() -> bool {
		static_assert(player == 0 || player == 1); 
		int tmp = player;
		return thrown_to_.compare_exchange_weak(tmp, NO_PLAYER, std::memory_order_acquire, std::memory_order_relaxed);
	}
private:
	static inline constexpr int NO_PLAYER{ -1 };
	std::atomic<int> thrown_to_;
};

template <int player>
struct beach_ball_player {
	beach_ball* const ball;
	beach_ball_player(beach_ball* ball_)
		: ball{ ball_ }
	{
		static_assert(player == 0 || player == 1);
	}
	auto throw_ball() -> void {
		assert(have_ball_);
		have_ball_ = false;
		ball->throw_ball<player>();
	}
	auto catch_ball() -> bool {
		assert(!have_ball_);
		if (ball->catch_ball<player>()) {
			have_ball_ = true;
		}
		return have_ball_;
	} 
	auto have_ball() const -> bool {
		return have_ball_;
	}
	auto ensure() -> bool {
		if (!have_ball_) {
			if (!catch_ball()) return false;
		}
		return true;
	}
private:
	bool have_ball_{};
};

} // ez
