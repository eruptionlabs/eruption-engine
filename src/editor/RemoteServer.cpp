#include "editor/RemoteServer.hpp"

#include <algorithm>
#include <cstring>
#include <fstream>
#include <random>
#include <sstream>

#ifndef _WIN32
#include <arpa/inet.h>
#include <ifaddrs.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace eruption {

namespace {

namespace fs = std::filesystem;

constexpr size_t kMaxBody = 1 << 20;

std::string jsonString(const std::string& s) {
    std::string o = "\"";
    for (unsigned char c : s) {
        switch (c) {
            case '"': o += "\\\""; break;
            case '\\': o += "\\\\"; break;
            case '\n': o += "\\n"; break;
            case '\r': o += "\\r"; break;
            case '\t': o += "\\t"; break;
            default:
                if (c < 0x20) {
                    char b[8];
                    std::snprintf(b, sizeof(b), "\\u%04x", c);
                    o += b;
                } else {
                    o += char(c);
                }
        }
    }
    return o + "\"";
}

std::string urlDecode(const std::string& s) {
    std::string o;
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '%' && i + 2 < s.size()) {
            o += char(std::stoi(s.substr(i + 1, 2), nullptr, 16));
            i += 2;
        } else {
            o += s[i] == '+' ? ' ' : s[i];
        }
    }
    return o;
}

std::string queryParam(const std::string& query, const std::string& key) {
    size_t pos = 0;
    while (pos <= query.size()) {
        const size_t amp = query.find('&', pos);
        const std::string kv = query.substr(pos, amp == std::string::npos ? std::string::npos : amp - pos);
        const size_t eq = kv.find('=');
        if (kv.substr(0, eq) == key) return eq == std::string::npos ? std::string() : urlDecode(kv.substr(eq + 1));
        if (amp == std::string::npos) break;
        pos = amp + 1;
    }
    return {};
}

std::string http(int code, const char* status, const char* type, const std::string& body) {
    std::ostringstream o;
    o << "HTTP/1.1 " << code << " " << status << "\r\nContent-Type: " << type << "\r\nContent-Length: " << body.size()
      << "\r\nCache-Control: no-store\r\nConnection: close\r\n\r\n"
      << body;
    return o.str();
}

// Página da segunda tela. Monaco (MIT) vem da CDN; sem internet, cai numa
// área de texto simples.
const char* kPage = R"HTML(<!doctype html>
<html lang="en"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>Eruption scripts</title>
<style>
:root{--bg:#1d1e21;--panel:#26282c;--text:#e6e6e8;--dim:#9a9ca3;--accent:#ff3c28}
*{box-sizing:border-box}body{margin:0;background:var(--bg);color:var(--text);font:14px system-ui,sans-serif;height:100vh;display:flex;flex-direction:column}
header{display:flex;gap:8px;align-items:center;padding:8px 12px;background:var(--panel)}
header b{color:var(--accent)}#status{color:var(--dim);margin-left:auto}
button{background:var(--accent);color:#fff;border:0;border-radius:6px;padding:8px 14px;font-weight:600}
main{flex:1;display:flex;min-height:0}
#files{width:200px;overflow:auto;background:var(--panel);border-right:1px solid #333}
#files div{padding:8px 12px;cursor:pointer}#files div.on{background:#3a2a28;color:#fff}
#edit{flex:1;min-width:0;display:flex;flex-direction:column}#editor{flex:1;min-height:0}
textarea{width:100%;height:100%;background:#151619;color:var(--text);border:0;padding:10px;font:13px monospace}
#log{height:22vh;overflow:auto;background:#121315;font:12px monospace;padding:6px 10px;color:#c9c9cf;white-space:pre-wrap}
@media (max-width:700px){#files{width:120px}}
</style></head><body>
<header><b>Eruption</b> scripts <button id="save">Save</button><span id="status">loading...</span></header>
<main><div id="files"></div><div id="edit"><div id="editor"></div><div id="log"></div></div></main>
<script>
const T=new URLSearchParams(location.search).get('t')||'';
const api=(p,o)=>fetch(p+(p.includes('?')?'&':'?')+'t='+encodeURIComponent(T),o);
let cur=null,ed=null,area=null,since=0;
const st=s=>document.getElementById('status').textContent=s;
function getText(){return ed?ed.getValue():area.value}
function setText(v){if(ed)ed.setValue(v);else area.value=v}
async function list(){const r=await api('/api/files');if(!r.ok){st('access denied');return}
 const fs=await r.json();const el=document.getElementById('files');el.innerHTML='';
 fs.forEach(f=>{const d=document.createElement('div');d.textContent=f;d.onclick=()=>openF(f);if(f===cur)d.className='on';el.appendChild(d)});
 if(!cur&&fs.length)openF(fs[0]);}
async function openF(f){const r=await api('/api/file?path='+encodeURIComponent(f));setText(await r.text());cur=f;st(f);list()}
async function save(){if(!cur)return;const r=await api('/api/file?path='+encodeURIComponent(cur),{method:'POST',body:getText()});
 st(r.ok?('saved '+cur+' - the game reloads it'):'could not save')}
async function poll(){try{const r=await api('/api/log?since='+since);if(r.ok){const j=await r.json();since=j.next;
 const l=document.getElementById('log');j.lines.forEach(x=>{l.textContent+=x+'\n'});if(j.lines.length)l.scrollTop=l.scrollHeight}}catch(e){}
 setTimeout(poll,1000)}
document.getElementById('save').onclick=save;
addEventListener('keydown',e=>{if((e.ctrlKey||e.metaKey)&&e.key==='s'){e.preventDefault();save()}});
function plain(){if(ed||area)return;area=document.createElement('textarea');document.getElementById('editor').appendChild(area);list()}
const s=document.createElement('script');s.src='https://cdn.jsdelivr.net/npm/monaco-editor@0.52.0/min/vs/loader.js';
s.onload=()=>{require.config({paths:{vs:'https://cdn.jsdelivr.net/npm/monaco-editor@0.52.0/min/vs'}});
 require(['vs/editor/editor.main'],()=>{if(area)return;ed=monaco.editor.create(document.getElementById('editor'),
 {language:'lua',theme:'vs-dark',automaticLayout:true,fontSize:13,minimap:{enabled:false}});list()})};
s.onerror=plain;document.head.appendChild(s);setTimeout(()=>{if(!ed)plain()},5000);poll();
</script></body></html>)HTML";

} // namespace

std::vector<std::string> RemoteServer::localAddresses() {
    std::vector<std::string> out;
#ifndef _WIN32
    ifaddrs* list = nullptr;
    if (getifaddrs(&list) != 0) return out;
    for (ifaddrs* a = list; a; a = a->ifa_next) {
        if (!a->ifa_addr || a->ifa_addr->sa_family != AF_INET) continue;
        char buf[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &reinterpret_cast<sockaddr_in*>(a->ifa_addr)->sin_addr, buf, sizeof(buf));
        if (std::strcmp(buf, "127.0.0.1") != 0) out.push_back(buf);
    }
    freeifaddrs(list);
#endif
    return out;
}

bool RemoteServer::start(uint16_t port, const fs::path& scriptsDir) {
#ifdef _WIN32
    (void)port;
    (void)scriptsDir;
    error = "The second screen is not available on Windows yet.";
    return false;
#else
    if (m_running) return true;
    m_dir = scriptsDir;
    std::random_device rd;
    const char* hex = "0123456789abcdef";
    m_token.clear();
    for (int i = 0; i < 16; ++i) m_token += hex[rd() % 16];
    m_listen = socket(AF_INET, SOCK_STREAM, 0);
    if (m_listen < 0) {
        error = "Could not open a network socket.";
        return false;
    }
    int yes = 1;
    setsockopt(m_listen, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(port);
    if (bind(m_listen, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0 || listen(m_listen, 8) != 0) {
        error = "Port " + std::to_string(port) + " is in use.";
        close(m_listen);
        m_listen = -1;
        return false;
    }
    m_port = port;
    m_running = true;
    m_thread = std::thread([this] { loop(); });
    error.clear();
    return true;
#endif
}

void RemoteServer::stop() {
    if (!m_running) return;
    m_running = false;
    if (m_thread.joinable()) m_thread.join();
#ifndef _WIN32
    if (m_listen >= 0) close(m_listen);
#endif
    m_listen = -1;
}

void RemoteServer::pushLog(const std::string& line) {
    std::lock_guard<std::mutex> lock(m_logMutex);
    m_log.push_back(line);
    ++m_logSerial;
    while (m_log.size() > 400) m_log.pop_front();
}

void RemoteServer::loop() {
#ifndef _WIN32
    while (m_running) {
        pollfd p{m_listen, POLLIN, 0};
        if (poll(&p, 1, 200) <= 0) continue;
        const int fd = accept(m_listen, nullptr, nullptr);
        if (fd < 0) continue;
        timeval tv{2, 0};
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        handle(fd);
        close(fd);
    }
#endif
}

void RemoteServer::handle(int fd) {
#ifndef _WIN32
    std::string req;
    char buf[8192];
    size_t headerEnd = std::string::npos;
    while (headerEnd == std::string::npos && req.size() < 65536) {
        const ssize_t n = recv(fd, buf, sizeof(buf), 0);
        if (n <= 0) return;
        req.append(buf, size_t(n));
        headerEnd = req.find("\r\n\r\n");
    }
    if (headerEnd == std::string::npos) return;
    std::istringstream head(req.substr(0, headerEnd));
    std::string method, target, version;
    head >> method >> target >> version;
    size_t length = 0;
    std::string line;
    while (std::getline(head, line)) {
        std::string lower = line;
        std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return std::tolower(c); });
        if (lower.rfind("content-length:", 0) == 0) length = std::stoul(line.substr(15));
    }
    if (length > kMaxBody) {
        const std::string r = http(413, "Payload Too Large", "text/plain", "too large");
        send(fd, r.data(), r.size(), MSG_NOSIGNAL);
        return;
    }
    std::string body = req.substr(headerEnd + 4);
    while (body.size() < length) {
        const ssize_t n = recv(fd, buf, sizeof(buf), 0);
        if (n <= 0) break;
        body.append(buf, size_t(n));
    }
    const std::string out = respond(method, target, body);
    size_t sent = 0;
    while (sent < out.size()) {
        const ssize_t n = send(fd, out.data() + sent, out.size() - sent, MSG_NOSIGNAL);
        if (n <= 0) break;
        sent += size_t(n);
    }
#else
    (void)fd;
#endif
}

bool RemoteServer::safePath(const std::string& rel, fs::path& out) const {
    // Só .luau dentro da pasta de scripts, sem subir de pasta.
    if (rel.empty() || rel.find("..") != std::string::npos || rel.front() == '/' || rel.find('\\') != std::string::npos)
        return false;
    const fs::path p = m_dir / rel;
    if (p.extension() != ".luau") return false;
    out = p;
    return true;
}

std::string RemoteServer::respond(const std::string& method, const std::string& target, const std::string& body) {
    const size_t q = target.find('?');
    const std::string path = target.substr(0, q);
    const std::string query = q == std::string::npos ? std::string() : target.substr(q + 1);
    if (queryParam(query, "t") != m_token) return http(403, "Forbidden", "text/plain", "Open the address shown in the editor.");
    if (method == "GET" && (path == "/" || path == "/index.html")) return http(200, "OK", "text/html; charset=utf-8", kPage);
    if (method == "GET" && path == "/api/files") {
        std::vector<std::string> files;
        std::error_code ec;
        for (auto it = fs::recursive_directory_iterator(m_dir, ec); !ec && it != fs::recursive_directory_iterator(); ++it)
            if (it->is_regular_file() && it->path().extension() == ".luau")
                files.push_back(fs::relative(it->path(), m_dir, ec).generic_string());
        std::sort(files.begin(), files.end());
        std::string j = "[";
        for (size_t i = 0; i < files.size(); ++i) j += (i ? "," : "") + jsonString(files[i]);
        return http(200, "OK", "application/json", j + "]");
    }
    if (path == "/api/file") {
        fs::path p;
        if (!safePath(queryParam(query, "path"), p)) return http(400, "Bad Request", "text/plain", "bad path");
        if (method == "GET") {
            std::ifstream f(p, std::ios::binary);
            if (!f) return http(404, "Not Found", "text/plain", "no such file");
            std::ostringstream ss;
            ss << f.rdbuf();
            return http(200, "OK", "text/plain; charset=utf-8", ss.str());
        }
        if (method == "POST" || method == "PUT") {
            std::error_code ec;
            fs::create_directories(p.parent_path(), ec);
            std::ofstream(p, std::ios::binary) << body;
            pushLog("[second screen] saved " + p.filename().string());
            return http(200, "OK", "text/plain", "saved");
        }
    }
    if (method == "GET" && path == "/api/log") {
        std::lock_guard<std::mutex> lock(m_logMutex);
        const uint64_t since = std::strtoull(queryParam(query, "since").c_str(), nullptr, 10);
        const uint64_t first = m_logSerial - m_log.size();
        std::string j = "{\"next\":" + std::to_string(m_logSerial) + ",\"lines\":[";
        bool any = false;
        for (uint64_t s = std::max(since, first); s < m_logSerial; ++s) {
            j += (any ? "," : "") + jsonString(m_log[size_t(s - first)]);
            any = true;
        }
        return http(200, "OK", "application/json", j + "]}");
    }
    return http(404, "Not Found", "text/plain", "not found");
}

} // namespace eruption
