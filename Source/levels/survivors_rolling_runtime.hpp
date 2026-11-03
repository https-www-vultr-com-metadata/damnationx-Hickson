#pragma once

#include <vector>
#include <cstdint>
#include <cstddef>

#include <span>

#include "engine/point.hpp"
#include "tables/objdat.h"
#include "tables/spelldat.h"

namespace devilution {

struct SurvivorsRollingSoul {
	Point tile;
	unsigned value;
};
struct Item;
struct Monster;
struct Missile;
struct Player;
enum class SurvivorsRollingTarget { Item, Object, Monster };
// Drop a queued action before its target slot is parked or reused.
void SurvivorsRollingCancelTarget(Player &player, SurvivorsRollingTarget kind, unsigned id);

// Rolling terrain is the default in solo and asynchronous co-op Survivors.
bool SurvivorsRollingRequested();
bool SurvivorsRollingActive();
// Every native object the rolling world can generate, for graphics loading.
std::span<const _object_id> SurvivorsRollingSceneryObjects();
uint32_t SurvivorsRollingSeed();
int64_t SurvivorsRollingOriginX();
int64_t SurvivorsRollingOriginY();
uint64_t SurvivorsRollingEpoch();
// Called only while installing a validated authority snapshot, before its
// tiles and actor positions replace the client replica.
bool SurvivorsRollingApplyOrigin(int64_t x, int64_t y, uint64_t epoch);
size_t SurvivorsRollingSuspendedMissileCount();
bool SurvivorsRollingMonsterParked(unsigned id);
bool SurvivorsRollingDeadPlayerParked(unsigned id);
// Returns false when the slot is a dormant holding-cell golem or its actor
// cannot fit the translated window; otherwise rebases its actor position.
bool SurvivorsRollingRebaseMonsterPosition(Monster &monster, unsigned id, int dx, int dy);
bool SurvivorsRollingRebaseSuspendedMissile(Missile &missile, int dx, int dy);
bool SurvivorsRollingMissileSupported(MissileID type);
void SurvivorsRollingWarnIfTethered();
void InitSurvivorsRollingWorld();
void SurvivorsRollingPrepareLevelChange();
// Include items outside the rolling window in the cooperative ground journal.
void SurvivorsRollingAppendParkedItems(std::vector<Item> &items);
unsigned SurvivorsRollingTakeParkedSouls();
void ResetSurvivorsRollingWorld();
void UpdateSurvivorsRollingWorld(std::vector<SurvivorsRollingSoul> &souls, int boss, int elite);

} // namespace devilution
