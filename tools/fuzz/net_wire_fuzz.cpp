// Fuzz do leitor de bits da rede (src/net/Wire). Os primeiros bytes da
// entrada são um "esquema" (sequência de campos com faixas arbitrárias) e o
// resto é o pacote. Confere: nenhuma leitura sai do buffer (ASan), nada de
// comportamento indefinido (UBSan), e o que foi lido sem erro, reescrito com o
// mesmo esquema e lido de novo, dá os mesmos valores.
//
// Com libFuzzer (clang -fsanitize=fuzzer) usa só LLVMFuzzerTestOneInput.
// Sem ele (GCC), o main abaixo muta entradas aleatórias por N segundos:
//   tools/fuzz/run_net_fuzz.sh [segundos]
#include "net/Wire.hpp"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <string>
#include <vector>

using namespace eruption;

namespace {

struct Field {
    uint8_t op;
    int32_t a = 0, b = 0;
    float fmin = 0.0f, fmax = 0.0f, fstep = 0.0f;
    uint32_t len = 0;
};

struct Value {
    uint32_t u = 0;
    int32_t i = 0;
    float f = 0.0f;
    std::string s;
};

uint32_t take32(const uint8_t*& p, const uint8_t* end) {
    uint32_t v = 0;
    for (int k = 0; k < 4 && p < end; ++k) v |= static_cast<uint32_t>(*p++) << (8 * k);
    return v;
}

std::vector<Field> parseSchema(const uint8_t*& p, const uint8_t* end) {
    std::vector<Field> fields;
    const size_t count = p < end ? (*p++ % 24) : 0;
    for (size_t n = 0; n < count && p < end; ++n) {
        Field f;
        f.op = static_cast<uint8_t>(*p++ % 8);
        const uint32_t x = take32(p, end);
        const uint32_t y = take32(p, end);
        f.a = static_cast<int32_t>(x);
        f.b = static_cast<int32_t>(y);
        std::memcpy(&f.fmin, &x, 4);
        std::memcpy(&f.fmax, &y, 4);
        const uint32_t z = take32(p, end);
        std::memcpy(&f.fstep, &z, 4);
        f.len = z % 300;
        fields.push_back(f);
    }
    return fields;
}

void readAll(BitReader& r, const std::vector<Field>& fields, std::vector<Value>& out) {
    out.clear();
    for (const Field& f : fields) {
        Value v;
        switch (f.op) {
            case 0: v.u = r.readBits(1 + static_cast<int>(static_cast<uint32_t>(f.a) % 32)); break;
            case 1: v.u = r.readBool() ? 1u : 0u; break;
            case 2: v.i = r.readInt(f.a, f.b); break;
            case 3: v.f = r.readFloat(); break;
            case 4: v.f = r.readQuantized(f.fmin, f.fmax, f.fstep); break;
            case 5: r.readString(v.s, f.len); break;
            case 6: {
                uint8_t tmp[64];
                const size_t n = f.len % sizeof(tmp);
                if (r.readBytes(tmp, n)) v.s.assign(reinterpret_cast<const char*>(tmp), n);
                break;
            }
            default: r.alignToByte(); break;
        }
        if (!r.ok()) return;
        out.push_back(v);
    }
}

void writeAll(BitWriter& w, const std::vector<Field>& fields, const std::vector<Value>& values) {
    for (size_t k = 0; k < values.size(); ++k) {
        const Field& f = fields[k];
        const Value& v = values[k];
        switch (f.op) {
            case 0: w.writeBits(v.u, 1 + static_cast<int>(static_cast<uint32_t>(f.a) % 32)); break;
            case 1: w.writeBool(v.u != 0); break;
            case 2: w.writeInt(v.i, f.a, f.b); break;
            case 3: w.writeFloat(v.f); break;
            case 4: w.writeQuantized(v.f, f.fmin, f.fmax, f.fstep); break;
            case 5: w.writeString(v.s, f.len); break;
            case 6: w.writeBytes(reinterpret_cast<const uint8_t*>(v.s.data()), v.s.size()); break;
            default: w.alignToByte(); break;
        }
    }
}

bool sameValue(const Field& f, const Value& x, const Value& y) {
    switch (f.op) {
        case 0:
        case 1: return x.u == y.u;
        case 2: return x.i == y.i;
        case 3: return std::memcmp(&x.f, &y.f, 4) == 0;
        // Quantizado: reescrever o valor lido cai no mesmo passo, a menos de
        // um passo por arredondamento, mais o erro do float no valor (2^-22).
        case 4: return std::fabs(x.f - y.f) <= f.fstep * 1.01f + std::fabs(x.f) * 2.4e-7f;
        case 5:
        case 6: return x.s == y.s;
        default: return true;
    }
}

} // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    const uint8_t* p = data;
    const uint8_t* end = data + size;
    const std::vector<Field> fields = parseSchema(p, end);

    // O pacote em cópia exata do tamanho: o ASan pega um byte lido além.
    std::vector<uint8_t> packet(p, end);
    BitReader r(packet.empty() ? nullptr : packet.data(), packet.size());
    std::vector<Value> first;
    readAll(r, fields, first);
    if (!r.ok()) return 0;

    std::vector<uint8_t> buf(packet.size() + 16);
    BitWriter w(buf.data(), buf.size());
    writeAll(w, fields, first);
    if (!w.ok()) {
        std::fprintf(stderr, "reescrita do que foi lido estourou\n");
        std::abort();
    }
    const size_t n = w.finish();
    BitReader r2(buf.data(), n);
    std::vector<Value> second;
    readAll(r2, fields, second);
    if (!r2.ok() || second.size() != first.size()) {
        std::fprintf(stderr, "releitura falhou\n");
        std::abort();
    }
    for (size_t k = 0; k < first.size(); ++k)
        if (!sameValue(fields[k], first[k], second[k])) {
            std::fprintf(stderr, "campo %zu (op %d) mudou na ida e volta\n", k, fields[k].op);
            std::abort();
        }
    return 0;
}

#ifndef ERUPTION_LIBFUZZER
// Mutação simples (troca de bit, byte, corte, emenda) sobre um corpus que
// cresce com entradas que leram mais campos sem erro.
int main(int argc, char** argv) {
    const double seconds = argc > 1 ? std::atof(argv[1]) : 60.0;
    const unsigned seed = argc > 2 ? static_cast<unsigned>(std::atoi(argv[2])) : 1u;
    std::mt19937 rng(seed);
    std::vector<std::vector<uint8_t>> corpus;
    for (int k = 0; k < 64; ++k) {
        std::vector<uint8_t> v(1 + rng() % 256);
        for (uint8_t& b : v) b = static_cast<uint8_t>(rng());
        corpus.push_back(v);
    }
    const auto start = std::chrono::steady_clock::now();
    uint64_t runs = 0;
    for (;;) {
        std::vector<uint8_t> in = corpus[rng() % corpus.size()];
        const int edits = 1 + static_cast<int>(rng() % 8);
        for (int e = 0; e < edits; ++e) {
            switch (rng() % 5) {
                case 0: if (!in.empty()) in[rng() % in.size()] ^= static_cast<uint8_t>(1u << (rng() % 8)); break;
                case 1: if (!in.empty()) in[rng() % in.size()] = static_cast<uint8_t>(rng()); break;
                case 2: if (in.size() > 1) in.resize(rng() % in.size()); break;
                case 3: in.push_back(static_cast<uint8_t>(rng())); break;
                default: {
                    const std::vector<uint8_t>& other = corpus[rng() % corpus.size()];
                    in.insert(in.end(), other.begin(), other.begin() + static_cast<long>(rng() % (other.size() + 1)));
                    if (in.size() > 4096) in.resize(4096);
                }
            }
        }
        LLVMFuzzerTestOneInput(in.data(), in.size());
        if (rng() % 64 == 0 && corpus.size() < 4096) corpus.push_back(in);
        if ((++runs & 0xFFF) == 0) {
            const double t = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
            if (t >= seconds) break;
        }
    }
    std::printf("fuzz: %llu entradas em %.0f s, sem falha\n", static_cast<unsigned long long>(runs), seconds);
    return 0;
}
#endif
