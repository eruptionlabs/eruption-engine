#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace eruption {

// Leitura e escrita de bits dos pacotes de rede. É o único código do módulo
// que lê bytes vindos de fora, então nada aqui confia no conteúdo: toda
// leitura confere o espaço que resta e a faixa declarada do campo. Um erro
// marca o leitor como inválido; a partir daí toda leitura devolve zero e o
// pacote inteiro deve ser descartado (ok() == false). Sem alocação a partir
// de um tamanho lido do pacote antes de conferir que os bytes existem.
//
// Ordem dos bits: little-endian, byte a byte (igual em qualquer CPU).
class BitWriter {
public:
    BitWriter(uint8_t* buffer, size_t capacityBytes);

    // 1..32 bits; bits acima de `bits` em `value` são ignorados.
    void writeBits(uint32_t value, int bits);
    void writeBool(bool value) { writeBits(value ? 1u : 0u, 1); }
    // Inteiro em [min, max] com o menor número de bits da faixa. Fora da
    // faixa é erro de quem escreve (overflow fica marcado).
    void writeInt(int32_t value, int32_t min, int32_t max);
    void writeFloat(float value);
    // Real em [min, max] na grade `step` (arredonda; fora da faixa é preso
    // nas bordas: quem escreve é o servidor, a fonte da verdade).
    void writeQuantized(float value, float min, float max, float step);
    void writeBytes(const uint8_t* data, size_t count);
    // Comprimento em [0, maxLength] seguido dos bytes, alinhados ao byte.
    void writeString(const std::string& value, size_t maxLength);
    void alignToByte();

    // Fecha o último byte parcial e devolve o tamanho usado em bytes.
    size_t finish();
    size_t bitsWritten() const { return m_bitsWritten; }
    bool ok() const { return !m_overflow; }

private:
    void emitFullBytes();

    uint8_t* m_buffer;
    size_t m_capacity;
    size_t m_bytePos = 0;
    size_t m_bitsWritten = 0;
    uint64_t m_scratch = 0;
    int m_scratchBits = 0;
    bool m_overflow = false;
};

class BitReader {
public:
    BitReader(const uint8_t* data, size_t sizeBytes);

    uint32_t readBits(int bits);
    bool readBool() { return readBits(1) != 0; }
    // Valor fora de [min, max] (possível quando a faixa não é potência de 2)
    // é erro: o pacote foi forjado ou corrompido.
    int32_t readInt(int32_t min, int32_t max);
    // NaN e infinito são erro (nunca entram na simulação).
    float readFloat();
    float readQuantized(float min, float max, float step);
    bool readBytes(uint8_t* out, size_t count);
    // Falha (sem alocar) se o comprimento passa de maxLength ou dos bytes que restam.
    bool readString(std::string& out, size_t maxLength);
    // Os bits de enchimento do alinhamento têm de ser zero.
    void alignToByte();

    bool ok() const { return !m_error; }
    // Marca o pacote como inválido por regra de quem chama (faixa de jogo, tick...).
    void fail() { m_error = true; }
    size_t bitsRemaining() const { return m_error ? 0 : m_size * 8 - m_bitsRead; }
    size_t bitsRead() const { return m_bitsRead; }

private:
    const uint8_t* m_data;
    size_t m_size;
    size_t m_bytePos = 0;
    size_t m_bitsRead = 0;
    uint64_t m_scratch = 0;
    int m_scratchBits = 0;
    bool m_error = false;
};

// Bits para guardar [0, range] (0 para range 0).
int bitsForRange(uint32_t range);
// Passos de quantização de [min, max] na grade `step` (0 quando min == max).
// Falso se a faixa for inválida (não finita, step <= 0, max < min, mais de 2^30 passos).
bool quantizedSteps(float min, float max, float step, uint32_t& steps);

} // namespace eruption
