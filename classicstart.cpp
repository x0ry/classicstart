// ClassicStart — a lightweight, classic-style Start menu replacement.
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

#include <string>
#include <vector>
#include <algorithm>
#include <cwctype>
#include <fstream>
#include <sstream>

#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "msimg32.lib")
#pragma comment(lib, "version.lib")
#pragma comment(lib, "gdiplus.lib")

static const wchar_t START_CLASS[] = L"ClassicStart.Native";

static HWND  g_start = nullptr;
static HHOOK g_keyboardHook = nullptr;
static HHOOK g_mouseHook = nullptr;

static HFONT g_font = nullptr;
static HFONT g_bold = nullptr;
static HFONT g_small = nullptr;
static HFONT g_icon = nullptr;

static UINT g_dpi = 96;

// Overall UI scale, independent of DPI. Configurable via
// classicstart_scale.txt next to the exe; defaults to 0.85 (15%
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

static int g_hover = -1;
static int g_powerHover = -1;

// Keyboard focus, as a flat index into a dynamic sequence of
// focusable controls (search, the 7 items, any quick-launch
// tools, the opacity slider, the power button, and — only while
// the flyout is open — restart/shutdown). -1 means the user
// hasn't started tabbing yet, so no focus ring is drawn.
static int g_focusIndex = -1;

static std::wstring g_searchText;

static bool g_lwinDown = false;
static bool g_rwinDown = false;
static bool g_winCombo = false;

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

static void RefreshSystemColors()
{
    // Dark, slightly blue-black base.
    g_bg = RGB(14, 15, 18);

    // Main surface.
    g_panel = RGB(24, 26, 31);

    // Fine separators / borders.
    g_border = RGB(53, 57, 64);

    // Windows accent — the real one from the current theme.
    g_accent = GetWindowsAccentColor();
    g_accentText = ContrastTextFor(g_accent);

    // Accent, lightened — the border/outline shown around a
    // selected row or an active button.
    g_accentBorder = MixColor(
        g_accent,
        RGB(255, 255, 255),
        18);

    g_text = RGB(237, 240, 244);
    g_muted = RGB(145, 151, 161);

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

static void HandleSearchEnter()
{
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

        OpenNativeRun();

        return;
    }

    LaunchResult result =
        ExecuteSmartInput(command);

    if (result ==
        LaunchResult::Success)
    {
        g_searchText.clear();

        ShowWindow(
            g_start,
            SW_HIDE);

        g_startVisible = false;
        ResetUIState();

        return;
    }

    g_searchText.clear();

    OpenNativeRun();
}

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

// The full set of menu items ClassicStart knows how to show. Which
// of these are actually visible is configurable (classicstart.ini,
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
// all of ClassicStart's plain-text config files are expected.
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
// Configuration (classicstart.ini)
// ============================================================
//
// One old-school INI file next to the exe holds every user-facing
// setting. If it isn't there on startup, a fully-commented default
// copy is written out first, so there's always something present
// to find and hand-edit — nothing to configure by trial and error.

static const wchar_t CONFIG_FILE_NAME[] = L"classicstart.ini";

static const char DEFAULT_CONFIG_CONTENT[] =
"; ClassicStart configuration.\r\n"
"; Edit this file to customize ClassicStart, then relaunch it (or\r\n"
"; just reopen the menu, for settings that refresh live) to pick\r\n"
"; up changes. Delete this file to regenerate these defaults.\r\n"
"\r\n"
"[Appearance]\r\n"
"; Overall UI scale, applied on top of normal DPI scaling.\r\n"
"; 1.0 = original full size. Valid range: 0.3 - 2.0.\r\n"
"Scale=0.85\r\n"
"\r\n"
"; Window opacity when ClassicStart opens, 90-255. Adjustable live\r\n"
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
"; Every row ClassicStart can show, in order. Set any of these to\r\n"
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
"Run=1\r\n";

static std::wstring GetConfigPath()
{
    return
        GetExeDirectory() +
        CONFIG_FILE_NAME;
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

// Reads classicstart.ini's [QuickTools] section. Called both at
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

// Reads classicstart.ini's [Appearance] section: scale and start
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

    // Key under classicstart.ini's [MenuItems] section that toggles
    // this item on/off.
    const wchar_t* iniKey;
};

// Every item ClassicStart can show. Order here is the default
// order; BuildVisibleItems() filters this down to g_items based on
// classicstart.ini.
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

// Reads classicstart.ini's [MenuItems] section (one on/off toggle
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

    SetLayeredWindowAttributes(
        hwnd,
        0,
        g_windowAlpha,
        LWA_ALPHA);

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

    // GradientColor is 0xAABBGGRR. COLORREF is already 0x00BBGGRR,
    // so only the alpha byte needs adding on top.
    DWORD tint =
        (DWORD)g_panel &
        0x00FFFFFF;

    DWORD gradientColor =
        (0x90u << 24) |
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
    // search + items + quick tools + slider + power
    int n =
        1 +
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

    SetLayeredWindowAttributes(
        hwnd,
        0,
        g_windowAlpha,
        LWA_ALPHA);

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

        DrawTextSimple(
            back,
            g_searchText.c_str(),
            S(46),
            search.top,
            width - S(58),
            S(48),
            g_text,
            g_font);

        // Blinking text-entry caret, right after the typed text.
        if (g_caretVisible)
        {
            HGDIOBJ oldFont =
                SelectObject(
                    back,
                    g_font);

            SIZE textSize{ 0, 0 };

            GetTextExtentPoint32W(
                back,
                g_searchText.c_str(),
                (int)g_searchText.size(),
                &textSize);

            SelectObject(
                back,
                oldFont);

            int caretX =
                S(46) +
                textSize.cx +
                S(3);

            RECT caret =
            {
                caretX,
                search.top + S(9),
                caretX + S(2),
                search.top + S(39)
            };

            FillRectColor(
                back,
                caret,
                g_accent);
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

            InvalidateRect(
                hwnd,
                nullptr,
                FALSE);

            return 0;
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

        case WM_LBUTTONUP:
        {
            if (g_sliderDragging)
            {
                g_sliderDragging = false;

                ReleaseCapture();

                return 0;
            }

            break;
        }

        case WM_CAPTURECHANGED:
        {
            g_sliderDragging = false;

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
                wp == VK_RIGHT)
            {
                AdjustFocusedSlider(
                    hwnd,
                    wp == VK_RIGHT
                        ? 1
                        : -1);

                return 0;
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
                if (!g_searchText.empty())
                {
                    g_searchText.pop_back();

                    g_caretVisible = true;

                    if (g_searchText.empty())
                    {
                        ResizeStartToContent(
                            hwnd);
                    }

                    InvalidateRect(
                        hwnd,
                        nullptr,
                        FALSE);
                }

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
                bool wasEmpty =
                    g_searchText.empty();

                g_searchText += c;

                g_caretVisible = true;

                if (wasEmpty)
                {
                    ResizeStartToContent(
                        hwnd);
                }

                InvalidateRect(
                    hwnd,
                    nullptr,
                    FALSE);

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
        CS_VREDRAW;

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
            L"ClassicStart",
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
    // Left Windows key
    // --------------------------------------------------------

    if (vk == VK_LWIN)
    {
        if (down)
        {
            g_lwinDown = true;
            g_winCombo = false;

            return CallNextHookEx(
                g_keyboardHook,
                code,
                wp,
                lp);
        }

        if (up)
        {
            bool standalone =
                g_lwinDown &&
                !g_winCombo;

            g_lwinDown = false;

            if (!g_lwinDown &&
                !g_rwinDown)
            {
                g_winCombo = false;
            }

            if (standalone)
            {
                ToggleStart();
                return 1;
            }

            return CallNextHookEx(
                g_keyboardHook,
                code,
                wp,
                lp);
        }
    }

    // --------------------------------------------------------
    // Right Windows key
    // --------------------------------------------------------

    if (vk == VK_RWIN)
    {
        if (down)
        {
            g_rwinDown = true;
            g_winCombo = false;

            return CallNextHookEx(
                g_keyboardHook,
                code,
                wp,
                lp);
        }

        if (up)
        {
            bool standalone =
                g_rwinDown &&
                !g_winCombo;

            g_rwinDown = false;

            if (!g_lwinDown &&
                !g_rwinDown)
            {
                g_winCombo = false;
            }

            if (standalone)
            {
                ToggleStart();
                return 1;
            }

            return CallNextHookEx(
                g_keyboardHook,
                code,
                wp,
                lp);
        }
    }

    // --------------------------------------------------------
    // Preserve native Windows shortcuts.
    // --------------------------------------------------------

    if (down &&
        (g_lwinDown ||
         g_rwinDown))
    {
        g_winCombo = true;

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

        if (vk == VK_BACK)
        {
            if (!g_searchText.empty())
            {
                g_searchText.pop_back();

                if (g_searchText.empty())
                {
                    ResizeStartToContent(
                        g_start);
                }

                InvalidateRect(
                    g_start,
                    nullptr,
                    FALSE);
            }

            return 1;
        }

        if (IsPrintableKey(vk))
        {
            wchar_t c =
                VirtualKeyToChar(vk);

            if (c >= 32)
            {
                bool wasEmpty =
                    g_searchText.empty();

                g_searchText += c;

                if (wasEmpty)
                {
                    ResizeStartToContent(
                        g_start);
                }

                InvalidateRect(
                    g_start,
                    nullptr,
                    FALSE);

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
// Taskbar Start click
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

    if (!PtInRect(
            &taskbarRect,
            point))
    {
        return false;
    }

    int taskbarHeight =
        taskbarRect.bottom -
        taskbarRect.top;

    int usableHeight =
        taskbarHeight > S(32)
            ? taskbarHeight
            : S(32);

    RECT left =
    {
        taskbarRect.left,
        taskbarRect.bottom -
            usableHeight,
        taskbarRect.left + S(62),
        taskbarRect.bottom
    };

    if (PtInRect(
            &left,
            point))
    {
        return true;
    }

    int center =
        taskbarRect.left +
        (taskbarRect.right -
         taskbarRect.left) / 2;

    RECT centered =
    {
        center - S(34),
        taskbarRect.bottom -
            usableHeight,
        center + S(34),
        taskbarRect.bottom
    };

    return PtInRect(
        &centered,
        point) != FALSE;
}

// ============================================================
// Mouse hook
// ============================================================

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
              LLMHF_INJECTED) &&
            (wp == WM_LBUTTONDOWN ||
             wp == WM_RBUTTONDOWN ||
             wp == WM_MBUTTONDOWN))
        {
            POINT point =
                mouse->pt;

            if (wp == WM_LBUTTONDOWN &&
                IsStartButtonClick(
                    point))
            {
                ToggleStart();
                return 1;
            }

            // Any click (left, right, or middle) landing outside
            // the menu while it's open dismisses it.
            if (g_startVisible)
            {
                RECT r =
                    GetStartRect();

                if (!PtInRect(
                        &r,
                        point))
                {
                    CloseStart();
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
            L"Could not create ClassicStart.",
            L"ClassicStart",
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
            L"ClassicStart",
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
            L"ClassicStart",
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

    DestroyIcons();
    DestroyFonts();

    Gdiplus::GdiplusShutdown(
        g_gdiplusToken);

    if (SUCCEEDED(com))
        CoUninitialize();

    return 0;
}
