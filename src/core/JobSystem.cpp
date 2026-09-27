#include "core/JobSystem.hpp"
#include <algorithm>

namespace eruption {

JobSystem& JobSystem::instance() {
    static JobSystem js;
    return js;
}

void JobSystem::init(uint32_t numThreads) {
    if (!m_threads.empty()) return; // Already initialized
    if (numThreads == 0) {
        numThreads = std::max(1u, std::thread::hardware_concurrency() - 1);
    }
    m_stop = false;
    m_threads.reserve(numThreads);
    for (uint32_t i = 0; i < numThreads; i++) {
        m_threads.emplace_back(&JobSystem::workerThread, this);
    }
}

void JobSystem::shutdown() {
    {
        std::unique_lock<std::mutex> lock(m_queueMutex);
        m_stop = true;
    }
    m_condition.notify_all();
    for (auto& t : m_threads) {
        if (t.joinable()) t.join();
    }
    m_threads.clear();
}

void JobSystem::submit(JobFunc job) {
    {
        std::unique_lock<std::mutex> lock(m_queueMutex);
        if (m_stop) return;
        m_tasks.emplace(std::move(job));
    }
    m_condition.notify_one();
}

void JobSystem::parallelFor(uint32_t count, std::function<void(uint32_t index)> func) {
    if (count == 0) return;
    if (count == 1 || m_threads.empty()) {
        func(0);
        return;
    }

    uint32_t numThreads = static_cast<uint32_t>(m_threads.size());
    uint32_t chunkSize = (count + numThreads - 1) / numThreads;
    std::atomic<uint32_t> remaining(0);

    for (uint32_t t = 0; t < numThreads && t * chunkSize < count; t++) {
        uint32_t start = t * chunkSize;
        uint32_t end = std::min(start + chunkSize, count);
        remaining++;
        submit([start, end, &func, &remaining, this]() {
            for (uint32_t i = start; i < end; i++) {
                func(i);
            }
            remaining--;
            std::unique_lock<std::mutex> lock(m_completionMutex);
            m_completionCondition.notify_one();
        });
    }

    // Wait for completion without busy-waiting.
    std::unique_lock<std::mutex> lock(m_completionMutex);
    m_completionCondition.wait(lock, [&remaining] {
        return remaining.load() == 0;
    });
}

void JobSystem::waitIdle() {
    std::unique_lock<std::mutex> lock(m_completionMutex);
    m_completionCondition.wait(lock, [this] {
        return m_activeJobs.load() == 0 && m_tasks.empty();
    });
}

void JobSystem::workerThread() {
    while (true) {
        JobFunc job;
        {
            std::unique_lock<std::mutex> lock(m_queueMutex);
            m_condition.wait(lock, [this] { return m_stop || !m_tasks.empty(); });
            if (m_stop && m_tasks.empty()) return;
            job = std::move(m_tasks.front());
            m_tasks.pop();
        }
        m_activeJobs++;
        job();
        m_activeJobs--;
        {
            std::unique_lock<std::mutex> lock(m_completionMutex);
            m_completionCondition.notify_one();
        }
    }
}

} // namespace eruption
