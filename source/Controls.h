#pragma once

#include <cstdint>

/**
    Host parameters, and what they mean.

    ## Two kinds of parameter, and the difference is load-bearing

    Most of the fleet declares **every** numeric parameter as a plain 0..1
    `FF_TYPE_STANDARD` float and does the conversion here, because
    `CFFGLPluginManager::SetParamInfo` clamps an `FF_TYPE_STANDARD` default into
    0..1 *before* returning, and `SetParamRange` can only be called afterwards.
    There is no `SetParamDefault`, so a parameter declared in columns cannot
    declare a default in columns: 5 becomes 1, silently.

    **That clamp applies to `FF_TYPE_STANDARD` only.** Read the SDK's
    `SetParamInfo`: the `if( pType == FF_TYPE_STANDARD )` guard is the whole of
    it, and an `FF_TYPE_INTEGER` default passes through untouched. So the counts
    here — Columns, Rows, Start Frame, Frame Count, Copies — are declared as
    real integers with real ranges, defaults and all.

    They have to be. Every other plugin in the fleet is generating its picture
    from arithmetic, so "about sixty columns" is a perfectly good thing for a
    slider to mean. This one is *reading somebody else's grid*, and a sheet that
    is five across is five across. A 0..1 slider that lands on 4 or 6 on either
    side of the right answer would make the plugin unusable on the one input it
    exists to handle. Exact integers are not a nicety here, they are the feature.

    Everything continuous is still 0..1 and still converted below, with curves
    rather than straight lines wherever the useful part of a range is bunched at
    one end. Rate is the clear case: 8 fps and 12 fps are different animations,
    350 fps and 360 fps are the same strobe.
*/
namespace flipbook
{
/// Parameter ids. The declaration order in Flipbook.cpp is the order they
/// appear in the host, and the groups depend on consecutive ids staying
/// consecutive -- SetParamGroup collapses *runs* of same-group parameters, so
/// reordering these silently splits a group into two.
enum ParamId : unsigned int
{
	// Sheet
	PT_SHEET_FILE = 0,
	PT_SHEET_SOURCE,
	PT_COLUMNS,
	PT_ROWS,
	PT_START,
	PT_LENGTH,
	PT_SAMPLING,

	// Playback
	PT_SYNC,
	PT_RATE,
	PT_MODE,
	PT_PHASE,
	PT_RESET,

	// Layout
	PT_FIT,
	PT_POS_X,
	PT_POS_Y,
	PT_SCALE,
	PT_ROTATION,
	PT_FLIP_H,
	PT_FLIP_V,

	// Copies
	PT_COPIES,
	PT_ARRANGE,
	PT_SPREAD,
	PT_STAGGER,
	PT_SEED,

	// Key
	PT_KEY,
	PT_KEY_R,
	PT_KEY_G,
	PT_KEY_B,
	PT_KEY_TOLERANCE,
	PT_KEY_SOFTNESS,

	// Colour
	PT_TINT_R,
	PT_TINT_G,
	PT_TINT_B,
	PT_OPACITY,
	PT_BACK_R,
	PT_BACK_G,
	PT_BACK_B,
	PT_BACK_OPACITY,

	// Output. Declared by both plugins so that a composition can be moved
	// between them without the parameter list shifting underneath it; the
	// source plugin has nothing to mix against and ignores it.
	PT_MIX,

	// Preset. Declared after the real controls so their ids — which a saved
	// composition refers to — do not shift under existing users.
	PT_PRESET,

	// -- The Stoatworks About block ------------------------------------------
	//
	// One display-only text line, then one button per link the block carries:
	// the guide, the project page, the source, the funding page. A button opens
	// a browser and stores nothing.
	//
	// How many buttons there are is decided by which URLs StoatworksAbout.h
	// actually holds, so Flipbook.cpp static_asserts this run against
	// `about::kParamCount` -- the user guide added the fourth button, and without
	// the assert that would silently shift PT_COUNT and leave the last button
	// undeclared.
	//
	// Last in the enum so no saved composition's parameter ids shift.
	PT_ABOUT_TEXT,
	PT_ABOUT_BUTTON_1,
	PT_ABOUT_BUTTON_2,
	PT_ABOUT_BUTTON_3,
	PT_ABOUT_BUTTON_4,
	PT_COUNT
};

/// Copies drawn in one instanced pass, and the literal size of the `Xform[]`
/// and `Cell[]` uniform arrays in the vertex shader. The shader is a plain
/// string, so it cannot be told this number; Flipbook.cpp carries a
/// `static_assert` that it is still 64, making a mismatch a build error rather
/// than a uniform-array overrun.
constexpr int kMaxCopies = 64;

/// The largest grid the Columns and Rows parameters offer. Not a limit of
/// anything downstream -- a cell is just a division -- but a sheet 64 across is
/// already 64 sprites of 32 px on a 2K page, and an unbounded spinner is a way
/// to type 4000 by accident and wonder why the picture went black.
constexpr int kMaxGrid = 64;

/// The largest addressable frame. A 64x64 sheet is 4096 cells, so this is the
/// grid limit squared and not an independent choice.
constexpr int kMaxFrames = kMaxGrid * kMaxGrid;

/// Where the sheet comes from. **File** decodes whatever the file parameter
/// points at, at its own resolution. **Input** treats the incoming clip as the
/// sheet, which is the effect plugin's reason to exist -- and which the source
/// plugin has no way to honour, so it forces File.
enum class SheetSource : int
{
	File = 0,
	Input,

	Count
};

/// How a cell is sampled. **Smooth** is bilinear and right for rendered or
/// photographed sheets. **Pixel** is nearest-neighbour and is the difference
/// between pixel art and mush -- a 32 px sprite blown up to a metre of LED wall
/// is the case, and bilinear turns every hard edge into a gradient.
enum class Sampling : int
{
	Smooth = 0,
	Pixel,

	Count
};

/// What Rate is measured against. Free: frames per second, on the host's clock.
/// Beat and Bar: frames per beat or per bar, locked to the host's BPM clock.
/// Manual: the clock is ignored entirely and Phase drives the sequence, which
/// is the mode for keyframing against an edit rather than free-running.
enum class SyncMode : int
{
	Free = 0,
	Beat,
	Bar,
	Manual,

	Count
};

/// What happens at the end of the run. **Once** holds the last frame; it is the
/// mode an explosion wants, and the only one that needs Reset to mean anything.
enum class PlayMode : int
{
	Loop = 0,
	PingPong,
	Once,

	Count
};

/// How the sprite is fitted to the raster before Scale is applied.
///
/// **Native** keeps the cell's own aspect and sizes it off the frame's short
/// edge, so a square sprite is square on a 16:9 output. **Fill** covers the
/// whole raster preserving aspect, cropping the overflow. **Stretch** maps one
/// cell onto the whole raster exactly, aspect be damned -- which is what a sheet
/// of full-frame stills wants, and nothing else does.
enum class Fit : int
{
	Native = 0,
	Fill,
	Stretch,

	Count
};

/// Where the copies go. Single is Grid with one copy, so it is not a separate
/// mode; Grid with Copies at 1 puts it wherever Position says.
enum class Arrange : int
{
	Grid = 0,
	Ring,
	Scatter,

	Count
};

/// How the sprite's background is removed. **Alpha** trusts the file's own
/// alpha and is the default because a PNG sprite sheet almost always has one.
/// **Luma** and **Colour** exist for the sheets that do not -- the JPEG
/// explosion on white, the pixel-art sheet on its one flat blue.
enum class KeyMode : int
{
	Alpha = 0,
	Luma,
	Colour,
	None,

	Count
};

/// Round an option parameter's value to an element index and clamp it into
/// range. Option parameters do **not** hold 0..1: they hold the element value
/// the operator chose. The clamp is for a stale composition naming an element
/// that no longer exists.
int Option( float value, int count );

/// Frames per second -- or per beat or per bar under those Sync modes.
/// 0.5..60, exponential. Twelve is the middle of the useful range, not the
/// middle of the slider, which is why the default is not 0.5.
float RateFromParam( float value );

/// Sprite size, as a multiple of whatever Fit decided. 0.05..4, exponential,
/// with 1.0 landing exactly at 0.5 so "the size it says it is" is a place you
/// can find by dragging to the middle rather than by reading this file.
float ScaleFromParam( float value );

/// Rotation in radians. -pi..pi, zero at the centre of the slider.
float RotationFromParam( float value );

/// How far apart the copies sit, in frame widths. 0..1.5 -- past 1 they leave
/// the raster, which is a legitimate thing to want when the sprite is big.
float SpreadFromParam( float value );

/// Phase offset per copy, in frames. 0..8, with a dead zone at the bottom so
/// that "all copies in step" is reachable by dragging to zero rather than by
/// luck. A tenth of a frame of stagger is invisible on a still and reads as
/// jitter in motion, which is worse than either end.
float StaggerFromParam( float value );

/// The seed. 1..9999 -- an integer, so that nudging the slider gives a
/// different scatter rather than an imperceptibly different one.
uint32_t SeedFromParam( float value );

/// Key tolerance: how far from the key colour still counts as background.
/// 0..1 straight through, but squared, because the useful settings for a clean
/// render are all in the bottom fifth.
float KeyToleranceFromParam( float value );

/// Key softness, as a width added outside the tolerance. 0..0.5, squared for
/// the same reason.
float KeySoftnessFromParam( float value );

} // namespace flipbook
