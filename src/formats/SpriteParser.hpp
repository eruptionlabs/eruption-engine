#pragma once

#include "formats/SpriteTypes.hpp"
#include <cstdint>
#include <string>

namespace eruption {

// Parser for the open ERUPTSPR sprite format.
// Third-party sprite formats are no longer supported by the engine;
// use the conversion tools in eruption-tools to migrate source assets.
class SpriteParser {
public:
    struct Result {
        SpriteFile sprite;
        AnimFile anim;
        bool ok = false;
    };

    static Result parse(const uint8_t* data, size_t size);
    static Result parse(const std::vector<uint8_t>& data) {
        return parse(data.data(), data.size());
    }

    // Convenience: parse from disk.
    static Result parseFile(const std::string& path);
};

} // namespace eruption
