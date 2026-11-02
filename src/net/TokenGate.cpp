#include "net/TokenGate.hpp"

#include "core/Logger.hpp"

#include "net/Blake2b.hpp"
#include "net/NetcodeEndpoint.hpp"
#include "net/Wire.hpp"

#include <monocypher.h>

#include <algorithm>
#include <cstring>
#include <iterator>
#include <random>

namespace eruption {

namespace {

constexpr uint8_t kRequestMagic[4] = {'E', 'R', 'T', 'Q'};
constexpr uint8_t kReplyMagic[4] = {'E', 'R', 'T', 'A'};
constexpr uint8_t kVersion = 2; // 2: resposta lacrada para a chave efêmera do cliente
constexpr size_t kMaxDialed = 64;
constexpr double kPerIpBurst = 3.0;
constexpr double kPerIpRate = 1.0;     // tokens/s por IP
constexpr double kGlobalBurst = 20.0;
constexpr double kGlobalRate = 10.0;   // tokens/s no total
constexpr double kConnectBurst = 10.0;  // pedidos de conexão por IP (o netcode reenvia a 10 Hz)
constexpr double kConnectRate = 5.0;
constexpr double kGlobalConnectBurst = 100.0; // rajada de 400 custou 1,3 ms num quadro sob ataque
constexpr double kGlobalConnectRate = 200.0;
constexpr double kFloodBurst = 600.0;   // pacotes por endereço (o jogo manda ~60/s)
constexpr double kFloodRate = 300.0;
constexpr double kIdleSeconds = 10.0;   // balde parado há tanto tempo pode sair da tabela
constexpr size_t kMaxBuckets = 4096;
constexpr double kLogEvery = 5.0;

// Pedido de conexão do netcode 1.02: prefixo 0, versão (13 B), protocolo
// (8 B), validade (8 B), nonce (24 B), token privado (1024 B).
constexpr int kConnectRequestBytes = 1 + 13 + 8 + 8 + 24 + 1024;
constexpr uint8_t kNetcodeVersion[13] = {'N', 'E', 'T', 'C', 'O', 'D', 'E', ' ', '1', '.', '0', '2', '\0'};
constexpr int kTokenBytes = 2048;
constexpr size_t kHeader = 4 + 8;  // mágica + nonce do pedido
constexpr size_t kKeyAt = kHeader; // chave pública efêmera do host
constexpr size_t kMacAt = kKeyAt + 32;
constexpr size_t kCipherAt = kMacAt + 16;
static_assert(kCipherAt + kTokenBytes == static_cast<size_t>(kTokenReplyBytes), "layout da resposta");

static_assert(kTokenRequestBytes > kTokenReplyBytes, "pedido tem de ser maior que a resposta");

// IP sem porta: o limite é por máquina, não por porta de origem.
NetAddress hostOnly(const NetAddress& a) {
    NetAddress h = a;
    h.port = 0;
    return h;
}

uint64_t randomNonce() {
    std::random_device rd;
    return (static_cast<uint64_t>(rd()) << 32) ^ rd();
}

// Par X25519 efêmero (gerador do sistema, via libsodium do netcode).
void ephemeralKeys(uint8_t secret[32], uint8_t pub[32]) {
    const NetKey k = makeNetKey();
    std::memcpy(secret, k.data(), 32);
    crypto_x25519_public_key(pub, secret);
}

// Chave da resposta: BLAKE2b(rótulo || segredo X25519 || chave do cliente ||
// chave do host). Falso se o segredo deu zero (chave pública de ordem baixa:
// forjada).
bool sealKey(const uint8_t secret[32], const uint8_t peer[32], const uint8_t clientPub[32],
             const uint8_t hostPub[32], uint8_t key[32]) {
    uint8_t shared[32];
    crypto_x25519(shared, secret, peer);
    uint8_t zero = 0;
    for (uint8_t b : shared) zero |= b;
    if (zero == 0) return false;
    static const char kLabel[] = "eruption token v2";
    Blake2b h(32);
    h.update(kLabel, sizeof(kLabel) - 1);
    h.update(shared, 32);
    h.update(clientPub, 32);
    h.update(hostPub, 32);
    h.final(key);
    crypto_wipe(shared, sizeof(shared));
    return true;
}

// Nonce da cifra: a chave é nova a cada resposta, mas o nonce do pedido
// entra mesmo assim (e vai como dado autenticado).
void aeadNonce(const uint8_t requestNonce[8], uint8_t nonce[24]) {
    std::memset(nonce, 0, 24);
    std::memcpy(nonce, requestNonce, 8);
}

} // namespace

TokenGate::TokenGate(PacketLink& inner, uint16_t port, uint64_t protocolId, Issue issue)
    : m_inner(inner), m_port(port), m_issue(std::move(issue)), m_protocolId(protocolId), m_globalTokens(kGlobalBurst),
      m_globalConnects(kGlobalConnectBurst), m_buffer(kTokenRequestBytes + 64) {}

void TokenGate::send(const NetAddress& to, const uint8_t* data, int bytes) { m_inner.send(to, data, bytes); }

void TokenGate::update(double time) {
    const double dt = std::max(0.0, time - m_time);
    m_globalTokens = std::min(kGlobalBurst, m_globalTokens + dt * kGlobalRate);
    m_globalConnects = std::min(kGlobalConnectBurst, m_globalConnects + dt * kGlobalConnectRate);
    m_time = time;
    logDrops();
    m_inner.update(time);
}

// Log agregado (nunca uma linha por pacote: o log não pode virar o ataque).
void TokenGate::logDrops() {
    if (m_time - m_lastLog < kLogEvery) return;
    m_lastLog = m_time;
    const uint64_t requests = m_droppedRequests - m_loggedRequests;
    const uint64_t flood = m_droppedFlood - m_loggedFlood;
    if (requests == 0 && flood == 0) return;
    ERUPTION_LOG_WARN("net: porta barrou %llu pedidos de conexão e %llu pacotes acima do teto em %.0f s",
                      static_cast<unsigned long long>(requests), static_cast<unsigned long long>(flood), kLogEvery);
    m_loggedRequests = m_droppedRequests;
    m_loggedFlood = m_droppedFlood;
}

// FNV-1a com semente por processo: quem forja endereços não escolhe
// colisões para degradar a tabela.
size_t TokenGate::AddressHash::operator()(const NetAddress& a) const {
    static const uint64_t seed = randomNonce();
    uint64_t h = 1469598103934665603ull ^ seed;
    auto mixByte = [&h](uint8_t b) {
        h ^= b;
        h *= 1099511628211ull;
    };
    mixByte(static_cast<uint8_t>(a.type));
    mixByte(static_cast<uint8_t>(a.port));
    mixByte(static_cast<uint8_t>(a.port >> 8));
    const int n = a.type == NetAddress::IPv6 ? 16 : 4;
    for (int i = 0; i < n; ++i) mixByte(a.bytes[i]);
    return static_cast<size_t>(h);
}

bool TokenGate::take(Buckets& table, const NetAddress& key, double burst, double rate) {
    auto it = table.map.find(key);
    if (it == table.map.end()) {
        // Cheia: limpa os parados no máximo 1x/s (varrer a cada endereço novo
        // seria o próprio ataque: medido 536 ms de quadro sob 100 mil IPs).
        if (table.map.size() >= kMaxBuckets && m_time - table.lastPrune >= 1.0) {
            table.lastPrune = m_time;
            for (auto i = table.map.begin(); i != table.map.end();)
                i = m_time - i->second.last > kIdleSeconds ? table.map.erase(i) : std::next(i);
        }
        if (table.map.size() >= kMaxBuckets) return false; // quem já está continua, novo fica de fora
        if (table.map.empty()) table.map.reserve(kMaxBuckets);
        it = table.map.emplace(key, Bucket{burst, m_time}).first;
    }
    Bucket& b = it->second;
    b.tokens = std::min(burst, b.tokens + (m_time - b.last) * rate);
    b.last = m_time;
    if (b.tokens < 1.0) return false;
    b.tokens -= 1.0;
    return true;
}

bool TokenGate::allowToken(const NetAddress& from) {
    if (m_globalTokens < 1.0 || !take(m_tokenBuckets, hostOnly(from), kPerIpBurst, kPerIpRate)) return false;
    m_globalTokens -= 1.0;
    return true;
}

// Checagens baratas antes de o netcode decifrar o token do pedido.
bool TokenGate::admitConnectRequest(const NetAddress& from, const uint8_t* data, int bytes) {
    if (bytes != kConnectRequestBytes || std::memcmp(data + 1, kNetcodeVersion, sizeof(kNetcodeVersion)) != 0)
        return false;
    uint64_t protocol = 0;
    for (int i = 7; i >= 0; --i) protocol = (protocol << 8) | data[1 + 13 + i];
    if (protocol != m_protocolId) return false;
    if (m_globalConnects < 1.0 || !take(m_connectBuckets, hostOnly(from), kConnectBurst, kConnectRate)) return false;
    m_globalConnects -= 1.0;
    ++m_admittedRequests;
    return true;
}

void TokenGate::handleRequest(const NetAddress& from, const uint8_t* data, int bytes) {
    BitReader r(data, static_cast<size_t>(bytes));
    uint8_t magic[4];
    r.readBytes(magic, 4);
    const uint32_t version = r.readBits(8);
    uint8_t nonce[8];
    r.readBytes(nonce, 8);
    uint8_t clientPub[32];
    r.readBytes(clientPub, 32);
    std::string dialed;
    r.readString(dialed, kMaxDialed);
    NetAddress dialedAddr;
    if (!r.ok() || version != kVersion || !NetAddress::parse(dialed, dialedAddr) || dialedAddr.port != m_port ||
        !allowToken(from)) {
        ++m_refused;
        return;
    }
    std::vector<uint8_t> token;
    if (!m_issue || !m_issue(dialed, token) || token.size() != static_cast<size_t>(kTokenBytes)) {
        ++m_refused;
        return;
    }
    uint8_t hostSecret[32], hostPub[32], key[32], aead[24];
    ephemeralKeys(hostSecret, hostPub);
    const bool keyed = sealKey(hostSecret, clientPub, clientPub, hostPub, key);
    crypto_wipe(hostSecret, sizeof(hostSecret));
    if (!keyed) {
        crypto_wipe(token.data(), token.size());
        ++m_refused;
        return;
    }
    std::vector<uint8_t> reply(kTokenReplyBytes);
    std::memcpy(reply.data(), kReplyMagic, 4);
    std::memcpy(reply.data() + 4, nonce, 8);
    std::memcpy(reply.data() + kKeyAt, hostPub, 32);
    aeadNonce(nonce, aead);
    crypto_aead_lock(reply.data() + kCipherAt, reply.data() + kMacAt, key, aead, nonce, 8, token.data(), token.size());
    crypto_wipe(key, sizeof(key));
    crypto_wipe(token.data(), token.size());
    m_inner.send(from, reply.data(), static_cast<int>(reply.size()));
    ++m_issued;
}

int TokenGate::receive(NetAddress& from, uint8_t* out, int maxBytes) {
    for (;;) {
        const int n = m_inner.receive(from, m_buffer.data(), static_cast<int>(m_buffer.size()));
        if (n <= 0) return 0;
        if (!take(m_floodBuckets, from, kFloodBurst, kFloodRate)) {
            ++m_droppedFlood;
            continue;
        }
        if (n == kTokenRequestBytes && std::memcmp(m_buffer.data(), kRequestMagic, 4) == 0) {
            handleRequest(from, m_buffer.data(), n);
            continue;
        }
        if (n > maxBytes) continue; // maior que qualquer pacote do netcode: lixo
        if (m_buffer[0] == 0 && !admitConnectRequest(from, m_buffer.data(), n)) {
            ++m_droppedRequests;
            continue;
        }
        std::memcpy(out, m_buffer.data(), static_cast<size_t>(n));
        return n;
    }
}

// --- TokenRequest ---

TokenRequest::~TokenRequest() {
    crypto_wipe(m_secret, sizeof(m_secret));
    if (!m_token.empty()) crypto_wipe(m_token.data(), m_token.size());
}

bool TokenRequest::start(const std::string& dialed, PacketLink& link, double time, double timeoutSeconds) {
    m_state = State::Failed;
    m_token.clear();
    if (dialed.size() > kMaxDialed || !NetAddress::parse(dialed, m_server) || m_server.port == 0) return false;
    m_link = &link;
    m_dialed = dialed;
    m_nonce = randomNonce();
    ephemeralKeys(m_secret, m_public);
    m_deadline = time + timeoutSeconds;
    m_lastSend = -1.0;
    m_state = State::Waiting;
    return true;
}

void TokenRequest::sendRequest() {
    std::vector<uint8_t> request(kTokenRequestBytes, 0);
    BitWriter w(request.data(), request.size());
    w.writeBytes(kRequestMagic, 4);
    w.writeBits(kVersion, 8);
    uint8_t nonce[8];
    for (int i = 0; i < 8; ++i) nonce[i] = static_cast<uint8_t>(m_nonce >> (8 * i));
    w.writeBytes(nonce, 8);
    w.writeBytes(m_public, 32);
    w.writeString(m_dialed, kMaxDialed);
    w.finish(); // o resto fica zero: enchimento até o tamanho fixo
    m_link->send(m_server, request.data(), static_cast<int>(request.size()));
}

void TokenRequest::update(double time) {
    if (m_state != State::Waiting) return;
    m_link->update(time);
    std::vector<uint8_t> buf(kTokenRequestBytes);
    NetAddress from;
    int n;
    while ((n = m_link->receive(from, buf.data(), static_cast<int>(buf.size()))) > 0) {
        if (n != kTokenReplyBytes || from != m_server || std::memcmp(buf.data(), kReplyMagic, 4) != 0) continue;
        uint64_t nonce = 0;
        for (int i = 7; i >= 0; --i) nonce = (nonce << 8) | buf[static_cast<size_t>(4 + i)];
        if (nonce != m_nonce) continue;
        uint8_t key[32], aead[24];
        if (!sealKey(m_secret, buf.data() + kKeyAt, m_public, buf.data() + kKeyAt, key)) continue;
        aeadNonce(buf.data() + 4, aead);
        std::vector<uint8_t> token(kTokenBytes);
        const int bad = crypto_aead_unlock(token.data(), buf.data() + kMacAt, key, aead, buf.data() + 4, 8,
                                           buf.data() + kCipherAt, token.size());
        crypto_wipe(key, sizeof(key));
        if (bad) continue; // forjada ou corrompida: espera a verdadeira
        m_token = std::move(token);
        crypto_wipe(m_secret, sizeof(m_secret));
        m_state = State::Done;
        return;
    }
    if (time >= m_deadline) {
        crypto_wipe(m_secret, sizeof(m_secret));
        m_state = State::Failed;
        return;
    }
    if (m_lastSend < 0.0 || time - m_lastSend >= 0.5) {
        m_lastSend = time;
        sendRequest();
    }
}

} // namespace eruption
