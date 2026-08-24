// starthook.h — pure, testable logic behind ClassicShell's interception of
// the physical Windows key and the taskbar Start button.
//
// This is deliberately kept free of SetWindowsHookEx/CallNextHookEx and any
// other live-hook plumbing, so it can run the same in the shipped app and in
// a plain console unit test: WinKeyTracker and StartButtonMouseTracker only
// track state and hand back a decision, and IsStartButtonHit only does
// rectangle math.
#pragma once

#include <windows.h>
#include <shlwapi.h>
#include <string>
#include <cmath>
#include <cwctype>

// ============================================================
// Windows-key tap tracking
// ============================================================

enum class WinKeyAction
{
    // Nothing for the caller to do.
    None,

    // A standalone tap (down, then up, with nothing else pressed in
    // between) just completed — open/close the Start menu.
    Toggle,

    // Some other key went down while Win was held — this is a native
    // shortcut (Win+D, Win+E, ...), not a tap. If the menu is open,
    // the caller should close it; the key itself is not ours to touch.
    Close,
};

// Mirrors the state machine ClassicShell's keyboard hook uses to tell a
// standalone Windows-key tap apart from the first key of a Win+ shortcut.
// The hook is expected to have already filtered out injected key events
// (LLKHF_INJECTED) before ever reaching this class — see KeyboardProc's
// comment in classicshell.cpp for why injected events must never affect
// this bookkeeping.
class WinKeyTracker
{
public:
    // vk must be VK_LWIN or VK_RWIN.
    WinKeyAction OnWinKeyDown(DWORD vk)
    {
        if (vk == VK_LWIN)
            m_lwinDown = true;
        else if (vk == VK_RWIN)
            m_rwinDown = true;

        m_winCombo = false;

        return WinKeyAction::None;
    }

    // vk must be VK_LWIN or VK_RWIN.
    WinKeyAction OnWinKeyUp(DWORD vk)
    {
        bool standalone = false;

        if (vk == VK_LWIN)
        {
            standalone = m_lwinDown && !m_winCombo;
            m_lwinDown = false;
        }
        else if (vk == VK_RWIN)
        {
            standalone = m_rwinDown && !m_winCombo;
            m_rwinDown = false;
        }

        if (!m_lwinDown && !m_rwinDown)
            m_winCombo = false;

        return standalone ? WinKeyAction::Toggle : WinKeyAction::None;
    }

    // Called for any key-down that is not itself VK_LWIN/VK_RWIN.
    WinKeyAction OnOtherKeyDown()
    {
        if (!m_lwinDown && !m_rwinDown)
            return WinKeyAction::None;

        m_winCombo = true;

        return WinKeyAction::Close;
    }

private:
    bool m_lwinDown = false;
    bool m_rwinDown = false;
    bool m_winCombo = false;
};

// ============================================================
// Taskbar Start-button hit testing
// ============================================================

// Pure rectangle math behind IsStartButtonClick: true if `point` lands on
// either the classic bottom-left Start button or a centered-taskbar Start
// button, given the taskbar's own screen rect and the (already DPI/scale
// adjusted) button dimensions.
inline bool IsStartButtonHit(
    POINT point,
    RECT taskbarRect,
    int leftButtonWidth,
    int centerButtonHalfWidth,
    int minUsableHeight)
{
    if (!PtInRect(&taskbarRect, point))
        return false;

    int taskbarHeight =
        taskbarRect.bottom - taskbarRect.top;

    int usableHeight =
        taskbarHeight > minUsableHeight
            ? taskbarHeight
            : minUsableHeight;

    RECT left =
    {
        taskbarRect.left,
        taskbarRect.bottom - usableHeight,
        taskbarRect.left + leftButtonWidth,
        taskbarRect.bottom
    };

    if (PtInRect(&left, point))
        return true;

    int center =
        taskbarRect.left +
        (taskbarRect.right - taskbarRect.left) / 2;

    RECT centered =
    {
        center - centerButtonHalfWidth,
        taskbarRect.bottom - usableHeight,
        center + centerButtonHalfWidth,
        taskbarRect.bottom
    };

    return PtInRect(&centered, point) != FALSE;
}

// ============================================================
// Taskbar Start-button mouse-hook swallow tracking
// ============================================================

// Guarantees a taskbar Start-button click is swallowed symmetrically: if
// the physical WM_LBUTTONDOWN over the button is eaten (not forwarded via
// CallNextHookEx) to fire our own toggle, the matching physical
// WM_LBUTTONUP must also be eaten, unconditionally, regardless of where the
// cursor ends up by the time it's released. An asymmetric swallow here is
// the same class of bug that caused the Windows-key incident (see
// SPEC.md 2.1) — just for a mouse button instead of a modifier key — so
// this class exists to make that structurally impossible rather than
// relying on it happening to be harmless.
class StartButtonMouseTracker
{
public:
    // Called on a real (non-injected) WM_LBUTTONDOWN. hitStartButton is
    // the result of hit-testing this press against the Start button's
    // rect. Returns true iff the hook should swallow this down (and act
    // on it); the same value is remembered so the matching up is handled
    // consistently.
    bool OnLeftButtonDown(bool hitStartButton)
    {
        m_capturingUp = hitStartButton;
        return hitStartButton;
    }

    // Called on a real (non-injected) WM_LBUTTONUP. Returns true iff this
    // up must be swallowed to match a down that was swallowed above —
    // regardless of the current cursor position, so a press-then-drag-off
    // still releases cleanly instead of leaking a stray up to whatever is
    // under the cursor now.
    bool OnLeftButtonUp()
    {
        bool consume = m_capturingUp;
        m_capturingUp = false;
        return consume;
    }

private:
    bool m_capturingUp = false;
};

// ============================================================
// Search-box open/close lifecycle
// ============================================================

// Whether the search box's typed text and any leftover results/God Mode
// browse listing should be cleared the next time the Start menu opens.
// Unconditionally true — every fresh open starts clean, the same way the
// native Start menu does — but pulled out into its own named decision
// point (mirroring WinKeyTracker/StartButtonMouseTracker's style) rather
// than being three scattered clear() calls with no single place a future
// change or an accidental deletion would be caught. This is what fixes
// stale search text/results (including the God Mode Control Panel
// listing, which reads like a leftover context menu when it reappears)
// surviving across a close-then-reopen.
inline bool ShouldClearSearchOnOpen()
{
    return true;
}

// ============================================================
// Search query matching
// ============================================================

// True if `query` contains a wildcard character (`*` or `?`), in which
// case it's an explicit glob pattern rather than a plain substring query.
inline bool IsWildcardQuery(const std::wstring& query)
{
    return
        query.find(L'*') != std::wstring::npos ||
        query.find(L'?') != std::wstring::npos;
}

// Decides whether a single candidate name matches a typed search query,
// against a precomputed lowercase form of that name (callers precompute
// this once per indexed entry rather than lowercasing on every keystroke).
// A `*`/`?` anywhere in the query routes to real wildcard-pattern matching
// (PathMatchSpecW, case-insensitive by its own contract) against the raw
// `name`; anything else, if 2+ characters, is a plain lowercase substring
// ("contains") match against `nameLower`. Below 2 characters, nothing
// matches at all — a 1-character query against a large index is mostly
// noise. `queryLower` must already be the lowercased form of `query`.
inline bool MatchesSearchQuery(
    const std::wstring& name,
    const std::wstring& nameLower,
    const std::wstring& query,
    const std::wstring& queryLower)
{
    if (query.empty())
        return false;

    if (IsWildcardQuery(query))
        return PathMatchSpecW(name.c_str(), query.c_str()) != FALSE;

    if (query.size() < 2)
        return false;

    return nameLower.find(queryLower) != std::wstring::npos;
}

// ============================================================
// Hover-preview panel sizing
// ============================================================

// Clamps a text/JSON/XML preview panel to a readable range relative to
// the work area, given the raw content size measured by the caller (GDI
// text-extent math stays in classicshell.cpp; only this clamp arithmetic
// is pulled out here to be testable without a live HDC).
inline SIZE ClampPreviewTextSize(
    int contentW,
    int contentH,
    int workAreaW,
    int workAreaH)
{
    int minW = (int)(workAreaW * 0.20);
    int maxW = (int)(workAreaW * 0.60);
    int minH = (int)(workAreaH * 0.12);
    int maxH = (int)(workAreaH * 0.65);

    int w = contentW;
    int h = contentH;

    if (w < minW) w = minW;
    if (w > maxW) w = maxW;
    if (h < minH) h = minH;
    if (h > maxH) h = maxH;

    return { w, h };
}

// Scales an image preview to roughly a quarter of the screen's area
// (by linear scale factor, preserving aspect ratio), then clamps to a
// fraction of the work area and a minimum floor so a tiny source image
// isn't shown illegibly small and a huge one doesn't fill the screen.
inline SIZE ScalePreviewImageSize(
    int imageW,
    int imageH,
    int workAreaW,
    int workAreaH)
{
    if (imageW <= 0 || imageH <= 0)
        return { 0, 0 };

    double imageArea = (double)imageW * (double)imageH;
    double targetArea = (double)workAreaW * (double)workAreaH * 0.25;

    double scale = sqrt(targetArea / imageArea);

    int w = (int)(imageW * scale);
    int h = (int)(imageH * scale);

    int maxW = (int)(workAreaW * 0.80);
    int maxH = (int)(workAreaH * 0.80);

    if (w > maxW || h > maxH)
    {
        double shrink =
            (double)maxW / w < (double)maxH / h
                ? (double)maxW / w
                : (double)maxH / h;

        w = (int)(w * shrink);
        h = (int)(h * shrink);
    }

    const int MIN_DIMENSION = 60;

    if (w < MIN_DIMENSION) w = MIN_DIMENSION;
    if (h < MIN_DIMENSION) h = MIN_DIMENSION;

    return { w, h };
}

// A fixed, content-independent width for the text/JSON/XML preview
// panel, so it always reads as a clean, compact rectangle in the corner
// regardless of how long any single line in the file is — long lines
// word-wrap to this width instead of the panel stretching to fit them.
// Proportional to the work area, but clamped so it stays readable on a
// tiny screen and doesn't sprawl on a huge one.
inline int PreviewTextTargetWidth(
    int workAreaW,
    int minWidth,
    int maxWidth)
{
    int w = (int)(workAreaW * 0.25);

    if (w < minWidth) w = minWidth;
    if (w > maxWidth) w = maxWidth;

    return w;
}

// ============================================================
// Text-box caret/selection model
// ============================================================

// Pure caret + selection tracking for a single-line text box, independent
// of the actual text storage (the caller owns that) — so click-to-place,
// drag-to-select, Shift+arrow, and Ctrl+A all have one small, testable
// place their behavior lives, run the same in the shipped app and in a
// console unit test.
class TextSelection
{
public:
    int Caret() const { return m_caret; }

    bool HasSelection() const
    {
        return m_anchor >= 0 && m_anchor != m_caret;
    }

    int SelectionStart() const
    {
        return HasSelection()
            ? (m_anchor < m_caret ? m_anchor : m_caret)
            : m_caret;
    }

    int SelectionEnd() const
    {
        return HasSelection()
            ? (m_anchor > m_caret ? m_anchor : m_caret)
            : m_caret;
    }

    // Keeps the caret/anchor from pointing past the end of the text
    // after it's changed size out from under this (a fresh load, an
    // external clear).
    void ClampTo(int textLength)
    {
        if (textLength < 0) textLength = 0;

        if (m_caret > textLength) m_caret = textLength;
        if (m_caret < 0) m_caret = 0;

        if (m_anchor > textLength) m_anchor = textLength;
    }

    void Reset()
    {
        m_caret = 0;
        m_anchor = -1;
    }

    // Places the caret at an absolute position (e.g. a mouse click).
    // extend=true grows/starts a selection from wherever the anchor
    // already was (or from the old caret position, if this is the start
    // of a new drag); extend=false collapses any selection.
    void PlaceCaret(int pos, bool extend)
    {
        if (extend && m_anchor < 0)
            m_anchor = m_caret;
        else if (!extend)
            m_anchor = -1;

        m_caret = pos;
    }

    // Same as PlaceCaret(pos, false) — an edit (typed char, delete)
    // collapses any selection to a fresh caret position.
    void PlaceCaret(int pos)
    {
        PlaceCaret(pos, false);
    }

    void MoveLeft(bool extend)
    {
        int target =
            (HasSelection() && !extend)
                ? SelectionStart()
                : m_caret - 1;

        if (target < 0) target = 0;

        PlaceCaret(target, extend);
    }

    void MoveRight(bool extend, int textLength)
    {
        int target =
            (HasSelection() && !extend)
                ? SelectionEnd()
                : m_caret + 1;

        if (target > textLength) target = textLength;

        PlaceCaret(target, extend);
    }

    void MoveHome(bool extend)
    {
        PlaceCaret(0, extend);
    }

    void MoveEnd(bool extend, int textLength)
    {
        PlaceCaret(textLength, extend);
    }

    void SelectAll(int textLength)
    {
        m_anchor = 0;
        m_caret = textLength;
    }

private:
    int m_caret = 0;
    int m_anchor = -1; // -1 = no selection
};

// Given a character position, returns [outStart, outEnd) for the word
// (or, if the position is on punctuation/whitespace, the contiguous run
// of that instead) touching it — the classic double-click-selects-a-word
// text-box convention. A "word" character is alnum or underscore; any
// other contiguous run of non-word characters groups together the same
// way, so double-clicking a run of dashes or spaces selects that run
// rather than doing nothing or reaching into the nearest real word.
inline void FindWordBoundsAt(
    const std::wstring& text,
    int pos,
    int& outStart,
    int& outEnd)
{
    int n = (int)text.size();

    if (n == 0)
    {
        outStart = 0;
        outEnd = 0;
        return;
    }

    if (pos < 0) pos = 0;
    if (pos >= n) pos = n - 1;

    auto isWordChar = [](wchar_t c)
    {
        return iswalnum((wint_t)c) || c == L'_';
    };

    bool wantWord = isWordChar(text[pos]);

    int start = pos;

    while (start > 0 && isWordChar(text[start - 1]) == wantWord)
        start--;

    int end = pos + 1;

    while (end < n && isWordChar(text[end]) == wantWord)
        end++;

    outStart = start;
    outEnd = end;
}
