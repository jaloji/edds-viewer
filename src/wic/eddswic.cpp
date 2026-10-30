// EddsWic.dll: Windows Imaging Component decoder for Enfusion .edds textures.
// Gives Explorer thumbnails, the preview pane and the Windows photo viewers through WIC.
// Install: regsvr32 EddsWic.dll (elevated). Uninstall: regsvr32 /u EddsWic.dll.
#include <windows.h>
#include <olectl.h>
#include <shlobj.h>
#include <wincodec.h>

#include <atomic>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "edds.h"

namespace {

// {8E4AD0BE-3DBB-4E2B-BF56-69A27A32922C}
const CLSID CLSID_EddsDecoder = {0x8e4ad0be, 0x3dbb, 0x4e2b, {0xbf, 0x56, 0x69, 0xa2, 0x7a, 0x32, 0x92, 0x2c}};
// {E7E86432-F449-4412-B529-D49210C7489B}
const GUID GUID_ContainerFormatEdds = {0xe7e86432, 0xf449, 0x4412, {0xb5, 0x29, 0xd4, 0x92, 0x10, 0xc7, 0x48, 0x9b}};
// {4D7715CD-8D5A-4090-88D9-C5DF78DCF8CB}
const GUID GUID_VendorEddsViewer = {0x4d7715cd, 0x8d5a, 0x4090, {0x88, 0xd9, 0xc5, 0xdf, 0x78, 0xdc, 0xf8, 0xcb}};

const wchar_t kFriendlyName[] = L"EDDS Texture Decoder (Enfusion)";
const wchar_t kExtension[] = L".edds";

// Shell handlers that render any WIC-decodable image.
const wchar_t kThumbnailHandlerKey[] = L"ShellEx\\{e357fccd-a995-4576-b01f-234630154e96}";
const wchar_t kPhotoThumbnailProvider[] = L"{C7657C4A-9F68-40fa-A4DF-96BC08EB3551}";
const wchar_t kPreviewHandlerKey[] = L"ShellEx\\{8895b1c6-b41f-4c1c-a562-0d564250836f}";
const wchar_t kPhotoPreviewHandler[] = L"{FFE2A43C-56B9-4bf5-9A79-CC6D4285608A}";

HMODULE g_module = nullptr;
std::atomic<long> g_objects{0};
std::atomic<long> g_locks{0};

// Minimal IUnknown implementation shared by the COM objects below.
template <class Derived>
class ComBase {
public:
    ComBase() { ++g_objects; }
    virtual ~ComBase() { --g_objects; }

protected:
    ULONG addRef() { return ++refs_; }
    ULONG release() {
        ULONG r = --refs_;
        if (r == 0) delete static_cast<Derived*>(this);
        return r;
    }

private:
    std::atomic<ULONG> refs_{1};
};

// Loaded file shared by the decoder and its frame. Mipmaps are converted to BGRA on first use.
struct Texture {
    edds::Texture tex;
    std::mutex lock;
    std::vector<std::unique_ptr<edds::Image>> mips;
    std::atomic<ULONG> refs{1};

    void addRef() { ++refs; }
    void release() {
        if (--refs == 0) delete this;
    }

    const edds::Image* mip(size_t i) {
        std::lock_guard<std::mutex> guard(lock);
        if (i >= tex.mipCount()) return nullptr;
        if (mips.empty()) mips.resize(tex.mipCount());
        if (!mips[i]) {
            auto img = std::make_unique<edds::Image>();
            if (!tex.decode(i, *img)) return nullptr;
            mips[i] = std::move(img);
        }
        return mips[i].get();
    }
};

HRESULT readStream(IStream* stream, std::vector<uint8_t>& out) {
    if (!stream) return E_INVALIDARG;
    STATSTG st = {};
    HRESULT hr = stream->Stat(&st, STATFLAG_NONAME);
    if (FAILED(hr)) return hr;
    if (st.cbSize.QuadPart < 128 || st.cbSize.QuadPart > (1ULL << 31)) return WINCODEC_ERR_BADIMAGE;
    LARGE_INTEGER zero = {};
    hr = stream->Seek(zero, STREAM_SEEK_SET, nullptr);
    if (FAILED(hr)) return hr;
    out.resize(size_t(st.cbSize.QuadPart));
    size_t done = 0;
    while (done < out.size()) {
        ULONG got = 0;
        hr = stream->Read(out.data() + done, ULONG(out.size() - done), &got);
        if (FAILED(hr)) return hr;
        if (got == 0) break;
        done += got;
    }
    return done == out.size() ? S_OK : WINCODEC_ERR_BADIMAGE;
}

// An EDDS file is a DDS header followed by a COPY / LZ4 block table. Plain DDS files are left
// to the DDS decoders.
bool hasBlockTable(const std::vector<uint8_t>& d) {
    size_t at = std::memcmp(d.data() + 84, "DX10", 4) == 0 ? 148 : 128;
    return std::memcmp(d.data(), "DDS ", 4) == 0 && d.size() >= at + 8 &&
           (std::memcmp(&d[at], "COPY", 4) == 0 || std::memcmp(&d[at], "LZ4 ", 4) == 0);
}

HRESULT copyRect(const edds::Image& img, const WICRect* prc, UINT stride, UINT bufferSize, BYTE* buffer) {
    WICRect full = {0, 0, INT(img.width), INT(img.height)};
    const WICRect& r = prc ? *prc : full;
    if (r.X < 0 || r.Y < 0 || r.Width < 0 || r.Height < 0 || UINT(r.X + r.Width) > img.width ||
        UINT(r.Y + r.Height) > img.height)
        return E_INVALIDARG;
    const UINT rowBytes = UINT(r.Width) * 4;
    if (stride < rowBytes) return E_INVALIDARG;
    if (r.Height > 0 && UINT64(stride) * (r.Height - 1) + rowBytes > bufferSize) return WINCODEC_ERR_INSUFFICIENTBUFFER;
    for (INT y = 0; y < r.Height; ++y)
        std::memcpy(buffer + size_t(stride) * y, &img.bgra[(size_t(r.Y + y) * img.width + r.X) * 4], rowBytes);
    return S_OK;
}

class Frame : public ComBase<Frame>, public IWICBitmapFrameDecode, public IWICBitmapSourceTransform {
public:
    explicit Frame(Texture* tex) : tex_(tex) { tex_->addRef(); }
    ~Frame() override { tex_->release(); }

    // IUnknown
    STDMETHODIMP QueryInterface(REFIID iid, void** out) override {
        if (!out) return E_POINTER;
        if (iid == IID_IUnknown || iid == IID_IWICBitmapSource || iid == IID_IWICBitmapFrameDecode)
            *out = static_cast<IWICBitmapFrameDecode*>(this);
        else if (iid == IID_IWICBitmapSourceTransform)
            *out = static_cast<IWICBitmapSourceTransform*>(this);
        else {
            *out = nullptr;
            return E_NOINTERFACE;
        }
        addRef();
        return S_OK;
    }
    STDMETHODIMP_(ULONG) AddRef() override { return addRef(); }
    STDMETHODIMP_(ULONG) Release() override { return release(); }

    // IWICBitmapSource
    STDMETHODIMP GetSize(UINT* w, UINT* h) override {
        if (!w || !h) return E_INVALIDARG;
        *w = tex_->tex.mipWidth(0);
        *h = tex_->tex.mipHeight(0);
        return S_OK;
    }
    STDMETHODIMP GetPixelFormat(WICPixelFormatGUID* fmt) override {
        if (!fmt) return E_INVALIDARG;
        *fmt = GUID_WICPixelFormat32bppBGRA;
        return S_OK;
    }
    STDMETHODIMP GetResolution(double* x, double* y) override {
        if (!x || !y) return E_INVALIDARG;
        *x = *y = 96.0;
        return S_OK;
    }
    STDMETHODIMP CopyPalette(IWICPalette*) override { return WINCODEC_ERR_PALETTEUNAVAILABLE; }
    STDMETHODIMP CopyPixels(const WICRect* prc, UINT stride, UINT size, BYTE* buffer) override {
        if (!buffer) return E_INVALIDARG;
        const edds::Image* img = tex_->mip(0);
        return img ? copyRect(*img, prc, stride, size, buffer) : WINCODEC_ERR_BADIMAGE;
    }

    // IWICBitmapFrameDecode
    STDMETHODIMP GetMetadataQueryReader(IWICMetadataQueryReader**) override {
        return WINCODEC_ERR_UNSUPPORTEDOPERATION;
    }
    STDMETHODIMP GetColorContexts(UINT, IWICColorContext**, UINT* actual) override {
        if (!actual) return E_INVALIDARG;
        *actual = 0;
        return S_OK;
    }
    STDMETHODIMP GetThumbnail(IWICBitmapSource**) override { return WINCODEC_ERR_CODECNOTHUMBNAIL; }

    // IWICBitmapSourceTransform: lets thumbnailers read a small mipmap instead of the full texture.
    STDMETHODIMP CopyPixels(const WICRect* prc, UINT w, UINT h, WICPixelFormatGUID* fmt,
                            WICBitmapTransformOptions transform, UINT stride, UINT size, BYTE* buffer) override {
        if (!buffer) return E_INVALIDARG;
        if (transform != WICBitmapTransformRotate0) return WINCODEC_ERR_UNSUPPORTEDOPERATION;
        if (fmt && *fmt != GUID_WICPixelFormat32bppBGRA) return WINCODEC_ERR_UNSUPPORTEDPIXELFORMAT;
        for (size_t i = 0; i < tex_->tex.mipCount(); ++i) {
            if (tex_->tex.mipWidth(i) == w && tex_->tex.mipHeight(i) == h) {
                const edds::Image* img = tex_->mip(i);
                return img ? copyRect(*img, prc, stride, size, buffer) : WINCODEC_ERR_BADIMAGE;
            }
        }
        return WINCODEC_ERR_UNSUPPORTEDOPERATION;
    }
    STDMETHODIMP GetClosestSize(UINT* w, UINT* h) override {
        if (!w || !h) return E_INVALIDARG;
        const size_t best = tex_->tex.pickMipmap(*w, *h);
        *w = tex_->tex.mipWidth(best);
        *h = tex_->tex.mipHeight(best);
        return S_OK;
    }
    STDMETHODIMP GetClosestPixelFormat(WICPixelFormatGUID* fmt) override {
        if (!fmt) return E_INVALIDARG;
        *fmt = GUID_WICPixelFormat32bppBGRA;
        return S_OK;
    }
    STDMETHODIMP DoesSupportTransform(WICBitmapTransformOptions transform, BOOL* supported) override {
        if (!supported) return E_INVALIDARG;
        *supported = transform == WICBitmapTransformRotate0;
        return S_OK;
    }

private:
    Texture* tex_;
};

class Decoder : public ComBase<Decoder>, public IWICBitmapDecoder {
public:
    ~Decoder() override {
        if (tex_) tex_->release();
    }

    // IUnknown
    STDMETHODIMP QueryInterface(REFIID iid, void** out) override {
        if (!out) return E_POINTER;
        if (iid == IID_IUnknown || iid == IID_IWICBitmapDecoder) {
            *out = static_cast<IWICBitmapDecoder*>(this);
            addRef();
            return S_OK;
        }
        *out = nullptr;
        return E_NOINTERFACE;
    }
    STDMETHODIMP_(ULONG) AddRef() override { return addRef(); }
    STDMETHODIMP_(ULONG) Release() override { return release(); }

    // IWICBitmapDecoder
    STDMETHODIMP QueryCapability(IStream* stream, DWORD* caps) override {
        if (!caps) return E_INVALIDARG;
        *caps = 0;
        std::vector<uint8_t> data;
        HRESULT hr = readStream(stream, data);
        if (FAILED(hr)) return hr;
        edds::Texture tex;
        if (hasBlockTable(data) && tex.load(data.data(), data.size()))
            *caps = WICBitmapDecoderCapabilityCanDecodeAllImages | WICBitmapDecoderCapabilityCanDecodeSomeImages;
        return S_OK;
    }
    STDMETHODIMP Initialize(IStream* stream, WICDecodeOptions) override {
        if (tex_) return WINCODEC_ERR_WRONGSTATE;
        std::vector<uint8_t> data;
        HRESULT hr = readStream(stream, data);
        if (FAILED(hr)) return hr;
        Texture* tex = new (std::nothrow) Texture;
        if (!tex) return E_OUTOFMEMORY;
        if (!tex->tex.load(data.data(), data.size())) {
            tex->release();
            return WINCODEC_ERR_BADHEADER;
        }
        tex_ = tex;
        return S_OK;
    }
    STDMETHODIMP GetContainerFormat(GUID* fmt) override {
        if (!fmt) return E_INVALIDARG;
        *fmt = GUID_ContainerFormatEdds;
        return S_OK;
    }
    STDMETHODIMP GetDecoderInfo(IWICBitmapDecoderInfo** info) override {
        if (!info) return E_INVALIDARG;
        *info = nullptr;
        IWICImagingFactory* factory = nullptr;
        HRESULT hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory));
        if (FAILED(hr)) return hr;
        IWICComponentInfo* component = nullptr;
        hr = factory->CreateComponentInfo(CLSID_EddsDecoder, &component);
        if (SUCCEEDED(hr)) {
            hr = component->QueryInterface(IID_PPV_ARGS(info));
            component->Release();
        }
        factory->Release();
        return hr;
    }
    STDMETHODIMP CopyPalette(IWICPalette*) override { return WINCODEC_ERR_PALETTEUNAVAILABLE; }
    STDMETHODIMP GetMetadataQueryReader(IWICMetadataQueryReader**) override {
        return WINCODEC_ERR_UNSUPPORTEDOPERATION;
    }
    STDMETHODIMP GetPreview(IWICBitmapSource**) override { return WINCODEC_ERR_UNSUPPORTEDOPERATION; }
    STDMETHODIMP GetColorContexts(UINT, IWICColorContext**, UINT* actual) override {
        if (!actual) return E_INVALIDARG;
        *actual = 0;
        return S_OK;
    }
    STDMETHODIMP GetThumbnail(IWICBitmapSource**) override { return WINCODEC_ERR_CODECNOTHUMBNAIL; }
    STDMETHODIMP GetFrameCount(UINT* count) override {
        if (!count) return E_INVALIDARG;
        *count = tex_ ? 1 : 0;
        return tex_ ? S_OK : WINCODEC_ERR_NOTINITIALIZED;
    }
    STDMETHODIMP GetFrame(UINT index, IWICBitmapFrameDecode** frame) override {
        if (!frame) return E_INVALIDARG;
        *frame = nullptr;
        if (!tex_) return WINCODEC_ERR_NOTINITIALIZED;
        if (index != 0) return WINCODEC_ERR_FRAMEMISSING;
        Frame* f = new (std::nothrow) Frame(tex_);
        if (!f) return E_OUTOFMEMORY;
        *frame = f;
        return S_OK;
    }

private:
    Texture* tex_ = nullptr;
};

class ClassFactory : public IClassFactory {
public:
    STDMETHODIMP QueryInterface(REFIID iid, void** out) override {
        if (!out) return E_POINTER;
        if (iid == IID_IUnknown || iid == IID_IClassFactory) {
            *out = static_cast<IClassFactory*>(this);
            return S_OK;
        }
        *out = nullptr;
        return E_NOINTERFACE;
    }
    STDMETHODIMP_(ULONG) AddRef() override { return 2; }  // static instance
    STDMETHODIMP_(ULONG) Release() override { return 1; }
    STDMETHODIMP CreateInstance(IUnknown* outer, REFIID iid, void** out) override {
        if (!out) return E_POINTER;
        *out = nullptr;
        if (outer) return CLASS_E_NOAGGREGATION;
        Decoder* d = new (std::nothrow) Decoder;
        if (!d) return E_OUTOFMEMORY;
        HRESULT hr = d->QueryInterface(iid, out);
        d->Release();
        return hr;
    }
    STDMETHODIMP LockServer(BOOL lock) override {
        if (lock) ++g_locks;
        else --g_locks;
        return S_OK;
    }
};

ClassFactory g_factory;

// ---- Registration -------------------------------------------------------------------------

std::wstring guidString(const GUID& g) {
    wchar_t buf[40];
    StringFromGUID2(g, buf, 40);
    return buf;
}

bool setValue(const std::wstring& path, const wchar_t* name, DWORD type, const void* data, DWORD size) {
    HKEY key;
    if (RegCreateKeyExW(HKEY_LOCAL_MACHINE, path.c_str(), 0, nullptr, 0, KEY_WRITE, nullptr, &key, nullptr) !=
        ERROR_SUCCESS)
        return false;
    LONG r = RegSetValueExW(key, name, 0, type, static_cast<const BYTE*>(data), size);
    RegCloseKey(key);
    return r == ERROR_SUCCESS;
}

bool setString(const std::wstring& path, const wchar_t* name, const std::wstring& value) {
    return setValue(path, name, REG_SZ, value.c_str(), DWORD((value.size() + 1) * sizeof(wchar_t)));
}

bool setDword(const std::wstring& path, const wchar_t* name, DWORD value) {
    return setValue(path, name, REG_DWORD, &value, sizeof(value));
}

const wchar_t kClasses[] = L"SOFTWARE\\Classes\\";
const wchar_t kKindMap[] = L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Explorer\\KindMap";

HRESULT registerServer() {
    wchar_t path[MAX_PATH];
    DWORD n = GetModuleFileNameW(g_module, path, MAX_PATH);
    if (n == 0 || n == MAX_PATH) return SELFREG_E_CLASS;

    const std::wstring clsid = guidString(CLSID_EddsDecoder);
    const std::wstring cls = std::wstring(kClasses) + L"CLSID\\" + clsid;
    bool ok = true;

    ok &= setString(cls, nullptr, kFriendlyName);
    ok &= setString(cls, L"FriendlyName", kFriendlyName);
    ok &= setString(cls, L"Author", L"edds-viewer");
    ok &= setString(cls, L"Version", L"1.0.0.0");
    ok &= setString(cls, L"SpecVersion", L"1.0.0.0");
    ok &= setString(cls, L"Vendor", guidString(GUID_VendorEddsViewer));
    ok &= setString(cls, L"ContainerFormat", guidString(GUID_ContainerFormatEdds));
    ok &= setString(cls, L"FileExtensions", kExtension);
    ok &= setString(cls, L"MimeTypes", L"image/x-edds");
    ok &= setDword(cls, L"SupportAnimation", 0);
    ok &= setDword(cls, L"SupportChromakey", 0);
    ok &= setDword(cls, L"SupportLossless", 1);
    ok &= setDword(cls, L"SupportMultiframe", 0);
    ok &= setDword(cls, L"ArbitrationPriority", 10);
    ok &= setString(cls + L"\\InprocServer32", nullptr, path);
    ok &= setString(cls + L"\\InprocServer32", L"ThreadingModel", L"Both");
    ok &= setString(cls + L"\\Formats\\" + guidString(GUID_WICPixelFormat32bppBGRA), nullptr, L"");

    // Content sniffing: "DDS " plus a COPY or LZ4 block entry right after the header (or the
    // DX10 header), so plain .dds files never match.
    int index = 0;
    for (bool dx10 : {false, true}) {
        for (const char* block : {"COPY", "LZ4 "}) {
            const size_t at = dx10 ? 148 : 128;
            std::vector<BYTE> pattern(at + 4, 0), mask(at + 4, 0);
            std::memcpy(&pattern[0], "DDS ", 4);
            std::memcpy(&pattern[at], block, 4);
            for (size_t i = 0; i < 4; ++i) mask[i] = mask[at + i] = 0xFF;
            if (dx10) {
                std::memcpy(&pattern[84], "DX10", 4);
                for (size_t i = 84; i < 88; ++i) mask[i] = 0xFF;
            }
            const std::wstring pat = cls + L"\\Patterns\\" + std::to_wstring(index++);
            ok &= setDword(pat, L"Position", 0);
            ok &= setDword(pat, L"Length", DWORD(pattern.size()));
            ok &= setValue(pat, L"Pattern", REG_BINARY, pattern.data(), DWORD(pattern.size()));
            ok &= setValue(pat, L"Mask", REG_BINARY, mask.data(), DWORD(mask.size()));
        }
    }

    // WIC decoder category.
    const std::wstring inst =
        std::wstring(kClasses) + L"CLSID\\" + guidString(CATID_WICBitmapDecoders) + L"\\Instance\\" + clsid;
    ok &= setString(inst, L"CLSID", clsid);
    ok &= setString(inst, L"FriendlyName", kFriendlyName);

    // Shell: picture kind plus the system photo thumbnail and preview handlers, which use WIC.
    const std::wstring sfa = std::wstring(kClasses) + L"SystemFileAssociations\\" + kExtension;
    ok &= setString(sfa + L"\\" + kThumbnailHandlerKey, nullptr, kPhotoThumbnailProvider);
    ok &= setString(sfa + L"\\" + kPreviewHandlerKey, nullptr, kPhotoPreviewHandler);
    ok &= setString(sfa, L"PerceivedType", L"image");
    ok &= setString(kKindMap, kExtension, L"picture");

    SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, nullptr, nullptr);
    return ok ? S_OK : SELFREG_E_CLASS;
}

HRESULT unregisterServer() {
    const std::wstring clsid = guidString(CLSID_EddsDecoder);
    RegDeleteTreeW(HKEY_LOCAL_MACHINE, (std::wstring(kClasses) + L"CLSID\\" + clsid).c_str());
    RegDeleteTreeW(HKEY_LOCAL_MACHINE, (std::wstring(kClasses) + L"CLSID\\" + guidString(CATID_WICBitmapDecoders) +
                                        L"\\Instance\\" + clsid).c_str());
    const std::wstring sfa = std::wstring(kClasses) + L"SystemFileAssociations\\" + kExtension;
    RegDeleteTreeW(HKEY_LOCAL_MACHINE, (sfa + L"\\" + kThumbnailHandlerKey).c_str());
    RegDeleteTreeW(HKEY_LOCAL_MACHINE, (sfa + L"\\" + kPreviewHandlerKey).c_str());
    RegDeleteKeyValueW(HKEY_LOCAL_MACHINE, sfa.c_str(), L"PerceivedType");
    RegDeleteKeyValueW(HKEY_LOCAL_MACHINE, kKindMap, kExtension);
    SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, nullptr, nullptr);
    return S_OK;
}

}  // namespace

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        g_module = instance;
        DisableThreadLibraryCalls(instance);
    }
    return TRUE;
}

STDAPI DllGetClassObject(REFCLSID clsid, REFIID iid, void** out) {
    if (!out) return E_POINTER;
    *out = nullptr;
    if (clsid != CLSID_EddsDecoder) return CLASS_E_CLASSNOTAVAILABLE;
    return g_factory.QueryInterface(iid, out);
}

STDAPI DllCanUnloadNow() { return g_objects == 0 && g_locks == 0 ? S_OK : S_FALSE; }

STDAPI DllRegisterServer() { return registerServer(); }

STDAPI DllUnregisterServer() { return unregisterServer(); }
