#include "net/NetcodeEndpoint.hpp"

#include "core/Logger.hpp"

#include <netcode.h>

#include <cstdarg>
#include <cstdio>
#include <cstring>

namespace eruption {

namespace {

static_assert(NETCODE_CONNECT_TOKEN_BYTES == kNetConnectTokenBytes, "token do netcode mudou de tamanho");
static_assert(NETCODE_USER_DATA_BYTES == kNetUserDataBytes, "user data do netcode mudou de tamanho");
static_assert(NETCODE_KEY_BYTES == 32, "chave do netcode mudou de tamanho");

// O netcode fala por printf; vira log do motor (só erro, para o log não
// virar vetor de ataque com um pacote ruim por linha).
int netcodePrintf(const char* fmt, ...) {
    char line[512];
    va_list args;
    va_start(args, fmt);
    const int n = std::vsnprintf(line, sizeof(line), fmt, args);
    va_end(args);
    size_t len = std::strlen(line);
    while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r')) line[--len] = '\0';
    ERUPTION_LOG_WARN("netcode: %s", line);
    return n;
}

// Referência contada pelo próprio netcode_init; o printf é global.
bool acquireNetcode() {
    netcode_set_printf_function(netcodePrintf);
    netcode_log_level(NETCODE_LOG_LEVEL_ERROR);
    if (netcode_init() != NETCODE_OK) {
        ERUPTION_LOG_ERROR("net: netcode_init falhou");
        return false;
    }
    return true;
}

void toNetcode(const NetAddress& a, netcode_address_t& out) {
    std::memset(&out, 0, sizeof(out));
    out.port = a.port;
    if (a.type == NetAddress::IPv4) {
        out.type = NETCODE_ADDRESS_IPV4;
        std::memcpy(out.data.ipv4, a.bytes, 4);
    } else if (a.type == NetAddress::IPv6) {
        out.type = NETCODE_ADDRESS_IPV6;
        for (int i = 0; i < 8; ++i)
            out.data.ipv6[i] = static_cast<uint16_t>((a.bytes[2 * i] << 8) | a.bytes[2 * i + 1]);
    }
}

void fromNetcode(const netcode_address_t& a, NetAddress& out) {
    out = NetAddress();
    out.port = a.port;
    if (a.type == NETCODE_ADDRESS_IPV4) {
        out.type = NetAddress::IPv4;
        std::memcpy(out.bytes, a.data.ipv4, 4);
    } else if (a.type == NETCODE_ADDRESS_IPV6) {
        out.type = NetAddress::IPv6;
        for (int i = 0; i < 8; ++i) {
            out.bytes[2 * i] = static_cast<uint8_t>(a.data.ipv6[i] >> 8);
            out.bytes[2 * i + 1] = static_cast<uint8_t>(a.data.ipv6[i] & 0xFF);
        }
    }
}

void linkSend(PacketLink* link, netcode_address_t* to, const uint8_t* data, int bytes) {
    NetAddress a;
    fromNetcode(*to, a);
    link->send(a, data, bytes);
}

int linkReceive(PacketLink* link, netcode_address_t* from, uint8_t* data, int maxBytes) {
    NetAddress a;
    const int n = link->receive(a, data, maxBytes);
    if (n > 0) toNetcode(a, *from);
    return n;
}

bool validAddresses(const std::vector<std::string>& list) {
    if (list.empty() || list.size() > NETCODE_MAX_SERVERS_PER_CONNECT) return false;
    for (const std::string& s : list) {
        NetAddress a;
        if (!NetAddress::parse(s, a) || a.port == 0) return false;
    }
    return true;
}

} // namespace

NetKey makeNetKey() {
    NetKey key{};
    if (acquireNetcode()) {
        netcode_random_bytes(key.data(), static_cast<int>(key.size()));
        netcode_term();
    }
    return key;
}

bool makeConnectToken(const ConnectTokenRequest& request, const NetKey& key, std::vector<uint8_t>& token) {
    const std::vector<std::string>& internal =
        request.internalAddresses.empty() ? request.publicAddresses : request.internalAddresses;
    if (!validAddresses(request.publicAddresses) || !validAddresses(internal) ||
        internal.size() != request.publicAddresses.size() || request.expireSeconds <= 0 || request.timeoutSeconds <= 0)
        return false;
    std::vector<const char*> pub, in;
    for (const std::string& s : request.publicAddresses) pub.push_back(s.c_str());
    for (const std::string& s : internal) in.push_back(s.c_str());
    std::array<uint8_t, kNetUserDataBytes> userData = request.userData;
    token.assign(kNetConnectTokenBytes, 0);
    if (!acquireNetcode()) return false;
    const int ok = netcode_generate_connect_token(static_cast<int>(pub.size()), pub.data(), in.data(),
                                                  request.expireSeconds, request.timeoutSeconds, request.clientId,
                                                  request.protocolId, key.data(), userData.data(), token.data());
    netcode_term();
    return ok == NETCODE_OK;
}

// --- NetServer ---

NetServer::NetServer() = default;
NetServer::~NetServer() { stop(); }

void NetServer::sendOverride(void* ctx, netcode_address_t* to, const uint8_t* data, int bytes) {
    linkSend(static_cast<NetServer*>(ctx)->m_link, to, data, bytes);
}

int NetServer::receiveOverride(void* ctx, netcode_address_t* from, uint8_t* data, int maxBytes) {
    return linkReceive(static_cast<NetServer*>(ctx)->m_link, from, data, maxBytes);
}

void NetServer::connectionCallback(void* ctx, int client, int connected) {
    NetServer* self = static_cast<NetServer*>(ctx);
    if (self->onConnection) self->onConnection(client, connected != 0);
}

bool NetServer::start(const std::string& publicAddress, uint64_t protocolId, const NetKey& key, int maxClients,
                      PacketLink& link, double time) {
    stop();
    if (maxClients < 1 || maxClients > NETCODE_MAX_CLIENTS) return false;
    if (!acquireNetcode()) return false;
    netcode_server_config_t config;
    netcode_default_server_config(&config);
    config.protocol_id = protocolId;
    std::memcpy(config.private_key, key.data(), key.size());
    config.callback_context = this;
    config.connect_disconnect_callback = connectionCallback;
    config.override_send_and_receive = 1;
    config.send_packet_override = sendOverride;
    config.receive_packet_override = receiveOverride;
    m_link = &link;
    m_server = netcode_server_create(publicAddress.c_str(), &config, time);
    if (!m_server) {
        ERUPTION_LOG_ERROR("net: servidor em %s não abriu (erro %d)", publicAddress.c_str(), netcode_server_create_error());
        m_link = nullptr;
        netcode_term();
        return false;
    }
    netcode_server_start(m_server, maxClients);
    m_maxClients = maxClients;
    return true;
}

void NetServer::stop() {
    if (!m_server) return;
    netcode_server_destroy(m_server);
    m_server = nullptr;
    m_link = nullptr;
    m_maxClients = 0;
    netcode_term();
}

void NetServer::update(double time) {
    if (!m_server) return;
    m_link->update(time);
    netcode_server_update(m_server, time);
}

bool NetServer::connected(int client) const {
    return m_server && client >= 0 && client < m_maxClients && netcode_server_client_connected(m_server, client);
}

uint64_t NetServer::clientId(int client) const {
    return connected(client) ? netcode_server_client_id(m_server, client) : 0;
}

void NetServer::send(int client, const uint8_t* data, int bytes) {
    if (!connected(client) || bytes <= 0 || bytes > kNetMaxPayload) return;
    netcode_server_send_packet(m_server, client, data, bytes);
}

bool NetServer::receive(int client, std::vector<uint8_t>& out, uint64_t* sequence) {
    if (!connected(client)) return false;
    int bytes = 0;
    uint64_t seq = 0;
    uint8_t* packet = netcode_server_receive_packet(m_server, client, &bytes, &seq);
    if (!packet) return false;
    out.assign(packet, packet + bytes);
    netcode_server_free_packet(m_server, packet);
    if (sequence) *sequence = seq;
    return true;
}

void NetServer::disconnect(int client) {
    if (connected(client)) netcode_server_disconnect_client(m_server, client);
}

// --- NetClient ---

NetClient::NetClient() = default;
NetClient::~NetClient() { disconnect(); }

void NetClient::sendOverride(void* ctx, netcode_address_t* to, const uint8_t* data, int bytes) {
    linkSend(static_cast<NetClient*>(ctx)->m_link, to, data, bytes);
}

int NetClient::receiveOverride(void* ctx, netcode_address_t* from, uint8_t* data, int maxBytes) {
    return linkReceive(static_cast<NetClient*>(ctx)->m_link, from, data, maxBytes);
}

bool NetClient::connect(const std::vector<uint8_t>& token, const std::string& localAddress, PacketLink& link,
                        double time) {
    disconnect();
    if (token.size() != static_cast<size_t>(kNetConnectTokenBytes)) return false;
    if (!acquireNetcode()) return false;
    netcode_client_config_t config;
    netcode_default_client_config(&config);
    config.callback_context = this;
    config.override_send_and_receive = 1;
    config.send_packet_override = sendOverride;
    config.receive_packet_override = receiveOverride;
    m_link = &link;
    m_client = netcode_client_create(localAddress.c_str(), &config, time);
    if (!m_client) {
        ERUPTION_LOG_ERROR("net: cliente em %s não abriu (erro %d)", localAddress.c_str(), netcode_client_create_error());
        m_link = nullptr;
        netcode_term();
        return false;
    }
    // O netcode lê o token sem alterá-lo, mas a assinatura pede não-const.
    std::vector<uint8_t> copy = token;
    netcode_client_connect(m_client, copy.data());
    return true;
}

void NetClient::disconnect() {
    if (!m_client) return;
    netcode_client_disconnect(m_client);
    netcode_client_destroy(m_client);
    m_client = nullptr;
    m_link = nullptr;
    netcode_term();
}

void NetClient::update(double time) {
    if (!m_client) return;
    m_link->update(time);
    netcode_client_update(m_client, time);
}

NetClient::State NetClient::state() const {
    return m_client ? static_cast<State>(netcode_client_state(m_client)) : State::Disconnected;
}

int NetClient::clientIndex() const { return m_client ? netcode_client_index(m_client) : -1; }

void NetClient::send(const uint8_t* data, int bytes) {
    if (state() != State::Connected || bytes <= 0 || bytes > kNetMaxPayload) return;
    netcode_client_send_packet(m_client, data, bytes);
}

bool NetClient::receive(std::vector<uint8_t>& out, uint64_t* sequence) {
    if (!m_client) return false;
    int bytes = 0;
    uint64_t seq = 0;
    uint8_t* packet = netcode_client_receive_packet(m_client, &bytes, &seq);
    if (!packet) return false;
    out.assign(packet, packet + bytes);
    netcode_client_free_packet(m_client, packet);
    if (sequence) *sequence = seq;
    return true;
}

const char* toString(NetClient::State state) {
    switch (state) {
        case NetClient::State::TokenExpired: return "token vencido";
        case NetClient::State::InvalidToken: return "token inválido";
        case NetClient::State::TimedOut: return "conexão caiu";
        case NetClient::State::ResponseTimedOut: return "sem resposta ao desafio";
        case NetClient::State::RequestTimedOut: return "servidor não respondeu";
        case NetClient::State::Denied: return "recusado (cheio)";
        case NetClient::State::Disconnected: return "desconectado";
        case NetClient::State::SendingRequest: return "pedindo conexão";
        case NetClient::State::SendingResponse: return "respondendo desafio";
        case NetClient::State::Connected: return "conectado";
    }
    return "?";
}

} // namespace eruption
