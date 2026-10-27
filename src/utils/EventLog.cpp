#include "utils/EventLog.hpp"

#include "core/Logger.hpp"

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cmath>
#include <ctime>
#include <system_error>

namespace eruption {

namespace {

#ifdef __EMSCRIPTEN__
// Navegador sem pthreads: o proprio quadro esvazia o anel uma vez por segundo.
constexpr bool kThreaded = false;
#else
constexpr bool kThreaded = true;
#endif

std::string localDate() {
    const std::time_t now = std::time(nullptr);
    std::tm tm{};
#ifdef _WIN32
    localtime_s(&tm, &now);
#else
    localtime_r(&now, &tm);
#endif
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%S", &tm);
    return buf;
}

const char* buildName() {
#ifdef NDEBUG
    return "release " __DATE__ " " __TIME__;
#else
    return "debug " __DATE__ " " __TIME__;
#endif
}

void jsonString(std::string& out, std::string_view s) {
    out += '"';
    for (const char ch : s) {
        const unsigned char c = static_cast<unsigned char>(ch);
        if (c == '"' || c == '\\') {
            out += '\\';
            out += ch;
        } else if (c < 0x20) {
            char buf[8];
            std::snprintf(buf, sizeof(buf), "\\u%04x", c);
            out += buf;
        } else {
            out += ch;
        }
    }
    out += '"';
}

void jsonNumber(std::string& out, double v) {
    if (!std::isfinite(v)) {
        out += "null";
        return;
    }
    // Inteiro sai sem ponto (contagem, dano): o arquivo fica curto e o
    // leitor nao ve' 12.000000001.
    // to_chars: sem locale nem parse de formato (o snprintf era metade do
    // custo de um evento de 4 campos).
    char buf[32];
    const std::to_chars_result r =
        std::fabs(v) < 1e15 && v == std::floor(v)
            ? std::to_chars(buf, buf + sizeof(buf), static_cast<long long>(v))
            : std::to_chars(buf, buf + sizeof(buf), v, std::chars_format::general, 7);
    out.append(buf, size_t(r.ptr - buf));
}

void jsonKey(std::string& out, std::string_view k) {
    out += ',';
    jsonString(out, k);
    out += ':';
}

} // namespace

EventLog& EventLog::instance() {
    static EventLog log;
    return log;
}

EventLog::~EventLog() { close(); }

bool EventLog::open(const std::filesystem::path& path, const Header& header, uint64_t maxFileBytes) {
    close();
    std::error_code ec;
    if (path.has_parent_path()) std::filesystem::create_directories(path.parent_path(), ec);
    m_file = std::fopen(path.string().c_str(), "wb");
    if (!m_file) {
        ERUPTION_LOG_WARN("[TELEMETRY] nao abriu %s", path.string().c_str());
        return false;
    }
    m_path = path;
    m_header = header;
    m_maxFileBytes = std::max<uint64_t>(maxFileBytes, 64u << 10);
    m_timeMs = 0.0;
    m_frame = 0;
    m_events = 0;
    m_fileBytes = 0;
    m_part = 0;
    m_line.clear();
    m_line.reserve(kMaxLineBytes);
    m_ring.assign(kRingBytes, '\0');
    m_out.clear();
    m_out.reserve(kRingBytes);
    m_ringHead = m_ringSize = 0;
    m_dropped = 0;
    m_stop = false;
    writeHeader();
    m_open = true;
    if (kThreaded) m_writer = std::thread(&EventLog::writerLoop, this);
    ERUPTION_LOG_WARN("[TELEMETRY] gravando eventos do jogo em %s", path.string().c_str());
    return true;
}

void EventLog::close() {
    if (!m_open) return;
    m_open = false;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_stop = true;
    }
    m_wake.notify_one();
    if (m_writer.joinable()) m_writer.join();
    drain(m_out);
    writeChunk(m_out.data(), m_out.size());
    m_line.clear();
    m_line += "{\"ch\":\"_end\"";
    jsonKey(m_line, "t");
    jsonNumber(m_line, std::round(m_timeMs));
    jsonKey(m_line, "f");
    jsonNumber(m_line, double(m_frame));
    jsonKey(m_line, "events");
    jsonNumber(m_line, double(m_events));
    jsonKey(m_line, "dropped");
    jsonNumber(m_line, double(m_dropped));
    m_line += "}\n";
    writeChunk(m_line.data(), m_line.size());
    if (m_file) std::fclose(m_file);
    m_file = nullptr;
    ERUPTION_LOG_WARN("[TELEMETRY] %s fechado: %llu eventos, %llu descartados", m_path.string().c_str(),
                      static_cast<unsigned long long>(m_events), static_cast<unsigned long long>(m_dropped));
    // Solta a memoria: fechado, o registro nao segura nada.
    std::string().swap(m_line);
    std::vector<char>().swap(m_ring);
    std::vector<char>().swap(m_out);
}

uint64_t EventLog::dropped() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_dropped;
}

void EventLog::advance(float dt) {
    if (!m_open) return;
    m_timeMs += double(dt) * 1000.0;
    ++m_frame;
    if (!kThreaded && (m_frame % 60 == 0 || m_ringSize > kRingBytes / 2)) {
        drain(m_out);
        writeChunk(m_out.data(), m_out.size());
    }
}

void EventLog::begin(std::string_view channel) {
    if (!m_open) return;
    m_line.clear();
    m_line += "{\"t\":";
    jsonNumber(m_line, std::round(m_timeMs));
    jsonKey(m_line, "f");
    jsonNumber(m_line, double(m_frame));
    jsonKey(m_line, "ch");
    jsonString(m_line, channel);
}

void EventLog::field(std::string_view k, double value) {
    if (!m_open) return;
    jsonKey(m_line, k);
    jsonNumber(m_line, value);
}

void EventLog::field(std::string_view k, bool value) {
    if (!m_open) return;
    jsonKey(m_line, k);
    m_line += value ? "true" : "false";
}

void EventLog::field(std::string_view k, std::string_view value) {
    if (!m_open) return;
    jsonKey(m_line, k);
    jsonString(m_line, value.substr(0, kMaxStringBytes));
}

void EventLog::field(std::string_view k, const float* values, int count) {
    if (!m_open) return;
    jsonKey(m_line, k);
    m_line += '[';
    for (int i = 0; i < count; ++i) {
        if (i) m_line += ',';
        jsonNumber(m_line, double(values[i]));
    }
    m_line += ']';
}

void EventLog::commit() {
    if (!m_open) return;
    m_line += "}\n";
    if (m_line.size() > kMaxLineBytes) {
        // Linha gigante (tabela com centenas de campos): cai inteira, como
        // anel cheio, em vez de crescer o buffer reservado.
        std::lock_guard<std::mutex> lock(m_mutex);
        ++m_dropped;
        return;
    }
    push(m_line.data(), m_line.size());
}

void EventLog::push(const char* data, size_t n) {
    bool wake = false;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (n > kRingBytes - m_ringSize) {
            ++m_dropped;
            return;
        }
        size_t tail = (m_ringHead + m_ringSize) % kRingBytes;
        const size_t first = std::min(n, kRingBytes - tail);
        std::copy(data, data + first, m_ring.data() + tail);
        std::copy(data + first, data + n, m_ring.data());
        m_ringSize += n;
        ++m_events;
        wake = m_ringSize > kRingBytes / 2;
    }
    if (wake && kThreaded) m_wake.notify_one();
}

void EventLog::drain(std::vector<char>& out) {
    out.clear();
    std::lock_guard<std::mutex> lock(m_mutex);
    const size_t first = std::min(m_ringSize, kRingBytes - m_ringHead);
    out.insert(out.end(), m_ring.data() + m_ringHead, m_ring.data() + m_ringHead + first);
    out.insert(out.end(), m_ring.data(), m_ring.data() + (m_ringSize - first));
    m_ringHead = (m_ringHead + m_ringSize) % kRingBytes;
    m_ringSize = 0;
}

void EventLog::writerLoop() {
    for (;;) {
        bool stop;
        {
            std::unique_lock<std::mutex> lock(m_mutex);
            m_wake.wait_for(lock, std::chrono::seconds(1),
                            [this] { return m_stop || m_ringSize > kRingBytes / 2; });
            stop = m_stop;
        }
        if (stop) return; // o close esvazia o resto na thread que fechou
        drain(m_out);
        writeChunk(m_out.data(), m_out.size());
        std::fflush(m_file);
    }
}

void EventLog::writeChunk(const char* data, size_t n) {
    if (!m_file || n == 0) return;
    if (m_fileBytes > 0 && m_fileBytes + n > m_maxFileBytes) {
        std::fclose(m_file);
        std::filesystem::path old = m_path;
        old += ".1";
        std::error_code ec;
        std::filesystem::rename(m_path, old, ec);
        m_file = std::fopen(m_path.string().c_str(), "wb");
        m_fileBytes = 0;
        ++m_part;
        if (!m_file) return;
        writeHeader();
    }
    m_fileBytes += std::fwrite(data, 1, n, m_file);
}

void EventLog::writeHeader() {
    // Na thread de escrita (rotacao): linha propria, longe da m_line do jogo.
    std::string h = "{\"ch\":\"_head\",\"v\":1";
    jsonKey(h, "project");
    jsonString(h, m_header.project);
    jsonKey(h, "map");
    jsonString(h, m_header.map);
    jsonKey(h, "build");
    jsonString(h, buildName());
    jsonKey(h, "date");
    jsonString(h, localDate());
    jsonKey(h, "part");
    jsonNumber(h, double(m_part));
    h += "}\n";
    m_fileBytes += std::fwrite(h.data(), 1, h.size(), m_file);
}

} // namespace eruption
