// Teste dos pacotes .ecp no PackManager (formats/PackManager, formats/EcpReader.hpp).
// Os pacotes sao montados AQUI, sem compressor: o indice e os arquivos "zstd"
// usam quadros zstd validos com blocos crus (um decodificador de verdade os
// le' igual); o arquivo grande tambem vai em blocos de 128 KiB. Com um
// argumento --bench <pacote...> mede abertura e vazao de pacotes reais.
#include "formats/EcpReader.hpp"
#include "formats/PackManager.hpp"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>

using namespace eruption;
namespace fs = std::filesystem;

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FALHOU %s:%d: %s\n", __FILE__, __LINE__, #c); g_fail = 1; } } while (0)

static void put16(std::vector<uint8_t>& v, uint16_t x) { for (int i = 0; i < 2; ++i) v.push_back(uint8_t(x >> (8 * i))); }
static void put32(std::vector<uint8_t>& v, uint32_t x) { for (int i = 0; i < 4; ++i) v.push_back(uint8_t(x >> (8 * i))); }
static void put64(std::vector<uint8_t>& v, uint64_t x) { for (int i = 0; i < 8; ++i) v.push_back(uint8_t(x >> (8 * i))); }

// Quadro zstd com blocos crus: magic, descritor (conteudo de 4 bytes, segmento
// unico), blocos de ate' 128 KiB (o ultimo marcado). Valido para qualquer decodificador.
static std::vector<uint8_t> rawZstdFrame(const std::vector<uint8_t>& d) {
    std::vector<uint8_t> f;
    put32(f, 0xFD2FB528u);
    f.push_back(0xA0); // single segment + Frame_Content_Size de 4 bytes
    put32(f, uint32_t(d.size()));
    size_t pos = 0;
    do {
        const size_t n = std::min<size_t>(d.size() - pos, 128 * 1024 - 1);
        const bool last = pos + n >= d.size();
        const uint32_t hdr = (last ? 1u : 0u) | (0u << 1) | (uint32_t(n) << 3);
        f.push_back(uint8_t(hdr)); f.push_back(uint8_t(hdr >> 8)); f.push_back(uint8_t(hdr >> 16));
        f.insert(f.end(), d.begin() + long(pos), d.begin() + long(pos + n));
        pos += n;
    } while (pos < d.size());
    return f;
}

struct Item { std::string path; std::vector<uint8_t> data; uint8_t codec = 0; bool tomb = false; };

static bool writePack(const fs::path& out, std::vector<Item> items) {
    std::vector<uint8_t> f(32, 0);
    std::memcpy(f.data(), "ECPK", 4);
    f[4] = 1;
    std::sort(items.begin(), items.end(), [](const Item& a, const Item& b) { return a.path < b.path; });
    struct E { uint64_t off, cs, rs, xx; };
    std::vector<E> es;
    for (const Item& it : items) {
        E e{0, 0, 0, 0};
        if (!it.tomb) {
            const std::vector<uint8_t> stored = it.codec == 0 ? it.data : rawZstdFrame(it.data);
            e = {f.size(), stored.size(), it.data.size(), ecp::detail::xxh64(it.data.data(), it.data.size())};
            f.insert(f.end(), stored.begin(), stored.end());
        }
        es.push_back(e);
    }
    std::vector<uint8_t> ix;
    ix.insert(ix.end(), {'E', 'C', 'I', 'X'});
    put32(ix, 1); put32(ix, uint32_t(items.size())); put16(ix, 0); put16(ix, 0);
    for (size_t i = 0; i < items.size(); ++i) {
        put16(ix, uint16_t(items[i].path.size()));
        ix.insert(ix.end(), items[i].path.begin(), items[i].path.end());
        ix.push_back(items[i].tomb ? 0 : items[i].codec);
        ix.push_back(items[i].tomb ? 1 : 0);
        put16(ix, 0);
        put64(ix, es[i].off); put64(ix, es[i].cs); put64(ix, es[i].rs); put64(ix, es[i].xx); put64(ix, 0);
    }
    const std::vector<uint8_t> frame = rawZstdFrame(ix);
    const uint64_t ioff = f.size();
    f.insert(f.end(), frame.begin(), frame.end());
    std::vector<uint8_t> foot;
    foot.insert(foot.end(), {'E', 'C', 'P', 'F'});
    put16(foot, 1); put16(foot, 0); put64(foot, ioff); put64(foot, frame.size());
    put64(foot, ecp::detail::xxh64(frame.data(), frame.size(), ecp::detail::xxh64(foot.data(), 24)));
    f.insert(f.end(), foot.begin(), foot.end());
    std::ofstream o(out, std::ios::binary);
    o.write(reinterpret_cast<const char*>(f.data()), std::streamsize(f.size()));
    return bool(o);
}

static std::vector<uint8_t> bytesOf(const std::string& s) { return {s.begin(), s.end()}; }
static std::vector<uint8_t> pattern(size_t n, uint32_t seed) {
    std::vector<uint8_t> v(n);
    uint32_t x = seed;
    for (size_t i = 0; i < n; ++i) { x = x * 1664525u + 1013904223u; v[i] = uint8_t(x >> 24); }
    return v;
}
static std::string str(const std::vector<uint8_t>& v) { return {v.begin(), v.end()}; }

static void bench(int argc, char** argv) {
    using clk = std::chrono::steady_clock;
    const auto t0 = clk::now();
    PackManager pm;
    for (int i = 2; i < argc; ++i) pm.addDirectory(argv[i]);
    const double open = std::chrono::duration<double, std::milli>(clk::now() - t0).count();
    const auto names = pm.getAllFilenames();
    const auto t1 = clk::now();
    uint64_t bytes = 0;
    for (const auto& n : names) bytes += pm.extract(n).size();
    const double s = std::chrono::duration<double>(clk::now() - t1).count();
    std::printf("bench: abertura %.2f ms, %zu arquivos, leitura de %.1f MB em %.3f s = %.0f MB/s\n", open, names.size(),
                double(bytes) / 1e6, s, double(bytes) / 1e6 / s);
}

int main(int argc, char** argv) {
    if (argc > 2 && std::string(argv[1]) == "--bench") { bench(argc, argv); return 0; }
    const fs::path dir = fs::temp_directory_path() / ("eruption_ecp_test_" + std::to_string(std::rand()));
    fs::create_directories(dir / "loose" / "dir");
    const std::vector<uint8_t> big = pattern(8u << 20, 7), big2 = pattern(3u << 20, 9);

    // Pacote base: raw (codec 0) e "zstd" (1); caminho misto/maiusculo e UTF-8.
    CHECK(writePack(dir / "base.ecp", {
        {"dir/a.txt", bytesOf("A1"), 1}, {"dir/b.bin", pattern(1 << 20, 3), 0},
        {"data/big.bin", big, 1}, {"texto/\xC3\xA7" "a.txt", bytesOf("cedilha"), 1},
        {"vazio.bin", {}, 0}, {"extra/so_base.txt", bytesOf("base"), 0}}));
    // Patch: substitui dir/a.txt, apaga dir/b.bin (tombstone), adiciona new/c.txt e troca o grande.
    CHECK(writePack(dir / "patch.ecp", {
        {"dir/a.txt", bytesOf("A2"), 0}, {"dir/b.bin", {}, 0, true},
        {"new/c.txt", bytesOf("novo"), 1}, {"data/big.bin", big2, 1}}));

    PackManager pm;
    pm.addDirectory((dir / "base.ecp").string());
    CHECK(pm.packCount() == 1);
    CHECK(str(pm.extract("Dir\\A.TXT")) == "A1");     // caminho normalizado
    pm.addDirectory((dir / "patch.ecp").string());
    CHECK(pm.packCount() == 2 && pm.sourceCount() == 2 && pm.dirCount() == 0);
    CHECK(str(pm.extract("dir/a.txt")) == "A2");      // o ultimo pacote vence
    CHECK(!pm.exists("dir/b.bin") && pm.extract("dir/b.bin").empty()); // tombstone
    CHECK(pm.exists("new/c.txt") && str(pm.extractSafe("NEW\\C.txt")) == "novo");
    CHECK(pm.extract("data/big.bin") == big2);       // 3 MiB do patch, nao os 8 do base
    CHECK(str(pm.extract("texto/\xC3\xA7" "a.txt")) == "cedilha");
    CHECK(pm.exists("vazio.bin") && pm.extract("vazio.bin").empty());
    CHECK(str(pm.extract("extra/so_base.txt")) == "base"); // so' no base continua la'
    CHECK(str(pm.extract("c.txt")) == "novo");        // so' o nome do arquivo (basename)
    CHECK(!pm.exists("nao/existe.txt") && pm.extract("../x").empty());
    auto all = pm.getAllFilenames();
    CHECK(std::find(all.begin(), all.end(), "dir\\a.txt") != all.end());
    CHECK(std::find(all.begin(), all.end(), "dir\\b.bin") == all.end());
    CHECK(pm.getFilenamesByPrefix("NEW\\").size() == 1);
    CHECK(pm.getSources().size() == 2 && !pm.getSources()[0].isDirectory && pm.getSources()[0].fileCount == 6);

    // Pasta solta adicionada DEPOIS ainda vence o pacote (pastas primeiro).
    { std::ofstream(dir / "loose" / "dir" / "a.txt") << "LOOSE"; }
    pm.addDirectory((dir / "loose").string());
    CHECK(pm.sourceCount() == 3 && pm.dirCount() == 1);
    CHECK(str(pm.extract("dir/a.txt")) == "LOOSE");
    CHECK(str(pm.extract("new/c.txt")) == "novo");

    // Pacote invalido: recusado, estado intacto. Truncado, rodape ruim, nao e' ecp.
    {
        std::vector<uint8_t> raw;
        { std::ifstream f(dir / "base.ecp", std::ios::binary); raw.assign(std::istreambuf_iterator<char>(f), {}); }
        auto bad = [&](const char* name, std::vector<uint8_t> v) {
            { std::ofstream o(dir / name, std::ios::binary); o.write(reinterpret_cast<const char*>(v.data()), std::streamsize(v.size())); }
            const size_t before = pm.packCount();
            CHECK(!pm.addPack((dir / name).string()));
            CHECK(pm.packCount() == before);
        };
        bad("trunc.ecp", {raw.begin(), raw.begin() + 100});
        auto flip = raw; flip[flip.size() - 5] ^= 0x55; bad("footer.ecp", flip);   // checksum do indice
        auto ver = raw; ver[4] = 9; bad("ver.ecp", ver);                            // versao desconhecida
        bad("lixo.ecp", std::vector<uint8_t>(200, 0x41));
        CHECK(!pm.addPack((dir / "naoexiste.ecp").string()));
        CHECK(str(pm.extract("new/c.txt")) == "novo");
    }
    // Dado corrompido no meio de um arquivo (so' a leitura dele falha, o resto segue).
    {
        std::vector<uint8_t> raw;
        { std::ifstream f(dir / "base.ecp", std::ios::binary); raw.assign(std::istreambuf_iterator<char>(f), {}); }
        for (size_t i = 100; i < 200; ++i) raw[i] ^= 0xFF; // dentro de dir/b.bin (cru) ou a.txt: tolerado
        { std::ofstream o(dir / "dano.ecp", std::ios::binary); o.write(reinterpret_cast<const char*>(raw.data()), std::streamsize(raw.size())); }
        PackManager p2;
        CHECK(p2.addPack((dir / "dano.ecp").string()));
        (void)p2.extract("data/big.bin"); // nao pode derrubar o processo
    }

    // Medicao com os dados deste teste (sem compressao real: so' o custo de abrir e copiar).
    {
        using clk = std::chrono::steady_clock;
        PackManager p3;
        const auto t0 = clk::now();
        p3.addDirectory((dir / "base.ecp").string());
        const double open = std::chrono::duration<double, std::milli>(clk::now() - t0).count();
        const auto t1 = clk::now();
        size_t n = 0;
        for (int i = 0; i < 10; ++i) n += p3.extract("data/big.bin").size();
        const double s = std::chrono::duration<double>(clk::now() - t1).count();
        std::printf("test_ecp_pack: abertura %.2f ms; leitura de %.0f MB em %.3f s (%.0f MB/s)\n", open,
                    double(n) / 1e6, s, double(n) / 1e6 / s);
    }
    std::error_code ec;
    fs::remove_all(dir, ec);
    if (!g_fail) std::printf("test_ecp_pack: OK\n");
    return g_fail;
}
