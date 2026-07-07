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
#include "Element.h"
#include "ModalContainerTypes.h"

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

RefPtr<PopupMenu> PortChromeClient::createPopupMenu(PopupMenuClient&) const { return nullptr; }
RefPtr<SearchPopupMenu> PortChromeClient::createSearchPopupMenu(PopupMenuClient&) const { return nullptr; }

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

void PortChromeClient::requestCookieConsent(CompletionHandler<void(CookieConsentDecisionResult)>&& completion) { completion(CookieConsentDecisionResult::NotSupported); }
void PortChromeClient::classifyModalContainerControls(Vector<String>&&, CompletionHandler<void(Vector<ModalContainerControlType>&&)>&& completion) { completion({ }); }
void PortChromeClient::decidePolicyForModalContainer(OptionSet<ModalContainerControlType>, CompletionHandler<void(ModalContainerDecision)>&& completion) { completion(ModalContainerDecision::Show); }

} // namespace WebCore
