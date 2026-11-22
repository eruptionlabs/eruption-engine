#pragma once

#include "utils/BinaryReader.hpp"
#include "math/Types.hpp"
#include "formats/NavGridTypes.hpp"
#include <vector>
#include <string>

namespace eruption {

class NavGridParser {
public:
    static NavGridFile parse(const uint8_t* data, size_t size);
    static NavGridFile parse(const std::vector<uint8_t>& data) {
        return parse(data.data(), data.size());
    }
};

} // namespace eruption
