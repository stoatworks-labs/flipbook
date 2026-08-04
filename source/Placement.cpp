#include "Placement.h"

#include <algorithm>
#include <cmath>

#include "Hash.h"

namespace flipbook
{
namespace
{
constexpr float kPi = 3.14159265358979323846f;

/// The short-edge correction, expressed as a fraction of each axis.
///
/// A size given as a fraction of the short edge has to be divided by the long
/// axis's length in short-edge units before it can be used as a fraction of
/// that axis. On a 16:9 raster that is (9/16, 1): a sprite half the short edge
/// tall is half the frame's height and 0.28 of its width, which is the same
/// number of pixels.
void ShortEdge( int width, int height, float& sx, float& sy )
{
	if( width <= 0 || height <= 0 )
	{
		sx = sy = 1.0f;
		return;
	}

	const float w = static_cast< float >( width );
	const float h = static_cast< float >( height );

	if( w >= h )
	{
		sx = h / w;
		sy = 1.0f;
	}
	else
	{
		sx = 1.0f;
		sy = w / h;
	}
}
} // namespace

void SolveCopies( const LayoutState& state, std::vector< Copy >& out )
{
	const int count = std::clamp( state.copies, 1, kMaxCopies );
	out.assign( static_cast< size_t >( count ), Copy {} );

	float sx = 1.0f, sy = 1.0f;
	ShortEdge( state.width, state.height, sx, sy );

	const float frameAspect = ( state.height > 0 )
	                          ? static_cast< float >( state.width ) / static_cast< float >( state.height )
	                          : 1.0f;
	const float cellAspect = state.cellAspect > 0.0001f ? state.cellAspect : 1.0f;

	//-----------------------------------------------------------------------
	// Size. Fit decides the base half-extents; Scale multiplies whichever it
	// was, so 1.0 means "the size Fit said" in all three modes.
	//-----------------------------------------------------------------------
	float halfW = 0.5f;
	float halfH = 0.5f;

	switch( state.fit )
	{
	case Fit::Native:
		// Height is the short-edge fraction; width follows the cell's own
		// proportions. Both then corrected onto their own axis.
		halfH = 0.5f * sy;
		halfW = 0.5f * cellAspect * sx;
		break;

	case Fit::Fill:
		// Cover the raster, cropping the overflow on whichever axis is
		// proportionally smaller. No short-edge correction anywhere here: Fill
		// is defined against the frame, not against the short edge, and
		// applying it as well would shrink the sprite off one edge.
		if( cellAspect > frameAspect )
		{
			halfH = 0.5f;
			halfW = 0.5f * cellAspect / frameAspect;
		}
		else
		{
			halfW = 0.5f;
			halfH = 0.5f * frameAspect / cellAspect;
		}
		break;

	case Fit::Stretch:
	default:
		halfW = 0.5f;
		halfH = 0.5f;
		break;
	}

	halfW *= state.scale;
	halfH *= state.scale;

	// The flips are applied to the half-extents rather than to the texture
	// coordinates, so a flip composes with rotation the way it looks like it
	// should: flipping a rotated sprite mirrors what is on the screen. Doing it
	// in UV space would mirror the sprite inside its own rotated box, which
	// gives a different picture for every angle but 0.
	if( state.flipH )
		halfW = -halfW;
	if( state.flipV )
		halfH = -halfH;

	//-----------------------------------------------------------------------
	// Placement.
	//-----------------------------------------------------------------------
	// A near-square grid, wider than tall when it cannot be square -- which is
	// the way a contact sheet reads and the way the eye expects a field of
	// sprites to fill a landscape raster.
	const int gridColumns = std::max( 1, static_cast< int >( std::ceil( std::sqrt( static_cast< double >( count ) ) ) ) );
	const int gridRows    = std::max( 1, ( count + gridColumns - 1 ) / gridColumns );

	for( int i = 0; i < count; ++i )
	{
		Copy& copy = out[ static_cast< size_t >( i ) ];

		copy.halfWidth  = halfW;
		copy.halfHeight = halfH;
		copy.rotation   = state.rotation;

		// A chase, not chaos: copy i is i * stagger frames behind copy 0 in
		// every arrangement, scattered ones included. Sixty-four sprites at
		// independent random phases is noise; sixty-four a fixed step apart is
		// a wave travelling through them, which is the thing worth having.
		copy.phaseOffset = static_cast< float >( i ) * state.stagger;

		switch( state.arrange )
		{
		case Arrange::Ring:
		{
			// Twelve o'clock first, then clockwise, so that a stagger reads as
			// a chase going the way the numbers do.
			const float angle = -kPi * 0.5f + 2.0f * kPi * static_cast< float >( i ) / static_cast< float >( count );
			const float r     = state.spread * 0.5f;
			copy.centreX      = state.centreX + std::cos( angle ) * r * sx;
			copy.centreY      = state.centreY + std::sin( angle ) * r * sy;
			break;
		}

		case Arrange::Scatter:
		{
			// Seeded by the copy index, so adding a copy leaves the ones
			// already placed exactly where they were. Seeding by (index,
			// count) instead would reshuffle the whole field every time the
			// spinner moved, which makes Copies unusable as a live control.
			copy.centreX = state.centreX + Hash11( static_cast< uint32_t >( i ), state.seed ) * state.spread * 0.5f * sx;
			copy.centreY = state.centreY + Hash11( static_cast< uint32_t >( i ), state.seed ^ 0x5bf03635u ) * state.spread * 0.5f * sy;
			break;
		}

		case Arrange::Grid:
		default:
		{
			if( count == 1 )
			{
				copy.centreX = state.centreX;
				copy.centreY = state.centreY;
				break;
			}

			const float column = static_cast< float >( i % gridColumns );
			const float row    = static_cast< float >( i / gridColumns );

			// Centred on the whole grid, not on cell 0, so raising Copies grows
			// the field outwards from Position rather than dragging it off to
			// one side.
			const float offsetX = ( column - ( static_cast< float >( gridColumns ) - 1.0f ) * 0.5f );
			const float offsetY = ( row - ( static_cast< float >( gridRows ) - 1.0f ) * 0.5f );

			const float step = ( gridColumns > 1 || gridRows > 1 )
			                   ? state.spread / static_cast< float >( std::max( gridColumns, gridRows ) )
			                   : 0.0f;

			copy.centreX = state.centreX + offsetX * step * sx;
			copy.centreY = state.centreY + offsetY * step * sy;
			break;
		}
		}
	}
}

} // namespace flipbook
