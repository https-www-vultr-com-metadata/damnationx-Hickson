#pragma once

#include <algorithm>
#include <array>
#include <cstdint>
#include <string>

namespace devilution {

struct Player;

inline constexpr unsigned SurvivorsWeaponCount = 10;
inline constexpr unsigned SurvivorsPassiveCount = 6;
inline constexpr unsigned SurvivorsWeaponSlots = 6;
inline constexpr unsigned SurvivorsWeaponMaxRank = 6;
inline constexpr unsigned SurvivorsPassiveMaxRank = 5;

inline constexpr unsigned SurvivorsArsenalFirstFallback = SurvivorsWeaponCount + SurvivorsPassiveCount;
inline constexpr unsigned SurvivorsArsenalInvalidOffer = SurvivorsArsenalFirstFallback + 3;

struct SurvivorsArsenalSnapshot {
	std::array<unsigned, SurvivorsWeaponCount> ranks { };
	std::array<unsigned, SurvivorsPassiveCount> passives { };
	std::array<bool, SurvivorsArsenalFirstFallback> banished { };
	std::array<unsigned, 3> offers { SurvivorsArsenalInvalidOffer, SurvivorsArsenalInvalidOffer, SurvivorsArsenalInvalidOffer };
	unsigned rerolls = 3;
	unsigned skips = 2;
	unsigned banishes = 2;
	bool drafted = false;
};

// The draft uses this same pure eligibility rule; callers can inspect a snapshot
// without mutating the live run. Item IDs are weapons, then passives, then rites.
bool SurvivorsArsenalCanOffer(const SurvivorsArsenalSnapshot &state, unsigned item);
unsigned SurvivorsArsenalEvolutionCatalyst(unsigned weapon);
SurvivorsArsenalSnapshot GetSurvivorsArsenalSnapshot();

// Permanent bonuses are expressed in actual units, not profile upgrade ranks.
struct SurvivorsArsenalBonuses {
	unsigned damage = 0;
	unsigned haste = 0;
	unsigned magnet = 0;
	unsigned vitality = 0;
	unsigned rerolls = 0;
	bool autoMelee = true;
};

// Helpers shared by the runtime and tests. Milliseconds keep cooldowns independent
// of the configured simulation rate. No weapon fires faster than five times/sec.
constexpr unsigned SurvivorsWeaponCooldown(unsigned milliseconds, unsigned rank, unsigned haste, unsigned tickRate)
{
	const unsigned reduction = std::min(65U, std::min(haste, 20U) * 5 + std::min(rank, SurvivorsWeaponMaxRank) * 3);
	const unsigned adjusted = std::max(200U, std::min(milliseconds, 10000U) * (100 - reduction) / 100);
	return std::max(1U, (adjusted * std::clamp(tickRate, 1U, 120U) + 999) / 1000);
}
constexpr bool SurvivorsWeaponCanAdvance(unsigned rank, unsigned catalystRank)
{
	return rank < 5 || (rank == 5 && catalystRank >= 2);
}
constexpr unsigned SurvivorsWeaponDamage(unsigned level, unsigned rank, unsigned bonus)
{
	return 5 + std::min(level, 1000U) * 2 + std::min(rank, SurvivorsWeaponMaxRank) * 5 + std::min(bonus, 1000U);
}

struct SurvivorsWeaponBalanceStats { unsigned cooldownMilliseconds; unsigned damagePercent; };
inline constexpr std::array<SurvivorsWeaponBalanceStats, SurvivorsWeaponCount> SurvivorsWeaponBalance { {
	{ 650, 100 }, { 850, 100 }, { 1600, 170 }, { 1450, 65 }, { 2400, 80 },
	{ 1000, 180 }, { 3000, 60 }, { 2600, 100 }, { 1300, 125 }, { 1800, 160 },
} };
constexpr unsigned SurvivorsWeaponImpactDamage(unsigned weapon, unsigned level, unsigned rank, unsigned bonus)
{
	return weapon < SurvivorsWeaponCount ? SurvivorsWeaponDamage(level, rank, bonus) * SurvivorsWeaponBalance[weapon].damagePercent / 100 : 0;
}
// As in D2 Survivors, each point of effective Magic above the class's starting
// Magic grants 1% spell damage. Physical arrows and melee do not benefit.
constexpr unsigned SurvivorsWeaponStatDamage(unsigned weapon, unsigned damage, int magic, int startingMagic)
{
	if (weapon >= SurvivorsWeaponCount || weapon == 1 || weapon == 9)
		return weapon < SurvivorsWeaponCount ? damage : 0;
	const auto bonus = std::clamp<int64_t>(static_cast<int64_t>(magic) - startingMagic, 0, 1000);
	return static_cast<unsigned>(std::min<uint64_t>(static_cast<uint64_t>(damage) * (100 + bonus) / 100, UINT32_MAX));
}
// Reference rate for ONE impact per cast, in thousandths of damage/sec. This
// deliberately excludes accuracy, extra projectiles, pierce, AOE and DOT uptime;
// the native playtest measures those effects instead of pretending they are hits.
constexpr unsigned SurvivorsWeaponReferenceDpsMilli(unsigned weapon, unsigned level, unsigned rank, unsigned bonus, unsigned haste, unsigned tickRate)
{
	if (weapon >= SurvivorsWeaponCount) return 0;
	return SurvivorsWeaponImpactDamage(weapon, level, rank, bonus) * std::clamp(tickRate, 1U, 120U) * 1000 / SurvivorsWeaponCooldown(SurvivorsWeaponBalance[weapon].cooldownMilliseconds, rank, haste, tickRate);
}

void ResetSurvivorsArsenal();
void ResetAllSurvivorsArsenals();
void SelectSurvivorsArsenalPlayer(unsigned playerId);
bool ApplySurvivorsArsenalReplica(unsigned playerId, const SurvivorsArsenalSnapshot &snapshot);
void StartSurvivorsArsenal(Player &player, const SurvivorsArsenalBonuses &bonuses, uint32_t seed);
void ProcessSurvivorsArsenal(unsigned ticks, unsigned tickRate, unsigned level);
/** Make room for a new shot by retiring this owner's oldest projectile, never enemy shots. */
unsigned ReclaimSurvivorsArsenalProjectiles(unsigned owner);
void GrantSurvivorsArsenalLife(unsigned hitPoints);
void DraftSurvivorsArsenal();
struct SurvivorsArsenalOfferInfo {
	unsigned item = SurvivorsArsenalInvalidOffer;
	unsigned currentRank = 0;
	unsigned nextRank = 0;
	unsigned maxRank = 0;
	bool isWeapon = false;
	bool evolution = false;
	bool enabled = false;
	std::string name;
	std::string description;
	std::string requirements;
};
std::array<SurvivorsArsenalOfferInfo, 3> SurvivorsArsenalOfferDetails();
std::string SurvivorsArsenalItemName(unsigned item);
std::array<std::string, 3> SurvivorsArsenalOfferLabels();
bool ChooseSurvivorsArsenalUpgrade(unsigned choice);
bool RerollSurvivorsArsenal();
bool BanishSurvivorsArsenalOffer(unsigned choice);
void SkipSurvivorsArsenalDraft();
bool CanSkipSurvivorsArsenalDraft();
unsigned SurvivorsArsenalMagnet();
std::string SurvivorsArsenalSummary();
std::string SurvivorsArsenalDraftResources();

} // namespace devilution
