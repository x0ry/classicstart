# ClassicShell

A lightweight, classic-style Start menu replacement for Windows — a single
self-contained C++ source file, no runtime dependencies beyond the OS.

![ClassicShell](screenshot.png)

## Features

- Classic single-column Start menu: This PC, Programs, Documents, Downloads,
  Pictures, Music, Videos, Games, Control Panel, and Run, each with real
  Windows-accurate icons — Games resolves to whatever actually has games in
  it on your system, falling back through a couple of locations rather than
  assuming one exact folder exists.
- A search/Run box that resolves typed commands, known folders, and
  installed apps (including packaged/Store apps).
- Wildcard file search: ClassicShell indexes a folder in the background at
  startup, and typing a pattern like `*.txt` or `report.*` — or just plain
  text, matched as a contains-search — pops open a translucent panel
  beside the menu with matching files to arrow through and launch — the
  main menu's own layout never shifts to make room for it.
- Hover preview: resting the mouse on a text/plaintext, JSON/XML, or image
  (PNG, JPEG, GIF, BMP, ICO, TIFF, or SVG — vector-rendered at exact
  preview resolution, with true alpha transparency) search result pops a
  quick-look panel in the screen's opposite corner. The text/JSON/XML panel
  sizes itself to the actual content — measuring the widest line and total
  line count against the panel's own font, then clamping to a readable-but-
  not-huge range — instead of always claiming a fixed fraction of the
  screen; the image panel scales so it reads at roughly a quarter of the
  screen regardless of its native resolution. Moving the mouse into the
  panel keeps it open and lets the wheel scroll a text preview; losing
  hover elsewhere fades it out on a short delay instead of snapping shut.
- Power flyout with Restart, Shut down, Sign out, and Lock, each with a
  hover label.
- Up to 3 configurable quick-launch buttons.
- Live opacity slider, acrylic blur, rounded corners, and the real Windows
  accent color — all anti-aliased. The slider isn't limited to the Start
  menu itself: hover it and scroll to cycle its target through every other
  real window on the desktop (an accent-colored outline follows along to
  show which one), letting you drag the same slider to adjust any other
  app's transparency. Scroll back down to return to the Start menu; closing
  the menu always hands a targeted window's opacity back exactly as found.
- Renders in Segoe UI Variable and Segoe Fluent Icons — the same fonts
  Windows 11's own Start menu and Settings app use — falling back to
  classic Segoe UI / Segoe MDL2 Assets on older Windows versions that
  don't have them installed.
- Full keyboard navigation (Tab / Shift+Tab / arrows) with a visible focus
  ring, and Ctrl+Esc to open.
- Single-instance: launching it again cleanly replaces any running copy.
- Tapping the Windows key alone opens the menu, the same as the native
  Start Menu — and only ours; clicking the taskbar Start button does
  the same. Win+key combos (Win+D, Win+E, ...) pass both the key-down
  and key-up straight through untouched, exactly like the OS would see
  them without ClassicShell running at all. A completed standalone tap
  is different: its physical key-up is eaten rather than forwarded, so
  the native Start menu never sees the real release it needs to open —
  confirmed against real physical key presses, not just simulated
  input, since a same-process `SendInput` can't reliably stand in for
  what the OS's own native tap-detection actually reacts to. Eating a
  real key-up isn't free: with nothing to replace it, Windows is left
  believing the key is still held, turning every following keystroke
  into a phantom Win+key shortcut. The fix is a synthetic replacement
  key-up sent ~700ms later from a background thread, not immediately —
  sent immediately, it retriggers the very native menu it's meant to
  suppress (whatever's watching the release doesn't appear to
  discriminate real input from injected), but by ~700ms out, the
  native tap-detector's own eligibility window has apparently already
  closed, so the late arrival clears Windows' key-state tracking
  unnoticed. See `tests/wintap_experiment.cpp` for the standalone
  harness this was worked out with.

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
physical Windows key and the taskbar Start button — kept free of
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
behavior again.

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
Videos=1
Games=1
ControlPanel=1
Run=1

[Search]
IndexPath=C:\Users\you  ; folder indexed in the background for wildcard
                        ; search; defaults to your profile folder
```

`Scale`, `StartOpacity`, `MenuItems`, and `Search` apply the next time the
app launches. `QuickTools` entries refresh live, every time the menu is
opened.

## License

MIT — see [LICENSE](LICENSE).
