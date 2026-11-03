#pragma once

#include <algorithm>
#include <cstdint>

#include "engine/point.hpp"
#include "engine/size.hpp"

namespace devilution {

struct Surface;
struct Player;

inline constexpr unsigned SurvivorsActCount = 5;
inline constexpr unsigned SurvivorsActDurationSeconds = 4 * 60;
inline constexpr unsigned SurvivorsDurationSeconds = SurvivorsActCount * SurvivorsActDurationSeconds;
inline constexpr unsigned SurvivorsTicksPerSecond = 20;
inline constexpr unsigned SurvivorsStartingLifeBonus = 20;
inline constexpr unsigned SurvivorsActLifeBonus = 15;
// Player network packs store unspent points in one byte. Never award more
// than the remaining room in the character's attributes or that field.
constexpr int SurvivorsStatPointsAfterLevel(int current, int remaining)
{
	return std::min({ 255, std::max(0, remaining), std::clamp(current, 0, 255) + 5 });
}
constexpr unsigned SurvivorsDamageGraceTicks(unsigned tickRate)
{
	return std::max(1U, std::clamp(tickRate, 1U, 120U) * 3 / 10);
}
struct SurvivorsDamageGrace {
	uint64_t until = 0;
	constexpr bool Accept(uint32_t tick, unsigned tickRate)
	{
		if (tick < until) return false;
		until = static_cast<uint64_t>(tick) + SurvivorsDamageGraceTicks(tickRate);
		return true;
	}
};
bool SurvivorsAllowPlayerDamage(Player &player);
/** Make a short opening in a surrounding pack after an accepted hit. */
void SurvivorsOpenEscapeLane(Player &player);

// Wire validation admits only this mode's finite life progression: expedition
// vigor, four checkpoints, capped permanent/Fortitude ranks and at most one
// +5-life rite for each of the 1000 bounded run levels.
inline constexpr unsigned SurvivorsMaximumLifeBonus = SurvivorsStartingLifeBonus + (SurvivorsActCount - 1) * SurvivorsActLifeBonus + 50 + 50 + 1000 * 5;
constexpr bool SurvivorsPackedLifeInRange(int32_t naturalBaseLife, int32_t packedBaseLife)
{
	return naturalBaseLife > 0 && packedBaseLife > 0 && static_cast<int64_t>(packedBaseLife) <= static_cast<int64_t>(naturalBaseLife) + static_cast<int64_t>(SurvivorsMaximumLifeBonus) * 64;
}

constexpr unsigned SurvivorsWaveForSeconds(unsigned seconds)
{
	return 1 + std::min(seconds, SurvivorsDurationSeconds - 1) / 60;
}
constexpr unsigned SurvivorsSpawnInterval(unsigned wave)
{
	return 14 - std::min(wave, 20U) / 2;
}
constexpr unsigned SurvivorsSpawnBatch(unsigned wave)
{
	return 1 + (std::clamp(wave, 1U, 20U) - 1) / 5;
}
constexpr unsigned SurvivorsExperienceForLevel(unsigned level)
{
	return 8 + std::min(level, 1000U) * 4;
}
constexpr unsigned SurvivorsDamageForLevel(unsigned level)
{
	return 6 + std::min(level, 1000U) * 3;
}

// All hit testing uses UI-space pixels, matching the native three-card view.
enum class SurvivorsUpgradeAction : int {
	None = -1, Choose0 = 0, Choose1 = 1, Choose2 = 2,
	Reroll = 3, Skip = 4, Banish0 = 5, Banish1 = 6, Banish2 = 7,
};
struct SurvivorsUpgradeRect {
	int x, y, width, height;
	constexpr bool Contains(Point point) const { return point.x >= x && point.x < x + width && point.y >= y && point.y < y + height; }
};
constexpr SurvivorsUpgradeRect SurvivorsUpgradePanelRect(Size screen)
{
	const int width = std::min(920, screen.width - 24);
	return { (screen.width - width) / 2, std::max(12, (screen.height - 330) / 2), width, 330 };
}
constexpr SurvivorsUpgradeRect SurvivorsUpgradeCardRect(unsigned choice, Size screen)
{
	const auto panel = SurvivorsUpgradePanelRect(screen);
	const int width = (panel.width - 40) / 3;
	return { panel.x + 12 + static_cast<int>(std::min(choice, 2U)) * (width + 8), panel.y + 48, width, 208 };
}
constexpr SurvivorsUpgradeRect SurvivorsUpgradeBanishRect(unsigned choice, Size screen)
{
	const auto card = SurvivorsUpgradeCardRect(choice, screen);
	return { card.x + 8, card.y + 176, card.width - 16, 24 };
}
constexpr SurvivorsUpgradeRect SurvivorsUpgradeRerollRect(Size screen)
{
	const auto panel = SurvivorsUpgradePanelRect(screen);
	return { screen.width / 2 - 134, panel.y + 268, 128, 30 };
}
constexpr SurvivorsUpgradeRect SurvivorsUpgradeSkipRect(Size screen)
{
	const auto panel = SurvivorsUpgradePanelRect(screen);
	return { screen.width / 2 + 6, panel.y + 268, 128, 30 };
}
constexpr SurvivorsUpgradeAction SurvivorsUpgradeActionAt(Point position, Size screen)
{
	if (SurvivorsUpgradeRerollRect(screen).Contains(position)) return SurvivorsUpgradeAction::Reroll;
	if (SurvivorsUpgradeSkipRect(screen).Contains(position)) return SurvivorsUpgradeAction::Skip;
	for (unsigned choice = 0; choice < 3; ++choice) {
		if (SurvivorsUpgradeBanishRect(choice, screen).Contains(position)) return static_cast<SurvivorsUpgradeAction>(5 + choice);
		if (SurvivorsUpgradeCardRect(choice, screen).Contains(position)) return static_cast<SurvivorsUpgradeAction>(choice);
	}
	return SurvivorsUpgradeAction::None;
}
constexpr int SurvivorsUpgradeChoiceAt(Point position, Size screen)
{
	const int action = static_cast<int>(SurvivorsUpgradeActionAt(position, screen));
	return action >= 0 && action < 3 ? action : -1;
}

// Act indices are zero-based internally. The fifth act uses the deepest Hell
// tile set, keeping the campaign available without the Hellfire expansion.
constexpr unsigned SurvivorsActLevel(unsigned act)
{
	constexpr unsigned levels[] = { 1, 5, 9, 13, 15 };
	return levels[std::min(act, SurvivorsActCount - 1)];
}
constexpr unsigned SurvivorsBossHitPoints(unsigned act, unsigned difficulty)
{
	constexpr unsigned health[] = { 2000, 6000, 12000, 22000, 36000 };
	return health[std::min(act, 4U)] * (1 + std::min(difficulty, 2U) * 2);
}

// Pure campaign director shared by gameplay and tests. Time opens a boss gate;
// only defeating that gate's boss advances an act. It never advances twice.
struct SurvivorsCampaignDirector {
	unsigned act = 0;
	unsigned actsCleared = 0;
	bool bossGate = false;
	bool victory = false;
	bool awaitingFreshAct = false;
	constexpr bool Update(unsigned actSeconds, bool bossDefeated)
	{
		if (victory)
			return false;
		if (awaitingFreshAct) {
			if (actSeconds >= SurvivorsActDurationSeconds)
				return false;
			awaitingFreshAct = false;
		}
		if (actSeconds >= SurvivorsActDurationSeconds)
			bossGate = true;
		if (!bossGate || !bossDefeated)
			return false;
		++actsCleared;
		victory = actsCleared == SurvivorsActCount;
		if (!victory)
			++act;
		bossGate = false;
		awaitingFreshAct = true;
		return true;
	}
};

struct SurvivorsRunStatus {
	unsigned act; // 1..5 for display.
	unsigned difficulty; // Normal=0, Nightmare=1, Hell=2.
	unsigned actSeconds;
	unsigned totalSeconds;
	unsigned level;
	unsigned kills;
	unsigned actsCleared;
	unsigned bosses;
	bool bossPending;
	bool transitionPending;
	bool victory;
	int bossMonsterId;
};
SurvivorsRunStatus GetSurvivorsRunStatus();
bool IsSurvivorsTransitionPending();
void SurvivorsLevelLoaded();
bool RerollSurvivorsUpgrades();
bool BanishSurvivorsUpgrade(unsigned choice);
bool SkipSurvivorsUpgrade();

// The active build is ephemeral; settlement writes a separate Survivors profile.
void SetSurvivorsMode(bool enabled);
void SetSurvivorsCampShop(bool enabled);
bool IsSurvivorsCampShop();
#ifdef DAMNATION_S1_BRIDGE
/** Isolated headless QA only: shorten the wait for the first boss gate. */
bool SurvivorsQaAdvanceBossGate();
/** Isolated headless QA only: start an encircle event on the next tick. */
bool SurvivorsQaTriggerEncircle();
#endif
uint32_t SurvivorsLoadedSectorEpoch();
Point SurvivorsSectorPosition();
uint32_t SurvivorsSectorEpoch();
uint32_t SurvivorsLevelSeed(uint32_t baseSeed);
Point SurvivorsSectorArrival(Point fallback, uint32_t seed);
bool RequestSurvivorsSectorTravel();
/** Persist earned progress once when a native game session ends, including disconnects. */
void FinalizeSurvivorsRun();
bool IsSurvivorsMode();
// The host must durably commit gear before sending snapshots that acknowledge
// gameplay input. These are no-ops outside authoritative cooperative hunts.
bool SurvivorsCoopPeerLedgerReady(unsigned peer);
bool SurvivorsCanTransferCoopGround(unsigned peer);
// The level being left is no longer checkpointed once an act or sector
// transition starts, so no item may move between it and a hunter's gear.
bool SurvivorsGroundTransferBlocked();
uint64_t SurvivorsCoopAdmissionNonce(unsigned peer);
uint64_t SurvivorsCoopLedgerGeneration(unsigned peer);
uint64_t SurvivorsCoopCheckpointGeneration(unsigned peer);
// 0: not cooperative, 1: syncing, 2: checkpoint saved locally,
// 3: session-only, 4: authority has saved its own gear.
unsigned SurvivorsCoopGearSyncStatus();
void SurvivorsObserveLedgerGeneration(uint64_t generation);
bool SurvivorsCommitCoopLedgers();
void SurvivorsForgetCoopLedger(unsigned peer);
void ProcessSurvivorsMode();
// Opt-in autonomous native playtest: normal commands and real simulation time.
bool IsSurvivorsPlaytestEnabled();
void ProcessSurvivorsPlaytestInput();
void DrawSurvivorsMode(const Surface &out);
bool IsSurvivorsUpgradePending();
bool IsSurvivorsDraftRequestPending();
bool ShouldPauseSurvivorsSimulation();
int SurvivorsBossMonsterId();
bool IsSurvivorsRunFinished();
// Returns false for invalid choices or when no level-up is pending.
bool ChooseSurvivorsUpgrade(unsigned choice);
bool ChooseSurvivorsUpgradeAt(Point position, Size screenSize);

} // namespace devilution
