#include "core/task_scheduler.hpp"

#include <condition_variable>
#include <deque>
#include <exception>
#include <latch>
#include <mutex>
#include <thread>
#include <utility>
#include <vector>

namespace dayo::core {

struct TaskScheduler::Impl {
    struct Task {
        std::function<void()> function;
    };

    static Impl*& currentWorker() noexcept {
        static thread_local Impl* owner = nullptr;
        return owner;
    }

    struct WorkerScope {
        Impl* previous;

        explicit WorkerScope(Impl* owner) : previous(currentWorker()) {
            currentWorker() = owner;
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
            workers.emplace_back([this] { run(); });
    }

    ~Impl() {
        {
            std::lock_guard lock(mutex);
            stopping = true;
        }
        condition.notify_all();
        for (auto& worker : workers)
            if (worker.joinable())
                worker.join();
    }

    void run() {
        while (true) {
            Task task;
            {
                std::unique_lock lock(mutex);
                condition.wait(lock, [this] { return stopping || !tasks.empty(); });
                if (stopping && tasks.empty())
                    return;
                task = std::move(tasks.front());
                tasks.pop_front();
            }
            WorkerScope workerScope(this);
            task.function();
        }
    }

    std::vector<std::thread> workers;
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
    if (count == 0)
        return;
    if (impl_->workers.empty() || count == 1 || Impl::currentWorker() == impl_.get()) {
        for (std::size_t index = 0; index < count; ++index)
            function(index);
        return;
    }

    std::latch complete(static_cast<std::ptrdiff_t>(count));
    std::mutex exceptionMutex;
    std::exception_ptr firstException;
    for (std::size_t index = 0; index < count; ++index) {
        {
            std::lock_guard lock(impl_->mutex);
            impl_->tasks.push_back({[&, index] {
                try {
                    function(index);
                } catch (...) {
                    std::lock_guard exceptionLock(exceptionMutex);
                    if (firstException == nullptr)
                        firstException = std::current_exception();
                }
                complete.count_down();
            }});
        }
        impl_->condition.notify_one();
    }
    complete.wait();
    if (firstException != nullptr)
        std::rethrow_exception(firstException);
}

std::size_t TaskScheduler::workerCount() const noexcept {
    return impl_ == nullptr ? 0 : impl_->workers.size();
}

} // namespace dayo::core
