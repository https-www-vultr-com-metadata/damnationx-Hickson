#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace devilution {

struct SurvivorsArsenalBonuses;

enum class SurvivorsPermanentUpgrade : unsigned { Might,
	Vitality,
	Haste,
	Magnet };
inline constexpr uint32_t SurvivorsMaxPermanentRank = 10;
inline constexpr uint32_t SurvivorsMaxShards = 10000000;
struct SurvivorsProfile {
	uint32_t soulShards = 0;
	uint32_t totalKills = 0;
	uint32_t runs = 0;
	uint32_t clearedActs = 0;
	uint32_t slainBosses = 0;
	uint32_t victories = 0;
	uint32_t bestSeconds = 0;
	uint32_t bestLevel = 0;
	uint32_t unlockedDifficulty = 0;
	uint32_t selectedDifficulty = 0;
	bool autoMelee = true;
	std::array<uint32_t, 4> ranks { };
	bool operator==(const SurvivorsProfile &) const = default;
};

bool IsValidSurvivorsProfile(const SurvivorsProfile &profile);
std::string SerializeSurvivorsProfile(const SurvivorsProfile &profile);
std::optional<SurvivorsProfile> ParseSurvivorsProfile(std::string_view data);
// The explicit-path operations never touch the active profile or campaign data.
// Loading only assigns result after a valid primary or backup has been found.
bool LoadSurvivorsProfileFromPath(std::string_view path, SurvivorsProfile &result);
bool SaveSurvivorsProfileToPath(std::string_view path, const SurvivorsProfile &profile);
unsigned SurvivorsPermanentUpgradeCost(const SurvivorsProfile &profile, SurvivorsPermanentUpgrade upgrade);
bool ApplySurvivorsPermanentPurchase(SurvivorsProfile &profile, SurvivorsPermanentUpgrade upgrade);
unsigned SurvivorsRunShardReward(unsigned kills, unsigned acts, unsigned bosses, bool victory);

const SurvivorsProfile &GetSurvivorsProfile();
bool LoadSurvivorsProfile();
bool SaveSurvivorsProfile();
bool PurchaseSurvivorsPermanentUpgrade(SurvivorsPermanentUpgrade upgrade);
unsigned SurvivorsPermanentRank(SurvivorsPermanentUpgrade upgrade);
SurvivorsArsenalBonuses GetSurvivorsRunBonuses();
unsigned SurvivorsSelectedDifficulty();
bool SelectSurvivorsDifficulty(unsigned difficulty);
bool SetSurvivorsAutoMelee(bool enabled);
bool RecordSurvivorsRunProgress(unsigned kills, unsigned runLevel, unsigned actsCleared, unsigned bosses, unsigned seconds, bool victory, unsigned difficulty);
const std::string &SurvivorsProgressionStatus();

} // namespace devilution
