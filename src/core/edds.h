// EDDS (Enfusion engine DDS) reader.
// The container is unpacked here (block table, Enfusion LZ4 chunk stream, mip order);
// pixel formats are then decoded by DirectXTex.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "DirectXTex.h"

namespace edds {

struct Image {
    uint32_t width = 0;
    uint32_t height = 0;
    std::vector<uint8_t> bgra;  // top-down, straight alpha, stride = width * 4
};

class Texture {
public:
    // Unpacks an .edds file (or a plain DDS) held in memory.
    bool load(const uint8_t* data, size_t size, std::string* error = nullptr);

    size_t mipCount() const { return meta_.mipLevels; }
    uint32_t mipWidth(size_t mip) const;
    uint32_t mipHeight(size_t mip) const;
    DXGI_FORMAT format() const { return meta_.format; }

    // Converts one mipmap to 8-bit BGRA. Only the first face / array item / depth slice is used.
    // sRGB textures are kept as stored; HDR values are clipped; channels are shown as stored
    // (Workbench swizzle profiles are not recorded in the file).
    bool decode(size_t mip, Image& out) const;

    // Smallest mipmap whose sides still cover w x h (0 when none does).
    size_t pickMipmap(uint32_t w, uint32_t h) const;

private:
    DirectX::ScratchImage scratch_;
    DirectX::TexMetadata meta_ = {};
};

// Short display name of a pixel format ("BC7", "B8G8R8A8"...).
std::string formatName(DXGI_FORMAT format);

// Low-level codecs, exposed for tests.
// Enfusion LZ4 chunk stream: [u24 size][u8 flags, 0x80 = last][LZ4 block]..., each chunk
// decoding to at most 64 KiB and allowed to reference the previous 64 KiB of output.
bool lz4ChunkStream(const uint8_t* src, size_t srcSize, uint8_t* dst, size_t dstSize);

}  // namespace edds
