#pragma once

#include <FFGLSDK.h>

// After FFGLSDK.h, which is where FFUInt32 comes from.
#include "StoatworksAboutParams.h"

#include <mutex>
#include <string>
#include <vector>

#include "Controls.h"
#include "Placement.h"
#include "Playback.h"
#include "Presets.h"
#include "Sheet.h"

/**
    Flipbook -- a sprite sheet player for Resolume.

    A page of stills, cut into a grid, played as an animation and placed in the
    raster. What is worth knowing about how it works is in four other files and
    not repeated here:

    - **Sheet.h** -- what a sheet is, why the pixels stay straight-alpha, and
      the texture size guard.
    - **Playback.h** -- the frame is a pure function of the clock, and Reset is
      a subtraction rather than a state machine.
    - **Placement.h** -- the two coordinate conventions, and why Fit happens
      before Scale.
    - **Shaders.h** -- two passes, the half-texel inset, and why the sheet is
      not mipmapped.

    This class is the part that talks to the host: it declares the parameters,
    reloads the sheet when the file changes, solves the copies and draws.

    Both plugins are this class. The source plays a sheet over its own
    background; the effect plays it over the incoming clip, and can also take
    that clip *as* the sheet. They differ by a constructor flag, a `#define`
    handed to the shader compiler, and their input count -- little enough that
    keeping them as one class is what stops them drifting apart.

    See AGENTS.md for the traps.
*/
namespace flipbook
{
/// The sheet as the renderer sees it, whichever end it came from. A file sheet
/// owns its texture; an input sheet borrows the host's, which is why this is a
/// value handed around per frame rather than anything with a destructor.
struct ActiveSheet
{
	GLuint texture = 0;
	int width      = 0;
	int height     = 0;
	int columns    = 1;
	int rows       = 1;
	int cells      = 1;

	/// True when the texture's v runs bottom-up, which is how the host hands
	/// over a clip and is never how our own upload comes out. See Shaders.h.
	bool flipV = false;

	bool Valid() const { return texture != 0 && width > 0 && height > 0; }
};

class FlipbookPlugin : public CFFGLPlugin
{
public:
	explicit FlipbookPlugin( bool overInput );

	// CFFGLPlugin
	FFResult InitGL( const FFGLViewportStruct* vp ) override;
	FFResult ProcessOpenGL( ProcessOpenGLStruct* pGL ) override;
	FFResult DeInitGL() override;

	FFResult SetFloatParameter( unsigned int index, float value ) override;
	float GetFloatParameter( unsigned int index ) override;

	FFResult SetTextParameter( unsigned int index, const char* value ) override;
	char* GetTextParameter( unsigned int index ) override;

	/// Render one frame into whatever is currently bound. Exposed for the
	/// offline harness, which drives this class rather than a copy of it -- a
	/// test that exercises a reimplementation tests the reimplementation.
	void Render( int width, int height, const ActiveSheet& input, float maxU, float maxV );

	//---------------------------------------------------------------------
	// For the harness. Every one of these answers a question the rendered
	// picture cannot be asked directly.
	//---------------------------------------------------------------------

	/// The decoded file sheet, valid or not. `note` says which and why.
	const Sheet& FileSheet() const { return sheet; }

	/// The sheet the last Render actually used.
	const ActiveSheet& LastSheet() const { return lastSheet; }

	/// The layout as it would be solved at this size, for `--aspect` to measure
	/// the rendered sprite against.
	LayoutState CurrentLayout( int width, int height ) const;

	/// Position through the run, in frames, on the current clock.
	double CurrentFramePos() const;

	/// The absolute cell index copy `i` is showing, for `--frames` to check
	/// against what actually got drawn.
	int CellForCopy( int copyIndex, int cells ) const;

	/// Force the sheet to be reloaded before the next draw. The harness needs
	/// this because it sets parameters directly.
	void Invalidate() { contentDirty = true; }

	/// Feed the clock directly, bypassing the host-unit detection. The harness
	/// wants frame 3.25 exactly, not "whatever 3.25 of some unit turns out to
	/// mean".
	void SetSecondsForTest( double seconds );

private:
	/// The ParamId each presets::Param drives, in presets::Param order. The
	/// preset table stays host-agnostic; this is the FFGL binding of it.
	static constexpr unsigned int kPresetParamIDs[ presets::kParamCount ] = {
		PT_RATE, PT_MODE, PT_SAMPLING, PT_FIT, PT_SCALE, PT_ROTATION,
		PT_COPIES, PT_ARRANGE, PT_SPREAD, PT_STAGGER,
		PT_KEY, PT_KEY_R, PT_KEY_G, PT_KEY_B, PT_KEY_TOLERANCE, PT_KEY_SOFTNESS,
		PT_TINT_R, PT_TINT_G, PT_TINT_B, PT_OPACITY,
		PT_BACK_R, PT_BACK_G, PT_BACK_B, PT_BACK_OPACITY
	};

	/// Copy a factory preset's values into params[] and raise value events so
	/// the host re-reads the sliders. `presetIndex` is 1-based; 0 is Custom.
	void applyPreset( int presetIndex );

	void ReloadSheet();
	bool UploadSheet();
	void UpdateClock();

public:
	FFResult SetTime( double time ) override;

	//---------------------------------------------------------------------
	// Clock test hooks. The offline harness DECLARES its unit rather than
	// leaving UpdateClock to infer one -- a single absolute time handed over
	// in one frame is genuinely ambiguous, and an implicit unit is what let
	// the millisecond bug through in the first place.
	//---------------------------------------------------------------------
	void SetClockScaleForTest( double scale );
	void TickClockForTest();
	double ClockScaleForTest() const;
	double HostSecondsForTest() const;

private:

	const bool overInput;

	ffglex::FFGLShader backgroundShader;
	ffglex::FFGLShader spriteShader;

	/// Core profile will not draw without a vertex array object bound, even for
	/// an attributeless draw that reads nothing from it.
	GLuint emptyVAO = 0;

	/// A sampler object rather than texture parameters, and the reason is the
	/// input-as-sheet mode: setting GL_TEXTURE_MIN_FILTER on the clip's texture
	/// would be modifying an object the *host* owns and reuses, so Resolume
	/// would go on rendering that clip with whatever filter this plugin last
	/// wanted, on every other layer it appears on. A sampler overrides the
	/// filter for the duration of the bind and touches nothing.
	GLuint sampler = 0;

	GLuint sheetTexture = 0;

	/// Whether the GL resources above exist. InitGL is idempotent because the
	/// harness calls it every frame -- it is what sets the viewport -- and
	/// because a host is entitled to call it twice.
	bool glReady = false;

	Sheet sheet;
	ActiveSheet lastSheet;
	std::vector< Copy > copies;
	std::vector< float > xformUpload;///< 4 floats per copy
	std::vector< float > cellUpload; ///< 4 floats per copy

	bool contentDirty = true;
	bool uploadDirty  = true;

	//---------------------------------------------------------------------
	// The file path. The only plugin state that is not a float, and the only
	// place the host's thread and the render thread touch the same object
	// rather than racing on a word. A std::string reallocating under a reader
	// is a crash in somebody else's process, so it gets a lock -- taken on a
	// change and on a reload, never per frame.
	//---------------------------------------------------------------------
	mutable std::mutex textMutex;
	std::string sheetFilePath;
	char textReturn[ 4096 ] = { 0 };

	//---------------------------------------------------------------------
	// Host clock units.
	//
	// The FFGL header never says what unit SetTime is in, and hosts disagree:
	// Resolume hands over MILLISECONDS (measured live: 20.0 per frame at its
	// 50 fps, and the SDK's own Particles sample divides by 1000), while the
	// offline harness sends seconds. UpdateClock calibrates the host's clock
	// against a steady_clock and lets the ratio name the unit, over several
	// agreeing frames, failing safe to the wall clock while undecided.
	//---------------------------------------------------------------------
	double clockScale  = 0.0;///< 0 until decided; then 1.0 or 0.001
	double lastWallTime = -1.0;
	double wallStart    = -1.0;
	int secondsVotes    = 0;
	int millisVotes     = 0;
	bool hostTimeSeen   = false;
	double lastRawTime = -1.0;
	double hostSeconds = 0.0;

	/// The clock reading at the last Reset. A base, not an accumulator: written
	/// on an event, subtracted otherwise. See Playback.h for why that is
	/// admissible where an integrated position would not be.
	double clockBase   = 0.0;
	bool resetPending  = false;

	//---------------------------------------------------------------------
	// Frame-position continuity across a Rate change.
	//
	// Which cell is shown stays a pure function of the frame position -- that
	// is the whole design and none of it changes here. What changes is only
	// which position a given clock reading maps to.
	//
	// `framePos = seconds * rate` means a rate change moves the position by
	// `seconds * delta`, and `seconds` is however long the composition has been
	// open. Nudging Rate an hour in is a jump of thousands of frames: the sheet
	// cuts to an unrelated cell, which is what orrery issue #6 reported once
	// the 1000x clock bug was out of the way. So remember the position reached
	// so far and carry on from there at the new rate.
	//
	// Free only. Beat and Bar deliberately keep jumping: their contract is that
	// a cell boundary lands on the host's grid, and an offset that made a rate
	// change seamless would slide the sequence off the grid it exists to sit
	// on. Manual is driven entirely by the Phase slider. Nor is any of this in
	// the OpenFX build: that host renders arbitrary times in arbitrary order
	// and can keyframe Rate, so a running anchor there would make a frame
	// depend on which frames were rendered before it.
	//
	// Reset clears it along with `clockBase` -- starting again means starting
	// again.
	//---------------------------------------------------------------------
	void UpdateFrameAnchor();

	double frameAnchor   = 0.0; ///< frame position already reached at `anchorSeconds`
	double anchorSeconds = 0.0; ///< the elapsed reading that position belongs to
	float anchorRate     = -1.0f;///< rate in force since then; < 0 until the first frame
	bool forcedSeconds = false;

	float params[ PT_COUNT ] = { 0.0f };
};

} // namespace flipbook
