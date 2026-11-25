#pragma once

#include <string>

namespace eruption::probecache {

// ONDE FICA A GRADE DE SONDAS COZIDA de um mapa (IrradianceProbes::saveCache/loadCache).
//
// Dois lugares, nesta ordem na leitura:
//   1. ao lado do .glb (<mapa>.glb.probes): e' o que vai dentro de pacotes e APKs
//      (vfs, GameExport, MapBundleBaker, build_apk.sh) e o que o celular le';
//   2. o cache do usuario (<pasta de cache>/<mapa>.glb-<hash do caminho>.probes):
//      onde o PC grava por padrao, para o motor nao escrever dentro dos assets do
//      projeto (pasta do usuario, as vezes sob controle de versao ou somente leitura).
// A validade (conteudo do mapa, caixa da cena, versao) esta no cabecalho do arquivo,
// entao o nome so' precisa ser estavel por mapa: um cache velho e' sobrescrito, nao acumula.
//
// ERUPTION_PROBE_CACHE_DIR=<pasta>: usa esta pasta no lugar da padrao (leitura e escrita).
// Sem ela: $XDG_CACHE_HOME/eruption/probes, senao $HOME/.cache/eruption/probes
// (no Windows, %LOCALAPPDATA%/eruption/probes). No Android nao ha' pasta de usuario:
// userFile() devolve "" e o comportamento e' o de sempre (ao lado do .glb).

enum class WritePolicy { User, Beside };

// "" no Android sem ERUPTION_PROBE_CACHE_DIR, ou sem HOME.
std::string userDir();

// Arquivo do cache de usuario para o .glb `glbPath` ("" sem userDir()).
std::string userFile(const std::string& glbPath);

// Ao lado do .glb.
inline std::string besideFile(const std::string& glbPath) { return glbPath + ".probes"; }

// Onde gravar. `projectChoice` e' a chave "probe_cache" do project.eruption:
// "beside" = ao lado do .glb (o projeto pede), qualquer outra coisa = padrao.
// O padrao e' o cache do usuario; sem pasta de usuario (Android), ao lado do .glb.
// ERUPTION_PROBE_CACHE_DIR sempre vence a chave do projeto.
WritePolicy writePolicy(const std::string& projectChoice);

} // namespace eruption::probecache
