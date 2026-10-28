// include/BackgroundManager.h
// Scans Data/Interface/CustomMapMarkers/Backgrounds/ for user-supplied panel
// background images and hands them out by 1-based index.
//
// Ported from Party Sheet's BackgroundManager, cut down to the one type CMM
// needs. Kept as a separate manager rather than folding into TextureManager
// because the job is different: TextureManager loads a KNOWN path on demand,
// this DISCOVERS what the user dropped in a folder at startup.
//
// Naming contract: files must be "ListPanel-<n>.<ext>" (ListPanel-1.png,
// ListPanel-2.jpg, ...). The number drives sort order and is what gets stored
// in the INI, so renaming a file after picking it changes which image shows.
#pragma once

#include <d3d11.h>
#include <string>
#include <vector>

namespace BackgroundManager {

    enum class Type {
        ListPanel,   // The marker list panel (hotkey C)
    };

    // One discovered file plus its loaded texture, for the picker grid.
    struct BackgroundEntry {
        std::string filename;
        ID3D11ShaderResourceView* texture;
    };

    // Scan the Backgrounds folder. Safe to call more than once; only the first
    // call does work. Call after TextureManager::Init (it needs the device).
    void Init();

    // Texture for a 1-based index, or nullptr when the index is out of range
    // or the folder is empty. Index 0 is "none" by convention.
    ID3D11ShaderResourceView* GetBackground(Type type, int index);

    // Discovered filenames, sorted by their numeric suffix.
    const std::vector<std::string>& GetBackgrounds(Type type);

    int GetBackgroundCount(Type type);

    // Filename for a 1-based index; empty string when out of range.
    const std::string& GetBackgroundName(Type type, int index);

    // Filenames + textures, for drawing the picker grid. Entries whose texture
    // fails to load are dropped, so the grid never shows a blank button.
    std::vector<BackgroundEntry> GetBackgroundEntries(Type type);

}  // namespace BackgroundManager
