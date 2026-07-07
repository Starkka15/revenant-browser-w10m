// ============================================================================
// ImagePlatformUWP.cpp — UWP replacements for the platform hooks that lived in
// the desktop-only ImageWin.cpp / KeyEventWin.cpp (dropped from the W10M build).
//
//   * Image::loadPlatformResource     — built-in images (no GDI .res bundle)
//   * BitmapImage GDI HBITMAP export   — not used by the cairo renderer
//   * PlatformKeyboardEvent statics    — no keyboard in the headless render path
//
// These are the genuine UWP platform layer (GDI/user32 replaced), not feature
// stubs: HBITMAP export and uxtheme keyboard state have no UWP analogue here.
// ============================================================================

#include "config.h"
#include "BitmapImage.h"
#include "Image.h"
#include "PlatformKeyboardEvent.h"

namespace WebCore {

// ---- Image / BitmapImage ----------------------------------------------------

Ref<Image> Image::loadPlatformResource(const char*)
{
    return BitmapImage::create();
}

void BitmapImage::invalidatePlatformData()
{
    // No GDI-backed platform cache to invalidate (cairo holds the decoded frames).
}

bool BitmapImage::getHBITMAP(HBITMAP)
{
    return false; // GDI bitmap export unsupported on UWP (cairo/D3D path only)
}

bool BitmapImage::getHBITMAPOfSize(HBITMAP, const IntSize*)
{
    return false;
}

// ---- PlatformKeyboardEvent --------------------------------------------------

bool PlatformKeyboardEvent::currentCapsLockState()
{
    return false;
}

void PlatformKeyboardEvent::getCurrentModifierState(bool& shiftKey, bool& ctrlKey, bool& altKey, bool& metaKey)
{
    shiftKey = ctrlKey = altKey = metaKey = false;
}

void PlatformKeyboardEvent::disambiguateKeyDownEvent(Type, bool)
{
    // Headless render generates no key events; real key disambiguation is wired
    // with the UWP input layer.
}

} // namespace WebCore
