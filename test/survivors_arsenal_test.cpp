#include <algorithm>
#include <array>
#include <limits>

#include <gtest/gtest.h>

#include "player.h"
#include "survivors_arsenal.h"

namespace devilution {
namespace {

TEST(SurvivorsWeapons, EvolutionRequiresMatchingCatalystAndCannotExceedFinalRank)
{
	for (unsigned weapon = 0; weapon < SurvivorsWeaponCount; ++weapon) {
		SurvivorsArsenalSnapshot state;
		const auto catalyst = SurvivorsArsenalEvolutionCatalyst(weapon);
		ASSERT_LT(catalyst, SurvivorsPassiveCount);
		state.ranks[weapon] = 5;
		EXPECT_FALSE(SurvivorsArsenalCanOffer(state, weapon));
		state.passives[catalyst] = 1;
		EXPECT_FALSE(SurvivorsArsenalCanOffer(state, weapon));
		state.passives[catalyst] = 2;
		EXPECT_TRUE(SurvivorsArsenalCanOffer(state, weapon));
		state.ranks[weapon] = SurvivorsWeaponMaxRank;
		EXPECT_FALSE(SurvivorsArsenalCanOffer(state, weapon));
	}
}

TEST(SurvivorsWeapons, FullSlotsPermitOwnedWeaponUpgradesButRejectNewWeapons)
{
	SurvivorsArsenalSnapshot state;
	for (unsigned weapon = 0; weapon < SurvivorsWeaponSlots; ++weapon)
		state.ranks[weapon] = 1;
	EXPECT_TRUE(SurvivorsArsenalCanOffer(state, 0));
	EXPECT_FALSE(SurvivorsArsenalCanOffer(state, SurvivorsWeaponSlots));
	state.banished[0] = true;
	EXPECT_FALSE(SurvivorsArsenalCanOffer(state, 0));
	state.passives[0] = SurvivorsPassiveMaxRank;
	EXPECT_FALSE(SurvivorsArsenalCanOffer(state, SurvivorsWeaponCount));
	EXPECT_FALSE(SurvivorsArsenalCanOffer(state, SurvivorsArsenalInvalidOffer));
}

TEST(SurvivorsWeapons, CooldownsPreserveRealTimeAndRemainBoundedAtExtremeInputs)
{
	for (const auto rate : { 20U, 40U, 60U, 120U }) {
		const auto ticks = SurvivorsWeaponCooldown(2400, 3, 2, rate);
		const auto milliseconds = ticks * 1000 / rate;
		EXPECT_GE(milliseconds, 1900U);
		EXPECT_LE(milliseconds, 2000U);
	}
	const auto maximum = std::numeric_limits<unsigned>::max();
	EXPECT_GE(SurvivorsWeaponCooldown(maximum, maximum, maximum, 0), 1U);
	EXPECT_LE(SurvivorsWeaponCooldown(maximum, maximum, maximum, maximum), 1200U);
	EXPECT_GT(SurvivorsWeaponDamage(10, 2, 4), SurvivorsWeaponDamage(1, 1, 0));
}

TEST(SurvivorsWeapons, MagicAboveClassStartingValueIncreasesOnlySpellDamage)
{
	for (unsigned weapon = 0; weapon < SurvivorsWeaponCount; ++weapon) {
		const unsigned base = SurvivorsWeaponImpactDamage(weapon, 10, 2, 4);
		EXPECT_EQ(SurvivorsWeaponStatDamage(weapon, base, 20, 20), base);
		EXPECT_EQ(SurvivorsWeaponStatDamage(weapon, base, 10, 20), base);
		const unsigned expected = weapon == 1 || weapon == 9 ? base : base * 125 / 100;
		EXPECT_EQ(SurvivorsWeaponStatDamage(weapon, base, 45, 20), expected);
	}
	EXPECT_EQ(SurvivorsWeaponStatDamage(SurvivorsWeaponCount, 100, 100, 0), 0U);
	EXPECT_EQ(SurvivorsWeaponStatDamage(0, 100, std::numeric_limits<int>::max(), std::numeric_limits<int>::min()), 1100U);
}

class SurvivorsDraft : public testing::Test {
protected:
	void SetUp() override
	{
		hero._pClass = HeroClass::Sorcerer;
		hero._pMaxHP = hero._pMaxHPBase = 100 << 6;
		hero._pHitPoints = hero._pHPBase = 50 << 6;
		StartSurvivorsArsenal(hero, { }, 12345);
	}
	void TearDown() override { ResetSurvivorsArsenal(); }
	void ExpectValidDraft()
	{
		const auto state = GetSurvivorsArsenalSnapshot();
		ASSERT_TRUE(state.drafted);
		EXPECT_NE(state.offers[0], state.offers[1]);
		EXPECT_NE(state.offers[1], state.offers[2]);
		EXPECT_NE(state.offers[0], state.offers[2]);
		for (const auto item : state.offers)
			EXPECT_TRUE(SurvivorsArsenalCanOffer(state, item)) << item;
	}
	Player hero { };
};

TEST_F(SurvivorsDraft, WarriorStartsWithMeleeOnlyAndClassesHaveDistinctArsenals)
{
	std::array<std::array<unsigned, SurvivorsWeaponCount>, 3> starts;
	unsigned index = 0;
	for (const auto playerClass : { HeroClass::Warrior, HeroClass::Rogue, HeroClass::Sorcerer }) {
		hero._pClass = playerClass;
		StartSurvivorsArsenal(hero, { }, 12345);
		starts[index] = GetSurvivorsArsenalSnapshot().ranks;
		EXPECT_EQ(std::count(starts[index].begin(), starts[index].end(), 1U), playerClass == HeroClass::Warrior ? 1 : 2);
		++index;
	}
	EXPECT_EQ(starts[0][0], 0U); // Ember is a ranged fire spell, never a Warrior starter.
	EXPECT_EQ(starts[0][9], 1U); // Sanctified Edge is the Warrior's close-range strike.
	EXPECT_NE(starts[0], starts[1]);
	EXPECT_NE(starts[1], starts[2]);
	EXPECT_NE(starts[0], starts[2]);
}

TEST_F(SurvivorsDraft, DraftSequenceIsDeterministicAndRejectsRepeatedConsumption)
{
	DraftSurvivorsArsenal();
	ExpectValidDraft();
	const auto first = GetSurvivorsArsenalSnapshot().offers;
	ASSERT_TRUE(ChooseSurvivorsArsenalUpgrade(0));
	EXPECT_FALSE(ChooseSurvivorsArsenalUpgrade(0));
	StartSurvivorsArsenal(hero, { }, 12345);
	DraftSurvivorsArsenal();
	EXPECT_EQ(GetSurvivorsArsenalSnapshot().offers, first);
	EXPECT_FALSE(ChooseSurvivorsArsenalUpgrade(3));
}

TEST_F(SurvivorsDraft, RerollsSkipsAndBanishesSpendOnlyTheirOwnLimitedResources)
{
	DraftSurvivorsArsenal();
	const auto initial = GetSurvivorsArsenalSnapshot();
	const auto banned = initial.offers[0];
	ASSERT_LT(banned, SurvivorsArsenalFirstFallback);
	ASSERT_TRUE(BanishSurvivorsArsenalOffer(0));
	auto state = GetSurvivorsArsenalSnapshot();
	EXPECT_EQ(state.banishes, initial.banishes - 1);
	EXPECT_EQ(state.rerolls, initial.rerolls);
	EXPECT_TRUE(state.banished[banned]);
	EXPECT_EQ(std::count(state.offers.begin(), state.offers.end(), banned), 0);
	ExpectValidDraft();
	for (unsigned reroll = 0; reroll < initial.rerolls; ++reroll) {
		ASSERT_TRUE(RerollSurvivorsArsenal());
		ExpectValidDraft();
		state = GetSurvivorsArsenalSnapshot();
		EXPECT_EQ(std::count(state.offers.begin(), state.offers.end(), banned), 0);
	}
	EXPECT_FALSE(RerollSurvivorsArsenal());
	for (unsigned skip = 0; skip < initial.skips; ++skip) {
		ASSERT_TRUE(CanSkipSurvivorsArsenalDraft());
		SkipSurvivorsArsenalDraft();
		EXPECT_FALSE(GetSurvivorsArsenalSnapshot().drafted);
		DraftSurvivorsArsenal();
	}
	EXPECT_FALSE(CanSkipSurvivorsArsenalDraft());
}

TEST_F(SurvivorsDraft, LongBuildNeverExceedsSlotsOrRanksAndDraftsNeverDeadEnd)
{
	for (unsigned level = 0; level < 300; ++level) {
		DraftSurvivorsArsenal();
		ExpectValidDraft();
		ASSERT_TRUE(ChooseSurvivorsArsenalUpgrade(level % 3));
		const auto state = GetSurvivorsArsenalSnapshot();
		EXPECT_LE(std::count_if(state.ranks.begin(), state.ranks.end(), [](unsigned rank) { return rank > 0; }), SurvivorsWeaponSlots);
		for (const auto rank : state.ranks)
			EXPECT_LE(rank, SurvivorsWeaponMaxRank);
		for (const auto rank : state.passives)
			EXPECT_LE(rank, SurvivorsPassiveMaxRank);
	}
}

TEST_F(SurvivorsDraft, PermanentBonusesApplyOnceAndResetBetweenRuns)
{
	SurvivorsArsenalBonuses bonuses;
	bonuses.vitality = 15;
	bonuses.magnet = 3;
	bonuses.rerolls = 2;
	const int before = hero._pMaxHP;
	StartSurvivorsArsenal(hero, bonuses, 12345);
	EXPECT_EQ(hero._pMaxHP, before + (15 << 6));
	EXPECT_EQ(SurvivorsArsenalMagnet(), 3U);
	EXPECT_EQ(GetSurvivorsArsenalSnapshot().rerolls, 5U);
	ResetSurvivorsArsenal();
	EXPECT_EQ(SurvivorsArsenalMagnet(), 0U);
	EXPECT_FALSE(ChooseSurvivorsArsenalUpgrade(0));
}

} // namespace
} // namespace devilution
