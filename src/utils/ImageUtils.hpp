#pragma once

#include <cstdint>
#include <vector>
#include <string>

namespace eruption {

struct ImageData {
    std::vector<uint8_t> pixels;
    int width = 0;
    int height = 0;
    int channels = 0;

    bool isValid() const { return width > 0 && height > 0 && !pixels.empty(); }
    size_t size() const { return pixels.size(); }
};

class ImageUtils {
public:
    static ImageData loadPNG(const std::string& filepath);
    // Pixels crus (stbi, `desired` canais; libere com stbi_image_free) do arquivo
    // no disco ou, sem ele, de um pacote .ecp do projeto (vfs). Nulo se falha.
    static uint8_t* loadRawVfs(const std::string& path, int& w, int& h, int& channels, int desired);
    static ImageData loadFromMemory(const uint8_t* data, size_t size);
    static ImageData loadFromMemoryRaw(const uint8_t* data, size_t size);
    static bool writePNG(const std::string& filepath, int width, int height, int channels, const uint8_t* data);
    // PNG em memoria (extrator do .tuf, previa do editor). false se falha.
    static bool encodePNG(int width, int height, int channels, const uint8_t* data, std::vector<uint8_t>& out);
    static void applyMagentaTransparencyToPixel(uint8_t& r, uint8_t& g, uint8_t& b, uint8_t& a);
    
    /**
     * Fills transparent pixels with the average color of neighboring opaque pixels.
     * Prevents black bleeding artifacts during linear filtering.
     */
    static void dilate(int width, int height, std::vector<uint8_t>& pixels);

    /**
     * Downscale an RGBA8 image so that neither dimension exceeds maxDimension.
     * Uses a simple 2x2 box filter. Returns true if the image was resized.
     */
    static bool downscaleRGBA(int& width, int& height, std::vector<uint8_t>& pixels, int maxDimension);
};

} // namespace eruption
