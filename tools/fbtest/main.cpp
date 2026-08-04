/**
    fbtest -- the offline harness.

    It drives **the real plugin class** through the real FFGL sequence in a
    headless core-profile context. Not a reimplementation of the playback and
    not a preview: the thing under test is `FlipbookPlugin`, compiled from the
    same objects that go into the bundles, and every number below comes out of a
    frame it actually rendered.

        --make-sheet PATH   write the synthetic test sheet
        --out PATH          render a frame
        --list              parameters, with their types and defaults
        --sheet             what the sheet decoded to
        --frames            the cell on screen against Playback.cpp
        --copies            where each copy landed, and which cell it showed
        --aspect            a square cell stays square off 1:1
        --seam              no cell bleeds into its neighbour
        --key               the four key modes, on known colours
        --presets           every factory preset frames itself on the raster

    ## The synthetic sheet

    Nearly every test below needs to answer "which cell is on screen?" from
    pixels, so `--make-sheet` writes a sheet whose cells are **flat, saturated,
    well-separated colours**. Identifying a cell is then a nearest-colour lookup
    that cannot be fooled, and — the part that matters more — a *neighbouring*
    cell's colour appearing anywhere inside the drawn sprite is unambiguous
    evidence of a sampling bug rather than something that has to be argued
    about. That is what makes `--seam` possible at all.

    Cell 0 is drawn at half alpha and every other cell is opaque, so the alpha
    path is exercised without a second sheet.

    ## What each test can and cannot catch

    `--frames` walks the clock and checks the cell on screen. It exercises the
    clock, the run arithmetic, `CellRect`, the uniform upload and the vertex
    transform at once. It cannot catch a sprite drawn in the wrong *place*,
    because it only samples the centre.

    `--copies` is the one that can: it measures at the centre predicted by
    `Placement.cpp` for every copy, so a copy that is somewhere else registers
    as background and fails. Deliberately not a nearest-blob search — that would
    assume the very thing under test.

    `--aspect` is the two-coordinate-conventions trap, and it checks the sprite's
    *size* as well as its proportions, because sizing off the wrong edge gives a
    perfectly square sprite of entirely the wrong size.

    `--seam` is the half-texel inset, and it is the test this plugin most needs.

    `--presets` checks the framing of every factory preset, which none of the
    others touch: they all use their own parameter values, and the sweep only
    asks whether the Preset dropdown changes the picture at all.

    None of them catches a dead uniform. See `tools/sweep.py`.
*/

#include <OpenGL/OpenGL.h>
#include <OpenGL/gl3.h>
#include <zlib.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <vector>

#include "Controls.h"
#include "Flipbook.h"
#include "Placement.h"
#include "Playback.h"
#include "Presets.h"
#include "Sheet.h"

using namespace flipbook;

namespace
{
//---------------------------------------------------------------------------
// A PNG writer. zlib ships with the OS, so this is a few chunk headers and a
// CRC rather than a dependency.
//---------------------------------------------------------------------------
void putU32( std::vector< unsigned char >& out, uint32_t value )
{
	out.push_back( static_cast< unsigned char >( value >> 24 ) );
	out.push_back( static_cast< unsigned char >( value >> 16 ) );
	out.push_back( static_cast< unsigned char >( value >> 8 ) );
	out.push_back( static_cast< unsigned char >( value ) );
}

void putChunk( std::vector< unsigned char >& out, const char* type, const std::vector< unsigned char >& data )
{
	putU32( out, static_cast< uint32_t >( data.size() ) );
	const size_t start = out.size();
	out.insert( out.end(), type, type + 4 );
	out.insert( out.end(), data.begin(), data.end() );
	uLong crc = crc32( 0L, Z_NULL, 0 );
	crc       = crc32( crc, out.data() + start, static_cast< uInt >( 4 + data.size() ) );
	putU32( out, static_cast< uint32_t >( crc ) );
}

bool writePng( const std::string& path, int width, int height, const std::vector< unsigned char >& rgba )
{
	std::vector< unsigned char > raw;
	raw.reserve( static_cast< size_t >( height ) * ( 1 + static_cast< size_t >( width ) * 4 ) );
	for( int y = 0; y < height; ++y )
	{
		raw.push_back( 0 );// filter: none
		const unsigned char* row = rgba.data() + static_cast< size_t >( y ) * width * 4;
		raw.insert( raw.end(), row, row + static_cast< size_t >( width ) * 4 );
	}

	uLongf compressedSize = compressBound( static_cast< uLong >( raw.size() ) );
	std::vector< unsigned char > compressed( compressedSize );
	if( compress2( compressed.data(), &compressedSize, raw.data(), static_cast< uLong >( raw.size() ), 6 ) != Z_OK )
		return false;
	compressed.resize( compressedSize );

	std::vector< unsigned char > png = { 0x89, 'P', 'N', 'G', 0x0d, 0x0a, 0x1a, 0x0a };

	std::vector< unsigned char > header;
	putU32( header, static_cast< uint32_t >( width ) );
	putU32( header, static_cast< uint32_t >( height ) );
	header.push_back( 8 );// bit depth
	header.push_back( 6 );// colour type: RGBA
	header.push_back( 0 );
	header.push_back( 0 );
	header.push_back( 0 );
	putChunk( png, "IHDR", header );
	putChunk( png, "IDAT", compressed );
	putChunk( png, "IEND", {} );

	std::FILE* file = std::fopen( path.c_str(), "wb" );
	if( file == nullptr )
		return false;
	const size_t written = std::fwrite( png.data(), 1, png.size(), file );
	std::fclose( file );
	return written == png.size();
}

//---------------------------------------------------------------------------
// The synthetic sheet.
//---------------------------------------------------------------------------
constexpr int kTestColumns = 5;
constexpr int kTestRows    = 4;
constexpr int kTestCells   = kTestColumns * kTestRows;
constexpr int kTestCellPx  = 100;///< square, so --aspect has something to assert

struct Rgb
{
	float r = 0.0f, g = 0.0f, b = 0.0f;
};

/// Cell colours: full-saturation hues, evenly spaced. Twenty cells is eighteen
/// degrees apart, which is far outside anything filtering or rounding moves a
/// colour by, and the reason a nearest-colour match is safe to assert on.
Rgb TestColour( int cell )
{
	const float h = static_cast< float >( cell % kTestCells ) / static_cast< float >( kTestCells ) * 6.0f;
	const int sector = static_cast< int >( h ) % 6;
	const float f    = h - std::floor( h );

	switch( sector )
	{
	case 0: return { 1.0f, f, 0.0f };
	case 1: return { 1.0f - f, 1.0f, 0.0f };
	case 2: return { 0.0f, 1.0f, f };
	case 3: return { 0.0f, 1.0f - f, 1.0f };
	case 4: return { f, 0.0f, 1.0f };
	default: return { 1.0f, 0.0f, 1.0f - f };
	}
}

/// Two sheets, because the assertions and the sweep want opposite things from
/// one.
///
/// **Flat** is what every check in this file needs: a cell is one exact colour,
/// so "which cell is this" is a lookup that cannot be argued with and "is this
/// pixel from the neighbouring cell" has a yes-or-no answer.
///
/// **Detailed** is what `tools/sweep.py` needs, and the flat sheet is actively
/// useless to it. Two controls are *invisible* on a flat cell however correctly
/// they work: Flip, because a flat square is its own mirror image, and
/// Sampling, because nearest and bilinear agree exactly everywhere inside a
/// region of constant colour. A sweep run against the flat sheet would report
/// both as dead controls, every time, and be right to. So the detailed cells
/// carry an asymmetric corner mark and a one-pixel checker: the mark breaks the
/// mirror symmetry, and the checker is the highest frequency the sheet can hold,
/// which is the only thing a filter choice can show up on.
int makeSheet( const std::string& path, bool detailed )
{
	const int width  = kTestColumns * kTestCellPx;
	const int height = kTestRows * kTestCellPx;
	std::vector< unsigned char > rgba( static_cast< size_t >( width ) * height * 4 );

	for( int cell = 0; cell < kTestCells; ++cell )
	{
		const Rgb c        = TestColour( cell );
		const int originX  = ( cell % kTestColumns ) * kTestCellPx;
		const int originY  = ( cell / kTestColumns ) * kTestCellPx;
		const unsigned char alpha = cell == 0 ? 128 : 255;

		for( int y = 0; y < kTestCellPx; ++y )
		{
			for( int x = 0; x < kTestCellPx; ++x )
			{
				float r = c.r, g = c.g, b = c.b;

				if( detailed )
				{
					// A solid white block in one corner only: asymmetric on
					// both axes, so Flip H and Flip V each change the picture
					// on their own rather than only together.
					if( x < kTestCellPx / 5 && y < kTestCellPx / 4 )
						r = g = b = 1.0f;
					// A one-pixel checker over the lower half. Nyquist's worst
					// case, and the only pattern a nearest-versus-bilinear
					// choice is guaranteed to be visible on.
					else if( y > kTestCellPx / 2 && ( ( x + y ) & 1 ) != 0 )
					{
						r *= 0.25f;
						g *= 0.25f;
						b *= 0.25f;
					}
				}

				unsigned char* p = rgba.data()
				                   + ( static_cast< size_t >( originY + y ) * width + originX + x ) * 4;
				p[ 0 ] = static_cast< unsigned char >( std::lround( r * 255.0f ) );
				p[ 1 ] = static_cast< unsigned char >( std::lround( g * 255.0f ) );
				p[ 2 ] = static_cast< unsigned char >( std::lround( b * 255.0f ) );
				p[ 3 ] = alpha;
			}
		}
	}

	if( !writePng( path, width, height, rgba ) )
	{
		std::fprintf( stderr, "could not write %s\n", path.c_str() );
		return 1;
	}

	std::printf( "wrote %s (%d x %d, %d x %d cells of %d px, cell 0 at half alpha%s)\n",
	             path.c_str(), width, height, kTestColumns, kTestRows, kTestCellPx,
	             detailed ? ", detailed" : "" );
	return 0;
}

/// Which cell a rendered pixel came from, or -1 if it matches none of them
/// closely enough. `tolerance` is generous because a bilinear fetch inside a
/// flat cell is still exact but an 8-bit round trip is not.
int cellFromColour( float r, float g, float b, float tolerance = 0.12f )
{
	int best        = -1;
	float bestError = tolerance;
	for( int cell = 0; cell < kTestCells; ++cell )
	{
		const Rgb c     = TestColour( cell );
		const float err = std::sqrt( ( r - c.r ) * ( r - c.r ) + ( g - c.g ) * ( g - c.g ) + ( b - c.b ) * ( b - c.b ) );
		if( err < bestError )
		{
			bestError = err;
			best      = cell;
		}
	}
	return best;
}

//---------------------------------------------------------------------------
// GL plumbing.
//---------------------------------------------------------------------------
CGLContextObj createContext()
{
	const CGLPixelFormatAttribute accelerated[] = {
		kCGLPFAOpenGLProfile, static_cast< CGLPixelFormatAttribute >( kCGLOGLPVersion_GL4_Core ),
		kCGLPFAAccelerated,
		kCGLPFAColorSize, static_cast< CGLPixelFormatAttribute >( 24 ),
		kCGLPFAAlphaSize, static_cast< CGLPixelFormatAttribute >( 8 ),
		static_cast< CGLPixelFormatAttribute >( 0 )
	};
	const CGLPixelFormatAttribute software[] = {
		kCGLPFAOpenGLProfile, static_cast< CGLPixelFormatAttribute >( kCGLOGLPVersion_GL4_Core ),
		kCGLPFAColorSize, static_cast< CGLPixelFormatAttribute >( 24 ),
		kCGLPFAAlphaSize, static_cast< CGLPixelFormatAttribute >( 8 ),
		static_cast< CGLPixelFormatAttribute >( 0 )
	};

	CGLPixelFormatObj format = nullptr;
	GLint formatCount        = 0;
	if( CGLChoosePixelFormat( accelerated, &format, &formatCount ) != kCGLNoError || format == nullptr )
	{
		if( CGLChoosePixelFormat( software, &format, &formatCount ) != kCGLNoError || format == nullptr )
			return nullptr;
	}

	CGLContextObj context = nullptr;
	const CGLError error  = CGLCreateContext( format, nullptr, &context );
	CGLDestroyPixelFormat( format );
	if( error != kCGLNoError )
		return nullptr;

	CGLSetCurrentContext( context );
	return context;
}

struct Target
{
	GLuint texture = 0;
	GLuint fbo     = 0;
	int width      = 0;
	int height     = 0;
};

Target makeTarget( int width, int height )
{
	Target target;
	target.width  = width;
	target.height = height;

	glGenTextures( 1, &target.texture );
	glBindTexture( GL_TEXTURE_2D, target.texture );
	glTexImage2D( GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST );
	glBindTexture( GL_TEXTURE_2D, 0 );

	glGenFramebuffers( 1, &target.fbo );
	glBindFramebuffer( GL_FRAMEBUFFER, target.fbo );
	glFramebufferTexture2D( GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, target.texture, 0 );
	return target;
}

void releaseTarget( Target& target )
{
	if( target.fbo != 0 )
		glDeleteFramebuffers( 1, &target.fbo );
	if( target.texture != 0 )
		glDeleteTextures( 1, &target.texture );
	target = Target();
}

/// Straight out of GL, **bottom row first**. Every sampler below takes frame
/// coordinates with y down and flips here, in one place.
std::vector< unsigned char > readBytes( const Target& target )
{
	std::vector< unsigned char > pixels( static_cast< size_t >( target.width ) * target.height * 4 );
	glBindFramebuffer( GL_FRAMEBUFFER, target.fbo );
	glPixelStorei( GL_PACK_ALIGNMENT, 1 );
	glReadPixels( 0, 0, target.width, target.height, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data() );
	return pixels;
}

std::vector< unsigned char > flipRows( const std::vector< unsigned char >& image, int width, int height )
{
	std::vector< unsigned char > flipped( image.size() );
	const size_t stride = static_cast< size_t >( width ) * 4;
	for( int y = 0; y < height; ++y )
		std::memcpy( flipped.data() + static_cast< size_t >( y ) * stride,
		             image.data() + static_cast< size_t >( height - 1 - y ) * stride, stride );
	return flipped;
}

/// One pixel, in frame coordinates (0..1, y down), un-premultiplied.
///
/// Un-premultiplied because everything downstream compares against the sheet's
/// own colours, which are straight. Comparing a premultiplied pixel against a
/// straight swatch is the same mistake the shader is careful not to make, and
/// it would show up here as every half-alpha cell failing to identify.
bool samplePixel( const std::vector< unsigned char >& bottomUp, int width, int height,
                  float fx, float fy, float& r, float& g, float& b, float& a )
{
	const int x = std::clamp( static_cast< int >( fx * static_cast< float >( width ) ), 0, width - 1 );
	const int yDown = std::clamp( static_cast< int >( fy * static_cast< float >( height ) ), 0, height - 1 );
	const int y     = height - 1 - yDown;

	const unsigned char* p = bottomUp.data() + ( static_cast< size_t >( y ) * width + x ) * 4;
	a                      = static_cast< float >( p[ 3 ] ) / 255.0f;
	if( a <= 0.001f )
	{
		r = g = b = 0.0f;
		return false;
	}

	r = static_cast< float >( p[ 0 ] ) / 255.0f / a;
	g = static_cast< float >( p[ 1 ] ) / 255.0f / a;
	b = static_cast< float >( p[ 2 ] ) / 255.0f / a;
	return true;
}

//---------------------------------------------------------------------------
// Driving the plugin.
//---------------------------------------------------------------------------
bool render( FlipbookPlugin& plugin, const Target& target, double seconds, GLuint input = 0 )
{
	FFGLViewportStruct viewport {};
	viewport.width  = static_cast< FFUInt32 >( target.width );
	viewport.height = static_cast< FFUInt32 >( target.height );

	// Straight into the plugin's normalised clock, bypassing the milliseconds-
	// or-seconds detection. The harness wants frame 3.25 exactly, not "whatever
	// 3.25 of some unit turns out to mean once two frames have gone by".
	plugin.SetSecondsForTest( seconds );

	// A synthetic transport to go with it: 120 BPM in 4/4 from time zero, so
	// bar N starts at exactly 2N seconds and the Beat and Bar modes are as
	// reproducible offline as Free is. Left at the SDK's defaults, barPhase
	// would sit frozen at zero and a synced sheet would step once a bar.
	constexpr double kBarSeconds = 2.0;
	plugin.SetBeatInfo( 120.0f, static_cast< float >( std::fmod( seconds, kBarSeconds ) / kBarSeconds ) );

	FFGLTextureStruct inputStruct {};
	inputStruct.Width = inputStruct.HardwareWidth = static_cast< FFUInt32 >( target.width );
	inputStruct.Height = inputStruct.HardwareHeight = static_cast< FFUInt32 >( target.height );
	inputStruct.Handle                              = input;
	FFGLTextureStruct* inputs[ 1 ]                  = { &inputStruct };

	ProcessOpenGLStruct process {};
	process.numInputTextures = input != 0 ? 1 : 0;
	process.inputTextures    = input != 0 ? inputs : nullptr;
	process.HostFBO          = target.fbo;

	glBindFramebuffer( GL_FRAMEBUFFER, target.fbo );
	glViewport( 0, 0, target.width, target.height );
	glClearColor( 0.0f, 0.0f, 0.0f, 0.0f );
	glClear( GL_COLOR_BUFFER_BIT );

	// The plugin reads its size out of the viewport its base class holds, and
	// InitGL is what sets it. Calling it per frame is what lets one harness
	// process render at several sizes without tearing the GL resources down --
	// which is safe because FlipbookPlugin::InitGL is idempotent.
	plugin.InitGL( &viewport );
	return plugin.ProcessOpenGL( &process ) == FF_SUCCESS;
}

/// Every parameter's host-facing name, read out of the plugin itself.
///
/// Built at runtime rather than kept as a table beside Controls.h, and that is
/// not tidiness: a hand-written table is a second place for a name to live, and
/// the failure it produces is a `--set` or a video cue that silently addresses
/// nothing while everything else about the run looks correct.
std::map< std::string, unsigned int > parameterIndex( FlipbookPlugin& plugin )
{
	std::map< std::string, unsigned int > byName;
	for( unsigned int id = 0; id < PT_COUNT; ++id )
	{
		const char* name = plugin.GetParamName( id );
		if( name != nullptr && name[ 0 ] != '\0' )
			byName[ name ] = id;
	}
	return byName;
}

/// Point the plugin at a sheet and make it reload before the next draw.
void useSheet( FlipbookPlugin& plugin, const std::string& path, int columns, int rows )
{
	plugin.SetTextParameter( PT_SHEET_FILE, path.c_str() );
	plugin.SetFloatParameter( PT_COLUMNS, static_cast< float >( columns ) );
	plugin.SetFloatParameter( PT_ROWS, static_cast< float >( rows ) );
	plugin.Invalidate();
}

//---------------------------------------------------------------------------
// --frames
//---------------------------------------------------------------------------
int checkFrames( const std::string& sheetPath )
{
	Target target = makeTarget( 512, 512 );
	int failures  = 0;

	struct Case
	{
		PlayMode mode;
		const char* name;
		int start;
		int count;
	};

	// Start 0 / 8 cells is the plain case. Start 14 / 6 is the wrap: the run
	// runs off the end of a 20-cell sheet and comes round to the beginning,
	// which is what a sheet whose animation straddles the last row needs and
	// what a naive clamp would silently turn into a two-frame loop.
	const Case cases[] = {
		{ PlayMode::Loop, "Loop", 0, 8 },
		{ PlayMode::PingPong, "Ping-Pong", 0, 5 },
		{ PlayMode::Once, "Once", 3, 4 },
		{ PlayMode::Loop, "Loop wrapping", 14, 6 },
		{ PlayMode::Loop, "Loop, whole sheet", 0, 0 },
	};

	for( const Case& c : cases )
	{
		FlipbookPlugin plugin( false );
		useSheet( plugin, sheetPath, kTestColumns, kTestRows );
		plugin.SetFloatParameter( PT_MODE, static_cast< float >( c.mode ) );
		plugin.SetFloatParameter( PT_START, static_cast< float >( c.start ) );
		plugin.SetFloatParameter( PT_LENGTH, static_cast< float >( c.count ) );
		plugin.SetFloatParameter( PT_FIT, static_cast< float >( Fit::Stretch ) );
		plugin.SetFloatParameter( PT_RATE, 0.53f );

		const float rate   = RateFromParam( 0.53f );
		const int length   = RunLength( c.start, c.count, kTestCells );
		int mismatches     = 0;

		// Twenty-odd steps rather than a round number of frames: a sweep that
		// lands on frame boundaries agrees with an off-by-a-frame rounding
		// error at every sample it takes.
		for( int step = 0; step < 23; ++step )
		{
			const double seconds = static_cast< double >( step ) * 0.137;
			if( !render( plugin, target, seconds ) )
			{
				std::fprintf( stderr, "  %s: render failed at %.3f s\n", c.name, seconds );
				++failures;
				break;
			}

			const std::vector< unsigned char > pixels = readBytes( target );
			float r = 0, g = 0, b = 0, a = 0;
			samplePixel( pixels, target.width, target.height, 0.5f, 0.5f, r, g, b, a );

			const int seen     = cellFromColour( r, g, b );
			const double clock = seconds * static_cast< double >( rate );
			const int expected = ( c.start + FrameOffset( clock, length, c.mode ) ) % kTestCells;

			if( seen != expected )
			{
				if( mismatches < 3 )
					std::fprintf( stderr, "  %s: at %.3f s saw cell %d, expected %d\n",
					              c.name, seconds, seen, expected );
				++mismatches;
			}
		}

		if( mismatches > 0 )
		{
			std::fprintf( stderr, "%s: %d of 23 frames wrong\n", c.name, mismatches );
			++failures;
		}
		else
		{
			std::printf( "  %-20s %2d frames from cell %2d, 23 samples correct\n",
			             c.name, length, c.start );
		}
	}

	releaseTarget( target );
	std::printf( failures == 0 ? "frames: ok\n" : "frames: %d FAILED\n", failures );
	return failures;
}

//---------------------------------------------------------------------------
// --copies
//---------------------------------------------------------------------------
int checkCopies( const std::string& sheetPath )
{
	Target target = makeTarget( 960, 540 );
	int failures  = 0;

	struct Case
	{
		Arrange arrange;
		const char* name;
		int copies;
		float stagger;
	};

	// Two constraints on every case here, and both were learned by getting
	// them wrong.
	//
	// **Spread has to keep the copies on the raster.** Spread is 0..1.5, so the
	// 0.9 that looks like "most of the way out" is 1.35 and puts a ring's top
	// copy at y = -0.175. That is correct behaviour -- a big sprite wants to be
	// able to sit off the edge -- but it is not a placement this test can
	// measure, because there is no pixel there to measure.
	//
	// **Stagger is only checked where copies cannot overlap.** Scatter's whole
	// job is putting copies wherever the hash says, including on top of each
	// other, and the pixel at an overlapped centre belongs to whichever copy
	// was drawn last. So Scatter runs with every copy on the same frame, where
	// overlap cannot change the answer, and the chase is checked on Grid and
	// Ring instead.
	const Case cases[] = {
		{ Arrange::Grid, "Grid", 9, 0.0f },
		{ Arrange::Ring, "Ring", 8, 0.4f },
		{ Arrange::Scatter, "Scatter", 16, 0.0f },
		{ Arrange::Grid, "Grid, staggered", 25, 0.4f },
	};

	for( const Case& c : cases )
	{
		FlipbookPlugin plugin( false );
		useSheet( plugin, sheetPath, kTestColumns, kTestRows );
		plugin.SetFloatParameter( PT_COPIES, static_cast< float >( c.copies ) );
		plugin.SetFloatParameter( PT_ARRANGE, static_cast< float >( c.arrange ) );
		plugin.SetFloatParameter( PT_STAGGER, c.stagger );
		plugin.SetFloatParameter( PT_SPREAD, 0.5f );// 0.75 of the frame; see above
		plugin.SetFloatParameter( PT_SCALE, 0.10f );// small enough that copies do not overlap
		plugin.SetFloatParameter( PT_LENGTH, 0.0f );

		const double seconds = 0.611;
		if( !render( plugin, target, seconds ) )
		{
			std::fprintf( stderr, "  %s: render failed\n", c.name );
			++failures;
			continue;
		}

		const std::vector< unsigned char > pixels = readBytes( target );

		// The prediction comes from the same solver the plugin used, and the
		// measurement is taken at that exact point -- not at the nearest blob.
		// A nearest-blob search would find *a* sprite near where one was
		// expected and call it a pass, which is assuming the thing under test.
		std::vector< Copy > copies;
		SolveCopies( plugin.CurrentLayout( target.width, target.height ), copies );

		int wrongPlace = 0;
		int wrongCell  = 0;
		bool seenCell[ kTestCells ] = {};
		for( int i = 0; i < static_cast< int >( copies.size() ); ++i )
		{
			const Copy& copy = copies[ static_cast< size_t >( i ) ];
			float r = 0, g = 0, b = 0, a = 0;
			const bool covered = samplePixel( pixels, target.width, target.height,
			                                  copy.centreX, copy.centreY, r, g, b, a );

			if( !covered )
			{
				if( wrongPlace < 3 )
					std::fprintf( stderr, "  %s: copy %d has nothing at (%.3f, %.3f)\n",
					              c.name, i, copy.centreX, copy.centreY );
				++wrongPlace;
				continue;
			}

			const int seen     = cellFromColour( r, g, b );
			const int expected = plugin.CellForCopy( i, kTestCells ) % kTestCells;
			if( seen != expected )
			{
				if( wrongCell < 3 )
					std::fprintf( stderr, "  %s: copy %d shows cell %d, expected %d\n",
					              c.name, i, seen, expected );
				++wrongCell;
			}
			if( seen >= 0 && seen < kTestCells )
				seenCell[ seen ] = true;
		}

		int distinct = 0;
		for( bool seen : seenCell )
			distinct += seen ? 1 : 0;

		// Without this, a Stagger that had quietly stopped reaching the picture
		// would pass every check above: each copy would show frame 0, which is
		// exactly what CellForCopy would also predict, and the two would agree
		// perfectly on nothing happening.
		const bool chaseExpected = c.stagger > 0.0f;
		if( chaseExpected && distinct < 2 )
		{
			std::fprintf( stderr, "%s: staggered, but every copy is on the same frame\n", c.name );
			++failures;
		}
		else if( wrongPlace > 0 || wrongCell > 0 )
		{
			std::fprintf( stderr, "%s: %d misplaced, %d showing the wrong cell\n",
			              c.name, wrongPlace, wrongCell );
			++failures;
		}
		else
		{
			std::printf( "  %-18s %2d copies placed, right cell, %d distinct frames on screen\n",
			             c.name, c.copies, distinct );
		}
	}

	releaseTarget( target );
	std::printf( failures == 0 ? "copies: ok\n" : "copies: %d FAILED\n", failures );
	return failures;
}

//---------------------------------------------------------------------------
// --aspect
//---------------------------------------------------------------------------
/// The bounding box of everything that is not background, in pixels.
bool spriteBounds( const std::vector< unsigned char >& bottomUp, int width, int height,
                   int& x0, int& y0, int& x1, int& y1 )
{
	x0 = width;
	y0 = height;
	x1 = -1;
	y1 = -1;

	for( int y = 0; y < height; ++y )
	{
		for( int x = 0; x < width; ++x )
		{
			const unsigned char alpha = bottomUp[ ( static_cast< size_t >( y ) * width + x ) * 4 + 3 ];
			if( alpha < 8 )
				continue;
			x0 = std::min( x0, x );
			x1 = std::max( x1, x );
			y0 = std::min( y0, y );
			y1 = std::max( y1, y );
		}
	}

	return x1 >= x0 && y1 >= y0;
}

int checkAspect( const std::string& sheetPath )
{
	int failures = 0;

	struct Size
	{
		int width, height;
	};
	const Size sizes[] = { { 512, 512 }, { 1280, 720 }, { 720, 1280 }, { 1024, 400 } };

	for( const Size& size : sizes )
	{
		Target target = makeTarget( size.width, size.height );

		FlipbookPlugin plugin( false );
		useSheet( plugin, sheetPath, kTestColumns, kTestRows );
		plugin.SetFloatParameter( PT_FIT, static_cast< float >( Fit::Native ) );
		plugin.SetFloatParameter( PT_SCALE, 0.5f );// exactly 1.0
		plugin.SetFloatParameter( PT_START, 5.0f );// an opaque cell
		plugin.SetFloatParameter( PT_LENGTH, 1.0f );
		plugin.SetFloatParameter( PT_SAMPLING, static_cast< float >( Sampling::Pixel ) );

		if( !render( plugin, target, 0.0 ) )
		{
			std::fprintf( stderr, "  %dx%d: render failed\n", size.width, size.height );
			++failures;
			releaseTarget( target );
			continue;
		}

		const std::vector< unsigned char > pixels = readBytes( target );
		int x0 = 0, y0 = 0, x1 = 0, y1 = 0;
		if( !spriteBounds( pixels, size.width, size.height, x0, y0, x1, y1 ) )
		{
			std::fprintf( stderr, "  %dx%d: nothing drawn\n", size.width, size.height );
			++failures;
			releaseTarget( target );
			continue;
		}

		const float drawnW = static_cast< float >( x1 - x0 + 1 );
		const float drawnH = static_cast< float >( y1 - y0 + 1 );

		// The cell is square, so the sprite must be square in PIXELS -- which is
		// the whole of the trap. A plugin that placed and sized in the same 0..1
		// space would draw 1280 x 720 here and pass any test that only asked
		// whether something was drawn.
		const float ratio = drawnW / drawnH;

		// Scale 1.0 means the sprite's short-edge extent is the whole short
		// edge, so both sides should be the short edge. Checking the size and
		// not only the ratio matters: sizing off the LONG edge gives a
		// perfectly square sprite of entirely the wrong size.
		const float shortEdge = static_cast< float >( std::min( size.width, size.height ) );

		const bool squareOk = std::fabs( ratio - 1.0f ) < 0.02f;
		const bool sizeOk   = std::fabs( drawnH - shortEdge ) < 3.0f;

		if( !squareOk || !sizeOk )
		{
			std::fprintf( stderr, "  %dx%d: sprite %.0f x %.0f, ratio %.3f, expected %.0f square\n",
			              size.width, size.height, drawnW, drawnH, ratio, shortEdge );
			++failures;
		}
		else
		{
			std::printf( "  %4d x %4d  sprite %.0f x %.0f  (short edge %.0f)\n",
			             size.width, size.height, drawnW, drawnH, shortEdge );
		}

		releaseTarget( target );
	}

	std::printf( failures == 0 ? "aspect: ok\n" : "aspect: %d FAILED\n", failures );
	return failures;
}

//---------------------------------------------------------------------------
// --seam
//---------------------------------------------------------------------------
int checkSeam( const std::string& sheetPath )
{
	// Stretch, so one cell covers the whole raster and every one of its texels
	// is magnified across many pixels -- which is the condition under which a
	// boundary fetch reaches into the next cell, and the condition the plugin
	// is actually used in.
	Target target = makeTarget( 700, 700 );
	int failures  = 0;

	for( int filterPass = 0; filterPass < 2; ++filterPass )
	{
		const Sampling sampling = filterPass == 0 ? Sampling::Smooth : Sampling::Pixel;
		const char* name        = filterPass == 0 ? "Smooth" : "Pixel";

		int bleeding = 0;

		// Cell 6 is in the middle of the grid, so it has neighbours on all four
		// sides -- a corner cell would pass this test with a broken inset on
		// two of its edges simply by not having anything to bleed from.
		for( int cell : { 6, 7, 12 } )
		{
			FlipbookPlugin plugin( false );
			useSheet( plugin, sheetPath, kTestColumns, kTestRows );
			plugin.SetFloatParameter( PT_FIT, static_cast< float >( Fit::Stretch ) );
			plugin.SetFloatParameter( PT_START, static_cast< float >( cell ) );
			plugin.SetFloatParameter( PT_LENGTH, 1.0f );
			plugin.SetFloatParameter( PT_SAMPLING, static_cast< float >( sampling ) );

			if( !render( plugin, target, 0.0 ) )
			{
				std::fprintf( stderr, "  %s cell %d: render failed\n", name, cell );
				++failures;
				continue;
			}

			const std::vector< unsigned char > pixels = readBytes( target );

			for( int y = 0; y < target.height; ++y )
			{
				for( int x = 0; x < target.width; ++x )
				{
					const unsigned char* p = pixels.data() + ( static_cast< size_t >( y ) * target.width + x ) * 4;
					const float a          = static_cast< float >( p[ 3 ] ) / 255.0f;
					if( a <= 0.001f )
						continue;

					const float r = static_cast< float >( p[ 0 ] ) / 255.0f / a;
					const float g = static_cast< float >( p[ 1 ] ) / 255.0f / a;
					const float b = static_cast< float >( p[ 2 ] ) / 255.0f / a;

					// Tight tolerance: inside a flat cell every pixel is the
					// cell's exact colour, so anything that is not is either a
					// neighbour bleeding in or a blend of the two.
					const Rgb want  = TestColour( cell );
					const float err = std::sqrt( ( r - want.r ) * ( r - want.r )
					                             + ( g - want.g ) * ( g - want.g )
					                             + ( b - want.b ) * ( b - want.b ) );
					if( err > 0.03f )
					{
						if( bleeding < 3 )
							std::fprintf( stderr,
							              "  %s cell %d: pixel (%d, %d) is (%.2f %.2f %.2f), off by %.3f\n",
							              name, cell, x, y, r, g, b, err );
						++bleeding;
					}
				}
			}
		}

		if( bleeding > 0 )
		{
			std::fprintf( stderr, "%s: %d pixels carry a neighbouring cell\n", name, bleeding );
			++failures;
		}
		else
		{
			std::printf( "  %-7s three cells, every pixel is its own cell's colour\n", name );
		}
	}

	releaseTarget( target );
	std::printf( failures == 0 ? "seam: ok\n" : "seam: %d FAILED\n", failures );
	return failures;
}

//---------------------------------------------------------------------------
// --key
//---------------------------------------------------------------------------
int checkKey( const std::string& sheetPath )
{
	Target target = makeTarget( 400, 400 );
	int failures  = 0;

	auto alphaAtCentre = [ & ]( KeyMode mode, int cell, float keyR, float keyG, float keyB,
	                            float tolerance ) -> float {
		FlipbookPlugin plugin( false );
		useSheet( plugin, sheetPath, kTestColumns, kTestRows );
		plugin.SetFloatParameter( PT_FIT, static_cast< float >( Fit::Stretch ) );
		plugin.SetFloatParameter( PT_START, static_cast< float >( cell ) );
		plugin.SetFloatParameter( PT_LENGTH, 1.0f );
		plugin.SetFloatParameter( PT_KEY, static_cast< float >( mode ) );
		plugin.SetFloatParameter( PT_KEY_R, keyR );
		plugin.SetFloatParameter( PT_KEY_G, keyG );
		plugin.SetFloatParameter( PT_KEY_B, keyB );
		plugin.SetFloatParameter( PT_KEY_TOLERANCE, tolerance );
		plugin.SetFloatParameter( PT_KEY_SOFTNESS, 0.0f );

		if( !render( plugin, target, 0.0 ) )
			return -1.0f;

		const std::vector< unsigned char > pixels = readBytes( target );
		const unsigned char* p = pixels.data() + ( static_cast< size_t >( target.height / 2 ) * target.width
		                                           + target.width / 2 ) * 4;
		return static_cast< float >( p[ 3 ] ) / 255.0f;
	};

	struct Check
	{
		const char* what;
		float got;
		float want;
	};

	const Rgb keyed = TestColour( 6 );

	// Tolerance 0.55 of the slider squares to ~0.30, comfortably more than the
	// zero distance from the key colour to itself and comfortably less than the
	// distance to any neighbouring hue.
	const std::vector< Check > checks = {
		// Cell 0 is the half-alpha one, so Alpha honours the file and None
		// ignores it. Those two together are what prove the mode is doing
		// anything at all -- either one alone would pass on an opaque cell.
		{ "Alpha honours the file's alpha", alphaAtCentre( KeyMode::Alpha, 0, 0, 0, 0, 0.0f ), 0.5f },
		{ "None ignores the file's alpha", alphaAtCentre( KeyMode::None, 0, 0, 0, 0, 0.0f ), 1.0f },
		{ "Alpha leaves an opaque cell alone", alphaAtCentre( KeyMode::Alpha, 6, 0, 0, 0, 0.0f ), 1.0f },
		{ "Colour removes its own colour", alphaAtCentre( KeyMode::Colour, 6, keyed.r, keyed.g, keyed.b, 0.55f ), 0.0f },
		{ "Colour keeps a different cell", alphaAtCentre( KeyMode::Colour, 12, keyed.r, keyed.g, keyed.b, 0.55f ), 1.0f },
		// Luma against white: cell 6's luma is not white's, so it survives, and
		// a white key with a wide tolerance would take it. The pair is what
		// distinguishes "the luma key works" from "the luma key removes
		// everything".
		{ "Luma keeps a mid-luma cell", alphaAtCentre( KeyMode::Luma, 6, 1.0f, 1.0f, 1.0f, 0.3f ), 1.0f },
		{ "Luma removes at a wide tolerance", alphaAtCentre( KeyMode::Luma, 6, 1.0f, 1.0f, 1.0f, 1.0f ), 0.0f },
	};

	for( const Check& check : checks )
	{
		if( check.got < 0.0f )
		{
			std::fprintf( stderr, "  %s: render failed\n", check.what );
			++failures;
			continue;
		}

		if( std::fabs( check.got - check.want ) > 0.06f )
		{
			std::fprintf( stderr, "  %s: alpha %.3f, expected %.3f\n", check.what, check.got, check.want );
			++failures;
		}
		else
		{
			std::printf( "  %-38s alpha %.2f\n", check.what, check.got );
		}
	}

	releaseTarget( target );
	std::printf( failures == 0 ? "key: ok\n" : "key: %d FAILED\n", failures );
	return failures;
}

//---------------------------------------------------------------------------
// --presets
//---------------------------------------------------------------------------
//
// Every factory preset, applied through the real dropdown, with every copy's
// centre checked against the raster.
//
// This exists because three of the seven presets shipped framed wrong and
// nothing here noticed. Carousel was the worst: Spread runs 0..1.5, so its 1.0
// was a ring of radius 0.75 with the top and bottom of the ring a fifth of the
// frame outside it -- a preset that showed four of its twelve copies. `--copies`
// could not catch it because that test uses its own parameter values, and
// `sweep.py` could not because it only asks whether the Preset dropdown changes
// the picture at all. It was found by clicking the presets in the web demo,
// which is not a test.
//
// The assertion is on the copy's CENTRE, not its edges. A sprite hanging over
// the edge of the frame is a legitimate look and a common one; a sprite whose
// middle is off the raster is a framing mistake in every case.
int checkPresets( const std::string& sheetPath )
{
	Target target = makeTarget( 1920, 1080 );
	int failures  = 0;

	for( int index = 1; index <= presets::kCount; ++index )
	{
		FlipbookPlugin plugin( false );
		useSheet( plugin, sheetPath, kTestColumns, kTestRows );
		plugin.SetFloatParameter( PT_PRESET, static_cast< float >( index ) );

		// Through a real render, so the preset has actually been applied by the
		// same path a host uses rather than read out of the table.
		if( !render( plugin, target, 0.611 ) )
		{
			std::fprintf( stderr, "  %s: render failed\n", presets::kPresets[ index - 1 ].name );
			++failures;
			continue;
		}

		std::vector< Copy > copies;
		SolveCopies( plugin.CurrentLayout( target.width, target.height ), copies );

		int outside = 0;
		for( const Copy& copy : copies )
		{
			if( copy.centreX < 0.0f || copy.centreX > 1.0f
			    || copy.centreY < 0.0f || copy.centreY > 1.0f )
				++outside;
		}

		if( outside > 0 )
		{
			std::fprintf( stderr, "  %-12s %d of %d copies are centred off the raster\n",
			              presets::kPresets[ index - 1 ].name, outside,
			              static_cast< int >( copies.size() ) );
			++failures;
		}
		else
		{
			std::printf( "  %-12s %2d copies, all on the raster\n",
			             presets::kPresets[ index - 1 ].name, static_cast< int >( copies.size() ) );
		}
	}

	releaseTarget( target );
	std::printf( failures == 0 ? "presets: ok\n" : "presets: %d FAILED\n", failures );
	return failures;
}

//---------------------------------------------------------------------------
// --sequence
//---------------------------------------------------------------------------
//
// A cue sheet, so the project video is the real plugin being *operated* rather
// than a mock-up or a screen recording:
//
//     12.0        Copies=9             set at a time
//     4.0..9.0    Stagger=0.0..0.3     ramp between two times
//
// Times are seconds on the video's own clock, which is also the host clock
// handed to the plugin — so a cue at 12s is the frame you see at 12s.
//
// The playback position is deliberately NOT pinned. The plugin runs off the
// host clock exactly as it does in Resolume, which is the only way the video
// can honestly show Rate, Mode and Sync doing anything.
//
struct Cue
{
	double from = 0.0;
	double to   = 0.0;
	std::string name;
	float first  = 0.0f;
	float second = 0.0f;
	bool ramp    = false;
};

bool parseCues( const std::string& path, std::vector< Cue >& cues )
{
	std::FILE* file = std::fopen( path.c_str(), "rb" );
	if( file == nullptr )
	{
		std::fprintf( stderr, "cannot open cue sheet %s\n", path.c_str() );
		return false;
	}

	char line[ 1024 ];
	int number = 0;
	while( std::fgets( line, sizeof( line ), file ) != nullptr )
	{
		++number;
		std::string text = line;

		const size_t hash = text.find( '#' );
		if( hash != std::string::npos )
			text = text.substr( 0, hash );

		const size_t firstReal = text.find_first_not_of( " \t\r\n" );
		if( firstReal == std::string::npos )
			continue;
		text = text.substr( firstReal );

		const size_t split = text.find_first_of( " \t" );
		if( split == std::string::npos )
			continue;

		const std::string when = text.substr( 0, split );
		std::string assignment = text.substr( split );

		const size_t assignStart = assignment.find_first_not_of( " \t" );
		if( assignStart == std::string::npos )
			continue;
		assignment = assignment.substr( assignStart );
		while( !assignment.empty() && std::strchr( " \t\r\n", assignment.back() ) != nullptr )
			assignment.pop_back();

		Cue cue;
		const size_t timeRange = when.find( ".." );
		if( timeRange != std::string::npos )
		{
			cue.from = std::strtod( when.substr( 0, timeRange ).c_str(), nullptr );
			cue.to   = std::strtod( when.substr( timeRange + 2 ).c_str(), nullptr );
			cue.ramp = true;
		}
		else
		{
			cue.from = cue.to = std::strtod( when.c_str(), nullptr );
		}

		const size_t equals = assignment.find( '=' );
		if( equals == std::string::npos )
		{
			std::fprintf( stderr, "%s:%d: expected Name=value\n", path.c_str(), number );
			std::fclose( file );
			return false;
		}

		cue.name                = assignment.substr( 0, equals );
		const std::string value = assignment.substr( equals + 1 );

		const size_t valueRange = value.find( ".." );
		if( cue.ramp && valueRange != std::string::npos )
		{
			cue.first  = std::strtof( value.substr( 0, valueRange ).c_str(), nullptr );
			cue.second = std::strtof( value.substr( valueRange + 2 ).c_str(), nullptr );
		}
		else
		{
			cue.first = cue.second = std::strtof( value.c_str(), nullptr );
			cue.ramp  = false;
		}

		cues.push_back( cue );
	}

	std::fclose( file );
	return true;
}

int renderSequence( const std::string& directory, const std::string& cuePath,
                    const std::string& sheetPath, int columns, int rows,
                    int width, int height, double seconds, double fps, bool effect )
{
	std::vector< Cue > cues;
	if( !cuePath.empty() && !parseCues( cuePath, cues ) )
		return 1;

	FlipbookPlugin plugin( effect );
	useSheet( plugin, sheetPath, columns, rows );

	// Every cue is checked against the real parameter list before a single frame
	// is rendered. A typo in a name would otherwise be a cue that silently never
	// fires, and the only symptom would be a video that is subtly less
	// interesting than the sheet says it is — after forty seconds of rendering.
	const std::map< std::string, unsigned int > byName = parameterIndex( plugin );
	for( const Cue& cue : cues )
	{
		if( byName.find( cue.name ) == byName.end() )
		{
			std::fprintf( stderr, "cue names '%s', which is not a parameter\n", cue.name.c_str() );
			return 1;
		}
	}

	Target target = makeTarget( width, height );

	GLuint clip = 0;
	if( effect )
	{
		std::vector< unsigned char > image( static_cast< size_t >( width ) * height * 4 );
		for( int y = 0; y < height; ++y )
			for( int x = 0; x < width; ++x )
			{
				unsigned char* p = image.data() + ( static_cast< size_t >( y ) * width + x ) * 4;
				p[ 0 ] = static_cast< unsigned char >( 255 * x / std::max( 1, width - 1 ) );
				p[ 1 ] = 40;
				p[ 2 ] = static_cast< unsigned char >( 255 * y / std::max( 1, height - 1 ) );
				p[ 3 ] = 255;
			}
		glGenTextures( 1, &clip );
		glBindTexture( GL_TEXTURE_2D, clip );
		glTexImage2D( GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, image.data() );
		glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR );
		glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR );
		glBindTexture( GL_TEXTURE_2D, 0 );
	}

	const int frames = static_cast< int >( seconds * fps + 0.5 );
	int lastColumns  = columns;
	int lastRows     = rows;

	for( int frame = 0; frame < frames; ++frame )
	{
		const double now = static_cast< double >( frame ) / fps;

		// Applied in file order every frame rather than tracked as state, so a
		// later cue on the same parameter simply wins — which is what reading
		// the sheet top to bottom would lead you to expect.
		for( const Cue& cue : cues )
		{
			if( now < cue.from )
				continue;

			float value = cue.second;
			if( cue.ramp && now < cue.to && cue.to > cue.from )
			{
				const double t = ( now - cue.from ) / ( cue.to - cue.from );
				// Smoothstep rather than linear. A parameter that starts and
				// stops abruptly reads as a jump cut even when the value in
				// between is right.
				const double eased = t * t * ( 3.0 - 2.0 * t );
				value = static_cast< float >( cue.first + ( cue.second - cue.first ) * eased );
			}

			plugin.SetFloatParameter( byName.at( cue.name ), value );
		}

		// The grid is the one thing a cue can change that needs the sheet
		// re-cut, and SetFloatParameter alone will not do it from here because
		// the harness reloads on demand rather than per frame.
		const int nowColumns = static_cast< int >( std::lround( plugin.GetFloatParameter( PT_COLUMNS ) ) );
		const int nowRows    = static_cast< int >( std::lround( plugin.GetFloatParameter( PT_ROWS ) ) );
		if( nowColumns != lastColumns || nowRows != lastRows )
		{
			plugin.Invalidate();
			lastColumns = nowColumns;
			lastRows    = nowRows;
		}

		if( !render( plugin, target, now, clip ) )
		{
			std::fprintf( stderr, "render failed at frame %d\n", frame );
			releaseTarget( target );
			return 1;
		}

		char path[ 1024 ];
		std::snprintf( path, sizeof( path ), "%s/frame%05d.png", directory.c_str(), frame );

		const std::vector< unsigned char > image = flipRows( readBytes( target ), width, height );
		if( !writePng( path, width, height, image ) )
		{
			std::fprintf( stderr, "could not write %s\n", path );
			releaseTarget( target );
			return 1;
		}

		if( ( frame + 1 ) % 60 == 0 )
			std::printf( "  %d / %d frames\n", frame + 1, frames );
	}

	if( clip != 0 )
		glDeleteTextures( 1, &clip );
	releaseTarget( target );

	std::printf( "wrote %d frames to %s (%dx%d at %g fps)\n", frames, directory.c_str(),
	             width, height, fps );
	return 0;
}

//---------------------------------------------------------------------------
// --list
//---------------------------------------------------------------------------
int listParameters()
{
	FlipbookPlugin plugin( false );

	const char* types[ 256 ] = {};
	types[ FF_TYPE_BOOLEAN ] = "boolean";
	types[ FF_TYPE_EVENT ]   = "event";
	types[ FF_TYPE_RED ]     = "red";
	types[ FF_TYPE_GREEN ]   = "green";
	types[ FF_TYPE_BLUE ]    = "blue";
	types[ FF_TYPE_XPOS ]    = "x";
	types[ FF_TYPE_YPOS ]    = "y";
	types[ FF_TYPE_STANDARD ] = "standard";
	types[ FF_TYPE_OPTION ]  = "option";
	types[ FF_TYPE_INTEGER ] = "integer";
	types[ FF_TYPE_FILE ]    = "file";

	for( unsigned int id = 0; id < PT_COUNT; ++id )
	{
		const char* name = plugin.GetParamName( id );
		const unsigned int type = plugin.GetParamType( id );
		const char* typeName    = ( type < 256 && types[ type ] != nullptr ) ? types[ type ] : "text";

		std::printf( "%2u  %-22s %-9s %8.4f", id, name != nullptr ? name : "?", typeName,
		             plugin.GetFloatParameter( id ) );

		const RangeStruct range = plugin.GetParamRange( id );
		if( range.min != range.max )
			std::printf( "   [%g .. %g]", range.min, range.max );
		std::printf( "\n" );
	}

	return 0;
}

//---------------------------------------------------------------------------
// --sheet
//---------------------------------------------------------------------------
int describeSheet( const std::string& path, int columns, int rows )
{
	const Sheet sheet = LoadSheet( path, columns, rows );
	std::printf( "%s\n", sheet.note.c_str() );
	if( !sheet.Valid() )
		return 1;

	std::printf( "  %d x %d px, grid %d x %d, %d frames%s\n", sheet.width, sheet.height,
	             sheet.columns, sheet.rows, sheet.frames,
	             sheet.gridFromFile ? ", grid from the file" : "" );
	return 0;
}

void usage()
{
	std::printf(
		"fbtest -- the flipbook offline harness\n"
		"\n"
		"  --make-sheet PATH   write the synthetic test sheet (--detail for the sweep's)\n"
		"  --sheet PATH        what a sheet decodes to (with --columns/--rows)\n"
		"  --out PATH          render a frame\n"
		"  --list              parameters, types, defaults and ranges\n"
		"  --frames            the cell on screen against Playback.cpp\n"
		"  --copies            where each copy landed, and which cell it showed\n"
		"  --aspect            a square cell stays square off 1:1\n"
		"  --seam              no cell bleeds into its neighbour\n"
		"  --key               the four key modes on known colours\n"
		"  --presets           every factory preset frames itself on the raster\n"
		"  --all               every check above\n"
		"\n"
		"  --file PATH         the sheet to use (default: a temporary synthetic one)\n"
		"  --columns N --rows N\n"
		"  --time SECONDS      the clock for --out\n"
		"  --size WxH          the raster for --out\n"
		"  --effect            render the effect variant over a test clip\n"
		"  --set \"Name=value\"  set any parameter by name\n"
		"\n"
		"  --sequence DIR      render a PNG per frame for the project video\n"
		"  --script FILE       the cue sheet driving it (tools/video.cues)\n"
		"  --seconds N --fps N\n" );
}
} // namespace

int main( int argc, char** argv )
{
	std::string outPath;
	std::string sheetPath;
	std::string makeSheetPath;
	std::string describePath;
	std::vector< std::pair< std::string, float > > sets;

	int columns   = kTestColumns;
	int rows      = kTestRows;
	double time   = 0.0;
	int width     = 1280;
	int height    = 720;
	bool effect   = false;
	bool detailed = false;
	std::string sequenceDir;
	std::string scriptPath;
	double seconds = 40.0;
	double fps     = 30.0;
	bool doList   = false;
	bool doFrames = false;
	bool doCopies = false;
	bool doAspect = false;
	bool doSeam   = false;
	bool doKey     = false;
	bool doPresets = false;

	for( int i = 1; i < argc; ++i )
	{
		const std::string arg = argv[ i ];
		auto next             = [ & ]() -> std::string { return ( i + 1 < argc ) ? argv[ ++i ] : std::string(); };

		if( arg == "--out" ) outPath = next();
		else if( arg == "--make-sheet" ) makeSheetPath = next();
		else if( arg == "--detail" ) detailed = true;
		else if( arg == "--sequence" ) sequenceDir = next();
		else if( arg == "--script" ) scriptPath = next();
		else if( arg == "--seconds" ) seconds = std::atof( next().c_str() );
		else if( arg == "--fps" ) fps = std::atof( next().c_str() );
		else if( arg == "--sheet" ) describePath = next();
		else if( arg == "--file" ) sheetPath = next();
		else if( arg == "--columns" ) columns = std::atoi( next().c_str() );
		else if( arg == "--rows" ) rows = std::atoi( next().c_str() );
		else if( arg == "--time" ) time = std::atof( next().c_str() );
		else if( arg == "--effect" ) effect = true;
		else if( arg == "--list" ) doList = true;
		else if( arg == "--frames" ) doFrames = true;
		else if( arg == "--copies" ) doCopies = true;
		else if( arg == "--aspect" ) doAspect = true;
		else if( arg == "--seam" ) doSeam = true;
		else if( arg == "--key" ) doKey = true;
		else if( arg == "--presets" ) doPresets = true;
		else if( arg == "--all" ) doFrames = doCopies = doAspect = doSeam = doKey = doPresets = true;
		else if( arg == "--size" )
		{
			const std::string value = next();
			const size_t cross      = value.find( 'x' );
			if( cross != std::string::npos )
			{
				width  = std::atoi( value.substr( 0, cross ).c_str() );
				height = std::atoi( value.substr( cross + 1 ).c_str() );
			}
		}
		else if( arg == "--set" )
		{
			const std::string value = next();
			const size_t equals     = value.find( '=' );
			if( equals != std::string::npos )
				sets.emplace_back( value.substr( 0, equals ),
				                   static_cast< float >( std::atof( value.substr( equals + 1 ).c_str() ) ) );
		}
		else
		{
			usage();
			return arg == "--help" || arg == "-h" ? 0 : 1;
		}
	}

	// --make-sheet and --sheet need no GL at all.
	if( !makeSheetPath.empty() )
		return makeSheet( makeSheetPath, detailed );
	if( !describePath.empty() )
		return describeSheet( describePath, columns, rows );

	const bool anything = doList || doFrames || doCopies || doAspect || doSeam || doKey || doPresets
	                      || !outPath.empty() || !sequenceDir.empty();
	if( !anything )
	{
		usage();
		return 1;
	}

	CGLContextObj context = createContext();
	if( context == nullptr )
	{
		std::fprintf( stderr, "could not create a GL 4.1 core context\n" );
		return 1;
	}

	int failures = 0;

	// The checks all need the synthetic sheet, and writing it to a temporary
	// file rather than keeping it in memory is deliberate: it means they go
	// through LoadSheet and stb_image exactly as a real sheet does, so a decode
	// bug fails a test rather than being bypassed by the harness.
	std::string testSheet = sheetPath;
	if( testSheet.empty() && ( doFrames || doCopies || doAspect || doSeam || doKey || doPresets ) )
	{
		testSheet = "/tmp/fbtest-sheet.png";
		if( makeSheet( testSheet, false ) != 0 )
		{
			CGLSetCurrentContext( nullptr );
			CGLDestroyContext( context );
			return 1;
		}
		columns = kTestColumns;
		rows    = kTestRows;
	}

	if( !sequenceDir.empty() )
	{
		const int result = renderSequence( sequenceDir, scriptPath, sheetPath, columns, rows,
		                                   width, height, seconds, fps, effect );
		CGLSetCurrentContext( nullptr );
		CGLDestroyContext( context );
		return result;
	}

	if( doList )
		failures += listParameters();
	if( doFrames )
		failures += checkFrames( testSheet );
	if( doCopies )
		failures += checkCopies( testSheet );
	if( doAspect )
		failures += checkAspect( testSheet );
	if( doSeam )
		failures += checkSeam( testSheet );
	if( doKey )
		failures += checkKey( testSheet );
	if( doPresets )
		failures += checkPresets( testSheet );

	if( !outPath.empty() )
	{
		Target target = makeTarget( width, height );

		FlipbookPlugin plugin( effect );
		if( !sheetPath.empty() )
			useSheet( plugin, sheetPath, columns, rows );
		else
			useSheet( plugin, testSheet.empty() ? std::string() : testSheet, columns, rows );

		const std::map< std::string, unsigned int > byName = parameterIndex( plugin );
		for( const auto& set : sets )
		{
			const auto found = byName.find( set.first );
			if( found == byName.end() )
			{
				std::fprintf( stderr, "no parameter called '%s'\n", set.first.c_str() );
				failures += 1;
				continue;
			}
			plugin.SetFloatParameter( found->second, set.second );
			if( found->second == PT_COLUMNS || found->second == PT_ROWS )
				plugin.Invalidate();
		}

		// A test clip for the effect variant: a coloured gradient, so what the
		// plugin laid over it and what it left alone are both visible.
		GLuint clip = 0;
		if( effect )
		{
			std::vector< unsigned char > image( static_cast< size_t >( width ) * height * 4 );
			for( int y = 0; y < height; ++y )
			{
				for( int x = 0; x < width; ++x )
				{
					unsigned char* p = image.data() + ( static_cast< size_t >( y ) * width + x ) * 4;
					p[ 0 ]           = static_cast< unsigned char >( 255 * x / std::max( 1, width - 1 ) );
					p[ 1 ]           = 40;
					p[ 2 ]           = static_cast< unsigned char >( 255 * y / std::max( 1, height - 1 ) );
					p[ 3 ]           = 255;
				}
			}
			glGenTextures( 1, &clip );
			glBindTexture( GL_TEXTURE_2D, clip );
			glTexImage2D( GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, image.data() );
			glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR );
			glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR );
			glBindTexture( GL_TEXTURE_2D, 0 );
		}

		if( !render( plugin, target, time, clip ) )
		{
			std::fprintf( stderr, "render failed\n" );
			failures += 1;
		}
		else
		{
			const std::vector< unsigned char > pixels = readBytes( target );
			const std::vector< unsigned char > image  = flipRows( pixels, width, height );
			if( !writePng( outPath, width, height, image ) )
			{
				std::fprintf( stderr, "could not write %s\n", outPath.c_str() );
				failures += 1;
			}
			else
			{
				std::printf( "wrote %s (%d x %d at %.3f s)\n  %s\n", outPath.c_str(), width, height, time,
				             plugin.FileSheet().note.c_str() );
			}
		}

		if( clip != 0 )
			glDeleteTextures( 1, &clip );
		releaseTarget( target );
	}

	CGLSetCurrentContext( nullptr );
	CGLDestroyContext( context );
	return failures == 0 ? 0 : 1;
}
