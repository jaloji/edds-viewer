#include "edds.h"

#include <algorithm>
#include <cstring>

namespace edds {

namespace {

uint32_t rd32(const uint8_t* p) { return uint32_t(p[0] | (p[1] << 8) | (p[2] << 16) | (uint32_t(p[3]) << 24)); }

bool fail(std::string* error, const char* message) {
    if (error) *error = message;
    return false;
}

constexpr size_t kDdsHeader = 128;     // "DDS " + DDS_HEADER
constexpr size_t kDx10Header = 20;     // DDS_HEADER_DXT10
constexpr size_t kChunkOutput = 65536;

// Decodes one LZ4 block into dst[op, limit), with matches allowed anywhere back in dst.
bool lz4Block(const uint8_t* src, size_t n, uint8_t* dst, size_t& op, size_t limit) {
    size_t ip = 0;
    for (;;) {
        if (ip >= n) return false;
        const uint8_t token = src[ip++];

        size_t lit = token >> 4;
        if (lit == 15) {
            uint8_t b;
            do {
                if (ip >= n) return false;
                b = src[ip++];
                lit += b;
            } while (b == 255);
        }
        if (ip + lit > n || op + lit > limit) return false;
        std::memcpy(dst + op, src + ip, lit);
        ip += lit;
        op += lit;
        if (ip == n) return true;  // the last sequence has literals only

        if (ip + 2 > n) return false;
        const size_t offset = src[ip] | (size_t(src[ip + 1]) << 8);
        ip += 2;
        if (offset == 0 || offset > op) return false;

        size_t len = (token & 15) + 4;
        if ((token & 15) == 15) {
            uint8_t b;
            do {
                if (ip >= n) return false;
                b = src[ip++];
                len += b;
            } while (b == 255);
        }
        if (op + len > limit) return false;
        for (size_t i = 0; i < len; ++i, ++op) dst[op] = dst[op - offset];  // may overlap
    }
}

}  // namespace

bool lz4ChunkStream(const uint8_t* src, size_t srcSize, uint8_t* dst, size_t dstSize) {
    size_t ip = 0, op = 0;
    for (;;) {
        if (ip + 4 > srcSize) return false;
        const size_t csize = src[ip] | (size_t(src[ip + 1]) << 8) | (size_t(src[ip + 2]) << 16);
        const uint8_t flags = src[ip + 3];
        ip += 4;
        if ((flags & 0x7F) != 0 || csize == 0 || ip + csize > srcSize) return false;
        const size_t limit = std::min(dstSize, op + kChunkOutput);
        if (!lz4Block(src + ip, csize, dst, op, limit)) return false;
        ip += csize;
        if (flags & 0x80) return op == dstSize;
    }
}

bool Texture::load(const uint8_t* data, size_t size, std::string* error) {
    using namespace DirectX;
    scratch_.Release();
    meta_ = {};

    if (size < kDdsHeader || std::memcmp(data, "DDS ", 4) != 0) return fail(error, "not a DDS / EDDS file");
    size_t headerSize = kDdsHeader;
    if (std::memcmp(data + 84, "DX10", 4) == 0) headerSize += kDx10Header;
    if (size < headerSize) return fail(error, "truncated header");

    uint32_t mips = rd32(data + 28);
    if (mips == 0) mips = 1;
    if (mips > 32) return fail(error, "bad mipmap count");

    const uint8_t* table = data + headerSize;
    const bool hasTable = size >= headerSize + 4 &&
                          (std::memcmp(table, "COPY", 4) == 0 || std::memcmp(table, "LZ4 ", 4) == 0);

    HRESULT hr;
    if (!hasTable) {
        // No block table: the payload is laid out like a plain DDS.
        hr = LoadFromDDSMemory(data, size, DDS_FLAGS_PERMISSIVE, &meta_, scratch_);
    } else {
        if (size < headerSize + size_t(mips) * 8) return fail(error, "truncated block table");

        // Blocks are stored from the smallest mipmap to the largest; DDS wants the reverse.
        std::vector<std::vector<uint8_t>> levels(mips);
        size_t body = headerSize + size_t(mips) * 8;
        size_t total = 0;
        for (uint32_t i = 0; i < mips; ++i) {
            const uint8_t* entry = table + size_t(i) * 8;
            const uint32_t blockSize = rd32(entry + 4);
            if (blockSize > size - body) return fail(error, "truncated block");
            const uint8_t* src = data + body;
            std::vector<uint8_t>& level = levels[mips - 1 - i];
            if (std::memcmp(entry, "COPY", 4) == 0) {
                level.assign(src, src + blockSize);
            } else if (std::memcmp(entry, "LZ4 ", 4) == 0) {
                if (blockSize < 4) return fail(error, "bad LZ4 block");
                const uint32_t rawSize = rd32(src);
                if (rawSize == 0 || rawSize > (1u << 30)) return fail(error, "bad LZ4 block size");
                level.resize(rawSize);
                if (!lz4ChunkStream(src + 4, blockSize - 4, level.data(), rawSize))
                    return fail(error, "LZ4 data corrupt");
            } else {
                return fail(error, "unknown block type");
            }
            total += level.size();
            body += blockSize;
        }

        std::vector<uint8_t> dds;
        dds.reserve(headerSize + total);
        dds.insert(dds.end(), data, data + headerSize);
        for (const auto& level : levels) dds.insert(dds.end(), level.begin(), level.end());
        hr = LoadFromDDSMemory(dds.data(), dds.size(), DDS_FLAGS_PERMISSIVE, &meta_, scratch_);
    }
    if (FAILED(hr)) return fail(error, "unsupported or corrupt DDS payload");

    if (IsVideo(meta_.format) || IsPlanar(meta_.format) || IsTypeless(meta_.format, false) ||
        IsDepthStencil(meta_.format) || meta_.width == 0 || meta_.height == 0) {
        scratch_.Release();
        return fail(error, "pixel format cannot be displayed");
    }
    return true;
}

uint32_t Texture::mipWidth(size_t mip) const { return uint32_t(std::max<size_t>(1, meta_.width >> mip)); }

uint32_t Texture::mipHeight(size_t mip) const { return uint32_t(std::max<size_t>(1, meta_.height >> mip)); }

bool Texture::decode(size_t mip, Image& out) const {
    using namespace DirectX;
    const DirectX::Image* img = scratch_.GetImage(mip, 0, 0);
    if (!img) return false;

    // sRGB textures are shown as stored (no gamma change), like any sRGB image on screen.
    const DXGI_FORMAT target = IsSRGB(img->format) ? DXGI_FORMAT_B8G8R8A8_UNORM_SRGB : DXGI_FORMAT_B8G8R8A8_UNORM;

    ScratchImage decompressed, converted;
    const DirectX::Image* cur = img;
    if (IsCompressed(cur->format)) {
        if (FAILED(Decompress(*cur, DXGI_FORMAT_UNKNOWN, decompressed))) return false;
        cur = decompressed.GetImage(0, 0, 0);
    }
    if (cur->format != target) {
        // DirectXTex's own converter: its WIC path would need COM, which XnView and tools may not set up.
        if (FAILED(Convert(*cur, target, TEX_FILTER_FORCE_NON_WIC, TEX_THRESHOLD_DEFAULT, converted))) return false;
        cur = converted.GetImage(0, 0, 0);
    }

    out.width = uint32_t(cur->width);
    out.height = uint32_t(cur->height);
    out.bgra.resize(size_t(out.width) * out.height * 4);
    for (uint32_t y = 0; y < out.height; ++y)
        std::memcpy(&out.bgra[size_t(y) * out.width * 4], cur->pixels + y * cur->rowPitch, size_t(out.width) * 4);
    return true;
}

size_t Texture::pickMipmap(uint32_t w, uint32_t h) const {
    size_t best = 0;
    for (size_t i = 0; i < meta_.mipLevels; ++i)
        if (mipWidth(i) >= w && mipHeight(i) >= h) best = i;
    return best;
}

std::string formatName(DXGI_FORMAT format) {
    switch (format) {
        case DXGI_FORMAT_BC1_UNORM: case DXGI_FORMAT_BC1_UNORM_SRGB: return "BC1";
        case DXGI_FORMAT_BC2_UNORM: case DXGI_FORMAT_BC2_UNORM_SRGB: return "BC2";
        case DXGI_FORMAT_BC3_UNORM: case DXGI_FORMAT_BC3_UNORM_SRGB: return "BC3";
        case DXGI_FORMAT_BC4_UNORM: case DXGI_FORMAT_BC4_SNORM: return "BC4";
        case DXGI_FORMAT_BC5_UNORM: case DXGI_FORMAT_BC5_SNORM: return "BC5";
        case DXGI_FORMAT_BC6H_UF16: case DXGI_FORMAT_BC6H_SF16: return "BC6H";
        case DXGI_FORMAT_BC7_UNORM: case DXGI_FORMAT_BC7_UNORM_SRGB: return "BC7";
        case DXGI_FORMAT_B8G8R8A8_UNORM: case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB: return "BGRA8";
        case DXGI_FORMAT_B8G8R8X8_UNORM: case DXGI_FORMAT_B8G8R8X8_UNORM_SRGB: return "BGRX8";
        case DXGI_FORMAT_R8G8B8A8_UNORM: case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB: return "RGBA8";
        case DXGI_FORMAT_R8G8_UNORM: return "RG8";
        case DXGI_FORMAT_R8_UNORM: return "R8";
        case DXGI_FORMAT_A8_UNORM: return "A8";
        case DXGI_FORMAT_R16G16B16A16_FLOAT: return "RGBA16F";
        case DXGI_FORMAT_R32G32B32A32_FLOAT: return "RGBA32F";
        default: return "DXGI " + std::to_string(int(format));
    }
}

}  // namespace edds
