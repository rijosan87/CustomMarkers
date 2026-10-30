// Loads, caches, and manages DirectX 11 textures and icons.
// A missing .png under \Icons\ or \Images\ falls back to the same stem as .svg.

#include "TextureManager.h"
#include "MarkerManager.h"

#include <RE/Skyrim.h>
#include <SKSE/SKSE.h>
#include <DirectXTex.h>
#include <lunasvg.h>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <unordered_map>
#include <vector>
#include <Windows.h>

namespace logger = SKSE::log;

namespace TextureManager {
    struct Entry {
        ID3D11ShaderResourceView* srv;
        std::uint32_t             width;
        std::uint32_t             height;
    };

    static ID3D11Device* g_Device = nullptr;
    static std::unordered_map<std::string, Entry> g_TextureCache;
    static std::unordered_map<ID3D11ShaderResourceView*, Size> g_SizeBySrv;

    static constexpr const char* ICONS_PATH = "Data\\Interface\\CustomMarkers\\Icons\\";

    static const char* GetIconsPath() {
        static const std::string kPrimary = "Data\\Interface\\CustomMarkers\\Icons\\";
        static const std::string kLegacy = "Data\\Interface\\CustomMapMarkers\\Icons\\";
        if (std::filesystem::exists(kPrimary)) {
            return kPrimary.c_str();
        }
        if (std::filesystem::exists(kLegacy)) {
            return kLegacy.c_str();
        }
        return kPrimary.c_str();
    }

    static std::wstring UTF8ToWide(const std::string& utf8) {
        if (utf8.empty()) return L"";
        int wideLen = MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), -1, nullptr, 0);
        if (wideLen <= 0) return L"";
        std::wstring wide(wideLen - 1, L'\0');
        MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), -1, &wide[0], wideLen);
        return wide;
    }

    // Same 256 px square as the shipped icon PNGs.
    static constexpr int   kSvgIconPx    = 256;
    // Art fills 232/256; the margin also stops WRAP sampling bleeding edges in at low mips.
    static constexpr float kSvgIconFit   = 232.0f / 256.0f;
    static constexpr int   kSvgImageLong = 1024;

    enum class SvgKind { kNone, kIcon, kImage };

    static bool EndsWithNoCase(const std::string& s, const char* suffix) {
        const std::size_t n = std::strlen(suffix);
        return s.size() >= n && _stricmp(s.c_str() + s.size() - n, suffix) == 0;
    }

    static SvgKind SvgKindFor(const std::string& pngPath) {
        if (!EndsWithNoCase(pngPath, ".png")) return SvgKind::kNone;
        // cursor.png hotspot is its top-left texel; a margined canvas would move it.
        if (EndsWithNoCase(pngPath, "\\cursor.png")) return SvgKind::kNone;
        if (pngPath.find("\\Icons\\") != std::string::npos) return SvgKind::kIcon;
        if (pngPath.find("\\Images\\") != std::string::npos) return SvgKind::kImage;
        return SvgKind::kNone;
    }

    static std::string SwapExt(const std::string& p, const char* ext) {
        return p.substr(0, p.size() - 4) + ext;
    }

    static HRESULT RasterizeSVGFile(const std::wstring& wpath, SvgKind kind, DirectX::ScratchImage& out) {
        std::ifstream file(wpath, std::ios::binary);
        if (!file) return E_FAIL;
        const std::string bytes((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
        const auto doc = lunasvg::Document::loadFromData(bytes.data(), bytes.size());
        if (!doc || doc->width() <= 0.0f || doc->height() <= 0.0f) return E_FAIL;

        const float docW = doc->width();
        const float docH = doc->height();
        int w = 0;
        int h = 0;
        float scale = 1.0f;
        float tx = 0.0f;
        float ty = 0.0f;
        if (kind == SvgKind::kIcon) {
            // Fit the art's bounding box, not the viewBox: packs pad the viewBox by varying amounts.
            const auto bb = doc->boundingBox();
            const bool useArt = bb.w > 0.0f && bb.h > 0.0f;
            const float boxX = useArt ? bb.x : 0.0f;
            const float boxY = useArt ? bb.y : 0.0f;
            const float boxW = useArt ? bb.w : docW;
            const float boxH = useArt ? bb.h : docH;
            w = h = kSvgIconPx;
            scale = kSvgIconPx * kSvgIconFit / (std::max)(boxW, boxH);
            tx = (w - boxW * scale) / 2 - boxX * scale;
            ty = (h - boxH * scale) / 2 - boxY * scale;
            logger::debug("TextureManager: SVG fit doc={:.1f}x{:.1f} art=({:.1f},{:.1f} {:.1f}x{:.1f}){}",
                docW, docH, bb.x, bb.y, bb.w, bb.h, useArt ? "" : " empty, viewBox fallback");
        } else {
            scale = kSvgImageLong / (std::max)(docW, docH);
            w = (std::max)(1, static_cast<int>(std::lround(docW * scale)));
            h = (std::max)(1, static_cast<int>(std::lround(docH * scale)));
            tx = (w - docW * scale) / 2;
            ty = (h - docH * scale) / 2;
        }
        lunasvg::Bitmap bitmap(w, h);
        if (bitmap.isNull()) return E_FAIL;
        bitmap.clear(0x00000000);
        doc->render(bitmap, lunasvg::Matrix(scale, 0, 0, scale, tx, ty));
        // lunasvg renders premultiplied ARGB32; the PNG path and ImGui expect straight RGBA.
        bitmap.convertToRGBA();
        if (FAILED(out.Initialize2D(DXGI_FORMAT_R8G8B8A8_UNORM, w, h, 1, 1))) return E_FAIL;
        const auto* img = out.GetImage(0, 0, 0);
        for (int y = 0; y < h; ++y) {
            std::memcpy(img->pixels + y * img->rowPitch, bitmap.data() + y * bitmap.stride(),
                        static_cast<std::size_t>(w) * 4);
        }
        return S_OK;
    }

    void Init(ID3D11Device* device) {
        g_Device = device;
        logger::info("TextureManager initialized");
    }

    void Shutdown() {
        for (auto& [path, entry] : g_TextureCache) {
            if (entry.srv) entry.srv->Release();
        }
        g_TextureCache.clear();
        g_SizeBySrv.clear();
        g_Device = nullptr;
        logger::info("TextureManager shutdown");
    }

    ID3D11ShaderResourceView* GetTexture(const std::string& path) {
        if (!g_Device) return nullptr;
        auto it = g_TextureCache.find(path);
        if (it != g_TextureCache.end()) {
            return it->second.srv;
        }
        std::string altPath = path;
        const std::string kPrimaryPrefix = "Data\\Interface\\CustomMarkers\\";
        const std::string kLegacyPrefix = "Data\\Interface\\CustomMapMarkers\\";
        if (altPath.rfind(kPrimaryPrefix, 0) == 0) {
            altPath.replace(0, kPrimaryPrefix.length(), kLegacyPrefix);
        } else if (altPath.rfind(kLegacyPrefix, 0) == 0) {
            altPath.replace(0, kLegacyPrefix.length(), kPrimaryPrefix);
        }

        // Folder first, extension second: the PNG wins in its folder, primary wins over legacy.
        const SvgKind svgKind = SvgKindFor(path);
        std::vector<std::string> candidates{ path };
        if (svgKind != SvgKind::kNone) candidates.push_back(SwapExt(path, ".svg"));
        if (altPath != path) {
            candidates.push_back(altPath);
            if (svgKind != SvgKind::kNone) candidates.push_back(SwapExt(altPath, ".svg"));
        }

        std::wstring wpath;
        bool isSvg = false;
        for (const auto& c : candidates) {
            std::wstring wc = UTF8ToWide(c);
            if (std::filesystem::exists(wc)) {
                wpath = std::move(wc);
                isSvg = EndsWithNoCase(c, ".svg");
                break;
            }
        }
        if (wpath.empty()) {
            g_TextureCache[path] = Entry{ nullptr, 0, 0 };
            return nullptr;
        }

        // The first existing candidate is final; a failed decode caches the miss.
        DirectX::ScratchImage image;
        HRESULT hr = S_OK;
        if (isSvg) {
            const auto t0 = std::chrono::steady_clock::now();
            hr = RasterizeSVGFile(wpath, svgKind, image);
            const auto us = std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::steady_clock::now() - t0).count();
            if (FAILED(hr)) {
                logger::warn("TextureManager: lunasvg failed for '{}' (svg fallback)", path);
                g_TextureCache[path] = Entry{ nullptr, 0, 0 };
                return nullptr;
            }
            logger::debug("TextureManager: SVG '{}' rastered {}x{} in {} us", path,
                image.GetMetadata().width, image.GetMetadata().height, us);
        } else {
            hr = DirectX::LoadFromWICFile(
                wpath.c_str(),
                DirectX::WIC_FLAGS_IGNORE_SRGB,
                nullptr,
                image
            );
            if (FAILED(hr)) {
                logger::warn("TextureManager: WIC load failed for '{}' hr=0x{:08X}",
                    path, static_cast<uint32_t>(hr));
                g_TextureCache[path] = Entry{ nullptr, 0, 0 };
                return nullptr;
            }
        }
        const std::uint32_t srcW = static_cast<std::uint32_t>(image.GetMetadata().width);
        const std::uint32_t srcH = static_cast<std::uint32_t>(image.GetMetadata().height);
        const bool generateMipmaps = path.find("\\Icons\\") != std::string::npos ||
                                     path.find("\\Images\\") != std::string::npos;
        DirectX::ScratchImage mipChain;
        const DirectX::ScratchImage* imageToUse = &image;
        if (generateMipmaps && image.GetImageCount() > 0) {
            // Build mips in premultiplied alpha, then reverse to straight: a straight-alpha
            // filter bleeds the black of transparent texels into edges at small sizes.
            const DirectX::Image& base = image.GetImages()[0];
            const bool hasAlpha = DirectX::HasAlpha(base.format);
            DirectX::ScratchImage premul;
            DirectX::ScratchImage mipsPM;
            const DirectX::Image* mipSource = &base;
            if (hasAlpha) {
                hr = DirectX::PremultiplyAlpha(base, DirectX::TEX_PMALPHA_IGNORE_SRGB, premul);
                if (SUCCEEDED(hr)) {
                    mipSource = premul.GetImages();
                } else {
                    logger::warn("TextureManager: PremultiplyAlpha failed for '{}' hr=0x{:08X} - straight mips",
                        path, static_cast<uint32_t>(hr));
                }
            }
            const bool premultiplied = hasAlpha && mipSource != &base;
            hr = DirectX::GenerateMipMaps(
                *mipSource,
                DirectX::TEX_FILTER_LINEAR,
                0,
                premultiplied ? mipsPM : mipChain
            );
            if (SUCCEEDED(hr) && premultiplied) {
                // The reverse refuses metadata not marked premultiplied.
                auto md = mipsPM.GetMetadata();
                md.SetAlphaMode(DirectX::TEX_ALPHA_MODE_PREMULTIPLIED);
                hr = DirectX::PremultiplyAlpha(mipsPM.GetImages(), mipsPM.GetImageCount(), md,
                    DirectX::TEX_PMALPHA_REVERSE | DirectX::TEX_PMALPHA_IGNORE_SRGB, mipChain);
            }
            if (SUCCEEDED(hr)) {
                imageToUse = &mipChain;
            } else {
                logger::warn("TextureManager: GenerateMipMaps failed for '{}' hr=0x{:08X} - single-level fallback",
                    path, static_cast<uint32_t>(hr));
            }
        }
        ID3D11Resource* texture = nullptr;
        hr = DirectX::CreateTexture(
            g_Device,
            imageToUse->GetImages(),
            imageToUse->GetImageCount(),
            imageToUse->GetMetadata(),
            &texture
        );
        if (FAILED(hr)) {
            logger::warn("TextureManager: CreateTexture failed for '{}' hr=0x{:08X}",
                path, static_cast<uint32_t>(hr));
            g_TextureCache[path] = Entry{ nullptr, 0, 0 };
            return nullptr;
        }
        ID3D11ShaderResourceView* srv = nullptr;
        D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
        srvDesc.Format = imageToUse->GetMetadata().format;
        srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
        srvDesc.Texture2D.MipLevels = static_cast<UINT>(imageToUse->GetMetadata().mipLevels);
        srvDesc.Texture2D.MostDetailedMip = 0;
        hr = g_Device->CreateShaderResourceView(texture, &srvDesc, &srv);
        texture->Release();
        if (FAILED(hr)) {
            logger::warn("TextureManager: CreateSRV failed for '{}' hr=0x{:08X}",
                path, static_cast<uint32_t>(hr));
            g_TextureCache[path] = Entry{ nullptr, 0, 0 };
            return nullptr;
        }
        g_TextureCache[path] = Entry{ srv, srcW, srcH };
        g_SizeBySrv[srv] = Size{ srcW, srcH };
        return srv;
    }

    ID3D11ShaderResourceView* GetCategoryIcon(std::uint32_t iconType) {
        std::string path = std::string(GetIconsPath()) + CategoryToFilename(iconType) + ".png";
        return GetTexture(path);
    }

    ID3D11ShaderResourceView* GetIconByName(const std::string& name) {
        std::string path = std::string(GetIconsPath()) + name + ".png";
        return GetTexture(path);
    }

    Size GetTextureSize(ID3D11ShaderResourceView* srv) {
        if (!srv) return Size{ 0, 0 };
        auto it = g_SizeBySrv.find(srv);
        if (it == g_SizeBySrv.end()) return Size{ 0, 0 };
        return it->second;
    }
}
