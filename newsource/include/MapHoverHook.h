#pragma once

// Vanilla marker-hover suppression for BOTH maps. Was LocalMapHoverHook, renamed
// 2026-07-14: its main detour, the card populator FUN_140982750 (SE 52187 /
// AE 53079), serves the world map AND the local map. The old name implied
// local-only and actively misled.
namespace MapHoverHook {
    void Install();
}
