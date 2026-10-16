#include "scene/ObjectWorld.hpp"

#include <algorithm>

namespace eruption {

namespace {

// Matriz de mundo -> posição, rotação e escala. Escala negativa (espelho)
// fica no X.
ObjectWorld::Transform decompose(const Mat4& m) {
    ObjectWorld::Transform t;
    t.position = Vec3(m[3]);
    Vec3 axis[3] = {Vec3(m[0]), Vec3(m[1]), Vec3(m[2])};
    t.scale = Vec3(glm::length(axis[0]), glm::length(axis[1]), glm::length(axis[2]));
    if (glm::dot(glm::cross(axis[0], axis[1]), axis[2]) < 0.0f) t.scale.x = -t.scale.x;
    Mat3 r(1.0f);
    for (int a = 0; a < 3; ++a) {
        const float s = std::abs(t.scale[a]) > 1e-8f ? t.scale[a] : 1.0f;
        r[a] = axis[a] / s;
    }
    t.rotation = glm::normalize(glm::quat_cast(r));
    return t;
}

const std::string kNoName;

} // namespace

ObjectWorld::Transform ObjectWorld::Transform::fromMatrix(const Mat4& m) { return decompose(m); }

Mat4 ObjectWorld::Transform::matrix() const {
    Mat4 m = glm::mat4_cast(rotation);
    m[0] *= scale.x;
    m[1] *= scale.y;
    m[2] *= scale.z;
    m[3] = Vec4(position, 1.0f);
    return m;
}

// ---------------------------------------------------------------------------
// Vida e morte
// ---------------------------------------------------------------------------

bool ObjectWorld::valid(ObjectId id) const {
    return id.generation != 0 && id.index < m_generation.size() && m_generation[id.index] == id.generation &&
           (m_flags[id.index] & kAlive);
}

bool ObjectWorld::alive(ObjectId id) const { return valid(id); }

ObjectId ObjectWorld::idAt(uint32_t index) const {
    if (index >= m_generation.size() || !(m_flags[index] & kAlive)) return {};
    return {index, m_generation[index]};
}

uint32_t ObjectWorld::allocate() {
    if (!m_free.empty()) {
        const uint32_t i = m_free.back();
        m_free.pop_back();
        return i;
    }
    const uint32_t i = static_cast<uint32_t>(m_generation.size());
    m_generation.push_back(1);
    m_flags.push_back(0);
    m_name.emplace_back();
    m_parent.push_back(kNone);
    m_firstChild.push_back(kNone);
    m_nextSibling.push_back(kNone);
    m_prevSibling.push_back(kNone);
    m_local.emplace_back();
    m_world.emplace_back(1.0f);
    m_tags.emplace_back();
    return i;
}

ObjectId ObjectWorld::create(const std::string& name, ObjectId parent, const Transform& local) {
    const uint32_t i = allocate();
    m_flags[i] = kAlive | kActive;
    m_name[i] = name;
    m_parent[i] = m_firstChild[i] = m_nextSibling[i] = m_prevSibling[i] = kNone;
    m_local[i] = local;
    m_tags[i].clear();
    if (valid(parent)) link(i, parent.index);
    m_byName[name].push_back(i);
    ++m_live;
    markDirty(i);
    return {i, m_generation[i]};
}

ObjectId ObjectWorld::create(const std::string& name, ObjectId parent) { return create(name, parent, Transform()); }

size_t ObjectWorld::destroy(ObjectId id) {
    if (!valid(id)) return 0;
    unlink(id.index);
    size_t count = 0;
    destroySubtree(id.index, count);
    return count;
}

void ObjectWorld::destroySubtree(uint32_t root, size_t& count) {
    // Pilha explícita: hierarquia funda não estoura a pilha do programa.
    std::vector<uint32_t> stack{root};
    while (!stack.empty()) {
        const uint32_t i = stack.back();
        stack.pop_back();
        for (uint32_t c = m_firstChild[i]; c != kNone; c = m_nextSibling[c]) stack.push_back(c);
        m_destroyed.push_back({i, m_generation[i]});
        unindexName(i);
        m_flags[i] = 0;
        m_generation[i] = m_generation[i] + 1 == 0 ? 1 : m_generation[i] + 1;
        m_name[i].clear();
        m_tags[i].clear();
        m_parent[i] = m_firstChild[i] = m_nextSibling[i] = m_prevSibling[i] = kNone;
        m_free.push_back(i);
        --m_live;
        ++count;
    }
}

std::vector<ObjectId> ObjectWorld::takeDestroyed() {
    std::vector<ObjectId> out;
    out.swap(m_destroyed);
    return out;
}

// Tudo destruído, mas as vagas guardam a geração: um id de antes do
// clear (num script, numa tabela) nunca passa a valer para um objeto novo.
void ObjectWorld::clear() {
    for (uint32_t i = 0; i < m_generation.size(); ++i) {
        if (!(m_flags[i] & kAlive)) continue;
        m_destroyed.push_back({i, m_generation[i]});
        m_flags[i] = 0;
        m_generation[i] = m_generation[i] + 1 == 0 ? 1 : m_generation[i] + 1;
        m_name[i].clear();
        m_tags[i].clear();
        m_parent[i] = m_firstChild[i] = m_nextSibling[i] = m_prevSibling[i] = kNone;
        m_free.push_back(i);
    }
    m_dirtyList.clear();
    m_changed.clear();
    m_changedPending.clear();
    m_byName.clear();
    m_live = 0;
}

void ObjectWorld::settle(ObjectId id) {
    if (!valid(id)) return;
    const uint32_t p = m_parent[id.index];
    if (p != kNone && (m_flags[p] & kDirty)) return; // o pai ainda vai ser recalculado
    m_world[id.index] = (p == kNone ? Mat4(1.0f) : m_world[p]) * m_local[id.index].matrix();
    m_flags[id.index] &= static_cast<uint8_t>(~kDirty);
}

// ---------------------------------------------------------------------------
// Nome
// ---------------------------------------------------------------------------

const std::string& ObjectWorld::name(ObjectId id) const { return valid(id) ? m_name[id.index] : kNoName; }

void ObjectWorld::unindexName(uint32_t i) {
    auto it = m_byName.find(m_name[i]);
    if (it == m_byName.end()) return;
    auto& v = it->second;
    v.erase(std::remove(v.begin(), v.end(), i), v.end());
    if (v.empty()) m_byName.erase(it);
}

void ObjectWorld::rename(ObjectId id, const std::string& name) {
    if (!valid(id) || m_name[id.index] == name) return;
    unindexName(id.index);
    m_name[id.index] = name;
    m_byName[name].push_back(id.index);
}

ObjectId ObjectWorld::find(const std::string& name) const {
    const auto it = m_byName.find(name);
    // A lista segue a ordem de criação (ou de renomeação): o primeiro é o mais antigo.
    if (it == m_byName.end() || it->second.empty()) return {};
    return idAt(it->second.front());
}

std::vector<ObjectId> ObjectWorld::findAll(const std::string& name) const {
    std::vector<ObjectId> out;
    const auto it = m_byName.find(name);
    if (it == m_byName.end()) return out;
    for (uint32_t i : it->second) out.push_back(idAt(i));
    return out;
}

// ---------------------------------------------------------------------------
// Hierarquia
// ---------------------------------------------------------------------------

void ObjectWorld::link(uint32_t child, uint32_t parent) {
    m_parent[child] = parent;
    m_prevSibling[child] = kNone;
    m_nextSibling[child] = m_firstChild[parent];
    if (m_firstChild[parent] != kNone) m_prevSibling[m_firstChild[parent]] = child;
    m_firstChild[parent] = child;
}

void ObjectWorld::unlink(uint32_t child) {
    const uint32_t p = m_parent[child];
    if (p == kNone) return;
    if (m_prevSibling[child] != kNone) m_nextSibling[m_prevSibling[child]] = m_nextSibling[child];
    else m_firstChild[p] = m_nextSibling[child];
    if (m_nextSibling[child] != kNone) m_prevSibling[m_nextSibling[child]] = m_prevSibling[child];
    m_parent[child] = m_nextSibling[child] = m_prevSibling[child] = kNone;
}

ObjectId ObjectWorld::parent(ObjectId id) const {
    return valid(id) && m_parent[id.index] != kNone ? idAt(m_parent[id.index]) : ObjectId{};
}

ObjectId ObjectWorld::firstChild(ObjectId id) const {
    return valid(id) && m_firstChild[id.index] != kNone ? idAt(m_firstChild[id.index]) : ObjectId{};
}

ObjectId ObjectWorld::nextSibling(ObjectId id) const {
    return valid(id) && m_nextSibling[id.index] != kNone ? idAt(m_nextSibling[id.index]) : ObjectId{};
}

std::vector<ObjectId> ObjectWorld::children(ObjectId id) const {
    std::vector<ObjectId> out;
    if (!valid(id)) return out;
    for (uint32_t c = m_firstChild[id.index]; c != kNone; c = m_nextSibling[c]) out.push_back(idAt(c));
    // A lista ligada guarda o mais novo primeiro; devolve na ordem de criação.
    std::reverse(out.begin(), out.end());
    return out;
}

bool ObjectWorld::isAncestor(ObjectId ancestor, ObjectId id) const {
    if (!valid(ancestor) || !valid(id)) return false;
    for (uint32_t p = m_parent[id.index]; p != kNone; p = m_parent[p])
        if (p == ancestor.index) return true;
    return false;
}

bool ObjectWorld::setParent(ObjectId id, ObjectId parent, bool keepWorld) {
    if (!valid(id)) return false;
    const bool toRoot = !valid(parent);
    if (!toRoot && (parent == id || isAncestor(id, parent))) return false;
    const Mat4 worldBefore = world(id);
    unlink(id.index);
    if (!toRoot) link(id.index, parent.index);
    if (keepWorld) m_local[id.index] = decompose(glm::inverse(parentWorld(id.index)) * worldBefore);
    markDirty(id.index);
    return true;
}

// ---------------------------------------------------------------------------
// Transform
// ---------------------------------------------------------------------------

const ObjectWorld::Transform& ObjectWorld::local(ObjectId id) const {
    static const Transform kIdentity;
    return valid(id) ? m_local[id.index] : kIdentity;
}

void ObjectWorld::setLocal(ObjectId id, const Transform& t) {
    if (!valid(id)) return;
    m_local[id.index] = t;
    markDirty(id.index);
}

void ObjectWorld::setLocalPosition(ObjectId id, const Vec3& p) {
    if (!valid(id)) return;
    m_local[id.index].position = p;
    markDirty(id.index);
}

void ObjectWorld::setLocalRotation(ObjectId id, const Quat& r) {
    if (!valid(id)) return;
    m_local[id.index].rotation = r;
    markDirty(id.index);
}

void ObjectWorld::setLocalScale(ObjectId id, const Vec3& s) {
    if (!valid(id)) return;
    m_local[id.index].scale = s;
    markDirty(id.index);
}

void ObjectWorld::markDirty(uint32_t i) {
    if (m_flags[i] & kDirty) return;
    m_flags[i] |= kDirty;
    m_dirtyList.push_back(i);
}

void ObjectWorld::touch(ObjectId id) {
    if (valid(id)) markChanged(id.index);
}

void ObjectWorld::markChanged(uint32_t i) {
    if (m_flags[i] & kChanged) return;
    m_flags[i] |= kChanged;
    m_changedPending.push_back(i);
}

// Mundo do pai já calculado (ou na hora, se a cadeia dele está suja).
Mat4 ObjectWorld::parentWorld(uint32_t i) const {
    const uint32_t p = m_parent[i];
    return p == kNone ? Mat4(1.0f) : world(idAt(p));
}

Mat4 ObjectWorld::world(ObjectId id) const {
    if (!valid(id)) return Mat4(1.0f);
    // O mais alto sujo da cadeia decide de onde recalcular: acima dele tudo
    // está limpo, então a matriz guardada do pai dele vale. Nada é gravado;
    // quem limpa é updateTransforms.
    uint32_t topDirty = kNone;
    for (uint32_t i = id.index; i != kNone; i = m_parent[i])
        if (m_flags[i] & kDirty) topDirty = i;
    return topDirty == kNone ? m_world[id.index] : composeDown(id.index, topDirty);
}

// Mundo de `i` refeito a partir de `top` (ancestral sujo mais alto de i).
// Sem recursão: hierarquia funda não estoura a pilha.
Mat4 ObjectWorld::composeDown(uint32_t i, uint32_t top) const {
    thread_local std::vector<uint32_t> chain;
    chain.clear();
    for (uint32_t k = i; k != top; k = m_parent[k]) chain.push_back(k);
    const uint32_t p = m_parent[top];
    Mat4 m = (p == kNone ? Mat4(1.0f) : m_world[p]) * m_local[top].matrix();
    for (size_t k = chain.size(); k-- > 0;) m = m * m_local[chain[k]].matrix();
    return m;
}

Vec3 ObjectWorld::worldPosition(ObjectId id) const { return Vec3(world(id)[3]); }

void ObjectWorld::setWorld(ObjectId id, const Mat4& m) {
    if (!valid(id)) return;
    m_local[id.index] = decompose(glm::inverse(parentWorld(id.index)) * m);
    markDirty(id.index);
}

void ObjectWorld::setWorldPosition(ObjectId id, const Vec3& p) {
    if (!valid(id)) return;
    m_local[id.index].position = Vec3(glm::inverse(parentWorld(id.index)) * Vec4(p, 1.0f));
    markDirty(id.index);
}

void ObjectWorld::recomputeSubtree(uint32_t root) {
    std::vector<uint32_t> stack{root};
    while (!stack.empty()) {
        const uint32_t i = stack.back();
        stack.pop_back();
        const uint32_t p = m_parent[i];
        m_world[i] = (p == kNone ? Mat4(1.0f) : m_world[p]) * m_local[i].matrix();
        m_flags[i] &= static_cast<uint8_t>(~kDirty);
        markChanged(i);
        for (uint32_t c = m_firstChild[i]; c != kNone; c = m_nextSibling[c]) stack.push_back(c);
    }
}

const std::vector<uint32_t>& ObjectWorld::updateTransforms() {
    for (size_t k = 0; k < m_dirtyList.size(); ++k) {
        const uint32_t i = m_dirtyList[k];
        if (!(m_flags[i] & kAlive) || !(m_flags[i] & kDirty)) continue;
        // Um ancestral sujo recalcula este junto; começar por ele evita
        // fazer a mesma subárvore duas vezes.
        bool ancestorDirty = false;
        for (uint32_t p = m_parent[i]; p != kNone && !ancestorDirty; p = m_parent[p])
            ancestorDirty = (m_flags[p] & kDirty) != 0;
        if (!ancestorDirty) recomputeSubtree(i);
    }
    m_dirtyList.clear();
    m_changed.clear();
    // A mesma vaga pode aparecer duas vezes (destruída e reaproveitada no
    // quadro); a marca kChanged deixa só a primeira passar.
    for (uint32_t i : m_changedPending) {
        if (!(m_flags[i] & kChanged)) continue;
        m_flags[i] &= static_cast<uint8_t>(~kChanged);
        if (m_flags[i] & kAlive) m_changed.push_back(i);
    }
    m_changedPending.clear();
    return m_changed;
}

// ---------------------------------------------------------------------------
// Ligado / desligado
// ---------------------------------------------------------------------------

void ObjectWorld::setActive(ObjectId id, bool on) {
    if (!valid(id) || active(id) == on) return;
    if (on) m_flags[id.index] |= kActive;
    else m_flags[id.index] &= static_cast<uint8_t>(~kActive);
    // A subárvore inteira muda de estado efetivo: avisa os componentes.
    std::vector<uint32_t> stack{id.index};
    while (!stack.empty()) {
        const uint32_t i = stack.back();
        stack.pop_back();
        markChanged(i);
        for (uint32_t c = m_firstChild[i]; c != kNone; c = m_nextSibling[c]) stack.push_back(c);
    }
}

bool ObjectWorld::active(ObjectId id) const { return valid(id) && (m_flags[id.index] & kActive); }

bool ObjectWorld::activeInHierarchy(ObjectId id) const {
    if (!valid(id)) return false;
    for (uint32_t i = id.index; i != kNone; i = m_parent[i])
        if (!(m_flags[i] & kActive)) return false;
    return true;
}

// ---------------------------------------------------------------------------
// Etiquetas
// ---------------------------------------------------------------------------

uint32_t ObjectWorld::internTag(const std::string& tag) {
    const auto it = m_tagIds.find(tag);
    if (it != m_tagIds.end()) return it->second;
    const uint32_t t = static_cast<uint32_t>(m_tagNames.size());
    m_tagNames.push_back(tag);
    m_tagIds.emplace(tag, t);
    return t;
}

void ObjectWorld::addTag(ObjectId id, const std::string& tag) {
    if (!valid(id)) return;
    const uint32_t t = internTag(tag);
    auto& v = m_tags[id.index];
    if (std::find(v.begin(), v.end(), t) == v.end()) v.push_back(t);
}

void ObjectWorld::removeTag(ObjectId id, const std::string& tag) {
    if (!valid(id)) return;
    const auto it = m_tagIds.find(tag);
    if (it == m_tagIds.end()) return;
    auto& v = m_tags[id.index];
    v.erase(std::remove(v.begin(), v.end(), it->second), v.end());
}

bool ObjectWorld::hasTag(ObjectId id, const std::string& tag) const {
    if (!valid(id)) return false;
    const auto it = m_tagIds.find(tag);
    if (it == m_tagIds.end()) return false;
    const auto& v = m_tags[id.index];
    return std::find(v.begin(), v.end(), it->second) != v.end();
}

std::vector<std::string> ObjectWorld::tags(ObjectId id) const {
    std::vector<std::string> out;
    if (!valid(id)) return out;
    for (uint32_t t : m_tags[id.index]) out.push_back(m_tagNames[t]);
    return out;
}

std::vector<ObjectId> ObjectWorld::findByTag(const std::string& tag) const {
    std::vector<ObjectId> out;
    const auto it = m_tagIds.find(tag);
    if (it == m_tagIds.end()) return out;
    for (uint32_t i = 0; i < m_generation.size(); ++i) {
        if (!(m_flags[i] & kAlive)) continue;
        const auto& v = m_tags[i];
        if (std::find(v.begin(), v.end(), it->second) != v.end()) out.push_back({i, m_generation[i]});
    }
    return out;
}

std::vector<ObjectId> ObjectWorld::all() const {
    std::vector<ObjectId> out;
    out.reserve(m_live);
    for (uint32_t i = 0; i < m_generation.size(); ++i)
        if (m_flags[i] & kAlive) out.push_back({i, m_generation[i]});
    return out;
}

std::vector<ObjectId> ObjectWorld::tagged() const {
    std::vector<ObjectId> out;
    for (uint32_t i = 0; i < m_generation.size(); ++i)
        if ((m_flags[i] & kAlive) && !m_tags[i].empty()) out.push_back({i, m_generation[i]});
    return out;
}

} // namespace eruption
