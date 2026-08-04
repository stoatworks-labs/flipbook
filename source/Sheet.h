#pragma once

#include <cstdint>
#include <string>
#include <vector>

/**
    The sheet: a page of stills, decoded once and left alone.

    ## What a sheet is

    One RGBA image plus a grid. Everything downstream — the playback clock, the
    placement, the shader — only ever asks two questions of it: how many cells
    are there, and where is cell *n*. That is deliberately the whole interface,
    because it is what lets three quite different things all be "a sheet":

    - **A still image with a grid the operator declares.** The ordinary case,
      and the only one where Columns and Rows mean anything.
    - **An animated GIF**, whose frames are laid out into a grid *here* rather
      than by whoever exported it. The file already knows how many frames it
      has, so Columns and Rows are ignored and reported as ignored.
    - **The incoming clip**, for the effect plugin, which never comes through
      this file at all — it arrives as a GL texture the host owns. `Sheet` is
      still the shape the rest of the code sees; only the ownership differs.

    ## Straight alpha, not premultiplied

    The decoded pixels are kept as stb hands them over: **straight** alpha.
    They are premultiplied at the very end of the fragment shader instead, and
    the ordering is not a preference.

    FFGL composites premultiplied, so *something* has to multiply. Doing it here
    would mean the shader's colour key sees `rgb * a` rather than `rgb` — and
    against a sheet with any partial alpha at all, a key set from a swatch of
    the visible background would then match nothing, because the pixels it is
    being compared against have been scaled by an alpha the operator never saw.
    Keying is a question about the picture, so it has to be asked before the
    picture is multiplied by anything.

    ## The size guard

    A sheet is uploaded as one texture, and `GL_MAX_TEXTURE_SIZE` is a real
    ceiling — 16384 on most of what runs Resolume, and there is no error worth
    reading when you cross it: `glTexImage2D` fails and the sampler returns
    black, which looks exactly like a sheet that did not load. So the decode
    refuses an oversized still with a message, and an oversized GIF drops
    trailing frames rather than failing outright. `note` says which happened,
    and it is what the harness prints and Diag logs.
*/
namespace flipbook
{
/// The widest or tallest texture the sheet is allowed to become. Conservative
/// on purpose: the true limit is a runtime query, this is a value that is safe
/// everywhere Resolume runs, and a sheet that needs more than 8192 px on a side
/// is not a sheet, it is a mistake.
constexpr int kMaxSheetPx = 8192;

/// Extensions offered by the file parameter. stb_image's set, minus the ones
/// nobody exports a sprite sheet as. HDR and PIC are omitted deliberately: they
/// decode to float and this path is 8-bit.
extern const char* const kSheetExtensions[];
extern const int kSheetExtensionCount;

struct Sheet
{
	int width  = 0;///< the decoded page, in pixels
	int height = 0;

	/// The grid. When `gridFromFile` is set these came out of the file (an
	/// animated GIF) and the Columns and Rows parameters were ignored.
	int columns = 1;
	int rows    = 1;

	/// Cells that actually hold a frame. Equal to columns * rows for a still —
	/// a declared grid has no way to say a cell is empty — and to the GIF's
	/// frame count otherwise, which is why the two are not the same number and
	/// why the last row of a GIF grid can be short.
	int frames = 0;

	bool gridFromFile = false;

	/// Straight-alpha RGBA8, `width * height * 4` bytes, row 0 at the top.
	std::vector< uint8_t > rgba;

	/// One line for the log and for `fbtest --sheet`: where it came from, what
	/// grid it ended up with, and anything that was dropped or refused.
	std::string note;

	bool Valid() const { return width > 0 && height > 0 && !rgba.empty(); }
};

/// Decode `path` into a sheet. `columns` and `rows` are the operator's declared
/// grid and are used only when the file does not carry one of its own.
///
/// A failure comes back as a Sheet with `Valid()` false and a `note` saying
/// why; there is no error code, because every caller does the same thing with
/// one (log it and draw nothing) and a code would only be a second way to spell
/// the message.
Sheet LoadSheet( const std::string& path, int columns, int rows );

/// The rectangle of cell `index`, in 0..1 texture coordinates with the origin
/// at the **top left**.
///
/// Top left and not bottom left, and no flip is needed anywhere, because
/// `glTexImage2D` puts the first row of the buffer at v = 0 and the first row
/// of this buffer is the top of the image. That is only true of a sheet *we*
/// uploaded: the incoming clip, which the effect plugin can also use as a
/// sheet, arrives from the host with v = 0 at the **bottom**. Hence the shader's
/// `SheetFlipV` uniform, which is 0 for a file and 1 for the clip.
///
/// Out-of-range indices wrap rather than clamp. A sheet whose Start Frame is
/// past the end should show the sheet from the beginning, not a hundred copies
/// of the last cell.
void CellRect( const Sheet& sheet, int index, float& u0, float& v0, float& u1, float& v1 );

} // namespace flipbook
