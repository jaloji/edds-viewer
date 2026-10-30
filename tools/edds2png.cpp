// edds2png: prints EDDS header info and writes a mipmap as PNG.
// Usage: edds2png <in.edds> [out.png] [--mip N]
#include "edds.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

namespace {

uint32_t crc32(const uint8_t* p, size_t n, uint32_t c = 0xFFFFFFFFu) {
    for (size_t i = 0; i < n; ++i) {
        c ^= p[i];
        for (int k = 0; k < 8; ++k) c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1)));
    }
    return c;
}

void be32(std::vector<uint8_t>& v, uint32_t x) {
    v.push_back(uint8_t(x >> 24)); v.push_back(uint8_t(x >> 16)); v.push_back(uint8_t(x >> 8)); v.push_back(uint8_t(x));
}

void chunk(std::vector<uint8_t>& png, const char* type, const std::vector<uint8_t>& body) {
    be32(png, uint32_t(body.size()));
    size_t start = png.size();
    png.insert(png.end(), type, type + 4);
    png.insert(png.end(), body.begin(), body.end());
    be32(png, crc32(&png[start], png.size() - start) ^ 0xFFFFFFFFu);
}

// RGBA8 PNG using stored (uncompressed) deflate blocks: no zlib needed.
bool writePng(const char* path, const edds::Image& img) {
    std::vector<uint8_t> raw;
    raw.reserve((size_t(img.width) * 4 + 1) * img.height);
    for (uint32_t y = 0; y < img.height; ++y) {
        raw.push_back(0);
        const uint8_t* row = &img.bgra[size_t(y) * img.width * 4];
        for (uint32_t x = 0; x < img.width; ++x) {
            raw.push_back(row[x * 4 + 2]); raw.push_back(row[x * 4 + 1]);
            raw.push_back(row[x * 4 + 0]); raw.push_back(row[x * 4 + 3]);
        }
    }
    std::vector<uint8_t> z = {0x78, 0x01};
    uint32_t a = 1, b = 0;
    for (uint8_t c : raw) { a = (a + c) % 65521; b = (b + a) % 65521; }
    for (size_t off = 0; off < raw.size(); off += 65535) {
        size_t n = raw.size() - off < 65535 ? raw.size() - off : 65535;
        z.push_back(off + n >= raw.size() ? 1 : 0);
        z.push_back(uint8_t(n)); z.push_back(uint8_t(n >> 8));
        z.push_back(uint8_t(~n)); z.push_back(uint8_t(~n >> 8));
        z.insert(z.end(), raw.begin() + off, raw.begin() + off + n);
    }
    be32(z, (b << 16) | a);

    std::vector<uint8_t> png = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
    std::vector<uint8_t> ihdr;
    be32(ihdr, img.width); be32(ihdr, img.height);
    ihdr.insert(ihdr.end(), {8, 6, 0, 0, 0});
    chunk(png, "IHDR", ihdr);
    chunk(png, "IDAT", z);
    chunk(png, "IEND", {});

    FILE* f = std::fopen(path, "wb");
    if (!f) return false;
    bool ok = std::fwrite(png.data(), 1, png.size(), f) == png.size();
    return std::fclose(f) == 0 && ok;
}

}  // namespace

int main(int argc, char** argv) {
    const char* in = nullptr;
    const char* out = nullptr;
    size_t mip = 0;
    for (int i = 1; i < argc; ++i) {
        if (!std::strcmp(argv[i], "--mip") && i + 1 < argc) mip = std::strtoul(argv[++i], nullptr, 10);
        else if (!in) in = argv[i];
        else out = argv[i];
    }
    if (!in) {
        std::fprintf(stderr, "usage: edds2png <in.edds> [out.png] [--mip N]\n");
        return 2;
    }

    std::ifstream file(in, std::ios::binary);
    std::vector<uint8_t> data((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    if (data.empty()) {
        std::fprintf(stderr, "%s: cannot read\n", in);
        return 1;
    }

    edds::Texture tex;
    std::string err;
    if (!tex.load(data.data(), data.size(), &err)) {
        std::fprintf(stderr, "%s: %s\n", in, err.c_str());
        return 1;
    }
    std::printf("%s: %s, %ux%u, %zu mipmaps\n", in, edds::formatName(tex.format()).c_str(), tex.mipWidth(0),
                tex.mipHeight(0), tex.mipCount());

    int status = 0;
    for (size_t i = 0; i < tex.mipCount(); ++i) {
        edds::Image img;
        if (!tex.decode(i, img)) {
            std::fprintf(stderr, "  mip %zu: decode failed\n", i);
            status = 1;
            continue;
        }
        if (out && i == mip && !writePng(out, img)) {
            std::fprintf(stderr, "%s: cannot write\n", out);
            status = 1;
        }
    }
    return status;
}
