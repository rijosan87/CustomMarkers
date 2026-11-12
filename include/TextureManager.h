#pragma once

// Loads, caches, and manages DirectX 11 textures and icons.


#include <d3d11.h>
#include <cstdint>
#include <string>

namespace TextureManager {
    void Init(ID3D11Device* device);

    void Shutdown();

    ID3D11ShaderResourceView* GetTexture(const std::string& path);

    ID3D11ShaderResourceView* GetCategoryIcon(std::uint32_t iconType, bool a_local = false);

    ID3D11ShaderResourceView* GetIconByName(const std::string& name);
    ID3D11ShaderResourceView* GetPackIcon(const std::string& a_path);

    // SVG icons, 8..128 px: single-mip raster at exactly px, cached. Otherwise returns base.
    ID3D11ShaderResourceView* GetIconAtPixelSize(ID3D11ShaderResourceView* base, int px);

    struct Size { std::uint32_t width; std::uint32_t height; };
    Size GetTextureSize(ID3D11ShaderResourceView* srv);
}
