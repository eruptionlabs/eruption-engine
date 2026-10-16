#pragma once

#include "scene/ObjectWorld.hpp"
#include "scene/SceneEdits.hpp"

#include <cstdint>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace eruption {

class Engine;
class ModelRenderer;

// Os objetos da cena e o que cada um é. Tudo que o mapa trouxe vira objeto:
// cada instância de modelo e cada personagem. Nome com barra ("casa/porta",
// os nós de um .glb acrescentado) fica filho do objeto "casa".
//
// As tabelas de sempre continuam sendo de quem desenha (ModelRenderer,
// personagens do Engine); o objeto diz ONDE e SE aparece. sync(), uma vez
// por quadro antes do desenho:
//   1. puxa o que foi mexido por fora (editor, scripts antigos, jogo salvo,
//      terreno) para o objeto - nada do motor precisou mudar de caminho;
//   2. recalcula a hierarquia;
//   3. empurra para as tabelas o que mudou no objeto (o pai levou o filho,
//      o script moveu o objeto, desligou a subárvore).
// Objeto parado não custa nada: só o que mudou passa por aqui.
class SceneObjects {
public:
    ObjectWorld& world() { return m_world; }
    const ObjectWorld& world() const { return m_world; }

    // Mapa novo: esquece tudo (os objetos voltam no próximo sync).
    void reset();
    // A cena vai ser lida de novo sobre o mesmo mapa: os objetos recomeçam,
    // mas o estoque de spawn (instâncias que continuam na lista) fica.
    void resetObjects();
    void sync(Engine& engine);
    // Garante objeto para tudo que já existe (antes do primeiro sync do mapa,
    // um script em on.start já acha os modelos).
    void refresh(Engine& engine);

    // Destrói o objeto e a subárvore já soltando os componentes: as
    // instâncias param de desenhar e ficam prontas para o próximo spawn igual
    // no mesmo quadro. (Destruir direto no world() solta no próximo sync.)
    size_t destroy(Engine& engine, ObjectId id);
    // Objetos que o jogo fez (spawn, create, filhos de prefab): todos somem no
    // Stop e antes de carregar um jogo salvo. Os do mapa que estavam dentro
    // deles voltam para o pai de sempre, no lugar onde estão.
    void markMade(ObjectId id);
    size_t destroyMade(Engine& engine);
    // Objeto de volta para modelos e personagens do mapa que ficaram sem (o
    // jogo destruiu); no Stop e ao carregar um jogo salvo.
    void rebindOrphans(Engine& engine);

    // Cria um objeto com modelo: um .glb ("arquivo.glb#Nó" pega só um nó) ou
    // um .prefab.luau. Todos os nós ficam no mesmo objeto. Reaproveita as
    // instâncias de um objeto igual já destruído: criar e destruir muito (tiro,
    // destroço) não faz a lista de instâncias crescer.
    // Prefab composto (children, tags, scripts) monta a árvore inteira; os
    // scripts de objeto que ele pede vão para `scripts` (quem chama liga).
    using ObjectScript = std::pair<ObjectId, std::string>;
    ObjectId spawn(Engine& engine, const std::string& source, const std::string& name, ObjectId parent,
                   const ObjectWorld::Transform& local, std::string* error = nullptr,
                   std::vector<ObjectScript>* scripts = nullptr);
    // Prefab posto na cena: filhos, etiquetas e scripts dele sob a raiz (o
    // modelo da raiz o SceneEdits já pôs). `placement` = onde a cena pôs o prefab.
    void buildPrefabExtras(Engine& engine, ObjectId root, const Mat4& placement, const std::string& prefabPath);
    // Nome de um objeto criado sem nome: o nó ("arquivo.glb#Nó") ou o arquivo.
    static std::string defaultName(const std::string& source);

    // Componente de modelo: instâncias do ModelRenderer que o objeto desenha.
    ObjectId objectOfInstance(uint32_t instance) const;
    std::vector<uint32_t> instancesOf(ObjectId id) const;
    // O modelo do objeto aparece? Desligar o objeto (world().setActive)
    // também esconde a subárvore; isto é só a parte dele.
    void setModelVisible(ObjectId id, bool visible);
    // Componente de personagem (sprite que anda e fala): índice em Engine::characters().
    ObjectId objectOfCharacter(size_t character) const;
    int characterOf(ObjectId id) const;

    // Luz, área de gatilho e emissor de som: objetos com posição (o pai leva a
    // lanterna junto). Desligar o objeto apaga a luz; destruir tira a área e
    // o som de cena (voltam no Stop).
    enum class PointKind : uint8_t { Light, Area, Sound };
    // Tipo e índice (na lista de luzes, áreas ou sons) do objeto, ou falso.
    bool pointOf(ObjectId id, PointKind& kind, size_t& index) const;
    ObjectId objectOfPoint(PointKind kind, size_t index) const;

    // Filho direto com esse nome (o mais antigo; "nome#2" = o terceiro), ou nenhum.
    ObjectId childNamed(ObjectId parent, const std::string& name) const;
    // "casa/porta": o primeiro trecho na raiz (ou, se não houver, em qualquer
    // lugar), os outros como filhos. Nome repetido: "inimigo#1".
    ObjectId findPath(const std::string& path) const;

    size_t boundInstances() const { return m_instanceOwner.size(); }
    // De onde veio o modelo do objeto (o que se passou ao spawn), ou vazio.
    std::string sourceOf(ObjectId id) const;
    // "casa/porta": nomes do topo até o objeto.
    std::string pathOf(ObjectId id) const;
    // Nome usado no arquivo de cena: o da instância (modelos, "casa/porta"),
    // senão o caminho do objeto (grupo, personagem, objeto vazio).
    std::string sceneName(ObjectId id) const;
    ObjectId findSceneName(const std::string& name) const;
    // Pai que o objeto ganha sozinho ("casa" de "casa/porta"); outro pai é
    // escolha do autor e vai para o arquivo de cena.
    ObjectId naturalParent(ObjectId id) const;

    // O que o arquivo de cena declara sobre um objeto (pai, etiquetas,
    // scripts). É isto que o editor grava - nunca o que um script mudou
    // durante o jogo - e é para isto que os objetos voltam no Stop.
    struct Authored {
        std::string name, parent;
        std::vector<std::string> tags, scripts;
    };
    // Declara (troca a declaração anterior do mesmo nome) e aplica já.
    void author(Engine& engine, const Authored& a);
    const std::vector<Authored>& authored() const { return m_authored; }
    // Volta ao declarado: tira etiquetas e pais postos pelo jogo e refaz os da cena.
    void restoreAuthored(Engine& engine);
    // Esquece as declarações (o arquivo de cena vai ser lido de novo).
    void clearAuthored(Engine& engine);
    // Scripts de objeto declarados, ligados a cada começo de jogo.
    std::vector<std::pair<ObjectId, std::string>> sceneScripts() const;

private:
    static constexpr uint32_t kNone = 0xFFFFFFFFu;
    // Instâncias de um objeto e a matriz de cada uma relativa a ele:
    // instância = mundo do objeto * local. `visible` é o liga/desliga da
    // própria instância (models.hide); ela só desenha se o objeto também estiver ligado.
    struct ModelPart {
        ObjectId owner;
        std::string source; // de onde veio (spawn): a chave do reaproveitamento
        std::vector<uint32_t> instances;
        std::vector<Mat4> local;
        std::vector<uint8_t> visible;
        Mat4 pushedWorld{1.0f}; // mundo do dono na última vez que as instâncias foram postas/lidas
        std::vector<Mat4> placed;  // por instância: o lugar que o objeto pôs (ou leu) por último
    };
    struct InstanceSlot {
        uint32_t part = kNone;
        uint32_t slot = 0;
    };
    // Componente por vaga de objeto, com a geração do dono: a vaga é
    // reaproveitada por objetos novos, e o registro velho não pode valer
    // para quem chegou depois.
    struct OwnerSlot {
        uint32_t generation = 0;
        uint32_t value = kNone;
    };
    // Instâncias de objetos destruídos, prontas para o próximo spawn igual.
    struct Pooled {
        std::vector<uint32_t> instances;
        std::vector<Mat4> local;
    };

    ObjectId spawnTree(Engine& engine, const std::string& source, const std::string& name, ObjectId parent,
                       const ObjectWorld::Transform& local, std::string* error, std::vector<ObjectScript>* scripts,
                       std::vector<std::string>& expanding);
    bool isMapObject(ObjectId id) const;
    void rescueMapObjects(ObjectId root);
    ObjectId spawnModel(Engine& engine, const std::string& source, const std::string& name, ObjectId parent,
                        const ObjectWorld::Transform& local, const Prefab* prefab, std::string* error);
    const Prefab* prefabFor(const std::string& path, std::string* error);
    void resetTable(bool keepPool);
    void placeRootAt(ObjectId root, const Mat4& placement);
    void relocalize(ObjectId id);
    void markSceneBuilt(ObjectId id, ObjectId parent);
    // Liga a marca "montando a cena" no escopo; desliga na saída, por qualquer caminho.
    struct SceneBuildGuard {
        bool& flag;
        explicit SceneBuildGuard(bool& f) : flag(f) { flag = true; }
        ~SceneBuildGuard() { flag = false; }
        SceneBuildGuard(const SceneBuildGuard&) = delete;
        SceneBuildGuard& operator=(const SceneBuildGuard&) = delete;
    };
    void bindNewInstances(ModelRenderer& mr);
    void bindInstance(ModelRenderer& mr, uint32_t instance);
    void bindNewCharacters(Engine& engine);
    void bindCharacter(Engine& engine, size_t character);
    struct PointSlot {
        ObjectId object;
        std::string name;    // nome na cena (o da área/som; "light_3" da luz)
        Vec3 last{0.0f};
        uint8_t hidden = 0;  // luz apagada porque o objeto foi desligado
        uint8_t wasOn = 0;   // como a luz estava antes de apagar
        uint8_t removed = 0; // área/som tirado porque o objeto foi destruído
    };
    std::vector<PointSlot>& points(PointKind kind);
    void bindPoints(Engine& engine);
    void bindPoint(Engine& engine, PointKind kind, size_t index);
    void pullPoints(Engine& engine);
    void pushPoint(Engine& engine, ObjectId id, bool pulled);
    void enforceHiddenLights(Engine& engine);
    void releasePoint(Engine& engine, uint32_t code);
    void restorePoints(Engine& engine);
    void renamedInPlace(Engine& engine, PointKind kind, size_t index);
    void renameAuthored(const std::string& from, const std::string& to);
    static uint32_t pointCode(PointKind kind, size_t index) { return (uint32_t(kind) << 28) | uint32_t(index); }
    ObjectId groupFor(const std::string& instanceName, const Mat4& firstTransform);
    void pullTouchedInstances(ModelRenderer& mr);
    void pullCharacters(Engine& engine);
    void pushChanged(Engine& engine, const std::vector<uint32_t>& changed);
    void releaseDestroyed(Engine& engine);
    void releasePart(ModelRenderer& mr, uint32_t p);
    void releaseCharacter(Engine& engine, uint32_t c, ObjectId id);
    void destroyOwn(ObjectId id);
    uint32_t partOf(ObjectId id) const;
    ObjectId groupOfPrefix(const std::string& name) const;
    std::vector<ObjectId> namesakes(ObjectId parent, const std::string& name) const;
    std::string ownName(ObjectId id) const;
    uint32_t attachPart(ModelRenderer& mr, ObjectId owner, const std::string& source,
                        const std::vector<uint32_t>& instances, const std::vector<Mat4>& local);
    void markPulled(uint32_t objectIndex);
    static uint32_t lookup(const std::vector<OwnerSlot>& table, ObjectId id);
    static void store(std::vector<OwnerSlot>& table, ObjectId id, uint32_t value);

    ObjectWorld m_world;
    std::vector<ModelPart> m_parts;
    std::vector<uint32_t> m_freeParts;
    std::vector<OwnerSlot> m_partOfObject;      // por vaga de objeto
    std::vector<InstanceSlot> m_instanceOwner;  // por instância do ModelRenderer
    std::vector<ObjectId> m_characterObject;    // por personagem
    std::vector<OwnerSlot> m_characterOfObject; // por vaga de objeto
    std::vector<Vec3> m_characterLast;          // posição que o objeto conhece
    std::vector<uint8_t> m_characterHidden;     // escondido porque o objeto foi desligado
    std::vector<uint8_t> m_characterReleased;   // o jogo destruiu o objeto (volta no Stop)
    std::vector<std::string> m_characterNameSeen; // nome do personagem que o objeto conhece
    std::vector<PointSlot> m_lightSlots, m_areaSlots, m_soundSlots; // por luz/área/som
    std::vector<OwnerSlot> m_pointOfObject;     // por vaga de objeto: pointCode
    std::vector<uint8_t> m_pooledInstance;      // por instância: está no estoque de spawn
    std::vector<uint8_t> m_pulled;              // por vaga: mudou por fora neste quadro
    std::vector<uint32_t> m_pulledList;
    std::vector<uint32_t> m_touched;
    std::unordered_map<std::string, ObjectId> m_groups; // "casa" de "casa/porta"
    std::unordered_map<uint64_t, std::string> m_groupPath; // o contrário (chave: groupKey)
    static uint64_t groupKey(ObjectId id) { return (uint64_t(id.index) << 32) | id.generation; }
    std::unordered_map<std::string, std::vector<Pooled>> m_pool;
    std::unordered_map<std::string, Prefab> m_prefabs;
    std::unordered_map<uint64_t, std::string> m_treeSource; // raiz de prefab -> caminho
    std::vector<ObjectId> m_made;                           // feitos pelo jogo
    std::vector<ObjectScript> m_prefabScripts;              // scripts de prefab posto na cena
    std::vector<std::pair<ObjectId, std::string>> m_prefabTags;  // etiquetas de prefab posto na cena
    std::unordered_map<uint64_t, ObjectId> m_sceneParent;   // filho de prefab da cena -> pai (chave: groupKey)
    std::unordered_map<uint32_t, ObjectId> m_sceneInstanceParent; // instância desse filho -> pai
    bool m_buildingScene = false;                           // montando prefab da cena (não é "feito")
    size_t m_madeLimit = 1024;                              // tamanho que dispara a limpeza dos mortos
    std::vector<Authored> m_authored;
    void applyAuthored(const Authored& a);
    ModelRenderer* m_models = nullptr; // nomes e lugares das instâncias
    uint32_t m_instanceEpoch = 0;
};

} // namespace eruption
