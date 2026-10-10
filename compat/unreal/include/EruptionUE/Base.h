#pragma once

// Subconjunto proprio do "CoreMinimal" para compilar codigo de plugins de
// Unreal Engine contra a camada de compatibilidade (compatibilidade de
// FONTE: o plugin e' recompilado; nao ha' compatibilidade binaria).
//
// Implementacao nossa, escrita a partir da superficie publica da API
// (nomes de tipos, metodos e macros que o codigo de plugin usa). Cobre
// tipos basicos, FString/FName/FText, TArray/TMap, vetores e FMath, log,
// asserts e as macros de reflexao (que aqui viram nada ou o que a
// ferramenta compat/unreal/ferramentas/compilar_plugin.py gera).

#include "EruptionUE/Host.h"

#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <math.h>
#include <random>
#include <string>
#include <utility>
#include <vector>

// ---- Tipos basicos -------------------------------------------------------------

typedef uint8_t uint8;
typedef uint16_t uint16;
typedef uint32_t uint32;
typedef uint64_t uint64;
typedef int8_t int8;
typedef int16_t int16;
typedef int32_t int32;
typedef int64_t int64;
typedef char ANSICHAR;
typedef char16_t TCHAR;
typedef char16_t UTF16CHAR;
typedef char UTF8CHAR;
typedef size_t SIZE_T;

#define TEXT(x) u##x
#define FORCEINLINE inline
#define FORCENOINLINE __attribute__((noinline))
#define DLLEXPORT __attribute__((visibility("default")))
#define DLLIMPORT
#define CORE_API
#define COREUOBJECT_API
#define ENGINE_API
#define WITH_EDITOR 0
#define WITH_EDITORONLY_DATA 0
#define UE_BUILD_SHIPPING 0
#define PLATFORM_LINUX 1
#define PLATFORM_WINDOWS 0
#define PLATFORM_MAC 0
#define PLATFORM_ANDROID 0
#define PLATFORM_IOS 0

#define INDEX_NONE (-1)
#define UE_PI (3.1415926535897932f)
#define PI UE_PI
#define UE_HALF_PI (1.57079632679f)
#define HALF_PI UE_HALF_PI
#define UE_TWO_PI (6.28318530717f)
#define UE_SMALL_NUMBER (1.e-8f)
#define SMALL_NUMBER UE_SMALL_NUMBER
#define UE_KINDA_SMALL_NUMBER (1.e-4f)
#define KINDA_SMALL_NUMBER UE_KINDA_SMALL_NUMBER
#define UE_BIG_NUMBER (3.4e+38f)
#define BIG_NUMBER UE_BIG_NUMBER
#define MAX_int32 (0x7fffffff)
#define MIN_int32 (-0x7fffffff - 1)
#define MAX_uint8 (0xff)
#define MAX_flt (3.402823466e+38F)

namespace EruptionUE {

inline std::string ParaUtf8(const char16_t* s) {
    std::string out;
    if (!s) return out;
    for (; *s; ++s) {
        uint32_t c = *s;
        if (c >= 0xD800 && c <= 0xDBFF && s[1] >= 0xDC00 && s[1] <= 0xDFFF) {
            c = 0x10000 + ((c - 0xD800) << 10) + (static_cast<uint32_t>(s[1]) - 0xDC00);
            ++s;
        }
        if (c < 0x80) {
            out.push_back(static_cast<char>(c));
        } else if (c < 0x800) {
            out.push_back(static_cast<char>(0xC0 | (c >> 6)));
            out.push_back(static_cast<char>(0x80 | (c & 0x3F)));
        } else if (c < 0x10000) {
            out.push_back(static_cast<char>(0xE0 | (c >> 12)));
            out.push_back(static_cast<char>(0x80 | ((c >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (c & 0x3F)));
        } else {
            out.push_back(static_cast<char>(0xF0 | (c >> 18)));
            out.push_back(static_cast<char>(0x80 | ((c >> 12) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | ((c >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (c & 0x3F)));
        }
    }
    return out;
}

inline std::u16string DeUtf8(const char* s) {
    std::u16string out;
    if (!s) return out;
    const unsigned char* p = reinterpret_cast<const unsigned char*>(s);
    while (*p) {
        uint32_t c = *p++;
        int extra = c >= 0xF0 ? 3 : c >= 0xE0 ? 2 : c >= 0xC0 ? 1 : 0;
        if (extra) c &= (0x3F >> extra);
        for (int k = 0; k < extra && *p; ++k) c = (c << 6) | (*p++ & 0x3F);
        if (c >= 0x10000) {
            c -= 0x10000;
            out.push_back(static_cast<char16_t>(0xD800 + (c >> 10)));
            out.push_back(static_cast<char16_t>(0xDC00 + (c & 0x3FF)));
        } else {
            out.push_back(static_cast<char16_t>(c));
        }
    }
    return out;
}

// Formatacao estilo printf com formato TCHAR; %s recebe const TCHAR*.
inline std::u16string FormatarV(const TCHAR* fmt, va_list args) {
    std::string out;
    const std::string f = ParaUtf8(fmt);
    for (size_t i = 0; i < f.size(); ++i) {
        if (f[i] != '%') {
            out.push_back(f[i]);
            continue;
        }
        size_t j = i + 1;
        while (j < f.size() && std::strchr("-+ #0123456789.*", f[j])) ++j;
        std::string mods;
        while (j < f.size() && std::strchr("hlLqjzt", f[j])) mods.push_back(f[j++]);
        if (j >= f.size()) break;
        const char conv = f[j];
        const std::string spec = f.substr(i, j - i - mods.size());
        char buf[256];
        switch (conv) {
        case '%': out.push_back('%'); break;
        case 's': {
            const TCHAR* s = va_arg(args, const TCHAR*);
            out += ParaUtf8(s ? s : u"(null)");
            break;
        }
        case 'c': out += ParaUtf8(std::u16string(1, static_cast<char16_t>(va_arg(args, int))).c_str()); break;
        case 'd':
        case 'i':
            if (mods.find('l') != std::string::npos || mods.find('j') != std::string::npos || mods.find('z') != std::string::npos)
                std::snprintf(buf, sizeof(buf), (spec + "lld").c_str(), static_cast<long long>(va_arg(args, long long)));
            else std::snprintf(buf, sizeof(buf), (spec + "d").c_str(), va_arg(args, int));
            out += buf;
            break;
        case 'u':
        case 'x':
        case 'X':
        case 'o':
            if (mods.find('l') != std::string::npos || mods.find('z') != std::string::npos)
                std::snprintf(buf, sizeof(buf), (spec + "ll" + conv).c_str(), static_cast<unsigned long long>(va_arg(args, unsigned long long)));
            else std::snprintf(buf, sizeof(buf), (spec + conv).c_str(), va_arg(args, unsigned int));
            out += buf;
            break;
        case 'f':
        case 'F':
        case 'e':
        case 'E':
        case 'g':
        case 'G':
            std::snprintf(buf, sizeof(buf), (spec + conv).c_str(), va_arg(args, double));
            out += buf;
            break;
        case 'p':
            std::snprintf(buf, sizeof(buf), "%p", va_arg(args, void*));
            out += buf;
            break;
        default: out += f.substr(i, j - i + 1); break;
        }
        i = j;
    }
    return DeUtf8(out.c_str());
}

} // namespace EruptionUE

// ---- FString ---------------------------------------------------------------------

template <typename T>
class TArray;

class FString {
public:
    FString() = default;
    FString(const TCHAR* s) : m_s(s ? s : u"") {}
    FString(const ANSICHAR* s) : m_s(EruptionUE::DeUtf8(s)) {}
    explicit FString(std::u16string s) : m_s(std::move(s)) {}
    FString(int32 n, const TCHAR* s) : m_s(s ? std::u16string(s, static_cast<size_t>(n)) : std::u16string()) {}

    const TCHAR* operator*() const { return m_s.c_str(); }
    int32 Len() const { return static_cast<int32>(m_s.size()); }
    bool IsEmpty() const { return m_s.empty(); }
    void Empty() { m_s.clear(); }
    void Reset() { m_s.clear(); }
    TCHAR& operator[](int32 i) { return m_s[static_cast<size_t>(i)]; }
    const TCHAR& operator[](int32 i) const { return m_s[static_cast<size_t>(i)]; }
    const std::u16string& Std() const { return m_s; }

    FString& Append(const FString& o) { m_s += o.m_s; return *this; }
    FString& AppendChar(TCHAR c) { m_s.push_back(c); return *this; }
    FString& operator+=(const FString& o) { return Append(o); }
    FString& operator+=(const TCHAR* o) { m_s += o; return *this; }
    FString& operator+=(TCHAR c) { m_s.push_back(c); return *this; }
    friend FString operator+(FString a, const FString& b) { return a += b; }
    friend FString operator+(FString a, const TCHAR* b) { return a += b; }
    friend FString operator+(const TCHAR* a, const FString& b) { return FString(a) += b; }
    bool operator==(const FString& o) const { return m_s == o.m_s; }
    bool operator!=(const FString& o) const { return m_s != o.m_s; }
    bool operator<(const FString& o) const { return m_s < o.m_s; }
    bool Equals(const FString& o) const { return m_s == o.m_s; }

    FString ToUpper() const {
        std::u16string r = m_s;
        for (auto& c : r) if (c >= u'a' && c <= u'z') c = static_cast<TCHAR>(c - 32);
        return FString(r);
    }
    FString ToLower() const {
        std::u16string r = m_s;
        for (auto& c : r) if (c >= u'A' && c <= u'Z') c = static_cast<TCHAR>(c + 32);
        return FString(r);
    }
    bool Contains(const FString& o) const { return m_s.find(o.m_s) != std::u16string::npos; }
    bool StartsWith(const FString& o) const { return m_s.rfind(o.m_s, 0) == 0; }
    bool EndsWith(const FString& o) const {
        return m_s.size() >= o.m_s.size() && m_s.compare(m_s.size() - o.m_s.size(), o.m_s.size(), o.m_s) == 0;
    }
    int32 Find(const FString& o) const {
        const size_t p = m_s.find(o.m_s);
        return p == std::u16string::npos ? INDEX_NONE : static_cast<int32>(p);
    }
    FString Left(int32 n) const { return FString(m_s.substr(0, static_cast<size_t>(std::max(0, std::min(n, Len()))))); }
    FString Right(int32 n) const {
        const int32 k = std::max(0, std::min(n, Len()));
        return FString(m_s.substr(m_s.size() - static_cast<size_t>(k)));
    }
    FString Mid(int32 ini, int32 n = MAX_int32) const {
        if (ini >= Len()) return FString();
        return FString(m_s.substr(static_cast<size_t>(std::max(0, ini)), static_cast<size_t>(std::max(0, n))));
    }
    FString Replace(const TCHAR* de, const TCHAR* para) const {
        std::u16string r = m_s, a = de, b = para;
        if (a.empty()) return FString(r);
        for (size_t p = r.find(a); p != std::u16string::npos; p = r.find(a, p + b.size())) r.replace(p, a.size(), b);
        return FString(r);
    }
    FString TrimStartAndEnd() const {
        size_t i = 0, j = m_s.size();
        while (i < j && m_s[i] <= u' ') ++i;
        while (j > i && m_s[j - 1] <= u' ') --j;
        return FString(m_s.substr(i, j - i));
    }
    int32 ParseIntoArray(TArray<FString>& out, const TCHAR* sep, bool cullEmpty = true) const;

    static FString Printf(const TCHAR* fmt, ...) {
        va_list a;
        va_start(a, fmt);
        FString r(EruptionUE::FormatarV(fmt, a));
        va_end(a);
        return r;
    }
    static FString FromInt(int32 n) { return FString(EruptionUE::DeUtf8(std::to_string(n).c_str())); }
    static FString SanitizeFloat(double d) {
        char b[64];
        std::snprintf(b, sizeof(b), "%.6g", d);
        std::string s = b;
        if (s.find_first_of(".en") == std::string::npos) s += ".0";
        return FString(b[0] ? s.c_str() : "0.0");
    }

private:
    std::u16string m_s;
};

namespace EruptionUE {
struct FUtf8Temp {
    explicit FUtf8Temp(const TCHAR* s) : Texto(ParaUtf8(s)) {}
    const char* Get() const { return Texto.c_str(); }
    std::string Texto;
};
struct FTcharTemp {
    explicit FTcharTemp(const char* s) : Texto(DeUtf8(s)) {}
    const TCHAR* Get() const { return Texto.c_str(); }
    std::u16string Texto;
};
} // namespace EruptionUE

#define TCHAR_TO_UTF8(s) (EruptionUE::FUtf8Temp(s).Get())
#define TCHAR_TO_ANSI(s) (EruptionUE::FUtf8Temp(s).Get())
#define UTF8_TO_TCHAR(s) (EruptionUE::FTcharTemp(s).Get())
#define ANSI_TO_TCHAR(s) (EruptionUE::FTcharTemp(s).Get())

// ---- Containers ----------------------------------------------------------------------

template <typename T>
class TArray {
public:
    TArray() = default;
    TArray(std::initializer_list<T> l) : m_v(l) {}
    int32 Num() const { return static_cast<int32>(m_v.size()); }
    bool IsEmpty() const { return m_v.empty(); }
    bool IsValidIndex(int32 i) const { return i >= 0 && i < Num(); }
    T& operator[](int32 i) { return m_v[static_cast<size_t>(i)]; }
    const T& operator[](int32 i) const { return m_v[static_cast<size_t>(i)]; }
    int32 Add(const T& x) { m_v.push_back(x); return Num() - 1; }
    int32 Add(T&& x) { m_v.push_back(std::move(x)); return Num() - 1; }
    template <typename... A>
    int32 Emplace(A&&... a) { m_v.emplace_back(std::forward<A>(a)...); return Num() - 1; }
    int32 AddUnique(const T& x) {
        const int32 i = Find(x);
        return i != INDEX_NONE ? i : Add(x);
    }
    void Append(const TArray& o) { m_v.insert(m_v.end(), o.m_v.begin(), o.m_v.end()); }
    void Insert(const T& x, int32 i) { m_v.insert(m_v.begin() + i, x); }
    void RemoveAt(int32 i, int32 n = 1) { m_v.erase(m_v.begin() + i, m_v.begin() + i + n); }
    int32 Remove(const T& x) {
        const size_t antes = m_v.size();
        m_v.erase(std::remove(m_v.begin(), m_v.end(), x), m_v.end());
        return static_cast<int32>(antes - m_v.size());
    }
    bool Contains(const T& x) const { return std::find(m_v.begin(), m_v.end(), x) != m_v.end(); }
    int32 Find(const T& x) const {
        auto it = std::find(m_v.begin(), m_v.end(), x);
        return it == m_v.end() ? INDEX_NONE : static_cast<int32>(it - m_v.begin());
    }
    void Empty(int32 = 0) { m_v.clear(); }
    void Reset(int32 = 0) { m_v.clear(); }
    void SetNum(int32 n) { m_v.resize(static_cast<size_t>(n)); }
    void Reserve(int32 n) { m_v.reserve(static_cast<size_t>(n)); }
    T& Last(int32 k = 0) { return m_v[m_v.size() - 1 - static_cast<size_t>(k)]; }
    const T& Last(int32 k = 0) const { return m_v[m_v.size() - 1 - static_cast<size_t>(k)]; }
    T& Top() { return m_v.back(); }
    T Pop() {
        T x = std::move(m_v.back());
        m_v.pop_back();
        return x;
    }
    void Sort() { std::sort(m_v.begin(), m_v.end()); }
    template <typename P>
    void Sort(P p) { std::sort(m_v.begin(), m_v.end(), p); }
    T* GetData() { return m_v.data(); }
    const T* GetData() const { return m_v.data(); }
    auto begin() { return m_v.begin(); }
    auto end() { return m_v.end(); }
    auto begin() const { return m_v.begin(); }
    auto end() const { return m_v.end(); }
    bool operator==(const TArray& o) const { return m_v == o.m_v; }

private:
    std::vector<T> m_v;
};

inline int32 FString::ParseIntoArray(TArray<FString>& out, const TCHAR* sep, bool cullEmpty) const {
    out.Empty();
    const std::u16string s(sep);
    size_t ini = 0;
    while (true) {
        const size_t p = s.empty() ? std::u16string::npos : m_s.find(s, ini);
        const std::u16string parte = m_s.substr(ini, p == std::u16string::npos ? std::u16string::npos : p - ini);
        if (!parte.empty() || !cullEmpty) out.Add(FString(parte));
        if (p == std::u16string::npos) break;
        ini = p + s.size();
    }
    return out.Num();
}

template <typename K, typename V>
struct TPair {
    K Key;
    V Value;
};

template <typename K, typename V>
class TMap {
public:
    V& Add(const K& k, const V& v) {
        if (V* x = Find(k)) return *x = v;
        m_v.push_back({k, v});
        return m_v.back().Value;
    }
    V* Find(const K& k) {
        for (auto& p : m_v) if (p.Key == k) return &p.Value;
        return nullptr;
    }
    const V* Find(const K& k) const {
        for (auto& p : m_v) if (p.Key == k) return &p.Value;
        return nullptr;
    }
    V FindRef(const K& k) const {
        const V* x = Find(k);
        return x ? *x : V();
    }
    V& FindOrAdd(const K& k) {
        if (V* x = Find(k)) return *x;
        m_v.push_back({k, V()});
        return m_v.back().Value;
    }
    V& operator[](const K& k) { return FindOrAdd(k); }
    bool Contains(const K& k) const { return Find(k) != nullptr; }
    int32 Remove(const K& k) {
        const size_t antes = m_v.size();
        m_v.erase(std::remove_if(m_v.begin(), m_v.end(), [&](const TPair<K, V>& p) { return p.Key == k; }), m_v.end());
        return static_cast<int32>(antes - m_v.size());
    }
    int32 Num() const { return static_cast<int32>(m_v.size()); }
    void Empty() { m_v.clear(); }
    auto begin() { return m_v.begin(); }
    auto end() { return m_v.end(); }
    auto begin() const { return m_v.begin(); }
    auto end() const { return m_v.end(); }

private:
    std::vector<TPair<K, V>> m_v;
};

// ---- FName / FText -------------------------------------------------------------------

enum EName { NAME_None = 0 };

class FName {
public:
    FName() = default;
    FName(EName) {}
    FName(const TCHAR* s) : m_s(s) {}
    FName(const ANSICHAR* s) : m_s(s) {}
    explicit FName(const FString& s) : m_s(s) {}
    FString ToString() const { return m_s; }
    bool IsNone() const { return m_s.IsEmpty(); }
    bool operator==(const FName& o) const { return m_s == o.m_s; }
    bool operator!=(const FName& o) const { return !(m_s == o.m_s); }

private:
    FString m_s;
};

class FText {
public:
    static FText FromString(const FString& s) {
        FText t;
        t.m_s = s;
        return t;
    }
    static FText FromName(const FName& n) { return FromString(n.ToString()); }
    static FText AsNumber(double d) { return FromString(FString::SanitizeFloat(d)); }
    static FText GetEmpty() { return FText(); }
    const FString& ToString() const { return m_s; }
    bool IsEmpty() const { return m_s.IsEmpty(); }

private:
    FString m_s;
};

#define LOCTEXT(Chave, Texto) FText::FromString(TEXT(Texto))
#define NSLOCTEXT(Espaco, Chave, Texto) FText::FromString(TEXT(Texto))
#define INVTEXT(Texto) FText::FromString(TEXT(Texto))

// ---- Matematica ----------------------------------------------------------------------

struct FMath {
    template <typename T> static T Abs(T a) { return a < T(0) ? -a : a; }
    template <typename T> static T Min(T a, T b) { return a < b ? a : b; }
    template <typename T> static T Max(T a, T b) { return a > b ? a : b; }
    template <typename T> static T Clamp(T x, T lo, T hi) { return x < lo ? lo : (x > hi ? hi : x); }
    template <typename T> static T Square(T a) { return a * a; }
    template <typename T> static T Sign(T a) { return a > T(0) ? T(1) : (a < T(0) ? T(-1) : T(0)); }
    template <typename T, typename U> static T Lerp(const T& a, const T& b, const U& t) { return static_cast<T>(a + (b - a) * t); }
    static float Sin(float x) { return std::sin(x); }
    static double Sin(double x) { return std::sin(x); }
    static float Cos(float x) { return std::cos(x); }
    static double Cos(double x) { return std::cos(x); }
    static float Tan(float x) { return std::tan(x); }
    static float Asin(float x) { return std::asin(x); }
    static float Acos(float x) { return std::acos(x); }
    static float Atan(float x) { return std::atan(x); }
    static float Atan2(float y, float x) { return std::atan2(y, x); }
    static float Sqrt(float x) { return std::sqrt(x); }
    static double Sqrt(double x) { return std::sqrt(x); }
    static float InvSqrt(float x) { return 1.0f / std::sqrt(x); }
    static float Pow(float a, float b) { return std::pow(a, b); }
    static double Pow(double a, double b) { return std::pow(a, b); }
    static float Exp(float x) { return std::exp(x); }
    static float Loge(float x) { return std::log(x); }
    static float Fmod(float a, float b) { return std::fmod(a, b); }
    static int32 FloorToInt(float x) { return static_cast<int32>(std::floor(x)); }
    static int32 CeilToInt(float x) { return static_cast<int32>(std::ceil(x)); }
    static int32 RoundToInt(float x) { return static_cast<int32>(std::lround(x)); }
    static int32 TruncToInt(float x) { return static_cast<int32>(x); }
    static float FloorToFloat(float x) { return std::floor(x); }
    static float CeilToFloat(float x) { return std::ceil(x); }
    static float RoundToFloat(float x) { return std::round(x); }
    static float Frac(float x) { return x - std::floor(x); }
    static bool IsNearlyEqual(double a, double b, double tol = UE_SMALL_NUMBER) { return std::fabs(a - b) <= tol; }
    static bool IsNearlyZero(double a, double tol = UE_SMALL_NUMBER) { return std::fabs(a) <= tol; }
    static bool IsNaN(double a) { return std::isnan(a); }
    template <typename T> static T DegreesToRadians(T d) { return d * static_cast<T>(PI / 180.0); }
    template <typename T> static T RadiansToDegrees(T r) { return r * static_cast<T>(180.0 / PI); }
    static std::mt19937& Gerador() {
        static std::mt19937 g(0x5eed);
        return g;
    }
    static float FRand() { return std::uniform_real_distribution<float>(0.0f, 1.0f)(Gerador()); }
    static int32 RandRange(int32 a, int32 b) { return std::uniform_int_distribution<int32>(a, b)(Gerador()); }
    static float RandRange(float a, float b) { return a + (b - a) * FRand(); }
    static float FRandRange(float a, float b) { return RandRange(a, b); }
    static bool RandBool() { return FRand() < 0.5f; }
    static void RandInit(int32 semente) { Gerador().seed(static_cast<uint32>(semente)); }
    static float InterpTo(float atual, float alvo, float dt, float vel) {
        if (vel <= 0.0f) return alvo;
        const float d = alvo - atual;
        if (d * d < UE_SMALL_NUMBER) return alvo;
        return atual + d * Clamp(dt * vel, 0.0f, 1.0f);
    }
};

struct FVector {
    double X = 0, Y = 0, Z = 0;
    FVector() = default;
    explicit FVector(double s) : X(s), Y(s), Z(s) {}
    FVector(double x, double y, double z) : X(x), Y(y), Z(z) {}
    static const FVector ZeroVector;
    static const FVector OneVector;
    static const FVector UpVector;
    static const FVector ForwardVector;
    static const FVector RightVector;
    FVector operator+(const FVector& o) const { return {X + o.X, Y + o.Y, Z + o.Z}; }
    FVector operator-(const FVector& o) const { return {X - o.X, Y - o.Y, Z - o.Z}; }
    FVector operator*(const FVector& o) const { return {X * o.X, Y * o.Y, Z * o.Z}; }
    FVector operator*(double s) const { return {X * s, Y * s, Z * s}; }
    FVector operator/(double s) const { return {X / s, Y / s, Z / s}; }
    FVector operator-() const { return {-X, -Y, -Z}; }
    FVector& operator+=(const FVector& o) { X += o.X; Y += o.Y; Z += o.Z; return *this; }
    FVector& operator-=(const FVector& o) { X -= o.X; Y -= o.Y; Z -= o.Z; return *this; }
    FVector& operator*=(double s) { X *= s; Y *= s; Z *= s; return *this; }
    bool operator==(const FVector& o) const { return X == o.X && Y == o.Y && Z == o.Z; }
    bool operator!=(const FVector& o) const { return !(*this == o); }
    double operator|(const FVector& o) const { return X * o.X + Y * o.Y + Z * o.Z; }
    FVector operator^(const FVector& o) const { return {Y * o.Z - Z * o.Y, Z * o.X - X * o.Z, X * o.Y - Y * o.X}; }
    double Size() const { return std::sqrt(X * X + Y * Y + Z * Z); }
    double Length() const { return Size(); }
    double SizeSquared() const { return X * X + Y * Y + Z * Z; }
    bool IsNearlyZero(double tol = UE_KINDA_SMALL_NUMBER) const { return std::fabs(X) <= tol && std::fabs(Y) <= tol && std::fabs(Z) <= tol; }
    bool IsZero() const { return X == 0 && Y == 0 && Z == 0; }
    FVector GetSafeNormal(double tol = UE_SMALL_NUMBER) const {
        const double s = SizeSquared();
        return s > tol ? *this / std::sqrt(s) : FVector();
    }
    bool Normalize(double tol = UE_SMALL_NUMBER) {
        const double s = SizeSquared();
        if (s <= tol) return false;
        *this = *this / std::sqrt(s);
        return true;
    }
    static double DotProduct(const FVector& a, const FVector& b) { return a | b; }
    static FVector CrossProduct(const FVector& a, const FVector& b) { return a ^ b; }
    static double Dist(const FVector& a, const FVector& b) { return (a - b).Size(); }
    static double Distance(const FVector& a, const FVector& b) { return (a - b).Size(); }
    FString ToString() const { return FString::Printf(TEXT("X=%3.3f Y=%3.3f Z=%3.3f"), X, Y, Z); }
};
inline FVector operator*(double s, const FVector& v) { return v * s; }
inline const FVector FVector::ZeroVector{0, 0, 0};
inline const FVector FVector::OneVector{1, 1, 1};
inline const FVector FVector::UpVector{0, 0, 1};
inline const FVector FVector::ForwardVector{1, 0, 0};
inline const FVector FVector::RightVector{0, 1, 0};

struct FVector2D {
    double X = 0, Y = 0;
    FVector2D() = default;
    FVector2D(double x, double y) : X(x), Y(y) {}
    FVector2D operator+(const FVector2D& o) const { return {X + o.X, Y + o.Y}; }
    FVector2D operator-(const FVector2D& o) const { return {X - o.X, Y - o.Y}; }
    FVector2D operator*(double s) const { return {X * s, Y * s}; }
    double Size() const { return std::sqrt(X * X + Y * Y); }
    bool operator==(const FVector2D& o) const { return X == o.X && Y == o.Y; }
    FString ToString() const { return FString::Printf(TEXT("X=%3.3f Y=%3.3f"), X, Y); }
};

struct FRotator {
    double Pitch = 0, Yaw = 0, Roll = 0;
    FRotator() = default;
    FRotator(double p, double y, double r) : Pitch(p), Yaw(y), Roll(r) {}
    static const FRotator ZeroRotator;
    FRotator operator+(const FRotator& o) const { return {Pitch + o.Pitch, Yaw + o.Yaw, Roll + o.Roll}; }
    FRotator operator*(double s) const { return {Pitch * s, Yaw * s, Roll * s}; }
    // Direcao (eixo X) apos a rotacao, como no motor: pitch e yaw em graus.
    FVector Vector() const {
        const double p = FMath::DegreesToRadians(Pitch), y = FMath::DegreesToRadians(Yaw);
        return {std::cos(p) * std::cos(y), std::cos(p) * std::sin(y), std::sin(p)};
    }
    FString ToString() const { return FString::Printf(TEXT("P=%f Y=%f R=%f"), Pitch, Yaw, Roll); }
};
inline const FRotator FRotator::ZeroRotator{0, 0, 0};

struct FQuat {
    double X = 0, Y = 0, Z = 0, W = 1;
    FQuat() = default;
    FQuat(double x, double y, double z, double w) : X(x), Y(y), Z(z), W(w) {}
    static const FQuat Identity;
};
inline const FQuat FQuat::Identity{0, 0, 0, 1};

struct FLinearColor {
    float R = 0, G = 0, B = 0, A = 1;
    FLinearColor() = default;
    FLinearColor(float r, float g, float b, float a = 1.0f) : R(r), G(g), B(b), A(a) {}
    static const FLinearColor White;
    static const FLinearColor Black;
    static const FLinearColor Red;
    static const FLinearColor Green;
    static const FLinearColor Blue;
};
inline const FLinearColor FLinearColor::White{1, 1, 1, 1};
inline const FLinearColor FLinearColor::Black{0, 0, 0, 1};
inline const FLinearColor FLinearColor::Red{1, 0, 0, 1};
inline const FLinearColor FLinearColor::Green{0, 1, 0, 1};
inline const FLinearColor FLinearColor::Blue{0, 0, 1, 1};

struct FColor {
    uint8 B = 0, G = 0, R = 0, A = 255;
    FColor() = default;
    FColor(uint8 r, uint8 g, uint8 b, uint8 a = 255) : B(b), G(g), R(r), A(a) {}
};

struct FTransform {
    FQuat Rotation;
    FVector Translation;
    FVector Scale3D{1, 1, 1};
    FVector GetLocation() const { return Translation; }
    void SetLocation(const FVector& v) { Translation = v; }
    FVector GetScale3D() const { return Scale3D; }
    void SetScale3D(const FVector& v) { Scale3D = v; }
};

// ---- Log -------------------------------------------------------------------------------

namespace ELogVerbosity {
enum Type : uint8 { NoLogging = 0, Fatal, Error, Warning, Display, Log, Verbose, VeryVerbose, All = VeryVerbose };
}

struct FLogCategoryBase {
    explicit FLogCategoryBase(const char* nome) : Nome(nome) {}
    const char* Nome;
};

namespace EruptionUE {
inline void LogF(const FLogCategoryBase& cat, int verbosidade, const TCHAR* fmt, ...) {
    va_list a;
    va_start(a, fmt);
    const std::string msg = ParaUtf8(FormatarV(fmt, a).c_str());
    va_end(a);
    if (const FEruptionUEHost* h = HostAtual()) h->Log(cat.Nome, verbosidade, msg.c_str());
    else std::fprintf(stderr, "%s: %s\n", cat.Nome, msg.c_str());
    if (verbosidade == ELogVerbosity::Fatal) std::abort();
}
} // namespace EruptionUE

#define DECLARE_LOG_CATEGORY_EXTERN(Nome, Padrao, Compilacao) extern FLogCategoryBase Nome
#define DEFINE_LOG_CATEGORY(Nome) FLogCategoryBase Nome(#Nome)
#define DEFINE_LOG_CATEGORY_STATIC(Nome, Padrao, Compilacao) static FLogCategoryBase Nome(#Nome)
#define DECLARE_LOG_CATEGORY_CLASS(Nome, Padrao, Compilacao) static FLogCategoryBase Nome(#Nome)
#define UE_LOG(Categoria, Verbosidade, Formato, ...) \
    ::EruptionUE::LogF(Categoria, ELogVerbosity::Verbosidade, Formato, ##__VA_ARGS__)

inline FLogCategoryBase LogTemp("LogTemp");

// ---- Asserts ---------------------------------------------------------------------------

#define check(expr) ((expr) ? (void)0 : ::EruptionUE::LogF(LogTemp, ELogVerbosity::Fatal, TEXT("check falhou: %s"), TEXT(#expr)))
#define checkf(expr, fmt, ...) ((expr) ? (void)0 : ::EruptionUE::LogF(LogTemp, ELogVerbosity::Fatal, fmt, ##__VA_ARGS__))
#define checkNoEntry() ::EruptionUE::LogF(LogTemp, ELogVerbosity::Fatal, TEXT("checkNoEntry"))
#define unimplemented() ::EruptionUE::LogF(LogTemp, ELogVerbosity::Fatal, TEXT("nao implementado"))
#define verify(expr) check(expr)
#define ensure(expr) ((expr) || (::EruptionUE::LogF(LogTemp, ELogVerbosity::Error, TEXT("ensure falhou: %s"), TEXT(#expr)), false))
#define ensureMsgf(expr, fmt, ...) ((expr) || (::EruptionUE::LogF(LogTemp, ELogVerbosity::Error, fmt, ##__VA_ARGS__), false))

// ---- Reflexao --------------------------------------------------------------------------
// As macros de marcacao somem; GENERATED_BODY() vira a macro que a
// ferramenta de compat gera por arquivo e linha (Super/ThisClass), no mesmo
// esquema publico CURRENT_FILE_ID + __LINE__.

#define UCLASS(...)
#define USTRUCT(...)
#define UENUM(...)
#define UPROPERTY(...)
#define UFUNCTION(...)
#define UMETA(...)
#define UPARAM(...)
#define UINTERFACE(...)
#define UDELEGATE(...)

#define EUE_JUNTAR_(A, B, C, D) A##B##C##D
#define EUE_JUNTAR(A, B, C, D) EUE_JUNTAR_(A, B, C, D)
#define GENERATED_BODY(...) EUE_JUNTAR(CURRENT_FILE_ID, _, __LINE__, _GENERATED_BODY)
#define GENERATED_UCLASS_BODY(...) EUE_JUNTAR(CURRENT_FILE_ID, _, __LINE__, _GENERATED_BODY_LEGACY)
#define GENERATED_USTRUCT_BODY(...) EUE_JUNTAR(CURRENT_FILE_ID, _, __LINE__, _GENERATED_BODY)
#define GENERATED_IINTERFACE_BODY(...)

struct FObjectInitializer {
    static const FObjectInitializer& Get() {
        static FObjectInitializer f;
        return f;
    }
};

class UObject {
public:
    UObject() = default;
    explicit UObject(const FObjectInitializer&) {}
    virtual ~UObject() = default;
};

class UBlueprintFunctionLibrary : public UObject {
public:
    UBlueprintFunctionLibrary() = default;
    explicit UBlueprintFunctionLibrary(const FObjectInitializer& oi) : UObject(oi) {}
};
