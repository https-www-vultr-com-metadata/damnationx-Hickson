#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

#include "levels/gendung_defs.hpp"

namespace devilution {

// A floor is sampled as one native 2x2 megatile. Keeping all four pieces
// together avoids mixing the edge halves of different pieces.
using SurvivorsFloorTile = std::array<uint16_t, 4>;

struct SurvivorsWorldOrigin {
	int64_t x = 0;
	int64_t y = 0;
};

// A sparse, deterministic chest location in a 16x16 world chunk. Negative
// chunk coordinates use the same stable mapping as positive coordinates.
std::optional<SurvivorsWorldOrigin> SurvivorsRollingChestPosition(uint32_t seed, int64_t chunkX, int64_t chunkY);

// Abstract roles of generated scenery. The runtime maps each role to a
// tileset-appropriate native object; the layout itself is engine-independent.
enum class SurvivorsPropRole : uint8_t {
	Chest,
	Shrine,
	Brazier, // tall standing light
	Candle,  // small light
	Firepit, // ground fire light
	Bones,
	Barrel,
	Banner,
	Altar, // fountain, cauldron or goat shrine
};
constexpr bool SurvivorsPropRoleEmitsLight(SurvivorsPropRole role)
{
	return role == SurvivorsPropRole::Brazier || role == SurvivorsPropRole::Candle || role == SurvivorsPropRole::Firepit;
}

struct SurvivorsRollingProp {
	int64_t x;
	int64_t y;
	SurvivorsPropRole role;
	uint8_t variant; // stable per-prop choice among the role's objects
};

struct SurvivorsRollingChunkProps {
	static constexpr size_t Capacity = 10;
	std::array<SurvivorsRollingProp, Capacity> props {};
	uint8_t count = 0;
	[[nodiscard]] std::span<const SurvivorsRollingProp> view() const { return { props.data(), count }; }
};

// Scenery for one 16x16 world chunk (chunk coordinates must be multiples of
// 16). Density follows a low-frequency noise field so props gather into
// clearings and themed vignettes (a brazier camp, a candle-lit shrine, a
// barrel cache, an ossuary, an altar court) with open ground between them.
// Every prop lies in [chunk + 1, chunk + 15) and members of one vignette keep
// at least one free tile between each other. Deterministic per seed and chunk.
SurvivorsRollingChunkProps SurvivorsRollingChunkScenery(uint32_t seed, int64_t chunkX, int64_t chunkY);

// The experimental rolling world keeps engine-local coordinates bounded,
// while all generated terrain is addressed in signed world coordinates.
// Even translations retain the native 2x2 megatile alignment.
class SurvivorsRollingTerrain {
public:
	bool Initialize(uint32_t seed, std::span<const SurvivorsFloorTile> floors);
	[[nodiscard]] uint16_t PieceAt(int64_t worldX, int64_t worldY) const;
	[[nodiscard]] SurvivorsWorldOrigin Origin() const { return origin_; }
	// Set the origin of an authority snapshot. The snapshot installs its own
	// tiles immediately afterwards; it may have skipped several windows.
	bool SetOrigin(int64_t x, int64_t y);
	void Fill(uint16_t (&pieces)[MAXDUNX][MAXDUNY]) const;
	bool Shift(int deltaX, int deltaY, uint16_t (&pieces)[MAXDUNX][MAXDUNY]);

private:
	uint32_t seed_ = 0;
	SurvivorsWorldOrigin origin_ {};
	// Unique floors, most common in the source level first, with cumulative
	// source frequencies used to keep plain floor dominant and variants rare.
	std::vector<SurvivorsFloorTile> floors_;
	std::vector<uint32_t> cumulative_;
};

} // namespace devilution
