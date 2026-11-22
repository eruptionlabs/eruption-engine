#pragma once

// Bloco etex (normal ou MRAH-W) que ainda esta' no arquivo do pacote; ver
// PbrDeferred.hpp. Sem Vulkan: o resultado do bake (EmbeddedTextureBake.hpp)
// carrega estes ponteiros.

#include "utils/EtexLoader.hpp"

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>

namespace eruption {

// Um bloco etex (normal ou MRAH-W) que ainda esta' no arquivo do pacote.
struct PbrDeferredSource {
    std::string path;
    uint64_t offset = 0; // inicio do bloco no arquivo
    uint64_t bytes = 0;  // tamanho do bloco (cabecalho + mips)
    EtexData header;     // formato, dimensoes e tamanhos dos mips; sem payload

    // Le' o bloco inteiro agora (quem precisa do payload: alivio do chao,
    // LOD, editor). Thread-safe; devolve um EtexData invalido se a leitura falhar.
    const EtexData& load() const;
    bool loaded() const { return m_loaded.load(std::memory_order_acquire); }

private:
    mutable std::once_flag m_once;
    mutable EtexData m_data;
    mutable std::atomic<bool> m_loaded{false};
};

// Le' um bloco do pacote (uma thread por vez no disco, como o resto das
// leituras grandes). Devolve false em erro de leitura ou bloco que nao fecha.
bool readPbrBlock(const std::string& path, uint64_t offset, uint64_t bytes, EtexData& out);

// Teste: imita um disco lento nas leituras do pacote de texturas, para medir a
// carga a frio de forma repetivel (o cache do sistema nao se esvazia sem root).
// ERUPTION_TEST_DISK_MBPS=<n> (vazao, MB/s) e ERUPTION_TEST_DISK_SEEK_MS=<n>
// (busca da cabeca, ms; padrao 0). Cada leitura espera bytes/vazao e, se `seek`,
// mais a busca. Sem as variaveis nao faz nada.
void testDiskThrottle(uint64_t bytes, bool seek);

} // namespace eruption
