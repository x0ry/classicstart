// ClassicShell — a lightweight, classic-style Start menu replacement.
// Copyright (c) 2026 cory@coryglenn.ai
// SPDX-License-Identifier: MIT — see LICENSE for the full text.

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX

#include <windows.h>
#include <shellapi.h>
#include <shlobj.h>
#include <objbase.h>
#include <objidl.h>
#include <dwmapi.h>
#include <tlhelp32.h>
#include <winioctl.h>
#include <winver.h>
#include <gdiplus.h>
#include <shlwapi.h>
#include <windowsx.h>
#include <d3d11.h>
#include <d2d1_3.h>
#include <wincodec.h>

#include <string>
#include <vector>
#include <algorithm>
#include <cwctype>
#include <fstream>
#include <sstream>
#include <mutex>
#include <atomic>

#include "starthook.h"

#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "msimg32.lib")
#pragma comment(lib, "version.lib")
#pragma comment(lib, "gdiplus.lib")
#pragma comment(lib, "shlwapi.lib")
#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "d2d1.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "windowscodecs.lib")

static const wchar_t START_CLASS[] = L"ClassicShell.Native";

static HWND  g_start = nullptr;
static HHOOK g_keyboardHook = nullptr;
static HHOOK g_mouseHook = nullptr;

static HFONT g_font = nullptr;
static HFONT g_bold = nullptr;
static HFONT g_small = nullptr;
static HFONT g_icon = nullptr;

static UINT g_dpi = 96;

// Overall UI scale, independent of DPI. Configurable via
// classicshell.ini's [Appearance] Scale key; defaults to 0.85 (15%
// smaller than the original 1.0 design size).
static float g_userScale = 0.85f;

static ULONG_PTR g_gdiplusToken = 0;

static bool g_startVisible = false;
static bool g_powerOpen = false;

// Power flyout open/close animation progress: 0 = fully closed,
// 1 = fully open. Driven by a UI timer toward whatever g_powerOpen
// currently wants.
static float g_powerAnim = 0.0f;
static UINT_PTR g_powerTimer = 0;
static const UINT_PTR TIMER_POWER_ANIM = 1;

// Search box hover highlight, animated the same way.
static bool g_searchHover = false;
static float g_searchAnim = 0.0f;
static UINT_PTR g_searchTimer = 0;
static const UINT_PTR TIMER_SEARCH_ANIM = 2;

// Blinking text caret in the search box.
static bool g_caretVisible = true;
static UINT_PTR g_caretTimer = 0;
static const UINT_PTR TIMER_CARET_BLINK = 3;

// Whole-window show/hide "pop" animation: the window grows up out
// of its own bottom edge (with a little overshoot bounce) when
// opening, and shrinks back into it when closing.
enum class ShowAnimMode
{
    None,
    Opening,
    Closing
};

static ShowAnimMode g_showAnimMode = ShowAnimMode::None;
static float g_showAnimT = 0.0f;
static UINT_PTR g_showAnimTimer = 0;
static const UINT_PTR TIMER_SHOW_ANIM = 4;
static RECT g_showAnimFinalRect{};

// Whole-window translucency, user-adjustable at runtime via the
// opacity slider in the bottom strip.
static const BYTE START_WINDOW_ALPHA = 225;
static const BYTE OPACITY_MIN = 90;
static const BYTE OPACITY_MAX = 255;
static BYTE g_windowAlpha = START_WINDOW_ALPHA;
static bool g_sliderDragging = false;
static bool g_searchDragging = false;

static int g_hover = -1;
static int g_powerHover = -1;

// Keyboard focus, as a flat index into a dynamic sequence of
// focusable controls (search, the 7 items, any quick-launch
// tools, the opacity slider, the power button, and — only while
// the flyout is open — restart/shutdown). -1 means the user
// hasn't started tabbing yet, so no focus ring is drawn.
static int g_focusIndex = -1;

static std::wstring g_searchText;
static TextSelection g_searchSelection;

static WinKeyTracker g_winKeyTracker;
static StartButtonMouseTracker g_startButtonTracker;

// [Experimental] CaptureWinKey from classicshell.ini — off by default.
// The physical Windows key is always forwarded via CallNextHookEx either
// way (see KeyboardProc); this only gates whether a standalone tap also
// triggers ToggleStart(), so users who want it can opt back in while the
// default leaves the native/legacy Start menu in charge of the Win key.
static bool g_captureWinKey = false;

// [Search] IndexPath from classicshell.ini — the folder background-indexed
// at startup for the search box. Defaults to the user's profile folder
// when blank.
static std::wstring g_searchIndexRoot;

static bool g_mouseTracking = false;

// ============================================================
// Colors
// ============================================================

static COLORREF g_bg;
static COLORREF g_panel;
static COLORREF g_hot;
static COLORREF g_border;
static COLORREF g_accent;
static COLORREF g_accentText;
static COLORREF g_accentBorder;
static COLORREF g_text;
static COLORREF g_muted;

// Set by RefreshSystemColors; read by ApplyAcrylicBlur, which needs to
// know this separately from just the colors themselves — see its own
// comment for why a light tint needs a different opacity than a dark
// one, not just a different color.
static bool g_isLightTheme = false;

static int MinI(int a, int b)
{
    return a < b ? a : b;
}

static int MaxI(int a, int b)
{
    return a > b ? a : b;
}

static COLORREF MixColor(
    COLORREF a,
    COLORREF b,
    int percentB)
{
    percentB = MaxI(0, MinI(100, percentB));

    int ar = GetRValue(a);
    int ag = GetGValue(a);
    int ab = GetBValue(a);

    int br = GetRValue(b);
    int bg = GetGValue(b);
    int bb = GetBValue(b);

    return RGB(
        ar + (br - ar) * percentB / 100,
        ag + (bg - ag) * percentB / 100,
        ab + (bb - ab) * percentB / 100);
}

// The actual accent color the user picked in Settings > Personalize
// > Colors — not the legacy COLOR_HIGHLIGHT system color, which on
// modern Windows doesn't reliably track it (custom or "automatic"
// accent colors especially). This is the same registry value DWM
// itself paints title bars/Start/taskbar with.
static COLORREF GetWindowsAccentColor()
{
    DWORD value = 0;
    DWORD size = sizeof(value);

    LSTATUS status =
        RegGetValueW(
            HKEY_CURRENT_USER,
            L"Software\\Microsoft\\Windows\\DWM",
            L"AccentColor",
            RRF_RT_REG_DWORD,
            nullptr,
            &value,
            &size);

    if (status == ERROR_SUCCESS)
    {
        // Stored as 0xAABBGGRR; COLORREF is 0x00BBGGRR, so this
        // is already the right byte order once alpha is dropped.
        return (COLORREF)(value & 0x00FFFFFF);
    }

    return GetSysColor(COLOR_HIGHLIGHT);
}

// Picks readable text for an arbitrary accent color rather than
// trusting COLOR_HIGHLIGHTTEXT, since user-chosen accents range
// from very dark to very pale and a fixed white/black won't
// always have enough contrast.
static COLORREF ContrastTextFor(
    COLORREF background)
{
    double luminance =
        0.299 * GetRValue(background) +
        0.587 * GetGValue(background) +
        0.114 * GetBValue(background);

    return luminance > 150.0
        ? RGB(20, 20, 20)
        : RGB(255, 255, 255);
}

// The taskbar's own light/dark mode — a separate registry value from
// the per-app "AppsUseLightTheme" setting, and the one that actually
// governs whether the real Windows 11 taskbar/Start renders light or
// dark. Matching this (rather than always rendering dark) is what makes
// ClassicShell read as "the same drapes" as the taskbar it sits on,
// instead of looking like a mismatched dark panel dropped onto a light
// system theme.
static bool IsSystemUsingLightTheme()
{
    DWORD value = 0;
    DWORD size = sizeof(value);

    LSTATUS status =
        RegGetValueW(
            HKEY_CURRENT_USER,
            L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
            L"SystemUsesLightTheme",
            RRF_RT_REG_DWORD,
            nullptr,
            &value,
            &size);

    // Windows itself defaults to dark when the key is missing (a clean
    // install's actual default is light, but that's set explicitly by
    // OOBE — an unreadable key here more plausibly means something else
    // is wrong than that this is a fresh-install edge case), so this
    // falls back to the dark palette already in use rather than
    // guessing light.
    if (status != ERROR_SUCCESS)
        return false;

    return value != 0;
}

// Light-theme palette matching was attempted and reverted: it caused a
// text-legibility regression (dark theme's own text/background pairing
// stayed correct, but something about the light branch made ordinary
// g_text-colored text render invisible everywhere in the menu, even at
// full window opacity) that couldn't be pinned down without live visual
// feedback on an actual light-themed system, which isn't available in
// this environment. g_isLightTheme and IsSystemUsingLightTheme() are
// left in place since they're harmless and correctly detect the
// taskbar's theme; RefreshSystemColors just doesn't act on it for now.
// Revisit this once it can actually be verified against a live light
// theme instead of guessed at blind.
static void RefreshSystemColors()
{
    g_isLightTheme = IsSystemUsingLightTheme();

    // Dark, slightly blue-black base.
    g_bg = RGB(14, 15, 18);

    // Main surface.
    g_panel = RGB(24, 26, 31);

    // Fine separators / borders.
    g_border = RGB(53, 57, 64);

    g_text = RGB(237, 240, 244);
    g_muted = RGB(145, 151, 161);

    // Windows accent — the real one from the current theme.
    g_accent = GetWindowsAccentColor();
    g_accentText = ContrastTextFor(g_accent);

    // Accent, lightened — the border/outline shown around a selected
    // row or an active button.
    g_accentBorder = MixColor(
        g_accent,
        RGB(255, 255, 255),
        18);

    // Subtle accent-tinted hover.
    g_hot = MixColor(
        g_panel,
        g_accent,
        16);
}

// ============================================================
// Scaling
// ============================================================

static int S(int value)
{
    float scaled =
        (float)MulDiv(
            value,
            (int)g_dpi,
            96) *
        g_userScale;

    return (int)(
        scaled >= 0.0f
            ? scaled + 0.5f
            : scaled - 0.5f);
}

// ============================================================
// UI animation
// ============================================================

static float EaseTo(
    float current,
    float target,
    float rate)
{
    float diff =
        target - current;

    current += diff * rate;

    float remaining =
        target - current;

    if (remaining < 0.0f)
        remaining = -remaining;

    return remaining < 0.01f
        ? target
        : current;
}

// Overshoots slightly past 1.0 before settling — the little
// "bounce" that makes the window feel like it pops into view
// instead of just fading.
static float EaseOutBack(
    float t)
{
    const float c1 = 1.70158f;
    const float c3 = c1 + 1.0f;

    float p = t - 1.0f;

    return
        1.0f +
        c3 * p * p * p +
        c1 * p * p;
}

// Accelerating, no overshoot — used for closing, so the window
// snaps away rather than bouncing on exit.
static float EaseInCubic(
    float t)
{
    return t * t * t;
}

static void StepShowAnim(
    HWND hwnd)
{
    bool opening =
        g_showAnimMode ==
        ShowAnimMode::Opening;

    if (opening)
    {
        g_showAnimT += 1.0f / 13.0f;

        if (g_showAnimT > 1.0f)
            g_showAnimT = 1.0f;
    }
    else
    {
        g_showAnimT -= 1.0f / 9.0f;

        if (g_showAnimT < 0.0f)
            g_showAnimT = 0.0f;
    }

    float shown =
        opening
            ? EaseOutBack(g_showAnimT)
            : EaseInCubic(g_showAnimT);

    int finalHeight =
        g_showAnimFinalRect.bottom -
        g_showAnimFinalRect.top;

    int startHeight =
        (int)(finalHeight * 0.55f);

    if (startHeight < 40)
        startHeight = 40;

    float hf =
        startHeight +
        (finalHeight - startHeight) *
            shown;

    int h = (int)hf;

    if (h < 20)
        h = 20;

    int top =
        g_showAnimFinalRect.bottom - h;

    SetWindowPos(
        hwnd,
        nullptr,
        g_showAnimFinalRect.left,
        top,
        g_showAnimFinalRect.right -
            g_showAnimFinalRect.left,
        h,
        SWP_NOZORDER |
            SWP_NOACTIVATE);

    float alphaT = shown;

    if (alphaT < 0.0f)
        alphaT = 0.0f;

    if (alphaT > 1.0f)
        alphaT = 1.0f;

    SetLayeredWindowAttributes(
        hwnd,
        0,
        (BYTE)(g_windowAlpha * alphaT),
        LWA_ALPHA);

    InvalidateRect(
        hwnd,
        nullptr,
        FALSE);

    bool done =
        opening
            ? g_showAnimT >= 1.0f
            : g_showAnimT <= 0.0f;

    if (!done)
        return;

    KillTimer(
        hwnd,
        TIMER_SHOW_ANIM);

    g_showAnimTimer = 0;
    g_showAnimMode = ShowAnimMode::None;

    if (opening)
    {
        SetWindowPos(
            hwnd,
            nullptr,
            g_showAnimFinalRect.left,
            g_showAnimFinalRect.top,
            g_showAnimFinalRect.right -
                g_showAnimFinalRect.left,
            g_showAnimFinalRect.bottom -
                g_showAnimFinalRect.top,
            SWP_NOZORDER |
                SWP_NOACTIVATE);

        SetLayeredWindowAttributes(
            hwnd,
            0,
            g_windowAlpha,
            LWA_ALPHA);
    }
    else
    {
        ShowWindow(
            hwnd,
            SW_HIDE);
    }
}

static void ResetUIState()
{
    g_powerOpen = false;
    g_powerAnim = 0.0f;

    g_searchHover = false;
    g_searchAnim = 0.0f;

    g_caretVisible = true;

    g_focusIndex = -1;

    if (g_start)
    {
        if (g_powerTimer)
            KillTimer(
                g_start,
                TIMER_POWER_ANIM);

        if (g_searchTimer)
            KillTimer(
                g_start,
                TIMER_SEARCH_ANIM);

        if (g_caretTimer)
            KillTimer(
                g_start,
                TIMER_CARET_BLINK);
    }

    g_powerTimer = 0;
    g_searchTimer = 0;
    g_caretTimer = 0;
}

static void SetPowerOpen(
    HWND hwnd,
    bool open)
{
    if (g_powerOpen == open)
        return;

    g_powerOpen = open;

    if (!g_powerTimer &&
        hwnd)
    {
        g_powerTimer =
            SetTimer(
                hwnd,
                TIMER_POWER_ANIM,
                10,
                nullptr);
    }
}

static void StepPowerAnim(
    HWND hwnd)
{
    g_powerAnim =
        EaseTo(
            g_powerAnim,
            g_powerOpen
                ? 1.0f
                : 0.0f,
            0.35f);

    if (g_powerTimer &&
        g_powerAnim ==
            (g_powerOpen
                ? 1.0f
                : 0.0f))
    {
        KillTimer(
            hwnd,
            TIMER_POWER_ANIM);

        g_powerTimer = 0;
    }

    InvalidateRect(
        hwnd,
        nullptr,
        FALSE);
}

static void SetSearchHover(
    HWND hwnd,
    bool hover)
{
    if (g_searchHover == hover)
        return;

    g_searchHover = hover;

    if (!g_searchTimer &&
        hwnd)
    {
        g_searchTimer =
            SetTimer(
                hwnd,
                TIMER_SEARCH_ANIM,
                10,
                nullptr);
    }
}

static void StepSearchAnim(
    HWND hwnd)
{
    g_searchAnim =
        EaseTo(
            g_searchAnim,
            g_searchHover
                ? 1.0f
                : 0.0f,
            0.25f);

    if (g_searchTimer &&
        g_searchAnim ==
            (g_searchHover
                ? 1.0f
                : 0.0f))
    {
        KillTimer(
            hwnd,
            TIMER_SEARCH_ANIM);

        g_searchTimer = 0;
    }

    InvalidateRect(
        hwnd,
        nullptr,
        FALSE);
}

static void StepCaretBlink(
    HWND hwnd)
{
    g_caretVisible =
        !g_caretVisible;

    InvalidateRect(
        hwnd,
        nullptr,
        FALSE);
}

// ============================================================
// Strings
// ============================================================

static std::wstring Trim(
    const std::wstring& s)
{
    size_t a = 0;
    size_t b = s.size();

    while (a < b &&
           iswspace(s[a]))
        ++a;

    while (b > a &&
           iswspace(s[b - 1]))
        --b;

    return s.substr(
        a,
        b - a);
}

static std::wstring Lower(
    std::wstring s)
{
    std::transform(
        s.begin(),
        s.end(),
        s.begin(),
        [](wchar_t c)
        {
            return (wchar_t)towlower(c);
        });

    return s;
}

static bool StartsWithI(
    const std::wstring& a,
    const std::wstring& b)
{
    if (a.size() < b.size())
        return false;

    return Lower(
        a.substr(0, b.size())) ==
        Lower(b);
}

static bool EndsWithI(
    const std::wstring& a,
    const std::wstring& b)
{
    if (a.size() < b.size())
        return false;

    return Lower(
        a.substr(
            a.size() - b.size())) ==
        Lower(b);
}

// ============================================================
// Files
// ============================================================

static bool FileExists(
    const std::wstring& path)
{
    DWORD a =
        GetFileAttributesW(
            path.c_str());

    return
        a != INVALID_FILE_ATTRIBUTES &&
        !(a & FILE_ATTRIBUTE_DIRECTORY);
}

static bool DirectoryExists(
    const std::wstring& path)
{
    DWORD a =
        GetFileAttributesW(
            path.c_str());

    return
        a != INVALID_FILE_ATTRIBUTES &&
        (a & FILE_ATTRIBUTE_DIRECTORY);
}

// ============================================================
// Known folders
// ============================================================

static bool KnownFolder(
    REFKNOWNFOLDERID id,
    std::wstring& out)
{
    PWSTR path = nullptr;

    HRESULT hr =
        SHGetKnownFolderPath(
            id,
            0,
            nullptr,
            &path);

    if (FAILED(hr) || !path)
        return false;

    out = path;

    CoTaskMemFree(path);

    return true;
}

// ============================================================
// Environment
// ============================================================

static std::wstring ExpandEnv(
    const std::wstring& value)
{
    DWORD needed =
        ExpandEnvironmentStringsW(
            value.c_str(),
            nullptr,
            0);

    if (!needed)
        return value;

    std::vector<wchar_t> buffer(
        needed + 2);

    DWORD result =
        ExpandEnvironmentStringsW(
            value.c_str(),
            buffer.data(),
            (DWORD)buffer.size());

    return result
        ? std::wstring(buffer.data())
        : value;
}

// ============================================================
// Aliases
// ============================================================

static bool ResolveAlias(
    const std::wstring& input,
    std::wstring& out)
{
    std::wstring s =
        Lower(Trim(input));

    struct Alias
    {
        const wchar_t* name;
        REFKNOWNFOLDERID id;
    };

    static const Alias aliases[] =
    {
        { L"desktop",       FOLDERID_Desktop },
        { L"documents",     FOLDERID_Documents },
        { L"my documents",  FOLDERID_Documents },
        { L"docs",          FOLDERID_Documents },
        { L"downloads",     FOLDERID_Downloads },
        { L"pictures",      FOLDERID_Pictures },
        { L"photos",        FOLDERID_Pictures },
        { L"music",         FOLDERID_Music },
        { L"videos",        FOLDERID_Videos },
        { L"home",          FOLDERID_Profile },
        { L"user",          FOLDERID_Profile },
        { L"profile",       FOLDERID_Profile },
        { L"appdata",       FOLDERID_RoamingAppData },
        { L"localappdata",  FOLDERID_LocalAppData },
        { L"startup",       FOLDERID_Startup },
        { L"programs",      FOLDERID_Programs },
        { L"start menu",    FOLDERID_Programs }
    };

    for (const auto& a : aliases)
    {
        if (s == a.name &&
            KnownFolder(a.id, out))
        {
            return true;
        }
    }

    if (s == L"temp" ||
        s == L"tmp")
    {
        wchar_t b[MAX_PATH]{};

        DWORD n =
            GetTempPathW(
                MAX_PATH,
                b);

        if (n)
        {
            out.assign(b, n);

            while (!out.empty() &&
                   (out.back() == L'\\' ||
                    out.back() == L'/'))
            {
                out.pop_back();
            }

            return true;
        }
    }

    if (s == L"windows" ||
        s == L"win")
    {
        wchar_t b[MAX_PATH]{};

        UINT n =
            GetWindowsDirectoryW(
                b,
                MAX_PATH);

        if (n)
        {
            out.assign(b, n);
            return true;
        }
    }

    if (s == L"system32" ||
        s == L"system")
    {
        wchar_t b[MAX_PATH]{};

        UINT n =
            GetSystemDirectoryW(
                b,
                MAX_PATH);

        if (n)
        {
            out.assign(b, n);
            return true;
        }
    }

    return false;
}

// ============================================================
// Special Windows targets
// ============================================================

static bool ResolveSpecial(
    const std::wstring& input,
    std::wstring& out)
{
    std::wstring s =
        Lower(Trim(input));

    struct Special
    {
        const wchar_t* name;
        const wchar_t* target;
    };

    static const Special specials[] =
    {
        { L"this pc",          L"shell:MyComputerFolder" },
        { L"computer",         L"shell:MyComputerFolder" },
        { L"my computer",      L"shell:MyComputerFolder" },
        { L"recycle bin",      L"shell:RecycleBinFolder" },
        { L"recyclebin",       L"shell:RecycleBinFolder" },
        { L"network",          L"shell:NetworkPlacesFolder" },
        { L"control panel",    L"control.exe" },
        { L"control",          L"control.exe" },
        { L"settings",         L"ms-settings:" },
        { L"task manager",     L"taskmgr.exe" },
        { L"taskmgr",          L"taskmgr.exe" },
        { L"command prompt",   L"cmd.exe" },
        { L"cmd",              L"cmd.exe" },
        { L"powershell",       L"powershell.exe" },
        { L"terminal",         L"wt.exe" },
        { L"notepad",          L"notepad.exe" },
        { L"calculator",       L"calc.exe" },
        { L"calc",             L"calc.exe" },
        { L"registry",         L"regedit.exe" },
        { L"registry editor",  L"regedit.exe" },
        { L"regedit",          L"regedit.exe" },
        { L"device manager",   L"devmgmt.msc" },
        { L"devmgmt",          L"devmgmt.msc" },
        { L"services",         L"services.msc" },
        { L"event viewer",     L"eventvwr.msc" },
        { L"eventvwr",         L"eventvwr.msc" },
        { L"msconfig",         L"msconfig.exe" },
        { L"dxdiag",           L"dxdiag.exe" },
        { L"winver",           L"winver.exe" },
        { L"windows version",  L"winver.exe" }
    };

    for (const auto& x : specials)
    {
        if (s == x.name)
        {
            out = x.target;
            return true;
        }
    }

    return false;
}

// ============================================================
// URL / path normalization
// ============================================================

static bool IsUrl(
    const std::wstring& s)
{
    std::wstring x =
        Lower(Trim(s));

    return
        StartsWithI(x, L"http://") ||
        StartsWithI(x, L"https://") ||
        StartsWithI(x, L"ftp://") ||
        StartsWithI(x, L"mailto:") ||
        StartsWithI(x, L"ms-settings:");
}

static std::wstring NormalizePath(
    std::wstring path)
{
    path = Trim(path);

    if (path.size() >= 2 &&
        path.front() == L'"' &&
        path.back() == L'"')
    {
        path =
            path.substr(
                1,
                path.size() - 2);
    }

    path = ExpandEnv(path);

    std::replace(
        path.begin(),
        path.end(),
        L'/',
        L'\\');

    return path;
}

// ============================================================
// Search roots
// ============================================================

static void AddSearchRoot(
    std::vector<std::wstring>& roots,
    const std::wstring& path)
{
    if (path.empty())
        return;

    std::wstring lower =
        Lower(path);

    for (const auto& x : roots)
    {
        if (Lower(x) == lower)
            return;
    }

    roots.push_back(path);
}

static void BuildSearchRoots(
    std::vector<std::wstring>& roots)
{
    static const KNOWNFOLDERID ids[] =
    {
        FOLDERID_Documents,
        FOLDERID_Desktop,
        FOLDERID_Downloads,
        FOLDERID_Pictures,
        FOLDERID_Music,
        FOLDERID_Videos
    };

    for (auto id : ids)
    {
        std::wstring p;

        if (KnownFolder(id, p))
            AddSearchRoot(
                roots,
                p);
    }
}

// ============================================================
// Bounded recursive search
// ============================================================

static bool SearchDirectory(
    const std::wstring& root,
    const std::wstring& wanted,
    std::wstring& result,
    int depth,
    int& inspected)
{
    if (depth > 5 ||
        inspected > 10000)
    {
        return false;
    }

    std::wstring pattern = root;

    if (!pattern.empty() &&
        pattern.back() != L'\\')
    {
        pattern += L'\\';
    }

    pattern += L"*";

    WIN32_FIND_DATAW fd{};

    HANDLE h =
        FindFirstFileW(
            pattern.c_str(),
            &fd);

    if (h == INVALID_HANDLE_VALUE)
        return false;

    std::wstring target =
        Lower(wanted);

    do
    {
        ++inspected;

        const wchar_t* name =
            fd.cFileName;

        if (!wcscmp(name, L".") ||
            !wcscmp(name, L".."))
        {
            continue;
        }

        std::wstring full = root;

        if (!full.empty() &&
            full.back() != L'\\')
        {
            full += L'\\';
        }

        full += name;

        if (!(fd.dwFileAttributes &
              FILE_ATTRIBUTE_DIRECTORY))
        {
            std::wstring filename =
                Lower(name);

            if (filename == target ||
                (filename.size() > target.size() &&
                 StartsWithI(
                     filename,
                     target) &&
                 filename[target.size()] == L'.'))
            {
                result = full;

                FindClose(h);
                return true;
            }
        }
    }
    while (FindNextFileW(h, &fd));

    FindClose(h);

    h =
        FindFirstFileW(
            pattern.c_str(),
            &fd);

    if (h == INVALID_HANDLE_VALUE)
        return false;

    do
    {
        const wchar_t* name =
            fd.cFileName;

        if (!wcscmp(name, L".") ||
            !wcscmp(name, L".."))
        {
            continue;
        }

        if (!(fd.dwFileAttributes &
              FILE_ATTRIBUTE_DIRECTORY))
        {
            continue;
        }

        if (fd.dwFileAttributes &
            FILE_ATTRIBUTE_REPARSE_POINT)
        {
            continue;
        }

        std::wstring full = root;

        if (!full.empty() &&
            full.back() != L'\\')
        {
            full += L'\\';
        }

        full += name;

        if (Lower(name) == target)
        {
            result = full;

            FindClose(h);
            return true;
        }

        if (SearchDirectory(
                full,
                wanted,
                result,
                depth + 1,
                inspected))
        {
            FindClose(h);
            return true;
        }
    }
    while (FindNextFileW(h, &fd));

    FindClose(h);

    return false;
}

static bool SearchKnownFolders(
    const std::wstring& wanted,
    std::wstring& result)
{
    std::vector<std::wstring> roots;

    BuildSearchRoots(roots);

    int inspected = 0;

    for (const auto& root : roots)
    {
        if (SearchDirectory(
                root,
                wanted,
                result,
                0,
                inspected))
        {
            return true;
        }

        if (inspected > 10000)
            break;
    }

    return false;
}

// ============================================================
// Command parsing
// ============================================================

struct ParsedCommand
{
    std::wstring file;
    std::wstring args;
};

static bool ParseCommand(
    const std::wstring& input,
    ParsedCommand& out)
{
    std::wstring s =
        Trim(input);

    if (s.empty())
        return false;

    int argc = 0;

    LPWSTR* argv =
        CommandLineToArgvW(
            s.c_str(),
            &argc);

    if (!argv ||
        argc <= 0)
    {
        return false;
    }

    out.file = argv[0];
    out.args.clear();

    for (int i = 1; i < argc; ++i)
    {
        if (!out.args.empty())
            out.args += L' ';

        std::wstring arg =
            argv[i];

        bool quote =
            arg.find_first_of(
                L" \t\"") !=
            std::wstring::npos;

        if (quote)
        {
            out.args += L'"';
            out.args += arg;
            out.args += L'"';
        }
        else
        {
            out.args += arg;
        }
    }

    LocalFree(argv);

    return !out.file.empty();
}

// ============================================================
// PATH
// ============================================================

static bool FindExecutableOnPath(
    const std::wstring& name,
    std::wstring& result)
{
    std::wstring candidate =
        name;

    if (!EndsWithI(candidate, L".exe") &&
        !EndsWithI(candidate, L".com") &&
        !EndsWithI(candidate, L".bat") &&
        !EndsWithI(candidate, L".cmd"))
    {
        candidate += L".exe";
    }

    wchar_t buffer[32768]{};

    DWORD n =
        SearchPathW(
            nullptr,
            candidate.c_str(),
            nullptr,
            32768,
            buffer,
            nullptr);

    if (n && n < 32768)
    {
        result.assign(
            buffer,
            n);

        return true;
    }

    return false;
}

// ============================================================
// Relative paths
// ============================================================

static bool ResolveRelative(
    const std::wstring& input,
    std::wstring& result)
{
    std::vector<std::wstring> roots;

    BuildSearchRoots(roots);

    for (const auto& root : roots)
    {
        std::wstring candidate =
            root;

        if (!candidate.empty() &&
            candidate.back() != L'\\')
        {
            candidate += L'\\';
        }

        candidate += input;

        if (FileExists(candidate) ||
            DirectoryExists(candidate))
        {
            result = candidate;
            return true;
        }
    }

    return false;
}

// ============================================================
// Launch
// ============================================================

static bool LaunchShell(
    const std::wstring& file,
    const std::wstring& args = L"",
    const std::wstring& directory = L"")
{
    HINSTANCE r =
        ShellExecuteW(
            nullptr,
            L"open",
            file.c_str(),
            args.empty()
                ? nullptr
                : args.c_str(),
            directory.empty()
                ? nullptr
                : directory.c_str(),
            SW_SHOWNORMAL);

    return
        (INT_PTR)r > 32;
}

enum class LaunchResult
{
    Success,
    NotFound,
    Error
};

// ============================================================
// Smart execution
// ============================================================

static LaunchResult ExecuteSmartInput(
    const std::wstring& raw)
{
    std::wstring input =
        Trim(raw);

    if (input.empty())
        return LaunchResult::NotFound;

    std::wstring lower =
        Lower(input);

    if (lower == L"run" ||
        lower == L"run...")
    {
        return LaunchResult::NotFound;
    }

    if (IsUrl(input))
    {
        return LaunchShell(input)
            ? LaunchResult::Success
            : LaunchResult::Error;
    }

    std::wstring special;

    if (ResolveSpecial(
            input,
            special))
    {
        return LaunchShell(special)
            ? LaunchResult::Success
            : LaunchResult::Error;
    }

    std::wstring expanded =
        ExpandEnv(input);

    if (expanded != input)
    {
        std::wstring normalized =
            NormalizePath(expanded);

        if (FileExists(normalized) ||
            DirectoryExists(normalized))
        {
            return LaunchShell(normalized)
                ? LaunchResult::Success
                : LaunchResult::Error;
        }

        if (IsUrl(normalized))
        {
            return LaunchShell(normalized)
                ? LaunchResult::Success
                : LaunchResult::Error;
        }
    }

    std::wstring aliasPath;

    if (ResolveAlias(
            input,
            aliasPath))
    {
        return LaunchShell(aliasPath)
            ? LaunchResult::Success
            : LaunchResult::Error;
    }

    ParsedCommand cmd;

    if (!ParseCommand(
            input,
            cmd))
    {
        return LaunchResult::NotFound;
    }

    std::wstring file =
        NormalizePath(cmd.file);

    if (FileExists(file) ||
        DirectoryExists(file))
    {
        return LaunchShell(
            file,
            cmd.args)
            ? LaunchResult::Success
            : LaunchResult::Error;
    }

    std::wstring relative;

    if (ResolveRelative(
            file,
            relative))
    {
        return LaunchShell(
            relative,
            cmd.args)
            ? LaunchResult::Success
            : LaunchResult::Error;
    }

    std::wstring exe;

    if (FindExecutableOnPath(
            file,
            exe))
    {
        if (LaunchShell(
                exe,
                cmd.args))
        {
            return LaunchResult::Success;
        }

        // Fall through: packaged-app execution alias stubs (e.g.
        // the Store version of mspaint) resolve to a real path
        // here but only actually launch when ShellExecute is
        // given their bare name, not this resolved path. The
        // bare-name fallback below picks those up.
    }

    if (cmd.args.empty())
    {
        std::wstring found;

        if (SearchKnownFolders(
                file,
                found))
        {
            return LaunchShell(found)
                ? LaunchResult::Success
                : LaunchResult::Error;
        }
    }

    if (EndsWithI(file, L".msc") ||
        EndsWithI(file, L".cpl") ||
        EndsWithI(file, L".lnk") ||
        EndsWithI(file, L".url"))
    {
        if (LaunchShell(
                file,
                cmd.args))
        {
            return LaunchResult::Success;
        }
    }

    // Last resort: hand the bare command straight to ShellExecute.
    // Unlike our own SearchPathW-based lookup above, ShellExecute
    // also consults the "App Paths" registry key, which is how
    // names like "mspaint" resolve from the real Run dialog even
    // though they're not on PATH. If it would run from Run, it
    // should run from here too.
    if (LaunchShell(
            file,
            cmd.args))
    {
        return LaunchResult::Success;
    }

    return LaunchResult::NotFound;
}

// ============================================================
// Native Windows Run
// ============================================================

static bool OpenNativeRun()
{
    if (g_start)
        ShowWindow(
            g_start,
            SW_HIDE);

    g_startVisible = false;
    ResetUIState();
    g_hover = -1;
    g_powerHover = -1;

    INPUT in[4]{};

    in[0].type =
        INPUT_KEYBOARD;
    in[0].ki.wVk =
        VK_LWIN;

    in[1].type =
        INPUT_KEYBOARD;
    in[1].ki.wVk =
        'R';

    in[2].type =
        INPUT_KEYBOARD;
    in[2].ki.wVk =
        'R';
    in[2].ki.dwFlags =
        KEYEVENTF_KEYUP;

    in[3].type =
        INPUT_KEYBOARD;
    in[3].ki.wVk =
        VK_LWIN;
    in[3].ki.dwFlags =
        KEYEVENTF_KEYUP;

    return SendInput(
        4,
        in,
        sizeof(INPUT)) == 4;
}

// ============================================================
// Search Enter
// ============================================================

// ============================================================
// GDI helpers
// ============================================================

static void FillRectColor(
    HDC dc,
    const RECT& r,
    COLORREF color)
{
    HBRUSH brush =
        CreateSolidBrush(color);

    if (!brush)
        return;

    FillRect(
        dc,
        &r,
        brush);

    DeleteObject(brush);
}

// Builds the same rounded-rect outline RoundRect() would, as a
// GDI+ path, so it can be filled/stroked with anti-aliasing. Note
// "radius" here is really the corner ellipse's diameter, matching
// the parameter GDI's RoundRect takes — kept under the same name
// so every existing call site's values still produce the exact
// same corner size as before.
static void AddRoundRectPath(
    Gdiplus::GraphicsPath& path,
    const RECT& r,
    int radius)
{
    int width =
        r.right - r.left;

    int height =
        r.bottom - r.top;

    int d = radius;

    if (d > width)
        d = width;

    if (d > height)
        d = height;

    if (d < 1)
    {
        path.AddRectangle(
            Gdiplus::Rect(
                r.left,
                r.top,
                width,
                height));

        return;
    }

    path.AddArc(
        r.left, r.top,
        d, d,
        180, 90);

    path.AddArc(
        r.right - d, r.top,
        d, d,
        270, 90);

    path.AddArc(
        r.right - d, r.bottom - d,
        d, d,
        0, 90);

    path.AddArc(
        r.left, r.bottom - d,
        d, d,
        90, 90);

    path.CloseFigure();
}

static void FillRoundRect(
    HDC dc,
    const RECT& r,
    int radius,
    COLORREF color,
    BYTE alpha = 255)
{
    Gdiplus::Graphics graphics(dc);

    graphics.SetSmoothingMode(
        Gdiplus::SmoothingModeAntiAlias);

    Gdiplus::GraphicsPath path;

    AddRoundRectPath(
        path,
        r,
        radius);

    Gdiplus::SolidBrush brush(
        Gdiplus::Color(
            alpha,
            GetRValue(color),
            GetGValue(color),
            GetBValue(color)));

    graphics.FillPath(
        &brush,
        &path);
}

static void DrawRoundBorder(
    HDC dc,
    const RECT& r,
    int radius,
    COLORREF color)
{
    Gdiplus::Graphics graphics(dc);

    graphics.SetSmoothingMode(
        Gdiplus::SmoothingModeAntiAlias);

    Gdiplus::GraphicsPath path;

    AddRoundRectPath(
        path,
        r,
        radius);

    Gdiplus::Pen pen(
        Gdiplus::Color(
            GetRValue(color),
            GetGValue(color),
            GetBValue(color)),
        1.0f);

    graphics.DrawPath(
        &pen,
        &path);
}

// The fill-then-outline pair every rounded panel in this UI is
// built from (menu rows, icon tiles, buttons, tooltips, the search
// box, the power flyout...) collapsed into one call.
static void DrawTile(
    HDC dc,
    const RECT& r,
    int radius,
    COLORREF fillColor,
    COLORREF borderColor,
    BYTE fillAlpha = 255)
{
    FillRoundRect(
        dc,
        r,
        radius,
        fillColor,
        fillAlpha);

    DrawRoundBorder(
        dc,
        r,
        radius,
        borderColor);
}

static void DrawTextSimple(
    HDC dc,
    const wchar_t* text,
    int x,
    int y,
    int width,
    int height,
    COLORREF color,
    HFONT font,
    UINT flags =
        DT_LEFT |
        DT_VCENTER |
        DT_SINGLELINE)
{
    if (!text || !font)
        return;

    SetBkMode(
        dc,
        TRANSPARENT);

    SetTextColor(
        dc,
        color);

    HGDIOBJ old =
        SelectObject(
            dc,
            font);

    RECT r =
    {
        x,
        y,
        x + width,
        y + height
    };

    DrawTextW(
        dc,
        text,
        -1,
        &r,
        flags);

    SelectObject(
        dc,
        old);
}

// ============================================================
// Fonts
// ============================================================

static void DestroyFonts()
{
    if (g_font)
    {
        DeleteObject(g_font);
        g_font = nullptr;
    }

    if (g_bold)
    {
        DeleteObject(g_bold);
        g_bold = nullptr;
    }

    if (g_small)
    {
        DeleteObject(g_small);
        g_small = nullptr;
    }

    if (g_icon)
    {
        DeleteObject(g_icon);
        g_icon = nullptr;
    }
}

// Windows 11 ships "Segoe UI Variable" (the font its own Start
// menu and Settings app use) and "Segoe Fluent Icons" (the current
// icon font), but older Windows 10 installs may only have the
// classic "Segoe UI" / "Segoe MDL2 Assets". Checked once via
// EnumFontFamiliesExW rather than assumed, so this degrades
// gracefully instead of silently substituting some unrelated font.
static bool IsFontInstalled(
    const wchar_t* faceName)
{
    HDC dc =
        GetDC(nullptr);

    if (!dc)
        return false;

    LOGFONTW lf{};

    lf.lfCharSet =
        DEFAULT_CHARSET;

    wcsncpy_s(
        lf.lfFaceName,
        faceName,
        LF_FACESIZE - 1);

    bool found = false;

    EnumFontFamiliesExW(
        dc,
        &lf,
        [](const LOGFONTW*,
           const TEXTMETRICW*,
           DWORD,
           LPARAM lParam) -> int
        {
            *reinterpret_cast<bool*>(
                lParam) = true;

            return 0;
        },
        reinterpret_cast<LPARAM>(
            &found),
        0);

    ReleaseDC(
        nullptr,
        dc);

    return found;
}

static const wchar_t* ResolveFontFace(
    const wchar_t* preferred,
    const wchar_t* fallback)
{
    return
        IsFontInstalled(preferred)
            ? preferred
            : fallback;
}

static void CreateFonts()
{
    DestroyFonts();

    const wchar_t* bodyFace =
        ResolveFontFace(
            L"Segoe UI Variable Text",
            L"Segoe UI");

    const wchar_t* boldFace =
        ResolveFontFace(
            L"Segoe UI Variable Text Semibold",
            L"Segoe UI Semibold");

    const wchar_t* iconFace =
        ResolveFontFace(
            L"Segoe Fluent Icons",
            L"Segoe MDL2 Assets");

    g_font =
        CreateFontW(
            -S(13),
            0,
            0,
            0,
            FW_NORMAL,
            FALSE,
            FALSE,
            FALSE,
            DEFAULT_CHARSET,
            OUT_DEFAULT_PRECIS,
            CLIP_DEFAULT_PRECIS,
            CLEARTYPE_QUALITY,
            DEFAULT_PITCH |
                FF_DONTCARE,
            bodyFace);

    // The bold face's weight is baked into the named instance
    // itself (Variable Text Semibold / Segoe UI Semibold) — asking
    // for FW_SEMIBOLD too would risk GDI synthetically emboldening
    // an already-semibold face on top of that.
    g_bold =
        CreateFontW(
            -S(15),
            0,
            0,
            0,
            FW_NORMAL,
            FALSE,
            FALSE,
            FALSE,
            DEFAULT_CHARSET,
            OUT_DEFAULT_PRECIS,
            CLIP_DEFAULT_PRECIS,
            CLEARTYPE_QUALITY,
            DEFAULT_PITCH |
                FF_DONTCARE,
            boldFace);

    g_small =
        CreateFontW(
            -S(10),
            0,
            0,
            0,
            FW_NORMAL,
            FALSE,
            FALSE,
            FALSE,
            DEFAULT_CHARSET,
            OUT_DEFAULT_PRECIS,
            CLIP_DEFAULT_PRECIS,
            CLEARTYPE_QUALITY,
            DEFAULT_PITCH |
                FF_DONTCARE,
            bodyFace);

    g_icon =
        CreateFontW(
            -S(16),
            0,
            0,
            0,
            FW_NORMAL,
            FALSE,
            FALSE,
            FALSE,
            DEFAULT_CHARSET,
            OUT_DEFAULT_PRECIS,
            CLIP_DEFAULT_PRECIS,
            CLEARTYPE_QUALITY,
            DEFAULT_PITCH |
                FF_DONTCARE,
            iconFace);
}

// ============================================================
// Work area
// ============================================================

static bool GetWorkArea(
    RECT& r)
{
    return SystemParametersInfoW(
        SPI_GETWORKAREA,
        0,
        &r,
        0) != FALSE;
}

// ============================================================
// Icons
// ============================================================

static void DrawFallbackIcon(
    HDC dc,
    int type,
    int x,
    int y,
    int size,
    bool hot)
{
    COLORREF color =
        hot
            ? g_accentText
            : g_accent;

    HPEN pen =
        CreatePen(
            PS_SOLID,
            S(1),
            color);

    if (!pen)
        return;

    HGDIOBJ oldPen =
        SelectObject(
            dc,
            pen);

    int cx =
        x + size / 2;

    int cy =
        y + size / 2;

    if (type == 0)
    {
        Rectangle(
            dc,
            cx - S(8),
            cy - S(8),
            cx - S(2),
            cy - S(2));

        Rectangle(
            dc,
            cx + S(2),
            cy - S(8),
            cx + S(8),
            cy - S(2));

        Rectangle(
            dc,
            cx - S(8),
            cy + S(2),
            cx - S(2),
            cy + S(8));

        Rectangle(
            dc,
            cx + S(2),
            cy + S(2),
            cx + S(8),
            cy + S(8));
    }
    else if (type == 1)
    {
        Rectangle(
            dc,
            cx - S(8),
            cy - S(10),
            cx + S(8),
            cy + S(10));

        MoveToEx(
            dc,
            cx - S(5),
            cy - S(3),
            nullptr);

        LineTo(
            dc,
            cx + S(5),
            cy - S(3));

        MoveToEx(
            dc,
            cx - S(5),
            cy + S(2),
            nullptr);

        LineTo(
            dc,
            cx + S(5),
            cy + S(2));
    }
    else if (type == 2)
    {
        MoveToEx(
            dc,
            cx,
            cy - S(9),
            nullptr);

        LineTo(
            dc,
            cx,
            cy + S(4));

        MoveToEx(
            dc,
            cx - S(6),
            cy - S(1),
            nullptr);

        LineTo(
            dc,
            cx,
            cy + S(5));

        LineTo(
            dc,
            cx + S(6),
            cy - S(1));

        MoveToEx(
            dc,
            cx - S(9),
            cy + S(9),
            nullptr);

        LineTo(
            dc,
            cx + S(9),
            cy + S(9));
    }
    else
    {
        Rectangle(
            dc,
            cx - S(8),
            cy - S(8),
            cx + S(8),
            cy + S(8));
    }

    SelectObject(
        dc,
        oldPen);

    DeleteObject(pen);
}

// The full set of menu items ClassicShell knows how to show. Which
// of these are actually visible is configurable (classicshell.ini,
// [MenuItems]) — g_itemCount is the runtime count of enabled ones,
// always <= MAX_ITEMS.
static const int MAX_ITEMS = 8;

static int g_itemCount = MAX_ITEMS;

// Icons for all MAX_ITEMS items, indexed the same way as ALL_ITEMS
// (i.e. by canonical item, not by visible position) — always fully
// populated by CreateIcons() regardless of visibility.
static HICON g_allIcons[MAX_ITEMS]{};

// Filtered down to just the enabled items, indexed by visible
// position (0..g_itemCount-1) — what painting/hit-testing use.
static HICON g_icons[MAX_ITEMS]{};

// User-configurable quick-launch tools, read from a plain text
// file (one command per line) that sits next to the exe.
struct QuickTool
{
    std::wstring command;
    std::wstring displayName;
    HICON icon;
};

static const int MAX_QUICK_TOOLS = 3;
static QuickTool g_quickTools[MAX_QUICK_TOOLS];
static int g_quickToolCount = 0;
static int g_quickToolHover = -1;

static void DestroyQuickToolIcons()
{
    for (int i = 0;
         i < g_quickToolCount;
         ++i)
    {
        if (g_quickTools[i].icon)
        {
            DestroyIcon(
                g_quickTools[i].icon);

            g_quickTools[i].icon = nullptr;
        }
    }

    g_quickToolCount = 0;
}

static void DestroyIcons()
{
    for (int i = 0;
         i < MAX_ITEMS;
         ++i)
    {
        if (g_allIcons[i])
        {
            DestroyIcon(
                g_allIcons[i]);

            g_allIcons[i] = nullptr;
        }

        g_icons[i] = nullptr;
    }

    DestroyQuickToolIcons();
}

static HICON GetStockIcon(
    SHSTOCKICONID id)
{
    SHSTOCKICONINFO info{};

    info.cbSize =
        sizeof(info);

    HRESULT hr =
        SHGetStockIconInfo(
            id,
            SHGSI_ICON |
                SHGSI_LARGEICON,
            &info);

    return SUCCEEDED(hr)
        ? info.hIcon
        : nullptr;
}

static HICON GetFileIcon(
    const wchar_t* path)
{
    if (!path)
        return nullptr;

    SHFILEINFOW sfi{};

    DWORD_PTR r =
        SHGetFileInfoW(
            path,
            0,
            &sfi,
            sizeof(sfi),
            SHGFI_ICON |
                SHGFI_LARGEICON);

    return r
        ? sfi.hIcon
        : nullptr;
}

// Resolves the real, on-disk icon for a known shell folder (which
// respects the desktop.ini icon overrides Windows applies to
// Documents/Downloads/Pictures/etc.), falling back to a generic
// stock icon only if the folder can't be resolved.
static HICON GetKnownFolderIcon(
    REFKNOWNFOLDERID id,
    SHSTOCKICONID fallbackStockId)
{
    std::wstring path;

    if (KnownFolder(
            id,
            path))
    {
        HICON icon =
            GetFileIcon(
                path.c_str());

        if (icon)
            return icon;
    }

    return GetStockIcon(
        fallbackStockId);
}

static HICON GetControlPanelIcon()
{
    wchar_t sys[MAX_PATH]{};

    UINT n =
        GetSystemDirectoryW(
            sys,
            MAX_PATH);

    if (n &&
        n < MAX_PATH)
    {
        std::wstring path =
            sys;

        path += L"\\control.exe";

        HICON icon =
            GetFileIcon(
                path.c_str());

        if (icon)
            return icon;
    }

    return GetStockIcon(
        SIID_APPLICATION);
}

// The classic "Run" icon (a small blue window), the same one used
// for handmade Run-command shortcuts and the one Windows itself
// has shown for Run since the 95-era Start menu.
static HICON GetRunIcon()
{
    wchar_t sys[MAX_PATH]{};

    UINT n =
        GetSystemDirectoryW(
            sys,
            MAX_PATH);

    if (n &&
        n < MAX_PATH)
    {
        std::wstring path =
            sys;

        path += L"\\shell32.dll";

        HICON large = nullptr;

        UINT extracted =
            ExtractIconExW(
                path.c_str(),
                24,
                &large,
                nullptr,
                1);

        if (extracted &&
            large)
        {
            return large;
        }
    }

    return GetStockIcon(
        SIID_APPLICATION);
}

// The classic yellow-folder File Explorer icon (explorer.exe's
// own icon, same one pinned to the taskbar) — used for Documents
// instead of the "known folder" icon, by request.
static HICON GetExplorerAppIcon()
{
    wchar_t win[MAX_PATH]{};

    UINT n =
        GetWindowsDirectoryW(
            win,
            MAX_PATH);

    if (n &&
        n < MAX_PATH)
    {
        std::wstring path =
            win;

        path += L"\\explorer.exe";

        HICON icon =
            GetFileIcon(
                path.c_str());

        if (icon)
            return icon;
    }

    return GetStockIcon(
        SIID_FOLDER);
}

static void CreateIcons()
{
    DestroyIcons();

    g_allIcons[0] =
        GetStockIcon(
            SIID_DESKTOPPC);

    g_allIcons[1] =
        GetStockIcon(
            SIID_APPLICATION);

    g_allIcons[2] =
        GetExplorerAppIcon();

    g_allIcons[3] =
        GetKnownFolderIcon(
            FOLDERID_Downloads,
            SIID_FOLDER);

    g_allIcons[4] =
        GetKnownFolderIcon(
            FOLDERID_Pictures,
            SIID_IMAGEFILES);

    g_allIcons[5] =
        GetKnownFolderIcon(
            FOLDERID_Music,
            SIID_FOLDER);

    g_allIcons[6] =
        GetControlPanelIcon();

    g_allIcons[7] =
        GetRunIcon();
}

// Packaged Windows apps (modern Paint, Snipping Tool, etc.) expose
// a bare "mspaint.exe"-style command as an execution-alias stub in
// WindowsApps — a zero-content reparse point with no icon resource
// of its own, so extracting an icon from it directly always yields
// a generic placeholder. The stub's reparse data itself, however,
// carries the real installed exe path (verified empirically: for
// mspaint this resolves to the actual Paint.exe under
// Program Files\WindowsApps, which has the real icon). This reads
// that raw reparse buffer and pulls it out.
static std::wstring ResolveAppExecLinkTarget(
    const std::wstring& path)
{
    HANDLE h =
        CreateFileW(
            path.c_str(),
            0,
            FILE_SHARE_READ |
                FILE_SHARE_WRITE |
                FILE_SHARE_DELETE,
            nullptr,
            OPEN_EXISTING,
            FILE_FLAG_OPEN_REPARSE_POINT |
                FILE_FLAG_BACKUP_SEMANTICS,
            nullptr);

    if (h == INVALID_HANDLE_VALUE)
        return L"";

    BYTE buffer[16384];
    DWORD bytesReturned = 0;

    BOOL ok =
        DeviceIoControl(
            h,
            FSCTL_GET_REPARSE_POINT,
            nullptr,
            0,
            buffer,
            sizeof(buffer),
            &bytesReturned,
            nullptr);

    CloseHandle(h);

    if (!ok ||
        bytesReturned < 12)
    {
        return L"";
    }

    ULONG tag =
        *(ULONG*)buffer;

    if (tag != IO_REPARSE_TAG_APPEXECLINK)
        return L"";

    ULONG stringCount =
        *(ULONG*)(buffer + 8);

    wchar_t* p =
        (wchar_t*)(buffer + 12);

    BYTE* end =
        buffer + bytesReturned;

    for (ULONG i = 0;
         i < stringCount &&
             (BYTE*)p < end;
         ++i)
    {
        size_t maxLen =
            (end - (BYTE*)p) /
            sizeof(wchar_t);

        size_t len =
            wcsnlen(
                p,
                maxLen);

        // Index 2 is the real target executable path.
        if (i == 2)
            return std::wstring(p, len);

        p += len + 1;
    }

    return L"";
}

// Pulls the app's real friendly name (e.g. "Paint" rather than
// "mspaint") out of its version resource, for the hover label.
// Falls back to empty if the file has no version info.
static std::wstring GetFileDescription(
    const std::wstring& path)
{
    DWORD handle = 0;

    DWORD size =
        GetFileVersionInfoSizeW(
            path.c_str(),
            &handle);

    if (!size)
        return L"";

    std::vector<BYTE> buffer(size);

    if (!GetFileVersionInfoW(
            path.c_str(),
            0,
            size,
            buffer.data()))
    {
        return L"";
    }

    struct LangCodepage
    {
        WORD language;
        WORD codepage;
    };

    LangCodepage* translations = nullptr;
    UINT translationsLen = 0;

    if (!VerQueryValueW(
            buffer.data(),
            L"\\VarFileInfo\\Translation",
            (void**)&translations,
            &translationsLen) ||
        !translations ||
        translationsLen < sizeof(LangCodepage))
    {
        return L"";
    }

    wchar_t subBlock[64];

    swprintf_s(
        subBlock,
        L"\\StringFileInfo\\%04x%04x\\FileDescription",
        translations[0].language,
        translations[0].codepage);

    wchar_t* description = nullptr;
    UINT descriptionLen = 0;

    if (!VerQueryValueW(
            buffer.data(),
            subBlock,
            (void**)&description,
            &descriptionLen) ||
        !description ||
        descriptionLen == 0)
    {
        return L"";
    }

    return std::wstring(description);
}

static std::wstring GetExePath()
{
    wchar_t path[MAX_PATH]{};

    GetModuleFileNameW(
        nullptr,
        path,
        MAX_PATH);

    return path;
}

// The folder the exe lives in, trailing backslash included — where
// all of ClassicShell's plain-text config files are expected.
static std::wstring GetExeDirectory()
{
    std::wstring path =
        GetExePath();

    size_t slash =
        path.find_last_of(L'\\');

    if (slash != std::wstring::npos)
        return path.substr(0, slash + 1);

    return path;
}

// ============================================================
// Configuration (classicshell.ini)
// ============================================================
//
// One old-school INI file next to the exe holds every user-facing
// setting. If it isn't there on startup, a fully-commented default
// copy is written out first, so there's always something present
// to find and hand-edit — nothing to configure by trial and error.

static const wchar_t CONFIG_FILE_NAME[] = L"classicshell.ini";

static const char DEFAULT_CONFIG_CONTENT[] =
"; ClassicShell configuration.\r\n"
"; Edit this file to customize ClassicShell, then relaunch it (or\r\n"
"; just reopen the menu, for settings that refresh live) to pick\r\n"
"; up changes. Delete this file to regenerate these defaults.\r\n"
"\r\n"
"[Appearance]\r\n"
"; Overall UI scale, applied on top of normal DPI scaling.\r\n"
"; 1.0 = original full size. Valid range: 0.3 - 2.0.\r\n"
"Scale=0.85\r\n"
"\r\n"
"; Window opacity when ClassicShell opens, 90-255. Adjustable live\r\n"
"; with the slider next to the power button; this is just the\r\n"
"; starting point for a new session.\r\n"
"StartOpacity=225\r\n"
"\r\n"
"[QuickTools]\r\n"
"; Up to 3 pinned quick-launch buttons, shown next to the opacity\r\n"
"; slider. One executable name per entry (e.g. calc.exe). Leave\r\n"
"; an entry blank for no button in that slot.\r\n"
"Tool1=\r\n"
"Tool2=\r\n"
"Tool3=\r\n"
"\r\n"
"[MenuItems]\r\n"
"; Every row ClassicShell can show, in order. Set any of these to\r\n"
"; 0 to hide that row; all default to 1 (shown). If every item ends\r\n"
"; up disabled, all of them show anyway rather than leaving an\r\n"
"; empty menu.\r\n"
"ThisPC=1\r\n"
"Programs=1\r\n"
"Documents=1\r\n"
"Downloads=1\r\n"
"Pictures=1\r\n"
"Music=1\r\n"
"ControlPanel=1\r\n"
"Run=1\r\n"
"\r\n"
"[Experimental]\r\n"
"; Off by default: the physical Windows key is only ever observed,\r\n"
"; never intercepted (see README/SPEC.md for why), so by default a\r\n"
"; Windows-key tap opens the normal legacy/native Start menu, not\r\n"
"; this one. Set to 1 to have a standalone Windows-key tap open\r\n"
"; ClassicShell's own menu instead (experimental: being hardened\r\n"
"; over time). The taskbar Start button and Ctrl+Esc always open\r\n"
"; ClassicShell's menu regardless of this setting.\r\n"
"CaptureWinKey=0\r\n"
"\r\n"
"[Search]\r\n"
"; Folder ClassicShell indexes in the background at startup so the\r\n"
"; search box can find files by name. Type a wildcard pattern like\r\n"
"; *.txt or report.* for an exact match, or just plain text like\r\n"
"; report to find any file containing it. Blank = your user profile\r\n"
"; folder. Indexing runs on a low-priority background thread and\r\n"
"; never blocks the UI, however long the folder takes to walk.\r\n"
"IndexPath=\r\n";

static std::wstring GetConfigPath()
{
    return
        GetExeDirectory() +
        CONFIG_FILE_NAME;
}

static std::wstring GetProfileDirectory()
{
    wchar_t path[MAX_PATH]{};
    DWORD len = GetEnvironmentVariableW(L"USERPROFILE", path, MAX_PATH);

    if (len == 0 || len >= MAX_PATH)
        return L"";

    return path;
}

// Writes the default config file iff nothing is there yet — never
// overwrites a file the user has already customized.
static void EnsureConfigFile()
{
    std::wstring path =
        GetConfigPath();

    if (FileExists(path))
        return;

    std::ofstream file(
        path,
        std::ios::binary);

    if (!file.is_open())
        return;

    file.write(
        DEFAULT_CONFIG_CONTENT,
        sizeof(DEFAULT_CONFIG_CONTENT) - 1);
}

// Reads classicshell.ini's [QuickTools] section. Called both at
// startup and every time the menu opens, so edits to the file show
// up without restarting the app.
static void LoadQuickTools()
{
    DestroyQuickToolIcons();

    std::wstring configPath =
        GetConfigPath();

    wchar_t key[16];
    wchar_t value[MAX_PATH];

    for (int slot = 1;
         slot <= MAX_QUICK_TOOLS;
         ++slot)
    {
        swprintf(
            key,
            16,
            L"Tool%d",
            slot);

        GetPrivateProfileStringW(
            L"QuickTools",
            key,
            L"",
            value,
            MAX_PATH,
            configPath.c_str());

        std::wstring trimmed =
            Trim(value);

        if (trimmed.empty())
            continue;

        std::wstring resolved;

        HICON icon = nullptr;
        std::wstring bestPath;

        if (FindExecutableOnPath(
                trimmed,
                resolved))
        {
            std::wstring target =
                ResolveAppExecLinkTarget(
                    resolved);

            if (!target.empty() &&
                FileExists(target))
            {
                bestPath = target;
            }
            else
            {
                bestPath = resolved;
            }

            icon =
                GetFileIcon(
                    bestPath.c_str());
        }

        if (!icon)
            icon =
                GetStockIcon(
                    SIID_APPLICATION);

        std::wstring displayName =
            bestPath.empty()
                ? L""
                : GetFileDescription(
                      bestPath);

        if (displayName.empty())
            displayName = trimmed;

        g_quickTools[g_quickToolCount]
            .command = trimmed;

        g_quickTools[g_quickToolCount]
            .displayName = displayName;

        g_quickTools[g_quickToolCount]
            .icon = icon;

        g_quickToolCount++;
    }
}

// Reads classicshell.ini's [Appearance] section: scale and start
// opacity. Called once at startup, before fonts/icons are created
// (they're sized from g_userScale) — quick tools load separately,
// after CreateIcons(), since it clears quick-tool icons as part of
// its own cleanup. Also unlike LoadQuickTools(), scale and opacity
// intentionally don't re-apply on every menu open, so a live
// opacity-slider adjustment isn't stomped the next time the user
// opens the menu.
static void LoadConfig()
{
    EnsureConfigFile();

    std::wstring configPath =
        GetConfigPath();

    wchar_t scaleText[32];

    GetPrivateProfileStringW(
        L"Appearance",
        L"Scale",
        L"0.85",
        scaleText,
        32,
        configPath.c_str());

    float scale =
        (float)_wtof(scaleText);

    g_userScale =
        (scale >= 0.3f &&
         scale <= 2.0f)
            ? scale
            : 0.85f;

    int opacity =
        GetPrivateProfileIntW(
            L"Appearance",
            L"StartOpacity",
            START_WINDOW_ALPHA,
            configPath.c_str());

    if (opacity < OPACITY_MIN)
        opacity = OPACITY_MIN;

    if (opacity > OPACITY_MAX)
        opacity = OPACITY_MAX;

    g_windowAlpha = (BYTE)opacity;

    g_captureWinKey =
        GetPrivateProfileIntW(
            L"Experimental",
            L"CaptureWinKey",
            0,
            configPath.c_str()) != 0;

    wchar_t indexPath[MAX_PATH];

    GetPrivateProfileStringW(
        L"Search",
        L"IndexPath",
        L"",
        indexPath,
        MAX_PATH,
        configPath.c_str());

    g_searchIndexRoot = Trim(indexPath);

    if (g_searchIndexRoot.empty())
        g_searchIndexRoot = GetProfileDirectory();
}

static void DrawRealIcon(
    HDC dc,
    HICON icon,
    int x,
    int y,
    int size)
{
    if (!icon)
        return;

    DrawIconEx(
        dc,
        x + S(3),
        y + S(3),
        icon,
        size - S(6),
        size - S(6),
        0,
        nullptr,
        DI_NORMAL);
}

// ============================================================
// Menu
// ============================================================

struct MenuItem
{
    const wchar_t* label;
    const wchar_t* command;

    // Key under classicshell.ini's [MenuItems] section that toggles
    // this item on/off.
    const wchar_t* iniKey;
};

// Every item ClassicShell can show. Order here is the default
// order; BuildVisibleItems() filters this down to g_items based on
// classicshell.ini.
static const MenuItem ALL_ITEMS[MAX_ITEMS] =
{
    { L"This PC",       L"this pc",       L"ThisPC" },
    { L"Programs",      L"",              L"Programs" },
    { L"Documents",     L"documents",     L"Documents" },
    { L"Downloads",     L"downloads",     L"Downloads" },
    { L"Pictures",      L"pictures",      L"Pictures" },
    { L"Music",         L"music",         L"Music" },
    { L"Control Panel", L"control panel", L"ControlPanel" },
    { L"Run...",        L"run",           L"Run" }
};

// Filtered down to just the enabled items, in the same order —
// what painting/hit-testing/activation actually iterate over.
static MenuItem g_items[MAX_ITEMS];

// Reads classicshell.ini's [MenuItems] section (one on/off toggle
// per entry in ALL_ITEMS, defaulting to on) and rebuilds g_items /
// g_icons / g_itemCount to match. Must run after CreateIcons(), so
// g_allIcons is already populated. If every item ends up disabled
// (e.g. a typo'd ini), falls back to showing all of them rather
// than leaving an empty, useless menu.
static void BuildVisibleItems()
{
    std::wstring configPath =
        GetConfigPath();

    g_itemCount = 0;

    for (int i = 0;
         i < MAX_ITEMS;
         ++i)
    {
        int enabled =
            GetPrivateProfileIntW(
                L"MenuItems",
                ALL_ITEMS[i].iniKey,
                1,
                configPath.c_str());

        if (!enabled)
            continue;

        g_items[g_itemCount] =
            ALL_ITEMS[i];

        g_icons[g_itemCount] =
            g_allIcons[i];

        g_itemCount++;
    }

    if (g_itemCount == 0)
    {
        for (int i = 0;
             i < MAX_ITEMS;
             ++i)
        {
            g_items[i] = ALL_ITEMS[i];
            g_icons[i] = g_allIcons[i];
        }

        g_itemCount = MAX_ITEMS;
    }
}

// Forward declarations for functions defined later in the file but
// needed by the search/God Mode/panel code below, which sits earlier
// than they do.
static void CloseStart();
static void ApplyWindowRounding(HWND hwnd);
static void ApplyAcrylicBlur(HWND hwnd);
static int GetFocusedSearchResultIndex();
static void ResizeStartToContent(HWND hwnd);
static void ShowPreviewForResult(int index);
static void CancelPreviewFadeOut();
static void BeginPreviewFadeOut();
static void HidePreview();
static void CancelPendingBlurClose();

// Hover-preview state needed by the search panel's own WndProc (defined
// below), even though the rest of the preview feature is implemented
// later in the file, after PreviewKind/etc. exist.
static HWND g_preview = nullptr;
static int g_previewHoverIndex = -1;
static UINT_PTR g_previewShowTimer = 0;
static const UINT_PTR TIMER_PREVIEW_HOVER = 5;

// Grace-period state for DismissNativeStartMenuIfNeeded's blur-close
// (declared here, not next to that function, since StartProc's WM_TIMER
// handling — also defined earlier in the file than that function — needs
// it too).
static UINT_PTR g_blurCloseTimer = 0;
static const UINT_PTR TIMER_BLUR_CLOSE = 10;

// ============================================================
// File search — background index + query matching
// ============================================================

struct IndexedFile
{
    std::wstring fullPath;
    std::wstring fileName;
    std::wstring fileNameLower;
};

static const size_t MAX_INDEX_FILES = 200000;

static std::vector<IndexedFile> g_fileIndex;
static std::mutex g_fileIndexMutex;
static std::atomic<size_t> g_indexedFileCount{ 0 };

static void IndexDirectoryRecursive(
    const std::wstring& dir,
    std::vector<IndexedFile>& pending)
{
    if (g_indexedFileCount.load() + pending.size() >= MAX_INDEX_FILES)
        return;

    std::wstring pattern = dir + L"\\*";
    WIN32_FIND_DATAW data{};
    HANDLE find = FindFirstFileW(pattern.c_str(), &data);

    if (find == INVALID_HANDLE_VALUE)
        return;

    do
    {
        std::wstring name = data.cFileName;

        if (name == L"." || name == L"..")
            continue;

        // Skip reparse points (junctions/symlinks) to avoid cycles.
        if (data.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)
            continue;

        std::wstring fullPath = dir + L"\\" + name;

        if (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
        {
            IndexDirectoryRecursive(fullPath, pending);
        }
        else
        {
            IndexedFile entry;
            entry.fullPath = fullPath;
            entry.fileName = name;
            entry.fileNameLower = Lower(name);
            pending.push_back(std::move(entry));

            if (pending.size() >= 2000)
            {
                std::lock_guard<std::mutex> lock(g_fileIndexMutex);

                for (auto& e : pending)
                    g_fileIndex.push_back(std::move(e));

                pending.clear();
                g_indexedFileCount.store(g_fileIndex.size());
            }
        }

        if (g_indexedFileCount.load() + pending.size() >= MAX_INDEX_FILES)
            break;
    }
    while (FindNextFileW(find, &data));

    FindClose(find);
}

static DWORD WINAPI IndexThreadProc(LPVOID)
{
    if (g_searchIndexRoot.empty())
        return 0;

    std::vector<IndexedFile> pending;
    IndexDirectoryRecursive(g_searchIndexRoot, pending);

    if (!pending.empty())
    {
        std::lock_guard<std::mutex> lock(g_fileIndexMutex);

        for (auto& e : pending)
            g_fileIndex.push_back(std::move(e));

        g_indexedFileCount.store(g_fileIndex.size());
    }

    return 0;
}

// Started once at startup, never re-triggered. Runs at the lowest thread
// priority so it never competes with UI responsiveness, however long the
// configured folder takes to walk.
static void StartBackgroundIndexing()
{
    HANDLE thread =
        CreateThread(
            nullptr, 0, IndexThreadProc, nullptr,
            CREATE_SUSPENDED, nullptr);

    if (!thread)
        return;

    SetThreadPriority(thread, THREAD_PRIORITY_LOWEST);
    ResumeThread(thread);
    CloseHandle(thread);
}

// ============================================================
// Control Panel "God Mode" — every Control Panel item as one flat,
// searchable/browsable list, via the well-known shell CLSID trick.
// ============================================================

struct GodModeItem
{
    std::wstring name;
    std::wstring nameLower;
    LPITEMIDLIST pidl = nullptr; // owned; never freed per-use, catalog lifetime.
    HICON icon = nullptr;        // catalog-owned; never destroyed per-use.
};

static std::vector<GodModeItem> g_godModeItems;
static std::mutex g_godModeMutex;

// A bare "::{CLSID}" display name is rejected by SHParseDisplayName
// directly (confirmed E_INVALIDARG); binding instead to a real, empty,
// hidden-named folder whose name *ends* in the CLSID is the documented
// workaround for exposing the "all Control Panel tasks" virtual folder.
static std::wstring GetGodModeFolderPath()
{
    wchar_t tempPath[MAX_PATH]{};
    DWORD len = GetTempPathW(MAX_PATH, tempPath);

    if (len == 0 || len >= MAX_PATH)
        return L"";

    return
        std::wstring(tempPath) +
        L"ClassicShellGodMode.{ED7BA470-8E54-465E-825C-99712043E01C}";
}

static DWORD WINAPI GodModeThreadProc(LPVOID)
{
    HRESULT com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

    std::wstring folder = GetGodModeFolderPath();

    if (!folder.empty())
        CreateDirectoryW(folder.c_str(), nullptr);

    PIDLIST_ABSOLUTE rootPidl = nullptr;

    if (folder.empty() ||
        FAILED(SHParseDisplayName(folder.c_str(), nullptr, &rootPidl, 0, nullptr)) ||
        !rootPidl)
    {
        if (SUCCEEDED(com)) CoUninitialize();
        return 0;
    }

    IShellFolder* desktop = nullptr;

    if (FAILED(SHGetDesktopFolder(&desktop)) || !desktop)
    {
        CoTaskMemFree(rootPidl);
        if (SUCCEEDED(com)) CoUninitialize();
        return 0;
    }

    IShellFolder* rootFolder = nullptr;
    HRESULT hr = desktop->BindToObject(rootPidl, nullptr, IID_PPV_ARGS(&rootFolder));

    if (FAILED(hr) || !rootFolder)
    {
        desktop->Release();
        CoTaskMemFree(rootPidl);
        if (SUCCEEDED(com)) CoUninitialize();
        return 0;
    }

    IEnumIDList* enumIds = nullptr;

    hr =
        rootFolder->EnumObjects(
            nullptr,
            SHCONTF_FOLDERS | SHCONTF_NONFOLDERS | SHCONTF_INCLUDEHIDDEN,
            &enumIds);

    std::vector<GodModeItem> items;

    if (SUCCEEDED(hr) && enumIds)
    {
        LPITEMIDLIST childPidl = nullptr;
        ULONG fetched = 0;

        while (enumIds->Next(1, &childPidl, &fetched) == S_OK && fetched == 1)
        {
            STRRET strret{};
            std::wstring name;

            if (SUCCEEDED(rootFolder->GetDisplayNameOf(childPidl, SHGDN_NORMAL, &strret)))
            {
                wchar_t buf[512]{};

                if (SUCCEEDED(StrRetToBufW(&strret, childPidl, buf, 512)))
                    name = buf;
            }

            LPITEMIDLIST absolutePidl =
                name.empty() ? nullptr : ILCombine(rootPidl, childPidl);

            if (absolutePidl)
            {
                GodModeItem item;
                item.name = name;
                item.nameLower = Lower(name);
                item.pidl = absolutePidl;

                SHFILEINFOW info{};

                if (SHGetFileInfoW(
                        (LPCWSTR)absolutePidl,
                        0,
                        &info,
                        sizeof(info),
                        SHGFI_PIDL | SHGFI_ICON | SHGFI_LARGEICON))
                {
                    item.icon = info.hIcon;
                }

                items.push_back(item);
            }

            CoTaskMemFree(childPidl);
        }

        enumIds->Release();
    }

    rootFolder->Release();
    desktop->Release();
    CoTaskMemFree(rootPidl);

    std::sort(
        items.begin(), items.end(),
        [](const GodModeItem& a, const GodModeItem& b)
        {
            return _wcsicmp(a.name.c_str(), b.name.c_str()) < 0;
        });

    {
        std::lock_guard<std::mutex> lock(g_godModeMutex);
        g_godModeItems = std::move(items);
    }

    if (SUCCEEDED(com))
        CoUninitialize();

    return 0;
}

static void StartGodModeIndexing()
{
    HANDLE thread =
        CreateThread(
            nullptr, 0, GodModeThreadProc, nullptr,
            CREATE_SUSPENDED, nullptr);

    if (!thread)
        return;

    SetThreadPriority(thread, THREAD_PRIORITY_LOWEST);
    ResumeThread(thread);
    CloseHandle(thread);
}

// Invokes the PIDL's default verb, exactly as a double-click would —
// there's no ".cpl" command string to shell out to for these tasks.
static void LaunchGodModeItem(const GodModeItem& item)
{
    if (!item.pidl)
        return;

    SHELLEXECUTEINFOW info{};
    info.cbSize = sizeof(info);
    info.fMask = SEE_MASK_INVOKEIDLIST;
    info.lpIDList = item.pidl;
    info.nShow = SW_SHOWNORMAL;

    ShellExecuteExW(&info);
}

// ============================================================
// Search results (files + God Mode Control Panel items)
// ============================================================

enum class SearchResultKind
{
    File,
    GodMode
};

struct SearchResultEntry
{
    SearchResultKind kind = SearchResultKind::File;
    size_t fileIndex = 0;
    size_t godModeIndex = 0;

    // Snapshotted at match time (not re-fetched per paint): the display
    // name and, for File results, a freshly-extracted icon this entry
    // owns and must destroy; GodMode icons are catalog-owned and never
    // destroyed here.
    std::wstring displayName;
    HICON icon = nullptr;
};

static const size_t MAX_SEARCH_RESULTS = 50;
static const size_t MAX_GODMODE_SEARCH_MATCHES = 6;

static std::vector<SearchResultEntry> g_searchResults;
static int g_searchResultHover = -1;
static int g_searchResultsScroll = 0;
static bool g_searchScrollbarDragging = false;
static int g_searchScrollbarDragGrabOffset = 0;
static bool g_controlPanelBrowsing = false;

// Defined later, near the search panel window itself — forward declared
// so the result-building functions above can show/hide/resize it.
static void RepositionSearchPanel();

static void ClearSearchResults()
{
    for (auto& entry : g_searchResults)
    {
        if (entry.kind == SearchResultKind::File && entry.icon)
            DestroyIcon(entry.icon);
    }

    g_searchResults.clear();
    g_searchResultHover = -1;
    g_searchResultsScroll = 0;
    g_controlPanelBrowsing = false;
}

// Recomputes g_searchResults from the current g_searchText. For an
// implicit-contains query (plain text, not a wildcard) God Mode items are
// matched first and sorted ahead of file results, capped at
// MAX_GODMODE_SEARCH_MATCHES — a wildcard pattern doesn't mean anything
// against a Control Panel task's name, so wildcard queries never match
// God Mode items. No relevance scoring beyond that: first-match-wins, in
// index order, up to MAX_SEARCH_RESULTS total.
static void RefreshSearchResultsImpl()
{
    ClearSearchResults();

    std::wstring query = Trim(g_searchText);

    if (query.empty())
    {
        RepositionSearchPanel();
        return;
    }

    bool wildcard = IsWildcardQuery(query);
    std::wstring queryLower = Lower(query);

    if (!wildcard)
    {
        std::lock_guard<std::mutex> lock(g_godModeMutex);

        for (size_t i = 0;
             i < g_godModeItems.size() &&
                 g_searchResults.size() < MAX_GODMODE_SEARCH_MATCHES;
             ++i)
        {
            if (g_godModeItems[i].nameLower.find(queryLower) == std::wstring::npos)
                continue;

            SearchResultEntry entry;
            entry.kind = SearchResultKind::GodMode;
            entry.godModeIndex = i;
            entry.displayName = g_godModeItems[i].name;
            entry.icon = g_godModeItems[i].icon;
            g_searchResults.push_back(entry);
        }
    }

    {
        std::lock_guard<std::mutex> lock(g_fileIndexMutex);

        for (size_t i = 0;
             i < g_fileIndex.size() &&
                 g_searchResults.size() < MAX_SEARCH_RESULTS;
             ++i)
        {
            const IndexedFile& file = g_fileIndex[i];

            if (!MatchesSearchQuery(file.fileName, file.fileNameLower, query, queryLower))
                continue;

            SearchResultEntry entry;
            entry.kind = SearchResultKind::File;
            entry.fileIndex = i;
            entry.displayName = file.fileName;
            entry.icon = GetFileIcon(file.fullPath.c_str());
            g_searchResults.push_back(entry);
        }
    }

    RepositionSearchPanel();
}

// Recomputes g_searchResults on every keystroke. Wrapped defensively for
// the same reason ShowPreviewForResult is: MatchesSearchQuery and the
// icon/file-index lookups it drives run against real filesystem content
// this app doesn't control, on the hottest input path in the app — an
// exception escaping here would crash the whole process on literally
// every keystroke until the offending character was deleted.
static void RefreshSearchResults()
{
    try
    {
        RefreshSearchResultsImpl();
    }
    catch (...)
    {
        ClearSearchResults();
        RepositionSearchPanel();
    }
}

static void LaunchSearchResult(size_t index)
{
    if (index >= g_searchResults.size())
        return;

    const SearchResultEntry entry = g_searchResults[index];

    if (entry.kind == SearchResultKind::GodMode)
    {
        std::lock_guard<std::mutex> lock(g_godModeMutex);

        if (entry.godModeIndex < g_godModeItems.size())
            LaunchGodModeItem(g_godModeItems[entry.godModeIndex]);
    }
    else
    {
        std::wstring path;

        {
            std::lock_guard<std::mutex> lock(g_fileIndexMutex);

            if (entry.fileIndex < g_fileIndex.size())
                path = g_fileIndex[entry.fileIndex].fullPath;
        }

        if (path.empty())
            return;

        HINSTANCE result =
            ShellExecuteW(
                nullptr, L"open", path.c_str(),
                nullptr, nullptr, SW_SHOWNORMAL);

        if ((INT_PTR)result <= 32)
            return;
    }

    g_searchText.clear();
    g_searchSelection.Reset();
    ClearSearchResults();
    RepositionSearchPanel();
    CloseStart();
}

// Right-click on a search result: reveal it in Explorer instead of
// launching it. A no-op for God Mode Control Panel items — there's no
// real file/folder location to show for one of those.
static void OpenSearchResultFolder(size_t index)
{
    if (index >= g_searchResults.size())
        return;

    const SearchResultEntry& entry = g_searchResults[index];

    if (entry.kind != SearchResultKind::File)
        return;

    std::wstring path;

    {
        std::lock_guard<std::mutex> lock(g_fileIndexMutex);

        if (entry.fileIndex < g_fileIndex.size())
            path = g_fileIndex[entry.fileIndex].fullPath;
    }

    if (path.empty())
        return;

    std::wstring params = L"/select,\"" + path + L"\"";

    ShellExecuteW(
        nullptr, L"open", L"explorer.exe",
        params.c_str(), nullptr, SW_SHOWNORMAL);

    g_searchText.clear();
    g_searchSelection.Reset();
    ClearSearchResults();
    RepositionSearchPanel();
    CloseStart();
}

// The "Control Panel" menu row toggles a full listing of every God Mode
// item into the results panel — a disclosure, not a launcher.
static void ToggleControlPanelBrowse()
{
    bool wasBrowsing = g_controlPanelBrowsing;

    ClearSearchResults();

    if (wasBrowsing)
    {
        RepositionSearchPanel();
        return;
    }

    std::lock_guard<std::mutex> lock(g_godModeMutex);

    for (size_t i = 0; i < g_godModeItems.size(); ++i)
    {
        SearchResultEntry entry;
        entry.kind = SearchResultKind::GodMode;
        entry.godModeIndex = i;
        entry.displayName = g_godModeItems[i].name;
        entry.icon = g_godModeItems[i].icon;
        g_searchResults.push_back(entry);
    }

    g_controlPanelBrowsing = true;

    RepositionSearchPanel();
}

// ============================================================
// Search box editing — shared by KeyboardProc (the guaranteed path) and
// StartProc's own WM_KEYDOWN/WM_CHAR, so caret/selection behavior can't
// drift between the two input paths the way the old append-only/
// pop_back-only logic was duplicated in both without a caret concept.
// ============================================================

static void InsertSearchChar(
    HWND hwnd,
    wchar_t c)
{
    bool wasEmpty = g_searchText.empty();

    if (g_searchSelection.HasSelection())
    {
        int start = g_searchSelection.SelectionStart();
        int end = g_searchSelection.SelectionEnd();

        g_searchText.erase(start, end - start);
        g_searchSelection.PlaceCaret(start);
    }

    int pos = g_searchSelection.Caret();

    g_searchText.insert((size_t)pos, 1, c);
    g_searchSelection.PlaceCaret(pos + 1);

    g_caretVisible = true;

    RefreshSearchResults();

    if (wasEmpty)
        ResizeStartToContent(hwnd);

    InvalidateRect(hwnd, nullptr, FALSE);
}

static void DeleteSearchSelectionOrCharBefore(
    HWND hwnd)
{
    if (g_searchSelection.HasSelection())
    {
        int start = g_searchSelection.SelectionStart();
        int end = g_searchSelection.SelectionEnd();

        g_searchText.erase(start, end - start);
        g_searchSelection.PlaceCaret(start);
    }
    else
    {
        int caret = g_searchSelection.Caret();

        if (caret <= 0)
            return;

        g_searchText.erase((size_t)caret - 1, 1);
        g_searchSelection.PlaceCaret(caret - 1);
    }

    g_caretVisible = true;

    RefreshSearchResults();

    if (g_searchText.empty())
        ResizeStartToContent(hwnd);

    InvalidateRect(hwnd, nullptr, FALSE);
}

static void DeleteSearchSelectionOrCharAfter(
    HWND hwnd)
{
    if (g_searchSelection.HasSelection())
    {
        int start = g_searchSelection.SelectionStart();
        int end = g_searchSelection.SelectionEnd();

        g_searchText.erase(start, end - start);
        g_searchSelection.PlaceCaret(start);
    }
    else
    {
        int caret = g_searchSelection.Caret();

        if (caret >= (int)g_searchText.size())
            return;

        g_searchText.erase((size_t)caret, 1);
    }

    g_caretVisible = true;

    RefreshSearchResults();

    if (g_searchText.empty())
        ResizeStartToContent(hwnd);

    InvalidateRect(hwnd, nullptr, FALSE);
}

static void CopySearchSelectionToClipboard(
    HWND hwnd)
{
    if (!g_searchSelection.HasSelection())
        return;

    int start = g_searchSelection.SelectionStart();
    int end = g_searchSelection.SelectionEnd();

    std::wstring selected = g_searchText.substr(start, end - start);

    if (!OpenClipboard(hwnd))
        return;

    EmptyClipboard();

    size_t bytes = (selected.size() + 1) * sizeof(wchar_t);
    HGLOBAL mem = GlobalAlloc(GMEM_MOVEABLE, bytes);

    if (mem)
    {
        void* ptr = GlobalLock(mem);

        if (ptr)
        {
            memcpy(ptr, selected.c_str(), bytes);
            GlobalUnlock(mem);
            SetClipboardData(CF_UNICODETEXT, mem);
        }
        else
        {
            GlobalFree(mem);
        }
    }

    CloseClipboard();
}

// Caret movement / selection / select-all / copy, shared by both input
// paths. Returns true if the key was handled (caller should swallow it).
// Does not handle character insertion, Backspace, or Delete — those stay
// as separate calls at each call site since the two paths already differ
// slightly in how they detect "should the search box handle this at
// all" before reaching here.
static bool HandleSearchBoxNavigationKey(
    HWND hwnd,
    DWORD vk,
    bool ctrl,
    bool shift)
{
    int textLength = (int)g_searchText.size();

    switch (vk)
    {
        case VK_LEFT:
            g_searchSelection.MoveLeft(shift);
            g_caretVisible = true;
            InvalidateRect(hwnd, nullptr, FALSE);
            return true;

        case VK_RIGHT:
            g_searchSelection.MoveRight(shift, textLength);
            g_caretVisible = true;
            InvalidateRect(hwnd, nullptr, FALSE);
            return true;

        case VK_HOME:
            g_searchSelection.MoveHome(shift);
            g_caretVisible = true;
            InvalidateRect(hwnd, nullptr, FALSE);
            return true;

        case VK_END:
            g_searchSelection.MoveEnd(shift, textLength);
            g_caretVisible = true;
            InvalidateRect(hwnd, nullptr, FALSE);
            return true;

        case 'A':
            if (ctrl)
            {
                g_searchSelection.SelectAll(textLength);
                InvalidateRect(hwnd, nullptr, FALSE);
                return true;
            }
            return false;

        case 'C':
            if (ctrl)
            {
                CopySearchSelectionToClipboard(hwnd);
                return true;
            }
            return false;
    }

    return false;
}

// Places the caret at the character offset nearest screen point `pt`,
// within the search box whose text starts at screen x `textLeft`.
// extend=true grows a selection (mouse drag); false starts a fresh one
// (mouse down).
// Character index into g_searchText nearest screen x `pointX`, given the
// text's own screen x origin `textLeft`. Shared by click-to-place-caret,
// drag-to-select, and double-click-to-select-word, so all three agree on
// exactly where in the text a given pixel maps to.
static int SearchCharIndexFromX(
    HWND hwnd,
    int textLeft,
    int pointX)
{
    HDC dc = GetDC(hwnd);

    if (!dc)
        return 0;

    HGDIOBJ old = SelectObject(dc, g_font);

    int relativeX = pointX - textLeft;

    int fitChars = 0;
    SIZE sz{};

    if (relativeX > 0)
    {
        GetTextExtentExPointW(
            dc, g_searchText.c_str(), (int)g_searchText.size(),
            relativeX, &fitChars, nullptr, &sz);
    }

    SelectObject(dc, old);
    ReleaseDC(hwnd, dc);

    return fitChars;
}

static void PlaceSearchCaretFromPoint(
    HWND hwnd,
    int textLeft,
    int pointX,
    bool extend)
{
    int pos = SearchCharIndexFromX(hwnd, textLeft, pointX);

    g_searchSelection.PlaceCaret(pos, extend);
    g_caretVisible = true;

    InvalidateRect(hwnd, nullptr, FALSE);
}

// Double-click: selects the word (or run of non-word characters) under
// the click, matching the classic text-box convention.
static void SelectSearchWordAtPoint(
    HWND hwnd,
    int textLeft,
    int pointX)
{
    if (g_searchText.empty())
        return;

    int pos = SearchCharIndexFromX(hwnd, textLeft, pointX);

    int start = 0;
    int end = 0;

    FindWordBoundsAt(g_searchText, pos, start, end);

    g_searchSelection.PlaceCaret(start, false);
    g_searchSelection.PlaceCaret(end, true);

    g_caretVisible = true;

    InvalidateRect(hwnd, nullptr, FALSE);
}

static void HandleSearchEnter()
{
    // A populated results panel takes priority over command resolution —
    // Enter launches the top result, same as clicking it, whether that's
    // a file or a God Mode Control Panel item.
    if (!g_searchResults.empty())
    {
        LaunchSearchResult(0);
        return;
    }

    std::wstring command =
        Trim(g_searchText);

    if (command.empty())
        return;

    std::wstring lower =
        Lower(command);

    if (lower == L"run" ||
        lower == L"run...")
    {
        g_searchText.clear();
        g_searchSelection.Reset();
        ClearSearchResults();
        RepositionSearchPanel();

        OpenNativeRun();

        return;
    }

    LaunchResult result =
        ExecuteSmartInput(command);

    if (result ==
        LaunchResult::Success)
    {
        g_searchText.clear();
        g_searchSelection.Reset();
        ClearSearchResults();
        RepositionSearchPanel();

        ShowWindow(
            g_start,
            SW_HIDE);

        g_startVisible = false;
        ResetUIState();

        return;
    }

    g_searchText.clear();
    g_searchSelection.Reset();
    ClearSearchResults();
    RepositionSearchPanel();

    OpenNativeRun();
}

// ============================================================
// Geometry
// ============================================================

static int MenuTop()
{
    return S(8);
}

static int MenuRow()
{
    return S(39);
}

static int MenuGap()
{
    return S(2);
}

// The search box is hidden until the user starts typing, so it no
// longer reserves space at the top — items start right where it
// used to be. Once there's text, it appears as its own row right
// below the last item ("Run...") instead.
static RECT GetSearchRect(
    int width)
{
    int itemsBottom =
        MenuTop() +
        g_itemCount *
            (MenuRow() + MenuGap());

    int top =
        itemsBottom + S(8);

    return
    {
        S(10),
        top,
        width - S(10),
        top + S(48)
    };
}

// Content-aware window height: just tall enough for the items,
// the search row when it's actually showing, and the bottom
// utility strip — no trailing whitespace when there's nothing to
// fill it.
static int GetStartHeight()
{
    int itemsBottom =
        MenuTop() +
        g_itemCount *
            (MenuRow() + MenuGap());

    int contentBottom =
        itemsBottom;

    if (!g_searchText.empty())
    {
        contentBottom =
            itemsBottom +
            S(8) +
            S(48);
    }

    const int UTILITY_HEIGHT =
        S(47);

    return
        contentBottom +
        S(9) +
        UTILITY_HEIGHT;
}

static RECT GetStartRect()
{
    RECT work{};

    if (!GetWorkArea(work))
    {
        work =
        {
            0,
            0,
            1920,
            1080
        };
    }

    const int width =
        S(210);

    const int height =
        GetStartHeight();

    const int margin =
        S(8);

    RECT r =
    {
        work.left + margin,
        work.bottom - height - margin,
        work.left + margin + width,
        work.bottom - margin
    };

    if (r.right > work.right)
    {
        int d =
            r.right -
            work.right;

        r.left -= d;
        r.right -= d;
    }

    return r;
}

// Called whenever the search box's visibility flips (the user
// typed the first character, or backspaced the last one), so the
// window grows or shrinks to match instead of leaving whitespace
// or clipping content. Always grows/shrinks from the top, since
// the bottom edge is anchored just above the taskbar.
static void ResizeStartToContent(
    HWND hwnd)
{
    if (!hwnd)
        return;

    RECT r =
        GetStartRect();

    if (g_showAnimMode ==
        ShowAnimMode::Opening)
    {
        // Still popping open — just retarget the animation, it
        // will settle at the new size on its own.
        g_showAnimFinalRect = r;
        return;
    }

    SetWindowPos(
        hwnd,
        nullptr,
        r.left,
        r.top,
        r.right - r.left,
        r.bottom - r.top,
        SWP_NOZORDER |
            SWP_NOACTIVATE);

    InvalidateRect(
        hwnd,
        nullptr,
        FALSE);
}

// ============================================================
// Search results panel — a separate top-level window shown beside the
// main menu, so the main menu's own layout never has to shift to make
// room for it.
// ============================================================

static const wchar_t SEARCH_PANEL_CLASS[] = L"ClassicShell.SearchPanel";
static HWND g_searchPanel = nullptr;

static int SidePanelGap()
{
    return S(10);
}

// Same width as the main column — a "twin card" beside it.
static int SidePanelWidth()
{
    return S(210);
}

static RECT GetSearchPanelRect()
{
    RECT main = GetStartRect();
    int width = SidePanelWidth();

    return
    {
        main.right + SidePanelGap(),
        main.top,
        main.right + SidePanelGap() + width,
        main.bottom
    };
}

static int SearchResultRowHeight()
{
    return S(36);
}

static int SearchResultsVisibleRowCount(
    int panelHeight)
{
    int usable = panelHeight - S(16);
    int stride = SearchResultRowHeight() + S(2);

    return usable > 0 ? usable / stride : 0;
}

static int SearchResultsContentHeight()
{
    return (int)g_searchResults.size() * (SearchResultRowHeight() + S(2));
}

static void ClampSearchResultsScroll(
    int panelHeight)
{
    int visibleRows = SearchResultsVisibleRowCount(panelHeight);
    int maxScrollRows =
        (int)g_searchResults.size() - visibleRows;

    int maxScroll =
        maxScrollRows > 0
            ? maxScrollRows * (SearchResultRowHeight() + S(2))
            : 0;

    if (g_searchResultsScroll < 0)
        g_searchResultsScroll = 0;

    if (g_searchResultsScroll > maxScroll)
        g_searchResultsScroll = maxScroll;
}

static int SearchScrollbarWidth()
{
    return S(6);
}

static bool SearchResultsNeedScrollbar(
    int panelHeight)
{
    return SearchResultsContentHeight() > (panelHeight - S(16));
}

// The track spans the same top/bottom inset as the row list itself.
static RECT GetSearchScrollbarTrackRect()
{
    RECT panel{};
    GetClientRect(g_searchPanel, &panel);

    int w = SearchScrollbarWidth();

    return
    {
        (panel.right - panel.left) - S(4) - w,
        S(8),
        (panel.right - panel.left) - S(4),
        (panel.bottom - panel.top) - S(8)
    };
}

static ScrollbarThumb GetSearchScrollbarThumb()
{
    RECT panel{};
    GetClientRect(g_searchPanel, &panel);

    RECT track = GetSearchScrollbarTrackRect();

    return
        ComputeScrollbarThumb(
            SearchResultsContentHeight(),
            (panel.bottom - panel.top) - S(16),
            g_searchResultsScroll,
            track.bottom - track.top,
            S(24));
}

static RECT GetSearchResultRect(
    int index)
{
    RECT panel{};
    GetClientRect(g_searchPanel, &panel);

    int panelHeight = panel.bottom - panel.top;

    int top =
        S(8) +
        index * (SearchResultRowHeight() + S(2)) -
        g_searchResultsScroll;

    int rightMargin =
        S(6) +
        (SearchResultsNeedScrollbar(panelHeight)
             ? SearchScrollbarWidth() + S(6)
             : 0);

    return
    {
        S(6),
        top,
        (panel.right - panel.left) - rightMargin,
        top + SearchResultRowHeight()
    };
}

static void PaintSearchPanel(
    HWND hwnd,
    HDC dc)
{
    RECT client{};
    GetClientRect(hwnd, &client);

    int width = client.right;
    int height = client.bottom;

    HDC back = CreateCompatibleDC(dc);

    if (!back)
        return;

    HBITMAP bitmap = CreateCompatibleBitmap(dc, width, height);

    if (!bitmap)
    {
        DeleteDC(back);
        return;
    }

    HGDIOBJ old = SelectObject(back, bitmap);

    FillRectColor(back, client, g_bg);
    DrawRoundBorder(back, client, S(18), g_border);

    // Wrapped defensively for the same reason PaintPreview is: this
    // touches loaded result data (display names, icons) on every
    // repaint, and an exception escaping into the WM_PAINT dispatch
    // would crash the whole process rather than just this one row.
    try
    {

    int keyboardFocusedIndex = GetFocusedSearchResultIndex();

    for (size_t i = 0; i < g_searchResults.size(); ++i)
    {
        RECT r = GetSearchResultRect((int)i);

        if (r.bottom < 0 || r.top > height)
            continue;

        bool hovered = g_searchResultHover == (int)i;
        bool keyboardFocused =
            keyboardFocusedIndex == (int)i;

        if (hovered || keyboardFocused)
        {
            DrawTile(
                back, r, S(9),
                g_hot, g_accentBorder,
                hovered ? 255 : 180);
        }

        const SearchResultEntry& entry = g_searchResults[i];

        if (entry.icon)
        {
            DrawRealIcon(
                back, entry.icon,
                r.left + S(4),
                r.top + (SearchResultRowHeight() - S(20)) / 2,
                S(20));
        }

        DrawTextSimple(
            back,
            entry.displayName.c_str(),
            r.left + S(30),
            r.top,
            (r.right - r.left) - S(34),
            r.bottom - r.top,
            g_text,
            g_font,
            DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    }

    if (g_searchResults.empty())
    {
        RECT msgRect = { S(10), S(10), width - S(10), height - S(10) };

        DrawTextSimple(
            back,
            g_controlPanelBrowsing ? L"No Control Panel items found" : L"No results",
            msgRect.left,
            msgRect.top,
            msgRect.right - msgRect.left,
            msgRect.bottom - msgRect.top,
            g_muted,
            g_font,
            DT_LEFT | DT_TOP | DT_WORDBREAK);
    }

    if (SearchResultsNeedScrollbar(height))
    {
        RECT track = GetSearchScrollbarTrackRect();
        ScrollbarThumb thumb = GetSearchScrollbarThumb();

        FillRoundRect(
            back, track, S(3),
            MixColor(g_bg, g_text, 8));

        RECT thumbRect =
        {
            track.left,
            track.top + thumb.top,
            track.right,
            track.top + thumb.top + thumb.height
        };

        FillRoundRect(
            back, thumbRect, S(3),
            g_searchScrollbarDragging
                ? g_accent
                : MixColor(g_bg, g_text, 35));
    }

    }
    catch (...)
    {
        // Leave whatever was already drawn (the plain background/border)
        // rather than trying to draw an error message with the same
        // machinery that just failed.
    }

    BitBlt(dc, 0, 0, width, height, back, 0, 0, SRCCOPY);

    SelectObject(back, old);
    DeleteObject(bitmap);
    DeleteDC(back);
}

static int SearchResultRowFromPoint(
    int y)
{
    RECT client{};
    GetClientRect(g_searchPanel, &client);

    for (size_t i = 0; i < g_searchResults.size(); ++i)
    {
        RECT r = GetSearchResultRect((int)i);

        if (y >= r.top && y < r.bottom)
            return (int)i;
    }

    return -1;
}

static LRESULT CALLBACK SearchPanelProc(
    HWND hwnd,
    UINT msg,
    WPARAM wp,
    LPARAM lp)
{
    switch (msg)
    {
        case WM_ERASEBKGND:
            return 1;

        case WM_PAINT:
        {
            PAINTSTRUCT ps{};
            HDC dc = BeginPaint(hwnd, &ps);

            if (dc)
                PaintSearchPanel(hwnd, dc);

            EndPaint(hwnd, &ps);
            return 0;
        }

        case WM_MOUSEMOVE:
        {
            int y = GET_Y_LPARAM(lp);

            if (g_searchScrollbarDragging)
            {
                RECT client{};
                GetClientRect(hwnd, &client);

                RECT track = GetSearchScrollbarTrackRect();
                ScrollbarThumb thumb = GetSearchScrollbarThumb();

                int newThumbTop = y - track.top - g_searchScrollbarDragGrabOffset;

                g_searchResultsScroll =
                    ScrollOffsetFromThumbTop(
                        newThumbTop,
                        SearchResultsContentHeight(),
                        client.bottom - S(16),
                        track.bottom - track.top,
                        thumb.height);

                ClampSearchResultsScroll(client.bottom);

                InvalidateRect(hwnd, nullptr, FALSE);
                return 0;
            }

            int row = SearchResultRowFromPoint(y);

            if (row != g_searchResultHover)
            {
                g_searchResultHover = row;
                InvalidateRect(hwnd, nullptr, FALSE);

                if (g_previewShowTimer)
                {
                    KillTimer(hwnd, TIMER_PREVIEW_HOVER);
                    g_previewShowTimer = 0;
                }

                if (row >= 0)
                {
                    CancelPreviewFadeOut();
                    g_previewHoverIndex = row;

                    g_previewShowTimer =
                        SetTimer(hwnd, TIMER_PREVIEW_HOVER, 150, nullptr);
                }
                else
                {
                    BeginPreviewFadeOut();
                }
            }

            TRACKMOUSEEVENT tme{};
            tme.cbSize = sizeof(tme);
            tme.dwFlags = TME_LEAVE;
            tme.hwndTrack = hwnd;
            TrackMouseEvent(&tme);

            return 0;
        }

        case WM_MOUSELEAVE:
        {
            if (g_searchResultHover != -1)
            {
                g_searchResultHover = -1;
                InvalidateRect(hwnd, nullptr, FALSE);
            }

            if (g_previewShowTimer)
            {
                KillTimer(hwnd, TIMER_PREVIEW_HOVER);
                g_previewShowTimer = 0;
            }

            BeginPreviewFadeOut();

            return 0;
        }

        case WM_TIMER:
        {
            if (wp == TIMER_PREVIEW_HOVER)
            {
                KillTimer(hwnd, TIMER_PREVIEW_HOVER);
                g_previewShowTimer = 0;

                if (g_previewHoverIndex == g_searchResultHover)
                    ShowPreviewForResult(g_previewHoverIndex);

                return 0;
            }

            break;
        }

        case WM_LBUTTONDOWN:
        {
            int y = GET_Y_LPARAM(lp);

            RECT client{};
            GetClientRect(hwnd, &client);

            if (SearchResultsNeedScrollbar(client.bottom))
            {
                RECT track = GetSearchScrollbarTrackRect();
                POINT pt{ GET_X_LPARAM(lp), y };

                if (PtInRect(&track, pt))
                {
                    ScrollbarThumb thumb = GetSearchScrollbarThumb();
                    int thumbTop = track.top + thumb.top;

                    // Clicking the thumb itself grabs it at the exact
                    // point clicked; clicking elsewhere on the track
                    // jumps so the thumb is centered under the cursor.
                    g_searchScrollbarDragGrabOffset =
                        (y >= thumbTop && y < thumbTop + thumb.height)
                            ? y - thumbTop
                            : thumb.height / 2;

                    g_searchScrollbarDragging = true;
                    SetCapture(hwnd);

                    int newThumbTop =
                        y - track.top - g_searchScrollbarDragGrabOffset;

                    g_searchResultsScroll =
                        ScrollOffsetFromThumbTop(
                            newThumbTop,
                            SearchResultsContentHeight(),
                            client.bottom - S(16),
                            track.bottom - track.top,
                            thumb.height);

                    ClampSearchResultsScroll(client.bottom);

                    InvalidateRect(hwnd, nullptr, FALSE);
                    return 0;
                }
            }

            int row = SearchResultRowFromPoint(y);

            if (row >= 0)
                LaunchSearchResult((size_t)row);

            return 0;
        }

        case WM_LBUTTONUP:
        {
            if (g_searchScrollbarDragging)
            {
                g_searchScrollbarDragging = false;
                ReleaseCapture();

                InvalidateRect(hwnd, nullptr, FALSE);
            }

            return 0;
        }

        case WM_CAPTURECHANGED:
        {
            g_searchScrollbarDragging = false;
            return 0;
        }

        case WM_RBUTTONDOWN:
        {
            int y = GET_Y_LPARAM(lp);
            int row = SearchResultRowFromPoint(y);

            if (row >= 0)
                OpenSearchResultFolder((size_t)row);

            return 0;
        }

        case WM_MOUSEWHEEL:
        {
            int delta = GET_WHEEL_DELTA_WPARAM(wp);
            RECT client{};
            GetClientRect(hwnd, &client);

            g_searchResultsScroll -=
                (delta / WHEEL_DELTA) *
                (SearchResultRowHeight() + S(2));

            ClampSearchResultsScroll(client.bottom);

            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        }
    }

    return DefWindowProcW(hwnd, msg, wp, lp);
}

static bool CreateSearchPanelWindow(
    HINSTANCE instance)
{
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = SearchPanelProc;
    wc.hInstance = instance;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = SEARCH_PANEL_CLASS;

    if (!RegisterClassExW(&wc) &&
        GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
    {
        return false;
    }

    RECT r = GetSearchPanelRect();

    g_searchPanel =
        CreateWindowExW(
            WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_LAYERED | WS_EX_NOACTIVATE,
            SEARCH_PANEL_CLASS,
            L"ClassicShell Search",
            WS_POPUP,
            r.left, r.top,
            r.right - r.left, r.bottom - r.top,
            nullptr, nullptr, instance, nullptr);

    if (!g_searchPanel)
        return false;

    SetLayeredWindowAttributes(g_searchPanel, 0, g_windowAlpha, LWA_ALPHA);
    ApplyWindowRounding(g_searchPanel);
    ApplyAcrylicBlur(g_searchPanel);

    return true;
}

// Shows/hides/repositions the panel to match current results and
// whether the main menu itself is visible — not animated like the main
// menu's own open/close, a deliberate simplification since this is a
// satellite window rather than part of the same surface.
static void RepositionSearchPanel()
{
    if (!g_searchPanel)
        return;

    if (!g_startVisible || g_searchResults.empty())
    {
        ShowWindow(g_searchPanel, SW_HIDE);
        HidePreview();
        return;
    }

    RECT r = GetSearchPanelRect();

    ClampSearchResultsScroll(r.bottom - r.top);

    SetWindowPos(
        g_searchPanel,
        HWND_TOPMOST,
        r.left, r.top,
        r.right - r.left, r.bottom - r.top,
        SWP_NOACTIVATE | SWP_SHOWWINDOW);

    // Reapplied every time the panel is (re)shown, not just once at
    // creation: DWM can drop the acrylic composition attribute across a
    // hide/show cycle, which is exactly what happens here every time
    // results go from empty to non-empty — without this, the panel could
    // end up a flat, un-blurred fill even though the call at creation
    // succeeded once. Opacity is kept in sync with the main menu's own
    // slider too, so the two windows always read as one consistent
    // translucent surface rather than two different-looking panes.
    ApplyAcrylicBlur(g_searchPanel);
    SetLayeredWindowAttributes(g_searchPanel, 0, g_windowAlpha, LWA_ALPHA);

    InvalidateRect(g_searchPanel, nullptr, FALSE);
}

// ============================================================
// Hover preview — a small translucent panel that pops up in the work
// area's opposite corner from the menu when the mouse rests on a search
// result row long enough, showing a quick look at a text/JSON/XML file
// or an image (including SVG) without having to open it.
// ============================================================

static const wchar_t PREVIEW_CLASS[] = L"ClassicShell.Preview";

enum class PreviewKind
{
    None,
    Text,
    Image
};

static PreviewKind g_previewKind = PreviewKind::None;
static std::wstring g_previewPath;
static std::wstring g_previewText;
static bool g_previewTextTruncated = false;
static int g_previewTextScroll = 0;
static bool g_previewScrollbarDragging = false;
static int g_previewScrollbarDragGrabOffset = 0;

// ------------------------------------------------------------------
// JSON/XML pretty-print + syntax coloring
// ------------------------------------------------------------------

enum class PreviewHighlightLang
{
    None,
    Json,
    Xml
};

enum class PreviewTokenColor
{
    Default,
    Key,
    String,
    Number,
    Keyword,
    Punctuation,
    TagName,
    AttrName,
    AttrValue,
    Comment
};

struct PreviewColorSpan
{
    size_t start;
    size_t length;
    PreviewTokenColor color;
};

static PreviewHighlightLang g_previewHighlightLang = PreviewHighlightLang::None;
static std::vector<PreviewColorSpan> g_previewColorSpans;

// ------------------------------------------------------------------
// Word-wrap layout
// ------------------------------------------------------------------

struct PreviewDisplayLine
{
    size_t start;
    size_t length;
};

static std::vector<PreviewDisplayLine> g_previewDisplayLines;

static Gdiplus::Bitmap* g_previewImage = nullptr;
static BYTE* g_previewImagePixels = nullptr; // backs g_previewImage for SVG renders; freed alongside it.
static int g_previewImageW = 0;
static int g_previewImageH = 0;

// g_preview, g_previewHoverIndex, g_previewShowTimer, and
// TIMER_PREVIEW_HOVER are declared earlier in the file (see the forward
// declarations before the file-search section), since the search panel's
// own WndProc needs them and is defined before this point.

static UINT_PTR g_previewFadeTimer = 0;
static const UINT_PTR TIMER_PREVIEW_FADE = 6;
static float g_previewAlpha = 1.0f;
static DWORD g_previewFadeStartTick = 0;

static const int PREVIEW_FADE_GRACE_MS = 550;
static const int PREVIEW_FADE_DURATION_MS = 250;
static const BYTE PREVIEW_BASE_ALPHA = 235;
static const size_t PREVIEW_TEXT_MAX_BYTES = 65536;

static const wchar_t* const PREVIEW_TEXT_EXTENSIONS[] =
{
    L"txt", L"log", L"ini", L"json", L"xml", L"csv", L"md",
    L"h", L"hpp", L"c", L"cpp", L"cs", L"js", L"ts", L"py",
    L"bat", L"ps1", L"yml", L"yaml", L"cfg", L"conf"
};

static const wchar_t* const PREVIEW_IMAGE_EXTENSIONS[] =
{
    L"png", L"jpg", L"jpeg", L"bmp", L"gif", L"ico", L"tif", L"tiff"
};

static std::wstring FileExtensionLower(const std::wstring& path)
{
    size_t dot = path.find_last_of(L'.');
    size_t slash = path.find_last_of(L"\\/");

    if (dot == std::wstring::npos ||
        (slash != std::wstring::npos && dot < slash))
    {
        return L"";
    }

    return Lower(path.substr(dot + 1));
}

// Reads a small sample from the front of the file and asks
// LooksLikeText (starthook.h) whether it's plausibly text — the only
// signal available for a file with no extension to go on. Deliberately
// generous rather than strict (see LooksLikeText's own comment): the
// goal is to actually show something for a README/LICENSE/Makefile/
// dotfile instead of silently offering no preview at all, not to be
// a rigorous binary detector.
static bool SniffFileLooksLikeText(
    const std::wstring& path)
{
    HANDLE file =
        CreateFileW(
            path.c_str(), GENERIC_READ, FILE_SHARE_READ,
            nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);

    if (file == INVALID_HANDLE_VALUE)
        return false;

    BYTE buf[4096];
    DWORD read = 0;

    BOOL ok = ReadFile(file, buf, sizeof(buf), &read, nullptr);

    CloseHandle(file);

    if (!ok)
        return false;

    return LooksLikeText(buf, read);
}

// isSvg is set when the image kind is specifically SVG, since that path
// needs Direct2D instead of GDI+.
static PreviewKind ClassifyPreview(const std::wstring& path, bool& isSvg)
{
    isSvg = false;

    std::wstring ext = FileExtensionLower(path);

    // A pure dotfile (.gitignore, .env) has no real extension either —
    // FileExtensionLower reads everything after its single leading dot
    // as if it were one, which never matches a known list, so it's
    // treated the same as a truly extensionless name below.
    size_t slash = path.find_last_of(L"\\/");
    std::wstring base = (slash == std::wstring::npos) ? path : path.substr(slash + 1);
    bool isDotfileOnly =
        base.size() > 1 && base[0] == L'.' && base.find(L'.', 1) == std::wstring::npos;

    // No usable extension at all (README, LICENSE, Makefile, a dotfile)
    // — the name gives no hint, so fall back to actually looking at the
    // content rather than assuming there's nothing to preview.
    if (ext.empty() || isDotfileOnly)
        return SniffFileLooksLikeText(path) ? PreviewKind::Text : PreviewKind::None;

    if (ext == L"svg")
    {
        isSvg = true;
        return PreviewKind::Image;
    }

    for (const wchar_t* e : PREVIEW_IMAGE_EXTENSIONS)
        if (ext == e) return PreviewKind::Image;

    for (const wchar_t* e : PREVIEW_TEXT_EXTENSIONS)
        if (ext == e) return PreviewKind::Text;

    return PreviewKind::None;
}

static bool LoadPreviewTextFile(
    const std::wstring& path,
    std::wstring& outText,
    bool& outTruncated)
{
    outText.clear();
    outTruncated = false;

    HANDLE file =
        CreateFileW(
            path.c_str(), GENERIC_READ, FILE_SHARE_READ,
            nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);

    if (file == INVALID_HANDLE_VALUE)
        return false;

    LARGE_INTEGER size{};
    GetFileSizeEx(file, &size);

    DWORD toRead =
        (DWORD)std::min<LONGLONG>(
            (LONGLONG)PREVIEW_TEXT_MAX_BYTES, size.QuadPart);

    outTruncated = (LONGLONG)toRead < size.QuadPart;

    std::vector<BYTE> buf(toRead);
    DWORD read = 0;
    BOOL ok = toRead == 0 ? TRUE : ReadFile(file, buf.data(), toRead, &read, nullptr);

    CloseHandle(file);

    if (!ok)
        return false;

    if (read >= 2 && buf[0] == 0xFF && buf[1] == 0xFE)
    {
        outText.assign(
            reinterpret_cast<wchar_t*>(buf.data() + 2),
            (read - 2) / 2);
    }
    else
    {
        size_t off =
            (read >= 3 && buf[0] == 0xEF && buf[1] == 0xBB && buf[2] == 0xBF)
                ? 3 : 0;

        int wlen =
            MultiByteToWideChar(
                CP_UTF8, 0,
                reinterpret_cast<char*>(buf.data()) + off,
                (int)(read - off),
                nullptr, 0);

        if (wlen > 0)
        {
            outText.resize(wlen);

            MultiByteToWideChar(
                CP_UTF8, 0,
                reinterpret_cast<char*>(buf.data()) + off,
                (int)(read - off),
                &outText[0], wlen);
        }
    }

    return true;
}

static Gdiplus::Bitmap* LoadPreviewImageFile(const std::wstring& path)
{
    Gdiplus::Bitmap* bmp = new Gdiplus::Bitmap(path.c_str());

    if (bmp->GetLastStatus() != Gdiplus::Ok)
    {
        delete bmp;
        return nullptr;
    }

    return bmp;
}

// GDI+ has no SVG support, so this renders through Direct2D's native SVG
// document API over a Direct3D11 device (hardware, falling back to the
// WARP software rasterizer when no GPU driver is available — some VMs/
// remote sessions). The full device/context is created and torn down on
// every call rather than kept alive, which is fine since a hover preview
// only pays this cost once per file, debounced behind the same hover
// timer as everything else. Returns a heap Gdiplus::Bitmap wrapping a
// pixel buffer stashed in *outPixels — GDI+ does not copy that buffer,
// so the caller must keep it alive exactly as long as the bitmap and
// free both together.
static Gdiplus::Bitmap* RenderSvgToBitmap(
    const std::wstring& path,
    int workAreaW,
    int workAreaH,
    BYTE** outPixels)
{
    *outPixels = nullptr;

    ID3D11Device* d3dDevice = nullptr;
    ID3D11DeviceContext* d3dContext = nullptr;
    D3D_FEATURE_LEVEL featureLevel{};

    HRESULT hr =
        D3D11CreateDevice(
            nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
            D3D11_CREATE_DEVICE_BGRA_SUPPORT,
            nullptr, 0, D3D11_SDK_VERSION,
            &d3dDevice, &featureLevel, &d3dContext);

    if (FAILED(hr))
    {
        hr =
            D3D11CreateDevice(
                nullptr, D3D_DRIVER_TYPE_WARP, nullptr,
                D3D11_CREATE_DEVICE_BGRA_SUPPORT,
                nullptr, 0, D3D11_SDK_VERSION,
                &d3dDevice, &featureLevel, &d3dContext);
    }

    if (FAILED(hr) || !d3dDevice)
        return nullptr;

    Gdiplus::Bitmap* result = nullptr;

    IDXGIDevice* dxgiDevice = nullptr;
    ID2D1Factory1* d2dFactory = nullptr;
    ID2D1Device* d2dDevice = nullptr;
    ID2D1DeviceContext* baseContext = nullptr;
    ID2D1DeviceContext5* dc5 = nullptr;
    IWICImagingFactory* wic = nullptr;
    IWICStream* stream = nullptr;
    ID2D1SvgDocument* svgDoc = nullptr;
    ID2D1Bitmap1* targetBitmap = nullptr;
    ID2D1Bitmap1* stagingBitmap = nullptr;

    d3dDevice->QueryInterface(IID_PPV_ARGS(&dxgiDevice));

    if (dxgiDevice &&
        SUCCEEDED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, IID_PPV_ARGS(&d2dFactory))) &&
        SUCCEEDED(d2dFactory->CreateDevice(dxgiDevice, &d2dDevice)) &&
        SUCCEEDED(d2dDevice->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE, &baseContext)) &&
        SUCCEEDED(baseContext->QueryInterface(IID_PPV_ARGS(&dc5))))
    {
        if (SUCCEEDED(CoCreateInstance(
                CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                IID_PPV_ARGS(&wic))) &&
            SUCCEEDED(wic->CreateStream(&stream)) &&
            SUCCEEDED(stream->InitializeFromFilename(path.c_str(), GENERIC_READ)))
        {
            // Parse at a nominal size first just to read the document's
            // own native width/height/viewBox, so the real render target
            // can be sized to the actual aspect ratio instead of
            // stretching/squishing it into a guessed box.
            hr = dc5->CreateSvgDocument(stream, D2D1::SizeF(100, 100), &svgDoc);

            if (SUCCEEDED(hr) && svgDoc)
            {
                float nativeW = 0, nativeH = 0;

                ID2D1SvgElement* root = nullptr;
                svgDoc->GetRoot(&root);

                if (root)
                {
                    float w = 0, h = 0;

                    bool haveW = SUCCEEDED(root->GetAttributeValue(L"width", &w));
                    bool haveH = SUCCEEDED(root->GetAttributeValue(L"height", &h));

                    if (haveW && haveH && w > 0 && h > 0)
                    {
                        nativeW = w;
                        nativeH = h;
                    }
                    else
                    {
                        D2D1_SVG_VIEWBOX vb{};

                        if (SUCCEEDED(root->GetAttributeValue(
                                L"viewBox",
                                D2D1_SVG_ATTRIBUTE_POD_TYPE_VIEWBOX,
                                &vb, sizeof(vb))) &&
                            vb.width > 0 && vb.height > 0)
                        {
                            nativeW = vb.width;
                            nativeH = vb.height;
                        }
                    }

                    root->Release();
                }

                if (nativeW <= 0 || nativeH <= 0)
                {
                    // SVG spec default when no size information at all.
                    nativeW = 300;
                    nativeH = 300;
                }

                SIZE targetSize =
                    ScalePreviewImageSize(
                        (int)nativeW, (int)nativeH,
                        workAreaW, workAreaH);

                svgDoc->SetViewportSize(
                    D2D1::SizeF((float)targetSize.cx, (float)targetSize.cy));

                D2D1_BITMAP_PROPERTIES1 targetProps =
                    D2D1::BitmapProperties1(
                        D2D1_BITMAP_OPTIONS_TARGET,
                        D2D1::PixelFormat(
                            DXGI_FORMAT_B8G8R8A8_UNORM,
                            D2D1_ALPHA_MODE_PREMULTIPLIED));

                hr =
                    dc5->CreateBitmap(
                        D2D1::SizeU((UINT32)targetSize.cx, (UINT32)targetSize.cy),
                        nullptr, 0, targetProps, &targetBitmap);

                if (SUCCEEDED(hr) && targetBitmap)
                {
                    dc5->SetTarget(targetBitmap);
                    dc5->BeginDraw();
                    dc5->Clear(D2D1::ColorF(0, 0, 0, 0));
                    dc5->DrawSvgDocument(svgDoc);
                    hr = dc5->EndDraw();
                }

                if (SUCCEEDED(hr) && targetBitmap)
                {
                    D2D1_BITMAP_PROPERTIES1 stagingProps =
                        D2D1::BitmapProperties1(
                            D2D1_BITMAP_OPTIONS_CPU_READ | D2D1_BITMAP_OPTIONS_CANNOT_DRAW,
                            D2D1::PixelFormat(
                                DXGI_FORMAT_B8G8R8A8_UNORM,
                                D2D1_ALPHA_MODE_PREMULTIPLIED));

                    hr =
                        dc5->CreateBitmap(
                            D2D1::SizeU((UINT32)targetSize.cx, (UINT32)targetSize.cy),
                            nullptr, 0, stagingProps, &stagingBitmap);

                    if (SUCCEEDED(hr) && stagingBitmap)
                        hr = stagingBitmap->CopyFromBitmap(nullptr, targetBitmap, nullptr);

                    D2D1_MAPPED_RECT mapped{};

                    if (SUCCEEDED(hr) &&
                        SUCCEEDED(stagingBitmap->Map(D2D1_MAP_OPTIONS_READ, &mapped)))
                    {
                        size_t bufSize = (size_t)mapped.pitch * targetSize.cy;
                        BYTE* pixels = new BYTE[bufSize];

                        memcpy(pixels, mapped.bits, bufSize);

                        result =
                            new Gdiplus::Bitmap(
                                targetSize.cx, targetSize.cy, mapped.pitch,
                                PixelFormat32bppPARGB, pixels);

                        if (result->GetLastStatus() != Gdiplus::Ok)
                        {
                            delete result;
                            delete[] pixels;
                            result = nullptr;
                        }
                        else
                        {
                            *outPixels = pixels;
                        }

                        stagingBitmap->Unmap();
                    }
                }
            }
        }
    }

    if (stagingBitmap) stagingBitmap->Release();
    if (targetBitmap) targetBitmap->Release();
    if (svgDoc) svgDoc->Release();
    if (stream) stream->Release();
    if (wic) wic->Release();
    if (dc5) dc5->Release();
    if (baseContext) baseContext->Release();
    if (d2dDevice) d2dDevice->Release();
    if (d2dFactory) d2dFactory->Release();
    if (dxgiDevice) dxgiDevice->Release();
    d3dContext->Release();
    d3dDevice->Release();

    return result;
}

// Measured once per shown file (not per paint): total wrapped content
// size at the panel's own mono-ish font, and the line height used both
// for layout and for wheel-scroll clamping.
static int g_previewContentW = 0;
static int g_previewContentH = 0;
static int g_previewLineHeight = S(18);

// ------------------------------------------------------------------
// JSON pretty-print + tokenizer
// ------------------------------------------------------------------

namespace
{
    // A small recursive-descent JSON re-serializer: parses just enough
    // to re-emit the same document with 2-space indentation. Any parse
    // failure aborts immediately (PrettyPrintJson returns false) so the
    // caller falls back to the raw, unformatted text rather than risk
    // emitting something mangled.
    struct JsonPrettyPrinter
    {
        const std::wstring& s;
        size_t pos = 0;
        bool ok = true;

        explicit JsonPrettyPrinter(const std::wstring& text) : s(text) {}

        void SkipWs()
        {
            while (pos < s.size() && iswspace(s[pos]))
                pos++;
        }

        void Indent(std::wstring& out, int depth)
        {
            out.append((size_t)depth * 2, L' ');
        }

        bool ParseString(std::wstring& out)
        {
            if (pos >= s.size() || s[pos] != L'"')
            {
                ok = false;
                return false;
            }

            size_t start = pos;
            pos++;

            while (pos < s.size() && s[pos] != L'"')
            {
                if (s[pos] == L'\\' && pos + 1 < s.size())
                    pos += 2;
                else
                    pos++;
            }

            if (pos >= s.size())
            {
                ok = false;
                return false;
            }

            pos++; // closing quote
            out.append(s, start, pos - start);
            return true;
        }

        bool ParseNumber(std::wstring& out)
        {
            size_t start = pos;

            if (s[pos] == L'-')
                pos++;

            while (pos < s.size() &&
                   (iswdigit(s[pos]) || s[pos] == L'.' ||
                    s[pos] == L'e' || s[pos] == L'E' ||
                    s[pos] == L'+' || s[pos] == L'-'))
            {
                pos++;
            }

            if (pos == start)
            {
                ok = false;
                return false;
            }

            out.append(s, start, pos - start);
            return true;
        }

        bool ParseLiteral(std::wstring& out)
        {
            static const wchar_t* const literals[] = { L"true", L"false", L"null" };

            for (const wchar_t* lit : literals)
            {
                size_t len = wcslen(lit);

                if (s.compare(pos, len, lit) == 0)
                {
                    out.append(lit);
                    pos += len;
                    return true;
                }
            }

            ok = false;
            return false;
        }

        // Deeply/pathologically nested JSON (thousands of levels of
        // "[[[[...") would otherwise recurse once per level here and
        // overflow the stack — a hard crash that a try/catch can't
        // recover from, since a stack overflow isn't an ordinary C++
        // exception. Bailing out past a generous depth (further than
        // any real hand- or tool-authored JSON should ever need) turns
        // that into the same graceful "fall back to raw text" path as
        // any other parse failure.
        static const int MAX_DEPTH = 64;

        bool ParseValue(std::wstring& out, int depth)
        {
            if (depth > MAX_DEPTH)
            {
                ok = false;
                return false;
            }

            SkipWs();

            if (pos >= s.size())
            {
                ok = false;
                return false;
            }

            wchar_t c = s[pos];

            if (c == L'{') return ParseObject(out, depth);
            if (c == L'[') return ParseArray(out, depth);
            if (c == L'"') return ParseString(out);
            if (c == L't' || c == L'f' || c == L'n') return ParseLiteral(out);
            if (c == L'-' || iswdigit(c)) return ParseNumber(out);

            ok = false;
            return false;
        }

        bool ParseObject(std::wstring& out, int depth)
        {
            pos++; // {
            SkipWs();

            if (pos < s.size() && s[pos] == L'}')
            {
                pos++;
                out += L"{}";
                return true;
            }

            out += L"{\n";
            bool first = true;

            while (true)
            {
                if (!first) out += L",\n";
                first = false;

                SkipWs();
                Indent(out, depth + 1);

                if (pos >= s.size() || s[pos] != L'"')
                {
                    ok = false;
                    return false;
                }

                if (!ParseString(out)) return false;

                SkipWs();

                if (pos >= s.size() || s[pos] != L':')
                {
                    ok = false;
                    return false;
                }

                pos++;
                out += L": ";

                if (!ParseValue(out, depth + 1)) return false;

                SkipWs();

                if (pos < s.size() && s[pos] == L',') { pos++; continue; }
                if (pos < s.size() && s[pos] == L'}') { pos++; break; }

                ok = false;
                return false;
            }

            out += L"\n";
            Indent(out, depth);
            out += L"}";
            return true;
        }

        bool ParseArray(std::wstring& out, int depth)
        {
            pos++; // [
            SkipWs();

            if (pos < s.size() && s[pos] == L']')
            {
                pos++;
                out += L"[]";
                return true;
            }

            out += L"[\n";
            bool first = true;

            while (true)
            {
                if (!first) out += L",\n";
                first = false;

                SkipWs();
                Indent(out, depth + 1);

                if (!ParseValue(out, depth + 1)) return false;

                SkipWs();

                if (pos < s.size() && s[pos] == L',') { pos++; continue; }
                if (pos < s.size() && s[pos] == L']') { pos++; break; }

                ok = false;
                return false;
            }

            out += L"\n";
            Indent(out, depth);
            out += L"]";
            return true;
        }
    };
}

static bool PrettyPrintJson(
    const std::wstring& raw,
    std::wstring& outPretty)
{
    JsonPrettyPrinter printer(raw);
    std::wstring out;

    if (!printer.ParseValue(out, 0) || !printer.ok)
        return false;

    printer.SkipWs();

    if (printer.pos != raw.size())
        return false; // trailing garbage after the top-level value.

    outPretty = out;
    return true;
}

static void TokenizeJsonForHighlight(
    const std::wstring& text,
    std::vector<PreviewColorSpan>& spans)
{
    size_t i = 0;
    size_t n = text.size();

    while (i < n)
    {
        wchar_t c = text[i];

        if (iswspace(c)) { i++; continue; }

        if (c == L'"')
        {
            size_t start = i;
            i++;

            while (i < n && text[i] != L'"')
            {
                if (text[i] == L'\\' && i + 1 < n) i += 2;
                else i++;
            }

            if (i < n) i++; // closing quote

            size_t look = i;
            while (look < n && iswspace(text[look])) look++;

            PreviewTokenColor color =
                (look < n && text[look] == L':')
                    ? PreviewTokenColor::Key
                    : PreviewTokenColor::String;

            spans.push_back({ start, i - start, color });
            continue;
        }

        if (c == L'-' || iswdigit(c))
        {
            size_t start = i;
            i++;

            while (i < n &&
                   (iswdigit(text[i]) || text[i] == L'.' ||
                    text[i] == L'e' || text[i] == L'E' ||
                    text[i] == L'+' || text[i] == L'-'))
            {
                i++;
            }

            spans.push_back({ start, i - start, PreviewTokenColor::Number });
            continue;
        }

        if (iswalpha(c))
        {
            size_t start = i;

            while (i < n && iswalpha(text[i]))
                i++;

            std::wstring word = text.substr(start, i - start);

            if (word == L"true" || word == L"false" || word == L"null")
                spans.push_back({ start, i - start, PreviewTokenColor::Keyword });

            continue;
        }

        if (c == L'{' || c == L'}' || c == L'[' || c == L']' ||
            c == L':' || c == L',')
        {
            spans.push_back({ i, 1, PreviewTokenColor::Punctuation });
            i++;
            continue;
        }

        i++;
    }
}

// ------------------------------------------------------------------
// XML pretty-print + tokenizer
// ------------------------------------------------------------------

// Finds the end (exclusive) of the tag/comment/CDATA/declaration
// starting at `start` (which must point at '<'), respecting quoted
// attribute values so a '>' inside one doesn't end the tag early.
// Returns std::wstring::npos if the construct is never closed.
static size_t FindXmlTagEnd(
    const std::wstring& text,
    size_t start)
{
    size_t n = text.size();

    if (text.compare(start, 4, L"<!--") == 0)
    {
        size_t end = text.find(L"-->", start);
        return end == std::wstring::npos ? std::wstring::npos : end + 3;
    }

    if (text.compare(start, 9, L"<![CDATA[") == 0)
    {
        size_t end = text.find(L"]]>", start);
        return end == std::wstring::npos ? std::wstring::npos : end + 3;
    }

    size_t j = start + 1;
    bool inQuote = false;
    wchar_t quoteChar = 0;

    while (j < n)
    {
        wchar_t c = text[j];

        if (inQuote)
        {
            if (c == quoteChar) inQuote = false;
        }
        else
        {
            if (c == L'"' || c == L'\'') { inQuote = true; quoteChar = c; }
            else if (c == L'>') return j + 1;
        }

        j++;
    }

    return std::wstring::npos;
}

static bool PrettyPrintXml(
    const std::wstring& raw,
    std::wstring& outPretty)
{
    std::wstring out;
    int depth = 0;
    size_t i = 0;
    size_t n = raw.size();

    while (i < n)
    {
        if (raw[i] == L'<')
        {
            size_t tagEnd = FindXmlTagEnd(raw, i);

            if (tagEnd == std::wstring::npos)
                return false;

            std::wstring tag = raw.substr(i, tagEnd - i);

            bool isClosing = tag.size() > 1 && tag[1] == L'/';
            bool isSelfClosing = tag.size() > 2 && tag[tag.size() - 2] == L'/';
            bool isSpecial =
                tag.compare(0, 4, L"<!--") == 0 ||
                tag.compare(0, 9, L"<![CDATA[") == 0 ||
                tag.compare(0, 2, L"<?") == 0 ||
                tag.compare(0, 2, L"<!") == 0;

            if (isClosing && depth > 0)
                depth--;

            out.append((size_t)depth * 2, L' ');
            out += tag;
            out += L'\n';

            if (!isClosing && !isSelfClosing && !isSpecial)
                depth++;

            i = tagEnd;
        }
        else
        {
            size_t textStart = i;
            size_t nextTag = raw.find(L'<', i);
            size_t textEnd = (nextTag == std::wstring::npos) ? n : nextTag;

            std::wstring trimmed = Trim(raw.substr(textStart, textEnd - textStart));

            if (!trimmed.empty())
            {
                out.append((size_t)depth * 2, L' ');
                out += trimmed;
                out += L'\n';
            }

            i = textEnd;
        }
    }

    if (depth != 0)
        return false; // mismatched tags — fail safe to raw text.

    outPretty = out;
    return true;
}

static void TokenizeXmlForHighlight(
    const std::wstring& text,
    std::vector<PreviewColorSpan>& spans)
{
    size_t i = 0;
    size_t n = text.size();

    while (i < n)
    {
        if (text[i] != L'<')
        {
            i++;
            continue;
        }

        size_t tagStart = i;

        if (text.compare(i, 4, L"<!--") == 0)
        {
            size_t end = text.find(L"-->", i);
            end = (end == std::wstring::npos) ? n : end + 3;
            spans.push_back({ tagStart, end - tagStart, PreviewTokenColor::Comment });
            i = end;
            continue;
        }

        size_t tagEnd = FindXmlTagEnd(text, i);

        if (tagEnd == std::wstring::npos)
        {
            i++;
            continue;
        }

        spans.push_back({ tagStart, 1, PreviewTokenColor::Punctuation });

        size_t nameStart = tagStart + 1;

        if (nameStart < tagEnd && text[nameStart] == L'/')
            nameStart++;

        size_t nameEnd = nameStart;

        while (nameEnd < tagEnd &&
               (iswalnum(text[nameEnd]) || text[nameEnd] == L'_' ||
                text[nameEnd] == L'-' || text[nameEnd] == L':' ||
                text[nameEnd] == L'.'))
        {
            nameEnd++;
        }

        if (nameEnd > nameStart)
            spans.push_back({ nameStart, nameEnd - nameStart, PreviewTokenColor::TagName });

        size_t k = nameEnd;

        while (k < tagEnd)
        {
            while (k < tagEnd && iswspace(text[k])) k++;

            if (k >= tagEnd || text[k] == L'/' || text[k] == L'>')
                break;

            size_t attrStart = k;

            while (k < tagEnd && text[k] != L'=' &&
                   !iswspace(text[k]) && text[k] != L'>' && text[k] != L'/')
            {
                k++;
            }

            if (k > attrStart)
                spans.push_back({ attrStart, k - attrStart, PreviewTokenColor::AttrName });

            while (k < tagEnd && iswspace(text[k])) k++;

            if (k < tagEnd && text[k] == L'=')
            {
                k++;

                while (k < tagEnd && iswspace(text[k])) k++;

                if (k < tagEnd && (text[k] == L'"' || text[k] == L'\''))
                {
                    wchar_t q = text[k];
                    size_t valStart = k;
                    k++;

                    while (k < tagEnd && text[k] != q) k++;
                    if (k < tagEnd) k++;

                    spans.push_back({ valStart, k - valStart, PreviewTokenColor::AttrValue });
                }
            }
        }

        i = tagEnd;
    }
}

static COLORREF PreviewTokenRGB(
    PreviewTokenColor color)
{
    switch (color)
    {
        case PreviewTokenColor::Key:         return RGB(156, 220, 254);
        case PreviewTokenColor::String:      return RGB(206, 145, 120);
        case PreviewTokenColor::Number:      return RGB(181, 206, 168);
        case PreviewTokenColor::Keyword:     return RGB(86, 156, 214);
        case PreviewTokenColor::Punctuation: return g_muted;
        case PreviewTokenColor::TagName:     return RGB(86, 156, 214);
        case PreviewTokenColor::AttrName:    return RGB(156, 220, 254);
        case PreviewTokenColor::AttrValue:   return RGB(206, 145, 120);
        case PreviewTokenColor::Comment:     return RGB(106, 153, 85);
        default:                             return g_text;
    }
}

// ------------------------------------------------------------------
// Word-wrap
// ------------------------------------------------------------------

// Greedy word-wrap of g_previewText into display lines no wider than
// targetWidth, preferring to break at the last space within reach so
// words don't split mid-token except when a single unbroken run (a long
// URL, a minified attribute value) genuinely has nowhere else to break.
static void WrapPreviewText(
    int targetWidth)
{
    g_previewDisplayLines.clear();

    HDC dc = GetDC(nullptr);

    if (!dc)
        return;

    HGDIOBJ old = SelectObject(dc, g_font);

    size_t pos = 0;
    size_t n = g_previewText.size();

    while (pos <= n)
    {
        size_t nl = g_previewText.find(L'\n', pos);
        size_t lineEnd = (nl == std::wstring::npos) ? n : nl;

        size_t segStart = pos;

        if (segStart == lineEnd)
        {
            g_previewDisplayLines.push_back({ segStart, 0 });
        }
        else
        {
            while (segStart < lineEnd)
            {
                int available = (int)(lineEnd - segStart);
                int fitChars = 0;
                SIZE sz{};

                GetTextExtentExPointW(
                    dc,
                    g_previewText.c_str() + segStart,
                    available,
                    targetWidth,
                    &fitChars,
                    nullptr,
                    &sz);

                if (fitChars <= 0)
                    fitChars = 1; // always make progress

                size_t breakAt = segStart + (size_t)fitChars;

                if (breakAt < lineEnd)
                {
                    size_t lastSpace = std::wstring::npos;

                    for (size_t k = breakAt; k > segStart; --k)
                    {
                        if (g_previewText[k - 1] == L' ')
                        {
                            lastSpace = k - 1;
                            break;
                        }
                    }

                    if (lastSpace != std::wstring::npos && lastSpace > segStart)
                        breakAt = lastSpace;
                }

                size_t displayLen = breakAt - segStart;
                g_previewDisplayLines.push_back({ segStart, displayLen });

                size_t next = breakAt;

                if (next < lineEnd && g_previewText[next] == L' ')
                    next++;

                segStart = next;
            }
        }

        if (nl == std::wstring::npos)
            break;

        pos = nl + 1;
    }

    SelectObject(dc, old);
    ReleaseDC(nullptr, dc);
}

// Orchestrates layout for the currently-loaded g_previewText: measures
// line height, picks the fixed target width (so the panel reads as a
// clean, compact rectangle regardless of content), word-wraps to it, and
// derives the total content height from the wrapped line count.
static void LayoutPreviewText(
    int workAreaW)
{
    HDC dc = GetDC(nullptr);

    if (dc)
    {
        HGDIOBJ old = SelectObject(dc, g_font);

        TEXTMETRICW tm{};
        GetTextMetricsW(dc, &tm);
        g_previewLineHeight = tm.tmHeight + tm.tmExternalLeading;

        SelectObject(dc, old);
        ReleaseDC(nullptr, dc);
    }

    g_previewContentW = PreviewTextTargetWidth(workAreaW, S(300), S(560));

    WrapPreviewText(g_previewContentW);

    g_previewContentH = (int)g_previewDisplayLines.size() * g_previewLineHeight;
}

static int PreviewScrollbarWidth()
{
    return S(6);
}

// True whenever the text view has more content than fits in its
// (already-clamped) height — the only case that gets a visible
// scrollbar; images never scroll.
static bool PreviewNeedsScrollbar(
    int viewportHeight)
{
    return
        g_previewKind == PreviewKind::Text &&
        g_previewContentH > viewportHeight;
}

static RECT GetPreviewRect()
{
    RECT work{};

    if (!GetWorkArea(work))
        work = { 0, 0, 1920, 1080 };

    int margin = S(8);
    int w, h;

    if (g_previewKind == PreviewKind::Image)
    {
        w = g_previewImageW;
        h = g_previewImageH;
    }
    else
    {
        SIZE clamped =
            ClampPreviewTextSize(
                g_previewContentW + S(24),
                g_previewContentH + S(24),
                work.right - work.left,
                work.bottom - work.top);

        w = clamped.cx;
        h = clamped.cy;

        // The clamp above sized the window to the content's own wrapped
        // width; when the content is also too tall to fit (and will
        // need to scroll), widen the window a little further rather
        // than shrinking the text area to make room for the scrollbar.
        if ((g_previewContentH + S(24)) > h)
            w += PreviewScrollbarWidth() + S(6);
    }

    return
    {
        work.right - margin - w,
        work.top + margin,
        work.right - margin,
        work.top + margin + h
    };
}

static RECT GetPreviewScrollbarTrackRect()
{
    RECT client{};
    GetClientRect(g_preview, &client);

    int w = PreviewScrollbarWidth();

    return
    {
        (client.right - client.left) - S(4) - w,
        S(8),
        (client.right - client.left) - S(4),
        (client.bottom - client.top) - S(8)
    };
}

static ScrollbarThumb GetPreviewScrollbarThumb()
{
    RECT client{};
    GetClientRect(g_preview, &client);

    RECT track = GetPreviewScrollbarTrackRect();

    return
        ComputeScrollbarThumb(
            g_previewContentH,
            (client.bottom - client.top) - S(20),
            g_previewTextScroll,
            track.bottom - track.top,
            S(24));
}

static void ApplyPreviewAlpha()
{
    if (!g_preview)
        return;

    SetLayeredWindowAttributes(
        g_preview, 0,
        (BYTE)(PREVIEW_BASE_ALPHA * g_previewAlpha),
        LWA_ALPHA);
}

static void CancelPreviewFadeOut()
{
    if (g_previewFadeTimer)
    {
        KillTimer(g_preview, TIMER_PREVIEW_FADE);
        g_previewFadeTimer = 0;
    }

    g_previewAlpha = 1.0f;
    ApplyPreviewAlpha();
}

static void BeginPreviewFadeOut()
{
    if (g_previewFadeTimer || g_previewKind == PreviewKind::None || !g_preview)
        return;

    g_previewFadeStartTick = GetTickCount();

    g_previewFadeTimer =
        SetTimer(g_preview, TIMER_PREVIEW_FADE, 16, nullptr);
}

static void ClearPreviewContent()
{
    if (g_previewImage)
    {
        delete g_previewImage;
        g_previewImage = nullptr;
    }

    if (g_previewImagePixels)
    {
        delete[] g_previewImagePixels;
        g_previewImagePixels = nullptr;
    }

    g_previewText.clear();
    g_previewTextTruncated = false;
    g_previewTextScroll = 0;
    g_previewKind = PreviewKind::None;
    g_previewPath.clear();
    g_previewImageW = 0;
    g_previewImageH = 0;
    g_previewContentW = 0;
    g_previewContentH = 0;
    g_previewHighlightLang = PreviewHighlightLang::None;
    g_previewColorSpans.clear();
    g_previewDisplayLines.clear();
}

static void HidePreview()
{
    if (g_previewFadeTimer)
    {
        KillTimer(g_preview, TIMER_PREVIEW_FADE);
        g_previewFadeTimer = 0;
    }

    if (g_previewShowTimer)
    {
        KillTimer(g_searchPanel, TIMER_PREVIEW_HOVER);
        g_previewShowTimer = 0;
    }

    if (g_preview)
        ShowWindow(g_preview, SW_HIDE);

    ClearPreviewContent();
    g_previewHoverIndex = -1;
}

// Loads and shows a hover preview for g_searchResults[index]. Not fatal
// on any failure along the way — an unsupported/unreadable file just
// means no preview appears, the search results themselves are unaffected.
static void ShowPreviewForResultImpl(int index)
{
    if (!g_preview)
        return;

    if (index < 0 || index >= (int)g_searchResults.size())
        return;

    const SearchResultEntry& entry = g_searchResults[index];

    if (entry.kind != SearchResultKind::File)
    {
        HidePreview();
        return;
    }

    std::wstring path;

    {
        std::lock_guard<std::mutex> lock(g_fileIndexMutex);

        if (entry.fileIndex < g_fileIndex.size())
            path = g_fileIndex[entry.fileIndex].fullPath;
    }

    if (path.empty())
        return;

    bool isSvg = false;
    PreviewKind kind = ClassifyPreview(path, isSvg);

    if (kind == PreviewKind::None)
    {
        HidePreview();
        return;
    }

    ClearPreviewContent();

    RECT work{};

    if (!GetWorkArea(work))
        work = { 0, 0, 1920, 1080 };

    int workW = work.right - work.left;
    int workH = work.bottom - work.top;

    if (kind == PreviewKind::Text)
    {
        if (!LoadPreviewTextFile(path, g_previewText, g_previewTextTruncated))
            return;

        std::wstring ext = FileExtensionLower(path);

        if (ext == L"json")
        {
            std::wstring pretty;

            if (PrettyPrintJson(g_previewText, pretty))
            {
                g_previewText = pretty;
                g_previewHighlightLang = PreviewHighlightLang::Json;
                TokenizeJsonForHighlight(g_previewText, g_previewColorSpans);
            }
            // On parse failure, g_previewText stays as the raw text and
            // g_previewHighlightLang stays None — fail safe to a plain,
            // unhighlighted view rather than risk mangling the content.
        }
        else if (ext == L"xml")
        {
            std::wstring pretty;

            if (PrettyPrintXml(g_previewText, pretty))
            {
                g_previewText = pretty;
                g_previewHighlightLang = PreviewHighlightLang::Xml;
                TokenizeXmlForHighlight(g_previewText, g_previewColorSpans);
            }
        }

        LayoutPreviewText(workW);
        g_previewKind = PreviewKind::Text;
    }
    else
    {
        Gdiplus::Bitmap* bmp = nullptr;

        if (isSvg)
        {
            BYTE* pixels = nullptr;
            bmp = RenderSvgToBitmap(path, workW, workH, &pixels);
            g_previewImagePixels = pixels;
        }
        else
        {
            bmp = LoadPreviewImageFile(path);
        }

        if (!bmp)
        {
            if (g_previewImagePixels)
            {
                delete[] g_previewImagePixels;
                g_previewImagePixels = nullptr;
            }

            return;
        }

        g_previewImage = bmp;

        int nativeW = (int)bmp->GetWidth();
        int nativeH = (int)bmp->GetHeight();

        // SVGs are already rendered at their final target size by
        // RenderSvgToBitmap; raster images still need scaling here.
        if (isSvg)
        {
            g_previewImageW = nativeW;
            g_previewImageH = nativeH;
        }
        else
        {
            SIZE scaled = ScalePreviewImageSize(nativeW, nativeH, workW, workH);
            g_previewImageW = scaled.cx;
            g_previewImageH = scaled.cy;
        }

        g_previewKind = PreviewKind::Image;
    }

    g_previewPath = path;

    RECT r = GetPreviewRect();

    SetWindowPos(
        g_preview, HWND_TOPMOST,
        r.left, r.top,
        r.right - r.left, r.bottom - r.top,
        SWP_NOACTIVATE | SWP_SHOWWINDOW);

    // Reapplied every time the panel is shown, not just once at
    // creation — see the identical note in RepositionSearchPanel for why.
    ApplyAcrylicBlur(g_preview);

    g_previewAlpha = 1.0f;
    ApplyPreviewAlpha();

    InvalidateRect(g_preview, nullptr, FALSE);
}

// Loads and shows a hover preview for g_searchResults[index]. This is
// the one place every risky per-file operation funnels through — text
// loading, JSON/XML parsing, image/SVG decoding, all running against
// content this app doesn't control the shape of. Any of that failing in
// an ordinary way (bad_alloc, out_of_range, or anything else derived
// from std::exception) is caught here and turned into "no preview
// shown," rather than left to propagate out through the WM_TIMER
// dispatch that calls this and crash the whole process — which, from
// the user's side, looks identical to the Start menu itself closing.
// (A stack overflow specifically can't be caught this way — see
// JsonPrettyPrinter's own MAX_DEPTH guard for how that risk is
// prevented instead, at the source, rather than recovered from here.)
static void ShowPreviewForResult(int index)
{
    try
    {
        ShowPreviewForResultImpl(index);
    }
    catch (...)
    {
        HidePreview();
    }
}

static void PaintPreview(
    HWND hwnd,
    HDC dc)
{
    RECT client{};
    GetClientRect(hwnd, &client);

    int width = client.right;
    int height = client.bottom;

    HDC back = CreateCompatibleDC(dc);

    if (!back)
        return;

    HBITMAP bitmap = CreateCompatibleBitmap(dc, width, height);

    if (!bitmap)
    {
        DeleteDC(back);
        return;
    }

    HGDIOBJ old = SelectObject(back, bitmap);

    FillRectColor(back, client, g_bg);
    DrawRoundBorder(back, client, S(18), g_border);

    // Wrapped defensively: this walks loaded file content (text runs
    // matched up against color spans, or a decoded image/SVG bitmap)
    // every time the panel repaints, which is far more often than it's
    // loaded — an exception escaping from here into the WM_PAINT
    // dispatch would crash the whole process, indistinguishable from the
    // user's side from the Start menu just closing. Falling back to the
    // plain background/border already drawn above is a safe, visible
    // "something went wrong showing this" rather than nothing at all.
    try
    {
    if (g_previewKind == PreviewKind::Image && g_previewImage)
    {
        Gdiplus::Graphics graphics(back);

        graphics.SetInterpolationMode(
            Gdiplus::InterpolationModeHighQualityBicubic);

        graphics.DrawImage(
            g_previewImage, 0, 0, width, height);
    }
    else if (g_previewKind == PreviewKind::Text)
    {
        SetBkMode(back, TRANSPARENT);
        HGDIOBJ oldFont = SelectObject(back, g_font);

        size_t spanCursor = 0;
        int y = S(10) - g_previewTextScroll;

        for (const PreviewDisplayLine& line : g_previewDisplayLines)
        {
            int x = S(10);
            size_t pos = line.start;
            size_t lineEnd = line.start + line.length;

            // Always walks the full line (even when scrolled off-screen
            // above/below the client rect) so spanCursor stays correctly
            // positioned for the next line — GDI itself clips any actual
            // drawing outside the target bitmap, so this costs a little
            // wasted measurement on off-screen lines rather than complex
            // bookkeeping to skip them.
            while (pos < lineEnd || (pos == lineEnd && line.length == 0))
            {
                while (spanCursor < g_previewColorSpans.size() &&
                       g_previewColorSpans[spanCursor].start +
                               g_previewColorSpans[spanCursor].length <= pos)
                {
                    spanCursor++;
                }

                size_t runEnd = lineEnd;
                COLORREF runColor = g_text;

                if (spanCursor < g_previewColorSpans.size())
                {
                    const PreviewColorSpan& span = g_previewColorSpans[spanCursor];

                    if (span.start <= pos)
                    {
                        runEnd = std::min(lineEnd, span.start + span.length);
                        runColor = PreviewTokenRGB(span.color);
                    }
                    else
                    {
                        runEnd = std::min(lineEnd, span.start);
                    }
                }

                if (runEnd > pos)
                {
                    SetTextColor(back, runColor);

                    RECT r = { x, y, x + 10000, y + g_previewLineHeight };

                    DrawTextW(
                        back,
                        g_previewText.c_str() + pos,
                        (int)(runEnd - pos),
                        &r,
                        DT_LEFT | DT_TOP | DT_SINGLELINE | DT_NOPREFIX | DT_NOCLIP);

                    SIZE sz{};

                    GetTextExtentPoint32W(
                        back, g_previewText.c_str() + pos, (int)(runEnd - pos), &sz);

                    x += sz.cx;
                }

                if (runEnd == pos)
                    break; // empty line

                pos = runEnd;
            }

            y += g_previewLineHeight;
        }

        SelectObject(back, oldFont);

        if (g_previewTextTruncated)
        {
            RECT tr = { S(10), height - S(20), width - S(10), height - S(4) };

            DrawTextSimple(
                back, L"(truncated)",
                tr.left, tr.top,
                tr.right - tr.left, tr.bottom - tr.top,
                g_muted, g_small,
                DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        }

        if (PreviewNeedsScrollbar(height - S(20)))
        {
            RECT track = GetPreviewScrollbarTrackRect();
            ScrollbarThumb thumb = GetPreviewScrollbarThumb();

            FillRoundRect(
                back, track, S(3),
                MixColor(g_bg, g_text, 8));

            RECT thumbRect =
            {
                track.left,
                track.top + thumb.top,
                track.right,
                track.top + thumb.top + thumb.height
            };

            FillRoundRect(
                back, thumbRect, S(3),
                g_previewScrollbarDragging
                    ? g_accent
                    : MixColor(g_bg, g_text, 35));
        }
    }
    }
    catch (...)
    {
        // Leave whatever was already drawn (the plain background/border)
        // rather than trying to draw an error message with the same
        // machinery that just failed.
    }

    BitBlt(dc, 0, 0, width, height, back, 0, 0, SRCCOPY);

    SelectObject(back, old);
    DeleteObject(bitmap);
    DeleteDC(back);
}

static LRESULT CALLBACK PreviewProc(
    HWND hwnd,
    UINT msg,
    WPARAM wp,
    LPARAM lp)
{
    switch (msg)
    {
        case WM_ERASEBKGND:
            return 1;

        case WM_PAINT:
        {
            PAINTSTRUCT ps{};
            HDC dc = BeginPaint(hwnd, &ps);

            if (dc)
                PaintPreview(hwnd, dc);

            EndPaint(hwnd, &ps);
            return 0;
        }

        case WM_MOUSEMOVE:
        {
            if (g_previewScrollbarDragging)
            {
                int y = GET_Y_LPARAM(lp);

                RECT client{};
                GetClientRect(hwnd, &client);

                RECT track = GetPreviewScrollbarTrackRect();
                ScrollbarThumb thumb = GetPreviewScrollbarThumb();

                int newThumbTop = y - track.top - g_previewScrollbarDragGrabOffset;

                g_previewTextScroll =
                    ScrollOffsetFromThumbTop(
                        newThumbTop,
                        g_previewContentH,
                        client.bottom - S(20),
                        track.bottom - track.top,
                        thumb.height);

                InvalidateRect(hwnd, nullptr, FALSE);
                return 0;
            }

            CancelPreviewFadeOut();

            TRACKMOUSEEVENT tme{};
            tme.cbSize = sizeof(tme);
            tme.dwFlags = TME_LEAVE;
            tme.hwndTrack = hwnd;
            TrackMouseEvent(&tme);

            return 0;
        }

        case WM_MOUSELEAVE:
            if (!g_previewScrollbarDragging)
                BeginPreviewFadeOut();
            return 0;

        case WM_LBUTTONDOWN:
        {
            RECT client{};
            GetClientRect(hwnd, &client);

            if (!PreviewNeedsScrollbar(client.bottom - S(20)))
                return 0;

            RECT track = GetPreviewScrollbarTrackRect();
            int y = GET_Y_LPARAM(lp);
            POINT pt{ GET_X_LPARAM(lp), y };

            if (!PtInRect(&track, pt))
                return 0;

            ScrollbarThumb thumb = GetPreviewScrollbarThumb();
            int thumbTop = track.top + thumb.top;

            g_previewScrollbarDragGrabOffset =
                (y >= thumbTop && y < thumbTop + thumb.height)
                    ? y - thumbTop
                    : thumb.height / 2;

            g_previewScrollbarDragging = true;
            SetCapture(hwnd);
            CancelPreviewFadeOut();

            int newThumbTop = y - track.top - g_previewScrollbarDragGrabOffset;

            g_previewTextScroll =
                ScrollOffsetFromThumbTop(
                    newThumbTop,
                    g_previewContentH,
                    client.bottom - S(20),
                    track.bottom - track.top,
                    thumb.height);

            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        }

        case WM_LBUTTONUP:
        {
            if (g_previewScrollbarDragging)
            {
                g_previewScrollbarDragging = false;
                ReleaseCapture();

                InvalidateRect(hwnd, nullptr, FALSE);
            }

            return 0;
        }

        case WM_CAPTURECHANGED:
        {
            g_previewScrollbarDragging = false;
            return 0;
        }

        case WM_MOUSEWHEEL:
        {
            if (g_previewKind != PreviewKind::Text)
                return 0;

            int delta = GET_WHEEL_DELTA_WPARAM(wp);

            g_previewTextScroll -=
                (delta / WHEEL_DELTA) * 3 * g_previewLineHeight;

            RECT client{};
            GetClientRect(hwnd, &client);

            int maxScroll =
                g_previewContentH - (client.bottom - S(20));

            if (maxScroll < 0)
                maxScroll = 0;

            if (g_previewTextScroll < 0)
                g_previewTextScroll = 0;

            if (g_previewTextScroll > maxScroll)
                g_previewTextScroll = maxScroll;

            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        }

        case WM_TIMER:
        {
            if (wp == TIMER_PREVIEW_FADE)
            {
                DWORD elapsed = GetTickCount() - g_previewFadeStartTick;

                if (elapsed < (DWORD)PREVIEW_FADE_GRACE_MS)
                    return 0;

                DWORD fadeElapsed = elapsed - PREVIEW_FADE_GRACE_MS;

                if (fadeElapsed >= (DWORD)PREVIEW_FADE_DURATION_MS)
                {
                    KillTimer(hwnd, TIMER_PREVIEW_FADE);
                    g_previewFadeTimer = 0;
                    HidePreview();
                    return 0;
                }

                g_previewAlpha =
                    1.0f - (float)fadeElapsed / (float)PREVIEW_FADE_DURATION_MS;

                ApplyPreviewAlpha();
                return 0;
            }

            break;
        }
    }

    return DefWindowProcW(hwnd, msg, wp, lp);
}

static bool CreatePreviewWindow(
    HINSTANCE instance)
{
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = PreviewProc;
    wc.hInstance = instance;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = PREVIEW_CLASS;

    if (!RegisterClassExW(&wc) &&
        GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
    {
        return false;
    }

    g_preview =
        CreateWindowExW(
            WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_LAYERED | WS_EX_NOACTIVATE,
            PREVIEW_CLASS,
            L"ClassicShell Preview",
            WS_POPUP,
            0, 0, 10, 10,
            nullptr, nullptr, instance, nullptr);

    if (!g_preview)
        return false;

    SetLayeredWindowAttributes(g_preview, 0, PREVIEW_BASE_ALPHA, LWA_ALPHA);
    ApplyWindowRounding(g_preview);
    ApplyAcrylicBlur(g_preview);

    return true;
}

static RECT GetPowerButtonRect(
    int width,
    int height)
{
    return
    {
        width - S(50),
        height - S(38),
        width - S(10),
        height - S(8)
    };
}

static const int QUICK_TOOL_SIZE = 24;
static const int QUICK_TOOL_GAP = 6;

// Small square quick-launch buttons, left-aligned in the bottom
// strip. Only present when the user has configured any.
static RECT GetQuickToolRect(
    int height,
    int index)
{
    // Matches the vertical center of GetPowerButtonRect, which
    // doesn't depend on width.
    int cy =
        height - S(23);

    int size =
        S(QUICK_TOOL_SIZE);

    int x =
        S(14) +
        index *
            (size + S(QUICK_TOOL_GAP));

    return
    {
        x,
        cy - size / 2,
        x + size,
        cy + size / 2
    };
}

static int QuickToolsWidth()
{
    if (g_quickToolCount <= 0)
        return 0;

    return
        g_quickToolCount *
            (S(QUICK_TOOL_SIZE) +
                S(QUICK_TOOL_GAP)) +
        S(8);
}

// Thin opacity-adjustment track filling the space between the
// quick-launch buttons (if any) and the power button, in the
// same bottom strip.
static RECT GetOpacitySliderRect(
    int width,
    int height)
{
    RECT power =
        GetPowerButtonRect(
            width,
            height);

    int cy =
        (power.top + power.bottom) / 2;

    return
    {
        S(14) + QuickToolsWidth(),
        cy - S(2),
        power.left - S(16),
        cy + S(2)
    };
}

static float OpacityToFraction(
    BYTE alpha)
{
    float t =
        (float)(alpha - OPACITY_MIN) /
        (float)(OPACITY_MAX - OPACITY_MIN);

    if (t < 0.0f)
        t = 0.0f;

    if (t > 1.0f)
        t = 1.0f;

    return t;
}

static int GetSliderThumbX(
    const RECT& track)
{
    float t =
        OpacityToFraction(
            g_windowAlpha);

    return
        track.left +
        (int)(
            t *
            (track.right - track.left));
}

static BYTE FractionToOpacity(
    float t)
{
    if (t < 0.0f)
        t = 0.0f;

    if (t > 1.0f)
        t = 1.0f;

    return (BYTE)(
        OPACITY_MIN +
        t * (OPACITY_MAX - OPACITY_MIN));
}

// ============================================================
// Opacity slider retargeting — hover the slider and scroll to cycle
// through every other real window on the desktop (including the
// taskbar), controlling that window's opacity instead of the Start
// menu's own. Scrolling back down returns to index 0, the Start menu
// itself. A toast in the opposite corner names the current target, and
// an accent-colored outline follows it live.
// ============================================================

static const wchar_t TOAST_CLASS[] = L"ClassicShell.Toast";
static HWND g_toast = nullptr;
static std::wstring g_toastTitle;
static std::wstring g_toastDetail;
static float g_toastAlpha = 1.0f;
static DWORD g_toastStartTick = 0;
static UINT_PTR g_toastTimer = 0;
static const UINT_PTR TIMER_TOAST = 8;
static const BYTE TOAST_BASE_ALPHA = 235;
static const int TOAST_HOLD_MS = 1400;
static const int TOAST_FADE_MS = 300;

static const wchar_t OPACITY_HIGHLIGHT_CLASS[] = L"ClassicShell.OpacityHighlight";
static HWND g_opacityHighlight = nullptr;
static HWND g_opacityHighlightTarget = nullptr;
static UINT_PTR g_opacityTrackTimer = 0;
static const UINT_PTR TIMER_OPACITY_TRACK = 9;

static int g_opacityIndex = 0;
static std::vector<HWND> g_opacityExternalWindows;
static BYTE g_startOwnAlpha = START_WINDOW_ALPHA;
static bool g_sliderHover = false;

static RECT GetToastRect()
{
    RECT work{};

    if (!GetWorkArea(work))
        work = { 0, 0, 1920, 1080 };

    int width = S(300);
    int height = S(64);
    int margin = S(8);

    return
    {
        work.right - margin - width,
        work.top + margin,
        work.right - margin,
        work.top + margin + height
    };
}

static void PaintToast(
    HWND hwnd,
    HDC dc)
{
    RECT client{};
    GetClientRect(hwnd, &client);

    int width = client.right;
    int height = client.bottom;

    HDC back = CreateCompatibleDC(dc);

    if (!back)
        return;

    HBITMAP bitmap = CreateCompatibleBitmap(dc, width, height);

    if (!bitmap)
    {
        DeleteDC(back);
        return;
    }

    HGDIOBJ old = SelectObject(back, bitmap);

    FillRectColor(back, client, g_bg);
    DrawRoundBorder(back, client, S(14), g_border);

    DrawTextSimple(
        back, g_toastTitle.c_str(),
        S(14), S(8), width - S(28), S(24),
        g_text, g_bold,
        DT_LEFT | DT_TOP | DT_SINGLELINE | DT_END_ELLIPSIS);

    DrawTextSimple(
        back, g_toastDetail.c_str(),
        S(14), S(32), width - S(28), height - S(40),
        g_muted, g_small,
        DT_LEFT | DT_TOP | DT_WORDBREAK);

    BitBlt(dc, 0, 0, width, height, back, 0, 0, SRCCOPY);

    SelectObject(back, old);
    DeleteObject(bitmap);
    DeleteDC(back);
}

static LRESULT CALLBACK ToastProc(
    HWND hwnd,
    UINT msg,
    WPARAM wp,
    LPARAM lp)
{
    switch (msg)
    {
        case WM_ERASEBKGND:
            return 1;

        case WM_PAINT:
        {
            PAINTSTRUCT ps{};
            HDC dc = BeginPaint(hwnd, &ps);

            if (dc)
                PaintToast(hwnd, dc);

            EndPaint(hwnd, &ps);
            return 0;
        }
    }

    return DefWindowProcW(hwnd, msg, wp, lp);
}

static bool CreateToastWindow(
    HINSTANCE instance)
{
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = ToastProc;
    wc.hInstance = instance;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = TOAST_CLASS;

    if (!RegisterClassExW(&wc) &&
        GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
    {
        return false;
    }

    g_toast =
        CreateWindowExW(
            WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_LAYERED | WS_EX_NOACTIVATE,
            TOAST_CLASS,
            L"ClassicShell Toast",
            WS_POPUP,
            0, 0, 10, 10,
            nullptr, nullptr, instance, nullptr);

    if (!g_toast)
        return false;

    ApplyWindowRounding(g_toast);
    ApplyAcrylicBlur(g_toast);

    return true;
}

// Pops (or replaces) the toast with a new message. Safe to call
// repeatedly — each call just restarts the hold-then-fade clock.
static void ShowToast(
    const std::wstring& title,
    const std::wstring& detail)
{
    if (!g_toast || !g_start)
        return;

    g_toastTitle = title;
    g_toastDetail = detail;
    g_toastAlpha = 1.0f;
    g_toastStartTick = GetTickCount();

    RECT r = GetToastRect();

    SetWindowPos(
        g_toast, HWND_TOPMOST,
        r.left, r.top,
        r.right - r.left, r.bottom - r.top,
        SWP_SHOWWINDOW | SWP_NOACTIVATE);

    // Reapplied every time the toast is shown, not just once at
    // creation — see the identical note in RepositionSearchPanel for why.
    ApplyAcrylicBlur(g_toast);
    SetLayeredWindowAttributes(g_toast, 0, TOAST_BASE_ALPHA, LWA_ALPHA);

    InvalidateRect(g_toast, nullptr, FALSE);

    if (g_toastTimer)
        KillTimer(g_start, TIMER_TOAST);

    g_toastTimer = SetTimer(g_start, TIMER_TOAST, 16, nullptr);
}

static void HideOpacityHighlight()
{
    if (g_opacityTrackTimer)
    {
        if (g_start)
            KillTimer(g_start, TIMER_OPACITY_TRACK);

        g_opacityTrackTimer = 0;
    }

    if (g_opacityHighlight)
        ShowWindow(g_opacityHighlight, SW_HIDE);
}

// Stops actively tracking whichever window the slider was last pointed
// at — hides the highlight border and stops re-polling its rect —
// without touching its actual opacity at all.
static void StopOpacityHighlight()
{
    g_opacityHighlightTarget = nullptr;
    HideOpacityHighlight();
}

// Renders the border into a true per-pixel-alpha bitmap via
// UpdateLayeredWindow rather than WM_PAINT + color-key: DWM's own corner
// rounding (ApplyWindowRounding) clips with an anti-aliased mask, and a
// color-keyed window has no real alpha for DWM to blend that mask
// against, so the 1px stroke would come out faded right at the corners.
static void RenderOpacityHighlight(
    int width,
    int height,
    int screenX,
    int screenY)
{
    if (!g_opacityHighlight || width <= 0 || height <= 0)
        return;

    // Premultiplied alpha — UpdateLayeredWindow's ULW_ALPHA blend
    // (AC_SRC_ALPHA) requires it; straight (non-premultiplied) alpha
    // here would show as a faint fringe around the anti-aliased edge of
    // the stroke.
    Gdiplus::Bitmap bitmap(width, height, PixelFormat32bppPARGB);

    {
        Gdiplus::Graphics graphics(&bitmap);

        graphics.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
        graphics.Clear(Gdiplus::Color(0, 0, 0, 0));

        Gdiplus::Pen pen(
            Gdiplus::Color(
                255, GetRValue(g_accent), GetGValue(g_accent), GetBValue(g_accent)),
            2.0f);

        graphics.DrawRectangle(
            &pen, 1.0f, 1.0f, (float)(width - 2), (float)(height - 2));
    }

    Gdiplus::Rect fullRect(0, 0, width, height);
    Gdiplus::BitmapData bmpData{};

    if (bitmap.LockBits(
            &fullRect, Gdiplus::ImageLockModeRead,
            PixelFormat32bppPARGB, &bmpData) != Gdiplus::Ok)
    {
        return;
    }

    HDC screenDc = GetDC(nullptr);
    HDC memDc = CreateCompatibleDC(screenDc);

    BITMAPINFO bmi{};
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = width;
    bmi.bmiHeader.biHeight = -height;
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;

    void* dibBits = nullptr;
    HBITMAP dib = CreateDIBSection(screenDc, &bmi, DIB_RGB_COLORS, &dibBits, nullptr, 0);

    if (dib && dibBits)
    {
        for (int y = 0; y < height; ++y)
        {
            memcpy(
                (BYTE*)dibBits + (size_t)y * width * 4,
                (BYTE*)bmpData.Scan0 + (size_t)y * bmpData.Stride,
                (size_t)width * 4);
        }

        HGDIOBJ oldBitmap = SelectObject(memDc, dib);

        POINT destPos{ screenX, screenY };
        POINT srcPos{ 0, 0 };
        SIZE sz{ width, height };

        BLENDFUNCTION blend{};
        blend.BlendOp = AC_SRC_OVER;
        blend.SourceConstantAlpha = 255;
        blend.AlphaFormat = AC_SRC_ALPHA;

        UpdateLayeredWindow(
            g_opacityHighlight, screenDc, &destPos, &sz,
            memDc, &srcPos, 0, &blend, ULW_ALPHA);

        SelectObject(memDc, oldBitmap);
        DeleteObject(dib);
    }

    bitmap.UnlockBits(&bmpData);

    DeleteDC(memDc);
    ReleaseDC(nullptr, screenDc);
}

static void RepositionOpacityHighlight(
    HWND target)
{
    if (!g_opacityHighlight || !target)
        return;

    RECT r{};

    if (!GetWindowRect(target, &r))
        return;

    ShowWindow(g_opacityHighlight, SW_SHOWNOACTIVATE);

    RenderOpacityHighlight(r.right - r.left, r.bottom - r.top, r.left, r.top);

    SetWindowPos(
        g_opacityHighlight, HWND_TOPMOST,
        0, 0, 0, 0,
        SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
}

// Shows the highlight for whatever's currently targeted without
// changing what that is — defaults to the Start menu itself the first
// time this runs in a given session. Pairs with HideOpacityHighlight(),
// so hovering away and back always resumes on the same target.
static void ResumeOpacityHighlight()
{
    if (!g_opacityHighlightTarget)
        g_opacityHighlightTarget = g_start;

    RepositionOpacityHighlight(g_opacityHighlightTarget);

    if (g_start && !g_opacityTrackTimer)
        g_opacityTrackTimer = SetTimer(g_start, TIMER_OPACITY_TRACK, 150, nullptr);
}

// Points the highlight at the Start menu itself — index 0. No
// WS_EX_LAYERED/alpha bookkeeping needed unlike SetExternalOpacityTarget:
// g_start is already layered, and its own opacity is handled by
// ApplyOpacityToCurrentTarget's index==0 case.
static void SetStartAsOpacityTarget()
{
    g_opacityHighlightTarget = g_start;
    ResumeOpacityHighlight();
}

// Makes hwnd the slider's active external target: adds WS_EX_LAYERED if
// it doesn't already have it (left in place afterward — undoing it on
// every scroll-past would be needless churn on someone else's window),
// reads its current opacity so the slider starts exactly where that
// window already was, and moves the highlight border onto it.
static void SetExternalOpacityTarget(
    HWND hwnd)
{
    if (hwnd == g_opacityHighlightTarget)
        return;

    StopOpacityHighlight();

    if (!hwnd)
        return;

    LONG_PTR ex = GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
    BYTE currentAlpha = 255;

    if (ex & WS_EX_LAYERED)
    {
        BYTE a = 255;
        DWORD flags = 0;
        COLORREF key = 0;

        if (GetLayeredWindowAttributes(hwnd, &key, &a, &flags) &&
            (flags & LWA_ALPHA))
        {
            currentAlpha = a;
        }
    }
    else
    {
        SetWindowLongPtrW(hwnd, GWL_EXSTYLE, ex | WS_EX_LAYERED);

        SetWindowPos(
            hwnd, nullptr, 0, 0, 0, 0,
            SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER |
                SWP_NOACTIVATE | SWP_FRAMECHANGED);
    }

    g_opacityHighlightTarget = hwnd;
    g_windowAlpha = currentAlpha;

    SetLayeredWindowAttributes(hwnd, 0, g_windowAlpha, LWA_ALPHA);

    RepositionOpacityHighlight(hwnd);

    if (g_start && !g_opacityTrackTimer)
        g_opacityTrackTimer = SetTimer(g_start, TIMER_OPACITY_TRACK, 150, nullptr);
}

// Same filtering an Alt-Tab-style window switcher uses: visible,
// unowned, not a tool window, and not cloaked (a suspended UWP app or a
// window parked on another virtual desktop). Unlike a plain Alt-Tab
// list, the taskbar itself is deliberately included — see the taskbar
// special-case in EnumOpacityCandidateProc below — since being able to
// control its opacity was explicitly asked for.
static const wchar_t* const OPACITY_EXCLUDED_CLASSES[] =
{
    L"Progman",
    L"WorkerW",
    L"Windows.UI.Core.CoreWindow",
    L"DesktopWindowXamlSource",
    L"tooltips_class32",
    L"#32768",
};

static BOOL CALLBACK EnumOpacityCandidateProc(
    HWND hwnd,
    LPARAM lParam)
{
    auto* list = reinterpret_cast<std::vector<HWND>*>(lParam);

    if (hwnd == g_start ||
        hwnd == g_preview ||
        hwnd == g_toast ||
        hwnd == g_searchPanel ||
        hwnd == g_opacityHighlight)
    {
        return TRUE;
    }

    DWORD ownerPid = 0;
    GetWindowThreadProcessId(hwnd, &ownerPid);

    if (ownerPid == GetCurrentProcessId())
        return TRUE;

    if (!IsWindowVisible(hwnd))
        return TRUE;

    // A minimized window's "position" is a meaningless off-screen
    // sentinel — nothing sensible to frame with the highlight.
    if (IsIconic(hwnd))
        return TRUE;

    wchar_t className[64]{};

    GetClassNameW(hwnd, className, (int)(sizeof(className) / sizeof(className[0])));

    bool isTaskbar =
        _wcsicmp(className, L"Shell_TrayWnd") == 0 ||
        _wcsicmp(className, L"Shell_SecondaryTrayWnd") == 0;

    if (!isTaskbar)
    {
        if (GetWindow(hwnd, GW_OWNER) != nullptr)
            return TRUE;

        LONG_PTR ex = GetWindowLongPtrW(hwnd, GWL_EXSTYLE);

        if (ex & WS_EX_TOOLWINDOW)
            return TRUE;

        if (GetWindowTextLengthW(hwnd) == 0)
            return TRUE;

        for (const wchar_t* excluded : OPACITY_EXCLUDED_CLASSES)
        {
            if (_wcsicmp(className, excluded) == 0)
                return TRUE;
        }

        BOOL cloaked = FALSE;

        DwmGetWindowAttribute(hwnd, DWMWA_CLOAKED, &cloaked, sizeof(cloaked));

        if (cloaked)
            return TRUE;
    }

    RECT r{};

    if (!GetWindowRect(hwnd, &r) || r.right <= r.left || r.bottom <= r.top)
        return TRUE;

    list->push_back(hwnd);

    return TRUE;
}

static void RefreshOpacityCandidates()
{
    g_opacityExternalWindows.clear();

    EnumWindows(
        EnumOpacityCandidateProc,
        reinterpret_cast<LPARAM>(&g_opacityExternalWindows));
}

// Display name for the toast: real windows use their own title; the
// taskbar has none, so it gets a synthetic, human-readable label.
static std::wstring OpacityTargetDisplayName(
    HWND hwnd)
{
    wchar_t className[64]{};

    GetClassNameW(hwnd, className, (int)(sizeof(className) / sizeof(className[0])));

    if (_wcsicmp(className, L"Shell_TrayWnd") == 0)
        return L"Taskbar";

    if (_wcsicmp(className, L"Shell_SecondaryTrayWnd") == 0)
        return L"Taskbar (other monitor)";

    wchar_t caption[256]{};

    GetWindowTextW(hwnd, caption, (int)(sizeof(caption) / sizeof(caption[0])));

    return caption[0] ? caption : L"(untitled window)";
}

// Moves the slider's target by one step in either direction — called
// once per wheel notch while hovering the slider. The candidate list is
// (re)built fresh the moment it's first needed scrolling up from index
// 0, rather than kept constantly up to date, so it's never stale by
// more than the current scroll session.
static void CycleOpacityTarget(
    int direction)
{
    if (g_opacityIndex == 0 && direction > 0)
        RefreshOpacityCandidates();

    int maxIndex = (int)g_opacityExternalWindows.size();
    int newIndex = g_opacityIndex + direction;

    if (newIndex < 0) newIndex = 0;
    if (newIndex > maxIndex) newIndex = maxIndex;

    // A candidate can vanish between being listed and being scrolled to
    // (closed, or otherwise stopped existing) — skip past any dead ones
    // in the same direction rather than landing on a dangling handle.
    while (newIndex > 0 &&
           newIndex <= maxIndex &&
           !IsWindow(g_opacityExternalWindows[newIndex - 1]))
    {
        g_opacityExternalWindows.erase(g_opacityExternalWindows.begin() + (newIndex - 1));
        maxIndex--;

        if (newIndex > maxIndex)
            newIndex = maxIndex;
    }

    if (newIndex == g_opacityIndex)
        return;

    g_opacityIndex = newIndex;

    if (g_opacityIndex == 0)
    {
        g_windowAlpha = g_startOwnAlpha;

        SetStartAsOpacityTarget();

        ShowToast(
            L"Start Menu",
            L"Scroll to reach another window, or drag the slider to "
            L"adjust this one.");
    }
    else
    {
        HWND target = g_opacityExternalWindows[g_opacityIndex - 1];

        SetExternalOpacityTarget(target);

        ShowToast(
            OpacityTargetDisplayName(target),
            L"Scroll to browse windows, or drag the slider to adjust "
            L"this one's opacity.");
    }

    if (g_start)
        InvalidateRect(g_start, nullptr, FALSE);
}

// Applies g_windowAlpha to whatever's currently targeted — the one
// place both the mouse-drag and keyboard paths funnel through, so
// dragging the slider or nudging it with the arrow keys behaves
// identically regardless of which window is on the other end.
static void ApplyOpacityToCurrentTarget()
{
    HWND target =
        (g_opacityIndex > 0 && g_opacityHighlightTarget)
            ? g_opacityHighlightTarget
            : g_start;

    if (target)
        SetLayeredWindowAttributes(target, 0, g_windowAlpha, LWA_ALPHA);

    if (g_opacityIndex == 0)
        g_startOwnAlpha = g_windowAlpha;
}

static bool CreateOpacityHighlightWindow(
    HINSTANCE instance)
{
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = DefWindowProcW;
    wc.hInstance = instance;
    wc.lpszClassName = OPACITY_HIGHLIGHT_CLASS;

    if (!RegisterClassExW(&wc) &&
        GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
    {
        return false;
    }

    g_opacityHighlight =
        CreateWindowExW(
            WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_LAYERED |
                WS_EX_NOACTIVATE | WS_EX_TRANSPARENT,
            OPACITY_HIGHLIGHT_CLASS,
            L"ClassicShell Opacity Highlight",
            WS_POPUP,
            0, 0, 10, 10,
            nullptr, nullptr, instance, nullptr);

    return g_opacityHighlight != nullptr;
}

static void UpdateSliderFromX(
    HWND hwnd,
    int x)
{
    RECT client{};

    GetClientRect(
        hwnd,
        &client);

    RECT track =
        GetOpacitySliderRect(
            client.right,
            client.bottom);

    float t =
        (float)(x - track.left) /
        (float)(track.right - track.left);

    g_windowAlpha =
        FractionToOpacity(t);

    ApplyOpacityToCurrentTarget();

    InvalidateRect(
        hwnd,
        nullptr,
        FALSE);
}

// Power flyout action indices, used consistently across hit
// testing, drawing, and keyboard activation.
static const int POWER_ACTION_RESTART = 0;
static const int POWER_ACTION_SHUTDOWN = 1;
static const int POWER_ACTION_SIGNOUT = 2;
static const int POWER_ACTION_LOCK = 3;
static const int POWER_ACTION_COUNT = 4;

// Sentinel for "hovering the main power toggle button" in
// g_powerHover, kept out of the 0..POWER_ACTION_COUNT-1 range so it
// can never be mistaken for one of the flyout action buttons (e.g.
// POWER_ACTION_SIGNOUT == 2 would otherwise collide).
static const int POWER_HOVER_MAIN_BUTTON = 100;

static RECT GetPowerMenuRect(
    int width,
    int height)
{
    RECT button =
        GetPowerButtonRect(
            width,
            height);

    int menuWidth =
        S(71);

    int menuHeight =
        S(71);

    return
    {
        button.right + S(2) - menuWidth,
        button.top - S(6) - menuHeight,
        button.right + S(2),
        button.top - S(6)
    };
}

// A 2x2 grid: restart/shutdown on top, sign out/lock below.
static RECT GetPowerActionRect(
    int width,
    int height,
    int index)
{
    RECT menu =
        GetPowerMenuRect(
            width,
            height);

    int btn =
        S(28);

    int pad =
        S(5);

    int gap =
        S(5);

    int col =
        index % 2;

    int row =
        index / 2;

    int x =
        menu.left +
        pad +
        col * (btn + gap);

    int y =
        menu.top +
        pad +
        row * (btn + gap);

    return
    {
        x,
        y,
        x + btn,
        y + btn
    };
}

// ============================================================
// Hit testing
// ============================================================

static int HitStartItem(
    int y)
{
    const int top =
        MenuTop();

    const int row =
        MenuRow();

    const int gap =
        MenuGap();

    if (y < top)
        return -1;

    int index =
        (y - top) /
        (row + gap);

    if (index < 0 ||
        index >= g_itemCount)
    {
        return -1;
    }

    int actual =
        top +
        index * (row + gap);

    return
        (y >= actual &&
         y < actual + row)
            ? index
            : -1;
}

static int HitPowerAction(
    int x,
    int y,
    int width,
    int height)
{
    if (!g_powerOpen)
        return -1;

    POINT p{ x, y };

    for (int i = 0;
         i < POWER_ACTION_COUNT;
         ++i)
    {
        RECT r =
            GetPowerActionRect(
                width,
                height,
                i);

        if (PtInRect(
                &r,
                p))
        {
            return i;
        }
    }

    return -1;
}

// ============================================================
// Power
// ============================================================

static void ExecutePowerAction(
    int action)
{
    ResetUIState();
    g_powerHover = -1;

    if (g_start)
        ShowWindow(
            g_start,
            SW_HIDE);

    g_startVisible = false;
    g_hover = -1;

    if (action == POWER_ACTION_RESTART)
    {
        LaunchShell(
            L"shutdown.exe",
            L"/r /t 0");
    }
    else if (action == POWER_ACTION_SHUTDOWN)
    {
        LaunchShell(
            L"shutdown.exe",
            L"/s /t 0");
    }
    else if (action == POWER_ACTION_SIGNOUT)
    {
        ExitWindowsEx(
            EWX_LOGOFF,
            0);
    }
    else if (action == POWER_ACTION_LOCK)
    {
        LockWorkStation();
    }
}

static void DrawIconGlyph(
    HDC dc,
    const RECT& r,
    COLORREF color,
    const wchar_t* glyph)
{
    HFONT old =
        (HFONT)SelectObject(
            dc,
            g_icon);

    COLORREF oldColor =
        SetTextColor(
            dc,
            color);

    int oldMode =
        SetBkMode(
            dc,
            TRANSPARENT);

    RECT rect = r;

    DrawTextW(
        dc,
        glyph,
        -1,
        &rect,
        DT_CENTER |
            DT_VCENTER |
            DT_SINGLELINE |
            DT_NOPREFIX);

    SetBkMode(
        dc,
        oldMode);

    SetTextColor(
        dc,
        oldColor);

    SelectObject(
        dc,
        old);
}

static void DrawPowerIcon(
    HDC dc,
    const RECT& r,
    COLORREF color)
{
    DrawIconGlyph(
        dc,
        r,
        color,
        L"\uE7E8");
}

static void DrawRestartIcon(
    HDC dc,
    const RECT& r,
    COLORREF color)
{
    DrawIconGlyph(
        dc,
        r,
        color,
        L"\uE72C");
}

static void DrawShutdownIcon(
    HDC dc,
    const RECT& r,
    COLORREF color)
{
    DrawIconGlyph(
        dc,
        r,
        color,
        L"\uE7E8");
}

static void DrawSignOutIcon(
    HDC dc,
    const RECT& r,
    COLORREF color)
{
    DrawIconGlyph(
        dc,
        r,
        color,
        L"\uF3B1");
}

static void DrawLockIcon(
    HDC dc,
    const RECT& r,
    COLORREF color)
{
    DrawIconGlyph(
        dc,
        r,
        color,
        L"\uE72E");
}

// ============================================================
// Rounded window styling
// ============================================================

static void ApplyWindowRounding(
    HWND hwnd)
{
    if (!hwnd)
        return;

    // Windows 11.
    // Dynamically resolve this so the binary remains usable
    // on older Windows versions.
    HMODULE dwm =
        GetModuleHandleW(
            L"dwmapi.dll");

    if (!dwm)
        dwm =
            LoadLibraryW(
                L"dwmapi.dll");

    if (dwm)
    {
        using DwmSetWindowAttributeFn =
            HRESULT(WINAPI*)(
                HWND,
                DWORD,
                LPCVOID,
                DWORD);

        auto proc =
            reinterpret_cast<
                DwmSetWindowAttributeFn>(
                GetProcAddress(
                    dwm,
                    "DwmSetWindowAttribute"));

        if (proc)
        {
            // DWMWA_WINDOW_CORNER_PREFERENCE = 33
            constexpr DWORD
                DWMWA_WINDOW_CORNER_PREFERENCE_LOCAL =
                    33;

            // DWMWCP_ROUND = 2
            constexpr DWORD
                DWMWCP_ROUND_LOCAL =
                    2;

            proc(
                hwnd,
                DWMWA_WINDOW_CORNER_PREFERENCE_LOCAL,
                &DWMWCP_ROUND_LOCAL,
                sizeof(DWMWCP_ROUND_LOCAL));

            // DWMWA_BORDER_COLOR = 34
            constexpr DWORD
                DWMWA_BORDER_COLOR_LOCAL =
                    34;

            // DWMWA_COLOR_NONE
            constexpr COLORREF
                DWMWA_COLOR_NONE_LOCAL =
                    0xFFFFFFFE;

            proc(
                hwnd,
                DWMWA_BORDER_COLOR_LOCAL,
                &DWMWA_COLOR_NONE_LOCAL,
                sizeof(DWMWA_COLOR_NONE_LOCAL));
        }
    }

    // Fallback clipping. This is deliberately fairly subtle;
    // DWM handles the actual Windows 11 rounded presentation.
    HRGN region =
        CreateRoundRectRgn(
            0,
            0,
            10000,
            10000,
            S(18),
            S(18));

    if (region)
    {
        SetWindowRgn(
            hwnd,
            region,
            TRUE);
    }
}

// ============================================================
// Acrylic blur-behind
// ============================================================

// Samples the taskbar's own actually-rendered pixels and averages them,
// rather than computing a guessed tint from theme/accent registry
// values — the taskbar's real translucency depends on the live
// wallpaper, lighting, and DWM's own blur math in ways that are much
// simpler to copy directly than to re-derive. Averaged over a small
// block (not one pixel) since the taskbar's surface isn't a flat color.
// Falls back to `fallback` if the taskbar can't be found or sampling
// fails for any reason.
static COLORREF SampleTaskbarColor(
    COLORREF fallback)
{
    HWND taskbar =
        FindWindowW(L"Shell_TrayWnd", nullptr);

    if (!taskbar)
        return fallback;

    RECT taskbarRect{};

    if (!GetWindowRect(taskbar, &taskbarRect))
        return fallback;

    // A point well inside the taskbar's own strip, away from the very
    // edges (which can show a border/seam pixel) and away from the
    // Start button/tray icons clustered at the ends.
    int sampleX = taskbarRect.left + (taskbarRect.right - taskbarRect.left) / 3;
    int sampleY = (taskbarRect.top + taskbarRect.bottom) / 2;

    // Guard against ever sampling our own window instead of the real
    // taskbar, in case of an unusual taskbar position/size.
    if (g_start)
    {
        RECT startRect{};

        if (GetWindowRect(g_start, &startRect))
        {
            POINT p{ sampleX, sampleY };

            if (PtInRect(&startRect, p))
                return fallback;
        }
    }

    HDC screenDc = GetDC(nullptr);

    if (!screenDc)
        return fallback;

    long rSum = 0, gSum = 0, bSum = 0;
    int count = 0;

    for (int dx = -12; dx <= 12; dx += 6)
    {
        for (int dy = -6; dy <= 6; dy += 6)
        {
            COLORREF px = GetPixel(screenDc, sampleX + dx, sampleY + dy);

            if (px == CLR_INVALID)
                continue;

            rSum += GetRValue(px);
            gSum += GetGValue(px);
            bSum += GetBValue(px);
            count++;
        }
    }

    ReleaseDC(nullptr, screenDc);

    if (count == 0)
        return fallback;

    return RGB(rSum / count, gSum / count, bSum / count);
}

static void ApplyAcrylicBlur(
    HWND hwnd)
{
    if (!hwnd)
        return;

    // Undocumented but long-stable user32 API used throughout the
    // Windows tooling ecosystem to get a real frosted-glass blur
    // behind a classic GDI window. Dynamically resolved so the
    // binary still runs (without the blur) on systems where it's
    // missing.
    HMODULE user32 =
        GetModuleHandleW(
            L"user32.dll");

    if (!user32)
        return;

    struct AccentPolicyLocal
    {
        int AccentState;
        int AccentFlags;
        int GradientColor;
        int AnimationId;
    };

    struct CompositionAttribDataLocal
    {
        int Attrib;
        void* Data;
        size_t DataSize;
    };

    using SetWindowCompositionAttributeFn =
        BOOL(WINAPI*)(
            HWND,
            CompositionAttribDataLocal*);

    auto proc =
        reinterpret_cast<
            SetWindowCompositionAttributeFn>(
            GetProcAddress(
                user32,
                "SetWindowCompositionAttribute"));

    if (!proc)
        return;

    // ACCENT_ENABLE_ACRYLICBLURBEHIND = 4
    // WCA_ACCENT_POLICY = 19
    constexpr int
        ACCENT_ENABLE_ACRYLICBLURBEHIND_LOCAL =
            4;

    constexpr int
        WCA_ACCENT_POLICY_LOCAL =
            19;

    // GradientColor is 0xAABBGGRR. COLORREF is already 0x00BBGGRR, so
    // only the alpha byte needs adding on top. The base tint is sampled
    // directly from the real taskbar's own rendered pixels (see
    // SampleTaskbarColor), so it matches the live wallpaper/lighting
    // behind it — but the raw sampled pixels mostly show blurred
    // wallpaper, not the user's chosen accent color (that only shows up
    // there if "Show accent color on Start, taskbar" is on), so it's
    // blended with the real accent color afterward to keep the tint
    // clearly reading as *this theme's* highlight color rather than just
    // whatever happened to be on screen. Falls back to a panel/accent
    // blend if the taskbar can't be sampled at all. This only changes
    // what DWM blends *behind* the window, not any of this app's own
    // opaque panel/text rendering, so it can't touch text legibility the
    // way a base-palette change could.
    COLORREF sampledTint =
        SampleTaskbarColor(g_panel);

    COLORREF finalTint =
        MixColor(sampledTint, g_accent, 35);

    DWORD tint =
        (DWORD)finalTint &
        0x00FFFFFF;

    DWORD gradientColor =
        (0x78u << 24) |
        tint;

    AccentPolicyLocal accent{};

    accent.AccentState =
        ACCENT_ENABLE_ACRYLICBLURBEHIND_LOCAL;

    accent.GradientColor =
        (int)gradientColor;

    CompositionAttribDataLocal data{};

    data.Attrib =
        WCA_ACCENT_POLICY_LOCAL;

    data.Data = &accent;

    data.DataSize =
        sizeof(accent);

    proc(
        hwnd,
        &data);
}

// ============================================================
// Close
// ============================================================

static void CloseStart()
{
    if (g_start &&
        g_startVisible)
    {
        g_showAnimMode =
            ShowAnimMode::Closing;

        if (!g_showAnimTimer)
        {
            g_showAnimTimer =
                SetTimer(
                    g_start,
                    TIMER_SHOW_ANIM,
                    12,
                    nullptr);
        }
    }

    g_startVisible = false;
    ResetUIState();

    g_hover = -1;
    g_powerHover = -1;

    CancelPendingBlurClose();
    StopOpacityHighlight();

    if (g_toast)
    {
        if (g_toastTimer)
        {
            KillTimer(g_start, TIMER_TOAST);
            g_toastTimer = 0;
        }

        ShowWindow(g_toast, SW_HIDE);
    }

    RepositionSearchPanel();
}

// Runs whatever a menu item does, shared by mouse clicks and
// keyboard activation (Enter/Space while it has focus).
static void ActivateItem(
    int item)
{
    if (item < 0 ||
        item >= g_itemCount)
    {
        return;
    }

    std::wstring command =
        g_items[item].command;

    if (command == L"run")
    {
        OpenNativeRun();
        return;
    }

    // Programs is the only item with no command — it opens the
    // full apps list instead of resolving anything.
    if (command.empty())
    {
        ShellExecuteW(
            nullptr,
            L"open",
            L"shell:AppsFolder",
            nullptr,
            nullptr,
            SW_SHOWNORMAL);

        CloseStart();

        return;
    }

    // A disclosure, not a launcher: toggles the full God Mode Control
    // Panel listing into the search results panel, keeping the menu
    // open rather than closing it like every other item.
    if (command == L"control panel")
    {
        ToggleControlPanelBrowse();
        return;
    }

    ExecuteSmartInput(
        command);

    CloseStart();
}

// ============================================================
// Keyboard focus navigation
// ============================================================

enum class FocusKind
{
    None,
    Search,
    SearchResult,
    Item,
    Tool,
    Slider,
    Power,
    PowerAction
};

struct FocusTarget
{
    FocusKind kind;
    int index;
};

static int FocusCount()
{
    // search + search results + items + quick tools + slider + power
    int n =
        1 +
        (int)g_searchResults.size() +
        g_itemCount +
        g_quickToolCount +
        1 +
        1;

    if (g_powerOpen)
        n += POWER_ACTION_COUNT;

    return n;
}

static FocusTarget ResolveFocus(
    int flat)
{
    int count =
        FocusCount();

    if (flat < 0 ||
        flat >= count)
    {
        return { FocusKind::None, 0 };
    }

    if (flat == 0)
        return { FocusKind::Search, 0 };

    flat -= 1;

    if (flat < (int)g_searchResults.size())
        return { FocusKind::SearchResult, flat };

    flat -= (int)g_searchResults.size();

    if (flat < g_itemCount)
        return { FocusKind::Item, flat };

    flat -= g_itemCount;

    if (flat < g_quickToolCount)
        return { FocusKind::Tool, flat };

    flat -= g_quickToolCount;

    if (flat == 0)
        return { FocusKind::Slider, 0 };

    flat -= 1;

    if (flat == 0)
        return { FocusKind::Power, 0 };

    flat -= 1;

    return { FocusKind::PowerAction, flat };
}

static RECT GetItemRect(
    int width,
    int index)
{
    int top =
        MenuTop();

    int row =
        MenuRow();

    int gap =
        MenuGap();

    int y =
        top +
        index * (row + gap);

    return
    {
        S(7),
        y,
        width - S(7),
        y + row
    };
}

static void MoveFocus(
    HWND hwnd,
    int delta)
{
    int count =
        FocusCount();

    if (count <= 0)
        return;

    if (g_focusIndex < 0)
    {
        g_focusIndex =
            delta >= 0
                ? 0
                : count - 1;
    }
    else
    {
        g_focusIndex =
            ((g_focusIndex + delta) %
                 count +
             count) %
            count;
    }

    InvalidateRect(
        hwnd,
        nullptr,
        FALSE);
}

static void ActivateFocus(
    HWND hwnd)
{
    FocusTarget t =
        ResolveFocus(
            g_focusIndex);

    switch (t.kind)
    {
        case FocusKind::Search:
            HandleSearchEnter();
            break;

        case FocusKind::SearchResult:
            LaunchSearchResult((size_t)t.index);
            break;

        case FocusKind::Item:
            ActivateItem(
                t.index);
            break;

        case FocusKind::Tool:
            if (t.index >= 0 &&
                t.index <
                    g_quickToolCount)
            {
                ExecuteSmartInput(
                    g_quickTools[
                        t.index]
                        .command);

                CloseStart();
            }
            break;

        case FocusKind::Power:
            SetPowerOpen(
                hwnd,
                !g_powerOpen);
            break;

        case FocusKind::PowerAction:
            ExecutePowerAction(
                t.index);
            break;

        default:
            break;
    }

    InvalidateRect(
        hwnd,
        nullptr,
        FALSE);
}

// Lets the search panel's own paint routine (defined earlier in the
// file, before FocusKind exists) know which row, if any, is the current
// keyboard-focus target, without needing the enum itself in scope there.
static int GetFocusedSearchResultIndex()
{
    FocusTarget t = ResolveFocus(g_focusIndex);

    return
        t.kind == FocusKind::SearchResult
            ? t.index
            : -1;
}

static void AdjustFocusedSlider(
    HWND hwnd,
    int direction)
{
    FocusTarget t =
        ResolveFocus(
            g_focusIndex);

    if (t.kind != FocusKind::Slider)
        return;

    int step = 10;

    int value =
        (int)g_windowAlpha +
        direction * step;

    if (value < OPACITY_MIN)
        value = OPACITY_MIN;

    if (value > OPACITY_MAX)
        value = OPACITY_MAX;

    g_windowAlpha = (BYTE)value;

    ApplyOpacityToCurrentTarget();

    InvalidateRect(
        hwnd,
        nullptr,
        FALSE);
}

static void DrawFocusRing(
    HDC dc,
    const RECT& r,
    int radius)
{
    RECT ring =
    {
        r.left - S(3),
        r.top - S(3),
        r.right + S(3),
        r.bottom + S(3)
    };

    HPEN pen =
        CreatePen(
            PS_SOLID,
            S(2),
            g_accent);

    if (!pen)
        return;

    HGDIOBJ oldPen =
        SelectObject(
            dc,
            pen);

    HGDIOBJ oldBrush =
        SelectObject(
            dc,
            GetStockObject(
                NULL_BRUSH));

    RoundRect(
        dc,
        ring.left,
        ring.top,
        ring.right,
        ring.bottom,
        radius,
        radius);

    SelectObject(
        dc,
        oldBrush);

    SelectObject(
        dc,
        oldPen);

    DeleteObject(pen);
}

// Whichever quick-tool button is currently under the mouse, or
// (failing that) has keyboard focus — the one to show a label for.
static int GetTooltipToolIndex()
{
    if (g_quickToolHover >= 0)
        return g_quickToolHover;

    if (g_focusIndex >= 0)
    {
        FocusTarget t =
            ResolveFocus(
                g_focusIndex);

        if (t.kind == FocusKind::Tool)
            return t.index;
    }

    return -1;
}

// Shared bubble-with-label renderer for any hover tooltip: sizes
// itself to the text, centers over the anchor rect, and flips
// below it if there's no room above. Used for both quick-tool and
// power-action hover labels, which are otherwise identical.
static void DrawHoverTooltip(
    HDC dc,
    int width,
    const RECT& anchor,
    const wchar_t* label)
{
    if (!label ||
        !*label)
    {
        return;
    }

    HGDIOBJ oldFont =
        SelectObject(
            dc,
            g_small);

    SIZE textSize{};

    GetTextExtentPoint32W(
        dc,
        label,
        (int)wcslen(label),
        &textSize);

    SelectObject(
        dc,
        oldFont);

    int paddingX = S(8);
    int paddingY = S(5);

    int bubbleW =
        textSize.cx + paddingX * 2;

    int bubbleH =
        textSize.cy + paddingY * 2;

    int cx =
        (anchor.left +
         anchor.right) / 2;

    int bubbleLeft =
        cx - bubbleW / 2;

    int bubbleTop =
        anchor.top - bubbleH - S(8);

    if (bubbleLeft < S(4))
        bubbleLeft = S(4);

    if (bubbleLeft + bubbleW >
        width - S(4))
    {
        bubbleLeft =
            width - S(4) - bubbleW;
    }

    if (bubbleTop < S(4))
        bubbleTop =
            anchor.bottom + S(8);

    RECT bubble =
    {
        bubbleLeft,
        bubbleTop,
        bubbleLeft + bubbleW,
        bubbleTop + bubbleH
    };

    DrawTile(
        dc,
        bubble,
        S(6),
        MixColor(
            g_bg,
            RGB(0, 0, 0),
            20),
        g_border);

    DrawTextSimple(
        dc,
        label,
        bubble.left,
        bubble.top,
        bubbleW,
        bubbleH,
        g_text,
        g_small,
        DT_CENTER |
            DT_VCENTER |
            DT_SINGLELINE);
}

static void DrawQuickToolTooltip(
    HDC dc,
    int width,
    int height)
{
    int index =
        GetTooltipToolIndex();

    if (index < 0 ||
        index >= g_quickToolCount)
    {
        return;
    }

    const std::wstring& label =
        g_quickTools[index]
            .displayName;

    DrawHoverTooltip(
        dc,
        width,
        GetQuickToolRect(
            height,
            index),
        label.c_str());
}

static const wchar_t* POWER_ACTION_LABELS[POWER_ACTION_COUNT] =
{
    L"Restart",
    L"Shut down",
    L"Sign out",
    L"Lock"
};

// Whichever flyout action button is currently under the mouse, or
// (failing that) has keyboard focus — the one to show a label for.
// Only while the flyout is actually open, so a stale hover index
// left over from before it closed can't show a phantom label.
static int GetTooltipPowerActionIndex()
{
    if (!g_powerOpen)
        return -1;

    if (g_powerHover >= 0 &&
        g_powerHover < POWER_ACTION_COUNT)
    {
        return g_powerHover;
    }

    if (g_focusIndex >= 0)
    {
        FocusTarget t =
            ResolveFocus(
                g_focusIndex);

        if (t.kind == FocusKind::PowerAction)
            return t.index;
    }

    return -1;
}

static void DrawPowerActionTooltip(
    HDC dc,
    int width,
    int height)
{
    int index =
        GetTooltipPowerActionIndex();

    if (index < 0 ||
        index >= POWER_ACTION_COUNT)
    {
        return;
    }

    DrawHoverTooltip(
        dc,
        width,
        GetPowerActionRect(
            width,
            height,
            index),
        POWER_ACTION_LABELS[index]);
}

static void DrawCurrentFocusRing(
    HDC dc,
    int width,
    int height)
{
    if (g_focusIndex < 0)
        return;

    FocusTarget t =
        ResolveFocus(
            g_focusIndex);

    RECT r{};
    int radius = S(9);

    switch (t.kind)
    {
        case FocusKind::Search:
            if (g_searchText.empty())
                return;

            r = GetSearchRect(width);
            radius = S(12);
            break;

        case FocusKind::Item:
            r =
                GetItemRect(
                    width,
                    t.index);
            radius = S(9);
            break;

        case FocusKind::Tool:
            r =
                GetQuickToolRect(
                    height,
                    t.index);
            radius = S(6);
            break;

        case FocusKind::Slider:
            r =
                GetOpacitySliderRect(
                    width,
                    height);
            r.top -= S(6);
            r.bottom += S(6);
            radius = S(6);
            break;

        case FocusKind::Power:
            r =
                GetPowerButtonRect(
                    width,
                    height);
            radius = S(9);
            break;

        case FocusKind::PowerAction:
            r =
                GetPowerActionRect(
                    width,
                    height,
                    t.index);
            radius = S(7);
            break;

        default:
            return;
    }

    DrawFocusRing(
        dc,
        r,
        radius);
}

// ============================================================
// Paint
// ============================================================

static void PaintStart(
    HWND hwnd,
    HDC dc)
{
    RECT client{};

    GetClientRect(
        hwnd,
        &client);

    int width =
        client.right;

    int height =
        client.bottom;

    HDC back =
        CreateCompatibleDC(dc);

    if (!back)
        return;

    HBITMAP bitmap =
        CreateCompatibleBitmap(
            dc,
            width,
            height);

    if (!bitmap)
    {
        DeleteDC(back);
        return;
    }

    HGDIOBJ old =
        SelectObject(
            back,
            bitmap);

    // --------------------------------------------------------
    // Main background
    // --------------------------------------------------------

    FillRectColor(
        back,
        client,
        g_bg);

    // Soft outer border.
    DrawRoundBorder(
        back,
        client,
        S(18),
        g_border);

    // --------------------------------------------------------
    // Search — hidden until the user starts typing, then it
    // appears as its own row below the last menu item.
    // --------------------------------------------------------

    RECT search =
        GetSearchRect(width);

    if (!g_searchText.empty())
    {
        int searchGlow =
            (int)(g_searchAnim * 100.0f);

        COLORREF searchBg =
            MixColor(
                g_panel,
                g_hot,
                searchGlow);

        COLORREF searchBorder =
            MixColor(
                g_border,
                g_accent,
                searchGlow);

        // A soft accent glow that grows in behind the box on
        // hover, so the highlight reads as a gentle lift rather
        // than a hard color swap.
        if (g_searchAnim > 0.0f)
        {
            RECT glow =
            {
                search.left - S(3),
                search.top - S(3),
                search.right + S(3),
                search.bottom + S(3)
            };

            HDC glowDc =
                CreateCompatibleDC(
                    dc);

            if (glowDc)
            {
                int glowW =
                    glow.right - glow.left;

                int glowH =
                    glow.bottom - glow.top;

                HBITMAP glowBitmap =
                    CreateCompatibleBitmap(
                        dc,
                        glowW,
                        glowH);

                if (glowBitmap)
                {
                    HGDIOBJ oldGlow =
                        SelectObject(
                            glowDc,
                            glowBitmap);

                    BitBlt(
                        glowDc,
                        0,
                        0,
                        glowW,
                        glowH,
                        back,
                        glow.left,
                        glow.top,
                        SRCCOPY);

                    RECT glowLocal =
                    {
                        0,
                        0,
                        glowW,
                        glowH
                    };

                    FillRoundRect(
                        glowDc,
                        glowLocal,
                        S(15),
                        g_accent);

                    BLENDFUNCTION blend{};

                    blend.BlendOp =
                        AC_SRC_OVER;

                    blend.SourceConstantAlpha =
                        (BYTE)(g_searchAnim * 60.0f);

                    AlphaBlend(
                        back,
                        glow.left,
                        glow.top,
                        glowW,
                        glowH,
                        glowDc,
                        0,
                        0,
                        glowW,
                        glowH,
                        blend);

                    SelectObject(
                        glowDc,
                        oldGlow);

                    DeleteObject(
                        glowBitmap);
                }

                DeleteDC(
                    glowDc);
            }
        }

        DrawTile(
            back,
            search,
            S(12),
            searchBg,
            searchBorder);

        // Accent caret / prompt.
        DrawTextSimple(
            back,
            L">",
            S(20),
            search.top,
            S(22),
            S(48),
            g_accent,
            g_bold);

        // Text + caret + selection highlight. Measured all together
        // under one font selection (each DrawTextSimple call below
        // manages its own selection independently, so this doesn't need
        // to stay selected across them), then drawn as up to three runs
        // — before/inside/after the selection — so the selected portion
        // reads with contrast against its accent-colored highlight the
        // same way a native textbox would, instead of the caret always
        // being pinned to the end of the text regardless of where it
        // actually is.
        {
            int caretPos = g_searchSelection.Caret();
            bool hasSelection = g_searchSelection.HasSelection();
            int selStart = hasSelection ? g_searchSelection.SelectionStart() : caretPos;
            int selEnd = hasSelection ? g_searchSelection.SelectionEnd() : caretPos;

            HGDIOBJ oldFont =
                SelectObject(back, g_font);

            SIZE beforeSize{};
            GetTextExtentPoint32W(
                back, g_searchText.c_str(), selStart, &beforeSize);

            SIZE selSize{};
            if (hasSelection)
            {
                GetTextExtentPoint32W(
                    back, g_searchText.c_str() + selStart,
                    selEnd - selStart, &selSize);
            }

            SIZE toCaretSize{};
            GetTextExtentPoint32W(
                back, g_searchText.c_str(), caretPos, &toCaretSize);

            SelectObject(back, oldFont);

            if (hasSelection)
            {
                RECT selRect =
                {
                    S(46) + beforeSize.cx,
                    search.top + S(9),
                    S(46) + beforeSize.cx + selSize.cx,
                    search.top + S(39)
                };

                FillRectColor(back, selRect, g_accent);
            }

            if (selStart > 0)
            {
                std::wstring before = g_searchText.substr(0, selStart);

                DrawTextSimple(
                    back, before.c_str(),
                    S(46), search.top,
                    width - S(58), S(48),
                    g_text, g_font);
            }

            if (hasSelection)
            {
                std::wstring selected =
                    g_searchText.substr(selStart, selEnd - selStart);

                DrawTextSimple(
                    back, selected.c_str(),
                    S(46) + beforeSize.cx, search.top,
                    width - S(58) - beforeSize.cx, S(48),
                    g_accentText, g_font);
            }

            if ((size_t)selEnd < g_searchText.size())
            {
                SIZE toSelEndSize{};

                HGDIOBJ oldFont2 = SelectObject(back, g_font);
                GetTextExtentPoint32W(
                    back, g_searchText.c_str(), selEnd, &toSelEndSize);
                SelectObject(back, oldFont2);

                DrawTextSimple(
                    back, g_searchText.c_str() + selEnd,
                    S(46) + toSelEndSize.cx, search.top,
                    width - S(58) - toSelEndSize.cx, S(48),
                    g_text, g_font);
            }

            // Blinking text-entry caret, at its actual position.
            if (g_caretVisible)
            {
                int caretX = S(46) + toCaretSize.cx + S(3);

                RECT caret =
                {
                    caretX,
                    search.top + S(9),
                    caretX + S(2),
                    search.top + S(39)
                };

                FillRectColor(back, caret, g_accent);
            }
        }
    }

    // --------------------------------------------------------
    // Menu rows
    // --------------------------------------------------------

    const int top =
        MenuTop();

    const int row =
        MenuRow();

    const int gap =
        MenuGap();

    for (int i = 0;
         i < g_itemCount;
         ++i)
    {
        int y =
            top +
            i * (row + gap);

        bool selected =
            i == g_hover;

        RECT item =
        {
            S(7),
            y,
            width - S(7),
            y + row
        };

        COLORREF itemColor =
            selected
                ? g_accent
                : g_panel;

        // Translucent rather than a flat block, so the row reads
        // as glass tinted toward the panel/accent color instead
        // of a solid opaque slab.
        DrawTile(
            back,
            item,
            S(9),
            itemColor,
            selected
                ? g_accentBorder
                : g_border,
            165);

        // ----------------------------------------------------
        // Icon tile
        // ----------------------------------------------------

        RECT icon =
        {
            S(14),
            y + S(5),
            S(43),
            y + S(34)
        };

        COLORREF iconBackground =
            selected
                ? MixColor(
                    g_accent,
                    RGB(255,255,255),
                    9)
                : MixColor(
                    g_panel,
                    g_text,
                    6);

        DrawTile(
            back,
            icon,
            S(7),
            iconBackground,
            selected
                ? g_accentText
                : g_border);

        if (g_icons[i])
        {
            DrawRealIcon(
                back,
                g_icons[i],
                icon.left,
                icon.top,
                icon.right -
                    icon.left);
        }
        else
        {
            DrawFallbackIcon(
                back,
                i,
                icon.left,
                icon.top,
                icon.right -
                    icon.left,
                selected);
        }

        // ----------------------------------------------------
        // Label
        // ----------------------------------------------------

        DrawTextSimple(
            back,
            g_items[i].label,
            S(56),
            y,
            width - S(66),
            row,
            selected
                ? g_accentText
                : g_text,
            i == g_itemCount - 1
                ? g_bold
                : g_font);
    }

    // --------------------------------------------------------
    // Bottom utility strip
    //
    // No labels. Just a subtle divider and power control.
    // --------------------------------------------------------

    const int utilityTop =
        height - S(47);

    HPEN separator =
        CreatePen(
            PS_SOLID,
            1,
            MixColor(
                g_bg,
                g_text,
                7));

    if (separator)
    {
        HGDIOBJ oldPen =
            SelectObject(
                back,
                separator);

        MoveToEx(
            back,
            S(10),
            utilityTop,
            nullptr);

        LineTo(
            back,
            width - S(10),
            utilityTop);

        SelectObject(
            back,
            oldPen);

        DeleteObject(separator);
    }

    // --------------------------------------------------------
    // Quick-launch tools
    // --------------------------------------------------------

    for (int i = 0;
         i < g_quickToolCount;
         ++i)
    {
        RECT toolRect =
            GetQuickToolRect(
                height,
                i);

        bool toolHot =
            g_quickToolHover == i;

        DrawTile(
            back,
            toolRect,
            S(6),
            toolHot
                ? g_hot
                : MixColor(
                    g_panel,
                    g_text,
                    4),
            toolHot
                ? g_accent
                : g_border);

        if (g_quickTools[i].icon)
        {
            DrawRealIcon(
                back,
                g_quickTools[i].icon,
                toolRect.left,
                toolRect.top,
                toolRect.right -
                    toolRect.left);
        }
    }

    // --------------------------------------------------------
    // Opacity slider
    // --------------------------------------------------------

    RECT slider =
        GetOpacitySliderRect(
            width,
            height);

    FillRoundRect(
        back,
        slider,
        S(2),
        MixColor(
            g_bg,
            g_text,
            10));

    int thumbX =
        GetSliderThumbX(
            slider);

    RECT sliderFill =
        slider;

    sliderFill.right =
        thumbX;

    if (sliderFill.right >
        sliderFill.left)
    {
        FillRoundRect(
            back,
            sliderFill,
            S(2),
            g_accent);
    }

    int thumbCy =
        (slider.top + slider.bottom) / 2;

    int thumbR =
        g_sliderDragging
            ? S(7)
            : S(6);

    RECT thumb =
    {
        thumbX - thumbR,
        thumbCy - thumbR,
        thumbX + thumbR,
        thumbCy + thumbR
    };

    DrawTile(
        back,
        thumb,
        thumbR,
        RGB(255, 255, 255),
        g_accent);

    if (g_sliderDragging)
    {
        wchar_t label[8];

        wsprintfW(
            label,
            L"%d%%",
            (int)(
                OpacityToFraction(
                    g_windowAlpha) *
                100.0f));

        RECT labelRect =
        {
            thumbX - S(20),
            thumbCy - S(28),
            thumbX + S(20),
            thumbCy - S(8)
        };

        DrawTextSimple(
            back,
            label,
            labelRect.left,
            labelRect.top,
            labelRect.right -
                labelRect.left,
            labelRect.bottom -
                labelRect.top,
            g_text,
            g_small,
            DT_CENTER |
                DT_VCENTER |
                DT_SINGLELINE);
    }

    // --------------------------------------------------------
    // Power button
    // --------------------------------------------------------

    RECT power =
        GetPowerButtonRect(
            width,
            height);

    bool powerHot =
        g_powerHover ==
            POWER_HOVER_MAIN_BUTTON;

    bool powerActive =
        g_powerOpen;

    COLORREF powerColor =
        powerHot || powerActive
            ? g_accent
            : g_panel;

    DrawTile(
        back,
        power,
        S(9),
        powerColor,
        powerHot || powerActive
            ? g_accentBorder
            : g_border,
        210);

    DrawPowerIcon(
        back,
        power,
        powerHot || powerActive
            ? g_accentText
            : g_muted);

    // --------------------------------------------------------
    // Power flyout
    // --------------------------------------------------------

    if (g_powerOpen ||
        g_powerAnim > 0.0f)
    {
        RECT popup =
            GetPowerMenuRect(
                width,
                height);

        int popupW =
            popup.right - popup.left;

        int popupH =
            popup.bottom - popup.top;

        HDC flyoutDc =
            CreateCompatibleDC(
                dc);

        if (flyoutDc)
        {
            HBITMAP flyoutBitmap =
                CreateCompatibleBitmap(
                    dc,
                    popupW,
                    popupH);

            if (flyoutBitmap)
            {
                HGDIOBJ oldFlyout =
                    SelectObject(
                        flyoutDc,
                        flyoutBitmap);

                // Start from whatever's already behind the flyout
                // so the alpha blend reveals real content instead
                // of fading to black.
                BitBlt(
                    flyoutDc,
                    0,
                    0,
                    popupW,
                    popupH,
                    back,
                    popup.left,
                    popup.top,
                    SRCCOPY);

                RECT local =
                {
                    0,
                    0,
                    popupW,
                    popupH
                };

                DrawTile(
                    flyoutDc,
                    local,
                    S(10),
                    MixColor(
                        g_panel,
                        g_bg,
                        25),
                    g_border);

                using DrawIconFn =
                    void (*)(
                        HDC,
                        const RECT&,
                        COLORREF);

                static const DrawIconFn
                    actionIcons[
                        POWER_ACTION_COUNT] =
                {
                    DrawRestartIcon,
                    DrawShutdownIcon,
                    DrawSignOutIcon,
                    DrawLockIcon
                };

                for (int i = 0;
                     i < POWER_ACTION_COUNT;
                     ++i)
                {
                    RECT actionRect =
                        GetPowerActionRect(
                            width,
                            height,
                            i);

                    OffsetRect(
                        &actionRect,
                        -popup.left,
                        -popup.top);

                    bool actionHot =
                        g_powerHover == i;

                    FillRoundRect(
                        flyoutDc,
                        actionRect,
                        S(7),
                        actionHot
                            ? g_accent
                            : MixColor(
                                g_panel,
                                g_text,
                                5));

                    actionIcons[i](
                        flyoutDc,
                        actionRect,
                        actionHot
                            ? g_accentText
                            : g_text);
                }

                float anim =
                    g_powerAnim;

                if (anim > 1.0f)
                    anim = 1.0f;

                if (anim < 0.0f)
                    anim = 0.0f;

                BLENDFUNCTION blend{};

                blend.BlendOp =
                    AC_SRC_OVER;

                blend.SourceConstantAlpha =
                    (BYTE)(anim * 255.0f);

                int slide =
                    (int)(
                        (1.0f - anim) *
                        S(8));

                AlphaBlend(
                    back,
                    popup.left,
                    popup.top + slide,
                    popupW,
                    popupH,
                    flyoutDc,
                    0,
                    0,
                    popupW,
                    popupH,
                    blend);

                SelectObject(
                    flyoutDc,
                    oldFlyout);

                DeleteObject(
                    flyoutBitmap);
            }

            DeleteDC(
                flyoutDc);
        }
    }

    // --------------------------------------------------------
    // Quick-tool hover label
    // --------------------------------------------------------

    DrawQuickToolTooltip(
        back,
        width,
        height);

    // --------------------------------------------------------
    // Power action hover label
    // --------------------------------------------------------

    DrawPowerActionTooltip(
        back,
        width,
        height);

    // --------------------------------------------------------
    // Keyboard focus ring
    // --------------------------------------------------------

    DrawCurrentFocusRing(
        back,
        width,
        height);

    // --------------------------------------------------------
    // Present
    // --------------------------------------------------------

    BitBlt(
        dc,
        0,
        0,
        width,
        height,
        back,
        0,
        0,
        SRCCOPY);

    SelectObject(
        back,
        old);

    DeleteObject(bitmap);
    DeleteDC(back);
}

// ============================================================
// Window procedure
// ============================================================

static LRESULT CALLBACK StartProc(
    HWND hwnd,
    UINT msg,
    WPARAM wp,
    LPARAM lp)
{
    switch (msg)
    {
        case WM_MOUSEMOVE:
        {
            int x =
                (int)(short)LOWORD(lp);

            int y =
                (int)(short)HIWORD(lp);

            if (g_sliderDragging)
            {
                UpdateSliderFromX(
                    hwnd,
                    x);

                return 0;
            }

            RECT client{};

            GetClientRect(
                hwnd,
                &client);

            int oldHover =
                g_hover;

            int oldPowerHover =
                g_powerHover;

            POINT point
            {
                x,
                y
            };

            g_hover =
                HitStartItem(y);

            g_powerHover = -1;

            // The flyout floats above whatever menu rows happen to
            // sit underneath it, so those rows shouldn't light up
            // just because the cursor is over the flyout.
            if (g_powerOpen ||
                g_powerAnim > 0.0f)
            {
                RECT flyout =
                    GetPowerMenuRect(
                        client.right,
                        client.bottom);

                if (PtInRect(
                        &flyout,
                        point))
                {
                    g_hover = -1;
                }
            }

            RECT power =
                GetPowerButtonRect(
                    client.right,
                    client.bottom);

            if (PtInRect(
                    &power,
                    point))
            {
                g_powerHover =
                    POWER_HOVER_MAIN_BUTTON;

                g_hover = -1;
            }

            int powerAction =
                HitPowerAction(
                    x,
                    y,
                    client.right,
                    client.bottom);

            if (powerAction >= 0)
            {
                g_powerHover =
                    powerAction;
                g_hover = -1;
            }

            RECT search =
                GetSearchRect(
                    client.right);

            SetSearchHover(
                hwnd,
                !g_searchText.empty() &&
                    PtInRect(
                        &search,
                        point) ==
                        TRUE);

            if (g_searchDragging)
            {
                PlaceSearchCaretFromPoint(hwnd, S(46), x, true);
            }

            // Opacity slider hover — same inflated grab area as the
            // drag hit-test, so hovering to scroll feels exactly as
            // forgiving as hovering to drag.
            {
                RECT sliderForHover =
                    GetOpacitySliderRect(
                        client.right,
                        client.bottom);

                RECT sliderHoverHit =
                {
                    sliderForHover.left - S(4),
                    sliderForHover.top - S(10),
                    sliderForHover.right + S(4),
                    sliderForHover.bottom + S(10)
                };

                bool nowHoveringSlider =
                    PtInRect(&sliderHoverHit, point) == TRUE;

                if (nowHoveringSlider != g_sliderHover)
                {
                    g_sliderHover = nowHoveringSlider;

                    if (g_sliderHover)
                        ResumeOpacityHighlight();
                    else
                        HideOpacityHighlight();
                }
            }

            int oldToolHover =
                g_quickToolHover;

            g_quickToolHover = -1;

            for (int i = 0;
                 i < g_quickToolCount;
                 ++i)
            {
                RECT toolRect =
                    GetQuickToolRect(
                        client.bottom,
                        i);

                if (PtInRect(
                        &toolRect,
                        point))
                {
                    g_quickToolHover = i;
                    g_hover = -1;
                    g_powerHover = -1;
                    break;
                }
            }

            if (oldToolHover !=
                g_quickToolHover)
            {
                InvalidateRect(
                    hwnd,
                    nullptr,
                    FALSE);
            }

            if (oldHover != g_hover ||
                oldPowerHover !=
                    g_powerHover)
            {
                InvalidateRect(
                    hwnd,
                    nullptr,
                    FALSE);
            }

            if (!g_mouseTracking)
            {
                TRACKMOUSEEVENT tme{};

                tme.cbSize =
                    sizeof(tme);

                tme.dwFlags =
                    TME_LEAVE;

                tme.hwndTrack =
                    hwnd;

                if (TrackMouseEvent(
                        &tme))
                {
                    g_mouseTracking = true;
                }
            }

            return 0;
        }

        case WM_MOUSELEAVE:
        {
            g_hover = -1;
            g_powerHover = -1;
            g_quickToolHover = -1;
            g_mouseTracking = false;

            SetSearchHover(
                hwnd,
                false);

            if (g_sliderHover)
            {
                g_sliderHover = false;
                HideOpacityHighlight();
            }

            InvalidateRect(
                hwnd,
                nullptr,
                FALSE);

            return 0;
        }

        case WM_MOUSEWHEEL:
        {
            // Wheel messages arrive with screen, not client, coordinates.
            POINT screenPoint
            {
                (int)(short)LOWORD(lp),
                (int)(short)HIWORD(lp)
            };

            ScreenToClient(hwnd, &screenPoint);

            RECT client{};
            GetClientRect(hwnd, &client);

            RECT slider =
                GetOpacitySliderRect(client.right, client.bottom);

            RECT sliderHit =
            {
                slider.left - S(4),
                slider.top - S(10),
                slider.right + S(4),
                slider.bottom + S(10)
            };

            if (PtInRect(&sliderHit, screenPoint))
            {
                int delta = (short)HIWORD(wp);

                CycleOpacityTarget(delta > 0 ? 1 : -1);

                return 0;
            }

            break;
        }

        case WM_LBUTTONDOWN:
        {
            int x =
                (int)(short)LOWORD(lp);

            int y =
                (int)(short)HIWORD(lp);

            RECT client{};

            GetClientRect(
                hwnd,
                &client);

            // Quick-launch tool buttons.
            POINT toolPoint
            {
                x,
                y
            };

            for (int i = 0;
                 i < g_quickToolCount;
                 ++i)
            {
                RECT toolRect =
                    GetQuickToolRect(
                        client.bottom,
                        i);

                if (PtInRect(
                        &toolRect,
                        toolPoint))
                {
                    ExecuteSmartInput(
                        g_quickTools[i]
                            .command);

                    CloseStart();

                    return 0;
                }
            }

            // Opacity slider. Hit area is inflated vertically so
            // it's easy to grab without needing pixel precision.
            RECT slider =
                GetOpacitySliderRect(
                    client.right,
                    client.bottom);

            RECT sliderHit =
            {
                slider.left - S(4),
                slider.top - S(10),
                slider.right + S(4),
                slider.bottom + S(10)
            };

            POINT sliderPoint
            {
                x,
                y
            };

            if (PtInRect(
                    &sliderHit,
                    sliderPoint))
            {
                g_sliderDragging = true;

                SetCapture(
                    hwnd);

                UpdateSliderFromX(
                    hwnd,
                    x);

                return 0;
            }

            // Search box — click-to-place-caret, drag-to-select.
            if (!g_searchText.empty())
            {
                RECT search =
                    GetSearchRect(client.right);

                POINT searchPoint{ x, y };

                if (PtInRect(&search, searchPoint))
                {
                    bool shift =
                        (GetKeyState(VK_SHIFT) & 0x8000) != 0;

                    PlaceSearchCaretFromPoint(hwnd, S(46), x, shift);

                    g_searchDragging = true;
                    SetCapture(hwnd);

                    g_focusIndex = 0; // FocusKind::Search is always flat index 0.

                    InvalidateRect(hwnd, nullptr, FALSE);

                    return 0;
                }
            }

            // Power flyout action.
            int powerAction =
                HitPowerAction(
                    x,
                    y,
                    client.right,
                    client.bottom);

            if (powerAction >= 0)
            {
                ExecutePowerAction(
                    powerAction);

                return 0;
            }

            // Main power button.
            RECT power =
                GetPowerButtonRect(
                    client.right,
                    client.bottom);

            POINT point
            {
                x,
                y
            };

            if (PtInRect(
                    &power,
                    point))
            {
                SetPowerOpen(
                    hwnd,
                    !g_powerOpen);

                g_powerHover = -1;

                InvalidateRect(
                    hwnd,
                    nullptr,
                    FALSE);

                return 0;
            }

            // Clicking anywhere else closes the power flyout.
            if (g_powerOpen)
            {
                RECT flyout =
                    GetPowerMenuRect(
                        client.right,
                        client.bottom);

                if (!PtInRect(
                        &flyout,
                        point))
                {
                    SetPowerOpen(
                        hwnd,
                        false);

                    g_powerHover = -1;

                    InvalidateRect(
                        hwnd,
                        nullptr,
                        FALSE);
                }
            }

            int item =
                HitStartItem(y);

            if (item >= 0)
            {
                ActivateItem(
                    item);

                return 0;
            }

            return 0;
        }

        case WM_LBUTTONDBLCLK:
        {
            int x =
                (int)(short)LOWORD(lp);

            int y =
                (int)(short)HIWORD(lp);

            if (!g_searchText.empty())
            {
                RECT client{};
                GetClientRect(hwnd, &client);

                RECT search =
                    GetSearchRect(client.right);

                POINT searchPoint{ x, y };

                if (PtInRect(&search, searchPoint))
                {
                    SelectSearchWordAtPoint(hwnd, S(46), x);
                    return 0;
                }
            }

            break;
        }

        case WM_LBUTTONUP:
        {
            if (g_sliderDragging)
            {
                g_sliderDragging = false;

                ReleaseCapture();

                return 0;
            }

            if (g_searchDragging)
            {
                g_searchDragging = false;

                ReleaseCapture();

                return 0;
            }

            break;
        }

        case WM_CAPTURECHANGED:
        {
            g_sliderDragging = false;
            g_searchDragging = false;

            return 0;
        }

        case WM_RBUTTONDOWN:
        {
            int x =
                (int)(short)LOWORD(lp);

            int y =
                (int)(short)HIWORD(lp);

            RECT client{};

            GetClientRect(
                hwnd,
                &client);

            RECT power =
                GetPowerButtonRect(
                    client.right,
                    client.bottom);

            POINT point
            {
                x,
                y
            };

            if (PtInRect(
                    &power,
                    point))
            {
                // Right click power = restart immediately.
                ExecutePowerAction(
                    POWER_ACTION_RESTART);
                return 0;
            }

            if (g_powerOpen)
            {
                SetPowerOpen(
                    hwnd,
                    false);

                g_powerHover = -1;

                InvalidateRect(
                    hwnd,
                    nullptr,
                    FALSE);

                return 0;
            }

            return 0;
        }

        case WM_KEYDOWN:
        {
            if (wp == VK_ESCAPE)
            {
                CloseStart();
                return 0;
            }

            if (wp == VK_TAB)
            {
                bool shift =
                    (GetKeyState(
                        VK_SHIFT) &
                     0x8000) != 0;

                MoveFocus(
                    hwnd,
                    shift ? -1 : 1);

                return 0;
            }

            if (wp == VK_DOWN)
            {
                MoveFocus(
                    hwnd,
                    1);

                return 0;
            }

            if (wp == VK_UP)
            {
                MoveFocus(
                    hwnd,
                    -1);

                return 0;
            }

            if (wp == VK_LEFT ||
                wp == VK_RIGHT ||
                wp == VK_HOME ||
                wp == VK_END)
            {
                FocusTarget t =
                    ResolveFocus(g_focusIndex);

                bool searchActive =
                    g_focusIndex < 0 ||
                    t.kind == FocusKind::Search;

                if (searchActive)
                {
                    bool shift =
                        (GetKeyState(VK_SHIFT) & 0x8000) != 0;

                    HandleSearchBoxNavigationKey(
                        hwnd, (DWORD)wp, false, shift);

                    return 0;
                }

                if (wp == VK_LEFT || wp == VK_RIGHT)
                {
                    AdjustFocusedSlider(
                        hwnd,
                        wp == VK_RIGHT
                            ? 1
                            : -1);
                }

                return 0;
            }

            if ((wp == 'A' || wp == 'C') &&
                (GetKeyState(VK_CONTROL) & 0x8000))
            {
                FocusTarget t =
                    ResolveFocus(g_focusIndex);

                bool searchActive =
                    g_focusIndex < 0 ||
                    t.kind == FocusKind::Search;

                if (searchActive)
                {
                    HandleSearchBoxNavigationKey(
                        hwnd, (DWORD)wp, true, false);

                    return 0;
                }

                break;
            }

            if (wp == VK_RETURN)
            {
                FocusTarget t =
                    ResolveFocus(
                        g_focusIndex);

                if (g_focusIndex >= 0 &&
                    t.kind !=
                        FocusKind::Search)
                {
                    ActivateFocus(
                        hwnd);
                }
                else
                {
                    HandleSearchEnter();
                }

                return 0;
            }

            if (wp == VK_SPACE)
            {
                FocusTarget t =
                    ResolveFocus(
                        g_focusIndex);

                if (g_focusIndex >= 0 &&
                    t.kind !=
                        FocusKind::Search)
                {
                    ActivateFocus(
                        hwnd);

                    return 0;
                }

                break;
            }

            if (wp == VK_BACK)
            {
                DeleteSearchSelectionOrCharBefore(hwnd);
                return 0;
            }

            if (wp == VK_DELETE)
            {
                DeleteSearchSelectionOrCharAfter(hwnd);
                return 0;
            }

            break;
        }

        case WM_CHAR:
        {
            wchar_t c =
                (wchar_t)wp;

            FocusTarget t =
                ResolveFocus(
                    g_focusIndex);

            bool searchActive =
                g_focusIndex < 0 ||
                t.kind ==
                    FocusKind::Search;

            if (searchActive &&
                c >= 32 &&
                c != 127)
            {
                InsertSearchChar(hwnd, c);
                return 0;
            }

            break;
        }

        case WM_ERASEBKGND:
            return 1;

        case WM_THEMECHANGED:
        case WM_SETTINGCHANGE:
        {
            RefreshSystemColors();
            ApplyAcrylicBlur(hwnd);

            InvalidateRect(
                hwnd,
                nullptr,
                FALSE);

            return 0;
        }

        case WM_DPICHANGED:
        {
            g_dpi =
                LOWORD(wp);

            if (!g_dpi)
                g_dpi = 96;

            CreateFonts();

            RECT* suggested =
                reinterpret_cast<
                    RECT*>(lp);

            if (suggested)
            {
                SetWindowPos(
                    hwnd,
                    nullptr,
                    suggested->left,
                    suggested->top,
                    suggested->right -
                        suggested->left,
                    suggested->bottom -
                        suggested->top,
                    SWP_NOZORDER |
                        SWP_NOACTIVATE);
            }

            ApplyWindowRounding(
                hwnd);

            InvalidateRect(
                hwnd,
                nullptr,
                FALSE);

            return 0;
        }

        case WM_NCHITTEST:
        {
            // Keep this a normal interactive popup.
            return HTCLIENT;
        }

        case WM_PAINT:
        {
            PAINTSTRUCT ps{};

            HDC dc =
                BeginPaint(
                    hwnd,
                    &ps);

            PaintStart(
                hwnd,
                dc);

            EndPaint(
                hwnd,
                &ps);

            return 0;
        }

        case WM_TIMER:
        {
            if (wp == TIMER_POWER_ANIM)
            {
                StepPowerAnim(
                    hwnd);

                return 0;
            }

            if (wp == TIMER_SEARCH_ANIM)
            {
                StepSearchAnim(
                    hwnd);

                return 0;
            }

            if (wp == TIMER_CARET_BLINK)
            {
                StepCaretBlink(
                    hwnd);

                return 0;
            }

            if (wp == TIMER_SHOW_ANIM)
            {
                StepShowAnim(
                    hwnd);

                return 0;
            }

            if (wp == TIMER_OPACITY_TRACK)
            {
                if (!g_opacityHighlightTarget ||
                    !IsWindow(g_opacityHighlightTarget) ||
                    !IsWindowVisible(g_opacityHighlightTarget))
                {
                    // The targeted window closed, got hidden, or
                    // minimized out from under us — fall back to the
                    // Start menu rather than keep tracking a window
                    // that's no longer really there.
                    g_opacityIndex = 0;
                    g_windowAlpha = g_startOwnAlpha;

                    SetStartAsOpacityTarget();

                    InvalidateRect(hwnd, nullptr, FALSE);

                    return 0;
                }

                RepositionOpacityHighlight(g_opacityHighlightTarget);

                return 0;
            }

            if (wp == TIMER_TOAST)
            {
                DWORD elapsed = GetTickCount() - g_toastStartTick;

                if (elapsed < (DWORD)TOAST_HOLD_MS)
                    return 0;

                DWORD fadeElapsed = elapsed - TOAST_HOLD_MS;

                if (fadeElapsed >= (DWORD)TOAST_FADE_MS)
                {
                    KillTimer(hwnd, TIMER_TOAST);
                    g_toastTimer = 0;

                    if (g_toast)
                        ShowWindow(g_toast, SW_HIDE);

                    return 0;
                }

                g_toastAlpha =
                    1.0f - (float)fadeElapsed / (float)TOAST_FADE_MS;

                if (g_toast)
                {
                    SetLayeredWindowAttributes(
                        g_toast, 0,
                        (BYTE)(TOAST_BASE_ALPHA * g_toastAlpha),
                        LWA_ALPHA);
                }

                return 0;
            }

            if (wp == TIMER_BLUR_CLOSE)
            {
                // One-shot: the grace period has elapsed without the
                // foreground reverting to one of our own windows (see
                // DismissNativeStartMenuIfNeeded), so the menu really
                // has lost focus to something else now.
                KillTimer(hwnd, TIMER_BLUR_CLOSE);
                g_blurCloseTimer = 0;

                if (g_startVisible)
                    CloseStart();

                return 0;
            }

            break;
        }

        case WM_DESTROY:
        {
            g_startVisible = false;
            ResetUIState();

            if (g_showAnimTimer)
            {
                KillTimer(
                    hwnd,
                    TIMER_SHOW_ANIM);

                g_showAnimTimer = 0;
            }

            g_showAnimMode =
                ShowAnimMode::None;

            return 0;
        }
    }

    return DefWindowProcW(
        hwnd,
        msg,
        wp,
        lp);
}

// ============================================================
// Create Start
// ============================================================

static bool CreateStartWindow(
    HINSTANCE instance)
{
    WNDCLASSEXW wc{};

    wc.cbSize =
        sizeof(wc);

    wc.style =
        CS_HREDRAW |
        CS_VREDRAW |
        CS_DBLCLKS;

    wc.lpfnWndProc =
        StartProc;

    wc.hInstance =
        instance;

    wc.hCursor =
        LoadCursorW(
            nullptr,
            IDC_ARROW);

    wc.lpszClassName =
        START_CLASS;

    wc.hbrBackground =
        nullptr;

    if (!RegisterClassExW(
            &wc))
    {
        if (GetLastError() !=
            ERROR_CLASS_ALREADY_EXISTS)
        {
            return false;
        }
    }

    RECT r =
        GetStartRect();

    g_start =
        CreateWindowExW(
            WS_EX_TOOLWINDOW |
                WS_EX_TOPMOST |
                WS_EX_LAYERED,
            START_CLASS,
            L"ClassicShell",
            WS_POPUP,
            r.left,
            r.top,
            r.right - r.left,
            r.bottom - r.top,
            nullptr,
            nullptr,
            instance,
            nullptr);

    if (!g_start)
        return false;

    // Translucency.
    SetLayeredWindowAttributes(
        g_start,
        0,
        g_windowAlpha,
        LWA_ALPHA);

    ApplyWindowRounding(
        g_start);

    ApplyAcrylicBlur(
        g_start);

    return true;
}

// ============================================================
// Show / toggle
// ============================================================

static void ShowStart()
{
    if (!g_start)
        return;

    // Every fresh open starts clean: stale search text and any leftover
    // results/God Mode browse listing from a previous session must not
    // survive a close-then-reopen (see ShouldClearSearchOnOpen in
    // starthook.h). This runs before GetStartRect() below so the very
    // first frame of the open animation already reflects the
    // no-search-box height, instead of opening tall and snapping down.
    if (ShouldClearSearchOnOpen())
    {
        g_searchText.clear();
        g_searchSelection.Reset();
        ClearSearchResults();
    }

    // Every fresh open also starts back on index 0 (the Start menu
    // itself) for the opacity slider, rather than resuming wherever a
    // previous session left the scroll-to-retarget feature pointed.
    g_opacityIndex = 0;
    g_windowAlpha = g_startOwnAlpha;
    g_opacityHighlightTarget = nullptr;

    RefreshSystemColors();

    LoadQuickTools();

    RECT r =
        GetStartRect();

    g_showAnimFinalRect = r;

    int finalHeight =
        r.bottom - r.top;

    int startHeight =
        (int)(finalHeight * 0.55f);

    if (startHeight < 40)
        startHeight = 40;

    // Start small and invisible, anchored to the same bottom edge
    // it will finish at; the show timer grows and fades it in.
    SetWindowPos(
        g_start,
        HWND_TOPMOST,
        r.left,
        r.bottom - startHeight,
        r.right - r.left,
        startHeight,
        SWP_SHOWWINDOW |
            SWP_NOACTIVATE);

    SetLayeredWindowAttributes(
        g_start,
        0,
        0,
        LWA_ALPHA);

    g_startVisible = true;

    ResetUIState();

    g_caretTimer =
        SetTimer(
            g_start,
            TIMER_CARET_BLINK,
            530,
            nullptr);

    g_hover = -1;
    g_powerHover = -1;

    RepositionSearchPanel();

    InvalidateRect(
        g_start,
        nullptr,
        FALSE);

    UpdateWindow(
        g_start);

    g_showAnimMode =
        ShowAnimMode::Opening;

    g_showAnimT = 0.0f;

    if (!g_showAnimTimer)
    {
        g_showAnimTimer =
            SetTimer(
                g_start,
                TIMER_SHOW_ANIM,
                12,
                nullptr);
    }

    SetForegroundWindow(
        g_start);
}

static void ToggleStart()
{
    if (g_startVisible)
        CloseStart();
    else
        ShowStart();
}

// ============================================================
// Keyboard conversion
// ============================================================

static bool IsPrintableKey(
    DWORD vk)
{
    return
        (vk >= '0' &&
         vk <= '9') ||
        (vk >= 'A' &&
         vk <= 'Z') ||
        vk == VK_SPACE ||
        vk == VK_OEM_PERIOD ||
        vk == VK_OEM_MINUS ||
        vk == VK_OEM_PLUS ||
        vk == VK_OEM_1 ||
        vk == VK_OEM_2 ||
        vk == VK_OEM_3 ||
        vk == VK_OEM_4 ||
        vk == VK_OEM_5 ||
        vk == VK_OEM_6 ||
        vk == VK_OEM_7;
}

static wchar_t VirtualKeyToChar(
    DWORD vk)
{
    BYTE keyboard[256]{};

    if (!GetKeyboardState(
            keyboard))
    {
        return 0;
    }

    UINT scan =
        MapVirtualKeyW(
            vk,
            MAPVK_VK_TO_VSC);

    wchar_t output[4]{};

    int result =
        ToUnicode(
            vk,
            scan,
            keyboard,
            output,
            3,
            0);

    return result == 1
        ? output[0]
        : 0;
}

// ============================================================
// Native shell surface dismissal
// ============================================================
//
// The keyboard hook (below) never intercepts the physical Windows
// key — see its own comment for why — which means a bare Win tap
// can genuinely reach Explorer and pop the native Start menu (or,
// on some Windows configurations, Search-on-Win-press) for a frame
// before this app's own menu takes over. Rather than trying to
// prevent that from happening, this catches it immediately after
// the fact: the instant any window becomes foreground, check
// whether it's one of the known native shell surfaces, and if our
// own menu is meant to be showing, dismiss it with a synthetic
// Escape and reclaim focus. This is a dismiss-after, not a
// prevent-before, on purpose — see SPEC.md section 2.2.

static HWINEVENTHOOK g_foregroundEventHook = nullptr;

// True if hwnd belongs to a process named exeName (case-insensitive,
// base name only). PROCESS_QUERY_LIMITED_INFORMATION is enough for
// QueryFullProcessImageNameW and works without elevation against
// another normal user-mode process on the same desktop.
static bool IsWindowFromProcess(
    HWND hwnd,
    const wchar_t* exeName)
{
    DWORD pid = 0;

    GetWindowThreadProcessId(
        hwnd,
        &pid);

    if (!pid)
        return false;

    HANDLE process =
        OpenProcess(
            PROCESS_QUERY_LIMITED_INFORMATION,
            FALSE,
            pid);

    if (!process)
        return false;

    wchar_t path[MAX_PATH]{};
    DWORD size = MAX_PATH;

    bool ok =
        QueryFullProcessImageNameW(
            process,
            0,
            path,
            &size) != FALSE;

    CloseHandle(process);

    if (!ok)
        return false;

    const wchar_t* base =
        wcsrchr(path, L'\\');

    base = base ? base + 1 : path;

    return _wcsicmp(base, exeName) == 0;
}

// More than one candidate process on purpose: a synthetic (fully
// unintercepted) Win tap probed directly against this machine came
// up owned by SearchHost.exe rather than StartMenuExperienceHost.exe
// — which of the two actually answers a bare Win tap varies by
// Windows build/config (Search-on-Win-press is a real, not uncommon
// taskbar setting), so this checks every surface known to plausibly
// answer it rather than betting on one.
static const wchar_t* const NATIVE_SHELL_SURFACE_PROCESSES[] =
{
    L"StartMenuExperienceHost.exe",
    L"SearchHost.exe",
    L"ShellExperienceHost.exe"
};

// The search results panel and the hover preview sit in different
// corners of the screen by design, so moving the mouse between them
// crosses real desktop space in between — and on some systems (focus-
// follows-mouse being the clearest case, but not the only one) that's
// enough to make Windows transiently report some other window as the
// foreground window, even though the user never actually clicked away
// or switched apps. Rather than closing the menu the instant that
// happens, a short grace period gives it a chance to self-correct (the
// next foreground event reporting one of our own windows again, which
// is exactly what happens once the cursor arrives at the preview/panel)
// before actually closing — the same "grace window before acting on a
// loss of hover" pattern the preview's own fade-out already uses.
// (g_blurCloseTimer/TIMER_BLUR_CLOSE themselves are declared earlier in
// the file, alongside the other satellite-window timer state, since
// StartProc's WM_TIMER handling needs them too.)
static const int BLUR_CLOSE_GRACE_MS = 400;

static void CancelPendingBlurClose()
{
    if (g_blurCloseTimer)
    {
        if (g_start)
            KillTimer(g_start, TIMER_BLUR_CLOSE);

        g_blurCloseTimer = 0;
    }
}

// Originally just the native-Start/Search dismiss-after (SPEC.md 2.2);
// widened to also close ClassicShell's own menu whenever *anything
// else* becomes the foreground window while it's open — a real Control
// Panel dialog opened from the God Mode listing, another app via
// Alt+Tab, a taskbar button for a different running window, etc. Before
// this, only a native shell surface or an explicit click/Escape closed
// the menu, so it could be left open behind a newly-foregrounded window
// with nothing telling it to go away — the same way a native flyout
// menu dismisses whenever focus genuinely moves elsewhere. Doesn't close
// immediately, though — see the grace-period note above.
static void DismissNativeStartMenuIfNeeded(
    HWND foreground)
{
    if (!g_startVisible ||
        !foreground ||
        !g_start)
    {
        return;
    }

    // Our own satellite windows (results panel, hover preview) are
    // WS_EX_NOACTIVATE and never actually become the foreground window
    // themselves, but checked anyway as a defensive no-op guard. This is
    // also the "self-correct" side of the grace period below: the cursor
    // arriving at one of our own windows after a transient blip cancels
    // whatever pending close that blip armed.
    if (foreground == g_start ||
        foreground == g_searchPanel ||
        foreground == g_preview)
    {
        CancelPendingBlurClose();
        return;
    }

    // A NOACTIVATE window (the results panel, the preview) can't ever
    // become foreground itself, but interacting with one — scrolling
    // the results list with the mouse wheel, in particular — can make
    // Windows briefly report the taskbar or the desktop as the
    // foreground window instead, as a side effect of routing input near
    // a window that isn't allowed to take activation. That's not a real
    // "the user switched away" event the way a genuinely different app
    // grabbing foreground is, so it's excluded here — a real click on
    // the taskbar or desktop is already caught by MouseProc's own
    // outside-click check regardless.
    wchar_t foregroundClass[64]{};

    GetClassNameW(
        foreground, foregroundClass,
        (int)(sizeof(foregroundClass) / sizeof(foregroundClass[0])));

    static const wchar_t* const SHELL_CHROME_CLASSES[] =
    {
        L"Shell_TrayWnd",
        L"Shell_SecondaryTrayWnd",
        L"Progman",
        L"WorkerW",
    };

    for (const wchar_t* chromeClass : SHELL_CHROME_CLASSES)
    {
        if (_wcsicmp(foregroundClass, chromeClass) == 0)
            return;
    }

    bool isNativeSurface = false;

    for (const wchar_t* exeName :
         NATIVE_SHELL_SURFACE_PROCESSES)
    {
        if (IsWindowFromProcess(
                foreground,
                exeName))
        {
            isNativeSurface = true;
            break;
        }
    }

    if (isNativeSurface)
    {
        INPUT esc[2]{};

        esc[0].type = INPUT_KEYBOARD;
        esc[0].ki.wVk = VK_ESCAPE;

        esc[1].type = INPUT_KEYBOARD;
        esc[1].ki.wVk = VK_ESCAPE;
        esc[1].ki.dwFlags = KEYEVENTF_KEYUP;

        SendInput(
            2,
            esc,
            sizeof(INPUT));

        CancelPendingBlurClose();
        SetForegroundWindow(g_start);
        return;
    }

    // A genuinely different window — not one of ours, not shell chrome,
    // not the native Start/Search flash — took foreground. Don't close
    // immediately: arm the grace timer and let StartProc's WM_TIMER
    // handler do it once the grace period actually elapses, so a
    // transient blip (see the note above DismissNativeStartMenuIfNeeded)
    // has a chance to self-correct first. If one's already running, leave
    // it be rather than restarting the clock on every intermediate blip.
    if (!g_blurCloseTimer)
    {
        g_blurCloseTimer =
            SetTimer(g_start, TIMER_BLUR_CLOSE, BLUR_CLOSE_GRACE_MS, nullptr);
    }
}

static void CALLBACK ForegroundEventProc(
    HWINEVENTHOOK,
    DWORD event,
    HWND hwnd,
    LONG idObject,
    LONG,
    DWORD,
    DWORD)
{
    if (event != EVENT_SYSTEM_FOREGROUND ||
        idObject != OBJID_WINDOW)
    {
        return;
    }

    DismissNativeStartMenuIfNeeded(hwnd);
}

// ============================================================
// Keyboard hook
// ============================================================

static LRESULT CALLBACK KeyboardProc(
    int code,
    WPARAM wp,
    LPARAM lp)
{
    if (code != HC_ACTION)
    {
        return CallNextHookEx(
            g_keyboardHook,
            code,
            wp,
            lp);
    }

    KBDLLHOOKSTRUCT* key =
        reinterpret_cast<
            KBDLLHOOKSTRUCT*>(lp);

    if (!key)
    {
        return CallNextHookEx(
            g_keyboardHook,
            code,
            wp,
            lp);
    }

    if (key->flags &
        LLKHF_INJECTED)
    {
        return CallNextHookEx(
            g_keyboardHook,
            code,
            wp,
            lp);
    }

    DWORD vk =
        key->vkCode;

    bool down =
        wp == WM_KEYDOWN ||
        wp == WM_SYSKEYDOWN;

    bool up =
        wp == WM_KEYUP ||
        wp == WM_SYSKEYUP;

    // --------------------------------------------------------
    // Windows key — observed only, never intercepted.
    //
    // The physical key-down and key-up always reach
    // CallNextHookEx unchanged, every time, with no exceptions.
    // Eating a Win key-up (returning nonzero instead of calling
    // CallNextHookEx) leaves Windows' own key-state table
    // believing the key is still held, and every keystroke after
    // that gets misread as a Win+key shortcut until the user
    // reboots — see SPEC.md section 2.1/2.6 for the incident this
    // is guarding against. WinKeyTracker only ever tells us
    // *whether* a standalone tap just completed; it never gets a
    // vote on whether the OS sees the keystroke.
    // --------------------------------------------------------

    if (vk == VK_LWIN ||
        vk == VK_RWIN)
    {
        if (down)
        {
            g_winKeyTracker.OnWinKeyDown(vk);
        }
        else if (up)
        {
            // g_captureWinKey ([Experimental] CaptureWinKey in the ini,
            // off by default) gates only whether we *act* on a standalone
            // tap — the tracker still observes it either way, and the key
            // is unconditionally forwarded below regardless. With the
            // flag off, a Win tap is tracked but ToggleStart() never
            // fires, so the native/legacy Start menu is the only thing
            // that responds to it.
            if (g_winKeyTracker.OnWinKeyUp(vk) ==
                    WinKeyAction::Toggle &&
                g_captureWinKey)
            {
                ToggleStart();
            }
        }

        return CallNextHookEx(
            g_keyboardHook,
            code,
            wp,
            lp);
    }

    // --------------------------------------------------------
    // Preserve native Windows shortcuts.
    // --------------------------------------------------------

    if (down &&
        g_winKeyTracker.OnOtherKeyDown() ==
            WinKeyAction::Close)
    {
        if (g_startVisible)
            CloseStart();

        return CallNextHookEx(
            g_keyboardHook,
            code,
            wp,
            lp);
    }

    // --------------------------------------------------------
    // Ctrl+Esc
    // --------------------------------------------------------

    if (down &&
        vk == VK_ESCAPE &&
        (GetKeyState(VK_CONTROL) &
            0x8000))
    {
        ToggleStart();
        return 1;
    }

    // --------------------------------------------------------
    // Start search
    // --------------------------------------------------------

    if (g_startVisible &&
        down)
    {
        if (vk == VK_ESCAPE)
        {
            CloseStart();
            return 1;
        }

        if (vk == VK_RETURN)
        {
            HandleSearchEnter();
            return 1;
        }

        bool ctrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
        bool shift = (GetKeyState(VK_SHIFT) & 0x8000) != 0;

        if (vk == VK_LEFT || vk == VK_RIGHT ||
            vk == VK_HOME || vk == VK_END ||
            ((vk == 'A' || vk == 'C') && ctrl))
        {
            if (HandleSearchBoxNavigationKey(g_start, vk, ctrl, shift))
                return 1;
        }

        if (vk == VK_BACK)
        {
            DeleteSearchSelectionOrCharBefore(g_start);
            return 1;
        }

        if (vk == VK_DELETE)
        {
            DeleteSearchSelectionOrCharAfter(g_start);
            return 1;
        }

        if (!ctrl && IsPrintableKey(vk))
        {
            wchar_t c =
                VirtualKeyToChar(vk);

            if (c >= 32)
            {
                InsertSearchChar(g_start, c);
                return 1;
            }
        }
    }

    return CallNextHookEx(
        g_keyboardHook,
        code,
        wp,
        lp);
}

// ============================================================
// Taskbar Start-button hit testing
// ============================================================

static bool IsStartButtonClick(
    POINT point)
{
    HWND taskbar =
        FindWindowW(
            L"Shell_TrayWnd",
            nullptr);

    if (!taskbar)
        return false;

    RECT taskbarRect{};

    if (!GetWindowRect(
            taskbar,
            &taskbarRect))
    {
        return false;
    }

    return IsStartButtonHit(
        point,
        taskbarRect,
        S(62),
        S(34),
        S(32));
}

// ============================================================
// Mouse hook
// ============================================================
//
// Left-clicking the taskbar Start button hijacks into this app's own
// menu (see SPEC.md section 3.7) — right-click/middle-click on it are
// untouched, matching classic Windows behavior where only the left
// click opens Start. The WM_LBUTTONDOWN over the button is swallowed
// to fire our own toggle; g_startButtonTracker guarantees the matching
// WM_LBUTTONUP is swallowed too, unconditionally, so this can never
// turn into the same asymmetric-swallow bug that caused the Windows-key
// incident (see SPEC.md section 2.1) — just for a mouse button instead
// of a modifier key.

static LRESULT CALLBACK MouseProc(
    int code,
    WPARAM wp,
    LPARAM lp)
{
    if (code == HC_ACTION)
    {
        MSLLHOOKSTRUCT* mouse =
            reinterpret_cast<
                MSLLHOOKSTRUCT*>(lp);

        if (mouse &&
            !(mouse->flags &
              LLMHF_INJECTED))
        {
            if (wp == WM_LBUTTONUP)
            {
                // Only ever fed real WM_LBUTTONDOWN/WM_LBUTTONUP pairs
                // (see below) — never right/middle-button events — so a
                // capture started by a real left-button-down can't be
                // reset out from under it by an unrelated button.
                if (g_startButtonTracker.OnLeftButtonUp())
                    return 1;
            }
            else if (wp == WM_LBUTTONDOWN ||
                     wp == WM_RBUTTONDOWN ||
                     wp == WM_MBUTTONDOWN)
            {
                POINT point =
                    mouse->pt;

                if (wp == WM_LBUTTONDOWN)
                {
                    bool hitStartButton =
                        IsStartButtonClick(point);

                    if (g_startButtonTracker.OnLeftButtonDown(
                            hitStartButton))
                    {
                        ToggleStart();
                        return 1;
                    }
                }

                // Any click (left, right, or middle) landing outside
                // the menu while it's open dismisses it — except inside
                // the search results panel or the hover-preview panel
                // (both separate windows sitting beside the menu by
                // design), which handle their own clicks and shouldn't
                // read as "clicked away."
                if (g_startVisible)
                {
                    RECT r =
                        GetStartRect();

                    bool insideSatellite = false;

                    if (g_searchPanel &&
                        IsWindowVisible(g_searchPanel))
                    {
                        RECT panelRect{};

                        if (GetWindowRect(g_searchPanel, &panelRect))
                            insideSatellite = PtInRect(&panelRect, point) != FALSE;
                    }

                    if (!insideSatellite &&
                        g_preview &&
                        IsWindowVisible(g_preview))
                    {
                        RECT previewRect{};

                        if (GetWindowRect(g_preview, &previewRect))
                            insideSatellite = PtInRect(&previewRect, point) != FALSE;
                    }

                    if (!PtInRect(
                            &r,
                            point) &&
                        !insideSatellite)
                    {
                        CloseStart();
                    }
                }
            }
        }
    }

    return CallNextHookEx(
        g_mouseHook,
        code,
        wp,
        lp);
}

// ============================================================
// DPI
// ============================================================

static void InitializeDpi()
{
    HMODULE user32 =
        GetModuleHandleW(
            L"user32.dll");

    if (user32)
    {
        typedef BOOL(WINAPI*
            SetDpiContextProc)(
                DPI_AWARENESS_CONTEXT);

        auto proc =
            reinterpret_cast<
                SetDpiContextProc>(
                GetProcAddress(
                    user32,
                    "SetProcessDpiAwarenessContext"));

        if (proc)
        {
            proc(
                DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
        }
    }

    HDC dc =
        GetDC(nullptr);

    if (dc)
    {
        g_dpi =
            GetDeviceCaps(
                dc,
                LOGPIXELSX);

        if (!g_dpi)
            g_dpi = 96;

        ReleaseDC(
            nullptr,
            dc);
    }
}

// ============================================================
// Entry
// ============================================================

static void KillOtherInstances()
{
    DWORD currentPid = GetCurrentProcessId();

    std::wstring currentPath =
        GetExePath();

    const wchar_t* currentName =
        wcsrchr(
            currentPath.c_str(),
            L'\\');

    currentName =
        currentName
            ? currentName + 1
            : currentPath.c_str();

    HANDLE snapshot =
        CreateToolhelp32Snapshot(
            TH32CS_SNAPPROCESS,
            0);

    if (snapshot == INVALID_HANDLE_VALUE)
        return;

    PROCESSENTRY32W entry;
    entry.dwSize = sizeof(entry);

    if (Process32FirstW(
            snapshot,
            &entry))
    {
        do
        {
            if (entry.th32ProcessID == currentPid)
                continue;

            if (_wcsicmp(
                    entry.szExeFile,
                    currentName) != 0)
                continue;

            HANDLE process =
                OpenProcess(
                    PROCESS_TERMINATE |
                        SYNCHRONIZE,
                    FALSE,
                    entry.th32ProcessID);

            if (process)
            {
                TerminateProcess(
                    process,
                    0);

                WaitForSingleObject(
                    process,
                    2000);

                CloseHandle(
                    process);
            }
        } while (Process32NextW(
            snapshot,
            &entry));
    }

    CloseHandle(
        snapshot);
}

int WINAPI wWinMain(
    HINSTANCE instance,
    HINSTANCE,
    PWSTR,
    int)
{
    KillOtherInstances();

    HRESULT com =
        CoInitializeEx(
            nullptr,
            COINIT_APARTMENTTHREADED |
                COINIT_DISABLE_OLE1DDE);

    InitializeDpi();

    Gdiplus::GdiplusStartupInput gdiplusInput;

    GdiplusStartup(
        &g_gdiplusToken,
        &gdiplusInput,
        nullptr);

    LoadConfig();

    RefreshSystemColors();

    CreateFonts();

    CreateIcons();

    BuildVisibleItems();

    LoadQuickTools();

    if (!CreateStartWindow(
            instance))
    {
        MessageBoxW(
            nullptr,
            L"Could not create ClassicShell.",
            L"ClassicShell",
            MB_OK |
                MB_ICONERROR);

        DestroyIcons();
        DestroyFonts();

        Gdiplus::GdiplusShutdown(
            g_gdiplusToken);

        if (SUCCEEDED(com))
            CoUninitialize();

        return 1;
    }

    // Non-fatal if either of these fails — the app is fully usable
    // without live search results/God Mode, it just silently goes
    // without them (the search box still resolves typed commands as
    // before via HandleSearchEnter's ExecuteSmartInput fallback).
    CreateSearchPanelWindow(instance);
    CreatePreviewWindow(instance);
    CreateToastWindow(instance);
    CreateOpacityHighlightWindow(instance);
    StartBackgroundIndexing();
    StartGodModeIndexing();

    g_keyboardHook =
        SetWindowsHookExW(
            WH_KEYBOARD_LL,
            KeyboardProc,
            instance,
            0);

    if (!g_keyboardHook)
    {
        MessageBoxW(
            nullptr,
            L"Could not install the keyboard hook.",
            L"ClassicShell",
            MB_OK |
                MB_ICONERROR);

        DestroyWindow(
            g_start);

        g_start = nullptr;

        DestroyIcons();
        DestroyFonts();

        Gdiplus::GdiplusShutdown(
            g_gdiplusToken);

        if (SUCCEEDED(com))
            CoUninitialize();

        return 1;
    }

    g_mouseHook =
        SetWindowsHookExW(
            WH_MOUSE_LL,
            MouseProc,
            instance,
            0);

    if (!g_mouseHook)
    {
        MessageBoxW(
            nullptr,
            L"Could not install the mouse hook.",
            L"ClassicShell",
            MB_OK |
                MB_ICONERROR);

        UnhookWindowsHookEx(
            g_keyboardHook);

        g_keyboardHook = nullptr;

        DestroyWindow(
            g_start);

        g_start = nullptr;

        DestroyIcons();
        DestroyFonts();

        Gdiplus::GdiplusShutdown(
            g_gdiplusToken);

        if (SUCCEEDED(com))
            CoUninitialize();

        return 1;
    }

    g_foregroundEventHook =
        SetWinEventHook(
            EVENT_SYSTEM_FOREGROUND,
            EVENT_SYSTEM_FOREGROUND,
            nullptr,
            ForegroundEventProc,
            0,
            0,
            WINEVENT_OUTOFCONTEXT);

    MSG msg{};

    while (true)
    {
        BOOL result =
            GetMessageW(
                &msg,
                nullptr,
                0,
                0);

        if (result <= 0)
            break;

        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    if (g_foregroundEventHook)
    {
        UnhookWinEvent(
            g_foregroundEventHook);

        g_foregroundEventHook = nullptr;
    }

    if (g_mouseHook)
    {
        UnhookWindowsHookEx(
            g_mouseHook);

        g_mouseHook = nullptr;
    }

    if (g_keyboardHook)
    {
        UnhookWindowsHookEx(
            g_keyboardHook);

        g_keyboardHook = nullptr;
    }

    if (g_start)
    {
        DestroyWindow(
            g_start);

        g_start = nullptr;
    }

    if (g_searchPanel)
    {
        DestroyWindow(
            g_searchPanel);

        g_searchPanel = nullptr;
    }

    if (g_preview)
    {
        ClearPreviewContent();

        DestroyWindow(
            g_preview);

        g_preview = nullptr;
    }

    if (g_toast)
    {
        DestroyWindow(g_toast);
        g_toast = nullptr;
    }

    if (g_opacityHighlight)
    {
        DestroyWindow(g_opacityHighlight);
        g_opacityHighlight = nullptr;
    }

    DestroyIcons();
    DestroyFonts();

    Gdiplus::GdiplusShutdown(
        g_gdiplusToken);

    if (SUCCEEDED(com))
        CoUninitialize();

    return 0;
}
