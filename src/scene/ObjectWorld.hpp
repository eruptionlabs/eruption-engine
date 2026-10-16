#pragma once

#include "math/Types.hpp"

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace eruption {

// Identidade de um objeto da cena: posição na tabela + geração. Destruir o
// objeto avança a geração da posição, então um id guardado (num script, numa
// regra) passa a dizer "não existe" em vez de apontar para quem ocupou a vaga.
struct ObjectId {
    uint32_t index = 0;
    uint32_t generation = 0; // 0 = nenhum objeto
    bool valid() const { return generation != 0; }
    bool operator==(const ObjectId& o) const { return index == o.index && generation == o.generation; }
    bool operator!=(const ObjectId& o) const { return !(*this == o); }
};

// Tabela de objetos da cena: nome, hierarquia (pai e filhos), transform local
// e de mundo, ligado/desligado e etiquetas. Não sabe desenhar nem tocar som:
// o que o objeto É (modelo, sprite, luz, som, área, colisor, script) mora em
// tabelas de componente que guardam o ObjectId do dono.
//
// Dados em colunas (uma por campo), não um struct por objeto: a propagação de
// transform só toca as colunas de hierarquia e matriz, e quem desenha continua
// lendo as próprias tabelas compactas. Mexer num objeto só o marca como sujo;
// updateTransforms() recalcula os sujos e os filhos deles, uma vez por quadro,
// e devolve quem mudou para os componentes copiarem a matriz.
class ObjectWorld {
public:
    struct Transform {
        Vec3 position{0.0f};
        Quat rotation{1.0f, 0.0f, 0.0f, 0.0f};
        Vec3 scale{1.0f};
        Mat4 matrix() const;
        // Posição, giro e escala de uma matriz (cisalhamento se perde; escala
        // negativa fica no X).
        static Transform fromMatrix(const Mat4& m);
    };

    // Cria o objeto (filho de `parent`, se vivo) com o transform local dado.
    ObjectId create(const std::string& name, ObjectId parent, const Transform& local);
    ObjectId create(const std::string& name, ObjectId parent = {});
    // Destrói o objeto e todos os descendentes. Devolve quantos foram; os ids
    // vão para destroyedSinceLastTake(), para os componentes soltarem os seus.
    size_t destroy(ObjectId id);
    bool alive(ObjectId id) const;
    // Id vivo da posição `index` (o que os componentes guardam como índice).
    ObjectId idAt(uint32_t index) const;

    size_t count() const { return m_live; }
    uint32_t capacity() const { return static_cast<uint32_t>(m_generation.size()); }

    // Nome. Vários objetos podem ter o mesmo nome; find devolve o mais antigo vivo.
    const std::string& name(ObjectId id) const;
    void rename(ObjectId id, const std::string& name);
    ObjectId find(const std::string& name) const;
    std::vector<ObjectId> findAll(const std::string& name) const;

    // Hierarquia. setParent recusa ciclo (pai dentro do próprio filho).
    // keepWorld = o objeto fica onde está no mundo (o local é recalculado;
    // pai com escala desigual e giro junto vira cisalhamento, que se perde).
    ObjectId parent(ObjectId id) const;
    bool setParent(ObjectId id, ObjectId parent, bool keepWorld = true);
    std::vector<ObjectId> children(ObjectId id) const;
    ObjectId firstChild(ObjectId id) const;
    ObjectId nextSibling(ObjectId id) const;
    bool isAncestor(ObjectId ancestor, ObjectId id) const;

    // Transform local (relativo ao pai).
    const Transform& local(ObjectId id) const;
    void setLocal(ObjectId id, const Transform& t);
    void setLocalPosition(ObjectId id, const Vec3& p);
    void setLocalRotation(ObjectId id, const Quat& r);
    void setLocalScale(ObjectId id, const Vec3& s);
    // Mundo. A leitura recalcula a cadeia do objeto se algo nela está sujo
    // (vale logo depois de mexer, sem esperar o quadro).
    Mat4 world(ObjectId id) const;
    Vec3 worldPosition(ObjectId id) const;
    void setWorldPosition(ObjectId id, const Vec3& p);
    void setWorld(ObjectId id, const Mat4& m);

    // Ligado/desligado. Desligar o pai desliga a subárvore (activeInHierarchy).
    void setActive(ObjectId id, bool on);
    bool active(ObjectId id) const;
    bool activeInHierarchy(ObjectId id) const;

    // Etiquetas (nomes livres do jogo; o motor não dá sentido a nenhuma).
    void addTag(ObjectId id, const std::string& tag);
    void removeTag(ObjectId id, const std::string& tag);
    bool hasTag(ObjectId id, const std::string& tag) const;
    std::vector<std::string> tags(ObjectId id) const;
    std::vector<ObjectId> findByTag(const std::string& tag) const;
    // Todos os objetos vivos com alguma etiqueta.
    std::vector<ObjectId> tagged() const;
    // Todos os vivos.
    std::vector<ObjectId> all() const;

    // Recém-criado que já está onde deve (espelho de algo que existe):
    // calcula o mundo agora e não o anuncia como mudado.
    void settle(ObjectId id);

    // Põe o objeto na lista de mudados sem mexer no transform (um componente
    // dele mudou e precisa ser reaplicado).
    void touch(ObjectId id);

    // Recalcula as matrizes de mundo sujas. Devolve as posições (índices) cujo
    // mundo ou estado ligado mudou desde a última chamada.
    const std::vector<uint32_t>& updateTransforms();
    // Ids destruídos desde a última chamada (e esvazia a lista).
    std::vector<ObjectId> takeDestroyed();

    void clear();

private:
    static constexpr uint32_t kNone = 0xFFFFFFFFu;
    enum Flag : uint8_t { kAlive = 1, kActive = 2, kDirty = 4, kChanged = 8 };

    bool valid(ObjectId id) const;
    uint32_t allocate();
    void link(uint32_t child, uint32_t parent);
    void unlink(uint32_t child);
    void markDirty(uint32_t i);
    void markChanged(uint32_t i);
    void recomputeSubtree(uint32_t root);
    Mat4 parentWorld(uint32_t i) const;
    Mat4 composeDown(uint32_t i, uint32_t top) const;
    uint32_t internTag(const std::string& tag);
    void destroySubtree(uint32_t i, size_t& count);
    void unindexName(uint32_t i);

    // Colunas, uma posição por objeto (vivo ou vaga livre).
    std::vector<uint32_t> m_generation;
    std::vector<uint8_t> m_flags;
    std::vector<std::string> m_name;
    std::vector<uint32_t> m_parent, m_firstChild, m_nextSibling, m_prevSibling;
    std::vector<Transform> m_local;
    std::vector<Mat4> m_world;
    std::vector<std::vector<uint32_t>> m_tags;

    std::vector<uint32_t> m_free;            // vagas para reaproveitar
    std::vector<uint32_t> m_dirtyList;       // posições marcadas sujas
    std::vector<uint32_t> m_changed;         // resultado de updateTransforms
    std::vector<uint32_t> m_changedPending;  // mudanças de estado fora da propagação
    std::vector<ObjectId> m_destroyed;
    std::unordered_map<std::string, std::vector<uint32_t>> m_byName;
    std::unordered_map<std::string, uint32_t> m_tagIds;
    std::vector<std::string> m_tagNames;
    size_t m_live = 0;
};

} // namespace eruption
