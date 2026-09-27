#pragma once

#include <cstdint>
#include <cstddef>
#include <cstring>
#include <string>
#include <vector>
#include <type_traits>

namespace eruption {

class BinaryReader {
public:
    BinaryReader(const uint8_t* data, size_t size);

    template<typename T>
    T read() {
        static_assert(std::is_arithmetic_v<T>, "BinaryReader::read<T> only supports arithmetic types");
        if (m_pos + sizeof(T) > m_size) {
            m_pos = m_size;
            return T{};
        }
        T value;
        std::memcpy(&value, m_data + m_pos, sizeof(T));
        m_pos += sizeof(T);
        return value;
    }

    uint8_t readU8()  { return read<uint8_t>(); }
    uint16_t readU16() { return read<uint16_t>(); }
    uint32_t readU32() { return read<uint32_t>(); }
    int8_t  readI8()  { return read<int8_t>(); }
    int16_t readI16() { return read<int16_t>(); }
    int32_t readI32() { return read<int32_t>(); }
    float readFloat() { return read<float>(); }

    // Big-endian variants
    uint16_t readU16BE();
    uint32_t readU32BE();
    int16_t  readI16BE();
    int32_t  readI32BE();
    float    readFloatBE();

    void readBytes(uint8_t* out, size_t count);
    std::vector<uint8_t> readBytes(size_t count);
    std::string readString(size_t length);
    std::string readCString();

    size_t pos() const { return m_pos; }
    void seek(size_t pos);
    void skip(size_t count);
    size_t remaining() const;
    bool eof() const { return m_pos >= m_size; }

    const uint8_t* data() const { return m_data; }
    size_t size() const { return m_size; }

private:
    const uint8_t* m_data;
    size_t m_size;
    size_t m_pos = 0;
};

} // namespace eruption
