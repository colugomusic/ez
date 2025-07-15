# ez

Realtime-safe thread synchronization. The motivation for this library is audio application programming but it may be useful in other realtime contexts.

## ez::sync<T>

This can be used in any situation where you have one or more non-realtime writers and one or more realtime readers.

There is more documentation [in the header](include/ez.hpp).

### Example

```c++
struct Value { ... };

ez::sync<Value> value_;

void ui_thread() {
	// 'update' means 'update the working value'. 'publish' means 'make
	// the working value visible to realtime readers'.
	value_.update_publish(ez::ui, [](Value&& v) {
		// Update v
		return v;
	});
}

void realtime_safe_audio_thread() {
	// Get the latest published version of the value. This is realtime-safe.
	auto v = value_.read(ez::audio);
	// As long as we hold at least one reference to v,
	// this version of the value will not be reclaimed.
}

void garbage_collection_thread() {
	// Periodically call this to reclaim unused versions of the value.
	// The memory is reused.
	value_.gc(ez::gc);
}
```

For a fairly extensive usage example you could look at [this project](https://github.com/colugomusic/scuff).

## Let's go to the beach

<img width="512" height="512" align="right" alt="beach-ball-512" src="https://github.com/user-attachments/assets/724a573d-90cb-4325-adf9-e3f40e1bc632" />

I developed a method of realtime-safe sychronization which I call Beach Ball Synchronization that can be used in any situation where two or more threads take it in turns to work with some critical memory region.

By "take it in turns" I mean that, once a thread finishes working with the shared resource, it cannot do so again until some other thread says, "Okay, it's your turn again." Every time a thread finishes doing some work, it must pick another thread whose turn it is to work with the resource next.

This turns out to be a very generic and versatile technique for use in situations where you know at compile-time exactly how many threads are involved in a particular algorithm, and you know that those threads will all be constantly running. (Technically you could write a dynamic version which works with a runtime-known number of threads but that makes my head spin a bit and I don't personally need it so I'll leave it as an exercise for the reader.)

This is implemented in [ez-beach.hpp](include/ez-beach.hpp).

If it helps then you can imagine the threads as people on a beach throwing a beach ball to each other. Only the player currently holding the beach ball is allowed to work on the shared resource. Once they are done working on the resource, they must throw the ball to another player. A player can only catch the ball if it has been specifically thrown to them by another player.

Note that when using this technique, there is zero danger of that classic problem where a thread is repeatedly attempting to acquire access to a shared resource, and it keeps failing because other threads are jumping in and acquiring access before it gets a chance to. In this system, each thread is guaranteed to get its turn with the resource at a fair and regular interval, assuming all threads are, at the very least, fulfilling their contract of catching the ball and throwing it on.

I use this for updating sample data mipmaps (used for rendering waveform visuals) in [this library](https://github.com/colugomusic/adrian). In this library the audio thread can write sample data to a buffer. The work of generating sample mipmap information is done in the UI thread. The audio thread only wants to do the bare minimum amount of work (copy the raw sample data to an intermediate buffer). If it can't do this because it's not currently holding the beach ball then it simply marks the dirty region of the buffer and tries again later (on the next iteration of the audio callback.) It is guaranteed that eventually the beach ball will be thrown back to the audio thread and it will have its chance to transfer the dirty region of the buffer into the critical memory region.

### Example

```c++
static constexpr auto MIPMAP_AUDIO_CATCHER = ez::catcher{0};
static constexpr auto MIPMAP_UI_CATCHER    = ez::catcher{1};
using mipmap_beach_ball   = ez::beach_ball<ez::player_count{2}>;
using mipmap_player_audio = mipmap_beach_ball::player<MIPMAP_AUDIO_CATCHER.v>;
using mipmap_player_ui    = mipmap_beach_ball::player<MIPMAP_UI_CATCHER.v>;

struct mipmap_beach {
	mipmap_beach_ball ball    = {MIPMAP_AUDIO_CATCHER};
	mipmap_player_audio audio = ball.make_player<MIPMAP_AUDIO_CATCHER.v>();
	mipmap_player_ui ui       = ball.make_player<MIPMAP_UI_CATCHER.v>();
};

mipmap_beach beach;

...

// in the UI thread
void update_mipmaps(ez::ui_t, ...) {
	// Try to catch the ball. If this fails then it simply means that
	// the ball has not been thrown to the UI thread and so it's not
	// our turn yet to work with the shared buffer.
	beach.ui.with_ball<MIPMAP_AUDIO_CATCHER>([]{
		// Safely do work with the shared buffer.
		// When we are done, the ball will be thrown to the audio thread
		// (as specified by the with_ball<> template argument.)
	});
}

// in the audio thread
void update_mipmaps(ez::audio_t, ...) {
	// Try to catch the ball. If this fails then it simply means that
	// the ball has not been thrown to the audio thread and so it's not
	// our turn yet to work with the shared buffer.
	beach.audio.with_ball<MIPMAP_UI_CATCHER>([]{
		// Safely do work with the shared buffer.
		// When we are done, the ball will be thrown to the UI thread
		// (as specified by the with_ball<> template argument.)
	});
}
```

## Function annotations
These `ez::ui`, `ez::audio`, `ez::gc` things used above are basically just annotations which have no runtime cost (the compiler will optimize them away.) This is a coding convention that I have developed which I find useful. There is nothing magic about it. I just find that being forced to declare which thread you're in at a function call-site tends to make things much clearer and less error-prone, and it makes it more difficult to accidentally call a realtime-unsafe API from a realtime thread. Most of these annotations are simply aliases for `ez::rt` or `ez::nort`.

- `rt`: indicates that we are in a realtime thread. Aliases: `audio`
- `nort`: indicates that we are in a non-realtime thread. Aliases: `main`, `ui`, `gc`
- `safe` indicates that the function can be safely called from any thread.

You can use these annotations in your own code if you want:

```c++
// audio_t indicates that this function will be called from an audio thread.
void foo(ez::audio_t) { ... }

void bar1(ez::audio_t) {
  // Call another audio-thread function from an audio thread.
  foo(ez::audio);
}

void bar2(ez::audio_t c) {
  // You could also just pass on the argument like this if you prefer.
  foo(c);
}

void main() {
  // Won't compile, because foo expects to be called from an audio
  // thread.
  foo(ez::main);

  // Will compile. There's nothing stopping you from lying.
  // These annotations aren't magic. They're just a convention which
  // I find useful because it makes mistakes less likely and the code
  // clearer IMO.
  // Seeing this word 'audio' here should jump out at you as a mistake
  // because this clearly isn't the audio thread so we must be doing
  // something wrong.
  foo(ez::audio); 
}
```
