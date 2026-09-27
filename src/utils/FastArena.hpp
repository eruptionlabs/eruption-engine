#pragma once
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <type_traits>
#include <new>
#include <cassert>
#include <stdexcept>

namespace eruption {

class FastArena {
public:
    explicit FastArena(size_t size) {
        // Garante que o tamanho seja múltiplo de 64 (exigência do std::aligned_alloc)
        size_t aligned_size = (size + 63) & ~size_t(63);
        
        m_memory = static_cast<std::byte*>(std::aligned_alloc(64, aligned_size));
        if (!m_memory) throw std::bad_alloc();
        
        m_head = m_memory;
        m_size = aligned_size;
        m_end = m_memory + m_size;
    }

    ~FastArena() { std::free(m_memory); }

    FastArena(const FastArena&) = delete;
    FastArena& operator=(const FastArena&) = delete;

    void reset() { m_head = m_memory; }

    // Alinhamento explícito, O(1), branch provavelmente previsto corretamente
    [[nodiscard]] void* alloc(size_t size, size_t align = alignof(std::max_align_t)) {
        // Valida se o alinhamento é uma potência de 2 (necessário para a matemática bit a bit)
        assert((align > 0) && (align & (align - 1)) == 0 && "O alinhamento deve ser potencia de 2");

        uintptr_t current = reinterpret_cast<uintptr_t>(m_head);
        uintptr_t aligned = (current + align - 1) & ~(align - 1);
        uintptr_t new_head = aligned + size;

        if (new_head > reinterpret_cast<uintptr_t>(m_end)) {
            return nullptr; // ou assert/throw dependendo da sua política de erros
        }

        m_head = reinterpret_cast<std::byte*>(new_head);
        return reinterpret_cast<void*>(aligned);
    }

    template<typename T, typename... Args>
    [[nodiscard]] T* construct(Args&&... args) {
        // Bloqueia a alocação de objetos que vazariam memória (ex: std::string, std::vector)
        static_assert(std::is_trivially_destructible_v<T>,
                      "Objetos na FastArena não terão seus destrutores chamados automaticamente pelo reset().");
        
        void* mem = alloc(sizeof(T), alignof(T));
        if (!mem) return nullptr;
        return new (mem) T(std::forward<Args>(args)...);
    }

    template<typename T>
    [[nodiscard]] T* allocArray(size_t count) {
        return static_cast<T*>(alloc(sizeof(T) * count, alignof(T)));
    }

    size_t used() const { return static_cast<size_t>(m_head - m_memory); }
    size_t remaining() const { return static_cast<size_t>(m_end - m_head); }
    size_t capacity() const { return m_size; }

private:
    std::byte* m_memory;
    std::byte* m_head;
    std::byte* m_end;
    size_t m_size;
};

} // namespace eruption