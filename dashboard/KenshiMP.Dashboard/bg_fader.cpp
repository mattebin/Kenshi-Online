#include "bg_fader.h"
#include <Shlwapi.h>
#include <stdio.h>

#pragma comment(lib, "gdiplus.lib")

namespace kmp::dash {

ULONG_PTR BgFader::Startup() {
    Gdiplus::GdiplusStartupInput input;
    ULONG_PTR token = 0;
    Gdiplus::GdiplusStartup(&token, &input, nullptr);
    return token;
}

void BgFader::Shutdown(ULONG_PTR token) {
    if (token) Gdiplus::GdiplusShutdown(token);
}

BgFader::BgFader(const std::wstring& folder, int dwellMs, int fadeMs)
    : m_dwellMs(dwellMs), m_fadeMs(fadeMs),
      m_phaseStart(std::chrono::steady_clock::now()) {
    LoadFolder(folder);
    if (m_images.size() >= 2) {
        m_nextIdx = 1 % m_images.size();
    }
}

BgFader::~BgFader() = default;

void BgFader::LoadFolder(const std::wstring& folder) {
    static const wchar_t* kExtensions[] = {
        L"\\*.jpg", L"\\*.jpeg", L"\\*.png", L"\\*.bmp"
    };
    for (auto ext : kExtensions) {
        std::wstring pattern = folder + ext;
        WIN32_FIND_DATAW fd{};
        HANDLE h = FindFirstFileW(pattern.c_str(), &fd);
        if (h == INVALID_HANDLE_VALUE) continue;
        do {
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
            std::wstring full = folder + L"\\" + fd.cFileName;
            ImagePtr img(Gdiplus::Image::FromFile(full.c_str()));
            if (!img || img->GetLastStatus() != Gdiplus::Ok) {
                char dbg[300]{};
                char nameA[260]{};
                WideCharToMultiByte(CP_UTF8, 0, fd.cFileName, -1,
                                     nameA, sizeof(nameA), nullptr, nullptr);
                sprintf_s(dbg, "BgFader: failed to load %s\n", nameA);
                OutputDebugStringA(dbg);
                continue;
            }
            m_images.push_back(std::move(img));
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    char dbg[80]{};
    sprintf_s(dbg, "BgFader: loaded %zu image(s)\n", m_images.size());
    OutputDebugStringA(dbg);
}

bool BgFader::Tick() {
    if (m_images.size() < 2) return false;
    auto now = std::chrono::steady_clock::now();
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        now - m_phaseStart).count();

    bool changed = false;
    if (m_phase == Phase::Dwell) {
        if (elapsed >= m_dwellMs) {
            m_phase = Phase::Fade;
            m_phaseStart = now;
            changed = true;
        }
    } else {
        if (elapsed >= m_fadeMs) {
            // Promote next → current and pick a new next.
            m_currentIdx = m_nextIdx;
            m_nextIdx = (m_nextIdx + 1) % m_images.size();
            m_phase = Phase::Dwell;
            m_phaseStart = now;
            m_lastAlpha = 0.0f;
            changed = true;
        } else {
            float a = static_cast<float>(elapsed) /
                      static_cast<float>(m_fadeMs);
            if (a > 1.0f) a = 1.0f;
            // Repaint when the alpha moved by at least 1/256 — finer
            // updates are imperceptible on a typical monitor.
            if (m_lastAlpha < 0.0f ||
                std::abs(a - m_lastAlpha) > (1.0f / 256.0f)) {
                m_lastAlpha = a;
                changed = true;
            }
        }
    }
    return changed;
}

void BgFader::DrawAtAlpha(Gdiplus::Graphics& g, Gdiplus::Image* img,
                           const RECT& dest, float alpha) {
    if (!img) return;
    if (alpha < 0.0f) alpha = 0.0f;
    if (alpha > 1.0f) alpha = 1.0f;

    Gdiplus::ColorMatrix cm = {{
        {1.0f, 0,    0,    0,        0},
        {0,    1.0f, 0,    0,        0},
        {0,    0,    1.0f, 0,        0},
        {0,    0,    0,    alpha,    0},
        {0,    0,    0,    0,     1.0f}
    }};
    Gdiplus::ImageAttributes attr;
    attr.SetColorMatrix(&cm, Gdiplus::ColorMatrixFlagsDefault,
                         Gdiplus::ColorAdjustTypeBitmap);

    int dx = dest.left, dy = dest.top;
    int dw = dest.right - dest.left;
    int dh = dest.bottom - dest.top;
    Gdiplus::Rect r(dx, dy, dw, dh);

    g.DrawImage(img, r, 0, 0, img->GetWidth(), img->GetHeight(),
                 Gdiplus::UnitPixel, &attr);
}

void BgFader::PaintToMemDc(HDC memDc, int w, int h) {
    Gdiplus::Graphics g(memDc);
    g.SetInterpolationMode(Gdiplus::InterpolationModeHighQualityBicubic);

    // Background fill so under-sized images don't leave junk.  Solid
    // alpha so the image overwrites cleanly on each frame.
    Gdiplus::SolidBrush black(Gdiplus::Color(255, 20, 20, 24));
    g.FillRectangle(&black, 0, 0, w, h);

    if (m_images.empty()) return;

    RECT clientRect{0, 0, w, h};
    Gdiplus::Image* cur = m_images[m_currentIdx % m_images.size()].get();
    if (m_phase == Phase::Dwell || m_images.size() < 2) {
        DrawAtAlpha(g, cur, clientRect, 1.0f);
        return;
    }
    // Cross-fade: cur fades out, next fades in, simultaneously —
    // combined alpha sums to ~1 so the blended frame stays at
    // consistent perceived brightness.
    float a = m_lastAlpha < 0.0f ? 0.0f : m_lastAlpha;
    DrawAtAlpha(g, cur, clientRect, 1.0f - a);
    Gdiplus::Image* nxt = m_images[m_nextIdx % m_images.size()].get();
    DrawAtAlpha(g, nxt, clientRect, a);
}

void BgFader::Paint(HDC dc, const RECT& clientRect) {
    if (m_images.empty()) {
        // No images — draw a flat colour and bail.
        HBRUSH bg = CreateSolidBrush(RGB(40, 40, 50));
        FillRect(dc, &clientRect, bg);
        DeleteObject(bg);
        return;
    }

    // Double-buffer to a memory DC to avoid flicker.  Without this
    // each frame redraws on the live DC and the controls' borders
    // flash on every fade tick.
    int w = clientRect.right - clientRect.left;
    int h = clientRect.bottom - clientRect.top;
    HDC mem = CreateCompatibleDC(dc);
    HBITMAP bmp = CreateCompatibleBitmap(dc, w, h);
    HGDIOBJ oldBmp = SelectObject(mem, bmp);

    PaintToMemDc(mem, w, h);

    BitBlt(dc, 0, 0, w, h, mem, 0, 0, SRCCOPY);
    SelectObject(mem, oldBmp);
    DeleteObject(bmp);
    DeleteDC(mem);
}

} // namespace kmp::dash
