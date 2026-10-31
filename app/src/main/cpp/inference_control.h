#pragma once
#include <atomic>
#include <cstdint>
#include <mutex>

// A delayed cancellation must never abort the next page's inference.
class InferenceCancellation {
 public:
  class Scope {
   public:
    Scope(InferenceCancellation &owner, uint64_t token, std::atomic<bool> &abort)
        : owner_(owner), token_(token) { owner_.begin(token, abort); }
    ~Scope() { owner_.finish(token_); }
    Scope(const Scope &) = delete;
    Scope &operator=(const Scope &) = delete;
   private:
    InferenceCancellation &owner_;
    uint64_t token_;
  };
  void cancel(uint64_t token) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (token != 0 && token == token_ && abort_) abort_->store(true);
  }
 private:
  void begin(uint64_t token, std::atomic<bool> &abort) {
    std::lock_guard<std::mutex> lock(mutex_);
    token_ = token;
    abort_ = &abort;
  }
  void finish(uint64_t token) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (token_ == token) { token_ = 0; abort_ = nullptr; }
  }
  std::mutex mutex_;
  uint64_t token_ = 0;
  std::atomic<bool> *abort_ = nullptr;
};
