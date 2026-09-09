// Loads, caches, and manages DirectX 11 textures and icons.

#include "TextureManager.h"
#include "MarkerManager.h"

#include <RE/Skyrim.h>
#include <SKSE/SKSE.h>
#include <DirectXTex.h>
#include <filesystem>
#include <unordered_map>
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
        std::wstring wpath = UTF8ToWide(path);
        if (!std::filesystem::exists(wpath)) {
            std::string altPath = path;
            const std::string kPrimaryPrefix = "Data\\Interface\\CustomMarkers\\";
            const std::string kLegacyPrefix = "Data\\Interface\\CustomMapMarkers\\";
            if (altPath.rfind(kPrimaryPrefix, 0) == 0) {
                altPath.replace(0, kPrimaryPrefix.length(), kLegacyPrefix);
            } else if (altPath.rfind(kLegacyPrefix, 0) == 0) {
                altPath.replace(0, kLegacyPrefix.length(), kPrimaryPrefix);
            }
            std::wstring waltPath = UTF8ToWide(altPath);
            if (std::filesystem::exists(waltPath)) {
                wpath = waltPath;
            } else {
                g_TextureCache[path] = Entry{ nullptr, 0, 0 };
                return nullptr;
            }
        }
        DirectX::ScratchImage image;
        HRESULT hr = DirectX::LoadFromWICFile(
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
        const std::uint32_t srcW = static_cast<std::uint32_t>(image.GetMetadata().width);
        const std::uint32_t srcH = static_cast<std::uint32_t>(image.GetMetadata().height);
        const bool generateMipmaps = path.find("\\Icons\\") != std::string::npos ||
                                     path.find("\\Images\\") != std::string::npos;
        DirectX::ScratchImage mipChain;
        const DirectX::ScratchImage* imageToUse = &image;
        if (generateMipmaps && image.GetImageCount() > 0) {
            hr = DirectX::GenerateMipMaps(
                image.GetImages()[0],
                DirectX::TEX_FILTER_LINEAR,
                0,
                mipChain
            );
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
