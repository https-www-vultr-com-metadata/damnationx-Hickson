#pragma once

#include <array>
#include <cstdint>

namespace devilution {

// The native dungeon is a fixed 40x40 megatile grid (112x112 world tiles),
// so a hunt cannot stream an unbounded map. Build a connected, seed-specific
// clearing that uses most of that grid and keeps a wide route through its
// center. Bands overlap by at least 17 tiles, leaving room for swarm combat.
using SurvivorsLayout = std::array<std::array<bool, 40>, 40>;

constexpr SurvivorsLayout GenerateSurvivorsLayout(uint32_t seed)
{
	SurvivorsLayout result {};
	auto next = [&seed]() constexpr {
		seed = seed * 1664525U + 1013904223U;
		return seed ^ (seed >> 16);
	};
	for (int band = 0; band < 6; ++band) {
		const int left = 3 + next() % 9;
		const int right = 28 + next() % 9;
		const int top = 3 + band * 34 / 6;
		const int bottom = 3 + (band + 1) * 34 / 6;
		for (int y = top; y < bottom; ++y)
			for (int x = left; x <= right; ++x)
				result[x][y] = true;
	}
	return result;
}

constexpr std::array<int, 2> SurvivorsArrivalMega(const SurvivorsLayout &layout, int dx, int dy)
{
	if (dx != 0) {
		int left = 0, right = 39;
		for (int y = 18; y <= 22; ++y) {
			int rowLeft = 0, rowRight = 39;
			while (rowLeft < 40 && !layout[rowLeft][y]) ++rowLeft;
			while (rowRight >= 0 && !layout[rowRight][y]) --rowRight;
			if (rowLeft > left) left = rowLeft;
			if (rowRight < right) right = rowRight;
		}
		return { dx > 0 ? left + 3 : right - 3, 20 };
	}
	if (dy != 0) return { 20, dy > 0 ? 7 : 32 };
	return { 20, 20 };
}

} // namespace devilution
