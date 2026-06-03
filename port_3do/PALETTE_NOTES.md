# Per-pilot fight palettes — design note & hardware limitation

**Status:** landed 2026-06-03 as **primary-zone recolor**. Each fighter's
*primary* armor is tinted to its pilot's canon main colour at battle time
(CRYSTAL Jaguar = cobalt blue, Jean-Paul Thorn = red, …). Secondary, tertiary
and body parts stay on the shared neutral grey ramp.

## Why only the primary zone (the 3DO hardware limit)

OMF:2097 gives every HAR three remappable colour *zones* (primary / secondary /
tertiary), each a 16-shade ramp, swapped per pilot. On the DOS original this is
free — it runs in an 8-bit, 256-colour paletted framebuffer.

The 3DO renders sprites as **CEL**s. A *coded* CEL (the only kind whose colours
can be remapped at runtime via a PLUT pointer swap, which is how we recolour per
pilot without shipping a separate copy of every frame per pilot) indexes a
**PLUT that holds at most 32 colours**. That is a fixed width of the CEL engine,
not a tool limit.

Measured on the real assets:

| | distinct colours in the worst frame | frames needing >32 colours |
|---|---|---|
| Jaguar, all 3 zones distinct | 47 | 185 / 306 |
| Thorn, all 3 zones distinct | 48 | 197 / 299 |

So keeping all three zones distinct makes most frames exceed the 32-colour coded
PLUT, forcing 8-bit/uncoded CELs. That balloons two resident fighters to about
**2.4 MB**, against a DRAM+VRAM budget of roughly **1.2 MB**. It does not fit,
and no amount of effort changes the PLUT width. (Baking a separate atlas per
pilot does not help either — a baked frame with three real zone colours still
exceeds 32 colours per frame.)

Collapsing two of the three zones back onto the shared grey ramp brings every
frame to **≤32 colours** (measured max exactly 32, zero frames over), so the
frames stay coded and small. Two fighters then cost about **791 KB** resident —
fits comfortably. We recolour the one zone that carries most of a HAR's identity:
the **primary**.

## Consequence for players

Pilots whose identity *is* their primary colour read correctly
(CRYSTAL → blue, STEFFAN → white, MILANO → beige, JEAN-PAUL → red).

Pilots built on a **two-colour contrast** lose some character: e.g. RAVEN's
black-and-gold reads as a single hue here, because only one zone is coloured.
This is a known limitation of the 3DO target, not a bug.

## Possible future improvements (not yet done)

- **Per-HAR identity zone:** recolour whichever single zone best carries that
  HAR's canon look instead of always the primary. Cheapest option; the distinct
  zone is fixed per shared atlas, so it's a per-HAR (not per-pilot) choice.
- **Two zones at reduced shading:** quantise each zone's 16 levels to ~10 so two
  zones fit in 32 colours (3×10 ≈ 30). Costs gradient smoothness.
- **Two-layer CCB:** render a primary layer and a secondary layer as separate
  coded CELs. Doubles CCB count and DRAM.

## How it's produced (pipeline)

1. `dump_har dump-all-sentinel` re-extracts the sprites with the primary zone
   (palette indices 32–47) painted in 47 injective "sentinel" colours so 3it
   keeps them as distinct PLUT entries.
2. `build_har_atlas_sentinel.sh` converts each frame to a packed coded CEL
   (4bpp, falling back to 6bpp) and builds the `.ATL`.
3. `dump_har gen-pluts` emits one `.PAL` per pilot, recolouring the primary
   sentinels to that pilot's `color_1` ramp.
4. At runtime `AtlasApplyPilotPlut` repoints each CCB's PLUT at the pilot `.PAL`.

Diagnostics used to size this live in `host_tools/`: `pal_diag.c` (zone / colour
counts), `verify_pal.py` (offline recolour check).
