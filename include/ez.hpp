#pragma once

#include <cassert>
#include <cs_lr_guarded.h>
#include <cs_plain_guarded.h>
#include <optional>
#include <vector>

namespace ez {

template <typename T> struct immutable_value_ref;

template <typename T>
struct value_ptr {
	value_ptr() = default;
	auto reset() -> void                         { ptr_->reset(); }
	auto set(T value) -> void                    { *ptr_ = std::move(value); }
	[[nodiscard]] auto use_count() const -> long { return ptr_.use_count(); }
	[[nodiscard]] static
	auto make(T value) -> value_ptr<T> {
		return value_ptr<T>{std::make_shared<std::optional<T>>(std::move(value))};
	}
private:
	value_ptr(std::shared_ptr<std::optional<T>>&& ptr) : ptr_{std::move(ptr)} {}
	std::shared_ptr<std::optional<T>> ptr_;
	friend struct immutable_value_ref<T>;
};

template <typename T>
struct immutable_value_ref {
	immutable_value_ref() = default;
	immutable_value_ref(value_ptr<T> ptr) : ptr_{std::move(ptr.ptr_)} {}
	const T* operator->() const { return &ptr_->value(); }
	const T& operator*() const  { return ptr_->value(); }
private:
	std::shared_ptr<const std::optional<T>> ptr_;
};

// Wrapper around lr_guarded for basic publishing of a value.
// Shared pointers to old versions of the value are kept in a list to ensure that they are
// not reclaimed if released by a realtime reader thread.
// The memory allocated for different versions of the data is reused as much as possible
// to avoid unnecessary allocations.
// garbage_collect() should be called periodically to reclaim old versions. You could do
// this every time you modify the value if you want, and that would work fine. Garbage
// collection is unlikely to be expensive. Or you could have a background thread that
// calls it on a timer or whatever.
// Note that if you don't call garbage_collect() then destructors won't be run for those
// old values.
// Every public function here is thread-safe.
template <typename T>
struct value {
	template <typename UpdateFn>
	auto modify(UpdateFn&& update_fn) -> void {
		modify({writer_mutex_}, std::forward<UpdateFn>(update_fn));
	}
	auto set(T value) -> void {
		auto lock = std::unique_lock(writer_mutex_);
		auto modify_fn = [value = std::move(value)](T&&) mutable { return std::move(value); };
		modify(std::move(lock), modify_fn);
	}
	// This is a lock-free operation which will get you the most recently published value.
	auto read() const -> immutable_value_ref<T> {
		return *ptr_.lock_shared().get();
	}
	auto garbage_collect() -> void {
		auto lock = std::unique_lock(writer_mutex_);
		auto is_garbage = [](const value_ptr<T>& ptr) { return ptr.use_count() == 1; };
		auto garbage_beg = std::remove_if(versions_.begin(), versions_.end(), is_garbage);
		auto garbage_end = versions_.end();
		for (auto it = garbage_beg; it != garbage_end; ++it) { move_to_pool(std::move(*it)); }
		versions_.erase(garbage_beg, garbage_end);
	}
private:
	template <typename UpdateFn>
	auto modify(std::unique_lock<std::mutex>&& lock, UpdateFn&& update_fn) -> void {
		auto copy = make_copy(writer_value_ = update_fn(std::move(writer_value_)));
		ptr_.modify([copy](value_ptr<T>& ptr) { ptr = copy; });
		versions_.push_back(std::move(copy));
	}
	auto make_copy(T value) -> value_ptr<T> {
		if (pool_.empty()) {
			return value_ptr<T>::make(std::move(value));
		}
		auto vptr = pop_from_pool();
		vptr.set(std::move(value));
		return vptr;
	}
	auto move_to_pool(value_ptr<T>&& vptr) -> void {
		vptr.reset();
		pool_.push_back(std::move(vptr));
	}
	auto pop_from_pool() -> value_ptr<T> {
		auto vptr = std::move(pool_.back());
		pool_.pop_back();
		return vptr;
	}
	T writer_value_;
	std::mutex writer_mutex_;
	libguarded::lr_guarded<value_ptr<T>> ptr_;
	std::vector<value_ptr<T>> versions_;
	std::vector<value_ptr<T>> pool_;
};

template <typename T>
struct sync {
	sync()                                                    { publish(); }
	auto gc() -> void                                         { published_value_.garbage_collect(); }
	[[nodiscard]] auto read() const -> T                      { auto lock = std::lock_guard{mutex_}; return working_value_; }
	template <typename Fn> auto update(Fn fn) -> void         { auto lock = std::lock_guard{mutex_}; working_value_ = fn(std::move(working_value_)); }
	template <typename Fn> auto update_publish(Fn fn) -> void { update(fn); publish(); }
	auto set(T value) -> void                                 { auto lock = std::lock_guard{mutex_}; working_value_ = std::move(value); }
	auto set_publish(T value) -> void                         { set(std::move(value)); publish(); }
	auto publish() -> void {
		published_value_.set(working_value_);
		unread_value_.store(true, std::memory_order_release);
	}
	[[nodiscard]] auto rt_read() -> immutable_value_ref<T> {
		auto value = published_value_.read();
		unread_value_.exchange(false);
		return value;
	}
	// This is a convenience function to check if the latest
	// published value has been observed at least once.
	[[nodiscard]] auto is_unread() const -> bool {
		return unread_value_.load(std::memory_order_acquire);
	}
private:
	mutable std::mutex mutex_;
	T working_value_;
	ez::value<T> published_value_;
	std::atomic_bool unread_value_ = true;
};

struct sync_signal {
	[[nodiscard]] auto get() const -> uint64_t { return value_.load(); }
	auto increment() -> void { value_.fetch_add(1); }
private:
	std::atomic<uint64_t> value_ = 1;
};

template <typename T>
struct signalled_sync : sync<T> {
	signalled_sync(const sync_signal& signal) : signal_{&signal} {}
	auto rt_read() -> immutable_value_ref<T>& {
		dbg_check_single_thread();
		auto signal_value = signal_->get();
		if (signal_value > local_signal_value_) {
			local_signal_value_ = signal_value;
			signalled_value_     = sync<T>::rt_read();
		}
		return signalled_value_;
	}
private:
	auto dbg_check_single_thread() -> void {
#	if _DEBUG
		static std::thread::id thread_id{};
		if (thread_id == std::thread::id{}) { thread_id = std::this_thread::get_id(); }
		else                                { assert(thread_id == std::this_thread::get_id() && "signal_sync only supports a single realtime reader thread."); }
#	endif
	}
	const sync_signal* signal_;
	uint64_t local_signal_value_ = 0;
	immutable_value_ref<T> signalled_value_;
};

template <typename T, size_t N>
struct signalled_sync_array {
	signalled_sync_array(const sync_signal& signal) : ss_{signal} {}
	auto gc() -> void                            { ss_.gc(); }
	auto rt_read_into(size_t slot) -> const T&   { assert (slot >= 0 && slot < N); return *(array_[slot] = ss_.rt_read()); }
	[[nodiscard]] auto is_unread() const -> bool { return ss_.is_unread(); }
	auto set_publish(T value) -> void            { ss_.set_publish(std::move(value)); }
private:
	ez::signalled_sync<T> ss_;
	std::array<immutable_value_ref<T>, N> array_;
};

struct trigger {
	trigger()                 { flag_.clear(); flag_.test_and_set(std::memory_order_relaxed); }
	auto operator()() -> void { flag_.clear(std::memory_order_relaxed); }
	operator bool()           { return !(flag_.test_and_set(std::memory_order_relaxed)); }
private:
	std::atomic_flag flag_;
};

} // ez
