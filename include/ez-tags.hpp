#pragma once

namespace ez {

// These tags have no runtime cost. The point of them is just to force the user
// to type ez::rt or ez::nort to reduce the chance of accidentally calling a
// non-realtime-safe function from a realtime thread.

struct nort_t {}; // Indicates that the calling thread is not a realtime thread.
struct rt_t {};   // Indicates that the calling thread is a realtime thread.

// Used to indicate that a function is completely thread-safe and realtime-safe.
struct safe_t {
	safe_t() = default;
	safe_t(nort_t){}
	safe_t(rt_t){}
};

static constexpr auto nort = nort_t{};
static constexpr auto rt   = rt_t{};
static constexpr auto safe = safe_t{};

// Just some aliases.
using audio_t = rt_t;
using gc_t    = nort_t;
using main_t  = nort_t;
using ui_t    = nort_t;
static constexpr auto audio = rt;
static constexpr auto gc    = nort;
static constexpr auto main  = nort;
static constexpr auto ui    = nort;

// It is possible for the user to lie about whether they are calling a function
// from a realtime thread or not. If they do that then I don't guarantee that
// anything will work the way they expect.

} // ez