#pragma once
// RELEVO EMPACOTADO (<mapa>.relevo). O chao de um mapa gerado vai em dois
// arquivos que repetem a mesma informacao: a malha "Ground" (.glb, posicao,
// normal, UV e indices de uma grade) e a grade de colisao (.ter, quatro cantos
// por casa). Os dois saem de DUAS grades de altura: a visual, e a de colisao,
// que difere so' onde uma ferramenta fechou o chao para andar (uma fenda que
// se ve' mas por onde se passa). Num mapa de 601x601 sao 30 MB de .glb+.ter
// para 1,4 MB de grade.
//
// packGround guarda a grade visual, as casas em que a colisao difere e o que
// nao e' derivavel (o JSON do .glb, o cabecalho e a cauda do .ter).
// unpackGround devolve os dois arquivos. O empacotamento so' aceita o par se a
// volta reproduzir o .ter byte a byte e o .glb com posicao, UV e indices
// byte a byte e a normal a menos de 1e-5 (ela e' recalculada; o .glb original
// pode ter vindo de outra ferramenta, com outra ordem de soma). Se nao
// reproduzir, o erro diz por que e o mapa continua indo como .glb+.ter.
#include <cstdint>
#include <filesystem>
#include <string>

namespace eruption {

bool packGround(const std::filesystem::path& glb, const std::filesystem::path& ter,
                const std::filesystem::path& out, std::string& error);
bool unpackGround(const std::filesystem::path& relevo, const std::filesystem::path& glbOut,
                  const std::filesystem::path& terOut, std::string& error);

// CARGA A PARTIR DO .relevo. O mapa exportado leva so' o .relevo; a carga o
// desempacota em .cache/eruption/relevo/<mapa>_<chave>/ (uma vez por
// conteudo) e devolve ali o .glb e o .ter. O .glb desempacotado tem a normal
// recalculada, que muda entre maquinas (contracao FMA no ARM): quem tira
// chave dos BYTES do .glb (pacote de textura assada, probes) pergunta aqui se
// o .glb veio de um .relevo e usa os bytes dele, iguais em toda maquina.
bool unpackGroundCached(const std::filesystem::path& relevo, std::filesystem::path& glbOut,
                        std::filesystem::path& terOut, std::string& error);
// O .relevo de que vem o .glb em glbPath (onde o .glb estaria): existe so' o
// .relevo ao lado. Vazio se o .glb existe ou nao ha .relevo.
std::string groundSourceFor(const std::string& glbPath);
// FNV-1a 64 do arquivo inteiro (0 se nao abrir).
uint64_t fileFnv64(const std::filesystem::path& path);

} // namespace eruption
