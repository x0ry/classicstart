// starthook.h — pure, testable logic behind ClassicShell's interception of
// the physical Windows key and the taskbar Start button.
//
// This is deliberately kept free of SetWindowsHookEx/CallNextHookEx and any
// other live-hook plumbing, so it can run the same in the shipped app and in
// a plain console unit test: WinKeyTracker only tracks up/down state and
// hands back a decision, and IsStartButtonHit only does rectangle math.
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
