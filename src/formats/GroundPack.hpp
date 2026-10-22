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
#include <filesystem>
#include <string>

namespace eruption {

bool packGround(const std::filesystem::path& glb, const std::filesystem::path& ter,
                const std::filesystem::path& out, std::string& error);
bool unpackGround(const std::filesystem::path& relevo, const std::filesystem::path& glbOut,
                  const std::filesystem::path& terOut, std::string& error);

} // namespace eruption
