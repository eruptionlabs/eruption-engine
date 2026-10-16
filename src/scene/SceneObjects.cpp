#include "scene/SceneObjects.hpp"

#include "core/Engine.hpp"
#include "core/Logger.hpp"
#include "renderer/ModelRenderer.hpp"
#include "scene/SceneEdits.hpp"
#include "script/ScriptApi.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>

namespace eruption {

namespace {

// Personagem olha para +Z com facing 0; o giro é em Y.
ObjectWorld::Transform characterTransform(const Vec3& position, float facingDeg) {
    ObjectWorld::Transform t;
    t.position = position;
    t.rotation = glm::angleAxis(glm::radians(facingDeg), Vec3(0.0f, 1.0f, 0.0f));
    return t;
}

} // namespace

// ---------------------------------------------------------------------------
// Tabelas por vaga de objeto
// ---------------------------------------------------------------------------

uint32_t SceneObjects::lookup(const std::vector<OwnerSlot>& table, ObjectId id) {
    if (!id.valid() || id.index >= table.size() || table[id.index].generation != id.generation) return kNone;
    return table[id.index].value;
}

void SceneObjects::store(std::vector<OwnerSlot>& table, ObjectId id, uint32_t value) {
    if (table.size() <= id.index) table.resize(id.index + 1);
    table[id.index] = {id.generation, value};
}

uint32_t SceneObjects::partOf(ObjectId id) const { return lookup(m_partOfObject, id); }

void SceneObjects::markPulled(uint32_t objectIndex) {
    if (m_pulled.size() <= objectIndex) m_pulled.resize(objectIndex + 1, 0);
    if (m_pulled[objectIndex]) return;
    m_pulled[objectIndex] = 1;
    m_pulledList.push_back(objectIndex);
}

void SceneObjects::reset() { resetTable(false); }

void SceneObjects::resetObjects() { resetTable(true); }

void SceneObjects::resetTable(bool keepPool) {
    std::unordered_map<std::string, std::vector<Pooled>> pool;
    std::vector<uint8_t> pooled;
    if (keepPool) {
        pool.swap(m_pool);
        pooled.swap(m_pooledInstance);
    }
    m_world.clear();
    m_world.takeDestroyed(); // nada a soltar: as tabelas de quem desenha já foram refeitas
    m_parts.clear();
    m_freeParts.clear();
    m_partOfObject.clear();
    m_instanceOwner.clear();
    m_characterObject.clear();
    m_characterOfObject.clear();
    m_characterLast.clear();
    m_characterHidden.clear();
    m_characterReleased.clear();
    m_characterNameSeen.clear();
    m_pooledInstance.clear();
    m_lightSlots.clear();
    m_areaSlots.clear();
    m_soundSlots.clear();
    m_pointOfObject.clear();
    m_pulled.clear();
    m_pulledList.clear();
    m_groups.clear();
    m_groupPath.clear();
    m_treeSource.clear();
    m_prefabScripts.clear();
    m_made.clear();
    m_madeLimit = 1024;
    m_prefabs.clear();
    m_pool.clear();
    m_authored.clear();
    m_prefabTags.clear();
    m_sceneParent.clear();
    m_sceneInstanceParent.clear();
    if (keepPool) {
        m_pool.swap(pool);
        m_pooledInstance.swap(pooled);
    }
}

// ---------------------------------------------------------------------------
// Consultas
// ---------------------------------------------------------------------------

ObjectId SceneObjects::objectOfInstance(uint32_t instance) const {
    if (instance >= m_instanceOwner.size() || m_instanceOwner[instance].part == kNone) return {};
    return m_parts[m_instanceOwner[instance].part].owner;
}

std::vector<uint32_t> SceneObjects::instancesOf(ObjectId id) const {
    const uint32_t p = partOf(id);
    return p == kNone || !m_world.alive(id) ? std::vector<uint32_t>{} : m_parts[p].instances;
}

ObjectId SceneObjects::objectOfCharacter(size_t character) const {
    return character < m_characterObject.size() ? m_characterObject[character] : ObjectId{};
}

int SceneObjects::characterOf(ObjectId id) const {
    const uint32_t c = lookup(m_characterOfObject, id);
    return c == kNone || !m_world.alive(id) ? -1 : static_cast<int>(c);
}

namespace {

// "nome#2" -> ("nome", 2); sem sufixo numérico, índice 0.
std::pair<std::string, size_t> splitIndexed(const std::string& segment) {
    const size_t hash = segment.rfind('#');
    if (hash == std::string::npos || hash + 1 >= segment.size()) return {segment, 0};
    for (size_t k = hash + 1; k < segment.size(); ++k)
        if (!std::isdigit(static_cast<unsigned char>(segment[k]))) return {segment, 0};
    return {segment.substr(0, hash), static_cast<size_t>(std::stoul(segment.substr(hash + 1)))};
}

} // namespace

// Objetos com o mesmo nome no mesmo lugar (filhos do mesmo pai, ou da raiz),
// na ordem de criação: o caminho distingue "inimigo", "inimigo#1"...
std::vector<ObjectId> SceneObjects::namesakes(ObjectId parent, const std::string& name) const {
    std::vector<ObjectId> out;
    if (parent.valid()) {
        for (const ObjectId& c : m_world.children(parent))
            if (m_world.name(c) == name) out.push_back(c);
        return out;
    }
    for (const ObjectId& c : m_world.findAll(name))
        if (!m_world.parent(c).valid()) out.push_back(c);
    return out;
}

ObjectId SceneObjects::childNamed(ObjectId parent, const std::string& name) const {
    const auto [base, k] = splitIndexed(name);
    const std::vector<ObjectId> same = namesakes(parent, base);
    return k < same.size() ? same[k] : ObjectId{};
}

ObjectId SceneObjects::findPath(const std::string& path) const {
    size_t slash = path.find('/');
    // Primeiro trecho: um da raiz com esse nome (o caminho que pathOf escreve);
    // sem nenhum na raiz, o mais antigo em qualquer lugar ("porta" solta).
    const auto [base, k] = splitIndexed(path.substr(0, slash));
    const std::vector<ObjectId> roots = namesakes({}, base);
    ObjectId at = k < roots.size() ? roots[k] : (k == 0 ? m_world.find(base) : ObjectId{});
    while (at.valid() && slash != std::string::npos) {
        const size_t next = path.find('/', slash + 1);
        at = childNamed(at, path.substr(slash + 1, next == std::string::npos ? std::string::npos : next - slash - 1));
        slash = next;
    }
    return at;
}

std::string SceneObjects::sourceOf(ObjectId id) const {
    if (!m_world.alive(id)) return {};
    const auto tree = m_treeSource.find(groupKey(id));
    if (tree != m_treeSource.end()) return tree->second;
    const uint32_t p = partOf(id);
    return p == kNone ? std::string() : m_parts[p].source;
}

// Nome que o objeto tem por ser o que é (modelo, personagem, luz, área, som,
// grupo de nós); vazio para os criados pelo jogo.
std::string SceneObjects::ownName(ObjectId id) const {
    const std::string scene = sceneName(id);
    if (characterOf(id) >= 0 || lookup(m_pointOfObject, id) != kNone) return scene;
    const auto group = m_groupPath.find(groupKey(id));
    if (group != m_groupPath.end()) return group->second.substr(group->second.rfind('/') + 1);
    const uint32_t p = partOf(id);
    if (p == kNone || !m_parts[p].source.empty() || m_sceneParent.count(groupKey(id))) return {};
    return scene.substr(scene.rfind('/') + 1);
}

std::string SceneObjects::pathOf(ObjectId id) const {
    std::string path;
    for (ObjectId at = id; at.valid(); at = m_world.parent(at)) {
        // Nome repetido no mesmo lugar ganha o índice: o jogo salvo e as
        // referências acham este, não o primeiro com o nome.
        std::string segment = m_world.name(at);
        const std::vector<ObjectId> same = namesakes(m_world.parent(at), segment);
        for (size_t k = 1; k < same.size(); ++k)
            if (same[k] == at) segment += "#" + std::to_string(k);
        path = path.empty() ? segment : segment + "/" + path;
    }
    return path;
}

std::string SceneObjects::sceneName(ObjectId id) const {
    const uint32_t p = partOf(id);
    if (p != kNone && m_parts[p].instances.size() == 1 && m_parts[p].source.empty()) {
        // Nome da instância: é como o mapa (e models.find) chamam o objeto.
        const uint32_t inst = m_parts[p].instances.front();
        if (m_models && inst < m_models->getInstances().size()) return m_models->getInstances()[inst].name;
    }
    // Nome que não muda quando o objeto troca de pai: o do personagem e o
    // caminho de origem do grupo ("casa" de "casa/porta").
    if (const int c = characterOf(id); c >= 0 && static_cast<size_t>(c) < m_characterNameSeen.size())
        return m_characterNameSeen[static_cast<size_t>(c)];
    PointKind kind;
    size_t index;
    if (pointOf(id, kind, index)) {
        const std::vector<PointSlot>& slots =
            kind == PointKind::Light ? m_lightSlots : (kind == PointKind::Area ? m_areaSlots : m_soundSlots);
        if (index < slots.size()) return slots[index].name;
    }
    const auto group = m_groupPath.find(groupKey(id));
    if (group != m_groupPath.end()) return group->second;
    return pathOf(id);
}

ObjectId SceneObjects::findSceneName(const std::string& name) const {
    const int inst = scriptFindModel(name);
    if (inst >= 0) {
        const ObjectId id = objectOfInstance(static_cast<uint32_t>(inst));
        if (m_world.alive(id)) return id;
    }
    const auto group = m_groups.find(name);
    if (group != m_groups.end() && m_world.alive(group->second)) return group->second;
    for (size_t c = 0; c < m_characterObject.size(); ++c)
        if (m_world.alive(m_characterObject[c]) && m_characterNameSeen[c] == name) return m_characterObject[c];
    for (const std::vector<PointSlot>* slots : {&m_lightSlots, &m_areaSlots, &m_soundSlots})
        for (const PointSlot& slot : *slots)
            if (slot.name == name && m_world.alive(slot.object)) return slot.object;
    return findPath(name);
}

// Grupo do prefixo de um nome com barra ("a/b" -> grupo "a"), ou nenhum.
ObjectId SceneObjects::groupOfPrefix(const std::string& name) const {
    const size_t slash = name.rfind('/');
    if (slash == std::string::npos || slash == 0) return {};
    const auto it = m_groups.find(name.substr(0, slash));
    return it != m_groups.end() && m_world.alive(it->second) ? it->second : ObjectId{};
}

ObjectId SceneObjects::naturalParent(ObjectId id) const {
    // Filho de prefab da cena: o pai de quando a cena o montou.
    const auto scene = m_sceneParent.find(groupKey(id));
    if (scene != m_sceneParent.end()) return m_world.alive(scene->second) ? scene->second : ObjectId{};
    // Grupo aninhado ("a/b", criado para "a/b/c"): o pai dele é o grupo "a".
    const auto group = m_groupPath.find(groupKey(id));
    if (group != m_groupPath.end()) return groupOfPrefix(group->second);
    const uint32_t p = partOf(id);
    if (p == kNone || m_parts[p].instances.size() != 1 || !m_parts[p].source.empty() || !m_models) return {};
    const uint32_t inst = m_parts[p].instances.front();
    if (inst >= m_models->getInstances().size()) return {};
    return groupOfPrefix(m_models->getInstances()[inst].name);
}

void SceneObjects::applyAuthored(const Authored& a) {
    const ObjectId id = findSceneName(a.name);
    if (!id.valid()) return;
    for (const auto& t : a.tags) m_world.addTag(id, t);
    if (a.parent.empty()) return;
    const ObjectId parent = findSceneName(a.parent);
    if (!parent.valid() || !m_world.setParent(id, parent, true))
        ERUPTION_LOG_WARN("Scene: %s cannot go inside %s", a.name.c_str(), a.parent.c_str());
}

void SceneObjects::author(Engine& engine, const Authored& a) {
    refresh(engine);
    // Declaração vazia (desfez a primeira edição) some do arquivo de cena.
    const bool empty = a.parent.empty() && a.tags.empty() && a.scripts.empty();
    for (size_t k = 0; k < m_authored.size();) {
        if (m_authored[k].name == a.name && empty) {
            m_authored.erase(m_authored.begin() + static_cast<std::ptrdiff_t>(k));
            continue;
        }
        if (m_authored[k].name == a.name) {
            m_authored[k] = a;
            applyAuthored(a);
            return;
        }
        ++k;
    }
    if (empty) return;
    m_authored.push_back(a);
    applyAuthored(a);
}

void SceneObjects::restoreAuthored(Engine& engine) {
    refresh(engine);
    rebindOrphans(engine);
    for (const ObjectId& id : m_world.all()) {
        // Nome trocado pelo jogo (obj.name) volta ao do mapa.
        const std::string own = ownName(id);
        if (!own.empty() && own != m_world.name(id)) m_world.rename(id, own);
        for (const auto& t : m_world.tags(id)) m_world.removeTag(id, t);
        if (!m_world.active(id)) m_world.setActive(id, true);
        const ObjectId natural = naturalParent(id);
        if (m_world.parent(id) != natural) m_world.setParent(id, natural, true);
    }
    for (const Authored& a : m_authored) applyAuthored(a);
    for (const auto& [id, tag] : m_prefabTags) m_world.addTag(id, tag);
}

void SceneObjects::clearAuthored(Engine& engine) {
    m_authored.clear();
    restoreAuthored(engine);
}

std::vector<std::pair<ObjectId, std::string>> SceneObjects::sceneScripts() const {
    std::vector<std::pair<ObjectId, std::string>> out;
    for (const auto& entry : m_prefabScripts)
        if (m_world.alive(entry.first)) out.push_back(entry);
    for (const Authored& a : m_authored) {
        if (a.scripts.empty()) continue;
        const ObjectId id = findSceneName(a.name);
        if (!m_world.alive(id)) continue;
        for (const auto& m : a.scripts) out.push_back({id, m});
    }
    return out;
}

void SceneObjects::setModelVisible(ObjectId id, bool visible) {
    const uint32_t p = m_world.alive(id) ? partOf(id) : kNone;
    if (p == kNone) return;
    for (auto& v : m_parts[p].visible) v = visible ? 1 : 0;
    // O estado efetivo é aplicado no próximo sync (junto com o ligado do pai).
    m_world.touch(id);
}

// ---------------------------------------------------------------------------
// Componentes: ligar e soltar
// ---------------------------------------------------------------------------

// Liga instâncias a um objeto (componente de modelo). Se a vaga do objeto
// ainda guarda a parte de um dono antigo já destruído, ela é solta antes.
uint32_t SceneObjects::attachPart(ModelRenderer& mr, ObjectId owner, const std::string& source,
                                  const std::vector<uint32_t>& instances, const std::vector<Mat4>& local) {
    if (owner.index < m_partOfObject.size()) {
        const OwnerSlot old = m_partOfObject[owner.index];
        if (old.value != kNone && old.generation != owner.generation) releasePart(mr, old.value);
    }
    uint32_t p;
    if (!m_freeParts.empty()) {
        p = m_freeParts.back();
        m_freeParts.pop_back();
    } else {
        p = static_cast<uint32_t>(m_parts.size());
        m_parts.emplace_back();
    }
    ModelPart& part = m_parts[p];
    part.owner = owner;
    part.source = source;
    part.instances = instances;
    part.local = local;
    part.visible.assign(instances.size(), 1);
    part.pushedWorld = m_world.world(owner);
    part.placed.resize(instances.size());
    for (size_t k = 0; k < instances.size(); ++k) part.placed[k] = part.pushedWorld * local[k];
    store(m_partOfObject, owner, p);
    for (uint32_t k = 0; k < instances.size(); ++k) {
        if (m_instanceOwner.size() <= instances[k]) m_instanceOwner.resize(instances[k] + 1);
        m_instanceOwner[instances[k]] = {p, k};
    }
    return p;
}

// As instâncias param de desenhar; as de spawn voltam ao estoque.
void SceneObjects::releasePart(ModelRenderer& mr, uint32_t p) {
    ModelPart& part = m_parts[p];
    for (uint32_t inst : part.instances) {
        mr.setInstanceEnabled(inst, false, true);
        if (inst < m_instanceOwner.size()) m_instanceOwner[inst] = InstanceSlot{};
    }
    if (!part.source.empty() && !part.instances.empty()) {
        for (uint32_t inst : part.instances) {
            if (m_pooledInstance.size() <= inst) m_pooledInstance.resize(inst + 1, 0);
            m_pooledInstance[inst] = 1;
        }
        m_pool[part.source].push_back({std::move(part.instances), std::move(part.local)});
    }
    if (lookup(m_partOfObject, part.owner) == p) store(m_partOfObject, part.owner, kNone);
    part = ModelPart{};
    m_freeParts.push_back(p);
}

// Objeto do personagem destruído: o personagem some até o Stop (ou um jogo
// carregado), que traz os dois de volta.
void SceneObjects::releaseCharacter(Engine& engine, uint32_t c, ObjectId id) {
    auto& chars = engine.characters();
    if (lookup(m_characterOfObject, id) == c) store(m_characterOfObject, id, kNone);
    // A vaga já pode ter outro objeto (o personagem foi religado depois).
    if (c >= m_characterObject.size() || m_characterObject[c] != id) return;
    if (c < chars.size() && !chars[c].removed) {
        chars[c].visible = false;
        m_characterReleased[c] = 1;
    }
    m_characterObject[c] = {};
}

void SceneObjects::releaseDestroyed(Engine& engine) {
    ModelRenderer& mr = engine.modelRenderer();
    for (const ObjectId& id : m_world.takeDestroyed()) {
        if (const uint32_t p = lookup(m_partOfObject, id); p != kNone) releasePart(mr, p);
        if (const uint32_t c = lookup(m_characterOfObject, id); c != kNone) releaseCharacter(engine, c, id);
        if (const uint32_t code = lookup(m_pointOfObject, id); code != kNone) releasePoint(engine, code);
    }
}

// Objeto que veio do mapa (modelo, personagem, grupo de nós): o jogo pode
// pô-lo dentro de um objeto seu, mas ele não morre junto quando o jogo some.
bool SceneObjects::isMapObject(ObjectId id) const {
    if (m_sceneParent.count(groupKey(id))) return true;
    const uint32_t p = partOf(id);
    if (p != kNone && m_parts[p].source.empty()) return true;
    if (characterOf(id) >= 0) return true;
    if (lookup(m_pointOfObject, id) != kNone) return true;
    return m_groupPath.count(groupKey(id)) != 0;
}

void SceneObjects::rescueMapObjects(ObjectId root) {
    for (const ObjectId& c : m_world.children(root)) {
        if (isMapObject(c)) m_world.setParent(c, naturalParent(c), true);
        else rescueMapObjects(c);
    }
}

// Objeto que a tabela de origem perdeu (personagem/área tirado, lista
// encolhida): sai, mas os objetos do mapa que estavam dentro dele ficam.
void SceneObjects::destroyOwn(ObjectId id) {
    if (!m_world.alive(id)) return;
    rescueMapObjects(id);
    m_world.destroy(id);
}

// Filho de prefab da cena: tem pai certo (o Stop devolve), é da cena (não
// some com o jogo) e, se o jogo o destruir, o modelo dele não vai para o
// estoque de spawn - volta com objeto no Stop.
void SceneObjects::markSceneBuilt(ObjectId id, ObjectId parent) {
    m_sceneParent[groupKey(id)] = parent;
    const uint32_t p = partOf(id);
    if (p == kNone) return;
    m_parts[p].source.clear();
    for (uint32_t inst : m_parts[p].instances) m_sceneInstanceParent[inst] = parent;
}

size_t SceneObjects::destroyMade(Engine& engine) {
    size_t n = 0;
    std::vector<ObjectId> made;
    made.swap(m_made);
    for (const ObjectId& id : made) {
        if (!m_world.alive(id)) continue;
        rescueMapObjects(id);
        n += m_world.destroy(id);
    }
    releaseDestroyed(engine);
    return n;
}

// Jogo que cria e destrói sem parar (tiros) não pode crescer estas listas:
// quando dobram, os mortos saem.
void SceneObjects::markMade(ObjectId id) {
    m_made.push_back(id);
    if (m_made.size() < m_madeLimit) return;
    std::vector<ObjectId> alive;
    for (const ObjectId& m : m_made)
        if (m_world.alive(m)) alive.push_back(m);
    m_made.swap(alive);
    for (auto it = m_treeSource.begin(); it != m_treeSource.end();) {
        const ObjectId owner{static_cast<uint32_t>(it->first >> 32), static_cast<uint32_t>(it->first & 0xFFFFFFFFu)};
        if (m_world.alive(owner)) ++it;
        else it = m_treeSource.erase(it);
    }
    m_madeLimit = std::max<size_t>(1024, m_made.size() * 2);
}

size_t SceneObjects::destroy(Engine& engine, ObjectId id) {
    const size_t n = m_world.destroy(id);
    releaseDestroyed(engine);
    return n;
}

// ---------------------------------------------------------------------------
// O que o mapa trouxe vira objeto
// ---------------------------------------------------------------------------

// "casa/porta" fica filho de "casa" (criado sem modelo, onde o primeiro nó
// dele está); "a/b/c" vira a -> b -> c.
ObjectId SceneObjects::groupFor(const std::string& instanceName, const Mat4& firstTransform) {
    const size_t slash = instanceName.rfind('/');
    if (slash == std::string::npos || slash == 0) return {};
    const std::string path = instanceName.substr(0, slash);
    const auto it = m_groups.find(path);
    if (it != m_groups.end() && m_world.alive(it->second)) return it->second;
    const ObjectId parent = groupFor(path, firstTransform);
    const size_t cut = path.rfind('/');
    ObjectWorld::Transform at;
    at.position = Vec3(firstTransform[3]);
    if (parent.valid()) at.position = Vec3(glm::inverse(m_world.world(parent)) * Vec4(at.position, 1.0f));
    const ObjectId g = m_world.create(cut == std::string::npos ? path : path.substr(cut + 1), parent, at);
    m_groups[path] = g;
    m_groupPath[groupKey(g)] = path;
    m_world.settle(g); // nasceu no lugar: nada a empurrar
    return g;
}

void SceneObjects::bindInstance(ModelRenderer& mr, uint32_t i) {
    const ModelInstance& inst = mr.getInstances()[i];
    const Mat4 placement = mr.instancePlacement(i); // sem a pose de animação
    const ObjectId group = groupFor(inst.name, placement);
    const Mat4 rel = group.valid() ? glm::inverse(m_world.world(group)) * placement : placement;
    const std::string name = group.valid() ? inst.name.substr(inst.name.rfind('/') + 1) : inst.name;
    const ObjectId id = m_world.create(name, group, ObjectWorld::Transform::fromMatrix(rel));
    // Matriz da instância relativa ao objeto: o que a decomposição não
    // representa (cisalhamento) fica aqui, e a instância não muda de lugar.
    const uint32_t p = attachPart(mr, id, {}, {i}, {glm::inverse(m_world.world(id)) * placement});
    m_parts[p].placed[0] = placement; // exato: o primeiro toque compara com ele
    if (!inst.enabled) m_parts[p].visible[0] = 0;
    m_world.settle(id); // nasceu igual à instância: nada a empurrar
}

void SceneObjects::bindNewInstances(ModelRenderer& mr) {
    const auto& insts = mr.getInstances();
    for (uint32_t i = static_cast<uint32_t>(m_instanceOwner.size()); i < insts.size(); ++i) {
        // Instância no estoque de spawn (tabela recomeçada sem refazer o mapa):
        // continua no estoque, sem objeto.
        if (i < m_pooledInstance.size() && m_pooledInstance[i]) {
            m_instanceOwner.resize(i + 1);
            continue;
        }
        bindInstance(mr, i);
    }
}

void SceneObjects::bindCharacter(Engine& engine, size_t c) {
    auto& chars = engine.characters();
    const ObjectId id = m_world.create(chars[c].name, {}, characterTransform(chars[c].position, chars[c].facing));
    if (id.index < m_characterOfObject.size()) {
        const OwnerSlot old = m_characterOfObject[id.index];
        if (old.value != kNone && old.generation != id.generation)
            releaseCharacter(engine, old.value, {id.index, old.generation});
    }
    store(m_characterOfObject, id, static_cast<uint32_t>(c));
    if (m_characterObject.size() <= c) {
        m_characterObject.resize(c + 1);
        m_characterLast.resize(c + 1);
        m_characterHidden.resize(c + 1, 0);
        m_characterReleased.resize(c + 1, 0);
        m_characterNameSeen.resize(c + 1);
    }
    m_characterObject[c] = id;
    m_characterLast[c] = chars[c].position;
    m_characterHidden[c] = 0;
    m_characterReleased[c] = 0;
    m_characterNameSeen[c] = chars[c].name;
    m_world.settle(id);
}

// Objeto do mapa destruído pelo jogo: no Stop o modelo (ou o personagem)
// ganha objeto de novo. As instâncias no estoque de spawn ficam de fora.
void SceneObjects::rebindOrphans(Engine& engine) {
    ModelRenderer& mr = engine.modelRenderer();
    const uint32_t n = static_cast<uint32_t>(std::min(m_instanceOwner.size(), mr.getInstances().size()));
    for (uint32_t i = 0; i < n; ++i) {
        const bool pooled = i < m_pooledInstance.size() && m_pooledInstance[i];
        if (m_instanceOwner[i].part != kNone || pooled) continue;
        bindInstance(mr, i);
        // Filho de prefab da cena destruído no jogo: volta para dentro da raiz.
        const auto scene = m_sceneInstanceParent.find(i);
        if (scene == m_sceneInstanceParent.end() || !m_world.alive(scene->second)) continue;
        const ObjectId back = objectOfInstance(i);
        m_world.setParent(back, scene->second, true);
        m_sceneParent[groupKey(back)] = scene->second;
    }
    auto& chars = engine.characters();
    for (size_t c = 0; c < m_characterObject.size() && c < chars.size(); ++c) {
        if (m_world.alive(m_characterObject[c]) || chars[c].removed) continue;
        if (m_characterReleased[c]) chars[c].visible = true; // escondido porque o jogo destruiu o objeto
        bindCharacter(engine, c);
    }
    restorePoints(engine);
}

void SceneObjects::bindNewCharacters(Engine& engine) {
    auto& chars = engine.characters();
    // Personagens apagados da lista (troca de mapa sem refazer modelos):
    // os objetos deles saem junto.
    while (m_characterObject.size() > chars.size()) {
        const ObjectId gone = m_characterObject.back();
        if (lookup(m_characterOfObject, gone) != kNone) store(m_characterOfObject, gone, kNone);
        destroyOwn(gone);
        m_characterObject.pop_back();
        m_characterLast.pop_back();
        m_characterHidden.pop_back();
        m_characterReleased.pop_back();
        m_characterNameSeen.pop_back();
    }
    for (size_t c = m_characterObject.size(); c < chars.size(); ++c) bindCharacter(engine, c);
}

void SceneObjects::refresh(Engine& engine) {
    ModelRenderer& mr = engine.modelRenderer();
    m_models = &mr;
    if (mr.instanceEpoch() != m_instanceEpoch) {
        reset();
        m_instanceEpoch = mr.instanceEpoch();
    }
    if (mr.getInstances().size() < m_instanceOwner.size()) reset(); // lista encolheu sem avisar
    const size_t before = m_world.count();
    const auto t0 = std::chrono::steady_clock::now();
    bindNewInstances(mr);
    bindNewCharacters(engine);
    bindPoints(engine);
    if (m_world.count() > before + 1000) {
        const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        ERUPTION_LOG_WARN("[OBJETOS] %zu objetos (%zu instancias, %zu personagens) em %.1f ms", m_world.count(),
                          m_instanceOwner.size(), m_characterObject.size(), ms);
    }
}

// Prefab posto na cena (SceneEdits "added"): o modelo da raiz já existe; aqui
// entram os filhos, as etiquetas e os scripts do prefab. Feito a cada carga
// da cena (não vai para o arquivo) e não some no Stop.
// Prefab posto na cena (SceneEdits "added"): o modelo da raiz já existe; aqui
// entram os filhos, as etiquetas e os scripts do prefab. Feito a cada carga
// da cena (não vai para o arquivo) e não some no Stop.
void SceneObjects::buildPrefabExtras(Engine& engine, ObjectId root, const Mat4& placement, const std::string& prefabPath) {
    std::string error;
    const Prefab* prefab = prefabFor(prefabPath, &error);
    if (!prefab || !m_world.alive(root)) return;
    // Só prefab com filhos precisa da raiz no lugar da cena (cena antiga com
    // prefab simples fica exatamente como era).
    if (!prefab->children.empty()) placeRootAt(root, placement);
    for (const auto& t : prefab->tags) {
        m_world.addTag(root, t);
        m_prefabTags.push_back({root, t});
    }
    for (const auto& m : prefab->scripts) m_prefabScripts.push_back({root, m});
    const std::vector<Prefab::Child> children = prefab->children;
    SceneBuildGuard guard(m_buildingScene);
    for (const Prefab::Child& c : children) {
        const std::string childSource = !c.prefab.empty() ? c.prefab : (c.node.empty() ? c.file : c.file + "#" + c.node);
        SceneEdits::Transform placed;
        placed.position = c.position;
        placed.rotation = c.rotation;
        placed.scale = c.scale;
        std::vector<std::string> expanding{prefabPath};
        std::string childError;
        const ObjectId child = spawnTree(engine, childSource, c.name.empty() ? defaultName(childSource) : c.name, root,
                                         ObjectWorld::Transform::fromMatrix(placed.matrix()), &childError,
                                         &m_prefabScripts, expanding);
        if (!child.valid()) ERUPTION_LOG_WARN("Prefab %s: %s", prefabPath.c_str(), childError.c_str());
    }
}

// A matriz de cada instância relativa ao objeto, de novo (o objeto mudou de
// lugar mas o modelo não).
void SceneObjects::relocalize(ObjectId id) {
    const uint32_t p = partOf(id);
    if (p == kNone || !m_models) return;
    ModelPart& part = m_parts[p];
    for (size_t k = 0; k < part.instances.size(); ++k)
        if (part.instances[k] < m_models->getInstances().size()) {
            part.placed[k] = m_models->instancePlacement(part.instances[k]);
            part.local[k] = glm::inverse(m_world.world(id)) * part.placed[k];
        }
    part.pushedWorld = m_world.world(id);
}

// A raiz fica onde a cena pôs o prefab (sem a escala/giro interno do
// prefab, que fica na matriz do modelo): os filhos ficam relativos a isso,
// como no objects.spawn, e escala desigual do modelo não deforma os filhos.
void SceneObjects::placeRootAt(ObjectId root, const Mat4& placement) {
    std::vector<std::pair<ObjectId, Mat4>> kids;
    for (const ObjectId& c : m_world.children(root)) kids.push_back({c, m_world.world(c)});
    m_world.setWorld(root, placement);
    markPulled(root.index); // as instâncias não mudam de lugar: nada a reescrever
    relocalize(root);
    for (const auto& [c, w] : kids) { // nós do .glb ficam onde estão
        m_world.setWorld(c, w);
        markPulled(c.index);
        relocalize(c);
    }
}

std::string SceneObjects::defaultName(const std::string& source) {
    // O nó pedido ("arquivo.glb#Nó") ou o arquivo sem pasta nem extensão.
    const size_t hash = source.find('#');
    if (hash != std::string::npos) return source.substr(hash + 1);
    const size_t slash = source.find_last_of("/\\");
    const std::string file = source.substr(slash == std::string::npos ? 0 : slash + 1);
    return file.substr(0, file.find('.'));
}

// Prefab lido uma vez por mapa: criar cem iguais não abre cem estados Luau.
const Prefab* SceneObjects::prefabFor(const std::string& path, std::string* error) {
    const auto it = m_prefabs.find(path);
    if (it != m_prefabs.end()) return &it->second;
    Prefab p;
    std::string err;
    if (!p.load(path, err)) {
        if (error) *error = err;
        return nullptr;
    }
    return &m_prefabs.emplace(path, std::move(p)).first->second;
}

ObjectId SceneObjects::spawn(Engine& engine, const std::string& source, const std::string& name, ObjectId parent,
                             const ObjectWorld::Transform& local, std::string* error, std::vector<ObjectScript>* scripts) {
    // O que já existe ganha objeto antes: as instâncias novas entram no fim.
    refresh(engine);
    std::vector<std::string> expanding;
    return spawnTree(engine, source, name, parent, local, error, scripts, expanding);
}

// Prefab composto: a raiz (com ou sem modelo), as etiquetas e scripts dela,
// e cada filho - outro prefab ou um nó de .glb - no lugar dele.
ObjectId SceneObjects::spawnTree(Engine& engine, const std::string& source, const std::string& name, ObjectId parent,
                                 const ObjectWorld::Transform& local, std::string* error,
                                 std::vector<ObjectScript>* scripts, std::vector<std::string>& expanding) {
    // Prefab que contém a si mesmo (direto ou por outro) pararia só no fim da memória.
    if (std::find(expanding.begin(), expanding.end(), source) != expanding.end()) {
        if (error) *error = source + " contains itself";
        return {};
    }
    const Prefab* prefab = nullptr;
    if (Prefab::isPrefabPath(source)) {
        prefab = prefabFor(source, error);
        if (!prefab) return {};
    }
    const ObjectId id = prefab && prefab->file.empty() ? m_world.create(name, parent, local)
                                                       : spawnModel(engine, source, name, parent, local, prefab, error);
    if (!id.valid()) return id;
    if (m_buildingScene) markSceneBuilt(id, parent);
    else markMade(id);
    if (!prefab) return id;
    m_treeSource[groupKey(id)] = source; // raiz sem modelo também sabe de onde veio
    for (const auto& t : prefab->tags) m_world.addTag(id, t);
    if (scripts)
        for (const auto& m : prefab->scripts) scripts->push_back({id, m});
    // Cópia: o mapa de prefabs pode crescer (rehash) enquanto os filhos são lidos.
    const std::vector<Prefab::Child> children = prefab->children;
    for (const Prefab::Child& c : children) {
        const std::string childSource = !c.prefab.empty() ? c.prefab : (c.node.empty() ? c.file : c.file + "#" + c.node);
        SceneEdits::Transform placed;
        placed.position = c.position;
        placed.rotation = c.rotation;
        placed.scale = c.scale;
        std::string childError;
        expanding.push_back(source);
        const ObjectId child = spawnTree(engine, childSource, c.name.empty() ? defaultName(childSource) : c.name, id,
                                         ObjectWorld::Transform::fromMatrix(placed.matrix()), &childError, scripts, expanding);
        expanding.pop_back();
        if (!child.valid()) ERUPTION_LOG_WARN("Prefab %s: %s", source.c_str(), childError.c_str());
    }
    return id;
}

// O modelo do objeto: um .glb, "arquivo.glb#Nó" ou o modelo da raiz de um prefab.
ObjectId SceneObjects::spawnModel(Engine& engine, const std::string& source, const std::string& name, ObjectId parent,
                                  const ObjectWorld::Transform& local, const Prefab* prefab, std::string* error) {
    ModelRenderer& mr = engine.modelRenderer();
    std::vector<uint32_t> instances;
    std::vector<Mat4> locals;
    auto pooled = m_pool.find(source);
    if (pooled != m_pool.end() && !pooled->second.empty()) {
        instances = std::move(pooled->second.back().instances);
        locals = std::move(pooled->second.back().local);
        pooled->second.pop_back();
        for (uint32_t inst : instances)
            if (inst < m_pooledInstance.size()) m_pooledInstance[inst] = 0;
    } else {
        std::string file = source, node;
        Mat4 inner(1.0f);
        if (prefab) {
            file = prefab->file;
            node = prefab->node;
            inner = prefab->matrix();
        } else if (const size_t hash = source.find('#'); hash != std::string::npos) {
            file = source.substr(0, hash);
            node = source.substr(hash + 1);
        }
        SceneEdits::AddedInstance added;
        if (!SceneEdits::addModel(engine, file, node, name, Mat4(1.0f), inner, added)) {
            if (error) *error = "could not load " + source;
            return {};
        }
        // Raiz na origem: o lugar de cada instância (sem pose) é a matriz dela
        // relativa ao objeto.
        for (uint32_t k = 0; k < added.count; ++k) {
            instances.push_back(added.first + k);
            locals.push_back(mr.instancePlacement(added.first + k));
        }
    }
    const ObjectId id = m_world.create(name, parent, local);
    attachPart(mr, id, source, instances, locals);
    // Já no lugar neste quadro. A montagem do modelo mexeu nas instâncias
    // "por fora": esquecido, senão o sync puxaria o objeto de volta para cá
    // por cima do que o script fizer com ele ainda neste quadro.
    const Mat4 w = m_world.world(id);
    const bool on = m_world.activeInHierarchy(id);
    for (size_t k = 0; k < instances.size(); ++k) {
        m_parts[partOf(id)].placed[k] = w * locals[k];
        mr.setInstancePlacement(instances[k], w * locals[k], true);
        mr.setInstanceEnabled(instances[k], on, true);
        mr.forgetTouch(instances[k]);
    }
    return id;
}

// ---------------------------------------------------------------------------
// Puxar, propagar, empurrar
// ---------------------------------------------------------------------------

// Mexido por fora: o objeto (ou só a parte, se ele tem várias) vai junto.
void SceneObjects::pullTouchedInstances(ModelRenderer& mr) {
    mr.takeTouchedInstances(m_touched);
    const auto& insts = mr.getInstances();
    for (uint32_t i : m_touched) {
        if (i >= m_instanceOwner.size() || m_instanceOwner[i].part == kNone) continue;
        ModelPart& part = m_parts[m_instanceOwner[i].part];
        const uint32_t slot = m_instanceOwner[i].slot;
        if (!m_world.alive(part.owner)) continue;
        // Com o dono desligado a instância está apagada por nós: não é o
        // liga/desliga dela.
        if (m_world.activeInHierarchy(part.owner)) part.visible[slot] = insts[i].enabled ? 1 : 0;
        // Referência = onde o objeto estava no último empurrão/leitura (um
        // script pode tê-lo movido neste quadro; isso não é "mexido por fora").
        const Mat4 placement = mr.instancePlacement(i);
        if (placement == part.placed[slot]) continue; // só o liga/desliga mudou
        part.placed[slot] = placement;
        if (part.instances.size() == 1) {
            m_world.setWorld(part.owner, placement * glm::inverse(part.local[slot]));
            markPulled(part.owner.index);
            part.pushedWorld = m_world.world(part.owner);
        } else {
            part.local[slot] = glm::inverse(part.pushedWorld) * placement;
        }
    }
}

// Personagem que andou sozinho (caminho, seguir, patrulha) leva o objeto.
void SceneObjects::pullCharacters(Engine& engine) {
    auto& chars = engine.characters();
    for (size_t c = 0; c < m_characterObject.size() && c < chars.size(); ++c) {
        // Apagado (no editor, ou criado no Play e tirado no Stop): sem objeto;
        // desfeito o apagar, ganha de novo.
        const bool alive = m_world.alive(m_characterObject[c]);
        if (chars[c].removed && alive) {
            store(m_characterOfObject, m_characterObject[c], kNone);
            destroyOwn(m_characterObject[c]);
            m_characterObject[c] = {};
            continue;
        }
        // Apagar desfeito no editor: objeto de novo, com o que a cena declara.
        // (Objeto destruído pelo jogo só volta no Stop / jogo carregado.)
        if (!chars[c].removed && !alive && !m_characterReleased[c]) {
            bindCharacter(engine, c);
            for (const Authored& a : m_authored)
                if (a.name == chars[c].name) applyAuthored(a);
        }
        if (chars[c].removed || !m_world.alive(m_characterObject[c])) continue;
        // O PERSONAGEM foi renomeado (inspetor): objeto e declaração acompanham.
        // Nome trocado só no objeto (obj.name do script) fica como está.
        if (m_characterNameSeen[c] != chars[c].name) {
            renameAuthored(m_characterNameSeen[c], chars[c].name);
            m_world.rename(m_characterObject[c], chars[c].name);
            m_characterNameSeen[c] = chars[c].name;
        }
        if (chars[c].position == m_characterLast[c]) continue;
        m_world.setWorldPosition(m_characterObject[c], chars[c].position);
        m_characterLast[c] = chars[c].position;
        markPulled(m_characterObject[c].index);
    }
}

void SceneObjects::pushChanged(Engine& engine, const std::vector<uint32_t>& changed) {
    ModelRenderer& mr = engine.modelRenderer();
    const auto& insts = mr.getInstances();
    auto& chars = engine.characters();
    for (uint32_t index : changed) {
        const ObjectId id = m_world.idAt(index);
        const bool pulled = index < m_pulled.size() && m_pulled[index];
        if (const uint32_t p = partOf(id); p != kNone) {
            ModelPart& part = m_parts[p];
            const Mat4 w = m_world.world(id);
            const bool on = m_world.activeInHierarchy(id);
            for (size_t k = 0; k < part.instances.size(); ++k) {
                const uint32_t inst = part.instances[k];
                if (inst >= insts.size()) continue;
                if (!pulled) {
                    part.placed[k] = w * part.local[k];
                    mr.setInstancePlacement(inst, part.placed[k], true);
                }
                const bool want = on && part.visible[k];
                if (insts[inst].enabled != want) mr.setInstanceEnabled(inst, want, true);
            }
            part.pushedWorld = w;
        }
        pushPoint(engine, id, pulled);
        const uint32_t c = lookup(m_characterOfObject, id);
        if (c != kNone && c < chars.size()) {
            if (!pulled) {
                chars[c].position = m_world.worldPosition(id);
                m_characterLast[c] = chars[c].position;
            }
            // Objeto desligado esconde o personagem; religado, mostra de novo
            // só se foi ele quem escondeu (characters.hide continua valendo).
            const bool on = m_world.activeInHierarchy(id);
            if (!on && chars[c].visible) {
                chars[c].visible = false;
                m_characterHidden[c] = 1;
            } else if (on && m_characterHidden[c]) {
                chars[c].visible = true;
                m_characterHidden[c] = 0;
            }
        }
    }
}

void SceneObjects::sync(Engine& engine) {
    refresh(engine);
    // Soltar antes de puxar: destruído direto no world() não pode parecer
    // "objeto que falta" e ganhar outro na hora.
    releaseDestroyed(engine);
    pullTouchedInstances(engine.modelRenderer());
    pullCharacters(engine);
    pullPoints(engine);
    pushChanged(engine, m_world.updateTransforms());
    enforceHiddenLights(engine);
    for (uint32_t i : m_pulledList) m_pulled[i] = 0;
    m_pulledList.clear();
}

// ---------------------------------------------------------------------------
// Luzes, áreas e sons
// ---------------------------------------------------------------------------

namespace {

size_t pointCount(Engine& engine, SceneObjects::PointKind kind) {
    switch (kind) {
        case SceneObjects::PointKind::Light: {
            // Até a luz do jogador (F10), que o Render tira e repõe no fim
            // a cada quadro: ela não é luz do mapa e não pode deslocar índices.
            const auto& lights = engine.deferredLighting().getPointLights();
            for (size_t i = 0; i < lights.size(); ++i)
                if (lights[i].shadowCubemapIndex == kPlayerLightTag) return i;
            return lights.size();
        }
        case SceneObjects::PointKind::Area: return engine.areas().size();
        case SceneObjects::PointKind::Sound: return engine.soundEmitters().size();
    }
    return 0;
}

Vec3& pointPosition(Engine& engine, SceneObjects::PointKind kind, size_t i) {
    switch (kind) {
        case SceneObjects::PointKind::Light: return engine.deferredLighting().getPointLights()[i].position;
        case SceneObjects::PointKind::Area: return engine.areas()[i].center;
        case SceneObjects::PointKind::Sound: break;
    }
    return engine.soundEmitters()[i].position;
}

std::string pointName(Engine& engine, SceneObjects::PointKind kind, size_t i) {
    switch (kind) {
        case SceneObjects::PointKind::Light: return "light_" + std::to_string(i + 1);
        case SceneObjects::PointKind::Area: return engine.areas()[i].name;
        case SceneObjects::PointKind::Sound: break;
    }
    return engine.soundEmitters()[i].name;
}

const SceneObjects::PointKind kPointKinds[] = {SceneObjects::PointKind::Light, SceneObjects::PointKind::Area,
                                               SceneObjects::PointKind::Sound};

} // namespace

std::vector<SceneObjects::PointSlot>& SceneObjects::points(PointKind kind) {
    switch (kind) {
        case PointKind::Light: return m_lightSlots;
        case PointKind::Area: return m_areaSlots;
        case PointKind::Sound: break;
    }
    return m_soundSlots;
}

bool SceneObjects::pointOf(ObjectId id, PointKind& kind, size_t& index) const {
    const uint32_t code = lookup(m_pointOfObject, id);
    if (code == kNone || !m_world.alive(id)) return false;
    kind = static_cast<PointKind>(code >> 28);
    index = code & 0x0FFFFFFFu;
    return true;
}

ObjectId SceneObjects::objectOfPoint(PointKind kind, size_t index) const {
    const std::vector<PointSlot>& slots =
        kind == PointKind::Light ? m_lightSlots : (kind == PointKind::Area ? m_areaSlots : m_soundSlots);
    return index < slots.size() ? slots[index].object : ObjectId{};
}

void SceneObjects::bindPoint(Engine& engine, PointKind kind, size_t index) {
    std::vector<PointSlot>& slots = points(kind);
    if (slots.size() <= index) slots.resize(index + 1);
    PointSlot& slot = slots[index];
    ObjectWorld::Transform at;
    at.position = pointPosition(engine, kind, index);
    slot.name = pointName(engine, kind, index);
    slot.object = m_world.create(slot.name, {}, at);
    slot.last = at.position;
    store(m_pointOfObject, slot.object, pointCode(kind, index));
    m_world.settle(slot.object);
}

void SceneObjects::bindPoints(Engine& engine) {
    for (const PointKind kind : kPointKinds) {
        std::vector<PointSlot>& slots = points(kind);
        const size_t n = pointCount(engine, kind);
        // Área/som tirado do MEIO da lista (cena relida, vizinho saiu): os
        // índices andaram. As listas são curtas: confere o nome e, se mudou
        // de lugar, refaz o tipo inteiro (as declarações da cena voltam).
        bool shifted = false;
        for (size_t i = 0; i < slots.size() && i < n && kind != PointKind::Light && !shifted; ++i) {
            if (slots[i].name == pointName(engine, kind, i)) continue;
            // Andou (um entrou ou saiu antes daqui) ou foi renomeado no lugar.
            const bool removedBefore = i + 1 < slots.size() && pointName(engine, kind, i) == slots[i + 1].name;
            const bool insertedBefore = i + 1 < n && pointName(engine, kind, i + 1) == slots[i].name;
            shifted = removedBefore || insertedBefore || slots.size() != n;
            if (!shifted) renamedInPlace(engine, kind, i);
        }
        if (shifted) {
            // Área/som que o jogo tirou (objeto destruído) continua tirado e
            // sem objeto: o Stop é quem devolve.
            std::unordered_map<std::string, PointSlot> before;
            for (PointSlot& slot : slots) {
                if (slot.removed) before[slot.name] = slot;
                if (m_world.alive(slot.object)) {
                    store(m_pointOfObject, slot.object, kNone);
                    destroyOwn(slot.object);
                }
            }
            slots.clear();
            for (size_t i = 0; i < n; ++i) {
                const auto was = before.find(pointName(engine, kind, i));
                if (was == before.end()) {
                    bindPoint(engine, kind, i);
                    continue;
                }
                slots.resize(i + 1);
                slots[i] = was->second;
                slots[i].object = {};
            }
            for (const Authored& a : m_authored) applyAuthored(a);
            continue;
        }
        // Lista encolheu (luzes do mapa refeitas, áreas limpas): os objetos saem.
        while (slots.size() > n) {
            if (m_world.alive(slots.back().object)) {
                store(m_pointOfObject, slots.back().object, kNone);
                destroyOwn(slots.back().object);
            }
            slots.pop_back();
        }
        for (size_t i = slots.size(); i < n; ++i) bindPoint(engine, kind, i);
    }
}

// Renomeada no editor (mesmo lugar, nome novo): o objeto e a declaração da
// cena trocam de nome junto.
void SceneObjects::renamedInPlace(Engine& engine, PointKind kind, size_t i) {
    std::vector<PointSlot>& slots = points(kind);
    const std::string now = pointName(engine, kind, i);
    renameAuthored(slots[i].name, now);
    if (m_world.alive(slots[i].object)) m_world.rename(slots[i].object, now);
    slots[i].name = now;
}

void SceneObjects::renameAuthored(const std::string& from, const std::string& to) {
    for (Authored& a : m_authored) {
        if (a.name == from) a.name = to;
        if (a.parent == from) a.parent = to;
    }
}

// Mexida por fora (editor, lights.set_position, mapa vizinho): o objeto vai junto.
void SceneObjects::pullPoints(Engine& engine) {
    for (const PointKind kind : kPointKinds) {
        std::vector<PointSlot>& slots = points(kind);
        for (size_t i = 0; i < slots.size() && i < pointCount(engine, kind); ++i) {
            const Vec3 p = pointPosition(engine, kind, i);
            if (p == slots[i].last || !m_world.alive(slots[i].object)) continue;
            m_world.setWorldPosition(slots[i].object, p);
            slots[i].last = p;
            markPulled(slots[i].object.index);
        }
    }
}

void SceneObjects::pushPoint(Engine& engine, ObjectId id, bool pulled) {
    PointKind kind;
    size_t i;
    if (!pointOf(id, kind, i) || i >= pointCount(engine, kind)) return;
    PointSlot& slot = points(kind)[i];
    if (!pulled) {
        pointPosition(engine, kind, i) = m_world.worldPosition(id);
        slot.last = pointPosition(engine, kind, i);
    }
    if (kind != PointKind::Light) return;
    PointLight& light = engine.deferredLighting().getPointLights()[i];
    const bool on = m_world.activeInHierarchy(id);
    if (!on && !slot.hidden) {
        slot.hidden = 1;
        slot.wasOn = light.enabled ? 1 : 0;
    } else if (on && slot.hidden) {
        slot.hidden = 0;
        light.enabled = slot.wasOn != 0; // a de noite o relógio acerta no próximo quadro
    }
}

// Luz de objeto desligado fica apagada mesmo com o relógio acendendo as da noite.
void SceneObjects::enforceHiddenLights(Engine& engine) {
    auto& lights = engine.deferredLighting().getPointLights();
    for (size_t i = 0; i < m_lightSlots.size() && i < lights.size(); ++i)
        if (m_lightSlots[i].hidden) lights[i].enabled = false;
}

void SceneObjects::releasePoint(Engine& engine, uint32_t code) {
    const auto kind = static_cast<PointKind>(code >> 28);
    const size_t i = code & 0x0FFFFFFFu;
    std::vector<PointSlot>& slots = points(kind);
    if (i >= slots.size() || i >= pointCount(engine, kind)) return;
    PointSlot& slot = slots[i];
    if (kind == PointKind::Light) {
        if (!slot.hidden) slot.wasOn = engine.deferredLighting().getPointLights()[i].enabled ? 1 : 0;
        slot.hidden = 1;
    } else if (kind == PointKind::Area && !engine.areas()[i].removed) {
        engine.areas()[i].removed = true;
        slot.removed = 1;
    } else if (kind == PointKind::Sound && !engine.soundEmitters()[i].removed) {
        engine.soundEmitters()[i].removed = true;
        slot.removed = 1;
    }
    store(m_pointOfObject, slot.object, kNone);
    slot.object = {};
}

// Stop / jogo carregado: luz, área e som cujo objeto o jogo destruiu voltam.
void SceneObjects::restorePoints(Engine& engine) {
    for (const PointKind kind : kPointKinds) {
        std::vector<PointSlot>& slots = points(kind);
        for (size_t i = 0; i < slots.size() && i < pointCount(engine, kind); ++i) {
            if (m_world.alive(slots[i].object)) continue;
            if (slots[i].removed) {
                if (kind == PointKind::Area) engine.areas()[i].removed = false;
                if (kind == PointKind::Sound) engine.soundEmitters()[i].removed = false;
            }
            if (kind == PointKind::Light && slots[i].hidden)
                engine.deferredLighting().getPointLights()[i].enabled = slots[i].wasOn != 0;
            slots[i] = PointSlot{};
            bindPoint(engine, kind, i);
        }
    }
}

} // namespace eruption
