// ClassicShell — a lightweight, classic-style Start menu replacement.
// Copyright (c) 2026 cory@coryglenn.ai
// SPDX-License-Identifier: MIT — see LICENSE for the full text.

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX

#include <windows.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shlwapi.h>
#include <objbase.h>
#include <objidl.h>
#include <dwmapi.h>
#include <tlhelp32.h>
#include <winioctl.h>
#include <winver.h>
#include <gdiplus.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <d2d1_3.h>
#include <d2d1svg.h>
#include <wincodec.h>

#include "starthook.h"

#include <string>
#include <vector>
#include <algorithm>
#include <cwctype>
#include <fstream>
#include <sstream>
#include <mutex>
#include <atomic>
#include <cmath>

#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "shlwapi.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "msimg32.lib")
#pragma comment(lib, "version.lib")
#pragma comment(lib, "gdiplus.lib")
#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "d2d1.lib")
#pragma comment(lib, "windowscodecs.lib")

static const wchar_t START_CLASS[] = L"ClassicShell.Native";
static const wchar_t PREVIEW_CLASS[] = L"ClassicShell.Preview";
static const wchar_t TOAST_CLASS[] = L"ClassicShell.Toast";
static const wchar_t OPACITY_HIGHLIGHT_CLASS[] =
    L"ClassicShell.OpacityHighlight";

static HWND  g_start = nullptr;
static HWND  g_preview = nullptr;
static HWND  g_toast = nullptr;
static HWND  g_opacityHighlight = nullptr;
static HHOOK g_keyboardHook = nullptr;
static HHOOK g_mouseHook = nullptr;
static HWINEVENTHOOK g_foregroundEventHook = nullptr;

static HFONT g_font = nullptr;
static HFONT g_bold = nullptr;
static HFONT g_small = nullptr;
static HFONT g_icon = nullptr;
static HFONT g_mono = nullptr;

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

// The little clear ("×") button at the search box's right edge —
// only ever hit-testable while there's text to clear, same as the
// box itself only showing then.
static bool g_searchClearHover = false;

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

static const UINT_PTR TIMER_SEARCH_RESULTS_ANIM = 5;

// Whole-window translucency, user-adjustable at runtime via the
// opacity slider in the bottom strip.
static const BYTE START_WINDOW_ALPHA = 225;
static const BYTE OPACITY_MIN = 90;
static const BYTE OPACITY_MAX = 255;

// The value the slider currently shows/controls — the Start menu's
// own opacity while it's the target, or whatever external window is
// currently targeted (see the opacity-targeting section further
// down). g_startOwnAlpha is the Start menu's own setting specifically,
// remembered separately so it's exactly what it was when control
// returns to it after targeting something else.
static BYTE g_windowAlpha = START_WINDOW_ALPHA;
static BYTE g_startOwnAlpha = START_WINDOW_ALPHA;
static bool g_sliderDragging = false;

// Whether the mouse is currently hovering (or dragging) the opacity
// slider — the opacity highlight only ever shows while this is
// true, never just because the menu happens to be open.
static bool g_sliderHover = false;

// 0 = the Start menu itself is the slider's target (the default);
// >= 1 = index (g_opacityIndex - 1) into g_opacityExternalWindows.
// Declared here (rather than alongside the rest of the opacity-
// targeting machinery further down, which needs EnumWindows/DWM) so
// early code like ResetUIState() can reference it directly.
static int g_opacityIndex = 0;

// Whichever window (g_start, or some other real window on the
// desktop) the highlight overlay currently frames — declared here
// rather than with the rest of the opacity-targeting machinery
// further down (which needs EnumWindows/DWM) so early code like
// StepShowAnim's animation tick can keep the highlight in lockstep
// while the menu grows/shrinks.
static HWND g_opacityHighlightTarget = nullptr;

// Forward declarations — early code (ResetUIState, StepShowAnim)
// needs these, but the full opacity-targeting machinery (which
// needs EnumWindows/DWM) is grouped later, next to the slider code
// it belongs with. StopOpacityHighlight does NOT undo a window's
// actual opacity — see that section's own comment for why.
static void StopOpacityHighlight();
static void RepositionOpacityHighlight(HWND target);

static int g_hover = -1;
static int g_powerHover = -1;

// Keyboard focus, as a flat index into a dynamic sequence of
// focusable controls (search, the 7 items, any quick-launch
// tools, the opacity slider, the power button, and — only while
// the flyout is open — restart/shutdown). -1 means the user
// hasn't started tabbing yet, so no focus ring is drawn.
static int g_focusIndex = -1;

static std::wstring g_searchText;

// Caret position, in characters (0..g_searchText.size()). No
// longer assumed to always be at the end, now that the box
// supports clicking and dragging to position/select within it.
static int g_searchCaretPos = 0;

// The other end of the current selection, or -1 if there isn't
// one (caret-only, nothing highlighted). The selected range is
// [min(anchor, caret), max(anchor, caret)).
static int g_searchSelAnchor = -1;

// True while the left button is held down after starting a click
// inside the search box, so WM_MOUSEMOVE knows to keep extending
// the selection instead of doing its usual hover checks.
static bool g_searchDragging = false;

// ============================================================
// Background file index (wildcard search autocomplete)
// ============================================================

struct IndexedFile
{
    std::wstring fullPath;
    std::wstring fileName;

    // Lowercased once at index time so implicit contains-matching
    // (a plain query with no '*'/'?' of its own) can do a cheap
    // substring search per keystroke instead of re-lowering every
    // filename on every refresh.
    std::wstring fileNameLower;
};

// classicshell.ini's [Search] IndexPath, resolved once at startup.
static std::wstring g_searchIndexRoot;

// Filled in by a low-priority background thread started once at
// launch; RefreshSearchResults() takes the lock only for the brief
// scan/copy, so it never contends with the indexer for long.
static std::vector<IndexedFile> g_fileIndex;
static std::mutex g_fileIndexMutex;
static std::atomic<size_t> g_indexedFileCount{ 0 };

static const size_t MAX_INDEX_FILES = 200000;

// File: a real filesystem match, same as always. GodMode: one entry
// from the Control Panel "All Tasks" catalog — see the God Mode
// section further down — surfaced either by browsing the catalog
// directly (Control Panel row click) or by a typed query matching
// its name.
enum class SearchResultKind
{
    File,
    GodMode
};

struct SearchResultEntry
{
    // File: the full path. GodMode: the task's display name — the
    // row painter and LaunchSearchResult both branch on kind, so
    // this single field never needs to hold two different shapes of
    // data at once.
    std::wstring path;
    HICON icon;
    SearchResultKind kind = SearchResultKind::File;
    int godModeIndex = -1;
};

// Matches against g_fileIndex, refreshed whenever g_searchText
// changes. A query with an explicit '*' or '?' is matched as a
// literal wildcard pattern; plain text of 2+ characters is matched
// as an implicit "contains" search (as if bookended with '*'s)
// instead, so typing e.g. "report" finds report.docx without
// needing "*report*". See g_searchResultsIsWildcard for why the
// two cases still behave differently on Enter. Also doubles as the
// Control Panel row's own "browse the whole God Mode catalog" list
// (see g_controlPanelBrowsing) — same struct, same rendering, same
// scrolling/keyboard-focus plumbing either way.
static std::vector<SearchResultEntry> g_searchResults;
static int g_searchResultHover = -1;

// True while the side panel is showing the full God Mode catalog
// (Control Panel row clicked) rather than actual search matches.
// Typing anything always ends this and falls back to a real search,
// the same way clicking a different item or closing the menu does.
static bool g_controlPanelBrowsing = false;

// True only when the current g_searchResults came from an explicit
// '*'/'?' pattern, as opposed to the implicit contains-match applied
// to plain text. HandleSearchEnter() uses this to decide whether a
// file match should preempt Enter (explicit patterns are clearly
// file searches) or merely be a fallback behind normal command
// resolution (implicit matches, so typing "calc" still launches
// Calculator on Enter even though some file also matches "calc").
static bool g_searchResultsIsWildcard = false;

// How far the result list has been scrolled, in whole rows. Reset
// to 0 on every new query so a narrower result set never starts
// scrolled past its own end.
static int g_searchResultsScroll = 0;

// A generous cap rather than a hard limit meant to be felt — high
// enough that "as many as matched" is true in practice, while still
// bounding the per-keystroke icon-fetching cost.
static const size_t MAX_SEARCH_RESULTS = 50;

// God Mode matches are always shown first (see RefreshSearchResults)
// but capped low — a broad query matching a dozen Control Panel
// tasks shouldn't be able to push every actual file match off the
// visible list.
static const size_t MAX_GODMODE_SEARCH_MATCHES = 6;

// 0 = fully hidden, 1 = fully shown. Eased toward 1 (with a touch
// of overshoot) every time the match list refreshes with content,
// so the side panel gives a little pop instead of just appearing;
// eased back to 0 — keeping the window wide until it settles —
// when matches disappear.
static float g_searchResultsAnim = 0.0f;
static UINT_PTR g_searchResultsTimer = 0;

static WinKeyTracker g_winKeyTracker;

static bool g_mouseTracking = false;

// ============================================================
// Hover preview (text / image quick-look)
// ============================================================
//
// A small translucent panel that pops up in the work area's
// opposite corner from the menu when the mouse rests on a search
// result row long enough — a quick look at the file without
// launching it. Text/plaintext files render as a scrollable page
// in a fixed-size box (mouse wheel scrolls it once the cursor is
// actually over the panel); images scale to fill a box sized so the
// rendered photo occupies roughly a quarter of the screen, whatever
// its native resolution. Losing hover doesn't hide it immediately —
// see BeginPreviewFadeOut() — so there's real time to move the
// mouse across the screen into the panel and use it before it's
// gone; hovering a different result always overrides/replaces it
// outright, fade or no fade.

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
static Gdiplus::Bitmap* g_previewImage = nullptr;
static int g_previewImageRenderW = 0;
static int g_previewImageRenderH = 0;

// Only set for an SVG preview — a Gdiplus::Bitmap built from raw
// pixels (see RenderSvgToPixels) wraps that memory rather than
// copying it, so the buffer has to outlive g_previewImage. Freed
// alongside it.
static std::vector<BYTE>* g_previewImagePixels = nullptr;

// Which search result the panel is currently showing (or about to,
// once the hover timer fires) — lets a stray WM_TIMER after the
// hover moved on get ignored instead of loading the wrong file.
static int g_previewHoverIndex = -1;

static UINT_PTR g_previewTimer = 0;
static const UINT_PTR TIMER_PREVIEW_HOVER = 6;

// The panel's own opacity fraction (1 = fully shown), independent
// of the whole-window alpha the rest of the app uses — eased down
// to 0 by TIMER_PREVIEW_FADE once BeginPreviewFadeOut()'s grace
// period elapses with hover still lost everywhere.
static float g_previewAlpha = 1.0f;
static DWORD g_previewFadeStartTick = 0;
static UINT_PTR g_previewFadeTimer = 0;
static const UINT_PTR TIMER_PREVIEW_FADE = 7;

static const DWORD PREVIEW_FADE_GRACE_MS = 550;
static const DWORD PREVIEW_FADE_DURATION_MS = 250;
static const BYTE PREVIEW_BASE_ALPHA = 235;

// True while the preview window itself is being tracked for its
// own WM_MOUSELEAVE, the same TrackMouseEvent dance StartProc does
// for the main menu.
static bool g_previewMouseTracking = false;

// Text selection inside the preview panel, as absolute character
// offsets into g_previewText — simplest representation both for
// turning a selection into a clipboard string (a plain substring)
// and for comparing against the syntax-highlighting spans below,
// which are offset-based too. Anchor is where the drag started;
// caret is wherever the mouse is now (or was on release). A plain
// click with no drag leaves g_previewHasSelection false, the same
// "click clears selection" behavior any text control has.
static bool g_previewSelecting = false;
static size_t g_previewSelAnchor = 0;
static size_t g_previewSelCaret = 0;
static bool g_previewHasSelection = false;

// The panel's own scrollbar thumb, click-and-dragged the same way —
// separate from text selection above since a click landing on the
// bar should grab it instead of starting a selection. dragGrabOffset
// is how far below the thumb's own top edge the mouse grabbed it,
// so dragging tracks the cursor smoothly instead of snapping the
// thumb to be centered under it the instant the drag starts.
static bool g_previewScrollbarDragging = false;
static bool g_previewScrollbarHover = false;
static int g_previewScrollbarDragGrabOffset = 0;

// Syntax highlighting for a pretty-printed JSON/XML preview only —
// a plain-text file's g_previewColorSpans just stays empty and
// every character draws in the panel's normal foreground color.
enum class PreviewHighlightLang
{
    None,
    Json,
    Xml
};

enum class PreviewTokenColor
{
    Default,
    Punctuation,
    Key,
    String,
    Number,
    Keyword,
    Comment,
    TagName,
    AttrName,
    AttrValue
};

struct PreviewColorSpan
{
    size_t start;
    size_t end;
    PreviewTokenColor color;
};

static PreviewHighlightLang g_previewHighlightLang =
    PreviewHighlightLang::None;

static std::vector<PreviewColorSpan> g_previewColorSpans;

static void ApplyPreviewAlpha()
{
    if (!g_preview)
        return;

    SetLayeredWindowAttributes(
        g_preview,
        0,
        (BYTE)(PREVIEW_BASE_ALPHA *
               g_previewAlpha),
        LWA_ALPHA);
}

// Snaps the panel back to fully visible and stops any fade in
// progress — called the instant hover lands on a result again (even
// before its content finishes (re)loading) or the mouse actually
// reaches the panel, so the panel never visibly continues fading
// away out from under an interaction that just started.
static void CancelPreviewFadeOut()
{
    if (g_previewFadeTimer)
    {
        if (g_start)
        {
            KillTimer(
                g_start,
                TIMER_PREVIEW_FADE);
        }

        g_previewFadeTimer = 0;
    }

    if (g_previewAlpha != 1.0f)
    {
        g_previewAlpha = 1.0f;
        ApplyPreviewAlpha();
    }
}

// Starts the countdown to actually hiding the preview, rather than
// yanking it away the instant hover is lost on a row — the panel
// sits in the opposite corner of the screen, so the user needs real
// time to move the mouse there to interact with it. Does nothing if
// a fade is already running (don't restart the clock) or nothing is
// showing to begin with.
static void BeginPreviewFadeOut()
{
    if (g_previewKind == PreviewKind::None)
        return;

    if (g_previewFadeTimer || !g_start)
        return;

    g_previewFadeStartTick = GetTickCount();

    g_previewFadeTimer =
        SetTimer(
            g_start,
            TIMER_PREVIEW_FADE,
            16,
            nullptr);
}

static void HidePreview()
{
    g_previewKind = PreviewKind::None;
    g_previewPath.clear();
    g_previewText.clear();
    g_previewTextTruncated = false;
    g_previewTextScroll = 0;
    g_previewImageRenderW = 0;
    g_previewImageRenderH = 0;

    if (g_previewImage)
    {
        delete g_previewImage;
        g_previewImage = nullptr;
    }

    if (g_previewImagePixels)
    {
        delete g_previewImagePixels;
        g_previewImagePixels = nullptr;
    }

    g_previewHoverIndex = -1;

    if (g_previewSelecting ||
        g_previewScrollbarDragging)
    {
        ReleaseCapture();
        g_previewSelecting = false;
        g_previewScrollbarDragging = false;
    }

    g_previewScrollbarHover = false;
    g_previewHasSelection = false;
    g_previewSelAnchor = 0;
    g_previewSelCaret = 0;

    g_previewHighlightLang = PreviewHighlightLang::None;
    g_previewColorSpans.clear();

    if (g_previewTimer)
    {
        if (g_start)
        {
            KillTimer(
                g_start,
                TIMER_PREVIEW_HOVER);
        }

        g_previewTimer = 0;
    }

    if (g_previewFadeTimer)
    {
        if (g_start)
        {
            KillTimer(
                g_start,
                TIMER_PREVIEW_FADE);
        }

        g_previewFadeTimer = 0;
    }

    g_previewAlpha = 1.0f;

    if (g_preview)
    {
        ShowWindow(
            g_preview,
            SW_HIDE);
    }
}

// ============================================================
// Toast notifications
// ============================================================
//
// A brief, translucent heads-up in the same corner as the hover
// preview — used for things worth a quiet "here's what happened"
// after the menu's already closed, like a menu item whose target
// couldn't be found. Shows fully opaque for a hold period, eases
// out over a short fade, then hides — no interaction, no dismiss
// button, matching how a real OS toast behaves.

static std::wstring g_toastTitle;
static std::wstring g_toastDetail;
static float g_toastAlpha = 1.0f;
static DWORD g_toastStartTick = 0;
static UINT_PTR g_toastTimer = 0;
static const UINT_PTR TIMER_TOAST = 8;

static const DWORD TOAST_HOLD_MS = 3200;
static const DWORD TOAST_FADE_MS = 350;
static const BYTE TOAST_BASE_ALPHA = 235;

static void HideToast()
{
    if (g_toastTimer)
    {
        if (g_start)
        {
            KillTimer(
                g_start,
                TIMER_TOAST);
        }

        g_toastTimer = 0;
    }

    g_toastAlpha = 1.0f;

    if (g_toast)
    {
        ShowWindow(
            g_toast,
            SW_HIDE);
    }
}

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

    // Keeps the highlight growing/shrinking in lockstep with the
    // menu itself instead of lagging a beat behind — the opacity
    // position-tracking timer alone only catches up once every
    // 150ms, plenty slow enough to be visible against a ~150-200ms
    // open/close animation.
    if (g_opacityHighlightTarget == hwnd)
        RepositionOpacityHighlight(hwnd);

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

// Destroys the icons owned by the current wildcard-match results
// and empties the list. Safe to call whether or not there are any.
// Deliberately leaves g_searchResultsAnim alone: this runs at the
// start of every RefreshSearchResults() call, including ones where
// results stay non-empty across a keystroke, and resetting it here
// would force the panel to invisibly restart its pop-in on every
// single character typed instead of just the first one.
static void ClearSearchResults()
{
    for (auto& result : g_searchResults)
    {
        // A GodMode entry's icon is owned by the long-lived catalog
        // cache (see g_godModeItems), not this one result list — it
        // has to survive being cleared here since the exact same
        // HICON gets reused the next time that item shows up.
        if (result.icon &&
            result.kind == SearchResultKind::File)
        {
            DestroyIcon(result.icon);
        }
    }

    g_searchResults.clear();
    g_searchResultHover = -1;
    g_searchResultsScroll = 0;
    g_controlPanelBrowsing = false;

    HidePreview();
}

static void ResetUIState()
{
    g_powerOpen = false;
    g_powerAnim = 0.0f;

    g_searchHover = false;
    g_searchAnim = 0.0f;
    g_searchClearHover = false;

    g_caretVisible = true;

    g_searchCaretPos = 0;
    g_searchSelAnchor = -1;
    g_searchDragging = false;

    g_focusIndex = -1;

    g_sliderHover = false;
    StopOpacityHighlight();
    g_opacityIndex = 0;
    g_windowAlpha = g_startOwnAlpha;

    ClearSearchResults();

    // Unlike ClearSearchResults() itself, this unconditional reset
    // is correct here: the whole menu is closing, so there's no
    // "still showing, just updating" case to preserve the pop-in
    // progress for.
    g_searchResultsAnim = 0.0f;

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

        if (g_searchResultsTimer)
            KillTimer(
                g_start,
                TIMER_SEARCH_RESULTS_ANIM);
    }

    g_powerTimer = 0;
    g_searchTimer = 0;
    g_caretTimer = 0;
    g_searchResultsTimer = 0;
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

// ============================================================
// Search box text selection
// ============================================================

static bool HasSearchSelection()
{
    return
        g_searchSelAnchor >= 0 &&
        g_searchSelAnchor !=
            g_searchCaretPos;
}

static void GetSearchSelectionRange(
    int& start,
    int& end)
{
    start =
        g_searchSelAnchor <
                g_searchCaretPos
            ? g_searchSelAnchor
            : g_searchCaretPos;

    end =
        g_searchSelAnchor <
                g_searchCaretPos
            ? g_searchCaretPos
            : g_searchSelAnchor;
}

// Removes the current selection (if there is one) from
// g_searchText, moves the caret to where it was, and clears the
// anchor. Callers still need to refresh results / resize / repaint
// themselves, same as any other edit to g_searchText.
static bool DeleteSearchSelection()
{
    if (!HasSearchSelection())
        return false;

    int start, end;

    GetSearchSelectionRange(
        start,
        end);

    g_searchText.erase(
        start,
        end - start);

    g_searchCaretPos = start;
    g_searchSelAnchor = -1;

    return true;
}

// Maps a click's x position (already relative to where the text
// itself starts, i.e. x - S(46)) to the nearest character boundary
// — whichever gap between two characters (or the very start/end)
// the click landed closest to.
static int SearchCharIndexFromX(
    HWND hwnd,
    int relativeX)
{
    int n =
        (int)g_searchText.size();

    if (relativeX <= 0 ||
        n == 0)
    {
        return 0;
    }

    HDC dc =
        GetDC(hwnd);

    if (!dc)
        return n;

    HGDIOBJ oldFont =
        SelectObject(
            dc,
            g_font);

    int prevWidth = 0;
    int result = n;

    for (int i = 1;
         i <= n;
         ++i)
    {
        SIZE sz{};

        GetTextExtentPoint32W(
            dc,
            g_searchText.c_str(),
            i,
            &sz);

        int mid =
            (prevWidth + sz.cx) / 2;

        if (relativeX < mid)
        {
            result = i - 1;
            break;
        }

        prevWidth = sz.cx;
    }

    SelectObject(
        dc,
        oldFont);

    ReleaseDC(
        hwnd,
        dc);

    return result;
}

static void CopyTextToClipboard(
    HWND hwnd,
    const std::wstring& text)
{
    if (!OpenClipboard(hwnd))
        return;

    EmptyClipboard();

    size_t bytes =
        (text.size() + 1) *
        sizeof(wchar_t);

    HGLOBAL mem =
        GlobalAlloc(
            GMEM_MOVEABLE,
            bytes);

    if (mem)
    {
        void* dest =
            GlobalLock(mem);

        if (dest)
        {
            memcpy(
                dest,
                text.c_str(),
                bytes);

            GlobalUnlock(mem);

            SetClipboardData(
                CF_UNICODETEXT,
                mem);
        }
        else
        {
            GlobalFree(mem);
        }
    }

    CloseClipboard();
}

// ============================================================
// Hover preview — file classification and loading
// ============================================================

static const wchar_t* const PREVIEW_TEXT_EXTENSIONS[] =
{
    L"txt", L"log", L"ini", L"cfg", L"conf", L"json", L"xml",
    L"csv", L"tsv", L"md", L"markdown", L"yml", L"yaml",
    L"bat", L"cmd", L"ps1", L"py", L"js", L"ts", L"c", L"h",
    L"cpp", L"hpp", L"cs", L"java", L"css", L"html", L"htm",
    L"sql", L"rs", L"go", L"sh", L"toml", L"nfo", L"diz",
    L"reg", L"properties", L"gitignore"
};

static const wchar_t* const PREVIEW_IMAGE_EXTENSIONS[] =
{
    L"png", L"jpg", L"jpeg", L"bmp", L"gif", L"ico",
    L"tif", L"tiff", L"svg"
};

static std::wstring FileExtensionLower(
    const std::wstring& path)
{
    const wchar_t* dot =
        PathFindExtensionW(
            path.c_str());

    if (!dot || *dot != L'.')
        return L"";

    return Lower(dot + 1);
}

// True for a dotfile like .gitignore, .env, or .eslintrc — a name
// whose only '.' is the leading one. PathFindExtensionW treats that
// leading dot as "the extension," so FileExtensionLower() returns
// the rest of the filename (e.g. "gitignore") rather than a real
// format tag — these need the same size-gated treatment as a
// truly extensionless file, not a lookup in the extension tables.
static bool IsDotfileName(
    const std::wstring& path)
{
    const wchar_t* name =
        PathFindFileNameW(
            path.c_str());

    return name && name[0] == L'.';
}

// A cheap stat — no data read — so checking it on every hover is
// effectively free.
static bool GetFileSizeBytes(
    const std::wstring& path,
    ULONGLONG& outSize)
{
    WIN32_FILE_ATTRIBUTE_DATA data{};

    if (!GetFileAttributesExW(
            path.c_str(),
            GetFileExInfoStandard,
            &data))
    {
        return false;
    }

    outSize =
        ((ULONGLONG)data.nFileSizeHigh
             << 32) |
        data.nFileSizeLow;

    return true;
}

// Extensionless files (README, LICENSE, and the like) are common
// enough to be worth a preview, but with nothing to classify them
// by, "presume text" only gets bet on below this size. It's a
// generous ceiling rather than a tight one — the actual read below
// is still capped at PREVIEW_TEXT_MAX_BYTES regardless of how big
// the file is, so this check is purely "is it worth trying at all,"
// not a performance knob; a wrong guess on a binary just costs a
// quick, harmless glance at some garbled bytes.
static const ULONGLONG
    PREVIEW_NO_EXT_MAX_BYTES =
        100ULL * 1024 * 1024;

static PreviewKind ClassifyPreview(
    const std::wstring& path)
{
    std::wstring ext =
        FileExtensionLower(path);

    if (!ext.empty())
    {
        for (auto* e :
             PREVIEW_TEXT_EXTENSIONS)
        {
            if (ext == e)
                return PreviewKind::Text;
        }

        for (auto* e :
             PREVIEW_IMAGE_EXTENSIONS)
        {
            if (ext == e)
                return PreviewKind::Image;
        }
    }

    // Nothing usable to classify by — either no extension at all,
    // or a dotfile whose "extension" is really just the rest of its
    // name. Both get the same size-gated benefit of the doubt.
    if (ext.empty() ||
        IsDotfileName(path))
    {
        ULONGLONG size = 0;

        if (GetFileSizeBytes(
                path,
                size) &&
            size <=
                PREVIEW_NO_EXT_MAX_BYTES)
        {
            return PreviewKind::Text;
        }
    }

    return PreviewKind::None;
}

// Capped well below "the whole file" so a huge log still loads
// instantly — this is a quick glance, not a text editor.
static const DWORD PREVIEW_TEXT_MAX_BYTES = 65536;

static bool LoadPreviewTextFile(
    const std::wstring& path,
    std::wstring& outText,
    bool& outTruncated)
{
    outText.clear();
    outTruncated = false;

    HANDLE file =
        CreateFileW(
            path.c_str(),
            GENERIC_READ,
            FILE_SHARE_READ |
                FILE_SHARE_WRITE,
            nullptr,
            OPEN_EXISTING,
            FILE_ATTRIBUTE_NORMAL,
            nullptr);

    if (file == INVALID_HANDLE_VALUE)
        return false;

    std::vector<unsigned char> raw(
        PREVIEW_TEXT_MAX_BYTES);

    DWORD read = 0;

    BOOL ok =
        ReadFile(
            file,
            raw.data(),
            (DWORD)raw.size(),
            &read,
            nullptr);

    LARGE_INTEGER size{};

    GetFileSizeEx(
        file,
        &size);

    CloseHandle(file);

    if (!ok)
        return false;

    outTruncated =
        size.QuadPart >
        (LONGLONG)read;

    if (read >= 2 &&
        raw[0] == 0xFF &&
        raw[1] == 0xFE)
    {
        // UTF-16 LE, as Notepad writes by default.
        size_t chars =
            (read - 2) / 2;

        outText.assign(
            reinterpret_cast<
                wchar_t*>(
                raw.data() + 2),
            chars);
    }
    else
    {
        size_t offset =
            (read >= 3 &&
             raw[0] == 0xEF &&
             raw[1] == 0xBB &&
             raw[2] == 0xBF)
                ? 3
                : 0;

        int needed =
            MultiByteToWideChar(
                CP_UTF8,
                MB_ERR_INVALID_CHARS,
                (char*)raw.data() +
                    offset,
                (int)(read - offset),
                nullptr,
                0);

        if (needed > 0)
        {
            outText.resize(needed);

            MultiByteToWideChar(
                CP_UTF8,
                MB_ERR_INVALID_CHARS,
                (char*)raw.data() +
                    offset,
                (int)(read - offset),
                &outText[0],
                needed);
        }
        else if (read > offset)
        {
            // Not valid UTF-8 — most likely a legacy ANSI file.
            int needed2 =
                MultiByteToWideChar(
                    CP_ACP,
                    0,
                    (char*)raw.data() +
                        offset,
                    (int)(read - offset),
                    nullptr,
                    0);

            if (needed2 > 0)
            {
                outText.resize(needed2);

                MultiByteToWideChar(
                    CP_ACP,
                    0,
                    (char*)raw.data() +
                        offset,
                    (int)(read - offset),
                    &outText[0],
                    needed2);
            }
        }
    }

    // The byte cap can land mid-line — trim that trailing partial
    // line rather than show it cut off.
    if (outTruncated)
    {
        size_t lastBreak =
            outText.find_last_of(
                L"\r\n");

        if (lastBreak !=
            std::wstring::npos)
        {
            outText.resize(
                lastBreak);
        }
    }

    return true;
}

// Minimal recursive-descent JSON parser whose only job is to
// re-emit consistently indented output — it doesn't build any kind
// of value tree. Deliberately fails safe: anything that doesn't
// parse as strict JSON (truncated by the preview's byte cap,
// actually malformed, or just not JSON despite the .json
// extension) returns false and the caller falls back to showing
// the raw text untouched, rather than risk mangling it.
struct JsonPrettyPrinter
{
    const std::wstring& s;
    size_t i = 0;
    std::wstring out;
    int indent = 0;
    int depth = 0;
    bool ok = true;

    explicit JsonPrettyPrinter(
        const std::wstring& src)
        : s(src)
    {
    }

    void SkipWs()
    {
        while (i < s.size() &&
               (s[i] == L' ' ||
                s[i] == L'\t' ||
                s[i] == L'\r' ||
                s[i] == L'\n'))
        {
            i++;
        }
    }

    void WriteIndent()
    {
        out.append(
            (size_t)indent * 2,
            L' ');
    }

    bool ParseString()
    {
        if (i >= s.size() ||
            s[i] != L'"')
        {
            ok = false;
            return false;
        }

        size_t start = i;
        i++;

        while (i < s.size())
        {
            wchar_t c = s[i];

            if (c == L'\\')
            {
                i += 2;
                continue;
            }

            if (c == L'"')
            {
                i++;

                out.append(
                    s,
                    start,
                    i - start);

                return true;
            }

            i++;
        }

        ok = false;
        return false;
    }

    bool ParseLiteral()
    {
        static const wchar_t* const
            LITERALS[] =
            {
                L"true",
                L"false",
                L"null"
            };

        for (auto* lit : LITERALS)
        {
            size_t len = wcslen(lit);

            if (s.compare(i, len, lit) == 0)
            {
                out.append(lit);
                i += len;
                return true;
            }
        }

        ok = false;
        return false;
    }

    bool ParseNumber()
    {
        size_t start = i;

        if (i < s.size() &&
            s[i] == L'-')
        {
            i++;
        }

        while (i < s.size() &&
               iswdigit(s[i]))
        {
            i++;
        }

        if (i < s.size() &&
            s[i] == L'.')
        {
            i++;

            while (i < s.size() &&
                   iswdigit(s[i]))
            {
                i++;
            }
        }

        if (i < s.size() &&
            (s[i] == L'e' ||
             s[i] == L'E'))
        {
            i++;

            if (i < s.size() &&
                (s[i] == L'+' ||
                 s[i] == L'-'))
            {
                i++;
            }

            while (i < s.size() &&
                   iswdigit(s[i]))
            {
                i++;
            }
        }

        if (i == start)
        {
            ok = false;
            return false;
        }

        out.append(
            s,
            start,
            i - start);

        return true;
    }

    bool ParseObject()
    {
        i++;
        SkipWs();

        if (i < s.size() &&
            s[i] == L'}')
        {
            i++;
            out += L"{}";
            return true;
        }

        out += L"{\n";
        indent++;

        while (true)
        {
            if (!ok || i >= s.size())
            {
                ok = false;
                return false;
            }

            SkipWs();
            WriteIndent();

            if (!ParseString())
                return false;

            SkipWs();

            if (i >= s.size() ||
                s[i] != L':')
            {
                ok = false;
                return false;
            }

            i++;
            out += L": ";
            SkipWs();

            if (!ParseValue())
                return false;

            SkipWs();

            if (i >= s.size())
            {
                ok = false;
                return false;
            }

            if (s[i] == L',')
            {
                i++;
                out += L",\n";
                continue;
            }

            if (s[i] == L'}')
            {
                i++;
                out += L"\n";
                indent--;
                WriteIndent();
                out += L"}";
                return true;
            }

            ok = false;
            return false;
        }
    }

    bool ParseArray()
    {
        i++;
        SkipWs();

        if (i < s.size() &&
            s[i] == L']')
        {
            i++;
            out += L"[]";
            return true;
        }

        out += L"[\n";
        indent++;

        while (true)
        {
            if (!ok || i >= s.size())
            {
                ok = false;
                return false;
            }

            SkipWs();
            WriteIndent();

            if (!ParseValue())
                return false;

            SkipWs();

            if (i >= s.size())
            {
                ok = false;
                return false;
            }

            if (s[i] == L',')
            {
                i++;
                out += L",\n";
                continue;
            }

            if (s[i] == L']')
            {
                i++;
                out += L"\n";
                indent--;
                WriteIndent();
                out += L"]";
                return true;
            }

            ok = false;
            return false;
        }
    }

    bool ParseValue()
    {
        if (!ok)
            return false;

        SkipWs();

        if (i >= s.size())
        {
            ok = false;
            return false;
        }

        wchar_t c = s[i];

        if (c == L'{' ||
            c == L'[')
        {
            // Bounded so a maliciously (or just very deeply)
            // nested document can't blow the call stack.
            if (++depth > 200)
            {
                ok = false;
                return false;
            }

            bool result =
                c == L'{'
                    ? ParseObject()
                    : ParseArray();

            depth--;
            return result;
        }

        if (c == L'"')
            return ParseString();

        if (c == L't' ||
            c == L'f' ||
            c == L'n')
        {
            return ParseLiteral();
        }

        if (c == L'-' ||
            (c >= L'0' &&
             c <= L'9'))
        {
            return ParseNumber();
        }

        ok = false;
        return false;
    }
};

static bool PrettyPrintJson(
    const std::wstring& raw,
    std::wstring& out)
{
    JsonPrettyPrinter printer(raw);

    printer.SkipWs();

    if (!printer.ParseValue())
        return false;

    printer.SkipWs();

    // Anything left over means it wasn't a single well-formed JSON
    // document (or the byte cap cut it off) — fail safe rather than
    // show a half-reformatted mess.
    if (printer.i != raw.size())
        return false;

    out = std::move(printer.out);
    return true;
}

// Non-validating XML reformatter: tokenizes tags/text/comments/
// CDATA/declarations, then re-emits them with consistent
// indentation, collapsing a "<tag>plain text</tag>" leaf onto one
// line the way most XML pretty-printers do. Fails safe the same
// way PrettyPrintJson does — any unrecognized or unterminated
// construct (including a truncated file) returns false and the
// raw text is shown as-is instead.
static bool PrettyPrintXml(
    const std::wstring& raw,
    std::wstring& out)
{
    struct Token
    {
        enum Type
        {
            Open,
            Close,
            SelfClose,
            Text,
            Verbatim
        } type;

        std::wstring content;
    };

    std::vector<Token> tokens;

    size_t i = 0;
    size_t n = raw.size();

    while (i < n)
    {
        if (raw[i] != L'<')
        {
            size_t end =
                raw.find(L'<', i);

            if (end == std::wstring::npos)
                end = n;

            std::wstring text =
                raw.substr(
                    i,
                    end - i);

            size_t a =
                text.find_first_not_of(
                    L" \t\r\n");

            size_t b =
                text.find_last_not_of(
                    L" \t\r\n");

            if (a != std::wstring::npos)
            {
                tokens.push_back(
                    {
                        Token::Text,
                        text.substr(
                            a,
                            b - a + 1)
                    });
            }

            i = end;
            continue;
        }

        if (raw.compare(i, 4, L"<!--") == 0)
        {
            size_t end =
                raw.find(L"-->", i + 4);

            if (end == std::wstring::npos)
                return false;

            end += 3;

            tokens.push_back(
                {
                    Token::Verbatim,
                    raw.substr(
                        i,
                        end - i)
                });

            i = end;
        }
        else if (raw.compare(
                     i, 9, L"<![CDATA[") ==
                 0)
        {
            size_t end =
                raw.find(L"]]>", i + 9);

            if (end == std::wstring::npos)
                return false;

            end += 3;

            tokens.push_back(
                {
                    Token::Verbatim,
                    raw.substr(
                        i,
                        end - i)
                });

            i = end;
        }
        else if (raw.compare(i, 2, L"<?") ==
                 0)
        {
            size_t end =
                raw.find(L"?>", i + 2);

            if (end == std::wstring::npos)
                return false;

            end += 2;

            tokens.push_back(
                {
                    Token::Verbatim,
                    raw.substr(
                        i,
                        end - i)
                });

            i = end;
        }
        else if (raw.compare(i, 2, L"<!") ==
                 0)
        {
            // DOCTYPE or similar — may contain an internal [ ... ]
            // subset with its own '>' characters, so track bracket
            // depth rather than stopping at the first '>'.
            size_t j = i + 2;
            int bracketDepth = 0;

            while (j < n)
            {
                if (raw[j] == L'[')
                    bracketDepth++;
                else if (raw[j] == L']')
                    bracketDepth--;
                else if (raw[j] == L'>' &&
                         bracketDepth <= 0)
                {
                    break;
                }

                j++;
            }

            if (j >= n)
                return false;

            j++;

            tokens.push_back(
                {
                    Token::Verbatim,
                    raw.substr(
                        i,
                        j - i)
                });

            i = j;
        }
        else if (i + 1 < n &&
                 raw[i + 1] == L'/')
        {
            size_t end =
                raw.find(L'>', i + 2);

            if (end == std::wstring::npos)
                return false;

            std::wstring name =
                raw.substr(
                    i + 2,
                    end - (i + 2));

            while (!name.empty() &&
                   iswspace(name.back()))
            {
                name.pop_back();
            }

            tokens.push_back(
                {
                    Token::Close,
                    name
                });

            i = end + 1;
        }
        else
        {
            size_t end = i + 1;
            bool inQuote = false;
            wchar_t quoteChar = 0;

            while (end < n)
            {
                wchar_t c = raw[end];

                if (inQuote)
                {
                    if (c == quoteChar)
                        inQuote = false;
                }
                else if (c == L'"' ||
                         c == L'\'')
                {
                    inQuote = true;
                    quoteChar = c;
                }
                else if (c == L'>')
                {
                    break;
                }

                end++;
            }

            if (end >= n)
                return false;

            bool selfClose =
                end > i &&
                raw[end - 1] == L'/';

            size_t contentEnd =
                selfClose
                    ? end - 1
                    : end;

            std::wstring content =
                raw.substr(
                    i + 1,
                    contentEnd -
                        (i + 1));

            while (!content.empty() &&
                   iswspace(
                       content.back()))
            {
                content.pop_back();
            }

            tokens.push_back(
                {
                    selfClose
                        ? Token::SelfClose
                        : Token::Open,
                    content
                });

            i = end + 1;
        }
    }

    auto TagName =
        [](const std::wstring& openContent)
        -> std::wstring
    {
        size_t end = 0;

        while (end < openContent.size() &&
               !iswspace(openContent[end]))
        {
            end++;
        }

        return openContent.substr(0, end);
    };

    out.clear();

    int indent = 0;

    for (size_t t = 0;
         t < tokens.size();
         ++t)
    {
        const Token& tok = tokens[t];

        if (tok.type == Token::Open)
        {
            // A leaf like <name>John</name> stays on one line
            // rather than being split across three.
            if (t + 2 < tokens.size() &&
                tokens[t + 1].type ==
                    Token::Text &&
                tokens[t + 2].type ==
                    Token::Close &&
                tokens[t + 2].content ==
                    TagName(tok.content))
            {
                out.append(
                    (size_t)indent * 2,
                    L' ');

                out += L"<" +
                       tok.content +
                       L">" +
                       tokens[t + 1]
                           .content +
                       L"</" +
                       tokens[t + 2]
                           .content +
                       L">\n";

                t += 2;
            }
            else
            {
                out.append(
                    (size_t)indent * 2,
                    L' ');

                out += L"<" +
                       tok.content +
                       L">\n";

                indent++;
            }
        }
        else if (tok.type == Token::Close)
        {
            if (indent > 0)
                indent--;

            out.append(
                (size_t)indent * 2,
                L' ');

            out += L"</" +
                   tok.content +
                   L">\n";
        }
        else if (tok.type ==
                 Token::SelfClose)
        {
            out.append(
                (size_t)indent * 2,
                L' ');

            out += L"<" +
                   tok.content +
                   L"/>\n";
        }
        else
        {
            out.append(
                (size_t)indent * 2,
                L' ');

            out += tok.content;
            out += L"\n";
        }
    }

    if (!out.empty() &&
        out.back() == L'\n')
    {
        out.pop_back();
    }

    return true;
}

// ============================================================
// JSON/XML syntax highlighting
// ============================================================
//
// Colors the panel's own pretty-printed output — never the raw
// plain-text case — with the same kind of token classes an IDE's
// syntax highlighter would use. Rather than have PrettyPrintJson/
// PrettyPrintXml above track token identity while they rebuild the
// document (which would tangle formatting logic with coloring
// logic), these re-scan the already-reformatted text in one forward
// pass and emit a flat, offset-sorted list of colored spans; any
// character not covered by a span just draws in the panel's normal
// foreground color. Since the input here is always this app's own
// re-emitted, well-formed output rather than arbitrary user text,
// the scan can stay simple — it never has to recover from malformed
// input the way a real editor's highlighter would.

// A VS Code "Dark+"-inspired palette, picked to sit comfortably
// against the panel's own near-black background and off-white body
// text (see RefreshSystemColors) — the app has no light theme, so
// there's no second palette to keep in sync with this one.
static COLORREF PreviewTokenRGB(
    PreviewTokenColor color)
{
    switch (color)
    {
        case PreviewTokenColor::Key:
            return RGB(156, 220, 254);

        case PreviewTokenColor::String:
            return RGB(206, 145, 120);

        case PreviewTokenColor::Number:
            return RGB(181, 206, 168);

        case PreviewTokenColor::Keyword:
            return RGB(86, 156, 214);

        case PreviewTokenColor::Comment:
            return RGB(106, 153, 85);

        case PreviewTokenColor::TagName:
            return RGB(86, 156, 214);

        case PreviewTokenColor::AttrName:
            return RGB(156, 220, 254);

        case PreviewTokenColor::AttrValue:
            return RGB(206, 145, 120);

        case PreviewTokenColor::Punctuation:
            return g_muted;

        default:
            return g_text;
    }
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

        if (c == L'"')
        {
            size_t start = i;
            i++;

            while (i < n)
            {
                if (text[i] == L'\\' &&
                    i + 1 < n)
                {
                    i += 2;
                    continue;
                }

                if (text[i] == L'"')
                {
                    i++;
                    break;
                }

                i++;
            }

            // A string followed (past any whitespace) by ':' is a
            // key; anything else is a value — pretty-printed JSON
            // never puts a raw newline inside a string, so this
            // lookahead can never cross a line boundary.
            size_t j = i;

            while (j < n &&
                   (text[j] == L' ' ||
                    text[j] == L'\t'))
            {
                j++;
            }

            bool isKey =
                j < n && text[j] == L':';

            spans.push_back(
                {
                    start,
                    i,
                    isKey
                        ? PreviewTokenColor::Key
                        : PreviewTokenColor::String
                });

            continue;
        }

        if (c == L'-' || iswdigit(c))
        {
            size_t start = i;
            i++;

            while (i < n &&
                   (iswdigit(text[i]) ||
                    text[i] == L'.' ||
                    text[i] == L'e' ||
                    text[i] == L'E' ||
                    text[i] == L'+' ||
                    text[i] == L'-'))
            {
                i++;
            }

            spans.push_back(
                {
                    start,
                    i,
                    PreviewTokenColor::Number
                });

            continue;
        }

        if (iswalpha(c))
        {
            size_t start = i;
            i++;

            while (i < n && iswalpha(text[i]))
                i++;

            std::wstring word =
                text.substr(
                    start,
                    i - start);

            if (word == L"true" ||
                word == L"false" ||
                word == L"null")
            {
                spans.push_back(
                    {
                        start,
                        i,
                        PreviewTokenColor::Keyword
                    });
            }

            continue;
        }

        if (c == L'{' || c == L'}' ||
            c == L'[' || c == L']' ||
            c == L':' || c == L',')
        {
            spans.push_back(
                {
                    i,
                    i + 1,
                    PreviewTokenColor::Punctuation
                });

            i++;
            continue;
        }

        i++;
    }
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

        if (text.compare(i, 4, L"<!--") == 0)
        {
            size_t end =
                text.find(L"-->", i + 4);

            end =
                end == std::wstring::npos
                    ? n
                    : end + 3;

            spans.push_back(
                { i, end, PreviewTokenColor::Comment });

            i = end;
            continue;
        }

        if (text.compare(i, 9, L"<![CDATA[") == 0)
        {
            size_t end =
                text.find(L"]]>", i + 9);

            end =
                end == std::wstring::npos
                    ? n
                    : end + 3;

            spans.push_back(
                { i, end, PreviewTokenColor::Comment });

            i = end;
            continue;
        }

        if (text.compare(i, 2, L"<?") == 0)
        {
            size_t end =
                text.find(L"?>", i + 2);

            end =
                end == std::wstring::npos
                    ? n
                    : end + 2;

            spans.push_back(
                { i, end, PreviewTokenColor::Comment });

            i = end;
            continue;
        }

        if (text.compare(i, 2, L"<!") == 0)
        {
            // DOCTYPE or similar — may have an internal [ ... ]
            // subset with its own '>' characters, so track bracket
            // depth rather than stopping at the first one.
            size_t j = i + 2;
            int bracketDepth = 0;

            while (j < n)
            {
                if (text[j] == L'[')
                    bracketDepth++;
                else if (text[j] == L']')
                    bracketDepth--;
                else if (text[j] == L'>' &&
                         bracketDepth <= 0)
                {
                    j++;
                    break;
                }

                j++;
            }

            spans.push_back(
                { i, j, PreviewTokenColor::Comment });

            i = j;
            continue;
        }

        // A regular open/close/self-close tag: '<' (or "</"), the
        // element name, any attributes, then '>' (or "/>").
        size_t tagStart = i;
        size_t p = i + 1;

        bool closing =
            p < n && text[p] == L'/';

        if (closing)
            p++;

        size_t nameStart = p;

        while (p < n &&
               !iswspace(text[p]) &&
               text[p] != L'>' &&
               text[p] != L'/')
        {
            p++;
        }

        size_t nameEnd = p;

        spans.push_back(
            {
                tagStart,
                closing
                    ? tagStart + 2
                    : tagStart + 1,
                PreviewTokenColor::Punctuation
            });

        spans.push_back(
            { nameStart, nameEnd, PreviewTokenColor::TagName });

        while (p < n)
        {
            while (p < n && iswspace(text[p]))
                p++;

            if (p >= n ||
                text[p] == L'>' ||
                (text[p] == L'/' &&
                 p + 1 < n &&
                 text[p + 1] == L'>'))
            {
                break;
            }

            size_t attrStart = p;

            while (p < n &&
                   text[p] != L'=' &&
                   !iswspace(text[p]) &&
                   text[p] != L'>' &&
                   text[p] != L'/')
            {
                p++;
            }

            size_t attrEnd = p;

            if (attrEnd > attrStart)
            {
                spans.push_back(
                    {
                        attrStart,
                        attrEnd,
                        PreviewTokenColor::AttrName
                    });
            }
            else
            {
                // Stray character (shouldn't happen in our own
                // pretty-printed output) — advance so this can't
                // spin forever.
                p++;
                continue;
            }

            while (p < n && iswspace(text[p]))
                p++;

            if (p < n && text[p] == L'=')
            {
                p++;

                while (p < n && iswspace(text[p]))
                    p++;

                if (p < n &&
                    (text[p] == L'"' ||
                     text[p] == L'\''))
                {
                    wchar_t q = text[p];
                    size_t valStart = p;
                    p++;

                    while (p < n && text[p] != q)
                        p++;

                    if (p < n)
                        p++;

                    spans.push_back(
                        {
                            valStart,
                            p,
                            PreviewTokenColor::AttrValue
                        });
                }
            }
        }

        size_t tagEnd =
            text.find(L'>', p);

        tagEnd =
            tagEnd == std::wstring::npos
                ? n
                : tagEnd + 1;

        bool selfClose =
            tagEnd >= 2 &&
            text[tagEnd - 2] == L'/';

        size_t punctStart =
            selfClose
                ? tagEnd - 2
                : tagEnd - 1;

        if (punctStart < tagEnd)
        {
            spans.push_back(
                {
                    punctStart,
                    tagEnd,
                    PreviewTokenColor::Punctuation
                });
        }

        i = tagEnd;
    }
}

static bool LoadPreviewImageFile(
    const std::wstring& path,
    Gdiplus::Bitmap*& outBitmap)
{
    outBitmap =
        new Gdiplus::Bitmap(
            path.c_str());

    if (!outBitmap ||
        outBitmap->GetLastStatus() !=
            Gdiplus::Ok ||
        outBitmap->GetWidth() == 0)
    {
        delete outBitmap;
        outBitmap = nullptr;

        return false;
    }

    return true;
}

// SVG has no fixed pixel size the way a raster image does — D2D
// renders it at whatever resolution we ask for — so this just reads
// the declared width/height (or viewBox) off the root <svg> tag to
// get an aspect ratio for GetPreviewImageRect's area math, via a
// plain text scan rather than spinning up Direct2D a second time
// just to ask. Reuses LoadPreviewTextFile, so it's the same capped,
// BOM-aware read the plaintext preview already relies on.
static bool ParseSvgSize(
    const std::wstring& path,
    int& outW,
    int& outH)
{
    std::wstring text;
    bool truncated = false;

    if (!LoadPreviewTextFile(
            path,
            text,
            truncated))
    {
        return false;
    }

    size_t tagStart =
        text.find(L"<svg");

    if (tagStart == std::wstring::npos)
        return false;

    size_t tagEnd =
        text.find(L'>', tagStart);

    if (tagEnd == std::wstring::npos)
        return false;

    std::wstring tag =
        text.substr(
            tagStart,
            tagEnd - tagStart);

    auto extractAttr =
        [&](const wchar_t* name)
        -> std::wstring
    {
        std::wstring key =
            std::wstring(name) +
            L"=\"";

        size_t p = tag.find(key);

        if (p == std::wstring::npos)
            return L"";

        p += key.size();

        size_t q =
            tag.find(L'"', p);

        if (q == std::wstring::npos)
            return L"";

        return tag.substr(
            p,
            q - p);
    };

    auto toNumber =
        [](const std::wstring& s)
        -> double
    {
        size_t end = 0;

        while (end < s.size() &&
               (iswdigit(s[end]) ||
                s[end] == L'.' ||
                s[end] == L'-'))
        {
            end++;
        }

        return end > 0
            ? _wtof(
                  s.substr(0, end)
                      .c_str())
            : 0.0;
    };

    std::wstring wStr =
        extractAttr(L"width");

    std::wstring hStr =
        extractAttr(L"height");

    double w = toNumber(wStr);
    double h = toNumber(hStr);

    if (w > 0 && h > 0 &&
        wStr.find(L'%') ==
            std::wstring::npos &&
        hStr.find(L'%') ==
            std::wstring::npos)
    {
        outW = (int)w;
        outH = (int)h;

        return true;
    }

    std::wstring viewBox =
        extractAttr(L"viewBox");

    if (!viewBox.empty())
    {
        std::wistringstream iss(
            viewBox);

        double minX, minY, vbW, vbH;

        if (iss >> minX >> minY >>
                vbW >> vbH &&
            vbW > 0 &&
            vbH > 0)
        {
            outW = (int)vbW;
            outH = (int)vbH;

            return true;
        }
    }

    return false;
}

// Rasterizes an SVG at an exact target resolution via Direct2D's
// native SVG support — real vector re-rendering rather than a
// scaled-up raster, so it's crisp at whatever size the preview asks
// for, and true alpha throughout: the target/staging bitmaps are
// premultiplied BGRA the whole way through, matching what
// Gdiplus::Bitmap's PixelFormat32bppPARGB expects byte-for-byte, so
// transparency composites correctly with no fringing. A fresh D3D/
// D2D device is created and torn down per call rather than kept
// around — simpler lifetime, and a hover preview only pays this
// cost once per file, debounced behind the same hover timer as
// every other preview kind.
static bool RenderSvgToPixels(
    const std::wstring& path,
    int nativeW,
    int nativeH,
    int targetW,
    int targetH,
    std::vector<BYTE>& outPixels)
{
    if (targetW <= 0 || targetH <= 0)
        return false;

    outPixels.clear();

    ID3D11Device* d3dDevice = nullptr;
    ID3D11DeviceContext* d3dContext = nullptr;
    IDXGIDevice* dxgiDevice = nullptr;
    ID2D1Factory1* d2dFactory = nullptr;
    ID2D1Device* d2dDevice = nullptr;
    ID2D1DeviceContext* baseContext = nullptr;
    ID2D1DeviceContext5* d2dContext = nullptr;
    IWICImagingFactory* wicFactory = nullptr;
    IWICStream* wicStream = nullptr;
    ID2D1SvgDocument* svgDoc = nullptr;
    ID2D1Bitmap1* targetBitmap = nullptr;
    ID2D1Bitmap1* stagingBitmap = nullptr;

    bool ok = false;

    UINT deviceFlags =
        D3D11_CREATE_DEVICE_BGRA_SUPPORT;

    HRESULT hr =
        D3D11CreateDevice(
            nullptr,
            D3D_DRIVER_TYPE_HARDWARE,
            nullptr,
            deviceFlags,
            nullptr,
            0,
            D3D11_SDK_VERSION,
            &d3dDevice,
            nullptr,
            &d3dContext);

    if (FAILED(hr))
    {
        // No hardware driver available (some VMs/remote sessions) —
        // the software rasterizer still gets the job done for a
        // one-off, small offscreen render like this.
        hr = D3D11CreateDevice(
            nullptr,
            D3D_DRIVER_TYPE_WARP,
            nullptr,
            deviceFlags,
            nullptr,
            0,
            D3D11_SDK_VERSION,
            &d3dDevice,
            nullptr,
            &d3dContext);
    }

    if (SUCCEEDED(hr))
    {
        hr = d3dDevice->QueryInterface(
            IID_PPV_ARGS(
                &dxgiDevice));
    }

    if (SUCCEEDED(hr))
    {
        hr = D2D1CreateFactory(
            D2D1_FACTORY_TYPE_SINGLE_THREADED,
            IID_PPV_ARGS(
                &d2dFactory));
    }

    if (SUCCEEDED(hr))
    {
        hr = d2dFactory->CreateDevice(
            dxgiDevice,
            &d2dDevice);
    }

    if (SUCCEEDED(hr))
    {
        hr = d2dDevice->CreateDeviceContext(
            D2D1_DEVICE_CONTEXT_OPTIONS_NONE,
            &baseContext);
    }

    if (SUCCEEDED(hr))
    {
        hr = baseContext->QueryInterface(
            IID_PPV_ARGS(
                &d2dContext));
    }

    if (SUCCEEDED(hr))
    {
        hr = CoCreateInstance(
            CLSID_WICImagingFactory,
            nullptr,
            CLSCTX_INPROC_SERVER,
            IID_PPV_ARGS(
                &wicFactory));
    }

    if (SUCCEEDED(hr))
        hr = wicFactory->CreateStream(&wicStream);

    if (SUCCEEDED(hr))
    {
        hr = wicStream->InitializeFromFilename(
            path.c_str(),
            GENERIC_READ);
    }

    if (SUCCEEDED(hr))
    {
        hr = d2dContext->CreateSvgDocument(
            wicStream,
            D2D1::SizeF(
                (float)targetW,
                (float)targetH),
            &svgDoc);
    }

    // The document's viewport size above only affects percentage-
    // based lengths elsewhere in the file — the ROOT element still
    // renders at whatever width/height IT declares, so without
    // this override a file like width="200" height="200" would
    // just draw at a fixed 200x200 in the corner of a larger
    // target instead of filling it. Forcing width/height to the
    // exact target size is the same "resize an SVG" mechanism a
    // browser uses: as long as a viewBox (existing, or this one
    // freshly added when the file didn't have one — an identity
    // mapping, so harmless either way) defines the internal
    // coordinate space, the content scales to fit rather than
    // getting clipped or left tiny.
    if (SUCCEEDED(hr))
    {
        ID2D1SvgElement* root = nullptr;

        svgDoc->GetRoot(&root);

        if (root)
        {
            if (!root->IsAttributeSpecified(
                    L"viewBox"))
            {
                D2D1_SVG_VIEWBOX viewBox{};

                viewBox.x = 0;
                viewBox.y = 0;

                viewBox.width =
                    (float)(nativeW > 0
                                ? nativeW
                                : targetW);

                viewBox.height =
                    (float)(nativeH > 0
                                ? nativeH
                                : targetH);

                root->SetAttributeValue(
                    L"viewBox",
                    D2D1_SVG_ATTRIBUTE_POD_TYPE_VIEWBOX,
                    &viewBox,
                    sizeof(viewBox));
            }

            D2D1_SVG_LENGTH w{};
            w.value = (float)targetW;
            w.units =
                D2D1_SVG_LENGTH_UNITS_NUMBER;

            D2D1_SVG_LENGTH h{};
            h.value = (float)targetH;
            h.units =
                D2D1_SVG_LENGTH_UNITS_NUMBER;

            root->SetAttributeValue(
                L"width",
                D2D1_SVG_ATTRIBUTE_POD_TYPE_LENGTH,
                &w,
                sizeof(w));

            root->SetAttributeValue(
                L"height",
                D2D1_SVG_ATTRIBUTE_POD_TYPE_LENGTH,
                &h,
                sizeof(h));

            root->Release();
        }
    }

    if (SUCCEEDED(hr))
    {
        D2D1_BITMAP_PROPERTIES1 targetProps =
            D2D1::BitmapProperties1(
                D2D1_BITMAP_OPTIONS_TARGET,
                D2D1::PixelFormat(
                    DXGI_FORMAT_B8G8R8A8_UNORM,
                    D2D1_ALPHA_MODE_PREMULTIPLIED));

        hr = d2dContext->CreateBitmap(
            D2D1::SizeU(
                (UINT32)targetW,
                (UINT32)targetH),
            nullptr,
            0,
            targetProps,
            &targetBitmap);
    }

    if (SUCCEEDED(hr))
    {
        d2dContext->SetTarget(
            targetBitmap);

        d2dContext->BeginDraw();

        d2dContext->Clear(
            D2D1::ColorF(
                0, 0, 0, 0));

        d2dContext->DrawSvgDocument(
            svgDoc);

        hr = d2dContext->EndDraw();
    }

    if (SUCCEEDED(hr))
    {
        D2D1_BITMAP_PROPERTIES1 stagingProps =
            D2D1::BitmapProperties1(
                D2D1_BITMAP_OPTIONS_CPU_READ |
                    D2D1_BITMAP_OPTIONS_CANNOT_DRAW,
                D2D1::PixelFormat(
                    DXGI_FORMAT_B8G8R8A8_UNORM,
                    D2D1_ALPHA_MODE_PREMULTIPLIED));

        hr = d2dContext->CreateBitmap(
            D2D1::SizeU(
                (UINT32)targetW,
                (UINT32)targetH),
            nullptr,
            0,
            stagingProps,
            &stagingBitmap);
    }

    if (SUCCEEDED(hr))
    {
        hr = stagingBitmap->CopyFromBitmap(
            nullptr,
            targetBitmap,
            nullptr);
    }

    if (SUCCEEDED(hr))
    {
        D2D1_MAPPED_RECT mapped{};

        hr = stagingBitmap->Map(
            D2D1_MAP_OPTIONS_READ,
            &mapped);

        if (SUCCEEDED(hr))
        {
            outPixels.resize(
                (size_t)targetW *
                targetH * 4);

            for (int y = 0;
                 y < targetH;
                 ++y)
            {
                memcpy(
                    outPixels.data() +
                        (size_t)y *
                            targetW * 4,
                    mapped.bits +
                        (size_t)y *
                            mapped.pitch,
                    (size_t)targetW * 4);
            }

            stagingBitmap->Unmap();

            ok = true;
        }
    }

    if (stagingBitmap)
        stagingBitmap->Release();

    if (targetBitmap)
        targetBitmap->Release();

    if (svgDoc)
        svgDoc->Release();

    if (wicStream)
        wicStream->Release();

    if (wicFactory)
        wicFactory->Release();

    if (d2dContext)
        d2dContext->Release();

    if (baseContext)
        baseContext->Release();

    if (d2dDevice)
        d2dDevice->Release();

    if (d2dFactory)
        d2dFactory->Release();

    if (dxgiDevice)
        dxgiDevice->Release();

    if (d3dContext)
        d3dContext->Release();

    if (d3dDevice)
        d3dDevice->Release();

    return ok;
}

// ============================================================
// Background file index
// ============================================================
//
// Walks the configured [Search] IndexPath recursively on a
// low-priority background thread, batching results into the
// shared index every couple thousand files so a query typed while
// indexing is still running gets whatever's been found so far
// rather than nothing at all.

static void IndexDirectoryRecursive(
    const std::wstring& dir,
    std::vector<IndexedFile>& pending)
{
    if (g_indexedFileCount +
            pending.size() >=
        MAX_INDEX_FILES)
    {
        return;
    }

    std::wstring pattern =
        dir + L"\\*";

    WIN32_FIND_DATAW fd{};

    HANDLE h =
        FindFirstFileW(
            pattern.c_str(),
            &fd);

    if (h == INVALID_HANDLE_VALUE)
        return;

    do
    {
        if (fd.cFileName[0] == L'.' &&
            (fd.cFileName[1] == L'\0' ||
             (fd.cFileName[1] == L'.' &&
              fd.cFileName[2] == L'\0')))
        {
            continue;
        }

        std::wstring full =
            dir + L'\\' + fd.cFileName;

        if (fd.dwFileAttributes &
            FILE_ATTRIBUTE_DIRECTORY)
        {
            // Reparse points (junctions/symlinks) can point back
            // up the tree and loop forever — skip them.
            if (fd.dwFileAttributes &
                FILE_ATTRIBUTE_REPARSE_POINT)
            {
                continue;
            }

            IndexDirectoryRecursive(
                full,
                pending);
        }
        else
        {
            pending.push_back(
                {
                    full,
                    fd.cFileName,
                    Lower(fd.cFileName)
                });

            if (pending.size() >= 2000)
            {
                std::lock_guard<std::mutex>
                    lock(g_fileIndexMutex);

                for (auto& p : pending)
                {
                    g_fileIndex.push_back(
                        std::move(p));
                }

                g_indexedFileCount =
                    g_fileIndex.size();

                pending.clear();
            }
        }

        if (g_indexedFileCount +
                pending.size() >=
            MAX_INDEX_FILES)
        {
            break;
        }

    } while (FindNextFileW(h, &fd));

    FindClose(h);
}

static DWORD WINAPI IndexThreadProc(
    LPVOID)
{
    SetThreadPriority(
        GetCurrentThread(),
        THREAD_PRIORITY_LOWEST);

    std::vector<IndexedFile> pending;
    pending.reserve(2048);

    if (!g_searchIndexRoot.empty())
    {
        IndexDirectoryRecursive(
            g_searchIndexRoot,
            pending);
    }

    if (!pending.empty())
    {
        std::lock_guard<std::mutex>
            lock(g_fileIndexMutex);

        for (auto& p : pending)
        {
            g_fileIndex.push_back(
                std::move(p));
        }

        g_indexedFileCount =
            g_fileIndex.size();
    }

    return 0;
}

// Fire-and-forget: the thread keeps running (at the lowest
// priority, so it never competes with the UI) for as long as it
// takes to walk the tree, merging into g_fileIndex as it goes.
static void StartBackgroundIndexing()
{
    if (g_searchIndexRoot.empty())
        return;

    HANDLE thread =
        CreateThread(
            nullptr,
            0,
            IndexThreadProc,
            nullptr,
            0,
            nullptr);

    if (thread)
        CloseHandle(thread);
}

// ============================================================
// God Mode catalog (Control Panel "All Tasks")
// ============================================================
//
// Windows exposes a hidden shell folder that flattens every Control
// Panel task across every category into one list — but only when
// addressed through a real folder whose name ends in
// ".{ED7BA470-8E54-465E-825C-99712043E01C}". Unlike a namespace root
// such as My Computer or Recycle Bin, this CLSID isn't independently
// bindable off the desktop: SHParseDisplayName rejects the bare
// "::{GUID}" form outright (confirmed against a real Windows install
// — it fails with E_INVALIDARG). The folder-naming trick everyone
// already knows for exposing this by hand is therefore not a
// workaround for something also reachable another way — it's the
// only way in. This creates that folder itself, tucked into the
// user's own Temp directory rather than somewhere visible, then
// walks it exactly as if the user had made it by hand. Runs on a
// low-priority background thread (mirroring the file indexer above),
// caching each item's display name, launchable PIDL, and icon for
// the rest of the process's life — the catalog never changes at
// runtime, so there's nothing to ever re-scan.

// Left in place permanently once created rather than cleaned up
// afterward — it's how a hand-made God Mode folder is meant to
// persist, it's empty (nothing is ever placed inside it), and the
// shell's own binding may keep relying on it existing for as long
// as this process still holds PIDLs/icons resolved through it.
static std::wstring GetGodModeFolderPath()
{
    wchar_t tempDir[MAX_PATH]{};

    DWORD n =
        GetTempPathW(
            MAX_PATH,
            tempDir);

    if (!n || n >= MAX_PATH)
        return std::wstring();

    std::wstring path = tempDir;

    if (!path.empty() &&
        path.back() != L'\\')
    {
        path += L'\\';
    }

    path +=
        L"ClassicShellGodMode."
        L"{ED7BA470-8E54-465E-825C-99712043E01C}";

    return path;
}

struct GodModeItem
{
    std::wstring name;
    std::wstring nameLower;

    // Fully-qualified (desktop-relative) PIDL — owned; the shell
    // namespace has no real path for these, so this is the only
    // thing that can actually launch one later. Freed at process
    // exit along with everything else in g_godModeItems.
    LPITEMIDLIST pidl = nullptr;

    // A real per-item icon where the shell has one, cached for as
    // long as the catalog lives — never destroyed per-use the way a
    // search result's own icon normally is (see ClearSearchResults'
    // kind check).
    HICON icon = nullptr;
};

static std::vector<GodModeItem> g_godModeItems;
static std::mutex g_godModeMutex;

// Diagnostics only — read by ToggleControlPanelBrowse to explain an
// empty catalog instead of just saying "not ready" with no way to
// tell a startup-timing race from an actual enumeration failure.
// Stage climbs as each COM step succeeds; g_godModeLastHr is
// whichever HRESULT the last-attempted step returned (0 if it
// never got that far). Seen/named track how many children the
// enumerator actually produced versus how many yielded a usable
// display name, since those can fail independently of each other.
static std::atomic<int> g_godModeStage{ 0 };
static std::atomic<long> g_godModeLastHr{ 0 };
static std::atomic<int> g_godModeEnumSeen{ 0 };
static std::atomic<int> g_godModeNamed{ 0 };

static DWORD WINAPI GodModeThreadProc(
    LPVOID)
{
    SetThreadPriority(
        GetCurrentThread(),
        THREAD_PRIORITY_LOWEST);

    HRESULT com =
        CoInitializeEx(
            nullptr,
            COINIT_APARTMENTTHREADED);

    g_godModeStage = 1;

    std::wstring folderPath =
        GetGodModeFolderPath();

    if (folderPath.empty())
    {
        if (SUCCEEDED(com))
            CoUninitialize();

        return 0;
    }

    // Idempotent — CreateDirectoryW failing because the folder
    // already exists (from a previous run) is the expected steady
    // state, not an error; only actually missing afterward matters,
    // and SHParseDisplayName below will fail on its own if so.
    CreateDirectoryW(
        folderPath.c_str(),
        nullptr);

    LPITEMIDLIST rootPidl = nullptr;

    HRESULT hr =
        SHParseDisplayName(
            folderPath.c_str(),
            nullptr,
            &rootPidl,
            0,
            nullptr);

    g_godModeLastHr = hr;

    if (FAILED(hr) || !rootPidl)
    {
        if (SUCCEEDED(com))
            CoUninitialize();

        return 0;
    }

    g_godModeStage = 2;

    IShellFolder* desktop = nullptr;

    hr = SHGetDesktopFolder(&desktop);

    g_godModeLastHr = hr;

    if (FAILED(hr) || !desktop)
    {
        CoTaskMemFree(rootPidl);

        if (SUCCEEDED(com))
            CoUninitialize();

        return 0;
    }

    g_godModeStage = 3;

    IShellFolder* rootFolder = nullptr;

    hr =
        desktop->BindToObject(
            rootPidl,
            nullptr,
            IID_PPV_ARGS(&rootFolder));

    g_godModeLastHr = hr;

    desktop->Release();

    if (FAILED(hr) || !rootFolder)
    {
        CoTaskMemFree(rootPidl);

        if (SUCCEEDED(com))
            CoUninitialize();

        return 0;
    }

    g_godModeStage = 4;

    IEnumIDList* enumIds = nullptr;

    // SHCONTF_INCLUDEHIDDEN matters here specifically — every item
    // in this particular folder is shell-flagged hidden (that's how
    // Windows keeps them out of an ordinary Explorer listing until
    // you view this exact CLSID), so without it EnumObjects silently
    // returns zero children instead of failing outright.
    hr =
        rootFolder->EnumObjects(
            nullptr,
            SHCONTF_FOLDERS |
                SHCONTF_NONFOLDERS |
                SHCONTF_INCLUDEHIDDEN,
            &enumIds);

    g_godModeLastHr = hr;

    std::vector<GodModeItem> found;

    if (SUCCEEDED(hr) && enumIds)
    {
        g_godModeStage = 5;

        LPITEMIDLIST childPidl = nullptr;
        ULONG fetched = 0;

        while (enumIds->Next(
                   1,
                   &childPidl,
                   &fetched) == S_OK)
        {
            g_godModeEnumSeen++;

            STRRET strret{};

            if (SUCCEEDED(
                    rootFolder->GetDisplayNameOf(
                        childPidl,
                        SHGDN_NORMAL,
                        &strret)))
            {
                wchar_t nameBuf[512]{};

                if (SUCCEEDED(
                        StrRetToBufW(
                            &strret,
                            childPidl,
                            nameBuf,
                            512)) &&
                    nameBuf[0])
                {
                    g_godModeNamed++;

                    LPITEMIDLIST fullPidl =
                        ILCombine(
                            rootPidl,
                            childPidl);

                    if (fullPidl)
                    {
                        GodModeItem item;

                        item.name = nameBuf;
                        item.nameLower =
                            Lower(item.name);
                        item.pidl = fullPidl;

                        SHFILEINFOW sfi{};

                        if (SHGetFileInfoW(
                                (LPCWSTR)fullPidl,
                                0,
                                &sfi,
                                sizeof(sfi),
                                SHGFI_PIDL |
                                    SHGFI_ICON |
                                    SHGFI_LARGEICON))
                        {
                            item.icon = sfi.hIcon;
                        }

                        found.push_back(
                            std::move(item));
                    }
                }
            }

            CoTaskMemFree(childPidl);
        }

        enumIds->Release();
    }

    rootFolder->Release();
    CoTaskMemFree(rootPidl);

    g_godModeStage = 6;

    std::sort(
        found.begin(),
        found.end(),
        [](const GodModeItem& a,
           const GodModeItem& b)
        {
            return
                _wcsicmp(
                    a.name.c_str(),
                    b.name.c_str()) < 0;
        });

    {
        std::lock_guard<std::mutex>
            lock(g_godModeMutex);

        g_godModeItems = std::move(found);
    }

    if (SUCCEEDED(com))
        CoUninitialize();

    return 0;
}

// Fire-and-forget, same shape as StartBackgroundIndexing() — the
// catalog is small (a couple hundred items) and read-only once
// built, so unlike the file index there's no incremental merging:
// the background thread just swaps the whole vector in when it's
// done.
static void StartGodModeIndexing()
{
    HANDLE thread =
        CreateThread(
            nullptr,
            0,
            GodModeThreadProc,
            nullptr,
            0,
            nullptr);

    if (thread)
        CloseHandle(thread);
}

// Invokes a God Mode item's default action — exactly what double-
// clicking it in Explorer's own God Mode folder would do — via its
// cached PIDL rather than any path, since these tasks don't live at
// a real filesystem location for ShellExecuteW to open directly.
static bool LaunchGodModeItem(
    const GodModeItem& item)
{
    if (!item.pidl)
        return false;

    SHELLEXECUTEINFOW info{};

    info.cbSize = sizeof(info);
    info.fMask = SEE_MASK_INVOKEIDLIST;
    info.lpIDList = item.pidl;
    info.nShow = SW_SHOWNORMAL;

    return ShellExecuteExW(&info) != FALSE;
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

// FOLDERID_Games is the old Vista/7 Games Explorer's virtual
// folder — still a recognized KNOWNFOLDERID, but on modern Windows
// it either fails to resolve or resolves to a path that isn't a
// real, browsable directory. When that happens, fall back to the
// "Games" group that game installers commonly create inside the
// Start Menu's own Programs folder — checking the shared, all-users
// location first (the more common install target), then the
// current user's own.
static bool ResolveGamesFolder(
    std::wstring& out)
{
    if (KnownFolder(
            FOLDERID_Games,
            out) &&
        DirectoryExists(out))
    {
        return true;
    }

    KNOWNFOLDERID programLocations[] =
    {
        FOLDERID_CommonPrograms,
        FOLDERID_Programs
    };

    for (const KNOWNFOLDERID& id :
         programLocations)
    {
        std::wstring programsPath;

        if (!KnownFolder(
                id,
                programsPath))
        {
            continue;
        }

        std::wstring gamesPath =
            programsPath +
            L"\\Games";

        if (DirectoryExists(gamesPath))
        {
            out = gamesPath;
            return true;
        }
    }

    return false;
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

    // Not a plain single-known-folder lookup like the ones above —
    // needs its own fallback chain, so it isn't in the table.
    if (s == L"games")
        return ResolveGamesFolder(out);

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

// Defined later (needs CloseStart) — launches the file at
// g_searchResults[index] and dismisses the menu.
static void LaunchSearchResult(
    int index);

static void HandleSearchEnter()
{
    // An explicit wildcard query ("*.txt") with matches takes
    // priority: Enter goes to the top hit, same as picking the
    // first row of a combo box. An implicit contains-match (typed
    // plain text that happens to also match some file) does NOT
    // preempt here — it falls through to the normal command
    // resolver below first, so e.g. "calc" still launches Calculator
    // on Enter even if some unrelated file also contains "calc"; it
    // only gets used as a fallback further down if that resolver
    // finds nothing.
    if (!g_searchResults.empty() &&
        g_searchResultsIsWildcard)
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
        g_searchCaretPos = 0;
        g_searchSelAnchor = -1;

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

    if (!g_searchResults.empty())
    {
        LaunchSearchResult(0);
        return;
    }

    g_searchText.clear();
    g_searchCaretPos = 0;
    g_searchSelAnchor = -1;

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

// The search box's clear ("×") button — two crossed lines rather
// than a font glyph, so it's crisp and perfectly centered regardless
// of whatever the active font's "x" happens to look like.
static void DrawClearGlyph(
    HDC dc,
    const RECT& r,
    COLORREF color)
{
    Gdiplus::Graphics graphics(dc);

    graphics.SetSmoothingMode(
        Gdiplus::SmoothingModeAntiAlias);

    Gdiplus::Pen pen(
        Gdiplus::Color(
            255,
            GetRValue(color),
            GetGValue(color),
            GetBValue(color)),
        1.6f);

    pen.SetStartCap(
        Gdiplus::LineCapRound);

    pen.SetEndCap(
        Gdiplus::LineCapRound);

    int inset =
        (r.right - r.left) / 4;

    graphics.DrawLine(
        &pen,
        r.left + inset,
        r.top + inset,
        r.right - inset,
        r.bottom - inset);

    graphics.DrawLine(
        &pen,
        r.right - inset,
        r.top + inset,
        r.left + inset,
        r.bottom - inset);
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

    if (g_mono)
    {
        DeleteObject(g_mono);
        g_mono = nullptr;
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

    const wchar_t* monoFace =
        ResolveFontFace(
            L"Cascadia Mono",
            L"Consolas");

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

    // The preview panel's "old nfo reader" look wants a fixed-pitch
    // face so ASCII layouts and columns of data line up.
    g_mono =
        CreateFontW(
            -S(12),
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
            FIXED_PITCH |
                FF_MODERN,
            monoFace);
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
// Hover preview geometry
// ============================================================
//
// The preview panel always anchors to the work area's top-right
// corner — the opposite corner from GetStartRect()'s bottom-left
// anchor — so it never overlaps the menu it's previewing for.

// Splits raw preview text into display lines, trimming a paired
// '\r' so both Windows and Unix line endings behave identically.
// Shared by the sizing pass below, PaintPreview's render pass, and
// the selection hit-testing further down, so none of them can ever
// disagree about line count or content. outOffsets, if given, gets
// each line's starting character offset into the original text —
// selection and syntax-highlight spans are both anchored to those
// same absolute offsets, so this is what ties a screen line back to
// them.
static std::vector<std::wstring> SplitPreviewLines(
    const std::wstring& text,
    std::vector<size_t>* outOffsets = nullptr)
{
    std::vector<std::wstring> lines;

    size_t lineStart = 0;

    for (size_t i = 0; i <= text.size(); ++i)
    {
        if (i == text.size() ||
            text[i] == L'\n')
        {
            size_t lineEnd = i;

            if (lineEnd > lineStart &&
                text[lineEnd - 1] == L'\r')
            {
                lineEnd--;
            }

            if (outOffsets)
                outOffsets->push_back(lineStart);

            lines.push_back(
                text.substr(
                    lineStart,
                    lineEnd - lineStart));

            lineStart = i + 1;
        }
    }

    return lines;
}

// Sized to the actual content rather than a fixed quarter of the
// screen: measures the widest line and total line count with the
// panel's own mono font — same idea as GetPreviewImageRect deriving
// a box from an image's native size — then clamps to a range that
// stays readable at one end and never swallows the desktop at the
// other. A short README gets a tight box instead of always eating
// half the screen; a long dump still caps out well short of full.
static RECT GetPreviewTextRect()
{
    RECT work{};

    if (!GetWorkArea(work))
        work = { 0, 0, 1920, 1080 };

    int workW = work.right - work.left;
    int workH = work.bottom - work.top;

    const int pad = S(10);

    std::vector<std::wstring> lines =
        SplitPreviewLines(g_previewText);

    int lineH = S(16);
    int maxLineW = 0;

    HDC dc = GetDC(nullptr);

    if (dc)
    {
        HGDIOBJ oldFont =
            SelectObject(dc, g_mono);

        TEXTMETRICW tm{};

        GetTextMetrics(dc, &tm);

        int measuredLineH =
            tm.tmHeight + tm.tmExternalLeading;

        if (measuredLineH > 0)
            lineH = measuredLineH;

        for (const auto& line : lines)
        {
            SIZE sz{};

            GetTextExtentPoint32W(
                dc,
                line.c_str(),
                (int)line.size(),
                &sz);

            if (sz.cx > maxLineW)
                maxLineW = sz.cx;
        }

        SelectObject(dc, oldFont);
        ReleaseDC(nullptr, dc);
    }

    int contentW = maxLineW + pad * 2;
    int contentH = (int)lines.size() * lineH + pad * 2;

    // Bounds chosen so the panel never reads as a tooltip (too
    // small to be worth opening) or a takeover (too large to feel
    // like a quick-look): roughly a fifth to three-fifths of the
    // work area's width, an eighth to two-thirds of its height.
    int minW = (int)(workW * 0.20);
    int maxW = (int)(workW * 0.60);
    int minH = (int)(workH * 0.12);
    int maxH = (int)(workH * 0.65);

    int w = contentW;

    if (w < minW) w = minW;
    if (w > maxW) w = maxW;

    int h = contentH;

    if (h < minH) h = minH;
    if (h > maxH) h = maxH;

    const int margin = S(8);

    return
    {
        work.right - margin - w,
        work.top + margin,
        work.right - margin,
        work.top + margin + h
    };
}

// ============================================================
// Preview text selection
// ============================================================
//
// The panel is a plain custom-painted popup, not a native edit
// control, so there's no built-in click-drag-to-select or Ctrl+C —
// both are implemented by hand here. Selection is tracked as flat
// character offsets into g_previewText rather than (line, column)
// pairs, since that's simultaneously the simplest thing to turn
// into a clipboard string (just a substring) and the same
// coordinate space the syntax-highlight spans already use.

// Line height and per-character advance for the panel's own
// fixed-pitch font. Shared by PaintPreview's layout and the mouse
// hit-testing below so a click always lands on the character it
// visually appears to.
static void GetPreviewMonoMetrics(
    HDC dc,
    int& outLineH,
    int& outCharW)
{
    HGDIOBJ oldFont =
        SelectObject(dc, g_mono);

    TEXTMETRICW tm{};

    GetTextMetrics(dc, &tm);

    SelectObject(dc, oldFont);

    outLineH =
        tm.tmHeight +
        tm.tmExternalLeading;

    if (outLineH < 1)
        outLineH = S(16);

    outCharW = tm.tmAveCharWidth;

    if (outCharW < 1)
        outCharW = S(7);
}

// Everything about how the text preview is currently laid out —
// computed fresh (and cheaply; the file is capped at 64 KB) by both
// PaintPreview and the mouse handlers below, so the two can never
// disagree about where a given character actually sits on screen.
struct PreviewTextLayout
{
    std::vector<std::wstring> lines;
    std::vector<size_t> lineOffsets;
    RECT content{};
    int lineH = 16;
    int charWidth = 7;
    int firstLine = 0;
    int visibleLines = 0;
    int textRight = 0;
    int scrollbarW = 0;
    bool needsScrollbar = false;
};

static PreviewTextLayout ComputePreviewTextLayout(
    HWND hwnd)
{
    PreviewTextLayout layout;

    RECT client{};

    GetClientRect(
        hwnd,
        &client);

    const int pad = S(10);

    layout.content =
    {
        pad,
        pad,
        client.right - pad,
        client.bottom - pad
    };

    HDC dc = GetDC(hwnd);

    if (dc)
    {
        GetPreviewMonoMetrics(
            dc,
            layout.lineH,
            layout.charWidth);

        ReleaseDC(hwnd, dc);
    }

    layout.lines =
        SplitPreviewLines(
            g_previewText,
            &layout.lineOffsets);

    layout.visibleLines =
        (layout.content.bottom -
         layout.content.top) /
        layout.lineH;

    if (layout.visibleLines < 0)
        layout.visibleLines = 0;

    layout.needsScrollbar =
        (int)layout.lines.size() >
        layout.visibleLines;

    int maxScroll =
        layout.needsScrollbar
            ? (int)layout.lines.size() -
                  layout.visibleLines
            : 0;

    if (g_previewTextScroll > maxScroll)
        g_previewTextScroll = maxScroll;

    if (g_previewTextScroll < 0)
        g_previewTextScroll = 0;

    layout.firstLine = g_previewTextScroll;

    // Widened from the original 4px sliver — too thin to reliably
    // grab with a mouse — plus GetPreviewScrollbarHitRect below adds
    // further invisible slop on top of this for the actual click
    // target, the same forgiving hit-testing a modern browser's own
    // scrollbar gives you.
    layout.scrollbarW = S(8);

    layout.textRight =
        layout.content.right -
        (layout.needsScrollbar
             ? layout.scrollbarW + S(6)
             : 0);

    return layout;
}

static int GetPreviewMaxScroll(
    const PreviewTextLayout& layout)
{
    return
        layout.needsScrollbar
            ? (int)layout.lines.size() -
                  layout.visibleLines
            : 0;
}

static RECT GetPreviewScrollbarTrackRect(
    const PreviewTextLayout& layout)
{
    return RECT
    {
        layout.content.right -
            layout.scrollbarW,
        layout.content.top,
        layout.content.right,
        layout.content.bottom
    };
}

// A generous invisible margin around the visible track — clicking a
// few pixels short of the bar itself still grabs it, rather than
// demanding pixel-perfect aim on an already-narrow strip.
static RECT GetPreviewScrollbarHitRect(
    const PreviewTextLayout& layout)
{
    RECT r = GetPreviewScrollbarTrackRect(layout);

    r.left -= S(8);

    return r;
}

static RECT GetPreviewScrollbarThumbRect(
    const PreviewTextLayout& layout)
{
    RECT track =
        GetPreviewScrollbarTrackRect(layout);

    int trackHeight =
        track.bottom - track.top;

    int thumbH =
        layout.lines.empty()
            ? trackHeight
            : trackHeight *
                  layout.visibleLines /
                  (int)layout.lines.size();

    if (thumbH < S(16))
        thumbH = S(16);

    if (thumbH > trackHeight)
        thumbH = trackHeight;

    int maxScroll =
        GetPreviewMaxScroll(layout);

    int thumbTravel =
        trackHeight - thumbH;

    int thumbTop =
        track.top +
        (maxScroll > 0
             ? thumbTravel *
                   g_previewTextScroll /
                   maxScroll
             : 0);

    return RECT
    {
        track.left,
        thumbTop,
        track.right,
        thumbTop + thumbH
    };
}

// Sets g_previewTextScroll from a thumb-top pixel position (already
// clamped to the track), the inverse of GetPreviewScrollbarThumbRect
// — shared by both "jump to where the track was clicked" and
// "follow the mouse while dragging the thumb".
static void SetPreviewScrollFromThumbTop(
    const PreviewTextLayout& layout,
    int thumbTop)
{
    RECT track =
        GetPreviewScrollbarTrackRect(layout);

    RECT thumb =
        GetPreviewScrollbarThumbRect(layout);

    int thumbH = thumb.bottom - thumb.top;
    int thumbTravel = (track.bottom - track.top) - thumbH;

    int maxScroll =
        GetPreviewMaxScroll(layout);

    if (thumbTravel <= 0 || maxScroll <= 0)
    {
        g_previewTextScroll = 0;
        return;
    }

    if (thumbTop < track.top)
        thumbTop = track.top;

    if (thumbTop > track.top + thumbTravel)
        thumbTop = track.top + thumbTravel;

    g_previewTextScroll =
        (thumbTop - track.top) *
        maxScroll /
        thumbTravel;
}

// Maps a client-area point to the nearest character offset in
// g_previewText — rounding to whichever side of a character cell
// the point is closer to, the same feel SearchCharIndexFromX gives
// the search box.
static size_t PreviewOffsetFromPoint(
    const PreviewTextLayout& layout,
    int x,
    int y)
{
    if (layout.lines.empty())
        return 0;

    int row =
        layout.firstLine +
        (y - layout.content.top) /
            layout.lineH;

    if (row < 0)
        row = 0;

    if (row >= (int)layout.lines.size())
        row = (int)layout.lines.size() - 1;

    int col =
        (x - layout.content.left +
         layout.charWidth / 2) /
        layout.charWidth;

    if (col < 0)
        col = 0;

    int lineLen =
        (int)layout.lines[row].size();

    if (col > lineLen)
        col = lineLen;

    return
        layout.lineOffsets[row] +
        (size_t)col;
}

// Copies the current preview selection, if any, to the clipboard —
// the Ctrl+C branch in KeyboardProc calls this before falling back
// to the search box's own copy handling.
static void CopyPreviewSelection()
{
    if (!g_previewHasSelection || !g_preview)
        return;

    size_t start =
        std::min(
            g_previewSelAnchor,
            g_previewSelCaret);

    size_t end =
        std::max(
            g_previewSelAnchor,
            g_previewSelCaret);

    CopyTextToClipboard(
        g_preview,
        g_previewText.substr(
            start,
            end - start));
}

// Scales the image so its rendered size — regardless of its native
// resolution — occupies about a quarter of the screen's area (a
// huge photo shrinks, a tiny one grows), then wraps a small padded
// card tightly around that rendered size. outRenderW/H are the
// image's actual drawn dimensions inside the returned rect.
static RECT GetPreviewImageRect(
    int imageW,
    int imageH,
    int& outRenderW,
    int& outRenderH)
{
    RECT work{};

    if (!GetWorkArea(work))
        work = { 0, 0, 1920, 1080 };

    double screenArea =
        (double)(work.right - work.left) *
        (work.bottom - work.top);

    double targetArea =
        screenArea * 0.25;

    double imageArea =
        (double)imageW * imageH;

    double scale =
        imageArea > 0.0
            ? std::sqrt(targetArea / imageArea)
            : 1.0;

    int renderW =
        (int)(imageW * scale);

    int renderH =
        (int)(imageH * scale);

    // Keep extremes sane — a tiny icon or a giant panorama
    // shouldn't blow the box past most of the screen.
    int maxW =
        (int)((work.right - work.left) * 0.8);

    int maxH =
        (int)((work.bottom - work.top) * 0.8);

    if (renderW > maxW ||
        renderH > maxH)
    {
        double clamp =
            std::min(
                (double)maxW / renderW,
                (double)maxH / renderH);

        renderW = (int)(renderW * clamp);
        renderH = (int)(renderH * clamp);
    }

    if (renderW < S(60))
        renderW = S(60);

    if (renderH < S(60))
        renderH = S(60);

    outRenderW = renderW;
    outRenderH = renderH;

    const int pad = S(10);
    const int margin = S(8);

    int boxW = renderW + pad * 2;
    int boxH = renderH + pad * 2;

    return
    {
        work.right - margin - boxW,
        work.top + margin,
        work.right - margin,
        work.top + margin + boxH
    };
}

// Same corner as the preview panel, but a small fixed size rather
// than one sized to fit content — toast messages are short by
// construction (see ShowToast's callers), so there's no need for
// the dynamic measurement the preview's image/text boxes do.
static RECT GetToastRect()
{
    RECT work{};

    if (!GetWorkArea(work))
        work = { 0, 0, 1920, 1080 };

    int width = S(300);
    int height = S(80);
    int margin = S(8);

    return
    {
        work.right - margin - width,
        work.top + margin,
        work.right - margin,
        work.top + margin + height
    };
}

// Pops (or replaces) the toast with a new message. Safe to call
// repeatedly — each call just restarts the hold-then-fade clock
// with the new text, the same "a fresh trigger always wins"
// override behavior the hover preview uses.
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

    RECT r =
        GetToastRect();

    SetWindowPos(
        g_toast,
        HWND_TOPMOST,
        r.left,
        r.top,
        r.right - r.left,
        r.bottom - r.top,
        SWP_SHOWWINDOW |
            SWP_NOACTIVATE);

    SetLayeredWindowAttributes(
        g_toast,
        0,
        TOAST_BASE_ALPHA,
        LWA_ALPHA);

    InvalidateRect(
        g_toast,
        nullptr,
        FALSE);

    if (g_toastTimer)
    {
        KillTimer(
            g_start,
            TIMER_TOAST);
    }

    g_toastTimer =
        SetTimer(
            g_start,
            TIMER_TOAST,
            16,
            nullptr);
}

// Loads and displays the quick-look preview for one search result,
// replacing whatever the panel was previously showing. Silently
// does nothing for an unrecognized extension (an .exe, say) — the
// panel just stays hidden, which is the right outcome for "there's
// nothing to preview here" rather than an error.
static void ShowPreviewForResult(
    int index)
{
    HidePreview();

    if (index < 0 ||
        index >=
            (int)g_searchResults.size())
    {
        return;
    }

    // Nothing to quick-look at — a God Mode row's "path" is a
    // display name, not a real file.
    if (g_searchResults[index].kind ==
        SearchResultKind::GodMode)
    {
        return;
    }

    if (!g_preview)
        return;

    const std::wstring& path =
        g_searchResults[index].path;

    PreviewKind kind =
        ClassifyPreview(path);

    if (kind == PreviewKind::None)
        return;

    RECT r{};

    if (kind == PreviewKind::Text)
    {
        std::wstring text;
        bool truncated = false;

        if (!LoadPreviewTextFile(
                path,
                text,
                truncated))
        {
            return;
        }

        // Pretty-print JSON/XML rather than showing it as whatever
        // single cramped or inconsistently-indented line it was
        // saved as. Falls back to the raw text untouched if the
        // file doesn't actually parse (including one cut short by
        // the byte cap above) rather than risk mangling it.
        std::wstring ext =
            FileExtensionLower(path);

        std::wstring pretty;

        PreviewHighlightLang highlightLang =
            PreviewHighlightLang::None;

        if (ext == L"json" &&
            PrettyPrintJson(
                text,
                pretty))
        {
            text = std::move(pretty);
            highlightLang = PreviewHighlightLang::Json;
        }
        else if (ext == L"xml" &&
                 PrettyPrintXml(
                     text,
                     pretty))
        {
            text = std::move(pretty);
            highlightLang = PreviewHighlightLang::Xml;
        }
        else if (ext != L"json" &&
                 ext != L"xml" &&
                 (ext.empty() ||
                  IsDotfileName(path)))
        {
            // The name didn't tell us the format — either no
            // extension, or a dotfile like .eslintrc/.babelrc whose
            // "extension" is just the rest of its name — so try
            // sniffing by content instead. Both printers fail safe
            // on anything that isn't actually well-formed JSON/XML,
            // so guessing here costs nothing when it's really just
            // plain text.
            if (PrettyPrintJson(
                    text,
                    pretty))
            {
                text = std::move(pretty);
                highlightLang = PreviewHighlightLang::Json;
            }
            else if (PrettyPrintXml(
                         text,
                         pretty))
            {
                text = std::move(pretty);
                highlightLang = PreviewHighlightLang::Xml;
            }
        }

        g_previewKind = PreviewKind::Text;
        g_previewPath = path;
        g_previewText = std::move(text);
        g_previewTextTruncated = truncated;
        g_previewHighlightLang = highlightLang;

        // Highlighting only ever applies to a pretty-printed JSON/
        // XML result — plain text (including JSON/XML that failed
        // to parse and fell back to its raw form) just keeps
        // g_previewColorSpans empty, so PaintPreview draws every
        // character in the normal foreground color.
        g_previewColorSpans.clear();

        if (highlightLang == PreviewHighlightLang::Json)
        {
            TokenizeJsonForHighlight(
                g_previewText,
                g_previewColorSpans);
        }
        else if (highlightLang == PreviewHighlightLang::Xml)
        {
            TokenizeXmlForHighlight(
                g_previewText,
                g_previewColorSpans);
        }

        r = GetPreviewTextRect();
    }
    else if (FileExtensionLower(path) ==
             L"svg")
    {
        int nativeW = 0;
        int nativeH = 0;

        if (!ParseSvgSize(
                path,
                nativeW,
                nativeH))
        {
            // Couldn't find a usable width/height/viewBox to derive
            // an aspect ratio from — square is as good a guess as
            // any, and D2D's default "meet" fit keeps the actual
            // artwork looking correct either way.
            nativeW = 512;
            nativeH = 512;
        }

        int renderW = 0;
        int renderH = 0;

        r = GetPreviewImageRect(
            nativeW,
            nativeH,
            renderW,
            renderH);

        std::vector<BYTE> pixels;

        if (!RenderSvgToPixels(
                path,
                nativeW,
                nativeH,
                renderW,
                renderH,
                pixels))
        {
            return;
        }

        g_previewImagePixels =
            new std::vector<BYTE>(
                std::move(pixels));

        g_previewImage =
            new Gdiplus::Bitmap(
                renderW,
                renderH,
                renderW * 4,
                PixelFormat32bppPARGB,
                g_previewImagePixels
                    ->data());

        g_previewKind = PreviewKind::Image;
        g_previewPath = path;
        g_previewImageRenderW = renderW;
        g_previewImageRenderH = renderH;
    }
    else
    {
        Gdiplus::Bitmap* bitmap = nullptr;

        if (!LoadPreviewImageFile(
                path,
                bitmap))
        {
            return;
        }

        g_previewKind = PreviewKind::Image;
        g_previewPath = path;
        g_previewImage = bitmap;

        r = GetPreviewImageRect(
            (int)bitmap->GetWidth(),
            (int)bitmap->GetHeight(),
            g_previewImageRenderW,
            g_previewImageRenderH);
    }

    SetWindowPos(
        g_preview,
        HWND_TOPMOST,
        r.left,
        r.top,
        r.right - r.left,
        r.bottom - r.top,
        SWP_SHOWWINDOW |
            SWP_NOACTIVATE);

    // A previous fade may have left the window's real layered
    // alpha partway down — g_previewAlpha was already reset to 1
    // by HidePreview() above, but that needs re-applying to the
    // window itself now that it's visible again.
    ApplyPreviewAlpha();

    InvalidateRect(
        g_preview,
        nullptr,
        FALSE);
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
static const int MAX_ITEMS = 10;

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

// Mirrors GetKnownFolderIcon()'s "real folder icon, generic
// fallback" shape, but sourced from ResolveGamesFolder()'s own
// fallback chain rather than a single known folder — whichever
// location it resolves to, this shows that folder's actual icon
// (honoring any desktop.ini override the same way the other
// folders' icons do).
static HICON GetGamesFolderIcon()
{
    std::wstring path;

    if (ResolveGamesFolder(path))
    {
        HICON icon =
            GetFileIcon(
                path.c_str());

        if (icon)
            return icon;
    }

    return GetStockIcon(
        SIID_FOLDER);
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
        GetKnownFolderIcon(
            FOLDERID_Videos,
            SIID_VIDEOFILES);

    g_allIcons[7] =
        GetGamesFolderIcon();

    g_allIcons[8] =
        GetControlPanelIcon();

    g_allIcons[9] =
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
"; Every row ClassicShell can show, in the order listed here — this\r\n"
"; file's own line order is the menu's display order, so reordering\r\n"
"; these lines reorders the menu. Set any of these to 0 to hide that\r\n"
"; row; all default to 1 (shown). If every item ends up disabled,\r\n"
"; all of them show anyway rather than leaving an empty menu.\r\n"
"ThisPC=1\r\n"
"Programs=1\r\n"
"Documents=1\r\n"
"Downloads=1\r\n"
"Pictures=1\r\n"
"Music=1\r\n"
"Videos=1\r\n"
"Games=1\r\n"
"ControlPanel=1\r\n"
"Run=1\r\n"
"\r\n"
"[Search]\r\n"
"; Folder ClassicShell indexes in the background at startup so the\r\n"
"; search box can autocomplete file paths. Type a wildcard pattern\r\n"
"; like *.txt or report.* for an exact match, or just plain text\r\n"
"; like report to find any file containing it; known apps/folders/\r\n"
"; commands still take priority when you press Enter.\r\n"
"; Indexing runs on a low-priority background thread and never\r\n"
"; blocks the UI, however long the folder takes to walk.\r\n"
"IndexPath={PROFILE}\r\n";

static std::wstring GetConfigPath()
{
    return
        GetExeDirectory() +
        CONFIG_FILE_NAME;
}

// The current user's profile folder (C:\Users\<name>) — the
// default search index root, substituted into the default config
// content in place of the "{PROFILE}" placeholder.
static std::wstring GetProfileDirectory()
{
    wchar_t buffer[MAX_PATH]{};

    DWORD n =
        GetEnvironmentVariableW(
            L"USERPROFILE",
            buffer,
            MAX_PATH);

    return
        (n && n < MAX_PATH)
            ? std::wstring(buffer)
            : std::wstring();
}

// Writes the default config file iff nothing is there yet — never
// overwrites a file the user has already customized.
static void EnsureConfigFile()
{
    std::wstring path =
        GetConfigPath();

    if (FileExists(path))
        return;

    std::string content =
        DEFAULT_CONFIG_CONTENT;

    std::wstring profile =
        GetProfileDirectory();

    int utf8Len =
        WideCharToMultiByte(
            CP_UTF8,
            0,
            profile.c_str(),
            -1,
            nullptr,
            0,
            nullptr,
            nullptr);

    std::string profileUtf8;

    if (utf8Len > 1)
    {
        profileUtf8.resize(
            utf8Len - 1);

        WideCharToMultiByte(
            CP_UTF8,
            0,
            profile.c_str(),
            -1,
            &profileUtf8[0],
            utf8Len,
            nullptr,
            nullptr);
    }

    size_t token =
        content.find(
            "{PROFILE}");

    if (token !=
        std::string::npos)
    {
        content.replace(
            token,
            9,
            profileUtf8);
    }

    std::ofstream file(
        path,
        std::ios::binary);

    if (!file.is_open())
        return;

    file.write(
        content.data(),
        (std::streamsize)
            content.size());
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
    g_startOwnAlpha = g_windowAlpha;

    wchar_t indexPath[MAX_PATH];

    GetPrivateProfileStringW(
        L"Search",
        L"IndexPath",
        L"",
        indexPath,
        MAX_PATH,
        configPath.c_str());

    g_searchIndexRoot =
        Trim(indexPath);

    if (g_searchIndexRoot.empty())
    {
        g_searchIndexRoot =
            GetProfileDirectory();
    }
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
    { L"Videos",        L"videos",        L"Videos" },
    { L"Games",         L"games",         L"Games" },
    { L"Control Panel", L"control panel", L"ControlPanel" },
    { L"Run...",        L"run",           L"Run" }
};

// Filtered down to just the enabled items, in the same order —
// what painting/hit-testing/activation actually iterate over.
static MenuItem g_items[MAX_ITEMS];

// Reads classicshell.ini's [MenuItems] section (one on/off toggle
// per entry in ALL_ITEMS, defaulting to on) and rebuilds g_items /
// g_icons / g_itemCount to match. The order the keys appear IN THE
// FILE becomes the menu's own display order — read via
// GetPrivateProfileSectionW, which returns a section's entries in
// their on-disk order, rather than looking each key up individually
// by name (which would have no way to know what order the user
// wanted). Any item the file never mentions at all — most commonly
// a toggle added by an app update, sitting in an older ini that
// predates it — defaults to shown and is appended after everything
// the file did list, so it doesn't just silently disappear. Must
// run after CreateIcons(), so g_allIcons is already populated. If
// every item ends up disabled (e.g. a typo'd ini), falls back to
// showing all of them rather than leaving an empty, useless menu.
static void BuildVisibleItems()
{
    std::wstring configPath =
        GetConfigPath();

    g_itemCount = 0;

    bool used[MAX_ITEMS] = {};

    std::vector<wchar_t> section(
        4096);

    GetPrivateProfileSectionW(
        L"MenuItems",
        section.data(),
        (DWORD)section.size(),
        configPath.c_str());

    const wchar_t* p =
        section.data();

    while (*p)
    {
        std::wstring entry = p;
        p += entry.size() + 1;

        size_t eq =
            entry.find(L'=');

        if (eq == std::wstring::npos)
            continue;

        std::wstring key =
            entry.substr(0, eq);

        std::wstring value =
            entry.substr(eq + 1);

        int foundIndex = -1;

        for (int i = 0;
             i < MAX_ITEMS;
             ++i)
        {
            if (_wcsicmp(
                    key.c_str(),
                    ALL_ITEMS[i]
                        .iniKey) == 0)
            {
                foundIndex = i;
                break;
            }
        }

        // Unrecognized key (a typo, or leftover from a removed
        // item) or one already seen once this pass — either way,
        // nothing to do with it.
        if (foundIndex < 0 ||
            used[foundIndex])
        {
            continue;
        }

        used[foundIndex] = true;

        if (_wtoi(value.c_str()) == 0)
            continue;

        g_items[g_itemCount] =
            ALL_ITEMS[foundIndex];

        g_icons[g_itemCount] =
            g_allIcons[foundIndex];

        g_itemCount++;
    }

    for (int i = 0;
         i < MAX_ITEMS;
         ++i)
    {
        if (used[i])
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

// The fixed width of the main menu column — items, search box,
// power button, slider. Independent of the window's actual client
// width, which widens to fit the side results panel without this
// (or anything laid out against it) shifting or resizing.
static int MainColumnWidth()
{
    return S(210);
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

// The clear button's hit/paint area — a small square, vertically
// centered in the search box, inset from its right edge by the
// traditional margin. Text drawing/hit-testing needs to know where
// this starts too, so nothing renders or is clickable underneath it.
static RECT GetSearchClearButtonRect(
    const RECT& search)
{
    int size = S(20);

    int top =
        search.top +
        ((search.bottom - search.top) -
         size) /
            2;

    int right =
        search.right - S(9);

    return
    {
        right - size,
        top,
        right,
        top + size
    };
}

static int SearchResultRowHeight()
{
    return S(38);
}

// Width of the translucent side panel showing wildcard matches —
// deliberately the same as the main column, so the two line up as
// matching twin cards rather than looking like a mismatched
// afterthought — and the gap that separates the two.
static int SidePanelWidth()
{
    return MainColumnWidth();
}

static int SidePanelGap()
{
    return S(10);
}

// True while the panel should occupy space and be drawn.
static bool ShouldShowSidePanel()
{
    return !g_searchResults.empty();
}

// To the right of the main column, matching its top and bottom
// margins exactly (same S(8) inset both edges) so the two read as
// a pair of same-sized twin cards rather than the panel being a
// shorter box that just happens to start at the same top. Rows
// are laid out from the top of this box, so with fewer than a
// full screen's worth of matches the card simply has some quiet
// space below the last row instead of shrinking to fit.
//
// The right edge is inset a couple of pixels short of the window's
// own true edge — without that, the card's border has nowhere to
// render its outer half (the window's backing canvas ends exactly
// where the border's stroke would need to), leaving that one side
// looking unbordered/cut off compared to the other three.
static RECT GetSideResultsPanelRect(
    int mainHeight)
{
    int left =
        MainColumnWidth() +
        SidePanelGap();

    return
    {
        left,
        S(8),
        left + SidePanelWidth() - S(2),
        mainHeight - S(8)
    };
}

// How many whole rows fit in the panel at once.
static int SearchResultsVisibleRowCount(
    int mainHeight)
{
    RECT panel =
        GetSideResultsPanelRect(
            mainHeight);

    int contentHeight =
        (panel.bottom - panel.top) -
        S(8);

    int rows =
        contentHeight /
        SearchResultRowHeight();

    return
        rows < 1
            ? 1
            : rows;
}

static int SearchResultsScrollbarWidth()
{
    return S(4);
}

// Keeps g_searchResultsScroll in range for the current result
// count and panel size — call after either changes.
static void ClampSearchResultsScroll(
    int mainHeight)
{
    int visible =
        SearchResultsVisibleRowCount(
            mainHeight);

    int maxScroll =
        (int)g_searchResults.size() -
        visible;

    if (maxScroll < 0)
        maxScroll = 0;

    if (g_searchResultsScroll > maxScroll)
        g_searchResultsScroll = maxScroll;

    if (g_searchResultsScroll < 0)
        g_searchResultsScroll = 0;
}

// The row rect for a given absolute result index, positioned per
// the current scroll offset — rows scrolled out of view land above
// or below the panel's own bounds, which callers rely on (painting
// only draws the visible slice; a focused-but-scrolled-out row
// simply doesn't get a ring drawn for it).
static RECT GetSearchResultRect(
    int mainHeight,
    int index)
{
    RECT panel =
        GetSideResultsPanelRect(
            mainHeight);

    int rowH =
        SearchResultRowHeight();

    int y =
        panel.top + S(4) +
        (index - g_searchResultsScroll) *
            rowH;

    int rightInset =
        (int)g_searchResults.size() >
            SearchResultsVisibleRowCount(
                mainHeight)
            ? SearchResultsScrollbarWidth() +
                  S(6)
            : 0;

    return
    {
        panel.left + S(4),
        y,
        panel.right - S(4) - rightInset,
        y + rowH
    };
}

// Content-aware window height: just tall enough for the items,
// the search row when it's actually showing, and the bottom
// utility strip — no trailing whitespace when there's nothing to
// fill it. The wildcard-match side panel is deliberately excluded:
// it's a companion card to the right, not something the main
// column grows to accommodate.
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

    int width =
        MainColumnWidth();

    if (ShouldShowSidePanel())
    {
        width +=
            SidePanelGap() +
            SidePanelWidth();
    }

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

// ============================================================
// Opacity slider — external window targeting
// ============================================================
//
// Scrolling the wheel while hovering the slider cycles its target
// through every other real window on the desktop, in front-to-back
// z-order, with the Start menu itself as the anchor at one end —
// scroll away from it to reach for other windows, scroll back to
// return. Whichever window is targeted gets an accent-colored
// highlight border so it's obvious what the slider is about to
// change, and its opacity syncs onto the slider immediately (drag
// picks up exactly where that window already was, not wherever the
// slider happened to be left).
//
// Adjustments stick: moving on to a different window, closing the
// menu, or quitting ClassicShell entirely never undoes an opacity
// change already made — the whole point is a real, lasting
// transparency adjustment, the same as any standalone window-
// transparency utility, not a preview that snaps back the moment
// you look away.

static std::vector<HWND> g_opacityExternalWindows;

static UINT_PTR g_opacityTrackTimer = 0;
static const UINT_PTR TIMER_OPACITY_TRACK = 9;

static void HideOpacityHighlight()
{
    if (g_opacityTrackTimer)
    {
        if (g_start)
        {
            KillTimer(
                g_start,
                TIMER_OPACITY_TRACK);
        }

        g_opacityTrackTimer = 0;
    }

    if (g_opacityHighlight)
    {
        ShowWindow(
            g_opacityHighlight,
            SW_HIDE);
    }
}

// Stops actively tracking whichever window the slider was last
// pointed at — hides the highlight border and stops re-polling its
// rect — without touching its actual opacity at all. Safe to call
// any time, including when nothing is currently targeted.
static void StopOpacityHighlight()
{
    g_opacityHighlightTarget = nullptr;

    HideOpacityHighlight();
}

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

// Belt-and-suspenders against the keyboard hook's own edge cases:
// even with KeyboardProc correctly swallowing a standalone Win tap's
// key-up (see WinKeyTracker/eat-and-replace in the hook itself), a
// native shell surface could in principle still slip open through
// some path this app's hook never sees. Any time some other window
// takes the foreground while our own menu
// is supposed to be showing, and that window belongs to one of the
// native shell surfaces a bare Win tap can raise, immediately
// dismiss it — a synthetic Escape, exactly what a real keypress
// would do — and hand the foreground back to our own window.
//
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

static void DismissNativeStartMenuIfNeeded(
    HWND foreground)
{
    if (!g_startVisible ||
        !foreground ||
        !g_start)
    {
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

    if (!isNativeSurface)
        return;

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

    SetForegroundWindow(g_start);
}

// SetWindowPos(..., HWND_TOPMOST, ...) only moves the highlight to
// the top of the topmost band at the instant it's called — it
// doesn't stay pinned there against whatever happens next. If the
// target itself is (or is under) an always-on-top window and that
// window gets activated, or really any window comes to the
// foreground, Windows can raise it back above our highlight before
// the next position-tracking timer tick gets a chance to reassert
// it, which is exactly what looked like the border "getting
// covered." Rather than reacting on a poll, this fires the instant
// any window becomes foreground, anywhere on the desktop, and
// immediately re-asserts topmost — no visible gap for something
// else to sneak in front during. Also doubles as the delivery point
// for DismissNativeStartMenuIfNeeded() above, since both are driven
// by the exact same "something just became foreground" event.
static void CALLBACK OpacityForegroundEventProc(
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

    if (!g_opacityHighlightTarget ||
        !g_opacityHighlight)
    {
        return;
    }

    if (!IsWindowVisible(
            g_opacityHighlight))
    {
        return;
    }

    SetWindowPos(
        g_opacityHighlight,
        HWND_TOPMOST,
        0, 0, 0, 0,
        SWP_NOMOVE |
            SWP_NOSIZE |
            SWP_NOACTIVATE);
}

// Renders the border into a true per-pixel-alpha bitmap and hands
// it straight to the OS via UpdateLayeredWindow, rather than the
// WM_PAINT + color-key approach used at first. Color-key
// transparency is a binary "this exact color = invisible" rule with
// no concept of partial alpha, but DWM's own corner rounding (see
// ApplyWindowRounding) clips the window with a smoothly anti-
// aliased mask — right at the curve, DWM tries to fade our content
// out gradually and there's no real alpha for it to fade, so the
// 1px stroke came out uneven/faded exactly at the corners instead
// of staying a crisp line all the way around. A real alpha channel
// is what DWM's rounding actually needs to blend against cleanly.
static void RenderOpacityHighlight(
    int width,
    int height,
    int screenX,
    int screenY)
{
    if (!g_opacityHighlight ||
        width <= 0 ||
        height <= 0)
    {
        return;
    }

    Gdiplus::Bitmap bitmap(
        width,
        height,
        PixelFormat32bppPARGB);

    {
        Gdiplus::Graphics graphics(
            &bitmap);

        graphics.SetSmoothingMode(
            Gdiplus::SmoothingModeAntiAlias);

        graphics.Clear(
            Gdiplus::Color(
                0, 0, 0, 0));

        Gdiplus::Pen pen(
            Gdiplus::Color(
                255,
                GetRValue(g_accent),
                GetGValue(g_accent),
                GetBValue(g_accent)),
            1.0f);

        // Inset half a pixel so the 1px stroke, centered on the
        // path, lands fully inside the bitmap on every side rather
        // than being half-clipped at the true edge — the same fix
        // used for the preview panel's own border earlier.
        graphics.DrawRectangle(
            &pen,
            0.5f,
            0.5f,
            (float)(width - 1),
            (float)(height - 1));
    }

    Gdiplus::Rect fullRect(
        0, 0, width, height);

    Gdiplus::BitmapData bmpData{};

    if (bitmap.LockBits(
            &fullRect,
            Gdiplus::ImageLockModeRead,
            PixelFormat32bppPARGB,
            &bmpData) != Gdiplus::Ok)
    {
        return;
    }

    HDC screenDc =
        GetDC(nullptr);

    HDC memDc =
        CreateCompatibleDC(
            screenDc);

    BITMAPINFO bmi{};

    bmi.bmiHeader.biSize =
        sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = width;
    bmi.bmiHeader.biHeight = -height;
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression =
        BI_RGB;

    void* dibBits = nullptr;

    HBITMAP dib =
        CreateDIBSection(
            screenDc,
            &bmi,
            DIB_RGB_COLORS,
            &dibBits,
            nullptr,
            0);

    if (dib && dibBits)
    {
        for (int y = 0;
             y < height;
             ++y)
        {
            memcpy(
                (BYTE*)dibBits +
                    (size_t)y *
                        width * 4,
                (BYTE*)bmpData.Scan0 +
                    (size_t)y *
                        bmpData.Stride,
                (size_t)width * 4);
        }

        HGDIOBJ oldBitmap =
            SelectObject(
                memDc,
                dib);

        POINT destPos
        {
            screenX,
            screenY
        };

        POINT srcPos{ 0, 0 };
        SIZE sz{ width, height };

        BLENDFUNCTION blend{};

        blend.BlendOp = AC_SRC_OVER;
        blend.SourceConstantAlpha = 255;
        blend.AlphaFormat = AC_SRC_ALPHA;

        UpdateLayeredWindow(
            g_opacityHighlight,
            screenDc,
            &destPos,
            &sz,
            memDc,
            &srcPos,
            0,
            &blend,
            ULW_ALPHA);

        SelectObject(
            memDc,
            oldBitmap);

        DeleteObject(dib);
    }

    bitmap.UnlockBits(&bmpData);

    DeleteDC(memDc);

    ReleaseDC(
        nullptr,
        screenDc);
}

static void RepositionOpacityHighlight(
    HWND target)
{
    if (!g_opacityHighlight || !target)
        return;

    RECT r{};

    if (!GetWindowRect(
            target,
            &r))
    {
        return;
    }

    ShowWindow(
        g_opacityHighlight,
        SW_SHOWNOACTIVATE);

    RenderOpacityHighlight(
        r.right - r.left,
        r.bottom - r.top,
        r.left,
        r.top);

    SetWindowPos(
        g_opacityHighlight,
        HWND_TOPMOST,
        0, 0, 0, 0,
        SWP_NOMOVE |
            SWP_NOSIZE |
            SWP_NOACTIVATE);
}

// Shows the highlight for whatever's currently targeted, without
// changing WHAT that is — defaults to the Start menu the first time
// this runs in a given menu session (before any scrolling has
// happened yet). Pairs with HideOpacityHighlight(), which hides the
// window again without forgetting the target, so hovering away and
// back always resumes on the same window rather than resetting.
static void ResumeOpacityHighlight()
{
    if (!g_opacityHighlightTarget)
        g_opacityHighlightTarget = g_start;

    RepositionOpacityHighlight(
        g_opacityHighlightTarget);

    if (g_start &&
        !g_opacityTrackTimer)
    {
        g_opacityTrackTimer =
            SetTimer(
                g_start,
                TIMER_OPACITY_TRACK,
                150,
                nullptr);
    }
}

// Points the highlight at the Start menu itself — the default,
// index-0 target. No WS_EX_LAYERED/alpha bookkeeping needed here
// unlike SetExternalOpacityTarget: g_start is already layered, and
// its own opacity is handled by ApplyOpacityToCurrentTarget's
// existing index == 0 case, not by this function.
static void SetStartAsOpacityTarget()
{
    g_opacityHighlightTarget = g_start;

    ResumeOpacityHighlight();
}

// Makes hwnd the slider's active external target: adds WS_EX_LAYERED
// if it doesn't already have it (left in place afterward — see the
// section note on why this never gets undone), reads its current
// opacity so the slider starts exactly where that window already
// was, and moves the highlight border onto it.
static void SetExternalOpacityTarget(
    HWND hwnd)
{
    if (hwnd == g_opacityHighlightTarget)
        return;

    StopOpacityHighlight();

    if (!hwnd)
        return;

    LONG_PTR ex =
        GetWindowLongPtrW(
            hwnd,
            GWL_EXSTYLE);

    BYTE currentAlpha = 255;

    if (ex & WS_EX_LAYERED)
    {
        BYTE a = 255;
        DWORD flags = 0;
        COLORREF key = 0;

        if (GetLayeredWindowAttributes(
                hwnd,
                &key,
                &a,
                &flags) &&
            (flags & LWA_ALPHA))
        {
            currentAlpha = a;
        }
    }
    else
    {
        SetWindowLongPtrW(
            hwnd,
            GWL_EXSTYLE,
            ex | WS_EX_LAYERED);

        SetWindowPos(
            hwnd,
            nullptr,
            0, 0, 0, 0,
            SWP_NOMOVE |
                SWP_NOSIZE |
                SWP_NOZORDER |
                SWP_NOACTIVATE |
                SWP_FRAMECHANGED);
    }

    g_opacityHighlightTarget = hwnd;
    g_windowAlpha = currentAlpha;

    SetLayeredWindowAttributes(
        hwnd,
        0,
        g_windowAlpha,
        LWA_ALPHA);

    RepositionOpacityHighlight(hwnd);

    if (g_start &&
        !g_opacityTrackTimer)
    {
        g_opacityTrackTimer =
            SetTimer(
                g_start,
                TIMER_OPACITY_TRACK,
                150,
                nullptr);
    }
}

// Same filtering an Alt-Tab-style window switcher uses: visible,
// unowned, not a tool window, has a title, and isn't cloaked (a
// suspended UWP app or a window parked on another virtual desktop —
// technically "visible" but not actually shown to the user).
// Window classes belonging to the shell/system rather than to a
// normal, standalone application — excluded even though most would
// already be filtered out by the title/owner checks below, since
// leaving these alone entirely is worth being explicit about rather
// than relying only on incidental filtering.
static const wchar_t* const
    OPACITY_EXCLUDED_CLASSES[] =
{
    L"Shell_TrayWnd",
    L"Shell_SecondaryTrayWnd",
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
    auto* list =
        reinterpret_cast<
            std::vector<HWND>*>(lParam);

    if (hwnd == g_start ||
        hwnd == g_preview ||
        hwnd == g_toast ||
        hwnd == g_opacityHighlight)
    {
        return TRUE;
    }

    // Blanket exclusion of every window this process itself owns —
    // catches anything above by construction, plus incidental
    // windows (IME, hidden helper windows) that don't have their
    // own named global to compare against individually.
    DWORD ownerPid = 0;

    GetWindowThreadProcessId(
        hwnd,
        &ownerPid);

    if (ownerPid == GetCurrentProcessId())
        return TRUE;

    if (!IsWindowVisible(hwnd))
        return TRUE;

    // A minimized window's "position" is a meaningless off-screen
    // sentinel (Windows' -32000 convention) — nothing sensible to
    // frame with the highlight, and no visible effect to see until
    // it's restored anyway.
    if (IsIconic(hwnd))
        return TRUE;

    if (GetWindow(hwnd, GW_OWNER) !=
        nullptr)
    {
        return TRUE;
    }

    LONG_PTR ex =
        GetWindowLongPtrW(
            hwnd,
            GWL_EXSTYLE);

    if (ex & WS_EX_TOOLWINDOW)
        return TRUE;

    if (GetWindowTextLengthW(hwnd) == 0)
        return TRUE;

    wchar_t className[64]{};

    GetClassNameW(
        hwnd,
        className,
        (int)(sizeof(className) /
              sizeof(className[0])));

    for (auto* excluded :
         OPACITY_EXCLUDED_CLASSES)
    {
        if (_wcsicmp(
                className,
                excluded) == 0)
        {
            return TRUE;
        }
    }

    BOOL cloaked = FALSE;

    DwmGetWindowAttribute(
        hwnd,
        DWMWA_CLOAKED,
        &cloaked,
        sizeof(cloaked));

    if (cloaked)
        return TRUE;

    RECT r{};

    if (!GetWindowRect(hwnd, &r) ||
        r.right <= r.left ||
        r.bottom <= r.top)
    {
        return TRUE;
    }

    list->push_back(hwnd);

    return TRUE;
}

static void RefreshOpacityCandidates()
{
    g_opacityExternalWindows.clear();

    EnumWindows(
        EnumOpacityCandidateProc,
        reinterpret_cast<LPARAM>(
            &g_opacityExternalWindows));
}

// Moves the slider's target by one step in either direction —
// called once per wheel notch while hovering the slider. Building
// the candidate list fresh the moment it's first needed (rather
// than keeping it constantly up to date) means it's never stale by
// more than the current scroll session.
static void CycleOpacityTarget(
    int direction)
{
    if (g_opacityIndex == 0 &&
        direction > 0)
    {
        RefreshOpacityCandidates();
    }

    int maxIndex =
        (int)g_opacityExternalWindows
            .size();

    int newIndex =
        g_opacityIndex + direction;

    if (newIndex < 0)
        newIndex = 0;

    if (newIndex > maxIndex)
        newIndex = maxIndex;

    // A candidate can vanish between being listed and being
    // scrolled to (closed, or otherwise stopped existing) — skip
    // past any dead ones in the same direction rather than landing
    // on a dangling handle.
    while (newIndex > 0 &&
           newIndex <= maxIndex &&
           !IsWindow(
               g_opacityExternalWindows
                   [newIndex - 1]))
    {
        g_opacityExternalWindows.erase(
            g_opacityExternalWindows
                .begin() +
            (newIndex - 1));

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
            L"Scroll to reach another window, or "
            L"drag the slider to adjust this one.");
    }
    else
    {
        HWND target =
            g_opacityExternalWindows
                [g_opacityIndex - 1];

        SetExternalOpacityTarget(target);

        wchar_t caption[256]{};

        GetWindowTextW(
            target,
            caption,
            (int)(sizeof(caption) /
                  sizeof(caption[0])));

        ShowToast(
            caption[0]
                ? caption
                : L"(untitled window)",
            L"Scroll to browse windows, or drag "
            L"the slider to adjust this one's "
            L"opacity.");
    }

    if (g_start)
    {
        InvalidateRect(
            g_start,
            nullptr,
            FALSE);
    }
}

// Applies g_windowAlpha to whatever's currently targeted — the one
// place both the mouse-drag and keyboard paths funnel through, so
// dragging the slider or nudging it with the arrow keys behaves
// identically regardless of which window is on the other end.
static void ApplyOpacityToCurrentTarget()
{
    HWND target =
        (g_opacityIndex > 0 &&
         g_opacityHighlightTarget)
            ? g_opacityHighlightTarget
            : g_start;

    if (target)
    {
        SetLayeredWindowAttributes(
            target,
            0,
            g_windowAlpha,
            LWA_ALPHA);
    }

    if (g_opacityIndex == 0)
        g_startOwnAlpha = g_windowAlpha;
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

    // The main column's fixed width, not however wide the window
    // actually is right now — see the note in PaintStart.
    client.right =
        MainColumnWidth();

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

// Kicks off (if it isn't already running) the timer that eases the
// side panel's pop-in. Only ever called when the panel is newly
// showing content — hiding is instant (ClearSearchResults resets
// g_searchResultsAnim to 0 directly), since the panel's own size
// is derived from the current result count and there'd be nothing
// left to size a fade-out against once that's cleared.
static void StartSearchResultsAnimation(
    HWND hwnd)
{
    if (!g_searchResultsTimer &&
        hwnd)
    {
        g_searchResultsTimer =
            SetTimer(
                hwnd,
                TIMER_SEARCH_RESULTS_ANIM,
                10,
                nullptr);
    }
}

static void StepSearchResultsAnim(
    HWND hwnd)
{
    g_searchResultsAnim =
        EaseTo(
            g_searchResultsAnim,
            1.0f,
            0.3f);

    if (g_searchResultsAnim == 1.0f &&
        g_searchResultsTimer)
    {
        KillTimer(
            hwnd,
            TIMER_SEARCH_RESULTS_ANIM);

        g_searchResultsTimer = 0;
    }

    InvalidateRect(
        hwnd,
        nullptr,
        FALSE);
}

// Re-matches g_searchText against the background file index. An
// explicit '*'/'?' pattern is matched literally via PathMatchSpecW;
// plain text of 2+ characters (below that, almost everything in a
// large index matches, which is noise rather than a useful result)
// is matched as an implicit contains-search instead — a cheap
// lowercase substring find against each entry's precomputed
// fileNameLower, deliberately avoided via PathMatchSpecW's pattern
// engine to keep the per-keystroke full-index scan this now runs on
// every plain query cheap. See g_searchResultsIsWildcard for how
// Enter still tells the two cases apart.
static void RefreshSearchResults(
    HWND hwnd)
{
    bool wasShowing =
        !g_searchResults.empty();

    ClearSearchResults();

    std::wstring query =
        Trim(g_searchText);

    bool isWildcard =
        query.find(L'*') !=
            std::wstring::npos ||
        query.find(L'?') !=
            std::wstring::npos;

    bool isImplicitContains =
        !isWildcard &&
        query.size() >= 2;

    g_searchResultsIsWildcard =
        isWildcard;

    if (!query.empty() &&
        (isWildcard ||
         isImplicitContains))
    {
        std::wstring queryLower =
            Lower(query);

        // God Mode matches always go first — a wildcard pattern
        // like "*.txt" doesn't mean anything against a Control
        // Panel task's name, so this only ever applies to a plain
        // contains-style query, the same condition the file index
        // below uses for its own contains matching.
        if (isImplicitContains)
        {
            std::lock_guard<std::mutex>
                lock(g_godModeMutex);

            for (size_t i = 0;
                 i < g_godModeItems.size();
                 ++i)
            {
                if (g_godModeItems[i]
                        .nameLower.find(
                            queryLower) ==
                    std::wstring::npos)
                {
                    continue;
                }

                g_searchResults.push_back(
                    {
                        g_godModeItems[i].name,
                        g_godModeItems[i].icon,
                        SearchResultKind::GodMode,
                        (int)i
                    });

                if (g_searchResults.size() >=
                    MAX_GODMODE_SEARCH_MATCHES)
                {
                    break;
                }
            }
        }

        size_t remaining =
            MAX_SEARCH_RESULTS -
            g_searchResults.size();

        std::vector<std::wstring> matches;

        {
            std::lock_guard<std::mutex>
                lock(g_fileIndexMutex);

            for (const auto& entry :
                 g_fileIndex)
            {
                bool matched =
                    isWildcard
                        ? PathMatchSpecW(
                              entry.fileName
                                  .c_str(),
                              query.c_str())
                        : entry.fileNameLower
                                  .find(
                                      queryLower) !=
                              std::wstring::npos;

                if (!matched)
                    continue;

                matches.push_back(
                    entry.fullPath);

                if (matches.size() >=
                    remaining)
                {
                    break;
                }
            }
        }

        for (auto& path : matches)
        {
            HICON icon =
                GetFileIcon(
                    path.c_str());

            g_searchResults.push_back(
                {
                    std::move(path),
                    icon
                });
        }
    }

    bool isShowingNow =
        !g_searchResults.empty();

    if (wasShowing != isShowingNow)
        StartSearchResultsAnimation(hwnd);
}

// Opens or closes the side panel showing the full God Mode catalog —
// the Control Panel row's click handler, a disclosure toggle rather
// than a launcher (matching how the power button's own flyout
// already works in this app), reusing the exact same panel that
// wildcard search results show in rather than a separate popup.
static void ToggleControlPanelBrowse(
    HWND hwnd)
{
    if (g_controlPanelBrowsing)
    {
        ClearSearchResults();
        ResizeStartToContent(hwnd);

        InvalidateRect(
            hwnd,
            nullptr,
            FALSE);

        return;
    }

    // Opening it always starts from a clean slate — any text
    // already typed into the search box is what this replaces, not
    // something it layers on top of.
    g_searchText.clear();
    g_searchCaretPos = 0;
    g_searchSelAnchor = -1;

    ClearSearchResults();

    {
        std::lock_guard<std::mutex>
            lock(g_godModeMutex);

        for (size_t i = 0;
             i < g_godModeItems.size();
             ++i)
        {
            g_searchResults.push_back(
                {
                    g_godModeItems[i].name,
                    g_godModeItems[i].icon,
                    SearchResultKind::GodMode,
                    (int)i
                });
        }
    }

    // Nothing to show — either the background enumeration hasn't
    // finished yet (the menu was opened right after launch) or it
    // failed outright. Leaving g_controlPanelBrowsing false here
    // means the arrow doesn't flip open over an empty panel; the
    // toast reports exactly how far GodModeThreadProc got and with
    // what HRESULT, since "not ready" alone gives no way to tell a
    // startup-timing race from an actual enumeration failure.
    if (g_searchResults.empty())
    {
        wchar_t detail[256];

        swprintf(
            detail,
            256,
            L"stage %d, hr 0x%08lX, "
            L"seen %d, named %d",
            g_godModeStage.load(),
            (unsigned long)
                g_godModeLastHr.load(),
            g_godModeEnumSeen.load(),
            g_godModeNamed.load());

        ShowToast(
            L"Control Panel catalog not ready",
            detail);

        return;
    }

    g_searchResultsIsWildcard = false;
    g_controlPanelBrowsing = true;

    StartSearchResultsAnimation(hwnd);
    ResizeStartToContent(hwnd);

    InvalidateRect(
        hwnd,
        nullptr,
        FALSE);
}

// ============================================================
// Search box editing — shared by StartProc's own WM_KEYDOWN/
// WM_CHAR (hit when the window has real OS focus, e.g. right
// after a click) and the KeyboardProc hook (the guaranteed path,
// since the menu usually doesn't hold real focus). Both need the
// exact same caret/selection/text mutations, so that logic lives
// here once; each caller just supplies its own hwnd and does its
// own gating/return-value convention around the call.
// ============================================================

static void SearchSelectAll(
    HWND hwnd)
{
    g_searchSelAnchor = 0;
    g_searchCaretPos =
        (int)g_searchText.size();

    g_caretVisible = true;

    InvalidateRect(
        hwnd,
        nullptr,
        FALSE);
}

static void SearchCopySelection(
    HWND hwnd)
{
    int start, end;

    GetSearchSelectionRange(
        start,
        end);

    CopyTextToClipboard(
        hwnd,
        g_searchText.substr(
            start,
            end - start));
}

static void SearchMoveCaret(
    HWND hwnd,
    DWORD vk,
    bool shiftHeld)
{
    int newPos =
        g_searchCaretPos;

    if (vk == VK_LEFT)
    {
        if (newPos > 0)
            newPos--;
    }
    else if (vk == VK_RIGHT)
    {
        if (newPos <
            (int)g_searchText.size())
        {
            newPos++;
        }
    }
    else if (vk == VK_HOME)
    {
        newPos = 0;
    }
    else // VK_END
    {
        newPos =
            (int)g_searchText.size();
    }

    g_searchCaretPos = newPos;

    if (shiftHeld)
    {
        if (g_searchSelAnchor < 0)
        {
            // Anchor wasn't set yet — whichever end the caret
            // didn't just move from.
            g_searchSelAnchor =
                vk == VK_LEFT ||
                    vk == VK_HOME
                    ? newPos + 1
                    : newPos - 1;
        }
    }
    else
    {
        g_searchSelAnchor = -1;
    }

    g_caretVisible = true;

    InvalidateRect(
        hwnd,
        nullptr,
        FALSE);
}

static void SearchBackspace(
    HWND hwnd)
{
    bool changed =
        DeleteSearchSelection();

    if (!changed &&
        g_searchCaretPos > 0)
    {
        g_searchText.erase(
            g_searchCaretPos - 1,
            1);

        g_searchCaretPos--;

        changed = true;
    }

    if (changed)
    {
        g_caretVisible = true;

        RefreshSearchResults(hwnd);
        ResizeStartToContent(hwnd);

        InvalidateRect(
            hwnd,
            nullptr,
            FALSE);
    }
}

static void SearchDeleteForward(
    HWND hwnd)
{
    bool changed =
        DeleteSearchSelection();

    if (!changed &&
        g_searchCaretPos <
            (int)g_searchText.size())
    {
        g_searchText.erase(
            g_searchCaretPos,
            1);

        changed = true;
    }

    if (changed)
    {
        g_caretVisible = true;

        RefreshSearchResults(hwnd);
        ResizeStartToContent(hwnd);

        InvalidateRect(
            hwnd,
            nullptr,
            FALSE);
    }
}

// Empties the box outright — the clear button's action. Resizing
// back down and re-running RefreshSearchResults (which clears any
// results/preview and, since the box goes from non-empty to empty,
// makes ShouldShowSidePanel() false) is what actually makes the box
// itself disappear again, same as backspacing the last character.
static void ClearSearchBox(
    HWND hwnd)
{
    g_searchText.clear();
    g_searchCaretPos = 0;
    g_searchSelAnchor = -1;
    g_caretVisible = true;

    RefreshSearchResults(hwnd);
    ResizeStartToContent(hwnd);

    InvalidateRect(
        hwnd,
        nullptr,
        FALSE);
}

static void SearchInsertChar(
    HWND hwnd,
    wchar_t c)
{
    DeleteSearchSelection();

    g_searchText.insert(
        g_searchCaretPos,
        1,
        c);

    g_searchCaretPos++;
    g_caretVisible = true;

    RefreshSearchResults(hwnd);
    ResizeStartToContent(hwnd);

    InvalidateRect(
        hwnd,
        nullptr,
        FALSE);
}

static void LaunchSearchResult(
    int index)
{
    if (index < 0 ||
        index >=
            (int)g_searchResults
                .size())
    {
        return;
    }

    if (g_searchResults[index].kind ==
        SearchResultKind::GodMode)
    {
        int gi =
            g_searchResults[index]
                .godModeIndex;

        bool ok = false;

        {
            std::lock_guard<std::mutex>
                lock(g_godModeMutex);

            if (gi >= 0 &&
                gi < (int)g_godModeItems.size())
            {
                ok =
                    LaunchGodModeItem(
                        g_godModeItems[gi]);
            }
        }

        if (!ok)
            return;

        g_searchText.clear();

        CloseStart();

        return;
    }

    // ShellExecuteW returns an HINSTANCE that's actually just an
    // error code when launching failed (documented as <= 32,
    // matching one of the SE_ERR_* values) — checked so a stale
    // index entry (the file's since been moved/deleted, common
    // for e.g. browser cache files that get rewritten constantly)
    // doesn't silently report success by closing the menu as if
    // it had actually opened something.
    HINSTANCE result =
        ShellExecuteW(
            nullptr,
            L"open",
            g_searchResults[index]
                .path.c_str(),
            nullptr,
            nullptr,
            SW_SHOWNORMAL);

    if ((INT_PTR)result <= 32)
        return;

    g_searchText.clear();

    CloseStart();
}

// Right-click alternative to LaunchSearchResult(): opens the
// result's containing folder in Explorer with the file itself
// pre-selected, rather than launching the file. Meaningless for a
// God Mode entry — there's no real file to select — so it's simply
// a no-op for that kind rather than trying to fabricate a path.
static void OpenSearchResultFolder(
    int index)
{
    if (index < 0 ||
        index >=
            (int)g_searchResults
                .size() ||
        g_searchResults[index].kind ==
            SearchResultKind::GodMode)
    {
        return;
    }

    std::wstring params =
        L"/select,\"" +
        g_searchResults[index]
            .path +
        L"\"";

    HINSTANCE result =
        ShellExecuteW(
            nullptr,
            L"open",
            L"explorer.exe",
            params.c_str(),
            nullptr,
            SW_SHOWNORMAL);

    if ((INT_PTR)result <= 32)
        return;

    g_searchText.clear();

    CloseStart();
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

    // Control Panel is a disclosure, not a launcher — clicking it
    // opens (or closes) the God Mode catalog in the side panel
    // rather than jumping straight to control.exe, the same way a
    // classic Start Menu's own "▸" submenu items never launch
    // anything by themselves.
    if (command == L"control panel")
    {
        ToggleControlPanelBrowse(g_start);
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

    LaunchResult result =
        ExecuteSmartInput(command);

    if (result != LaunchResult::Success)
    {
        if (command == L"games")
        {
            ShowToast(
                L"Games folder not found",
                L"Checked the Games Explorer folder and the "
                L"Start Menu's Games group (common and "
                L"per-user) — none exist on this PC.");
        }
        else
        {
            ShowToast(
                L"\"" +
                    std::wstring(
                        g_items[item].label) +
                    L"\" not found",
                L"Nothing could be resolved or launched for "
                L"this item.");
        }
    }

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
    // search + wildcard results + items + quick tools + slider +
    // power
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

    // If focus landed on a result row currently scrolled out of
    // view, scroll just enough to bring it back in — the same
    // "keep the focused item visible" behavior any scrollable list
    // gives you.
    FocusTarget t =
        ResolveFocus(
            g_focusIndex);

    if (t.kind ==
        FocusKind::SearchResult)
    {
        RECT client{};

        GetClientRect(
            hwnd,
            &client);

        int visible =
            SearchResultsVisibleRowCount(
                client.bottom);

        if (t.index <
            g_searchResultsScroll)
        {
            g_searchResultsScroll =
                t.index;
        }
        else if (t.index >=
                 g_searchResultsScroll +
                     visible)
        {
            g_searchResultsScroll =
                t.index - visible + 1;
        }

        ClampSearchResultsScroll(
            client.bottom);
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
            LaunchSearchResult(
                t.index);
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

        case FocusKind::SearchResult:
            r =
                GetSearchResultRect(
                    height,
                    t.index);
            radius = S(6);
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
// Preview paint
// ============================================================

static void PaintPreview(
    HWND hwnd,
    HDC dc)
{
    RECT client{};

    GetClientRect(
        hwnd,
        &client);

    int width = client.right;
    int height = client.bottom;

    if (width <= 0 || height <= 0)
        return;

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

    RECT full = { 0, 0, width, height };

    FillRectColor(
        back,
        full,
        g_bg);

    const int pad = S(10);

    if (g_previewKind == PreviewKind::Text)
    {
        PreviewTextLayout layout =
            ComputePreviewTextLayout(hwnd);

        const RECT& content = layout.content;
        int lineH = layout.lineH;
        int charWidth = layout.charWidth;
        const std::vector<std::wstring>& lines = layout.lines;
        const std::vector<size_t>& lineOffsets =
            layout.lineOffsets;
        int firstLine = layout.firstLine;
        int visibleLines = layout.visibleLines;
        int textRight = layout.textRight;
        bool needsScrollbar = layout.needsScrollbar;

        int lastLine =
            firstLine + visibleLines;

        if (lastLine > (int)lines.size())
            lastLine = (int)lines.size();

        size_t selStart = 0;
        size_t selEnd = 0;
        bool hasSel = g_previewHasSelection;

        if (hasSel)
        {
            selStart =
                std::min(
                    g_previewSelAnchor,
                    g_previewSelCaret);

            selEnd =
                std::max(
                    g_previewSelAnchor,
                    g_previewSelCaret);
        }

        int maxVisibleChars =
            charWidth > 0
                ? (textRight - content.left) /
                      charWidth
                : 0;

        // Only ever consulted for a highlighted (JSON/XML) preview,
        // and only ever moved forward — both spans and the lines
        // being drawn are already in increasing offset order, so a
        // span once passed can never apply to a later line either.
        size_t spanIdx = 0;

        if (!lines.empty())
        {
            size_t firstLineOffset =
                lineOffsets[firstLine];

            while (spanIdx <
                       g_previewColorSpans.size() &&
                   g_previewColorSpans[spanIdx].end <=
                       firstLineOffset)
            {
                spanIdx++;
            }
        }

        HGDIOBJ oldFont =
            SelectObject(back, g_mono);

        int y = content.top;

        for (int i = firstLine;
             i < lastLine;
             ++i)
        {
            const std::wstring& lineText =
                lines[i];

            size_t lineStart = lineOffsets[i];
            size_t lineLen = lineText.size();
            size_t lineEndOff = lineStart + lineLen;

            if (hasSel &&
                selEnd > lineStart &&
                selStart < lineEndOff)
            {
                size_t hiStart =
                    std::max(selStart, lineStart) -
                    lineStart;

                size_t hiEnd =
                    std::min(selEnd, lineEndOff) -
                    lineStart;

                RECT selRect =
                {
                    content.left +
                        (int)hiStart * charWidth,
                    y,
                    content.left +
                        (int)hiEnd * charWidth,
                    y + lineH
                };

                FillRectColor(
                    back,
                    selRect,
                    MixColor(
                        g_bg,
                        g_accent,
                        35));
            }

            bool highlighted =
                g_previewHighlightLang !=
                    PreviewHighlightLang::None &&
                !g_previewColorSpans.empty();

            // How much of the line fits before the panel's own
            // right edge, truncating with a trailing ellipsis the
            // same way DT_END_ELLIPSIS used to — done by hand only
            // for the highlighted path, since it draws a line as
            // several separately-colored runs instead of the one
            // DrawTextW call the plain path still gets to use.
            bool truncate =
                highlighted &&
                (int)lineLen > maxVisibleChars &&
                maxVisibleChars > 3;

            size_t drawLen =
                truncate
                    ? (size_t)(maxVisibleChars - 3)
                    : lineLen;

            if (!highlighted)
            {
                DrawTextSimple(
                    back,
                    lineText.c_str(),
                    content.left,
                    y,
                    textRight - content.left,
                    lineH,
                    g_text,
                    g_mono,
                    DT_LEFT |
                        DT_SINGLELINE |
                        DT_NOPREFIX |
                        DT_END_ELLIPSIS);
            }
            else
            {
                size_t col = 0;

                while (col < drawLen)
                {
                    size_t absPos = lineStart + col;

                    while (spanIdx <
                               g_previewColorSpans.size() &&
                           g_previewColorSpans[spanIdx]
                                   .end <= absPos)
                    {
                        spanIdx++;
                    }

                    bool inSpan =
                        spanIdx <
                            g_previewColorSpans.size() &&
                        g_previewColorSpans[spanIdx]
                                .start <= absPos &&
                        absPos <
                            g_previewColorSpans[spanIdx]
                                .end;

                    PreviewTokenColor color =
                        inSpan
                            ? g_previewColorSpans
                                  [spanIdx]
                                      .color
                            : PreviewTokenColor::
                                  Default;

                    size_t runEndOff =
                        inSpan
                            ? g_previewColorSpans
                                  [spanIdx]
                                      .end
                            : (spanIdx <
                               g_previewColorSpans
                                   .size()
                                   ? g_previewColorSpans
                                         [spanIdx]
                                             .start
                                   : lineStart +
                                         drawLen);

                    if (runEndOff >
                        lineStart + drawLen)
                    {
                        runEndOff =
                            lineStart + drawLen;
                    }

                    size_t colEnd =
                        runEndOff - lineStart;

                    if (colEnd <= col)
                        colEnd = col + 1;

                    if (colEnd > drawLen)
                        colEnd = drawLen;

                    std::wstring run =
                        lineText.substr(
                            col,
                            colEnd - col);

                    DrawTextSimple(
                        back,
                        run.c_str(),
                        content.left +
                            (int)col * charWidth,
                        y,
                        (int)(colEnd - col) *
                                charWidth +
                            S(4),
                        lineH,
                        PreviewTokenRGB(color),
                        g_mono,
                        DT_LEFT |
                            DT_SINGLELINE |
                            DT_NOPREFIX);

                    col = colEnd;
                }

                if (truncate)
                {
                    DrawTextSimple(
                        back,
                        L"...",
                        content.left +
                            (int)drawLen * charWidth,
                        y,
                        charWidth * 3 + S(4),
                        lineH,
                        g_muted,
                        g_mono,
                        DT_LEFT |
                            DT_SINGLELINE |
                            DT_NOPREFIX);
                }
            }

            y += lineH;
        }

        SelectObject(back, oldFont);

        if (needsScrollbar)
        {
            RECT track =
                GetPreviewScrollbarTrackRect(
                    layout);

            FillRoundRect(
                back,
                track,
                layout.scrollbarW / 2,
                MixColor(
                    g_bg,
                    g_text,
                    10));

            RECT thumb =
                GetPreviewScrollbarThumbRect(
                    layout);

            // Brightens on hover/drag, the same accent-hot feedback
            // every other draggable control in the app gives —
            // otherwise a thicker bar with no state change would
            // just look like a wider version of the same static
            // sliver, not an obviously grabbable control.
            FillRoundRect(
                back,
                thumb,
                layout.scrollbarW / 2,
                (g_previewScrollbarDragging ||
                 g_previewScrollbarHover)
                    ? MixColor(
                          g_accent,
                          RGB(255, 255, 255),
                          20)
                    : g_accent);
        }
    }
    else if (g_previewKind == PreviewKind::Image &&
             g_previewImage)
    {
        Gdiplus::Graphics graphics(back);

        graphics.SetInterpolationMode(
            Gdiplus::
                InterpolationModeHighQualityBicubic);

        graphics.SetSmoothingMode(
            Gdiplus::SmoothingModeAntiAlias);

        graphics.DrawImage(
            g_previewImage,
            (Gdiplus::REAL)pad,
            (Gdiplus::REAL)pad,
            (Gdiplus::REAL)
                g_previewImageRenderW,
            (Gdiplus::REAL)
                g_previewImageRenderH);
    }

    DrawRoundBorder(
        back,
        full,
        S(14),
        g_border);

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
// Toast paint
// ============================================================

static void PaintToast(
    HWND hwnd,
    HDC dc)
{
    RECT client{};

    GetClientRect(
        hwnd,
        &client);

    int width = client.right;
    int height = client.bottom;

    if (width <= 0 || height <= 0)
        return;

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

    RECT full = { 0, 0, width, height };

    FillRectColor(
        back,
        full,
        g_bg);

    const int pad = S(14);

    RECT titleRect =
    {
        pad,
        pad,
        width - pad,
        pad + S(20)
    };

    DrawTextSimple(
        back,
        g_toastTitle.c_str(),
        titleRect.left,
        titleRect.top,
        titleRect.right - titleRect.left,
        titleRect.bottom - titleRect.top,
        g_text,
        g_bold,
        DT_LEFT |
            DT_TOP |
            DT_NOPREFIX |
            DT_SINGLELINE |
            DT_END_ELLIPSIS);

    RECT detailRect =
    {
        pad,
        titleRect.bottom + S(4),
        width - pad,
        height - pad
    };

    DrawTextSimple(
        back,
        g_toastDetail.c_str(),
        detailRect.left,
        detailRect.top,
        detailRect.right - detailRect.left,
        detailRect.bottom - detailRect.top,
        g_muted,
        g_small,
        DT_LEFT |
            DT_TOP |
            DT_NOPREFIX |
            DT_WORDBREAK);

    DrawRoundBorder(
        back,
        full,
        S(14),
        g_border);

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

    // The window's actual client area, which widens to fit the
    // side results panel; distinct from "width" below, which
    // always stays the fixed main-column size so every existing
    // element (items, search box, power button...) keeps its
    // layout untouched by the panel appearing.
    int totalWidth =
        client.right;

    int width =
        MainColumnWidth();

    int height =
        client.bottom;

    HDC back =
        CreateCompatibleDC(dc);

    if (!back)
        return;

    HBITMAP bitmap =
        CreateCompatibleBitmap(
            dc,
            totalWidth,
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

    // Covers the whole canvas, including the side-panel area to
    // the right when it's showing — otherwise that region would
    // start out as whatever garbage was in the freshly allocated
    // bitmap. The rounded border below is scoped to just the main
    // column, though, so the two stay visually distinct cards.
    FillRectColor(
        back,
        client,
        g_bg);

    RECT mainRect =
    {
        0,
        0,
        width,
        height
    };

    // What's currently the opacity slider's target — including the
    // Start menu itself — gets its highlight from the separate
    // g_opacityHighlight overlay window instead (see
    // SetStartAsOpacityTarget), which draws a crisp, non-anti-
    // aliased outline rather than this rounded GDI+ stroke, so the
    // two read identically whether the target is this menu or some
    // other window entirely.
    DrawRoundBorder(
        back,
        mainRect,
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

        // Measuring text width up to N characters, used for the
        // selection highlight, the blinking caret, and click/drag
        // hit-testing (SearchCharIndexFromX) alike, so all three
        // always agree on where each character actually sits.
        HGDIOBJ measureOldFont =
            SelectObject(
                back,
                g_font);

        auto textWidthUpTo =
            [&](int n) -> int
        {
            SIZE sz{ 0, 0 };

            if (n > 0)
            {
                GetTextExtentPoint32W(
                    back,
                    g_searchText.c_str(),
                    n,
                    &sz);
            }

            return sz.cx;
        };

        if (HasSearchSelection())
        {
            int selStart, selEnd;

            GetSearchSelectionRange(
                selStart,
                selEnd);

            int hiLeft =
                S(46) +
                textWidthUpTo(
                    selStart);

            int hiRight =
                S(46) +
                textWidthUpTo(
                    selEnd);

            RECT highlight =
            {
                hiLeft,
                search.top + S(7),
                hiRight,
                search.top + S(41)
            };

            FillRectColor(
                back,
                highlight,
                MixColor(
                    searchBg,
                    g_accent,
                    45));
        }

        SelectObject(
            back,
            measureOldFont);

        RECT clearBtn =
            GetSearchClearButtonRect(
                search);

        DrawTextSimple(
            back,
            g_searchText.c_str(),
            S(46),
            search.top,
            clearBtn.left - S(6) -
                S(46),
            S(48),
            g_text,
            g_font);

        // Blinking text-entry caret, at whichever character
        // position it's actually sitting at now that the box
        // supports clicking/arrowing within the text, not just
        // always appending at the end.
        if (g_caretVisible &&
            !HasSearchSelection())
        {
            HGDIOBJ oldFont =
                SelectObject(
                    back,
                    g_font);

            SIZE textSize{ 0, 0 };

            if (g_searchCaretPos > 0)
            {
                GetTextExtentPoint32W(
                    back,
                    g_searchText.c_str(),
                    g_searchCaretPos,
                    &textSize);
            }

            SelectObject(
                back,
                oldFont);

            int caretX =
                S(46) +
                textSize.cx +
                (g_searchCaretPos ==
                         (int)g_searchText
                             .size()
                     ? S(3)
                     : 0);

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

        // Clear button — traditional placement, vertically centered
        // near the box's right edge. Only ever present while there's
        // something to clear, same as the box itself.
        if (g_searchClearHover)
        {
            FillRoundRect(
                back,
                clearBtn,
                S(5),
                MixColor(
                    searchBg,
                    g_accent,
                    35));
        }

        DrawClearGlyph(
            back,
            clearBtn,
            g_searchClearHover
                ? g_accent
                : g_muted);
    }

    // --------------------------------------------------------
    // Wildcard search results — a translucent, animated panel to
    // the right of the main column, matching the aesthetic of the
    // rest of the UI without affecting the main column's own
    // height. Rendered into its own buffer and alpha-blended in
    // as one unit so it fades/pops in together rather than each
    // row appearing separately.
    // --------------------------------------------------------

    if (!g_searchResults.empty())
    {
        RECT panel =
            GetSideResultsPanelRect(
                height);

        int panelW =
            panel.right - panel.left;

        int panelH =
            panel.bottom - panel.top;

        if (panelW > 0 &&
            panelH > 0)
        {
            HDC panelDc =
                CreateCompatibleDC(
                    back);

            if (panelDc)
            {
                // A rounded border drawn flush against its own
                // bitmap's edge gets half its 1px stroke clipped
                // outside the raster (GDI+ centers the stroke on
                // the path), leaving it invisible right where it
                // matters most. A small pad on every side gives
                // the stroke room to render in full; the AlphaBlend
                // below then draws that whole padded buffer at a
                // matching offset, so it lands in exactly the same
                // place as if there'd been no padding at all.
                const int pad = 2;

                HBITMAP panelBitmap =
                    CreateCompatibleBitmap(
                        back,
                        panelW + pad * 2,
                        panelH + pad * 2);

                if (panelBitmap)
                {
                    HGDIOBJ oldPanelBitmap =
                        SelectObject(
                            panelDc,
                            panelBitmap);

                    RECT fullBitmapRect =
                    {
                        0,
                        0,
                        panelW + pad * 2,
                        panelH + pad * 2
                    };

                    FillRectColor(
                        panelDc,
                        fullBitmapRect,
                        g_bg);

                    RECT panelLocal =
                    {
                        pad,
                        pad,
                        pad + panelW,
                        pad + panelH
                    };

                    DrawTile(
                        panelDc,
                        panelLocal,
                        S(10),
                        MixColor(
                            g_panel,
                            g_bg,
                            25),
                        g_border);

                    FocusTarget focusTarget =
                        ResolveFocus(
                            g_focusIndex);

                    int rowH =
                        SearchResultRowHeight();

                    int visibleRows =
                        SearchResultsVisibleRowCount(
                            height);

                    int firstRow =
                        g_searchResultsScroll;

                    int lastRow =
                        firstRow + visibleRows;

                    if (lastRow >
                        (int)g_searchResults.size())
                    {
                        lastRow =
                            (int)g_searchResults
                                .size();
                    }

                    for (int i = firstRow;
                         i < lastRow;
                         ++i)
                    {
                        RECT rowRect =
                            GetSearchResultRect(
                                height,
                                i);

                        OffsetRect(
                            &rowRect,
                            -panel.left + pad,
                            -panel.top + pad);

                        bool hot =
                            g_searchResultHover ==
                                (int)i ||
                            (focusTarget.kind ==
                                 FocusKind::SearchResult &&
                             focusTarget.index ==
                                 (int)i);

                        if (hot)
                        {
                            FillRoundRect(
                                panelDc,
                                rowRect,
                                S(6),
                                g_hot);
                        }

                        int iconSize =
                            rowH - S(12);

                        RECT iconRect =
                        {
                            rowRect.left + S(4),
                            rowRect.top +
                                (rowH - iconSize) / 2,
                            rowRect.left + S(4) +
                                iconSize,
                            rowRect.top +
                                (rowH - iconSize) / 2 +
                                iconSize
                        };

                        if (g_searchResults[i].icon)
                        {
                            DrawRealIcon(
                                panelDc,
                                g_searchResults[i]
                                    .icon,
                                iconRect.left,
                                iconRect.top,
                                iconSize);
                        }

                        const std::wstring& path =
                            g_searchResults[i].path;

                        std::wstring name;
                        std::wstring folder;

                        if (g_searchResults[i].kind ==
                            SearchResultKind::GodMode)
                        {
                            name = path;
                            folder = L"Control Panel";
                        }
                        else
                        {
                            size_t slash =
                                path.find_last_of(
                                    L'\\');

                            name =
                                slash ==
                                    std::wstring::npos
                                    ? path
                                    : path.substr(
                                          slash + 1);

                            folder =
                                slash ==
                                    std::wstring::npos
                                    ? L""
                                    : path.substr(
                                          0,
                                          slash);
                        }

                        int textX =
                            iconRect.right + S(8);

                        int textW =
                            rowRect.right -
                            textX -
                            S(4);

                        DrawTextSimple(
                            panelDc,
                            name.c_str(),
                            textX,
                            rowRect.top + S(3),
                            textW,
                            rowH / 2,
                            g_text,
                            g_font,
                            DT_LEFT |
                                DT_VCENTER |
                                DT_SINGLELINE |
                                DT_END_ELLIPSIS);

                        DrawTextSimple(
                            panelDc,
                            folder.c_str(),
                            textX,
                            rowRect.top + rowH / 2,
                            textW,
                            rowH / 2 - S(4),
                            g_muted,
                            g_small,
                            DT_LEFT |
                                DT_VCENTER |
                                DT_SINGLELINE |
                                DT_PATH_ELLIPSIS);
                    }

                    // Styled scrollbar — only when there are more
                    // results than fit, sized/positioned to show
                    // exactly how much of the list is visible.
                    if ((int)g_searchResults.size() >
                        visibleRows)
                    {
                        int trackTop =
                            pad + S(4);

                        int trackBottom =
                            pad + panelH - S(4);

                        int trackHeight =
                            trackBottom - trackTop;

                        int barW =
                            SearchResultsScrollbarWidth();

                        RECT track =
                        {
                            pad + panelW -
                                S(6) - barW,
                            trackTop,
                            pad + panelW -
                                S(6),
                            trackBottom
                        };

                        FillRoundRect(
                            panelDc,
                            track,
                            barW / 2,
                            MixColor(
                                g_bg,
                                g_text,
                                10));

                        int total =
                            (int)g_searchResults
                                .size();

                        int thumbH =
                            trackHeight *
                            visibleRows /
                            total;

                        if (thumbH < S(16))
                            thumbH = S(16);

                        if (thumbH > trackHeight)
                            thumbH = trackHeight;

                        int scrollRange =
                            total - visibleRows;

                        int thumbTravel =
                            trackHeight - thumbH;

                        int thumbTop =
                            trackTop +
                            (scrollRange > 0
                                 ? thumbTravel *
                                       g_searchResultsScroll /
                                       scrollRange
                                 : 0);

                        RECT thumb =
                        {
                            track.left,
                            thumbTop,
                            track.right,
                            thumbTop + thumbH
                        };

                        FillRoundRect(
                            panelDc,
                            thumb,
                            barW / 2,
                            g_accent);
                    }

                    BLENDFUNCTION blend{};

                    blend.BlendOp =
                        AC_SRC_OVER;

                    blend.SourceConstantAlpha =
                        (BYTE)(
                            g_searchResultsAnim *
                            255.0f);

                    AlphaBlend(
                        back,
                        panel.left - pad,
                        panel.top - pad,
                        panelW + pad * 2,
                        panelH + pad * 2,
                        panelDc,
                        0,
                        0,
                        panelW + pad * 2,
                        panelH + pad * 2,
                        blend);

                    SelectObject(
                        panelDc,
                        oldPanelBitmap);

                    DeleteObject(
                        panelBitmap);
                }

                DeleteDC(
                    panelDc);
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

        // A disclosure arrow, not a launch icon — Control Panel is
        // the one row that opens the God Mode catalog in the side
        // panel instead of launching anything itself (see
        // ToggleControlPanelBrowse). Points down while that panel
        // is open, the same flip a classic expand/collapse triangle
        // gives you.
        if (wcscmp(
                g_items[i].command,
                L"control panel") == 0)
        {
            DrawTextSimple(
                back,
                g_controlPanelBrowsing
                    ? L"▾"
                    : L"▸",
                width - S(26),
                y,
                S(18),
                row,
                selected
                    ? g_accentText
                    : g_muted,
                g_font,
                DT_CENTER |
                    DT_VCENTER |
                    DT_SINGLELINE |
                    DT_NOPREFIX);
        }
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
        totalWidth,
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

            if (g_searchDragging)
            {
                g_searchCaretPos =
                    SearchCharIndexFromX(
                        hwnd,
                        x - S(46));

                g_caretVisible = true;

                InvalidateRect(
                    hwnd,
                    nullptr,
                    FALSE);

                return 0;
            }

            RECT client{};

            GetClientRect(
                hwnd,
                &client);

            // The main column's fixed width, not however wide the
            // window actually is right now — see the note in
            // PaintStart. The side panel's own hit-testing below
            // uses client.bottom only, so this doesn't affect it.
            client.right =
                MainColumnWidth();

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

            bool oldClearHover =
                g_searchClearHover;

            RECT clearBtnHit =
                GetSearchClearButtonRect(
                    search);

            g_searchClearHover =
                !g_searchText.empty() &&
                PtInRect(
                    &clearBtnHit,
                    point) ==
                    TRUE;

            if (oldClearHover !=
                g_searchClearHover)
            {
                InvalidateRect(
                    hwnd,
                    nullptr,
                    FALSE);
            }

            // Opacity highlight — only ever visible while actually
            // hovering (or, per the drag branch at the very top of
            // this handler, dragging) the slider, never just
            // because the menu happens to be open.
            {
                RECT sliderForHover =
                    GetOpacitySliderRect(
                        client.right,
                        client.bottom);

                RECT sliderHoverHit =
                {
                    sliderForHover.left -
                        S(4),
                    sliderForHover.top -
                        S(10),
                    sliderForHover.right +
                        S(4),
                    sliderForHover.bottom +
                        S(10)
                };

                bool nowHoveringSlider =
                    PtInRect(
                        &sliderHoverHit,
                        point) == TRUE;

                if (nowHoveringSlider !=
                    g_sliderHover)
                {
                    g_sliderHover =
                        nowHoveringSlider;

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

            int oldSearchResultHover =
                g_searchResultHover;

            g_searchResultHover = -1;

            if (!g_searchResults.empty())
            {
                RECT panel =
                    GetSideResultsPanelRect(
                        client.bottom);

                if (PtInRect(
                        &panel,
                        point))
                {
                    int rowH =
                        SearchResultRowHeight();

                    int rel =
                        y -
                        (panel.top +
                         S(4));

                    int visibleIdx =
                        rel / rowH;

                    int idx =
                        visibleIdx +
                        g_searchResultsScroll;

                    if (visibleIdx >= 0 &&
                        visibleIdx <
                            SearchResultsVisibleRowCount(
                                client.bottom) &&
                        idx <
                            (int)g_searchResults
                                .size())
                    {
                        g_searchResultHover =
                            idx;

                        g_hover = -1;
                        g_powerHover = -1;
                    }
                }
            }

            if (oldSearchResultHover !=
                g_searchResultHover)
            {
                InvalidateRect(
                    hwnd,
                    nullptr,
                    FALSE);

                // Restart the quick-look timer on every hover
                // change rather than showing/loading immediately —
                // otherwise just sweeping the mouse down the list
                // would load a preview for every row it passed.
                if (g_previewTimer)
                {
                    KillTimer(
                        hwnd,
                        TIMER_PREVIEW_HOVER);

                    g_previewTimer = 0;
                }

                if (g_searchResultHover >= 0)
                {
                    // Snap back to fully visible immediately —
                    // even before the timer below actually reloads
                    // new content — so landing back on a row never
                    // looks like it's still fading away underneath
                    // the cursor.
                    CancelPreviewFadeOut();

                    g_previewHoverIndex =
                        g_searchResultHover;

                    g_previewTimer =
                        SetTimer(
                            hwnd,
                            TIMER_PREVIEW_HOVER,
                            150,
                            nullptr);
                }
                else
                {
                    BeginPreviewFadeOut();
                }
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
            g_searchResultHover = -1;
            g_searchClearHover = false;
            g_mouseTracking = false;

            SetSearchHover(
                hwnd,
                false);

            if (g_sliderHover &&
                !g_sliderDragging)
            {
                g_sliderHover = false;
                HideOpacityHighlight();
            }

            BeginPreviewFadeOut();

            InvalidateRect(
                hwnd,
                nullptr,
                FALSE);

            return 0;
        }

        case WM_MOUSEWHEEL:
        {
            // Wheel messages arrive with screen, not client,
            // coordinates.
            POINT screenPoint
            {
                (int)(short)LOWORD(lp),
                (int)(short)HIWORD(lp)
            };

            ScreenToClient(
                hwnd,
                &screenPoint);

            RECT client{};

            GetClientRect(
                hwnd,
                &client);

            // The main column's fixed width, not however wide the
            // window actually is right now — see the note in
            // PaintStart. The side panel's own hit-testing above
            // uses client.bottom only, so this doesn't affect it.
            client.right =
                MainColumnWidth();

            if (!g_searchResults.empty())
            {
                RECT panel =
                    GetSideResultsPanelRect(
                        client.bottom);

                if (PtInRect(
                        &panel,
                        screenPoint))
                {
                    int delta =
                        (short)HIWORD(wp);

                    // Three rows per notch — enough to feel
                    // responsive without overshooting past what's
                    // actually in view.
                    g_searchResultsScroll -=
                        (delta / WHEEL_DELTA) * 3;

                    ClampSearchResultsScroll(
                        client.bottom);

                    // Scrolling shifts which file sits under a
                    // stationary cursor without a mouse-move event
                    // to catch it, so let go of the preview rather
                    // than risk it going stale against the wrong
                    // row — same graceful fade as any other hover
                    // loss, not an instant snap.
                    g_searchResultHover = -1;
                    BeginPreviewFadeOut();

                    InvalidateRect(
                        hwnd,
                        nullptr,
                        FALSE);

                    return 0;
                }
            }

            // Opacity slider — same inflated grab area as the drag
            // hit-test below, so hovering to scroll feels exactly as
            // forgiving as hovering to drag.
            {
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

                if (PtInRect(
                        &sliderHit,
                        screenPoint))
                {
                    int delta =
                        (short)HIWORD(wp);

                    CycleOpacityTarget(
                        delta > 0 ? 1 : -1);

                    return 0;
                }
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

            // The main column's fixed width, not however wide the
            // window actually is right now — see the note in
            // PaintStart. The side panel's own hit-testing below
            // uses client.bottom only, so this doesn't affect it.
            client.right =
                MainColumnWidth();

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

            // Search box — click to position the caret, and start
            // tracking a drag in case this turns into a
            // click-and-drag selection.
            if (!g_searchText.empty())
            {
                RECT search =
                    GetSearchRect(
                        client.right);

                POINT searchPoint
                {
                    x,
                    y
                };

                RECT clearBtn =
                    GetSearchClearButtonRect(
                        search);

                if (PtInRect(
                        &clearBtn,
                        searchPoint))
                {
                    ClearSearchBox(
                        hwnd);

                    return 0;
                }

                if (PtInRect(
                        &search,
                        searchPoint))
                {
                    int index =
                        SearchCharIndexFromX(
                            hwnd,
                            x - S(46));

                    g_searchCaretPos =
                        index;

                    g_searchSelAnchor =
                        index;

                    g_searchDragging = true;

                    g_caretVisible = true;

                    SetCapture(
                        hwnd);

                    InvalidateRect(
                        hwnd,
                        nullptr,
                        FALSE);

                    return 0;
                }
            }

            // Wildcard search result rows.
            if (!g_searchResults.empty())
            {
                RECT panel =
                    GetSideResultsPanelRect(
                        client.bottom);

                POINT resultPoint
                {
                    x,
                    y
                };

                if (PtInRect(
                        &panel,
                        resultPoint))
                {
                    int rowH =
                        SearchResultRowHeight();

                    int rel =
                        y -
                        (panel.top +
                         S(4));

                    int visibleIdx =
                        rel / rowH;

                    int idx =
                        visibleIdx +
                        g_searchResultsScroll;

                    if (visibleIdx >= 0 &&
                        visibleIdx <
                            SearchResultsVisibleRowCount(
                                client.bottom) &&
                        idx <
                            (int)g_searchResults
                                .size())
                    {
                        LaunchSearchResult(
                            idx);

                        return 0;
                    }
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
                g_sliderHover = true;

                ResumeOpacityHighlight();

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

        case WM_LBUTTONDBLCLK:
        {
            int x =
                (int)(short)LOWORD(lp);

            int y =
                (int)(short)HIWORD(lp);

            RECT client{};

            GetClientRect(
                hwnd,
                &client);

            client.right =
                MainColumnWidth();

            if (!g_searchText.empty())
            {
                RECT search =
                    GetSearchRect(
                        client.right);

                POINT searchPoint
                {
                    x,
                    y
                };

                RECT clearBtn =
                    GetSearchClearButtonRect(
                        search);

                if (PtInRect(
                        &clearBtn,
                        searchPoint))
                {
                    ClearSearchBox(
                        hwnd);

                    return 0;
                }

                if (PtInRect(
                        &search,
                        searchPoint))
                {
                    SearchSelectAll(
                        hwnd);

                    return 0;
                }
            }

            // Anywhere else, the double-click's first half was
            // already an ordinary click (Windows delivers DOWN,
            // UP, DBLCLK, UP) — treat the second half the same way
            // instead of silently swallowing it.
            return StartProc(
                hwnd,
                WM_LBUTTONDOWN,
                wp,
                lp);
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

            client.right =
                MainColumnWidth();

            // Wildcard search result rows — right-click opens the
            // file's containing folder (with it pre-selected)
            // instead of launching the file itself.
            if (!g_searchResults.empty())
            {
                RECT panel =
                    GetSideResultsPanelRect(
                        client.bottom);

                POINT resultPoint
                {
                    x,
                    y
                };

                if (PtInRect(
                        &panel,
                        resultPoint))
                {
                    int rowH =
                        SearchResultRowHeight();

                    int rel =
                        y -
                        (panel.top +
                         S(4));

                    int visibleIdx =
                        rel / rowH;

                    int idx =
                        visibleIdx +
                        g_searchResultsScroll;

                    if (visibleIdx >= 0 &&
                        visibleIdx <
                            SearchResultsVisibleRowCount(
                                client.bottom) &&
                        idx <
                            (int)g_searchResults
                                .size())
                    {
                        OpenSearchResultFolder(
                            idx);

                        return 0;
                    }
                }
            }

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

            bool searchActiveForKeys =
                g_focusIndex < 0 ||
                ResolveFocus(
                    g_focusIndex)
                        .kind ==
                    FocusKind::Search;

            bool shiftHeld =
                (GetKeyState(
                     VK_SHIFT) &
                 0x8000) != 0;

            bool ctrlHeld =
                (GetKeyState(
                     VK_CONTROL) &
                 0x8000) != 0;

            if (searchActiveForKeys &&
                ctrlHeld &&
                (wp == 'A' ||
                 wp == 'a'))
            {
                SearchSelectAll(hwnd);
                return 0;
            }

            if (searchActiveForKeys &&
                ctrlHeld &&
                (wp == 'C' ||
                 wp == 'c') &&
                HasSearchSelection())
            {
                SearchCopySelection(hwnd);
                return 0;
            }

            if (searchActiveForKeys &&
                (wp == VK_LEFT ||
                 wp == VK_RIGHT ||
                 wp == VK_HOME ||
                 wp == VK_END))
            {
                SearchMoveCaret(
                    hwnd,
                    (DWORD)wp,
                    shiftHeld);

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
                SearchBackspace(hwnd);
                return 0;
            }

            if (wp == VK_DELETE &&
                searchActiveForKeys)
            {
                SearchDeleteForward(hwnd);
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
                SearchInsertChar(
                    hwnd,
                    c);

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

            if (wp == TIMER_SEARCH_RESULTS_ANIM)
            {
                StepSearchResultsAnim(
                    hwnd);

                return 0;
            }

            if (wp == TIMER_PREVIEW_HOVER)
            {
                KillTimer(
                    hwnd,
                    TIMER_PREVIEW_HOVER);

                g_previewTimer = 0;

                if (g_searchResultHover >= 0 &&
                    g_searchResultHover ==
                        g_previewHoverIndex)
                {
                    ShowPreviewForResult(
                        g_searchResultHover);
                }

                return 0;
            }

            if (wp == TIMER_PREVIEW_FADE)
            {
                DWORD elapsed =
                    GetTickCount() -
                    g_previewFadeStartTick;

                if (elapsed <
                    PREVIEW_FADE_GRACE_MS)
                {
                    return 0;
                }

                DWORD fadeElapsed =
                    elapsed -
                    PREVIEW_FADE_GRACE_MS;

                if (fadeElapsed >=
                    PREVIEW_FADE_DURATION_MS)
                {
                    KillTimer(
                        hwnd,
                        TIMER_PREVIEW_FADE);

                    g_previewFadeTimer = 0;

                    HidePreview();

                    return 0;
                }

                g_previewAlpha =
                    1.0f -
                    (float)fadeElapsed /
                        (float)
                            PREVIEW_FADE_DURATION_MS;

                ApplyPreviewAlpha();

                return 0;
            }

            if (wp == TIMER_TOAST)
            {
                DWORD elapsed =
                    GetTickCount() -
                    g_toastStartTick;

                if (elapsed <
                    TOAST_HOLD_MS)
                {
                    return 0;
                }

                DWORD fadeElapsed =
                    elapsed -
                    TOAST_HOLD_MS;

                if (fadeElapsed >=
                    TOAST_FADE_MS)
                {
                    KillTimer(
                        hwnd,
                        TIMER_TOAST);

                    g_toastTimer = 0;

                    HideToast();

                    return 0;
                }

                g_toastAlpha =
                    1.0f -
                    (float)fadeElapsed /
                        (float)
                            TOAST_FADE_MS;

                if (g_toast)
                {
                    SetLayeredWindowAttributes(
                        g_toast,
                        0,
                        (BYTE)(
                            TOAST_BASE_ALPHA *
                            g_toastAlpha),
                        LWA_ALPHA);
                }

                return 0;
            }

            if (wp == TIMER_OPACITY_TRACK)
            {
                if (!g_opacityHighlightTarget ||
                    !IsWindow(
                        g_opacityHighlightTarget) ||
                    !IsWindowVisible(
                        g_opacityHighlightTarget))
                {
                    // The targeted window closed, got hidden, or
                    // minimized out from under us — fall back to
                    // the Start menu rather than keep tracking a
                    // window that's no longer really there.
                    g_opacityIndex = 0;

                    g_windowAlpha =
                        g_startOwnAlpha;

                    SetStartAsOpacityTarget();

                    InvalidateRect(
                        hwnd,
                        nullptr,
                        FALSE);

                    return 0;
                }

                RepositionOpacityHighlight(
                    g_opacityHighlightTarget);

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
// Preview window
// ============================================================
//
// A second, separate popup rather than part of g_start: it needs to
// sit in the work area's opposite corner, entirely outside the
// menu's own bounds. WS_EX_NOACTIVATE keeps it from ever stealing
// focus or becoming the foreground window even though it now takes
// real mouse input — moving the mouse into it cancels any fade-out
// in progress (see CancelPreviewFadeOut) and lets the wheel scroll
// a text preview, without disturbing whatever actually has focus.

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

        case WM_MOUSEMOVE:
        {
            CancelPreviewFadeOut();

            if (!g_previewMouseTracking)
            {
                TRACKMOUSEEVENT tme{};

                tme.cbSize = sizeof(tme);
                tme.dwFlags = TME_LEAVE;
                tme.hwndTrack = hwnd;

                if (TrackMouseEvent(&tme))
                    g_previewMouseTracking = true;
            }

            int mouseX = (short)LOWORD(lp);
            int mouseY = (short)HIWORD(lp);

            if (g_previewScrollbarDragging &&
                g_previewKind == PreviewKind::Text)
            {
                PreviewTextLayout layout =
                    ComputePreviewTextLayout(hwnd);

                SetPreviewScrollFromThumbTop(
                    layout,
                    mouseY -
                        g_previewScrollbarDragGrabOffset);

                InvalidateRect(
                    hwnd,
                    nullptr,
                    FALSE);
            }
            else if (g_previewSelecting &&
                     g_previewKind == PreviewKind::Text)
            {
                PreviewTextLayout layout =
                    ComputePreviewTextLayout(hwnd);

                g_previewSelCaret =
                    PreviewOffsetFromPoint(
                        layout,
                        mouseX,
                        mouseY);

                g_previewHasSelection =
                    g_previewSelCaret !=
                    g_previewSelAnchor;

                InvalidateRect(
                    hwnd,
                    nullptr,
                    FALSE);
            }
            else if (g_previewKind == PreviewKind::Text)
            {
                PreviewTextLayout layout =
                    ComputePreviewTextLayout(hwnd);

                POINT pt = { mouseX, mouseY };

                bool overScrollbar =
                    layout.needsScrollbar &&
                    PtInRect(
                        &GetPreviewScrollbarHitRect(
                            layout),
                        pt) != FALSE;

                if (overScrollbar !=
                    g_previewScrollbarHover)
                {
                    g_previewScrollbarHover =
                        overScrollbar;

                    InvalidateRect(
                        hwnd,
                        nullptr,
                        FALSE);
                }
            }

            return 0;
        }

        case WM_MOUSELEAVE:
        {
            g_previewMouseTracking = false;
            g_previewScrollbarHover = false;

            BeginPreviewFadeOut();

            return 0;
        }

        case WM_LBUTTONDOWN:
        {
            if (g_previewKind == PreviewKind::Text)
            {
                PreviewTextLayout layout =
                    ComputePreviewTextLayout(hwnd);

                POINT pt =
                {
                    (short)LOWORD(lp),
                    (short)HIWORD(lp)
                };

                if (layout.needsScrollbar &&
                    PtInRect(
                        &GetPreviewScrollbarHitRect(
                            layout),
                        pt))
                {
                    RECT thumb =
                        GetPreviewScrollbarThumbRect(
                            layout);

                    int thumbH =
                        thumb.bottom - thumb.top;

                    if (PtInRect(&thumb, pt))
                    {
                        g_previewScrollbarDragGrabOffset =
                            pt.y - thumb.top;
                    }
                    else
                    {
                        // Clicked the track itself, not the thumb —
                        // jump straight there instead of requiring a
                        // separate drag to reach it, then keep the
                        // grab centered on the thumb so an immediate
                        // drag continues smoothly from here.
                        SetPreviewScrollFromThumbTop(
                            layout,
                            pt.y - thumbH / 2);

                        g_previewScrollbarDragGrabOffset =
                            thumbH / 2;
                    }

                    g_previewScrollbarDragging = true;

                    SetCapture(hwnd);

                    InvalidateRect(
                        hwnd,
                        nullptr,
                        FALSE);

                    return 0;
                }

                size_t offset =
                    PreviewOffsetFromPoint(
                        layout,
                        pt.x,
                        pt.y);

                g_previewSelAnchor = offset;
                g_previewSelCaret = offset;
                g_previewHasSelection = false;
                g_previewSelecting = true;

                SetCapture(hwnd);

                InvalidateRect(
                    hwnd,
                    nullptr,
                    FALSE);
            }

            return 0;
        }

        case WM_LBUTTONUP:
        {
            if (g_previewScrollbarDragging)
            {
                g_previewScrollbarDragging = false;
                ReleaseCapture();

                InvalidateRect(
                    hwnd,
                    nullptr,
                    FALSE);
            }
            else if (g_previewSelecting)
            {
                g_previewSelecting = false;
                ReleaseCapture();
            }

            return 0;
        }

        case WM_CAPTURECHANGED:
        {
            g_previewSelecting = false;
            g_previewScrollbarDragging = false;

            return 0;
        }

        case WM_MOUSEWHEEL:
        {
            if (g_previewKind ==
                PreviewKind::Text)
            {
                int delta =
                    (short)HIWORD(wp);

                g_previewTextScroll -=
                    (delta / WHEEL_DELTA) *
                    3;

                if (g_previewTextScroll < 0)
                    g_previewTextScroll = 0;

                // The upper bound depends on the line count, which
                // PaintPreview already has to compute to lay out
                // the text — it re-clamps there rather than this
                // handler duplicating that work.
                InvalidateRect(
                    hwnd,
                    nullptr,
                    FALSE);
            }

            return 0;
        }

        case WM_PAINT:
        {
            PAINTSTRUCT ps{};

            HDC dc =
                BeginPaint(
                    hwnd,
                    &ps);

            PaintPreview(
                hwnd,
                dc);

            EndPaint(
                hwnd,
                &ps);

            return 0;
        }
    }

    return DefWindowProcW(
        hwnd,
        msg,
        wp,
        lp);
}

static bool CreatePreviewWindow(
    HINSTANCE instance)
{
    WNDCLASSEXW wc{};

    wc.cbSize =
        sizeof(wc);

    wc.style =
        CS_HREDRAW |
        CS_VREDRAW;

    wc.lpfnWndProc =
        PreviewProc;

    wc.hInstance =
        instance;

    wc.hCursor =
        LoadCursorW(
            nullptr,
            IDC_ARROW);

    wc.lpszClassName =
        PREVIEW_CLASS;

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

    g_preview =
        CreateWindowExW(
            WS_EX_TOOLWINDOW |
                WS_EX_TOPMOST |
                WS_EX_LAYERED |
                WS_EX_NOACTIVATE,
            PREVIEW_CLASS,
            L"ClassicShell Preview",
            WS_POPUP,
            0,
            0,
            10,
            10,
            nullptr,
            nullptr,
            instance,
            nullptr);

    if (!g_preview)
        return false;

    SetLayeredWindowAttributes(
        g_preview,
        0,
        PREVIEW_BASE_ALPHA,
        LWA_ALPHA);

    ApplyWindowRounding(
        g_preview);

    return true;
}

// ============================================================
// Toast window
// ============================================================
//
// A third, separate popup, same reasoning as the preview window —
// except a toast is purely informational, so unlike the preview it
// stays click-through (HTTRANSPARENT) for its entire lifetime.

static LRESULT CALLBACK ToastProc(
    HWND hwnd,
    UINT msg,
    WPARAM wp,
    LPARAM lp)
{
    switch (msg)
    {
        case WM_NCHITTEST:
            return HTTRANSPARENT;

        case WM_ERASEBKGND:
            return 1;

        case WM_PAINT:
        {
            PAINTSTRUCT ps{};

            HDC dc =
                BeginPaint(
                    hwnd,
                    &ps);

            PaintToast(
                hwnd,
                dc);

            EndPaint(
                hwnd,
                &ps);

            return 0;
        }
    }

    return DefWindowProcW(
        hwnd,
        msg,
        wp,
        lp);
}

static bool CreateToastWindow(
    HINSTANCE instance)
{
    WNDCLASSEXW wc{};

    wc.cbSize =
        sizeof(wc);

    wc.style =
        CS_HREDRAW |
        CS_VREDRAW;

    wc.lpfnWndProc =
        ToastProc;

    wc.hInstance =
        instance;

    wc.hCursor =
        LoadCursorW(
            nullptr,
            IDC_ARROW);

    wc.lpszClassName =
        TOAST_CLASS;

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

    g_toast =
        CreateWindowExW(
            WS_EX_TOOLWINDOW |
                WS_EX_TOPMOST |
                WS_EX_LAYERED |
                WS_EX_NOACTIVATE,
            TOAST_CLASS,
            L"ClassicShell Toast",
            WS_POPUP,
            0,
            0,
            10,
            10,
            nullptr,
            nullptr,
            instance,
            nullptr);

    if (!g_toast)
        return false;

    SetLayeredWindowAttributes(
        g_toast,
        0,
        TOAST_BASE_ALPHA,
        LWA_ALPHA);

    ApplyWindowRounding(
        g_toast);

    return true;
}

// ============================================================
// Opacity highlight window
// ============================================================
//
// A fourth auxiliary popup: a true per-pixel-alpha outline (see
// RenderOpacityHighlight, which draws and presents it directly via
// UpdateLayeredWindow — this window never receives or needs a real
// WM_PAINT) that sits over whatever window the opacity slider
// currently targets, including the Start menu itself, matching
// that window's own DWM-rounded corners exactly (ApplyWindowRounding
// below gives this window the identical treatment).

static LRESULT CALLBACK OpacityHighlightProc(
    HWND hwnd,
    UINT msg,
    WPARAM wp,
    LPARAM lp)
{
    switch (msg)
    {
        case WM_NCHITTEST:
            return HTTRANSPARENT;

        case WM_ERASEBKGND:
            return 1;
    }

    return DefWindowProcW(
        hwnd,
        msg,
        wp,
        lp);
}

static bool CreateOpacityHighlightWindow(
    HINSTANCE instance)
{
    WNDCLASSEXW wc{};

    wc.cbSize =
        sizeof(wc);

    wc.style =
        CS_HREDRAW |
        CS_VREDRAW;

    wc.lpfnWndProc =
        OpacityHighlightProc;

    wc.hInstance =
        instance;

    wc.hCursor =
        LoadCursorW(
            nullptr,
            IDC_ARROW);

    wc.lpszClassName =
        OPACITY_HIGHLIGHT_CLASS;

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

    g_opacityHighlight =
        CreateWindowExW(
            WS_EX_TOOLWINDOW |
                WS_EX_TOPMOST |
                WS_EX_LAYERED |
                WS_EX_NOACTIVATE,
            OPACITY_HIGHLIGHT_CLASS,
            L"ClassicShell Opacity Highlight",
            WS_POPUP,
            0,
            0,
            10,
            10,
            nullptr,
            nullptr,
            instance,
            nullptr);

    if (!g_opacityHighlight)
        return false;

    // No SetLayeredWindowAttributes call here on purpose — this
    // window's actual pixels are presented via UpdateLayeredWindow
    // instead (see RenderOpacityHighlight), which owns the layered
    // surface directly; mixing the two APIs on the same window
    // isn't reliable.

    // Same DWM rounding g_start itself gets, rather than trying to
    // hand-match its corner radius with our own S(18)-based math —
    // Windows 11's actual corner radius for DWMWCP_ROUND isn't a
    // value this app controls or can query, so the only way to line
    // up exactly, at any scale, is to let DWM round both windows
    // with the identical request and let it worry about the number.
    ApplyWindowRounding(
        g_opacityHighlight);

    return true;
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

    // Deliberately not shown yet here — the highlight only appears
    // while actually hovering or dragging the opacity slider (see
    // ResumeOpacityHighlight/HideOpacityHighlight in StartProc's
    // WM_MOUSEMOVE and WM_LBUTTONDOWN), not just because the menu
    // happens to be open.

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

    // GetKeyboardState() only reflects modifier state as of the
    // last message this thread's own queue has processed. A
    // low-level keyboard hook runs ahead of that pump, so it can
    // see a stale, not-yet-updated Shift bit and resolve a shifted
    // key (e.g. "!") as its unshifted character ("1") instead —
    // this is the intermittent "symbols don't type" bug.
    // GetAsyncKeyState() reports the real physical state right
    // now regardless of queue timing, so use that for the held
    // modifiers instead of trusting the queue-synced snapshot.
    keyboard[VK_SHIFT] =
        (GetAsyncKeyState(
             VK_SHIFT) &
         0x8000)
            ? 0x80
            : 0x00;

    keyboard[VK_CONTROL] =
        (GetAsyncKeyState(
             VK_CONTROL) &
         0x8000)
            ? 0x80
            : 0x00;

    keyboard[VK_MENU] =
        (GetAsyncKeyState(
             VK_MENU) &
         0x8000)
            ? 0x80
            : 0x00;

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
    // Windows key
    // --------------------------------------------------------

    if (vk == VK_LWIN ||
        vk == VK_RWIN)
    {
        if (down)
        {
            g_winKeyTracker.OnWinKeyDown(vk);

            return CallNextHookEx(
                g_keyboardHook,
                code,
                wp,
                lp);
        }

        if (up)
        {
            WinKeyAction action =
                g_winKeyTracker.OnWinKeyUp(vk);

            if (action == WinKeyAction::Toggle)
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

        bool ctrlHeldHook =
            (GetKeyState(
                 VK_CONTROL) &
             0x8000) != 0;

        bool shiftHeldHook =
            (GetKeyState(
                 VK_SHIFT) &
             0x8000) != 0;

        if (ctrlHeldHook &&
            (vk == 'A' ||
             vk == 'a'))
        {
            SearchSelectAll(g_start);
            return 1;
        }

        if (ctrlHeldHook &&
            (vk == 'C' ||
             vk == 'c') &&
            g_previewHasSelection)
        {
            CopyPreviewSelection();
            return 1;
        }

        if (ctrlHeldHook &&
            (vk == 'C' ||
             vk == 'c') &&
            HasSearchSelection())
        {
            SearchCopySelection(g_start);
            return 1;
        }

        if (vk == VK_LEFT ||
            vk == VK_RIGHT ||
            vk == VK_HOME ||
            vk == VK_END)
        {
            SearchMoveCaret(
                g_start,
                vk,
                shiftHeldHook);

            return 1;
        }

        if (vk == VK_BACK)
        {
            SearchBackspace(g_start);
            return 1;
        }

        if (vk == VK_DELETE)
        {
            SearchDeleteForward(g_start);
            return 1;
        }

        if (IsPrintableKey(vk))
        {
            wchar_t c =
                VirtualKeyToChar(vk);

            if (c >= 32)
            {
                SearchInsertChar(
                    g_start,
                    c);

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
            // the menu while it's open dismisses it — except inside
            // the preview panel, which sits well outside the menu's
            // own bounds by design. A click there should just do
            // nothing (the panel itself doesn't handle clicks)
            // rather than read as "clicked away" and close the menu
            // out from under it.
            if (g_startVisible)
            {
                RECT r =
                    GetStartRect();

                bool insidePreview = false;

                if (g_preview &&
                    IsWindowVisible(
                        g_preview))
                {
                    RECT previewRect{};

                    if (GetWindowRect(
                            g_preview,
                            &previewRect))
                    {
                        insidePreview =
                            PtInRect(
                                &previewRect,
                                point) !=
                            FALSE;
                    }
                }

                if (!PtInRect(
                        &r,
                        point) &&
                    !insidePreview)
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

    StartBackgroundIndexing();
    StartGodModeIndexing();

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

    // Not fatal if this fails — the app is fully usable without
    // hover previews, so it just silently goes without the feature
    // rather than refusing to start over it.
    CreatePreviewWindow(
        instance);

    CreateToastWindow(
        instance);

    CreateOpacityHighlightWindow(
        instance);

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

    // Not fatal if this fails — the opacity highlight just falls
    // back to whatever the periodic position-tracking timer alone
    // manages, rather than losing the whole feature over it.
    g_foregroundEventHook =
        SetWinEventHook(
            EVENT_SYSTEM_FOREGROUND,
            EVENT_SYSTEM_FOREGROUND,
            nullptr,
            OpacityForegroundEventProc,
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

    if (g_mouseHook)
    {
        UnhookWindowsHookEx(
            g_mouseHook);

        g_mouseHook = nullptr;
    }

    if (g_foregroundEventHook)
    {
        UnhookWinEvent(
            g_foregroundEventHook);

        g_foregroundEventHook = nullptr;
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

    if (g_preview)
    {
        HidePreview();

        DestroyWindow(
            g_preview);

        g_preview = nullptr;
    }

    if (g_toast)
    {
        HideToast();

        DestroyWindow(
            g_toast);

        g_toast = nullptr;
    }

    // Deliberately just stops tracking (hides our own highlight
    // window) rather than undoing anything — whatever opacity the
    // user set on another window is meant to outlive ClassicShell
    // itself, the same as any standalone transparency tool.
    StopOpacityHighlight();

    if (g_opacityHighlight)
    {
        DestroyWindow(
            g_opacityHighlight);

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
