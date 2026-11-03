#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <limits>
#include <string>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "loadsave.h"
#include "controls/plrctrls.h"
#include "cursor.h"
#include "headless_mode.hpp"
#include "inv.h"
#include "items.h"
#include "levels/dun_tile_data.hpp"
#include "levels/survivors_rolling_runtime.hpp"
#include "levels/survivors_rolling_world.hpp"
#include "levels/tile_properties.hpp"
#include "lighting.h"
#include "missiles.h"
#include "monster.h"
#include "objects.h"
#include "pfile.h"
#include "player.h"
#include "qol/stash.h"
#include "survivors.h"
#include "survivors_arsenal.h"
#include "survivors_progression.h"
#include "tables/playerdat.hpp"
#include "tables/spelldat.h"
#include "utils/paths.h"

namespace devilution {
namespace {

TEST(SurvivorsRollingWorld, OverlapAndRevisitedTerrainRemainIdenticalAcrossSignedOrigins)
{
	const std::array<SurvivorsFloorTile, 3> floors { { { 1, 2, 3, 4 }, { 5, 6, 7, 8 }, { 9, 10, 11, 12 } } };
	SurvivorsRollingTerrain terrain;
	ASSERT_TRUE(terrain.Initialize(12345, floors));
	uint16_t pieces[MAXDUNX][MAXDUNY];
	terrain.Fill(pieces);
	std::array<std::array<uint16_t, MAXDUNY>, MAXDUNX> original {};
	for (int x = 0; x < MAXDUNX; ++x)
		std::copy_n(pieces[x], MAXDUNY, original[x].begin());
	ASSERT_TRUE(terrain.Shift(32, 0, pieces));
	for (int x = 0; x < MAXDUNX - 32; ++x)
		for (int y = 0; y < MAXDUNY; ++y)
			ASSERT_EQ(pieces[x][y], original[x + 32][y]);
	ASSERT_TRUE(terrain.Shift(-64, -32, pieces));
	for (int x = 0; x < MAXDUNX; ++x)
		for (int y = 0; y < MAXDUNY; ++y)
			ASSERT_EQ(pieces[x][y], terrain.PieceAt(x - 32, y - 32));
	ASSERT_TRUE(terrain.Shift(32, 32, pieces));
	for (int x = 0; x < MAXDUNX; ++x)
		for (int y = 0; y < MAXDUNY; ++y)
			ASSERT_EQ(pieces[x][y], original[x][y]);
	EXPECT_FALSE(terrain.Shift(1, 0, pieces));
}

TEST(SurvivorsRollingWorld, SnapshotOriginCanSkipWindowsButRejectsInvalidAndOverflowingCoordinates)
{
	const std::array<SurvivorsFloorTile, 1> floors { { { 1, 2, 3, 4 } } };
	SurvivorsRollingTerrain terrain;
	ASSERT_TRUE(terrain.Initialize(4, floors));
	ASSERT_TRUE(terrain.SetOrigin(-320, 640));
	EXPECT_EQ(terrain.Origin().x, -320);
	EXPECT_EQ(terrain.Origin().y, 640);
	EXPECT_FALSE(terrain.SetOrigin(-319, 640));
	EXPECT_FALSE(terrain.SetOrigin(std::numeric_limits<int64_t>::min(), 0));
	EXPECT_EQ(terrain.Origin().x, -320);
	EXPECT_EQ(terrain.Origin().y, 640);
}

TEST(SurvivorsRollingWorld, ChestLocationsAreSeededAndStableAcrossSignedChunks)
{
	unsigned present = 0;
	unsigned changed = 0;
	for (int x = -160; x <= 160; x += 16) {
		for (int y = -160; y <= 160; y += 16) {
			const auto a = SurvivorsRollingChestPosition(42, x, y);
			const auto b = SurvivorsRollingChestPosition(42, x, y);
			const auto otherSeed = SurvivorsRollingChestPosition(43, x, y);
			ASSERT_EQ(a.has_value(), b.has_value());
			if (a) {
				++present;
				EXPECT_EQ(a->x, b->x); EXPECT_EQ(a->y, b->y);
				EXPECT_GE(a->x, x + 4); EXPECT_LT(a->x, x + 12);
				EXPECT_GE(a->y, y + 4); EXPECT_LT(a->y, y + 12);
			}
			if (a.has_value() != otherSeed.has_value() || (a && otherSeed && (a->x != otherSeed->x || a->y != otherSeed->y))) ++changed;
		}
	}
	EXPECT_GT(present, 10U);
	EXPECT_GT(changed, 10U);
	EXPECT_FALSE(SurvivorsRollingChestPosition(42, 1, 0).has_value());
}

TEST(SurvivorsRollingWorld, ChunkSceneryIsSeededSpacedAndDenserThanTheLegacyChests)
{
	unsigned chunks = 0, props = 0, chests = 0, shrines = 0, lights = 0, legacyChests = 0, emptyChunks = 0, changed = 0;
	for (int64_t x = -480; x < 480; x += 16) {
		for (int64_t y = -480; y < 480; y += 16) {
			const auto a = SurvivorsRollingChunkScenery(42, x, y);
			const auto b = SurvivorsRollingChunkScenery(42, x, y);
			const auto other = SurvivorsRollingChunkScenery(43, x, y);
			ASSERT_EQ(a.count, b.count);
			++chunks;
			props += a.count;
			emptyChunks += a.count == 0 ? 1 : 0;
			legacyChests += SurvivorsRollingChestPosition(42, x, y).has_value() ? 1 : 0;
			if (a.count != other.count || (a.count > 0 && (a.props[0].x != other.props[0].x || a.props[0].y != other.props[0].y))) ++changed;
			for (size_t i = 0; i < a.count; ++i) {
				const auto &prop = a.props[i];
				EXPECT_EQ(prop.x, b.props[i].x);
				EXPECT_EQ(prop.y, b.props[i].y);
				EXPECT_EQ(prop.role, b.props[i].role);
				EXPECT_EQ(prop.variant, b.props[i].variant);
				EXPECT_GE(prop.x, x + 1); EXPECT_LT(prop.x, x + 15);
				EXPECT_GE(prop.y, y + 1); EXPECT_LT(prop.y, y + 15);
				chests += prop.role == SurvivorsPropRole::Chest ? 1 : 0;
				shrines += prop.role == SurvivorsPropRole::Shrine ? 1 : 0;
				lights += SurvivorsPropRoleEmitsLight(prop.role) ? 1 : 0;
				// Every prop keeps a free tile to each other prop in its chunk.
				for (size_t j = i + 1; j < a.count; ++j)
					EXPECT_GE(std::max(std::abs(prop.x - a.props[j].x), std::abs(prop.y - a.props[j].y)), 2) << x << ',' << y;
			}
		}
	}
	RecordProperty("propsPerChunk", std::to_string(static_cast<double>(props) / chunks));
	RecordProperty("chestsPerChunk", std::to_string(static_cast<double>(chests) / chunks));
	RecordProperty("legacyChestsPerChunk", std::to_string(static_cast<double>(legacyChests) / chunks));
	RecordProperty("shrinesPerChunk", std::to_string(static_cast<double>(shrines) / chunks));
	RecordProperty("lightsPerChunk", std::to_string(static_cast<double>(lights) / chunks));
	std::printf("rolling scenery per 16x16 chunk: props %.2f, chests %.2f (legacy %.2f), shrines %.2f, light sources %.2f, empty chunks %.0f%%\n",
	    static_cast<double>(props) / chunks, static_cast<double>(chests) / chunks, static_cast<double>(legacyChests) / chunks,
	    static_cast<double>(shrines) / chunks, static_cast<double>(lights) / chunks, 100.0 * emptyChunks / chunks);
	EXPECT_GT(chests, legacyChests * 2);
	EXPECT_GT(shrines, chunks / 12);
	EXPECT_GT(lights, chunks / 4);
	// Open ground remains: the window's object budget (127) is never planned away.
	EXPECT_LT(props, chunks * 5 / 2);
	EXPECT_GT(emptyChunks, chunks / 20);
	EXPECT_GT(changed, chunks / 2);
	EXPECT_EQ(SurvivorsRollingChunkScenery(42, 1, 0).count, 0);
}

TEST(SurvivorsRollingWorld, FloorVariantsFormPatchesAndKeepTheDominantFloor)
{
	// A source level with one common floor and two rare variants.
	std::vector<SurvivorsFloorTile> floors(40, SurvivorsFloorTile { 1, 2, 3, 4 });
	floors.push_back({ 5, 6, 7, 8 });
	floors.push_back({ 5, 6, 7, 8 });
	floors.push_back({ 9, 10, 11, 12 });
	SurvivorsRollingTerrain terrain;
	ASSERT_TRUE(terrain.Initialize(777, floors));
	unsigned plain = 0, total = 0, sameAsNeighbour = 0;
	for (int64_t x = -200; x < 200; x += 2) {
		for (int64_t y = -200; y < 200; y += 2) {
			const uint16_t piece = terrain.PieceAt(x, y);
			ASSERT_TRUE(piece == 1 || piece == 5 || piece == 9);
			ASSERT_EQ(terrain.PieceAt(x + 1, y + 1), piece + 3);
			plain += piece == 1 ? 1 : 0;
			sameAsNeighbour += terrain.PieceAt(x + 2, y) == piece ? 1 : 0;
			++total;
		}
	}
	EXPECT_GT(plain, total / 2);
	EXPECT_LT(plain, total * 95 / 100);
	// Clustering: neighbours agree more often than independent draws would.
	const double p = static_cast<double>(plain) / total;
	EXPECT_GT(static_cast<double>(sameAsNeighbour) / total, p * p + (1 - p) * (1 - p) * 0.5 + 0.03);
}

TEST(SurvivorsRollingWorld, ReservedDormantGolemHoldingCellsNeverWrapDuringRebase)
{
	Monster monster {};
	// A fresh reserved slot has no MFLAG_GOLEM yet. Its (1, 0) holding cell
	// must stay fixed even when the playable world moves by 32 tiles.
	monster.position.tile = monster.position.future = GolemHoldingCell;
	EXPECT_FALSE(SurvivorsRollingRebaseMonsterPosition(monster, 0, 32, 0));
	EXPECT_EQ(monster.position.tile, GolemHoldingCell);
	EXPECT_EQ(monster.position.future, GolemHoldingCell);

	// An active golem in the same reserved slot must still follow the world.
	monster.position.tile = monster.position.future = { 70, 55 };
	monster.position.old = monster.position.last = { 70, 55 };
	EXPECT_TRUE(SurvivorsRollingRebaseMonsterPosition(monster, 0, 32, 0));
	EXPECT_EQ(monster.position.tile, (Point { 38, 55 }));
	EXPECT_EQ(monster.position.future, (Point { 38, 55 }));
}

TEST(SurvivorsRollingWorld, SuspendedRuneRestoresInAnOverlappingDiagonalWindow)
{
	Missile rune {};
	rune._mitype = MissileID::RuneOfFire;
	rune.position.tile = rune.position.start = rune.position.tileForRendering = { 90, 70 };
	rune.var1 = static_cast<int>(MissileID::BigExplosion);
	// Origin (32, 32) overlaps the original window without being identical.
	ASSERT_TRUE(SurvivorsRollingRebaseSuspendedMissile(rune, 32, 32));
	EXPECT_EQ(rune.position.tile, (Point { 58, 38 }));
	EXPECT_EQ(rune.position.start, (Point { 58, 38 }));
	EXPECT_EQ(rune.var1, static_cast<int>(MissileID::BigExplosion));
	// A window that does not contain the rune must leave the record unchanged.
	EXPECT_FALSE(SurvivorsRollingRebaseSuspendedMissile(rune, 96, 0));
	EXPECT_EQ(rune.position.tile, (Point { 58, 38 }));
}

TEST(SurvivorsRollingWorld, QueuedInteractionsAreCancelledBeforeTargetSlotsAreReused)
{
	Player player {};
	for (const auto action : { ACTION_PICKUPITEM, ACTION_PICKUPAITEM, ACTION_OPERATE, ACTION_DISARM, ACTION_OPERATETK, ACTION_ATTACKMON, ACTION_RATTACKMON, ACTION_SPELLMON }) {
		const auto kind = action == ACTION_PICKUPITEM || action == ACTION_PICKUPAITEM ? SurvivorsRollingTarget::Item
		    : action == ACTION_OPERATE || action == ACTION_DISARM || action == ACTION_OPERATETK ? SurvivorsRollingTarget::Object
		                                                                                       : SurvivorsRollingTarget::Monster;
		player.destAction = action;
		player.destParam1 = 7;
		SurvivorsRollingCancelTarget(player, kind, 8);
		EXPECT_EQ(player.destAction, action);
		SurvivorsRollingCancelTarget(player, kind, 7);
		EXPECT_EQ(player.destAction, ACTION_NONE);
	}
	player.destAction = ACTION_OPERATE;
	player.destParam1 = 7;
	SurvivorsRollingCancelTarget(player, SurvivorsRollingTarget::Item, 7);
	EXPECT_EQ(player.destAction, ACTION_OPERATE);
}

TEST(SurvivorsRollingWorld, VisibleArrowContinuesWhenItsFiringTileLeavesWindow)
{
	Missile arrow {};
	arrow._mitype = MissileID::Arrow;
	arrow.position.start = { 30, 50 };
	arrow.position.tile = arrow.position.tileForRendering = { 40, 50 };
	arrow.position.traveled = Displacement { 10, 0 }.worldToScreen() * -65536;
	arrow.position.velocity = { 65536, 0 };
	const Displacement velocity = arrow.position.velocity;
	ASSERT_TRUE(SurvivorsRollingRebaseSuspendedMissile(arrow, 32, 0));
	EXPECT_EQ(arrow.position.tile, (Point { 8, 50 }));
	EXPECT_EQ(arrow.position.start, (Point { 8, 50 }));
	EXPECT_EQ(arrow.position.traveled, (Displacement { 0, 0 }));
	EXPECT_EQ(arrow.position.velocity, velocity);
	arrow.position.traveled += velocity;
	EXPECT_EQ(arrow.position.start + (arrow.position.traveled >> 16).screenToMissile(), (Point { 8, 50 }));
	Missile edge {};
	edge._mitype = MissileID::Arrow;
	edge.position.start = edge.position.tile = edge.position.tileForRendering = { 0, 0 };
	EXPECT_FALSE(SurvivorsRollingRebaseSuspendedMissile(edge, 32, 0));
}

TEST(SurvivorsRollingWorld, MovingSpellEffectsSurviveWhenTheirFiringTileLeavesWindow)
{
	for (const MissileID type : { MissileID::NovaBall, MissileID::FlameWave,
	         MissileID::LightningControl, MissileID::ThinLightningControl,
	         MissileID::InfernoControl, MissileID::LightningBow, MissileID::FireballBow,
	         MissileID::HolyBoltBow, MissileID::Elemental, MissileID::BoneSpirit }) {
		Missile effect {};
		effect._mitype = type;
		effect.position.start = { 30, 50 };
		effect.position.tile = effect.position.tileForRendering = { 40, 50 };
		const Point movementTile = type == MissileID::FlameWave ? Point { 40, 50 } + Direction::North : Point { 40, 50 };
		effect.position.traveled = (movementTile - Point { 30, 50 }).worldToScreen() * -65536;
		effect.position.velocity = { 65536, 0 };
		effect.var1 = 30;
		effect.var2 = 50;
		effect.var4 = 30; // A homing target may also fall outside the new window.
		effect.var5 = 50;
		effect.duration = 100;
		effect._midam = 42;
		ASSERT_TRUE(SurvivorsRollingRebaseSuspendedMissile(effect, 32, 0)) << static_cast<int>(type);
		EXPECT_EQ(effect.position.tile, (Point { 8, 50 }));
		EXPECT_EQ(effect.position.start, (type == MissileID::FlameWave ? Point { 8, 50 } + Direction::North : Point { 8, 50 }));
		EXPECT_EQ(effect.position.traveled, (Displacement { 0, 0 }));
		EXPECT_EQ(effect.position.velocity, (Displacement { 65536, 0 }));
		EXPECT_EQ(effect.duration, 100);
		EXPECT_EQ(effect._midam, 42);
		if (type == MissileID::Elemental || type == MissileID::BoneSpirit) {
			EXPECT_EQ(effect.var4, -2);
			EXPECT_EQ(effect.var5, 50);
		}
	}
}

TEST(SurvivorsRollingWorld, LightningSegmentKeepsItsSourceExclusionAcrossRebase)
{
	for (const auto type : { MissileID::Lightning, MissileID::ThinLightning }) {
		Missile effect {};
		effect._mitype = type;
		effect.position.start = { 30, 50 };
		effect.position.tile = effect.position.tileForRendering = { 40, 50 };
		effect.duration = 5;
		ASSERT_TRUE(SurvivorsRollingRebaseSuspendedMissile(effect, 32, 0));
		EXPECT_EQ(effect.position.tile, (Point { 8, 50 }));
		EXPECT_NE(effect.position.tile, effect.position.start);
		EXPECT_EQ(effect.duration, 5);
	}
}

TEST(SurvivorsProgression, LevelAwardsStatPointsWithinAttributeAndNetworkLimits)
{
	EXPECT_EQ(SurvivorsStatPointsAfterLevel(0, 100), 5);
	EXPECT_EQ(SurvivorsStatPointsAfterLevel(7, 100), 12);
	EXPECT_EQ(SurvivorsStatPointsAfterLevel(7, 9), 9);
	EXPECT_EQ(SurvivorsStatPointsAfterLevel(254, 500), 255);
	EXPECT_EQ(SurvivorsStatPointsAfterLevel(0, 0), 0);
}

TEST(SurvivorsProgression, WaveTransitionsOccurAtMinuteBoundaries)
{
	EXPECT_EQ(SurvivorsWaveForSeconds(0), 1U);
	EXPECT_EQ(SurvivorsWaveForSeconds(59), 1U);
	EXPECT_EQ(SurvivorsWaveForSeconds(60), 2U);
	EXPECT_EQ(SurvivorsWaveForSeconds(SurvivorsDurationSeconds - 1), 20U);
}

TEST(SurvivorsProgression, CompletedAndOverflowingTimesCannotCreateExtraWaves)
{
	const auto finalWave = SurvivorsWaveForSeconds(SurvivorsDurationSeconds - 1);
	EXPECT_EQ(SurvivorsWaveForSeconds(SurvivorsDurationSeconds), finalWave);
	EXPECT_EQ(SurvivorsWaveForSeconds(std::numeric_limits<unsigned>::max()), finalWave);
}

TEST(SurvivorsProgression, EntireRunIncreasesPressureWithoutZeroTickSpawnLoops)
{
	auto previousWave = SurvivorsWaveForSeconds(0);
	auto previousInterval = SurvivorsSpawnInterval(previousWave);
	for (unsigned second = 1; second <= SurvivorsDurationSeconds; ++second) {
		const auto wave = SurvivorsWaveForSeconds(second);
		const auto interval = SurvivorsSpawnInterval(wave);
		ASSERT_GE(wave, previousWave) << second;
		ASSERT_LE(wave, previousWave + 1) << second;
		ASSERT_GT(interval, 0U) << wave;
		ASSERT_LE(interval, previousInterval) << wave;
		previousWave = wave;
		previousInterval = interval;
	}
	EXPECT_LT(previousInterval, SurvivorsSpawnInterval(1));
	EXPECT_GT(SurvivorsSpawnInterval(std::numeric_limits<unsigned>::max()), 0U);
}

TEST(SurvivorsProgression, LevelRewardsKeepPaceWithIncreasingExperienceCosts)
{
	// Check all Diablo character levels: costs and automatic weapon damage
	// must stay positive and grow as a run advances, without wrapping.
	for (unsigned level = 1; level < 50; ++level) {
		EXPECT_GT(SurvivorsExperienceForLevel(level), 0U);
		EXPECT_GT(SurvivorsExperienceForLevel(level + 1), SurvivorsExperienceForLevel(level));
		EXPECT_GT(SurvivorsDamageForLevel(level + 1), SurvivorsDamageForLevel(level));
	}
}

TEST(SurvivorsProgression, GiftHitTestingMatchesCardsAndButtonsAndRejectsMargins)
{
	for (Size size : { Size { 640, 480 }, Size { 1280, 720 }, Size { 860, 480 } }) {
		const auto panel = SurvivorsUpgradePanelRect(size);
		for (unsigned choice = 0; choice < 3; ++choice) {
			const auto card = SurvivorsUpgradeCardRect(choice, size);
			EXPECT_EQ(SurvivorsUpgradeChoiceAt({ card.x, card.y }, size), choice);
			EXPECT_EQ(SurvivorsUpgradeChoiceAt({ card.x + card.width - 1, card.y + 100 }, size), choice);
			const auto banish = SurvivorsUpgradeBanishRect(choice, size);
			EXPECT_EQ(static_cast<int>(SurvivorsUpgradeActionAt({ banish.x + 2, banish.y + 2 }, size)), 5 + choice);
			EXPECT_EQ(SurvivorsUpgradeChoiceAt({ banish.x + 2, banish.y + 2 }, size), -1);
		}
		const auto reroll = SurvivorsUpgradeRerollRect(size);
		const auto skip = SurvivorsUpgradeSkipRect(size);
		EXPECT_EQ(SurvivorsUpgradeActionAt({ reroll.x + 2, reroll.y + 2 }, size), SurvivorsUpgradeAction::Reroll);
		EXPECT_EQ(SurvivorsUpgradeActionAt({ skip.x + 2, skip.y + 2 }, size), SurvivorsUpgradeAction::Skip);
		EXPECT_EQ(SurvivorsUpgradeActionAt({ panel.x - 1, panel.y + 60 }, size), SurvivorsUpgradeAction::None);
		EXPECT_EQ(SurvivorsUpgradeActionAt({ panel.x + 2, panel.y + 2 }, size), SurvivorsUpgradeAction::None);
	}
}

TEST(SurvivorsCampaign, EveryActRequiresItsOwnTimerAndBossAndStaleDefeatsAreInert)
{
	SurvivorsCampaignDirector director;
	for (unsigned act = 0; act < SurvivorsActCount; ++act) {
		EXPECT_EQ(director.act, act);
		EXPECT_FALSE(director.Update(0, false));
		EXPECT_FALSE(director.Update(SurvivorsActDurationSeconds - 1, true));
		EXPECT_EQ(director.actsCleared, act);
		EXPECT_FALSE(director.Update(SurvivorsActDurationSeconds, false));
		EXPECT_TRUE(director.bossGate);
		EXPECT_TRUE(director.Update(SurvivorsActDurationSeconds + 30, true));
		EXPECT_EQ(director.actsCleared, act + 1);
		EXPECT_FALSE(director.Update(SurvivorsActDurationSeconds + 30, true));
		EXPECT_EQ(director.actsCleared, act + 1);
	}
	EXPECT_TRUE(director.victory);
	EXPECT_FALSE(director.Update(0, true));
	EXPECT_EQ(director.act, 4U);
}

TEST(SurvivorsCampaign, NativeActDestinationsAndBossScalingStayBounded)
{
	const std::array<unsigned, 5> levels { 1, 5, 9, 13, 15 };
	for (unsigned act = 0; act < levels.size(); ++act) {
		EXPECT_EQ(SurvivorsActLevel(act), levels[act]);
		EXPECT_GT(SurvivorsBossHitPoints(act, 1), SurvivorsBossHitPoints(act, 0));
		EXPECT_GT(SurvivorsBossHitPoints(act, 2), SurvivorsBossHitPoints(act, 1));
		if (act > 0)
			EXPECT_GT(SurvivorsBossHitPoints(act, 0), SurvivorsBossHitPoints(act - 1, 0));
	}
	const auto maximum = std::numeric_limits<unsigned>::max();
	EXPECT_EQ(SurvivorsActLevel(maximum), 15U);
	EXPECT_EQ(SurvivorsBossHitPoints(maximum, maximum), SurvivorsBossHitPoints(4, 2));
}

class SurvivorsSession : public testing::Test {
protected:
	void SetUp() override
	{
		previousPrefPath = paths::PrefPath();
		previousPlayer = MyPlayer;
		previousValidSave = gbValidSaveFile;
		previousStashDirty = Stash.dirty;
		saveDirectory = std::filesystem::temp_directory_path()
		    / ("damnationx-survivors-test-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
		std::filesystem::create_directories(saveDirectory);
		paths::SetPrefPath(saveDirectory.string() + "/");
		MyPlayer = nullptr;
		SetSurvivorsMode(true);
	}

	void TearDown() override
	{
		SetSurvivorsMode(false);
		MyPlayer = previousPlayer;
		gbValidSaveFile = previousValidSave;
		Stash.dirty = previousStashDirty;
		paths::SetPrefPath(previousPrefPath);
		std::filesystem::remove_all(saveDirectory);
	}

	std::filesystem::path saveDirectory;
	std::string previousPrefPath;
	Player *previousPlayer;
	bool previousValidSave;
	bool previousStashDirty;
};

TEST_F(SurvivorsSession, CampaignAndStashSaveEntrypointsAreInertWithoutAPlayer)
{
	gbValidSaveFile = false;
	Stash.dirty = true;
	SaveGame();
	pfile_write_hero();
	pfile_write_hero(true);
	pfile_save_level();
	pfile_remove_temp_files();
	sfile_write_stash();
	EXPECT_FALSE(gbValidSaveFile);
	EXPECT_TRUE(Stash.dirty);
	EXPECT_TRUE(std::filesystem::is_empty(saveDirectory));
}

TEST_F(SurvivorsSession, CampShopDoesNotStartOrSettleAHunt)
{
	Player hero { };
	MyPlayer = &hero;
	SetSurvivorsCampShop(true);
	ASSERT_TRUE(IsSurvivorsCampShop());
	const auto before = GetSurvivorsRunStatus();
	for (int i = 0; i < 100; ++i) ProcessSurvivorsMode();
	const auto after = GetSurvivorsRunStatus();
	EXPECT_EQ(after.totalSeconds, before.totalSeconds);
	EXPECT_EQ(after.kills, before.kills);
	FinalizeSurvivorsRun();
	EXPECT_EQ(GetSurvivorsProfile().runs, 0U);
	SetSurvivorsMode(false);
	EXPECT_FALSE(IsSurvivorsCampShop());
	MyPlayer = nullptr;
}

TEST_F(SurvivorsSession, LoadingAnUnsavedHeroLeavesTheInMemoryHeroIntact)
{
	Player hero { };
	hero._pBaseStr = 17;
	hero._pGold = 12345;
	gbValidSaveFile = true;
	pfile_read_player_from_save(0, hero);
	EXPECT_EQ(hero._pBaseStr, 17);
	EXPECT_EQ(hero._pGold, 12345);
	EXPECT_FALSE(gbValidSaveFile);
	EXPECT_TRUE(std::filesystem::is_empty(saveDirectory));
}

TEST_F(SurvivorsSession, PortalsAndTownRespawnCannotLeaveTheArena)
{
	Player hero { };
	hero.setLevel(1);
	hero._pHitPoints = 10 << 6;
	StartWarpLvl(hero, 0);
	RestartTownLvl(hero);
	EXPECT_TRUE(hero.isOnLevel(1));
	EXPECT_EQ(hero._pHitPoints, 10 << 6);
}

TEST_F(SurvivorsSession, RepeatedHitsAndBlocksCannotInterruptWalking)
{
	Player hero { };
	hero._pHitPoints = 10 << 6;
	hero._pmode = PM_WALK_SOUTHWARDS;
	const Point startingTile = hero.position.tile;
	for (int hit = 0; hit < 8; ++hit) {
		StartPlrHit(hero, 10 << 6, true);
		StartPlrBlock(hero, Direction::North);
		EXPECT_EQ(hero._pmode, PM_WALK_SOUTHWARDS);
		EXPECT_EQ(hero.position.tile, startingTile);
	}
}

TEST_F(SurvivorsSession, AutomaticMeleePreservesAnInProgressWalk)
{
	auto previousPlayers = std::move(Players);
	Monster previousMonster = std::move(Monsters[0]);
	const auto previousOccupant = dMonster[51][50];
	Players.emplace_back();
	Monsters[0] = { };
	dMonster[51][50] = 1;
	Player &hero = Players[0];
	hero._pHitPoints = 10 << 6;
	hero._pmode = PM_WALK_SOUTHWARDS;
	hero._pdir = Direction::South;
	hero.position.tile = { 50, 50 };
	hero.position.future = { 50, 51 };
	hero.position.freeform.active = true;
	hero.position.freeform.start = { 12, 23 };
	hero.position.freeform.end = { 34, 45 };
	hero.walkpath[0] = WALK_S;
	// A dead target exercises the full swing dispatch without requiring loaded
	// monster assets or depending on the random hit roll.
	Monsters[0].hitPoints = 0;
	EXPECT_TRUE(StartSurvivorsAutoMelee(hero, { 51, 50 }, 10, false));
	EXPECT_EQ(hero._pmode, PM_WALK_SOUTHWARDS);
	EXPECT_EQ(hero._pdir, Direction::South);
	EXPECT_EQ(hero.position.tile, (Point { 50, 50 }));
	EXPECT_EQ(hero.position.future, (Point { 50, 51 }));
	EXPECT_TRUE(hero.position.freeform.active);
	EXPECT_EQ(hero.position.freeform.start, (SubTileDisplacement { 12, 23 }));
	EXPECT_EQ(hero.position.freeform.end, (SubTileDisplacement { 34, 45 }));
	EXPECT_EQ(hero.walkpath[0], WALK_S);
	Players = std::move(previousPlayers);
	Monsters[0] = std::move(previousMonster);
	dMonster[51][50] = previousOccupant;
}

TEST_F(SurvivorsSession, AutomaticMeleePlaysAttackAnimationOverWalk)
{
	auto previousPlayers = std::move(Players);
	Monster previousMonster = std::move(Monsters[0]);
	const auto previousOccupant = dMonster[51][50];
	Players.emplace_back();
	Monsters[0] = { };
	dMonster[51][50] = 1;
	Player &hero = Players[0];
	hero._pHitPoints = 10 << 6;
	hero._pmode = PM_WALK_SOUTHWARDS;
	hero._pdir = Direction::South;
	hero._pAFrames = 16;
	hero._pAFNum = 9;
	hero.position.tile = { 50, 50 };
	hero.position.future = { 50, 51 };
	Monsters[0].hitPoints = 0;
	EXPECT_TRUE(StartSurvivorsAutoMelee(hero, { 51, 50 }, 10, false));
	// The swing is shown without taking over the walk's mode or timing.
	EXPECT_EQ(hero._pmode, PM_WALK_SOUTHWARDS);
	EXPECT_EQ(hero.getGraphic(), player_graphic::Walk);
	EXPECT_TRUE(hero.survivorsActionActive);
	EXPECT_EQ(hero.getDisplayedGraphic(), player_graphic::Attack);
	EXPECT_EQ(hero.getDisplayedDirection(), GetDirection({ 50, 50 }, { 51, 50 }));
	EXPECT_EQ(hero.getDisplayedAnimation().numberOfFrames, 16);
	// A second weapon firing before the release frame keeps the swing going.
	hero.survivorsActionAnim.currentFrame = 3;
	StartSurvivorsActionAnimation(hero, player_graphic::Fire, { 50, 49 });
	EXPECT_EQ(hero.survivorsActionGraphic, player_graphic::Attack);
	EXPECT_EQ(hero.survivorsActionAnim.currentFrame, 3);
	Players = std::move(previousPlayers);
	Monsters[0] = std::move(previousMonster);
	dMonster[51][50] = previousOccupant;
}

TEST_F(SurvivorsSession, ManualSwingPreservesWalkAndIsRateLimited)
{
	auto previousPlayers = std::move(Players);
	Players.emplace_back();
	Player &hero = Players[0];
	hero._pHitPoints = 10 << 6;
	hero._pmode = PM_WALK_SOUTHWARDS;
	hero._pdir = Direction::South;
	hero.position.tile = { 50, 50 };
	hero.position.future = { 50, 51 };
	hero.walkpath[0] = WALK_S;
	const auto occupant = dMonster[51][50];
	dMonster[51][50] = 0;
	EXPECT_TRUE(SurvivorsManualAction(hero, { 51, 50 }, false, false, Direction::South));
	EXPECT_FALSE(SurvivorsManualAction(hero, { 51, 50 }, false, false, Direction::South));
	EXPECT_EQ(hero._pmode, PM_WALK_SOUTHWARDS);
	EXPECT_EQ(hero._pdir, Direction::South);
	EXPECT_EQ(hero.position.tile, (Point { 50, 50 }));
	EXPECT_EQ(hero.position.future, (Point { 50, 51 }));
	EXPECT_EQ(hero.walkpath[0], WALK_S);
	dMonster[51][50] = occupant;
	Players = std::move(previousPlayers);
}

TEST_F(SurvivorsSession, ManualArrowAndSpellFireWhileWalkingAndShareCooldown)
{
	LoadMissileData();
	LoadSpellData();
	auto previousPlayers = std::move(Players);
	auto previousMissiles = std::move(Missiles);
	const auto previousPlayerId = MyPlayerId;
	Players.emplace_back();
	MyPlayerId = 0;
	MyPlayer = &Players[0];
	Player &hero = Players[0];
	hero._pHitPoints = 10 << 6;
	hero._pmode = PM_WALK_SOUTHWARDS;
	hero._pdir = Direction::South;
	hero.position.tile = { 50, 50 };
	hero.position.future = { 50, 51 };
	hero.position.freeform.active = true;
	hero.position.freeform.start = { 12, 23 };
	hero.position.freeform.end = { 34, 45 };
	hero.walkpath[0] = WALK_S;
	hero.queuedSpell = { SpellID::Firebolt, SpellType::Skill, 0, 1 };
	hero._pSplLvl[static_cast<size_t>(SpellID::Firebolt)] = 1;

	EXPECT_TRUE(SurvivorsManualAction(hero, { 55, 50 }, true, false, Direction::South));
	EXPECT_EQ(Missiles.size(), 1U);
	if (!Missiles.empty()) EXPECT_EQ(Missiles.back()._mitype, MissileID::Arrow);
	// A different action cannot bypass the cooldown by changing input type.
	EXPECT_FALSE(SurvivorsManualAction(hero, { 55, 50 }, false, true, Direction::South));
	EXPECT_EQ(Missiles.size(), 1U);
	ResetSurvivorsManualActions();
	EXPECT_TRUE(SurvivorsManualAction(hero, { 55, 50 }, false, true, Direction::South));
	EXPECT_EQ(Missiles.size(), 2U);
	if (!Missiles.empty()) EXPECT_EQ(Missiles.back()._mitype, MissileID::Firebolt);
	EXPECT_FALSE(SurvivorsManualAction(hero, { 55, 50 }, true, false, Direction::South));
	EXPECT_EQ(hero._pmode, PM_WALK_SOUTHWARDS);
	EXPECT_EQ(hero._pdir, Direction::South);
	EXPECT_EQ(hero.position.tile, (Point { 50, 50 }));
	EXPECT_EQ(hero.position.future, (Point { 50, 51 }));
	EXPECT_TRUE(hero.position.freeform.active);
	EXPECT_EQ(hero.position.freeform.start, (SubTileDisplacement { 12, 23 }));
	EXPECT_EQ(hero.position.freeform.end, (SubTileDisplacement { 34, 45 }));
	EXPECT_EQ(hero.walkpath[0], WALK_S);

	Missiles = std::move(previousMissiles);
	Players = std::move(previousPlayers);
	MyPlayerId = previousPlayerId;
	MyPlayer = nullptr;
}

TEST_F(SurvivorsSession, AutomaticSpellProjectilesUseCurrentEffectiveMagic)
{
	LoadPlayerDataFiles();
	LoadMissileData();
	auto previousPlayers = std::move(Players);
	auto previousMissiles = std::move(Missiles);
	auto previousMonster = std::move(Monsters[0]);
	const auto previousPlayerId = MyPlayerId;
	const auto previousActiveCount = ActiveMonsterCount;
	const auto previousActive = ActiveMonsters[0];
	const auto previousFloor = SOLData[1];
	std::array<uint16_t, 6> previousPieces {};
	for (int x = 50; x <= 55; ++x) {
		previousPieces[x - 50] = dPiece[x][50];
		dPiece[x][50] = 1;
	}
	SOLData[1] = TileProperties {};
	Players.emplace_back();
	MyPlayerId = 0;
	MyPlayer = &Players[0];
	Player &hero = Players[0];
	hero._pClass = HeroClass::Sorcerer;
	hero._pHitPoints = hero._pHPBase = hero._pMaxHP = hero._pMaxHPBase = 100 << 6;
	hero.position.tile = hero.position.future = { 50, 50 };
	Monsters[0] = {};
	Monsters[0].position.tile = { 55, 50 };
	Monsters[0].hitPoints = 100 << 6;
	Monsters[0].isInvalid = false;
	ActiveMonsters[0] = 0;
	ActiveMonsterCount = 1;
	const int startingMagic = GetClassAttributes(hero._pClass).baseMag;
	auto castEmber = [&](int magic) {
		Missiles.clear();
		hero._pMagic = magic;
		StartSurvivorsArsenal(hero, {}, 7);
		ProcessSurvivorsArsenal(0, 20, 10);
		const auto projectile = std::find_if(Missiles.begin(), Missiles.end(), [](const Missile &missile) {
			return missile._mitype == MissileID::Firebolt;
		});
		return projectile == Missiles.end() ? -1 : projectile->_midam;
	};
	const int baseline = castEmber(startingMagic);
	const int boosted = castEmber(startingMagic + 100);
	EXPECT_GT(baseline, 0);
	EXPECT_EQ(boosted, baseline * 2);
	ResetSurvivorsArsenal();
	Missiles = std::move(previousMissiles);
	Monsters[0] = std::move(previousMonster);
	ActiveMonsterCount = previousActiveCount;
	ActiveMonsters[0] = previousActive;
	for (int x = 50; x <= 55; ++x) dPiece[x][50] = previousPieces[x - 50];
	SOLData[1] = previousFloor;
	Players = std::move(previousPlayers);
	MyPlayerId = previousPlayerId;
	MyPlayer = nullptr;
}

TEST_F(SurvivorsSession, NativeSpellProjectilesKeepMovingThroughARebase)
{
	LoadPlayerDataFiles();
	LoadMissileData();
	auto previousPlayers = std::move(Players);
	auto previousMissiles = std::move(Missiles);
	const auto previousPlayerId = MyPlayerId;
	const bool previousHeadless = HeadlessMode;
	const auto previousFloor = SOLData[1];
	std::vector<std::pair<void *, std::vector<unsigned char>>> saved;
	auto preserve = [&](auto &value) {
		auto *bytes = reinterpret_cast<unsigned char *>(&value);
		saved.emplace_back(bytes, std::vector<unsigned char>(bytes, bytes + sizeof(value)));
	};
	preserve(dPiece); preserve(dFlags); preserve(dObject); preserve(dMonster); preserve(dPlayer);
	for (int x = 0; x < MAXDUNX; ++x) {
		std::fill_n(dPiece[x], MAXDUNY, 1);
		std::fill_n(dFlags[x], MAXDUNY, DungeonFlag {});
		std::fill_n(dObject[x], MAXDUNY, 0);
		std::fill_n(dMonster[x], MAXDUNY, 0);
		std::fill_n(dPlayer[x], MAXDUNY, 0);
	}
	SOLData[1] = TileProperties {};
	HeadlessMode = true;
	Players.emplace_back();
	MyPlayerId = 0;
	MyPlayer = &Players[0];
	Player &hero = Players[0];
	hero._pClass = HeroClass::Sorcerer;
	hero._pMagic = GetClassAttributes(hero._pClass).baseMag;
	hero._pHitPoints = hero._pHPBase = hero._pMaxHP = hero._pMaxHPBase = 100 << 6;
	hero.position.tile = hero.position.future = { 70, 50 };
	for (const auto type : { MissileID::NovaBall, MissileID::LightningControl, MissileID::FlameWave }) {
		Missiles.clear();
		InitLighting();
		Missile *effect = AddMissile({ 30, 50 }, { 70, 50 }, Direction::South, type, TARGET_MONSTERS, hero, 12, 1);
		EXPECT_NE(effect, nullptr) << static_cast<int>(type);
		if (effect == nullptr) continue;
		for (unsigned tick = 0; tick < 40 && effect->position.tile.x < 35; ++tick) {
			ProcessMissiles();
			if (std::none_of(Missiles.begin(), Missiles.end(), [effect](const Missile &missile) { return &missile == effect; })) {
				effect = nullptr;
				break;
			}
		}
		EXPECT_NE(effect, nullptr) << static_cast<int>(type);
		if (effect == nullptr) continue;
		const Point before = effect->position.tile;
		EXPECT_GE(before.x, 32) << static_cast<int>(type);
		if (before.x < 32) continue;
		const Missile beforeRebase = *effect;
		ProcessMissiles();
		if (std::none_of(Missiles.begin(), Missiles.end(), [effect](const Missile &missile) { return &missile == effect; })) {
			ADD_FAILURE() << "projectile vanished during reference tick: " << static_cast<int>(type);
			continue;
		}
		const Point expected = effect->position.tile;
		*effect = beforeRebase;
		const bool rebased = SurvivorsRollingRebaseSuspendedMissile(*effect, 32, 0);
		EXPECT_TRUE(rebased) << static_cast<int>(type);
		if (!rebased) continue;
		EXPECT_EQ(effect->position.tile, (before - Displacement { 32, 0 }));
		ProcessMissiles();
		if (std::none_of(Missiles.begin(), Missiles.end(), [effect](const Missile &missile) { return &missile == effect; })) {
			ADD_FAILURE() << "projectile vanished after rebase: " << static_cast<int>(type);
			continue;
		}
		const Point after = Point(effect->position.tile) + Displacement { 32, 0 };
		EXPECT_EQ(after, expected) << static_cast<int>(type);
	}
	Missiles = std::move(previousMissiles);
	Players = std::move(previousPlayers);
	MyPlayerId = previousPlayerId;
	MyPlayer = nullptr;
	HeadlessMode = previousHeadless;
	SOLData[1] = previousFloor;
	for (auto &[address, bytes] : saved) std::memcpy(address, bytes.data(), bytes.size());
}

TEST_F(SurvivorsSession, ResettingModeClearsSessionAndRejectsSpuriousUpgrades)
{
	EXPECT_TRUE(IsSurvivorsMode());
	EXPECT_FALSE(IsSurvivorsUpgradePending());
	EXPECT_FALSE(IsSurvivorsRunFinished());
	EXPECT_FALSE(ChooseSurvivorsUpgrade(0));
	EXPECT_FALSE(ChooseSurvivorsUpgrade(3));
	SetSurvivorsMode(false);
	EXPECT_FALSE(IsSurvivorsMode());
	EXPECT_FALSE(ChooseSurvivorsUpgrade(0));
}

class SurvivorsLifecycle : public SurvivorsSession {
protected:
	void SetUp() override
	{
		SurvivorsSession::SetUp();
		previousTickRate = sgGameInitInfo.nTickRate;
		previousMultiplayer = gbIsMultiplayer;
		previousLevelType = leveltype;
		previousMonsterCount = ActiveMonsterCount;
		previousMonsterTypeCount = LevelMonsterTypeCount;
		sgGameInitInfo.nTickRate = 1;
		gbIsMultiplayer = false;
		leveltype = DTYPE_CATHEDRAL;
		ActiveMonsterCount = 0;
		LevelMonsterTypeCount = 0;
		hero._pMaxHP = hero._pMaxHPBase = 10 << 6;
		hero._pHitPoints = hero._pHPBase = 1 << 6;
		MyPlayer = &hero;
	}

	void TearDown() override
	{
		sgGameInitInfo.nTickRate = previousTickRate;
		gbIsMultiplayer = previousMultiplayer;
		leveltype = previousLevelType;
		ActiveMonsterCount = previousMonsterCount;
		LevelMonsterTypeCount = previousMonsterTypeCount;
		SurvivorsSession::TearDown();
	}

	Player hero { };
	uint8_t previousTickRate;
	bool previousMultiplayer;
	dungeon_type previousLevelType;
	size_t previousMonsterCount;
	size_t previousMonsterTypeCount;
};

TEST_F(SurvivorsLifecycle, RunCannotWinByWaitingInsteadOfDefeatingActBosses)
{
	// Without loaded monster types the guardian cannot spawn. Time alone must
	// never bypass the boss gate or count an act as cleared.
	for (unsigned tick = 0; tick < SurvivorsDurationSeconds; ++tick)
		ProcessSurvivorsMode();
	EXPECT_FALSE(IsSurvivorsRunFinished());
	const auto status = GetSurvivorsRunStatus();
	EXPECT_EQ(status.totalSeconds, SurvivorsDurationSeconds);
	EXPECT_EQ(status.act, 1U);
	EXPECT_EQ(status.actsCleared, 0U);
	EXPECT_TRUE(status.bossPending);
	EXPECT_FALSE(status.victory);
	EXPECT_TRUE(std::filesystem::is_empty(saveDirectory));
}

TEST_F(SurvivorsLifecycle, DeathFinishesTheRunAndLaterCallsCannotReviveIt)
{
	ProcessSurvivorsMode();
	ASSERT_FALSE(IsSurvivorsRunFinished());
	hero._pHitPoints = 0;
	ProcessSurvivorsMode();
	EXPECT_TRUE(IsSurvivorsRunFinished());
	ProcessSurvivorsMode();
	EXPECT_EQ(hero._pHitPoints, 0);
	EXPECT_FALSE(ChooseSurvivorsUpgrade(0));
	SurvivorsProfile settled;
	const auto profilePath = (saveDirectory / "survivors-profile.dat").string();
	ASSERT_TRUE(LoadSurvivorsProfileFromPath(profilePath, settled));
	EXPECT_EQ(settled.runs, 1U);
	SetSurvivorsMode(false);
	SurvivorsProfile afterExit;
	ASSERT_TRUE(LoadSurvivorsProfileFromPath(profilePath, afterExit));
	EXPECT_EQ(afterExit, settled);
}

TEST_F(SurvivorsLifecycle, KillAtEvictedEdgeIsCreditedBeforeRebase)
{
	// Start the ordinary run before installing a synthetic edge encounter.
	ProcessSurvivorsMode();
	ASSERT_FALSE(IsSurvivorsRunFinished());
	std::vector<std::pair<void *, std::vector<unsigned char>>> saved;
	auto preserve = [&](auto &value) {
		auto *bytes = reinterpret_cast<unsigned char *>(&value);
		saved.emplace_back(bytes, std::vector<unsigned char>(bytes, bytes + sizeof(value)));
	};
	preserve(dPiece); preserve(dObject); preserve(dFlags); preserve(dLight); preserve(dPreLight);
	preserve(dPlayer); preserve(dMonster); preserve(dItem); preserve(dCorpse); preserve(dSpecial); preserve(dTransVal);
	preserve(ActiveMonsters); preserve(ActiveMonsterCount); preserve(ActiveObjectCount);
	preserve(ViewPosition); preserve(cursPosition); preserve(SOLData[1]); preserve(DungeonSeeds[currlevel]);
	const char *oldSetting = std::getenv("DAMNATIONX_LEGACY_SURVIVORS_SECTORS");
	const std::optional<std::string> oldValue = oldSetting == nullptr ? std::nullopt : std::optional<std::string>(oldSetting);
#ifdef _WIN32
	_putenv_s("DAMNATIONX_LEGACY_SURVIVORS_SECTORS", "0");
#else
	setenv("DAMNATIONX_LEGACY_SURVIVORS_SECTORS", "0", 1);
#endif
	for (int x = 0; x < MAXDUNX; ++x) {
		std::fill_n(dPiece[x], MAXDUNY, 1);
		std::fill_n(dObject[x], MAXDUNY, 1); // Keep this isolated fixture free of generated props.
	}
	SOLData[1] = TileProperties {};
	ActiveObjectCount = 0;
	// Avoid generating props in the newly exposed strip: this unit fixture
	// deliberately does not load native object animation assets.
	for (uint32_t seed = 0;; ++seed) {
		bool exposedChest = false;
		for (int x = -16; x < MAXDUNX && !exposedChest; x += 16) {
			for (int y = -16; y < MAXDUNY; y += 16) {
				const auto chest = SurvivorsRollingChestPosition(seed, 32 + x, y);
				if (chest && chest->x - 32 >= 80 && chest->x - 32 < 108 && chest->y >= 4 && chest->y < 108) {
					exposedChest = true;
					break;
				}
			}
		}
		if (!exposedChest) { DungeonSeeds[currlevel] = seed; break; }
	}
	hero.position.tile = hero.position.future = hero.position.old = hero.position.last = { 91, 55 };
	InitSurvivorsRollingWorld();
	const bool rolling = SurvivorsRollingActive();
	constexpr unsigned MonsterId = 10;
	auto previousMonster = std::move(Monsters[MonsterId]);
	Monsters[MonsterId] = Monster {};
	Monster &monster = Monsters[MonsterId];
	monster.position.tile = monster.position.future = monster.position.old = monster.position.last = { 20, 50 };
	monster.hitPoints = 0;
	monster.isInvalid = false;
	monster.lightId = NO_LIGHT;
	ActiveMonsterCount = 1;
	ActiveMonsters[0] = MonsterId;
	dMonster[20][50] = MonsterId + 1;
	const unsigned before = GetSurvivorsRunStatus().kills;
	ProcessSurvivorsMode();
	const auto after = GetSurvivorsRunStatus();
	const auto epoch = SurvivorsRollingEpoch();
	const auto parkedSouls = SurvivorsRollingTakeParkedSouls();
	ResetSurvivorsRollingWorld();
	Monsters[MonsterId] = std::move(previousMonster);
	for (auto &[address, bytes] : saved) std::memcpy(address, bytes.data(), bytes.size());
#ifdef _WIN32
	_putenv_s("DAMNATIONX_LEGACY_SURVIVORS_SECTORS", oldValue ? oldValue->c_str() : "");
#else
	if (oldValue) setenv("DAMNATIONX_LEGACY_SURVIVORS_SECTORS", oldValue->c_str(), 1);
	else unsetenv("DAMNATIONX_LEGACY_SURVIVORS_SECTORS");
#endif
	EXPECT_TRUE(rolling);
	EXPECT_EQ(epoch, 1U);
	EXPECT_EQ(after.kills, before + 1);
	EXPECT_EQ(parkedSouls, 2U);
}

TEST_F(SurvivorsLifecycle, DamagingHitCanOpenAnAdjacentMonsterLane)
{
	ProcessSurvivorsMode();
	ASSERT_FALSE(IsSurvivorsRunFinished());
	const bool oldHeadless = HeadlessMode;
	const auto oldMonsterCount = ActiveMonsterCount;
	const auto oldMonsterTypeCount = LevelMonsterTypeCount;
	Monster oldMonster = std::move(Monsters[0]);
	CMonster oldType = std::move(LevelMonsterTypes[0]);
	const auto oldActive = ActiveMonsters[0];
	const auto oldMonsterTile = dMonster[50][50];
	const auto oldExitTile = dMonster[51][50];
	const auto oldExitPlayer = dPlayer[51][50];
	const auto oldExitPiece = dPiece[51][50];
	const auto oldCornerPiece = dPiece[51][51];
	const auto oldFloorProperties = SOLData[1];
	HeadlessMode = true;
	SOLData[1] = TileProperties::None;
	dPiece[51][50] = dPiece[51][51] = 1;
	dMonster[50][50] = 1;
	dMonster[51][50] = dPlayer[51][50] = 0;
	ActiveMonsters[0] = 0;
	ActiveMonsterCount = 1;
	LevelMonsterTypeCount = 1;
	LevelMonsterTypes[0] = { };
	LevelMonsterTypes[0].type = MT_HORNED;
	auto &hit = LevelMonsterTypes[0].anims[static_cast<unsigned>(MonsterGraphic::GotHit)];
	hit.frames = 4;
	hit.rate = 2;
	hit.width = 96;
	Monsters[0] = { };
	Monsters[0].levelType = 0;
	Monsters[0].hitPoints = 10 << 6;
	Monsters[0].lightId = NO_LIGHT;
	Monsters[0].position.tile = Monsters[0].position.old = Monsters[0].position.future = { 50, 50 };
	hero.position.tile = { 49, 50 };
	const int hitPointsBefore = hero._pHitPoints;
	ApplyPlrDamage(DamageType::Physical, hero, 1);
	EXPECT_EQ(hero._pHitPoints, hitPointsBefore - 64);
	EXPECT_EQ(Monsters[0].position.tile, (Point { 51, 50 }));
	EXPECT_EQ(dMonster[50][50], 0);
	EXPECT_EQ(dMonster[51][50], 1);
	// The escape shove must not force a monster into another occupied tile.
	Monsters[0].position.tile = Monsters[0].position.old = Monsters[0].position.future = { 50, 50 };
	dMonster[50][50] = 1;
	dMonster[51][50] = 2;
	ApplyPlrDamage(DamageType::Physical, hero, 1); // Rejected by contact grace.
	SurvivorsOpenEscapeLane(hero); // A forced shove must also respect occupancy.
	EXPECT_EQ(hero._pHitPoints, hitPointsBefore - 64);
	EXPECT_EQ(Monsters[0].position.tile, (Point { 50, 50 }));
	EXPECT_EQ(dMonster[51][50], 2);
	Monsters[0] = std::move(oldMonster);
	LevelMonsterTypes[0] = std::move(oldType);
	ActiveMonsterCount = oldMonsterCount;
	LevelMonsterTypeCount = oldMonsterTypeCount;
	ActiveMonsters[0] = oldActive;
	dMonster[50][50] = oldMonsterTile;
	dMonster[51][50] = oldExitTile;
	dPlayer[51][50] = oldExitPlayer;
	dPiece[51][50] = oldExitPiece;
	dPiece[51][51] = oldCornerPiece;
	SOLData[1] = oldFloorProperties;
	HeadlessMode = oldHeadless;
}

TEST_F(SurvivorsLifecycle, TownMultiplayerAndMissingPlayerDoNotAdvanceTheTimer)
{
	leveltype = DTYPE_TOWN;
	for (unsigned tick = 0; tick < SurvivorsDurationSeconds; ++tick)
		ProcessSurvivorsMode();
	EXPECT_FALSE(IsSurvivorsRunFinished());
	EXPECT_EQ(hero._pHitPoints, 1 << 6);
	leveltype = DTYPE_CATHEDRAL;
	gbIsMultiplayer = true;
	for (unsigned tick = 0; tick < SurvivorsDurationSeconds; ++tick)
		ProcessSurvivorsMode();
	EXPECT_FALSE(IsSurvivorsRunFinished());
	gbIsMultiplayer = false;
	MyPlayer = nullptr;
	for (unsigned tick = 0; tick < SurvivorsDurationSeconds; ++tick)
		ProcessSurvivorsMode();
	EXPECT_FALSE(IsSurvivorsRunFinished());
	MyPlayer = &hero;
	for (unsigned tick = 0; tick < SurvivorsDurationSeconds - 1; ++tick)
		ProcessSurvivorsMode();
	EXPECT_FALSE(IsSurvivorsRunFinished());
	ProcessSurvivorsMode();
	EXPECT_FALSE(IsSurvivorsRunFinished());
	EXPECT_EQ(GetSurvivorsRunStatus().totalSeconds, SurvivorsDurationSeconds);
	EXPECT_TRUE(GetSurvivorsRunStatus().bossPending);
}

TEST_F(SurvivorsLifecycle, PendingTransitionKeepsDroppedAndGroundItemsWhereTheyAre)
{
	// Once travel starts the old level is no longer checkpointed (co-op ground
	// file) and is discarded on load, so a drop or pickup in that window would
	// strand the item. It must stay in hand until the next level is ready.
	const auto previousItemCount = ActiveItemCount;
	for (unsigned tick = 0; tick < 4; ++tick)
		ProcessSurvivorsMode();
	ASSERT_FALSE(IsSurvivorsTransitionPending());
	EXPECT_FALSE(SurvivorsGroundTransferBlocked());
	EXPECT_TRUE(SurvivorsCanTransferCoopGround(MyPlayerId));
	hero.position.tile = hero.position.future = { 40, 56 }; // West edge.
	hero._pdir = Direction::South;
	ASSERT_TRUE(FindAdjacentPositionForItem(hero.position.future, hero._pdir)) << "fixture must allow a normal drop";
	ASSERT_TRUE(RequestSurvivorsSectorTravel());
	ProcessSurvivorsMode();
	ASSERT_TRUE(IsSurvivorsTransitionPending());
	EXPECT_TRUE(SurvivorsGroundTransferBlocked());
	EXPECT_FALSE(SurvivorsCanTransferCoopGround(MyPlayerId));

	Item held {};
	held._itype = ItemType::Misc;
	held._iSeed = 0x5eed;
	hero.HoldItem = held;
	EXPECT_FALSE(TryDropItem());
	EXPECT_FALSE(hero.HoldItem.isEmpty());
	EXPECT_EQ(hero.HoldItem._iSeed, 0x5eed);
	EXPECT_EQ(ActiveItemCount, previousItemCount);
	hero.HoldItem.clear();
	dPlayer[40][56] = 0;
}

} // namespace
} // namespace devilution
