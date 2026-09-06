#pragma once

#include <cstddef>
#include <functional>
#include <initializer_list>
#include <memory>
#include <span>

namespace dayo::core {

class TaskScheduler;

class TaskHandle {
  public:
    struct State;

    TaskHandle() = default;
    [[nodiscard]] bool valid() const noexcept {
        return state_ != nullptr;
    }

  private:
    explicit TaskHandle(std::shared_ptr<State> state) : state_(std::move(state)) {}

    std::shared_ptr<State> state_;
    friend class TaskScheduler;
};

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

    using TaskFunction = std::function<void()>;

    void parallelFor(std::size_t count, const std::function<void(std::size_t)>& function);
    void parallelFor(std::size_t count, std::size_t grainSize, const std::function<void(std::size_t)>& function);
    [[nodiscard]] TaskHandle schedule(TaskFunction function);
    [[nodiscard]] TaskHandle scheduleAfter(const TaskHandle& dependency, TaskFunction function);
    [[nodiscard]] TaskHandle scheduleAfter(std::span<const TaskHandle> dependencies, TaskFunction function);
    [[nodiscard]] TaskHandle scheduleAfter(std::initializer_list<TaskHandle> dependencies, TaskFunction function);
    // Blocking waits are for callers outside the worker pool. Worker tasks
    // should express ordering with scheduleAfter instead.
    void wait(const TaskHandle& handle) const;
    [[nodiscard]] std::size_t workerCount() const noexcept;

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace dayo::core
