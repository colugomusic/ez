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

struct catcher      { int v = -1; };
struct player       { int v = -1; };
struct thrower      { int v = -1; };
struct player_count { int v = -1; };

template <player_count PlayerCount, player Player> struct beach_ball_player;

// Ball thrown between two or more players.
// Can be used to coordinate access to some resource
// between two or more threads.
// Only the player currently holding the ball is
// allowed to access the resource..
// Each player must poll by calling catch_ball(), to check
// if the ball has been thrown to them yet.
// Calling throw_ball() when you don't have the ball is invalid.
template <player_count PlayerCount>
struct beach_ball {
	static_assert(PlayerCount.v > 1);
	template <int Player> using player = beach_ball_player<PlayerCount, ez::player{Player}>;
	beach_ball(catcher first_catcher) {
		assert(first_catcher.v >= 0 && first_catcher.v < PlayerCount.v);
		// Ball starts in the air, thrown to the first catcher
		thrown_to_.store(first_catcher.v, std::memory_order_relaxed);
	}
	template <int Player>
	auto make_player() -> player<Player> {
		return player<Player>{this};
	}
	// We're not allowed to call this unless we have the ball,
	// i.e. catch_ball() must have returned true since our
	// last call to throw_ball().
	template <thrower Thrower, catcher Catcher>
	auto throw_to() -> void {
		static_assert(Thrower.v >= 0 && Thrower.v < PlayerCount.v);
		static_assert(Catcher.v >= 0 && Catcher.v < PlayerCount.v);
		static_assert(Thrower.v != Catcher.v, "Can't throw ball to yourself!");
		thrown_to_.store(Catcher.v, std::memory_order_release);
	}
	// Returns true if the ball is caught.
	// Returns false if the ball has not been thrown to this player.
	template <catcher Catcher>
	auto catch_ball() -> bool {
		static_assert(Catcher.v >= 0 && Catcher.v < PlayerCount.v);
		int tmp = Catcher.v;
		return thrown_to_.compare_exchange_strong(tmp, catcher{}.v, std::memory_order_acquire, std::memory_order_relaxed);
	}
private:
	std::atomic<int> thrown_to_;
};

template <player_count PlayerCount, player Player>
struct beach_ball_player {
	static_assert(Player.v >= 0 && Player.v < PlayerCount.v);
	beach_ball<PlayerCount>* const ball;
	beach_ball_player(beach_ball<PlayerCount>* ball_)
		: ball{ ball_ }
	{
	}
	template <catcher Catcher>
	auto throw_to() -> void {
		if (!have_ball_) {
			throw std::logic_error{"Tried to throw ball but we don't have it!"};
		}
		static constexpr auto Thrower = thrower{Player.v};
		have_ball_ = false;
		ball->throw_to<Thrower, Catcher>();
	}
	auto catch_ball() -> bool {
		if (have_ball_) {
			throw std::logic_error{"Tried to catch ball but we already have it!"};
		}
		if (ball->catch_ball<catcher{Player.v}>()) {
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
	template <catcher IfSuccessThenThrowTo>
	auto with_ball(auto&& fn) -> void {
		if (ensure()) {
			fn();
			throw_to<IfSuccessThenThrowTo>();
		}
	}
private:
	bool have_ball_{};
};

} // ez
