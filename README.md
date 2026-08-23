# ClassicShell

A lightweight, classic-style Start menu replacement for Windows — a single
self-contained C++ source file, no runtime dependencies beyond the OS.

![ClassicShell](screenshot.png)

> This is a rebuild in progress. It was reset to the last known-good
> commit after a keyboard-hook regression in a since-discarded version
> made the Windows key get permanently stuck, forcing a reboot to
> recover. See [SPEC.md](SPEC.md) for the full intended feature set, the
> correctness rules this rebuild is held to, and the incident writeup
> explaining exactly what went wrong and why. Features listed below are
> what's actually implemented right now, not the eventual target.

## Features

- Classic single-column Start menu: This PC, Programs, Documents, Downloads,
  Pictures, Music, Control Panel, and Run, each with real Windows-accurate
  icons.
- A search/Run box that resolves typed commands, known folders, and
  installed apps (including packaged/Store apps).
- Power flyout with Restart, Shut down, Sign out, and Lock, each with a
  hover label.
- Up to 3 configurable quick-launch buttons.
- Live opacity slider, acrylic blur, rounded corners, and the real Windows
  accent color — all anti-aliased.
- Renders in Segoe UI Variable and Segoe Fluent Icons — the same fonts
  Windows 11's own Start menu and Settings app use — falling back to
  classic Segoe UI / Segoe MDL2 Assets on older Windows versions that
  don't have them installed.
- Full keyboard navigation (Tab / Shift+Tab / arrows) with a visible focus
  ring, and Ctrl+Esc to open.
- Single-instance: launching it again cleanly replaces any running copy.
- Left-clicking the taskbar Start button hijacks into ClassicShell's own
  menu — this is the primary way to open it, and works out of the box.
  Right-click and middle-click on the button are untouched. Win+key combos
  (Win+D, Win+E, ...) always reach their normal native targets untouched.
- Tapping the Windows key alone does **not** open ClassicShell's menu by
  default — the tap is only ever observed, so the normal legacy/native
  Start menu opens instead, letting you keep using it alongside
  ClassicShell. Set `CaptureWinKey=1` under `[Experimental]` in
  `classicshell.ini` to have a standalone Win tap open ClassicShell's menu
  instead (see Configuration below) — off by default while this gets
  hardened over time.

  **How the Windows-key handling actually works, and why:** unlike some
  alternative shells (including a previous version of this one), the
  physical Windows-key down and up are never intercepted — they always
  pass straight through to `CallNextHookEx`, unconditionally, every
  time, whether or not `CaptureWinKey` is on. A live hook that swallows a
  key-up without the OS ever seeing a matching release leaves Windows' own
  key-state table believing that key is still held down, which turns every
  following keystroke into a phantom Win+key shortcut until a reboot clears
  it — this exact failure is what took down a previous version (see
  SPEC.md's incident section). Because the tap is only *observed*, not
  blocked, enabling `CaptureWinKey` means the native Start menu (or, on
  some Windows configurations, Search) can genuinely flash open for a
  frame before this app's own menu takes over. Rather than trying to
  prevent that, a `SetWinEventHook` watches for exactly that surface
  becoming foreground and immediately dismisses it with a synthetic
  Escape, handing focus back to this app's own window — a dismiss-after
  instead of a prevent-before. See `tests/wintap_experiment.cpp` for the
  standalone harness this history was worked out with, including the
  earlier (rejected) approaches.

  The taskbar-button click uses the same discipline on the mouse side: the
  physical down/up over the button are swallowed as a matched pair (never
  one without the other), so it can't regress into the same class of bug.

## Building

Requires the MSVC Build Tools (Desktop development with C++) to be
installed. Then just run:

```
build.bat
```

This produces `classicshell.exe` in the same folder. No other files are
required at build time.

## Testing

`starthook.h` holds the logic behind ClassicShell's interception of the
physical Windows key and the taskbar Start-button click — kept free of
`SetWindowsHookEx`/`CallNextHookEx` plumbing so it runs the same in the
shipped app and in a plain console test binary. `classicshell.cpp`'s
keyboard and mouse hooks are thin wrappers around it.

```
tests\run_tests.bat
```

builds and runs `tests\tests.exe` — a small homemade test runner (no
external test library, matching the project's own no-dependencies
philosophy) covering `starthook.h`. Every bug fixed from here forward
should get a regression test alongside it, in the same spirit.

`tests\wintap_experiment.cpp` is a separate, standalone diagnostic —
not a unit test — for anything touching native Start-menu suppression
specifically: it isolates just the keyboard hook, logs every decision
to a console, and requires a real physical Windows-key press to
confirm or rule a theory out, since simulated input can't stand in
for what the OS's own native tap-detection reacts to. Compile and run
it directly (see the comment at the top of the file for the exact
`cl.exe` invocation and how modes are selected) before changing this
behavior again — and, per SPEC.md section 2.3, any change to the real
keyboard/mouse hooks still needs an actual physical Windows-key tap, an
actual physical Ctrl+Esc, and an actual physical click on the taskbar
Start button (including a press-then-drag-off) tried against the built
app before it's called done, not just a passing test suite.

## Configuration

On first launch, ClassicShell writes a fully-commented `classicshell.ini`
next to the exe with default values, so there's always something present to
find and hand-edit — nothing to configure by trial and error. Delete the
file at any time to regenerate the defaults.

```ini
[Appearance]
Scale=0.85        ; overall UI scale, 0.3 - 2.0
StartOpacity=225  ; window opacity on open, 90 - 255

[QuickTools]
Tool1=            ; up to 3 pinned quick-launch buttons
Tool2=            ; one executable name per entry, e.g. calc.exe
Tool3=

[MenuItems]
ThisPC=1          ; every row ClassicShell can show, in the order
Programs=1        ; listed here — reordering these lines reorders
Documents=1       ; the menu. Set any to 0 to hide that row. If
Downloads=1       ; every item ends up disabled, all of them show
Pictures=1        ; anyway rather than leaving an empty menu.
Music=1
ControlPanel=1
Run=1

[Experimental]
CaptureWinKey=0   ; 0 = Windows-key tap opens the native/legacy Start
                  ; menu (default). 1 = a tap opens ClassicShell's own
                  ; menu instead. The taskbar Start button and Ctrl+Esc
                  ; always open ClassicShell's menu either way.
```

`Scale`, `StartOpacity`, `MenuItems`, and `CaptureWinKey` apply the next
time the app launches. `QuickTools` entries refresh live, every time the
menu is opened.

## License

MIT — see [LICENSE](LICENSE).
