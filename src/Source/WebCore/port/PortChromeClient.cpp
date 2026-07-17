// PortChromeClient.cpp — out-of-line methods. The Empty version used EmptyClients-
// internal helpers (EmptyPopupMenu, ...) not visible here; none of these UI surfaces
// (popup menus, choosers, share sheets) are exercised by the headless render, so they
// are minimal stubs.
#include "config.h"
#include "PortChromeClient.h"

#include "PortDisplayRefreshMonitor.h"
#include "ColorChooser.h"
#include "CookieConsentDecisionResult.h"
#include "DataListSuggestionPicker.h"
#include "DateTimeChooser.h"
#include "Document.h"
#include "Element.h"
#include "FullscreenManager.h"
#include "ModalContainerTypes.h"
#include "PopupMenu.h"
#include "SearchPopupMenu.h"

namespace WebCorePort { void portLog(const char*); }
using WebCorePort::portLog;
extern "C" void WebCoreBrowserKeepCompositing(unsigned frames);

// Implemented in the C++/CX shell: shows/hides the on-screen keyboard via Windows InputPane.
extern "C" void PortShowKeyboard(int show);

namespace WebCore {

IntSize PortChromeClient::s_viewportSize; // set by the port on init/resize; empty until then

// Web page text inputs render inside our SwapChainPanel, so the touch keyboard doesn't auto-show
// the way it does for the native URL TextBox. Trigger it from WebCore's focus notifications.
void PortChromeClient::elementDidFocus(Element& element)
{
    bool editable = element.isTextField() || element.isContentEditable() || element.hasEditableStyle();
    PortShowKeyboard(editable ? 1 : 0);
}

void PortChromeClient::elementDidBlur(Element&)
{
    PortShowKeyboard(0);
}

// A <select> tap routes through RenderMenuList::showPopup(), which calls m_popup->show() with NO
// null-check -- so returning nullptr here is a guaranteed virtual-call-through-null crash on every
// dropdown (and WebCoreBrowserTap isn't SEH-wrapped, so the crash filter never catches it). WebKit's
// own EmptyClients returns a non-null EmptyPopupMenu for exactly this reason; mirror it so the tap is
// crash-safe. (Follow-up: a real native XAML list-picker so the options are actually selectable --
// tracked separately; this only removes the crash, it does not yet render the option list.)
namespace {
class PortEmptyPopupMenu final : public PopupMenu {
public:
    PortEmptyPopupMenu() = default;
private:
    void show(const IntRect&, FrameView*, int) final { }
    void hide() final { }
    void updateFromElement() final { }
    void disconnectClient() final { }
};

class PortEmptySearchPopupMenu final : public SearchPopupMenu {
public:
    PortEmptySearchPopupMenu() : m_popup(adoptRef(*new PortEmptyPopupMenu)) { }
private:
    PopupMenu* popupMenu() final { return m_popup.ptr(); }
    void saveRecentSearches(const AtomString&, const Vector<RecentSearch>&) final { }
    void loadRecentSearches(const AtomString&, Vector<RecentSearch>&) final { }
    bool enabled() final { return false; }
    Ref<PortEmptyPopupMenu> m_popup;
};
}

RefPtr<PopupMenu> PortChromeClient::createPopupMenu(PopupMenuClient&) const { return adoptRef(*new PortEmptyPopupMenu); }
RefPtr<SearchPopupMenu> PortChromeClient::createSearchPopupMenu(PopupMenuClient&) const { return adoptRef(*new PortEmptySearchPopupMenu); }

#if ENABLE(INPUT_TYPE_COLOR)
std::unique_ptr<ColorChooser> PortChromeClient::createColorChooser(ColorChooserClient&, const Color&) { return nullptr; }
#endif

#if ENABLE(DATALIST_ELEMENT)
std::unique_ptr<DataListSuggestionPicker> PortChromeClient::createDataListSuggestionPicker(DataListSuggestionsClient&) { return nullptr; }
#endif

#if ENABLE(DATE_AND_TIME_INPUT_TYPES)
std::unique_ptr<DateTimeChooser> PortChromeClient::createDateTimeChooser(DateTimeChooserClient&) { return nullptr; }
#endif

#if ENABLE(APP_HIGHLIGHTS)
void PortChromeClient::storeAppHighlight(AppHighlight&&) const { }
#endif

void PortChromeClient::setTextIndicator(const TextIndicatorData&) const { }

DisplayRefreshMonitorFactory* PortChromeClient::displayRefreshMonitorFactory() const { return &PortDisplayRefreshMonitorFactory::singleton(); }

void PortChromeClient::runOpenPanel(Frame&, FileChooser&) { }
void PortChromeClient::showShareSheet(ShareDataWithParsedURL&, CompletionHandler<void(bool)>&& callback) { callback(false); }

#if ENABLE(FULLSCREEN_API)
// Drive WebCore's FullscreenManager state machine. The manager has already done the spec checks
// (user gesture, fullscreen-enabled, element eligibility) before calling us; our job is to perform
// the "transition" and report back. Our window is already the whole screen, so there is nothing
// native to do — willEnterFullscreen() applies the fullscreen element/styles (the element is laid
// out at viewport size, with the ::backdrop behind it), and didEnterFullscreen() fires
// fullscreenchange so the page's JS (YouTube's player) completes its own transition and tears down
// its scrim. Calling these SYNCHRONOUSLY is correct: there is no async native animation to await.
void PortChromeClient::enterFullScreenForElement(Element& element)
{
    portLog("fullscreen: enter");
    auto& manager = element.document().fullscreenManager();
    if (!manager.willEnterFullscreen(element)) {
        portLog("fullscreen: willEnterFullscreen REFUSED");
        return;
    }
    manager.didEnterFullscreen();
    WebCoreBrowserKeepCompositing(30); // repaint the relaid-out page
}

void PortChromeClient::exitFullScreenForElement(Element* element)
{
    if (!element)
        return;
    portLog("fullscreen: exit");
    auto& manager = element->document().fullscreenManager();
    if (!manager.willExitFullscreen()) {
        portLog("fullscreen: willExitFullscreen REFUSED");
        return;
    }
    manager.didExitFullscreen();
    WebCoreBrowserKeepCompositing(30);
}
#endif

void PortChromeClient::requestCookieConsent(CompletionHandler<void(CookieConsentDecisionResult)>&& completion) { completion(CookieConsentDecisionResult::NotSupported); }
void PortChromeClient::classifyModalContainerControls(Vector<String>&&, CompletionHandler<void(Vector<ModalContainerControlType>&&)>&& completion) { completion({ }); }
void PortChromeClient::decidePolicyForModalContainer(OptionSet<ModalContainerControlType>, CompletionHandler<void(ModalContainerDecision)>&& completion) { completion(ModalContainerDecision::Show); }

} // namespace WebCore
