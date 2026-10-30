// Xedds.usr: XnView MP read plugin for Enfusion .edds textures (XnView PluginsSDK gfp* API).
// Install: copy Xedds.usr to <XnView MP>\Plugins\ and restart XnView MP.
#include <windows.h>

#include <cstring>
#include <new>
#include <string>
#include <vector>

#include "edds.h"

#define GFP_API __stdcall

#define GFP_RGB 0
#define GFP_BGR 1

#define GFP_READ 0x0001

struct GFP_COLORMAP {
    unsigned char red[256];
    unsigned char green[256];
    unsigned char blue[256];
};

namespace {

struct Picture {
    std::string format;
    uint32_t width = 0;
    uint32_t height = 0;
    std::vector<uint8_t> rgba;  // rows in XnView order (R, G, B, A)
};

// XnView MP passes narrow paths: try the ANSI code page first, then UTF-8.
bool readFile(LPCSTR name, std::vector<uint8_t>& out) {
    for (UINT cp : {CP_ACP, CP_UTF8}) {
        int n = MultiByteToWideChar(cp, MB_ERR_INVALID_CHARS, name, -1, nullptr, 0);
        if (n <= 0) continue;
        std::wstring wide(size_t(n), L'\0');
        MultiByteToWideChar(cp, MB_ERR_INVALID_CHARS, name, -1, &wide[0], n);
        HANDLE h = CreateFileW(wide.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                               FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h == INVALID_HANDLE_VALUE) continue;
        LARGE_INTEGER size;
        bool ok = GetFileSizeEx(h, &size) && size.QuadPart > 0 && size.QuadPart < (1LL << 31);
        if (ok) {
            out.resize(size_t(size.QuadPart));
            DWORD got = 0;
            ok = ReadFile(h, out.data(), DWORD(out.size()), &got, nullptr) && got == out.size();
        }
        CloseHandle(h);
        if (ok) return true;
    }
    return false;
}

void* open(LPCSTR filename) {
    std::vector<uint8_t> data;
    if (!filename || !readFile(filename, data)) return nullptr;
    edds::Texture tex;
    edds::Image img;
    if (!tex.load(data.data(), data.size()) || !tex.decode(0, img)) return nullptr;
    data.clear();
    data.shrink_to_fit();

    Picture* pic = new (std::nothrow) Picture;
    if (!pic) return nullptr;
    pic->format = edds::formatName(tex.format());
    pic->width = img.width;
    pic->height = img.height;
    pic->rgba.resize(img.bgra.size());
    for (size_t i = 0; i < img.bgra.size(); i += 4) {
        pic->rgba[i] = img.bgra[i + 2];
        pic->rgba[i + 1] = img.bgra[i + 1];
        pic->rgba[i + 2] = img.bgra[i];
        pic->rgba[i + 3] = img.bgra[i + 3];
    }
    return pic;
}

}  // namespace

extern "C" {

BOOL GFP_API gfpGetPluginInfo(DWORD version, LPSTR label, INT label_max_size, LPSTR extension,
                              INT extension_max_size, INT* support) {
    if (version != 0x0002) return FALSE;
    strncpy_s(label, label_max_size, "Enfusion texture (EDDS)", _TRUNCATE);
    strncpy_s(extension, extension_max_size, "edds", _TRUNCATE);
    *support = GFP_READ;
    return TRUE;
}

void* GFP_API gfpLoadPictureInit(LPCSTR filename) { return open(filename); }

// Not in the original SDK; XnView MP calls it with a narrow path as well.
void* GFP_API gfpLoadPictureInitEx(LPCSTR filename) { return open(filename); }

BOOL GFP_API gfpLoadPictureGetInfo(void* ptr, INT* pictype, INT* width, INT* height, INT* dpi, INT* bits_per_pixel,
                                   INT* bytes_per_line, BOOL* has_colormap, LPSTR label, INT label_max_size) {
    const Picture* pic = static_cast<const Picture*>(ptr);
    if (!pic) return FALSE;
    *pictype = GFP_RGB;
    *width = INT(pic->width);
    *height = INT(pic->height);
    *dpi = 72;
    *bits_per_pixel = 32;
    *bytes_per_line = INT(pic->width * 4);
    *has_colormap = FALSE;
    std::string text = "EDDS " + pic->format;
    strncpy_s(label, label_max_size, text.c_str(), _TRUNCATE);
    return TRUE;
}

BOOL GFP_API gfpLoadPictureGetLine(void* ptr, INT line, unsigned char* buffer) {
    const Picture* pic = static_cast<const Picture*>(ptr);
    if (!pic || line < 0 || UINT(line) >= pic->height) return FALSE;
    const size_t stride = size_t(pic->width) * 4;
    std::memcpy(buffer, pic->rgba.data() + stride * size_t(line), stride);
    return TRUE;
}

BOOL GFP_API gfpLoadPictureGetColormap(void*, GFP_COLORMAP*) { return FALSE; }

void GFP_API gfpLoadPictureExit(void* ptr) { delete static_cast<Picture*>(ptr); }

BOOL GFP_API gfpSavePictureIsSupported(INT, INT, INT, BOOL) { return FALSE; }

void* GFP_API gfpSavePictureInit(LPCSTR, INT, INT, INT, INT, INT*, LPSTR, INT) { return nullptr; }

BOOL GFP_API gfpSavePicturePutLine(void*, INT, const unsigned char*) { return FALSE; }

void GFP_API gfpSavePictureExit(void*) {}

}  // extern "C"
