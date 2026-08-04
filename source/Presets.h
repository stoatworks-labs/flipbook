#pragma once

/**
    Factory presets: named ways of *treating* a sheet.

    The important thing about this table is what is not in it. A preset here
    never touches the Sheet group — not the file, not Columns, Rows, Start Frame
    or Frame Count. Those describe the operator's own material, and a preset
    that reached into them would take a working 5x5 explosion and declare it an
    8x8, which is not a look, it is breakage. Every other plugin in the fleet
    excludes the equivalent (downpour leaves Custom Text and the Font alone);
    here the exclusion is most of the reason the presets are safe to click.

    It also leaves alone Position, Seed, Mix and Sync — where the sprite goes,
    which scatter, how much effect is wanted, and what clock it runs on are all
    decisions about the show rather than about the sheet. Sync in particular
    cannot be in the table: the FFGL build offers beat modes the OFX build has
    no clock for, so an index would mean different things in different hosts.

    The values live in the same parameter space both builds expose, so ONE table
    drives both and a preset looks identical in Resolume and Resolve. Plain data
    only; the application machinery lives with each host's glue. Both FFGL
    plugins share `FlipbookPlugin`, so both get the dropdown from this table too.

    Element 0 of the host-facing dropdown is "Custom" and is not in this table:
    it means "the sliders are the truth".
*/

namespace flipbook
{
namespace presets
{
/// The parameters a preset sets, in one fixed order. The FFGL build binds this
/// order to its ParamIDs and the OFX build to its param handles; both
/// static_assert against kParamCount so the three lists cannot drift apart
/// silently.
enum Param
{
	kRate,
	kMode,
	kSampling,
	kFit,
	kScale,
	kRotation,
	kCopies,
	kArrange,
	kSpread,
	kStagger,
	kKey,
	kKeyR,
	kKeyG,
	kKeyB,
	kKeyTolerance,
	kKeySoftness,
	kTintR,
	kTintG,
	kTintB,
	kOpacity,
	kBackR,
	kBackG,
	kBackB,
	kBackOpacity,
	kParamCount
};

struct Preset
{
	const char* name;
	float v[ kParamCount ];
};

// Continuous values are 0..1 and go through the curves in Controls.cpp (Rate
// 0.664 is 12 fps, Scale 0.5 is exactly 1.0, Rotation 0.5 is exactly zero).
// Option and integer parameters hold their real value: Mode 0 Loop / 1
// Ping-Pong / 2 Once; Sampling 0 Smooth / 1 Pixel; Fit 0 Native / 1 Fill /
// 2 Stretch; Arrange 0 Grid / 1 Ring / 2 Scatter; Key 0 Alpha / 1 Luma /
// 2 Colour / 3 None; Copies is a count.
inline constexpr Preset kPresets[] = {
	// The plugin's own defaults, named so they stay reachable after any amount
	// of fiddling: one sprite, its own proportions, its own alpha.
	{ "As Exported",
	  { /*Rate*/ 0.664f, /*Mode*/ 0, /*Sampling*/ 0, /*Fit*/ 0, /*Scale*/ 0.5f, /*Rot*/ 0.5f,
	    /*Copies*/ 1, /*Arrange*/ 0, /*Spread*/ 0.6f, /*Stagger*/ 0.0f,
	    /*Key*/ 0, /*KeyCol*/ 0.0f, 0.0f, 0.0f, /*Tol*/ 0.0f, /*Soft*/ 0.0f,
	    /*Tint*/ 1.0f, 1.0f, 1.0f, /*Opacity*/ 1.0f,
	    /*Back*/ 0.0f, 0.0f, 0.0f, /*BackOp*/ 0.0f } },

	// A sheet of whole frames rather than of sprites -- the page of geometric
	// tiles, a set of rendered backdrops. One cell fills the raster exactly and
	// the file's alpha is ignored, because a sheet like that has none worth
	// honouring and half of them have black where they mean opaque.
	{ "Full Frame",
	  { /*Rate*/ 0.52f, /*Mode*/ 0, /*Sampling*/ 0, /*Fit*/ 2, /*Scale*/ 0.5f, /*Rot*/ 0.5f,
	    /*Copies*/ 1, /*Arrange*/ 0, /*Spread*/ 0.6f, /*Stagger*/ 0.0f,
	    /*Key*/ 3, /*KeyCol*/ 0.0f, 0.0f, 0.0f, /*Tol*/ 0.0f, /*Soft*/ 0.0f,
	    /*Tint*/ 1.0f, 1.0f, 1.0f, /*Opacity*/ 1.0f,
	    /*Back*/ 0.0f, 0.0f, 0.0f, /*BackOp*/ 0.0f } },

	// One pass and hold: what an explosion, a muzzle flash or an impact wants.
	// Keyed off white on the luma, which is how the rendered VFX sheets that
	// ship as JPEG come.
	{ "One Shot",
	  { /*Rate*/ 0.75f, /*Mode*/ 2, /*Sampling*/ 0, /*Fit*/ 0, /*Scale*/ 0.72f, /*Rot*/ 0.5f,
	    /*Copies*/ 1, /*Arrange*/ 0, /*Spread*/ 0.6f, /*Stagger*/ 0.0f,
	    /*Key*/ 1, /*KeyCol*/ 1.0f, 1.0f, 1.0f, /*Tol*/ 0.30f, /*Soft*/ 0.35f,
	    /*Tint*/ 1.0f, 1.0f, 1.0f, /*Opacity*/ 1.0f,
	    /*Back*/ 0.0f, 0.0f, 0.0f, /*BackOp*/ 0.0f } },

	// A field of them, scattered and out of step: the sprite as texture rather
	// than as subject.
	{ "Swarm",
	  { /*Rate*/ 0.70f, /*Mode*/ 0, /*Sampling*/ 0, /*Fit*/ 0, /*Scale*/ 0.24f, /*Rot*/ 0.5f,
	    /*Copies*/ 24, /*Arrange*/ 2, /*Spread*/ 0.85f, /*Stagger*/ 0.28f,
	    /*Key*/ 0, /*KeyCol*/ 0.0f, 0.0f, 0.0f, /*Tol*/ 0.0f, /*Soft*/ 0.0f,
	    /*Tint*/ 1.0f, 1.0f, 1.0f, /*Opacity*/ 1.0f,
	    /*Back*/ 0.0f, 0.0f, 0.0f, /*BackOp*/ 0.0f } },

	// Twelve on a ring, each a frame behind the last, so the animation travels
	// round the circle. The clearest demonstration of what Stagger is for.
	{ "Carousel",
	  { /*Rate*/ 0.664f, /*Mode*/ 0, /*Sampling*/ 0, /*Fit*/ 0, /*Scale*/ 0.30f, /*Rot*/ 0.5f,
	    /*Copies*/ 12, /*Arrange*/ 1, /*Spread*/ 1.0f, /*Stagger*/ 0.17f,
	    /*Key*/ 0, /*KeyCol*/ 0.0f, 0.0f, 0.0f, /*Tol*/ 0.0f, /*Soft*/ 0.0f,
	    /*Tint*/ 1.0f, 1.0f, 1.0f, /*Opacity*/ 1.0f,
	    /*Back*/ 0.0f, 0.0f, 0.0f, /*BackOp*/ 0.0f } },

	// Pixel art, kept as pixel art: nearest-neighbour sampling and a flat
	// background keyed on colour rather than on brightness, because a pixel-art
	// sheet's backdrop is one exact RGB value and nothing else in the sprite
	// is.
	{ "Pixel Nine",
	  { /*Rate*/ 0.62f, /*Mode*/ 0, /*Sampling*/ 1, /*Fit*/ 0, /*Scale*/ 0.30f, /*Rot*/ 0.5f,
	    /*Copies*/ 9, /*Arrange*/ 0, /*Spread*/ 0.9f, /*Stagger*/ 0.13f,
	    /*Key*/ 2, /*KeyCol*/ 0.10f, 0.13f, 0.22f, /*Tol*/ 0.22f, /*Soft*/ 0.12f,
	    /*Tint*/ 1.0f, 1.0f, 1.0f, /*Opacity*/ 1.0f,
	    /*Back*/ 0.0f, 0.0f, 0.0f, /*BackOp*/ 0.0f } },

	// Slow, large and faint, over its own black: the sprite as a wash rather
	// than as a figure. Ping-pong because a slow loop's cut back to frame one
	// is the only thing in the frame moving quickly.
	{ "Ghost",
	  { /*Rate*/ 0.35f, /*Mode*/ 1, /*Sampling*/ 0, /*Fit*/ 1, /*Scale*/ 0.5f, /*Rot*/ 0.5f,
	    /*Copies*/ 1, /*Arrange*/ 0, /*Spread*/ 0.6f, /*Stagger*/ 0.0f,
	    /*Key*/ 0, /*KeyCol*/ 0.0f, 0.0f, 0.0f, /*Tol*/ 0.0f, /*Soft*/ 0.0f,
	    /*Tint*/ 0.80f, 0.86f, 1.00f, /*Opacity*/ 0.35f,
	    /*Back*/ 0.0f, 0.0f, 0.0f, /*BackOp*/ 1.0f } },
};

inline constexpr int kCount = int( sizeof( kPresets ) / sizeof( kPresets[ 0 ] ) );

} // namespace presets
} // namespace flipbook
