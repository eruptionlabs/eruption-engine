#include "utils/ImageUtils.hpp"
#include "core/Logger.hpp"

#include <algorithm>

#define STB_IMAGE_IMPLEMENTATION
#include <stb_image.h>

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include <stb_image_write.h>

namespace eruption {

ImageData ImageUtils::loadPNG(const std::string& filepath) {
    ImageData result;
    uint8_t* data = stbi_load(filepath.c_str(), &result.width, &result.height, &result.channels, 4);
    if (!data) {
        ERUPTION_LOG_ERROR("Failed to load image: %s", filepath.c_str());
        return result;
    }
    result.pixels.assign(data, data + result.width * result.height * 4);
    result.channels = 4;
    stbi_image_free(data);

    // Apply magenta chroma key transparency (FF00FF) with tolerance
    for (size_t i = 0; i < result.pixels.size(); i += 4) {
        applyMagentaTransparencyToPixel(result.pixels[i], result.pixels[i+1], result.pixels[i+2], result.pixels[i+3]);
    }

    // Dilate to prevent color bleeding during linear filtering / mipmap generation
    dilate(result.width, result.height, result.pixels);

    return result;
}

void ImageUtils::applyMagentaTransparencyToPixel(uint8_t& r, uint8_t& g, uint8_t& b, uint8_t& a) {
    if (r > 230 && g < 25 && b > 230) {
        // NÃO zerar R,G,B aqui. Manter a cor (magenta ou similar) 
        // para que o dilate tenha uma base, ou simplesmente marcar alpha 0.
        // O dilate vai sobrescrever isso com cores vizinhas reais.
        a = 0;
    }
}

void ImageUtils::dilate(int width, int height, std::vector<uint8_t>& pixels) {
    const int n = width * height;
    if (n <= 0 || pixels.size() < static_cast<size_t>(n) * 4) return;

    // Marca os pixels transparentes (alpha==0 ou magenta puro) in-place.
    std::vector<uint8_t> transparent(n, 0);
    int transparentCount = 0;
    for (int i = 0; i < n; ++i) {
        size_t idx = static_cast<size_t>(i) * 4;
        if (pixels[idx + 3] == 0 ||
            (pixels[idx] == 255 && pixels[idx + 1] == 0 && pixels[idx + 2] == 255)) {
            transparent[i] = 1;
            pixels[idx + 3] = 0; // Força alpha 0
            ++transparentCount;
        }
    }
    if (transparentCount == 0) return; // caso comum: nada a dilatar

    // BFS multi-fonte a partir dos pixels opacos: cada transparente recebe a
    // cor do opaco mais próximo (alpha continua 0). Equivale à dilatação
    // multi-pass com passes ilimitados, em O(n) e sem cópias do bitmap.
    std::vector<int32_t> queue;
    queue.reserve(n - transparentCount);
    for (int i = 0; i < n; ++i) {
        if (!transparent[i]) queue.push_back(i);
    }
    static const int dirs[8][2] = {
        {-1, 0}, {1, 0}, {0, -1}, {0, 1},
        {-1, -1}, {-1, 1}, {1, -1}, {1, 1}
    };
    for (size_t head = 0; head < queue.size(); ++head) {
        const int i = queue[head];
        const int x = i % width;
        const int y = i / width;
        const size_t idx = static_cast<size_t>(i) * 4;
        for (const auto& dir : dirs) {
            const int nx = x + dir[0];
            const int ny = y + dir[1];
            if (nx < 0 || nx >= width || ny < 0 || ny >= height) continue;
            const int ni = ny * width + nx;
            if (!transparent[ni]) continue;
            transparent[ni] = 0;
            const size_t nidx = static_cast<size_t>(ni) * 4;
            pixels[nidx + 0] = pixels[idx + 0];
            pixels[nidx + 1] = pixels[idx + 1];
            pixels[nidx + 2] = pixels[idx + 2];
            pixels[nidx + 3] = 0; // Mantém transparente
            queue.push_back(ni);
        }
    }

    // Imagem 100% transparente (fila vazia): vira preto para evitar bleeding.
    if (queue.empty()) {
        std::fill(pixels.begin(), pixels.begin() + static_cast<size_t>(n) * 4, 0);
    }
}

ImageData ImageUtils::loadFromMemory(const uint8_t* data, size_t size) {
    ImageData result;
    uint8_t* pixels = stbi_load_from_memory(data, static_cast<int>(size), &result.width, &result.height, &result.channels, 4);
    if (!pixels) {
        ERUPTION_LOG_ERROR("Failed to load image from memory");
        return result;
    }
    result.pixels.assign(pixels, pixels + result.width * result.height * 4);
    result.channels = 4;
    stbi_image_free(pixels);

    // Apply magenta chroma key transparency (FF00FF)
    for (size_t i = 0; i < result.pixels.size(); i += 4) {
        applyMagentaTransparencyToPixel(result.pixels[i], result.pixels[i+1], result.pixels[i+2], result.pixels[i+3]);
    }

    // Dilate to fix black bleeding
    dilate(result.width, result.height, result.pixels);

    return result;
}

ImageData ImageUtils::loadFromMemoryRaw(const uint8_t* data, size_t size) {
    ImageData result;
    uint8_t* pixels = stbi_load_from_memory(data, static_cast<int>(size), &result.width, &result.height, &result.channels, 4);
    if (!pixels) {
        ERUPTION_LOG_ERROR("Failed to load image from memory (raw)");
        return result;
    }
    result.pixels.assign(pixels, pixels + result.width * result.height * 4);
    result.channels = 4;
    stbi_image_free(pixels);
    return result;
}

bool ImageUtils::writePNG(const std::string& filepath, int width, int height, int channels, const uint8_t* data) {
    int stride = width * channels;
    int result = stbi_write_png(filepath.c_str(), width, height, channels, data, stride);
    return result != 0;
}

bool ImageUtils::downscaleRGBA(int& width, int& height, std::vector<uint8_t>& pixels, int maxDimension) {
    if (width <= 0 || height <= 0 || pixels.size() != static_cast<size_t>(width) * height * 4) {
        return false;
    }
    int maxSide = std::max(width, height);
    if (maxSide <= maxDimension) {
        return false;
    }

    // Compute new size preserving aspect ratio; both dimensions must stay <= maxDimension.
    float scale = static_cast<float>(maxDimension) / static_cast<float>(maxSide);
    int newWidth = std::max(1, static_cast<int>(std::floor(width * scale)));
    int newHeight = std::max(1, static_cast<int>(std::floor(height * scale)));

    std::vector<uint8_t> out(static_cast<size_t>(newWidth) * newHeight * 4, 0);
    // Média de área com amostragem no CENTRO do bloco. A versão anterior pegava
    // o texel do CANTO ((y*height)/newHeight), o que desloca a imagem meio bloco
    // para cima/esquerda: como só o mapa PBR sintetizado passa por aqui (o
    // albedo sobe em resolução cheia), o normal nascia DESLOCADO do albedo -
    // relevo fora de registro com a textura em qualquer iluminação. Medido por
    // correlação cruzada: 2 pixels de tela.
    for (int y = 0; y < newHeight; ++y) {
        const int y0 = (y * height) / newHeight;
        const int y1 = std::max(y0 + 1, ((y + 1) * height) / newHeight);
        for (int x = 0; x < newWidth; ++x) {
            const int x0 = (x * width) / newWidth;
            const int x1 = std::max(x0 + 1, ((x + 1) * width) / newWidth);
            uint32_t acc[4] = {0, 0, 0, 0};
            uint32_t n = 0;
            for (int sy = y0; sy < y1 && sy < height; ++sy) {
                for (int sx = x0; sx < x1 && sx < width; ++sx) {
                    const size_t si = (static_cast<size_t>(sy) * width + sx) * 4;
                    acc[0] += pixels[si]; acc[1] += pixels[si + 1];
                    acc[2] += pixels[si + 2]; acc[3] += pixels[si + 3];
                    ++n;
                }
            }
            if (n == 0) n = 1;
            const size_t di = (static_cast<size_t>(y) * newWidth + x) * 4;
            out[di]     = static_cast<uint8_t>(acc[0] / n);
            out[di + 1] = static_cast<uint8_t>(acc[1] / n);
            out[di + 2] = static_cast<uint8_t>(acc[2] / n);
            out[di + 3] = static_cast<uint8_t>(acc[3] / n);
        }
    }
    pixels = std::move(out);
    width = newWidth;
    height = newHeight;
    return true;
}

} // namespace eruption
