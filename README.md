# Custom Markers: Map, Mini Radar, Beacons

Custom Markers is an SKSE plugin for Skyrim Special Edition and Anniversary Edition (supporting SE 1.5.97, AE 1.6.x, and AE 1.7.104) providing a custom map marker system, local and interior map exploration tools, in-world beacons, loot beams, and navigation overlays.

## Features

- **World Map**: Custom marker placement via right-click, marker notes, context menus, custom hover stat cards (distance, boss, quests, services, spouse and companions), legend filtering, and middle-mouse camera rotation.
- **Local & Interior Maps**: Interior marker support, direct door navigation, and dungeon path breadcrumb tracking.
- **World Beacons & Loot Beams**: In-world 3D destination beacons, rarity-colored loot pillars for containers/items, and Look-and-Pin world placement.
- **Navigation Overlay**: HUD glance mini-radar and full-screen dungeon inspect mode with actor and interactive entity tracking.
- **Settings & Persistence**: In-game ImGui settings menu (also exposed through SKSE Menu Framework), master interface scale, cosave persistence, and multi-language support.

---

## Icons

Icons live in `Data/Interface/CustomMarkers/Icons/` and marker images in `Images/`. A `.svg` is used when the `.png` with the same name is missing.

---

## Loot Beam Blacklist

Loot beams can be suppressed for containers, exact references, cells, and locations through plain-text lists in `Data/SKSE/Plugins/CMMBlacklist/`:

- `lootbeam_blacklist.ini`: the base list.
- `<Name>_lootbeam_blacklist.ini`: companion lists for other mods. Files only add entries.

Each line is `PluginName|0xLocalFormID`. Empty lines and `;` or `#` comments are ignored. The entry resolves to one of four record types:

- `CONT`: every placed reference using that container base.
- `REFR`: one exact placed reference.
- `CELL`: one exact cell.
- `LCTN`: a location and all of its child locations.

```ini
; NobleChestDrawers01 (Wardrobe)
Skyrim.esm|0x000C2A05
```

The lists are read once at game start. Missing plugins and unresolved forms are skipped silently.

---

## Building from Source

### Prerequisites

1. **Visual Studio 2022** (v143 toolset) with the **Desktop development with C++** workload.
2. **CMake 3.21** or later.
3. **vcpkg**: Ensure `VCPKG_ROOT` is defined in your system environment variables pointing to your vcpkg installation.

### Step 1: Clone the Repository

```bash
git clone https://github.com/your-repo/CustomMarkers.git
cd CustomMarkers
```

### Step 2: Clone CommonLibSSE-NG

Clone [CommonLibSSE-NG](https://github.com/alandtse/CommonLibSSE-NG) directly into the project root:

```bash
git clone https://github.com/alandtse/CommonLibSSE-NG
```

> **Note:** Alternatively, you can place the checkout at `extern/CommonLibSSE-NG`, or point the `CommonLibSSEPath` environment variable to an existing CommonLibSSE-NG checkout.

### Step 3: Configure and Build

1. Open the project root folder in **Visual Studio 2022** (`File -> Open -> Folder...`).
2. Visual Studio will read `CMakePresets.json` and `vcpkg.json`, automatically restoring package dependencies (polyhook2, imgui, directxtex, lunasvg, nlohmann-json, spdlog, etc.).
3. Choose a build preset:
   - `Release`: SE + AE runtime dispatch binary.
   - `Release SE (1.5.97)`: Dedicated SE build.
   - `Debug`: Debug build with symbol generation.
4. Run **Build All** (`Ctrl + Shift + B`).

### Automatic Deployment (Optional)

You can set an environment variable to copy the built DLL automatically post-build:

- `SKYRIM_FOLDER`: Points to your Skyrim root directory (installs to `<SKYRIM_FOLDER>/Data/SKSE/Plugins`).
- `SKYRIM_MODS_FOLDER`: Points to your mod manager mods folder (installs to `<SKYRIM_MODS_FOLDER>/CustomMarkers/SKSE/Plugins`).

---

## License

This project is licensed under the GNU General Public License v3.0 or later (GPL-3.0-or-later) to comply with CommonLibSSE-NG. See [COPYING](COPYING) for details.
