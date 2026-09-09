#pragma once

// Loads, caches, and manages DirectX 11 textures and icons.


#include <d3d11.h>
#include <cstdint>
#include <string>

namespace TextureManager {
    void Init(ID3D11Device* device);

    void Shutdown();

    ID3D11ShaderResourceView* GetTexture(const std::string& path);

    ID3D11ShaderResourceView* GetCategoryIcon(std::uint32_t iconType);

    ID3D11ShaderResourceView* GetIconByName(const std::string& name);

    struct Size { std::uint32_t width; std::uint32_t height; };
    Size GetTextureSize(ID3D11ShaderResourceView* srv);
}
