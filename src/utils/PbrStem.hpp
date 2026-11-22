#pragma once
// Nome-base (stem) de uma chave de albedo, usado para achar os mapas PBR do
// disco (<stem>_normal.png / <stem>_mrahw.png ao lado do albedo).
// Header so' com inline: os testes de unidade usam sem linkar o carregador.
#include <algorithm>
#include <cctype>
#include <filesystem>
#include <string>

namespace eruption::pbr {

inline bool supportedImageExt(const std::filesystem::path& ext) {
    std::string e = ext.string();
    std::transform(e.begin(), e.end(), e.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return e == ".png" || e == ".jpg" || e == ".jpeg" || e == ".bmp" || e == ".tga";
}

// Chaves de albedo aceitas:
//  - com extensao de imagem: o stem do arquivo ("a/b/wood.png" -> "wood");
//  - sem extensao nenhuma (uri de imagem embutida/sem sufixo, "a/b/wood"):
//    o proprio nome e' o stem. Antes essas chaves eram rejeitadas e o PBR do
//    disco nunca era achado;
//  - imagens embutidas do GLB ("#nome", "#image3"): o nome inteiro, mesmo com
//    pontos ("#modelo.v2").
// Nome com ponto e extensao que NAO e' de imagem ("Material.001") continua
// rejeitado: um nome de material qualquer nao deve casar com mapas cozidos
// de textura sem relacao.
inline bool albedoStemOf(const std::filesystem::path& src, std::string& stem) {
    if (supportedImageExt(src.extension())) {
        stem = src.stem().string();
        return !stem.empty();
    }
    const std::string name = src.filename().string();
    if (name.size() >= 2 && name[0] == '#') { stem = name; return true; }
    if (!name.empty() && name.find('.') == std::string::npos) { stem = name; return true; }
    return false;
}

} // namespace eruption::pbr
