#include "editor/RuleSheet.hpp"

#include <Luau/Ast.h>
#include <Luau/Parser.h>

#include <sstream>

namespace eruption {

namespace {

using namespace Luau;

// Converte posições (linha, coluna) do analisador em trechos do texto.
class SourceSlicer {
public:
    explicit SourceSlicer(const std::string& src) : m_src(src) {
        m_lineStart.push_back(0);
        for (size_t i = 0; i < src.size(); ++i)
            if (src[i] == '\n') m_lineStart.push_back(i + 1);
    }
    size_t offset(const Position& p) const {
        if (p.line >= m_lineStart.size()) return m_src.size();
        return std::min(m_src.size(), m_lineStart[p.line] + p.column);
    }
    std::string slice(const Location& l) const {
        const size_t a = offset(l.begin), b = offset(l.end);
        return b > a ? m_src.substr(a, b - a) : std::string();
    }

private:
    const std::string& m_src;
    std::vector<size_t> m_lineStart;
};

// "ns.fn" de uma chamada `ns.fn(...)` ou "fn" de `fn(...)`.
std::string calleeName(AstExprCall* call) {
    if (auto* idx = call->func->as<AstExprIndexName>())
        if (auto* g = idx->expr->as<AstExprGlobal>()) return std::string(g->name.value) + "." + idx->index.value;
    if (auto* g = call->func->as<AstExprGlobal>()) return g->name.value;
    return {};
}

RuleCall rawCall(const std::string& code) {
    RuleCall c;
    c.code = code;
    return c;
}

// Chamada do catálogo com o tipo esperado e o número certo de argumentos
// (argumentos opcionais no fim podem faltar: entram com o valor padrão).
bool matchCall(AstExprCall* call, RuleKind kind, const SourceSlicer& src, RuleCall& out) {
    const RuleDef* def = findRuleDef(calleeName(call));
    if (!def || def->kind != kind || call->self) return false;
    if (call->args.size > def->params.size()) return false;
    out = RuleCall{};
    out.def = def;
    for (size_t i = 0; i < def->params.size(); ++i)
        out.args.push_back(i < call->args.size ? src.slice(call->args.data[i]->location) : def->params[i].defaultCode);
    return true;
}

const char* compareOp(AstExprBinary::Op op) {
    switch (op) {
        case AstExprBinary::CompareEq: return "==";
        case AstExprBinary::CompareNe: return "~=";
        case AstExprBinary::CompareLt: return "<";
        case AstExprBinary::CompareLe: return "<=";
        case AstExprBinary::CompareGt: return ">";
        case AstExprBinary::CompareGe: return ">=";
        default: return nullptr;
    }
}

void collectConditions(AstExpr* e, const SourceSlicer& src, std::vector<RuleCall>& out) {
    if (auto* g = e->as<AstExprGroup>()) return collectConditions(g->expr, src, out);
    if (auto* b = e->as<AstExprBinary>()) {
        if (b->op == AstExprBinary::And) {
            collectConditions(b->left, src, out);
            collectConditions(b->right, src, out);
            return;
        }
        if (const char* op = compareOp(b->op)) {
            auto* call = b->left->as<AstExprCall>();
            if (call && calleeName(call) == "vars.get" && call->args.size == 1) {
                RuleCall c;
                c.def = findRuleDef(kVarCompareId);
                c.op = op;
                c.args = {src.slice(call->args.data[0]->location), src.slice(b->right->location)};
                out.push_back(std::move(c));
                return;
            }
        }
    }
    bool negate = false;
    AstExpr* inner = e;
    if (auto* u = e->as<AstExprUnary>(); u && u->op == AstExprUnary::Op::Not) {
        negate = true;
        inner = u->expr;
        if (auto* g = inner->as<AstExprGroup>()) inner = g->expr;
    }
    RuleCall c;
    if (auto* call = inner->as<AstExprCall>(); call && matchCall(call, RuleKind::Condition, src, c)) {
        c.negate = negate;
        out.push_back(std::move(c));
        return;
    }
    out.push_back(rawCall(src.slice(e->location)));
}

void collectActions(AstStatBlock* block, const SourceSlicer& src, std::vector<RuleCall>& out) {
    for (size_t i = 0; i < block->body.size; ++i) {
        AstStat* st = block->body.data[i];
        RuleCall c;
        if (auto* se = st->as<AstStatExpr>())
            if (auto* call = se->expr->as<AstExprCall>(); call && matchCall(call, RuleKind::Action, src, c)) {
                out.push_back(std::move(c));
                continue;
            }
        out.push_back(rawCall(src.slice(st->location)));
    }
}

bool matchRule(AstStat* st, const SourceSlicer& src, Rule& rule) {
    auto* se = st->as<AstStatExpr>();
    auto* call = se ? se->expr->as<AstExprCall>() : nullptr;
    if (!call || call->args.size == 0) return false;
    const RuleDef* def = findRuleDef(calleeName(call));
    if (!def || def->kind != RuleKind::Trigger || call->args.size != def->params.size() + 1) return false;
    auto* fn = call->args.data[call->args.size - 1]->as<AstExprFunction>();
    if (!fn) return false;

    rule = Rule{};
    rule.trigger.def = def;
    for (size_t i = 0; i < def->params.size(); ++i) rule.trigger.args.push_back(src.slice(call->args.data[i]->location));

    AstStatBlock* body = fn->body;
    if (body->body.size == 1)
        if (auto* ifs = body->body.data[0]->as<AstStatIf>()) {
            // if ... then ... [else ...] end, sem elseif
            if (!ifs->elsebody || ifs->elsebody->is<AstStatBlock>()) {
                collectConditions(ifs->condition, src, rule.conditions);
                collectActions(ifs->thenbody, src, rule.actions);
                if (ifs->elsebody) collectActions(ifs->elsebody->as<AstStatBlock>(), src, rule.otherwise);
                return true;
            }
        }
    collectActions(body, src, rule.actions);
    return true;
}

void emitCall(std::ostringstream& o, const RuleCall& c) {
    if (!c.def) {
        o << c.code;
        return;
    }
    if (c.def->id == std::string(kVarCompareId)) {
        o << "vars.get(" << c.args[0] << ") " << c.op << " " << c.args[1];
        return;
    }
    if (c.negate) o << "not ";
    o << c.def->id << "(";
    for (size_t i = 0; i < c.args.size(); ++i) o << (i ? ", " : "") << c.args[i];
    o << ")";
}

void emitBlock(std::ostringstream& o, const std::vector<RuleCall>& calls, const std::string& indent) {
    for (const auto& c : calls) {
        // Código cru de várias linhas mantém a indentação relativa.
        std::istringstream lines(c.def ? std::string() : c.code);
        if (!c.def) {
            std::string line;
            bool first = true;
            while (std::getline(lines, line)) {
                o << (first ? indent : std::string()) << line << "\n";
                first = false;
            }
            continue;
        }
        o << indent;
        emitCall(o, c);
        o << "\n";
    }
}

} // namespace

RuleCall RuleSheet::makeCall(const RuleDef& def) {
    RuleCall c;
    c.def = &def;
    for (const auto& p : def.params) c.args.push_back(p.defaultCode);
    return c;
}

bool RuleSheet::parse(const std::string& source, std::string& error) {
    Allocator allocator;
    AstNameTable names(allocator);
    ParseResult result = Parser::parse(source.data(), source.size(), names, allocator);
    if (!result.errors.empty()) {
        const ParseError& e = result.errors.front();
        error = "line " + std::to_string(e.getLocation().begin.line + 1) + ": " + e.getMessage();
        return false;
    }
    SourceSlicer src(source);

    // Comentários do topo, até a primeira linha de código.
    header.clear();
    std::istringstream in(source);
    std::string line;
    while (std::getline(in, line)) {
        const size_t first = line.find_first_not_of(" \t\r");
        if (first == std::string::npos || line.compare(first, 2, "--") != 0) break;
        header += line + "\n";
    }

    rules.clear();
    for (size_t i = 0; i < result.root->body.size; ++i) {
        AstStat* st = result.root->body.data[i];
        Rule r;
        if (matchRule(st, src, r)) {
            rules.push_back(std::move(r));
        } else {
            Rule raw;
            raw.raw = true;
            raw.code = src.slice(st->location);
            rules.push_back(std::move(raw));
        }
    }
    return true;
}

std::string RuleSheet::generate() const {
    std::ostringstream o;
    o << (header.empty() ? "--!strict\n-- Rule sheet: edit it in the editor or here.\n" : header);
    for (const auto& r : rules) {
        o << "\n";
        if (r.raw) {
            o << r.code << "\n";
            continue;
        }
        o << r.trigger.def->id << "(";
        for (const auto& a : r.trigger.args) o << a << ", ";
        o << (r.trigger.def->id == std::string("on.update") ? "function(dt: number)\n" : "function()\n");
        if (r.conditions.empty()) {
            emitBlock(o, r.actions, "    ");
        } else {
            o << "    if ";
            for (size_t i = 0; i < r.conditions.size(); ++i) {
                if (i) o << " and ";
                const bool group = !r.conditions[i].def && r.conditions.size() > 1;
                if (group) o << "(";
                emitCall(o, r.conditions[i]);
                if (group) o << ")";
            }
            o << " then\n";
            emitBlock(o, r.actions, "        ");
            if (!r.otherwise.empty()) {
                o << "    else\n";
                emitBlock(o, r.otherwise, "        ");
            }
            o << "    end\n";
        }
        o << "end)\n";
    }
    return o.str();
}

std::string ruleArgDisplay(ParamType type, const std::string& code, UiLanguage lang) {
    if (code.size() >= 2 && code.front() == '"' && code.back() == '"') return code.substr(1, code.size() - 2);
    if (type == ParamType::Offset && code.rfind("vector.create(", 0) == 0 && code.back() == ')')
        return "(" + code.substr(14, code.size() - 15) + ")";
    if (code == "true") return ruleUi("yes", lang);
    if (code == "false") return ruleUi("no", lang);
    return code;
}

} // namespace eruption
