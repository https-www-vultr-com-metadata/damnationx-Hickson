#pragma once
#include <cstddef>
#include <cstdint>

struct DxpSurvivorsUiOffer {
	uint32_t rank, maxRank, kind, enabled, evolved, item;
	char name[96];
	char description[256];
	char requirements[128];
};
struct DxpSurvivorsUiLoadout {
	uint32_t rank, maxRank, evolved, kind;
	char name[96];
};
struct DxpSurvivorsUiSnapshot {
	uint32_t version, flags, revision, level;
	uint32_t rerolls, skips, banishes, loadoutCount;
	DxpSurvivorsUiOffer offers[3];
	DxpSurvivorsUiLoadout loadout[16];
	uint32_t act, difficulty, actSeconds, totalSeconds, kills, bossPending;
	uint32_t statPoints, gearSync;
	int64_t worldOriginX, worldOriginY;
	uint32_t worldSeed;
};
// flags: 1 mode, 2 draft, 4 run finished, 8 awaiting a queued/server action,
// 16 rolling world active. Origin is in world tiles relative to the engine grid.
extern "C" {
bool dxp_survivors_ui_snapshot(DxpSurvivorsUiSnapshot *output, uint32_t size);
// 1 choose, 2 reroll, 3 skip, 4 banish. Commands execute on the game thread.
bool dxp_survivors_ui_command(uint32_t revision, uint32_t action, uint32_t index);
}
static_assert(sizeof(DxpSurvivorsUiSnapshot) == 3392);
static_assert(offsetof(DxpSurvivorsUiSnapshot, worldSeed) == 3384);
