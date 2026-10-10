#pragma once

// Conversao texto <-> tipos simples usada pelos thunks gerados para as
// UFUNCTION estaticas (o host chama por nome, com argumentos em texto).

#include "EruptionUE/Base.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

namespace EruptionUE {

template <typename T>
struct Conv;

template <>
struct Conv<float> {
    static float De(const char* s) { return std::strtof(s, nullptr); }
    static std::string Para(float v) {
        char b[64];
        std::snprintf(b, sizeof(b), "%.9g", static_cast<double>(v));
        return b;
    }
};
template <>
struct Conv<double> {
    static double De(const char* s) { return std::strtod(s, nullptr); }
    static std::string Para(double v) {
        char b[64];
        std::snprintf(b, sizeof(b), "%.17g", v);
        return b;
    }
};
template <>
struct Conv<int32> {
    static int32 De(const char* s) { return static_cast<int32>(std::strtol(s, nullptr, 10)); }
    static std::string Para(int32 v) { return std::to_string(v); }
};
template <>
struct Conv<int64> {
    static int64 De(const char* s) { return static_cast<int64>(std::strtoll(s, nullptr, 10)); }
    static std::string Para(int64 v) { return std::to_string(v); }
};
template <>
struct Conv<uint8> {
    static uint8 De(const char* s) { return static_cast<uint8>(std::strtoul(s, nullptr, 10)); }
    static std::string Para(uint8 v) { return std::to_string(v); }
};
template <>
struct Conv<bool> {
    static bool De(const char* s) { return std::strcmp(s, "true") == 0 || std::strcmp(s, "1") == 0; }
    static std::string Para(bool v) { return v ? "true" : "false"; }
};
template <>
struct Conv<FString> {
    static FString De(const char* s) { return FString(s); }
    static std::string Para(const FString& v) { return ParaUtf8(*v); }
};
template <>
struct Conv<FName> {
    static FName De(const char* s) { return FName(s); }
    static std::string Para(const FName& v) { return ParaUtf8(*v.ToString()); }
};
// FVector como "x,y,z".
template <>
struct Conv<FVector> {
    static FVector De(const char* s) {
        FVector v;
        std::sscanf(s, "%lf,%lf,%lf", &v.X, &v.Y, &v.Z);
        return v;
    }
    static std::string Para(const FVector& v) {
        char b[128];
        std::snprintf(b, sizeof(b), "(%g, %g, %g)", v.X, v.Y, v.Z);
        return b;
    }
};

} // namespace EruptionUE
