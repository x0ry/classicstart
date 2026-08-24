// Tests for starthook.h — the logic that decides when ClassicShell's own
// Start menu should intercept a physical Windows-key tap or a taskbar
// Start-button click, instead of letting the OS's native Start menu
// handle it. This is the first bug/test pair for the project: regressions
// here mean either menu stops popping up, or both do.
#include "mini_test.h"
#include "../starthook.h"
#include <cstring>

// ------------------------------------------------------------------
// ShouldClearSearchOnOpen
// ------------------------------------------------------------------

TEST(SearchAlwaysClearsOnFreshOpen)
{
    // Regression guard for stale search text/results (including the God
    // Mode Control Panel listing) surviving a close-then-reopen instead
    // of the menu starting clean every time, the same way the native
    // Start menu does.
    CHECK(ShouldClearSearchOnOpen());
}

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

// ------------------------------------------------------------------
// MatchesSearchQuery / IsWildcardQuery
// ------------------------------------------------------------------

TEST(WildcardQuery_Detected)
{
    CHECK(IsWildcardQuery(L"*.txt"));
    CHECK(IsWildcardQuery(L"report.?"));
    CHECK(!IsWildcardQuery(L"report"));
}

TEST(WildcardPattern_MatchesExtension)
{
    bool hit =
        MatchesSearchQuery(
            L"report.txt",
            L"report.txt",
            L"*.txt",
            L"*.txt");

    CHECK(hit);
}

TEST(WildcardPattern_MissesWrongExtension)
{
    bool hit =
        MatchesSearchQuery(
            L"report.txt",
            L"report.txt",
            L"*.csv",
            L"*.csv");

    CHECK(!hit);
}

TEST(ContainsQuery_MatchesSubstring)
{
    bool hit =
        MatchesSearchQuery(
            L"Quarterly Report.txt",
            L"quarterly report.txt",
            L"report",
            L"report");

    CHECK(hit);
}

TEST(ContainsQuery_MissesUnrelatedName)
{
    bool hit =
        MatchesSearchQuery(
            L"budget.xlsx",
            L"budget.xlsx",
            L"report",
            L"report");

    CHECK(!hit);
}

TEST(ShortQuery_NeverMatches)
{
    bool hit =
        MatchesSearchQuery(
            L"report.txt",
            L"report.txt",
            L"r",
            L"r");

    CHECK(!hit);
}

TEST(EmptyQuery_NeverMatches)
{
    bool hit =
        MatchesSearchQuery(
            L"report.txt",
            L"report.txt",
            L"",
            L"");

    CHECK(!hit);
}

// ------------------------------------------------------------------
// ClampPreviewTextSize / ScalePreviewImageSize
// ------------------------------------------------------------------

TEST(PreviewTextSize_ClampsSmallContentToMinimum)
{
    SIZE size =
        ClampPreviewTextSize(
            10, 10,
            1920, 1080);

    CHECK(size.cx >= (int)(1920 * 0.20));
    CHECK(size.cy >= (int)(1080 * 0.12));
}

TEST(PreviewTextSize_ClampsLargeContentToMaximum)
{
    SIZE size =
        ClampPreviewTextSize(
            10000, 10000,
            1920, 1080);

    CHECK(size.cx <= (int)(1920 * 0.60));
    CHECK(size.cy <= (int)(1080 * 0.65));
}

TEST(PreviewImageSize_TargetsQuarterScreenArea)
{
    SIZE size =
        ScalePreviewImageSize(
            1000, 1000,
            1920, 1080);

    double area = (double)size.cx * size.cy;
    double target = 1920.0 * 1080.0 * 0.25;

    // Allow slack for the integer rounding in the scale/shrink math.
    CHECK(area > target * 0.7);
    CHECK(area < target * 1.3);
}

TEST(PreviewImageSize_ClampsToWorkAreaFraction)
{
    // A very wide, short source image shouldn't blow past the 80%
    // work-area width clamp even though it easily hits the target area.
    SIZE size =
        ScalePreviewImageSize(
            10000, 10,
            1920, 1080);

    CHECK(size.cx <= (int)(1920 * 0.80));
}

TEST(PreviewImageSize_FloorsTinySourceImage)
{
    SIZE size =
        ScalePreviewImageSize(
            4, 4,
            1920, 1080);

    CHECK(size.cx >= 60);
    CHECK(size.cy >= 60);
}

// ------------------------------------------------------------------
// PreviewTextTargetWidth
// ------------------------------------------------------------------

TEST(PreviewTextWidth_ScalesWithWorkArea)
{
    int narrow = PreviewTextTargetWidth(1000, 100, 2000);
    int wide = PreviewTextTargetWidth(2000, 100, 2000);

    CHECK(wide > narrow);
}

TEST(PreviewTextWidth_ClampsToMinimumOnTinyScreen)
{
    int w = PreviewTextTargetWidth(400, 300, 560);

    CHECK(w == 300);
}

TEST(PreviewTextWidth_ClampsToMaximumOnHugeScreen)
{
    int w = PreviewTextTargetWidth(10000, 300, 560);

    CHECK(w == 560);
}

// ------------------------------------------------------------------
// TextSelection
// ------------------------------------------------------------------

TEST(FreshSelection_HasNoSelectionAndCaretAtStart)
{
    TextSelection sel;

    CHECK(!sel.HasSelection());
    CHECK(sel.Caret() == 0);
}

TEST(PlaceCaretWithoutExtend_MovesCaretNoSelection)
{
    TextSelection sel;

    sel.PlaceCaret(5, false);

    CHECK(sel.Caret() == 5);
    CHECK(!sel.HasSelection());
}

TEST(PlaceCaretWithExtend_StartsSelectionFromPriorCaret)
{
    TextSelection sel;

    sel.PlaceCaret(3, false);
    sel.PlaceCaret(7, true);

    CHECK(sel.HasSelection());
    CHECK(sel.SelectionStart() == 3);
    CHECK(sel.SelectionEnd() == 7);
}

TEST(PlaceCaretWithExtend_CanSelectBackward)
{
    TextSelection sel;

    sel.PlaceCaret(7, false);
    sel.PlaceCaret(3, true);

    CHECK(sel.HasSelection());
    CHECK(sel.SelectionStart() == 3);
    CHECK(sel.SelectionEnd() == 7);
}

TEST(MoveLeftWithoutSelection_MovesCaretBackOne)
{
    TextSelection sel;

    sel.PlaceCaret(5, false);
    sel.MoveLeft(false);

    CHECK(sel.Caret() == 4);
    CHECK(!sel.HasSelection());
}

TEST(MoveLeftAtStart_StaysAtZero)
{
    TextSelection sel;

    sel.MoveLeft(false);

    CHECK(sel.Caret() == 0);
}

TEST(MoveRightWithoutSelection_MovesCaretForwardOne)
{
    TextSelection sel;

    sel.MoveRight(false, 10);

    CHECK(sel.Caret() == 1);
}

TEST(MoveRightAtEnd_ClampsToTextLength)
{
    TextSelection sel;

    sel.PlaceCaret(10, false);
    sel.MoveRight(false, 10);

    CHECK(sel.Caret() == 10);
}

TEST(MoveLeftWithActiveSelection_CollapsesToSelectionStart)
{
    // Standard textbox behavior: pressing Left (no Shift) with an active
    // selection collapses to the start of that selection, not one
    // character left of wherever the caret happened to be.
    TextSelection sel;

    sel.PlaceCaret(3, false);
    sel.PlaceCaret(8, true);

    sel.MoveLeft(false);

    CHECK(sel.Caret() == 3);
    CHECK(!sel.HasSelection());
}

TEST(MoveRightWithActiveSelection_CollapsesToSelectionEnd)
{
    TextSelection sel;

    sel.PlaceCaret(3, false);
    sel.PlaceCaret(8, true);

    sel.MoveRight(false, 20);

    CHECK(sel.Caret() == 8);
    CHECK(!sel.HasSelection());
}

TEST(ShiftLeftTwice_ExtendsSelectionByTwo)
{
    TextSelection sel;

    sel.PlaceCaret(5, false);
    sel.MoveLeft(true);
    sel.MoveLeft(true);

    CHECK(sel.HasSelection());
    CHECK(sel.SelectionStart() == 3);
    CHECK(sel.SelectionEnd() == 5);
}

TEST(SelectAll_SelectsEntireRange)
{
    TextSelection sel;

    sel.PlaceCaret(4, false);
    sel.SelectAll(12);

    CHECK(sel.HasSelection());
    CHECK(sel.SelectionStart() == 0);
    CHECK(sel.SelectionEnd() == 12);
}

TEST(MoveHomeAndEnd_JumpToBoundaries)
{
    TextSelection sel;

    sel.PlaceCaret(5, false);

    sel.MoveHome(false);
    CHECK(sel.Caret() == 0);

    sel.MoveEnd(false, 9);
    CHECK(sel.Caret() == 9);
}

TEST(ClampTo_PullsCaretAndAnchorInsideShrunkText)
{
    TextSelection sel;

    sel.PlaceCaret(2, false);
    sel.PlaceCaret(9, true);

    sel.ClampTo(5);

    CHECK(sel.Caret() == 5);
    CHECK(sel.SelectionEnd() == 5);
}

TEST(Reset_ClearsCaretAndSelection)
{
    TextSelection sel;

    sel.PlaceCaret(3, false);
    sel.PlaceCaret(8, true);

    sel.Reset();

    CHECK(sel.Caret() == 0);
    CHECK(!sel.HasSelection());
}

// ------------------------------------------------------------------
// FindWordBoundsAt
// ------------------------------------------------------------------

TEST(DoubleClickMiddleOfWord_SelectsWholeWord)
{
    // "hello world" — clicking inside "hello" (index 2, the 'l') should
    // select the whole word, indices [0, 5).
    int start = 0, end = 0;

    FindWordBoundsAt(L"hello world", 2, start, end);

    CHECK(start == 0);
    CHECK(end == 5);
}

TEST(DoubleClickSecondWord_SelectsThatWord)
{
    int start = 0, end = 0;

    FindWordBoundsAt(L"hello world", 7, start, end);

    CHECK(start == 6);
    CHECK(end == 11);
}

TEST(DoubleClickOnUnderscore_TreatsUnderscoreAsWordChar)
{
    int start = 0, end = 0;

    FindWordBoundsAt(L"foo_bar baz", 3, start, end);

    CHECK(start == 0);
    CHECK(end == 7);
}

TEST(DoubleClickOnPunctuationRun_SelectsThatRun)
{
    // "a---b" — clicking in the dashes selects the whole punctuation
    // run, not the nearest word, matching the classic text-box
    // convention of grouping same-class runs together.
    int start = 0, end = 0;

    FindWordBoundsAt(L"a---b", 2, start, end);

    CHECK(start == 1);
    CHECK(end == 4);
}

TEST(DoubleClickSingleCharWord_SelectsJustThatChar)
{
    int start = 0, end = 0;

    FindWordBoundsAt(L"a b c", 2, start, end);

    CHECK(start == 2);
    CHECK(end == 3);
}

TEST(FindWordBounds_EmptyText_ReturnsEmptyRange)
{
    int start = 5, end = 5;

    FindWordBoundsAt(L"", 0, start, end);

    CHECK(start == 0);
    CHECK(end == 0);
}

TEST(FindWordBounds_PositionPastEnd_ClampsToLastChar)
{
    int start = 0, end = 0;

    FindWordBoundsAt(L"hello", 99, start, end);

    CHECK(start == 0);
    CHECK(end == 5);
}

// ------------------------------------------------------------------
// ComputeScrollbarThumb / ScrollOffsetFromThumbTop
// ------------------------------------------------------------------

TEST(ScrollbarThumb_FillsTrackWhenContentFits)
{
    ScrollbarThumb thumb =
        ComputeScrollbarThumb(200, 400, 0, 400, 24);

    CHECK(thumb.top == 0);
    CHECK(thumb.height == 400);
}

TEST(ScrollbarThumb_ProportionalToVisibleFraction)
{
    // Half the content visible -> thumb roughly half the track.
    ScrollbarThumb thumb =
        ComputeScrollbarThumb(800, 400, 0, 400, 24);

    CHECK(thumb.height >= 190);
    CHECK(thumb.height <= 210);
}

TEST(ScrollbarThumb_NeverShrinksBelowMinimum)
{
    ScrollbarThumb thumb =
        ComputeScrollbarThumb(100000, 400, 0, 400, 24);

    CHECK(thumb.height == 24);
}

TEST(ScrollbarThumb_AtTopWhenScrollIsZero)
{
    ScrollbarThumb thumb =
        ComputeScrollbarThumb(800, 400, 0, 400, 24);

    CHECK(thumb.top == 0);
}

TEST(ScrollbarThumb_AtBottomWhenScrolledToMax)
{
    int maxScroll = 800 - 400;

    ScrollbarThumb thumb =
        ComputeScrollbarThumb(800, 400, maxScroll, 400, 24);

    CHECK(thumb.top + thumb.height == 400);
}

TEST(ScrollOffsetFromThumbTop_RoundTripsWithComputeScrollbarThumb)
{
    int contentHeight = 800;
    int viewportHeight = 400;
    int trackHeight = 400;
    int minThumbHeight = 24;

    ScrollbarThumb thumb =
        ComputeScrollbarThumb(
            contentHeight, viewportHeight, 150, trackHeight, minThumbHeight);

    int scroll =
        ScrollOffsetFromThumbTop(
            thumb.top, contentHeight, viewportHeight, trackHeight, thumb.height);

    // Round-tripping through pixel-quantized thumb positions won't land
    // on the exact original offset, but should be close.
    CHECK(scroll >= 130);
    CHECK(scroll <= 170);
}

TEST(ScrollOffsetFromThumbTop_ZeroWhenNothingToScroll)
{
    int scroll =
        ScrollOffsetFromThumbTop(50, 200, 400, 400, 400);

    CHECK(scroll == 0);
}

// ------------------------------------------------------------------
// LooksLikeText
// ------------------------------------------------------------------

TEST(LooksLikeText_EmptyFile_IsText)
{
    CHECK(LooksLikeText(nullptr, 0));
}

TEST(LooksLikeText_PlainAscii_IsText)
{
    const char* sample = "Hello, world!\nThis is a README.\n";

    CHECK(
        LooksLikeText(
            reinterpret_cast<const unsigned char*>(sample),
            strlen(sample)));
}

TEST(LooksLikeText_ContainsNulByte_IsNotText)
{
    unsigned char sample[] = { 'A', 'B', 0x00, 'C', 'D' };

    CHECK(!LooksLikeText(sample, sizeof(sample)));
}

TEST(LooksLikeText_MostlyControlBytes_IsNotText)
{
    unsigned char sample[100];

    for (int i = 0; i < 100; ++i)
        sample[i] = (unsigned char)(i % 2 == 0 ? 0x01 : 'A');

    CHECK(!LooksLikeText(sample, sizeof(sample)));
}

TEST(LooksLikeText_ToleratesAFewOddBytes)
{
    unsigned char sample[100];

    for (int i = 0; i < 100; ++i)
        sample[i] = 'A';

    // 5 odd bytes out of 100 — well under the tolerance.
    sample[10] = 0x01;
    sample[20] = 0x02;
    sample[30] = 0x03;
    sample[40] = 0x04;
    sample[50] = 0x05;

    CHECK(LooksLikeText(sample, sizeof(sample)));
}

TEST(LooksLikeText_TabsNewlinesCarriageReturns_AreFine)
{
    const char* sample = "line one\r\n\tindented\r\nline three\n";

    CHECK(
        LooksLikeText(
            reinterpret_cast<const unsigned char*>(sample),
            strlen(sample)));
}
