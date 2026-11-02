#include "net/Wire.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace eruption {

namespace {

uint32_t maskBits(uint32_t value, int bits) {
    return bits >= 32 ? value : value & ((1u << bits) - 1u);
}

} // namespace

int bitsForRange(uint32_t range) {
    int bits = 0;
    while (range != 0) {
        ++bits;
        range >>= 1;
    }
    return bits;
}

bool quantizedSteps(float min, float max, float step, uint32_t& steps) {
    steps = 0;
    if (!std::isfinite(min) || !std::isfinite(max) || !std::isfinite(step)) return false;
    if (step <= 0.0f || max < min) return false;
    // Folga de 1e-6 passo: (1 - 0) / 0.1 não pode virar 11 passos por arredondamento.
    const double n = std::ceil((static_cast<double>(max) - min) / step - 1e-6);
    if (n > static_cast<double>(1u << 30)) return false;
    steps = static_cast<uint32_t>(std::max(0.0, n));
    return true;
}

// --- BitWriter ---

BitWriter::BitWriter(uint8_t* buffer, size_t capacityBytes) : m_buffer(buffer), m_capacity(capacityBytes) {}

void BitWriter::emitFullBytes() {
    while (m_scratchBits >= 8) {
        m_buffer[m_bytePos++] = static_cast<uint8_t>(m_scratch & 0xFFu);
        m_scratch >>= 8;
        m_scratchBits -= 8;
    }
}

void BitWriter::writeBits(uint32_t value, int bits) {
    if (m_overflow) return;
    if (bits < 1 || bits > 32 || m_bitsWritten + static_cast<size_t>(bits) > m_capacity * 8) {
        m_overflow = true;
        return;
    }
    m_scratch |= static_cast<uint64_t>(maskBits(value, bits)) << m_scratchBits;
    m_scratchBits += bits;
    m_bitsWritten += static_cast<size_t>(bits);
    emitFullBytes();
}

void BitWriter::writeInt(int32_t value, int32_t min, int32_t max) {
    if (min > max || value < min || value > max) {
        m_overflow = true;
        return;
    }
    const uint32_t range = static_cast<uint32_t>(static_cast<int64_t>(max) - min);
    const int bits = bitsForRange(range);
    if (bits > 0) writeBits(static_cast<uint32_t>(static_cast<int64_t>(value) - min), bits);
}

void BitWriter::writeFloat(float value) {
    uint32_t raw = 0;
    std::memcpy(&raw, &value, sizeof(raw));
    writeBits(raw, 32);
}

void BitWriter::writeQuantized(float value, float min, float max, float step) {
    uint32_t steps = 0;
    if (!quantizedSteps(min, max, step, steps)) {
        m_overflow = true;
        return;
    }
    const int bits = bitsForRange(steps);
    if (bits == 0) return;
    if (!std::isfinite(value)) value = min;
    value = std::clamp(value, min, max);
    const double q = std::round((static_cast<double>(value) - min) / step);
    writeBits(std::min(steps, static_cast<uint32_t>(std::max(0.0, q))), bits);
}

void BitWriter::alignToByte() {
    const int pad = static_cast<int>((8 - m_bitsWritten % 8) % 8);
    if (pad > 0) writeBits(0, pad);
}

void BitWriter::writeBytes(const uint8_t* data, size_t count) {
    alignToByte();
    if (m_overflow) return;
    if (count > m_capacity - m_bytePos) {
        m_overflow = true;
        return;
    }
    // Alinhado: o rascunho está vazio e m_bytePos * 8 == m_bitsWritten.
    if (count > 0) std::memcpy(m_buffer + m_bytePos, data, count);
    m_bytePos += count;
    m_bitsWritten += count * 8;
}

void BitWriter::writeString(const std::string& value, size_t maxLength) {
    if (maxLength > 0xFFFFFFFFu || value.size() > maxLength) {
        m_overflow = true;
        return;
    }
    const int bits = bitsForRange(static_cast<uint32_t>(maxLength));
    if (bits > 0) writeBits(static_cast<uint32_t>(value.size()), bits);
    writeBytes(reinterpret_cast<const uint8_t*>(value.data()), value.size());
}

size_t BitWriter::finish() {
    alignToByte();
    return m_bytePos;
}

// --- BitReader ---

BitReader::BitReader(const uint8_t* data, size_t sizeBytes) : m_data(data), m_size(data ? sizeBytes : 0) {}

uint32_t BitReader::readBits(int bits) {
    if (m_error) return 0;
    if (bits < 1 || bits > 32 || m_bitsRead + static_cast<size_t>(bits) > m_size * 8) {
        m_error = true;
        return 0;
    }
    // Só carrega o byte quando precisa: a sobra no rascunho fica sempre < 8
    // bits, e o total conferido acima garante m_bytePos < m_size.
    while (m_scratchBits < bits) {
        m_scratch |= static_cast<uint64_t>(m_data[m_bytePos++]) << m_scratchBits;
        m_scratchBits += 8;
    }
    const uint32_t value = maskBits(static_cast<uint32_t>(m_scratch), bits);
    m_scratch >>= bits;
    m_scratchBits -= bits;
    m_bitsRead += static_cast<size_t>(bits);
    return value;
}

int32_t BitReader::readInt(int32_t min, int32_t max) {
    if (min > max) {
        m_error = true;
        return 0;
    }
    const uint32_t range = static_cast<uint32_t>(static_cast<int64_t>(max) - min);
    const int bits = bitsForRange(range);
    if (bits == 0) return min;
    const uint32_t v = readBits(bits);
    if (v > range) m_error = true;
    if (m_error) return min;
    return static_cast<int32_t>(static_cast<int64_t>(min) + v);
}

float BitReader::readFloat() {
    const uint32_t raw = readBits(32);
    float value = 0.0f;
    std::memcpy(&value, &raw, sizeof(value));
    if (!std::isfinite(value)) m_error = true;
    return m_error ? 0.0f : value;
}

float BitReader::readQuantized(float min, float max, float step) {
    uint32_t steps = 0;
    if (!quantizedSteps(min, max, step, steps)) {
        m_error = true;
        return 0.0f;
    }
    const int bits = bitsForRange(steps);
    if (bits == 0) return min;
    const uint32_t q = readBits(bits);
    if (q > steps) m_error = true;
    if (m_error) return min;
    return std::min(max, static_cast<float>(static_cast<double>(min) + static_cast<double>(q) * step));
}

void BitReader::alignToByte() {
    const int pad = static_cast<int>((8 - m_bitsRead % 8) % 8);
    if (pad > 0 && readBits(pad) != 0) m_error = true;
}

bool BitReader::readBytes(uint8_t* out, size_t count) {
    alignToByte();
    if (m_error) return false;
    if (count > m_size - m_bytePos) {
        m_error = true;
        return false;
    }
    if (count > 0) std::memcpy(out, m_data + m_bytePos, count);
    m_bytePos += count;
    m_bitsRead += count * 8;
    return true;
}

bool BitReader::readString(std::string& out, size_t maxLength) {
    out.clear();
    if (maxLength > 0xFFFFFFFFu) {
        m_error = true;
        return false;
    }
    const int bits = bitsForRange(static_cast<uint32_t>(maxLength));
    const size_t length = bits > 0 ? readBits(bits) : 0;
    if (length > maxLength) m_error = true;
    alignToByte();
    if (m_error) return false;
    // Confere os bytes antes de alocar: um comprimento forjado não reserva memória.
    if (length > m_size - m_bytePos) {
        m_error = true;
        return false;
    }
    out.assign(reinterpret_cast<const char*>(m_data + m_bytePos), length);
    m_bytePos += length;
    m_bitsRead += length * 8;
    return true;
}

} // namespace eruption
