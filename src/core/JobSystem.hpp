#pragma once

#include <vector>
#include <queue>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <functional>
#include <atomic>
#include <future>

namespace eruption {

class JobSystem {
public:
    using JobFunc = std::function<void()>;

    static JobSystem& instance();

    void init(uint32_t numThreads = 0); // 0 = hardware_concurrency - 1
    void shutdown();

    // Submit a job, returns future for result
    template<typename F, typename... Args>
    auto submit(F&& f, Args&&... args) -> std::future<decltype(f(args...))> {
        using ReturnType = decltype(f(args...));
        auto task = std::make_shared<std::packaged_task<ReturnType()>>(
            std::bind(std::forward<F>(f), std::forward<Args>(args)...)
        );
        std::future<ReturnType> result = task->get_future();
        {
            std::unique_lock<std::mutex> lock(m_queueMutex);
            if (m_stop) return result;
            m_tasks.emplace([task]() { (*task)(); });
        }
        m_condition.notify_one();
        return result;
    }

    // Submit fire-and-forget job
    void submit(JobFunc job);

    // Parallel for
    void parallelFor(uint32_t count, std::function<void(uint32_t index)> func);

    // Wait for all jobs to complete
    void waitIdle();

    uint32_t threadCount() const { return static_cast<uint32_t>(m_threads.size()); }

private:
    JobSystem() = default;
    ~JobSystem() { shutdown(); }

    std::vector<std::thread> m_threads;
    std::queue<JobFunc> m_tasks;
    std::mutex m_queueMutex;
    std::condition_variable m_condition;
    std::mutex m_completionMutex;
    std::condition_variable m_completionCondition;
    std::atomic<bool> m_stop{false};
    std::atomic<uint32_t> m_activeJobs{0};

    void workerThread();
};

} // namespace eruption
