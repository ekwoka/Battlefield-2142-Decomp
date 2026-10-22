// The SWF character layer: bit fields, shapes tessellated to the expected
// areas, curve flattening, bitmaps (lossless palettes, JPEG with Flash's
// quirks) and, with $BF2142_GAME_DIR, every definition in the shipped
// mainMenu.dfm.
#include "IO/File/ZipArchive.h"
#include "TestMain.h"
#include "UI/Characters/CharacterDictionary.h"
#include "UI/Characters/Tessellate.h"
#include "UI/Dfm.h"
#include "UI/Swf.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>

using namespace dice::hfe::ui;

namespace {

// Bits most-significant first, as SWF packs them.
class BitWriter {
public:
	void ub(uint32_t value, int bits)
	{
		for (int i = bits - 1; i >= 0; --i) {
			if (m_bit == 0) {
				m_bytes.push_back(0);
			}
			m_bytes.back() |= static_cast<uint8_t>(((value >> i) & 1) << (7 - m_bit));
			m_bit = (m_bit + 1) & 7;
		}
	}
	void sb(int32_t value, int bits) { ub(static_cast<uint32_t>(value), bits); }
	void align() { m_bit = 0; }
	void u8(uint8_t v)
	{
		align();
		m_bytes.push_back(v);
	}
	void u16(uint16_t v)
	{
		u8(static_cast<uint8_t>(v));
		u8(static_cast<uint8_t>(v >> 8));
	}
	void u32(uint32_t v)
	{
		u16(static_cast<uint16_t>(v));
		u16(static_cast<uint16_t>(v >> 16));
	}
	void bytes(const std::vector<uint8_t>& b)
	{
		align();
		m_bytes.insert(m_bytes.end(), b.begin(), b.end());
	}
	const std::vector<uint8_t>& data() const { return m_bytes; }

private:
	std::vector<uint8_t> m_bytes;
	int m_bit = 0;
};

// SHAPERECORD writers, 16-bit coordinate fields.
void moveTo(BitWriter& w, int32_t x, int32_t y, uint32_t fill0, uint32_t fill1, uint32_t line)
{
	w.ub(0, 1);
	w.ub(0x0f, 5); // line, fill1, fill0, move
	w.ub(16, 5);
	w.sb(x, 16);
	w.sb(y, 16);
	w.ub(fill0, 1);
	w.ub(fill1, 1);
	w.ub(line, 1);
}

void lineTo(BitWriter& w, int32_t dx, int32_t dy)
{
	w.ub(3, 2); // edge, straight
	w.ub(14, 4);
	w.ub(1, 1); // general line
	w.sb(dx, 16);
	w.sb(dy, 16);
}

void curveTo(BitWriter& w, int32_t cdx, int32_t cdy, int32_t adx, int32_t ady)
{
	w.ub(2, 2); // edge, curved
	w.ub(14, 4);
	for (int32_t v : {cdx, cdy, adx, ady}) {
		w.sb(v, 16);
	}
}

// DefineShape header: id 1, bounds, one red fill, one 40-twip line.
BitWriter shapeHeader(int32_t xMax, int32_t yMax)
{
	BitWriter w;
	w.u16(1);
	w.ub(16, 5);
	for (int32_t v : {0, xMax, 0, yMax}) {
		w.sb(v, 16);
	}
	w.u8(1);
	w.u8(0x00);
	w.u8(255), w.u8(0), w.u8(0);
	w.u8(1);
	w.u16(40);
	w.u8(0), w.u8(0), w.u8(255);
	w.align();
	w.ub(1, 4);
	w.ub(1, 4);
	return w;
}

void endShape(BitWriter& w)
{
	w.ub(0, 6);
	w.align();
}

double batchArea(const swf::ShapeMesh& mesh, swf::MeshBatch::Kind kind)
{
	double area = 0;
	for (const swf::MeshBatch& b : mesh.batches) {
		if (b.kind == kind) {
			area += swf::triangleArea(b.triangles);
		}
	}
	return area;
}

// zlib stream with one stored block (no compression needed for tests).
std::vector<uint8_t> zlibStored(const std::vector<uint8_t>& data)
{
	std::vector<uint8_t> out {0x78, 0x01, 0x01};
	const uint16_t len = static_cast<uint16_t>(data.size());
	out.push_back(static_cast<uint8_t>(len));
	out.push_back(static_cast<uint8_t>(len >> 8));
	out.push_back(static_cast<uint8_t>(~len));
	out.push_back(static_cast<uint8_t>(~len >> 8));
	out.insert(out.end(), data.begin(), data.end());
	uint32_t a = 1, b = 0;
	for (uint8_t c : data) {
		a = (a + c) % 65521;
		b = (b + a) % 65521;
	}
	const uint32_t adler = b << 16 | a;
	for (int shift = 24; shift >= 0; shift -= 8) {
		out.push_back(static_cast<uint8_t>(adler >> shift));
	}
	return out;
}

// A 16x8 baseline JPEG (cjpeg -quality 95 -sample 1x1): left half red,
// right half blue.
const std::vector<uint8_t> kTinyJpeg {
	0xff, 0xd8, 0xff, 0xe0, 0x00, 0x10, 0x4a, 0x46, 0x49, 0x46, 0x00, 0x01, 0x01, 0x00, 0x00, 0x01,
	0x00, 0x01, 0x00, 0x00, 0xff, 0xdb, 0x00, 0x43, 0x00, 0x02, 0x01, 0x01, 0x01, 0x01, 0x01, 0x02,
	0x01, 0x01, 0x01, 0x02, 0x02, 0x02, 0x02, 0x02, 0x04, 0x03, 0x02, 0x02, 0x02, 0x02, 0x05, 0x04,
	0x04, 0x03, 0x04, 0x06, 0x05, 0x06, 0x06, 0x06, 0x05, 0x06, 0x06, 0x06, 0x07, 0x09, 0x08, 0x06,
	0x07, 0x09, 0x07, 0x06, 0x06, 0x08, 0x0b, 0x08, 0x09, 0x0a, 0x0a, 0x0a, 0x0a, 0x0a, 0x06, 0x08,
	0x0b, 0x0c, 0x0b, 0x0a, 0x0c, 0x09, 0x0a, 0x0a, 0x0a, 0xff, 0xdb, 0x00, 0x43, 0x01, 0x02, 0x02,
	0x02, 0x02, 0x02, 0x02, 0x05, 0x03, 0x03, 0x05, 0x0a, 0x07, 0x06, 0x07, 0x0a, 0x0a, 0x0a, 0x0a,
	0x0a, 0x0a, 0x0a, 0x0a, 0x0a, 0x0a, 0x0a, 0x0a, 0x0a, 0x0a, 0x0a, 0x0a, 0x0a, 0x0a, 0x0a, 0x0a,
	0x0a, 0x0a, 0x0a, 0x0a, 0x0a, 0x0a, 0x0a, 0x0a, 0x0a, 0x0a, 0x0a, 0x0a, 0x0a, 0x0a, 0x0a, 0x0a,
	0x0a, 0x0a, 0x0a, 0x0a, 0x0a, 0x0a, 0x0a, 0x0a, 0x0a, 0x0a, 0x0a, 0x0a, 0x0a, 0x0a, 0xff, 0xc0,
	0x00, 0x11, 0x08, 0x00, 0x08, 0x00, 0x10, 0x03, 0x01, 0x11, 0x00, 0x02, 0x11, 0x01, 0x03, 0x11,
	0x01, 0xff, 0xc4, 0x00, 0x14, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x08, 0xff, 0xc4, 0x00, 0x14, 0x10, 0x01, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xff, 0xc4, 0x00,
	0x16, 0x01, 0x01, 0x01, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x0a, 0x08, 0x09, 0xff, 0xc4, 0x00, 0x14, 0x11, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xff, 0xda, 0x00, 0x0c, 0x03,
	0x01, 0x00, 0x02, 0x11, 0x03, 0x11, 0x00, 0x3f, 0x00, 0x17, 0xa5, 0x36, 0xfe, 0x08, 0x65, 0x50,
	0x35, 0xef, 0xff, 0xd9,
};

bool near(int value, int expected, int slack)
{
	return std::abs(value - expected) <= slack;
}

} // namespace

TEST(SwfCharacters_BitReader)
{
	BitWriter w;
	w.ub(5, 3);
	w.sb(-1, 1);
	w.sb(-3, 5);
	w.ub(0x1abc, 13); // crosses two byte boundaries
	w.sb(0x7fff, 16);
	w.sb(-0x8000, 16);
	w.u16(0x1234);
	const std::vector<uint8_t> bytes = w.data();
	swf::BitReader in(bytes.data(), bytes.size());
	CHECK(in.ub(3) == 5);
	CHECK(in.sb(1) == -1);
	CHECK(in.sb(5) == -3);
	CHECK(in.ub(13) == 0x1abc);
	CHECK(in.sb(16) == 0x7fff);
	CHECK(in.sb(16) == -0x8000);
	CHECK(in.ub(0) == 0 && in.sb(0) == 0);
	CHECK(in.u16() == 0x1234); // aligns first
	CHECK(in.ok());
	CHECK(in.ub(1) == 0);
	CHECK(!in.ok()); // overrun

	// MATRIX: scale 1.5/-0.5, rotate 0.25/-0.25, translate (-20, 300);
	// RECT; CXFORMWITHALPHA with mult and add.
	BitWriter m;
	m.ub(1, 1);
	m.ub(20, 5);
	m.sb(0x18000, 20);
	m.sb(-0x8000, 20);
	m.ub(1, 1);
	m.ub(18, 5);
	m.sb(0x4000, 18);
	m.sb(-0x4000, 18);
	m.ub(11, 5);
	m.sb(-20, 11);
	m.sb(300, 11);
	m.align();
	m.ub(12, 5);
	for (int32_t v : {-100, 1000, 0, -2000}) {
		m.sb(v, 12);
	}
	m.align();
	m.ub(1, 1);
	m.ub(1, 1);
	m.ub(11, 4);
	for (int32_t v : {256, 128, 0, 512, -255, 10, 0, 1}) {
		m.sb(v, 11);
	}
	const std::vector<uint8_t> mb = m.data();
	swf::BitReader r(mb.data(), mb.size());
	const swf::Matrix mat = r.matrix();
	CHECK(mat.a == 1.5f && mat.d == -0.5f && mat.b == 0.25f && mat.c == -0.25f);
	CHECK(mat.tx == -20 && mat.ty == 300);
	const swf::Rect rect = r.rect();
	CHECK(rect.xMin == -100 && rect.xMax == 1000 && rect.yMin == 0 && rect.yMax == -2000);
	const swf::ColorTransform cx = r.cxform(true);
	CHECK(cx.mult[0] == 1 && cx.mult[1] == 0.5f && cx.mult[2] == 0 && cx.mult[3] == 2);
	CHECK(cx.add[0] == -255 && cx.add[1] == 10 && cx.add[2] == 0 && cx.add[3] == 1);
	CHECK(r.ok() && r.remaining() == 0);
}

TEST(SwfCharacters_SquareWithHole)
{
	BitWriter w = shapeHeader(5000, 2000);
	moveTo(w, 0, 0, 0, 1, 0);
	lineTo(w, 2000, 0);
	lineTo(w, 0, 2000);
	lineTo(w, -2000, 0);
	lineTo(w, 0, -2000);
	// The hole runs the other way round, the fill still on its right.
	moveTo(w, 500, 500, 0, 1, 0);
	lineTo(w, 0, 1000);
	lineTo(w, 1000, 0);
	lineTo(w, 0, -1000);
	lineTo(w, -1000, 0);
	// An open 1000-twip line, 40 twips wide, no fill.
	moveTo(w, 3000, 0, 0, 0, 1);
	lineTo(w, 1000, 0);
	endShape(w);
	swf::Shape shape;
	std::string error;
	CHECK(swf::parseDefineShape(w.data().data(), w.data().size(), 2, shape, &error));
	CHECK(shape.id == 1 && shape.bounds.xMax == 5000);
	CHECK(shape.fillStyles.size() == 1 && shape.fillStyles[0].color == (swf::Rgba {255, 0, 0, 255}));
	CHECK(shape.paths.size() == 3);
	const swf::ShapeMesh mesh = swf::tessellateShape(shape);
	CHECK(std::fabs(batchArea(mesh, swf::MeshBatch::Kind::Fill) - 3000000.0) < 1.0);
	// Body plus two round caps (polygonal, so slightly under pi r^2).
	const double stroke = batchArea(mesh, swf::MeshBatch::Kind::Stroke);
	const double expected = 1000.0 * 40 + 3.14159265 * 20 * 20;
	CHECK(stroke > expected * 0.98 && stroke <= expected + 1);
	// Fills come before strokes.
	CHECK(mesh.batches.size() == 2 && mesh.batches[0].kind == swf::MeshBatch::Kind::Fill);
	// Non-zero agrees, the hole winding the other way.
	swf::TessellateOptions nz;
	nz.nonZero = true;
	CHECK(std::fabs(batchArea(swf::tessellateShape(shape, nz), swf::MeshBatch::Kind::Fill) - 3000000.0) < 1.0);
}

TEST(SwfCharacters_CurveFlattening)
{
	const swf::Point p0 {0, 0}, p1 {1000, 2000}, p2 {2000, 0};
	for (float tolerance : {0.5f, 2.0f, 20.0f}) {
		std::vector<swf::Point> pts {p0};
		swf::flattenQuadratic(p0, p1, p2, tolerance, pts);
		CHECK(pts.back().x == p2.x && pts.back().y == p2.y);
		double worst = 0;
		for (int i = 0; i <= 1000; ++i) {
			const double t = i / 1000.0, u = 1 - t;
			const double x = u * u * p0.x + 2 * u * t * p1.x + t * t * p2.x;
			const double y = u * u * p0.y + 2 * u * t * p1.y + t * t * p2.y;
			double best = 1e30;
			for (size_t s = 0; s + 1 < pts.size(); ++s) {
				const double ax = pts[s].x, ay = pts[s].y, bx = pts[s + 1].x, by = pts[s + 1].y;
				const double len2 = (bx - ax) * (bx - ax) + (by - ay) * (by - ay);
				const double k = std::fmax(0, std::fmin(1, ((x - ax) * (bx - ax) + (y - ay) * (by - ay)) / len2));
				best = std::fmin(best, std::hypot(x - ax - k * (bx - ax), y - ay - k * (by - ay)));
			}
			worst = std::fmax(worst, best);
		}
		CHECK(worst <= tolerance * 1.01);
	}
	// The region between that parabola and its chord is 2/3 of the control
	// triangle: 4/3 million square twips.
	BitWriter w = shapeHeader(2000, 1000);
	moveTo(w, 0, 0, 1, 0, 0);
	curveTo(w, 1000, 2000, 1000, -2000);
	lineTo(w, -2000, 0);
	endShape(w);
	swf::Shape shape;
	CHECK(swf::parseDefineShape(w.data().data(), w.data().size(), 2, shape));
	const double area = batchArea(swf::tessellateShape(shape), swf::MeshBatch::Kind::Fill);
	CHECK(std::fabs(area - 4000000.0 / 3) < 4000000.0 / 3 * 0.002);
}

TEST(SwfCharacters_MorphHalfway)
{
	// A 1000-square growing to 2000 (the end edges double), solid colour
	// black -> white.
	BitWriter w;
	w.u16(9);
	for (int32_t size : {1000, 2000}) {
		w.ub(16, 5);
		for (int32_t v : {0, size, 0, size}) {
			w.sb(v, 16);
		}
		w.align();
	}
	BitWriter styles;
	styles.u8(1);
	styles.u8(0x00);
	for (uint8_t c : {0, 0, 0, 255, 255, 255, 255, 255}) {
		styles.u8(c);
	}
	styles.u8(0);
	// Start: one fill bit, no line bits, fill0 = 1. End: pen moves only.
	BitWriter start, end;
	for (int32_t s : {1000, 2000}) {
		BitWriter& e = s == 1000 ? start : end;
		e.ub(s == 1000 ? 1 : 0, 4);
		e.ub(0, 4);
		e.ub(0, 1);
		e.ub(s == 1000 ? 0x03 : 0x01, 5);
		e.ub(16, 5);
		e.sb(0, 16);
		e.sb(0, 16);
		if (s == 1000) {
			e.ub(1, 1);
		}
		lineTo(e, 0, s);
		lineTo(e, s, 0);
		lineTo(e, 0, -s);
		lineTo(e, -s, 0);
		endShape(e);
	}
	std::vector<uint8_t> body = w.data();
	const std::vector<uint8_t>& sb = styles.data();
	const uint32_t offset = static_cast<uint32_t>(sb.size() + start.data().size());
	for (int i = 0; i < 4; ++i) {
		body.push_back(static_cast<uint8_t>(offset >> (8 * i)));
	}
	body.insert(body.end(), sb.begin(), sb.end());
	body.insert(body.end(), start.data().begin(), start.data().end());
	body.insert(body.end(), end.data().begin(), end.data().end());
	swf::MorphShape morph;
	std::string error;
	CHECK(swf::parseDefineMorphShape(body.data(), body.size(), morph, &error));
	CHECK(morph.endEdges.size() == 4);
	const swf::Shape half = swf::interpolateMorphShape(morph, 0.5f);
	CHECK(half.fillStyles.size() == 1 && half.fillStyles[0].color.r == 128);
	const double area = batchArea(swf::tessellateShape(half), swf::MeshBatch::Kind::Fill);
	CHECK(std::fabs(area - 1500.0 * 1500) < 1);
}

TEST(SwfCharacters_LosslessPalette)
{
	// 3x2, two colours (transparent, half-transparent orange), rows padded
	// to four bytes.
	const std::vector<uint8_t> pixels {0, 0, 0, 0, 128, 64, 32, 128, 0, 1, 0, 9, 1, 1, 0, 9};
	BitWriter w;
	w.u16(7);
	w.u8(3);
	w.u16(3);
	w.u16(2);
	w.u8(1);
	w.bytes(zlibStored(pixels));
	swf::Bitmap bitmap;
	std::string error;
	CHECK(swf::decodeBitsLossless(w.data().data(), w.data().size(), 2, bitmap, &error));
	CHECK(bitmap.id == 7 && bitmap.width == 3 && bitmap.height == 2 && bitmap.rgba.size() == 24);
	const std::vector<uint8_t> expected {0, 0, 0, 0, 128, 64, 32, 128, 0, 0, 0, 0, 128, 64, 32, 128, 128, 64, 32, 128, 0,
		0, 0, 0};
	CHECK(bitmap.rgba == expected);
	swf::unpremultiply(bitmap);
	CHECK(bitmap.rgba[4] == 255 && bitmap.rgba[5] == 128 && bitmap.rgba[7] == 128);

	// Format 5: ARGB.
	BitWriter argb;
	argb.u16(8);
	argb.u8(5);
	argb.u16(1);
	argb.u16(1);
	argb.bytes(zlibStored({200, 10, 20, 30}));
	CHECK(swf::decodeBitsLossless(argb.data().data(), argb.data().size(), 2, bitmap));
	CHECK((bitmap.rgba == std::vector<uint8_t> {10, 20, 30, 200}));
}

TEST(SwfCharacters_Jpeg)
{
	swf::Bitmap bitmap;
	std::string error;
	CHECK(swf::decodeJpeg(kTinyJpeg.data(), kTinyJpeg.size(), bitmap, &error));
	CHECK(bitmap.width == 16 && bitmap.height == 8 && bitmap.rgba.size() == 16 * 8 * 4);
	const auto pixel = [&](uint32_t x, uint32_t y) { return bitmap.rgba.data() + (y * bitmap.width + x) * 4; };
	CHECK(near(pixel(2, 4)[0], 255, 12) && near(pixel(2, 4)[2], 0, 12) && pixel(2, 4)[3] == 255);
	CHECK(near(pixel(13, 4)[0], 0, 12) && near(pixel(13, 4)[2], 255, 12));

	// DefineBitsJPEG2 with the FF D9 FF D8 lead-in.
	std::vector<uint8_t> body {3, 0, 0xff, 0xd9, 0xff, 0xd8};
	body.insert(body.end(), kTinyJpeg.begin(), kTinyJpeg.end());
	CHECK(swf::decodeDefineBitsJpeg2(body.data(), body.size(), bitmap, &error));
	CHECK(bitmap.id == 3 && bitmap.width == 16);

	// DefineBits + JPEGTables: the quantisation and Huffman tables in their
	// own SOI..EOI stream, the rest in the tag.
	std::vector<uint8_t> tables {0xff, 0xd8}, image {4, 0, 0xff, 0xd8};
	size_t at = 2;
	while (kTinyJpeg[at + 1] != 0xda) {
		const size_t length = 2 + (kTinyJpeg[at + 2] << 8 | kTinyJpeg[at + 3]);
		const bool table = kTinyJpeg[at + 1] == 0xdb || kTinyJpeg[at + 1] == 0xc4;
		(table ? tables : image).insert((table ? tables : image).end(), kTinyJpeg.begin() + at, kTinyJpeg.begin() + at + length);
		at += length;
	}
	tables.insert(tables.end(), {0xff, 0xd9});
	image.insert(image.end(), kTinyJpeg.begin() + at, kTinyJpeg.end());
	CHECK(swf::decodeDefineBits(image.data(), image.size(), tables, bitmap, &error));
	CHECK(bitmap.id == 4 && bitmap.width == 16 && bitmap.height == 8);
	CHECK(near(bitmap.rgba[(4 * 16 + 13) * 4 + 2], 255, 12));

	// DefineBitsJPEG3: alpha 128 everywhere, colour premultiplied.
	std::vector<uint8_t> j3 {5, 0};
	const uint32_t length = static_cast<uint32_t>(kTinyJpeg.size());
	for (int i = 0; i < 4; ++i) {
		j3.push_back(static_cast<uint8_t>(length >> (8 * i)));
	}
	j3.insert(j3.end(), kTinyJpeg.begin(), kTinyJpeg.end());
	const std::vector<uint8_t> alpha = zlibStored(std::vector<uint8_t>(16 * 8, 128));
	j3.insert(j3.end(), alpha.begin(), alpha.end());
	CHECK(swf::decodeDefineBitsJpeg3(j3.data(), j3.size(), bitmap, &error));
	CHECK(bitmap.id == 5 && bitmap.rgba[3] == 128 && near(bitmap.rgba[(4 * 16 + 2) * 4], 128, 8));
}

TEST(SwfCharacters_GameMenus)
{
	const char* gameDir = std::getenv("BF2142_GAME_DIR");
	if (!gameDir) {
		return;
	}
	auto zip = dice::hfe::io::ZipArchive::open(std::string(gameDir) + "/mods/bf2142/Menu_server.zip");
	CHECK(zip != nullptr);
	if (!zip) {
		return;
	}
	for (const char* name : {"External/FlashMenu/mainMenu.dfm", "External/FlashMenu/endOfRound.dfm"}) {
		std::vector<uint8_t> dfm;
		CHECK(zip->read(name, dfm));
		const auto swfData = decodeDfm(dfm);
		CHECK(swfData.has_value());
		if (!swfData) {
			continue;
		}
		const auto movie = parseSwf(*swfData);
		CHECK(movie.has_value());
		if (!movie) {
			continue;
		}
		swf::CharacterDictionary dict;
		std::string error;
		CHECK(dict.load(*swfData, *movie, &error));
		for (const std::string& w : dict.warnings()) {
			std::fprintf(stderr, "  %s: %s\n", name, w.c_str());
		}
		CHECK(dict.warnings().empty());

		size_t shapes = 0, morphs = 0, bitmaps = 0, fonts = 0, glyphs = 0, deviceFonts = 0, texts = 0, buttons = 0,
			   sprites = 0, triangles = 0, emptyShapes = 0;
		double fillArea = 0, boundsArea = 0;
		for (const auto& [id, character] : dict.characters()) {
			if (const auto* shape = std::get_if<swf::Shape>(&character)) {
				++shapes;
				const swf::ShapeMesh mesh = swf::tessellateShape(*shape);
				triangles += mesh.triangleCount();
				emptyShapes += mesh.batches.empty();
				// Each layer's fills are disjoint, so cover at most the bounds.
				const double bounds = double(shape->bounds.xMax - shape->bounds.xMin) * (shape->bounds.yMax - shape->bounds.yMin);
				std::vector<double> perLayer(shape->layerCount, 0);
				for (const swf::MeshBatch& b : mesh.batches) {
					for (const swf::Point& p : b.triangles) {
						CHECK(std::isfinite(p.x) && std::isfinite(p.y));
					}
					if (b.kind == swf::MeshBatch::Kind::Fill) {
						perLayer[b.layer] += swf::triangleArea(b.triangles);
					}
				}
				for (double a : perLayer) {
					CHECK(a <= bounds * 1.001 + 400);
					fillArea += a;
				}
				boundsArea += bounds;
			} else if (const auto* morph = std::get_if<swf::MorphShape>(&character)) {
				++morphs;
				for (float t : {0.0f, 0.5f, 1.0f}) {
					CHECK(swf::tessellateShape(swf::interpolateMorphShape(*morph, t)).triangleCount() > 0);
				}
			} else if (const auto* bitmap = std::get_if<swf::Bitmap>(&character)) {
				++bitmaps;
				CHECK(bitmap->width > 0 && bitmap->height > 0);
				CHECK(bitmap->rgba.size() == size_t(bitmap->width) * bitmap->height * 4);
			} else if (const auto* font = std::get_if<swf::Font>(&character)) {
				++fonts;
				deviceFonts += font->isDeviceFont();
				for (const swf::FontGlyph& g : font->glyphs) {
					++glyphs;
					swf::TessellateOptions em;
					em.curveTolerance = 4; // EM units: 1024 per em
					const swf::ShapeMesh mesh = swf::tessellateShape(g.shape, em);
					CHECK(g.shape.paths.empty() || mesh.triangleCount() > 0);
				}
			} else if (const auto* text = std::get_if<swf::EditText>(&character)) {
				++texts;
				CHECK(!text->hasFont || dict.font(text->fontId) != nullptr);
			} else if (const auto* button = std::get_if<swf::Button>(&character)) {
				++buttons;
				for (const swf::ButtonRecord& r : button->records) {
					CHECK(dict.find(r.characterId) != nullptr);
				}
				for (const swf::ButtonCondAction& a : button->actions) {
					CHECK(a.actionLength > 0 && a.actionOffset + a.actionLength <= swfData->size());
					CHECK((*swfData)[a.actionOffset + a.actionLength - 1] == 0); // ActionEnd
				}
			} else if (std::get_if<swf::Sprite>(&character)) {
				++sprites;
			}
		}
		size_t exportsResolved = 0;
		for (const auto& [exportName, id] : dict.exports()) {
			exportsResolved += dict.find(id) != nullptr;
		}
		CHECK(!dict.exports().empty() && exportsResolved == dict.exports().size());
		CHECK(shapes > 100 && bitmaps > 50 && fonts > 10 && sprites > 300 && texts > 100);
		std::fprintf(stderr,
			"  %s: %zu shapes (%zu triangles, %zu draw nothing, fills cover %.0f%% of bounds), %zu morphs, %zu bitmaps, "
			"%zu fonts (%zu glyphs, %zu device), %zu edit texts, %zu buttons, %zu sprites, %zu exports\n",
			name, shapes, triangles, emptyShapes, 100 * fillArea / std::max(1.0, boundsArea), morphs, bitmaps, fonts, glyphs,
			deviceFonts, texts, buttons, sprites, dict.exports().size());
	}
}
