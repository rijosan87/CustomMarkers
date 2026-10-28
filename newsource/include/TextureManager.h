#pragma once

#include <d3d11.h>
#include <cstdint>
#include <string>

// Trimmed from PartySheet's TextureManager (F:\Modding\SkyrimDev\SKSE Party
// Sheet\src\TextureManager.cpp). D3D11 SRV cache with WIC + DirectXTex loader.
// Conditional mipmap chain for paths containing "\Icons\". Negative caching
// for missing/failed files so the existence check runs once per path.
namespace TextureManager {
    // Call once after ImGui_ImplDX11_Init succeeds. Stores the device pointer
    // used by all subsequent GetTexture calls.
    void Init(ID3D11Device* device);

    // Releases every cached SRV and nulls the device pointer. Currently not
    // wired into a real shutdown path (the plugin's ImGui renderer leaks at
    // process exit by design) but kept here for symmetry.
    void Shutdown();

    // Loads (and caches) the texture at the given Skyrim Data-relative path.
    // Returns nullptr if the file is missing or loading fails. The negative
    // result is cached so a missing file is checked exactly once.
    ID3D11ShaderResourceView* GetTexture(const std::string& path);

    // Resolves an iconType (MarkerCategory value) to its PNG SRV. Internally:
    // CategoryToFilename(iconType) -> "Data\\Interface\\CustomMapMarkers\\Icons\\<stem>.png".
    ID3D11ShaderResourceView* GetCategoryIcon(std::uint32_t iconType);

    // Loads an icon by filename stem directly, for callers that aren't keyed
    // off a MarkerCategory (e.g. EntityMarkerSource::Category). Same cache +
    // mip path as GetCategoryIcon, just skips CategoryToFilename.
    ID3D11ShaderResourceView* GetIconByName(const std::string& name);

    // Returns source pixel dimensions of a previously loaded SRV (the size
    // at the highest mip level). Used so callers can preserve aspect ratio
    // when fitting an icon to a bounded box. Returns {0,0} if srv is null
    // or wasn't loaded by this manager.
    struct Size { std::uint32_t width; std::uint32_t height; };
    Size GetTextureSize(ID3D11ShaderResourceView* srv);
}
