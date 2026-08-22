#include "Flipbook.h"

#include <string>
#include <chrono>
#include <algorithm>
#include <cmath>
#include <cstring>

#include "Diag.h"
#include "Shaders.h"

namespace flipbook
{

/// Frames that must agree before the host's clock unit is settled.
constexpr int kClockVotes = 4;

/// Wall clock, to calibrate the host's against. Steady rather than system, so
/// nothing here moves if the machine's clock is corrected.
double wallSeconds()
{
	using namespace std::chrono;
	static const steady_clock::time_point start = steady_clock::now();
	return duration_cast< duration< double > >( steady_clock::now() - start ).count();
}
// The vertex shader declares Xform[64] and Cell[64] as literals, because it is
// a plain string. Raising kMaxCopies without raising them would be a uniform
// array overrun; this makes it a build error instead.
static_assert( kMaxCopies == 64, "the sprite vertex shader declares Xform[64] and Cell[64] as literals" );

// The buttons are declared one per link, so the run in the enum and the run the
// block actually has must agree. They diverge the day somebody writes a user
// guide, and this is what says so.
static_assert( PT_COUNT - PT_ABOUT_TEXT == stoatworks::about::kParamCount,
               "the About run no longer matches StoatworksAbout.h -- "
               "add or remove a PT_ABOUT_BUTTON_n to match" );

FlipbookPlugin::FlipbookPlugin( bool overInput_ ) :
	overInput( overInput_ )
{
	diag::init();

	// A source takes no input; an effect takes exactly one. Getting this wrong
	// is not a compile error -- Resolume simply files the plugin under the
	// wrong tab and hands it a texture it was not expecting.
	SetMinInputs( overInput ? 1 : 0 );
	SetMaxInputs( overInput ? 1 : 0 );

	//---------------------------------------------------------------------
	// Defaults.
	//
	// A 5x5 grid because that is what the sheets people already have look
	// like, and because a default of 1x1 would make the first thing anybody
	// sees a single still -- which reads as "the animation does not work"
	// rather than as "tell it the grid".
	//
	// Every one of these is read back out through GetFloatParameter, and
	// SetParamInfof below passes it straight to the host as the declared
	// default, so these assignments are what the operator starts from.
	//---------------------------------------------------------------------
	params[ PT_SHEET_SOURCE ] = static_cast< float >( SheetSource::File );
	params[ PT_COLUMNS ]      = 5.0f;
	params[ PT_ROWS ]         = 5.0f;
	params[ PT_START ]        = 0.0f;
	params[ PT_LENGTH ]       = 0.0f;// to the end of the sheet
	params[ PT_SAMPLING ]     = static_cast< float >( Sampling::Smooth );

	params[ PT_SYNC ]  = static_cast< float >( SyncMode::Free );
	params[ PT_RATE ]  = 0.664f;// exactly 12 fps, see RateFromParam
	params[ PT_MODE ]  = static_cast< float >( PlayMode::Loop );
	params[ PT_PHASE ] = 0.0f;
	params[ PT_RESET ] = 0.0f;

	params[ PT_FIT ]      = static_cast< float >( Fit::Native );
	params[ PT_POS_X ]    = 0.5f;
	params[ PT_POS_Y ]    = 0.5f;
	params[ PT_SCALE ]    = 0.5f;// exactly 1.0, see ScaleFromParam
	params[ PT_ROTATION ] = 0.5f;// exactly zero
	params[ PT_FLIP_H ]   = 0.0f;
	params[ PT_FLIP_V ]   = 0.0f;

	params[ PT_COPIES ]  = 1.0f;
	params[ PT_ARRANGE ] = static_cast< float >( Arrange::Grid );
	params[ PT_SPREAD ]  = 0.6f;
	params[ PT_STAGGER ] = 0.0f;
	params[ PT_SEED ]    = 0.0f;

	params[ PT_KEY ]           = static_cast< float >( KeyMode::Alpha );
	params[ PT_KEY_R ]         = 0.0f;
	params[ PT_KEY_G ]         = 0.0f;
	params[ PT_KEY_B ]         = 0.0f;
	params[ PT_KEY_TOLERANCE ] = 0.0f;
	params[ PT_KEY_SOFTNESS ]  = 0.0f;

	params[ PT_TINT_R ]  = 1.0f;
	params[ PT_TINT_G ]  = 1.0f;
	params[ PT_TINT_B ]  = 1.0f;
	params[ PT_OPACITY ] = 1.0f;

	// Transparent, not black. A sprite plugin's output is nearly always going
	// to be layered over something, and a black background would mean every
	// operator's first action is finding the opacity slider and turning it
	// down. It also makes "nothing loaded" and "loaded but transparent" look
	// the same, which is exactly what Diag's log is for.
	params[ PT_BACK_R ]       = 0.0f;
	params[ PT_BACK_G ]       = 0.0f;
	params[ PT_BACK_B ]       = 0.0f;
	params[ PT_BACK_OPACITY ] = 0.0f;

	params[ PT_MIX ]    = 1.0f;
	params[ PT_PRESET ] = 0.0f;//Custom: the sliders are the truth

	//---------------------------------------------------------------------
	// Declaration.
	//
	// Note the two kinds of numeric parameter here, and see Controls.h for
	// why: the counts are FF_TYPE_INTEGER with real ranges and real defaults,
	// because SetParamInfo's clamp applies to FF_TYPE_STANDARD only. A grid
	// this plugin cannot be told exactly is a grid it cannot read.
	//---------------------------------------------------------------------
	{
		std::vector< std::string > extensions;
		for( int i = 0; i < kSheetExtensionCount; ++i )
			extensions.emplace_back( kSheetExtensions[ i ] );
		SetFileParamInfo( PT_SHEET_FILE, "Sheet", extensions, "" );
	}

	// Declared by both plugins so a composition can move between them without
	// the ids shifting, but only the effect has an input to honour. The source
	// forces File wherever it is read.
	SetOptionParamInfo( PT_SHEET_SOURCE, "Sheet From", static_cast< int >( SheetSource::Count ), params[ PT_SHEET_SOURCE ] );
	SetParamElementInfo( PT_SHEET_SOURCE, 0, "File", 0.0f );
	SetParamElementInfo( PT_SHEET_SOURCE, 1, "Input Clip", 1.0f );

	SetParamInfof( PT_COLUMNS, "Columns", FF_TYPE_INTEGER );
	SetParamRange( PT_COLUMNS, 1.0f, static_cast< float >( kMaxGrid ) );
	SetParamInfof( PT_ROWS, "Rows", FF_TYPE_INTEGER );
	SetParamRange( PT_ROWS, 1.0f, static_cast< float >( kMaxGrid ) );

	SetParamInfof( PT_START, "Start Frame", FF_TYPE_INTEGER );
	SetParamRange( PT_START, 0.0f, static_cast< float >( kMaxFrames - 1 ) );

	// Zero means "to the end of the sheet", which is why the range starts there
	// and not at 1. Naming it Frame Count rather than Length is deliberate: an
	// operator reading "Length 0" reaches for the manual, and one reading
	// "Frame Count 0" on a sheet that is playing all of its frames does not.
	SetParamInfof( PT_LENGTH, "Frame Count", FF_TYPE_INTEGER );
	SetParamRange( PT_LENGTH, 0.0f, static_cast< float >( kMaxFrames ) );

	SetOptionParamInfo( PT_SAMPLING, "Sampling", static_cast< int >( Sampling::Count ), params[ PT_SAMPLING ] );
	SetParamElementInfo( PT_SAMPLING, 0, "Smooth", 0.0f );
	SetParamElementInfo( PT_SAMPLING, 1, "Pixel", 1.0f );

	SetOptionParamInfo( PT_SYNC, "Sync", static_cast< int >( SyncMode::Count ), params[ PT_SYNC ] );
	SetParamElementInfo( PT_SYNC, 0, "Free", 0.0f );
	SetParamElementInfo( PT_SYNC, 1, "Beat", 1.0f );
	SetParamElementInfo( PT_SYNC, 2, "Bar", 2.0f );
	SetParamElementInfo( PT_SYNC, 3, "Manual", 3.0f );

	SetParamInfof( PT_RATE, "Rate", FF_TYPE_STANDARD );

	SetOptionParamInfo( PT_MODE, "Mode", static_cast< int >( PlayMode::Count ), params[ PT_MODE ] );
	SetParamElementInfo( PT_MODE, 0, "Loop", 0.0f );
	SetParamElementInfo( PT_MODE, 1, "Ping-Pong", 1.0f );
	SetParamElementInfo( PT_MODE, 2, "Once", 2.0f );

	SetParamInfof( PT_PHASE, "Phase", FF_TYPE_STANDARD );
	SetParamInfof( PT_RESET, "Reset", FF_TYPE_EVENT );

	SetOptionParamInfo( PT_FIT, "Fit", static_cast< int >( Fit::Count ), params[ PT_FIT ] );
	SetParamElementInfo( PT_FIT, 0, "Native", 0.0f );
	SetParamElementInfo( PT_FIT, 1, "Fill Frame", 1.0f );
	SetParamElementInfo( PT_FIT, 2, "Stretch", 2.0f );

	SetParamInfof( PT_POS_X, "Position X", FF_TYPE_XPOS );
	SetParamInfof( PT_POS_Y, "Position Y", FF_TYPE_YPOS );
	SetParamInfof( PT_SCALE, "Scale", FF_TYPE_STANDARD );
	SetParamInfof( PT_ROTATION, "Rotation", FF_TYPE_STANDARD );
	SetParamInfof( PT_FLIP_H, "Flip H", FF_TYPE_BOOLEAN );
	SetParamInfof( PT_FLIP_V, "Flip V", FF_TYPE_BOOLEAN );

	SetParamInfof( PT_COPIES, "Copies", FF_TYPE_INTEGER );
	SetParamRange( PT_COPIES, 1.0f, static_cast< float >( kMaxCopies ) );

	SetOptionParamInfo( PT_ARRANGE, "Arrange", static_cast< int >( Arrange::Count ), params[ PT_ARRANGE ] );
	SetParamElementInfo( PT_ARRANGE, 0, "Grid", 0.0f );
	SetParamElementInfo( PT_ARRANGE, 1, "Ring", 1.0f );
	SetParamElementInfo( PT_ARRANGE, 2, "Scatter", 2.0f );

	SetParamInfof( PT_SPREAD, "Spread", FF_TYPE_STANDARD );
	SetParamInfof( PT_STAGGER, "Stagger", FF_TYPE_STANDARD );
	SetParamInfof( PT_SEED, "Seed", FF_TYPE_STANDARD );

	SetOptionParamInfo( PT_KEY, "Key", static_cast< int >( KeyMode::Count ), params[ PT_KEY ] );
	SetParamElementInfo( PT_KEY, 0, "Alpha", 0.0f );
	SetParamElementInfo( PT_KEY, 1, "Luma", 1.0f );
	SetParamElementInfo( PT_KEY, 2, "Colour", 2.0f );
	SetParamElementInfo( PT_KEY, 3, "None", 3.0f );

	// Consecutive red/green/blue parameters are what a host needs to show a
	// colour swatch instead of three sliders, so the naming follows the SDK's
	// own convention rather than being tidied up.
	SetParamInfof( PT_KEY_R, "Key Colour", FF_TYPE_RED );
	SetParamInfof( PT_KEY_G, "Key Colour_Green", FF_TYPE_GREEN );
	SetParamInfof( PT_KEY_B, "Key Colour_Blue", FF_TYPE_BLUE );
	SetParamInfof( PT_KEY_TOLERANCE, "Tolerance", FF_TYPE_STANDARD );
	SetParamInfof( PT_KEY_SOFTNESS, "Softness", FF_TYPE_STANDARD );

	SetParamInfof( PT_TINT_R, "Tint", FF_TYPE_RED );
	SetParamInfof( PT_TINT_G, "Tint_Green", FF_TYPE_GREEN );
	SetParamInfof( PT_TINT_B, "Tint_Blue", FF_TYPE_BLUE );
	SetParamInfof( PT_OPACITY, "Opacity", FF_TYPE_STANDARD );

	SetParamInfof( PT_BACK_R, "Background", FF_TYPE_RED );
	SetParamInfof( PT_BACK_G, "Background_Green", FF_TYPE_GREEN );
	SetParamInfof( PT_BACK_B, "Background_Blue", FF_TYPE_BLUE );
	SetParamInfof( PT_BACK_OPACITY, "Background Opacity", FF_TYPE_STANDARD );

	SetParamInfof( PT_MIX, "Mix", FF_TYPE_STANDARD );

	// Factory presets. Element 0 is Custom; picking anything else copies that
	// preset's values into the covered parameters and raises value events so
	// the host re-reads the sliders. Editing a covered slider flips back to
	// Custom. Nothing in the Sheet group is covered -- see Presets.h.
	SetOptionParamInfo( PT_PRESET, "Preset", 1 + presets::kCount, params[ PT_PRESET ] );
	SetParamElementInfo( PT_PRESET, 0, "Custom", 0.0f );
	for( int i = 0; i < presets::kCount; ++i )
		SetParamElementInfo( PT_PRESET, 1 + i, presets::kPresets[ i ].name, static_cast< float >( 1 + i ) );

	// Forty parameters is well past the point where an ungrouped list in
	// somebody else's inspector stops being readable. SetParamGroup collapses
	// *runs* of same-group parameters, which is why the ids in Controls.h have
	// to stay in this order.
	for( unsigned int id = PT_SHEET_FILE; id <= PT_SAMPLING; ++id )
		SetParamGroup( id, "Sheet" );
	for( unsigned int id = PT_SYNC; id <= PT_RESET; ++id )
		SetParamGroup( id, "Playback" );
	for( unsigned int id = PT_FIT; id <= PT_FLIP_V; ++id )
		SetParamGroup( id, "Layout" );
	for( unsigned int id = PT_COPIES; id <= PT_SEED; ++id )
		SetParamGroup( id, "Copies" );
	for( unsigned int id = PT_KEY; id <= PT_KEY_SOFTNESS; ++id )
		SetParamGroup( id, "Key" );
	for( unsigned int id = PT_TINT_R; id <= PT_BACK_OPACITY; ++id )
		SetParamGroup( id, "Colour" );
	SetParamGroup( PT_MIX, "Output" );
	SetParamGroup( PT_PRESET, "Preset" );
	// The About block. Declared inline rather than through a helper, because
	// SetParamInfo is protected on CFFGLPlugin and nothing outside the class
	// can call it.
	SetParamInfo( PT_ABOUT_TEXT, "About", FF_TYPE_TEXT, stoatworks::about::defaultText() );
	{
		FFUInt32 aboutId = PT_ABOUT_TEXT + 1;
		for( const auto& b : stoatworks::about::buttons() )
			SetParamInfo( aboutId++, b.label, FF_TYPE_EVENT, false );
	}
	for( unsigned int id = PT_ABOUT_TEXT; id < PT_COUNT; ++id )
		SetParamGroup( id, "About" );

}

//---------------------------------------------------------------------------
// Parameters
//---------------------------------------------------------------------------
FFResult FlipbookPlugin::SetFloatParameter( unsigned int index, float value )
{
	if( index >= PT_COUNT )
		return FF_FAIL;

	// The About buttons open a browser and store nothing, so they are handled
	// before any of the bookkeeping below: pressing one is not the operator
	// editing a control.
	if( index >= PT_ABOUT_TEXT )
		return stoatworks::about::handleParam( index - PT_ABOUT_TEXT, value ) ? FF_SUCCESS : FF_FAIL;

	if( index == PT_PRESET )
	{
		const int chosen = static_cast< int >( std::lround( value ) );
		if( chosen != static_cast< int >( std::lround( params[ PT_PRESET ] ) ) )
			applyPreset( chosen );
		return FF_SUCCESS;
	}

	if( index == PT_RESET )
	{
		// Consumed in Render rather than acted on here: this arrives on the
		// host's thread and the clock it needs to read is only meaningful on
		// the render thread.
		if( value >= 0.5f )
			resetPending = true;
		params[ PT_RESET ] = 0.0f;
		return FF_SUCCESS;
	}

	const float previous = params[ index ];
	params[ index ]      = value;

	// Only the grid changes what has to be decoded again -- and only because
	// the decode is where a still's cell count is decided. Everything else in
	// the Sheet group is read fresh every frame.
	if( ( index == PT_COLUMNS || index == PT_ROWS ) && value != previous )
		contentDirty = true;

	// A slider moved while a preset is active means the operator has taken
	// over: the dropdown falls back to Custom. The equality guard matters --
	// hosts that honour the value events echo the preset's own values straight
	// back through here, and that echo must not un-set the preset.
	const int active = static_cast< int >( std::lround( params[ PT_PRESET ] ) );
	if( active > 0 && std::fabs( value - previous ) > 1e-4f )
	{
		for( unsigned int id : kPresetParamIDs )
		{
			if( id == index )
			{
				params[ PT_PRESET ] = 0.0f;
				RaiseParamEvent( PT_PRESET, FF_EVENT_FLAG_VALUE );
				break;
			}
		}
	}

	return FF_SUCCESS;
}

void FlipbookPlugin::applyPreset( int presetIndex )
{
	params[ PT_PRESET ] = static_cast< float >( presetIndex );

	if( presetIndex <= 0 || presetIndex > presets::kCount )
		return;//Custom: the sliders keep whatever they said

	const presets::Preset& preset = presets::kPresets[ presetIndex - 1 ];
	for( int j = 0; j < presets::kParamCount; ++j )
	{
		const unsigned int id = kPresetParamIDs[ j ];
		if( std::fabs( params[ id ] - preset.v[ j ] ) <= 1e-6f )
			continue;

		// The copy is what changes the picture; the event only tells the host
		// to re-read the slider. A host that ignores it renders the preset
		// correctly and merely shows stale knobs.
		params[ id ] = preset.v[ j ];
		RaiseParamEvent( id, FF_EVENT_FLAG_VALUE );
	}
}

float FlipbookPlugin::GetFloatParameter( unsigned int index )
{
	return index < PT_COUNT ? params[ index ] : 0.0f;
}

FFResult FlipbookPlugin::SetTextParameter( unsigned int index, const char* value )
{
	// Display-only, and it MUST still succeed: instantiateGL pushes every
	// declared default back through the setters on a fresh instance and deletes
	// the instance if one fails, so failing here means no real host can load
	// the plugin -- while every offline harness here carries on passing.
	if( index == PT_ABOUT_TEXT )
		return FF_SUCCESS;

	if( index != PT_SHEET_FILE )
		return FF_FAIL;

	std::lock_guard< std::mutex > lock( textMutex );
	sheetFilePath = value != nullptr ? value : "";
	contentDirty  = true;
	return FF_SUCCESS;
}

char* FlipbookPlugin::GetTextParameter( unsigned int index )
{
	if( index == PT_ABOUT_TEXT )
	{
		// Function-local rather than a member: the line is built from
		// compile-time facts, so it is the same for every instance, and the
		// host only needs the pointer to outlive the call. Answered before the
		// lock below -- it shares no state with the text parameters.
		static const std::string aboutLine = stoatworks::about::textParam( 0 );
		return const_cast< char* >( aboutLine.c_str() );
	}

	std::lock_guard< std::mutex > lock( textMutex );

	textReturn[ 0 ] = '\0';
	if( index == PT_SHEET_FILE )
	{
		const size_t length = std::min( sheetFilePath.size(), sizeof( textReturn ) - 1 );
		std::memcpy( textReturn, sheetFilePath.data(), length );
		textReturn[ length ] = '\0';
	}
	return textReturn;
}

//---------------------------------------------------------------------------
// Content
//---------------------------------------------------------------------------
void FlipbookPlugin::ReloadSheet()
{
	std::string path;
	{
		std::lock_guard< std::mutex > lock( textMutex );
		path = sheetFilePath;
	}

	const int columns = std::clamp( static_cast< int >( std::lround( params[ PT_COLUMNS ] ) ), 1, kMaxGrid );
	const int rows    = std::clamp( static_cast< int >( std::lround( params[ PT_ROWS ] ) ), 1, kMaxGrid );

	sheet = LoadSheet( path, columns, rows );

	if( sheet.Valid() )
		diag::info( "sheet loaded: " + sheet.note );
	else if( !path.empty() )
		diag::error( "sheet not loaded: " + sheet.note );

	contentDirty = false;
	uploadDirty  = true;
}

bool FlipbookPlugin::UploadSheet()
{
	if( !sheet.Valid() )
		return false;

	if( sheetTexture == 0 )
		glGenTextures( 1, &sheetTexture );

	glBindTexture( GL_TEXTURE_2D, sheetTexture );
	glPixelStorei( GL_UNPACK_ALIGNMENT, 1 );
	glTexImage2D( GL_TEXTURE_2D, 0, GL_RGBA8, sheet.width, sheet.height, 0, GL_RGBA, GL_UNSIGNED_BYTE,
	              sheet.rgba.data() );

	// No glGenerateMipmap, deliberately. A mip level averages across cell
	// boundaries, so mip 3 of a sprite sheet is every sprite faintly containing
	// its neighbours -- and there is no padding to prevent it, because the
	// sheet's layout belongs to whoever exported it. See Shaders.h.
	//
	// The filters and the wrap live on a sampler object, not here: the same
	// sampler has to serve the input-as-sheet mode, where the texture belongs
	// to the host.
	glBindTexture( GL_TEXTURE_2D, 0 );

	return true;
}

//---------------------------------------------------------------------------
// Clock
//---------------------------------------------------------------------------
void FlipbookPlugin::UpdateClock()
{
	if( forcedSeconds )
		return;

	// FFGL never says what unit SetTime arrives in, and hosts disagree:
	// Resolume sends MILLISECONDS (measured live at 20.0 per frame at its
	// 50 fps, and the SDK's own Particles sample divides by 1000), while the
	// offline harness sends seconds. Reading it raw is a thousand times fast
	// on the one host that matters and exactly right on the one that gets
	// tested, which is how it stays hidden.
	//
	// This used to guess the unit from the magnitude of a single frame delta
	// and then lock. That had three holes: a delta between 0.5 and 2.0 decided
	// nothing, a burst of sub-0.5 ms frames at load -- a thumbnail render on a
	// quick GPU -- locked it to "seconds" for the rest of the session, and
	// while undecided it assumed seconds, which is precisely the millisecond
	// host's wrong answer.
	//
	// So measure instead of guessing. steady_clock says how much real time
	// passed, the host says how much host time passed, and the ratio names the
	// unit outright. Nothing plausible sits between 1 and 1000, so both bands
	// are wide and a frame fitting neither simply does not vote.
	const double wallNow = wallSeconds();
	if( wallStart < 0.0 )
		wallStart = wallNow;

	// Never read `hostTime` before the host has set it: CFFGLPlugin's
	// constructor initialises bpm and barPhase and leaves hostTime
	// uninitialised, so until SetTime lands it is whatever was in that memory.
	const double raw = hostTimeSeen ? hostTime : -1.0;

	if( clockScale == 0.0 && raw >= 0.0 && lastRawTime >= 0.0 && lastWallTime >= 0.0 )
	{
		const double hostDelta = raw - lastRawTime;
		const double wallDelta = wallNow - lastWallTime;

		// A paused host, a looping clip or a stalled frame tells us nothing.
		if( hostDelta > 0.0 && wallDelta >= 0.0005 )
		{
			const double ratio = hostDelta / wallDelta;
			if( ratio > 0.1 && ratio < 10.0 )
				++secondsVotes;
			else if( ratio > 100.0 && ratio < 10000.0 )
				++millisVotes;

			// Several frames rather than one, so a single odd frame -- the
			// first after a seek, say -- cannot decide it on its own.
			if( secondsVotes >= kClockVotes || millisVotes >= kClockVotes )
			{
				clockScale = millisVotes > secondsVotes ? 0.001 : 1.0;
				diag::info( std::string( "host clock is " )
				            + ( clockScale == 0.001 ? "milliseconds" : "seconds" )
				            + ", scale=" + std::to_string( clockScale ) );
			}
		}
	}

	if( raw >= 0.0 )
		lastRawTime = raw;
	lastWallTime = wallNow;

	// Until the unit is settled -- and for a host that never calls SetTime at
	// all -- run on the real clock. Wrong in origin but right in rate, where
	// assuming seconds would be a thousand times fast on Resolume.
	hostSeconds = ( raw >= 0.0 && clockScale != 0.0 ) ? raw * clockScale : wallNow - wallStart;
}

FFResult FlipbookPlugin::SetTime( double time )
{
	hostTimeSeen = true;
	return CFFGLPlugin::SetTime( time );
}

void FlipbookPlugin::SetClockScaleForTest( double scale )
{
	clockScale = scale;
}

void FlipbookPlugin::TickClockForTest()
{
	UpdateClock();
}

double FlipbookPlugin::ClockScaleForTest() const
{
	return clockScale;
}

double FlipbookPlugin::HostSecondsForTest() const
{
	return hostSeconds;
}

void FlipbookPlugin::SetSecondsForTest( double seconds )
{
	forcedSeconds = true;
	hostSeconds   = seconds;
}

double FlipbookPlugin::CurrentFramePos() const
{
	const SyncMode sync = static_cast< SyncMode >( Option( params[ PT_SYNC ], static_cast< int >( SyncMode::Count ) ) );
	const int cells     = std::max( 1, lastSheet.cells );
	const int start     = static_cast< int >( std::lround( params[ PT_START ] ) );
	const int count     = static_cast< int >( std::lround( params[ PT_LENGTH ] ) );
	const int length    = RunLength( start, count, cells );

	return FrameClock( hostSeconds - clockBase, bpm, barPhase, sync,
	                   RateFromParam( params[ PT_RATE ] ), params[ PT_PHASE ], length );
}

int FlipbookPlugin::CellForCopy( int copyIndex, int cells ) const
{
	const PlayMode mode = static_cast< PlayMode >( Option( params[ PT_MODE ], static_cast< int >( PlayMode::Count ) ) );
	const int start     = static_cast< int >( std::lround( params[ PT_START ] ) );
	const int count     = static_cast< int >( std::lround( params[ PT_LENGTH ] ) );
	const int length    = RunLength( start, count, std::max( 1, cells ) );

	const double offset = ( copyIndex >= 0 && copyIndex < static_cast< int >( copies.size() ) )
	                      ? static_cast< double >( copies[ static_cast< size_t >( copyIndex ) ].phaseOffset )
	                      : 0.0;

	return start + FrameOffset( CurrentFramePos() + offset, length, mode );
}

LayoutState FlipbookPlugin::CurrentLayout( int width, int height ) const
{
	LayoutState state;
	state.copies  = std::clamp( static_cast< int >( std::lround( params[ PT_COPIES ] ) ), 1, kMaxCopies );
	state.arrange = static_cast< Arrange >( Option( params[ PT_ARRANGE ], static_cast< int >( Arrange::Count ) ) );
	state.spread  = SpreadFromParam( params[ PT_SPREAD ] );
	state.stagger = StaggerFromParam( params[ PT_STAGGER ] );
	state.seed    = SeedFromParam( params[ PT_SEED ] );

	state.centreX  = params[ PT_POS_X ];
	state.centreY  = params[ PT_POS_Y ];
	state.scale    = ScaleFromParam( params[ PT_SCALE ] );
	state.rotation = RotationFromParam( params[ PT_ROTATION ] );

	state.fit    = static_cast< Fit >( Option( params[ PT_FIT ], static_cast< int >( Fit::Count ) ) );
	state.width  = width;
	state.height = height;
	state.flipH  = params[ PT_FLIP_H ] >= 0.5f;
	state.flipV  = params[ PT_FLIP_V ] >= 0.5f;

	// The cell's proportions, from whichever sheet the last Render used. A
	// default of 1.0 rather than of the frame's aspect, so that a plugin with
	// no sheet yet draws a square placeholder rather than something whose shape
	// changes with the output resolution.
	if( lastSheet.Valid() && lastSheet.columns > 0 && lastSheet.rows > 0 )
	{
		const float cellW = static_cast< float >( lastSheet.width ) / static_cast< float >( lastSheet.columns );
		const float cellH = static_cast< float >( lastSheet.height ) / static_cast< float >( lastSheet.rows );
		if( cellH > 0.0f )
			state.cellAspect = cellW / cellH;
	}

	return state;
}

//---------------------------------------------------------------------------
// GL
//---------------------------------------------------------------------------
FFResult FlipbookPlugin::InitGL( const FFGLViewportStruct* vp )
{
	// Idempotent on purpose. A host is entitled to call InitGL again -- and the
	// offline harness does, on every frame, because InitGL is what sets the
	// viewport the plugin reads its size from. Recompiling two shaders and
	// generating a fresh VAO and sampler each time would leak three GL objects
	// per frame and take several milliseconds doing it.
	if( glReady )
		return CFFGLPlugin::InitGL( vp );

	diag::init();

	const GLubyte* version = glGetString( GL_VERSION );
	diag::info( std::string( "InitGL, GL " )
	            + reinterpret_cast< const char* >( version != nullptr ? version : reinterpret_cast< const GLubyte* >( "unknown" ) ) );

	// One shader per pass, compiled with or without the input path. A shader
	// that will not compile is the failure that actually happens, and from the
	// operator's side it looks like "the plugin does nothing" with no message
	// anywhere -- which is what Diag is for.
	auto withDefines = []( const char* source, bool effect ) {
		std::string text = source;
		if( effect )
		{
			// After the #version line, which must be first in a GLSL source.
			const size_t afterVersion = text.find( '\n' );
			if( afterVersion != std::string::npos )
				text.insert( afterVersion + 1, kEffectDefine );
		}
		return text;
	};

	if( !backgroundShader.Compile( kBackgroundVertexShader,
	                               withDefines( kBackgroundFragmentShader, overInput ).c_str() ) )
	{
		diag::error( "background shader would not compile" );
		DeInitGL();
		return FF_FAIL;
	}

	if( !spriteShader.Compile( kSpriteVertexShader, kSpriteFragmentShader ) )
	{
		diag::error( "sprite shader would not compile" );
		DeInitGL();
		return FF_FAIL;
	}

	// Core profile will not draw without one bound, even for an attributeless
	// draw that reads nothing from it.
	glGenVertexArrays( 1, &emptyVAO );

	glGenSamplers( 1, &sampler );
	glSamplerParameteri( sampler, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE );
	glSamplerParameteri( sampler, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE );

	glReady      = true;
	contentDirty = true;
	return CFFGLPlugin::InitGL( vp );
}

void FlipbookPlugin::Render( int width, int height, const ActiveSheet& input, float maxU, float maxV )
{
	if( contentDirty )
		ReloadSheet();

	if( uploadDirty )
	{
		UploadSheet();
		uploadDirty = false;
	}

	UpdateClock();

	if( resetPending )
	{
		clockBase    = hostSeconds;
		resetPending = false;
	}

	//-----------------------------------------------------------------------
	// Which sheet. The source has no input to offer, so it forces File however
	// the parameter is set -- a composition moved across from the effect keeps
	// its value rather than being silently rewritten, and simply plays its file.
	//-----------------------------------------------------------------------
	const SheetSource wanted = overInput
	                           ? static_cast< SheetSource >( Option( params[ PT_SHEET_SOURCE ], static_cast< int >( SheetSource::Count ) ) )
	                           : SheetSource::File;

	ActiveSheet active;
	if( wanted == SheetSource::Input && input.Valid() )
	{
		active = input;
		// The operator's declared grid, because the clip carries none of its
		// own. Overwritten rather than trusted from the caller so there is one
		// place the grid comes from.
		active.columns = std::clamp( static_cast< int >( std::lround( params[ PT_COLUMNS ] ) ), 1, kMaxGrid );
		active.rows    = std::clamp( static_cast< int >( std::lround( params[ PT_ROWS ] ) ), 1, kMaxGrid );
		active.cells   = active.columns * active.rows;
		active.flipV   = true;
	}
	else if( sheet.Valid() && sheetTexture != 0 )
	{
		active.texture = sheetTexture;
		active.width   = sheet.width;
		active.height  = sheet.height;
		active.columns = sheet.columns;
		active.rows    = sheet.rows;
		active.cells   = std::max( 1, sheet.columns * sheet.rows );
		active.flipV   = false;
	}

	lastSheet = active;

	//-----------------------------------------------------------------------
	// Background pass. Always drawn, even with no sheet: it is what makes
	// Background Opacity work, and for the effect it is what puts the clip
	// down.
	//-----------------------------------------------------------------------
	glBindVertexArray( emptyVAO );
	glDisable( GL_BLEND );

	glUseProgram( backgroundShader.GetGLID() );
	backgroundShader.Set( "BackColour", params[ PT_BACK_R ], params[ PT_BACK_G ], params[ PT_BACK_B ],
	                      params[ PT_BACK_OPACITY ] );

	if( overInput )
	{
		glActiveTexture( GL_TEXTURE0 );
		glBindTexture( GL_TEXTURE_2D, input.texture );
		backgroundShader.Set( "Clip", 0 );
		backgroundShader.Set( "MaxUV", maxU, maxV );
	}

	glDrawArrays( GL_TRIANGLE_STRIP, 0, 4 );

	//-----------------------------------------------------------------------
	// Sprite pass.
	//-----------------------------------------------------------------------
	if( active.Valid() )
	{
		const LayoutState layout = CurrentLayout( width, height );
		SolveCopies( layout, copies );

		const PlayMode mode = static_cast< PlayMode >( Option( params[ PT_MODE ], static_cast< int >( PlayMode::Count ) ) );
		const int start     = static_cast< int >( std::lround( params[ PT_START ] ) );
		const int count     = static_cast< int >( std::lround( params[ PT_LENGTH ] ) );
		const int length    = RunLength( start, count, active.cells );
		const double framePos = CurrentFramePos();

		// A Sheet built from the active one purely so CellRect has something to
		// divide. The pixels are irrelevant to it -- it is arithmetic on the
		// grid -- which is why the input-as-sheet path can use it unchanged.
		Sheet grid;
		grid.width   = active.width;
		grid.height  = active.height;
		grid.columns = active.columns;
		grid.rows    = active.rows;

		const int drawn = static_cast< int >( copies.size() );
		xformUpload.assign( static_cast< size_t >( drawn ) * 4, 0.0f );
		cellUpload.assign( static_cast< size_t >( drawn ) * 4, 0.0f );

		for( int i = 0; i < drawn; ++i )
		{
			const Copy& copy = copies[ static_cast< size_t >( i ) ];

			xformUpload[ i * 4 + 0 ] = copy.centreX;
			xformUpload[ i * 4 + 1 ] = copy.centreY;
			xformUpload[ i * 4 + 2 ] = copy.halfWidth;
			xformUpload[ i * 4 + 3 ] = copy.halfHeight;

			const int cell = start + FrameOffset( framePos + static_cast< double >( copy.phaseOffset ), length, mode );
			CellRect( grid, cell, cellUpload[ i * 4 + 0 ], cellUpload[ i * 4 + 1 ],
			          cellUpload[ i * 4 + 2 ], cellUpload[ i * 4 + 3 ] );
		}

		glUseProgram( spriteShader.GetGLID() );

		glActiveTexture( GL_TEXTURE0 );
		glBindTexture( GL_TEXTURE_2D, active.texture );

		const Sampling sampling = static_cast< Sampling >( Option( params[ PT_SAMPLING ], static_cast< int >( Sampling::Count ) ) );
		const GLint filter      = sampling == Sampling::Pixel ? GL_NEAREST : GL_LINEAR;
		glSamplerParameteri( sampler, GL_TEXTURE_MIN_FILTER, filter );
		glSamplerParameteri( sampler, GL_TEXTURE_MAG_FILTER, filter );
		glBindSampler( 0, sampler );

		spriteShader.Set( "Sheet", 0 );
		spriteShader.Set( "Inset", 0.5f / static_cast< float >( active.width ),
		                  0.5f / static_cast< float >( active.height ) );
		spriteShader.Set( "SheetFlipV", active.flipV ? 1.0f : 0.0f );
		spriteShader.Set( "Rotation", layout.rotation );

		spriteShader.Set( "KeyMode", Option( params[ PT_KEY ], static_cast< int >( KeyMode::Count ) ) );
		spriteShader.Set( "KeyColour", params[ PT_KEY_R ], params[ PT_KEY_G ], params[ PT_KEY_B ] );
		spriteShader.Set( "KeyTolerance", KeyToleranceFromParam( params[ PT_KEY_TOLERANCE ] ) );
		spriteShader.Set( "KeySoftness", KeySoftnessFromParam( params[ PT_KEY_SOFTNESS ] ) );

		spriteShader.Set( "Tint", params[ PT_TINT_R ], params[ PT_TINT_G ], params[ PT_TINT_B ] );
		spriteShader.Set( "Opacity", params[ PT_OPACITY ] * ( overInput ? params[ PT_MIX ] : 1.0f ) );

		// FFGLShader has no array setter, so these go through raw uniform
		// calls. A name that does not match is silent -- glUniform on -1 is a
		// documented no-op -- which is what tools/sweep.py exists to catch.
		glUniform4fv( glGetUniformLocation( spriteShader.GetGLID(), "Xform" ), drawn, xformUpload.data() );
		glUniform4fv( glGetUniformLocation( spriteShader.GetGLID(), "Cell" ), drawn, cellUpload.data() );

		// Premultiplied over. The fragment shader premultiplies on its last
		// line and the host's buffer is premultiplied, so this is the whole of
		// the compositing.
		glEnable( GL_BLEND );
		glBlendFunc( GL_ONE, GL_ONE_MINUS_SRC_ALPHA );

		glDrawArraysInstanced( GL_TRIANGLE_STRIP, 0, 4, drawn );
	}

	//-----------------------------------------------------------------------
	// Put the state back by hand. Every ffglex::Scoped* binding clears to 0 on
	// scope exit rather than restoring, so they are no help here.
	//-----------------------------------------------------------------------
	glDisable( GL_BLEND );
	glBindSampler( 0, 0 );
	glActiveTexture( GL_TEXTURE0 );
	glBindTexture( GL_TEXTURE_2D, 0 );
	glUseProgram( 0 );
	glBindVertexArray( 0 );
}

FFResult FlipbookPlugin::ProcessOpenGL( ProcessOpenGLStruct* pGL )
{
	int width  = 0;
	int height = 0;
	ActiveSheet input;
	float maxU = 1.0f;
	float maxV = 1.0f;

	if( overInput )
	{
		if( pGL == nullptr || pGL->numInputTextures < 1 || pGL->inputTextures[ 0 ] == nullptr )
			return FF_FAIL;

		const FFGLTextureStruct& texture = *pGL->inputTextures[ 0 ];
		width                            = texture.Width;
		height                           = texture.Height;

		input.texture = texture.Handle;
		input.width   = texture.Width;
		input.height  = texture.Height;

		// The input texture can be bigger than the picture; MaxUV is the
		// fraction that was really drawn.
		const FFGLTexCoords coords = GetMaxGLTexCoords( texture );
		maxU                       = coords.s;
		maxV                       = coords.t;
	}
	else
	{
		width  = static_cast< int >( currentViewport.width );
		height = static_cast< int >( currentViewport.height );
	}

	if( width <= 0 || height <= 0 )
		return FF_FAIL;

	Render( width, height, input, maxU, maxV );
	return FF_SUCCESS;
}

FFResult FlipbookPlugin::DeInitGL()
{
	backgroundShader.FreeGLResources();
	spriteShader.FreeGLResources();

	if( sheetTexture != 0 )
	{
		glDeleteTextures( 1, &sheetTexture );
		sheetTexture = 0;
	}
	if( sampler != 0 )
	{
		glDeleteSamplers( 1, &sampler );
		sampler = 0;
	}
	if( emptyVAO != 0 )
	{
		glDeleteVertexArrays( 1, &emptyVAO );
		emptyVAO = 0;
	}

	glReady     = false;
	uploadDirty = true;
	return FF_SUCCESS;
}

} // namespace flipbook
