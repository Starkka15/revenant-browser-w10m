// ============================================================================
// PortChromeClient.h — ChromeClient for the W10M render driver. Vendor-copied
// from EmptyChromeClient (its compositing hooks are `final`, can't be subclassed).
// Captures the root GraphicsLayer when accelerated compositing turns on so the
// driver can composite the layer tree via TextureMapperGL on the Adreno GPU.
// ============================================================================
#pragma once
#include "ChromeClient.h"
#include <cstdio>
#include <wtf/text/CString.h>

// Defined in WebCoreDriver.cpp. Runs the HTML "update the rendering" step (updateRendering +
// finalizeRenderingUpdate + layout + compositing flush). Called from triggerRenderingUpdate()
// so WebCore's RenderingUpdateScheduler owns the cadence.
extern "C" void WebCoreDriverRunRenderingUpdate();

namespace WebCore {

class PortChromeClient : public ChromeClient {
    WTF_MAKE_FAST_ALLOCATED;
public:
    // Root of the composited layer tree (captured when accelerated compositing
    // turns on); the driver renders it via TextureMapperGL. Null = not composited.
    GraphicsLayer* rootGraphicsLayer() const { return m_rootGraphicsLayer; }
    bool needsCompositingFlush() const { return m_needsCompositingFlush; }
    void clearCompositingFlush() { m_needsCompositingFlush = false; }
    // Accumulated invalidation region (root-view / CSS px). Lets the driver re-raster only
    // the changed area (setNeedsDisplayInRect) instead of the whole page every frame —
    // a big win on image-heavy pages and animations. takeDirtyRegion() returns + clears it.
    bool hasDirtyRegion() const { return !m_dirtyRegion.isEmpty(); }
    IntRect takeDirtyRegion() { IntRect r = m_dirtyRegion; m_dirtyRegion = IntRect(); return r; }
    // Real CSS-px viewport, pushed from the port on init/resize (shared across clients).
    static void setViewportSize(const IntSize& s) { s_viewportSize = s; }
private:
    GraphicsLayer* m_rootGraphicsLayer { nullptr };
    bool m_needsCompositingFlush { false };
    IntRect m_dirtyRegion; // bounding box of invalidated rects since last take
    static IntSize s_viewportSize; // CSS-px viewport, set by the port (shared across clients)

    void chromeDestroyed() override { }

    void setWindowRect(const FloatRect&) final { }
    // Real viewport (CSS px), pushed from the port on init/resize. Feeds window.outerWidth/Height,
    // screenX/Y math, and any layout that reads the window/page rect. Fullscreen => window == page.
    FloatRect windowRect() final { return FloatRect(0, 0, s_viewportSize.width(), s_viewportSize.height()); }
    FloatRect pageRect() final { return FloatRect(0, 0, s_viewportSize.width(), s_viewportSize.height()); }

    void focus() final { }
    void unfocus() final { }

    bool canTakeFocus(FocusDirection) final { return false; }
    void takeFocus(FocusDirection) final { }

    void focusedElementChanged(Element*) final { }
    void focusedFrameChanged(Frame*) final { }

    Page* createWindow(Frame&, const WindowFeatures&, const NavigationAction&) final { return nullptr; }
    void show() final { }

    bool canRunModal() final { return false; }
    void runModal() final { }

    void setToolbarsVisible(bool) final { }
    bool toolbarsVisible() final { return false; }

    void setStatusbarVisible(bool) final { }
    bool statusbarVisible() final { return false; }

    void setScrollbarsVisible(bool) final { }
    bool scrollbarsVisible() final { return false; }

    void setMenubarVisible(bool) final { }
    bool menubarVisible() final { return false; }

    void setResizable(bool) final { }

    void addMessageToConsole(MessageSource, MessageLevel level, const String& message, unsigned lineNumber, unsigned, const String& sourceID) final
    {
        // Only surface JS warnings + errors (skip Log/Info/Debug chatter) — keeps the log to real
        // problems. sourceID is the originating script/site. (Error=3, Warning=2 in MessageLevel.)
        if (level != MessageLevel::Error && level != MessageLevel::Warning)
            return;
        extern void PortImgLog(const char*);
        auto msg = message.utf8();
        auto src = sourceID.utf8();
        char b[600];
        snprintf(b, sizeof b, "js: %s %s @ %s:%u", level == MessageLevel::Error ? "ERROR" : "WARN", msg.data(), src.data(), lineNumber);
        PortImgLog(b);
    }

    bool canRunBeforeUnloadConfirmPanel() final { return false; }
    bool runBeforeUnloadConfirmPanel(const String&, Frame&) final { return true; }

    void closeWindow() final { }

    void runJavaScriptAlert(Frame&, const String&) final { }
    bool runJavaScriptConfirm(Frame&, const String&) final { return false; }
    bool runJavaScriptPrompt(Frame&, const String&, const String&, String&) final { return false; }

    bool selectItemWritingDirectionIsNatural() final { return false; }
    bool selectItemAlignmentFollowsMenuWritingDirection() final { return false; }
    RefPtr<PopupMenu> createPopupMenu(PopupMenuClient&) const final;
    RefPtr<SearchPopupMenu> createSearchPopupMenu(PopupMenuClient&) const final;

    void setStatusbarText(const String&) final { }

    KeyboardUIMode keyboardUIMode() final { return KeyboardAccessDefault; }

    bool hoverSupportedByPrimaryPointingDevice() const final { return false; };
    bool hoverSupportedByAnyAvailablePointingDevice() const final { return false; }
    std::optional<PointerCharacteristics> pointerCharacteristicsOfPrimaryPointingDevice() const final { return std::nullopt; };
    OptionSet<PointerCharacteristics> pointerCharacteristicsOfAllAvailablePointingDevices() const final { return { }; }

    // Content invalidations (image decoded, animation, DOM change) must trigger a repaint
    // of our content layer, which is outside WebCore's own compositor invalidation tree.
    // Rect'd invalidations: accumulate the region so the driver re-rasters only that area.
    void invalidateRootView(const IntRect& r) final { m_dirtyRegion.unite(r); }
    void invalidateContentsAndRootView(const IntRect& r) override { m_dirtyRegion.unite(r); }
    void invalidateContentsForSlowScroll(const IntRect& r) final { m_dirtyRegion.unite(r); }
    void scroll(const IntSize&, const IntRect&, const IntRect&) final { }

    IntPoint screenToRootView(const IntPoint& p) const final { return p; }
    IntRect rootViewToScreen(const IntRect& r) const final { return r; }
    IntPoint accessibilityScreenToRootView(const IntPoint& p) const final { return p; };
    IntRect rootViewToAccessibilityScreen(const IntRect& r) const final { return r; };

    void didFinishLoadingImageForElement(HTMLImageElement&) final { }

    PlatformPageClient platformPageClient() const final { return 0; }
    void contentsSizeChanged(Frame&, const IntSize&) const final { }
    void intrinsicContentsSizeChanged(const IntSize&) const final { }

    void mouseDidMoveOverElement(const HitTestResult&, unsigned, const String&, TextDirection) final { }

    void print(Frame&, const StringWithDirection&) final { }

    void exceededDatabaseQuota(Frame&, const String&, DatabaseDetails) final { }

    void reachedMaxAppCacheSize(int64_t) final { }
    void reachedApplicationCacheOriginQuota(SecurityOrigin&, int64_t) final { }

#if ENABLE(INPUT_TYPE_COLOR)
    std::unique_ptr<ColorChooser> createColorChooser(ColorChooserClient&, const Color&) final;
#endif

#if ENABLE(DATALIST_ELEMENT)
    std::unique_ptr<DataListSuggestionPicker> createDataListSuggestionPicker(DataListSuggestionsClient&) final;
    bool canShowDataListSuggestionLabels() const final { return false; }
#endif

#if ENABLE(DATE_AND_TIME_INPUT_TYPES)
    std::unique_ptr<DateTimeChooser> createDateTimeChooser(DateTimeChooserClient&) final;
#endif

#if ENABLE(APP_HIGHLIGHTS)
    void storeAppHighlight(AppHighlight&&) const final;
#endif

    void setTextIndicator(const TextIndicatorData&) const final;

    DisplayRefreshMonitorFactory* displayRefreshMonitorFactory() const final;

    void runOpenPanel(Frame&, FileChooser&) final;
    void showShareSheet(ShareDataWithParsedURL&, CompletionHandler<void(bool)>&&) final;
    void loadIconForFiles(const Vector<String>&, FileIconLoader&) final { }

    void elementDidFocus(Element&) final;
    void elementDidBlur(Element&) final;

    void setCursor(const Cursor&) final { }
    void setCursorHiddenUntilMouseMoves(bool) final { }

    void scrollContainingScrollViewsToRevealRect(const IntRect&) const final { }
    void scrollMainFrameToRevealRect(const IntRect&) const final { }

    void attachRootGraphicsLayer(Frame&, GraphicsLayer* layer) final { m_rootGraphicsLayer = layer; m_needsCompositingFlush = true; }
    void attachViewOverlayGraphicsLayer(GraphicsLayer*) final { }
    void setNeedsOneShotDrawingSynchronization() final { m_needsCompositingFlush = true; }
    // WebCore's RenderingUpdateScheduler calls this when a rendering update is due (its WTF-timer
    // fallback is fired by the driver's RunLoop::iterate). THIS is the correct place to run the
    // real update — WebCore owns the cadence and cycles its own scheduler state. The driver's
    // per-frame RenderFrame only composites/presents; it no longer forces updateRendering.
    void triggerRenderingUpdate() final { m_needsCompositingFlush = true; WebCoreDriverRunRenderingUpdate(); }

#if PLATFORM(WIN)
    void setLastSetCursorToCurrentCursor() final { }
    void AXStartFrameLoad() final { }
    void AXFinishFrameLoad() final { }
#endif

#if ENABLE(IOS_TOUCH_EVENTS)
    void didPreventDefaultForEvent() final { }
#endif

#if PLATFORM(IOS_FAMILY)
    void didReceiveMobileDocType(bool) final { }
    void setNeedsScrollNotifications(Frame&, bool) final { }
    void didFinishContentChangeObserving(Frame&, WKContentChange) final { }
    void notifyRevealedSelectionByScrollingFrame(Frame&) final { }
    void didLayout(LayoutType) final { }
    void didStartOverflowScroll() final { }
    void didEndOverflowScroll() final { }

    void suppressFormNotifications() final { }
    void restoreFormNotifications() final { }

    void addOrUpdateScrollingLayer(Node*, PlatformLayer*, PlatformLayer*, const IntSize&, bool, bool) final { }
    void removeScrollingLayer(Node*, PlatformLayer*, PlatformLayer*) final { }

    void webAppOrientationsUpdated() final { }
    void showPlaybackTargetPicker(bool, RouteSharingPolicy, const String&) final { }

    bool showDataDetectorsUIForElement(const Element&, const Event&) final { return false; }
#endif // PLATFORM(IOS_FAMILY)

#if ENABLE(ORIENTATION_EVENTS)
    int deviceOrientation() const final { return 0; }
#endif

#if PLATFORM(IOS_FAMILY)
    bool isStopping() final { return false; }
#endif

    void wheelEventHandlersChanged(bool) final { }
    
    bool isEmptyChromeClient() const final { return false; }

    void didAssociateFormControls(const Vector<RefPtr<Element>>&, Frame&) final { }
    bool shouldNotifyOnFormChanges() final { return false; }

    RefPtr<Icon> createIconForFiles(const Vector<String>& /* filenames */) final { return nullptr; }

    void requestCookieConsent(CompletionHandler<void(CookieConsentDecisionResult)>&&) final;
    void classifyModalContainerControls(Vector<String>&&, CompletionHandler<void(Vector<ModalContainerControlType>&&)>&&) final;
    void decidePolicyForModalContainer(OptionSet<ModalContainerControlType>, CompletionHandler<void(ModalContainerDecision)>&&) final;
};

} // namespace WebCore
