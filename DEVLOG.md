# Devlog — porting OMF:2097 to the 3DO

Notes from bringing *One Must Fall: 2097* (a 1995 DOS fighting game) to the
Panasonic/3DO, on top of [OpenOMF](https://github.com/omf2097/openomf). Less a
diary, more a collection of war stories — the things you only learn when you
aim a modern reimplementation at a 32-bit console from 1993.

If you're porting anything to the 3DO, hopefully some of these save you a day.

---

## Memory: the console is smaller than your moveset

**306 CELs cost 707 KB of RAM but only 272 KB on disk.** The 3DO protects memory
in pages, and every `LoadCel` allocation gets rounded up to a page boundary. A
HAR (fighting robot) moveset is ~306 little sprites; loaded one-by-one that's a
2.6× blow-up from page rounding alone — enough to bust the budget with a single
fighter, never mind two. The fix was to pack a whole moveset into one **atlas**:
a single `LoadFile` of a concatenated pixel blob + a CCB array we point into.
Same pixels, ~440 KB saved per fighter, and it loads as one sequential read
instead of 306 seeks.

**The budget is DRAM + VRAM ≈ 1.2 MB, and CELs spill into VRAM.** Two full
symmetric fighters only fit after the atlas work — and we load just the one
arena that's actually being fought in, not all five.

**Hit masks: 1 bit per pixel.** Per-pixel hit detection wants a mask per frame;
stored as bytes that's ~812 KB. Packed to 1bpp it's ~100 KB. On a 1 MB machine
that difference is the whole feature shipping or not.

---

## Rendering: the CEL engine has sharp edges

**`CCB_NPABS` is not optional.** When you build an atlas of CCBs that share one
pixel buffer, each CCB's data pointer must be flagged absolute (`CCB_NPABS`).
Forget it and the render loop silently walks off after the first cel — in our
case Player 1 drew fine and *everything after it* (Player 2, health bars) was
invisible. No crash, just a half-rendered screen. Hours of "why is P2 a ghost."

**A coded CEL's palette (PLUT) holds at most 32 colours.** This one reshaped a
whole feature. OMF gives each robot three recolourable colour zones (16 shades
each) so every pilot has their own livery. On the 3DO, a *coded* CEL — the only
kind you can recolour at runtime by swapping a palette pointer — indexes a
32-entry PLUT. Three distinct zones push most frames past 32 colours, forcing
uncoded 16-bit CELs and ~2.4 MB for two fighters against a 1.2 MB budget. So we
recolour only the **primary** zone per pilot and leave the rest neutral grey.
Crystal reads as cobalt blue, Jean-Paul as red; Raven's black-and-gold can't be
fully expressed. A genuine hardware ceiling, not a shortcut — full write-up in
[PALETTE_NOTES.md](port_3do/PALETTE_NOTES.md).

**Skip the PLUT's 4-byte count word.** A 3DO `PLUT` chunk is `[count][colours…]`.
`ccb_PLUTPtr` must point at the *colours*, not the count — point it 4 bytes too
early and every colour shifts by one slot, painting the whole sprite in the
wrong palette. Subtle because it still "looks like a palette," just wrong.

**Watch the converter's stride fields.** The asset tool (3it) emits per-frame
`HDX`/`VDY` that already encode a *packed* stride. Override them naively to
"fix" positioning and you get gloriously enormous pixels — the engine multiplies
your value by the packed stride again.

---

## No floating point. At all.

We link the firmware with `-noscanlib`, so there's **no soft-float library** — a
single `float` multiply fails at *link* time with an undefined `_fmul`/`_fadd`.
The original game's physics (knockback, jump arcs, friction) are full of floats.
Every one became **fixed-point ×256** integer math at runtime. It's the kind of
constraint you discover not from docs but from a linker error referencing a
symbol you never wrote.

---

## Audio: silent failures everywhere

**`ehSetChannelLevels` divides your volume by the channel count.** Our sound
effects were dead silent for a whole session. The 3DO audio folio splits the
level you set across the channels a mixer uses — so a stereo SFX at "full"
volume comes out at half, and our multi-channel setup drove it toward zero. The
fix is to *pre-multiply* the level by the number of channels. Nothing logs an
error; it just whispers.

**Start the music before you load big art.** `MusicStart` has to run *before*
the large `LoadCel`/`LoadFile` calls, or the SoundPlayer silently never starts.
Load ordering, not a flag — reorder two lines and the arena has a soundtrack.

---

## Toolchain: 1993 console, 2026 laptop

**The ARM linker can't read your Windows drive.** `armlink` (ARM SDT 2.51, via
the [Trapexit devkit](https://github.com/trapexit/3do-devkit)) refuses to open
object files that live on a WSL `/mnt/d` DrvFS mount — reports "file not found"
for files every other tool reads fine. The build mirrors the tree into a
WSL-native dir first, links there, copies the artifacts back. Transparent once
you know; baffling until you do.

**`3doiso` allocates a big block per file — keep intermediates out of the image.**
The very last bug before publishing: a clean rebuild produced a **1.7 GB** ISO
instead of ~95 MB. The filesystem was only ~100 MB, but the asset pipeline had
left ~600 tiny per-frame intermediate CELs in the staged tree, and the ISO tool
reserves a large allocation unit per file. Six hundred 1 KB files don't cost
600 KB — they cost gigabytes. Staging intermediates in `/tmp` and shipping only
the consolidated atlases brought it back to 93 MB.

---

## The emulator is a ceiling, not a mirror

Early feasibility numbers came from [Opera](https://github.com/libretro/opera-libretro).
Useful to prove the CEL engine has headroom for OMF's workload — but Opera is an
*optimistic* bound. A real FZ-10 is noticeably slower, so "comfortable on Opera"
means "budget carefully for hardware." Every memory and timing decision was made
against the real-hardware assumption, not the emulator's.

## Confirmed on real hardware (FZ-10)

It runs — on an actual Panasonic FZ-10, off a burned disc, start to finish. The
build is *substantially identical* to Opera: same scenes, same fight, same
colours. The honest differences, for the record:

- **Lower framerate.** Perceptibly below Opera — slower than even the original
  DOS game's "slowest" speed setting. Still playable, but no longer brisk.
- **It doesn't just run slow, it micro-stutters** — a constant slow-down-and-
  recover rhythm rather than a steady lower framerate. (A future optimization
  target: this smells like per-frame allocation / cache pressure, not raw fill
  rate.)
- **Loads are a touch longer**, and the music *stutters during loading* — the
  most annoying artifact of the lot.

None of which is surprising given the "Opera is a ceiling" thesis above — but
it's one thing to assume it and another to feel it on the real thing. The
takeaway: it's a genuine, playable 1-on-1 fighter on 1993 hardware, with a clear
performance-polish runway ahead.

---

*Build it yourself (bring your own OMF:2097 data): see the [README](README.md).*
