# ClassicShell — Product Spec

This document captures the intended behavior of ClassicShell, independent of
any particular implementation. It exists because the previous implementation
(`classicshell.cpp`, committed at `072954a`) drifted from this intent in a
way that caused a serious regression — see **Incident: the stuck Windows
key** below — and is being thrown away. The rebuild starts from the last
tracked-in-git version (`classicstart.cpp` at commit `49c1bd2`) and works
back up to full feature parity against this spec, this time with tests and
the correctness requirements below treated as non-negotiable.

## 1. What this is

A lightweight, classic-style Start menu replacement for Windows: a single
self-contained C++ source file (plus a small testable core split into a
header), no runtime dependencies beyond the OS, no installer, no telemetry.
It should feel as polished and responsive as a commercial alternative shell
(Open-Shell, StartAllBack, StartIsBack, etc.) while staying small enough to
read end to end.

## 2. Non-negotiable correctness requirements

These take priority over every feature below. A feature that can't be built
without violating one of these must be redesigned, not shipped anyway.

### 2.1 Never desync the OS key-state table

**This is the rule that was broken and caused the incident.** The app must
never call a low-level keyboard hook and swallow (return non-zero / skip
`CallNextHookEx`) a key-up for a key whose key-down it did not *also*
swallow, or vice versa, for any key that Windows itself tracks modifier
state for (Win, Ctrl, Shift, Alt). An asymmetric swallow — eating the
release but not the press, or the press but not the release — leaves
Windows believing that key is still physically held, and every subsequent
keystroke gets misread as a shortcut chord until the user reboots.

Concretely, for the Windows key: **the physical key-down and key-up of
VK_LWIN/VK_RWIN must always be forwarded via `CallNextHookEx`, unconditionally,
every single time.** Tap-vs-combo detection is done by *observing* the
down/up sequence, not by intercepting it. The desired behavior (our own
Start menu opens instead of the native one) is achieved by letting the
native surface open and then immediately dismissing it and taking focus
ourselves — see 2.2 — not by preventing the native surface from ever seeing
the keystroke.

If, after implementation, a genuine need to swallow a modifier key event
is discovered, the fix must symmetrically swallow *both* the down and the
up for that same physical press, and must be validated per 2.3 before it's
considered done.

### 2.1b Mouse-hook swallows must also be symmetric

The same rule applies one level down, for the taskbar Start-button click
(2.7): if `WM_LBUTTONDOWN` over the button is swallowed to fire our own
toggle instead of `CallNextHookEx`, the matching `WM_LBUTTONUP` must also
be swallowed, unconditionally, regardless of where the cursor ends up by
release time (press, drag off, release elsewhere). `StartButtonMouseTracker`
in `starthook.h` exists specifically to make this structurally
impossible to get wrong — it is fed only real (non-injected)
`WM_LBUTTONDOWN`/`WM_LBUTTONUP` events, never right/middle-button events,
so an unrelated button can't reset its capture state mid-press. This isn't
as dangerous a failure mode as 2.1 (Windows doesn't track mouse-button
state the way it tracks modifier keys), but it's the same *class* of bug,
so it gets the same discipline rather than relying on it happening to be
harmless.

### 2.2 Suppressing the native Start menu is a dismiss-after, not a
    prevent-before

Because 2.1 forbids intercepting the Win key itself, the native Start menu
(or, on some Windows configurations, Search) may flash open for a frame
before our own menu takes over. Handle this the way the current code's
`DismissNativeStartMenuIfNeeded` already gestures at: watch for the
foreground window becoming one of the known native shell surfaces
(`StartMenuExperienceHost.exe`, `SearchHost.exe`, `ShellExperienceHost.exe`)
while our menu is meant to be showing, and immediately send it a synthetic
Escape and reclaim focus. This must be fast enough that a user doesn't
perceive two menus.

### 2.3 Any fix touching the keyboard/mouse hook must be validated against
    real physical input before being called done

Simulated input (`SendInput`) cannot stand in for what the OS's own native
tap-detection and modifier-state tracking actually react to — this was
already known and documented in the previous version, but the actual
mitigation was never implemented, and nobody caught that gap because it was
never tested against a real key press. Going forward: no change to
`KeyboardProc`/`MouseProc` ships without being exercised by an actual
physical Windows-key tap and an actual physical Ctrl+Esc, watching for (a)
the menu opening correctly and (b) normal typing being unaffected
immediately afterward and after leaving the machine idle for a few minutes.
This can't be a unit test; it's a manual pre-ship checklist item, and it
should stay one — call it out explicitly in the PR/commit description when
done.

### 2.4 Fail safe, not silent

If a future defensive mechanism (e.g. a periodic `GetAsyncKeyState` sanity
poll) is added to detect a desynced modifier state, its correction path
must be a synthetic key-up for the *specific* key found stuck, sent once
and logged — not a broad "release everything" hammer, and not a silent
no-op. Prefer not needing this at all (per 2.1) over needing it and getting
it wrong.

## 3. Feature scope

### 3.1 Menu contents

Classic single-column Start menu with these rows, each independently
toggleable and reorderable via config:

- This PC, Programs, Documents, Downloads, Pictures, Music, Videos, Games,
  Control Panel, Run
- Real Windows-accurate icons for each row
- "Games" resolves to whatever actually has games on the system, falling
  back through a couple of plausible locations rather than assuming one
  exact folder exists
- If every row is disabled in config, show all of them anyway rather than
  an empty menu

### 3.2 Search / Run box

- Resolves typed commands, known folders, and installed apps (including
  packaged/Store apps)
- Wildcard file search: background-indexes a configured folder at startup;
  typing a glob (`*.txt`, `report.*`) or plain text (contains-match) opens a
  translucent panel beside the menu with results to arrow through and
  launch. The main menu's layout must never shift to make room for it.
- Hover preview: hovering a text/plaintext, JSON/XML, or image (PNG, JPEG,
  GIF, BMP, ICO, TIFF, SVG with true alpha) result shows a quick-look panel
  in the screen's opposite corner. Text/JSON/XML sizes itself to content
  (measured against the panel's own font, clamped to a readable range);
  images scale to roughly a quarter of the screen regardless of native
  resolution. Moving into the panel keeps it open and enables wheel-scroll
  for text; losing hover elsewhere fades it out on a short delay.

### 3.3 Power flyout

Restart, Shut down, Sign out, Lock — each with a hover label.

### 3.4 Quick launch

Up to 3 configurable quick-launch buttons, refreshed live every time the
menu opens (no restart needed for these specifically).

### 3.5 Visuals

- Live opacity slider, acrylic blur, rounded corners, real Windows accent
  color, all anti-aliased
- The opacity slider's scroll-to-retarget-any-window behavior (hover +
  scroll cycles it through every other real window on the desktop, with an
  accent-colored outline following along) is a nice-to-have, not a
  correctness requirement — cut first if it's ever in tension with 2.x
- Segoe UI Variable / Segoe Fluent Icons, falling back to Segoe UI / Segoe
  MDL2 Assets on Windows versions without them

### 3.6 Input

- Full keyboard navigation (Tab / Shift+Tab / arrows) with a visible focus
  ring
- Ctrl+Esc opens the menu (chord swallow only — see 2.1, this is safe
  because both halves of a Ctrl+Esc press are ordinary non-modifier-tracked
  keys from the OS's perspective for this purpose)
- A standalone Windows-key tap is **opt-in**, off by default, governed by
  `[Experimental] CaptureWinKey` in the ini (see 3.8). With it off (the
  default), a Win tap is still observed but never acted on, so the
  normal legacy/native Start menu is the only thing that responds to it —
  this is what lets users keep using the native Start menu via the Windows
  key while ClassicShell handles the taskbar click (3.7). With it on, a
  standalone tap opens ClassicShell's own menu instead, using the existing
  observe-and-dismiss-after mechanism (2.2). Either way, Win+key combos
  (Win+D, Win+E, ...) must reach their normal native targets untouched —
  see 2.1 for how this is achieved without ever intercepting the key
  itself, regardless of the flag.

### 3.7 Taskbar Start button

Left-clicking the taskbar Start button hijacks into ClassicShell's own
menu — this is the primary, always-on way to open it, independent of the
`CaptureWinKey` experimental flag in 3.6. Right-click and middle-click on
the button are untouched (matches classic Windows behavior, where only the
left click opens Start). See 2.1b for the mouse-hook swallow discipline
this requires.

This reverses an earlier version of this document, which forbade any
taskbar-button hit-testing as a deliberate decision. That decision bundled
an unrelated, already-fixed keyboard-hook bug (6) together with the
taskbar button; the hit-testing logic itself (`IsStartButtonHit`) was
never the source of the incident, had its own passing tests, and is
restored here with the missing symmetric-swallow half (2.1b) it was
missing before.

### 3.8 Lifecycle

- Single-instance: launching again cleanly replaces any running copy, with
  no leftover hook or window from the old instance
- Config: on first launch, write a fully-commented `classicshell.ini` next
  to the exe with defaults. Delete it anytime to regenerate. `Scale`,
  `StartOpacity`, `MenuItems`, `Search`, and `[Experimental] CaptureWinKey`
  apply next launch; `QuickTools` refreshes live.
- `[Experimental] CaptureWinKey` (default `0`): see 3.6. Read once at
  startup, same as `Scale`/`StartOpacity`/`MenuItems` — not a live-reload
  setting like `QuickTools`.

## 4. Robustness & testing

- `starthook.h`-style separation: pure, hook-plumbing-free logic
  (state machines, rect math, string matching) lives in headers that
  compile into a plain console test binary, independent of
  `SetWindowsHookEx`. `classicshell.cpp`'s actual hooks are thin wrappers
  around that logic.
- Every bug fixed from here forward gets a regression test alongside it in
  `tests/`, in the spirit already established.
- No external test dependency — the existing homemade `mini_test.h` runner
  is fine to keep.
- Given 2.3, the test suite is necessary but not sufficient for anything
  touching the keyboard/mouse hooks specifically — those need the manual
  physical-input checklist every time, not instead of tests but in addition
  to them.

## 5. Explicitly out of scope

- No installer, no auto-update, no telemetry/network access of any kind
- No dependency beyond the Windows SDK / MSVC toolchain (matches the
  project's existing no-dependencies philosophy)
- No support for anything older than the Windows versions the fallback
  fonts already imply (font fallback exists specifically so a floor version
  doesn't need to be pinned any harder than that)

## 6. Incident: the stuck Windows key

For context on why section 2 exists at all: the previous implementation's
`KeyboardProc` swallowed the physical Windows-key key-up on every
standalone tap (`return 1`, skipping `CallNextHookEx`) and never sent a
replacement key-up — despite the README documenting a "~700ms deferred
synthetic replacement key-up" as the supposed fix. That fix was designed
and written up in prose but never actually implemented in code; nobody
caught the gap because it was never validated against a real physical key
press (see 2.3). The result: Windows' own key-state table permanently
believed the Windows key was held down, and every keystroke afterward was
misread as a Win+key shortcut, requiring a reboot to clear. The previous
committed version (`classicstart.cpp` at `49c1bd2`, the new base for this
rebuild) has the same architectural gap — it just hadn't surfaced as
reliably — which is why 2.1 mandates a design that structurally can't have
this failure mode, rather than a better-tested version of the same
eat-and-replace approach.

**Postscript:** an earlier revision of this document also pulled the
taskbar Start-button hit-testing (`IsStartButtonHit`) out of scope
entirely, bundling it in with the incident above as if it shared the same
root cause. It didn't — the button-click logic was pure rect math with
its own passing tests, and the actual bug was entirely in `KeyboardProc`'s
handling of the physical Windows key. The one real gap on the mouse side
was an asymmetric swallow of its own (down eaten, matching up never
explicitly handled) that happened to be harmless by luck rather than by
design. 2.1b and `StartButtonMouseTracker` close that gap explicitly, and
3.7 now restores the click-hijack as the default rather than repeating the
overcorrection.
