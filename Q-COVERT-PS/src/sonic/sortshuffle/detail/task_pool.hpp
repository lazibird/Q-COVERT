#pragma once

#include <cstddef>
#include <memory>
#include <utility>

#include "sonic/threads/thread_pool.hpp"

namespace sn {
namespace sortshuffle {
namespace detail {

class task_pool {
public:
  task_pool() noexcept = default;

  task_pool(sn::threads::thread_pool& pool, std::size_t capacity) { reset(pool, capacity); }

  task_pool(const task_pool&) = delete;
  task_pool& operator=(const task_pool&) = delete;

  task_pool(task_pool&& other) noexcept :
      pool_(other.pool_), storage_(std::move(other.storage_)), capacity_(other.capacity_), in_use_(other.in_use_) {
    other.pool_ = nullptr;
    other.capacity_ = 0;
    other.in_use_ = 0;
  }

  task_pool& operator=(task_pool&& other) noexcept {
    if (this != &other) {
      pool_ = other.pool_;
      storage_ = std::move(other.storage_);
      capacity_ = other.capacity_;
      in_use_ = other.in_use_;
      other.pool_ = nullptr;
      other.capacity_ = 0;
      other.in_use_ = 0;
    }
    return *this;
  }

  void reset(sn::threads::thread_pool& pool, std::size_t capacity) {
    pool_ = &pool;
    capacity_ = capacity;
    in_use_ = 0;
    if (capacity_ == 0) {
      storage_.reset();
    } else {
      storage_ = std::make_unique<sn::threads::thread_pool::task[]>(capacity_);
    }
  }

  [[nodiscard]] std::size_t capacity() const noexcept { return capacity_; }
  [[nodiscard]] std::size_t available() const noexcept { return capacity_ - in_use_; }
  [[nodiscard]] bool can_acquire(std::size_t count) const noexcept { return count <= available(); }

  class group {
  public:
    explicit group(task_pool& owner) noexcept : owner_(&owner), start_(owner.in_use_) {}
    group(const group&) = delete;
    group& operator=(const group&) = delete;

    group(group&& other) noexcept : owner_(other.owner_), start_(other.start_), scheduled_(other.scheduled_) {
      other.owner_ = nullptr;
      other.scheduled_ = 0;
    }

    group& operator=(group&& other) noexcept {
      if (this != &other) {
        if (owner_) {
          wait();
        }
        owner_ = other.owner_;
        start_ = other.start_;
        scheduled_ = other.scheduled_;
        other.owner_ = nullptr;
        other.scheduled_ = 0;
      }
      return *this;
    }

    ~group() { wait(); }

    template <typename Fn> bool schedule(Fn&& fn) noexcept {
      if (!owner_) {
        return false;
      }
      if (owner_->in_use_ >= owner_->capacity_) {
        return false;
      }
      auto& task = owner_->storage_[owner_->in_use_++];
      ++scheduled_;
      task.assign(std::forward<Fn>(fn));
      owner_->pool_->schedule(task);
      return true;
    }

    void wait() noexcept {
      if (!owner_) {
        return;
      }
      for (std::size_t i = 0; i < scheduled_; ++i) {
        auto& task = owner_->storage_[start_ + i];
        task.wait();
        task.reset();
      }
      owner_->in_use_ = start_;
      owner_ = nullptr;
      scheduled_ = 0;
    }

  private:
    task_pool* owner_ = nullptr;
    std::size_t start_ = 0;
    std::size_t scheduled_ = 0;
  };

  group make_group() noexcept { return group(*this); }

private:
  friend class group;
  sn::threads::thread_pool* pool_ = nullptr;
  std::unique_ptr<sn::threads::thread_pool::task[]> storage_;
  std::size_t capacity_ = 0;
  std::size_t in_use_ = 0;
};

}

}
}
