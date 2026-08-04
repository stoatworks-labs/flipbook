#pragma once

/**
    Two passes, and no mirrored arithmetic.

    ## Why two passes and not one

    A background pass over the whole raster, then the sprites as an instanced
    quad draw over it. The obvious alternative — one full-screen pass that works
    out for each pixel which copy it is inside — is O(copies) per *pixel*, and
    with sixty-four copies that is sixty-four rejected box tests for every pixel
    of a 4K frame. The sprites cover a small fraction of the raster; drawing
    them as geometry is the cheap direction, and it is also the one that lets
    the blend hardware do the compositing.

    ## There is no GLSL mirror of anything

    downpour has to carry its rain maths twice, once in C++ and once in the
    shader, with a test whose only job is catching the two copies drifting
    apart. It has to, because a cell there is a function of *every pixel*.

    Nothing here is. The placement is per copy and there are at most 64; the
    frame selection is one integer per copy. All of it is solved once on the CPU
    in `Placement.cpp` and `Playback.cpp` and uploaded as two `vec4` uniform
    arrays. One copy of the arithmetic, nothing to keep in step, and a harness
    that tests the real thing rather than a reimplementation of it.

    ## Things that will catch you out

    - **The half-texel inset is load-bearing.** Sample a cell at exactly its
      boundary with `GL_LINEAR` and the hardware blends in the first texel of
      the *next* cell — a one-pixel seam of the neighbouring sprite down one
      edge, which looks like a decode fault or an off-by-one in the grid and is
      neither. `Inset` clamps the sample into the cell, so the outer half texel
      repeats instead of bleeding.

    - **The sheet is not mipmapped, deliberately.** A mip level is an average
      across cell boundaries, so mip 3 of a sprite sheet is every sprite faintly
      containing its neighbours. There is no padding to prevent it — the sheet's
      layout belongs to whoever exported it. Minification therefore aliases, and
      that is the trade: a sprite drawn small shimmers a little, rather than
      every sprite being contaminated at every size.

    - **The sheet's v axis runs top-down; the clip's runs bottom-up.**
      `glTexImage2D` puts our first row at v = 0 and our first row is the top of
      the image, so a file sheet needs no flip. The incoming clip arrives from
      the host the other way up. `SheetFlipV` is the one place that difference
      lives, and getting it wrong plays the sheet's rows in the wrong order,
      which on a symmetrical sheet looks like nothing at all until it does not.

    - **Keying happens before premultiplication, and premultiplication happens
      last.** See Sheet.h. A colour key against premultiplied pixels compares
      the operator's swatch with `rgb * a`, and matches nothing on any sheet
      with soft edges.

    - **A uniform name that does not match the C++ is silently ignored.**
      `glGetUniformLocation` returns -1 and `glUniform` on -1 is a documented
      no-op, so a control is simply dead while everything compiles, links, loads
      and renders. `tools/sweep.py` is what catches it.

    - **`flat`, `active`, `filter`, `input`, `output`, `sample` and `common` are
      GLSL reserved words**, and a shader that will not compile surfaces only at
      runtime, as "the plugin does nothing". That is what Diag is for.
*/
namespace flipbook
{
/// Background pass: the plugin's own colour, or the incoming clip.
extern const char* const kBackgroundVertexShader;
extern const char* const kBackgroundFragmentShader;

/// Sprite pass: one instanced quad per copy, sampling one cell of the sheet.
extern const char* const kSpriteVertexShader;
extern const char* const kSpriteFragmentShader;

/// Prepended after the `#version` line to build the effect variant of either
/// pass. A define rather than a second copy of the shader, so the two builds
/// cannot drift.
extern const char* const kEffectDefine;

} // namespace flipbook
