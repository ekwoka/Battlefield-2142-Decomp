// What a Flash movie's frame gives the renderer: batches of triangles in
// the movie's stage (1024 x 768 for the menus, y down), each with one
// texture (a file, or an image made in memory: bitmaps, gradients, glyph
// atlases) and per-vertex colour transforms, drawn in order over
// everything else. Header-only so the renderer needs nothing else of the
// UI library.
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace dice::hfe::ui {

struct MenuVertex {
	float x, y;    // stage pixels
	float u, v;    // texture coordinates
	float mult[4]; // colour = texture * mult + add (Flash's colour transform)
	float add[4];
};

// An image made in memory: RGBA8, straight (not premultiplied) alpha.
struct MenuImage {
	uint32_t width = 0, height = 0;
	std::vector<uint8_t> rgba;
};

// A movie a clip shows in place of an image: the menu's Bink videos
// (loadMovie / the image component's loadImg of "?titan_tutorial.bik"),
// which SwiffPlayer draws as a video texture. The host decodes them
// (Movie/MovieDecoder.h); the player asks for the picture as time passes.
class VideoSource {
public:
	virtual ~VideoSource() = default;
	// The picture `seconds` later: true and `frame` replaced when a new
	// picture is due, false when it stays (also after the last one).
	virtual bool advance(double seconds, MenuImage& frame) = 0;
	// Whether the last picture was shown.
	virtual bool finished() const = 0;
};

struct MenuBatch {
	std::string texture;                     // a file path, or empty with `image`
	std::shared_ptr<const MenuImage> image;  // kept alive while drawn; uploaded once per object
	bool repeat = false;                     // bitmap fills that tile
	std::vector<MenuVertex> vertices;        // triangles
	// The mask state it draws with (Stencil.h): Submit marks the stencil
	// at `stencilLevel` (no colour), Test draws where the stencil equals
	// it, Clear (no vertices) clears the stencil to 0.
	enum class Stencil : uint8_t { Off, Submit, Test, Clear } stencil = Stencil::Off;
	int stencilLevel = 0;
};

struct MenuFrame {
	float width = 1024, height = 768; // the stage
	std::vector<MenuBatch> batches;
};

} // namespace dice::hfe::ui
