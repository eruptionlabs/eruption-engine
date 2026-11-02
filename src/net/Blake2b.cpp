#include "net/Blake2b.hpp"

#include <monocypher.h>

namespace eruption {

struct Blake2b::Ctx {
    crypto_blake2b_ctx state;
};

Blake2b::Blake2b(size_t outBytes, const uint8_t* key, size_t keyBytes) {
    if (outBytes == 0 || outBytes > 64 || keyBytes > 64 || (keyBytes > 0 && !key)) return;
    m_ctx = std::make_unique<Ctx>();
    if (keyBytes > 0) crypto_blake2b_keyed_init(&m_ctx->state, outBytes, key, keyBytes);
    else crypto_blake2b_init(&m_ctx->state, outBytes);
}

Blake2b::~Blake2b() {
    if (m_ctx) crypto_wipe(m_ctx.get(), sizeof(Ctx));
}

void Blake2b::update(const void* data, size_t bytes) {
    if (m_ctx && !m_done) crypto_blake2b_update(&m_ctx->state, static_cast<const uint8_t*>(data), bytes);
}

bool Blake2b::final(uint8_t* out) {
    if (!m_ctx || m_done || !out) return false;
    crypto_blake2b_final(&m_ctx->state, out);
    m_done = true;
    return true;
}

} // namespace eruption
