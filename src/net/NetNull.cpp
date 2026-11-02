// Módulo de rede desligado (ERUPTION_MODULE_NET=OFF): o jogo não abre socket
// nem leva a cripto. Mesma interface; nada conecta.
#include "net/Net.hpp"

namespace eruption {

struct NetSystem::Impl {};

NetSystem& NetSystem::instance() {
    static NetSystem s;
    return s;
}

NetSystem::NetSystem() = default;
NetSystem::~NetSystem() = default;
bool NetSystem::available() const { return false; }
bool NetSystem::host(int, int, bool, const std::string&, const std::vector<std::string>&, const std::string&,
                     const std::string&, std::string& error) {
    error = "rede_desligada";
    return false;
}
bool NetSystem::join(const std::string&, const std::string&, const std::vector<std::string>&) { return false; }
void NetSystem::leave() {}
void NetSystem::update(double) {}
const char* NetSystem::state() const { return "offline"; }
bool NetSystem::isServer() const { return false; }
int NetSystem::myId() const { return 0; }
int NetSystem::port() const { return 0; }
std::string NetSystem::address() const { return {}; }
std::vector<NetPlayerInfo> NetSystem::players() const { return {}; }
void NetSystem::allowFromClient(const std::string&, size_t) {}
bool NetSystem::send(const std::string&, const NetValue&) { return false; }
bool NetSystem::sendTo(int, const std::string&, const NetValue&) { return false; }
bool NetSystem::ask(const std::string&, const NetValue&) { return false; }
bool NetSystem::poll(NetScriptEvent&) { return false; }
NetStatsInfo NetSystem::stats() const { return {}; }

} // namespace eruption
