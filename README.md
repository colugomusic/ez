# ez

Realtime-safe thread synchronization. The motivation for this library is audio application programming but it may be useful in other realtime contexts.

## Usage

There is more documentation [in the header](include/ez.hpp).

Here is a very basic example:

```c++
struct Value {
   ...
};
ez::sync<Value> value_;
void ui_thread() {
  value_.update_publish(ez::ui, [](Value&& v) {
    // Update v
    return v;
  });
}
void realtime_safe_audio_thread() {
  // Get the latest published version of the value. This is lock-free.
  auto v = value_.read(ez::audio);
  // As long as we hold at least one reference to v,
  // this version of the value will not be reclaimed.
}
void garbage_collection_thread() {
  // Reclaim unused versions of the value.
  value_.gc(ez::gc);
}
```

For a fairly extensive usage example you could look at [this project](https://github.com/search?q=repo%3Acolugomusic%2Fscuff+ez%3A%3A&type=code).

## Function annotations
These `ez::ui`, `ez::audio`, `ez::gc` things shown above are basically just annotations which have no runtime cost (the compiler will optimize them away.) This is a coding convention that I have developed which I find useful. There is nothing magic about it. I just find that being forced to declare at a function call-site which thread you're in makes things clearer and less error-prone. It makes it more difficult to accidentally call a realtime-unsafe API from a realtime thread. Most of these annotations are simply aliases for `ez::rt` or `ez::nort`.

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

If you're doing this in your code then you could consider a function with no annotation to be implicitly thread-safe and realtime-safe. You can also explicitly annotate a function as thread-safe and realtime-safe using `ez::safe_t`, which is a bit special in that it can be implicitly constructed from either `ez::rt` or `ez::nort`:

```c++
void foo(ez::safe_t) { ... }

void audio_callback() {
  foo(ez::audio);
}

void ui_thread() {
  foo(ez::ui);
}
```
