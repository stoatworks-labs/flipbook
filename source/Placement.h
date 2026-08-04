#pragma once

#include <cstdint>
#include <vector>

#include "Controls.h"

/**
    Where each copy of the sprite sits, and which frame it is showing.

    ## Two coordinate conventions, and mixing them up is invisible at 1:1

    This is the trap that costs the most time in every plugin in this fleet, and
    it is worth stating in the file that commits it.

    **Copies are placed in frame space** — 0..1 across the raster on each axis,
    y down — so a spread of 1 reaches the edges of a 16:9 frame rather than
    leaving bars at the sides. **Sprites are sized off the short edge**, so a
    square cell comes out square instead of stretching with the output.

    Those are the same number only on a square render. Get it wrong and a 1:1
    test frame looks perfect while every real output draws the sprite wider than
    it is tall. `fbtest --aspect` exists for exactly this, and it checks the
    *size* as well as the proportion, because sizing off the wrong edge gives a
    perfectly proportioned sprite of entirely the wrong size — which a
    proportion check alone would pass.

    ## Fit happens before Scale, and the order is the point

    `Fit` decides what "actual size" means for this sheet: the cell's own
    proportions on the short edge (Native), covering the raster (Fill), or the
    raster exactly (Stretch). `Scale` is then a plain multiplier on whichever it
    was, so 1.0 always means "the size Fit said" and the slider means the same
    thing in all three modes. Folding them together would give a Scale whose
    useful range moved every time Fit changed.
*/
namespace flipbook
{
/// One drawn instance. Everything the vertex shader needs, and nothing it can
/// work out for itself.
struct Copy
{
	float centreX = 0.5f;///< frame space, 0..1, y down
	float centreY = 0.5f;

	/// Half-extents in frame space, per axis. Already carries the cell's aspect,
	/// the fit, the scale and the short-edge correction, so the shader multiplies
	/// a unit quad by these and stops thinking.
	float halfWidth  = 0.5f;
	float halfHeight = 0.5f;

	float rotation = 0.0f;///< radians, positive clockwise on screen

	/// Added to the run position before the frame is resolved, in frames. What
	/// turns a field of copies into a chase rather than sixty-four sprites
	/// doing the same thing at the same instant.
	float phaseOffset = 0.0f;
};

/// Everything the solver needs. Assembled from the parameters by the plugin;
/// kept as a plain struct so the harness can build one by hand.
struct LayoutState
{
	int copies      = 1;
	Arrange arrange = Arrange::Grid;
	float spread    = 0.6f;
	float stagger   = 0.0f;
	uint32_t seed   = 1;

	float centreX = 0.5f;
	float centreY = 0.5f;
	float scale   = 1.0f;
	float rotation = 0.0f;

	Fit fit = Fit::Native;

	/// The cell's own proportions: its width divided by its height, in pixels.
	/// 1.0 for a square cell. This is a property of the *sheet's grid*, not of
	/// the sheet, which is why a sheet that does not divide exactly still gives
	/// a sensible number here.
	float cellAspect = 1.0f;

	int width  = 1920;///< the raster being drawn into
	int height = 1080;

	bool flipH = false;
	bool flipV = false;
};

/// Solve every copy's placement. Pure: same state in, same copies out, with no
/// reference to any clock. `out` is resized to `state.copies`, clamped to
/// `kMaxCopies`.
void SolveCopies( const LayoutState& state, std::vector< Copy >& out );

} // namespace flipbook
