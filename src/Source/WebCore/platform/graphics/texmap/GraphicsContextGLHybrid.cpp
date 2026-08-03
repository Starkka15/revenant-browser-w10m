/*
 * GraphicsContextGLHybrid — Revenant hybrid GPU/software 2D painter (task 34/35).
 * See GraphicsContextGLHybrid.h for the design. P1: forward everything to an inner cairo context over
 * a scratch surface; flushToTarget() uploads the scratch into the target BitmapTextureGL. Behaviourally
 * identical to the current tile-raster path — the correctness baseline P2's GPU fast-paths build on.
 */

#include "config.h"
#include "GraphicsContextGLHybrid.h"

#if USE(CAIRO)

#include "BitmapTextureGL.h"
#include "GraphicsContextCairo.h"
#include "NativeImage.h"
#include "TextureMapperGL.h"
#include "TransformationMatrix.h"
#include <cstdio>

extern "C" void WebCoreBrowserLog(const char*); // Revenant port logger (WebCoreDriver.cpp)

namespace WebCore {

GraphicsContextGLHybrid::GraphicsContextGLHybrid(const IntRect& targetRect, BitmapTextureGL& target, TextureMapper* tm)
    : m_size(targetRect.size())
    , m_targetRect(targetRect)
    , m_target(target)
    , m_textureMapper(tm)
{
    m_scratch = adoptRef(cairo_image_surface_create(CAIRO_FORMAT_ARGB32, m_size.width(), m_size.height()));
    if (m_scratch && cairo_surface_status(m_scratch.get()) == CAIRO_STATUS_SUCCESS)
        m_cairo = makeUnique<GraphicsContextCairo>(m_scratch.get());
}

GraphicsContextGLHybrid::~GraphicsContextGLHybrid() = default;

void GraphicsContextGLHybrid::beginGPU()
{
    if (!m_textureMapper || !isValid())
        return;
    GLint prevFbo = 0;
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &prevFbo); // remember the swapchain/default FBO to restore later
    m_prevFramebuffer = prevFbo;
    m_target.bindAsSurface();                 // bind the tile FBO as the GL render target
    IntSize ts = m_target.size();
    glViewport(0, 0, ts.width(), ts.height());
    // REPLACE, not blend: clear the dirty region so re-rastered content overwrites stale prior-frame
    // pixels. Without this, source-over accumulated old copies -> ghosting/duplication on progressive
    // load + heavy flicker during video (every frame re-rasters and layered on top). GL scissor is
    // bottom-up, so flip m_targetRect's Y. (For a full-tile dirty rect the flip is a no-op.)
    glEnable(GL_SCISSOR_TEST);
    glScissor(m_targetRect.x(), ts.height() - m_targetRect.y() - m_targetRect.height(), m_targetRect.width(), m_targetRect.height());
    glClearColor(0.f, 0.f, 0.f, 0.f);
    glClear(GL_COLOR_BUFFER_BIT);
    glDisable(GL_SCISSOR_TEST);
    // PaintingMirrored => Default (non-inverted) Y axis, so content renders top-down INTO the tile
    // texture to match P1's glTexSubImage upload (the default Inverted axis rendered images upside-down).
    m_textureMapper->beginPainting(TextureMapper::PaintingMirrored);
    m_gpuSession = true;
}

void GraphicsContextGLHybrid::compositeScratch()
{
    if (!m_scratchDirty || !m_gpuSession)
        return;
    cairo_surface_flush(m_scratch.get());
    const unsigned char* data = cairo_image_surface_get_data(m_scratch.get());
    int stride = cairo_image_surface_get_stride(m_scratch.get());
    if (data) {
        if (auto tmp = m_textureMapper->acquireTextureFromPool(m_size)) {
            tmp->updateContents(data, IntRect(IntPoint(), m_size), IntPoint(), stride);
            // Source-over the software content into the tile FBO at m_targetRect.
            m_textureMapper->drawTexture(*tmp, FloatRect(m_targetRect), TransformationMatrix(), 1.0f);
        }
    }
    // Clear the scratch (independent cairo_t) so later cairo ops resume from transparent.
    if (cairo_t* cr = cairo_create(m_scratch.get())) {
        cairo_set_operator(cr, CAIRO_OPERATOR_CLEAR);
        cairo_paint(cr);
        cairo_destroy(cr);
    }
    m_scratchDirty = false;
}

bool GraphicsContextGLHybrid::canGPUImage(NativeImage& image, const FloatRect&, const FloatRect& srcRect) const
{
    cairo_surface_t* surface = image.platformImage().get();
    if (!surface || cairo_surface_get_type(surface) != CAIRO_SURFACE_TYPE_IMAGE)
        return false;
    int w = cairo_image_surface_get_width(surface), h = cairo_image_surface_get_height(surface);
    // Whole-image only (sub-rect sprites/atlases -> cairo).
    if (srcRect.x() > 0.5f || srcRect.y() > 0.5f || srcRect.width() < w - 0.5f || srcRect.height() < h - 0.5f)
        return false;
    // Worth a GPU session only for sizeable images (thumbnails/photos) where GPU scaling pays off; tiny
    // icons/spacers stay on cairo so we don't open a session per trivial draw.
    if (w < 64 || h < 64)
        return false;
    return true;
}

bool GraphicsContextGLHybrid::drawImageGPU(NativeImage& image, const FloatRect& destRect, const FloatRect& srcRect, const ImagePaintingOptions&)
{
    if (!m_gpuSession)
        return false;
    cairo_surface_t* surface = image.platformImage().get();
    if (!surface || cairo_surface_get_type(surface) != CAIRO_SURFACE_TYPE_IMAGE)
        return false;
    IntSize imgSize(cairo_image_surface_get_width(surface), cairo_image_surface_get_height(surface));
    auto tex = m_textureMapper->acquireTextureFromPool(imgSize);
    if (!tex)
        return false;
    cairo_surface_flush(surface);
    const unsigned char* data = cairo_image_surface_get_data(surface);
    int stride = cairo_image_surface_get_stride(surface);
    if (!data)
        return false;
    tex->updateContents(data, IntRect(IntPoint(), imgSize), IntPoint(), stride);
    // destRect is in drawing coords; the current CTM maps it to tile-local device px. GPU bilinear scales.
    AffineTransform ctm = m_cairo->getCTM(GraphicsContext::PossiblyIncludeDeviceScale);
    static int s_imgDiag = 0;
    if (s_imgDiag < 14) {
        char b[240]; snprintf(b, sizeof b, "imgdiag: img=%dx%d dest=%.0f,%.0f,%.0fx%.0f ctm[a%.2f d%.2f e%.0f f%.0f] tgt=%d,%d,%dx%d tile=%dx%d",
            imgSize.width(), imgSize.height(), destRect.x(), destRect.y(), destRect.width(), destRect.height(),
            ctm.a(), ctm.d(), ctm.e(), ctm.f(),
            m_targetRect.x(), m_targetRect.y(), m_targetRect.width(), m_targetRect.height(),
            m_target.size().width(), m_target.size().height());
        WebCoreBrowserLog(b); ++s_imgDiag;
    }
    // The CTM is dirty-rect-relative (dirty origin -> 0,0), so ctm*destRect is in scratch coords. The
    // scratch composite places that at tile position m_targetRect; the image must get the SAME offset,
    // else on sub-region (scroll) re-rasters the image lands at the tile origin (bottom-left) while the
    // cleared dirty region shows white. Post-translate by m_targetRect.location().
    TransformationMatrix m;
    m.translate(m_targetRect.x(), m_targetRect.y());
    m.multiply(TransformationMatrix(ctm));
    m_textureMapper->drawTexture(*tex, destRect, m, 1.0f);
    return true;
}

void GraphicsContextGLHybrid::flushToTarget()
{
    if (!isValid())
        return;
    if (m_gpuSession) {
        compositeScratch();                   // composite the final software content
        m_textureMapper->endPainting();
        glBindFramebuffer(GL_FRAMEBUFFER, m_prevFramebuffer); // restore swapchain FBO so the frame paint
        // The FBO now holds final RGBA (my drawTexture calls already did BGRA->RGBA). Clear the tile's
        // convert flag so the compositor does NOT convert again (a stale Convert from an earlier P1-branch
        // frame on this same tile is what double-swapped R/B -> blue skin, red X shown blue).
        m_target.setColorConvertFlags(TextureMapperGL::NoFlag);
        m_gpuSession = false;                 // (frame paint targets the screen, not this tile)
        return;                               // content already lives in the tile texture (the FBO)
    }
    // P1 path (no TextureMapperGL): replace-upload the whole scratch into the texture.
    cairo_surface_flush(m_scratch.get());
    const unsigned char* data = cairo_image_surface_get_data(m_scratch.get());
    int stride = cairo_image_surface_get_stride(m_scratch.get());
    if (data)
        m_target.updateContents(data, m_targetRect, IntPoint(), stride);
}

bool GraphicsContextGLHybrid::hasPlatformContext() const { return m_cairo && m_cairo->hasPlatformContext(); }
GraphicsContextCairo* GraphicsContextGLHybrid::platformContext() const { return m_cairo ? m_cairo->platformContext() : nullptr; }

// State: updateState() (not didUpdateState) so the inner context's own GraphicsContextState is updated,
// since GraphicsContextCairo draw ops read state() at draw time.
void GraphicsContextGLHybrid::didUpdateState(const GraphicsContextState& state, GraphicsContextState::StateChangeFlags flags)
{
    if (m_cairo)
        m_cairo->updateState(state, flags);
}

void GraphicsContextGLHybrid::setLineCap(LineCap v) { if (m_cairo) m_cairo->setLineCap(v); }
void GraphicsContextGLHybrid::setLineDash(const DashArray& a, float o) { if (m_cairo) m_cairo->setLineDash(a, o); }
void GraphicsContextGLHybrid::setLineJoin(LineJoin v) { if (m_cairo) m_cairo->setLineJoin(v); }
void GraphicsContextGLHybrid::setMiterLimit(float v) { if (m_cairo) m_cairo->setMiterLimit(v); }

void GraphicsContextGLHybrid::fillRect(const FloatRect& r) { markScratchDirty(); if (m_cairo) m_cairo->fillRect(r); }
void GraphicsContextGLHybrid::fillRect(const FloatRect& r, const Color& c) { markScratchDirty(); if (m_cairo) m_cairo->fillRect(r, c); }
void GraphicsContextGLHybrid::fillRect(const FloatRect& r, Gradient& g) { markScratchDirty(); if (m_cairo) m_cairo->fillRect(r, g); }
void GraphicsContextGLHybrid::fillRect(const FloatRect& r, const Color& c, CompositeOperator op, BlendMode bm) { markScratchDirty(); if (m_cairo) m_cairo->fillRect(r, c, op, bm); }
void GraphicsContextGLHybrid::fillRoundedRectImpl(const FloatRoundedRect& r, const Color& c) { markScratchDirty(); if (m_cairo) m_cairo->fillRoundedRectImpl(r, c); }
void GraphicsContextGLHybrid::fillRectWithRoundedHole(const FloatRect& r, const FloatRoundedRect& hole, const Color& c) { markScratchDirty(); if (m_cairo) m_cairo->fillRectWithRoundedHole(r, hole, c); }
void GraphicsContextGLHybrid::fillPath(const Path& p) { markScratchDirty(); if (m_cairo) m_cairo->fillPath(p); }
void GraphicsContextGLHybrid::strokeRect(const FloatRect& r, float w) { markScratchDirty(); if (m_cairo) m_cairo->strokeRect(r, w); }
void GraphicsContextGLHybrid::strokePath(const Path& p) { markScratchDirty(); if (m_cairo) m_cairo->strokePath(p); }
void GraphicsContextGLHybrid::clearRect(const FloatRect& r) { markScratchDirty(); if (m_cairo) m_cairo->clearRect(r); }

void GraphicsContextGLHybrid::drawGlyphs(const Font& f, const GlyphBufferGlyph* g, const GlyphBufferAdvance* a, unsigned n, const FloatPoint& p, FontSmoothingMode m) { markScratchDirty(); if (m_cairo) m_cairo->drawGlyphs(f, g, a, n, p, m); }

void GraphicsContextGLHybrid::drawNativeImage(NativeImage& img, const FloatSize& sz, const FloatRect& dest, const FloatRect& src, const ImagePaintingOptions& o)
{
    // Only a scalable, whole, reasonably-large image is worth the GPU path. If so, LAZILY open the
    // session (so text/gif/plain tiles never pay the session cost -> no dynamic-content flashing).
    if (m_textureMapper && canGPUImage(img, dest, src)) {
        if (!m_gpuSession)
            beginGPU();
        if (m_gpuSession) {
            compositeScratch();                // flush software content below the image (painter z-order)
            if (drawImageGPU(img, dest, src, o))
                return;
        }
    }
    markScratchDirty();
    if (m_cairo) m_cairo->drawNativeImage(img, sz, dest, src, o);
}
void GraphicsContextGLHybrid::drawPattern(NativeImage& img, const FloatRect& dest, const FloatRect& src, const AffineTransform& t, const FloatPoint& phase, const FloatSize& spacing, const ImagePaintingOptions& o) { markScratchDirty(); if (m_cairo) m_cairo->drawPattern(img, dest, src, t, phase, spacing, o); }

void GraphicsContextGLHybrid::drawRect(const FloatRect& r, float b) { markScratchDirty(); if (m_cairo) m_cairo->drawRect(r, b); }
void GraphicsContextGLHybrid::drawLine(const FloatPoint& a, const FloatPoint& b) { markScratchDirty(); if (m_cairo) m_cairo->drawLine(a, b); }
void GraphicsContextGLHybrid::drawLinesForText(const FloatPoint& p, float th, const DashArray& w, bool pr, bool dl, StrokeStyle s) { markScratchDirty(); if (m_cairo) m_cairo->drawLinesForText(p, th, w, pr, dl, s); }
void GraphicsContextGLHybrid::drawDotsForDocumentMarker(const FloatRect& r, DocumentMarkerLineStyle s) { markScratchDirty(); if (m_cairo) m_cairo->drawDotsForDocumentMarker(r, s); }
void GraphicsContextGLHybrid::drawEllipse(const FloatRect& r) { markScratchDirty(); if (m_cairo) m_cairo->drawEllipse(r); }

void GraphicsContextGLHybrid::drawFocusRing(const Path& p, float w, float o, const Color& c) { markScratchDirty(); if (m_cairo) m_cairo->drawFocusRing(p, w, o, c); }
void GraphicsContextGLHybrid::drawFocusRing(const Vector<FloatRect>& r, float w, float o, const Color& c) { markScratchDirty(); if (m_cairo) m_cairo->drawFocusRing(r, w, o, c); }

// save/restore: keep OUR base state stack in sync (state getters read it) AND forward to the inner context.
void GraphicsContextGLHybrid::save() { GraphicsContext::save(); if (m_cairo) m_cairo->save(); }
void GraphicsContextGLHybrid::restore() { GraphicsContext::restore(); if (m_cairo) m_cairo->restore(); }

void GraphicsContextGLHybrid::translate(float x, float y) { if (m_cairo) m_cairo->translate(x, y); }
void GraphicsContextGLHybrid::rotate(float a) { if (m_cairo) m_cairo->rotate(a); }
void GraphicsContextGLHybrid::scale(const FloatSize& s) { if (m_cairo) m_cairo->scale(s); }
void GraphicsContextGLHybrid::concatCTM(const AffineTransform& t) { if (m_cairo) m_cairo->concatCTM(t); }
void GraphicsContextGLHybrid::setCTM(const AffineTransform& t) { if (m_cairo) m_cairo->setCTM(t); }
AffineTransform GraphicsContextGLHybrid::getCTM(GraphicsContext::IncludeDeviceScale s) const { return m_cairo ? m_cairo->getCTM(s) : AffineTransform(); }

void GraphicsContextGLHybrid::beginTransparencyLayer(float o) { GraphicsContext::beginTransparencyLayer(o); if (m_cairo) m_cairo->beginTransparencyLayer(o); }
void GraphicsContextGLHybrid::endTransparencyLayer() { GraphicsContext::endTransparencyLayer(); if (m_cairo) m_cairo->endTransparencyLayer(); }

void GraphicsContextGLHybrid::clip(const FloatRect& r) { if (m_cairo) m_cairo->clip(r); }
void GraphicsContextGLHybrid::clipOut(const FloatRect& r) { if (m_cairo) m_cairo->clipOut(r); }
void GraphicsContextGLHybrid::clipOut(const Path& p) { if (m_cairo) m_cairo->clipOut(p); }
void GraphicsContextGLHybrid::clipPath(const Path& p, WindRule w) { if (m_cairo) m_cairo->clipPath(p, w); }
IntRect GraphicsContextGLHybrid::clipBounds() const { return m_cairo ? m_cairo->clipBounds() : IntRect(); }
void GraphicsContextGLHybrid::clipToImageBuffer(ImageBuffer& b, const FloatRect& r) { if (m_cairo) m_cairo->clipToImageBuffer(b, r); }

RenderingMode GraphicsContextGLHybrid::renderingMode() const { return m_cairo ? m_cairo->renderingMode() : RenderingMode::Unaccelerated; }

FloatRect GraphicsContextGLHybrid::roundToDevicePixels(const FloatRect& r, GraphicsContext::RoundingMode m) { return m_cairo ? m_cairo->roundToDevicePixels(r, m) : r; }

} // namespace WebCore

#endif // USE(CAIRO)
