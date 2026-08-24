# Notes

Working notes for this repo: status, decisions, and the traps that have actually bitten.
Migrated out of Claude Code's memory on 2026-08-24, so they are written in the first
person and dated by when each thing was learned — that date is usually the useful part.

Cross-cutting notes that are not specific to this repo live in
[fleet-notes](https://github.com/stoatworks-labs/fleet-notes).

*flipbook — sprite sheet player: two FFGL plugins + OpenFX; PUBLIC MIT v0.1.0 RELEASED with video, live web demo and Instagram post; verified offline only, never loaded into Resolume or Resolve*

**flipbook** (started 2026-08-04) — a page of stills, cut into a grid and played
as an animation. `~/Projects/flipbook`, **PUBLIC MIT**, repo
`stoatworks-labs/flipbook`, **v0.1.0** tagged 2026-08-04.

**Two FFGL plugins from one class** (the fleet's usual shape): `Flipbook` is a
source that decodes a sheet from a file via `SetFileParamInfo` + vendored
`stb_image`; `Flipbook Over` is an effect that plays one over the incoming clip
**or takes that clip as the sheet** ("Sheet From: Input Clip"). Plus an OpenFX
bundle carrying both, CPU render, for Resolve/Nuke/Natron/Vegas.

**The one idea:** the frame on screen is a pure function of (clock, parameters).
No counter, nothing advanced. It matters more here than in orrery or downpour
because a sprite sheet has a *right answer* — twelve frames at 12 fps is one
second, and a loop cued against a bar has to land on the bar line whatever
Resolume's frame rate does that night.

**The trap that defines the plugin: the half-texel inset.** Sample a cell at its
boundary with `GL_LINEAR` and the hardware blends in the neighbouring sprite's
first texel — a one-pixel seam that reads as a decode fault. `Inset` clamps the
sample into the cell. Consequence: **the sheet is deliberately NOT mipmapped** —
a mip level averages across cell boundaries, and unlike downpour's atlas there
is no padding to prevent it, because the layout belongs to whoever exported the
sheet. Adding `glGenerateMipmap` is the obvious "improvement" and would
contaminate every sprite at every size.

**Straight alpha until the last line of the fragment shader.** Premultiplying at
upload would make the colour key compare the operator's swatch against `rgb * a`.

Sheet v runs **top-down** (our own upload); the incoming clip's runs bottom-up —
one `SheetFlipV` uniform.

Filters live on a **sampler object**, not the texture: input-as-sheet borrows the
host's texture and must not modify it.

Copies (≤64) are placed on the CPU and uploaded as two `vec4` arrays, so **there
is no GLSL mirror of any arithmetic** — only the OFX build mirrors the per-pixel
half (cell fetch, key, composite). `Stagger` offsets each copy's position in the
run, which is the control worth finding first: it turns a field of sprites into a
chase travelling through them.

**Verified offline only — never loaded into Resolume or Resolve.** `tools/fbtest`
drives the real plugin class headless: `--frames` (cell on screen vs
Playback.cpp, 3 modes + a run wrapping off the end), `--copies` (every copy
against Placement.cpp's prediction, plus "a staggered field shows >1 distinct
frame"), `--aspect`, `--seam` (the inset), `--key`. `tools/sweep.py` sweeps all
39 params, none dead. Open host questions: whether Resolume renders
`FF_TYPE_INTEGER` as typed spinners (see [ffgl sdk bugs](https://github.com/stoatworks-labs/fleet-notes/blob/main/notes/reference_ffgl_sdk_bugs.md)), whether
Beat/Bar lock to a real transport, and how usable input-as-sheet is with a real
clip transform in the way.

`docs/example-sheet.png` is an original 5×4 burst from
`tools/make_example_sheet.py` — generated so nothing here depends on somebody
else's artwork, and shipped in the release so the plugin has something to play
on first run.

Deliberately not done: sidecar TexturePacker/Aseprite JSON (Start Frame + Frame
Count cover it manually), per-copy rotation, motion (that is orrery's job),
reload-on-change.

Related: [downpour](https://github.com/stoatworks-labs/downpour/blob/main/docs/NOTES.md) (`downpour`) and [orrery](https://github.com/stoatworks-labs/orrery/blob/main/docs/NOTES.md) (`orrery`) (CMake, harness, Diag and
preset patterns came from there), [plugin factory presets](https://github.com/stoatworks-labs/fleet-notes/blob/main/notes/reference_plugin_factory_presets.md),
[new plugin repo copy traps](https://github.com/stoatworks-labs/fleet-notes/blob/main/notes/reference_new_plugin_repo_copy_traps.md).

## Released 2026-08-04, all homes done

Video `https://www.youtube.com/watch?v=k6OB4enGnMo` (rendered in TWO segments
from two cue sheets against two example sheets — the plugin's sprite case and
its full-frame case want opposite settings, so either alone demonstrates half a
plugin). Instagram Reel `https://www.instagram.com/reel/Dbn6mzokXLS/`. Website
card at `stoatworks-labs.com/software/flipbook`.

**Live web demo: `flipbook.stoatworks-labs.com`** — the plugin's playback and
placement ported to WebGL2, and it takes the visitor's **own** sheet by
drag-and-drop, read locally with nothing uploaded. That is the demo: a plugin
whose job is playing somebody else's artwork is best argued for with theirs.
Deployed from `web/` with `cf-run npx wrangler deploy`.

**Clicking through the demo found four things the test suite could not**, which
is the argument for building one:
- Carousel, Swarm and Pixel Nine were framed as though Spread ran 0..1 when it
  runs 0..1.5 — Carousel showed four of its twelve copies. `fbtest --presets`
  now applies every preset through the real dropdown and checks each copy's
  centre against the raster; nothing else could, because `--copies` uses its own
  values and the sweep only asks whether the dropdown changes the picture.
- The "Full Frame" preset used Fit: **Stretch**, which maps a square cell onto
  16:9 and turns a circular tile into an ellipse. It is **Fill** now: identical
  on a sheet whose cells are already the output aspect, and it crops rather than
  distorts on one that is not.

`tools/video.cues` + `video-tiles.cues` drive `fbtest --sequence`; the backend's
`video/projects/flipbook/render.py` cuts them together. **Beat times past 24s
are on the CUT timeline** and carry the first segment's length as an offset —
getting that wrong put "the same chase, on a ring" over a grid, and nothing
catches it.
