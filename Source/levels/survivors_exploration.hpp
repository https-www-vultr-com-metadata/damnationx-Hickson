#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <list>
#include <unordered_map>

#include "engine/point.hpp"

namespace devilution {

// Exploration belongs to world coordinates, not the reusable 112-tile engine
// window. Each cell covers the same 2x2 world tiles as the native automap.
class SurvivorsExploration {
public:
	static constexpr size_t MaxCells = 65536;
	SurvivorsExploration() = default;
	SurvivorsExploration(const SurvivorsExploration &) = delete;
	SurvivorsExploration &operator=(const SurvivorsExploration &) = delete;

	void Clear()
	{
		cells_.clear();
		order_.clear();
	}

	void Mark(int64_t originX, int64_t originY, Point tile, uint8_t explorer)
	{
		if (explorer == 0)
			return;
		const Cell cell { HalfFloor(originX + tile.x - 16), HalfFloor(originY + tile.y - 16) };
		auto existing = cells_.find(cell);
		if (existing != cells_.end()) {
			existing->second.explorer = std::max(existing->second.explorer, explorer);
			order_.splice(order_.end(), order_, existing->second.order);
			return;
		}
		if (cells_.size() == MaxCells) {
			cells_.erase(order_.front());
			order_.pop_front();
		}
		order_.push_back(cell);
		cells_.emplace(cell, Entry { explorer, std::prev(order_.end()) });
	}

	uint8_t At(int64_t originX, int64_t originY, Point localMega) const
	{
		const Cell cell { HalfFloor(originX) + localMega.x, HalfFloor(originY) + localMega.y };
		const auto it = cells_.find(cell);
		return it == cells_.end() ? 0 : it->second.explorer;
	}

private:
	struct Cell {
		int64_t x;
		int64_t y;
		bool operator==(const Cell &) const = default;
	};
	struct CellHash {
		size_t operator()(const Cell &cell) const noexcept
		{
			// Mix both signed axes without losing negative-coordinate bits.
			auto mix = [](uint64_t value) {
				value ^= value >> 30;
				value *= 0xbf58476d1ce4e5b9ULL;
				value ^= value >> 27;
				value *= 0x94d049bb133111ebULL;
				return value ^ (value >> 31);
			};
			return static_cast<size_t>(mix(static_cast<uint64_t>(cell.x)) ^ mix(static_cast<uint64_t>(cell.y) + 0x9e3779b97f4a7c15ULL));
		}
	};
	static constexpr int64_t HalfFloor(int64_t value)
	{
		return value / 2 - (value < 0 && value % 2 != 0);
	}
	struct Entry {
		uint8_t explorer;
		std::list<Cell>::iterator order;
	};
	std::list<Cell> order_;
	std::unordered_map<Cell, Entry, CellHash> cells_;
};

} // namespace devilution
