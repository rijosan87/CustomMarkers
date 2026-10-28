#pragma once
#include "MinimapRenderer.h"

// Shared presentation only. SnapshotProjection remains the capture data contract.
namespace MinimapSnapshotTransition {
    struct SnapshotEaseDraw;

    void BeginFrame();  // Present, before visibility gates.
    void Prepare(const MinimapRenderer::SnapshotProjection& snapshot,
                 const MinimapRenderer::NavigationFrame& frame, unsigned views);
    std::shared_ptr<MinimapRenderer::SnapshotImage> CopyImage(ID3D11ShaderResourceView* srv, std::uint64_t generation);

    // Bracket only terrain drawing; the opaque payload survives until FinishFrame.
    SnapshotEaseDraw* BeginDraw(ImDrawList* dl, const MinimapRenderer::SnapshotProjection& snapshot);
    void EndDraw(ImDrawList* dl, SnapshotEaseDraw* draw);
    ID3D11ShaderResourceView* DrawSRV(const MinimapRenderer::SnapshotProjection& snapshot);
    void FinishFrame();  // After DX11 draw-data submission, including early exits.

    // Keep terrain reprojection and overlay glyph projection on identical math.
    bool ProjectWithSnapshot(const MinimapRenderer::SnapshotProjection& snapshot,
                             const RE::NiPoint3& world, const ImVec2& imageMin,
                             const ImVec2& imageSize, ImVec2& out);
}
