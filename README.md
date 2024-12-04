# ez

Realtime-safe thread synchronization.

## Usage

There are many ways to use this library so I will document them properly one day. There is some documentation [in the header](include/ez.hpp).

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
void realtime_safe_thread() {
  // Get the latest version of the value. This is lock-free.
  auto v = value_.read(ez::rt);
  // As long as we hold at least one reference to v,
  // this version of the value will not be reclaimed.
}
void gc_thread() {
  // Reclaim unused versions of the value.
  value_.gc(ez::nort);
}
```
