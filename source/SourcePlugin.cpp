#include "Flipbook.h"

/**
    The generator: a sheet played over its own background, no input.

    **This file is listed directly in the FlipbookSource target, not in
    flipbook_core.** Both plugins share the class; what they do not share is the
    `CFFGLPluginInfo` below, and putting either registration in the shared
    library would register both plugins into both bundles.

    It is also why the shared library is an OBJECT library rather than a STATIC
    one. `CFFGLPluginInfo` registers itself from a file-scope constructor and
    nothing ever references it by name, so in an archive the linker is entitled
    to drop the whole translation unit -- giving a bundle that loads, exports
    `plugMain`, and reports that it contains no plugins.

        nm -gU Flipbook.bundle/Contents/MacOS/Flipbook | grep plugMain
*/
namespace
{
class FlipbookSource : public flipbook::FlipbookPlugin
{
public:
	FlipbookSource() :
		FlipbookPlugin( false )
	{
	}
};
} // namespace

static CFFGLPluginInfo PluginInfo(
	PluginFactory< FlipbookSource >,                  // Create method
	"FB01",                                           // Plugin unique ID of maximum length 4
	"Flipbook",                                       // Plugin name
	2,                                                // API major version number
	1,                                                // API minor version number
	0,                                                // Plugin major version number
	1,                                                // Plugin minor version number
	FF_SOURCE,                                        // Plugin type
	"Sprite sheet player: a page of stills, animated.\n\nPoint it at a page of stills, tell it the grid, and it plays the cells as an animation - one copy or sixty-four of them, placed and scattered as you like.\n\nThe sprites are drawn as geometry rather than tested for at every pixel, which is why a screen full of small copies stays cheap and stays cheap at 4K.\n\nStart from a Preset, at the bottom.",// Plugin description
	"Flipbook FFGL source"                            // About
);

extern "C" const char* FlipbookSourceBuildStamp()
{
	return "flipbook " FLIPBOOK_VERSION " source, built " __DATE__ " " __TIME__;
}
