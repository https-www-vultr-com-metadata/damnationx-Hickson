#include "levels/survivors_rolling_world.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <limits>
#include <map>
#include <utility>

namespace devilution {
namespace {

// C++ division rounds towards zero; world generation needs floor division to
// keep the pieces of negative-coordinate megatiles together across the origin.
constexpr int64_t FloorHalf(int64_t value)
{
	return value / 2 - (value < 0 && value % 2 != 0);
}
constexpr int64_t FloorDiv(int64_t value, int64_t divisor)
{
	return value / divisor - (value % divisor != 0 && (value < 0) != (divisor < 0));
}

uint64_t Mix(uint64_t value)
{
	value ^= value >> 30;
	value *= 0xbf58476d1ce4e5b9ULL;
	value ^= value >> 27;
	value *= 0x94d049bb133111ebULL;
	return value ^ (value >> 31);
}

uint64_t CellHash(uint32_t seed, int64_t x, int64_t y, uint64_t salt)
{
	return Mix(static_cast<uint64_t>(seed) ^ Mix(static_cast<uint64_t>(x) + salt)
	    ^ Mix(static_cast<uint64_t>(y) + 0x9e3779b97f4a7c15ULL + salt * 0x632be59bd9b4e019ULL));
}

// Smooth value noise in [0, 65536) on a lattice of `cell` units. Integer
// arithmetic keeps every platform and compiler on the same world.
int64_t ValueNoise(uint32_t seed, int64_t x, int64_t y, int64_t cell, uint64_t salt)
{
	const int64_t gx = FloorDiv(x, cell), gy = FloorDiv(y, cell);
	auto smooth = [](int64_t t) { return t * t * (3 * 65536 - 2 * t) / (65536LL * 65536LL); };
	const int64_t wx = smooth((x - gx * cell) * 65536 / cell);
	const int64_t wy = smooth((y - gy * cell) * 65536 / cell);
	auto lattice = [&](int64_t lx, int64_t ly) { return static_cast<int64_t>(CellHash(seed, lx, ly, salt) & 0xffff); };
	const int64_t a = lattice(gx, gy), b = lattice(gx + 1, gy), c = lattice(gx, gy + 1), d = lattice(gx + 1, gy + 1);
	const int64_t top = a + (b - a) * wx / 65536;
	const int64_t bottom = c + (d - c) * wx / 65536;
	return top + (bottom - top) * wy / 65536;
}

// Probabilities are 24-bit fractions of one.
constexpr uint32_t Chance(unsigned percent) { return static_cast<uint32_t>((uint64_t { percent } << 24) / 100); }

class ChunkRandom {
public:
	ChunkRandom(uint32_t seed, int64_t chunkX, int64_t chunkY)
	    : base_(CellHash(seed, chunkX, chunkY, 0x51ced2a7f00dULL))
	{
	}
	uint64_t Next() { return Mix(base_ + 0x9e3779b97f4a7c15ULL * ++counter_); }
	uint32_t Fraction() { return static_cast<uint32_t>(Next() >> 40); }
	bool Roll(uint32_t chance) { return Fraction() < chance; }
	unsigned Below(unsigned bound) { return static_cast<unsigned>(Next() % bound); }
	uint8_t Variant() { return static_cast<uint8_t>(Next() >> 56); }

private:
	uint64_t base_;
	uint64_t counter_ = 0;
};

// Ring offsets on a two-tile grid: any two entries are at least two tiles
// apart, so vignette members always leave a walkable gap between them.
constexpr std::array<std::array<int, 2>, 8> Ring { { { 2, 0 }, { 2, 2 }, { 0, 2 }, { -2, 2 }, { -2, 0 }, { -2, -2 }, { 0, -2 }, { 2, -2 } } };

enum class Vignette : uint8_t { Camp, Shrine, BarrelCache, Ossuary, AltarCourt, Treasure, Chapel };
constexpr std::array<std::pair<Vignette, unsigned>, 7> VignetteWeights { {
	{ Vignette::Camp, 3 },
	{ Vignette::Shrine, 2 },
	{ Vignette::BarrelCache, 2 },
	{ Vignette::Ossuary, 2 },
	{ Vignette::AltarCourt, 1 },
	{ Vignette::Treasure, 1 },
	{ Vignette::Chapel, 1 },
} };
constexpr unsigned VignetteWeightTotal = 12;

bool ValidChunk(int64_t chunkX, int64_t chunkY)
{
	constexpr int64_t margin = MAXDUNX * 2;
	return chunkX % 16 == 0 && chunkY % 16 == 0
	    && chunkX >= std::numeric_limits<int64_t>::min() + margin && chunkX <= std::numeric_limits<int64_t>::max() - margin
	    && chunkY >= std::numeric_limits<int64_t>::min() + margin && chunkY <= std::numeric_limits<int64_t>::max() - margin;
}

} // namespace

std::optional<SurvivorsWorldOrigin> SurvivorsRollingChestPosition(uint32_t seed, int64_t chunkX, int64_t chunkY)
{
	if (!ValidChunk(chunkX, chunkY)) return std::nullopt;
	const uint64_t hash = Mix(static_cast<uint64_t>(seed) ^ Mix(static_cast<uint64_t>(chunkX))
	    ^ Mix(static_cast<uint64_t>(chunkY) + 0x52dce729ULL));
	if (hash % 5 != 0) return std::nullopt;
	return SurvivorsWorldOrigin { chunkX + 4 + static_cast<int64_t>((hash >> 8) % 8),
	    chunkY + 4 + static_cast<int64_t>((hash >> 16) % 8) };
}

SurvivorsRollingChunkProps SurvivorsRollingChunkScenery(uint32_t seed, int64_t chunkX, int64_t chunkY)
{
	SurvivorsRollingChunkProps result;
	if (!ValidChunk(chunkX, chunkY)) return result;
	ChunkRandom random(seed, chunkX, chunkY);
	auto add = [&](int64_t x, int64_t y, SurvivorsPropRole role) {
		if (result.count == SurvivorsRollingChunkProps::Capacity) return;
		result.props[result.count++] = { chunkX + x, chunkY + y, role, random.Variant() };
	};
	// As in d2survivors' scenery cells, a low-frequency field (three chunks per
	// lattice step) modulates density: dense ruins alternate with open ground
	// where swarms can be fought without props in the way.
	const int64_t density = ValueNoise(seed, chunkX / 16, chunkY / 16, 3, 0x7a11);
	const uint32_t vignetteChance = Chance(10) + static_cast<uint32_t>(density * Chance(42) / 65536);
	const int64_t centreX = 5 + random.Below(6), centreY = 5 + random.Below(6);
	auto ring = [&](unsigned index, SurvivorsPropRole role) {
		const auto &offset = Ring[index % Ring.size()];
		add(centreX + offset[0], centreY + offset[1], role);
	};
	if (random.Roll(vignetteChance)) {
		unsigned pick = random.Below(VignetteWeightTotal);
		Vignette vignette = Vignette::Camp;
		for (const auto &[kind, weight] : VignetteWeights) {
			if (pick < weight) {
				vignette = kind;
				break;
			}
			pick -= weight;
		}
		const unsigned start = random.Below(8);
		switch (vignette) {
		case Vignette::Camp: // a fire ringed by debris
			add(centreX, centreY, random.Roll(Chance(50)) ? SurvivorsPropRole::Firepit : SurvivorsPropRole::Brazier);
			for (unsigned i = 0, n = 1 + random.Below(2); i < n; ++i)
				ring(start + i * 3, random.Roll(Chance(60)) ? SurvivorsPropRole::Bones : SurvivorsPropRole::Barrel);
			break;
		case Vignette::Shrine: // a shrine between two candles
			add(centreX, centreY, SurvivorsPropRole::Shrine);
			ring(7, SurvivorsPropRole::Candle);
			ring(3, SurvivorsPropRole::Candle);
			if (random.Roll(Chance(40))) ring(1, SurvivorsPropRole::Bones);
			break;
		case Vignette::BarrelCache:
			if (random.Roll(Chance(60))) add(centreX, centreY, SurvivorsPropRole::Chest);
			for (unsigned i = 0, n = 2 + random.Below(2); i < n; ++i)
				ring(start + i, SurvivorsPropRole::Barrel);
			break;
		case Vignette::Ossuary:
			add(centreX, centreY, SurvivorsPropRole::Candle);
			for (unsigned i = 0; i < 2; ++i)
				ring(start + i * 3, SurvivorsPropRole::Bones);
			break;
		case Vignette::AltarCourt:
			add(centreX, centreY, SurvivorsPropRole::Altar);
			ring(1, SurvivorsPropRole::Brazier);
			ring(5, SurvivorsPropRole::Brazier);
			break;
		case Vignette::Treasure:
			add(centreX, centreY, SurvivorsPropRole::Chest);
			ring(7, SurvivorsPropRole::Candle);
			ring(3, SurvivorsPropRole::Candle);
			if (random.Roll(Chance(50))) ring(1 + 4 * (start % 2), SurvivorsPropRole::Bones); // never a candle's slot
			break;
		case Vignette::Chapel:
			add(centreX, centreY, SurvivorsPropRole::Brazier);
			ring(4, SurvivorsPropRole::Banner);
			ring(5, SurvivorsPropRole::Banner);
			ring(6, SurvivorsPropRole::Banner);
			ring(1, SurvivorsPropRole::Candle);
			break;
		}
	} else if (random.Roll(Chance(55))) {
		const unsigned pick = random.Below(100);
		const SurvivorsPropRole role = pick < 25 ? SurvivorsPropRole::Chest
		    : pick < 55                         ? SurvivorsPropRole::Brazier
		    : pick < 75                         ? SurvivorsPropRole::Firepit
		    : pick < 90                         ? SurvivorsPropRole::Bones
		                                        : SurvivorsPropRole::Barrel;
		add(centreX, centreY, role);
	}
	// Independent finds along the edge away from the vignette, at least four
	// tiles from any of its members: a chest in roughly every third chunk, a
	// lone shrine in roughly every fourteenth, and a waymark fire in about two
	// of five so the open field has a light pool every dozen or so tiles
	// (d2survivors scatters light props on their own coarse grid the same way).
	const int64_t farX = centreX <= 7 ? 13 + random.Below(2) : 1 + random.Below(2);
	const int64_t nearY = centreY <= 7 ? 13 + random.Below(2) : 1 + random.Below(2);
	const int64_t otherY = nearY >= 13 ? 1 + random.Below(2) : 13 + random.Below(2);
	const int64_t middleY = 7 + random.Below(2);
	if (random.Roll(Chance(30))) add(farX, nearY, SurvivorsPropRole::Chest);
	if (random.Roll(Chance(7))) add(farX, otherY, SurvivorsPropRole::Shrine);
	if (random.Roll(Chance(40))) add(farX, middleY, random.Roll(Chance(55)) ? SurvivorsPropRole::Brazier : SurvivorsPropRole::Firepit);
	return result;
}

bool SurvivorsRollingTerrain::Initialize(uint32_t seed, std::span<const SurvivorsFloorTile> floors)
{
	if (floors.empty()) return false;
	seed_ = seed;
	origin_ = {};
	std::map<SurvivorsFloorTile, uint32_t> counts;
	for (const SurvivorsFloorTile &floor : floors) ++counts[floor];
	std::vector<std::pair<SurvivorsFloorTile, uint32_t>> ranked(counts.begin(), counts.end());
	std::stable_sort(ranked.begin(), ranked.end(), [](const auto &a, const auto &b) { return a.second > b.second; });
	floors_.clear();
	cumulative_.clear();
	// Follow the source level's frequencies, but cap any one floor at four
	// times an even share so its rarer variants still show up in patches.
	const uint32_t cap = static_cast<uint32_t>(std::max<size_t>(1, 4 * floors.size() / ranked.size()));
	uint32_t total = 0;
	for (const auto &[floor, count] : ranked) {
		floors_.push_back(floor);
		total += std::clamp<uint32_t>(count, 1, cap);
		cumulative_.push_back(total);
	}
	return true;
}

bool SurvivorsRollingTerrain::SetOrigin(int64_t x, int64_t y)
{
	// PieceAt accesses origin + a local coordinate and Shift advances by 32.
	// Leave sufficient headroom for both operations when validating network data.
	constexpr int64_t margin = MAXDUNX * 2;
	if (floors_.empty() || x % 32 != 0 || y % 32 != 0
	    || x < std::numeric_limits<int64_t>::min() + margin || x > std::numeric_limits<int64_t>::max() - margin
	    || y < std::numeric_limits<int64_t>::min() + margin || y > std::numeric_limits<int64_t>::max() - margin)
		return false;
	origin_ = { x, y };
	return true;
}

uint16_t SurvivorsRollingTerrain::PieceAt(int64_t worldX, int64_t worldY) const
{
	if (floors_.empty()) return 0;
	const int64_t megaX = FloorHalf(worldX);
	const int64_t megaY = FloorHalf(worldY);
	const unsigned x = static_cast<unsigned>(worldX - megaX * 2);
	const unsigned y = static_cast<unsigned>(worldY - megaY * 2);
	if (floors_.size() == 1) return floors_[0][x + 2 * y];
	const uint64_t hash = Mix(static_cast<uint64_t>(seed_) ^ Mix(static_cast<uint64_t>(megaX))
	    ^ Mix(static_cast<uint64_t>(megaY) + 0x9e3779b97f4a7c15ULL));
	// Most megatiles share their 3x3 block's choice, so variants form patches
	// instead of per-tile noise. Worn regions of a slow noise field show only
	// the rarer floors; elsewhere the source level's frequencies are followed.
	const bool patch = ((hash >> 8) & 0xffff) < 0x9000;
	const uint64_t pick = patch ? CellHash(seed_, FloorDiv(megaX, 3), FloorDiv(megaY, 3), 0xf1002) : hash >> 24;
	const bool worn = ValueNoise(seed_, megaX, megaY, 7, 0x3e47) > 41000;
	const uint32_t total = cumulative_.back();
	const uint32_t low = worn && cumulative_[0] < total ? cumulative_[0] : 0;
	const uint32_t roll = low + static_cast<uint32_t>(pick % (total - low));
	const size_t index = static_cast<size_t>(std::upper_bound(cumulative_.begin(), cumulative_.end(), roll) - cumulative_.begin());
	return floors_[std::min(index, floors_.size() - 1)][x + 2 * y];
}

void SurvivorsRollingTerrain::Fill(uint16_t (&pieces)[MAXDUNX][MAXDUNY]) const
{
	for (int x = 0; x < MAXDUNX; ++x)
		for (int y = 0; y < MAXDUNY; ++y)
			pieces[x][y] = PieceAt(origin_.x + x, origin_.y + y);
}

bool SurvivorsRollingTerrain::Shift(int deltaX, int deltaY, uint16_t (&pieces)[MAXDUNX][MAXDUNY])
{
	if (floors_.empty() || (deltaX == 0 && deltaY == 0) || deltaX % 2 != 0 || deltaY % 2 != 0
	    || deltaX <= -MAXDUNX || deltaX >= MAXDUNX || deltaY <= -MAXDUNY || deltaY >= MAXDUNY)
		return false;
	std::array<std::array<uint16_t, MAXDUNY>, MAXDUNX> old {};
	for (int x = 0; x < MAXDUNX; ++x)
		std::copy_n(pieces[x], MAXDUNY, old[x].begin());
	origin_.x += deltaX;
	origin_.y += deltaY;
	for (int x = 0; x < MAXDUNX; ++x) {
		for (int y = 0; y < MAXDUNY; ++y) {
			const int sourceX = x + deltaX;
			const int sourceY = y + deltaY;
			pieces[x][y] = sourceX >= 0 && sourceX < MAXDUNX && sourceY >= 0 && sourceY < MAXDUNY
			    ? old[sourceX][sourceY] : PieceAt(origin_.x + x, origin_.y + y);
		}
	}
	return true;
}

} // namespace devilution
