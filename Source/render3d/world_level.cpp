/**
 * @file render3d/world_level.cpp
 *
 * Static 3D level geometry from the dungeon tile data (see world_level.hpp).
 *
 * Texturing: every piece (dPiece value) is rendered once with the classic tile renderer (fully lit) into a 64 x N
 * image; then
 *  - the floor diamond (bottom 32 rows) is unprojected to a 32 x 32 square,
 *  - the wall halves are unprojected to 32 x H columns: a wall half stands on the cell's centre line, so the wall pixel
 *    of height h in column c of the left half (the SE face, plane x = const) sits at row h + c / 2 above the image
 *    bottom, the right half (SW face, plane y = const) at h + (31 - c) / 2 (tile_geometry.hpp, scrollrt DrawCell).
 *    Only halves the micro types mark as a face (tile_geometry faces) are used; holes and the art's black void are
 *    filled from the same row, then the same column, so faces are solid without vertical streaks.
 *  - town pieces also give roof tile crops and whole-image cards (trees, the well, signs).
 * Pixels the renderer never wrote are transparent (alpha-tested in the shader). Slots are 32-texel aligned, so the
 * box-filtered mips (BuildAtlasMips) never mix two slots.
 */
#include "render3d/world_level.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <span>
#include <string>
#include <unordered_map>

#include "engine/light_tables.hpp"
#include "engine/palette.h"
#include "engine/render/dun_render.hpp"
#include "engine/render/light_render.hpp"
#include "engine/surface.hpp"
#include "levels/dun_tile.hpp"
#include "levels/dun_tile_data.hpp"
#include "levels/gendung.h"
#include "levels/survivors_rolling_runtime.hpp"
#include "levels/tile_geometry.hpp"
#include "levels/trigs.h"
#include "render3d/placement_runtime.hpp"
#include "objects.h"
#include "towners.h"
#include "render3d/fpv_contract.hpp"
#include "utils/log.hpp"
#include "utils/sdl_wrap.h"

namespace devilution {
namespace render3d {

namespace {

bool ForceRebuild = true;
uint64_t LastStateHash = 0;
uint32_t ExternalDoorModels = 0;
// Masonry aperture (width, height in tiles) per external door family (L1, L2, L3, L5); zero keeps the shared contract.
std::array<std::array<float, 2>, 4> ExternalDoorApertures {};

std::array<std::array<uint8_t, 4>, 256> ArtPalette {};
uint64_t ArtPaletteHash = 0;

constexpr int kAtlasWidth = 2048;
constexpr int kSlotWidth = 32;
constexpr int kSlotsPerRow = kAtlasWidth / kSlotWidth;
constexpr int kMaxAtlasHeight = 8192;

uint64_t Mix(uint64_t hash, uint64_t value)
{
	hash ^= value + 0x9E3779B97F4A7C15ULL + (hash << 6) + (hash >> 2);
	return hash;
}

uint64_t HashBytes(uint64_t hash, const void *data, size_t size)
{
	const auto *bytes = static_cast<const uint8_t *>(data);
	size_t i = 0;
	for (; i + 8 <= size; i += 8) {
		uint64_t word;
		std::memcpy(&word, bytes + i, 8);
		hash = (hash ^ word) * 0x100000001B3ULL;
		hash ^= hash >> 29;
	}
	for (; i < size; ++i)
		hash = (hash ^ bytes[i]) * 0x100000001B3ULL;
	return hash;
}

bool IsLeftDoor(_object_id type)
{
	return IsAnyOf(type, OBJ_L1LDOOR, OBJ_L2LDOOR, OBJ_L3LDOOR, OBJ_L5LDOOR);
}

bool ExternalDoorModel(_object_id type)
{
	constexpr _object_id Types[] = { OBJ_L1LDOOR, OBJ_L1RDOOR, OBJ_L2LDOOR, OBJ_L2RDOOR, OBJ_L3LDOOR, OBJ_L3RDOOR, OBJ_L5LDOOR, OBJ_L5RDOOR };
	for (size_t i = 0; i < std::size(Types); ++i)
		if (type == Types[i])
			return (ExternalDoorModels & (1U << i)) != 0;
	return false;
}

// The doorway an external leaf fills: its own width and height when the host sent them, else the shared aperture.
std::array<float, 2> DoorOpening(_object_id type)
{
	constexpr _object_id Types[] = { OBJ_L1LDOOR, OBJ_L1RDOOR, OBJ_L2LDOOR, OBJ_L2RDOOR, OBJ_L3LDOOR, OBJ_L3RDOOR, OBJ_L5LDOOR, OBJ_L5RDOOR };
	for (size_t i = 0; i < std::size(Types); ++i) {
		if (type == Types[i] && ExternalDoorModel(type) && ExternalDoorApertures[i / 2][0] > 0.0F)
			return ExternalDoorApertures[i / 2];
	}
	return { fpv::kDungeonOpeningWidth, fpv::kDungeonOpeningHeight };
}

uint64_t ComputeStateHash()
{
	uint64_t hash = 0xCBF29CE484222325ULL;
	hash = HashBytes(hash, dPiece, sizeof(dPiece));
	hash = HashBytes(hash, dSpecial, sizeof(dSpecial));
	hash = Mix(hash, static_cast<uint64_t>(leveltype));
	hash = Mix(hash, currlevel);
	hash = Mix(hash, setlevel ? 1 : 0);
	hash = Mix(hash, static_cast<uint64_t>(reinterpret_cast<uintptr_t>(pDungeonCels.get())));
	hash = Mix(hash, ArtPaletteHash);
	// Even an identical repeated floor needs a new host revision: the D2R
	// presenter uses world origin to anchor texture coordinates and decoration.
	if (SurvivorsRollingActive()) {
		hash = Mix(hash, static_cast<uint64_t>(SurvivorsRollingOriginX()));
		hash = Mix(hash, static_cast<uint64_t>(SurvivorsRollingOriginY()));
	}
	for (int i = 0; i < ActiveObjectCount; ++i) {
		const Object &object = Objects[ActiveObjects[i]];
		if (!object.isDoor())
			continue;
		hash = Mix(hash, static_cast<uint64_t>(ActiveObjects[i]) << 8 | static_cast<uint64_t>(object._oVar4 & 0xFF));
	}
	return hash;
}

// ---- Piece images -------------------------------------------------------------------------------------------

struct PieceImage {
	bool built = false;
	int height = 0;                 // 32 * (MicroTileLen / 2)
	std::vector<uint8_t> index;     // 64 x height, row 0 = top
	std::vector<uint8_t> opaque;    // 1 where the renderer wrote a pixel
};

std::vector<PieceImage> PieceImages;
std::vector<PieceImage> PieceCardImages; // floor pieces without their ground triangles: only the foliage over them

void RenderPieceInto(const Surface &out, uint16_t piece, int height, bool withoutGround = false)
{
	std::vector<uint8_t> lightmapBuffer(static_cast<size_t>(out.pitch()) * height, 0);
	const Lightmap lightmap(out.at(0, 0), std::span<const uint8_t>(lightmapBuffer), out.pitch(), LightTables, FullyLitLightTable, FullyDarkLightTable);
	const MICROS &micros = DPieceMicros[piece];
	const int count = std::min<int>(MicroTileLen, 16);
	// Like scrollrt: a floor piece's ground micros are re-encoded triangles (DrawFloorTile), plus foliage drawn over them
	// for TransparentSquare ones (DrawCell); every other micro is drawn by its type.
	const bool floorPiece = !HasAnyOf(SOLData[piece], TileProperties::Solid | TileProperties::BlockMissile);
	for (int block = 0; block < count; ++block) {
		const LevelCelBlock levelCelBlock = micros.mt[block];
		if (!levelCelBlock.hasValue())
			continue;
		const Point position { static_cast<int>((block & 1) * DunFrameWidth), static_cast<int>(height - 1 - (block / 2) * DunFrameHeight) };
		if (block < 2 && floorPiece) {
			if (!withoutGround) {
				RenderTileFrame(out, lightmap, position, block == 0 ? TileType::LeftTriangle : TileType::RightTriangle,
				    GetDunFrame(pDungeonCels.get(), levelCelBlock.frame()), DunFrameTriangleHeight, MaskType::Solid, FullyLitLightTable);
			}
			if (levelCelBlock.type() == TileType::TransparentSquare)
				RenderTileFoliage(out, lightmap, position, pDungeonCels.get(), levelCelBlock, FullyLitLightTable);
			continue;
		}
		RenderTile(out, lightmap, position, pDungeonCels.get(), levelCelBlock, MaskType::Solid, FullyLitLightTable);
	}
	FlushTileRenderCommands();
}

/** The piece as the classic renderer draws it; `withoutGround`: a floor piece's ground triangles left out. */
const PieceImage &GetPieceImage(uint16_t piece, bool withoutGround = false)
{
	std::vector<PieceImage> &images = withoutGround ? PieceCardImages : PieceImages;
	if (images.size() < MAXTILES)
		images.resize(MAXTILES);
	PieceImage &image = images[std::min<size_t>(piece, MAXTILES - 1)];
	if (image.built)
		return image;
	image.built = true;
	const int height = std::max(1, static_cast<int>(MicroTileLen) / 2) * DunFrameHeight;
	image.height = height;
	image.index.assign(static_cast<size_t>(64) * height, 0);
	image.opaque.assign(static_cast<size_t>(64) * height, 0);
	if (pDungeonCels == nullptr || piece >= MAXTILES)
		return image;

	// Own buffers with a generous margin around the 64 x height image (the renderer clips to the surface, the margin is
	// only a safety net), wrapped as SDL surfaces.
	constexpr int Margin = 64;
	const int bufferWidth = 64 + 2 * Margin;
	const int bufferHeight = height + 2 * Margin;
	std::vector<uint8_t> bufferA(static_cast<size_t>(bufferWidth) * bufferHeight, 0x00);
	std::vector<uint8_t> bufferB(static_cast<size_t>(bufferWidth) * bufferHeight, 0xFF);
	SDLSurfaceUniquePtr surfaceA = SDLWrap::CreateRGBSurfaceWithFormatFrom(bufferA.data(), bufferWidth, bufferHeight, 8, bufferWidth, SDL_PIXELFORMAT_INDEX8);
	SDLSurfaceUniquePtr surfaceB = SDLWrap::CreateRGBSurfaceWithFormatFrom(bufferB.data(), bufferWidth, bufferHeight, 8, bufferWidth, SDL_PIXELFORMAT_INDEX8);
	const Surface a(surfaceA.get(), SDL_Rect { Margin, Margin, 64, height });
	const Surface b(surfaceB.get(), SDL_Rect { Margin, Margin, 64, height });
	RenderPieceInto(a, piece, height, withoutGround);
	RenderPieceInto(b, piece, height, withoutGround);
	for (int y = 0; y < height; ++y) {
		const uint8_t *rowA = a.at(0, y);
		const uint8_t *rowB = b.at(0, y);
		for (int x = 0; x < 64; ++x) {
			const size_t i = static_cast<size_t>(y) * 64 + x;
			if (rowA[x] == rowB[x]) {
				image.index[i] = rowA[x];
				image.opaque[i] = 1;
			}
		}
	}
	return image;
}

// ---- Atlas slots -------------------------------------------------------------------------------------------

enum class SlotKind : uint8_t { Square, Wall, Card, Art }; // Art: a whole building composite (ArtComposites)

struct Slot {
	SlotKind kind = SlotKind::Square;
	int textureHeight = 32; // wall: content rows (bottom-aligned)
	std::vector<uint8_t> rgba;
};

struct AtlasBuilder {
	int wallSlotHeight = 160;
	std::vector<Slot> squares;
	std::vector<Slot> walls;
	std::vector<Slot> cards; // 64 x wallSlotHeight: a whole piece image (town trees, signs, the well)
	std::unordered_map<uint32_t, int> floorSlotOfPiece;
	std::unordered_map<uint32_t, int> roofSlotOfPiece;
	std::unordered_map<uint32_t, int> cardSlotOfPiece;
	std::unordered_map<uint32_t, int> capSlotOfPiece;
	std::unordered_map<uint32_t, int> wallSlotOfPiece; // piece * 2 + side -> slot or -1
	std::vector<int> wallUse;

	void Reset(int slotHeight)
	{
		wallSlotHeight = slotHeight;
		squares.clear();
		walls.clear();
		cards.clear();
		floorSlotOfPiece.clear();
		roofSlotOfPiece.clear();
		cardSlotOfPiece.clear();
		capSlotOfPiece.clear();
		wallSlotOfPiece.clear();
		wallUse.clear();
	}
};

AtlasBuilder Atlas;

/** Near-black art pixels: the void the iso art leaves around and behind walls (treated as holes on wall faces). */
bool IsVoidColor(uint8_t index)
{
	const auto &c = ArtPalette[index];
	return index == 0 || c[0] + c[1] + c[2] < 24;
}

void PutPixel(std::vector<uint8_t> &rgba, int width, int x, int y, uint8_t index)
{
	const auto &color = ArtPalette[index];
	const size_t o = (static_cast<size_t>(y) * width + x) * 4;
	rgba[o + 0] = color[0];
	rgba[o + 1] = color[1];
	rgba[o + 2] = color[2];
	rgba[o + 3] = 255;
}

/** Unprojects the diamond whose top vertex is at image row `topRow` into a 32 x 32 square (x along +x, y along +z). */
Slot ExtractDiamond(const PieceImage &image, int topRow, bool forceOpaque)
{
	Slot slot;
	slot.kind = SlotKind::Square;
	slot.rgba.assign(32 * 32 * 4, 0);
	uint32_t sum[3] = { 0, 0, 0 };
	uint32_t count = 0;
	for (int v = 0; v < 32; ++v) {
		for (int u = 0; u < 32; ++u) {
			const float fx = (static_cast<float>(u) + 0.5F) / 32.0F;
			const float fz = (static_cast<float>(v) + 0.5F) / 32.0F;
			const int sx = std::clamp(static_cast<int>(32.0F + 32.0F * (fx - fz)), 0, 63);
			const int sy = topRow + std::clamp(static_cast<int>(16.0F * (fx + fz)), 0, 31);
			if (sy < 0 || sy >= image.height)
				continue;
			const size_t i = static_cast<size_t>(sy) * 64 + sx;
			if (image.opaque[i] == 0)
				continue;
			PutPixel(slot.rgba, 32, u, v, image.index[i]);
			const auto &c = ArtPalette[image.index[i]];
			sum[0] += c[0];
			sum[1] += c[1];
			sum[2] += c[2];
			++count;
		}
	}
	if (forceOpaque) {
		const uint8_t fill[3] = {
			static_cast<uint8_t>(count != 0 ? sum[0] / count / 2 : 8),
			static_cast<uint8_t>(count != 0 ? sum[1] / count / 2 : 8),
			static_cast<uint8_t>(count != 0 ? sum[2] / count / 2 : 8),
		};
		for (size_t o = 0; o < slot.rgba.size(); o += 4) {
			if (slot.rgba[o + 3] != 0)
				continue;
			slot.rgba[o + 0] = fill[0];
			slot.rgba[o + 1] = fill[1];
			slot.rgba[o + 2] = fill[2];
			slot.rgba[o + 3] = 255;
		}
	}
	return slot;
}

/**
 * Unprojects one wall half. side 0: the left half (SE face, plane x = const, facing +x); side 1: the right half
 * (SW face, plane y = const, facing +y). Returns false when the half shows no wall.
 */
bool ExtractWall(uint16_t piece, const PieceImage &image, int side, int slotHeight, Slot &slot)
{
	// Only halves the micro types mark as a wall face (tile_geometry: SE for the left half, SW for the right half) carry
	// face art; the others show a wall top, a corner or the dark back of a wall, which unprojected into vertical streaks.
	const uint8_t faceBit = static_cast<uint8_t>(side == 0 ? tile_geometry::Face::SE : tile_geometry::Face::SW);
	if ((tile_geometry::GetPieceGeometry(piece).faces & faceBit) == 0)
		return false;
	std::array<int, 32> tops {};
	int columnsWithWall = 0;
	for (int u = 0; u < 32; ++u) {
		const int c = side == 0 ? u : 32 + u;
		const int foot = side == 0 ? u / 2 : (31 - u) / 2;
		int top = -1;
		int filled = 0;
		for (int h = 0; h + foot < image.height; ++h) {
			const int row = image.height - 1 - (h + foot);
			if (image.opaque[static_cast<size_t>(row) * 64 + c] != 0) {
				top = h;
				++filled;
			}
		}
		tops[u] = top;
		// A face column is mostly covered from its foot to its top.
		if (top >= 16 && filled * 4 >= (top + 1) * 3)
			++columnsWithWall;
	}
	if (columnsWithWall < 16)
		return false;
	std::array<int, 32> sorted = tops;
	std::sort(sorted.begin(), sorted.end());
	const int textureHeight = std::clamp(sorted[20] + 1, 16, slotHeight);
	{
		// Halves the iso art leaves (nearly) black, the unlit backs of walls, would show as black panels.
		uint64_t luma = 0;
		uint32_t count = 0;
		for (int u = 0; u < 32; ++u) {
			const int c = side == 0 ? u : 32 + u;
			const int foot = side == 0 ? u / 2 : (31 - u) / 2;
			for (int h = 0; h < textureHeight; ++h) {
				const int row = image.height - 1 - (h + foot);
				if (row < 0)
					break;
				const size_t i = static_cast<size_t>(row) * 64 + c;
				if (image.opaque[i] == 0)
					continue;
				const auto &color = ArtPalette[image.index[i]];
				luma += color[0] * 3U + color[1] * 6U + color[2];
				++count;
			}
		}
		if (count == 0 || luma / count < 10 * 14)
			return false;
	}
	slot.kind = SlotKind::Wall;
	slot.textureHeight = textureHeight;
	slot.rgba.assign(static_cast<size_t>(32) * slotHeight * 4, 0);
	for (int u = 0; u < 32; ++u) {
		const int c = side == 0 ? u : 32 + u;
		const int foot = side == 0 ? u / 2 : (31 - u) / 2;
		for (int h = 0; h < textureHeight; ++h) {
			const int row = image.height - 1 - (h + foot);
			if (row < 0)
				break;
			const size_t i = static_cast<size_t>(row) * 64 + c;
			if (image.opaque[i] == 0 || IsVoidColor(image.index[i])) // the art's black void is filled below
				continue;
			PutPixel(slot.rgba, 32, u, slotHeight - 1 - h, image.index[i]);
		}
	}
	{
		// Walls are solid: close the holes the iso art leaves (ragged foot and top lines, see-through bits). First
		// from the nearest opaque texel of the same row (a column-wise smear streaks vertically over short columns),
		// then rows without any art from the nearest pixel below, then above, in the same column.
		const auto alphaAt = [&](int u, int row) { return slot.rgba[(static_cast<size_t>(row) * 32 + u) * 4 + 3]; };
		const auto copy = [&](int u, int fromRow, int toRow) {
			std::memcpy(&slot.rgba[(static_cast<size_t>(toRow) * 32 + u) * 4], &slot.rgba[(static_cast<size_t>(fromRow) * 32 + u) * 4], 4);
		};
		const int firstRow = slotHeight - textureHeight;
		for (int row = firstRow; row < slotHeight; ++row) {
			std::array<int, 32> source {};
			bool any = false;
			for (int u = 0; u < 32; ++u) {
				source[u] = -1;
				if (alphaAt(u, row) != 0) {
					source[u] = u;
					any = true;
					continue;
				}
				for (int d = 1; d < 32; ++d) {
					if (u - d >= 0 && alphaAt(u - d, row) != 0) {
						source[u] = u - d;
						break;
					}
					if (u + d < 32 && alphaAt(u + d, row) != 0) {
						source[u] = u + d;
						break;
					}
				}
			}
			if (!any)
				continue;
			uint8_t *line = &slot.rgba[static_cast<size_t>(row) * 32 * 4];
			for (int u = 0; u < 32; ++u) {
				if (source[u] != u && source[u] >= 0)
					std::memcpy(line + u * 4, line + source[u] * 4, 4);
			}
		}
		for (int u = 0; u < 32; ++u) {
			int last = -1;
			for (int row = slotHeight - 1; row >= firstRow; --row) {
				if (alphaAt(u, row) != 0)
					last = row;
				else if (last >= 0)
					copy(u, last, row);
			}
			int next = -1;
			for (int row = firstRow; row < slotHeight; ++row) {
				if (alphaAt(u, row) != 0)
					next = row;
				else if (next >= 0)
					copy(u, next, row);
			}
			if (next < 0 && u > 0) {
				for (int row = firstRow; row < slotHeight; ++row)
					std::memcpy(&slot.rgba[(static_cast<size_t>(row) * 32 + u) * 4], &slot.rgba[(static_cast<size_t>(row) * 32 + u - 1) * 4], 4);
			}
		}
	}
	return true;
}

int FloorSlot(uint16_t piece)
{
	const auto it = Atlas.floorSlotOfPiece.find(piece);
	if (it != Atlas.floorSlotOfPiece.end())
		return it->second;
	const PieceImage &image = GetPieceImage(piece);
	Atlas.squares.push_back(ExtractDiamond(image, image.height - 32, /*forceOpaque=*/true));
	const int slot = static_cast<int>(Atlas.squares.size()) - 1;
	Atlas.floorSlotOfPiece[piece] = slot;
	return slot;
}

/** Wall slot of a piece half, -1 when that half shows no wall. */
int WallSlot(uint16_t piece, int side)
{
	const uint32_t key = static_cast<uint32_t>(piece) * 2 + side;
	const auto it = Atlas.wallSlotOfPiece.find(key);
	if (it != Atlas.wallSlotOfPiece.end())
		return it->second;
	Slot slot;
	int result = -1;
	if (ExtractWall(piece, GetPieceImage(piece), side, Atlas.wallSlotHeight, slot)) {
		Atlas.walls.push_back(std::move(slot));
		result = static_cast<int>(Atlas.walls.size()) - 1;
		Atlas.wallUse.push_back(0);
	}
	Atlas.wallSlotOfPiece[key] = result;
	return result;
}

/** Preferred side first, then the other side. */
int WallSlotEither(uint16_t piece, int preferredSide)
{
	int slot = WallSlot(piece, preferredSide);
	if (slot < 0)
		slot = WallSlot(piece, 1 - preferredSide);
	return slot;
}

/** A 32 x 32 window of a piece image used as roof art. */
struct RoofCrop {
	uint16_t piece = 0;
	int x0 = 16;
	int y0 = 0;
};

/** Opaque art texels (not the black void) in the 32 x 32 window of `image` at (x0, y0). */
int RoofWindowCoverage(const PieceImage &image, int x0, int y0)
{
	int covered = 0;
	for (int v = 0; v < 32; ++v) {
		for (int u = 0; u < 32; ++u) {
			const size_t i = static_cast<size_t>(y0 + v) * 64 + x0 + u;
			if (image.opaque[i] != 0 && !IsVoidColor(image.index[i]))
				++covered;
		}
	}
	return covered;
}

/**
 * The roof art of a house: thatch (or slate) from the top of its tallest columns. Tall town pieces are drawn as one
 * 32 px column (the left or the right half of the 64 px piece, rarely both), with the roof above the facade, so the
 * window must sit inside the drawn column: a window straddling both halves is half empty. Picks the highest window that
 * is (nearly) fully covered, a few rows below its column's outline and ridge, over the floor diamond; the coverage asked
 * for drops in steps when no column is fully covered (thatch with dark gaps or a column a little narrower than 32 px).
 */
RoofCrop FindRoofCrop(const std::vector<uint16_t> &pieces)
{
	constexpr int SkipTop = 4;
	RoofCrop best;
	int bestAbove = -1;
	for (const int minCoverage : { 1000, 940, 860, 760 }) { // of 1024
		for (const uint16_t piece : pieces) {
			const PieceImage &image = GetPieceImage(piece);
			const int lastY0 = image.height - 32 - 32; // the window stays above the floor diamond
			for (int x0 = 0; x0 <= 32; x0 += 4) {
				for (int y0 = 0; y0 <= lastY0; y0 += 2) {
					if (RoofWindowCoverage(image, x0, y0) < minCoverage)
						continue;
					const int y = std::min(lastY0, y0 + SkipTop);
					const int above = image.height - (y + 32);
					if (above > bestAbove && RoofWindowCoverage(image, x0, y) >= minCoverage) {
						bestAbove = above;
						best = { piece, x0, y };
					}
					break;
				}
			}
		}
		if (bestAbove >= 0)
			break;
	}
	if (bestAbove < 0 && !pieces.empty()) {
		// No covered column: the old crop of the first (tallest) piece, holes filled with its average colour.
		const PieceImage &image = GetPieceImage(pieces.front());
		int top = image.height - 32;
		for (int y = 0; y < image.height && top == image.height - 32; ++y) {
			for (int x = 16; x < 48; ++x) {
				if (image.opaque[static_cast<size_t>(y) * 64 + x] != 0) {
					top = y;
					break;
				}
			}
		}
		best = { pieces.front(), 16, std::clamp(top + 10, 0, std::max(0, image.height - 32)) };
	}
	return best;
}

/**
 * Whether a piece's art above its floor diamond is part of a building rather than foliage or a sign: a solid 32 x 32
 * block of wall or roof art, or compact art (eave wedges, roof slabs): most of its texels inside the shape. Foliage is
 * lacy (most of its texels border a hole), and trunks, posts and signs are thin.
 */
bool IsBuildingArt(uint16_t piece)
{
	const PieceImage &image = GetPieceImage(piece);
	const int floorTop = image.height - 32;
	for (int x0 = 0; x0 <= 32; x0 += 4) {
		for (int y0 = 0; y0 + 32 <= floorTop; y0 += 4) {
			if (RoofWindowCoverage(image, x0, y0) >= 900)
				return true;
		}
	}
	const auto opaqueAt = [&](int x, int y) {
		return x >= 0 && x < 64 && y >= 0 && y < floorTop && image.opaque[static_cast<size_t>(y) * 64 + x] != 0;
	};
	int texels = 0;
	int interior = 0;
	for (int y = 0; y < floorTop; ++y) {
		for (int x = 0; x < 64; ++x) {
			if (!opaqueAt(x, y))
				continue;
			++texels;
			if (opaqueAt(x - 1, y) && opaqueAt(x + 1, y) && opaqueAt(x, y - 1) && opaqueAt(x, y + 1))
				++interior;
		}
	}
	return texels >= 48 && interior * 5 >= texels * 3;
}

/** Roof tile art: a 32 x 32 crop of the roof area of a house's tall pieces (FindRoofCrop), opaque. */
int RoofSlot(const RoofCrop &crop)
{
	// Piece images are at most 256 rows tall, so y0 fits in 10 bits; x0 is a multiple of 4 up to 32.
	const uint32_t key = static_cast<uint32_t>(crop.piece) << 16 | static_cast<uint32_t>(crop.x0 / 4) << 10 | static_cast<uint32_t>(crop.y0);
	const auto it = Atlas.roofSlotOfPiece.find(key);
	if (it != Atlas.roofSlotOfPiece.end())
		return it->second;
	const PieceImage &image = GetPieceImage(crop.piece);
	const int y0 = crop.y0;
	Slot slot;
	slot.kind = SlotKind::Square;
	slot.rgba.assign(32 * 32 * 4, 0);
	uint32_t sum[3] = { 0, 0, 0 };
	uint32_t count = 0;
	for (int v = 0; v < 32; ++v) {
		for (int u = 0; u < 32; ++u) {
			const size_t i = static_cast<size_t>(y0 + v) * 64 + crop.x0 + u;
			if (image.opaque[i] == 0 || IsVoidColor(image.index[i]))
				continue;
			PutPixel(slot.rgba, 32, u, v, image.index[i]);
			const auto &c = ArtPalette[image.index[i]];
			sum[0] += c[0];
			sum[1] += c[1];
			sum[2] += c[2];
			++count;
		}
	}
	for (size_t o = 0; o < slot.rgba.size(); o += 4) {
		if (slot.rgba[o + 3] != 0)
			continue;
		for (int k = 0; k < 3; ++k)
			slot.rgba[o + k] = static_cast<uint8_t>(count != 0 ? sum[k] / count : 24);
		slot.rgba[o + 3] = 255;
	}
	Atlas.squares.push_back(std::move(slot));
	const int result = static_cast<int>(Atlas.squares.size()) - 1;
	Atlas.roofSlotOfPiece[key] = result;
	return result;
}

// A card stands on the cell centre, which the iso art puts on the centre line of the piece's floor diamond, 16 rows above
// the image bottom. The diamond (the bottom 32 rows) is ground art: cards show it only where a trunk or post runs down
// through it (PlaceCards), elsewhere only the foliage a floor piece draws over its ground.
constexpr int kCardGround = 16;
constexpr int kCardFloorTop = 32;

/**
 * Card slot: the piece image, bottom-aligned in a 64 x slot-height slot. Without ground a floor piece leaves its ground
 * triangles out; `withGround` keeps them (a trunk's foot is often painted into the ground under it).
 */
int CardSlot(uint16_t piece, int &contentRows, bool withGround = false)
{
	const auto it = Atlas.cardSlotOfPiece.find(static_cast<uint32_t>(piece) * 2 + (withGround ? 1 : 0));
	const PieceImage &image = GetPieceImage(piece, /*withoutGround=*/!withGround);
	contentRows = std::min(Atlas.wallSlotHeight, image.height);
	if (it != Atlas.cardSlotOfPiece.end())
		return it->second;
	const int slotHeight = Atlas.wallSlotHeight;
	Slot slot;
	slot.kind = SlotKind::Card;
	slot.textureHeight = contentRows;
	slot.rgba.assign(static_cast<size_t>(64) * slotHeight * 4, 0);
	for (int r = 0; r < contentRows; ++r) {
		const int row = image.height - 1 - r;
		if (row < 0)
			break;
		for (int x = 0; x < 64; ++x) {
			const size_t i = static_cast<size_t>(row) * 64 + x;
			if (image.opaque[i] != 0)
				PutPixel(slot.rgba, 64, x, slotHeight - 1 - r, image.index[i]);
		}
	}
	Atlas.cards.push_back(std::move(slot));
	const int result = static_cast<int>(Atlas.cards.size()) - 1;
	Atlas.cardSlotOfPiece[static_cast<uint32_t>(piece) * 2 + (withGround ? 1 : 0)] = result;
	return result;
}

// ---- Mesh building -----------------------------------------------------------------------------------------

struct PendingVertex {
	WorldVertex vertex;
	SlotKind kind;
	int slot; // -1: default wall slot (resolved at the end)
	float s, t; // texels inside the slot (t = 0 at the slot's top row)
};

struct MeshBuilder {
	std::vector<PendingVertex> vertices;
	std::vector<uint32_t> indices;

	void Quad(const Vec3 &p0, const Vec3 &p1, const Vec3 &p2, const Vec3 &p3,
	    float s0, float t0, float s1, float t1, const Vec3 &normal, SlotKind kind, int slot, uint32_t flags)
	{
		// p0 -> (s0, t0), p1 -> (s1, t0), p2 -> (s1, t1), p3 -> (s0, t1)
		const auto base = static_cast<uint32_t>(vertices.size());
		const Vec3 ps[4] = { p0, p1, p2, p3 };
		const float ss[4] = { s0, s1, s1, s0 };
		const float ts[4] = { t0, t0, t1, t1 };
		for (int i = 0; i < 4; ++i) {
			PendingVertex v;
			v.vertex = { ps[i].x, ps[i].y, ps[i].z, 0.0F, 0.0F, normal.x, normal.y, normal.z, flags };
			v.kind = kind;
			v.slot = slot;
			v.s = ss[i];
			v.t = ts[i];
			vertices.push_back(v);
		}
		indices.insert(indices.end(), { base, base + 1, base + 2, base, base + 2, base + 3 });
	}

	void Triangle(const Vec3 &p0, const Vec3 &p1, const Vec3 &p2, float s0, float t0, float s1, float t1, float s2, float t2,
	    const Vec3 &normal, SlotKind kind, int slot, uint32_t flags)
	{
		const auto base = static_cast<uint32_t>(vertices.size());
		const Vec3 ps[3] = { p0, p1, p2 };
		const float ss[3] = { s0, s1, s2 };
		const float ts[3] = { t0, t1, t2 };
		for (int i = 0; i < 3; ++i) {
			PendingVertex v;
			v.vertex = { ps[i].x, ps[i].y, ps[i].z, 0.0F, 0.0F, normal.x, normal.y, normal.z, flags };
			v.kind = kind;
			v.slot = slot;
			v.s = ss[i];
			v.t = ts[i];
			vertices.push_back(v);
		}
		indices.insert(indices.end(), { base, base + 1, base + 2 });
	}
};

constexpr float kInset = 0.02F; // texels: keeps textureLoad inside the slot at face edges

/** Horizontal square over [x0, x1] x [z0, z1] at height y, textured with a square slot (texel x along +x, y along +z). */
void AddHorizontal(MeshBuilder &mesh, float x0, float z0, float x1, float z1, float y, bool up, int slot, float albedo, uint32_t extraFlags = 0)
{
	const Vec3 normal { 0.0F, up ? 1.0F : -1.0F, 0.0F };
	const float s0 = kInset + 32.0F * (x0 - std::floor(x0));
	const float t0 = kInset + 32.0F * (z0 - std::floor(z0));
	const float s1 = std::max(s0, 32.0F * (x1 - std::floor(x0)) - kInset);
	const float t1 = std::max(t0, 32.0F * (z1 - std::floor(z0)) - kInset);
	mesh.Quad({ x0, y, z0 }, { x1, y, z0 }, { x1, y, z1 }, { x0, y, z1 }, s0, t0, s1, t1, normal, SlotKind::Square, slot,
	    PackVertexFlags(extraFlags, albedo));
}

/**
 * Vertical face on the plane `axis` (0: x = c, spans z in [a0, a1]; 1: z = c, spans x in [a0, a1]) from y0 to y1.
 * The wall slot's texture height maps to [0, fullHeight] so partial faces (lintels) show the matching band.
 * u runs 0..32 from a1 to a0 on x planes (column 0 of the left half is the +y end) and a0 to a1 on z planes.
 */
void AddWallFace(MeshBuilder &mesh, int axis, float c, float a0, float a1, float y0, float y1, float fullHeight, const Vec3 &normal,
    int slot, int slotHeight, float albedo, float u0 = 0.0F, float u1 = 32.0F)
{
	const auto tOf = [&](float y, int textureHeight) {
		return static_cast<float>(slotHeight) - static_cast<float>(textureHeight) * std::clamp(y / fullHeight, 0.0F, 1.0F);
	};
	const int textureHeight = slot >= 0 && slot < static_cast<int>(Atlas.walls.size()) ? Atlas.walls[slot].textureHeight : slotHeight;
	const float tTop = std::clamp(tOf(y1, textureHeight) + kInset, 0.0F, static_cast<float>(slotHeight));
	const float tBottom = std::clamp(tOf(y0, textureHeight) - kInset, 0.0F, static_cast<float>(slotHeight));
	const float sa = std::clamp(u0, kInset, 32.0F - kInset);
	const float sb = std::clamp(u1, kInset, 32.0F - kInset);
	const uint32_t flags = PackVertexFlags(0, albedo);
	if (axis == 0) {
		// p0 top at a1 (s = sa), p1 top at a0 (s = sb)
		mesh.Quad({ c, y1, a1 }, { c, y1, a0 }, { c, y0, a0 }, { c, y0, a1 }, sa, tTop, sb, tBottom, normal, SlotKind::Wall, slot, flags);
	} else {
		mesh.Quad({ a0, y1, c }, { a1, y1, c }, { a1, y0, c }, { a0, y0, c }, sa, tTop, sb, tBottom, normal, SlotKind::Wall, slot, flags);
	}
}

/** Axis-aligned box with wall-textured sides (x faces use slotX, z faces slotZ) and an optional square-textured top. */
void AddBox(MeshBuilder &mesh, Vec3 lo, Vec3 hi, float fullHeight, int slotX, int slotZ, int topSlot, int slotHeight, float albedo)
{
	AddWallFace(mesh, 0, hi.x, lo.z, hi.z, lo.y, hi.y, fullHeight, { 1, 0, 0 }, slotX, slotHeight, albedo);
	AddWallFace(mesh, 0, lo.x, lo.z, hi.z, lo.y, hi.y, fullHeight, { -1, 0, 0 }, slotX, slotHeight, albedo);
	AddWallFace(mesh, 1, hi.z, lo.x, hi.x, lo.y, hi.y, fullHeight, { 0, 0, 1 }, slotZ, slotHeight, albedo);
	AddWallFace(mesh, 1, lo.z, lo.x, hi.x, lo.y, hi.y, fullHeight, { 0, 0, -1 }, slotZ, slotHeight, albedo);
	if (topSlot >= 0)
		AddHorizontal(mesh, lo.x, lo.z, hi.x, hi.z, hi.y, true, topSlot, albedo);
}

constexpr int kStairSteps = 4;
constexpr float kStairRiseUp = 0.1F;
constexpr float kStairRiseDown = -0.12F;
constexpr std::pair<int, int> kStairDirs[4] = { { 1, 0 }, { -1, 0 }, { 0, 1 }, { 0, -1 } };

/**
 * Stairs on a trigger cell: steps rising (up) or a pit with steps going down (down), climbing toward the solid
 * neighbour the stairs art leans against. Treads alternate in brightness and the risers are darker, so the steps read.
 */
template <typename SolidFn>
void AddStairs(LevelGeometry &geometry, MeshBuilder &mesh, int x, int y, bool up, const SolidFn &solid, int slot)
{
	int dirIndex = 3;
	for (const int candidate : { 3, 1, 0, 2 }) {
		if (solid(x + kStairDirs[candidate].first, y + kStairDirs[candidate].second)) {
			dirIndex = candidate;
			break;
		}
	}
	const int dx = kStairDirs[dirIndex].first;
	const int dy = kStairDirs[dirIndex].second;
	geometry.stairs[static_cast<size_t>(y) * kGridSize + x] = up ? 1 : -1;
	geometry.stairsDir[static_cast<size_t>(y) * kGridSize + x] = static_cast<uint8_t>(dirIndex);
	const float fx = static_cast<float>(x);
	const float fz = static_cast<float>(y);
	const float rise = up ? kStairRiseUp : kStairRiseDown;
	// The band [a, b] of the cell measured toward the solid neighbour.
	const auto band = [&](float a, float b, float &x0, float &z0, float &x1, float &z1) {
		x0 = fx, x1 = fx + 1.0F, z0 = fz, z1 = fz + 1.0F;
		if (dx > 0) { x0 = fx + a; x1 = fx + b; }
		if (dx < 0) { x0 = fx + 1.0F - b; x1 = fx + 1.0F - a; }
		if (dy > 0) { z0 = fz + a; z1 = fz + b; }
		if (dy < 0) { z0 = fz + 1.0F - b; z1 = fz + 1.0F - a; }
	};
	for (int i = 0; i < kStairSteps; ++i) {
		const float a = static_cast<float>(i) / kStairSteps;
		const float b = static_cast<float>(i + 1) / kStairSteps;
		const float h = rise * static_cast<float>(i + 1);
		float x0, z0, x1, z1;
		band(a, b, x0, z0, x1, z1);
		const float albedo = (up ? 1.05F : 0.95F - 0.1F * static_cast<float>(i)) * ((i & 1) != 0 ? 0.85F : 1.0F);
		AddHorizontal(mesh, x0, z0, x1, z1, h, true, slot, albedo);
		// Riser at the front edge of this tread (from the previous tread height).
		const float h0 = rise * static_cast<float>(i);
		const float lo = std::min(h0, h);
		const float hi = std::max(h0, h);
		const float riserAlbedo = up ? 0.6F : 0.45F;
		if (dx != 0) {
			const float c = dx > 0 ? fx + a : fx + 1.0F - a;
			const Vec3 n { static_cast<float>(up ? -dx : dx), 0, 0 };
			AddWallFace(mesh, 0, c, fz, fz + 1.0F, lo, hi, 1.0F, n, -1, Atlas.wallSlotHeight, riserAlbedo);
		} else {
			const float c = dy > 0 ? fz + a : fz + 1.0F - a;
			const Vec3 n { 0, 0, static_cast<float>(up ? -dy : dy) };
			AddWallFace(mesh, 1, c, fx, fx + 1.0F, lo, hi, 1.0F, n, -1, Atlas.wallSlotHeight, riserAlbedo);
		}
		if (up) {
			// Step sides down to the floor (seen from the open neighbours).
			const float sideLo = 0.0F;
			if (dx != 0) {
				AddWallFace(mesh, 1, fz, x0, x1, sideLo, h, 1.0F, { 0, 0, -1 }, -1, Atlas.wallSlotHeight, 0.55F);
				AddWallFace(mesh, 1, fz + 1.0F, x0, x1, sideLo, h, 1.0F, { 0, 0, 1 }, -1, Atlas.wallSlotHeight, 0.55F);
			} else {
				AddWallFace(mesh, 0, fx, z0, z1, sideLo, h, 1.0F, { -1, 0, 0 }, -1, Atlas.wallSlotHeight, 0.55F);
				AddWallFace(mesh, 0, fx + 1.0F, z0, z1, sideLo, h, 1.0F, { 1, 0, 0 }, -1, Atlas.wallSlotHeight, 0.55F);
			}
		}
	}
	if (!up) {
		// Pit walls from the floor down to the deepest tread.
		const float depth = rise * kStairSteps;
		AddWallFace(mesh, 0, fx, fz, fz + 1.0F, depth, 0.0F, 1.0F, { 1, 0, 0 }, -1, Atlas.wallSlotHeight, 0.35F);
		AddWallFace(mesh, 0, fx + 1.0F, fz, fz + 1.0F, depth, 0.0F, 1.0F, { -1, 0, 0 }, -1, Atlas.wallSlotHeight, 0.35F);
		AddWallFace(mesh, 1, fz, fx, fx + 1.0F, depth, 0.0F, 1.0F, { 0, 0, 1 }, -1, Atlas.wallSlotHeight, 0.35F);
		AddWallFace(mesh, 1, fz + 1.0F, fx, fx + 1.0F, depth, 0.0F, 1.0F, { 0, 0, -1 }, -1, Atlas.wallSlotHeight, 0.35F);
	}
}

struct DoorInfo {
	bool present = false;
	bool open = false;
	bool left = true;
};

std::unordered_map<int, int> DoorClosedSlot; // object index -> wall slot captured while closed (per level build)

void BuildDungeon(LevelGeometry &geometry, MeshBuilder &mesh)
{
	const float wallHeight = fpv::kWallHeight;
	const int slotHeight = Atlas.wallSlotHeight;
	std::vector<DoorInfo> doors(static_cast<size_t>(kGridSize) * kGridSize);
	std::vector<int> doorObject(static_cast<size_t>(kGridSize) * kGridSize, -1);
	for (int i = 0; i < ActiveObjectCount; ++i) {
		const Object &object = Objects[ActiveObjects[i]];
		if (!object.isDoor() || !InDungeonBounds(object.position))
			continue;
		const size_t c = static_cast<size_t>(object.position.y) * kGridSize + object.position.x;
		doors[c] = { true, object._oVar4 != 0, IsLeftDoor(object._otype) };
		doorObject[c] = ActiveObjects[i];
	}
	const auto door = [&](int x, int y) -> const DoorInfo & { return doors[static_cast<size_t>(y) * kGridSize + x]; };
	const auto solid = [&](int x, int y) {
		if (x < 0 || y < 0 || x >= kGridSize || y >= kGridSize)
			return true;
		return TileHasAny({ x, y }, TileProperties::Solid) && !door(x, y).present;
	};
	const auto pillar = [&](int x, int y) {
		return solid(x, y) && !solid(x - 1, y) && !solid(x + 1, y) && !solid(x, y - 1) && !solid(x, y + 1);
	};

	// Stairs: +1 up (WM_DIABPREVLVL), -1 down (WM_DIABNEXTLVL) on the trigger cells.
	std::vector<int8_t> stairsAt(static_cast<size_t>(kGridSize) * kGridSize, 0);
	for (int i = 0; i < numtrigs; ++i) {
		const TriggerStruct &trigger = trigs[i];
		if (!InDungeonBounds(trigger.position))
			continue;
		if (trigger._tmsg == WM_DIABPREVLVL || trigger._tmsg == WM_DIABNEXTLVL) {
			stairsAt[static_cast<size_t>(trigger.position.y) * kGridSize + trigger.position.x] = trigger._tmsg == WM_DIABPREVLVL ? 1 : -1;
			Log("render3d: stairs {} at {},{}", trigger._tmsg == WM_DIABPREVLVL ? "up" : "down", trigger.position.x, trigger.position.y);
		}
	}

	for (int y = 0; y < kGridSize; ++y) {
		for (int x = 0; x < kGridSize; ++x) {
			const uint16_t piece = dPiece[x][y];
			uint32_t &occluder = geometry.occluders[static_cast<size_t>(y) * kGridSize + x];
			occluder = 0;
			if (PlacementHidesTile(x, y)) {
				// A placed mesh replaces this cell above the ground (render3d/placement_runtime.hpp); the floor stays.
				if (!solid(x, y) && stairsAt[static_cast<size_t>(y) * kGridSize + x] == 0)
					AddHorizontal(mesh, static_cast<float>(x), static_cast<float>(y), static_cast<float>(x) + 1.0F, static_cast<float>(y) + 1.0F, 0.0F, true,
					    FloorSlot(piece), 1.0F);
				else if (!solid(x, y))
					AddStairs(geometry, mesh, x, y, stairsAt[static_cast<size_t>(y) * kGridSize + x] > 0, solid, FloorSlot(piece));
				continue;
			}
			if (solid(x, y)) {
				occluder = PackOccluder(0.0F, wallHeight);
				if (pillar(x, y)) {
					const int slotX = WallSlotEither(piece, 0);
					const int slotZ = WallSlotEither(piece, 1);
					const float fx = static_cast<float>(x);
					const float fz = static_cast<float>(y);
					AddHorizontal(mesh, fx, fz, fx + 1.0F, fz + 1.0F, 0.0F, true, FloorSlot(piece), 1.0F);
					AddHorizontal(mesh, fx, fz, fx + 1.0F, fz + 1.0F, wallHeight, false, FloorSlot(piece), 0.45F, kVertexSoft);
					AddBox(mesh, { fx + 0.3F, 0.0F, fz + 0.3F }, { fx + 0.7F, wallHeight, fz + 0.7F }, wallHeight, slotX, slotZ, -1, slotHeight, 1.0F);
				}
				continue;
			}
			const float fx = static_cast<float>(x);
			const float fz = static_cast<float>(y);
			const int floorSlot = FloorSlot(piece);
			const uint8_t archFacing = tile_geometry::ArchFacing(leveltype, dSpecial[x][y]);
			const DoorInfo &doorInfo = door(x, y);
			const bool passage = archFacing != 0 || doorInfo.present;
			const bool alongY = doorInfo.present ? doorInfo.left : archFacing == static_cast<uint8_t>(tile_geometry::Face::SE);
			// An external door leaf keeps the shared model scale; the masonry aperture fits that leaf instead.
			const std::array<float, 2> opening = doorInfo.present
			    ? DoorOpening(Objects[doorObject[static_cast<size_t>(y) * kGridSize + x]]._otype)
			    : std::array<float, 2> { fpv::kDungeonOpeningWidth, fpv::kDungeonOpeningHeight };
			const float PassagePadding = (opening[0] - 1.0F) * 0.5F;
			const int stairs = stairsAt[static_cast<size_t>(y) * kGridSize + x];
			if (stairs == 0) {
				if (!passage)
					AddHorizontal(mesh, fx, fz, fx + 1.0F, fz + 1.0F, 0.0F, true, floorSlot, 1.0F);
			} else
				AddStairs(geometry, mesh, x, y, stairs > 0, solid, floorSlot);
			if (passage && stairs == 0)
				AddHorizontal(mesh, fx - (alongY ? 0.0F : PassagePadding), fz - (alongY ? PassagePadding : 0.0F),
				    fx + 1.0F + (alongY ? 0.0F : PassagePadding), fz + 1.0F + (alongY ? PassagePadding : 0.0F), 0.0F, true, floorSlot, 1.0F);
			AddHorizontal(mesh, fx, fz, fx + 1.0F, fz + 1.0F, wallHeight, false, floorSlot, 0.45F, kVertexSoft);

			// Walls of the solid (non-pillar) neighbours facing this cell.
			struct Side {
				int dx, dy;
				int axis;   // 0: x plane, 1: z plane
				float c;    // plane coordinate
				Vec3 normal; // pointing into this cell
				int artSide; // 0 left half (x planes), 1 right half (z planes)
			};
			const Side sides[4] = {
				{ 1, 0, 0, fx + 1.0F, { -1, 0, 0 }, 0 },
				{ -1, 0, 0, fx, { 1, 0, 0 }, 0 },
				{ 0, 1, 1, fz + 1.0F, { 0, 0, -1 }, 1 },
				{ 0, -1, 1, fz, { 0, 0, 1 }, 1 },
			};
			for (const Side &side : sides) {
				const int nx = x + side.dx;
				const int ny = y + side.dy;
				if (!solid(nx, ny) || pillar(nx, ny) || PlacementHidesTile(nx, ny))
					continue;
				int slot = -1;
				if (nx >= 0 && ny >= 0 && nx < kGridSize && ny < kGridSize)
					slot = WallSlotEither(dPiece[nx][ny], side.artSide);
				if (slot >= 0)
					++Atlas.wallUse[slot];
				const float a0 = side.axis == 0 ? fz : fx;
				// Move the jamb face into its solid neighbour rather than merely scaling
				// a frame over a one-tile masonry hole. Native collision stays tile based.
				const bool jamb = passage && side.axis == (alongY ? 1 : 0);
				const float plane = side.c - (jamb ? PassagePadding * (side.axis == 0 ? side.normal.x : side.normal.z) : 0.0F);
				AddWallFace(mesh, side.axis, plane, a0, a0 + 1.0F, 0.0F, wallHeight, wallHeight, side.normal, slot, slotHeight, 1.0F);
				if (jamb) {
					// Short reveals join the widened jamb to the unchanged wall faces
					// on both sides of this one-tile-deep opening.
					const float lo = std::min(side.c, plane);
					const float hi = std::max(side.c, plane);
					AddWallFace(mesh, 1 - side.axis, a0, lo, hi, 0.0F, wallHeight, wallHeight,
					    alongY ? Vec3 { -1, 0, 0 } : Vec3 { 0, 0, -1 }, slot, slotHeight, 1.0F);
					AddWallFace(mesh, 1 - side.axis, a0 + 1.0F, lo, hi, 0.0F, wallHeight, wallHeight,
					    alongY ? Vec3 { 1, 0, 0 } : Vec3 { 0, 0, 1 }, slot, slotHeight, 1.0F);
				}
			}

			// Open arches and native door panels share the hero-sized masonry aperture; external leaves bring their own.
			if (passage) {
				const float DoorTop = opening[1];
				const float lintelBottom = opening[1];
				// The wall the doorway is cut into runs along y (plane x = const) for SE arches and left doors.
				const int wallSide = alongY ? 0 : 1;
				int slot = -1;
				for (const auto &[dx, dy] : { std::pair { 0, -1 }, std::pair { 0, 1 }, std::pair { -1, 0 }, std::pair { 1, 0 } }) {
					if (slot >= 0)
						break;
					const int nx = x + dx;
					const int ny = y + dy;
					if (nx >= 0 && ny >= 0 && nx < kGridSize && ny < kGridSize && solid(nx, ny))
						slot = WallSlotEither(dPiece[nx][ny], wallSide);
				}
				// An arch rule with hide = self replaces the lintel's look; its shadow band stays (render3d/placement_runtime.hpp).
				// A door rule (show = closed, hide = self) also replaces the lintel while the door is closed: the door mesh has
				// its own frame.
				const bool doorReplaced = doorInfo.present && !doorInfo.open && PlacementHidesObject(doorObject[static_cast<size_t>(y) * kGridSize + x]);
				if (!PlacementHidesLintel(x, y) && !doorReplaced)
					AddBox(mesh, { fx - (alongY ? 0.0F : PassagePadding), lintelBottom, fz - (alongY ? PassagePadding : 0.0F) },
					    { fx + 1.0F + (alongY ? 0.0F : PassagePadding), wallHeight, fz + 1.0F + (alongY ? PassagePadding : 0.0F) }, wallHeight, slot, slot, -1, slotHeight, 0.9F);
				occluder = PackOccluder(lintelBottom, wallHeight);

				if (doorInfo.present) {
					const int objectIndex = doorObject[static_cast<size_t>(y) * kGridSize + x];
					int doorSlot = -1;
					if (!doorInfo.open) {
						doorSlot = WallSlotEither(piece, wallSide);
						DoorClosedSlot[objectIndex] = doorSlot;
					} else if (const auto it = DoorClosedSlot.find(objectIndex); it != DoorClosedSlot.end()) {
						doorSlot = it->second;
					}
					if (doorSlot < 0)
						doorSlot = slot;
					const float panelTop = DoorTop;
					const bool externalPanel = ExternalDoorModel(Objects[objectIndex]._otype);
					if (!doorInfo.open) {
						occluder = PackOccluder(0.0F, wallHeight);
						if (externalPanel || PlacementHidesObject(objectIndex)) {
							// A door rule (show = closed, hide = self) draws this closed door instead.
						} else if (alongY)
							AddBox(mesh, { fx + 0.44F, 0.0F, fz - PassagePadding }, { fx + 0.56F, panelTop, fz + 1.0F + PassagePadding }, panelTop, doorSlot, doorSlot, -1, slotHeight, 1.0F);
						else
							AddBox(mesh, { fx - PassagePadding, 0.0F, fz + 0.44F }, { fx + 1.0F + PassagePadding, panelTop, fz + 0.56F }, panelTop, doorSlot, doorSlot, -1, slotHeight, 1.0F);
					} else if (!externalPanel) {
						// Swung open against the jamb.
						if (alongY)
							AddBox(mesh, { fx + 0.5F, 0.0F, fz - PassagePadding + 0.02F }, { fx + 0.5F + opening[0], panelTop, fz - PassagePadding + 0.12F }, panelTop, doorSlot, doorSlot, -1, slotHeight, 0.9F);
						else
							AddBox(mesh, { fx - PassagePadding + 0.02F, 0.0F, fz + 0.5F }, { fx - PassagePadding + 0.12F, panelTop, fz + 0.5F + opening[0] }, panelTop, doorSlot, doorSlot, -1, slotHeight, 0.9F);
					}
				}
			}
		}
	}
}

struct PieceStats {
	int heightPx = 0;   // topmost opaque row above the image bottom
	float density = 0;  // opaque fraction of the image above the floor diamond, from the topmost opaque row down
	float r = 0, g = 0, b = 0; // mean colour of the opaque pixels
	int opaqueAbove = 0;
};

PieceStats GetPieceStats(uint16_t piece)
{
	PieceStats stats;
	const PieceImage &image = GetPieceImage(piece);
	int top = image.height;
	int count = 0;
	float r = 0, g = 0, b = 0;
	for (int y = 0; y < image.height; ++y) {
		for (int x = 0; x < 64; ++x) {
			const size_t i = static_cast<size_t>(y) * 64 + x;
			if (image.opaque[i] == 0)
				continue;
			top = std::min(top, y);
			const auto &c = ArtPalette[image.index[i]];
			r += c[0];
			g += c[1];
			b += c[2];
			++count;
			if (y < image.height - 32)
				++stats.opaqueAbove;
		}
	}
	stats.heightPx = image.height - top;
	if (count > 0) {
		stats.r = r / count;
		stats.g = g / count;
		stats.b = b / count;
	}
	const int rowsAbove = image.height - 32 - top;
	stats.density = rowsAbove > 0 ? static_cast<float>(stats.opaqueAbove) / static_cast<float>(rowsAbove * 64) : 0.0F;
	return stats;
}

std::unordered_map<uint32_t, PieceStats> PieceStatsCache; // cleared per town build

const PieceStats &CachedPieceStats(uint16_t piece)
{
	auto it = PieceStatsCache.find(piece);
	if (it == PieceStatsCache.end())
		it = PieceStatsCache.emplace(piece, GetPieceStats(piece)).first;
	return it->second;
}

void DumpTownCells(const char *path)
{
	FILE *file = std::fopen(path, "w");
	if (file == nullptr)
		return;
	std::fprintf(file, "x,y,piece,solid,heightPx,density,r,g,b,faces,blockMissile\n");
	for (int y = 0; y < kGridSize; ++y) {
		for (int x = 0; x < kGridSize; ++x) {
			const uint16_t piece = dPiece[x][y];
			const PieceStats s = GetPieceStats(piece);
			std::fprintf(file, "%d,%d,%d,%d,%d,%.3f,%.0f,%.0f,%.0f,%d,%d\n", x, y, piece, TileHasAny({ x, y }, TileProperties::Solid) ? 1 : 0, s.heightPx,
			    s.density, s.r, s.g, s.b, tile_geometry::GetPieceGeometry(piece).faces, TileHasAny({ x, y }, TileProperties::BlockMissile) ? 1 : 0);
		}
	}
	std::fclose(file);
}

// ---- Tristram ------------------------------------------------------------------------------------------------
//
// Town art is drawn by a few tall pieces per object: a house's facades and roof live in the pieces along its front
// (+x / +y) edge, the cells behind are flat solid ground, and a tree crown spills into the pieces around its trunk.
// So the town is rebuilt per object instead of per cell:
//  - houses: compact groups of solid cells with tall facade pieces become walls on the group outline (facade art of
//    the edge pieces, back walls borrow it) with a gable roof textured with the roof tile art of the tallest piece;
//    the largest group (the cathedral) is taller and gets a bell tower;
//  - the creek: long thin chains of flat solid cells become a water plane below the ground;
//  - every other tall piece (trees, the well, signs, fences) becomes a pair of crossed upright cards carrying the
//    piece image itself.

constexpr float kWaterLevel = 0.0F; // the creek is flat: its art carries the channel and the banks

struct TownGroup {
	std::vector<std::pair<int, int>> cells;
	int minX = 0, maxX = 0, minY = 0, maxY = 0;
};

/** 4-connected groups of the cells where `member` holds. */
template <typename Fn>
std::vector<TownGroup> FindGroups(const Fn &member, std::vector<int> &groupOf)
{
	std::vector<TownGroup> groups;
	groupOf.assign(static_cast<size_t>(kGridSize) * kGridSize, -1);
	std::vector<std::pair<int, int>> stack;
	for (int y0 = 0; y0 < kGridSize; ++y0) {
		for (int x0 = 0; x0 < kGridSize; ++x0) {
			if (groupOf[static_cast<size_t>(y0) * kGridSize + x0] >= 0 || !member(x0, y0))
				continue;
			TownGroup group;
			group.minX = group.maxX = x0;
			group.minY = group.maxY = y0;
			const int id = static_cast<int>(groups.size());
			stack.assign(1, { x0, y0 });
			groupOf[static_cast<size_t>(y0) * kGridSize + x0] = id;
			while (!stack.empty()) {
				const auto [x, y] = stack.back();
				stack.pop_back();
				group.cells.emplace_back(x, y);
				group.minX = std::min(group.minX, x);
				group.maxX = std::max(group.maxX, x);
				group.minY = std::min(group.minY, y);
				group.maxY = std::max(group.maxY, y);
				for (const auto &[dx, dy] : kStairDirs) {
					const int nx = x + dx;
					const int ny = y + dy;
					if (nx < 0 || ny < 0 || nx >= kGridSize || ny >= kGridSize)
						continue;
					int &g = groupOf[static_cast<size_t>(ny) * kGridSize + nx];
					if (g >= 0 || !member(nx, ny))
						continue;
					g = id;
					stack.emplace_back(nx, ny);
				}
			}
			groups.push_back(std::move(group));
		}
	}
	return groups;
}

/** A tall town piece shown as a cut-out card. */
struct TownCard {
	int x, y;
	uint16_t piece;
};

/** A card of an object laid out in the object's plane. */
struct PlacedCard {
	int slot = -1;       // the art without ground (a solid piece's ground band is not shown from it)
	int groundSlot = -1; // the art with ground: the ground band shows it where `groundMask` is set
	int rows = 0;        // card rows, from the bottom of the piece image up
	int across = 0;      // classic screen columns from the anchor, in 32 px steps
	int back = 0;        // steps behind the anchor in x + y: 16 rows up each
	std::array<uint64_t, kCardFloorTop> groundMask {}; // per ground band row (card rows below kCardFloorTop), bit = column
};

bool CardTexelOpaque(int slot, int x, int r)
{
	return Atlas.cards[static_cast<size_t>(slot)].rgba[(static_cast<size_t>(Atlas.wallSlotHeight - 1 - r) * 64 + x) * 4 + 3] != 0;
}

/** Adds the bare ground colours (the ground triangles) of a floor piece to a palette histogram. */
void CountGroundColors(uint16_t piece, std::array<uint32_t, 256> &count, uint32_t &total)
{
	const PieceImage &full = GetPieceImage(piece);
	const PieceImage &foliage = GetPieceImage(piece, /*withoutGround=*/true);
	for (int r = 0; r < kCardFloorTop && r < full.height; ++r) {
		for (int c = 0; c < 64; ++c) {
			const size_t i = static_cast<size_t>(full.height - 1 - r) * 64 + c;
			if (full.opaque[i] != 0 && foliage.opaque[i] == 0) {
				++count[full.index[i]];
				++total;
			}
		}
	}
}

/**
 * Lays out the cards of one object around its anchor. The classic art paints the foot of an object into the ground band
 * of its pieces (their floor diamonds): the foot of a trunk, the lower slope of a boulder, the base of the well. Above
 * the band everything opaque is the object, and so is the foliage a floor piece draws over its ground; band pixels join
 * it top down when they touch the object right above them and their palette colour is more typical of the object (its
 * pixels outside the ground) than of the bare ground around it (the floor pieces within two tiles), then gaps of up to
 * three pixels between joined ones close. So the object reaches the ground with its own outline while the grass and
 * paving around it stay on the floor. Pieces further back go first: their band is the "above" of the pieces in front.
 */
std::vector<PlacedCard> PlaceCards(const std::vector<TownCard> &cards, int anchorX, int anchorY)
{
	constexpr int Band = kCardFloorTop;
	std::vector<PlacedCard> placed(cards.size());
	int minAcross = 0, maxAcross = 0, top = 1;
	for (size_t i = 0; i < cards.size(); ++i) {
		PlacedCard &card = placed[i];
		card.slot = CardSlot(cards[i].piece, card.rows);
		int groundRows = 0;
		card.groundSlot = CardSlot(cards[i].piece, groundRows, /*withGround=*/true);
		card.across = (cards[i].x - cards[i].y) - (anchorX - anchorY);
		card.back = (anchorX + anchorY) - (cards[i].x + cards[i].y);
		minAcross = std::min(minAcross, card.across);
		maxAcross = std::max(maxAcross, card.across);
		top = std::max(top, card.back * 16 + card.rows + 1);
	}
	const int width = (maxAcross - minAcross) * 32 + 64;
	std::vector<uint8_t> composite(static_cast<size_t>(width) * top, 0);
	const auto at = [&](const PlacedCard &card, int x, int r) -> uint8_t & {
		return composite[static_cast<size_t>(card.back * 16 + r) * width + (card.across - minAcross) * 32 + x];
	};
	std::array<uint32_t, 256> objectCount {};
	uint32_t objectTotal = 0;
	for (size_t i = 0; i < cards.size(); ++i) {
		const PlacedCard &card = placed[i];
		const PieceImage &foliage = GetPieceImage(cards[i].piece, /*withoutGround=*/true);
		const bool solid = TileHasAny({ cards[i].x, cards[i].y }, TileProperties::Solid);
		for (int r = solid ? Band : 0; r < card.rows; ++r) {
			for (int x = 0; x < 64; ++x) {
				const size_t p = static_cast<size_t>(foliage.height - 1 - r) * 64 + x;
				if (foliage.opaque[p] == 0)
					continue;
				at(card, x, r) = 1;
				++objectCount[foliage.index[p]];
				++objectTotal;
				if (r < Band)
					placed[i].groundMask[r] |= uint64_t { 1 } << x;
			}
		}
	}
	std::array<uint32_t, 256> groundCount {};
	uint32_t groundTotal = 0;
	{
		std::vector<std::pair<int, int>> counted;
		for (const TownCard &card : cards) {
			for (int dy = -2; dy <= 2; ++dy) {
				for (int dx = -2; dx <= 2; ++dx) {
					const int x = card.x + dx;
					const int y = card.y + dy;
					if (x < 0 || y < 0 || x >= kGridSize || y >= kGridSize || TileHasAny({ x, y }, TileProperties::Solid)
					    || std::find(counted.begin(), counted.end(), std::make_pair(x, y)) != counted.end()
					    || std::any_of(cards.begin(), cards.end(), [&](const TownCard &c) { return c.x == x && c.y == y; }))
						continue;
					counted.emplace_back(x, y);
					CountGroundColors(dPiece[x][y], groundCount, groundTotal);
				}
			}
		}
	}
	const auto objectLike = [&](uint8_t index) {
		// Laplace-smoothed likelihoods of the colour under "object" and under "bare ground".
		const double object = (objectCount[index] + 1.0) / (objectTotal + 256.0);
		const double ground = (groundCount[index] + 1.0) / (groundTotal + 256.0);
		return object > ground;
	};
	std::vector<size_t> order(cards.size());
	for (size_t i = 0; i < order.size(); ++i)
		order[i] = i;
	std::sort(order.begin(), order.end(), [&](size_t a, size_t b) { return placed[a].back > placed[b].back; });
	for (const size_t i : order) {
		PlacedCard &card = placed[i];
		const PieceImage &full = GetPieceImage(cards[i].piece);
		const int column0 = (card.across - minAcross) * 32;
		for (int r = std::min(Band, card.rows) - 1; r >= 0; --r) {
			const int above = card.back * 16 + r + 1;
			for (int x = 0; x < 64; ++x) {
				if ((card.groundMask[r] >> x & 1) != 0)
					continue;
				const size_t p = static_cast<size_t>(full.height - 1 - r) * 64 + x;
				if (full.opaque[p] == 0 || !objectLike(full.index[p]))
					continue;
				bool touches = false;
				for (int dx = -1; dx <= 1 && !touches; ++dx) {
					const int cx = column0 + x + dx;
					touches = cx >= 0 && cx < width && above < top && composite[static_cast<size_t>(above) * width + cx] != 0;
				}
				if (!touches)
					continue;
				card.groundMask[r] |= uint64_t { 1 } << x;
				at(card, x, r) = 1;
			}
		}
		// Close gaps of up to three pixels between joined ones, across and down (speckle of shared colours).
		const auto opaqueAt = [&](int x, int r) { return full.opaque[static_cast<size_t>(full.height - 1 - r) * 64 + x] != 0; };
		for (int pass = 0; pass < 2; ++pass) {
			for (int r = 0; r < std::min(Band, card.rows); ++r) {
				for (int x = 0; x < 64; ++x) {
					if ((card.groundMask[r] >> x & 1) != 0 || !opaqueAt(x, r))
						continue;
					const auto joined = [&](int cx, int cr) { return cx >= 0 && cx < 64 && cr >= 0 && cr < Band && (card.groundMask[cr] >> cx & 1) != 0; };
					bool left = false, right = false, below = false, up = false;
					for (int d = 1; d <= 3; ++d) {
						left = left || joined(x - d, r);
						right = right || joined(x + d, r);
						below = below || joined(x, r - d);
						up = up || joined(x, r + d) || (r + d >= Band && at(card, x, std::min(r + d, card.rows - 1)) != 0);
					}
					if ((left && right) || (below && up)) {
						card.groundMask[r] |= uint64_t { 1 } << x;
						at(card, x, r) = 1;
					}
				}
			}
		}
	}
	return placed;
}

/**
 * The cut-out cards of one object (a tree, the well, a sign) in a single upright plane: the iso screen plane (along
 * tile (1, -1)) through the anchor cell. A crown spills over several pieces; each piece keeps its classic screen offset
 * from the anchor (32 px across per step of x - y, 16 rows up per step back in x + y), so the slices rejoin as in the
 * iso art instead of standing as separate slabs on their own cells. Pieces further back sit a hair behind the ones in
 * front (no z-fighting where slices overlap). Every card is emitted as 8 x 8 texel cells without the empty ones, so
 * shadows (the ray tracer treats all geometry as opaque) follow the silhouette rather than the quad.
 */
void AddCardGroup(MeshBuilder &mesh, const std::vector<TownCard> &cards, int anchorX, int anchorY, float albedo)
{
	constexpr float InvSqrt2 = 0.70710678F;
	// The iso art is the 2:1 projection: 64 px across a tile diagonal (sqrt 2 units), and an upright unit seen from the
	// 30 degree camera pitch of that projection spans cos 30 of the screen length a unit across does. Texel rows per
	// world unit up are therefore (64 / sqrt 2) * cos 30, so the card shows the same width:height as the 2D sprite.
	constexpr float UnitsPerTexel = 1.41421356F / 64.0F;
	constexpr float RowsPerUnit = 64.0F / 1.41421356F * 0.8660254F;
	constexpr int Cell = 8;
	constexpr int Band = kCardFloorTop;
	constexpr float DepthBias = 0.004F;
	const Vec3 axis { InvSqrt2, 0.0F, -InvSqrt2 };  // screen right
	const Vec3 normal { InvSqrt2, 0.0F, InvSqrt2 }; // toward the iso camera
	const Vec3 anchor { static_cast<float>(anchorX) + 0.5F, 0.0F, static_cast<float>(anchorY) + 0.5F };
	const uint32_t flags = PackVertexFlags(kVertexBillboard, albedo);
	const int slotHeight = Atlas.wallSlotHeight;
	for (const PlacedCard &card : PlaceCards(cards, anchorX, anchorY)) {
		if (card.rows <= 0)
			continue;
		const std::vector<uint8_t> &rgba = Atlas.cards[static_cast<size_t>(card.slot)].rgba;
		const Vec3 left = anchor + axis * ((static_cast<float>(card.across) * 32.0F - 32.0F) * UnitsPerTexel) - normal * (static_cast<float>(card.back) * DepthBias);
		// Rows above the anchor's ground line: 16 per step back, less the piece's own ground line.
		const int lift = card.back * 16 - kCardGround;
		const auto shown = [&](int s, int r) {
			return rgba[(static_cast<size_t>(slotHeight - 1 - r) * 64 + s) * 4 + 3] != 0;
		};
		const auto quad = [&](int s0, int s1, int r0, int r1, int slot) {
			const Vec3 a = left + axis * (static_cast<float>(s0) * UnitsPerTexel);
			const Vec3 b = left + axis * (static_cast<float>(s1) * UnitsPerTexel);
			const float y0 = static_cast<float>(lift + r0) / RowsPerUnit;
			const float y1 = static_cast<float>(lift + r1) / RowsPerUnit;
			mesh.Quad({ a.x, y1, a.z }, { b.x, y1, b.z }, { b.x, y0, b.z }, { a.x, y0, a.z }, static_cast<float>(s0) + kInset,
			    static_cast<float>(slotHeight - r1) + kInset, static_cast<float>(s1) - kInset, static_cast<float>(slotHeight - r0) - kInset, normal,
			    SlotKind::Card, slot, flags);
		};
		for (int r0 = 0; r0 < card.rows; r0 += Cell) {
			const int r1 = std::min(card.rows, r0 + Cell);
			if (lift + r1 <= 0)
				continue; // below the ground (rows are 8-aligned, the ground line 16-aligned)
			if (r1 <= Band) {
				// The ground band: runs of the masked texels of the art with ground, rows with the same runs merged.
				for (int r = std::max(r0, -lift); r < r1;) {
					const uint64_t mask = card.groundMask[r];
					int e = r + 1;
					while (e < r1 && card.groundMask[e] == mask)
						++e;
					for (int s = 0; s < 64;) {
						if ((mask >> s & 1) == 0) {
							++s;
							continue;
						}
						int t = s + 1;
						while (t < 64 && (mask >> t & 1) != 0)
							++t;
						quad(s, t, r, e, card.groundSlot);
						s = t;
					}
					r = e;
				}
				continue;
			}
			for (int s0 = 0; s0 < 64; s0 += Cell) {
				const int s1 = s0 + Cell;
				bool any = false;
				for (int r = r0; r < r1 && !any; ++r) {
					for (int s = s0; s < s1 && !any; ++s)
						any = shown(s, r);
				}
				if (any)
					quad(s0, s1, r0, r1, card.slot);
			}
		}
	}
}

/**
 * DVL_W3D_DUMP_TREES=dir: one PPM per card object, the classic layout of its pieces (back to front, the 2D renderer's
 * picture of the object) on the left and the card layer the 3D view stands up on the right; magenta is empty.
 */
void DumpCardGroup(const char *dir, const std::vector<TownCard> &cards, int anchorX, int anchorY)
{
	int minAcross = 0, maxAcross = 0, top = 0;
	for (const TownCard &card : cards) {
		const int across = (card.x - card.y) - (anchorX - anchorY);
		const int back = (anchorX + anchorY) - (card.x + card.y);
		minAcross = std::min(minAcross, across);
		maxAcross = std::max(maxAcross, across);
		top = std::max(top, GetPieceImage(card.piece).height + back * 16);
		Log("render3d: tree {},{} piece {},{} #{} across {} back {} solid {} height {}", anchorX, anchorY, card.x, card.y, card.piece, across, back,
		    TileHasAny({ card.x, card.y }, TileProperties::Solid) ? 1 : 0, GetPieceImage(card.piece).height);
	}
	const int width = (maxAcross - minAcross) * 32 + 64;
	std::vector<uint8_t> rgb(static_cast<size_t>(width) * 2 * top * 3, 0);
	for (size_t o = 0; o < rgb.size(); o += 3) {
		rgb[o] = 255;
		rgb[o + 2] = 255;
	}
	const std::vector<PlacedCard> placed = PlaceCards(cards, anchorX, anchorY);
	std::vector<size_t> order(cards.size());
	for (size_t i = 0; i < order.size(); ++i)
		order[i] = i;
	std::sort(order.begin(), order.end(), [&](size_t a, size_t b) { return cards[a].x + cards[a].y < cards[b].x + cards[b].y; });
	for (int half = 0; half < 2; ++half) {
		for (const size_t cardIndex : order) {
			const TownCard &card = cards[cardIndex];
			const PieceImage &image = GetPieceImage(card.piece);
			int rows = 0;
			const int slot = CardSlot(card.piece, rows);
			const int across = (card.x - card.y) - (anchorX - anchorY);
			const int back = (anchorX + anchorY) - (card.x + card.y);
			for (int r = 0; r < image.height; ++r) { // r: rows up from the image bottom
				const int y = top - 1 - (r + back * 16);
				if (y < 0)
					continue;
				for (int x = 0; x < 64; ++x) {
					const int cx = half * width + (across - minAcross) * 32 + x;
					uint8_t index = 0;
					if (half == 0) {
						const size_t i = static_cast<size_t>(image.height - 1 - r) * 64 + x;
						if (image.opaque[i] == 0)
							continue;
						index = image.index[i];
					} else {
						const int cr = r;
						const PlacedCard &laid = placed[cardIndex];
						int from = slot;
						if (cr < kCardFloorTop)
							from = (laid.groundMask[cr] >> x & 1) != 0 ? laid.groundSlot : -1;
						if (cr >= rows || from < 0 || cr + back * 16 < kCardGround)
							continue;
						const uint8_t *px = &Atlas.cards[static_cast<size_t>(from)].rgba[(static_cast<size_t>(Atlas.wallSlotHeight - 1 - cr) * 64 + x) * 4];
						if (px[3] == 0)
							continue;
						const size_t o = (static_cast<size_t>(y) * 2 * width + cx) * 3;
						rgb[o] = px[0];
						rgb[o + 1] = px[1];
						rgb[o + 2] = px[2];
						continue;
					}
					const size_t o = (static_cast<size_t>(y) * 2 * width + cx) * 3;
					rgb[o] = ArtPalette[index][0];
					rgb[o + 1] = ArtPalette[index][1];
					rgb[o + 2] = ArtPalette[index][2];
				}
			}
		}
	}
	const std::string path = std::string(dir) + "/tree_" + std::to_string(anchorX) + "_" + std::to_string(anchorY) + ".ppm";
	if (FILE *file = std::fopen(path.c_str(), "wb"); file != nullptr) {
		std::fprintf(file, "P6\n%d %d\n255\n", width * 2, top);
		std::fwrite(rgb.data(), 1, rgb.size(), file);
		std::fclose(file);
	}
}

/**
 * Cut-out cards for the tall town pieces flagged in `cardCell`. 8-connected pieces form objects; every solid piece (a
 * trunk, a post) anchors one, and the other pieces join the nearest anchor at most two steps in front of them and six
 * across (a crown spans up to six pieces). Pieces no solid anchor takes anchor on the front-most, tallest of them.
 */
void AddTownCards(MeshBuilder &mesh, const std::vector<uint8_t> &cardCell, const std::vector<int> &pieceHeight, const std::vector<int> &smallObjectOf,
    const std::vector<uint8_t> &artRegion, std::vector<uint8_t> &toArt)
{
	const auto index = [](int x, int y) { return static_cast<size_t>(y) * kGridSize + x; };
	std::vector<int> groupOf(static_cast<size_t>(kGridSize) * kGridSize, -1);
	std::vector<std::pair<int, int>> stack;
	std::vector<std::pair<int, int>> group;
	for (int y0 = 0; y0 < kGridSize; ++y0) {
		for (int x0 = 0; x0 < kGridSize; ++x0) {
			if (cardCell[index(x0, y0)] == 0 || groupOf[index(x0, y0)] >= 0)
				continue;
			group.clear();
			stack.assign(1, { x0, y0 });
			groupOf[index(x0, y0)] = 1;
			while (!stack.empty()) {
				const auto [x, y] = stack.back();
				stack.pop_back();
				group.emplace_back(x, y);
				for (int dy = -1; dy <= 1; ++dy) {
					for (int dx = -1; dx <= 1; ++dx) {
						const int nx = x + dx;
						const int ny = y + dy;
						if (nx < 0 || ny < 0 || nx >= kGridSize || ny >= kGridSize || cardCell[index(nx, ny)] == 0 || groupOf[index(nx, ny)] >= 0)
							continue;
						groupOf[index(nx, ny)] = 1;
						stack.emplace_back(nx, ny);
					}
				}
			}
			// Anchors: a small solid object (the well, a boulder of a few pieces) stands as one on its front-most,
			// tallest piece; then every other solid piece; then (for the pieces none of them takes, such as a crown behind a
			// fence post it touches) the front-most, tallest piece left, until every piece has one.
			std::vector<size_t> anchors;
			std::vector<int> anchorOf(group.size(), -1);
			const auto frontTallest = [&](size_t a, size_t b) {
				const auto &[ax, ay] = group[a];
				const auto &[bx, by] = group[b];
				return ax + ay > bx + by || (ax + ay == bx + by && pieceHeight[index(ax, ay)] > pieceHeight[index(bx, by)]);
			};
			for (size_t i = 0; i < group.size(); ++i) {
				const int object = smallObjectOf[index(group[i].first, group[i].second)];
				if (object < 0 || anchorOf[i] >= 0)
					continue;
				size_t best = i;
				for (size_t j = i + 1; j < group.size(); ++j) {
					if (smallObjectOf[index(group[j].first, group[j].second)] == object && frontTallest(j, best))
						best = j;
				}
				anchors.push_back(best);
				for (size_t j = i; j < group.size(); ++j) {
					if (smallObjectOf[index(group[j].first, group[j].second)] == object)
						anchorOf[j] = static_cast<int>(best);
				}
			}
			for (size_t i = 0; i < group.size(); ++i) {
				if (anchorOf[i] < 0 && TileHasAny({ group[i].first, group[i].second }, TileProperties::Solid)) {
					anchors.push_back(i);
					anchorOf[i] = static_cast<int>(i);
				}
			}
			while (true) {
				bool unassigned = false;
				for (size_t i = 0; i < group.size(); ++i) {
					if (anchorOf[i] >= 0)
						continue;
					const auto &[x, y] = group[i];
					int bestScore = 1 << 30;
					for (const size_t a : anchors) {
						const auto &[ax, ay] = group[a];
						const int back = (ax + ay) - (x + y);
						const int across = std::abs((ax - ay) - (x - y));
						if (back < 0 || back > 2 || across > 6)
							continue;
						const int score = across + 2 * back;
						if (score < bestScore) {
							bestScore = score;
							anchorOf[i] = static_cast<int>(a);
						}
					}
					unassigned = unassigned || anchorOf[i] < 0;
				}
				if (!unassigned)
					break;
				size_t best = group.size();
				for (size_t i = 0; i < group.size(); ++i) {
					if (anchorOf[i] >= 0)
						continue;
					const auto &[x, y] = group[i];
					if (best == group.size())
						best = i;
					const auto &[bx, by] = group[best];
					if (x + y > bx + by || (x + y == bx + by && pieceHeight[index(x, y)] > pieceHeight[index(bx, by)]))
						best = i;
				}
				anchors.push_back(best);
				anchorOf[best] = static_cast<int>(best);
			}
			std::vector<TownCard> cards;
			for (size_t a = 0; a < group.size(); ++a) {
				if (anchorOf[a] != static_cast<int>(a))
					continue;
				cards.clear();
				for (size_t i = 0; i < group.size(); ++i) {
					if (anchorOf[i] == static_cast<int>(a))
						cards.push_back({ group[i].first, group[i].second, dPiece[group[i].first][group[i].second] });
				}
				// An object wholly inside a built house's art region (a sign, a post, a crown on the house's cells) is part of the
				// house's composite at the art's angle (AddArtRelief): its card belongs to the reconstructed houses then.
				const bool art = std::all_of(cards.begin(), cards.end(), [&](const TownCard &card) { return artRegion[index(card.x, card.y)] != 0; });
				const size_t firstVertex = mesh.vertices.size();
				AddCardGroup(mesh, cards, group[a].first, group[a].second, 1.0F);
				if (art) {
					for (size_t v = firstVertex; v < mesh.vertices.size(); ++v)
						mesh.vertices[v].vertex.flags |= kVertexTownHouse;
					for (const TownCard &card : cards)
						toArt[index(card.x, card.y)] = 1;
				}
				if (const char *dir = std::getenv("DVL_W3D_DUMP_TREES"); dir != nullptr && dir[0] != '\0')
					DumpCardGroup(dir, cards, group[a].first, group[a].second);
			}
		}
	}
}

/** A rectangle of house cells, inclusive. */
struct HouseRect {
	int x0, y0, x1, y1;
};

/**
 * Covers the volume cells of a house with rectangles, each of which gets its own gable roof: repeatedly the rectangle
 * inside the volume that covers the most still uncovered cells, the smallest on a tie. Rectangles may overlap (cross
 * gables where two wings meet); the smallest one keeps a ragged edge a narrow annex next to the main roof instead of a
 * second roof of nearly the same size crossing it.
 */
std::vector<HouseRect> HouseRects(const std::vector<std::pair<int, int>> &cells, int minX, int minY, int maxX, int maxY)
{
	const int w = maxX - minX + 1;
	const int h = maxY - minY + 1;
	std::vector<uint8_t> in(static_cast<size_t>(w) * h, 0);
	for (const auto &[x, y] : cells)
		in[static_cast<size_t>(y - minY) * w + (x - minX)] = 1;
	std::vector<uint8_t> covered(in.size(), 0);
	// Prefix sums: of volume cells (fixed) and of uncovered volume cells (per round).
	const auto prefix = [&](const auto &value) {
		std::vector<int> sum(static_cast<size_t>(w + 1) * (h + 1), 0);
		for (int y = 0; y < h; ++y)
			for (int x = 0; x < w; ++x)
				sum[static_cast<size_t>(y + 1) * (w + 1) + x + 1] = value(x, y) + sum[static_cast<size_t>(y) * (w + 1) + x + 1]
				    + sum[static_cast<size_t>(y + 1) * (w + 1) + x] - sum[static_cast<size_t>(y) * (w + 1) + x];
		return sum;
	};
	const auto area = [&](const std::vector<int> &sum, int x0, int y0, int x1, int y1) {
		return sum[static_cast<size_t>(y1 + 1) * (w + 1) + x1 + 1] - sum[static_cast<size_t>(y0) * (w + 1) + x1 + 1]
		    - sum[static_cast<size_t>(y1 + 1) * (w + 1) + x0] + sum[static_cast<size_t>(y0) * (w + 1) + x0];
	};
	const std::vector<int> volume = prefix([&](int x, int y) { return static_cast<int>(in[static_cast<size_t>(y) * w + x]); });
	std::vector<HouseRect> rects;
	int left = static_cast<int>(cells.size());
	while (left > 0 && rects.size() < 8) {
		const std::vector<int> open = prefix([&](int x, int y) {
			const size_t i = static_cast<size_t>(y) * w + x;
			return in[i] != 0 && covered[i] == 0 ? 1 : 0;
		});
		HouseRect best { 0, 0, -1, -1 };
		int bestNew = 0;
		int bestArea = 0;
		for (int y0 = 0; y0 < h; ++y0)
			for (int x0 = 0; x0 < w; ++x0)
				for (int y1 = y0; y1 < h; ++y1)
					for (int x1 = x0; x1 < w; ++x1) {
						const int a = (x1 - x0 + 1) * (y1 - y0 + 1);
						if (area(volume, x0, y0, x1, y1) != a) {
							break; // wider rectangles from this corner leave the volume too
						}
						const int fresh = area(open, x0, y0, x1, y1);
						if (fresh > bestNew || (fresh == bestNew && fresh > 0 && a < bestArea)) {
							bestNew = fresh;
							bestArea = a;
							best = { x0, y0, x1, y1 };
						}
					}
		if (bestNew == 0)
			break;
		for (int y = best.y0; y <= best.y1; ++y)
			for (int x = best.x0; x <= best.x1; ++x)
				covered[static_cast<size_t>(y) * w + x] = 1;
		left -= bestNew;
		rects.push_back({ best.x0 + minX, best.y0 + minY, best.x1 + minX, best.y1 + minY });
	}
	return rects;
}

/**
 * Rows of facade art below the eave of one wall half: the lowest unprojected row (above the wall foot) from which the
 * roof's thatch takes over, -1 when the half shows no such line. `thatch` marks the palette entries of the roof art that
 * the wall's own foot does not use.
 */
int FacadeEaveRows(uint16_t piece, int side, const std::array<bool, 256> &thatch)
{
	const PieceImage &image = GetPieceImage(piece);
	int run = 0;
	for (int h = 0; h < image.height; ++h) {
		int opaque = 0;
		int roof = 0;
		for (int u = 0; u < 32; ++u) {
			const int c = side == 0 ? u : 32 + u;
			const int foot = side == 0 ? u / 2 : (31 - u) / 2;
			const int row = image.height - 1 - (h + foot);
			if (row < 0)
				continue;
			const size_t i = static_cast<size_t>(row) * 64 + c;
			if (image.opaque[i] == 0)
				continue;
			++opaque;
			roof += thatch[image.index[i]] ? 1 : 0;
		}
		run = opaque >= 16 && roof * 2 >= opaque ? run + 1 : 0;
		if (run == 4)
			return h - 3 >= 32 ? h - 3 : -1;
	}
	return -1;
}

/** Vertical rows of town art per world unit: the card scale (AddCardGroup), so walls keep the 2D art's proportions. */
constexpr float kTownRowsPerUnit = 64.0F / 1.41421356F * 0.8660254F;
/** Roof rise per unit from the wall to the ridge: 45 degrees, a thatched cottage's pitch. */
constexpr float kHousePitch = 1.0F;
/**
 * The facade art is lit for the classic view's fixed light; in 3D the walls facing the camera face away from the town's
 * key light and only take ambient, so a little extra albedo keeps their timber and windows readable.
 */
constexpr float kHouseWallAlbedo = 1.6F;

/**
 * Gable roof over the cell rectangle [x0, x1] x [z0, z1] (cell borders). The walls stand on the centre line of the edge
 * cells, so the roof passes through the wall top half a cell in from the border and overhangs the walls by that half
 * cell, never past the house's own cells (towners and the hero walk right next to them). Thatch repeats every unit along
 * the ridge and up the slope; eaves and ridge get a thin edge, and the gable ends are walls textured with facade art at
 * the facade's scale.
 */
void AddHouseRoof(MeshBuilder &mesh, float x0, float z0, float x1, float z1, float eave, float pitch, float maxRise, int roofSlot, int gableSlot,
    float gableRow0)
{
	const bool alongX = (x1 - x0) >= (z1 - z0);
	// Local frame: `a` along the ridge, `b` across it (b = 0 on the ridge).
	const float a0 = alongX ? x0 : z0;
	const float a1 = alongX ? x1 : z1;
	const float bc = alongX ? (z0 + z1) * 0.5F : (x0 + x1) * 0.5F;
	const float halfSpan = (alongX ? z1 - z0 : x1 - x0) * 0.5F;
	const float wallHalf = std::max(0.2F, halfSpan - 0.5F);
	const float rise = std::min(maxRise, pitch * wallHalf);
	const float slopeK = rise / wallHalf; // drop per unit away from the ridge
	const float ridge = eave + rise;
	const auto heightAt = [&](float d) { return ridge - slopeK * std::abs(d); };
	const auto point = [&](float a, float y, float b) { return alongX ? Vec3 { a, y, bc + b } : Vec3 { bc + b, y, a }; };
	const float edgeY = heightAt(halfSpan);
	const float slopeLen = std::sqrt(halfSpan * halfSpan + (ridge - edgeY) * (ridge - edgeY));
	const int bands = std::max(1, static_cast<int>(std::ceil(slopeLen - 1e-3F)));
	const float bandTexels = 32.0F * slopeLen / static_cast<float>(bands);
	for (const float sign : { -1.0F, 1.0F }) {
		const Vec3 n = Normalize(alongX ? Vec3 { 0.0F, halfSpan, sign * (ridge - edgeY) } : Vec3 { sign * (ridge - edgeY), halfSpan, 0.0F });
		for (float a = a0; a < a1 - 1e-3F; a += 1.0F) {
			const float ae = std::min(a + 1.0F, a1);
			const float sEnd = 32.0F * (ae - a) - kInset;
			for (int k = 0; k < bands; ++k) {
				const float d0 = halfSpan * (1.0F - static_cast<float>(k) / static_cast<float>(bands));
				const float d1 = halfSpan * (1.0F - static_cast<float>(k + 1) / static_cast<float>(bands));
				// Darker toward the eaves, as under the thatch's own overhang.
				const float albedo = 0.85F + 0.2F * static_cast<float>(k) / static_cast<float>(std::max(1, bands - 1));
				mesh.Quad(point(a, heightAt(d0), sign * d0), point(ae, heightAt(d0), sign * d0), point(ae, heightAt(d1), sign * d1), point(a, heightAt(d1), sign * d1),
				    kInset, 32.0F - kInset, sEnd, std::max(kInset, 32.0F - bandTexels + kInset), n, SlotKind::Square, roofSlot, PackVertexFlags(0, albedo));
			}
		}
		// Eave edge: a short drop below the roof's lower edge, so the thatch reads as a layer rather than a sheet.
		const Vec3 down = alongX ? Vec3 { 0.0F, 0.0F, sign } : Vec3 { sign, 0.0F, 0.0F };
		for (float a = a0; a < a1 - 1e-3F; a += 1.0F) {
			const float ae = std::min(a + 1.0F, a1);
			mesh.Quad(point(a, edgeY, sign * halfSpan), point(ae, edgeY, sign * halfSpan), point(ae, edgeY - 0.14F, sign * halfSpan), point(a, edgeY - 0.14F, sign * halfSpan),
			    kInset, 2.0F, 32.0F * (ae - a) - kInset, 6.0F, down, SlotKind::Square, roofSlot, PackVertexFlags(0, 0.55F));
		}
	}
	// Ridge cap: a narrow raised saddle along the ridge.
	for (float a = a0; a < a1 - 1e-3F; a += 1.0F) {
		const float ae = std::min(a + 1.0F, a1);
		for (const float sign : { -1.0F, 1.0F }) {
			const Vec3 n = Normalize(alongX ? Vec3 { 0.0F, 1.0F, sign } : Vec3 { sign, 1.0F, 0.0F });
			mesh.Quad(point(a, ridge - 0.04F, sign * 0.16F), point(ae, ridge - 0.04F, sign * 0.16F), point(ae, ridge + 0.08F, 0.0F), point(a, ridge + 0.08F, 0.0F),
			    kInset, 20.0F, 32.0F * (ae - a) - kInset, 26.0F, n, SlotKind::Square, roofSlot, PackVertexFlags(0, 0.62F));
		}
	}
	// Gable ends on the wall line, half a cell in from the rectangle's ends, as unit-wide strips (wall slots are one unit
	// wide) up to the roof line, and the barge edge along the roof's end.
	const int slotHeight = Atlas.wallSlotHeight;
	const auto tOf = [&](float y) {
		return std::clamp(static_cast<float>(slotHeight) - 1.0F - (gableRow0 + (y - eave) * kTownRowsPerUnit), 0.0F, static_cast<float>(slotHeight) - 1.0F);
	};
	for (const float end : { a0 + 0.5F, a1 - 0.5F }) {
		const float outward = end < (a0 + a1) * 0.5F ? -1.0F : 1.0F;
		const Vec3 n = alongX ? Vec3 { outward, 0.0F, 0.0F } : Vec3 { 0.0F, 0.0F, outward };
		for (float b = -wallHalf; b < wallHalf - 1e-3F; b += 1.0F) {
			const float be = std::min(b + 1.0F, wallHalf);
			// Split at the ridge so each strip's top is one straight roof line.
			const float splits[3] = { b, (b < 0.0F && be > 0.0F) ? 0.0F : be, be };
			for (int s = 0; s < 2; ++s) {
				const float p = splits[s];
				const float q = splits[s + 1];
				if (q - p < 1e-3F)
					continue;
				const float sp = 32.0F * (p - b) + kInset;
				const float sq = std::max(sp, 32.0F * (q - b) - kInset);
				const float yp = heightAt(p);
				const float yq = heightAt(q);
				// Quad p-bottom, q-bottom, q-top, p-top; its t is linear in y, so the two top corners take their own rows.
				const auto base = static_cast<uint32_t>(mesh.vertices.size());
				const Vec3 ps[4] = { point(end, eave, p), point(end, eave, q), point(end, yq, q), point(end, yp, p) };
				const float ss[4] = { sp, sq, sq, sp };
				const float ts[4] = { tOf(eave), tOf(eave), tOf(yq), tOf(yp) };
				for (int i = 0; i < 4; ++i) {
					PendingVertex v;
					v.vertex = { ps[i].x, ps[i].y, ps[i].z, 0.0F, 0.0F, n.x, n.y, n.z, PackVertexFlags(0, 1.4F) };
					v.kind = SlotKind::Wall;
					v.slot = gableSlot;
					v.s = ss[i];
					v.t = ts[i];
					mesh.vertices.push_back(v);
				}
				mesh.indices.insert(mesh.indices.end(), { base, base + 1, base + 2, base, base + 2, base + 3 });
			}
		}
		// Barge edge: the roof's end seen edge on, a thin band under both slopes.
		const float ae = outward < 0.0F ? a0 : a1;
		for (const float sign : { -1.0F, 1.0F }) {
			mesh.Quad(point(ae, ridge, 0.0F), point(ae, edgeY, sign * halfSpan), point(ae, edgeY - 0.14F, sign * halfSpan), point(ae, ridge - 0.14F, 0.0F),
			    kInset, 2.0F, 30.0F, 6.0F, n, SlotKind::Square, roofSlot, PackVertexFlags(0, 0.55F));
		}
	}
}

/** A square pyramid roof (the cathedral tower). */
void AddPyramidRoof(MeshBuilder &mesh, float x0, float z0, float x1, float z1, float eave, float apexY, int roofSlot)
{
	const Vec3 apex { (x0 + x1) * 0.5F, apexY, (z0 + z1) * 0.5F };
	const Vec3 corners[4] = { { x0, eave, z0 }, { x1, eave, z0 }, { x1, eave, z1 }, { x0, eave, z1 } };
	for (int i = 0; i < 4; ++i) {
		const Vec3 &a = corners[i];
		const Vec3 &b = corners[(i + 1) % 4];
		Vec3 n = Normalize(Cross(b - a, apex - a));
		if (n.y < 0.0F)
			n = -n;
		mesh.Triangle(a, b, apex, kInset, 32.0F - kInset, 32.0F - kInset, 32.0F - kInset, 16.0F, kInset, n, SlotKind::Square, roofSlot,
		    PackVertexFlags(0, 0.8F));
	}
}

/**
 * One built house: walls on the outline of its volume cells (the cells of the group inside a full 2 x 2 block of it;
 * one-cell fences and posts stay cut-out cards), with the edge pieces' own facade art at the art's proportions up to the
 * eave the art shows, and a gable roof per rectangle of the volume (HouseRects). `volume` marks the cells over the grid.
 */
void BuildHouse(LevelGeometry &geometry, MeshBuilder &mesh, const TownGroup &group, const std::vector<uint8_t> &volume, bool cathedral)
{
	const auto inVolume = [&](int x, int y) {
		return x >= 0 && y >= 0 && x < kGridSize && y < kGridSize && volume[static_cast<size_t>(y) * kGridSize + x] != 0;
	};
	std::vector<std::pair<int, int>> cells;
	int minX = kGridSize, minY = kGridSize, maxX = -1, maxY = -1;
	for (const auto &[x, y] : group.cells) {
		if (!inVolume(x, y))
			continue;
		cells.emplace_back(x, y);
		minX = std::min(minX, x);
		minY = std::min(minY, y);
		maxX = std::max(maxX, x);
		maxY = std::max(maxY, y);
	}
	if (cells.empty())
		return;
	// Facade art: the left halves (SE faces, +x) and right halves (SW faces, +y) of the front edge pieces.
	struct FacadeHalf {
		uint16_t piece;
		int side;
		int slot;
	};
	std::vector<FacadeHalf> facades;
	std::vector<int> facadeX;
	std::vector<int> facadeZ;
	uint16_t tallest = dPiece[cells.front().first][cells.front().second];
	int tallestHeight = -1;
	for (const auto &[x, y] : group.cells) {
		const uint16_t piece = dPiece[x][y];
		const int height = tile_geometry::GetPieceGeometry(piece).heightPx;
		if (height > tallestHeight) {
			tallestHeight = height;
			tallest = piece;
		}
		if (!inVolume(x, y))
			continue;
		if (!inVolume(x + 1, y)) {
			if (const int slot = WallSlot(piece, 0); slot >= 0) {
				facadeX.push_back(slot);
				facades.push_back({ piece, 0, slot });
			}
		}
		if (!inVolume(x, y + 1)) {
			if (const int slot = WallSlot(piece, 1); slot >= 0) {
				facadeZ.push_back(slot);
				facades.push_back({ piece, 1, slot });
			}
		}
	}
	if (facadeX.empty())
		facadeX = facadeZ;
	if (facadeZ.empty())
		facadeZ = facadeX;
	std::vector<uint16_t> roofPieces { tallest };
	for (const auto &[x, y] : group.cells) {
		if (std::find(roofPieces.begin(), roofPieces.end(), dPiece[x][y]) == roofPieces.end())
			roofPieces.push_back(dPiece[x][y]);
	}
	const RoofCrop crop = FindRoofCrop(roofPieces);
	const int roofSlot = RoofSlot(crop);

	// The eave: where the facades' thatch begins. Thatch colours are the roof art's main palette entries that the walls'
	// own feet (the bottom rows, always wall) do not use.
	std::array<int, 256> roofCount {};
	{
		const PieceImage &image = GetPieceImage(crop.piece);
		for (int v = 0; v < 32; ++v)
			for (int u = 0; u < 32; ++u) {
				const size_t i = static_cast<size_t>(crop.y0 + v) * 64 + crop.x0 + u;
				if (image.opaque[i] != 0)
					++roofCount[image.index[i]];
			}
	}
	std::array<int, 256> footCount {};
	int footTotal = 0;
	for (const FacadeHalf &f : facades) {
		const PieceImage &image = GetPieceImage(f.piece);
		for (int h = 0; h < 20; ++h)
			for (int u = 0; u < 32; ++u) {
				const int c = f.side == 0 ? u : 32 + u;
				const int foot = f.side == 0 ? u / 2 : (31 - u) / 2;
				const int row = image.height - 1 - (h + foot);
				if (row < 0)
					continue;
				const size_t i = static_cast<size_t>(row) * 64 + c;
				if (image.opaque[i] != 0) {
					++footCount[image.index[i]];
					++footTotal;
				}
			}
	}
	std::array<bool, 256> thatch {};
	for (int i = 0; i < 256; ++i)
		thatch[i] = roofCount[i] >= 6 && footCount[i] * 100 <= footTotal;
	std::vector<int> eaves;
	for (const FacadeHalf &f : facades) {
		if (const int rows = FacadeEaveRows(f.piece, f.side, thatch); rows > 0)
			eaves.push_back(rows);
	}
	int eaveRows = cathedral ? 150 : 96;
	if (!eaves.empty()) {
		std::nth_element(eaves.begin(), eaves.begin() + static_cast<std::ptrdiff_t>(eaves.size() / 2), eaves.end());
		eaveRows = eaves[eaves.size() / 2];
	}
	const float eave = std::clamp(static_cast<float>(eaveRows) / kTownRowsPerUnit, cathedral ? 3.0F : 1.9F, cathedral ? 4.6F : 3.4F);
	eaveRows = static_cast<int>(eave * kTownRowsPerUnit + 0.5F);
	const int slotHeight = Atlas.wallSlotHeight;
	const std::vector<HouseRect> rects = HouseRects(cells, minX, minY, maxX, maxY);
	if (std::getenv("DVL_W3D_DUMP_TOWN") != nullptr) {
		Log("render3d: built house {}..{} x {}..{}: eave {} rows ({} facades measured), {} roof rects, roof crop piece {} x{} y{}", minX, maxX, minY, maxY,
		    eaveRows, eaves.size(), rects.size(), crop.piece, crop.x0, crop.y0);
		for (const HouseRect &r : rects)
			Log("render3d:   roof rect {}..{} x {}..{}", r.x0, r.x1, r.y0, r.y1);
	}

	for (const auto &[x, y] : cells) {
		geometry.occluders[static_cast<size_t>(y) * kGridSize + x] = PackOccluder(0.0F, eave);
		const float fx = static_cast<float>(x);
		const float fz = static_cast<float>(y);
		struct Side {
			int dx, dy, axis;
			Vec3 normal;
			int artSide;
		};
		const Side sides[4] = {
			{ 1, 0, 0, { 1, 0, 0 }, 0 },
			{ -1, 0, 0, { -1, 0, 0 }, 0 },
			{ 0, 1, 1, { 0, 0, 1 }, 1 },
			{ 0, -1, 1, { 0, 0, -1 }, 1 },
		};
		for (const Side &side : sides) {
			if (inVolume(x + side.dx, y + side.dy))
				continue;
			// The iso art stands its walls on the centre line of the edge cells: the face plane is inset by half a
			// cell; along the edge each end stops at the cell centre (convex corner), at the cell border (straight)
			// or half a cell beyond it (concave corner), so the faces meet.
			const int ex = side.axis == 0 ? 0 : 1;
			const int ey = side.axis == 0 ? 1 : 0;
			const auto endOffset = [&](int sign) {
				const int nx = x + ex * sign;
				const int ny = y + ey * sign;
				if (!inVolume(nx, ny))
					return -0.5F;
				return inVolume(nx + side.dx, ny + side.dy) ? 0.5F : 0.0F;
			};
			const float plane = side.axis == 0 ? fx + 0.5F : fz + 0.5F;
			const float cellStart = side.axis == 0 ? fz : fx;
			const float spanStart = cellStart - endOffset(-1);
			const float spanEnd = cellStart + 1.0F + endOffset(1);
			// Front faces show the edge piece's own facade, other faces (and fronts without art) borrow the group's.
			int slot = side.dx > 0 || side.dy > 0 ? WallSlot(dPiece[x][y], side.artSide) : -1;
			if (slot < 0) {
				const std::vector<int> &pool = side.artSide == 0 ? facadeX : facadeZ;
				if (!pool.empty())
					slot = pool[static_cast<size_t>(side.axis == 0 ? y : x) % pool.size()];
			}
			if (slot >= 0)
				++Atlas.wallUse[slot];
			// The facade's rows up to the eave fill the wall, at the art's own proportions.
			const int textureHeight = slot >= 0 ? Atlas.walls[slot].textureHeight : slotHeight;
			const float fullHeight = eave * static_cast<float>(textureHeight) / static_cast<float>(std::max(1, std::min(eaveRows, textureHeight)));
			AddWallFace(mesh, side.axis, plane, spanStart, spanEnd, 0.0F, eave, std::max(eave, fullHeight), side.normal, slot, slotHeight, kHouseWallAlbedo);
		}
	}
	// Gables show the facade's middle band (timber and plaster, rarely the door), at the wall's scale.
	const int gableSlot = facadeX.empty() ? -1 : facadeX.front();
	const float gableRow0 = static_cast<float>(eaveRows) * 0.3F;
	float highestRidge = eave;
	for (const HouseRect &r : rects) {
		const float span = static_cast<float>(std::min(r.x1 - r.x0, r.y1 - r.y0) + 1);
		const float maxRise = cathedral ? 3.0F : 2.2F;
		AddHouseRoof(mesh, static_cast<float>(r.x0), static_cast<float>(r.y0), static_cast<float>(r.x1 + 1), static_cast<float>(r.y1 + 1), eave, kHousePitch, maxRise,
		    roofSlot, gableSlot, gableRow0);
		highestRidge = std::max(highestRidge, eave + std::min(maxRise, kHousePitch * std::max(0.2F, span * 0.5F - 0.5F)));
	}
	if (cathedral) {
		// Bell tower on the front corner (toward the town), inside the volume.
		const float tx1 = static_cast<float>(maxX) + 0.5F;
		const float tz1 = static_cast<float>(maxY) + 0.5F;
		const float tx0 = tx1 - 2.6F;
		const float tz0 = tz1 - 2.6F;
		const float top = highestRidge + 2.2F;
		const int slotX = facadeX.empty() ? -1 : facadeX.back();
		const int slotZ = facadeZ.empty() ? -1 : facadeZ.back();
		const float art = slotX >= 0 ? static_cast<float>(Atlas.walls[slotX].textureHeight) / kTownRowsPerUnit : top;
		AddBox(mesh, { tx0, 0.0F, tz0 }, { tx1, top, tz1 }, std::max(art, 1.0F), slotX, slotZ, -1, slotHeight, 0.95F);
		AddPyramidRoof(mesh, tx0 - 0.2F, tz0 - 0.2F, tx1 + 0.2F, tz1 + 0.2F, top, top + 2.4F, roofSlot);
	}
}

// ---- Tristram building art at the classic projection ---------------------------------------------------------
//
// The default 3D camera looks at the town from the iso art's own angle (yaw 45 degrees, pitch about 30), so a house can
// be shown as its original pixels instead of a reconstruction. Every building's pieces are painted, in the classic
// renderer's order, into one composite image of the classic screen (ArtComposite), and that image is laid on a relief:
// a mesh over the composite whose points sit at the classic screen position of their pixel, at a depth taken from the
// pieces that drew it. Any point on the line of sight of a classic screen pixel projects back onto that pixel, so from
// the art's angle the relief shows the 2D art pixel for pixel whatever the depths are; the depths only decide occlusion
// and how the picture bends for other cameras. Because the relief is one continuous surface per building, the
// perspective of the 3D camera (and the legacy look's steeper pitch) only stretches the picture a little; separate
// per-piece panels tore apart where pieces of different depth meet.
//
// The depth of a pixel is the back edge of the cell whose piece drew it (plane x = i under the left image half, plane
// y = j under the right half): the whole floor diamond lies in front of it, the classic painter's order follows (a piece
// hides what stands in the cells behind it; an actor in its own cell or the cells in front stands in front of it), and a
// wall reaches the ground exactly on its cell's edge. The relief's vertices average the depths around them, so steps
// between a roof edge and the wall behind become short slopes rather than cracks.
//
// Classic screen coordinates (pixels, y down) of a world point (x, h, z): sx = 32 (x - z), sy = 16 (x + z) - h * RowsPerUnit.
// With u = x + z, a screen point and a depth give the world point back: x = (u + sx / 32) / 2, z = (u - sx / 32) / 2,
// h = (16 u - sy) / RowsPerUnit; larger u is nearer the camera and higher up the line of sight.

/** Vertical rows of town art per world unit: the card scale (AddCardGroup), the 30 degree view of the 2:1 art. */
constexpr float kArtRowsPerUnit = 64.0F / 1.41421356F * 0.8660254F;
/** Relief grid step in composite pixels. */
constexpr int kArtBlock = 8;

/** One building's art at the classic projection: palette indices, coverage and depth (u) per pixel. */
struct ArtComposite {
	int x0 = 0, y0 = 0; // classic screen position of pixel (0, 0)
	int width = 0, height = 0;
	std::vector<uint8_t> index;
	std::vector<uint8_t> opaque;
	std::vector<float> depth;
	std::vector<uint8_t> rgba; // filled by AddArtRelief, width x height (padded to 32 in the atlas)
};

std::vector<ArtComposite> ArtComposites;

/** World point of a classic screen point at depth u. */
Vec3 ArtWorldPoint(float sx, float sy, float u)
{
	return { (u + sx / 32.0F) * 0.5F, (16.0F * u - sy) / kArtRowsPerUnit, (u - sx / 32.0F) * 0.5F };
}

/**
 * Paints the pieces of `cells` (one building) into a composite in the classic order (back rows first) and lays it on a
 * relief mesh in `art`. Only what a piece draws above its floor diamond is building art: the diamond is ground, which
 * the ground mesh shows.
 */
void AddArtRelief(MeshBuilder &art, const std::vector<std::pair<int, int>> &cells)
{
	if (cells.empty())
		return;
	const int pieceHeight = GetPieceImage(dPiece[cells.front().first][cells.front().second]).height;
	int minX = 1 << 30, minY = 1 << 30, maxX = -(1 << 30), maxY = -(1 << 30);
	for (const auto &[i, j] : cells) {
		const int left = (i - j) * 32 - 32;
		const int bottom = (i + j + 2) * 16;
		minX = std::min(minX, left);
		maxX = std::max(maxX, left + 64);
		minY = std::min(minY, bottom - pieceHeight);
		maxY = std::max(maxY, bottom - 16);
	}
	ArtComposite composite;
	composite.x0 = minX;
	composite.y0 = minY;
	composite.width = maxX - minX;
	composite.height = maxY - minY;
	const size_t pixels = static_cast<size_t>(composite.width) * composite.height;
	composite.index.assign(pixels, 0);
	composite.opaque.assign(pixels, 0);
	composite.depth.assign(pixels, 0.0F);
	std::vector<std::pair<int, int>> order = cells;
	std::stable_sort(order.begin(), order.end(), [](const auto &a, const auto &b) { return a.first + a.second < b.first + b.second; });
	bool any = false;
	for (const auto &[i, j] : order) {
		const PieceImage &image = GetPieceImage(dPiece[i][j]);
		const int left = (i - j) * 32 - 32 - composite.x0;
		const int top = (i + j + 2) * 16 - image.height - composite.y0;
		for (int r = 0; r < image.height; ++r) {
			const float rowUp = static_cast<float>(image.height - 1 - r) + 0.5F; // pixel centre, rows up from the bottom
			const int y = top + r;
			if (y < 0 || y >= composite.height)
				continue;
			for (int c = 0; c < 64; ++c) {
				const size_t p = static_cast<size_t>(r) * 64 + c;
				if (image.opaque[p] == 0)
					continue;
				const float column = static_cast<float>(c) + 0.5F;
				// The back edges of the diamond: row 16 at the outer columns, row 32 at the top corner.
				const float foot = c < 32 ? 16.0F + column * 0.5F : 16.0F + (64.0F - column) * 0.5F;
				if (rowUp < foot)
					continue;
				const int x = left + c;
				if (x < 0 || x >= composite.width)
					continue;
				const size_t q = static_cast<size_t>(y) * composite.width + x;
				const float sx = static_cast<float>(composite.x0 + x) + 0.5F;
				composite.index[q] = image.index[p];
				composite.opaque[q] = 1;
				composite.depth[q] = c < 32 ? 2.0F * static_cast<float>(i) - sx / 32.0F : 2.0F * static_cast<float>(j) + sx / 32.0F;
				any = true;
			}
		}
	}
	if (!any)
		return;

	// Relief vertices on a kArtBlock grid: the mean depth of the art within one block around them, never below the ground.
	const int gw = composite.width / kArtBlock + 1;
	const int gh = composite.height / kArtBlock + 2;
	std::vector<float> vertexDepth(static_cast<size_t>(gw) * gh, 0.0F);
	std::vector<uint8_t> vertexKnown(static_cast<size_t>(gw) * gh, 0);
	for (int gy = 0; gy < gh; ++gy) {
		for (int gx = 0; gx < gw; ++gx) {
			double sum = 0.0;
			int count = 0;
			for (int y = std::max(0, (gy - 1) * kArtBlock); y < std::min(composite.height, (gy + 1) * kArtBlock); ++y) {
				for (int x = std::max(0, (gx - 1) * kArtBlock); x < std::min(composite.width, (gx + 1) * kArtBlock); ++x) {
					const size_t q = static_cast<size_t>(y) * composite.width + x;
					if (composite.opaque[q] != 0) {
						sum += composite.depth[q];
						++count;
					}
				}
			}
			if (count == 0)
				continue;
			const float sy = static_cast<float>(composite.y0 + gy * kArtBlock);
			const size_t g = static_cast<size_t>(gy) * gw + gx;
			vertexDepth[g] = std::max(static_cast<float>(sum / count), sy / 16.0F);
			vertexKnown[g] = 1;
		}
	}
	const int slot = static_cast<int>(ArtComposites.size());
	const uint32_t flags = PackVertexFlags(kVertexTownArt | kVertexBillboard);
	// The relief faces the iso camera; its lighting normal is straight up, like the ground: the 2D town lights a house
	// exactly like the ground it stands on (the art carries its own shading), and both sides of a house the same.
	const Vec3 up { 0.0F, 1.0F, 0.0F };
	const auto corner = [&](int gx, int gy) {
		const float sx = static_cast<float>(composite.x0 + gx * kArtBlock);
		const float sy = static_cast<float>(composite.y0 + gy * kArtBlock);
		return ArtWorldPoint(sx, sy, vertexDepth[static_cast<size_t>(gy) * gw + gx]);
	};
	for (int by = 0; by * kArtBlock < composite.height; ++by) {
		for (int bx = 0; bx * kArtBlock < composite.width; ++bx) {
			bool shown = false;
			for (int y = by * kArtBlock; y < std::min(composite.height, (by + 1) * kArtBlock) && !shown; ++y) {
				for (int x = bx * kArtBlock; x < std::min(composite.width, (bx + 1) * kArtBlock) && !shown; ++x)
					shown = composite.opaque[static_cast<size_t>(y) * composite.width + x] != 0;
			}
			if (!shown)
				continue;
			// A block with art has art within one block of each of its corners.
			if (vertexKnown[static_cast<size_t>(by) * gw + bx] == 0 || vertexKnown[static_cast<size_t>(by) * gw + bx + 1] == 0
			    || vertexKnown[static_cast<size_t>(by + 1) * gw + bx] == 0 || vertexKnown[static_cast<size_t>(by + 1) * gw + bx + 1] == 0)
				continue;
			const float s0 = static_cast<float>(bx * kArtBlock);
			const float t0 = static_cast<float>(by * kArtBlock);
			const float s1 = s0 + static_cast<float>(kArtBlock);
			const float t1 = t0 + static_cast<float>(kArtBlock);
			const Vec3 p00 = corner(bx, by);
			const Vec3 p10 = corner(bx + 1, by);
			const Vec3 p11 = corner(bx + 1, by + 1);
			const Vec3 p01 = corner(bx, by + 1);
			art.Triangle(p00, p10, p11, s0, t0, s1, t0, s1, t1, up, SlotKind::Art, slot, flags);
			art.Triangle(p00, p11, p01, s0, t0, s1, t1, s0, t1, up, SlotKind::Art, slot, flags);
		}
	}
	const auto &palette = CurrentArtPalette();
	composite.rgba.assign(pixels * 4, 0);
	for (size_t q = 0; q < pixels; ++q) {
		if (composite.opaque[q] == 0)
			continue;
		const auto &color = palette[composite.index[q]];
		composite.rgba[q * 4 + 0] = color[0];
		composite.rgba[q * 4 + 1] = color[1];
		composite.rgba[q * 4 + 2] = color[2];
		composite.rgba[q * 4 + 3] = 255;
	}
	composite.index.clear();
	composite.opaque.clear();
	composite.depth.clear();
	ArtComposites.push_back(std::move(composite));
}

void BuildTown(LevelGeometry &geometry, MeshBuilder &mesh, MeshBuilder &art)
{
	PieceStatsCache.clear();
	if (const char *dump = std::getenv("DVL_W3D_DUMP_TOWN"); dump != nullptr && dump[0] != '\0')
		DumpTownCells(dump);
	const auto index = [](int x, int y) { return static_cast<size_t>(y) * kGridSize + x; };
	std::vector<int> pieceHeight(static_cast<size_t>(kGridSize) * kGridSize, 0);
	for (int y = 0; y < kGridSize; ++y)
		for (int x = 0; x < kGridSize; ++x)
			pieceHeight[index(x, y)] = tile_geometry::GetPieceGeometry(dPiece[x][y]).heightPx;
	const auto solid = [&](int x, int y) { return TileHasAny({ x, y }, TileProperties::Solid); };
	const auto flatSolid = [&](int x, int y) {
		return solid(x, y) && pieceHeight[index(x, y)] < 40 && tile_geometry::GetPieceGeometry(dPiece[x][y]).faces == 0;
	};

	// The creek: long, thin chains of flat solid cells.
	std::vector<int> flatGroupOf;
	const std::vector<TownGroup> flatGroups = FindGroups(flatSolid, flatGroupOf);
	std::vector<uint8_t> water(static_cast<size_t>(kGridSize) * kGridSize, 0);
	for (const TownGroup &group : flatGroups) {
		const int w = group.maxX - group.minX + 1;
		const int h = group.maxY - group.minY + 1;
		if (group.cells.size() >= 12 && static_cast<float>(group.cells.size()) < 0.3F * static_cast<float>(w * h) && std::max(w, h) >= 12) {
			for (const auto &[x, y] : group.cells)
				water[index(x, y)] = 1;
		}
	}

	// Houses: compact groups of the other solid cells with tall facade pieces.
	std::vector<int> groupOf;
	std::vector<TownGroup> groups = FindGroups([&](int x, int y) { return solid(x, y) && water[index(x, y)] == 0; }, groupOf);
	std::vector<uint8_t> isHouse(groups.size(), 0);
	int cathedral = -1;
	size_t cathedralSize = 0;
	for (size_t i = 0; i < groups.size(); ++i) {
		const TownGroup &group = groups[i];
		const int w = group.maxX - group.minX + 1;
		const int h = group.maxY - group.minY + 1;
		int facades = 0;
		for (const auto &[x, y] : group.cells) {
			if (pieceHeight[index(x, y)] >= 90 && tile_geometry::GetPieceGeometry(dPiece[x][y]).faces != 0)
				++facades;
		}
		if (group.cells.size() >= 6 && w >= 2 && h >= 2 && w <= 16 && h <= 16 && facades >= 2
		    && static_cast<float>(group.cells.size()) >= 0.6F * static_cast<float>(w * h)) {
			isHouse[i] = 1;
			if (group.cells.size() > cathedralSize) {
				cathedralSize = group.cells.size();
				cathedral = static_cast<int>(i);
			}
		}
	}
	// The cathedral: the thick body (cells with at least 6 of 8 neighbours in the group, so the fences joined to it
	// drop out) of the largest solid group next to the dungeon entrance (the town trigger to level 1), flood filled
	// from the cell closest to the entrance.
	cathedral = -1;
	for (int t = 0; t < numtrigs && cathedral < 0; ++t) {
		if (trigs[t]._tmsg != WM_DIABNEXTLVL)
			continue;
		const Point at = trigs[t].position;
		int best = -1;
		size_t bestSize = 0;
		for (int y = std::max(0, at.y - 6); y <= std::min(kGridSize - 1, at.y + 6); ++y) {
			for (int x = std::max(0, at.x - 6); x <= std::min(kGridSize - 1, at.x + 6); ++x) {
				const int g = groupOf[index(x, y)];
				if (g >= 0 && groups[static_cast<size_t>(g)].cells.size() > bestSize) {
					bestSize = groups[static_cast<size_t>(g)].cells.size();
					best = g;
				}
			}
		}
		if (best < 0)
			break;
		const auto inBest = [&](int x, int y) { return x >= 0 && y >= 0 && x < kGridSize && y < kGridSize && groupOf[index(x, y)] == best; };
		const auto thick = [&](int x, int y) {
			if (!inBest(x, y))
				return false;
			int n = 0;
			for (int dy = -1; dy <= 1; ++dy)
				for (int dx = -1; dx <= 1; ++dx)
					n += (dx != 0 || dy != 0) && inBest(x + dx, y + dy) ? 1 : 0;
			return n >= 6;
		};
		int seedX = -1;
		int seedY = -1;
		int seedDistance = 1 << 30;
		for (const auto &[x, y] : groups[static_cast<size_t>(best)].cells) {
			const int d = (x - at.x) * (x - at.x) + (y - at.y) * (y - at.y);
			if (d < seedDistance && thick(x, y)) {
				seedDistance = d;
				seedX = x;
				seedY = y;
			}
		}
		if (seedX < 0)
			break;
		std::vector<int> bodyOf;
		std::vector<TownGroup> bodies = FindGroups([&](int x, int y) { return thick(x, y); }, bodyOf);
		const int bodyId = bodyOf[index(seedX, seedY)];
		if (bodyId < 0 || bodies[static_cast<size_t>(bodyId)].cells.size() < 20)
			break;
		cathedral = static_cast<int>(groups.size());
		for (const auto &[x, y] : bodies[static_cast<size_t>(bodyId)].cells)
			groupOf[index(x, y)] = cathedral;
		groups.push_back(std::move(bodies[static_cast<size_t>(bodyId)]));
		isHouse.push_back(1);
	}
	if (std::getenv("DVL_W3D_DUMP_TOWN") != nullptr) {
		for (size_t i = 0; i < groups.size(); ++i) {
			if (isHouse[i] != 0)
				Log("render3d: town house {} cells, {}..{} x {}..{}{}", groups[i].cells.size(), groups[i].minX, groups[i].maxX, groups[i].minY, groups[i].maxY,
				    static_cast<int>(i) == cathedral ? " (cathedral)" : "");
		}
	}

	std::vector<uint8_t> cardCell(static_cast<size_t>(kGridSize) * kGridSize, 0);
	// Small solid objects: compact solid groups (at most eight pieces within 4 x 4; not houses, not the creek) with art
	// above the floor: the well, boulders and their clusters, gravestones.
	std::vector<uint8_t> smallObject(groups.size(), 0);
	for (size_t i = 0; i < groups.size(); ++i) {
		if (isHouse[i] != 0 || groups[i].cells.size() > 8 || groups[i].maxX - groups[i].minX >= 4 || groups[i].maxY - groups[i].minY >= 4)
			continue;
		for (const auto &[x, y] : groups[i].cells)
			smallObject[i] = smallObject[i] != 0 || pieceHeight[index(x, y)] > 32 ? 1 : 0;
	}
	std::vector<int> smallObjectOf(static_cast<size_t>(kGridSize) * kGridSize, -1);
	// Built houses (not replaced by a placed mesh) stand on their volume: the cells of the group inside a full 2 x 2 block
	// of it. One-cell fences and posts joined to a house stay cut-out cards like the town's other fences; the roofs cover
	// only volume cells, so no towner or hero tile is ever under one.
	// Towners stand in porches and open forges that the iso art draws under an eave, some on solid tiles (Griswold at
	// his anvil): the cells around them stay open, so no wall or roof is built over or right next to a towner.
	std::vector<uint8_t> townerSpace(static_cast<size_t>(kGridSize) * kGridSize, 0);
	for (const Towner &towner : Towners) {
		for (int dy = -1; dy <= 1; ++dy)
			for (int dx = -1; dx <= 1; ++dx) {
				const int x = towner.position.x + dx;
				const int y = towner.position.y + dy;
				if (x >= 0 && y >= 0 && x < kGridSize && y < kGridSize)
					townerSpace[index(x, y)] = 1;
			}
	}
	std::vector<uint8_t> houseVolume(static_cast<size_t>(kGridSize) * kGridSize, 0);
	std::vector<uint8_t> replacedHouse(groups.size(), 0);
	for (size_t i = 0; i < groups.size(); ++i) {
		if (isHouse[i] == 0)
			continue;
		replacedHouse[i] = std::any_of(groups[i].cells.begin(), groups[i].cells.end(), [](const auto &cell) { return PlacementHidesTile(cell.first, cell.second); }) ? 1 : 0;
		if (replacedHouse[i] != 0)
			continue;
		const auto same = [&](int x, int y) {
			return x >= 0 && y >= 0 && x < kGridSize && y < kGridSize && groupOf[index(x, y)] == static_cast<int>(i) && townerSpace[index(x, y)] == 0;
		};
		for (const auto &[x, y] : groups[i].cells) {
			for (int oy = -1; oy <= 0 && houseVolume[index(x, y)] == 0; ++oy)
				for (int ox = -1; ox <= 0; ++ox)
					if (same(x + ox, y + oy) && same(x + ox + 1, y + oy) && same(x + ox, y + oy + 1) && same(x + ox + 1, y + oy + 1)) {
						houseVolume[index(x, y)] = 1;
						break;
					}
		}
	}
	// Around a built house (its bounding box and the ring of cells next to its volume) the iso art spills eaves, roof
	// corners and gable ends onto other cells. As cards those stood up through or beside the roof as dark slanted panels,
	// so there only building art is dropped (IsBuildingArt); trees, signs and fences stay.
	std::vector<uint8_t> nearBuiltHouse(static_cast<size_t>(kGridSize) * kGridSize, 0);
	for (size_t i = 0; i < groups.size(); ++i) {
		if (isHouse[i] == 0 || replacedHouse[i] != 0)
			continue;
		for (int y = groups[i].minY; y <= groups[i].maxY; ++y)
			for (int x = groups[i].minX; x <= groups[i].maxX; ++x)
				nearBuiltHouse[index(x, y)] = 1;
		for (const auto &[x, y] : groups[i].cells) {
			if (houseVolume[index(x, y)] == 0)
				continue;
			for (int dy = -1; dy <= 1; ++dy)
				for (int dx = -1; dx <= 1; ++dx)
					if (x + dx >= 0 && y + dy >= 0 && x + dx < kGridSize && y + dy < kGridSize)
						nearBuiltHouse[index(x + dx, y + dy)] = 1;
		}
	}
	for (int y = 0; y < kGridSize; ++y) {
		for (int x = 0; x < kGridSize; ++x) {
			const uint16_t piece = dPiece[x][y];
			const float fx = static_cast<float>(x);
			const float fz = static_cast<float>(y);
			geometry.occluders[index(x, y)] = 0;
			const int group = groupOf[index(x, y)];
			if (houseVolume[index(x, y)] != 0 || (group >= 0 && isHouse[static_cast<size_t>(group)] != 0 && replacedHouse[static_cast<size_t>(group)] != 0))
				continue;
			if (water[index(x, y)] != 0) {
				// Level with the ground: the creek cells include the painted banks, so lowering whole cells cut a per-cell
				// staircase (with dark bank faces) across the smooth painted channel.
				AddHorizontal(mesh, fx, fz, fx + 1.0F, fz + 1.0F, kWaterLevel, true, FloorSlot(piece), 1.0F, kVertexWater);
				continue;
			}
			AddHorizontal(mesh, fx, fz, fx + 1.0F, fz + 1.0F, 0.0F, true, FloorSlot(piece), 1.0F);
			// Cards for trees, the well, signs and fences; dense facade pieces left outside a house stay flat. A house a
			// placed mesh replaces spills its porch and awning art one row past its front (+x / +y) edge: as a card that
			// art stood next to the mesh as a dark wedge (the smithy's forge beside Griswold).
			const bool frontOfPlacedHouse = PlacementHidesTile(x - 1, y) || PlacementHidesTile(x, y - 1);
			const bool builtHouseArt = nearBuiltHouse[index(x, y)] != 0 && IsBuildingArt(piece);
			if (pieceHeight[index(x, y)] >= 56 && !PlacementHidesTile(x, y) && !frontOfPlacedHouse && !builtHouseArt) {
				const PieceStats &stats = CachedPieceStats(piece);
				if (tile_geometry::GetPieceGeometry(piece).faces == 0 || stats.density < 0.6F) {
					cardCell[index(x, y)] = 1;
					if (nearBuiltHouse[index(x, y)] != 0 && std::getenv("DVL_W3D_DUMP_TOWN") != nullptr)
						Log("render3d: card next to a house at {},{} (piece {})", x, y, piece);
				}
			}
			// Small solid objects (the well, boulders, stones) stand up as one card object, all their pieces.
			if (group >= 0 && smallObject[static_cast<size_t>(group)] != 0 && !PlacementHidesTile(x, y) && !frontOfPlacedHouse) {
				cardCell[index(x, y)] = 1;
				smallObjectOf[index(x, y)] = group;
			}
		}
	}
	// The original art of the built houses (AddArtRelief) covers the art region: the cells of a house group and around one
	// (nearBuiltHouse: eaves, porches and gables spill onto the cells next to the volume). Its pieces form one composite
	// per building (art cells within two cells of each other), cut-out objects wholly inside it included (their cards then
	// belong to the reconstructed houses), with the floor of the volume cells, which the built house covers otherwise. The
	// host shows it instead of the built houses while the camera keeps the art's angle.
	// Only for the town without an art pack's 3D houses: a pack that replaces any house keeps the town as it was.
	const bool townArt = std::getenv("DVL_W3D_NO_TOWN_ART") == nullptr
	    && std::none_of(replacedHouse.begin(), replacedHouse.end(), [](uint8_t replaced) { return replaced != 0; });
	std::vector<uint8_t> artRegion(static_cast<size_t>(kGridSize) * kGridSize, 0);
	std::vector<uint8_t> artCards(artRegion.size(), 0);
	for (int y = 0; y < kGridSize && townArt; ++y) {
		for (int x = 0; x < kGridSize; ++x) {
			const size_t at = index(x, y);
			const int group = groupOf[at];
			const bool builtGroup = group >= 0 && isHouse[static_cast<size_t>(group)] != 0 && replacedHouse[static_cast<size_t>(group)] == 0;
			artRegion[at] = (builtGroup || nearBuiltHouse[at] != 0) && !PlacementHidesTile(x, y) ? 1 : 0;
		}
	}
	if (std::getenv("DVL_W3D_NO_TOWN_CARDS") == nullptr) // debugging: the town without its cut-out objects
		AddTownCards(mesh, cardCell, pieceHeight, smallObjectOf, artRegion, artCards);
	ArtComposites.clear();
	if (townArt) {
		std::vector<uint8_t> artCell(static_cast<size_t>(kGridSize) * kGridSize, 0);
		for (int y = 0; y < kGridSize; ++y) {
			for (int x = 0; x < kGridSize; ++x) {
				const size_t at = index(x, y);
				if (artRegion[at] == 0)
					continue;
				if (houseVolume[at] != 0)
					AddHorizontal(art, static_cast<float>(x), static_cast<float>(y), static_cast<float>(x) + 1.0F, static_cast<float>(y) + 1.0F, 0.0F, true,
					    FloorSlot(dPiece[x][y]), 1.0F, kVertexTownArt);
				if ((cardCell[at] == 0 || artCards[at] != 0) && pieceHeight[at] > 0)
					artCell[at] = 1;
			}
		}
		std::vector<uint8_t> seen(artCell.size(), 0);
		for (int y = 0; y < kGridSize; ++y) {
			for (int x = 0; x < kGridSize; ++x) {
				if (artCell[index(x, y)] == 0 || seen[index(x, y)] != 0)
					continue;
				std::vector<std::pair<int, int>> building { { x, y } };
				seen[index(x, y)] = 1;
				for (size_t k = 0; k < building.size(); ++k) {
					const auto [cx, cy] = building[k];
					for (int dy = -2; dy <= 2; ++dy) {
						for (int dx = -2; dx <= 2; ++dx) {
							const int nx = cx + dx;
							const int ny = cy + dy;
							if (nx < 0 || ny < 0 || nx >= kGridSize || ny >= kGridSize || artCell[index(nx, ny)] == 0 || seen[index(nx, ny)] != 0)
								continue;
							seen[index(nx, ny)] = 1;
							building.emplace_back(nx, ny);
						}
					}
				}
				AddArtRelief(art, building);
			}
		}
		if (std::getenv("DVL_W3D_DUMP_TOWN") != nullptr)
			Log("render3d: town art {} buildings, {} triangles", ArtComposites.size(), art.indices.size() / 3);
	}
	for (size_t i = 0; i < groups.size(); ++i) {
		if (isHouse[i] == 0)
			continue;
		if (replacedHouse[i] == 0) {
			const size_t first = mesh.vertices.size();
			BuildHouse(geometry, mesh, groups[i], houseVolume, static_cast<int>(i) == cathedral);
			for (size_t v = first; v < mesh.vertices.size(); ++v)
				mesh.vertices[v].vertex.flags |= kVertexTownHouse;
			continue;
		}
		// A placed mesh that hides any cell of a house replaces the whole block; its ground is drawn instead.
		for (const auto &[x, y] : groups[i].cells)
			AddHorizontal(mesh, static_cast<float>(x), static_cast<float>(y), static_cast<float>(x) + 1.0F, static_cast<float>(y) + 1.0F, 0.0F, true, FloorSlot(dPiece[x][y]), 1.0F);
	}
	if (std::getenv("DVL_W3D_DUMP_TOWN") != nullptr) {
		// Towners must never stand under a built roof: roofs cover volume cells only.
		for (const Towner &towner : Towners) {
			const Point p = towner.position;
			const bool covered = p.x >= 0 && p.y >= 0 && p.x < kGridSize && p.y < kGridSize && houseVolume[index(p.x, p.y)] != 0;
			Log("render3d: towner {} at {},{}{}", static_cast<int>(towner._ttype), p.x, p.y, covered ? " UNDER A BUILT HOUSE" : "");
		}
	}
}

/**
 * Mips 1..5 of the atlas: 2 x 2 box filter of the opaque texels (alpha = coverage >= half). Every slot is 32-texel
 * aligned with sizes in multiples of 32, so no 2 x 2 block of these levels straddles two slots: no bleeding.
 */
void BuildAtlasMips(LevelGeometry &geometry)
{
	geometry.atlasMips.clear();
	geometry.atlasMips.reserve(5); // `previous` points into it
	if (std::getenv("DVL_W3D_NO_MIPS") != nullptr)
		return;
	uint32_t width = geometry.atlasWidth;
	uint32_t height = geometry.atlasHeight;
	const std::vector<uint8_t> *previous = &geometry.atlasRgba;
	for (int level = 1; level <= 5 && width >= 2 && height >= 2 && (height % 2) == 0; ++level) {
		const uint32_t w = width / 2;
		const uint32_t h = height / 2;
		std::vector<uint8_t> next(static_cast<size_t>(w) * h * 4, 0);
		const std::vector<uint8_t> &src = *previous;
		for (uint32_t y = 0; y < h; ++y) {
			for (uint32_t x = 0; x < w; ++x) {
				uint32_t sum[3] = { 0, 0, 0 };
				uint32_t n = 0;
				for (uint32_t k = 0; k < 4; ++k) {
					const size_t o = ((static_cast<size_t>(y) * 2 + (k >> 1)) * width + x * 2 + (k & 1)) * 4;
					if (src[o + 3] < 128)
						continue;
					sum[0] += src[o];
					sum[1] += src[o + 1];
					sum[2] += src[o + 2];
					++n;
				}
				if (n == 0)
					continue;
				uint8_t *d = &next[(static_cast<size_t>(y) * w + x) * 4];
				d[0] = static_cast<uint8_t>(sum[0] / n);
				d[1] = static_cast<uint8_t>(sum[1] / n);
				d[2] = static_cast<uint8_t>(sum[2] / n);
				d[3] = n >= 2 ? 255 : 0;
			}
		}
		geometry.atlasMips.push_back(std::move(next));
		previous = &geometry.atlasMips.back();
		width = w;
		height = h;
	}
}

void ResolveAtlas(LevelGeometry &geometry, MeshBuilder &mesh)
{
	// Default wall slot for faces without art of their own: the most used one.
	int defaultWall = -1;
	int bestUse = -1;
	for (size_t i = 0; i < Atlas.wallUse.size(); ++i) {
		if (Atlas.wallUse[i] > bestUse) {
			bestUse = Atlas.wallUse[i];
			defaultWall = static_cast<int>(i);
		}
	}
	if (defaultWall < 0) {
		Slot slot;
		slot.kind = SlotKind::Wall;
		slot.textureHeight = Atlas.wallSlotHeight;
		slot.rgba.assign(static_cast<size_t>(32) * Atlas.wallSlotHeight * 4, 0);
		for (size_t o = 0; o < slot.rgba.size(); o += 4) {
			slot.rgba[o] = 40;
			slot.rgba[o + 1] = 36;
			slot.rgba[o + 2] = 32;
			slot.rgba[o + 3] = 255;
		}
		Atlas.walls.push_back(std::move(slot));
		defaultWall = static_cast<int>(Atlas.walls.size()) - 1;
	}

	const int squareRows = (static_cast<int>(Atlas.squares.size()) + kSlotsPerRow - 1) / kSlotsPerRow;
	const int squareRegion = squareRows * 32;
	constexpr int CardsPerRow = kAtlasWidth / 64;
	const int cardRowsWanted = (static_cast<int>(Atlas.cards.size()) + CardsPerRow - 1) / CardsPerRow;
	const int maxWallRows = std::max(1, (kMaxAtlasHeight - squareRegion) / Atlas.wallSlotHeight);
	const int wallRows = std::min((static_cast<int>(Atlas.walls.size()) + kSlotsPerRow - 1) / kSlotsPerRow, maxWallRows);
	const int wallRegion = wallRows * Atlas.wallSlotHeight;
	const int cardRows = std::min(cardRowsWanted, std::max(0, (kMaxAtlasHeight - squareRegion - wallRegion) / Atlas.wallSlotHeight));
	const int cardRegion = cardRows * Atlas.wallSlotHeight;
	// Building composites (town art) on shelves below the cards, 32-aligned like every slot so the mips never mix two.
	// The region starts with a transparent 32 x 32 block: art that does not fit the atlas samples it and is cut out.
	const int artTop = squareRegion + wallRegion + cardRegion;
	std::vector<std::array<int, 2>> artOrigin(ArtComposites.size(), { -1, -1 });
	int artRegion = 0;
	if (!ArtComposites.empty()) {
		const auto align = [](int v) { return (v + 31) / 32 * 32; };
		int shelfX = 32;
		int shelfY = 0;
		int shelfHeight = 32;
		for (size_t i = 0; i < ArtComposites.size(); ++i) {
			const int w = align(ArtComposites[i].width);
			const int h = align(ArtComposites[i].height);
			if (w > kAtlasWidth)
				continue;
			if (shelfX + w > kAtlasWidth) {
				shelfY += shelfHeight;
				shelfX = 0;
				shelfHeight = 0;
			}
			if (artTop + shelfY + h > kMaxAtlasHeight) {
				Log("render3d: town art composite {} ({} x {}) does not fit the level atlas", i, ArtComposites[i].width, ArtComposites[i].height);
				continue;
			}
			artOrigin[i] = { shelfX, artTop + shelfY };
			shelfX += w;
			shelfHeight = std::max(shelfHeight, h);
		}
		artRegion = shelfY + shelfHeight;
	}
	const int height = std::max(32, artTop + artRegion);
	geometry.atlasWidth = kAtlasWidth;
	geometry.atlasHeight = static_cast<uint32_t>(height);
	geometry.atlasRgba.assign(static_cast<size_t>(kAtlasWidth) * height * 4, 0);
	const int wallCapacity = wallRows * kSlotsPerRow;
	const int cardCapacity = cardRows * CardsPerRow;

	const auto slotOrigin = [&](SlotKind kind, int slot, int &ox, int &oy) {
		if (kind == SlotKind::Square) {
			ox = (slot % kSlotsPerRow) * kSlotWidth;
			oy = (slot / kSlotsPerRow) * 32;
		} else if (kind == SlotKind::Card && slot >= 0 && slot < cardCapacity) {
			ox = (slot % CardsPerRow) * 64;
			oy = squareRegion + wallRegion + (slot / CardsPerRow) * Atlas.wallSlotHeight;
		} else {
			if (slot < 0 || slot >= wallCapacity || kind == SlotKind::Card)
				slot = std::min(defaultWall, wallCapacity - 1);
			ox = (slot % kSlotsPerRow) * kSlotWidth;
			oy = squareRegion + (slot / kSlotsPerRow) * Atlas.wallSlotHeight;
		}
	};
	const auto blit = [&](const Slot &slot, int ox, int oy, int width, int rows) {
		for (int r = 0; r < rows; ++r) {
			std::memcpy(&geometry.atlasRgba[(static_cast<size_t>(oy + r) * kAtlasWidth + ox) * 4], &slot.rgba[static_cast<size_t>(r) * width * 4], static_cast<size_t>(width) * 4);
		}
	};
	for (size_t i = 0; i < Atlas.squares.size(); ++i) {
		int ox, oy;
		slotOrigin(SlotKind::Square, static_cast<int>(i), ox, oy);
		blit(Atlas.squares[i], ox, oy, 32, 32);
	}
	for (size_t i = 0; i < Atlas.walls.size() && static_cast<int>(i) < wallCapacity; ++i) {
		int ox, oy;
		slotOrigin(SlotKind::Wall, static_cast<int>(i), ox, oy);
		blit(Atlas.walls[i], ox, oy, 32, Atlas.wallSlotHeight);
	}
	for (size_t i = 0; i < Atlas.cards.size() && static_cast<int>(i) < cardCapacity; ++i) {
		int ox, oy;
		slotOrigin(SlotKind::Card, static_cast<int>(i), ox, oy);
		blit(Atlas.cards[i], ox, oy, 64, Atlas.wallSlotHeight);
	}
	for (size_t i = 0; i < ArtComposites.size(); ++i) {
		const ArtComposite &art = ArtComposites[i];
		if (artOrigin[i][0] < 0)
			continue;
		for (int r = 0; r < art.height; ++r) {
			std::memcpy(&geometry.atlasRgba[(static_cast<size_t>(artOrigin[i][1] + r) * kAtlasWidth + artOrigin[i][0]) * 4], &art.rgba[static_cast<size_t>(r) * art.width * 4],
			    static_cast<size_t>(art.width) * 4);
		}
	}
	BuildAtlasMips(geometry);

	geometry.vertices.clear();
	geometry.vertices.reserve(mesh.vertices.size());
	const float invW = 1.0F / static_cast<float>(kAtlasWidth);
	const float invH = 1.0F / static_cast<float>(height);
	for (PendingVertex &pending : mesh.vertices) {
		int slot = pending.slot;
		if (pending.kind == SlotKind::Wall && slot < 0)
			slot = defaultWall;
		if (pending.kind == SlotKind::Art) {
			// A composite that did not fit samples the transparent block at the start of the art region.
			const bool placed = slot >= 0 && static_cast<size_t>(slot) < artOrigin.size() && artOrigin[static_cast<size_t>(slot)][0] >= 0;
			WorldVertex vertex = pending.vertex;
			vertex.u = (placed ? static_cast<float>(artOrigin[static_cast<size_t>(slot)][0]) + pending.s : 16.0F) * invW;
			vertex.v = (placed ? static_cast<float>(artOrigin[static_cast<size_t>(slot)][1]) + pending.t : static_cast<float>(artTop) + 16.0F) * invH;
			geometry.vertices.push_back(vertex);
			continue;
		}
		int ox, oy;
		slotOrigin(pending.kind, slot, ox, oy);
		WorldVertex vertex = pending.vertex;
		vertex.u = (static_cast<float>(ox) + pending.s) * invW;
		vertex.v = (static_cast<float>(oy) + pending.t) * invH;
		geometry.vertices.push_back(vertex);
	}
	geometry.indices = std::move(mesh.indices);
}

/** Developer knob DVL_W3D_DUMP_ATLAS=<prefix>: writes <prefix>_<leveltype>.ppm (transparent texels magenta) per rebuild. */
void DumpAtlas(const LevelGeometry &geometry, const char *prefix)
{
	const std::string path = std::string(prefix) + "_" + std::to_string(static_cast<int>(leveltype)) + ".ppm";
	FILE *file = std::fopen(path.c_str(), "wb");
	if (file == nullptr)
		return;
	std::fprintf(file, "P6\n%u %u\n255\n", geometry.atlasWidth, geometry.atlasHeight);
	std::vector<uint8_t> row(static_cast<size_t>(geometry.atlasWidth) * 3);
	for (uint32_t y = 0; y < geometry.atlasHeight; ++y) {
		for (uint32_t x = 0; x < geometry.atlasWidth; ++x) {
			const uint8_t *p = &geometry.atlasRgba[(static_cast<size_t>(y) * geometry.atlasWidth + x) * 4];
			const bool clear = p[3] == 0;
			row[x * 3 + 0] = clear ? 255 : p[0];
			row[x * 3 + 1] = clear ? 0 : p[1];
			row[x * 3 + 2] = clear ? 255 : p[2];
		}
		std::fwrite(row.data(), 1, row.size(), file);
	}
	std::fclose(file);
}

} // namespace

void SetExternalDoorModels(uint32_t mask)
{
	mask &= 0xFFU;
	if (mask != ExternalDoorModels) {
		ExternalDoorModels = mask;
		ForceRebuild = true;
	}
}

void SetExternalDoorAperture(uint32_t family, float width, float height)
{
	if (family >= ExternalDoorApertures.size())
		return;
	// Native collision is one tile wide; the masonry never narrows below it or rises past the arch contract.
	const std::array<float, 2> aperture = width > 0.0F && height > 0.0F
	    ? std::array<float, 2> { std::clamp(width, 1.0F, 2.4F), std::clamp(height, 1.2F, fpv::kDungeonOpeningHeight) }
	    : std::array<float, 2> { 0.0F, 0.0F };
	if (aperture != ExternalDoorApertures[family]) {
		ExternalDoorApertures[family] = aperture;
		ForceRebuild = true;
	}
}

void InvalidateLevel()
{
	ForceRebuild = true;
}

void RefreshArtPalette()
{
	uint64_t hash = 0xCBF29CE484222325ULL;
	for (size_t i = 0; i < 256; ++i) {
		const SDL_Color &c = logical_palette[i];
		ArtPalette[i] = { c.r, c.g, c.b, 255 };
		hash = (hash ^ (static_cast<uint32_t>(c.r) | static_cast<uint32_t>(c.g) << 8 | static_cast<uint32_t>(c.b) << 16)) * 0x100000001B3ULL;
	}
	ArtPaletteHash = hash;
}

const std::array<std::array<uint8_t, 4>, 256> &CurrentArtPalette()
{
	return ArtPalette;
}

uint64_t CurrentArtPaletteHash()
{
	return ArtPaletteHash;
}

float FloorHeightOf(const LevelGeometry &geometry, float x, float z)
{
	const int cx = static_cast<int>(std::floor(x));
	const int cz = static_cast<int>(std::floor(z));
	if (cx < 0 || cz < 0 || cx >= kGridSize || cz >= kGridSize)
		return 0.0F;
	const size_t cell = static_cast<size_t>(cz) * kGridSize + cx;
	const int kind = geometry.stairs[cell];
	if (kind == 0)
		return 0.0F;
	const auto [dx, dy] = kStairDirs[geometry.stairsDir[cell] & 3];
	const float fx = x - static_cast<float>(cx);
	const float fz = z - static_cast<float>(cz);
	float progress = 0.0F;
	if (dx > 0) progress = fx;
	if (dx < 0) progress = 1.0F - fx;
	if (dy > 0) progress = fz;
	if (dy < 0) progress = 1.0F - fz;
	// A ramp through the middle of each tread (smooth for the camera).
	const float total = (kind > 0 ? kStairRiseUp : kStairRiseDown) * kStairSteps;
	return total * std::clamp(progress + 0.5F / kStairSteps, 1.0F / kStairSteps, 1.0F);
}

float OccluderTopAt(const LevelGeometry &geometry, int x, int y)
{
	if (x < 0 || y < 0 || x >= kGridSize || y >= kGridSize)
		return geometry.wallHeight;
	return static_cast<float>(geometry.occluders[static_cast<size_t>(y) * kGridSize + x] & 0xFFFF) / 1000.0F;
}

bool UpdateLevelGeometry(LevelGeometry &geometry)
{
	const auto started = std::chrono::steady_clock::now();
	if (pDungeonCels == nullptr || MicroTileLen == 0)
		return false;
	if (PollLevelPlacements())
		ForceRebuild = true;
	uint64_t hash = ComputeStateHash();
	// Colour cycling (caves, crypt, nest) rotates palette entries 1..31 every frame; it must not rebuild the level.
	if (leveltype == DTYPE_CAVES || leveltype == DTYPE_HELL || leveltype == DTYPE_NEST || leveltype == DTYPE_CRYPT) {
		const uint64_t paletteHash = ArtPaletteHash;
		ArtPaletteHash = 0;
		hash = ComputeStateHash();
		ArtPaletteHash = paletteHash;
	}
	if (!ForceRebuild && hash == LastStateHash && geometry.generation != 0)
		return false;
	const bool levelChanged = ForceRebuild || geometry.generation == 0;
	ForceRebuild = false;
	LastStateHash = hash;

	// Piece images depend on the tileset only; a level/palette change invalidates them.
	static uint64_t PieceImagesKey = 0;
	const uint64_t piecesKey = Mix(Mix(Mix(0, reinterpret_cast<uintptr_t>(pDungeonCels.get())), static_cast<uint64_t>(leveltype)), ArtPaletteHash);
	if (piecesKey != PieceImagesKey || levelChanged) {
		PieceImages.clear();
		PieceCardImages.clear();
		PieceImagesKey = piecesKey;
	}
	static uint64_t DoorLevelKey = 0;
	const uint64_t doorKey = Mix(Mix(currlevel, setlevel ? 1 : 0), static_cast<uint64_t>(leveltype));
	if (doorKey != DoorLevelKey) {
		DoorClosedSlot.clear();
		DoorLevelKey = doorKey;
	}

	geometry.isTown = leveltype == DTYPE_TOWN;
	geometry.stairs.fill(0);
	geometry.stairsDir.fill(0);
	geometry.wallHeight = fpv::kWallHeight;
	Atlas.Reset(std::clamp(static_cast<int>(MicroTileLen) / 2 * 32, 64, 256));
	MeshBuilder mesh;
	mesh.vertices.reserve(200000);
	mesh.indices.reserve(300000);
	ResolveLevelPlacements();
	MeshBuilder art;
	ArtComposites.clear();
	if (geometry.isTown)
		BuildTown(geometry, mesh, art);
	else
		BuildDungeon(geometry, mesh);
	// The town art goes after the whole main mesh (vertices and indices), so the main range stays exactly what it was.
	geometry.mainIndexCount = static_cast<uint32_t>(mesh.indices.size());
	if (!art.indices.empty()) {
		const auto base = static_cast<uint32_t>(mesh.vertices.size());
		mesh.vertices.insert(mesh.vertices.end(), art.vertices.begin(), art.vertices.end());
		for (const uint32_t i : art.indices)
			mesh.indices.push_back(base + i);
	}
	ApplyPlacementOccluders(geometry.occluders);
	ResolveAtlas(geometry, mesh);
	++geometry.generation;
	if (const char *dump = std::getenv("DVL_W3D_DUMP_ATLAS"); dump != nullptr && dump[0] != '\0')
		DumpAtlas(geometry, dump);
	Log("render3d: level mesh {} vertices, {} triangles, atlas {}x{} ({} squares, {} walls)", geometry.vertices.size(), geometry.indices.size() / 3,
	    geometry.atlasWidth, geometry.atlasHeight, Atlas.squares.size(), Atlas.walls.size());
	if (SurvivorsRollingActive() && std::getenv("DAMNATIONX_TRACE_ROLLING") != nullptr) {
		const auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - started).count();
		Log("rolling-render: origin {},{} mesh rebuild {} us", SurvivorsRollingOriginX(), SurvivorsRollingOriginY(), elapsed);
	}
	return true;
}

} // namespace render3d
} // namespace devilution
