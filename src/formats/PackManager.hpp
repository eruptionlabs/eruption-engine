#pragma once

#include <cstdint>
#include <vector>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <memory>
#include <mutex>

namespace eruption {

struct ResourceSourceInfo {
    std::string path;
    bool isDirectory;
    uint32_t fileCount = 0;
};

namespace ecp { class Reader; } // formats/EcpReader.hpp

// Asset manager: pastas no disco (assets/, assets/external/...) e pacotes
// .ecp (formats/EcpReader.hpp) ao lado delas.
//
// REGRA DE ORDEM: pastas soltas primeiro (a primeira pasta que tem o arquivo
// vence, como sempre), depois os pacotes, que formam UMA sobreposicao na ordem
// em que foram adicionados (o ULTIMO pacote que cita o caminho vence; um
// tombstone esconde o arquivo dos pacotes anteriores). Assim um arquivo solto
// ao lado do projeto vence o pacote sem reempacotar. Um pacote nao cai no
// "mesmo nome em qualquer pasta" das pastas: o nome sem pasta (basename)
// so' e' tentado depois de todos os caminhos exatos, nas pastas e nos pacotes.
class PackManager {
public:
    PackManager();
    ~PackManager();
    PackManager(const PackManager&) = delete;
    PackManager& operator=(const PackManager&) = delete;

    // Um caminho terminado em ".ecp" entra como pacote (addPack); o resto e' pasta.
    void addDirectory(const std::string& dir);
    // Adiciona um pacote .ecp; false (com aviso no log) se nao abrir ou for
    // invalido, e entao nada muda. Chame na carga: nao reabra com threads lendo.
    bool addPack(const std::string& ecpPath);
    void clear();

    std::vector<uint8_t> extract(const std::string& filename) const;
    bool exists(const std::string& filename) const;
    std::vector<std::string> getAllFilenames() const;
    std::vector<std::string> getFilenamesByPrefix(const std::string& prefix) const;

    size_t dirCount() const { return m_dirs.size(); }
    size_t packCount() const { return m_packPaths.size(); }
    size_t sourceCount() const { return m_dirs.size() + m_packPaths.size(); }

    const std::vector<ResourceSourceInfo>& getSources() const { return m_sources; }

    // Thread-safe wrapper
    std::vector<uint8_t> extractSafe(const std::string& filename) const;

private:
    struct DirCache {
        std::string rootPath;
        std::unordered_set<std::string> files;
    };

    mutable std::mutex m_mutex;
    std::vector<DirCache> m_dirs;
    std::vector<ResourceSourceInfo> m_sources;
    // Pacotes na ordem de adicao, abertos juntos num leitor so' (a sobreposicao).
    // shared_ptr: quem le leva uma copia e nao depende do mutex enquanto decodifica.
    std::vector<std::string> m_packPaths;
    std::shared_ptr<ecp::Reader> m_packs;
    std::unordered_map<std::string, std::string> m_packBasenames; // nome sem pasta -> caminho (o primeiro, em ordem)

    bool reopenPacksLocked(const std::string& changed);
    static bool scanBasename(const std::string& root, const std::string& lowerBase,
                             const std::string& filename, std::vector<uint8_t>& out);

    static std::string normalizePath(const std::string& path);
    static bool pathContains(const std::string& haystack, const std::string& needle);
    void scanDirectory(const std::string& dir, DirCache& cache);
};

} // namespace eruption
