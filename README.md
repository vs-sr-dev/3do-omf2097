# OMF2097-3DO

A port of **One Must Fall: 2097** to the **Panasonic/3DO** (Real3DO), built on
top of the [OpenOMF](https://github.com/omf2097/openomf) reimplementation.

It started as a feasibility experiment ("can a 3DO actually run OMF's CEL engine
at a playable rate?") and grew into a real, if minimal, playable build.

📓 **[DEVLOG.md](DEVLOG.md)** — war stories on the 3DO hardware quirks this port
ran into (32-colour PLUT cap, no float runtime, the ISO tool that ballooned a
95 MB image to 1.7 GB, …). Probably the most fun read here.

> **No game data is included in this repository.** OMF:2097 is © Diversions
> Entertainment; you must supply your own copy of the original game files. See
> [Assets & legal](#assets--legal).

---

## Status — 0.1 "playable 1v1" (honest caveats)

This is an early milestone, not a finished game. What it **does**:

- A complete **1-on-1 match vs the AI**: pick a pilot + HAR, VS screen, fight,
  KO → **YOU WIN / YOU LOSE** → back to menu.
- Both fighters are real: walk / crouch / jump, ground attacks (data-driven
  input grammar), throws, air attacks, launchers; **bidirectional** damage.
- Hit feel: clang + hit-pause + source-derived knockback; health bars with
  canon damage; SFX; per-arena music.
- Per-pilot **primary armour colour** in battle (e.g. Crystal = cobalt blue,
  Jean-Paul = red) — see the hardware caveat below.
- Full scene flow: splash → menu → pilot/HAR select → VS → battle → menu.
- A heuristic AI (spacing, timed attacks with reaction delay, jump-ins, light
  evasion).

What it **does not** do yet (known, intentional for 0.1):

- **2 of 10 HARs** are wired as fighters (Jaguar, Thorn). The rest aren't ported.
- **Single round** only — no best-of-three, no round counter.
- **No victory animation / scrap pose** — the match cuts to the win/lose text.
- **No "screws/debris"** flying off a hit HAR, no impact sparks (cosmetic).
- **AI does not block.**
- **No endurance/stun bar** — in OMF a second bar depletes and staggers the
  opponent when empty; not implemented yet (only the health bar + KO are).
- **No arena hazards** (the environmental damage some arenas inflict).
- No **"FIGHT!" voice** (the audio path was silent on the emulator; deferred).
- Network / settings / language / mech-lab menus are **out of scope** by design.

### Per-pilot colour — a real 3DO hardware limit

Only each HAR's **primary** colour zone is recoloured per pilot; secondary and
tertiary zones stay neutral grey. This is **not** laziness — a 3DO *coded* CEL's
palette (PLUT) caps at **32 colours**, and three distinct OMF colour zones push
most frames past that, which would blow the DRAM budget (~2.4 MB vs ~1.2 MB for
two fighters). Recolouring one zone keeps every frame ≤32 colours and fits.

Consequence: pilots whose identity *is* their primary colour read well; pilots
built on a two-colour contrast (e.g. Raven's black-and-gold) lose some character.
Full write-up and possible future fixes: [port_3do/PALETTE_NOTES.md](port_3do/PALETTE_NOTES.md).

---

## Assets & legal

This repo contains **only original source code**. To build a runnable ISO you
must locally provide things that are **not ours to redistribute**:

1. **OMF:2097 game data** — your own copy of the original files (`FIGHTR*.AF`,
   `ARENA*.BK`, `ALTPALS.DAT`, `SOUNDS.DAT`, `CHARSMAL.DAT`, `MELEE.BK`, …).
   OMF:2097 is freeware since 1999 but still © Diversions Entertainment;
   "freeware" ≠ redistributable. The pipeline reads these to generate the CELs,
   atlases, palettes, audio and `*_data.h` headers (all `.gitignore`d).
2. **3DO system boot files** — `takeme/System/` (Kernel, Folios, Drivers, …)
   comes from the [Trapexit 3do-devkit](https://github.com/trapexit/3do-devkit),
   © The 3DO Company. Copy it from the devkit; it is not committed here.

[OpenOMF](https://github.com/omf2097/openomf) itself is **MIT-licensed** and is
vendored under [`openomf-master/`](openomf-master/) (unmodified). We link a
subset of its `.c` files in the host tools and firmware and port a few systems
(`intersect.c`, har/player logic). See [THIRD_PARTY.md](THIRD_PARTY.md).

The port's own code is MIT — see [LICENSE](LICENSE).

---

## Building

> Primary toolflow is **WSL (Ubuntu) + the Trapexit 3do-devkit**, driven from
> Windows by `port_3do/build.ps1`. A `Dockerfile` is provided as an alternative.

### Prerequisites

- WSL Ubuntu (or Linux) with `gcc`, `make`, `rsync`, `libpng`, `python3`.
- [Trapexit 3do-devkit](https://github.com/trapexit/3do-devkit) at `~/3do-devkit`
  (override with `OMF3DO_DEVKIT_PATH`). Provides `armcc`/`armlink`, `3it`,
  `3doiso`, `modbin`, `3DOEncrypt`, and the `System/` boot files.
- Your own OMF:2097 data directory.

### 1. Point the pipeline at your OMF data

```bash
export OMF_DATA=/path/to/your/OMF      # required by every pipeline script
```

### 2. Regenerate the assets + data headers (one-time, from your OMF data)

One command rebuilds every `.gitignore`d artifact (the `*_data.h`/`*_meta.h`
headers the firmware `#include`s, plus the CELs/atlases/palettes/audio):

```bash
cd port_3do
./host_tools/regen_assets.sh
```

It runs all the individual generators in [`port_3do/host_tools/`](port_3do/host_tools/)
in the right order (HAR movesets + `*_meta.h`, pilots, portraits, font, idles,
VS sheet, intro, insults, per-pilot primary palettes, arena/VS backgrounds, HUD,
and — if `ffmpeg`+`libopenmpt` is present — music). The header steps are
mandatory to compile; music/background steps warn if a tool is missing.

> Verified 2026-06-03: a clean `git clone` + `regen_assets.sh` + the devkit
> regenerates all 14 required headers and the firmware compiles and links
> (`make launchme` → `takeme/LaunchMe`).

Then copy the devkit's `System/` boot files into `port_3do/takeme/System/` (the
3DO OS image — © The 3DO Company, not shipped here).

### 3. Build the ISO

```powershell
# from Windows, in port_3do/
.\build.ps1            # builds takeme/LaunchMe + iso/omf2097.iso (encrypted)
.\build.ps1 clean
```

Run `iso/omf2097.iso` in a 3DO emulator (e.g. RetroArch + Opera) or on real
hardware.

---

## Credits

- **OpenOMF** — the reimplementation this port derives from (MIT). Thanks to the
  OpenOMF authors; their license and notice are in [THIRD_PARTY.md](THIRD_PARTY.md).
- **One Must Fall: 2097** — © Diversions Entertainment / Epic MegaGames.
- **Trapexit 3do-devkit** — the open 3DO toolchain that makes this buildable.

This is a fan project, not affiliated with or endorsed by any of the above.
