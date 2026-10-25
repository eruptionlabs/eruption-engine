#include "core/JobSystem.hpp"

#include <algorithm>
#include <condition_variable>
#include <memory>
#include <mutex>

namespace eruption {

JobSystem& JobSystem::instance() {
    static JobSystem js;
    return js;
}

void JobSystem::init(uint32_t numThreads) {
#if defined(__EMSCRIPTEN__)
    (void)numThreads; // navegador sem threads (v1): submit e parallelFor rodam aqui
    return;
#endif
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
#if defined(__EMSCRIPTEN__)
    job(); // navegador sem threads (v1), ver o submit com futuro
    return;
#endif
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
        for (uint32_t i = 0; i < count; ++i) func(i);
        return;
    }

    // Cada um pega o proximo indice livre (nao um bloco fixo): com tarefas de
    // tamanho muito diferente - o LOD do chao de 500 mil triangulos ao lado de
    // um arbusto - blocos fixos deixavam uma thread com as maiores e as outras
    // paradas.
    //
    // O CHAMADOR TAMBEM TRABALHA, e espera so' os indices que algum worker ja'
    // pegou - nunca uma tarefa ainda na fila. As tarefas deste laco entram na
    // mesma fila FIFO dos jobs de fundo (pre-carga de mapa, preload de
    // modelos); esperar que elas rodassem deixava o fio principal parado atras
    // desse trabalho: uma conversao de 3 ms levou ~220-257 ms na chegada a um
    // mapa com a pre-carga do mapa seguinte na fila. O estado vive num
    // shared_ptr: a tarefa que comeca tarde, com tudo ja' feito, sai sem tocar
    // na pilha do chamador. Contador e condicao sao desta chamada, entao dois
    // parallelFor ao mesmo tempo (fio principal e uma thread de fundo) nao se
    // acordam errado.
    struct State {
        std::atomic<uint32_t> next{0};
        std::atomic<uint32_t> done{0};
        uint32_t count = 0;
        std::function<void(uint32_t)> func;
        std::mutex mutex;
        std::condition_variable finished;
    };
    auto state = std::make_shared<State>();
    state->count = count;
    state->func = std::move(func);
    auto work = [](State& st) {
        uint32_t mine = 0;
        for (uint32_t i = st.next.fetch_add(1); i < st.count; i = st.next.fetch_add(1)) {
            st.func(i);
            ++mine;
        }
        if (mine > 0 && st.done.fetch_add(mine) + mine == st.count) {
            std::lock_guard<std::mutex> lock(st.mutex);
            st.finished.notify_all();
        }
    };
    const uint32_t helpers = std::min(static_cast<uint32_t>(m_threads.size()), count - 1);
    for (uint32_t t = 0; t < helpers; t++) submit([state, work]() { work(*state); });
    work(*state);
    std::unique_lock<std::mutex> lock(state->mutex);
    state->finished.wait(lock, [&state] { return state->done.load() == state->count; });
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
            m_completionCondition.notify_all();
        }
    }
}

} // namespace eruption
