// eruption-compat-run: carrega plugins pela camada de compatibilidade e roda
// um roteiro curto, sem janela. Serve para testar plugins de terceiros sem
// subir o motor inteiro.
//
//   eruption-compat-run --godot ext.gdextension --classes
//       --novo Summator --chamar add 10 --chamar add 20 --chamar get_total
//       --novo GDExample --cena --quadros 60 --descrever
//
// Cada --novo cria um objeto e o torna o "atual" para --cena, --chamar e
// --descrever. --quadros N roda N quadros de --dt segundos (padrao 1/60).
// Sai com codigo 1 se algum passo falhar.

#include "godot/GodotHost.hpp"
#include "host/Compat.hpp"
#include "host/Host.hpp"
#include "unity/UnityHost.hpp"
#include "unreal/UnrealHost.hpp"

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

using namespace eruption::compat;

int main(int argc, char** argv) {
    std::vector<std::string> a(argv + 1, argv + argc);
    double dt = 1.0 / 60.0;
    uint64_t atual = 0;
    int falhas = 0;
    auto opcao = [](const std::string& s) { return s.rfind("--", 0) == 0; };

    for (size_t i = 0; i < a.size(); ++i) {
        const std::string& op = a[i];
        auto proximo = [&]() -> std::string { return i + 1 < a.size() ? a[++i] : std::string(); };
        if (op == "--plugin") {
            falhas += carregarPlugin(proximo()) ? 0 : 1;
        } else if (op == "--godot") {
            const std::string p = proximo();
            falhas += carregarPlugin(p.find(".gdextension") != std::string::npos ? p : "godot:" + p) ? 0 : 1;
        } else if (op == "--classes") {
            for (const std::string& c : godot::classesRegistradas()) std::printf("classe: %s\n", c.c_str());
        } else if (op == "--novo") {
            const std::string c = proximo();
            atual = godot::instanciar(c);
            if (!atual) ++falhas;
            else std::printf("novo: %s\n", godot::descrever(atual).c_str());
        } else if (op == "--cena") {
            falhas += godot::adicionarNaCena(atual) ? 0 : 1;
        } else if (op == "--chamar") {
            const std::string metodo = proximo();
            std::vector<godot::Valor> args;
            while (i + 1 < a.size() && !opcao(a[i + 1])) args.push_back(godot::Valor::deTexto(a[++i]));
            std::string r;
            if (godot::chamar(atual, metodo, args, &r)) std::printf("%s() -> %s\n", metodo.c_str(), r.c_str());
            else ++falhas;
        } else if (op == "--unity") {
            falhas += carregarPlugin("unity:" + proximo()) ? 0 : 1;
        } else if (op == "--unity-chamar") {
            // --unity-chamar Funcao "v(f)" 1.5
            const std::string nome = proximo();
            const std::string assinatura = proximo();
            std::vector<std::string> vals;
            while (i + 1 < a.size() && !opcao(a[i + 1])) vals.push_back(a[++i]);
            std::string r;
            if (unity::chamarFuncao(nome, assinatura, vals, &r)) std::printf("%s%s -> %s\n", nome.c_str(), assinatura.c_str() + 1, r.c_str());
            else ++falhas;
        } else if (op == "--unity-evento") {
            const int id = std::atoi(proximo().c_str());
            const int n = unity::emitirEventoRender(id);
            std::printf("evento de render %d entregue a %d plugin(s)\n", id, n);
            if (n == 0) ++falhas;
        } else if (op == "--unreal") {
            falhas += carregarPlugin("unreal:" + proximo()) ? 0 : 1;
        } else if (op == "--unreal-funcoes") {
            for (const std::string& f : unreal::funcoes()) std::printf("funcao: %s\n", f.c_str());
        } else if (op == "--unreal-chamar") {
            // --unreal-chamar Classe.Funcao arg1 arg2 ...
            const std::string nome = proximo();
            std::vector<std::string> vals;
            while (i + 1 < a.size() && !opcao(a[i + 1])) vals.push_back(a[++i]);
            std::string r;
            if (unreal::chamarFuncao(nome, vals, &r)) std::printf("%s() -> %s\n", nome.c_str(), r.c_str());
            else ++falhas;
        } else if (op == "--dt") {
            dt = std::atof(proximo().c_str());
        } else if (op == "--quadros") {
            const int n = std::atoi(proximo().c_str());
            for (int q = 0; q < n; ++q) quadro(dt);
            std::printf("quadros: %d (t=%.3fs)\n", n, host().tempo());
        } else if (op == "--descrever") {
            std::printf("objeto: %s\n", godot::descrever(atual).c_str());
        } else if (op == "--liberar") {
            godot::liberar(atual);
            atual = 0;
        } else {
            std::fprintf(stderr, "opcao desconhecida: %s\n", op.c_str());
            return 2;
        }
        std::fflush(stdout);
    }
    encerrar();
    std::printf("falhas: %d\n", falhas);
    return falhas ? 1 : 0;
}
