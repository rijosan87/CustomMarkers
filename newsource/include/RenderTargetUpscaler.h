#pragma once

// RenderTargetUpscaler — bumps the engine's local-map render targets from the
// hardcoded 960x540 to 1920x1080 at runtime so the Inspect overlay / Glance
// radar backdrop (which sample kLOCAL_MAP) stay sharp on high-res displays.
//
// finding: findings/findings-LocalMap-RenderTarget-Resolution.md (strategy b).
// Both kLOCAL_MAP AND kLOCAL_MAP_SWAP must be resized identically — SWAP is the
// ping-pong buffer for the fog blur pass, so mismatched sizes mix resolutions.
namespace RenderTargetUpscaler {
    // Recreate the two local-map D3D11 textures + views at 1920x1080 and update
    // the RenderTargetManager dims table. Call once from kDataLoaded (targets
    // must exist; kPostLoad is too early). Idempotent-safe: bails per-target if
    // already at the target size. Any D3D11 failure aborts cleanly, leaving the
    // vanilla 960x540 targets intact.
    void Apply();

    // Cheap guard against engine target recreation (display-mode change resets
    // the targets to 960x540). Reads the current kLOCAL_MAP width; if it
    // regressed, re-runs Apply(). Call before rendering into the snapshot.
    void EnsureApplied();
}
