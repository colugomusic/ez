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
These `ez::ui`, `ez::audio`, `ez::gc` things shown above are basically just annotations which have no runtime cost (the compiler will optimize them away.) They are there just to make it more difficult to accidentally call a realtime-unsafe API from a realtime thread. Most of these are simply aliases for `ez::rt` or `ez::nort`. Just [look at the header](include/ez.hpp) to see what's going on. It's really simple.

You can also use these annotations in your own code if you want:

```c+++
// audio_t indicates that this function will be called from an audio thread.
void foo(ez::audio_t) { ... }
void bar1(ez::audio_t) {
  foo(ez::audio); // Call another audio-thread function from an audio thread.
}
void bar2(ez::audio_t c) {
  foo(c); // You could also just pass on the argument like this if you prefer.
}
void main() {
  foo(ez::main); // Won't compile
  foo(ez::audio); // Will compile, there's nothing stopping you from lying. These annotations aren't magic. They're just a convention which I find useful because it makes mistakes less likely and the code clearer IMO.
}
```

If you're doing this then you could consider a function with no annotation to be implicitly thread-safe and realtime-safe. You can also explicitly annotate a function as thread-safe and realtime-safe using `ez::safe_t` which can be constructed from `ez::rt` or `ez::nort`:

```c++
void foo(ez::safe_t) { ... }

void audio_callback() {
  foo(ez::audio); // Compiles
}

void ui_thread() {
  foo(ez::ui); // Compiles
}
```
