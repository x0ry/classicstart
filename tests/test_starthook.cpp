// Tests for starthook.h — the logic that decides when ClassicShell's own
// Start menu should intercept a physical Windows-key tap or a taskbar
// Start-button click, instead of letting the OS's native Start menu
// handle it. This is the first bug/test pair for the project: regressions
// here mean either menu stops popping up, or both do.
#include "mini_test.h"
#include "../starthook.h"

// ------------------------------------------------------------------
// WinKeyTracker
// ------------------------------------------------------------------

TEST(StandaloneLeftWinTap_Toggles)
{
    WinKeyTracker tracker;

    tracker.OnWinKeyDown(VK_LWIN);
    WinKeyAction action = tracker.OnWinKeyUp(VK_LWIN);

    CHECK(action == WinKeyAction::Toggle);
}

TEST(StandaloneRightWinTap_Toggles)
{
    WinKeyTracker tracker;

    tracker.OnWinKeyDown(VK_RWIN);
    WinKeyAction action = tracker.OnWinKeyUp(VK_RWIN);

    CHECK(action == WinKeyAction::Toggle);
}

TEST(RepeatedTaps_EachOneToggles)
{
    WinKeyTracker tracker;

    for (int i = 0; i < 3; ++i)
    {
        tracker.OnWinKeyDown(VK_LWIN);
        CHECK(
            tracker.OnWinKeyUp(VK_LWIN) ==
            WinKeyAction::Toggle);
    }
}

TEST(WinPlusOtherKey_ClosesInsteadOfToggling)
{
    // Win+D and friends must reach Explorer untouched, not toggle our
    // own menu. Holding Win, pressing another key, then releasing Win
    // must not report a standalone tap.
    WinKeyTracker tracker;

    tracker.OnWinKeyDown(VK_LWIN);

    WinKeyAction otherKeyAction = tracker.OnOtherKeyDown();
    CHECK(otherKeyAction == WinKeyAction::Close);

    WinKeyAction upAction = tracker.OnWinKeyUp(VK_LWIN);
    CHECK(upAction == WinKeyAction::None);
}

TEST(OtherKeyWithoutWinHeld_DoesNothing)
{
    WinKeyTracker tracker;

    WinKeyAction action = tracker.OnOtherKeyDown();

    CHECK(action == WinKeyAction::None);
}

TEST(ComboThenReleaseThenFreshTap_TogglesAgain)
{
    // A Win+key combo shouldn't leave the tracker stuck thinking every
    // future tap is also a combo.
    WinKeyTracker tracker;

    tracker.OnWinKeyDown(VK_LWIN);
    tracker.OnOtherKeyDown();
    tracker.OnWinKeyUp(VK_LWIN);

    tracker.OnWinKeyDown(VK_LWIN);
    WinKeyAction action = tracker.OnWinKeyUp(VK_LWIN);

    CHECK(action == WinKeyAction::Toggle);
}

// ------------------------------------------------------------------
// IsStartButtonHit
// ------------------------------------------------------------------

namespace
{
    // A 1920-wide taskbar docked at the bottom of a 1080-tall screen,
    // 40px tall — a plain, unscaled stand-in for what GetWindowRect
    // would report for Shell_TrayWnd.
    RECT TestTaskbarRect()
    {
        return RECT{ 0, 1040, 1920, 1080 };
    }
}

TEST(ClickOnLeftStartButton_Hits)
{
    POINT point{ 30, 1060 };

    bool hit = IsStartButtonHit(
        point,
        TestTaskbarRect(),
        62,
        34,
        32);

    CHECK(hit);
}

TEST(ClickOnCenteredStartButton_Hits)
{
    POINT point{ 960, 1060 };

    bool hit = IsStartButtonHit(
        point,
        TestTaskbarRect(),
        62,
        34,
        32);

    CHECK(hit);
}

TEST(ClickBetweenButtons_Misses)
{
    POINT point{ 500, 1060 };

    bool hit = IsStartButtonHit(
        point,
        TestTaskbarRect(),
        62,
        34,
        32);

    CHECK(!hit);
}

TEST(ClickAboveTaskbar_Misses)
{
    POINT point{ 30, 1000 };

    bool hit = IsStartButtonHit(
        point,
        TestTaskbarRect(),
        62,
        34,
        32);

    CHECK(!hit);
}

// ------------------------------------------------------------------
// StartButtonMouseTracker
// ------------------------------------------------------------------

TEST(DownOnButton_UpSwallowsBoth)
{
    StartButtonMouseTracker tracker;

    bool downConsumed = tracker.OnLeftButtonDown(true);
    bool upConsumed = tracker.OnLeftButtonUp();

    CHECK(downConsumed);
    CHECK(upConsumed);
}

TEST(DownOffButton_UpSwallowsNeither)
{
    StartButtonMouseTracker tracker;

    bool downConsumed = tracker.OnLeftButtonDown(false);
    bool upConsumed = tracker.OnLeftButtonUp();

    CHECK(!downConsumed);
    CHECK(!upConsumed);
}

TEST(DownOnButtonThenDragOff_UpStillSwallowed)
{
    // The up must be swallowed based on where the down landed, not
    // where the cursor happens to be when the button is released —
    // otherwise a press-then-drag-off leaks a stray up to whatever is
    // now under the cursor.
    StartButtonMouseTracker tracker;

    tracker.OnLeftButtonDown(true);

    // Simulate the cursor having moved elsewhere before release: the
    // tracker takes no positional input on OnLeftButtonUp, so there's
    // nothing to feed it here, but the symmetry itself is the assertion.
    bool upConsumed = tracker.OnLeftButtonUp();

    CHECK(upConsumed);
}

TEST(RepeatedClicks_EachCycleIndependent)
{
    StartButtonMouseTracker tracker;

    for (int i = 0; i < 3; ++i)
    {
        CHECK(tracker.OnLeftButtonDown(true));
        CHECK(tracker.OnLeftButtonUp());
    }

    CHECK(!tracker.OnLeftButtonDown(false));
    CHECK(!tracker.OnLeftButtonUp());
}

TEST(UpWithoutPriorDown_DoesNotSwallow)
{
    StartButtonMouseTracker tracker;

    CHECK(!tracker.OnLeftButtonUp());
}
