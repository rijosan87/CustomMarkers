#pragma once
#include "MinimapRenderer.h"

namespace MapDraw { struct RoundedRectBoundary; }

namespace MinimapEntityTransition {
    struct VisualEntity {
        EntityMarkerSource::EntityMarker marker;
        float alpha;
    };

    // Present-only, called once whenever Mini Radar draws. No collection or discovery.
    const std::vector<VisualEntity>& Update(const MinimapRenderer::SnapshotProjection& snapshot,
                                          const MinimapRenderer::NavigationFrame& frame);
    float EdgeAlpha(const MapDraw::RoundedRectBoundary& boundary, const ImVec2& point,
                    float extent, float fadeWidth);
}
