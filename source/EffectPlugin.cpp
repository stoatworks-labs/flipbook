#include "Flipbook.h"

/**
    The effect: the same sheet, over the incoming clip -- and, if you want, the
    incoming clip *as* the sheet.

    The first half exists because the obvious way to get a sprite over footage
    in Resolume -- put the source on its own layer and pick a blend mode --
    costs a layer and puts the sprite's controls somewhere other than the clip
    it belongs to. This one sits on the clip.

    The second half is the more interesting one, and it is the reason "Sheet
    From" is a parameter rather than a build flag. Point this at a clip that is
    itself a sheet and Resolume's own media management does the file handling:
    the sheet is in the composition, it moves with it, it can be swapped from
    the deck, and it can be a *moving* image rather than a still. What you give
    up is exactness -- the host has already scaled the sheet into the layer's
    raster by the time the plugin sees it, so the cell boundaries are only as
    true as the clip's own transform. Use a file when the grid has to be right,
    and the input when the convenience is worth more.

    See SourcePlugin.cpp for why this file is listed in its own target rather
    than in the shared library.
*/
namespace
{
class FlipbookEffect : public flipbook::FlipbookPlugin
{
public:
	FlipbookEffect() :
		FlipbookPlugin( true )
	{
	}
};
} // namespace

static CFFGLPluginInfo PluginInfo(
	PluginFactory< FlipbookEffect >,                  // Create method
	"FB02",                                           // Plugin unique ID of maximum length 4
	"Flipbook Over",                                  // Plugin name
	2,                                                // API major version number
	1,                                                // API minor version number
	0,                                                // Plugin major version number
	1,                                                // Plugin minor version number
	FF_EFFECT,                                        // Plugin type
	"Sprite sheet over the clip, or of the clip.\n\nPoint it at a page of stills, tell it the grid, and it plays the cells as an animation - one copy or sixty-four of them, placed and scattered as you like.\n\nThe sprites are drawn as geometry rather than tested for at every pixel, which is why a screen full of small copies stays cheap and stays cheap at 4K.\n\nStart from a Preset, at the bottom.",// Plugin description
	"Flipbook FFGL effect"                            // About
);

extern "C" const char* FlipbookEffectBuildStamp()
{
	return "flipbook " FLIPBOOK_VERSION " effect, built " __DATE__ " " __TIME__;
}
