#include "Playback.h"

#include <algorithm>
#include <cmath>

namespace flipbook
{
double FrameClock( double seconds, float bpm, float barPhase, SyncMode mode,
                   float rate, float manualPhase, int length )
{
	const int runLength = std::max( 1, length );

	if( mode == SyncMode::Manual )
	{
		// Across exactly one pass, and *not* one frame short of it. The slider
		// at 1.0 should be the last frame, so the span is length - 1 rather
		// than length -- with length as the multiplier, the top of the travel
		// lands one past the end and wraps to the first frame, which reads as
		// the slider skipping back to the start just before it gets there.
		const double v = std::clamp( static_cast< double >( manualPhase ), 0.0, 1.0 );
		return v * static_cast< double >( runLength - 1 );
	}

	const double perUnit = static_cast< double >( rate );

	if( mode == SyncMode::Free )
		return seconds * perUnit;

	const double tempo      = bpm > 1.0f ? static_cast< double >( bpm ) : 120.0;
	const double barSeconds = 240.0 / tempo;//four beats to the bar
	const double estimate   = seconds / barSeconds;
	const double within     = std::clamp( static_cast< double >( barPhase ), 0.0, 1.0 );

	const double bars = within + std::round( estimate - within );

	// Rate is frames per beat or per bar in these modes. Unlike downpour's rain
	// there is no easing curve on the fraction: a sprite sheet is already a
	// sequence of discrete frames, and easing the position inside an interval
	// would hold some frames for longer than others -- which is not "kicking on
	// the grid", it is dropping frames.
	return ( mode == SyncMode::Beat ? bars * 4.0 : bars ) * perUnit;
}

int FrameOffset( double framePos, int length, PlayMode mode )
{
	const int runLength = std::max( 1, length );
	if( runLength == 1 )
		return 0;

	// Floor, not truncate. A negative clock -- which a host scrubbing backwards
	// through a timeline hands over quite normally -- truncates towards zero,
	// so frames -0.5 and +0.5 would both be frame 0 and the sequence would
	// stutter on one frame every time it crossed the origin.
	const double floored = std::floor( framePos );

	switch( mode )
	{
	case PlayMode::Once:
	{
		const double clamped = std::clamp( floored, 0.0, static_cast< double >( runLength - 1 ) );
		return static_cast< int >( clamped );
	}

	case PlayMode::PingPong:
	{
		const int period = 2 * runLength - 2;
		int step         = static_cast< int >( std::fmod( floored, static_cast< double >( period ) ) );
		if( step < 0 )
			step += period;
		return step < runLength ? step : period - step;
	}

	case PlayMode::Loop:
	default:
	{
		int step = static_cast< int >( std::fmod( floored, static_cast< double >( runLength ) ) );
		if( step < 0 )
			step += runLength;
		return step;
	}
	}
}

int RunLength( int start, int frameCount, int cells )
{
	const int total = std::max( 1, cells );
	const int from  = std::clamp( start, 0, total - 1 );

	if( frameCount <= 0 )
		return std::max( 1, total - from );

	// Not clamped to the end of the sheet. CellRect wraps, so a run that starts
	// at cell 14 of 16 and asks for 6 frames gets 14, 15, 0, 1, 2, 3 -- which is
	// a legitimate thing to want on a sheet whose animation straddles the last
	// row, and which clamping would silently turn into a two-frame loop.
	return std::max( 1, std::min( frameCount, total ) );
}

} // namespace flipbook
