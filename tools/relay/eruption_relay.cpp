// Relay de partida para VPS (src/net/Relay): quem está atrás de CGNAT abre a
// sala aqui e os outros entram pelo IP da VPS. Programa à parte, sem GPU nem
// janela; só UDP.
//
//   eruption-relay --genkey > relay.key          (chave nova; vai para o jogo também)
//   eruption-relay --key-file relay.key [--port 41921] [--rooms 41930:64]
//                  [--clients 32] [--allow-private] [--ipv6]
//
// Abrir na VPS a porta de controle e a faixa das salas (UDP). O jogo hospeda
// com net.host{ relay = "ip-da-vps:41921", relay_key = "<a mesma chave>" }.
#include "core/Logger.hpp"
#include "net/Relay.hpp"

#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>
#include <thread>

using namespace eruption;

namespace {

volatile std::sig_atomic_t g_stop = 0;
void onSignal(int) { g_stop = 1; }

int usage() {
    std::fprintf(stderr,
                 "uso: eruption-relay --genkey\n"
                 "     eruption-relay --key-file <arquivo> [--port N] [--rooms primeira:quantas] [--clients N]\n"
                 "                    [--allow-private] [--ipv6]\n");
    return 2;
}

} // namespace

int main(int argc, char** argv) {
    Logger::init();
    RelayServer::Options options;
    std::string keyFile;
    bool ipv6 = false;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        auto next = [&]() -> std::string { return i + 1 < argc ? argv[++i] : std::string(); };
        if (a == "--genkey") {
            const NetKey k = makeNetKey();
            RelayKey key{};
            std::memcpy(key.data(), k.data(), key.size());
            std::printf("%s\n", relayKeyToHex(key).c_str());
            return 0;
        } else if (a == "--key-file") {
            keyFile = next();
        } else if (a == "--port") {
            options.controlPort = static_cast<uint16_t>(std::atoi(next().c_str()));
        } else if (a == "--rooms") {
            const std::string v = next();
            const size_t colon = v.find(':');
            if (colon == std::string::npos) return usage();
            options.firstRoomPort = static_cast<uint16_t>(std::atoi(v.substr(0, colon).c_str()));
            options.maxRooms = std::atoi(v.substr(colon + 1).c_str());
        } else if (a == "--clients") {
            options.maxClientsPerRoom = std::atoi(next().c_str());
        } else if (a == "--allow-private") {
            options.allowPrivate = true;
        } else if (a == "--ipv6") {
            ipv6 = true;
        } else {
            return usage();
        }
    }
    if (keyFile.empty() || options.controlPort == 0 || options.maxRooms < 1 || options.maxClientsPerRoom < 1)
        return usage();
    std::ifstream f(keyFile);
    std::string hex;
    f >> hex;
    RelayKey key{};
    if (!parseRelayKey(hex, key)) {
        std::fprintf(stderr, "chave inválida em %s (64 dígitos hexadecimais; gere com --genkey)\n", keyFile.c_str());
        return 1;
    }
    std::signal(SIGINT, onSignal);
    std::signal(SIGTERM, onSignal);
    RelayServer relay;
    auto factory = [ipv6](uint16_t port) -> std::unique_ptr<PacketLink> {
        auto link = std::make_unique<UdpLink>();
        NetAddress bind;
        NetAddress::parse(ipv6 ? "[::]:0" : "0.0.0.0:0", bind);
        bind.port = port;
        if (!link->open(bind)) return nullptr;
        return link;
    };
    const auto t0 = std::chrono::steady_clock::now();
    auto now = [&] { return std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count(); };
    if (!relay.start(options, key, factory, now())) return 1;
    // Laço de 1 ms: um relay de jogo pequeno não precisa de epoll; medir antes
    // de trocar (doc 24, item 10).
    while (!g_stop) {
        relay.update(now());
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    ERUPTION_LOG_INFO("relay: encerrado (%llu pacotes encaminhados, %llu recusados)",
                      static_cast<unsigned long long>(relay.forwarded()), static_cast<unsigned long long>(relay.rejected()));
    return 0;
}
