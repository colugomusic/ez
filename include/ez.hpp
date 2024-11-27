#pragma once

#include <cassert>
#include <deque>
#include <optional>
#include <vector>

namespace ez {

template <typename T> struct immutable_value_ref;

template <typename T>
struct version {
	version() : ptr_{std::make_shared<std::optional<T>>()} {}
	auto clear() -> void                          { ptr_->reset(); }
	auto set(T value) -> void                     { *ptr_ = std::move(value); }
	[[nodiscard]] auto is_garbage() const -> bool { return ptr_.use_count() <= 1; }
private:
	std::shared_ptr<std::optional<T>> ptr_;
	friend struct immutable_value_ref<T>;
};

template <typename T>
struct immutable_value_ref {
	immutable_value_ref() = default;
	immutable_value_ref(version<T> ptr) : ptr_{std::move(ptr.ptr_)} {}
	const T* operator->() const { return &ptr_->value(); }
	const T& operator*() const  { return ptr_->value(); }
private:
	std::shared_ptr<const std::optional<T>> ptr_;
};

// Shared pointers to old versions of the value are kept in a list to ensure that they are
// not reclaimed if released by a realtime reader thread.
// The memory allocated for different versions of the data is reused to avoid unnecessary
// allocations.
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
		auto lock = std::lock_guard{writer_mutex_};
		auto new_value = update_fn(std::move(writer_value_));
		writer_value_ = new_value;
		const auto index = get_empty_version();
		current_version_ = versions_[index];
		versions_[index].set(std::move(new_value));
		dead_flags_[index] = false;
		current_version_ptr_.store(&versions_[index], std::memory_order_release);
	}
	auto set(T value) -> void {
		modify([value = std::move(value)](T&&) mutable { return std::move(value); });
	}
	// This is a lock-free operation which will get you the most recently published value.
	auto read() const -> immutable_value_ref<T> {
		auto version = *current_version_ptr_.load(std::memory_order_acquire);
		return immutable_value_ref<T>{version};
	}
	auto garbage_collect() -> void {
		auto lock = std::lock_guard{writer_mutex_};
		get_alive_versions(&index_buffer_);
		for (auto index : index_buffer_) {
			auto& version = versions_[index];
			if (version.is_garbage()) {
				version.clear();
				dead_flags_[index] = true;
			}
		}
	}
private:
	auto get_alive_versions(std::vector<size_t>* out) -> void {
		out->clear();
		for (size_t i = 0; i < dead_flags_.size(); i++) {
			if (!dead_flags_[i]) { out->push_back(i); }
		}
	}
	[[nodiscard]]
	auto get_empty_version() -> size_t {
		for (size_t i = 0; i < dead_flags_.size(); i++) {
			if (dead_flags_[i]) { return i; }
		}
		auto index = size_t{versions_.size()};
		versions_.emplace_back();
		dead_flags_.push_back(true);
		assert ((index + 1) == versions_.size() == dead_flags_.size());
		return index;
	}
	T writer_value_;
	std::mutex writer_mutex_;
	std::atomic<version<T>*> current_version_ptr_ = nullptr;
	version<T> current_version_;
	std::deque<version<T>> versions_;
	std::vector<bool> dead_flags_;
	std::vector<size_t> index_buffer_;
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
