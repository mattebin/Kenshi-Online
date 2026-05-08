// =========================================================================
//                          bg_fader
// =========================================================================
// Crossfading background image cycler for the Dashboard window.
//
// Loads every JPG/PNG/BMP it finds in a watch folder into GDI+ Image
// objects at startup, then cycles through them on a clock with a
// dwell phase (image visible at full alpha) and a fade phase (current
// image fades out, next fades in).  Painting goes through `Paint`
// which the host window calls from `WM_ERASEBKGND`.
//
// Animation timing
// ----------------
//   dwell  4000 ms — image at full visibility
//   fade   2000 ms — alpha-blend to the next image
// Total period per image = 6 s.  Override at construction.
//
// Why GDI+
// --------
// Win32 GDI alone has no built-in alpha-blended image draw with
// per-image opacity.  GDI+ does, via `ImageAttributes` + a
// ColorMatrix.  No third-party dependencies; comes with Windows.
#pragma once

// GDI+ requires `min`/`max` macros to be visible (the headers use
// them unqualified) and `IStream` from objidl.h.  Both must be set
// up BEFORE including <gdiplus.h>.  We undo the macro pollution at
// the bottom of this file so the rest of the project keeps using
// the C++ <algorithm> versions.
#include <Windows.h>
#include <objidl.h>
#ifndef NOMINMAX
#define KMP_GDIPLUS_RESTORE_NOMINMAX 1
#endif
#undef NOMINMAX
#include <algorithm>
using std::min;
using std::max;
#include <gdiplus.h>
#ifdef KMP_GDIPLUS_RESTORE_NOMINMAX
#define NOMINMAX 1
#undef KMP_GDIPLUS_RESTORE_NOMINMAX
#endif
#include <chrono>
#include <memory>
#include <string>
#include <vector>

namespace kmp::dash {

class BgFader {
public:
    // Initialise GDI+ once per process.  Returns the token to pass to
    // `Shutdown` at exit.  The Dashboard calls this in wWinMain.
    static ULONG_PTR Startup();
    static void     Shutdown(ULONG_PTR token);

    // Construct + load every image we can find in `folder`.
    // Failures (corrupt files, unreadable paths) are logged via
    // OutputDebugString and skipped — partial loads are fine.
    explicit BgFader(const std::wstring& folder,
                     int dwellMs = 4000,
                     int fadeMs  = 2000);
    ~BgFader();

    // Returns the number of images successfully loaded.  When 0 the
    // host should fall back to a solid colour.
    size_t ImageCount() const { return m_images.size(); }

    // Paint into the window's client area.  Idempotent and self-
    // contained; call from `WM_ERASEBKGND` and return non-zero so
    // Windows doesn't redraw the default background underneath.
    void Paint(HDC dc, const RECT& clientRect);

    // Paint just the background image into a caller-owned memory DC.
    // Use when you want to layer additional content (e.g. translucent
    // log overlay) on top of the bg before BitBlt'ing the composed
    // result to the window — composing in one off-screen buffer
    // gives flicker-free, single-blit output.  The caller is
    // responsible for creating the memory DC + bitmap and BitBlt'ing
    // when done.  `w`/`h` are the area the image is painted into,
    // origin (0,0).
    void PaintToMemDc(HDC memDc, int w, int h);

    // Advance the animation clock.  Call from the window's refresh
    // timer; the host invalidates after each call so Paint runs.
    // Returns true if the visible state changed (alpha advanced past
    // a perceptible threshold) — host can use this to skip
    // InvalidateRect when nothing visually changed.
    bool Tick();

private:
    using ImagePtr = std::unique_ptr<Gdiplus::Image>;

    void LoadFolder(const std::wstring& folder);
    void DrawAtAlpha(Gdiplus::Graphics& g, Gdiplus::Image* img,
                      const RECT& dest, float alpha);

    std::vector<ImagePtr>      m_images;
    int                         m_dwellMs;
    int                         m_fadeMs;
    std::chrono::steady_clock::time_point m_phaseStart;
    enum class Phase { Dwell, Fade };
    Phase                       m_phase = Phase::Dwell;
    size_t                      m_currentIdx = 0;
    size_t                      m_nextIdx    = 0;
    float                       m_lastAlpha  = -1.0f;
};

} // namespace kmp::dash
