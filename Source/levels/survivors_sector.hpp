#pragma once

#include <cstdint>

namespace devilution {

// Coordinates identify stable neighbouring sectors; the origin uses the
// original dungeon seed so existing first-sector maps remain unchanged.
struct SurvivorsSector {
	int32_t x = 0;
	int32_t y = 0;
	constexpr bool operator==(const SurvivorsSector &) const = default;
};

inline constexpr int SurvivorsSectorLimit = 32767;
constexpr bool IsValidSurvivorsSector(SurvivorsSector sector)
{
	return sector.x >= -SurvivorsSectorLimit && sector.x <= SurvivorsSectorLimit
	    && sector.y >= -SurvivorsSectorLimit && sector.y <= SurvivorsSectorLimit;
}
constexpr bool SurvivorsSectorSnapshotIsCurrent(uint32_t snapshot, uint32_t loaded, uint32_t expected)
{
	return snapshot == loaded && loaded == expected;
}
constexpr uint32_t SurvivorsSectorSeed(uint32_t base, unsigned act, SurvivorsSector sector)
{
	if (sector.x == 0 && sector.y == 0) return base;
	uint32_t value = base ^ (static_cast<uint32_t>(sector.x) * 0x9e3779b9U)
	    ^ (static_cast<uint32_t>(sector.y) * 0x85ebca6bU) ^ (act * 0xc2b2ae35U);
	value ^= value >> 16;
	value *= 0x7feb352dU;
	value ^= value >> 15;
	value *= 0x846ca68bU;
	return value ^ (value >> 16);
}

} // namespace devilution
