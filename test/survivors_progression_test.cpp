#include <chrono>
#include <filesystem>
#include <fstream>
#include <limits>
#include <string>

#include <gtest/gtest.h>

#include "survivors_arsenal.h"
#include "survivors_progression.h"
#include "utils/paths.h"

namespace devilution {
namespace {

TEST(SurvivorsProfileData, RoundTripsAllProgressAndPermanentRanks)
{
	SurvivorsProfile profile;
	profile.soulShards = 4321;
	profile.totalKills = 9876;
	profile.runs = 12;
	profile.clearedActs = 23;
	profile.slainBosses = 25;
	profile.victories = 2;
	profile.bestSeconds = 1400;
	profile.bestLevel = 42;
	profile.unlockedDifficulty = 2;
	profile.selectedDifficulty = 1;
	profile.ranks = { 1, 5, 9, 10 };
	profile.autoMelee = false;
	const auto parsed = ParseSurvivorsProfile(SerializeSurvivorsProfile(profile));
	ASSERT_TRUE(parsed.has_value());
	EXPECT_EQ(*parsed, profile);
}

TEST(SurvivorsProfileData, LoadsVersionOneWithAutoMeleeEnabled)
{
	const std::string payload = "DamnationXSurvivors 1\n100 200 3 2 1 0 60 8 0 0 1 2 3 4\n";
	uint32_t checksum = 2166136261U;
	for (const unsigned char value : payload)
		checksum = (checksum ^ value) * 16777619U;
	const auto parsed = ParseSurvivorsProfile(payload + "checksum " + std::to_string(checksum) + "\n");
	ASSERT_TRUE(parsed.has_value());
	EXPECT_TRUE(parsed->autoMelee);
	EXPECT_EQ(parsed->ranks[3], 4U);
}

TEST(SurvivorsProfileData, RejectsTamperingTruncationAndOversizedInput)
{
	const std::string valid = SerializeSurvivorsProfile({ });
	for (size_t length = 0; length < valid.size() - 1; ++length)
		EXPECT_FALSE(ParseSurvivorsProfile(valid.substr(0, length)).has_value()) << length;
	std::string tampered = valid;
	tampered[tampered.find('\n') + 1] = '9';
	EXPECT_FALSE(ParseSurvivorsProfile(tampered).has_value());
	EXPECT_FALSE(ParseSurvivorsProfile(valid + "unexpected data").has_value());
	EXPECT_FALSE(ParseSurvivorsProfile(std::string(4097, 'x')).has_value());
}

TEST(SurvivorsProfileData, RejectsImpossibleRanksCurrenciesAndDifficulties)
{
	SurvivorsProfile profile;
	profile.ranks[0] = SurvivorsMaxPermanentRank + 1;
	EXPECT_TRUE(SerializeSurvivorsProfile(profile).empty());
	profile = { };
	profile.soulShards = SurvivorsMaxShards + 1;
	EXPECT_FALSE(IsValidSurvivorsProfile(profile));
	profile = { };
	profile.selectedDifficulty = 1;
	EXPECT_FALSE(IsValidSurvivorsProfile(profile));
	profile = { };
	profile.victories = 1;
	EXPECT_FALSE(IsValidSurvivorsProfile(profile));
}

TEST(SurvivorsProfileData, PurchaseSpendsExactlyOnceAndCannotOverspendOrExceedCap)
{
	SurvivorsProfile profile;
	const auto upgrade = SurvivorsPermanentUpgrade::Might;
	const auto initial = profile;
	EXPECT_FALSE(ApplySurvivorsPermanentPurchase(profile, upgrade));
	EXPECT_EQ(profile, initial);
	profile.soulShards = SurvivorsPermanentUpgradeCost(profile, upgrade);
	EXPECT_TRUE(ApplySurvivorsPermanentPurchase(profile, upgrade));
	EXPECT_EQ(profile.soulShards, 0U);
	EXPECT_EQ(profile.ranks[0], 1U);
	EXPECT_GT(SurvivorsPermanentUpgradeCost(profile, upgrade), 25U);
	profile.soulShards = SurvivorsMaxShards;
	while (profile.ranks[0] < SurvivorsMaxPermanentRank)
		ASSERT_TRUE(ApplySurvivorsPermanentPurchase(profile, upgrade));
	const auto capped = profile;
	EXPECT_FALSE(ApplySurvivorsPermanentPurchase(profile, upgrade));
	EXPECT_FALSE(ApplySurvivorsPermanentPurchase(profile, static_cast<SurvivorsPermanentUpgrade>(99)));
	EXPECT_EQ(profile, capped);
}

TEST(SurvivorsProfileData, RewardsIncludeBossAndActMilestonesWithoutOverflow)
{
	EXPECT_EQ(SurvivorsRunShardReward(0, 0, 0, false), 0U);
	const auto kills = SurvivorsRunShardReward(100, 0, 0, false);
	const auto act = SurvivorsRunShardReward(100, 1, 0, false);
	const auto boss = SurvivorsRunShardReward(100, 1, 1, false);
	EXPECT_GT(kills, 0U);
	EXPECT_GT(act, kills);
	EXPECT_GT(boss, act);
	EXPECT_GT(SurvivorsRunShardReward(100, 1, 1, true), boss);
	const auto maximum = std::numeric_limits<unsigned>::max();
	EXPECT_LE(SurvivorsRunShardReward(maximum, maximum, maximum, true), SurvivorsMaxShards);
}

class SurvivorsProfileStorage : public testing::Test {
protected:
	void SetUp() override
	{
		folder = std::filesystem::temp_directory_path() / ("damnationx-profile-test-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
		std::filesystem::create_directories(folder);
		path = (folder / "survivors-profile.dat").string();
		previousPrefPath = paths::PrefPath();
		paths::SetPrefPath(folder.string() + "/");
	}
	void TearDown() override
	{
		paths::SetPrefPath(previousPrefPath);
		std::filesystem::remove_all(folder);
	}
	static void Write(const std::string &filename, std::string_view data)
	{
		std::ofstream stream(filename, std::ios::binary | std::ios::trunc);
		stream << data;
	}
	static std::string Read(const std::string &filename)
	{
		std::ifstream stream(filename, std::ios::binary);
		return { std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>() };
	}
	std::filesystem::path folder;
	std::string path;
	std::string previousPrefPath;
};

TEST_F(SurvivorsProfileStorage, PersistsPurchasesAndNeverTouchesCampaignFiles)
{
	const auto campaign = (folder / "single_0.sv").string();
	Write(campaign, "campaign sentinel");
	SurvivorsProfile profile;
	profile.soulShards = 100;
	ASSERT_TRUE(ApplySurvivorsPermanentPurchase(profile, SurvivorsPermanentUpgrade::Vitality));
	ASSERT_TRUE(SaveSurvivorsProfileToPath(path, profile));
	SurvivorsProfile loaded;
	ASSERT_TRUE(LoadSurvivorsProfileFromPath(path, loaded));
	EXPECT_EQ(loaded, profile);
	EXPECT_EQ(Read(campaign), "campaign sentinel");
	EXPECT_FALSE(std::filesystem::exists(path + ".tmp"));
}

TEST_F(SurvivorsProfileStorage, RecoversBackupWithoutReplacingItWithCorruptPrimary)
{
	SurvivorsProfile first;
	first.soulShards = 100;
	ASSERT_TRUE(SaveSurvivorsProfileToPath(path, first));
	auto second = first;
	second.soulShards = 200;
	ASSERT_TRUE(SaveSurvivorsProfileToPath(path, second));
	const std::string backup = Read(path + ".bak");
	Write(path, "truncated");
	SurvivorsProfile loaded;
	ASSERT_TRUE(LoadSurvivorsProfileFromPath(path, loaded));
	EXPECT_EQ(loaded, first);
	ASSERT_TRUE(SaveSurvivorsProfileToPath(path, second));
	EXPECT_EQ(Read(path + ".bak"), backup);
	ASSERT_TRUE(LoadSurvivorsProfileFromPath(path, loaded));
	EXPECT_EQ(loaded, second);
}

TEST_F(SurvivorsProfileStorage, InvalidOrMissingLoadsDoNotMutateCallerAndInvalidSavesAreInert)
{
	SurvivorsProfile result;
	result.soulShards = 777;
	const auto before = result;
	EXPECT_FALSE(LoadSurvivorsProfileFromPath(path, result));
	EXPECT_EQ(result, before);
	Write(path, "damaged profile");
	EXPECT_FALSE(LoadSurvivorsProfileFromPath(path, result));
	EXPECT_EQ(result, before);
	auto invalid = result;
	invalid.ranks[0] = 999;
	EXPECT_FALSE(SaveSurvivorsProfileToPath(path, invalid));
	EXPECT_EQ(Read(path), "damaged profile");
}

TEST_F(SurvivorsProfileStorage, VictoryUnlocksNextDifficultyAndBonusesSurviveReload)
{
	ASSERT_TRUE(LoadSurvivorsProfile());
	EXPECT_FALSE(SelectSurvivorsDifficulty(1));
	ASSERT_TRUE(RecordSurvivorsRunProgress(100, 10, 5, 5, 1300, true, 0));
	EXPECT_EQ(GetSurvivorsProfile().unlockedDifficulty, 1U);
	EXPECT_FALSE(SelectSurvivorsDifficulty(2));
	ASSERT_TRUE(SelectSurvivorsDifficulty(1));
	ASSERT_TRUE(PurchaseSurvivorsPermanentUpgrade(SurvivorsPermanentUpgrade::Might));
	ASSERT_TRUE(PurchaseSurvivorsPermanentUpgrade(SurvivorsPermanentUpgrade::Vitality));
	const auto saved = GetSurvivorsProfile();
	ASSERT_TRUE(LoadSurvivorsProfile());
	EXPECT_EQ(GetSurvivorsProfile(), saved);
	EXPECT_EQ(SurvivorsSelectedDifficulty(), 1U);
	EXPECT_EQ(GetSurvivorsRunBonuses().damage, 2U);
	EXPECT_EQ(GetSurvivorsRunBonuses().vitality, 5U);
	ASSERT_TRUE(RecordSurvivorsRunProgress(100, 10, 5, 5, 1300, true, 1));
	EXPECT_EQ(GetSurvivorsProfile().unlockedDifficulty, 2U);
	EXPECT_EQ(GetSurvivorsProfile().victories, 2U);
	EXPECT_EQ(GetSurvivorsProfile().runs, 2U);
	ASSERT_TRUE(SelectSurvivorsDifficulty(2));
	ASSERT_TRUE(LoadSurvivorsProfile());
	EXPECT_EQ(SurvivorsSelectedDifficulty(), 2U);
}

TEST_F(SurvivorsProfileStorage, UnreadableActiveProfileCannotBeOverwrittenByRewardsOrPurchases)
{
	const std::string previousPath = paths::PrefPath();
	paths::SetPrefPath(folder.string() + "/");
	Write(path, "damaged profile");
	EXPECT_FALSE(LoadSurvivorsProfile());
	EXPECT_FALSE(PurchaseSurvivorsPermanentUpgrade(SurvivorsPermanentUpgrade::Might));
	EXPECT_FALSE(RecordSurvivorsRunProgress(100, 10, 1, 1, 240, false, 0));
	EXPECT_FALSE(SaveSurvivorsProfile());
	EXPECT_EQ(Read(path), "damaged profile");
	paths::SetPrefPath(previousPath);
}

} // namespace
} // namespace devilution
