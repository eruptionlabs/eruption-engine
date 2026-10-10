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

    // Cada thread pega o próximo índice livre (não um bloco fixo): com
    // tarefas de tamanho muito diferente - o LOD do chão de 500 mil
    // triângulos ao lado de um arbusto - blocos fixos deixavam uma thread com
    // as maiores e as outras paradas.
    const uint32_t numThreads = std::min(static_cast<uint32_t>(m_threads.size()), count);
    std::atomic<uint32_t> next(0);
    std::atomic<uint32_t> remaining(0);

    for (uint32_t t = 0; t < numThreads; t++) {
        remaining++;
        submit([count, &next, &func, &remaining, this]() {
            for (uint32_t i = next.fetch_add(1); i < count; i = next.fetch_add(1)) {
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
