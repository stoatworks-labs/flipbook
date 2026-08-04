#include "Controls.h"

#include <algorithm>
#include <cmath>

namespace flipbook
{
namespace
{
float Saturate( float v )
{
	return v < 0.0f ? 0.0f : ( v > 1.0f ? 1.0f : v );
}

/// Geometric interpolation: equal slider travel gives equal *ratio*, which is
/// how the eye reads a rate.
float Exponential( float value, float low, float high )
{
	return low * std::pow( high / low, Saturate( value ) );
}

float Linear( float value, float low, float high )
{
	return low + ( high - low ) * Saturate( value );
}
} // namespace

int Option( float value, int count )
{
	const int index = static_cast< int >( std::lround( value ) );
	return std::max( 0, std::min( count - 1, index ) );
}

float RateFromParam( float value )
{
	return Exponential( value, 0.5f, 60.0f );
}

float ScaleFromParam( float value )
{
	// Two geometric segments meeting at exactly 1.0 in the middle. A single
	// 0.05..4 exponential would put 1.0 at 0.73 of the travel, which is a place
	// nobody finds and everybody wants.
	const float v = Saturate( value );
	if( v <= 0.5f )
		return Exponential( v / 0.5f, 0.05f, 1.0f );
	return Exponential( ( v - 0.5f ) / 0.5f, 1.0f, 4.0f );
}

float RotationFromParam( float value )
{
	constexpr float kPi = 3.14159265358979323846f;
	return Linear( value, -kPi, kPi );
}

float SpreadFromParam( float value )
{
	return Linear( value, 0.0f, 1.5f );
}

float StaggerFromParam( float value )
{
	// Below a twentieth of the travel the answer is exactly zero. Without the
	// dead zone the bottom of the range is a twentieth of a frame of offset per
	// copy, which is invisible on a still frame and reads as the copies
	// twitching out of step in motion -- a bug report waiting to happen, from a
	// slider the operator believes is at zero.
	const float v = Saturate( value );
	if( v < 0.05f )
		return 0.0f;
	return Linear( ( v - 0.05f ) / 0.95f, 0.0f, 8.0f );
}

uint32_t SeedFromParam( float value )
{
	// Quantised on purpose. The seed picks a scatter, not a position in a
	// continuum, and a slider that changes the answer on every pixel of travel
	// cannot be set to the same thing twice.
	return static_cast< uint32_t >( 1 + static_cast< int >( std::lround( Saturate( value ) * 9998.0f ) ) );
}

float KeyToleranceFromParam( float value )
{
	const float v = Saturate( value );
	return v * v;
}

float KeySoftnessFromParam( float value )
{
	const float v = Saturate( value );
	return v * v * 0.5f;
}

} // namespace flipbook
