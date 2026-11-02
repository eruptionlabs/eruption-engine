#include "net/NetValue.hpp"

#include "net/Wire.hpp"

#include <cmath>
#include <cstring>

namespace eruption {

namespace {

constexpr int kTypeBits = 3;

void writeDouble(BitWriter& w, double v) {
    uint64_t bits;
    std::memcpy(&bits, &v, 8);
    w.writeBits(static_cast<uint32_t>(bits), 32);
    w.writeBits(static_cast<uint32_t>(bits >> 32), 32);
}

double readDouble(BitReader& r) {
    const uint64_t lo = r.readBits(32);
    const uint64_t hi = r.readBits(32);
    const uint64_t bits = lo | (hi << 32);
    double v;
    std::memcpy(&v, &bits, 8);
    if (!std::isfinite(v)) r.fail();
    return v;
}

bool writeScalar(BitWriter& w, const NetValue& v) {
    w.writeBits(static_cast<uint32_t>(v.type), kTypeBits);
    switch (v.type) {
        case NetValue::Type::Nil: return true;
        case NetValue::Type::Bool: w.writeBool(v.boolean); return true;
        case NetValue::Type::Number:
            if (!std::isfinite(v.number)) return false;
            writeDouble(w, v.number);
            return true;
        case NetValue::Type::String:
            if (v.string.size() > kNetValueMaxString) return false;
            w.writeString(v.string, kNetValueMaxString);
            return true;
        case NetValue::Type::Table: return false;
    }
    return false;
}

bool readScalar(BitReader& r, NetValue& v) {
    const uint32_t t = r.readBits(kTypeBits);
    v = NetValue();
    switch (t) {
        case 0: return r.ok();
        case 1: v.type = NetValue::Type::Bool; v.boolean = r.readBool(); return r.ok();
        case 2: v.type = NetValue::Type::Number; v.number = readDouble(r); return r.ok();
        case 3: v.type = NetValue::Type::String; return r.readString(v.string, kNetValueMaxString);
        default: r.fail(); return false; // tabela dentro de tabela ou tipo desconhecido
    }
}

bool validKey(const NetValue& k) {
    return k.type == NetValue::Type::Number || k.type == NetValue::Type::String;
}

} // namespace

NetValue NetValue::makeNumber(double v) {
    NetValue n;
    n.type = Type::Number;
    n.number = v;
    return n;
}

NetValue NetValue::makeString(std::string v) {
    NetValue n;
    n.type = Type::String;
    n.string = std::move(v);
    return n;
}

NetValue NetValue::makeBool(bool v) {
    NetValue n;
    n.type = Type::Bool;
    n.boolean = v;
    return n;
}

bool encodeNetValue(const NetValue& value, std::vector<uint8_t>& out, size_t maxBytes) {
    out.assign(maxBytes, 0);
    BitWriter w(out.data(), out.size());
    if (value.type == NetValue::Type::Table) {
        if (value.table.size() > kNetValueMaxEntries) return false;
        w.writeBits(static_cast<uint32_t>(NetValue::Type::Table), kTypeBits);
        w.writeInt(static_cast<int32_t>(value.table.size()), 0, static_cast<int32_t>(kNetValueMaxEntries));
        for (const auto& kv : value.table) {
            if (!validKey(kv.first) || kv.second.type == NetValue::Type::Table || kv.second.type == NetValue::Type::Nil)
                return false;
            if (!writeScalar(w, kv.first) || !writeScalar(w, kv.second)) return false;
        }
    } else if (!writeScalar(w, value)) {
        return false;
    }
    const size_t n = w.finish();
    if (!w.ok()) return false;
    out.resize(n);
    return true;
}

bool decodeNetValue(const uint8_t* data, size_t bytes, NetValue& out) {
    BitReader r(data, bytes);
    out = NetValue();
    const uint32_t t = r.readBits(kTypeBits);
    if (t == static_cast<uint32_t>(NetValue::Type::Table)) {
        out.type = NetValue::Type::Table;
        const int32_t count = r.readInt(0, static_cast<int32_t>(kNetValueMaxEntries));
        for (int32_t i = 0; i < count && r.ok(); ++i) {
            NetValue k, v;
            if (!readScalar(r, k) || !validKey(k) || !readScalar(r, v) || v.type == NetValue::Type::Nil) return false;
            out.table.emplace_back(std::move(k), std::move(v));
        }
    } else {
        BitReader again(data, bytes);
        if (!readScalar(again, out)) return false;
        r = again;
    }
    // Sobra só o enchimento do último byte (menos de 8 bits).
    return r.ok() && r.bitsRemaining() < 8;
}

} // namespace eruption
