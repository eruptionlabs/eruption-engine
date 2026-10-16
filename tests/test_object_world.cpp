// Testes do ObjectWorld (src/scene/ObjectWorld): identidade com geração,
// hierarquia, transforms com propagação de sujos, nomes, etiquetas, ligado e
// desligado, e um estresse contra o cálculo ingênuo. Sem GPU nem motor:
//   tools/run_tests.sh  (ou o alvo eruption-object-tests com ERUPTION_BUILD_TESTS)
#include "scene/ObjectWorld.hpp"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <random>

using namespace eruption;

namespace {

int g_failures = 0;

void check(bool ok, const char* what, int line) {
    if (ok) return;
    std::printf("  FALHOU (linha %d): %s\n", line, what);
    ++g_failures;
}
#define CHECK(x) check((x), #x, __LINE__)

bool near(const Mat4& a, const Mat4& b, float eps = 1e-3f) {
    for (int c = 0; c < 4; ++c)
        for (int r = 0; r < 4; ++r)
            if (std::abs(a[c][r] - b[c][r]) > eps * std::max(1.0f, std::abs(b[c][r]))) return false;
    return true;
}

ObjectWorld::Transform at(float x, float y, float z) {
    ObjectWorld::Transform t;
    t.position = Vec3(x, y, z);
    return t;
}

void testIdentity() {
    std::printf("identidade e geração\n");
    ObjectWorld w;
    const ObjectId a = w.create("a");
    CHECK(w.alive(a));
    CHECK(w.count() == 1);
    CHECK(w.destroy(a) == 1);
    CHECK(!w.alive(a));
    const ObjectId b = w.create("b");
    CHECK(b.index == a.index);            // vaga reaproveitada
    CHECK(b.generation != a.generation);  // mas outra identidade
    CHECK(!w.alive(a));
    CHECK(w.name(a).empty());
    CHECK(w.idAt(b.index) == b);
    CHECK(!w.alive(ObjectId{}));
    CHECK(!w.alive(ObjectId{999, 1}));
    CHECK(w.destroy(a) == 0); // id velho não destrói o novo
    CHECK(w.alive(b));
}

void testHierarchy() {
    std::printf("hierarquia e transform\n");
    ObjectWorld w;
    const ObjectId root = w.create("root", {}, at(10, 0, 0));
    const ObjectId child = w.create("child", root, at(0, 5, 0));
    const ObjectId grand = w.create("grand", child, at(0, 0, 2));
    // Antes do quadro: a leitura já vem certa.
    CHECK(glm::length(w.worldPosition(grand) - Vec3(10, 5, 2)) < 1e-4f);
    const auto& changed = w.updateTransforms();
    CHECK(changed.size() == 3);
    CHECK(glm::length(w.worldPosition(grand) - Vec3(10, 5, 2)) < 1e-4f);
    // Mexer no pai move os filhos; só a subárvore muda.
    const ObjectId other = w.create("other");
    w.updateTransforms();
    w.setLocalPosition(root, Vec3(20, 0, 0));
    CHECK(glm::length(w.worldPosition(grand) - Vec3(20, 5, 2)) < 1e-4f);
    const auto& moved = w.updateTransforms();
    CHECK(moved.size() == 3);
    bool otherMoved = false;
    for (uint32_t i : moved) otherMoved = otherMoved || i == other.index;
    CHECK(!otherMoved);
    // Nada mexeu: nada muda.
    CHECK(w.updateTransforms().empty());
    // Giro do pai leva o filho junto.
    w.setLocalRotation(root, glm::angleAxis(glm::radians(90.0f), Vec3(0, 1, 0)));
    w.updateTransforms();
    const Vec3 g = w.worldPosition(grand);
    CHECK(std::abs(g.x - 22.0f) < 1e-3f && std::abs(g.y - 5.0f) < 1e-3f && std::abs(g.z) < 1e-3f);
    // Ordem dos filhos = ordem de criação.
    const ObjectId c2 = w.create("c2", root);
    const auto kids = w.children(root);
    CHECK(kids.size() == 2 && kids[0] == child && kids[1] == c2);
    CHECK(w.parent(grand) == child);
    CHECK(w.isAncestor(root, grand));
    CHECK(!w.isAncestor(grand, root));
}

void testReparent() {
    std::printf("trocar de pai\n");
    ObjectWorld w;
    ObjectWorld::Transform pt = at(100, 0, 0);
    pt.rotation = glm::angleAxis(glm::radians(30.0f), Vec3(0, 1, 0));
    pt.scale = Vec3(2.0f);
    const ObjectId a = w.create("a", {}, pt);
    const ObjectId b = w.create("b", {}, at(-50, 3, 7));
    const ObjectId c = w.create("c", b, at(1, 2, 3));
    w.updateTransforms();
    const Mat4 before = w.world(c);
    CHECK(w.setParent(c, a, true));
    CHECK(near(w.world(c), before));
    w.updateTransforms();
    CHECK(near(w.world(c), before));
    CHECK(w.parent(c) == a);
    CHECK(w.children(b).empty());
    // Sem manter o mundo: o local vale relativo ao novo pai.
    CHECK(w.setParent(c, b, false));
    w.updateTransforms();
    CHECK(near(w.world(c), w.world(b) * w.local(c).matrix()));
    // Ciclo recusado (b dentro do próprio filho), e o objeto não muda de lugar.
    CHECK(!w.setParent(b, c));
    CHECK(!w.setParent(b, b));
    CHECK(w.parent(b) == ObjectId{});
    // Para a raiz.
    const Mat4 cw = w.world(c);
    CHECK(w.setParent(c, {}, true));
    CHECK(near(w.world(c), cw));
}

void testDestroySubtree() {
    std::printf("destruir subárvore\n");
    ObjectWorld w;
    const ObjectId root = w.create("root");
    const ObjectId a = w.create("a", root);
    const ObjectId b = w.create("b", a);
    const ObjectId keep = w.create("keep");
    w.takeDestroyed();
    CHECK(w.destroy(a) == 2);
    CHECK(!w.alive(a) && !w.alive(b) && w.alive(root) && w.alive(keep));
    CHECK(w.children(root).empty());
    const auto gone = w.takeDestroyed();
    CHECK(gone.size() == 2);
    CHECK(w.takeDestroyed().empty());
    CHECK(w.count() == 2);
    // Destruir e criar no mesmo quadro: a vaga nova aparece uma vez só.
    const ObjectId n = w.create("n");
    w.setLocalPosition(n, Vec3(1, 0, 0));
    const auto& changed = w.updateTransforms();
    size_t seen = 0;
    for (uint32_t i : changed) seen += i == n.index;
    CHECK(seen == 1);
}

void testNamesTagsActive() {
    std::printf("nomes, etiquetas e ligado\n");
    ObjectWorld w;
    const ObjectId x1 = w.create("porta");
    const ObjectId x2 = w.create("porta");
    CHECK(w.find("porta") == x1);
    CHECK(w.findAll("porta").size() == 2);
    w.destroy(x1);
    CHECK(w.find("porta") == x2);
    w.rename(x2, "saida");
    CHECK(!w.find("porta").valid());
    CHECK(w.find("saida") == x2);

    w.addTag(x2, "interativo");
    w.addTag(x2, "interativo");
    CHECK(w.tags(x2).size() == 1);
    CHECK(w.hasTag(x2, "interativo"));
    CHECK(!w.hasTag(x2, "outro"));
    CHECK(w.findByTag("interativo").size() == 1);
    w.removeTag(x2, "interativo");
    CHECK(!w.hasTag(x2, "interativo"));

    const ObjectId p = w.create("p");
    const ObjectId c = w.create("c", p);
    w.updateTransforms();
    w.setActive(p, false);
    CHECK(!w.activeInHierarchy(c));
    CHECK(w.active(c));
    CHECK(w.updateTransforms().size() == 2); // os dois mudam de estado efetivo
    w.setActive(p, true);
    CHECK(w.activeInHierarchy(c));
}

void testClearKeepsGeneration() {
    std::printf("clear não faz id velho valer\n");
    ObjectWorld w;
    const ObjectId a = w.create("a");
    w.clear();
    CHECK(!w.alive(a));
    CHECK(w.takeDestroyed().size() == 1);
    const ObjectId b = w.create("b");
    CHECK(b.index == a.index && b.generation != a.generation);
    CHECK(!w.alive(a));
    CHECK(w.count() == 1);
    // settle: nasce no lugar e não aparece como mudado.
    const ObjectId c = w.create("c", b, at(1, 2, 3));
    w.updateTransforms();
    const ObjectId d = w.create("d", c, at(1, 0, 0));
    w.settle(d);
    CHECK(w.updateTransforms().empty());
    CHECK(glm::length(w.worldPosition(d) - Vec3(2, 2, 3)) < 1e-4f);
    w.setLocalPosition(d, Vec3(0, 0, 0));
    CHECK(w.updateTransforms().size() == 1);
}

// Estresse: árvore aleatória grande, mexidas, trocas de pai e destruições,
// conferida contra o produto ingênuo das matrizes locais até a raiz.
void testStress() {
    std::printf("estresse\n");
    ObjectWorld w;
    std::mt19937 rng(1234);
    std::uniform_real_distribution<float> u(-100.0f, 100.0f);
    std::vector<ObjectId> ids;
    const int n = 100000;
    ids.reserve(n);
    for (int i = 0; i < n; ++i) {
        const ObjectId parent = ids.empty() || rng() % 8 == 0 ? ObjectId{} : ids[rng() % ids.size()];
        ObjectWorld::Transform t = at(u(rng), u(rng), u(rng));
        t.rotation = glm::angleAxis(u(rng) * 0.03f, glm::normalize(Vec3(u(rng), u(rng), u(rng)) + Vec3(0.01f)));
        t.scale = Vec3(1.0f + std::abs(u(rng)) * 0.002f);
        ids.push_back(w.create("o", parent, t));
    }
    auto t0 = std::chrono::steady_clock::now();
    w.updateTransforms();
    const double fullMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();

    for (int round = 0; round < 20; ++round) {
        for (int k = 0; k < 1000; ++k) {
            const ObjectId id = ids[rng() % ids.size()];
            if (!w.alive(id)) continue;
            switch (rng() % 10) {
            case 0: w.setParent(id, ids[rng() % ids.size()], rng() % 2 == 0); break;
            case 1: w.destroy(id); break;
            default: w.setLocalPosition(id, Vec3(u(rng), u(rng), u(rng))); break;
            }
        }
        w.updateTransforms();
    }
    t0 = std::chrono::steady_clock::now();
    for (int k = 0; k < 1000; ++k) {
        const ObjectId id = ids[rng() % ids.size()];
        if (w.alive(id)) w.setLocalPosition(id, Vec3(u(rng), u(rng), u(rng)));
    }
    const size_t moved = w.updateTransforms().size();
    const double partialMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();

    int wrong = 0, checked = 0;
    for (const ObjectId id : ids) {
        if (!w.alive(id) || ++checked > 20000) continue;
        Mat4 m(1.0f);
        std::vector<ObjectId> chain;
        for (ObjectId p = id; p.valid(); p = w.parent(p)) chain.push_back(p);
        for (size_t k = chain.size(); k-- > 0;) m = m * w.local(chain[k]).matrix();
        if (!near(w.world(id), m, 2e-3f)) ++wrong;
    }
    CHECK(wrong == 0);
    std::printf("  %zu vivos; quadro inteiro sujo %.2f ms; 1000 mexidos (%zu mudaram) %.2f ms; %d errados de %d\n",
                w.count(), fullMs, moved, partialMs, wrong, std::min(checked, 20000));
}

void testDeep() {
    std::printf("hierarquia funda\n");
    ObjectWorld w;
    ObjectId last;
    for (int i = 0; i < 200000; ++i) last = w.create("d", last, at(1, 0, 0));
    CHECK(std::abs(w.worldPosition(last).x - 200000.0f) < 1.0f);
    w.updateTransforms();
    CHECK(std::abs(w.worldPosition(last).x - 200000.0f) < 1.0f);
    const ObjectId first = w.find("d");
    CHECK(w.destroy(first) == 200000);
    CHECK(w.count() == 0);
}

} // namespace

int main() {
    testIdentity();
    testHierarchy();
    testReparent();
    testDestroySubtree();
    testNamesTagsActive();
    testClearKeepsGeneration();
    testStress();
    testDeep();
    if (g_failures) {
        std::printf("ObjectWorld: %d falha(s)\n", g_failures);
        return 1;
    }
    std::printf("ObjectWorld: tudo certo\n");
    return 0;
}
