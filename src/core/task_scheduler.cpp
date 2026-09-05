#include "core/task_scheduler.hpp"

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <exception>
#include <latch>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <utility>
#include <vector>

namespace dayo::core {

struct TaskHandle::State {
    using Continuation = std::function<void()>;

    std::condition_variable condition;
    std::mutex mutex;
    bool completed{};
    std::exception_ptr exception;
    std::vector<Continuation> continuations;
};

void completeTask(const std::shared_ptr<TaskHandle::State>& state, std::exception_ptr exception) {
    std::vector<TaskHandle::State::Continuation> continuations;
    {
        std::lock_guard lock(state->mutex);
        state->exception = std::move(exception);
        state->completed = true;
        continuations = std::move(state->continuations);
    }
    state->condition.notify_all();
    for (auto& continuation : continuations)
        continuation();
}

void addContinuation(const std::shared_ptr<TaskHandle::State>& state, TaskHandle::State::Continuation continuation) {
    TaskHandle::State::Continuation readyContinuation;
    {
        std::lock_guard lock(state->mutex);
        if (state->completed)
            readyContinuation = std::move(continuation);
        else
            state->continuations.push_back(std::move(continuation));
    }
    if (readyContinuation)
        readyContinuation();
}

void waitTask(const std::shared_ptr<TaskHandle::State>& state) {
    if (state == nullptr)
        throw std::invalid_argument("cannot wait for an invalid task handle");
    std::unique_lock lock(state->mutex);
    state->condition.wait(lock, [&] { return state->completed; });
    if (state->exception != nullptr)
        std::rethrow_exception(state->exception);
}

struct TaskScheduler::Impl {
    struct Task {
        std::function<void()> function;
    };

    struct Worker {
        std::deque<Task> tasks;
        std::mutex mutex;
    };

    struct WorkerContext {
        Impl* owner{};
        std::size_t index{};
    };

    static WorkerContext*& currentWorker() noexcept {
        static thread_local WorkerContext* context = nullptr;
        return context;
    }

    struct WorkerScope {
        WorkerContext context;
        WorkerContext* previous;

        WorkerScope(Impl* owner, std::size_t index) : context{owner, index}, previous(currentWorker()) {
            currentWorker() = &context;
        }

        ~WorkerScope() {
            currentWorker() = previous;
        }
    };

    explicit Impl(std::size_t requestedWorkers) {
        const auto hardware = static_cast<std::size_t>(std::thread::hardware_concurrency());
        const auto desired = requestedWorkers == 0 ? (hardware > 1 ? hardware - 1U : 1U) : requestedWorkers;
        workers.reserve(desired);
        for (std::size_t index = 0; index < desired; ++index)
            workers.push_back(std::make_unique<Worker>());
        for (std::size_t index = 0; index < desired; ++index)
            threads.emplace_back([this, index] { run(index); });
    }

    ~Impl() {
        {
            std::lock_guard lock(mutex);
            stopping = true;
        }
        condition.notify_all();
        for (auto& worker : threads)
            if (worker.joinable())
                worker.join();
    }

    bool anyLocalTasks() {
        for (const auto& worker : workers) {
            std::lock_guard lock(worker->mutex);
            if (!worker->tasks.empty())
                return true;
        }
        return false;
    }

    bool tryTake(std::size_t index, Task& result) {
        {
            auto& local = *workers[index];
            std::lock_guard lock(local.mutex);
            if (!local.tasks.empty()) {
                result = std::move(local.tasks.back());
                local.tasks.pop_back();
                return true;
            }
        }
        {
            std::lock_guard lock(mutex);
            if (!tasks.empty()) {
                result = std::move(tasks.front());
                tasks.pop_front();
                return true;
            }
        }
        for (std::size_t offset = 1; offset < workers.size(); ++offset) {
            auto& victim = *workers[(index + offset) % workers.size()];
            std::lock_guard lock(victim.mutex);
            if (!victim.tasks.empty()) {
                result = std::move(victim.tasks.front());
                victim.tasks.pop_front();
                return true;
            }
        }
        return false;
    }

    void run(std::size_t index) {
        while (true) {
            Task task;
            if (tryTake(index, task)) {
                WorkerScope workerScope(this, index);
                task.function();
                continue;
            }

            std::unique_lock lock(mutex);
            condition.wait(lock, [this] { return stopping || !tasks.empty() || anyLocalTasks(); });
            if (stopping && tasks.empty() && !anyLocalTasks())
                return;
        }
    }

    void enqueue(std::function<void()> function) {
        const auto worker = currentWorker();
        if (worker != nullptr && worker->owner == this) {
            auto& local = *workers[worker->index];
            {
                std::lock_guard lock(local.mutex);
                local.tasks.push_back({std::move(function)});
            }
        } else {
            std::lock_guard lock(mutex);
            tasks.push_back({std::move(function)});
        }
        condition.notify_one();
    }

    std::vector<std::unique_ptr<Worker>> workers;
    std::vector<std::thread> threads;
    std::deque<Task> tasks;
    std::mutex mutex;
    std::condition_variable condition;
    bool stopping{};
};

TaskScheduler::TaskScheduler(std::size_t workerCount) : impl_(std::make_unique<Impl>(workerCount)) {}
TaskScheduler::~TaskScheduler() = default;
TaskScheduler::TaskScheduler(TaskScheduler&&) noexcept = default;
TaskScheduler& TaskScheduler::operator=(TaskScheduler&&) noexcept = default;

void TaskScheduler::parallelFor(std::size_t count, const std::function<void(std::size_t)>& function) {
    const auto workers = workerCount();
    const auto grainSize = workers == 0 ? count : std::max<std::size_t>(1U, count / (workers * 4U));
    parallelFor(count, grainSize, function);
}

void TaskScheduler::parallelFor(std::size_t count, std::size_t grainSize,
                                const std::function<void(std::size_t)>& function) {
    if (count == 0)
        return;
    grainSize = std::max<std::size_t>(1U, grainSize);
    const auto worker = Impl::currentWorker();
    if (impl_->workers.empty() || count == 1 || (worker != nullptr && worker->owner == impl_.get())) {
        for (std::size_t index = 0; index < count; ++index)
            function(index);
        return;
    }

    const auto taskCount = (count - 1U) / grainSize + 1U;
    std::latch complete(static_cast<std::ptrdiff_t>(taskCount));
    std::mutex exceptionMutex;
    std::exception_ptr firstException;
    for (std::size_t taskIndex = 0; taskIndex < taskCount; ++taskIndex) {
        const auto begin = taskIndex * grainSize;
        const auto end = begin + std::min(grainSize, count - begin);
        impl_->enqueue([&, begin, end] {
            for (auto index = begin; index < end; ++index) {
                try {
                    function(index);
                } catch (...) {
                    std::lock_guard exceptionLock(exceptionMutex);
                    if (firstException == nullptr)
                        firstException = std::current_exception();
                }
            }
            complete.count_down();
        });
    }
    complete.wait();
    if (firstException != nullptr)
        std::rethrow_exception(firstException);
}

TaskHandle TaskScheduler::schedule(TaskFunction function) {
    if (!function)
        throw std::invalid_argument("cannot schedule an empty task");
    auto state = std::make_shared<TaskHandle::State>();
    const TaskHandle handle(state);
    impl_->enqueue([state = std::move(state), function = std::move(function)]() mutable {
        std::exception_ptr exception;
        try {
            function();
        } catch (...) {
            exception = std::current_exception();
        }
        completeTask(state, std::move(exception));
    });
    return handle;
}

TaskHandle TaskScheduler::scheduleAfter(const TaskHandle& dependency, TaskFunction function) {
    return scheduleAfter(std::span<const TaskHandle>(&dependency, 1), std::move(function));
}

TaskHandle TaskScheduler::scheduleAfter(std::span<const TaskHandle> dependencies, TaskFunction function) {
    if (!function)
        throw std::invalid_argument("cannot schedule an empty task");
    std::vector<std::shared_ptr<TaskHandle::State>> states;
    states.reserve(dependencies.size());
    for (const auto& dependency : dependencies) {
        if (!dependency.valid())
            throw std::invalid_argument("cannot schedule after an invalid task handle");
        states.push_back(dependency.state_);
    }
    if (states.empty())
        return schedule(std::move(function));

    auto state = std::make_shared<TaskHandle::State>();
    const TaskHandle handle(state);
    struct DependencyGate {
        std::atomic<std::size_t> remaining{0};
        std::function<void()> ready;
    };
    auto gate = std::make_shared<DependencyGate>();
    gate->remaining = states.size();
    gate->ready = [this, dependencies = std::move(states), state = std::move(state),
                   function = std::move(function)]() mutable {
        impl_->enqueue([dependencies = std::move(dependencies), state = std::move(state),
                        function = std::move(function)]() mutable {
            std::exception_ptr exception;
            try {
                for (const auto& dependency : dependencies)
                    waitTask(dependency);
                function();
            } catch (...) {
                exception = std::current_exception();
            }
            completeTask(state, std::move(exception));
        });
    };
    for (const auto& dependency : dependencies) {
        addContinuation(dependency.state_, [gate] {
            if (gate->remaining.fetch_sub(1U, std::memory_order_acq_rel) == 1U)
                gate->ready();
        });
    }
    return handle;
}

TaskHandle TaskScheduler::scheduleAfter(std::initializer_list<TaskHandle> dependencies, TaskFunction function) {
    return scheduleAfter(std::span<const TaskHandle>(dependencies.begin(), dependencies.size()), std::move(function));
}

void TaskScheduler::wait(const TaskHandle& handle) const {
    if (Impl::currentWorker() == impl_.get())
        throw std::logic_error("worker tasks must use scheduleAfter instead of blocking wait");
    waitTask(handle.state_);
}

std::size_t TaskScheduler::workerCount() const noexcept {
    return impl_ == nullptr ? 0 : impl_->workers.size();
}

} // namespace dayo::core
