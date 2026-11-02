#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace eruption {

// Dados de um evento de rede (`dados` de net.send/net.ask): número, bool,
// string curta ou tabela rasa desses. Sem Lua aqui: ScriptApiNet converte.
// A decodificação é parser de bytes vindos de fora: tudo tem teto (tamanho
// total, string, entradas da tabela, sem tabela dentro de tabela).
struct NetValue {
    enum class Type : uint8_t { Nil, Bool, Number, String, Table };
    Type type = Type::Nil;
    bool boolean = false;
    double number = 0.0;
    std::string string;
    std::vector<std::pair<NetValue, NetValue>> table; // chave (número ou string) -> valor escalar

    static NetValue makeNumber(double v);
    static NetValue makeString(std::string v);
    static NetValue makeBool(bool v);
};

constexpr size_t kNetValueMaxString = 200;
constexpr size_t kNetValueMaxEntries = 32;

// Falso se o valor não cabe nas regras (tabela aninhada, chave inválida,
// número não finito, string longa) ou em maxBytes.
bool encodeNetValue(const NetValue& value, std::vector<uint8_t>& out, size_t maxBytes);
bool decodeNetValue(const uint8_t* data, size_t bytes, NetValue& out);

} // namespace eruption
