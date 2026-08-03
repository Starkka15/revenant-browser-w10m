/*
 * GraphicsContextGLHybrid — Revenant hybrid GPU/software 2D painter (task 34/35).
 *
 * A GraphicsContext whose target is a GL texture (a BitmapTextureGL's FBO). It composes an inner
 * GraphicsContextCairo over a CPU "scratch" image surface and forwards every op to it (the proven
 * BifurcatedGraphicsContext pattern, single child). flushToTarget() uploads the scratch into the
 * target texture. In P1 EVERY op — including drawNativeImage — goes to cairo, so output is
 * pixel-identical to the current tile-raster path (the correctness baseline / A-B safety net).
 *
 * P2 will override drawNativeImage to draw the image on the GPU directly into the target FBO,
 * flushing the pending scratch region first to preserve painter's-algorithm z-order. Everything the
 * GPU fast-path does not handle keeps falling back to cairo-into-scratch, so the engine always
 * renders correctly regardless of which ops are accelerated.
 */

#pragma once

#if USE(CAIRO)

#include "GraphicsContext.h"
#include "IntRect.h"
#include "IntSize.h"
#include <cairo.h>
#include <memory>
#include <wtf/RefPtr.h>

namespace WebCore {

class GraphicsContextCairo;
class BitmapTextureGL;
class TextureMapper;

class GraphicsContextGLHybrid final : public GraphicsContext {
public:
    // targetRect: where in `target` the result is uploaded. tm: nullptr in P1 (cairo-only flush
    // doesn't need it); the P2 GPU fast-path requires it.
    GraphicsContextGLHybrid(const IntRect& targetRect, BitmapTextureGL& target, TextureMapper* tm = nullptr);
    virtual ~GraphicsContextGLHybrid();

    bool isValid() const { return m_scratch && m_cairo; }

    // Open/close a GPU compositing session for this tile (P2). No-op if no TextureMapperGL was given
    // (then the class behaves exactly like P1: cairo-into-scratch + replace-upload at flushToTarget).
    // Call beginGPU() AFTER the initial CTM/state setup and BEFORE painting.
    void beginGPU();

    // Finish the tile. GPU session: composite the final scratch into the FBO + endPainting (content is
    // already in the texture). P1 path: replace-upload the scratch into the texture.
    void flushToTarget();

    bool hasPlatformContext() const final;
    GraphicsContextCairo* platformContext() const final;

    void didUpdateState(const GraphicsContextState&, GraphicsContextState::StateChangeFlags) final;

    void setLineCap(LineCap) final;
    void setLineDash(const DashArray&, float) final;
    void setLineJoin(LineJoin) final;
    void setMiterLimit(float) final;

    using GraphicsContext::fillRect;
    void fillRect(const FloatRect&) final;
    void fillRect(const FloatRect&, const Color&) final;
    void fillRect(const FloatRect&, Gradient&) final;
    void fillRect(const FloatRect&, const Color&, CompositeOperator, BlendMode = BlendMode::Normal) final;
    void fillRoundedRectImpl(const FloatRoundedRect&, const Color&) final;
    void fillRectWithRoundedHole(const FloatRect&, const FloatRoundedRect&, const Color&) final;
    void fillPath(const Path&) final;
    void strokeRect(const FloatRect&, float) final;
    void strokePath(const Path&) final;
    void clearRect(const FloatRect&) final;

    void drawGlyphs(const Font&, const GlyphBufferGlyph*, const GlyphBufferAdvance*, unsigned numGlyphs, const FloatPoint&, FontSmoothingMode) final;

    void drawNativeImage(NativeImage&, const FloatSize&, const FloatRect&, const FloatRect&, const ImagePaintingOptions&) final;
    void drawPattern(NativeImage&, const FloatRect& destRect, const FloatRect& srcRect, const AffineTransform&, const FloatPoint& phase, const FloatSize& spacing, const ImagePaintingOptions&) final;

    void drawRect(const FloatRect&, float) final;
    void drawLine(const FloatPoint&, const FloatPoint&) final;
    void drawLinesForText(const FloatPoint&, float thickness, const DashArray&, bool, bool, StrokeStyle) final;
    void drawDotsForDocumentMarker(const FloatRect&, DocumentMarkerLineStyle) final;
    void drawEllipse(const FloatRect&) final;

    void drawFocusRing(const Path&, float, float, const Color&) final;
    void drawFocusRing(const Vector<FloatRect>&, float, float, const Color&) final;

    void save() final;
    void restore() final;

    void translate(float, float) final;
    void rotate(float) final;
    using GraphicsContext::scale;
    void scale(const FloatSize&) final;
    void concatCTM(const AffineTransform&) final;
    void setCTM(const AffineTransform&) final;
    AffineTransform getCTM(GraphicsContext::IncludeDeviceScale) const final;

    void beginTransparencyLayer(float) final;
    void endTransparencyLayer() final;

    void clip(const FloatRect&) final;
    void clipOut(const FloatRect&) final;
    void clipOut(const Path&) final;
    void clipPath(const Path&, WindRule) final;
    IntRect clipBounds() const final;
    void clipToImageBuffer(ImageBuffer&, const FloatRect&) final;

    RenderingMode renderingMode() const final;

    FloatRect roundToDevicePixels(const FloatRect&, GraphicsContext::RoundingMode) final;

private:
    void markScratchDirty() { m_scratchDirty = true; } // called by every draw op that touches the scratch
    void compositeScratch();                  // composite dirty scratch onto the FBO (source-over), clear it
    bool canGPUImage(NativeImage&, const FloatRect& destRect, const FloatRect& srcRect) const; // cheap gate
    bool drawImageGPU(NativeImage&, const FloatRect& destRect, const FloatRect& srcRect, const ImagePaintingOptions&);

    IntSize m_size;
    IntRect m_targetRect;                     // destination region in m_target for flushToTarget()
    RefPtr<cairo_surface_t> m_scratch;        // CPU BGRA scratch the inner cairo paints into
    std::unique_ptr<GraphicsContextCairo> m_cairo;
    BitmapTextureGL& m_target;                // tile texture; its FBO receives flushToTarget()
    TextureMapper* m_textureMapper;           // nullptr in P1; required by the P2 GPU image fast-path
    bool m_gpuSession { false };              // a TextureMapperGL painting session is open on the tile FBO
    bool m_scratchDirty { false };            // cairo has drawn to the scratch since the last composite
    int m_prevFramebuffer { 0 };              // FBO bound before beginGPU; MUST restore after endPainting
                                              // (else the compositor's frame paint targets the tile FBO -> white)
};

} // namespace WebCore

#endif // USE(CAIRO)
