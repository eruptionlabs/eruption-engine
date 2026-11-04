#pragma once

#include <vulkan/vulkan.h>

#include <cstdint>
#include <string>
#include <vector>

// MEDIDOR DE BANDA ("Fluxo Frio", spec 50). Conta, a cada quadro, os bytes que
// cada passe de render move nos anexos: formato x area x camadas x operacao de
// carga/gravacao. Todo passe do motor abre por vkc::cmdBeginRendering, entao o
// medidor nao depende de cada passe lembrar de se declarar: so' o NOME vem de
// fora (bw::setPass, o mesmo nome do tempo de GPU no Profiler), e o que roda
// sem nome cai em "outros".
//
// O que a estimativa NAO conta (limite declarado): leitura amostrada de
// texturas e de alvos por shader, compute (storage), copias e a apresentacao.
// O numero real, onde existe contador, vem ao lado: controlador de memoria da
// Intel (perf, sem root) e ocupacao da VRAM pelo NVML.
//
// Junto vai a AUDITORIA: todo VkImage criado pelo VMA e' anotado (gancho no
// vkCreateImage), e na primeira vez que vira anexo as flags que desligam a
// compressao de framebuffer (STORAGE, MUTABLE sem lista, ALIAS, TRANSFER_DST,
// HOST_TRANSFER, LINEAR, MSAA) viram um aviso [FLUXO] no log, uma vez por
// combinacao de passe, formato e problema.

namespace eruption::bw {

// ---------------------------------------------------------------- formula
// Bytes por texel (formato comprimido em bloco: bytes do bloco / texels do
// bloco). 0 = formato desconhecido (o anexo nao entra na soma).
double bytesPerTexel(VkFormat f);
bool isBlockCompressed(VkFormat f);

struct Traffic {
    uint64_t read = 0;
    uint64_t written = 0;
};
// LOAD le' a imagem inteira da area; STORE grava. STORE_OP_NONE conta como
// gravacao: a camada Vulkan 1.1 do motor (VkCompat) troca NONE por STORE.
// CLEAR e DONT_CARE nao leem; DONT_CARE nao grava.
Traffic attachmentTraffic(VkFormat f, uint32_t w, uint32_t h, uint32_t layers, VkAttachmentLoadOp load,
                          VkAttachmentStoreOp store);
// Com STORE_OP_NONE nativo (VK_KHR/EXT_load_store_op_none ligada e usada,
// spec 52) o anexo so' lido nao e' gravado: NONE deixa de contar.
void setStoreOpNoneNative(bool native);
bool storeOpNoneNative();

// --------------------------------------------------------------- auditoria
struct ImageDesc {
    VkFormat format = VK_FORMAT_UNDEFINED;
    VkExtent3D extent{0, 0, 0};
    uint32_t mips = 1;
    uint32_t layers = 1;
    VkSampleCountFlagBits samples = VK_SAMPLE_COUNT_1_BIT;
    VkImageTiling tiling = VK_IMAGE_TILING_OPTIMAL;
    VkImageUsageFlags usage = 0;
    VkImageCreateFlags flags = 0;
    bool formatList = false; // VkImageFormatListCreateInfo na cadeia pNext
};
ImageDesc describeImage(const VkImageCreateInfo& ci);

enum AuditFlag : uint32_t {
    kAuditStorage = 1u << 0,       // STORAGE no anexo
    kAuditMutableNoList = 1u << 1, // MUTABLE_FORMAT sem lista de formatos
    kAuditAlias = 1u << 2,         // ALIAS
    kAuditTransferDst = 1u << 3,   // TRANSFER_DST no anexo
    kAuditHostTransfer = 1u << 4,  // HOST_TRANSFER (host image copy)
    kAuditLinear = 1u << 5,        // tiling LINEAR
    kAuditMultisample = 1u << 6,   // MSAA (sem resolve no tile)
};
// Problemas de um alvo de render (anexo de cor ou profundidade). 0 = limpo.
uint32_t auditRenderTarget(const ImageDesc& d);
// Texto curto (pt-BR) de cada problema, separado por "; ".
std::string describeAudit(uint32_t flags);
// Clear de cor fora de {0, 1} por canal perde o "fast clear" (AMD; Intel em
// geracoes antigas).
bool clearLosesFastClear(const VkClearColorValue& c, VkFormat f);
// Bytes de uma imagem com a cadeia de mips inteira.
uint64_t imageBytes(const ImageDesc& d);

// ------------------------------------------------- registro em tempo de execucao
// Ganchos (VulkanContext/VkCompat): imagem criada/destruida pelo VMA, view criada.
void noteImage(VkImage image, const ImageDesc& d);
void forgetImage(VkImage image);
void noteView(VkImageView view, VkImage image);

// Nome do passe atual (o mesmo do Profiler::addGpuTime). nullptr = "outros".
// O ponteiro precisa viver ate' o fim do quadro (use literais).
void setPass(const char* name);
const char* currentPass();
// Subpasse aninhado (ex.: Bloom dentro de Post Total): empilha e volta ao
// nome anterior no pop. Pilha de 8 niveis por thread.
void pushPass(const char* name);
void popPass();
class PassScope {
public:
    explicit PassScope(const char* name) : m_prev(currentPass()) { setPass(name); }
    ~PassScope() { setPass(m_prev); }
    PassScope(const PassScope&) = delete;
    PassScope& operator=(const PassScope&) = delete;

private:
    const char* m_prev;
};

// Chamado por vkc::cmdBeginRendering. viewFormat: formato de cada view.
void recordRendering(const VkRenderingInfo& info, VkFormat (*viewFormat)(VkImageView));
// Bytes de vertice buscados por um draw (indices x instancias x passo), no
// passe corrente.
void addVertexFetch(uint64_t bytes);
// Fecha o quadro anterior (chamado por vkc::beginFrame) e le' o contador real.
void beginFrame();
// Fabricante do dispositivo em uso (o NVML so' vale se for NVIDIA).
void setDeviceVendor(uint32_t vendorId);
uint32_t deviceVendor();

struct PassStat {
    std::string name;
    uint64_t read = 0;
    uint64_t written = 0;
    uint32_t renders = 0; // quantos cmdBeginRendering o passe abriu
    // Busca de vertice (spec 52): indices desenhados x passo do vertice, limite
    // superior (sem o cache pos-transformacao). Fora de read/written, que sao
    // so' anexos.
    uint64_t vertexBytes = 0;
};
struct FrameStat {
    uint64_t frame = 0;
    uint64_t read = 0;
    uint64_t written = 0;
    uint64_t vertexBytes = 0;     // soma de PassStat::vertexBytes
    std::vector<PassStat> passes; // na ordem em que apareceram no quadro
    // Contador real do controlador de memoria (sistema inteiro: CPU, iGPU,
    // tela), no intervalo entre o inicio deste quadro e o do proximo.
    bool dramValid = false;
    double dramReadBytes = 0.0;
    double dramWriteBytes = 0.0;
    double dramSeconds = 0.0;
};
FrameStat lastFrame();

struct AuditStat {
    uint32_t targets = 0;  // imagens auditadas como anexo
    uint32_t flagged = 0;  // com algum problema
    uint32_t warnings = 0; // avisos distintos no log
    uint64_t textureCompressedBytes = 0;   // amostradas, formato em bloco
    uint64_t textureUncompressedBytes = 0; // amostradas, sem compressao
};
AuditStat auditStats();

// Contador real de DRAM (Intel uncore_imc_free_running, Linux de PC). Liga na
// primeira chamada; false = indisponivel (motivo em dramStatus()).
bool enableDramCounter();
const char* dramStatus();

// ERUPTION_TEST_FLUXO_REPORT=<quadro>: no quadro pedido (contagem do motor,
// a mesma do ERUPTION_TEST_RECORD), o log recebe a tabela de bytes por passe
// do quadro anterior e o resumo da auditoria (bancada e spec 50).
void maybeReport(uint64_t frame);

} // namespace eruption::bw
