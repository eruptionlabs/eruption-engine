// Medidor de banda e auditoria de compressao (spec 50, "Fluxo Frio").
//
//   1. bytes por texel                     — os formatos do motor e os blocos
//                                            BC/ETC/ASTC dao o numero da tabela
//   2. trafego de um anexo                 — LOAD le, STORE/NONE grava, CLEAR e
//                                            DONT_CARE nao custam
//   3. passe G-buffer de 1920x1080         — a soma bate com a conta a mao, byte
//                                            a byte, e o nome do passe vem do
//                                            setPass/pushPass
//   4. auditoria                           — cada flag que desliga a compressao
//                                            vira um bit; alvo limpo da 0; o
//                                            aviso sai uma vez por combinacao
//   5. clear fora de {0,1} e imageBytes    — fast clear e cadeia de mips
//
//   g++ -std=c++17 -I src tests/test_bandwidth_meter.cpp src/renderer/BandwidthMeter.cpp
//       src/utils/Profiler.cpp src/core/Logger.cpp -lpthread
#include "renderer/BandwidthMeter.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>
#include <unordered_map>

using namespace eruption;

static int g_falhas = 0;
#define CHECK(cond)                                                                  \
    do {                                                                             \
        if (!(cond)) {                                                               \
            std::printf("  FALHA linha %d: %s\n", __LINE__, #cond);                  \
            ++g_falhas;                                                              \
        }                                                                            \
    } while (0)

template <class H> static H handle(uintptr_t v) {
    H h;
    static_assert(sizeof(H) == sizeof(uint64_t), "handle de 64 bits");
    uint64_t x = v;
    __builtin_memcpy(&h, &x, sizeof(h));
    return h;
}

static std::unordered_map<uint64_t, VkFormat> g_viewFormats;
static VkFormat fakeViewFormat(VkImageView v) {
    uint64_t k = 0;
    __builtin_memcpy(&k, &v, sizeof(k));
    auto it = g_viewFormats.find(k);
    return it == g_viewFormats.end() ? VK_FORMAT_UNDEFINED : it->second;
}

static VkRenderingAttachmentInfo att(VkImageView v, VkAttachmentLoadOp l, VkAttachmentStoreOp s) {
    VkRenderingAttachmentInfo a{};
    a.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
    a.imageView = v;
    a.loadOp = l;
    a.storeOp = s;
    return a;
}

int main() {
    std::printf("1. bytes por texel\n");
    CHECK(bw::bytesPerTexel(VK_FORMAT_R8G8B8A8_UNORM) == 4.0);
    CHECK(bw::bytesPerTexel(VK_FORMAT_R16G16B16A16_SFLOAT) == 8.0);
    CHECK(bw::bytesPerTexel(VK_FORMAT_B10G11R11_UFLOAT_PACK32) == 4.0);
    CHECK(bw::bytesPerTexel(VK_FORMAT_A2B10G10R10_UNORM_PACK32) == 4.0);
    CHECK(bw::bytesPerTexel(VK_FORMAT_R8_UINT) == 1.0);
    CHECK(bw::bytesPerTexel(VK_FORMAT_R16G16_SFLOAT) == 4.0);
    CHECK(bw::bytesPerTexel(VK_FORMAT_D32_SFLOAT) == 4.0);
    CHECK(bw::bytesPerTexel(VK_FORMAT_D16_UNORM) == 2.0);
    CHECK(bw::bytesPerTexel(VK_FORMAT_R32G32B32A32_SFLOAT) == 16.0);
    CHECK(bw::bytesPerTexel(VK_FORMAT_BC1_RGBA_UNORM_BLOCK) == 0.5);
    CHECK(bw::bytesPerTexel(VK_FORMAT_BC7_SRGB_BLOCK) == 1.0);
    CHECK(bw::bytesPerTexel(VK_FORMAT_BC5_UNORM_BLOCK) == 1.0);
    CHECK(bw::bytesPerTexel(VK_FORMAT_ETC2_R8G8B8_UNORM_BLOCK) == 0.5);
    CHECK(bw::bytesPerTexel(VK_FORMAT_ASTC_4x4_SRGB_BLOCK) == 1.0);
    CHECK(std::fabs(bw::bytesPerTexel(VK_FORMAT_ASTC_6x6_UNORM_BLOCK) - 16.0 / 36.0) < 1e-12);
    CHECK(bw::bytesPerTexel(VK_FORMAT_ASTC_12x12_SRGB_BLOCK) == 16.0 / 144.0);
    CHECK(bw::bytesPerTexel(VK_FORMAT_UNDEFINED) == 0.0);
    CHECK(bw::isBlockCompressed(VK_FORMAT_BC3_UNORM_BLOCK));
    CHECK(!bw::isBlockCompressed(VK_FORMAT_R8G8B8A8_SRGB));

    std::printf("2. trafego de um anexo\n");
    const uint64_t px = 1920ull * 1080ull;
    auto t = bw::attachmentTraffic(VK_FORMAT_R16G16B16A16_SFLOAT, 1920, 1080, 1, VK_ATTACHMENT_LOAD_OP_LOAD,
                                   VK_ATTACHMENT_STORE_OP_STORE);
    CHECK(t.read == px * 8 && t.written == px * 8);
    t = bw::attachmentTraffic(VK_FORMAT_R8G8B8A8_UNORM, 1920, 1080, 1, VK_ATTACHMENT_LOAD_OP_CLEAR,
                              VK_ATTACHMENT_STORE_OP_STORE);
    CHECK(t.read == 0 && t.written == px * 4);
    t = bw::attachmentTraffic(VK_FORMAT_D32_SFLOAT, 1920, 1080, 1, VK_ATTACHMENT_LOAD_OP_DONT_CARE,
                              VK_ATTACHMENT_STORE_OP_DONT_CARE);
    CHECK(t.read == 0 && t.written == 0);
    t = bw::attachmentTraffic(VK_FORMAT_D32_SFLOAT, 1920, 1080, 1, VK_ATTACHMENT_LOAD_OP_LOAD,
                              VK_ATTACHMENT_STORE_OP_NONE);
    CHECK(t.read == px * 4 && t.written == px * 4); // NONE vira STORE no Vulkan 1.1 do motor
    // Spec 52: com a extensao load_store_op_none ligada, NONE nao grava.
    bw::setStoreOpNoneNative(true);
    t = bw::attachmentTraffic(VK_FORMAT_D32_SFLOAT, 1920, 1080, 1, VK_ATTACHMENT_LOAD_OP_LOAD,
                              VK_ATTACHMENT_STORE_OP_NONE);
    CHECK(t.read == px * 4 && t.written == 0);
    t = bw::attachmentTraffic(VK_FORMAT_D32_SFLOAT, 1920, 1080, 1, VK_ATTACHMENT_LOAD_OP_LOAD,
                              VK_ATTACHMENT_STORE_OP_STORE);
    CHECK(t.written == px * 4); // STORE continua gravando
    bw::setStoreOpNoneNative(false);
    t = bw::attachmentTraffic(VK_FORMAT_D32_SFLOAT, 2048, 2048, 4, VK_ATTACHMENT_LOAD_OP_CLEAR,
                              VK_ATTACHMENT_STORE_OP_STORE);
    CHECK(t.written == 2048ull * 2048ull * 4ull * 4ull);

    std::printf("3. passe G-buffer de 1920x1080\n");
    // Formatos do G-buffer de hoje: albedo, normal, pbr, material, emissivo, profundidade.
    const VkFormat fmts[6] = {VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_R16G16B16A16_SFLOAT, VK_FORMAT_R8G8B8A8_UNORM,
                              VK_FORMAT_R8_UINT, VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_D32_SFLOAT};
    VkImageView views[6];
    for (int i = 0; i < 6; ++i) {
        views[i] = handle<VkImageView>(0x1000 + i);
        g_viewFormats[0x1000 + i] = fmts[i];
    }
    VkRenderingAttachmentInfo colors[5];
    for (int i = 0; i < 5; ++i) colors[i] = att(views[i], VK_ATTACHMENT_LOAD_OP_CLEAR, VK_ATTACHMENT_STORE_OP_STORE);
    VkRenderingAttachmentInfo depth = att(views[5], VK_ATTACHMENT_LOAD_OP_CLEAR, VK_ATTACHMENT_STORE_OP_STORE);
    VkRenderingInfo ri{};
    ri.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
    ri.renderArea = {{0, 0}, {1920, 1080}};
    ri.layerCount = 1;
    ri.colorAttachmentCount = 5;
    ri.pColorAttachments = colors;
    ri.pDepthAttachment = &depth;
    bw::beginFrame(); // zera o que houver
    bw::setPass("GBuffer");
    bw::recordRendering(ri, &fakeViewFormat);
    // Reabertura com LOAD (particulas de malha por cima): le e grava tudo de novo.
    for (auto& c : colors) c.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
    depth.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
    bw::pushPass("Sub");
    bw::recordRendering(ri, &fakeViewFormat);
    bw::popPass();
    CHECK(bw::currentPass() != nullptr && std::string(bw::currentPass()) == "GBuffer");
    bw::setPass(nullptr);
    ri.pDepthAttachment = nullptr;
    ri.colorAttachmentCount = 1;
    bw::recordRendering(ri, &fakeViewFormat); // sem nome: "outros"
    bw::beginFrame();
    const bw::FrameStat f = bw::lastFrame();
    const uint64_t gbuf = px * (4 + 8 + 4 + 1 + 4 + 4); // 25 B/px
    CHECK(f.passes.size() == 3);
    if (f.passes.size() == 3) {
        CHECK(f.passes[0].name == "GBuffer" && f.passes[0].read == 0 && f.passes[0].written == gbuf);
        CHECK(f.passes[0].renders == 1);
        CHECK(f.passes[1].name == "Sub" && f.passes[1].read == gbuf && f.passes[1].written == gbuf);
        CHECK(f.passes[2].name == "outros" && f.passes[2].read == px * 4 && f.passes[2].written == px * 4);
    }
    CHECK(f.read == gbuf + px * 4 && f.written == 2 * gbuf + px * 4);
    std::printf("   G-buffer 1920x1080: %.2f MB gravados (25 B/px)\n", gbuf / (1024.0 * 1024.0));

    // Custo de CPU do medidor: um quadro do motor abre ~45 passes; o teto
    // aceito (spec 50, FF-01) e' 2 us por abertura de 6 anexos.
    {
        ri.colorAttachmentCount = 5;
        ri.pDepthAttachment = &depth;
        bw::setPass("GBuffer");
        const int n = 200000;
        // Melhor de 5 rodadas: a maquina carregada (outro agente na CPU) estica uma rodada, nao as 5.
        double ns = 1e30;
        for (int rep = 0; rep < 5; ++rep) {
            const auto t0 = std::chrono::steady_clock::now();
            for (int i = 0; i < n; ++i) bw::recordRendering(ri, &fakeViewFormat);
            ns = std::min(ns, std::chrono::duration<double, std::nano>(std::chrono::steady_clock::now() - t0).count() / n);
        }
        bw::setPass(nullptr);
        bw::beginFrame();
        std::printf("   custo: %.0f ns por abertura de passe com 6 anexos (melhor de 5)\n", ns);
        CHECK(ns < 2000.0);
    }

    std::printf("3b. mapa de views limitado\n");
    {
        bw::ImageDesc di;
        di.format = VK_FORMAT_R8G8B8A8_UNORM;
        di.extent = {64, 64, 1};
        di.usage = VK_IMAGE_USAGE_SAMPLED_BIT;
        const VkImage im = handle<VkImage>(0x7000);
        bw::noteImage(im, di);
        const size_t before = bw::trackedViews();
        for (uint64_t k = 0; k < 3; ++k) bw::noteView(handle<VkImageView>(0x7100 + k), im);
        CHECK(bw::trackedViews() == before + 3);
        bw::forgetImage(im);                      // as views da imagem saem junto
        CHECK(bw::trackedViews() == before);
        for (uint64_t k = 0; k < 40000; ++k) bw::noteView(handle<VkImageView>(0x80000 + k), handle<VkImage>(0x90000 + k % 7));
        CHECK(bw::trackedViews() <= 16385);       // views de imagem desconhecida: poda no teto
    }

    std::printf("4. auditoria\n");
    bw::ImageDesc d;
    d.format = VK_FORMAT_R16G16B16A16_SFLOAT;
    d.extent = {1920, 1080, 1};
    d.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    CHECK(bw::auditRenderTarget(d) == 0);
    d.usage |= VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    CHECK(bw::auditRenderTarget(d) == bw::kAuditTransferDst);
    d.usage |= VK_IMAGE_USAGE_STORAGE_BIT;
    CHECK(bw::auditRenderTarget(d) == (bw::kAuditTransferDst | bw::kAuditStorage));
    d.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    d.flags = VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT;
    CHECK(bw::auditRenderTarget(d) == bw::kAuditMutableNoList);
    d.formatList = true;
    CHECK(bw::auditRenderTarget(d) == 0);
    d.flags = VK_IMAGE_CREATE_ALIAS_BIT;
    d.tiling = VK_IMAGE_TILING_LINEAR;
    d.samples = VK_SAMPLE_COUNT_4_BIT;
    CHECK(bw::auditRenderTarget(d) == (bw::kAuditAlias | bw::kAuditLinear | bw::kAuditMultisample));
    // Textura (sem anexo) nao e' alvo de render: nao se audita.
    bw::ImageDesc tex;
    tex.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    CHECK(bw::auditRenderTarget(tex) == 0);
    CHECK(bw::describeAudit(bw::kAuditStorage | bw::kAuditTransferDst).find("STORAGE") != std::string::npos);
    CHECK(bw::describeAudit(0).empty());
    // VkImageCreateInfo com lista de formatos na cadeia pNext.
    VkImageFormatListCreateInfo list{};
    list.sType = VK_STRUCTURE_TYPE_IMAGE_FORMAT_LIST_CREATE_INFO;
    VkImageCreateInfo ci{};
    ci.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    ci.pNext = &list;
    ci.flags = VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT;
    ci.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    ci.samples = VK_SAMPLE_COUNT_1_BIT;
    CHECK(bw::describeImage(ci).formatList);
    CHECK(bw::auditRenderTarget(bw::describeImage(ci)) == 0);

    // Aviso uma vez por combinacao: dois alvos iguais (resize) dao 1 aviso.
    const bw::AuditStat a0 = bw::auditStats();
    bw::ImageDesc rt;
    rt.format = VK_FORMAT_R16G16B16A16_SFLOAT;
    rt.extent = {1920, 1080, 1};
    rt.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    for (int k = 0; k < 2; ++k) {
        const VkImage img = handle<VkImage>(0x2000 + k);
        const VkImageView v = handle<VkImageView>(0x3000 + k);
        g_viewFormats[0x3000 + k] = rt.format;
        bw::noteImage(img, rt);
        bw::noteView(v, img);
        VkRenderingAttachmentInfo c = att(v, VK_ATTACHMENT_LOAD_OP_DONT_CARE, VK_ATTACHMENT_STORE_OP_STORE);
        VkRenderingInfo r{};
        r.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
        r.renderArea = {{0, 0}, {1920, 1080}};
        r.layerCount = 1;
        r.colorAttachmentCount = 1;
        r.pColorAttachments = &c;
        bw::setPass("Post");
        bw::recordRendering(r, &fakeViewFormat);
        bw::recordRendering(r, &fakeViewFormat); // segunda vez: ja' auditado
    }
    const bw::AuditStat a1 = bw::auditStats();
    CHECK(a1.targets == a0.targets + 2);
    CHECK(a1.flagged == a0.flagged + 2);
    CHECK(a1.warnings == a0.warnings + 1);
    // Textura: entra na conta de memoria comprimida/sem compressao.
    bw::ImageDesc bc;
    bc.format = VK_FORMAT_BC7_UNORM_BLOCK;
    bc.extent = {1024, 1024, 1};
    bc.mips = 1;
    bc.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    bw::noteImage(handle<VkImage>(0x4000), bc);
    CHECK(bw::auditStats().textureCompressedBytes == a1.textureCompressedBytes + 1024ull * 1024ull);
    bw::forgetImage(handle<VkImage>(0x4000));
    CHECK(bw::auditStats().textureCompressedBytes == a1.textureCompressedBytes);

    std::printf("5. clear e imageBytes\n");
    VkClearColorValue cv{};
    CHECK(!bw::clearLosesFastClear(cv, VK_FORMAT_R8G8B8A8_UNORM));
    cv.float32[0] = 0.5f;
    CHECK(bw::clearLosesFastClear(cv, VK_FORMAT_R8G8B8A8_UNORM));
    VkClearColorValue ci1{};
    ci1.uint32[0] = 1;
    CHECK(!bw::clearLosesFastClear(ci1, VK_FORMAT_R8_UINT));
    bw::ImageDesc m;
    m.format = VK_FORMAT_R8G8B8A8_UNORM;
    m.extent = {256, 256, 1};
    m.mips = 9; // 256..1
    uint64_t want = 0;
    for (uint32_t s = 256; s >= 1; s /= 2) want += uint64_t(s) * s * 4;
    CHECK(bw::imageBytes(m) == want);
    // Bloco do formato: ASTC 6x6 de 256x256 = ceil(256/6)^2 = 43^2 blocos de 16 bytes
    // (antes contava como 4x4: 4096 blocos, 2,2x a mais).
    bw::ImageDesc a6;
    a6.format = VK_FORMAT_ASTC_6x6_UNORM_BLOCK;
    a6.extent = {256, 256, 1};
    CHECK(bw::imageBytes(a6) == 43ull * 43ull * 16ull);
    // Mip menor que o bloco ocupa um bloco inteiro: BC1 2x2 = 8 bytes.
    bw::ImageDesc b1;
    b1.format = VK_FORMAT_BC1_RGBA_UNORM_BLOCK;
    b1.extent = {2, 2, 1};
    CHECK(bw::imageBytes(b1) == 8);
    CHECK(bw::formatLabel(VK_FORMAT_ASTC_6x6_SRGB_BLOCK) == "ASTC6x6");
    CHECK(bw::formatLabel(VK_FORMAT_BC7_UNORM_BLOCK) == "BC7");
    CHECK(bw::formatLabel(VK_FORMAT_EAC_R11G11_UNORM_BLOCK) == "EAC_RG11");

    std::printf("6. censo de texturas (spec 51)\n");
    {
        const bw::CensusTotal c0 = bw::censusTotal(bw::kTexContent);
        const bw::CensusTotal l0 = bw::censusTotal(bw::kTexLayers);
        // Conteudo pelas flags: BC7 1024x1024 com mips e RGBA8 com mips por compute (STORAGE).
        bw::ImageDesc t1;
        t1.format = VK_FORMAT_BC7_UNORM_BLOCK;
        t1.extent = {1024, 1024, 1};
        t1.mips = 11;
        t1.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        bw::ImageDesc t2 = m;
        t2.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_STORAGE_BIT;
        CHECK(bw::categorize(t1) == bw::kTexContent);
        CHECK(bw::categorize(t2) == bw::kTexContent);
        bw::ImageDesc dep = t1;
        dep.format = VK_FORMAT_D32_SFLOAT; // copia de profundidade: nao e' conteudo
        CHECK(bw::categorize(dep) == bw::kTexNone);
        bw::noteImage(handle<VkImage>(0x5001), t1);
        bw::noteImage(handle<VkImage>(0x5002), t2);
        bw::CensusTotal c1 = bw::censusTotal(bw::kTexContent);
        CHECK(c1.block == c0.block + bw::imageBytes(t1));
        CHECK(c1.raw == c0.raw + bw::imageBytes(t2));
        CHECK(c1.count == c0.count + 2);
        // Array de camadas: pelas flags e' alvo de render (nao entra) ate' a marca.
        bw::ImageDesc arr = m;
        arr.layers = 32;
        arr.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                    VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
        CHECK(bw::categorize(arr) == bw::kTexNone);
        bw::noteImage(handle<VkImage>(0x5003), arr);
        CHECK(bw::censusTotal(bw::kTexLayers).raw == l0.raw);
        bw::tagImage(handle<VkImage>(0x5003), bw::kTexLayers);
        CHECK(bw::censusTotal(bw::kTexLayers).raw == l0.raw + 32ull * want);
        // Marca muda a categoria: o RGBA8 sai do conteudo e vai para sprite.
        bw::tagImage(handle<VkImage>(0x5002), bw::kTexSprite);
        CHECK(bw::censusTotal(bw::kTexContent).raw == c0.raw);
        CHECK(bw::censusTotal(bw::kTexSprite).raw == bw::imageBytes(t2));
        // A linha do censo bate com os totais.
        uint64_t layersRows = 0;
        for (const bw::CensusRow& r : bw::textureCensus())
            if (r.category == bw::kTexLayers) layersRows += r.bytes;
        CHECK(layersRows == bw::censusTotal(bw::kTexLayers).raw + bw::censusTotal(bw::kTexLayers).block);
        // Destruir tira de onde estava (com a marca).
        for (uint64_t h : {0x5001ull, 0x5002ull, 0x5003ull}) bw::forgetImage(handle<VkImage>(h));
        c1 = bw::censusTotal(bw::kTexContent);
        CHECK(c1.block == c0.block && c1.raw == c0.raw && c1.count == c0.count);
        CHECK(bw::censusTotal(bw::kTexLayers).raw == l0.raw);
        CHECK(bw::censusTotal(bw::kTexSprite).count == 0);
        bw::ImageDesc found;
        CHECK(!bw::findImage(handle<VkImage>(0x5001), found));
    }

    std::printf(g_falhas ? "HOUVE FALHA (%d)\n" : "OK\n", g_falhas);
    return g_falhas ? 1 : 0;
}
