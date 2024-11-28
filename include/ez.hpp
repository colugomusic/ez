#pragma once

#include <cassert>
#include <deque>
#include <memory>
#include <optional>
#include <vector>

namespace ez {

// These tags have no runtime cost. The point of them is just to force the user
// to type ez::rt or ez::nort to reduce the chance of accidentally calling a
// non-realtime-safe function from a realtime thread.

struct nort_t {}; // Indicates that the calling thread is not a realtime thread.
struct rt_t {};   // Indicates that the calling thread is a realtime thread.

static constexpr auto nort = nort_t{};
static constexpr auto rt   = rt_t{};

// Just some aliases.
static constexpr auto audio = rt;
static constexpr auto main  = nort;
static constexpr auto ui    = nort;

// It is possible for the user to lie about whether they are calling a function
// from a realtime thread or not. If they do that then I don't guarantee that
// anything will work the way they expect.

template <typename T> struct immutable;

// The reason for storing shared_ptr<optional<T>>s instead of simply
// shared_ptr<T>s is that we DON'T want to free the memory being used for
// garbage collected Ts; we want to reuse that memory instead. However we
// still want to run the T's destructor when it is flagged as dead, and
// if we did that manually then shared_ptr would end up re-calling the
// destructor on an already-destroyed T. Using optional<T> just gets us
// exactly the behavior we want.

template <typename T>
struct version {
	version() : ptr_{std::make_shared<std::optional<T>>()} {}
	auto clear() -> void                          { ptr_->reset(); }
	auto set(T value) -> void                     { *ptr_ = std::move(value); }
	[[nodiscard]] auto is_garbage() const -> bool { return ptr_.use_count() <= 1; }
private:
	std::shared_ptr<std::optional<T>> ptr_;
	friend struct immutable<T>;
};

template <typename T>
struct immutable {
	immutable() = default;
	immutable(version<T> v) : ptr_{std::move(v.ptr_)} {}
	const T* operator->() const { return &ptr_->value(); }
	const T& operator*() const  { return ptr_->value(); }
private:
	// The underlying optional accessible though this interface
	// is logically guaranteed to always hold a value.
	std::shared_ptr<const std::optional<T>> ptr_;
};

// Shared pointers to old versions of the value are kept in a list to ensure that they are
// not reclaimed if released by a realtime reader thread.
// The memory allocated for different versions of the data is reused to avoid unnecessary
// (de)allocations.
// garbage_collect() should be called periodically to reclaim memory. You could do this every
// time you modify the value if you want, and that would work fine. If the template
// parameter 'auto_gc' is set to true then that will happen automatically.
// Or you could have a background thread that calls garbage_collect() on a timer or whatever.
// The garbage collection operation is relatively inexpensive.
// Note that if garbage collection isn't run then destructors won't be run for old Ts.
// Every public function here is thread-safe.
// Only read() is lock-free.
template <typename T, bool auto_gc = false>
struct value {
	template <typename UpdateFn>
	auto modify(ez::nort_t, UpdateFn&& update_fn) -> void {
		auto lock = std::unique_lock{writer_mutex_};
		auto new_value = update_fn(std::move(writer_value_));
		writer_value_ = new_value;
		const auto index = get_empty_version();
		current_version_ = versions_[index];
		versions_[index].set(std::move(new_value));
		dead_flags_[index] = false;
		current_version_ptr_.store(&versions_[index], std::memory_order_release);
		if constexpr (auto_gc) { garbage_collect(lock); }
	}
	auto set(ez::nort_t, T value) -> void {
		modify(ez::nort, [value = std::move(value)](T&&) mutable { return std::move(value); });
	}
	auto read(ez::nort_t) const -> immutable<T> {
		return read();
	}
	auto read(ez::rt_t) const -> immutable<T> {
		return read();
	}
	auto garbage_collect(ez::nort_t) -> void {
		garbage_collect(std::unique_lock{writer_mutex_});
	}
private:
	auto garbage_collect(std::unique_lock<std::mutex>&& lock) -> void {
		for (auto index : get_alive_versions(&index_buffer_)) {
			if (versions_[index].is_garbage()) {
				kill(index);
			}
		}
	}
	auto kill(size_t index) -> void {
		versions_[index].clear();
		dead_flags_[index] = true;
	}
	auto read() const -> immutable<T> {
		auto version = *current_version_ptr_.load(std::memory_order_acquire);
		return immutable<T>{version};
	}
	auto get_alive_versions(std::vector<size_t>* out) const -> const std::vector<size_t>& {
		out->clear();
		for (size_t i = 0; i < dead_flags_.size(); i++) {
			if (!dead_flags_[i]) { out->push_back(i); }
		}
		return *out;
	}
	auto get_empty_version() -> size_t {
		for (size_t i = 0; i < dead_flags_.size(); i++) {
			if (dead_flags_[i]) { return i; }
		}
		auto index = size_t{versions_.size()};
		versions_.emplace_back();
		dead_flags_.push_back(true);
		assert ((index + 1) == versions_.size() && (index + 1) == dead_flags_.size());
		return index;
	}
	T writer_value_;
	std::mutex writer_mutex_;
	std::atomic<version<T>*> current_version_ptr_ = nullptr;
	// We hold this just to keep the refcount > 1 so that the current
	// version is not considered garbage.
	version<T> current_version_;
	std::deque<version<T>> versions_;
	std::vector<bool> dead_flags_;
	std::vector<size_t> index_buffer_;
};

// An 'update' or 'set' operation changes the working value, but does not yet
// commit the change to be visible to realtime readers.
// A 'publish' operation makes the new value visible to realtime readers.
template <typename T, bool auto_gc = false>
struct sync {
	sync()                                                                { publish(ez::nort); }
	auto gc(ez::nort_t) -> void                                           { published_value_.garbage_collect(ez::nort); }
	[[nodiscard]] auto read(ez::nort_t) const -> T                        { auto lock = std::lock_guard{mutex_}; return working_value_; }
	template <typename Fn> auto update(ez::nort_t, Fn fn) -> void         { auto lock = std::lock_guard{mutex_}; working_value_ = fn(std::move(working_value_)); }
	template <typename Fn> auto update_publish(ez::nort_t, Fn fn) -> void { update(ez::nort, fn); publish(ez::nort); }
	auto set(ez::nort_t, T value) -> void                                 { auto lock = std::lock_guard{mutex_}; working_value_ = std::move(value); }
	auto set_publish(ez::nort_t, T value) -> void                         { set(ez::nort, std::move(value)); publish(ez::nort); }
	auto publish(ez::nort_t) -> void {
		published_value_.set(ez::nort, working_value_);
		unread_value_.store(true, std::memory_order_release);
	}
	[[nodiscard]] auto read(ez::rt_t) -> immutable<T> {
		auto value = published_value_.read(ez::rt);
		unread_value_.store(false, std::memory_order_release);
		return value;
	}
	// Check if the latest published value has been observed by a realtime reader at least once.
	[[nodiscard]] auto is_unread(ez::rt_t) const -> bool { return unread_value_.load(std::memory_order_acquire); }
private:
	mutable std::mutex mutex_;
	T working_value_;
	ez::value<T, auto_gc> published_value_;
	std::atomic_bool unread_value_ = true;
};

struct sync_signal {
	auto get(ez::rt_t) const -> uint64_t { return value_; }
	auto increment(ez::rt_t) -> void     { value_++; }
private:
	uint64_t value_ = 1;
};

// Holds the most recently fetched version of the most recently published
// version of a value.
// The published value is only fetched when the associated sync_signal is
// incremented.
// The motivation for this is an audio callback, e.g.
/* ----------------------------------------------------------------------
static ez::sync_signal sync_signal;
static ez::signalled_sync<Value> sync{sync_signal};

void audio_callback(...) {
	
	// The signal is incremented once at the beginning of each audio
	// iteration. This ensures that read() will always return the same
	// value until the next iteration.
	sync_signal.increment();
	
	// Retrieve the most recently fetched value.
	auto value1 = sync.read(ez::audio);
	
	...
	
	// The UI thread might publish a new version of the value at this
	// point.
	
	...
	
	// Retrieve the value again.
	auto value2 = sync.read(ez::audio);
	
	// Guaranteed to pass, because the signal is still at the same value.
	assert(value1 == value2);
	
}
---------------------------------------------------------------------- */
template <typename T, bool auto_gc = false>
struct signalled_sync : sync<T, auto_gc> {
	signalled_sync(const sync_signal& signal) : signal_{&signal} {}
	// CAUTION:
	// This assumes that there is exactly one simultaneous realtime reader.
	// If you want to support multiple realtime readers you'll have to fork
	// and complicate this class.
	auto read(ez::rt_t) -> immutable<T>& {
		auto signal_value = signal_->get(ez::rt);
		if (signal_value > local_signal_value_) {
			local_signal_value_ = signal_value;
			signalled_value_    = sync<T, auto_gc>::read(ez::rt);
		}
		return signalled_value_;
	}
private:
	const sync_signal* signal_;
	uint64_t local_signal_value_ = 0;
	immutable<T> signalled_value_;
};

template <typename T, size_t N, bool auto_gc = false>
struct signalled_sync_array {
	signalled_sync_array(const sync_signal& signal) : ss_{signal} {}
	auto gc(ez::nort_t) -> void                            { ss_.gc(ez::nort); }
	auto read_into(ez::rt_t, size_t slot) -> const T&      { assert (slot >= 0 && slot < N); return *(array_[slot] = ss_.read(ez::rt)); }
	[[nodiscard]] auto is_unread(ez::nort_t) const -> bool { return ss_.is_unread(ez::nort); }
	[[nodiscard]] auto is_unread(ez::rt_t) const -> bool   { return ss_.is_unread(ez::rt); }
	auto set_publish(ez::nort_t, T value) -> void          { ss_.set_publish(ez::nort, std::move(value)); }
private:
	ez::signalled_sync<T, auto_gc> ss_;
	std::array<immutable<T>, N> array_;
};

struct trigger {
	trigger()                 { flag_.clear(); flag_.test_and_set(std::memory_order_relaxed); }
	auto operator()() -> void { flag_.clear(std::memory_order_relaxed); }
	operator bool()           { return !(flag_.test_and_set(std::memory_order_relaxed)); }
private:
	std::atomic_flag flag_;
};

} // ez
