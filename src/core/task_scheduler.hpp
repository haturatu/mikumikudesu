#pragma once

#include <cstddef>
#include <functional>
#include <memory>

namespace dayo::core {

// Small persistent worker pool for independent frame jobs. Work is submitted
// through parallelFor so callers wait only for the specific batch they own;
// the pool itself remains alive across frames.
class TaskScheduler {
  public:
    explicit TaskScheduler(std::size_t workerCount = 0);
    ~TaskScheduler();
    TaskScheduler(TaskScheduler&&) noexcept;
    TaskScheduler& operator=(TaskScheduler&&) noexcept;
    TaskScheduler(const TaskScheduler&) = delete;
    TaskScheduler& operator=(const TaskScheduler&) = delete;

    void parallelFor(std::size_t count, const std::function<void(std::size_t)>& function);
    void parallelFor(std::size_t count, std::size_t grainSize, const std::function<void(std::size_t)>& function);
    [[nodiscard]] std::size_t workerCount() const noexcept;

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace dayo::core
