#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <set>
#include <utility>

#include "survivors.h"
#include "survivors_encircle.h"

namespace devilution {
namespace {

using namespace SurvivorsEncircle;

int Chebyshev(Point a, Point b) { return std::max(std::abs(a.x - b.x), std::abs(a.y - b.y)); }

// Screen-space position of a tile offset (isometric 64x32 tiles).
std::pair<int, int> ScreenOf(Displacement offset) { return { (offset.deltaX - offset.deltaY) * 32, (offset.deltaX + offset.deltaY) * 16 }; }

TEST(SurvivorsEncircle, ScheduleOpensAfterACoupleOfMinutesThenEveryOneToTwo)
{
	for (uint32_t seed : { 0U, 1U, 0xdeadbeefU, 0x12345678U }) {
		const unsigned first = SurvivorsEncircleFirstSecond(0, seed);
		EXPECT_GE(first, 100U);
		EXPECT_LE(first, 130U);
		for (unsigned act = 0; act < SurvivorsActCount; ++act) {
			for (unsigned fired = 1; fired < 6; ++fired) {
				const unsigned gap = SurvivorsEncircleGapSeconds(act, fired, seed);
				EXPECT_GE(gap, MinimumGapSeconds);
				EXPECT_LE(gap, 120U);
			}
		}
	}
	static_assert(LastStartSecond < SurvivorsActDurationSeconds);
}

TEST(SurvivorsEncircle, DirectorFiresDeterministicallyAndResetsPerAct)
{
	const auto simulate = [](uint32_t seed) {
		std::vector<std::pair<unsigned, unsigned>> fires;
		SurvivorsEncircleDirector director;
		for (unsigned act = 0; act < SurvivorsActCount; ++act) {
			for (unsigned second = 0; second < SurvivorsActDurationSeconds; ++second) {
				if (director.Due(act, second, seed)) {
					fires.emplace_back(act, second);
					director.Fired(second, seed);
				}
			}
		}
		return fires;
	};
	const auto a = simulate(77);
	EXPECT_EQ(a, simulate(77));
	EXPECT_NE(a, simulate(78));
	std::array<unsigned, SurvivorsActCount> perAct {};
	for (const auto &[act, second] : a) {
		++perAct[act];
		EXPECT_LE(second, LastStartSecond);
	}
	EXPECT_GE(perAct[0], 1U);
	EXPECT_LE(perAct[0], 2U);
	for (unsigned act = 1; act < SurvivorsActCount; ++act) {
		EXPECT_GE(perAct[act], 2U);
		EXPECT_LE(perAct[act], 5U);
	}
}

TEST(SurvivorsEncircle, DirectorRetryDefersWithoutConsumingTheEvent)
{
	SurvivorsEncircleDirector director;
	const unsigned first = SurvivorsEncircleFirstSecond(0, 5);
	ASSERT_TRUE(director.Due(0, first, 5));
	director.Retry(first);
	EXPECT_FALSE(director.Due(0, first + RetrySeconds - 1, 5));
	EXPECT_TRUE(director.Due(0, first + RetrySeconds, 5));
	EXPECT_EQ(director.fired, 0U);
}

TEST(SurvivorsEncircle, CountScalesWithActDifficultyAndPartyWithinCaps)
{
	EXPECT_EQ(SurvivorsEncircleCount(0, 0, 0, 1), BaseCount);
	EXPECT_LT(SurvivorsEncircleCount(0, 0, 0, 1), SurvivorsEncircleCount(2, 0, 0, 1));
	EXPECT_LT(SurvivorsEncircleCount(0, 0, 0, 1), SurvivorsEncircleCount(0, 1, 0, 1));
	EXPECT_LT(SurvivorsEncircleCount(0, 0, 0, 1), SurvivorsEncircleCount(0, 0, 0, 3));
	EXPECT_EQ(SurvivorsEncircleCount(4, 2, 9, 4), MaximumCount);
	// Never eat into the reserve kept for waves, elites and bosses.
	EXPECT_EQ(SurvivorsEncircleCapacity(30, 0, 200), 30U);
	EXPECT_EQ(SurvivorsEncircleCapacity(30, 160, 200), 16U);
	EXPECT_EQ(SurvivorsEncircleCapacity(30, 170, 200), 0U);
	EXPECT_EQ(SurvivorsEncircleCapacity(30, 250, 200), 0U);
}

TEST(SurvivorsEncircle, RingStartsJustOffScreenAllAround)
{
	constexpr int halfWidth = 320;
	constexpr int halfHeight = 176;
	constexpr unsigned count = 24;
	for (unsigned index = 0; index < count; ++index) {
		const Displacement offset = SurvivorsEncircleIdealOffset(index, count, 0.25, halfWidth, halfHeight);
		const int radius = std::max(std::abs(offset.deltaX), std::abs(offset.deltaY));
		EXPECT_GE(radius, MinimumTileRadius);
		EXPECT_LE(radius, MaximumTileRadius);
		const auto [sx, sy] = ScreenOf(offset);
		// Outside the view rectangle (allowing half a tile of rounding).
		EXPECT_TRUE(std::abs(sx) > halfWidth - 32 || std::abs(sy) > halfHeight - 16) << index << ": " << sx << "," << sy;
	}
}

TEST(SurvivorsEncircle, RingIsAFullCircleOfDistinctOpenTiles)
{
	const Point center { 56, 56 };
	const auto ring = SurvivorsEncircleRing(center, 30, 0.1, 320, 176, [](Point) { return true; });
	ASSERT_EQ(ring.size(), 30U);
	std::set<std::pair<int, int>> seen;
	int quadrants[4] {};
	for (const Point tile : ring) {
		EXPECT_TRUE(seen.emplace(tile.x, tile.y).second) << "duplicate " << tile.x << "," << tile.y;
		const auto [sx, sy] = ScreenOf(tile - center);
		++quadrants[(sx >= 0 ? 1 : 0) + (sy >= 0 ? 2 : 0)];
	}
	for (int quadrant : quadrants)
		EXPECT_GE(quadrant, 5);
}

TEST(SurvivorsEncircle, BlockedSpokesAreSkoochedOrSkipped)
{
	const Point center { 56, 56 };
	const auto open = SurvivorsEncircleRing(center, 24, 0.0, 320, 176, [](Point) { return true; });
	// Wall off every ideal tile: members skooch inward instead.
	std::set<std::pair<int, int>> walls;
	for (const Point tile : open) walls.emplace(tile.x, tile.y);
	const auto skooched = SurvivorsEncircleRing(center, 24, 0.0, 320, 176, [&walls](Point tile) { return !walls.contains({ tile.x, tile.y }); });
	EXPECT_GE(skooched.size(), 20U);
	for (const Point tile : skooched) {
		EXPECT_FALSE(walls.contains({ tile.x, tile.y }));
		EXPECT_GE(Chebyshev(tile, center), MinimumPlacedTileRadius);
	}
	// A solid world yields an empty ring rather than stacking monsters.
	EXPECT_TRUE(SurvivorsEncircleRing(center, 24, 0.0, 320, 176, [](Point) { return false; }).empty());
	// A large viewport is clamped to the rolling window's safe radius.
	for (const Point tile : SurvivorsEncircleRing(center, 24, 0.0, 1600, 900, [](Point) { return true; }))
		EXPECT_LE(Chebyshev(tile, center), MaximumTileRadius + 1);
}

} // namespace
} // namespace devilution
