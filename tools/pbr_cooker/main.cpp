#include "utils/PbrMapGen.hpp"

#define STB_IMAGE_IMPLEMENTATION
#include <stb_image.h>
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include <stb_image_write.h>

#include <algorithm>
#include <cctype>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

static void printUsage(const char* argv0) {
    std::cerr << "Usage: " << argv0 << " --input <dir> --output <dir> [options]\n"
              << "Options:\n"
              << "  --strength <f>   Normal map strength (default 1.0)\n"
              << "  --force          Overwrite existing maps\n";
}

static std::string toLower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c){ return static_cast<char>(std::tolower(c)); });
    return s;
}

static bool isImageExtension(const fs::path& ext) {
    std::string e = toLower(ext.string());
    return e == ".png" || e == ".jpg" || e == ".jpeg" || e == ".bmp" || e == ".tga";
}

int main(int argc, char** argv) {
    fs::path inputDir;
    fs::path outputDir;
    float strength = 1.0f;
    bool force = false;

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--input" && i + 1 < argc) {
            inputDir = argv[++i];
        } else if (arg == "--output" && i + 1 < argc) {
            outputDir = argv[++i];
        } else if (arg == "--strength" && i + 1 < argc) {
            strength = std::stof(argv[++i]);
        } else if (arg == "--force") {
            force = true;
        } else if (arg == "--help" || arg == "-h") {
            printUsage(argv[0]);
            return 0;
        }
    }

    if (inputDir.empty() || outputDir.empty()) {
        printUsage(argv[0]);
        return 1;
    }

    if (!fs::exists(inputDir) || !fs::is_directory(inputDir)) {
        std::cerr << "Input directory does not exist: " << inputDir << "\n";
        return 1;
    }

    fs::create_directories(outputDir);

    int processed = 0;
    int skipped = 0;

    for (const auto& entry : fs::recursive_directory_iterator(inputDir)) {
        if (!entry.is_regular_file()) continue;
        if (!isImageExtension(entry.path().extension())) continue;

        fs::path relPath = fs::relative(entry.path(), inputDir);
        fs::path stem = relPath.stem();
        fs::path outDir = outputDir / relPath.parent_path();
        fs::create_directories(outDir);

        fs::path mrahwPath = outDir / (stem.string() + "_mrahw.png");
        fs::path normalPath = outDir / (stem.string() + "_normal.png");

        if (!force && fs::exists(mrahwPath) && fs::exists(normalPath)) {
            ++skipped;
            continue;
        }

        int w = 0, h = 0, channels = 0;
        unsigned char* data = stbi_load(entry.path().string().c_str(), &w, &h, &channels, 4);
        if (!data) {
            std::cerr << "Failed to load " << entry.path() << "\n";
            continue;
        }

        eruption::PbrMapSet maps = eruption::generatePbrMaps(data, w, h, stem.string(), strength);
        stbi_image_free(data);

        if (maps.mrahw.empty() || maps.normal.empty()) {
            std::cerr << "Failed to generate maps for " << entry.path() << "\n";
            continue;
        }

        if (!stbi_write_png(mrahwPath.string().c_str(), maps.width, maps.height, 4, maps.mrahw.data(), maps.width * 4)) {
            std::cerr << "Failed to write " << mrahwPath << "\n";
            continue;
        }
        if (!stbi_write_png(normalPath.string().c_str(), maps.width, maps.height, 4, maps.normal.data(), maps.width * 4)) {
            std::cerr << "Failed to write " << normalPath << "\n";
            continue;
        }

        std::cout << "Processed: " << relPath << " -> " << mrahwPath.filename() << " + "
                  << normalPath.filename() << "\n";
        ++processed;
    }

    std::cout << "Done. Processed " << processed << " textures, skipped " << skipped << ".\n";
    return 0;
}
