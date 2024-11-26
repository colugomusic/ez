#pragma once

#include <cassert>
#include <cs_lr_guarded.h>
#include <cs_plain_guarded.h>
#include <vector>

namespace ez {

// Wrapper around lr_guarded for basic publishing of data.
// Shared pointers to old versions of the data are kept in a list to ensure that they are
// not deleted if released by a realtime reader thread.
// garbage_collect() should be called periodically to delete old versions.
template <typename T>
struct data {
	template <typename UpdateFn>
	auto modify(UpdateFn&& update_fn) -> void {
		modify({writer_mutex_}, std::forward<UpdateFn>(update_fn));
	}
	auto set(T data) -> void {
		auto lock = std::unique_lock(writer_mutex_);
		auto modify_fn = [data = std::move(data)](T&&) mutable { return std::move(data); };
		modify(std::move(lock), modify_fn);
	}
	auto read() const -> std::shared_ptr<const T> {
		return *ptr_.lock_shared().get();
	}
	auto garbage_collect() -> void {
		auto lock = std::unique_lock(writer_mutex_);
		auto is_garbage = [](const std::shared_ptr<const T>& ptr) { return ptr.use_count() == 1; };
		versions_.erase(std::remove_if(versions_.begin(), versions_.end(), is_garbage), versions_.end());
	}
private:
	template <typename UpdateFn>
	auto modify(std::unique_lock<std::mutex>&& lock, UpdateFn&& update_fn) -> void {
		auto copy = std::make_shared<const T>(writer_data_ = update_fn(std::move(writer_data_)));
		ptr_.modify([copy](std::shared_ptr<const T>& ptr) { ptr = copy; });
		versions_.push_back(std::move(copy));
	}
	T writer_data_;
	std::mutex writer_mutex_;
	libguarded::lr_guarded<std::shared_ptr<const T>> ptr_;
	std::vector<std::shared_ptr<const T>> versions_;
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
	[[nodiscard]] auto rt_read() -> std::shared_ptr<const T> {
		auto value = published_value_.read();
		unread_value_.exchange(false);
		return value;
	}
	[[nodiscard]] auto is_unread() const -> bool {
		return unread_value_.load(std::memory_order_acquire);
	}
private:
	mutable std::mutex mutex_;
	T working_value_;
	ez::data<T> published_value_;
	std::atomic_bool unread_value_ = true;
};

struct sync_signal {
	std::atomic<uint64_t> value = 1;
};

inline
auto signal(sync_signal* signal) -> void {
	signal->value.fetch_add(1, std::memory_order_release);
}

template <typename T>
struct signalled_sync : sync<T> {
	signalled_sync(const sync_signal& signal) : signal_{&signal} {}
	auto rt_read() -> std::shared_ptr<const T>& {
		dbg_check_single_thread();
		auto signal_value = signal_->value.load(std::memory_order_release);
		if (signal_value > local_signal_value_) {
			local_signal_value_ = signal_value;
			signalled_data_     = sync<T>::rt_read();
		}
		return signalled_data_;
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
	std::shared_ptr<const T> signalled_data_;
};

template <typename T, size_t N>
struct signalled_sync_array {
	signalled_sync_array(const sync_signal& signal) : ss_{signal} {}
	auto gc() -> void {
		ss_.gc();
	}
	auto rt_read_into(size_t slot) -> const T& {
		assert (slot >= 0 && slot < N);
		return *(array_[slot] = ss_.rt_read());
	}
	[[nodiscard]] auto is_unread() const -> bool {
		return ss_.is_unread();
	}
	auto set_publish(T value) -> void {
		ss_.set_publish(std::move(value));
	}
private:
	ez::signalled_sync<T> ss_;
	std::array<std::shared_ptr<const T>, N> array_;
};

} // ez
