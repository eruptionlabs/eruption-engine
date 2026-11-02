#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>

namespace eruption {

// BLAKE2b (RFC 7693): hash de 1 a 64 bytes, com chave opcional de até 64.
// Livre de patente. Implementação da Monocypher (third_party/monocypher,
// BSD-2/CC0); aqui só a interface do motor. Serve ao hash de conteúdo do
// handshake (mapa + faixas + scripts + versão) e à derivação de chave do
// token lacrado (TokenGate).
class Blake2b {
public:
    // outBytes em 1..64, keyBytes em 0..64. Fora disso o objeto fica inválido
    // e final() devolve falso.
    explicit Blake2b(size_t outBytes = 32, const uint8_t* key = nullptr, size_t keyBytes = 0);
    ~Blake2b();
    Blake2b(const Blake2b&) = delete;
    Blake2b& operator=(const Blake2b&) = delete;

    void update(const void* data, size_t bytes);
    bool final(uint8_t* out);

private:
    struct Ctx;
    std::unique_ptr<Ctx> m_ctx;
    bool m_done = false;
};

using ContentHash = std::array<uint8_t, 32>;

} // namespace eruption
