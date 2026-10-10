#include "host/Host.hpp"

#include <cstdarg>
#include <cstdio>

namespace eruption::compat {

void HostPadrao::log(Nivel nivel, const char* origem, const char* texto) {
    const char* rotulo = nivel == Nivel::Erro ? "ERRO" : nivel == Nivel::Aviso ? "AVISO" : "INFO";
    FILE* saida = nivel == Nivel::Info ? stdout : stderr;
    std::fprintf(saida, "[compat:%s] %s: %s\n", origem, rotulo, texto);
    std::fflush(saida);
}

void HostPadrao::avancar(double dt) {
    m_dt = dt;
    m_tempo += dt;
    ++m_quadro;
}

NoId HostPadrao::criarNo(const std::string& nome, NoId pai) {
    No no;
    no.vivo = true;
    no.nome = nome;
    no.pai = pai;
    m_nos.push_back(no);
    return static_cast<NoId>(m_nos.size() - 1);
}

void HostPadrao::destruirNo(NoId no) {
    if (no != kSemNo && no < m_nos.size()) m_nos[no].vivo = false;
}

void HostPadrao::definirPai(NoId no, NoId pai) {
    if (no != kSemNo && no < m_nos.size()) m_nos[no].pai = pai;
}

void HostPadrao::definirNome(NoId no, const std::string& nome) {
    if (no != kSemNo && no < m_nos.size()) m_nos[no].nome = nome;
}

void HostPadrao::definirTransformacao(NoId no, const Transformacao& t) {
    if (no != kSemNo && no < m_nos.size()) m_nos[no].t = t;
}

Transformacao HostPadrao::transformacao(NoId no) const {
    if (no != kSemNo && no < m_nos.size()) return m_nos[no].t;
    return {};
}

RecursoId HostPadrao::criarMalha(const std::vector<float>&, const std::vector<uint32_t>&) {
    return m_proximoRecurso++;
}

RecursoId HostPadrao::criarTextura(int, int, const std::vector<uint8_t>&) {
    return m_proximoRecurso++;
}

void HostPadrao::liberarRecurso(RecursoId) {}

namespace {
HostPadrao& hostInterno() {
    static HostPadrao h;
    return h;
}
Host* g_host = nullptr;
} // namespace

Host& host() {
    return g_host ? *g_host : hostInterno();
}

void definirHost(Host* h) {
    g_host = h;
}

void logf(Nivel nivel, const char* origem, const char* fmt, ...) {
    char buf[2048];
    va_list args;
    va_start(args, fmt);
    std::vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    host().log(nivel, origem, buf);
}

} // namespace eruption::compat
