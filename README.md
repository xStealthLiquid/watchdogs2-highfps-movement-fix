# Watch Dogs 2 – High FPS Movement Fix

Fixes the framerate-dependent character **strafe sliding** that occurs above 60 FPS in Watch Dogs 2 — without capping your framerate. High-FPS movement feels exactly as tight as 60 FPS again.

An ASI plugin (loaded via [Ultimate ASI Loader](https://github.com/ThirteenAG/Ultimate-ASI-Loader)). **Singleplayer only.**

## The problem

Watch Dogs 2 (Ubisoft's Disrupt engine, Havok physics) steps the player's Havok character controller **once per rendered frame**. At 120 FPS it runs twice as often as it was tuned for, so the per-frame velocity/acceleration limit "over-charges" — the classic Quake/Source *strafe* bug. The result is a sideways slide that scales with your framerate (measured: ~31 % lateral spread at 60 FPS vs. ~71 % at 120 FPS).

## The fix

A trampoline hook on the character-step function re-times **only the player's** physics step to a fixed **60 Hz** using an accumulator, decoupled from the render framerate. The engine's own interpolation keeps the picture smooth at full FPS. NPCs, animals and vehicles are left untouched.

- Framerate-independent (correct at 60 / 75 / 90 / 120 / 144+ FPS; handles fluctuating FPS and lag spikes)
- Real movement speed preserved (no slow-motion / floaty feel)
- At 60 FPS effectively inactive
- The player is detected at runtime by **movement synchrony** (bodies that move exactly with your transform component), so nearby NPCs/animals are never mistaken for the player — no config, no hardcoded offsets
- The hook site is found via a unique AOB signature (version-robust)

See the source ([`src/wd2fix.c`](src/wd2fix.c)) — it's thoroughly commented.

## Building

Compiled with [Zig](https://ziglang.org/) as a portable C compiler (no MSVC/Visual Studio needed):

```sh
zig cc -shared -target x86_64-windows-gnu -O2 -Wall -o wd2fix.asi src/wd2fix.c -lpsapi -luser32
```

The output `wd2fix.asi` is a standard Windows DLL renamed to `.asi`.

## Installing

1. Set the Steam launch option `-eac_launcher` (disables EasyAntiCheat → allows mods; also disables online).
2. Install the [Ultimate ASI Loader](https://github.com/ThirteenAG/Ultimate-ASI-Loader) — put its `dinput8.dll` into `...\steamapps\common\Watch_Dogs2\bin\`.
3. Put `wd2fix.asi` into that same `bin\` folder.
4. Launch the game. The fix is on automatically; **F1** toggles it.

## ⚠️ Singleplayer only — do not use online

This injects a DLL and patches game code in memory. Watch Dogs 2's online modes use EasyAntiCheat, which does not distinguish between harmless and cheating mods — any in-memory modification during an online session can get you **permanently banned from the online features**. With `-eac_launcher` set, online is disabled anyway. To play online, remove `wd2fix.asi` and the `-eac_launcher` option.

## Credits

- ASI loading: [Ultimate ASI Loader](https://github.com/ThirteenAG/Ultimate-ASI-Loader) by ThirteenAG.
- Fix built by reverse engineering the Disrupt/Havok character controller.
