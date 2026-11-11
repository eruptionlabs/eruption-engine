#pragma once
// Le um arquivo binario inteiro com um so' read(). O jeito com
// istreambuf_iterator copia byte a byte e, no build sem otimizacao, custa
// ~40 s por GB (a carga de um mapa com 1 GB de modelos e pacotes ia de 8 s
// para 15 s so' nisso).

#include <cstdint>
#include <fstream>
#include <string>
#include <vector>

namespace eruption {

template <class Byte>
bool readWholeFile(std::ifstream& f, std::vector<Byte>& out) {
    static_assert(sizeof(Byte) == 1, "readWholeFile: vetor de bytes");
    out.clear();
    if (!f.is_open()) return false;
    f.seekg(0, std::ios::end);
    const std::streamoff n = f.tellg();
    if (n < 0) return false;
    f.seekg(0, std::ios::beg);
    out.resize(static_cast<size_t>(n));
    if (n > 0 && !f.read(reinterpret_cast<char*>(out.data()), n)) {
        out.clear();
        return false;
    }
    return true;
}

template <class Byte>
bool readWholeFile(const std::string& path, std::vector<Byte>& out) {
    std::ifstream f(path, std::ios::binary);
    return readWholeFile(f, out);
}

} // namespace eruption
