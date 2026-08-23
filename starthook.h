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
