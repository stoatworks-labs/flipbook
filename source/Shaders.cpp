#include "Shaders.h"

namespace flipbook
{
const char* const kEffectDefine = "#define FLIPBOOK_OVER_INPUT 1\n";

//---------------------------------------------------------------------------
// Background pass
//---------------------------------------------------------------------------
const char* const kBackgroundVertexShader = R"(#version 410 core

out vec2 vUV;

void main()
{
	// Attributeless. A triangle strip of four gives the corners in the order
	// (-1,-1) (1,-1) (-1,1) (1,1), which is what the bit tests below produce.
	vec2 c = vec2( ( gl_VertexID & 1 ) == 0 ? -1.0 : 1.0,
	               ( gl_VertexID & 2 ) == 0 ? -1.0 : 1.0 );

	vUV         = c * 0.5 + 0.5;
	gl_Position = vec4( c, 0.0, 1.0 );
}
)";

const char* const kBackgroundFragmentShader = R"(#version 410 core

in vec2 vUV;

uniform vec4 BackColour;

#ifdef FLIPBOOK_OVER_INPUT
uniform sampler2D Clip;
uniform vec2 MaxUV;
#endif

out vec4 fragColor;

void main()
{
#ifdef FLIPBOOK_OVER_INPUT
	// The clip arrives premultiplied and leaves the same way, so it is laid
	// down untouched and the plugin's own background colour then veils it --
	// which keeps Background Alpha meaning the same thing in both plugins
	// rather than becoming a second, differently-behaved control on this side.
	vec4 clip = texture( Clip, vUV * MaxUV );
	vec4 veil = vec4( BackColour.rgb * BackColour.a, BackColour.a );
	fragColor = veil + clip * ( 1.0 - BackColour.a );
#else
	fragColor = vec4( BackColour.rgb * BackColour.a, BackColour.a );
#endif
}
)";

//---------------------------------------------------------------------------
// Sprite pass
//---------------------------------------------------------------------------
//
// The array sizes are written out as 64 rather than substituted from C++
// because the shader is a plain string literal. Flipbook.cpp carries a
// static_assert that kMaxCopies still equals 64, so raising one without the
// other is a build error rather than a uniform-array overrun.
//
const char* const kSpriteVertexShader = R"(#version 410 core

uniform vec4 Xform[ 64 ];   // centre.xy in frame space (y down), half extents.xy
uniform vec4 Cell[ 64 ];    // the cell rect in sheet UV: u0, v0, u1, v1
uniform float Rotation;     // radians, shared by every copy

out vec2 vCellUV;           // 0..1 within the cell, y down
out vec4 vCell;

void main()
{
	vec2 c = vec2( ( gl_VertexID & 1 ) == 0 ? -1.0 : 1.0,
	               ( gl_VertexID & 2 ) == 0 ? -1.0 : 1.0 );

	vec4 xf = Xform[ gl_InstanceID ];
	vCell   = Cell[ gl_InstanceID ];

	// The half extents already carry the cell's aspect, the fit, the scale and
	// the short-edge correction -- and their SIGN carries the flips. Multiplying
	// a unit corner by them is the whole of the sizing; see Placement.h for why
	// none of that arithmetic is here.
	vec2 local = c * xf.zw;

	float ca = cos( Rotation );
	float sa = sin( Rotation );
	vec2 rotated = vec2( local.x * ca - local.y * sa,
	                     local.x * sa + local.y * ca );

	vec2 framePos = xf.xy + rotated;

	// c runs -1..1 with -1 at the top in frame space, and the cell rect's v0 is
	// the top of the cell, so this maps corner to corner with no flip. Taken
	// from c rather than from the rotated position, because it has to be the
	// sprite's own coordinate: rotating the quad must rotate the picture with
	// it, not slide the sprite across a fixed window.
	vCellUV = c * 0.5 + 0.5;

	// Frame space (0..1, y down) to clip space (-1..1, y up).
	gl_Position = vec4( framePos.x * 2.0 - 1.0, 1.0 - framePos.y * 2.0, 0.0, 1.0 );
}
)";

const char* const kSpriteFragmentShader = R"(#version 410 core

in vec2 vCellUV;
in vec4 vCell;

uniform sampler2D Sheet;

// Half a texel of the sheet, on each axis. The sample is clamped into the cell
// by this much, so GL_LINEAR at a cell boundary repeats the cell's own outer
// texel instead of blending in the first texel of the neighbouring sprite.
// Without it every sprite carries a one-pixel seam of the one beside it, which
// reads as a decode fault rather than as a filtering choice.
uniform vec2 Inset;

// 1 when the sheet is the incoming clip, which the host hands over with v = 0
// at the bottom; 0 for a sheet we uploaded ourselves, whose first row is its
// top. The one place that difference lives.
uniform float SheetFlipV;

uniform int KeyMode;        // 0 alpha, 1 luma, 2 colour, 3 none
uniform vec3 KeyColour;
uniform float KeyTolerance;
uniform float KeySoftness;

uniform vec3 Tint;
uniform float Opacity;

out vec4 fragColor;

void main()
{
	vec2 cellUV = vCellUV;
	if( SheetFlipV > 0.5 )
		cellUV.y = 1.0 - cellUV.y;

	vec2 uv = mix( vCell.xy, vCell.zw, cellUV );
	uv = clamp( uv, vCell.xy + Inset, vCell.zw - Inset );

	// Straight alpha, on purpose, and it stays that way until the last line of
	// this function. Keying a premultiplied pixel compares the operator's
	// swatch against rgb * a and matches nothing on any sheet with soft edges.
	vec4 texel = texture( Sheet, uv );

	float alpha = texel.a;

	if( KeyMode == 1 )
	{
		// Luma: distance in brightness only, so a background that is flat in
		// tone but noisy in hue -- a white studio sweep, a black that is not
		// quite black -- comes out cleanly. The weights are Rec. 709.
		float luma    = dot( texel.rgb, vec3( 0.2126, 0.7152, 0.0722 ) );
		float keyLuma = dot( KeyColour, vec3( 0.2126, 0.7152, 0.0722 ) );
		alpha *= smoothstep( KeyTolerance, KeyTolerance + KeySoftness + 0.0001, abs( luma - keyLuma ) );
	}
	else if( KeyMode == 2 )
	{
		// Colour: straight RGB distance. Not a chroma key -- there is no hue
		// separation and no spill suppression, because a sprite sheet's
		// background is a flat exported colour rather than a lit cyclorama, and
		// the machinery that makes a real chroma key work would only give this
		// one more ways to be set wrong.
		float d = length( texel.rgb - KeyColour );
		alpha *= smoothstep( KeyTolerance, KeyTolerance + KeySoftness + 0.0001, d );
	}
	else if( KeyMode == 3 )
	{
		// None: the file's own alpha is ignored rather than honoured. The mode
		// for looking at a sheet whole, including whatever it has in its
		// transparent regions -- which on a badly exported sheet is not black.
		alpha = 1.0;
	}

	alpha *= Opacity;

	// Premultiply, once, here. Everything above this line is straight alpha and
	// everything below the plugin is premultiplied.
	fragColor = vec4( texel.rgb * Tint * alpha, alpha );
}
)";

} // namespace flipbook
