#include "survivors_encircle.h"

#include <array>
#include <cmath>
#include <cstdlib>
#include <numbers>

#include "engine/direction.hpp"
#include "levels/dun_tile_data.hpp"
#include "levels/survivors_rolling_runtime.hpp"
#include "levels/tile_properties.hpp"
#include "monster.h"
#include "player.h"
#include "utils/ui_fwd.h"

namespace devilution {
namespace {

// Isometric tile footprint in screen pixels.
constexpr double TileHalfWidth = 32.0;
constexpr double TileHalfHeight = 16.0;

int Chebyshev(Displacement offset) { return std::max(std::abs(offset.deltaX), std::abs(offset.deltaY)); }

Displacement ClampRadius(Displacement offset)
{
	using namespace SurvivorsEncircle;
	const int radius = Chebyshev(offset);
	if (radius == 0) return { MinimumTileRadius, 0 };
	if (radius >= MinimumTileRadius && radius <= MaximumTileRadius) return offset;
	const double scale = static_cast<double>(std::clamp(radius, MinimumTileRadius, MaximumTileRadius)) / radius;
	return { static_cast<int>(std::lround(offset.deltaX * scale)), static_cast<int>(std::lround(offset.deltaY * scale)) };
}

int StepToward(int value) { return (value > 0) - (value < 0); }

struct MarchingMember {
	unsigned id;
	MonsterAIID nativeAi;
	unsigned deadline;
};
std::vector<MarchingMember> Marching;

bool OpenRingTile(Point tile)
{
	return InDungeonBounds(tile) && dPiece[tile.x][tile.y] != 0 && IsTileWalkable(tile) && !IsTileOccupied(tile);
}

void StepInward(Monster &monster)
{
	const Point target = monster.enemyPosition;
	const Direction toward = GetDirection(monster.position.tile, target);
	monster.direction = toward;
	for (const Direction direction : { toward, Left(toward), Right(toward), Left(Left(toward)), Right(Right(toward)) }) {
		if (Walk(monster, direction)) return;
	}
}

} // namespace

Displacement SurvivorsEncircleIdealOffset(unsigned index, unsigned count, double phase, int halfWidth, int halfHeight)
{
	const double angle = (static_cast<double>(index) + phase) / std::max(1U, count) * 2.0 * std::numbers::pi;
	const double cx = std::cos(angle);
	const double cy = std::sin(angle);
	// Distance from the view centre to its rectangle edge along this spoke.
	const double w = std::max(1, halfWidth);
	const double h = std::max(1, halfHeight);
	const double edge = std::min(std::abs(cx) > 1e-6 ? w / std::abs(cx) : 1e9, std::abs(cy) > 1e-6 ? h / std::abs(cy) : 1e9);
	const double distance = edge + SurvivorsEncircle::EdgeMarginPixels;
	const double sx = cx * distance;
	const double sy = cy * distance;
	// Inverse of the isometric projection: x = (tx - ty) * 32, y = (tx + ty) * 16.
	const double tx = (sx / TileHalfWidth + sy / TileHalfHeight) / 2.0;
	const double ty = (sy / TileHalfHeight - sx / TileHalfWidth) / 2.0;
	return ClampRadius({ static_cast<int>(std::lround(tx)), static_cast<int>(std::lround(ty)) });
}

std::vector<Point> SurvivorsEncircleRing(Point center, unsigned count, double phase, int halfWidth, int halfHeight, const std::function<bool(Point)> &open)
{
	using namespace SurvivorsEncircle;
	std::vector<Point> ring;
	ring.reserve(count);
	const auto unused = [&ring](Point tile) { return std::find(ring.begin(), ring.end(), tile) == ring.end(); };
	for (unsigned index = 0; index < count; ++index) {
		const Displacement ideal = SurvivorsEncircleIdealOffset(index, count, phase, halfWidth, halfHeight);
		const Displacement inward { -StepToward(ideal.deltaX), -StepToward(ideal.deltaY) };
		// Perpendicular to the spoke, in tile space.
		const Displacement side { -inward.deltaY, inward.deltaX };
		std::array<Displacement, 1 + MaximumInwardSkooch + 4 * MaximumSideSkooch> candidates {};
		size_t candidateCount = 0;
		candidates[candidateCount++] = ideal;
		for (int step = 1; step <= MaximumInwardSkooch; ++step)
			candidates[candidateCount++] = ideal + inward * step;
		for (int step = 1; step <= MaximumSideSkooch; ++step) {
			candidates[candidateCount++] = ideal + side * step;
			candidates[candidateCount++] = ideal - side * step;
			candidates[candidateCount++] = ideal + side * step + inward;
			candidates[candidateCount++] = ideal - side * step + inward;
		}
		for (size_t candidate = 0; candidate < candidateCount; ++candidate) {
			const Displacement offset = candidates[candidate];
			if (Chebyshev(offset) < MinimumPlacedTileRadius) continue;
			const Point tile = center + offset;
			if (unused(tile) && open(tile)) {
				ring.push_back(tile);
				break;
			}
		}
	}
	return ring;
}

unsigned SpawnSurvivorsEncircle(const SurvivorsEncircleSpawn &spawn)
{
	const Point center = spawn.focus.position.tile;
	const int halfWidth = gnScreenWidth / 2;
	const int halfHeight = gnViewportHeight / 2;
	const auto ring = SurvivorsEncircleRing(center, spawn.count, spawn.phase, halfWidth, halfHeight, OpenRingTile);
	const unsigned deadline = spawn.tick + SurvivorsEncircle::MarchTimeoutSeconds * std::max(1U, spawn.tickRate);
	unsigned placed = 0;
	for (const Point tile : ring) {
		if (!OpenRingTile(tile)) continue; // an earlier member may have landed here
		Monster *monster = AddMonster(tile, GetDirection(tile, center), spawn.typeIndex, true);
		if (monster == nullptr) break;
		if (spawn.configure) spawn.configure(*monster);
		monster->activeForTicks = 255;
		monster->enemy = spawn.focus.getId();
		monster->enemyPosition = spawn.focus.position.tile;
		monster->position.last = spawn.focus.position.tile;
		Marching.push_back({ static_cast<unsigned>(monster->getId()), monster->ai, deadline });
		// A plain pursuer for the march; native AI (ranged, fleeing, special
		// attacks) returns once the member reaches its hunter.
		monster->ai = MonsterAIID::SkeletonMelee;
		++placed;
	}
	return placed;
}

void ProcessSurvivorsEncircleMarch(unsigned tick)
{
	for (auto it = Marching.begin(); it != Marching.end();) {
		Monster &monster = Monsters[it->id];
		// Something else re-tasked the slot (or it was reused): leave it alone.
		if (monster.isInvalid || monster.hasNoLife() || monster.ai != MonsterAIID::SkeletonMelee) {
			it = Marching.erase(it);
			continue;
		}
		// A member parked by the rolling world comes back as a normal monster.
		if (SurvivorsRollingMonsterParked(it->id)) {
			monster.ai = it->nativeAi;
			it = Marching.erase(it);
			continue;
		}
		if (tick >= it->deadline) {
			monster.ai = it->nativeAi;
			it = Marching.erase(it);
			continue;
		}
		// Members that reached their hunter hold the noose with the pursuer AI
		// (it attacks when adjacent) instead of e.g. a Fallen One's retreat.
		const bool arrived = monster.position.tile.WalkingDistance(monster.enemyPosition) <= static_cast<int>(SurvivorsEncircle::MarchReleaseDistance);
		if (!arrived) {
			// Native AI may have parked the member in a delay; the ring keeps moving.
			if (monster.mode == MonsterMode::Delay) M_StartStand(monster, monster.direction);
			if (monster.mode == MonsterMode::Stand) StepInward(monster);
		}
		++it;
	}
}

void ResetSurvivorsEncircleMarch() { Marching.clear(); }
size_t SurvivorsEncircleMarchingCount() { return Marching.size(); }

} // namespace devilution
