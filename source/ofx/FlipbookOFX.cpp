/// The OpenFX builds of Flipbook, for DaVinci Resolve, Nuke, Natron, Vegas and
/// other OFX hosts. Two plugins from this one file, as the FFGL side ships two
/// bundles: "Flipbook" is a generator, "Flipbook Over" plays the sheet over the
/// incoming clip.
///
/// ## What is shared and what is mirrored
///
/// Almost everything is shared. `Sheet.cpp` decodes the page, `Playback.cpp`
/// decides the frame, `Placement.cpp` places the copies and `Controls.cpp` maps
/// the sliders — all four are linked straight from source and are the same
/// objects `fbtest` measures. `Presets.h` is the same table.
///
/// What this file mirrors from `Shaders.cpp` is only the per-pixel half: the
/// cell fetch with its half-texel inset, the key, and the premultiplied
/// composite. The GPU evaluated those per fragment, so there is no way to share
/// them. **When editing the fragment shader's inset, key or composite, edit
/// this too.**
///
/// ## The differences that are not bugs
///
/// **Sync offers Free and Manual only.** OFX carries no tempo, so Beat and Bar
/// have no clock to lock to. Manual is the mode for keyframing Phase against
/// the edit, and it is the one mode that behaves identically in both builds.
///
/// **OFX hands render time in frames.** The clip's frame rate turns it into the
/// seconds `FrameClock` wants. A host that reports no frame rate gets 25, which
/// is wrong somewhere but is never zero.
///
/// **There is no "Sheet From: Input Clip".** It exists in the FFGL effect
/// because Resolume's own media management is the reason to want it. An OFX
/// host has a file browser and a media pool already, so the file parameter is
/// the whole story here, and offering a mode that behaved differently would be
/// a second thing to explain rather than a feature.

#include <algorithm>
#include <cmath>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "ofxsImageEffect.h"
#include "ofxsProcessing.h"

// After the OFX Support headers, which is where the OFX types come from.
#include "StoatworksAboutOFX.h"

#include "../Controls.h"
#include "../Placement.h"
#include "../Playback.h"
#include "../Presets.h"
#include "../Sheet.h"

namespace
{
constexpr const char* kSourceIdentifier = "com.stoatworks.flipbook";
constexpr const char* kOverIdentifier   = "com.stoatworks.flipbookover";
constexpr const char* kPluginGrouping   = "Stoatworks";
constexpr const char* kPluginDescription =
	"A page of stills, cut into a grid and played as an animation.\n\n"
	"Point it at a sprite sheet, tell it the grid, and it plays the cells in "
	"order — looping, ping-ponging or once. Copies can be arranged and "
	"staggered so the animation travels through them. The frame is a pure "
	"function of the clock, so the loop point cannot drift.\n\n"
	"https://stoatworks-labs.com";

constexpr const char* kParamSheetFile  = "sheetFile";
constexpr const char* kParamColumns    = "columns";
constexpr const char* kParamRows       = "rows";
constexpr const char* kParamStart      = "startFrame";
constexpr const char* kParamLength     = "frameCount";
constexpr const char* kParamSampling   = "sampling";
constexpr const char* kParamSync       = "sync";
constexpr const char* kParamRate       = "rate";
constexpr const char* kParamMode       = "mode";
constexpr const char* kParamPhase      = "phase";
constexpr const char* kParamFit        = "fit";
constexpr const char* kParamCentre     = "centre";
constexpr const char* kParamScale      = "scale";
constexpr const char* kParamRotation   = "rotation";
constexpr const char* kParamFlipH      = "flipH";
constexpr const char* kParamFlipV      = "flipV";
constexpr const char* kParamCopies     = "copies";
constexpr const char* kParamArrange    = "arrange";
constexpr const char* kParamSpread     = "spread";
constexpr const char* kParamStagger    = "stagger";
constexpr const char* kParamSeed       = "seed";
constexpr const char* kParamKey        = "key";
constexpr const char* kParamKeyColour  = "keyColour";
constexpr const char* kParamTolerance  = "tolerance";
constexpr const char* kParamSoftness   = "softness";
constexpr const char* kParamTint       = "tint";
constexpr const char* kParamOpacity    = "opacity";
constexpr const char* kParamBackColour = "backColour";
constexpr const char* kParamBackOpacity = "backOpacity";
constexpr const char* kParamMix        = "mix";
constexpr const char* kParamPreset     = "preset";

/// OFX has no tempo, so the FFGL build's Beat and Bar have no clock. The two
/// modes here map onto SyncMode::Free and SyncMode::Manual.
const char* const kSyncNames[] = { "Free", "Manual" };
constexpr int kSyncCount       = 2;

//---------------------------------------------------------------------------
// Everything the per-pixel loop needs, worked out once per render.
//---------------------------------------------------------------------------
struct Setup
{
	const flipbook::Sheet* sheet = nullptr;

	std::vector< flipbook::Copy > copies;
	std::vector< float > cellU0, cellV0, cellU1, cellV1;

	float rotation = 0.0f;
	bool pixelFilter = false;

	/// Half a texel of the sheet, on each axis. The mirror of the shader's
	/// `Inset` uniform, and load-bearing for the same reason: a bilinear fetch
	/// at a cell boundary otherwise blends in the first texel of the
	/// neighbouring sprite.
	float insetU = 0.0f, insetV = 0.0f;

	int keyMode = 0;
	float keyR = 0.0f, keyG = 0.0f, keyB = 0.0f;
	float keyTolerance = 0.0f, keySoftness = 0.0f;

	float tintR = 1.0f, tintG = 1.0f, tintB = 1.0f;
	float opacity = 1.0f;

	float backR = 0.0f, backG = 0.0f, backB = 0.0f, backA = 0.0f;

	bool over = false;
};

inline float Saturate( float v )
{
	return v < 0.0f ? 0.0f : ( v > 1.0f ? 1.0f : v );
}

//---------------------------------------------------------------------------
// The cell fetch. Mirrors the fragment shader's, including the clamp.
//---------------------------------------------------------------------------
void FetchTexel( const flipbook::Sheet& sheet, float u, float v, bool pixelFilter,
                 float& r, float& g, float& b, float& a )
{
	const float fx = u * static_cast< float >( sheet.width ) - 0.5f;
	const float fy = v * static_cast< float >( sheet.height ) - 0.5f;

	auto texel = [ & ]( int x, int y, float& tr, float& tg, float& tb, float& ta ) {
		x = std::clamp( x, 0, sheet.width - 1 );
		y = std::clamp( y, 0, sheet.height - 1 );
		const uint8_t* p = sheet.rgba.data() + ( static_cast< size_t >( y ) * sheet.width + x ) * 4;
		tr = p[ 0 ] / 255.0f;
		tg = p[ 1 ] / 255.0f;
		tb = p[ 2 ] / 255.0f;
		ta = p[ 3 ] / 255.0f;
	};

	if( pixelFilter )
	{
		texel( static_cast< int >( std::floor( fx + 0.5f ) ), static_cast< int >( std::floor( fy + 0.5f ) ),
		       r, g, b, a );
		return;
	}

	const int x0 = static_cast< int >( std::floor( fx ) );
	const int y0 = static_cast< int >( std::floor( fy ) );
	const float tx = fx - static_cast< float >( x0 );
	const float ty = fy - static_cast< float >( y0 );

	float r00, g00, b00, a00, r10, g10, b10, a10, r01, g01, b01, a01, r11, g11, b11, a11;
	texel( x0, y0, r00, g00, b00, a00 );
	texel( x0 + 1, y0, r10, g10, b10, a10 );
	texel( x0, y0 + 1, r01, g01, b01, a01 );
	texel( x0 + 1, y0 + 1, r11, g11, b11, a11 );

	const float w00 = ( 1.0f - tx ) * ( 1.0f - ty );
	const float w10 = tx * ( 1.0f - ty );
	const float w01 = ( 1.0f - tx ) * ty;
	const float w11 = tx * ty;

	r = r00 * w00 + r10 * w10 + r01 * w01 + r11 * w11;
	g = g00 * w00 + g10 * w10 + g01 * w01 + g11 * w11;
	b = b00 * w00 + b10 * w10 + b01 * w01 + b11 * w11;
	a = a00 * w00 + a10 * w10 + a01 * w01 + a11 * w11;
}

/// The key. Mirrors the fragment shader's, weights and all.
float KeyedAlpha( const Setup& s, float r, float g, float b, float a )
{
	switch( s.keyMode )
	{
	case 1:
	{
		const float luma    = 0.2126f * r + 0.7152f * g + 0.0722f * b;
		const float keyLuma = 0.2126f * s.keyR + 0.7152f * s.keyG + 0.0722f * s.keyB;
		const float d       = std::fabs( luma - keyLuma );
		const float lo      = s.keyTolerance;
		const float hi      = s.keyTolerance + s.keySoftness + 0.0001f;
		const float t       = Saturate( ( d - lo ) / ( hi - lo ) );
		return a * t * t * ( 3.0f - 2.0f * t );
	}
	case 2:
	{
		const float dr = r - s.keyR, dg = g - s.keyG, db = b - s.keyB;
		const float d  = std::sqrt( dr * dr + dg * dg + db * db );
		const float lo = s.keyTolerance;
		const float hi = s.keyTolerance + s.keySoftness + 0.0001f;
		const float t  = Saturate( ( d - lo ) / ( hi - lo ) );
		return a * t * t * ( 3.0f - 2.0f * t );
	}
	case 3:
		return 1.0f;
	default:
		return a;
	}
}

class FlipbookProcessorBase : public OFX::ImageProcessor
{
public:
	explicit FlipbookProcessorBase( OFX::ImageEffect& effect ) :
		OFX::ImageProcessor( effect )
	{
	}

	void setSetup( OFX::Image* src, const Setup* v )
	{
		srcImg = src;
		setup  = v;
	}

protected:
	OFX::Image* srcImg = nullptr;
	const Setup* setup = nullptr;
};

template< class PIX, int nComponents, int maxValue >
class FlipbookProcessor : public FlipbookProcessorBase
{
public:
	explicit FlipbookProcessor( OFX::ImageEffect& effect ) :
		FlipbookProcessorBase( effect )
	{
	}

	void multiThreadProcessImages( OfxRectI window ) override
	{
		const OfxRectI bounds = _dstImg->getBounds();
		const int outW        = bounds.x2 - bounds.x1;
		const int outH        = bounds.y2 - bounds.y1;
		const Setup& s        = *setup;

		const float ca = std::cos( -s.rotation );
		const float sa = std::sin( -s.rotation );

		for( int y = window.y1; y < window.y2; ++y )
		{
			if( _effect.abort() )
				break;

			PIX* dstPix = static_cast< PIX* >( _dstImg->getPixelAddress( window.x1, y ) );

			// Frame space runs y-down; OFX rows run bottom-up.
			const float frameY = 1.0f - ( static_cast< float >( y - bounds.y1 ) + 0.5f ) / static_cast< float >( outH );

			for( int x = window.x1; x < window.x2; ++x, dstPix += nComponents )
			{
				const float frameX = ( static_cast< float >( x - bounds.x1 ) + 0.5f ) / static_cast< float >( outW );

				//--- the background ----------------------------------------
				float accR = 0.0f, accG = 0.0f, accB = 0.0f, accA = 0.0f;
				if( s.over )
					readClip( x, y, accR, accG, accB, accA );

				// Premultiplied veil, exactly as the background pass does it.
				accR = s.backR * s.backA + accR * ( 1.0f - s.backA );
				accG = s.backG * s.backA + accG * ( 1.0f - s.backA );
				accB = s.backB * s.backA + accB * ( 1.0f - s.backA );
				accA = s.backA + accA * ( 1.0f - s.backA );

				//--- the copies, in order ----------------------------------
				//
				// O(copies) per pixel, where the GL build rasterises a quad per
				// copy. That is the right trade here and not laziness: an OFX
				// host is rendering offline, the rejection below is four
				// comparisons, and a scanline rasteriser would have to be
				// reconciled with the multi-threaded window split for no
				// visible gain.
				for( size_t i = 0; i < s.copies.size(); ++i )
				{
					const flipbook::Copy& copy = s.copies[ i ];

					// Into the copy's own space: translate, then rotate by the
					// negative angle, then divide by the half extents. The sign
					// of those extents carries the flips, which is why nothing
					// here mirrors a texture coordinate.
					const float dx = frameX - copy.centreX;
					const float dy = frameY - copy.centreY;
					const float rx = dx * ca - dy * sa;
					const float ry = dx * sa + dy * ca;

					const float u = rx / copy.halfWidth;
					const float v = ry / copy.halfHeight;
					if( u < -1.0f || u > 1.0f || v < -1.0f || v > 1.0f )
						continue;

					const float cellX = u * 0.5f + 0.5f;
					const float cellY = v * 0.5f + 0.5f;

					float su = s.cellU0[ i ] + ( s.cellU1[ i ] - s.cellU0[ i ] ) * cellX;
					float sv = s.cellV0[ i ] + ( s.cellV1[ i ] - s.cellV0[ i ] ) * cellY;
					su = std::clamp( su, s.cellU0[ i ] + s.insetU, s.cellU1[ i ] - s.insetU );
					sv = std::clamp( sv, s.cellV0[ i ] + s.insetV, s.cellV1[ i ] - s.insetV );

					float tr, tg, tb, ta;
					FetchTexel( *s.sheet, su, sv, s.pixelFilter, tr, tg, tb, ta );

					const float alpha = KeyedAlpha( s, tr, tg, tb, ta ) * s.opacity;
					if( alpha <= 0.0f )
						continue;

					// Premultiplied over, the same blend function the GL build
					// asks the hardware for.
					accR = tr * s.tintR * alpha + accR * ( 1.0f - alpha );
					accG = tg * s.tintG * alpha + accG * ( 1.0f - alpha );
					accB = tb * s.tintB * alpha + accB * ( 1.0f - alpha );
					accA = alpha + accA * ( 1.0f - alpha );
				}

				dstPix[ 0 ] = toPix( accR );
				dstPix[ 1 ] = toPix( accG );
				dstPix[ 2 ] = toPix( accB );
				if( nComponents == 4 )
					dstPix[ 3 ] = toPix( accA );
			}
		}
	}

private:
	static PIX toPix( float value )
	{
		if( maxValue == 1 )
			return static_cast< PIX >( value );
		const float scaled = value * static_cast< float >( maxValue );
		return static_cast< PIX >( std::clamp( scaled, 0.0f, static_cast< float >( maxValue ) ) + 0.5f );
	}

	void readClip( int x, int y, float& r, float& g, float& b, float& a ) const
	{
		r = g = b = a = 0.0f;
		if( srcImg == nullptr )
			return;

		const PIX* pix = static_cast< const PIX* >( srcImg->getPixelAddress( x, y ) );
		if( pix == nullptr )
			return;

		const float inv = 1.0f / static_cast< float >( maxValue );
		r               = static_cast< float >( pix[ 0 ] ) * inv;
		g               = static_cast< float >( pix[ 1 ] ) * inv;
		b               = static_cast< float >( pix[ 2 ] ) * inv;
		a               = nComponents == 4 ? static_cast< float >( pix[ 3 ] ) * inv : 1.0f;
	}
};

//---------------------------------------------------------------------------
// The plugin instance.
//---------------------------------------------------------------------------
class FlipbookOFXPlugin : public OFX::ImageEffect
{
public:
	FlipbookOFXPlugin( OfxImageEffectHandle handle, bool overInput ) :
		OFX::ImageEffect( handle ),
		over( overInput )
	{
		dstClip = fetchClip( kOfxImageEffectOutputClipName );
		if( over )
			srcClip = fetchClip( kOfxImageEffectSimpleSourceClipName );

		sheetFile = fetchStringParam( kParamSheetFile );
		columns   = fetchIntParam( kParamColumns );
		rows      = fetchIntParam( kParamRows );
		start     = fetchIntParam( kParamStart );
		length    = fetchIntParam( kParamLength );
		sampling  = fetchChoiceParam( kParamSampling );
		sync      = fetchChoiceParam( kParamSync );
		rate      = fetchDoubleParam( kParamRate );
		mode      = fetchChoiceParam( kParamMode );
		phase     = fetchDoubleParam( kParamPhase );
		fit       = fetchChoiceParam( kParamFit );
		centre    = fetchDouble2DParam( kParamCentre );
		scale     = fetchDoubleParam( kParamScale );
		rotation  = fetchDoubleParam( kParamRotation );
		flipH     = fetchBooleanParam( kParamFlipH );
		flipV     = fetchBooleanParam( kParamFlipV );
		copies    = fetchIntParam( kParamCopies );
		arrange   = fetchChoiceParam( kParamArrange );
		spread    = fetchDoubleParam( kParamSpread );
		stagger   = fetchDoubleParam( kParamStagger );
		seed      = fetchDoubleParam( kParamSeed );
		key       = fetchChoiceParam( kParamKey );
		keyColour = fetchRGBParam( kParamKeyColour );
		tolerance = fetchDoubleParam( kParamTolerance );
		softness  = fetchDoubleParam( kParamSoftness );
		tint      = fetchRGBParam( kParamTint );
		opacity   = fetchDoubleParam( kParamOpacity );
		backColour  = fetchRGBParam( kParamBackColour );
		backOpacity = fetchDoubleParam( kParamBackOpacity );
		preset      = fetchChoiceParam( kParamPreset );
		if( over )
			mix = fetchDoubleParam( kParamMix );
	}

	void render( const OFX::RenderArguments& args ) override
	{
		std::unique_ptr< OFX::Image > dst( dstClip->fetchImage( args.time ) );
		std::unique_ptr< OFX::Image > src;
		if( over && srcClip != nullptr && srcClip->isConnected() )
			src.reset( srcClip->fetchImage( args.time ) );

		const OFX::BitDepthEnum depth       = dst->getPixelDepth();
		const OFX::PixelComponentEnum comps = dst->getPixelComponents();
		if( comps != OFX::ePixelComponentRGBA && comps != OFX::ePixelComponentRGB )
			OFX::throwSuiteStatusException( kOfxStatErrUnsupported );

		Setup setup;
		buildSetup( args, *dst, setup );

		// Nothing decoded: hand back the background alone rather than failing.
		// A missing sheet is the commonest state this plugin is ever in -- it
		// is how every instance starts -- and a host error there would be
		// hostile.
		switch( depth )
		{
		case OFX::eBitDepthUByte:
			comps == OFX::ePixelComponentRGBA
				? run< FlipbookProcessor< unsigned char, 4, 255 > >( args, dst.get(), src.get(), setup )
				: run< FlipbookProcessor< unsigned char, 3, 255 > >( args, dst.get(), src.get(), setup );
			break;
		case OFX::eBitDepthUShort:
			comps == OFX::ePixelComponentRGBA
				? run< FlipbookProcessor< unsigned short, 4, 65535 > >( args, dst.get(), src.get(), setup )
				: run< FlipbookProcessor< unsigned short, 3, 65535 > >( args, dst.get(), src.get(), setup );
			break;
		case OFX::eBitDepthFloat:
			comps == OFX::ePixelComponentRGBA
				? run< FlipbookProcessor< float, 4, 1 > >( args, dst.get(), src.get(), setup )
				: run< FlipbookProcessor< float, 3, 1 > >( args, dst.get(), src.get(), setup );
			break;
		default:
			OFX::throwSuiteStatusException( kOfxStatErrUnsupported );
		}
	}

	void changedParam( const OFX::InstanceChangedArgs& args, const std::string& paramName ) override
	{
		// The About links open a browser and change nothing about the render.
		if( stoatworks::about::ofx::changedParam( args, paramName ) )
			return;

		if( applyingPreset )
			return;

		if( paramName == kParamSheetFile || paramName == kParamColumns || paramName == kParamRows )
		{
			cachedPath.clear();//force a reload
			return;
		}

		if( paramName == kParamPreset )
		{
			applyPreset( args );
			return;
		}

		// Editing anything a preset covers falls back to Custom. Judged by
		// comparing values rather than by the change reason, so a host echoing
		// our own writes cannot un-set the preset.
		int activePreset = 0;
		preset->getValue( activePreset );
		if( activePreset > 0 && coveredByPreset( paramName ) )
		{
			applyingPreset = true;
			preset->setValue( 0 );
			applyingPreset = false;
		}
	}

private:
	template< class PROC >
	void run( const OFX::RenderArguments& args, OFX::Image* dst, OFX::Image* src, const Setup& setup )
	{
		PROC processor( *this );
		processor.setDstImg( dst );
		processor.setSetup( src, &setup );
		processor.setRenderWindow( args.renderWindow );
		processor.process();
	}

	/// Decode the sheet if the path or the grid has changed. Called from
	/// render, before the processor is spawned, so the decode is never racing
	/// the host's render threads.
	const flipbook::Sheet& resolveSheet()
	{
		std::string path;
		sheetFile->getValue( path );
		const int c = std::clamp( columns->getValue(), 1, flipbook::kMaxGrid );
		const int r = std::clamp( rows->getValue(), 1, flipbook::kMaxGrid );

		if( path != cachedPath || c != cachedColumns || r != cachedRows )
		{
			cached        = flipbook::LoadSheet( path, c, r );
			cachedPath    = path;
			cachedColumns = c;
			cachedRows    = r;
		}
		return cached;
	}

	void buildSetup( const OFX::RenderArguments& args, OFX::Image& dst, Setup& setup )
	{
		using namespace flipbook;

		const flipbook::Sheet& sheet = resolveSheet();

		const OfxRectI bounds = dst.getBounds();
		const int outW        = bounds.x2 - bounds.x1;
		const int outH        = bounds.y2 - bounds.y1;

		setup.over = over;

		double backR = 0.0, backG = 0.0, backB = 0.0;
		backColour->getValueAtTime( args.time, backR, backG, backB );
		setup.backR = static_cast< float >( backR );
		setup.backG = static_cast< float >( backG );
		setup.backB = static_cast< float >( backB );
		setup.backA = static_cast< float >( backOpacity->getValueAtTime( args.time ) );

		if( !sheet.Valid() )
			return;//the background alone; copies stays empty

		setup.sheet       = &sheet;
		int samplingChoice = 0;
		sampling->getValue( samplingChoice );
		setup.pixelFilter = samplingChoice == static_cast< int >( Sampling::Pixel );
		setup.insetU      = 0.5f / static_cast< float >( sheet.width );
		setup.insetV      = 0.5f / static_cast< float >( sheet.height );

		//--- placement ------------------------------------------------------
		LayoutState layout;
		layout.copies  = std::clamp( copies->getValueAtTime( args.time ), 1, kMaxCopies );
		int arrangeChoice = 0;
		arrange->getValue( arrangeChoice );
		layout.arrange = static_cast< Arrange >( Option( static_cast< float >( arrangeChoice ),
		                                                 static_cast< int >( Arrange::Count ) ) );
		layout.spread  = SpreadFromParam( static_cast< float >( spread->getValueAtTime( args.time ) ) );
		layout.stagger = StaggerFromParam( static_cast< float >( stagger->getValueAtTime( args.time ) ) );
		layout.seed    = SeedFromParam( static_cast< float >( seed->getValueAtTime( args.time ) ) );

		double cx = 0.5, cy = 0.5;
		centre->getValueAtTime( args.time, cx, cy );
		// The OFX position parameter runs y-up, the way an OFX host's canvas
		// does; frame space runs y-down. One flip, here.
		layout.centreX = static_cast< float >( cx );
		layout.centreY = 1.0f - static_cast< float >( cy );

		layout.scale    = ScaleFromParam( static_cast< float >( scale->getValueAtTime( args.time ) ) );
		layout.rotation = RotationFromParam( static_cast< float >( rotation->getValueAtTime( args.time ) ) );
		int fitChoice = 0;
		fit->getValue( fitChoice );
		layout.fit      = static_cast< Fit >( Option( static_cast< float >( fitChoice ),
		                                              static_cast< int >( Fit::Count ) ) );
		layout.width    = outW;
		layout.height   = outH;
		layout.flipH    = flipH->getValueAtTime( args.time );
		layout.flipV    = flipV->getValueAtTime( args.time );

		const float cellW = static_cast< float >( sheet.width ) / static_cast< float >( sheet.columns );
		const float cellH = static_cast< float >( sheet.height ) / static_cast< float >( sheet.rows );
		if( cellH > 0.0f )
			layout.cellAspect = cellW / cellH;

		SolveCopies( layout, setup.copies );
		setup.rotation = layout.rotation;

		//--- the frame each copy shows --------------------------------------
		const int cells     = std::max( 1, sheet.columns * sheet.rows );
		const int from      = start->getValueAtTime( args.time );
		const int count     = length->getValueAtTime( args.time );
		const int runLength = RunLength( from, count, cells );

		// OFX hands time in frames; FrameClock wants seconds. A host that
		// reports no frame rate gets 25 -- wrong somewhere, but never zero,
		// which would make every frame the first one.
		double fps = dstClip->getFrameRate();
		if( !( fps > 0.0 ) )
			fps = 25.0;

		int syncChoice = 0, modeChoice = 0;
		sync->getValue( syncChoice );
		mode->getValue( modeChoice );
		const SyncMode syncMode = syncChoice == 1 ? SyncMode::Manual : SyncMode::Free;
		const PlayMode playMode = static_cast< PlayMode >( Option( static_cast< float >( modeChoice ),
		                                                           static_cast< int >( PlayMode::Count ) ) );

		const double framePos = FrameClock( args.time / fps, 120.0f, 0.0f, syncMode,
		                                    RateFromParam( static_cast< float >( rate->getValueAtTime( args.time ) ) ),
		                                    static_cast< float >( phase->getValueAtTime( args.time ) ),
		                                    runLength );

		const size_t drawn = setup.copies.size();
		setup.cellU0.resize( drawn );
		setup.cellV0.resize( drawn );
		setup.cellU1.resize( drawn );
		setup.cellV1.resize( drawn );

		for( size_t i = 0; i < drawn; ++i )
		{
			const int cell = from + FrameOffset( framePos + static_cast< double >( setup.copies[ i ].phaseOffset ),
			                                     runLength, playMode );
			CellRect( sheet, cell, setup.cellU0[ i ], setup.cellV0[ i ], setup.cellU1[ i ], setup.cellV1[ i ] );
		}

		//--- key and colour --------------------------------------------------
		int keyChoice = 0;
		key->getValue( keyChoice );
		setup.keyMode = Option( static_cast< float >( keyChoice ), static_cast< int >( KeyMode::Count ) );

		double kr = 0.0, kg = 0.0, kb = 0.0;
		keyColour->getValueAtTime( args.time, kr, kg, kb );
		setup.keyR = static_cast< float >( kr );
		setup.keyG = static_cast< float >( kg );
		setup.keyB = static_cast< float >( kb );
		setup.keyTolerance = KeyToleranceFromParam( static_cast< float >( tolerance->getValueAtTime( args.time ) ) );
		setup.keySoftness  = KeySoftnessFromParam( static_cast< float >( softness->getValueAtTime( args.time ) ) );

		double tr = 1.0, tg = 1.0, tb = 1.0;
		tint->getValueAtTime( args.time, tr, tg, tb );
		setup.tintR = static_cast< float >( tr );
		setup.tintG = static_cast< float >( tg );
		setup.tintB = static_cast< float >( tb );

		setup.opacity = static_cast< float >( opacity->getValueAtTime( args.time ) );
		if( over && mix != nullptr )
			setup.opacity *= static_cast< float >( mix->getValueAtTime( args.time ) );
	}

	//-----------------------------------------------------------------------
	// Presets. The same table the FFGL build reads, applied inside one edit
	// block so undo takes the whole preset back at once.
	//-----------------------------------------------------------------------
	bool coveredByPreset( const std::string& name ) const
	{
		static const char* const covered[] = {
			kParamRate, kParamMode, kParamSampling, kParamFit, kParamScale, kParamRotation,
			kParamCopies, kParamArrange, kParamSpread, kParamStagger,
			kParamKey, kParamKeyColour, kParamTolerance, kParamSoftness,
			kParamTint, kParamOpacity, kParamBackColour, kParamBackOpacity
		};
		for( const char* entry : covered )
			if( name == entry )
				return true;
		return false;
	}

	void applyPreset( const OFX::InstanceChangedArgs& args )
	{
		int chosen = 0;
		preset->getValue( chosen );
		if( chosen <= 0 || chosen > flipbook::presets::kCount )
			return;//Custom: the sliders keep whatever they said

		const flipbook::presets::Preset& p = flipbook::presets::kPresets[ chosen - 1 ];
		using namespace flipbook::presets;

		applyingPreset = true;
		beginEditBlock( "preset" );

		rate->setValue( p.v[ kRate ] );
		mode->setValue( static_cast< int >( p.v[ kMode ] ) );
		sampling->setValue( static_cast< int >( p.v[ kSampling ] ) );
		fit->setValue( static_cast< int >( p.v[ kFit ] ) );
		scale->setValue( p.v[ kScale ] );
		rotation->setValue( p.v[ kRotation ] );
		copies->setValue( static_cast< int >( p.v[ kCopies ] ) );
		arrange->setValue( static_cast< int >( p.v[ kArrange ] ) );
		spread->setValue( p.v[ kSpread ] );
		stagger->setValue( p.v[ kStagger ] );
		key->setValue( static_cast< int >( p.v[ kKey ] ) );
		keyColour->setValue( p.v[ kKeyR ], p.v[ kKeyG ], p.v[ kKeyB ] );
		tolerance->setValue( p.v[ kKeyTolerance ] );
		softness->setValue( p.v[ kKeySoftness ] );
		tint->setValue( p.v[ kTintR ], p.v[ kTintG ], p.v[ kTintB ] );
		opacity->setValue( p.v[ kOpacity ] );
		backColour->setValue( p.v[ kBackR ], p.v[ kBackG ], p.v[ kBackB ] );
		backOpacity->setValue( p.v[ kBackOpacity ] );

		endEditBlock();
		applyingPreset = false;

		( void )args;
	}

	const bool over;
	bool applyingPreset = false;

	OFX::Clip* dstClip = nullptr;
	OFX::Clip* srcClip = nullptr;

	OFX::StringParam* sheetFile   = nullptr;
	OFX::IntParam* columns        = nullptr;
	OFX::IntParam* rows           = nullptr;
	OFX::IntParam* start          = nullptr;
	OFX::IntParam* length         = nullptr;
	OFX::ChoiceParam* sampling    = nullptr;
	OFX::ChoiceParam* sync        = nullptr;
	OFX::DoubleParam* rate        = nullptr;
	OFX::ChoiceParam* mode        = nullptr;
	OFX::DoubleParam* phase       = nullptr;
	OFX::ChoiceParam* fit         = nullptr;
	OFX::Double2DParam* centre    = nullptr;
	OFX::DoubleParam* scale       = nullptr;
	OFX::DoubleParam* rotation    = nullptr;
	OFX::BooleanParam* flipH      = nullptr;
	OFX::BooleanParam* flipV      = nullptr;
	OFX::IntParam* copies         = nullptr;
	OFX::ChoiceParam* arrange     = nullptr;
	OFX::DoubleParam* spread      = nullptr;
	OFX::DoubleParam* stagger     = nullptr;
	OFX::DoubleParam* seed        = nullptr;
	OFX::ChoiceParam* key         = nullptr;
	OFX::RGBParam* keyColour      = nullptr;
	OFX::DoubleParam* tolerance   = nullptr;
	OFX::DoubleParam* softness    = nullptr;
	OFX::RGBParam* tint           = nullptr;
	OFX::DoubleParam* opacity     = nullptr;
	OFX::RGBParam* backColour     = nullptr;
	OFX::DoubleParam* backOpacity = nullptr;
	OFX::DoubleParam* mix         = nullptr;
	OFX::ChoiceParam* preset      = nullptr;

	flipbook::Sheet cached;
	std::string cachedPath;
	int cachedColumns = 0;
	int cachedRows    = 0;
};

//---------------------------------------------------------------------------
// Description
//---------------------------------------------------------------------------
OFX::DoubleParamDescriptor* defineSlider( OFX::ImageEffectDescriptor& desc, OFX::PageParamDescriptor* page,
                                          const char* name, const char* label, const char* hint, double def )
{
	OFX::DoubleParamDescriptor* p = desc.defineDoubleParam( name );
	p->setLabels( label, label, label );
	p->setHint( hint );
	p->setRange( 0.0, 1.0 );
	p->setDisplayRange( 0.0, 1.0 );
	p->setDefault( def );
	page->addChild( *p );
	return p;
}

OFX::IntParamDescriptor* defineCount( OFX::ImageEffectDescriptor& desc, OFX::PageParamDescriptor* page,
                                      const char* name, const char* label, const char* hint,
                                      int def, int lo, int hi )
{
	OFX::IntParamDescriptor* p = desc.defineIntParam( name );
	p->setLabels( label, label, label );
	p->setHint( hint );
	p->setRange( lo, hi );
	p->setDisplayRange( lo, hi );
	p->setDefault( def );
	page->addChild( *p );
	return p;
}

void describeCommon( OFX::ImageEffectDescriptor& desc, const char* name )
{
	desc.setLabels( name, name, name );
	desc.setPluginGrouping( kPluginGrouping );
	desc.setPluginDescription( kPluginDescription );

	desc.addSupportedBitDepth( OFX::eBitDepthUByte );
	desc.addSupportedBitDepth( OFX::eBitDepthUShort );
	desc.addSupportedBitDepth( OFX::eBitDepthFloat );

	// The frame is a pure function of the clock, so frames render in any order,
	// alone, concurrently. Tiles are declined because copies are placed in
	// frame space and a tile does not know the whole frame's geometry.
	desc.setSupportsTiles( false );
	desc.setTemporalClipAccess( false );
	desc.setRenderThreadSafety( OFX::eRenderFullySafe );
	desc.setSupportsMultiResolution( true );
}

void describeParams( OFX::ImageEffectDescriptor& desc, bool overVariant )
{
	using namespace flipbook;

	OFX::PageParamDescriptor* page = desc.definePageParam( "Controls" );

	// Factory presets, from the same table the FFGL build reads (Presets.h).
	// Custom is not a preset: it means the sliders are the truth. Nothing in
	// the Sheet group is covered -- a preset must never redefine somebody
	// else's grid.
	OFX::ChoiceParamDescriptor* presetParam = desc.defineChoiceParam( kParamPreset );
	presetParam->setLabels( "Preset", "Preset", "Preset" );
	presetParam->setHint( "Named ways of treating a sheet. Picking one sets the covered controls; "
	                      "editing any of them afterwards falls back to Custom. Never touches the "
	                      "sheet, the grid or the frame range." );
	presetParam->appendOption( "Custom" );
	for( int i = 0; i < presets::kCount; ++i )
		presetParam->appendOption( presets::kPresets[ i ].name );
	presetParam->setDefault( 0 );
	presetParam->setEvaluateOnChange( false );//the copied values re-render; the label itself does not
	page->addChild( *presetParam );

	//--- Sheet -------------------------------------------------------------
	OFX::GroupParamDescriptor* sheetGroup = desc.defineGroupParam( "Sheet" );
	sheetGroup->setLabels( "Sheet", "Sheet", "Sheet" );

	OFX::StringParamDescriptor* file = desc.defineStringParam( kParamSheetFile );
	file->setLabels( "Sheet", "Sheet", "Sheet" );
	file->setHint( "The page of stills. PNG, JPEG, GIF, BMP, TGA or PSD. An animated GIF brings "
	               "its own frame count and ignores Columns and Rows." );
	file->setStringType( OFX::eStringTypeFilePath );
	file->setFilePathExists( false );//a path that no longer exists must still load the composition
	file->setParent( *sheetGroup );
	page->addChild( *file );

	defineCount( desc, page, kParamColumns, "Columns", "Cells across the sheet.", 5, 1, kMaxGrid )
		->setParent( *sheetGroup );
	defineCount( desc, page, kParamRows, "Rows", "Cells down the sheet.", 5, 1, kMaxGrid )
		->setParent( *sheetGroup );
	defineCount( desc, page, kParamStart, "Start Frame",
	             "The first cell of the run, counting left to right then top to bottom from 0.",
	             0, 0, kMaxFrames - 1 )
		->setParent( *sheetGroup );
	defineCount( desc, page, kParamLength, "Frame Count",
	             "How many cells the run covers. 0 plays to the end of the sheet. A run may pass "
	             "the last cell and come round to the first.",
	             0, 0, kMaxFrames )
		->setParent( *sheetGroup );

	OFX::ChoiceParamDescriptor* samplingParam = desc.defineChoiceParam( kParamSampling );
	samplingParam->setLabels( "Sampling", "Sampling", "Sampling" );
	samplingParam->setHint( "Smooth is bilinear. Pixel is nearest-neighbour, and is the difference "
	                        "between pixel art and mush." );
	samplingParam->appendOption( "Smooth" );
	samplingParam->appendOption( "Pixel" );
	samplingParam->setDefault( 0 );
	samplingParam->setParent( *sheetGroup );
	page->addChild( *samplingParam );

	//--- Playback ----------------------------------------------------------
	OFX::GroupParamDescriptor* playGroup = desc.defineGroupParam( "Playback" );
	playGroup->setLabels( "Playback", "Playback", "Playback" );

	OFX::ChoiceParamDescriptor* syncParam = desc.defineChoiceParam( kParamSync );
	syncParam->setLabels( "Sync", "Sync", "Sync" );
	syncParam->setHint( "Free runs on the clip's clock. Manual ignores it and lets Phase drive the "
	                    "sequence, which is the mode for keyframing against the edit." );
	for( int i = 0; i < kSyncCount; ++i )
		syncParam->appendOption( kSyncNames[ i ] );
	syncParam->setDefault( 0 );
	syncParam->setParent( *playGroup );
	page->addChild( *syncParam );

	defineSlider( desc, page, kParamRate, "Rate", "Frames per second, 0.5 to 60.", 0.664 )
		->setParent( *playGroup );

	OFX::ChoiceParamDescriptor* modeParam = desc.defineChoiceParam( kParamMode );
	modeParam->setLabels( "Mode", "Mode", "Mode" );
	modeParam->setHint( "Loop repeats, Ping-Pong reverses at each end, Once holds the last frame." );
	modeParam->appendOption( "Loop" );
	modeParam->appendOption( "Ping-Pong" );
	modeParam->appendOption( "Once" );
	modeParam->setDefault( 0 );
	modeParam->setParent( *playGroup );
	page->addChild( *modeParam );

	defineSlider( desc, page, kParamPhase, "Phase",
	              "Position through the run, under Manual sync. 0 is the first frame, 1 the last.", 0.0 )
		->setParent( *playGroup );

	//--- Layout ------------------------------------------------------------
	OFX::GroupParamDescriptor* layoutGroup = desc.defineGroupParam( "Layout" );
	layoutGroup->setLabels( "Layout", "Layout", "Layout" );

	OFX::ChoiceParamDescriptor* fitParam = desc.defineChoiceParam( kParamFit );
	fitParam->setLabels( "Fit", "Fit", "Fit" );
	fitParam->setHint( "Native keeps the cell's proportions on the short edge. Fill Frame covers "
	                   "the raster and crops. Stretch maps one cell onto the whole raster." );
	fitParam->appendOption( "Native" );
	fitParam->appendOption( "Fill Frame" );
	fitParam->appendOption( "Stretch" );
	fitParam->setDefault( 0 );
	fitParam->setParent( *layoutGroup );
	page->addChild( *fitParam );

	OFX::Double2DParamDescriptor* centreParam = desc.defineDouble2DParam( kParamCentre );
	centreParam->setLabels( "Position", "Position", "Position" );
	centreParam->setHint( "Where the sprite sits, or the centre of the field of copies." );
	// Not eDoubleTypeNormalisedXY: that enumerator is guarded by a symbol the
	// OFX 1.4 headers no longer define, so it does not compile. Plain 0..1 is
	// what the FFGL side uses anyway, which keeps one meaning for Position.
	centreParam->setRange( -1.0, -1.0, 2.0, 2.0 );
	centreParam->setDisplayRange( 0.0, 0.0, 1.0, 1.0 );
	centreParam->setDefault( 0.5, 0.5 );
	centreParam->setParent( *layoutGroup );
	page->addChild( *centreParam );

	defineSlider( desc, page, kParamScale, "Scale",
	              "A multiple of whatever Fit decided. Exactly 1.0 at the centre of the slider.", 0.5 )
		->setParent( *layoutGroup );
	defineSlider( desc, page, kParamRotation, "Rotation",
	              "-180 to 180 degrees, zero at the centre of the slider.", 0.5 )
		->setParent( *layoutGroup );

	OFX::BooleanParamDescriptor* flipHParam = desc.defineBooleanParam( kParamFlipH );
	flipHParam->setLabels( "Flip H", "Flip H", "Flip H" );
	flipHParam->setDefault( false );
	flipHParam->setParent( *layoutGroup );
	page->addChild( *flipHParam );

	OFX::BooleanParamDescriptor* flipVParam = desc.defineBooleanParam( kParamFlipV );
	flipVParam->setLabels( "Flip V", "Flip V", "Flip V" );
	flipVParam->setDefault( false );
	flipVParam->setParent( *layoutGroup );
	page->addChild( *flipVParam );

	//--- Copies ------------------------------------------------------------
	OFX::GroupParamDescriptor* copyGroup = desc.defineGroupParam( "Copies" );
	copyGroup->setLabels( "Copies", "Copies", "Copies" );

	defineCount( desc, page, kParamCopies, "Copies", "How many of the sprite to draw.", 1, 1, kMaxCopies )
		->setParent( *copyGroup );

	OFX::ChoiceParamDescriptor* arrangeParam = desc.defineChoiceParam( kParamArrange );
	arrangeParam->setLabels( "Arrange", "Arrange", "Arrange" );
	arrangeParam->setHint( "How the copies are laid out. Does nothing with one copy." );
	arrangeParam->appendOption( "Grid" );
	arrangeParam->appendOption( "Ring" );
	arrangeParam->appendOption( "Scatter" );
	arrangeParam->setDefault( 0 );
	arrangeParam->setParent( *copyGroup );
	page->addChild( *arrangeParam );

	defineSlider( desc, page, kParamSpread, "Spread",
	              "How far apart the copies sit. Past two thirds they start to leave the raster.", 0.6 )
		->setParent( *copyGroup );
	defineSlider( desc, page, kParamStagger, "Stagger",
	              "Frames of offset per copy, up to 8. What turns a field of copies into a chase.", 0.0 )
		->setParent( *copyGroup );
	defineSlider( desc, page, kParamSeed, "Seed", "Which scatter. Ignored unless Arrange is Scatter.", 0.0 )
		->setParent( *copyGroup );

	//--- Key ---------------------------------------------------------------
	OFX::GroupParamDescriptor* keyGroup = desc.defineGroupParam( "Key" );
	keyGroup->setLabels( "Key", "Key", "Key" );

	OFX::ChoiceParamDescriptor* keyParam = desc.defineChoiceParam( kParamKey );
	keyParam->setLabels( "Key", "Key", "Key" );
	keyParam->setHint( "Alpha trusts the file. Luma removes what matches the key colour's "
	                   "brightness; Colour removes what matches the key colour itself. None makes "
	                   "the whole cell opaque." );
	keyParam->appendOption( "Alpha" );
	keyParam->appendOption( "Luma" );
	keyParam->appendOption( "Colour" );
	keyParam->appendOption( "None" );
	keyParam->setDefault( 0 );
	keyParam->setParent( *keyGroup );
	page->addChild( *keyParam );

	OFX::RGBParamDescriptor* keyColourParam = desc.defineRGBParam( kParamKeyColour );
	keyColourParam->setLabels( "Key Colour", "Key Colour", "Key Colour" );
	keyColourParam->setHint( "The background to remove. Ignored under Alpha and None." );
	keyColourParam->setDefault( 0.0, 0.0, 0.0 );
	keyColourParam->setParent( *keyGroup );
	page->addChild( *keyColourParam );

	defineSlider( desc, page, kParamTolerance, "Tolerance",
	              "How far from the key colour still counts as background.", 0.0 )
		->setParent( *keyGroup );
	defineSlider( desc, page, kParamSoftness, "Softness", "A soft edge outside the tolerance.", 0.0 )
		->setParent( *keyGroup );

	//--- Colour ------------------------------------------------------------
	OFX::GroupParamDescriptor* colourGroup = desc.defineGroupParam( "Colour" );
	colourGroup->setLabels( "Colour", "Colour", "Colour" );

	OFX::RGBParamDescriptor* tintParam = desc.defineRGBParam( kParamTint );
	tintParam->setLabels( "Tint", "Tint", "Tint" );
	tintParam->setHint( "Multiplies the sprite's colour. White leaves it alone." );
	tintParam->setDefault( 1.0, 1.0, 1.0 );
	tintParam->setParent( *colourGroup );
	page->addChild( *tintParam );

	defineSlider( desc, page, kParamOpacity, "Opacity", "How much of the sprite is drawn.", 1.0 )
		->setParent( *colourGroup );

	OFX::RGBParamDescriptor* backParam = desc.defineRGBParam( kParamBackColour );
	backParam->setLabels( "Background", "Background", "Background" );
	backParam->setDefault( 0.0, 0.0, 0.0 );
	backParam->setParent( *colourGroup );
	page->addChild( *backParam );

	defineSlider( desc, page, kParamBackOpacity, "Background Opacity",
	              "The plugin's own backdrop. Zero leaves the output transparent.", 0.0 )
		->setParent( *colourGroup );

	//--- Output ------------------------------------------------------------
	if( overVariant )
	{
		OFX::GroupParamDescriptor* output = desc.defineGroupParam( "Output" );
		output->setLabels( "Output", "Output", "Output" );

		defineSlider( desc, page, kParamMix, "Mix", "Dry/wet with the untouched clip.", 1.0 )
			->setParent( *output );
	}

	// The Stoatworks About block: a read-only credit line and one push button
	// per link, in a group that starts folded. Last, so it sits under the
	// effect's own controls.
	stoatworks::about::ofx::describe( desc, page );
}

} // namespace

//---------------------------------------------------------------------------
// "Flipbook": the generator.
//---------------------------------------------------------------------------
mDeclarePluginFactory( FlipbookSourceFactory, {}, {} );

void FlipbookSourceFactory::describe( OFX::ImageEffectDescriptor& desc )
{
	describeCommon( desc, "Flipbook" );
	desc.addSupportedContext( OFX::eContextGenerator );
	desc.addSupportedContext( OFX::eContextGeneral );
}

void FlipbookSourceFactory::describeInContext( OFX::ImageEffectDescriptor& desc, OFX::ContextEnum )
{
	OFX::ClipDescriptor* dstClip = desc.defineClip( kOfxImageEffectOutputClipName );
	dstClip->addSupportedComponent( OFX::ePixelComponentRGBA );
	dstClip->addSupportedComponent( OFX::ePixelComponentRGB );
	dstClip->setSupportsTiles( false );

	describeParams( desc, false );
}

OFX::ImageEffect* FlipbookSourceFactory::createInstance( OfxImageEffectHandle handle, OFX::ContextEnum )
{
	return new FlipbookOFXPlugin( handle, false );
}

//---------------------------------------------------------------------------
// "Flipbook Over": the effect.
//---------------------------------------------------------------------------
mDeclarePluginFactory( FlipbookOverFactory, {}, {} );

void FlipbookOverFactory::describe( OFX::ImageEffectDescriptor& desc )
{
	describeCommon( desc, "Flipbook Over" );
	desc.addSupportedContext( OFX::eContextFilter );
	desc.addSupportedContext( OFX::eContextGeneral );
}

void FlipbookOverFactory::describeInContext( OFX::ImageEffectDescriptor& desc, OFX::ContextEnum )
{
	OFX::ClipDescriptor* srcClip = desc.defineClip( kOfxImageEffectSimpleSourceClipName );
	srcClip->addSupportedComponent( OFX::ePixelComponentRGBA );
	srcClip->addSupportedComponent( OFX::ePixelComponentRGB );
	srcClip->setSupportsTiles( false );

	OFX::ClipDescriptor* dstClip = desc.defineClip( kOfxImageEffectOutputClipName );
	dstClip->addSupportedComponent( OFX::ePixelComponentRGBA );
	dstClip->addSupportedComponent( OFX::ePixelComponentRGB );
	dstClip->setSupportsTiles( false );

	describeParams( desc, true );
}

OFX::ImageEffect* FlipbookOverFactory::createInstance( OfxImageEffectHandle handle, OFX::ContextEnum )
{
	return new FlipbookOFXPlugin( handle, true );
}

void OFX::Plugin::getPluginIDs( OFX::PluginFactoryArray& ids )
{
	// Deliberately leaked: a by-value static would register an exit-time
	// destructor inside this module, and a host that dlclose()s the bundle
	// before process exit then jumps through a dangling pointer.
	static FlipbookSourceFactory* sourceFactory =
		new FlipbookSourceFactory( kSourceIdentifier, PLUGIN_VERSION_MAJOR, PLUGIN_VERSION_MINOR );
	static FlipbookOverFactory* overFactory =
		new FlipbookOverFactory( kOverIdentifier, PLUGIN_VERSION_MAJOR, PLUGIN_VERSION_MINOR );
	ids.push_back( sourceFactory );
	ids.push_back( overFactory );
}
