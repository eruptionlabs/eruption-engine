#pragma once
// PACOTE DE MAPA (.emap) E A CAMADA DE ARQUIVOS POR CIMA DELE.
//
// Abrir um mapa lia centenas de arquivos pequenos: o .glb do mapa, os .glb de
// todos os modelos da cena com os pacotes de textura (<glb>.1024s.etexpack),
// os mapas PBR do disco (.png.etex), os caches de relevo (.cache/eruption/
// relevo), a cena, o .ter/.nav/.env. Num HDD com NTFS via FUSE o que custa e'
// abrir e posicionar a cabeca em cada um: o warp para um mapa com os arquivos
// no cache do sistema levava 1-4 s (3 s num mapa de teste), o mesmo warp com o disco
// frio 20-44 s, e a primeira visita (sem pacotes) 50-150 s.
//
// O "build" do mapa (--bake-map, ou File > Build Map no editor) carrega o
// mapa uma vez com tudo sendo gerado (BC, PBR, relevo, sondas), anota cada
// arquivo que a carga pediu, na ordem em que pediu, e grava tudo num arquivo
// so': <pasta do mapa>/<mapa>.emap. Na carga, o pacote e' montado e lido em
// sequencia (uma leitura so', na ordem da carga); quem pede um arquivo recebe
// a entrada do pacote (descomprimida no fio que pediu, em paralelo) e so' o
// que nao esta' nele vai ao disco.
//
// Formato (little-endian):
//   cabecalho, 64 bytes: "ERUPEMAP", u32 versao, u32 entradas, u64 posicao e
//     u64 tamanho do indice, u64 posicao e u64 tamanho dos dados, u64 hash do
//     indice, u64 reservado.
//   indice: u32 raizes, cada uma u16 + texto ("@project|../../..": raiz
//     simbolica e o caminho dela relativo a' pasta do .emap, para ferramentas);
//     depois cada entrada: u16 + chave (caminho relativo a' raiz), u8 raiz,
//     u8 tipo, u8 codec, u64 posicao, u64 tamanho guardado, u64 tamanho cru,
//     u64 hash do conteudo cru, u64 tamanho e i64 data do arquivo de origem.
//   dados: as entradas na ordem em que a carga as leu.
//
// O pacote e' valido por construcao: as entradas foram lidas juntas, depois
// de a carga validar cada cache contra a sua origem. Quem valida cache por
// tamanho/data/conteudo da origem (pacote de texturas, .etex, sondas) recebe
// do pacote o tamanho e a data que a origem tinha no build.
//
// Origem mudou depois do build (o editor gravou a cena, um .glb foi trocado):
// na montagem o tamanho+data de cada arquivo que existe no disco e' comparado
// com o do indice; o que mudou sai do pacote e volta ao caminho antigo (disco),
// com um aviso para reconstruir o mapa. Nada e' reconstruido com o jogo rodando.

#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <system_error>
#include <vector>

namespace eruption {
namespace vfs {

// ---- leitura (pacote primeiro, disco depois) -----------------------------

bool read(const std::string& path, std::vector<uint8_t>& out);
bool read(const std::filesystem::path& path, std::vector<uint8_t>& out);
bool readText(const std::string& path, std::string& out);
bool exists(const std::string& path);
bool exists(const std::filesystem::path& path);
// Tamanho e data (marca opaca: so' serve para comparar com outra marca do
// vfs) do arquivo. Do pacote: os do arquivo de origem no momento do build.
bool stat(const std::string& path, uint64_t& size, int64_t& mtime);
uint64_t fileSize(const std::string& path, std::error_code& ec);
// A entrada vem do pacote (montado, nao vencida pela origem)?
bool inBundle(const std::string& path);
// O arquivo de verdade no disco, mesmo com a entrada no pacote: para o cache
// cuja copia do pacote nao bateu com a validacao (e anotado no build).
bool readDisk(const std::string& path, std::vector<uint8_t>& out);

// ---- pacotes -----------------------------------------------------------

// Monta o pacote do mapa (<mapa>.emap ao lado do .glb dele) se existir. Ficam
// montados no maximo dois (o atual e o anterior, para a volta). Devolve se
// algum pacote ficou montado para o mapa.
bool mountMap(const std::string& mapStem);
void unmountAll();
// Liga/desliga o uso de pacotes (o build le' sempre do disco).
void setBundlesEnabled(bool enabled);
bool anyMounted();
// Caminho onde o build grava o pacote do mapa.
std::filesystem::path bundlePathFor(const std::string& mapStem);

// Contagem desde a ultima montagem: o que veio do pacote e o que foi ao disco.
struct Counters {
    uint64_t bundleReads = 0, bundleBytes = 0, bundleProbes = 0;
    uint64_t diskReads = 0, diskBytes = 0, diskProbes = 0;
};
Counters counters();
void logCounters(const char* label);

// ---- build -------------------------------------------------------------

// Anota cada caminho pedido ao vfs (ordem da primeira vez) ate endRecord.
void beginRecord();
bool recording();
// Quantos caminhos foram anotados ate agora (o build espera parar de crescer).
size_t recordedCount();
struct Recorded {
    std::string path; // absoluto, normalizado
    uint8_t access = 0; // 1 existe?, 2 tamanho/data, 3 conteudo
};
std::vector<Recorded> endRecord();
// Acrescenta um arquivo ao que o build anotou (gerado fora do vfs).
void recordExtra(const std::string& path);
// O caminho como o vfs o anota (absoluto, normalizado).
std::string key(const std::string& path);

struct WriteReport {
    size_t entries = 0, data = 0, meta = 0, absent = 0;
    uint64_t rawBytes = 0, storedBytes = 0, sourceBytes = 0;
    double ms = 0.0;
    std::string error;
};
// Muda o conteudo de uma entrada antes de entrar no pacote (o .glb sem as
// imagens que o pacote de texturas ja' traz). Chamada em paralelo, com a
// chave do arquivo; o tamanho e a data da origem no indice continuam os do disco.
using Transform = std::function<void(const std::string& key, std::vector<uint8_t>& bytes)>;
// Grava o pacote com os arquivos anotados, no estado em que estao agora no
// disco. `compress`: zstd por entrada (cru se o build nao tem zstd ou se a
// compressao nao ganha).
WriteReport writeBundle(const std::filesystem::path& emapPath, const std::vector<Recorded>& files, bool compress,
                        int level, const Transform& transform = {});

} // namespace vfs
} // namespace eruption
