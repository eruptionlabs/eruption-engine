#include "utils/TextureFormatPolicy.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <mutex>
#include <sstream>

namespace eruption::texfmt {

const char* className(Class c) {
    switch (c) {
    case Class::AlbedoOpaque: return "albedo";
    case Class::AlbedoAlpha: return "albedo_alfa";
    case Class::Normal: return "normal";
    case Class::Data4: return "dados";
    case Class::Mask1: return "mascara";
    case Class::Sprite: return "sprite";
    }
    return "?";
}

const char* modeName(Mode m) {
    switch (m) {
    case Mode::Off: return "off";
    case Mode::Auto: return "auto";
    case Mode::Bc: return "bc";
    case Mode::Astc: return "astc";
    case Mode::Etc2: return "etc2";
    }
    return "?";
}

namespace {

std::string lower(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

} // namespace

Mode parseMode(const std::string& s0, Mode fallback) {
    const std::string s = lower(s0);
    if (s == "off" || s == "0" || s == "none" || s == "rgba8") return Mode::Off;
    if (s == "auto" || s == "1" || s == "on") return Mode::Auto;
    if (s == "bc") return Mode::Bc;
    if (s == "astc") return Mode::Astc;
    if (s == "etc2" || s == "etc") return Mode::Etc2;
    return fallback;
}

Caps parseCaps(const std::string& list) {
    Caps c;
    std::stringstream ss(lower(list));
    std::string t;
    while (std::getline(ss, t, ',')) {
        t.erase(std::remove_if(t.begin(), t.end(), [](char ch) { return std::isspace(static_cast<unsigned char>(ch)); }),
                t.end());
        if (t == "bc") c.bc1 = c.bc3 = c.bc4 = c.bc5 = c.bc7 = true;
        else if (t == "bc1") c.bc1 = true;
        else if (t == "bc3") c.bc3 = true;
        else if (t == "bc4") c.bc4 = true;
        else if (t == "bc5") c.bc5 = true;
        else if (t == "bc7") c.bc7 = true;
        else if (t == "astc") c.astc4x4 = c.astc6x6 = true;
        else if (t == "astc4x4") c.astc4x4 = true;
        else if (t == "astc6x6") c.astc6x6 = true;
        else if (t == "etc2") c.etc2Rgb = c.etc2Rgba = true;
        else if (t == "etc2rgb") c.etc2Rgb = true;
        else if (t == "etc2rgba") c.etc2Rgba = true;
        else if (t == "eac") c.eacR11 = c.eacRg11 = true;
        else if (t == "eacr11") c.eacR11 = true;
        else if (t == "eacrg11") c.eacRg11 = true;
    }
    return c;
}

Caps intersect(const Caps& a, const Caps& b) {
    Caps c;
    c.bc1 = a.bc1 && b.bc1;
    c.bc3 = a.bc3 && b.bc3;
    c.bc4 = a.bc4 && b.bc4;
    c.bc5 = a.bc5 && b.bc5;
    c.bc7 = a.bc7 && b.bc7;
    c.astc4x4 = a.astc4x4 && b.astc4x4;
    c.astc6x6 = a.astc6x6 && b.astc6x6;
    c.etc2Rgb = a.etc2Rgb && b.etc2Rgb;
    c.etc2Rgba = a.etc2Rgba && b.etc2Rgba;
    c.eacR11 = a.eacR11 && b.eacR11;
    c.eacRg11 = a.eacRg11 && b.eacRg11;
    return c;
}

std::string describeCaps(const Caps& c) {
    std::string s;
    auto add = [&s](bool on, const char* n) {
        if (!on) return;
        if (!s.empty()) s += ",";
        s += n;
    };
    add(c.bc1, "bc1");
    add(c.bc3, "bc3");
    add(c.bc4, "bc4");
    add(c.bc5, "bc5");
    add(c.bc7, "bc7");
    add(c.astc4x4, "astc4x4");
    add(c.astc6x6, "astc6x6");
    add(c.etc2Rgb, "etc2rgb");
    add(c.etc2Rgba, "etc2rgba");
    add(c.eacR11, "eacr11");
    add(c.eacRg11, "eacrg11");
    return s.empty() ? "nenhum" : s;
}

Mode family(const Caps& caps, const Options& o) {
    switch (o.mode) {
    case Mode::Off: return Mode::Off;
    case Mode::Bc: return caps.anyBc() ? Mode::Bc : Mode::Off;
    case Mode::Astc: return caps.anyAstc() ? Mode::Astc : Mode::Off;
    case Mode::Etc2: return caps.anyEtc2() ? Mode::Etc2 : Mode::Off;
    case Mode::Auto:
        if (caps.anyBc()) return Mode::Bc;
        if (caps.anyAstc()) return Mode::Astc;
        if (caps.anyEtc2()) return Mode::Etc2;
        return Mode::Off;
    }
    return Mode::Off;
}

namespace {

VkFormat chooseBc(Class c, const Caps& k, const Options& o) {
    switch (c) {
    case Class::AlbedoOpaque:
        if (o.albedoBc7 && k.bc7) return VK_FORMAT_BC7_UNORM_BLOCK;
        if (k.bc1) return VK_FORMAT_BC1_RGBA_UNORM_BLOCK;
        if (k.bc7) return VK_FORMAT_BC7_UNORM_BLOCK;
        if (k.bc3) return VK_FORMAT_BC3_UNORM_BLOCK;
        return VK_FORMAT_UNDEFINED;
    case Class::AlbedoAlpha:
        if (o.alphaBc7 && k.bc7) return VK_FORMAT_BC7_UNORM_BLOCK;
        if (k.bc3) return VK_FORMAT_BC3_UNORM_BLOCK;
        if (k.bc7) return VK_FORMAT_BC7_UNORM_BLOCK;
        return VK_FORMAT_UNDEFINED;
    case Class::Normal:
        // Nunca BC1/BC3 (cor conjunta: x e y passam a dividir a mesma reta).
        if (o.normalTwoChannel && k.bc5) return VK_FORMAT_BC5_UNORM_BLOCK;
        if (k.bc7) return VK_FORMAT_BC7_UNORM_BLOCK;
        return VK_FORMAT_UNDEFINED;
    case Class::Data4:
        if (k.bc7) return VK_FORMAT_BC7_UNORM_BLOCK;
        if (k.bc3) return VK_FORMAT_BC3_UNORM_BLOCK;
        return VK_FORMAT_UNDEFINED;
    case Class::Mask1:
        if (k.bc4) return VK_FORMAT_BC4_UNORM_BLOCK;
        return VK_FORMAT_UNDEFINED;
    case Class::Sprite:
        if (k.bc7) return VK_FORMAT_BC7_UNORM_BLOCK;
        return VK_FORMAT_UNDEFINED;
    }
    return VK_FORMAT_UNDEFINED;
}

VkFormat chooseAstc(Class c, const Caps& k, const Options& o) {
    if (c == Class::AlbedoOpaque && o.astcAlbedo6x6 && k.astc6x6) return VK_FORMAT_ASTC_6x6_UNORM_BLOCK;
    if (k.astc4x4) return VK_FORMAT_ASTC_4x4_UNORM_BLOCK;
    // So' 6x6 na GPU (nao existe na pratica: o 4x4 e' o minimo do ASTC LDR), mas
    // so' onde o bloco grande e' aceitavel.
    if (k.astc6x6 && (c == Class::AlbedoOpaque || c == Class::Mask1)) return VK_FORMAT_ASTC_6x6_UNORM_BLOCK;
    return VK_FORMAT_UNDEFINED;
}

VkFormat chooseEtc2(Class c, const Caps& k, const Options& o) {
    switch (c) {
    case Class::AlbedoOpaque:
        if (k.etc2Rgb) return VK_FORMAT_ETC2_R8G8B8_UNORM_BLOCK;
        if (k.etc2Rgba) return VK_FORMAT_ETC2_R8G8B8A8_UNORM_BLOCK;
        return VK_FORMAT_UNDEFINED;
    case Class::Normal:
        if (o.normalTwoChannel && k.eacRg11) return VK_FORMAT_EAC_R11G11_UNORM_BLOCK;
        [[fallthrough]];
    case Class::AlbedoAlpha:
    case Class::Data4:
    case Class::Sprite:
        if (k.etc2Rgba) return VK_FORMAT_ETC2_R8G8B8A8_UNORM_BLOCK;
        return VK_FORMAT_UNDEFINED;
    case Class::Mask1:
        if (k.eacR11) return VK_FORMAT_EAC_R11_UNORM_BLOCK;
        return VK_FORMAT_UNDEFINED;
    }
    return VK_FORMAT_UNDEFINED;
}

} // namespace

VkFormat choose(Class c, const Caps& caps, const Options& o) {
    switch (family(caps, o)) {
    case Mode::Bc: return chooseBc(c, caps, o);
    case Mode::Astc: return chooseAstc(c, caps, o);
    case Mode::Etc2: return chooseEtc2(c, caps, o);
    default: return VK_FORMAT_UNDEFINED;
    }
}

BlockInfo blockInfo(VkFormat f) {
    switch (f) {
    case VK_FORMAT_BC1_RGBA_UNORM_BLOCK:
    case VK_FORMAT_BC1_RGB_UNORM_BLOCK:
    case VK_FORMAT_BC4_UNORM_BLOCK:
    case VK_FORMAT_ETC2_R8G8B8_UNORM_BLOCK:
    case VK_FORMAT_EAC_R11_UNORM_BLOCK:
        return {8, 4, 4};
    case VK_FORMAT_BC3_UNORM_BLOCK:
    case VK_FORMAT_BC5_UNORM_BLOCK:
    case VK_FORMAT_BC7_UNORM_BLOCK:
    case VK_FORMAT_ETC2_R8G8B8A8_UNORM_BLOCK:
    case VK_FORMAT_EAC_R11G11_UNORM_BLOCK:
    case VK_FORMAT_ASTC_4x4_UNORM_BLOCK:
        return {16, 4, 4};
    case VK_FORMAT_ASTC_6x6_UNORM_BLOCK:
        return {16, 6, 6};
    default:
        return {};
    }
}

bool encodable(VkFormat f) { return blockInfo(f).bytes != 0; }

const char* formatTag(VkFormat f) {
    switch (f) {
    case VK_FORMAT_UNDEFINED:
    case VK_FORMAT_R8G8B8A8_UNORM: return "RGBA8";
    case VK_FORMAT_BC1_RGBA_UNORM_BLOCK:
    case VK_FORMAT_BC1_RGB_UNORM_BLOCK: return "BC1";
    case VK_FORMAT_BC3_UNORM_BLOCK: return "BC3";
    case VK_FORMAT_BC4_UNORM_BLOCK: return "BC4";
    case VK_FORMAT_BC5_UNORM_BLOCK: return "BC5";
    case VK_FORMAT_BC7_UNORM_BLOCK: return "BC7";
    case VK_FORMAT_ASTC_4x4_UNORM_BLOCK: return "ASTC4x4";
    case VK_FORMAT_ASTC_6x6_UNORM_BLOCK: return "ASTC6x6";
    case VK_FORMAT_ETC2_R8G8B8_UNORM_BLOCK: return "ETC2";
    case VK_FORMAT_ETC2_R8G8B8A8_UNORM_BLOCK: return "ETC2A8";
    case VK_FORMAT_EAC_R11_UNORM_BLOCK: return "EAC_R11";
    case VK_FORMAT_EAC_R11G11_UNORM_BLOCK: return "EAC_RG11";
    default: return "?";
    }
}

uint32_t policyKey(const Caps& caps, const Options& o) {
    // FNV-1a sobre o formato escolhido para cada classe: dois conjuntos de
    // opcoes que dao os mesmos formatos dao a mesma chave (o pacote serve).
    uint32_t h = 2166136261u;
    for (int i = 0; i < kClassCount; ++i) {
        const uint32_t f = static_cast<uint32_t>(choose(static_cast<Class>(i), caps, o));
        for (int b = 0; b < 4; ++b) {
            h ^= (f >> (8 * b)) & 0xFFu;
            h *= 16777619u;
        }
    }
    return h;
}

namespace {
std::mutex g_mutex;
Caps g_caps;
Options g_options;
} // namespace

void setDeviceCaps(const Caps& c) {
    std::lock_guard<std::mutex> lk(g_mutex);
    g_caps = c;
}
void setOptions(const Options& o) {
    std::lock_guard<std::mutex> lk(g_mutex);
    g_options = o;
}
Caps deviceCaps() {
    std::lock_guard<std::mutex> lk(g_mutex);
    return g_caps;
}
Options options() {
    std::lock_guard<std::mutex> lk(g_mutex);
    return g_options;
}

VkFormat chooseActive(Class c) {
    std::lock_guard<std::mutex> lk(g_mutex);
    return choose(c, g_caps, g_options);
}

bool anyActive() {
    std::lock_guard<std::mutex> lk(g_mutex);
    for (int i = 0; i < kClassCount; ++i)
        if (choose(static_cast<Class>(i), g_caps, g_options) != VK_FORMAT_UNDEFINED) return true;
    return false;
}

bool classicActive() {
    std::lock_guard<std::mutex> lk(g_mutex);
    return family(g_caps, g_options) == Mode::Bc && !g_options.albedoBc7 && !g_options.alphaBc7 &&
           !g_options.normalTwoChannel;
}

std::string activeTag() {
    if (classicActive()) return {};
    const Caps c = deviceCaps();
    const Options o = options();
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%08x", policyKey(c, o));
    return buf;
}

bool deviceSamples(VkFormat f) {
    if (f == VK_FORMAT_R8G8B8A8_UNORM || f == VK_FORMAT_R8G8B8A8_SRGB) return true;
    const Caps c = deviceCaps();
    switch (f) {
    case VK_FORMAT_BC1_RGBA_UNORM_BLOCK:
    case VK_FORMAT_BC1_RGB_UNORM_BLOCK: return c.bc1;
    case VK_FORMAT_BC3_UNORM_BLOCK: return c.bc3;
    case VK_FORMAT_BC4_UNORM_BLOCK: return c.bc4;
    case VK_FORMAT_BC5_UNORM_BLOCK: return c.bc5;
    case VK_FORMAT_BC7_UNORM_BLOCK: return c.bc7;
    case VK_FORMAT_ASTC_4x4_UNORM_BLOCK: return c.astc4x4;
    case VK_FORMAT_ASTC_6x6_UNORM_BLOCK: return c.astc6x6;
    case VK_FORMAT_ETC2_R8G8B8_UNORM_BLOCK: return c.etc2Rgb;
    case VK_FORMAT_ETC2_R8G8B8A8_UNORM_BLOCK: return c.etc2Rgba;
    case VK_FORMAT_EAC_R11_UNORM_BLOCK: return c.eacR11;
    case VK_FORMAT_EAC_R11G11_UNORM_BLOCK: return c.eacRg11;
    default: return false;
    }
}

} // namespace eruption::texfmt
