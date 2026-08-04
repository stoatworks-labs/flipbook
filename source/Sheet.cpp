#include "Sheet.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

// stb_image is compiled here and only here. STBI_NO_HDR and STBI_NO_PIC drop
// the float paths this 8-bit pipeline could not use anyway; STBI_NO_FAILURE_
// STRINGS is deliberately *not* set, because stbi_failure_reason() is the only
// thing that distinguishes "not an image" from "truncated" in the log.
#define STB_IMAGE_IMPLEMENTATION
#define STBI_NO_HDR
#define STBI_NO_PIC
#include "stb_image.h"

namespace flipbook
{
const char* const kSheetExtensions[] = { "png", "jpg", "jpeg", "gif", "bmp", "tga", "psd" };
const int kSheetExtensionCount       = static_cast< int >( sizeof( kSheetExtensions ) / sizeof( kSheetExtensions[ 0 ] ) );

namespace
{
/// Read a whole file. Done by hand rather than through `stbi_load` so that the
/// GIF path and the still path start from the same bytes -- `stbi_load_gif_from_
/// memory` has no file-taking twin, and loading the file twice to find out which
/// it is would read a 40 MB page off disk for nothing.
bool ReadFile( const std::string& path, std::vector< uint8_t >& out )
{
	std::FILE* file = std::fopen( path.c_str(), "rb" );
	if( file == nullptr )
		return false;

	std::fseek( file, 0, SEEK_END );
	const long size = std::ftell( file );
	std::fseek( file, 0, SEEK_SET );

	if( size <= 0 )
	{
		std::fclose( file );
		return false;
	}

	out.resize( static_cast< size_t >( size ) );
	const size_t read = std::fread( out.data(), 1, out.size(), file );
	std::fclose( file );

	out.resize( read );
	return read > 0;
}

bool LooksLikeGif( const std::vector< uint8_t >& bytes )
{
	return bytes.size() >= 6 && std::memcmp( bytes.data(), "GIF8", 4 ) == 0;
}

std::string Basename( const std::string& path )
{
	const size_t cut = path.find_last_of( "/\\" );
	return cut == std::string::npos ? path : path.substr( cut + 1 );
}

/// Lay `count` frames of `w` x `h` into the squarest grid that fits inside
/// kMaxSheetPx on both axes, dropping trailing frames if even the squarest one
/// will not.
///
/// Squarest rather than a single strip because a strip is the obvious layout
/// and the wrong one: 80 frames of a 480 px GIF is 38400 px across, which no
/// driver will take, while the same frames in a 9 x 9 grid are 4320 square.
void ChooseGifGrid( int w, int h, int count, int& columns, int& rows, int& kept )
{
	columns = std::max( 1, static_cast< int >( std::ceil( std::sqrt( static_cast< double >( count ) ) ) ) );
	rows    = std::max( 1, ( count + columns - 1 ) / columns );
	kept    = count;

	if( w <= 0 || h <= 0 )
		return;

	const int maxColumns = std::max( 1, kMaxSheetPx / w );
	const int maxRows    = std::max( 1, kMaxSheetPx / h );

	if( columns <= maxColumns && rows <= maxRows )
		return;

	columns = std::min( columns, maxColumns );
	rows    = std::min( rows, maxRows );
	kept    = std::min( count, columns * rows );
	rows    = std::max( 1, ( kept + columns - 1 ) / columns );
}
} // namespace

Sheet LoadSheet( const std::string& path, int columns, int rows )
{
	Sheet sheet;

	if( path.empty() )
	{
		sheet.note = "no sheet chosen";
		return sheet;
	}

	std::vector< uint8_t > bytes;
	if( !ReadFile( path, bytes ) )
	{
		// The ordinary way this happens is a composition saved on another
		// machine, so the message names the path rather than just failing.
		sheet.note = "could not read '" + path + "'";
		return sheet;
	}

	const std::string name = Basename( path );

	//-----------------------------------------------------------------------
	// Animated GIF: the file already knows its own frame count, so it brings
	// its own grid and the operator's Columns and Rows are ignored.
	//-----------------------------------------------------------------------
	if( LooksLikeGif( bytes ) )
	{
		int w = 0, h = 0, frameCount = 0, channels = 0;
		int* delays = nullptr;

		stbi_uc* frames = stbi_load_gif_from_memory( bytes.data(), static_cast< int >( bytes.size() ),
		                                             &delays, &w, &h, &frameCount, &channels, 4 );

		// A single-frame GIF comes back through here too, and a grid of one is
		// exactly right for it -- but so is treating it as a still with a
		// declared grid, which is what somebody exporting a sprite sheet AS a
		// GIF meant. One frame is the case where the file's own answer is
		// useless, so it falls through to the still path.
		if( frames != nullptr && frameCount > 1 && w > 0 && h > 0 )
		{
			int kept = 0;
			ChooseGifGrid( w, h, frameCount, sheet.columns, sheet.rows, kept );

			sheet.width        = sheet.columns * w;
			sheet.height       = sheet.rows * h;
			sheet.frames       = kept;
			sheet.gridFromFile = true;
			sheet.rgba.assign( static_cast< size_t >( sheet.width ) * sheet.height * 4, 0 );

			for( int i = 0; i < kept; ++i )
			{
				const int cx             = ( i % sheet.columns ) * w;
				const int cy             = ( i / sheet.columns ) * h;
				const stbi_uc* frameData = frames + static_cast< size_t >( i ) * w * h * 4;

				for( int y = 0; y < h; ++y )
				{
					uint8_t* dst = sheet.rgba.data()
					               + ( static_cast< size_t >( cy + y ) * sheet.width + cx ) * 4;
					std::memcpy( dst, frameData + static_cast< size_t >( y ) * w * 4,
					             static_cast< size_t >( w ) * 4 );
				}
			}

			sheet.note = name + ": animated GIF, " + std::to_string( kept ) + " frames laid out "
			             + std::to_string( sheet.columns ) + "x" + std::to_string( sheet.rows )
			             + " (Columns and Rows ignored)";
			if( kept < frameCount )
				sheet.note += ", " + std::to_string( frameCount - kept ) + " dropped to stay under "
				              + std::to_string( kMaxSheetPx ) + " px";

			stbi_image_free( frames );
			STBI_FREE( delays );
			return sheet;
		}

		if( frames != nullptr )
			stbi_image_free( frames );
		STBI_FREE( delays );
		// and fall through: a one-frame GIF is a still.
	}

	//-----------------------------------------------------------------------
	// A still, with the grid the operator declared.
	//-----------------------------------------------------------------------
	int w = 0, h = 0, channels = 0;
	stbi_uc* pixels = stbi_load_from_memory( bytes.data(), static_cast< int >( bytes.size() ),
	                                         &w, &h, &channels, 4 );
	if( pixels == nullptr )
	{
		const char* reason = stbi_failure_reason();
		sheet.note         = name + ": will not decode (" + ( reason != nullptr ? reason : "unknown" ) + ")";
		return sheet;
	}

	if( w > kMaxSheetPx || h > kMaxSheetPx )
	{
		// Refused rather than downscaled. A sheet is a grid of cells, and
		// resampling one is how a hard sprite edge becomes a soft one and a
		// pixel-art sheet stops being pixel art. If it is too big, it is the
		// wrong file.
		sheet.note = name + ": " + std::to_string( w ) + "x" + std::to_string( h )
		             + " is larger than the " + std::to_string( kMaxSheetPx ) + " px limit";
		stbi_image_free( pixels );
		return sheet;
	}

	sheet.width   = w;
	sheet.height  = h;
	sheet.columns = std::max( 1, columns );
	sheet.rows    = std::max( 1, rows );
	sheet.frames  = sheet.columns * sheet.rows;
	sheet.rgba.assign( pixels, pixels + static_cast< size_t >( w ) * h * 4 );
	stbi_image_free( pixels );

	sheet.note = name + ": " + std::to_string( w ) + "x" + std::to_string( h ) + ", grid "
	             + std::to_string( sheet.columns ) + "x" + std::to_string( sheet.rows ) + " = "
	             + std::to_string( sheet.frames ) + " cells";

	// Worth saying out loud, because an inexact division is the single most
	// common reason a sheet plays with a sliver of the next sprite down one
	// edge, and nothing else in the picture explains it.
	if( w % sheet.columns != 0 || h % sheet.rows != 0 )
		sheet.note += " (does not divide exactly: cells are "
		              + std::to_string( static_cast< double >( w ) / sheet.columns ).substr( 0, 6 ) + "x"
		              + std::to_string( static_cast< double >( h ) / sheet.rows ).substr( 0, 6 ) + " px)";

	return sheet;
}

void CellRect( const Sheet& sheet, int index, float& u0, float& v0, float& u1, float& v1 )
{
	const int cells = std::max( 1, sheet.columns * sheet.rows );

	// Wrap, not clamp. A Start Frame past the end should come round to the
	// beginning; clamping would give a run of identical last cells, which looks
	// like the animation has stopped rather than like a number being out of
	// range.
	int cell = index % cells;
	if( cell < 0 )
		cell += cells;

	const float cw = 1.0f / static_cast< float >( sheet.columns );
	const float ch = 1.0f / static_cast< float >( sheet.rows );

	u0 = static_cast< float >( cell % sheet.columns ) * cw;
	v0 = static_cast< float >( cell / sheet.columns ) * ch;
	u1 = u0 + cw;
	v1 = v0 + ch;
}

} // namespace flipbook
