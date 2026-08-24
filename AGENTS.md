# flipbook — orientation for another LLM (or a newcomer)

**What it is:** a sprite sheet player — a page of stills, cut into a grid and
played as an animation — as **two** FFGL 2.1 plugins for Resolume Arena/Avenue,
plus an OpenFX build of both for Resolve/Nuke/Natron/Vegas. `Flipbook` is a
source that decodes a sheet from a file; `Flipbook Over` is an effect that plays
one over the incoming clip, or takes that clip *as* the sheet. C++17 + GLSL 4.1,
CMake, universal macOS `.bundle` and a Windows `.dll`. Public, MIT,
`github.com/stoatworks-labs/flipbook`.

`CLAUDE.md` is the command reference — build, install, verify. This file is the
*why*: read it before touching the grid arithmetic, the coordinate conventions
or the sampling.

---

## The one idea

**The frame on screen is a pure function of (clock, parameters).**

There is no "current frame" anywhere, nothing is advanced and nothing
accumulates. The fleet's usual invariant, and it earns its place here for the
usual reasons — the harness renders frame 3.25 cold, beat sync is a different
clock rather than a second code path, any output resolution gives the same
picture. But it earns it hardest for one reason specific to this plugin:

**A sprite sheet has a right answer.** Twelve frames at twelve frames a second
is one second of animation, and an operator who cues that against a bar expects
the loop point to land on the bar line every time. A plugin that advanced a
counter per rendered frame would run at whatever rate Resolume managed that
night — fine in the preview, a quarter-frame short per bar once the show gets
heavy, and visibly adrift by the end of a track. Closed-form, the loop point is
arithmetic on the host's clock and cannot drift whatever the frame rate does.

### What falls out of it

**There is no GLSL mirror of anything, and no `//= mirrored` markers.** downpour
carries its rain maths twice — once in C++ and once in the shader — with a test
whose only job is catching the two copies drifting apart. It has to, because a
cell there is a function of *every pixel*. Nothing here is: placement is per
copy and there are at most 64, frame selection is one integer per copy. All of
it is solved once on the CPU in `Placement.cpp` and `Playback.cpp` and uploaded
as two `vec4` uniform arrays. One copy of the arithmetic, and the harness tests
the real one.

The OFX build is the exception, and only for the per-pixel half — the cell
fetch, the key and the composite, which the GPU evaluated per fragment. Those
three are mirrored in `FlipbookOFX.cpp` and there is no way around it.

**Reset is a subtraction, not a state machine.** Once mode needs somewhere to
start from and the host clock does not restart when a clip is triggered, so the
plugin keeps exactly one number: the clock reading at the last Reset. It is
worth being honest that this is state. It is admissible because it is a *base*
rather than an accumulator — written on an event, read otherwise — so it cannot
creep, cannot depend on the frame rate, and still leaves any frame renderable on
its own given the same base.

---

## The traps

Ordered by how much time they will cost you.

**The half-texel inset is the whole ball game.** Sample a cell at exactly its
boundary with `GL_LINEAR` and the hardware blends in the first texel of the
*next* cell — a one-pixel seam of the neighbouring sprite down one edge. It
looks like a decode fault, or an off-by-one in the grid, and it is neither.
`Inset` clamps the sample into the cell so the outer half texel repeats instead
of bleeding. `fbtest --seam` is the test, and it is the test this plugin most
needs: it renders three interior cells of a flat-colour sheet under both filters
and asserts that *every* lit pixel is its own cell's exact colour.

**The sheet is deliberately not mipmapped.** A mip level is an average across
cell boundaries, so mip 3 of a sprite sheet is every sprite faintly containing
its neighbours — and there is no padding to prevent it, because the sheet's
layout belongs to whoever exported it. downpour can mipmap its atlas because it
*builds* that atlas with borders. The trade here is that minification aliases,
which is much the lesser problem. Adding `glGenerateMipmap` is the obvious
"improvement" and it would quietly contaminate every sprite at every size.

**Two coordinate conventions, and mixing them up is invisible at 1:1.** Copies
are placed in **frame space** — 0..1 across the raster on each axis, y down — so
a spread of 1 reaches the edges of a 16:9 frame instead of leaving bars at the
sides. Sprites are sized off the **short edge**, so a square cell comes out
square. Those are the same number only on a square render: get it wrong and a
512×512 test frame looks perfect while every real output draws the sprite wider
than it is tall. `fbtest --aspect` exists solely for this, and it checks the
*size* as well as the proportions, because sizing off the long edge gives a
perfectly square sprite of entirely the wrong size.

**The sheet's v axis runs top-down; the incoming clip's runs bottom-up.**
`glTexImage2D` puts our first buffer row at v = 0 and our first row is the top of
the image, so a file sheet needs no flip at all. The clip arrives from the host
the other way up. `SheetFlipV` is the one place that difference lives, and
getting it wrong plays the sheet's rows bottom-to-top — which on a symmetrical
sheet looks like nothing whatever until it does not.

**`SetParamInfo`'s clamp applies to `FF_TYPE_STANDARD` only.** The rest of the
fleet states flatly that "a ranged parameter cannot have a ranged default", and
that is true *of standard parameters*. Read the SDK: the `if( pType ==
FF_TYPE_STANDARD )` guard is the whole of it, and an `FF_TYPE_INTEGER` default
passes through untouched, with `SetParamRange` free to widen it afterwards. So
Columns, Rows, Start Frame, Frame Count and Copies are real integers here rather
than 0..1 floats. They **have** to be: every other plugin in the fleet generates
its picture from arithmetic, where "about sixty columns" is a fine thing for a
slider to mean, and this one is *reading somebody else's grid*, where a slider
that lands on 4 or 6 either side of 5 makes the plugin useless on the one input
it exists to handle. This is unverified in Resolume itself — see below.

**Keying happens before premultiplication, and premultiplication happens last.**
The decoded pixels stay straight-alpha all the way to the final line of the
fragment shader. Premultiplying at upload would mean the colour key compares the
operator's swatch against `rgb * a`, which matches nothing on any sheet with
soft edges. Keying is a question about the picture, so it is asked before the
picture is multiplied by anything.

**Option parameters do NOT hold 0..1.** They hold the element value the operator
chose — 0, 1, 2… — so they are read through `Option()`, which rounds and clamps.
A stale composition naming an element that no longer exists is why it clamps.

**Every `ffglex::Scoped*` binding clears to 0 on scope exit — it does not
restore.** Which is why the render path uses plain `glUseProgram` and
`glBindTexture` and puts the state back by hand at the end.

**The filters live on a sampler object, not on the texture.** In the
input-as-sheet mode the texture belongs to the *host*: setting
`GL_TEXTURE_MIN_FILTER` on it would leave Resolume rendering that clip with
whatever filter this plugin last wanted, on every other layer it appears on. A
sampler overrides the filter for the duration of the bind and touches nothing.

**The GLSL declares `Xform[64]` and `Cell[64]` as literals**, because the shader
is a plain string. `Flipbook.cpp` carries a `static_assert` that `kMaxCopies` is
still 64, so raising one without the other is a build error rather than a
uniform-array overrun.

**`InitGL` is idempotent, and has to be.** The harness calls it every frame —
it is what sets the viewport — and a host is entitled to call it twice.
Recompiling two shaders and generating a fresh VAO and sampler each time would
leak three GL objects per frame.

**The plugin registers itself from a file-scope constructor.** `CFFGLPluginInfo`
is never referenced by name, so in a **STATIC** archive the linker may drop the
whole translation unit — giving a bundle that loads, exports `plugMain`, and
reports that it contains no plugins. `flipbook_core` is an **OBJECT** library,
and `SourcePlugin.cpp` / `EffectPlugin.cpp` are listed **directly** in their own
`MODULE` targets. Putting either in the shared library would register both
plugins into both bundles. `tools/verify.sh` checks both failure directions.

**Most parameters are supposed to do nothing in the default configuration.**
Spread, Stagger and Arrange need more than one copy; Seed needs Scatter; the key
colour and its tolerance need a key mode that is not Alpha; Phase needs Manual
sync; Mix is effect-only. Every one is a false failure waiting to happen, which
is what `sweep.py`'s `CONTEXT` table is for. Two more are subtler: **Flip and
Sampling are invisible on a flat-coloured cell** — a flat square is its own
mirror image, and nearest and bilinear agree exactly inside a region of constant
colour — which is why `fbtest --make-sheet` has a `--detail` variant that exists
only for the sweep.

**Two settings can be the same picture.** Rotation runs −180 to +180, so
sweeping the slider end to end renders the sprite at −180 and then at +180,
which is the same frame to the last bit; a sweep of the obvious two ends reports
a working Rotation as dead. Loop and Once are likewise identical whenever the
clock lands on the last frame of the run — Loop wraps to it, Once clamps to it —
and the sweep's fixed 6.25 s did exactly that on the default rate. `sweep.py`'s
`ENDS` table is where both live.

**`flat`, `active`, `filter`, `input`, `output`, `sample` and `common` are GLSL
reserved words**, and a shader that will not compile surfaces only at runtime, as
"the plugin does nothing". That is what `Diag` is for.

---

## Checking your work

`tools/verify.sh` runs the lot. The ones that matter check different things:

- **`--frames`** walks the clock and identifies the cell on screen by its
  colour, against `Playback.cpp`, across all three modes and a run that wraps
  off the end of the sheet. It exercises the clock, the run arithmetic,
  `CellRect`, the uniform upload and the vertex transform at once. It cannot
  catch a sprite drawn in the wrong *place*, because it only samples the centre.
- **`--copies`** is the one that can. It measures at the centre `Placement.cpp`
  predicted for every copy, so a copy that is somewhere else registers as an
  empty sample and fails — deliberately, because a nearest-blob search would
  assume the very thing under test. It also asserts that a staggered field shows
  more than one distinct frame, without which a Stagger that had stopped
  reaching the picture would agree perfectly with its own prediction of nothing
  happening.
- **`--aspect`** is the two-coordinate-conventions trap, above.
- **`--seam`** is the half-texel inset, above.
- **`--key`** checks the four key modes against known colours, in pairs. Alpha
  and None are checked on the sheet's one half-alpha cell, because on an opaque
  cell they are the same picture; Luma is checked both keeping and removing,
  because "removes everything" would otherwise pass.
- **`sweep.py`** is the only thing that catches a dead control.

The OFX build is smoke-tested with `ofxprobe` from
[resolume-ofx-bridge](https://github.com/stoatworks-labs/resolume-ofx-bridge).
Note that the probe only drives the **Filter** context, so the generator's
`describe` is checked but its render runs only in a real host. Setting the sheet
needs `--set-string`, which was added to the probe for this plugin — without it
every numeric setting is measuring an empty frame that renders perfectly and
draws nothing.

**Host verification is Allan's, not an agent's.** Driving the Resolume GUI from
a session is unreliable. **Nothing in this repo has been loaded into Resolume or
Resolve yet**, and three things are worth checking there first:

- **Whether Resolume honours `FF_TYPE_INTEGER` with a range.** It is the
  documented path — `FF_GET_RANGE` is a real host opcode and the SDK stores what
  `SetParamRange` is given — but no other plugin in this fleet uses it, so it is
  structurally right and empirically unproven. If it comes out as a 0..1 slider,
  the fix is `Controls.cpp` conversions and nothing else changes.
- **Whether the file picker offers the right extensions**, and what it does with
  a path that no longer resolves.
- **Whether "Sheet From: Input Clip" is usable in practice.** The host has
  already scaled the sheet into the layer's raster by the time the plugin sees
  it, so the cell boundaries are only as true as the clip's own transform. It
  may want a documented "set the clip transform to Stretch" note, or it may be
  fine.

---

## Things deliberately not done

- **No sidecar JSON.** TexturePacker and Aseprite both export named animations
  with non-uniform frame rectangles, and reading one would cover the game-asset
  case properly. Start Frame and Frame Count already cover it manually — a
  four-frame idle and an eight-frame run are two parameter sets — and a parser
  would add a second file to lose, a second format to track and a whole class of
  "it loaded but the names are wrong" failures, for something an operator does
  once per sheet.
- **No per-copy rotation.** Rotation is one uniform shared by every copy.
  Per-copy would mean a third `vec4` array for one float, and the look it buys —
  a field of sprites at scattered angles — is a Scatter arrangement away from
  what is already there.
- **No motion.** Copies do not travel; that is orrery's job, and a Flipbook
  source on a layer with orrery's mask over it composes better than either
  plugin growing the other's feature.
- **No mip levels, and no anisotropy.** See the traps.
- **No reload-on-change.** The sheet is decoded when the path or grid changes
  and left alone. Watching the file would mean a stat per frame or a thread, for
  a workflow — editing a sheet with the plugin live — that is not how anyone
  uses this.
- **The Colour key is not a chroma key.** No hue separation, no spill
  suppression. A sprite sheet's background is a flat exported colour rather than
  a lit cyclorama, and the machinery that makes a real chroma key work would
  only give this one more ways to be set wrong.

Related: [downpour](https://github.com/stoatworks-labs/downpour) and
[orrery](https://github.com/stoatworks-labs/orrery) (the CMake, harness, Diag
and preset patterns came from there), old-cathode, porthole, asciify.

## Factory presets

`source/Presets.h` is one table of named looks in the host-facing parameter
space, and it drives **both** builds — the FFGL constructor and the OFX describe
each read it, so a preset cannot drift between Resolume and Resolve. Element 0
of the dropdown is always **Custom**, which is not in the table: it means "the
sliders are the truth".

The mechanics are the fleet's: picking a preset copies the table row into the
real parameters — the FFGL side raises `FF_EVENT_FLAG_VALUE` per changed
parameter so the host re-reads its sliders, the OFX side setValues inside one
edit block so undo takes the whole preset back at once. A host that ignores the
events still renders the preset correctly and merely shows stale knobs. Editing
any covered parameter afterwards flips the dropdown back to Custom — judged by
comparing values, not by the change reason, so a host echoing our own writes
cannot un-set the preset.

**What a preset covers here is the unusual part.** It never touches the Sheet
group — not the file, not Columns, Rows, Start Frame or Frame Count. Those
describe the operator's own material, and a preset that reached into them would
take a working 5×5 explosion and declare it an 8×8, which is not a look, it is
breakage. It also leaves alone Position, Seed, Mix and Sync; Sync in particular
cannot be in the table, because the FFGL build offers beat modes the OFX build
has no clock for and an index would mean different things in different hosts.

## Notes

`docs/NOTES.md` carries this repo's working notes — current status, decisions
already made, and the traps that have actually bitten. Read it before changing
anything non-obvious. Cross-cutting fleet knowledge lives in
[fleet-notes](https://github.com/stoatworks-labs/fleet-notes).
