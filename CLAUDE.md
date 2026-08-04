# flipbook

A sprite sheet player — a page of stills, cut into a grid and played as an
animation — as **two** FFGL plugins for Resolume Arena/Avenue: a source
(`Flipbook`) that decodes a sheet from a file, and an effect (`Flipbook Over`)
that plays one over the incoming clip or takes that clip as the sheet. C++/GLSL,
CMake MODULE → universal `.bundle` (macOS) + Windows `.dll`. Public MIT repo.

Read `AGENTS.md` before changing the grid arithmetic, the coordinate conventions
or the sampling.

## Commands (CMake)
- Configure: `cmake -B build -DCMAKE_BUILD_TYPE=Release`
- Fast dev build: add `-DCMAKE_OSX_ARCHITECTURES=arm64`
- Build: `cmake --build build`
- Install both bundles to Resolume: `cmake --install build`
- Render a frame offline: `./build/fbtest --out /tmp/frame.png --file docs/example-sheet.png --columns 5 --rows 4 --time 1.25`
- The effect over a test clip: `./build/fbtest --effect --out /tmp/over.png`
- What a sheet decodes to: `./build/fbtest --sheet docs/example-sheet.png --columns 5 --rows 4`
- List parameters, with types, defaults and ranges: `./build/fbtest --list`
- Set anything by name: `--set "Copies=9" --set "Arrange=1" --set "Stagger=0.3"`
- Write the synthetic test sheet: `./build/fbtest --make-sheet /tmp/s.png` (`--detail` for the sweep's)
- Regenerate the example sheet: `python3 tools/make_example_sheet.py`

## OpenFX build
- `source/ofx/FlipbookOFX.cpp` → `build/Flipbook.ofx.bundle` (target `FlipbookOFX`,
  `-DBUILD_OFX=OFF` to skip): **both** plugins in one bundle —
  `com.stoatworks.flipbook` (generator) and `com.stoatworks.flipbookover` (filter).
- Sheet.cpp, Playback.cpp, Placement.cpp and Controls.cpp are linked straight
  from source (still one home). Only the fragment shader's per-pixel half — the
  cell fetch with its inset, the key, the composite — is mirrored. Change the
  shader's inset, key or blend, change this too.
- Sync offers Free and Manual only: OFX hosts carry no tempo. Manual is the mode
  for keyframing Phase against the edit.
- OFX time arrives in *frames*; the plugin divides by the clip frame rate to get
  the seconds `FrameClock` wants.
- Smoke test (ofxprobe drives the Filter context; the generator's render runs
  only in a real host). **`--set-string` is required** — without the sheet, every
  numeric setting measures an empty frame that renders perfectly and draws nothing:
  ```
  ../resolume-ofx-bridge/build/ofxprobe --dir build --render com.stoatworks.flipbookover \
    --size 480x270 --out /tmp/f.bmp --set-string "sheetFile=$PWD/docs/example-sheet.png" \
    --set "columns=5" --set "rows=4" --set "fit=2"
  ```
- OFX SDK subset (BSD-3) vendored under `external/openfx`.
- Install for Resolve: copy the bundle into `/Library/OFX/Plugins`.

## Verify
- Everything: `tools/verify.sh`
- The cell on screen, against `Playback.cpp`: `./build/fbtest --frames`
- Where every copy landed, against `Placement.cpp`: `./build/fbtest --copies`
- A square cell stays square off 1:1: `./build/fbtest --aspect`
- No cell bleeds into its neighbour: `./build/fbtest --seam`
- The four key modes: `./build/fbtest --key`
- No dead controls: `python3 tools/sweep.py`

## Notes
- **The frame is a pure function of (clock, parameters).** No counter, nothing
  advanced. That is what makes the loop point land on the bar line whatever the
  host's frame rate does.
- **No GLSL mirror of anything.** Placement and frame selection are per copy,
  not per pixel, so they are solved once on the CPU and uploaded as two `vec4`
  arrays. The OFX build mirrors only the per-pixel half.
- **The half-texel inset is load-bearing.** Without it every sprite carries a
  one-pixel seam of its neighbour. `--seam` is the test.
- **The sheet is not mipmapped, deliberately** — a mip level averages across
  cell boundaries. See `AGENTS.md`.
- **Two coordinate conventions.** Copies are placed in frame space (0..1 per
  axis, y down); sprites are sized in short-edge fractions. Same number only at
  1:1 — `--aspect` is the test for exactly this.
- **`SetParamInfo` clamps a default only for `FF_TYPE_STANDARD`.** The counts
  (Columns, Rows, Start Frame, Frame Count, Copies) are real `FF_TYPE_INTEGER`
  parameters with real ranges. **Option parameters hold the element value**, not
  0..1, and are read through `Option()`.
- **Straight alpha until the last line of the fragment shader.** Keying a
  premultiplied pixel compares the swatch against `rgb * a`.
- The filters live on a **sampler object**: the input-as-sheet mode borrows the
  host's texture and must not modify it.
- The GLSL declares `Xform[64]` and `Cell[64]` as literals; a `static_assert`
  keeps `kMaxCopies` in step.
- `flipbook_core` is an **OBJECT** library, and each plugin's registration is
  listed directly in its own target — see `AGENTS.md`. `verify.sh` checks it.
- macOS build must be universal (arm64 + x86_64). Verify with `lipo`, never the
  build log.
- `flat`, `active`, `filter`, `input`, `output`, `sample`, `common` are GLSL
  reserved words. Shader errors surface only at runtime, in the diagnostics log.
- Public repo. "Commit" = commit **and** push.

## Diagnostics

`source/Diag.{h,cpp}` — log file only, no crash handler (this runs inside
Resolume), no bundle command. It covers the failures that all look identical
from outside ("it does nothing"): a sheet that would not load and why, a grid
that does not divide the sheet exactly, and a shader that would not compile.

    ~/Library/Logs/flipbook/flipbook.YYYY-MM-DD.log
