#include "utils/BinaryReader.hpp"

namespace eruption {

BinaryReader::BinaryReader(const uint8_t* data, size_t size)
    : m_data(data), m_size(size), m_pos(0) {}

void BinaryReader::readBytes(uint8_t* out, size_t count) {
    size_t toRead = count;
    if (m_pos + toRead > m_size) {
        toRead = m_size > m_pos ? m_size - m_pos : 0;
    }
    if (toRead > 0 && out != nullptr) {
        std::memcpy(out, m_data + m_pos, toRead);
    }
    m_pos += toRead;
}

std::vector<uint8_t> BinaryReader::readBytes(size_t count) {
    std::vector<uint8_t> result;
    size_t toRead = count;
    if (m_pos + toRead > m_size) {
        toRead = m_size > m_pos ? m_size - m_pos : 0;
    }
    if (toRead > 0) {
        result.resize(toRead);
        std::memcpy(result.data(), m_data + m_pos, toRead);
    }
    m_pos += toRead;
    return result;
}

std::string BinaryReader::readString(size_t length) {
    std::string result;
    size_t toRead = length;
    if (m_pos + toRead > m_size) {
        toRead = m_size > m_pos ? m_size - m_pos : 0;
    }
    if (toRead > 0) {
        result.assign(reinterpret_cast<const char*>(m_data + m_pos), toRead);
    }
    m_pos += toRead;
    return result;
}

std::string BinaryReader::readCString() {
    std::string result;
    while (m_pos < m_size && m_data[m_pos] != '\0') {
        result.push_back(static_cast<char>(m_data[m_pos]));
        ++m_pos;
    }
    if (m_pos < m_size && m_data[m_pos] == '\0') {
        ++m_pos; // skip null terminator
    }
    return result;
}

void BinaryReader::seek(size_t pos) {
    m_pos = pos > m_size ? m_size : pos;
}

void BinaryReader::skip(size_t count) {
    m_pos += count;
    if (m_pos > m_size) m_pos = m_size;
}

size_t BinaryReader::remaining() const {
    return m_pos >= m_size ? 0 : m_size - m_pos;
}

static inline uint16_t swap16(uint16_t v) {
    return static_cast<uint16_t>((v >> 8) | (v << 8));
}
static inline uint32_t swap32(uint32_t v) {
    return ((v >> 24) & 0x000000FF) |
           ((v >>  8) & 0x0000FF00) |
           ((v <<  8) & 0x00FF0000) |
           ((v << 24) & 0xFF000000);
}

uint16_t BinaryReader::readU16BE() {
    return swap16(readU16());
}

uint32_t BinaryReader::readU32BE() {
    return swap32(readU32());
}

int16_t BinaryReader::readI16BE() {
    return static_cast<int16_t>(swap16(static_cast<uint16_t>(readI16())));
}

int32_t BinaryReader::readI32BE() {
    return static_cast<int32_t>(swap32(static_cast<uint32_t>(readI32())));
}

float BinaryReader::readFloatBE() {
    uint32_t raw = readU32BE();
    float v;
    std::memcpy(&v, &raw, sizeof(v));
    return v;
}

} // namespace eruption
