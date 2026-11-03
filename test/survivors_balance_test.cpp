#include <limits>

#include <gtest/gtest.h>

#include "survivors.h"
#include "player.h"
#include "headless_mode.hpp"
#include "missiles.h"
#include "monster.h"
#include "levels/dun_tile_data.hpp"
#include "survivors_arsenal.h"

namespace devilution {
namespace {
TEST(SurvivorsBalance, ContactGraceRejectsSameFrameBurstsAndKeepsPlayersIndependent)
{
	for (unsigned rate : { 20U, 30U, 60U, 120U }) {
		SurvivorsDamageGrace first, second;
		ASSERT_TRUE(first.Accept(100, rate));
		EXPECT_FALSE(first.Accept(100, rate));
		EXPECT_TRUE(second.Accept(100, rate));
		const unsigned duration = SurvivorsDamageGraceTicks(rate);
		EXPECT_FALSE(first.Accept(100 + duration - 1, rate));
		EXPECT_TRUE(first.Accept(100 + duration, rate));
		first = { }; // Act/run reset.
		EXPECT_TRUE(first.Accept(0, rate));
	}
	SurvivorsDamageGrace rollover;
	EXPECT_TRUE(rollover.Accept(std::numeric_limits<uint32_t>::max(), 20));
	EXPECT_FALSE(rollover.Accept(std::numeric_limits<uint32_t>::max(), 20));
	EXPECT_GE(SurvivorsDamageGraceTicks(0), 1U);
	EXPECT_LE(SurvivorsDamageGraceTicks(std::numeric_limits<unsigned>::max()), 36U);
}

TEST(SurvivorsBalance, NetworkLifeValidationAllowsFiniteModeBonusesButRejectsOverflow)
{
	const int base = 70 << 6;
	EXPECT_TRUE(SurvivorsPackedLifeInRange(base, 90 << 6));
	EXPECT_TRUE(SurvivorsPackedLifeInRange(base, base + static_cast<int>(SurvivorsMaximumLifeBonus * 64)));
	EXPECT_FALSE(SurvivorsPackedLifeInRange(base, base + static_cast<int>(SurvivorsMaximumLifeBonus * 64) + 1));
	EXPECT_FALSE(SurvivorsPackedLifeInRange(base, std::numeric_limits<int32_t>::max()));
	EXPECT_FALSE(SurvivorsPackedLifeInRange(base, -1));
	EXPECT_FALSE(SurvivorsPackedLifeInRange(0, 64));
}

TEST(SurvivorsBalance, HeadlessChargingEnemiesKeepNativePhysicsWithoutRenderingSprites)
{
	const bool wasHeadless = HeadlessMode;
	Monster oldMonster = std::move(Monsters[0]);
	CMonster oldType = std::move(LevelMonsterTypes[0]);
	const Point tile { 50, 50 };
	const auto oldFlags = dFlags[tile.x][tile.y];
	HeadlessMode = true;
	Monsters[0] = { };
	Monsters[0].levelType = 0;
	Monsters[0].uniqueType = UniqueMonsterType::None;
	LevelMonsterTypes[0] = { };
	LevelMonsterTypes[0].type = MT_HORNED;
	auto &animation = LevelMonsterTypes[0].anims[static_cast<unsigned>(MonsterGraphic::Special)];
	animation.frames = 8;
	animation.rate = 2;
	animation.width = 96;
	Missile missile { };
	missile._mitype = MissileID::Rhino;
	missile._miAnimType = MissileGraphicID::None; // Native Rhino has no missile sprite sheet.
	missile._misource = 0;
	missile._midam = 23;
	missile.position.start = missile.position.tile = tile;
	AddMissileParameter parameter { };
	parameter.dst = { 54, 54 };
	parameter.midir = Direction::South;
	AddRhino(missile, parameter);
	EXPECT_FALSE(missile._miAnimData.has_value());
	EXPECT_EQ(missile._miAnimLen, 8);
	EXPECT_EQ(missile._miAnimDelay, 2);
	EXPECT_EQ(missile._miAnimWidth, 96);
	EXPECT_EQ(missile._midam, 23);
	EXPECT_EQ(missile.duration, 256);
	EXPECT_NE(missile.position.velocity, Displacement {});
	dFlags[tile.x][tile.y] = oldFlags;
	Monsters[0] = std::move(oldMonster);
	LevelMonsterTypes[0] = std::move(oldType);
	HeadlessMode = wasHeadless;
}

TEST(SurvivorsBalance, EnemyAndOtherPlayerMissilesCannotSilenceAnArsenal)
{
	auto previous = std::move(Missiles);
	Missiles.clear();
	const auto add = [](int source, auto caster, unsigned count) {
		for (unsigned i = 0; i < count; ++i) {
			Missile missile { };
			missile._misource = source;
			missile._micaster = caster;
			missile._mitype = MissileID::Firebolt;
			Missiles.push_back(std::move(missile));
		}
	};
	add(0, TARGET_PLAYERS, 400); // A horde's volleys exceed the old global cap.
	add(1, TARGET_MONSTERS, 320); // Another teammate has an independent budget.
	EXPECT_EQ(ReclaimSurvivorsArsenalProjectiles(0), 0U);
	add(0, TARGET_MONSTERS, 320);
	EXPECT_EQ(ReclaimSurvivorsArsenalProjectiles(0), 1U);
	EXPECT_EQ(std::count_if(Missiles.begin(), Missiles.end(), [](const Missile &m) { return m._miDelFlag; }), 1);
	const auto retired = std::find_if(Missiles.begin(), Missiles.end(), [](const Missile &m) { return m._miDelFlag; });
	ASSERT_NE(retired, Missiles.end());
	EXPECT_EQ(retired->_misource, 0);
	EXPECT_EQ(retired->_micaster, TARGET_MONSTERS);
	EXPECT_EQ(ReclaimSurvivorsArsenalProjectiles(0), 0U); // A slot is now free.
	Missiles = std::move(previous);
}

TEST(SurvivorsBalance, NativeFireWallDescendantsShareTheirOwnersBoundedQuota)
{
	auto previous = std::move(Missiles);
	Missiles.clear();
	for (unsigned i = 0; i < 320; ++i) {
		Missile wall { };
		wall._misource = 2;
		wall._micaster = TARGET_BOTH;
		wall._mitype = MissileID::FireWall;
		Missiles.push_back(std::move(wall));
	}
	Missile trap { };
	trap._misource = -1;
	trap._micaster = TARGET_BOTH;
	trap._mitype = MissileID::FireWall;
	Missiles.push_front(trap);
	EXPECT_EQ(ReclaimSurvivorsArsenalProjectiles(1), 0U);
	EXPECT_EQ(ReclaimSurvivorsArsenalProjectiles(2), 1U);
	EXPECT_FALSE(Missiles.front()._miDelFlag); // An unowned trap is not a player effect.
	const auto retired = std::find_if(Missiles.begin(), Missiles.end(), [](const Missile &m) { return m._miDelFlag; });
	ASSERT_NE(retired, Missiles.end());
	EXPECT_EQ(retired->_micaster, TARGET_BOTH);
	EXPECT_EQ(retired->_misource, 2);
	EXPECT_EQ(ReclaimSurvivorsArsenalProjectiles(2), 0U);
	Missiles = std::move(previous);
}

TEST(SurvivorsBalance, HordePressureGrowsWithTheArsenalAndRemainsBounded)
{
	unsigned previousRate = 0;
	for (unsigned wave = 1; wave <= 20; ++wave) {
		const unsigned rateMilli = 20000 * SurvivorsSpawnBatch(wave) / SurvivorsSpawnInterval(wave);
		EXPECT_GE(rateMilli, previousRate);
		EXPECT_LE(rateMilli, 20000U);
		EXPECT_LE(SurvivorsSpawnBatch(wave), 4U);
		previousRate = rateMilli;
	}
	EXPECT_GE(previousRate, 10 * (20000 * SurvivorsSpawnBatch(1) / SurvivorsSpawnInterval(1)));
	EXPECT_LE(SurvivorsSpawnBatch(std::numeric_limits<unsigned>::max()), 4U);
	EXPECT_GT(SurvivorsSpawnInterval(std::numeric_limits<unsigned>::max()), 0U);
}

TEST(SurvivorsBalance, AllTenWeaponReferenceRatesScaleWithoutExceedingCooldownBounds)
{
	for (unsigned weapon = 0; weapon < SurvivorsWeaponCount; ++weapon) {
		const unsigned early = SurvivorsWeaponReferenceDpsMilli(weapon, 5, 1, 0, 0, 20);
		const unsigned middle = SurvivorsWeaponReferenceDpsMilli(weapon, 25, 3, 12, 2, 20);
		const unsigned evolved = SurvivorsWeaponReferenceDpsMilli(weapon, 55, 6, 40, 5, 20);
		EXPECT_GT(early, 0U) << weapon;
		EXPECT_GT(middle, early) << weapon;
		EXPECT_GT(evolved, middle) << weapon;
		for (unsigned tickRate : { 20U, 30U, 60U, 120U }) {
			const unsigned cooldown = SurvivorsWeaponCooldown(SurvivorsWeaponBalance[weapon].cooldownMilliseconds, 6, 100, tickRate);
			EXPECT_GE(cooldown * 1000, tickRate * 200) << weapon;
		}
	}
	EXPECT_EQ(SurvivorsWeaponReferenceDpsMilli(SurvivorsWeaponCount, 20, 3, 0, 0, 20), 0U);
}

TEST(SurvivorsBalance, BossDurabilitySupportsLongerFightsAndAllPartySizesFitFixedPointHealth)
{
	unsigned previous = 0;
	for (unsigned act = 0; act < SurvivorsActCount; ++act) {
		const unsigned normal = SurvivorsBossHitPoints(act, 0);
		EXPECT_GT(normal, previous);
		EXPECT_EQ(SurvivorsBossHitPoints(act, 1), normal * 3);
		EXPECT_EQ(SurvivorsBossHitPoints(act, 2), normal * 5);
		const uint64_t fullPartyHell = static_cast<uint64_t>(SurvivorsBossHitPoints(act, 2)) * 325 / 100;
		EXPECT_LT(fullPartyHell * 64, static_cast<uint64_t>(std::numeric_limits<int>::max()));
		previous = normal;
	}
	EXPECT_GE(SurvivorsBossHitPoints(0, 0), 2000U);
}

TEST(SurvivorsBalance, ExpeditionVigorStacksWithPurchasedVitalityAndStopsAfterArsenalReset)
{
	ResetAllSurvivorsArsenals();
	SelectSurvivorsArsenalPlayer(0);
	Player hero { };
	hero._pMaxHP = hero._pMaxHPBase = 30 << 6;
	hero._pHitPoints = hero._pHPBase = 30 << 6;
	SurvivorsArsenalBonuses bonuses;
	bonuses.vitality = 15;
	StartSurvivorsArsenal(hero, bonuses, 1);
	GrantSurvivorsArsenalLife(SurvivorsStartingLifeBonus);
	for (unsigned clear = 0; clear < 4; ++clear) GrantSurvivorsArsenalLife(SurvivorsActLifeBonus);
	EXPECT_EQ(hero._pMaxHP, (30 + 15 + 20 + 4 * 15) << 6);
	EXPECT_EQ(hero._pMaxHPBase, hero._pMaxHP);
	ResetAllSurvivorsArsenals();
	GrantSurvivorsArsenalLife(100);
	EXPECT_EQ(hero._pMaxHP, (30 + 15 + 20 + 4 * 15) << 6);
}

TEST(SurvivorsUpgradeCards, EveryVisibleButtonHasItsOwnNonOverlappingAction)
{
	for (const Size screen : { Size { 640, 480 }, Size { 860, 480 }, Size { 1280, 720 } }) {
		for (unsigned choice = 0; choice < 3; ++choice) {
			const auto card = SurvivorsUpgradeCardRect(choice, screen);
			const auto banish = SurvivorsUpgradeBanishRect(choice, screen);
			EXPECT_EQ(static_cast<int>(SurvivorsUpgradeActionAt({ card.x + card.width / 2, card.y + 80 }, screen)), static_cast<int>(choice));
			EXPECT_EQ(static_cast<int>(SurvivorsUpgradeActionAt({ banish.x + 2, banish.y + 2 }, screen)), static_cast<int>(5 + choice));
			EXPECT_EQ(SurvivorsUpgradeChoiceAt({ banish.x + 2, banish.y + 2 }, screen), -1);
		}
		const auto reroll = SurvivorsUpgradeRerollRect(screen);
		const auto skip = SurvivorsUpgradeSkipRect(screen);
		EXPECT_EQ(SurvivorsUpgradeActionAt({ reroll.x + 1, reroll.y + 1 }, screen), SurvivorsUpgradeAction::Reroll);
		EXPECT_EQ(SurvivorsUpgradeActionAt({ skip.x + 1, skip.y + 1 }, screen), SurvivorsUpgradeAction::Skip);
		EXPECT_EQ(SurvivorsUpgradeActionAt({ 0, 0 }, screen), SurvivorsUpgradeAction::None);
	}
}

TEST(SurvivorsUpgradeCards, StructuredDetailsExposeEvolutionRanksAndRealCatalysts)
{
	ResetAllSurvivorsArsenals();
	SurvivorsArsenalSnapshot state;
	state.ranks[2] = 5;
	state.passives[0] = 2;
	state.offers = { 2, 10, 16 };
	state.drafted = true;
	ASSERT_TRUE(ApplySurvivorsArsenalReplica(0, state));
	SelectSurvivorsArsenalPlayer(0);
	const auto details = SurvivorsArsenalOfferDetails();
	EXPECT_EQ(details[0].name, "ARMAGEDDON");
	EXPECT_TRUE(details[0].evolution);
	EXPECT_EQ(details[0].currentRank, 5U);
	EXPECT_EQ(details[0].nextRank, 6U);
	EXPECT_NE(details[0].requirements.find("Might"), std::string::npos);
	EXPECT_EQ(details[1].name, "Might");
	EXPECT_NE(details[1].requirements.find("Ember"), std::string::npos);
	EXPECT_NE(details[1].requirements.find("Fireball"), std::string::npos);
	EXPECT_EQ(details[2].name, "Blood Rite");
	EXPECT_TRUE(details[2].enabled);
	EXPECT_FALSE(details[2].description.empty());
	ResetAllSurvivorsArsenals();
	SelectSurvivorsArsenalPlayer(0);
}
} // namespace
} // namespace devilution
