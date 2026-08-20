// wintap_experiment.cpp — standalone, isolated test harness for one
// question: which way of intercepting a standalone Windows-key tap
// actually suppresses the native Start menu, confirmed by a real
// physical key press rather than simulated input.
//
// Build (from a VS Developer Command Prompt, or after VsDevCmd.bat —
// see build.bat for how to locate/invoke it):
//
//   cl.exe /nologo /std:c++17 /EHsc wintap_experiment.cpp user32.lib
//
// Mode is a command-line argument, not interactive stdin, on purpose:
// mode 1 is expected to leave the OS believing the Windows key is
// still held (a known side effect being tested for, not a bug in
// this harness), which turns every subsequent keystroke into a
// phantom Win+key combo — including anything typed at this console.
// A stdin-driven mode switch would be unusable exactly when it's
// needed. Run it once per mode instead:
//
//   wintap_experiment.exe 1
//   wintap_experiment.exe 2
//   wintap_experiment.exe 3
//   wintap_experiment.exe 4
//
// After 3 standalone taps it always unhooks and sends recovery
// key-ups for both Windows keys — regardless of mode — then exits.
// Ctrl+C does the same cleanup early if needed.
//
// Modes:
//   1 = block only: eat the real key-up, nothing else (matches the
//       pre-rewrite classicstart.cpp, which reportedly did suppress
//       the native menu). Expected side effect: typing breaks after
//       the first tap, until this harness's own cleanup runs.
//   2 = block + synthetic Win key-up replacement (this session's
//       shipped classicshell.cpp fix — suspected of being the actual
//       regression, since the injected replacement may itself
//       trigger whatever native mechanism watches for the release).
//   3 = poison key only: inject a Ctrl tap on Win-down, let the real
//       key-up pass through untouched (this session's first fix).
//   4 = block + synthetic Ctrl tap interleaved *before* the synthetic
//       Win key-up replacement, so — if the native trigger checks
//       for "another key pressed while Win was still down" — the
//       poison key lands while the system still considers Win held,
//       ahead of the replacement release.
//   5 = block + synthetic Win key-up replacement sent from a worker
//       thread after a delay (see WIN_UP_DELAY_MS), instead of
//       immediately. Theory: the native tap-vs-hold detector likely
//       has a bounded eligibility window after the real key-up it
//       never saw; a late-arriving synthetic release might land
//       after that window closes and go unnoticed by it, while still
//       eventually clearing GetAsyncKeyState. Never sleeps inside the
//       hook callback itself — that would stall all system input.
#include <windows.h>
#include <cstdio>
#include <cstdlib>

static int g_mode = 1;
static int g_tapsHandled = 0;
static const int MAX_TAPS = 3;
static const DWORD WIN_UP_DELAY_MS = 700;

static bool g_lwinDown = false;
static bool g_rwinDown = false;
static bool g_winCombo = false;

static HHOOK g_hook = nullptr;

static void Log(const char* msg)
{
    printf("[mode %d, tap %d/%d] %s\n", g_mode, g_tapsHandled, MAX_TAPS, msg);
    fflush(stdout);
}

static void SendCtrlTap()
{
    INPUT in[2]{};
    in[0].type = INPUT_KEYBOARD;
    in[0].ki.wVk = VK_LCONTROL;
    in[1].type = INPUT_KEYBOARD;
    in[1].ki.wVk = VK_LCONTROL;
    in[1].ki.dwFlags = KEYEVENTF_KEYUP;
    SendInput(2, in, sizeof(INPUT));
}

static void SendKeyUp(WORD vk)
{
    INPUT in{};
    in.type = INPUT_KEYBOARD;
    in.ki.wVk = vk;
    in.ki.dwFlags = KEYEVENTF_KEYUP;
    SendInput(1, &in, sizeof(INPUT));
}

// Always safe to call, regardless of what mode did or didn't send —
// redundant key-ups are harmless, a missing one is what leaves the
// system stuck.
static void RecoverKeyState()
{
    SendKeyUp(VK_LWIN);
    SendKeyUp(VK_RWIN);
}

static DWORD WINAPI DelayedKeyUpThread(LPVOID param)
{
    WORD vk = (WORD)(ULONG_PTR)param;
    Sleep(WIN_UP_DELAY_MS);
    SendKeyUp(vk);
    printf("  -> (delayed) synthetic Win key-up sent, %lu ms after the real one was eaten\n", WIN_UP_DELAY_MS);
    fflush(stdout);
    return 0;
}

static void SendKeyUpDelayed(WORD vk)
{
    HANDLE thread = CreateThread(nullptr, 0, DelayedKeyUpThread, (LPVOID)(ULONG_PTR)vk, 0, nullptr);
    if (thread)
        CloseHandle(thread);
}

static void Cleanup()
{
    if (g_hook)
    {
        UnhookWindowsHookEx(g_hook);
        g_hook = nullptr;
    }

    RecoverKeyState();

    printf("\n=== cleaned up: hook removed, key state recovery sent ===\n");
    fflush(stdout);
}

static BOOL WINAPI ConsoleCtrlHandler(DWORD)
{
    Cleanup();
    ExitProcess(0);
    return TRUE;
}

static LRESULT CALLBACK KeyboardProc(int code, WPARAM wp, LPARAM lp)
{
    if (code != HC_ACTION)
        return CallNextHookEx(nullptr, code, wp, lp);

    auto* key = reinterpret_cast<KBDLLHOOKSTRUCT*>(lp);

    if (!key)
        return CallNextHookEx(nullptr, code, wp, lp);

    if (key->flags & LLKHF_INJECTED)
        return CallNextHookEx(nullptr, code, wp, lp);

    DWORD vk = key->vkCode;
    bool down = wp == WM_KEYDOWN || wp == WM_SYSKEYDOWN;
    bool up = wp == WM_KEYUP || wp == WM_SYSKEYUP;

    if (vk == VK_LWIN || vk == VK_RWIN)
    {
        bool isLeft = vk == VK_LWIN;

        if (down)
        {
            if (isLeft) g_lwinDown = true; else g_rwinDown = true;
            g_winCombo = false;

            Log("Win DOWN");

            if (g_mode == 3)
            {
                Log("  -> sending poison Ctrl tap");
                SendCtrlTap();
            }

            return CallNextHookEx(nullptr, code, wp, lp);
        }

        if (up)
        {
            bool standalone = (isLeft ? g_lwinDown : g_rwinDown) && !g_winCombo;

            if (isLeft) g_lwinDown = false; else g_rwinDown = false;
            if (!g_lwinDown && !g_rwinDown) g_winCombo = false;

            if (!standalone)
            {
                Log("Win UP (combo, passing through)");
                return CallNextHookEx(nullptr, code, wp, lp);
            }

            ++g_tapsHandled;
            LRESULT result;

            switch (g_mode)
            {
            default:
            case 1:
                Log("Win UP standalone -> BLOCKING real key-up, no replacement (would ToggleStart here)");
                result = 1;
                break;

            case 2:
                Log("Win UP standalone -> blocking + sending SYNTHETIC Win key-up replacement (would ToggleStart here)");
                SendKeyUp((WORD)vk);
                result = 1;
                break;

            case 3:
                Log("Win UP standalone -> letting real key-up pass through untouched (poison key was sent on down) (would ToggleStart here)");
                result = CallNextHookEx(nullptr, code, wp, lp);
                break;

            case 4:
                Log("Win UP standalone -> blocking + interleaved poison Ctrl tap + synthetic Win key-up (would ToggleStart here)");
                SendCtrlTap();
                SendKeyUp((WORD)vk);
                result = 1;
                break;

            case 5:
                Log("Win UP standalone -> blocking + DELAYED synthetic Win key-up (would ToggleStart here)");
                SendKeyUpDelayed((WORD)vk);
                result = 1;
                break;
            }

            printf("Tap the physical Windows key, watch for the native Start menu.\n");
            fflush(stdout);

            if (g_tapsHandled >= MAX_TAPS)
            {
                PostQuitMessage(0);
            }

            return result;
        }
    }

    if (down && (g_lwinDown || g_rwinDown))
    {
        g_winCombo = true;
        Log("other key while Win held -> combo, not a tap");
    }

    return CallNextHookEx(nullptr, code, wp, lp);
}

int main(int argc, char** argv)
{
    if (argc >= 2)
    {
        int m = atoi(argv[1]);
        if (m >= 1 && m <= 5)
            g_mode = m;
    }

    SetConsoleCtrlHandler(ConsoleCtrlHandler, TRUE);

    printf("wintap_experiment: mode %d. Will auto-cleanup and exit after %d taps.\n", g_mode, MAX_TAPS);
    printf("Tap the physical Windows key alone and watch this console AND your screen.\n\n");

    g_hook = SetWindowsHookExW(WH_KEYBOARD_LL, KeyboardProc, GetModuleHandleW(nullptr), 0);

    if (!g_hook)
    {
        printf("SetWindowsHookExW failed, err=%lu\n", GetLastError());
        return 1;
    }

    MSG msg{};
    while (GetMessageW(&msg, nullptr, 0, 0) > 0)
    {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    Cleanup();

    printf("Done. This window will close in 5 seconds.\n");
    fflush(stdout);
    Sleep(5000);

    return 0;
}
