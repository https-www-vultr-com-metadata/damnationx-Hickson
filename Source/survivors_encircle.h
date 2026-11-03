#pragma once

// Survivors "encircle" events: a ring of monsters appears just past the edge
// of the screen all the way around a hunter and closes in, so the hunter has
// to break through. Modelled on d2survivors' `ring` act events (one monster
// type per ring, evenly spaced angles, each placed on the view's edge plus a
// small margin and snapped to an open tile; ring members then simply chase).
//
// Spawning is host-authoritative like every other Survivors spawn: only the
// authority calls the runtime functions, and the resulting monsters reach
// cooperative peers through the normal world snapshots.

#include <algorithm>
#include <cstdint>
#include <functional>
#include <span>
#include <vector>

#include "engine/point.hpp"

namespace devilution {

struct Monster;
struct Player;

// ---------------------------------------------------------------------------
// Tuning. Every encircle constant lives here.
// ---------------------------------------------------------------------------
namespace SurvivorsEncircle {
// Schedule, in act seconds (an act lasts SurvivorsActDurationSeconds = 240).
// The first act gives the hunter a couple of minutes before the first ring;
// later acts start with a run that is already minutes old.
inline constexpr unsigned FirstActFirstSecond = 115;
inline constexpr unsigned LaterActFirstSecond = 45;
inline constexpr unsigned FirstSecondJitter = 12; // +/- seconds
// Gap between rings: 1-2 minutes, shrinking a little per act.
inline constexpr unsigned BaseGapSeconds = 95;
inline constexpr unsigned GapShrinkPerAct = 7;
inline constexpr unsigned GapJitter = 25; // +/- seconds
inline constexpr unsigned MinimumGapSeconds = 45;
// No ring may start this close to the boss gate (or during it).
inline constexpr unsigned LastStartSecond = 228;
// A ring that cannot be placed (no free monster slots, walls) retries soon.
inline constexpr unsigned RetrySeconds = 8;
// Do not trap a hunter who has just arrived in a new rolling sector.
inline constexpr unsigned SectorArrivalGraceSeconds = 10;

// Size. d2survivors rings hold 12-36 monsters.
inline constexpr unsigned BaseCount = 16;
inline constexpr unsigned CountPerAct = 4;
inline constexpr unsigned CountPerDifficulty = 3;
inline constexpr unsigned CountPerEventInAct = 2; // later rings in an act are denser
inline constexpr unsigned CountEventSteps = 3;
inline constexpr unsigned CountPerExtraHunter = 4;
inline constexpr unsigned MinimumCount = 10;
inline constexpr unsigned MaximumCount = 36;
// Monster slots kept free for regular waves, bosses and elites.
inline constexpr unsigned ReservedMonsterSlots = 24;

// Geometry, in screen pixels and tiles. The ring follows the view rectangle
// (an ellipse-like diamond in tile space) plus a margin, so it starts just
// off-screen in every direction.
inline constexpr int EdgeMarginPixels = 56;
inline constexpr int MinimumTileRadius = 8;
inline constexpr int MaximumTileRadius = 18;
// Blocked ideal tiles are skooched: first inward along the spoke, then
// sideways. Never closer than this to the hunter.
inline constexpr int MaximumInwardSkooch = 4;
inline constexpr int MaximumSideSkooch = 2;
inline constexpr int MinimumPlacedTileRadius = 6;

// Closing in: ring members use a plain pursuer AI and are steered straight at
// their hunter while farther than MarchReleaseDistance; after the march
// timeout they return to their native AI (ranged, fleeing, specials).
inline constexpr unsigned MarchReleaseDistance = 2;
inline constexpr unsigned MarchTimeoutSeconds = 14;
} // namespace SurvivorsEncircle

constexpr uint32_t SurvivorsEncircleHash(uint32_t seed, unsigned act, unsigned index)
{
	uint32_t h = seed ^ 0x52494e47U; // "RING"
	h ^= (act + 1) * 0x9e3779b1U;
	h = (h ^ (h >> 16)) * 0x85ebca6bU;
	h ^= (index + 1) * 0xc2b2ae35U;
	h = (h ^ (h >> 13)) * 0x27d4eb2fU;
	return h ^ (h >> 16);
}

constexpr unsigned SurvivorsEncircleFirstSecond(unsigned act, uint32_t seed)
{
	using namespace SurvivorsEncircle;
	const unsigned base = act == 0 ? FirstActFirstSecond : LaterActFirstSecond;
	const unsigned jitter = SurvivorsEncircleHash(seed, act, 0xffU) % (FirstSecondJitter * 2 + 1);
	return base + jitter - FirstSecondJitter;
}

/** Seconds from ring `fired` (1-based count of rings so far in this act) to the next one. */
constexpr unsigned SurvivorsEncircleGapSeconds(unsigned act, unsigned fired, uint32_t seed)
{
	using namespace SurvivorsEncircle;
	const unsigned base = BaseGapSeconds - std::min(act, 4U) * GapShrinkPerAct;
	const unsigned jitter = SurvivorsEncircleHash(seed, act, fired) % (GapJitter * 2 + 1);
	return std::max(MinimumGapSeconds, base + jitter - GapJitter);
}

/** Ring size before the monster-cap clamp. `eventInAct` is zero-based. */
constexpr unsigned SurvivorsEncircleCount(unsigned act, unsigned difficulty, unsigned eventInAct, unsigned partySize)
{
	using namespace SurvivorsEncircle;
	const unsigned count = BaseCount + std::min(act, 4U) * CountPerAct + std::min(difficulty, 2U) * CountPerDifficulty
	    + std::min(eventInAct, CountEventSteps) * CountPerEventInAct + (std::clamp(partySize, 1U, 4U) - 1) * CountPerExtraHunter;
	return std::clamp(count, MinimumCount, MaximumCount);
}

/** Clamp a ring to the free monster slots; 0 means "too crowded, retry later". */
constexpr unsigned SurvivorsEncircleCapacity(unsigned wanted, unsigned activeMonsters, unsigned maxMonsters)
{
	using namespace SurvivorsEncircle;
	const unsigned used = activeMonsters + ReservedMonsterSlots;
	const unsigned room = used >= maxMonsters ? 0 : maxMonsters - used;
	const unsigned count = std::min(wanted, room);
	return count < MinimumCount ? 0 : count;
}

// Pure act-relative scheduler; the run calls Due() every tick it may spawn.
struct SurvivorsEncircleDirector {
	unsigned act = ~0U;
	unsigned fired = 0;
	unsigned nextSecond = 0;

	constexpr void Reset(unsigned newAct, uint32_t seed)
	{
		act = newAct;
		fired = 0;
		nextSecond = SurvivorsEncircleFirstSecond(newAct, seed);
	}
	constexpr bool Due(unsigned currentAct, unsigned actSeconds, uint32_t seed)
	{
		if (currentAct != act) Reset(currentAct, seed);
		return actSeconds >= nextSecond && actSeconds <= SurvivorsEncircle::LastStartSecond;
	}
	constexpr void Fired(unsigned actSeconds, uint32_t seed)
	{
		++fired;
		nextSecond = actSeconds + SurvivorsEncircleGapSeconds(act, fired, seed);
	}
	constexpr void Retry(unsigned actSeconds) { nextSecond = actSeconds + SurvivorsEncircle::RetrySeconds; }
};

/**
 * Ideal (unsnapped) offset of ring member `index` of `count`, in tiles, for a
 * view of `halfWidth` x `halfHeight` screen pixels. `phase` in [0, 1) rotates
 * the whole ring so consecutive rings do not line up.
 */
Displacement SurvivorsEncircleIdealOffset(unsigned index, unsigned count, double phase, int halfWidth, int halfHeight);

/**
 * Ring tiles around `center`, each snapped to a tile accepted by `open`
 * (walkable, unoccupied). Members whose spoke is blocked are skipped, so the
 * result can hold fewer than `count` tiles; it never holds duplicates.
 */
std::vector<Point> SurvivorsEncircleRing(Point center, unsigned count, double phase, int halfWidth, int halfHeight, const std::function<bool(Point)> &open);

// ---------------------------------------------------------------------------
// Runtime (authority only).
// ---------------------------------------------------------------------------
struct SurvivorsEncircleSpawn {
	Player &focus;
	size_t typeIndex;
	unsigned count;
	double phase;
	unsigned tick;
	unsigned tickRate;
	/** Applies the run's wave stats to a freshly added monster. */
	std::function<void(Monster &)> configure;
};
/** Spawns the ring; returns how many monsters were placed. */
unsigned SpawnSurvivorsEncircle(const SurvivorsEncircleSpawn &spawn);
/** Steers marching ring members inward; call once per simulation tick before monsters act. */
void ProcessSurvivorsEncircleMarch(unsigned tick);
/** Forget marching ring members (arena reset, level change, run end). */
void ResetSurvivorsEncircleMarch();
size_t SurvivorsEncircleMarchingCount();

} // namespace devilution
