#include "net/PacketLink.hpp"

#include "core/Logger.hpp"

#include <cstring>

#ifndef _WIN32
#include <arpa/inet.h>
#include <cerrno>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace eruption {

// --- NetAddress ---

bool NetAddress::parse(const std::string& text, NetAddress& out) {
    out = NetAddress();
#ifdef _WIN32
    (void)text;
    return false;
#else
    std::string host = text;
    std::string port;
    if (!text.empty() && text[0] == '[') {
        const size_t close = text.find(']');
        if (close == std::string::npos) return false;
        host = text.substr(1, close - 1);
        if (close + 1 < text.size()) {
            if (text[close + 1] != ':') return false;
            port = text.substr(close + 2);
        }
    } else {
        const size_t colon = text.rfind(':');
        // Mais de um ':' sem colchete é IPv6 sem porta.
        if (colon != std::string::npos && text.find(':') == colon) {
            host = text.substr(0, colon);
            port = text.substr(colon + 1);
        }
    }
    if (!port.empty()) {
        if (port.size() > 5 || port.find_first_not_of("0123456789") != std::string::npos) return false;
        const unsigned long p = std::stoul(port);
        if (p > 65535) return false;
        out.port = static_cast<uint16_t>(p);
    }
    if (inet_pton(AF_INET, host.c_str(), out.bytes) == 1) {
        out.type = IPv4;
        return true;
    }
    if (inet_pton(AF_INET6, host.c_str(), out.bytes) == 1) {
        out.type = IPv6;
        return true;
    }
    out = NetAddress();
    return false;
#endif
}

std::string NetAddress::toString() const {
#ifdef _WIN32
    return std::string();
#else
    char host[INET6_ADDRSTRLEN] = {};
    if (type == IPv4) {
        inet_ntop(AF_INET, bytes, host, sizeof(host));
        return std::string(host) + ":" + std::to_string(port);
    }
    if (type == IPv6) {
        inet_ntop(AF_INET6, bytes, host, sizeof(host));
        return "[" + std::string(host) + "]:" + std::to_string(port);
    }
    return "none";
#endif
}

bool NetAddress::operator==(const NetAddress& o) const {
    if (type != o.type || port != o.port) return false;
    const size_t n = type == IPv4 ? 4 : type == IPv6 ? 16 : 0;
    return std::memcmp(bytes, o.bytes, n) == 0;
}

bool NetAddress::operator<(const NetAddress& o) const {
    if (type != o.type) return type < o.type;
    if (port != o.port) return port < o.port;
    return std::memcmp(bytes, o.bytes, sizeof(bytes)) < 0;
}

// --- UdpLink ---

UdpLink::~UdpLink() { close(); }

#ifdef _WIN32
bool UdpLink::open(const NetAddress&) {
    ERUPTION_LOG_ERROR("net: UDP ainda não existe no Windows");
    return false;
}
void UdpLink::close() {}
void UdpLink::send(const NetAddress&, const uint8_t*, int) {}
int UdpLink::receive(NetAddress&, uint8_t*, int) { return 0; }
#else
namespace {

socklen_t toSockaddr(const NetAddress& a, sockaddr_storage& ss) {
    std::memset(&ss, 0, sizeof(ss));
    if (a.type == NetAddress::IPv4) {
        auto* s = reinterpret_cast<sockaddr_in*>(&ss);
        s->sin_family = AF_INET;
        s->sin_port = htons(a.port);
        std::memcpy(&s->sin_addr, a.bytes, 4);
        return sizeof(sockaddr_in);
    }
    auto* s = reinterpret_cast<sockaddr_in6*>(&ss);
    s->sin6_family = AF_INET6;
    s->sin6_port = htons(a.port);
    std::memcpy(&s->sin6_addr, a.bytes, 16);
    return sizeof(sockaddr_in6);
}

bool fromSockaddr(const sockaddr_storage& ss, NetAddress& a) {
    a = NetAddress();
    if (ss.ss_family == AF_INET) {
        const auto* s = reinterpret_cast<const sockaddr_in*>(&ss);
        a.type = NetAddress::IPv4;
        a.port = ntohs(s->sin_port);
        std::memcpy(a.bytes, &s->sin_addr, 4);
        return true;
    }
    if (ss.ss_family == AF_INET6) {
        const auto* s = reinterpret_cast<const sockaddr_in6*>(&ss);
        a.type = NetAddress::IPv6;
        a.port = ntohs(s->sin6_port);
        std::memcpy(a.bytes, &s->sin6_addr, 16);
        return true;
    }
    return false;
}

} // namespace

bool UdpLink::open(const NetAddress& bindAddress) {
    close();
    if (bindAddress.type == NetAddress::None) return false;
    const int family = bindAddress.type == NetAddress::IPv4 ? AF_INET : AF_INET6;
    m_fd = ::socket(family, SOCK_DGRAM, IPPROTO_UDP);
    if (m_fd < 0) {
        ERUPTION_LOG_ERROR("net: socket UDP falhou (%s)", std::strerror(errno));
        return false;
    }
    if (family == AF_INET6) {
        const int only6 = 1;
        setsockopt(m_fd, IPPROTO_IPV6, IPV6_V6ONLY, &only6, sizeof(only6));
    }
    // 16 clientes a 30 Hz cabem com folga; o padrão do kernel às vezes não.
    const int bufferBytes = 1 << 20;
    setsockopt(m_fd, SOL_SOCKET, SO_SNDBUF, &bufferBytes, sizeof(bufferBytes));
    setsockopt(m_fd, SOL_SOCKET, SO_RCVBUF, &bufferBytes, sizeof(bufferBytes));
    sockaddr_storage ss;
    const socklen_t len = toSockaddr(bindAddress, ss);
    if (::bind(m_fd, reinterpret_cast<sockaddr*>(&ss), len) != 0) {
        ERUPTION_LOG_ERROR("net: bind em %s falhou (%s)", bindAddress.toString().c_str(), std::strerror(errno));
        close();
        return false;
    }
    socklen_t got = sizeof(ss);
    getsockname(m_fd, reinterpret_cast<sockaddr*>(&ss), &got);
    fromSockaddr(ss, m_local);
    const int flags = fcntl(m_fd, F_GETFL, 0);
    fcntl(m_fd, F_SETFL, flags | O_NONBLOCK);
    return true;
}

void UdpLink::close() {
    if (m_fd >= 0) ::close(m_fd);
    m_fd = -1;
}

void UdpLink::send(const NetAddress& to, const uint8_t* data, int bytes) {
    if (m_fd < 0 || bytes <= 0 || to.type != m_local.type) return;
    sockaddr_storage ss;
    const socklen_t len = toSockaddr(to, ss);
    // Perda no envio é como perda na rede: o protocolo já lida com ela.
    ::sendto(m_fd, data, static_cast<size_t>(bytes), 0, reinterpret_cast<sockaddr*>(&ss), len);
}

int UdpLink::receive(NetAddress& from, uint8_t* out, int maxBytes) {
    if (m_fd < 0 || maxBytes <= 0) return 0;
    for (;;) {
        sockaddr_storage ss;
        socklen_t len = sizeof(ss);
        const ssize_t n = ::recvfrom(m_fd, out, static_cast<size_t>(maxBytes), 0, reinterpret_cast<sockaddr*>(&ss), &len);
        if (n <= 0) return 0;
        if (fromSockaddr(ss, from)) return static_cast<int>(n);
    }
}
#endif

// --- MemoryHub ---

class MemoryHub::Link final : public PacketLink {
public:
    Link(MemoryHub& hub, const NetAddress& address) : m_hub(hub), m_address(address) {}
    void send(const NetAddress& to, const uint8_t* data, int bytes) override { m_hub.deliver(m_address, to, data, bytes); }
    int receive(NetAddress& from, uint8_t* out, int maxBytes) override {
        std::deque<Queued>& q = m_hub.m_queues[m_address];
        while (!q.empty()) {
            Queued packet = std::move(q.front());
            q.pop_front();
            if (static_cast<int>(packet.data.size()) > maxBytes) continue; // como o UDP: maior que o buffer some
            from = packet.from;
            std::memcpy(out, packet.data.data(), packet.data.size());
            return static_cast<int>(packet.data.size());
        }
        return 0;
    }

private:
    MemoryHub& m_hub;
    NetAddress m_address;
};

std::unique_ptr<PacketLink> MemoryHub::createLink(const NetAddress& address) {
    m_queues[address];
    return std::make_unique<Link>(*this, address);
}

void MemoryHub::deliver(const NetAddress& from, const NetAddress& to, const uint8_t* data, int bytes) {
    if (bytes <= 0) return;
    std::vector<uint8_t> packet(data, data + bytes);
    if (m_tap && !m_tap(from, to, packet)) return;
    auto it = m_queues.find(to);
    if (it != m_queues.end()) it->second.push_back(Queued{from, std::move(packet)});
}

void MemoryHub::inject(const NetAddress& from, const NetAddress& to, const uint8_t* data, int bytes) {
    if (bytes <= 0) return;
    auto it = m_queues.find(to);
    if (it != m_queues.end()) it->second.push_back(Queued{from, std::vector<uint8_t>(data, data + bytes)});
}

} // namespace eruption
