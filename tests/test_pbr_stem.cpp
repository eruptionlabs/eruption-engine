// Teste de unidade: stem de albedo para a busca de PBR do disco.
#include "utils/PbrStem.hpp"
#include <cstdio>

using eruption::pbr::albedoStemOf;
static int g_fail = 0;
static void expect(const char* key, bool ok, const char* stem) {
    std::string s;
    const bool r = albedoStemOf(std::filesystem::path(key), s);
    const bool pass = (r == ok) && (!ok || s == stem);
    if (!pass) { std::printf("FALHOU: '%s' -> %d '%s' (esperado %d '%s')\n", key, r, s.c_str(), ok, stem); g_fail = 1; }
}

int main() {
    expect("a/b/wood.png", true, "wood");
    expect("wood.PNG", true, "wood");
    expect("x/y.tga", true, "y");
    expect("a/b/wood", true, "wood");          // uri sem extensao (o bug)
    expect("wood", true, "wood");
    expect("#image3", true, "#image3");
    expect("#modelo.v2", true, "#modelo.v2");
    expect("#__x_0001.png", true, "#__x_0001");
    expect("Material.001", false, "");          // nome de material: segue rejeitado
    expect("a/b/foo.bin", false, "");
    expect("", false, "");
    expect("dir/", false, "");
    expect("..", false, "");
    // contorno do agente anterior: "<sha12>.png.png" continua achando o stem
    expect("0123456789ab.png.png", true, "0123456789ab.png");
    if (!g_fail) std::printf("test_pbr_stem: OK\n");
    return g_fail;
}
