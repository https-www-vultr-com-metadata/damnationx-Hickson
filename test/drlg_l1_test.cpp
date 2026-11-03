#include <gtest/gtest.h>

#include "drlg_test.hpp"
#include "survivors.h"
#include "levels/survivors_layout.hpp"
#include <queue>

using namespace devilution;

namespace {

class SurvivorsArena : public testing::Test {
protected:
	void SetUp() override
	{
		SetSurvivorsMode(true);
		currlevel = 1;
		leveltype = DTYPE_CATHEDRAL;
		pMegaTiles = std::make_unique<MegaTile[]>(206);
		// Synthetic nonzero pieces verify the normal mega-to-world conversion
		// without requiring owned Diablo archives for this geometry test.
		for (unsigned tile = 0; tile < 206; ++tile) {
			const auto piece = static_cast<uint16_t>(tile + 1);
			pMegaTiles[tile] = { piece, piece, piece, piece };
		}
	}

	void TearDown() override
	{
		SetSurvivorsMode(false);
	}
};

TEST_F(SurvivorsArena, EntryHasContinuousFloorAndAnEnclosedExteriorAcrossSeeds)
{
	for (const uint32_t seed : { 2588U, 743271966U, 0U }) {
		CreateDungeon(seed, ENTRY_MAIN);
		EXPECT_EQ(ViewPosition, Point(56, 56));
		const auto room = dTransVal[56][56];
		ASSERT_NE(room, 0);
		// A wide clear area around entry accommodates the entire spawn ring.
		for (int y = 44; y <= 68; ++y) {
			for (int x = 44; x <= 68; ++x) {
				ASSERT_EQ(dTransVal[x][y], room) << seed << " at " << x << "," << y;
				ASSERT_NE(dPiece[x][y], 0) << seed << " at " << x << "," << y;
			}
		}
		for (int edge = 0; edge < MAXDUNX; ++edge) {
			EXPECT_EQ(dTransVal[edge][0], 0);
			EXPECT_EQ(dTransVal[edge][MAXDUNY - 1], 0);
			EXPECT_EQ(dTransVal[0][edge], 0);
			EXPECT_EQ(dTransVal[MAXDUNX - 1][edge], 0);
		}
		EXPECT_EQ(SetPieceRoom.size.width, 0);
		EXPECT_EQ(SetPieceRoom.size.height, 0);
	}
}

TEST_F(SurvivorsArena, AllFiveActsUseClearNativeArenasAcrossSeeds)
{
	for (unsigned act = 0; act < SurvivorsActCount; ++act) {
		currlevel = static_cast<uint8_t>(SurvivorsActLevel(act));
		leveltype = GetLevelType(currlevel);
		for (const uint32_t seed : { 2588U, 743271966U }) {
			CreateDungeon(seed, ENTRY_MAIN);
			ASSERT_EQ(ViewPosition, Point(56, 56)) << act;
			const auto room = dTransVal[56][56];
			ASSERT_NE(room, 0) << act;
			// Each biome must keep a clear spawn ring around the central entry.
			for (int y = 44; y <= 68; ++y) {
				for (int x = 44; x <= 68; ++x) {
					ASSERT_EQ(dTransVal[x][y], room) << act << " at " << x << "," << y;
					ASSERT_NE(dPiece[x][y], 0) << act << " at " << x << "," << y;
				}
			}
			// Each transition threshold must have an outward walking step in
			// the converted native floor region, not just in the input mask.
			for (int direction = 0; direction < 4; ++direction) {
				bool reachable = false;
				for (int lateral = 44; lateral <= 68; ++lateral) {
					const Point a = direction == 0 ? Point { 42, lateral } : direction == 1 ? Point { 70, lateral } : direction == 2 ? Point { lateral, 36 } : Point { lateral, 76 };
					const Displacement step = direction == 0 ? Displacement { -1, 0 } : direction == 1 ? Displacement { 1, 0 } : direction == 2 ? Displacement { 0, -1 } : Displacement { 0, 1 };
					const Point b = a + step;
					if (dTransVal[a.x][a.y] == room && dTransVal[b.x][b.y] == room && dPiece[a.x][a.y] != 0 && dPiece[b.x][b.y] != 0) reachable = true;
				}
				EXPECT_TRUE(reachable) << "act " << act << ", seed " << seed << ", direction " << direction;
			}
			EXPECT_EQ(SetPieceRoom.size.width, 0);
			EXPECT_EQ(SetPieceRoom.size.height, 0);
		}
	}
}

TEST(SurvivorsLayout, SeedsProduceRepeatableConnectedExplorableClearings)
{
	const auto first = GenerateSurvivorsLayout(2588);
	EXPECT_EQ(first, GenerateSurvivorsLayout(2588));
	EXPECT_NE(first, GenerateSurvivorsLayout(743271966));
	for (uint32_t seed = 0; seed < 128; ++seed) {
		const auto layout = GenerateSurvivorsLayout(seed);
		for (const auto direction : { std::array { -1, 0 }, std::array { 1, 0 }, std::array { 0, -1 }, std::array { 0, 1 } }) {
			const auto tile = SurvivorsArrivalMega(layout, direction[0], direction[1]);
			for (int x = tile[0] - 2; x <= tile[0] + 2; ++x)
				for (int y = tile[1] - 2; y <= tile[1] + 2; ++y)
					EXPECT_TRUE(layout[x][y]) << "arrival seed " << seed;
		}
		std::array<std::array<bool, 40>, 40> reached {};
		std::queue<Point> open;
		ASSERT_TRUE(layout[20][20]);
		open.push({ 20, 20 });
		reached[20][20] = true;
		unsigned count = 0;
		while (!open.empty()) {
			const Point p = open.front();
			open.pop();
			++count;
			for (const Displacement d : { Displacement { 1, 0 }, Displacement { -1, 0 }, Displacement { 0, 1 }, Displacement { 0, -1 } }) {
				const Point n = p + d;
				if (n.x < 0 || n.x >= 40 || n.y < 0 || n.y >= 40 || !layout[n.x][n.y] || reached[n.x][n.y]) continue;
				reached[n.x][n.y] = true;
				open.push(n);
			}
		}
		unsigned area = 0;
		for (int x = 0; x < 40; ++x)
			for (int y = 0; y < 40; ++y)
				if (layout[x][y]) { ++area; EXPECT_TRUE(reached[x][y]) << seed; }
		EXPECT_GE(area, 600U) << seed;
	}
}

TEST(Drlg_l1, CreateL5Dungeon_diablo_1_2588)
{
	LoadExpectedLevelData("diablo/1-2588.dun");

	TestInitGame();

	TestCreateDungeon(1, 2588, ENTRY_MAIN);
	EXPECT_EQ(ViewPosition, Point(77, 46));
	TestCreateDungeon(1, 2588, ENTRY_PREV);
	EXPECT_EQ(ViewPosition, Point(49, 49));
}

TEST(Drlg_l1, CreateL5Dungeon_diablo_1_743271966)
{
	LoadExpectedLevelData("diablo/1-743271966.dun");

	TestInitGame();

	TestCreateDungeon(1, 743271966, ENTRY_MAIN);
	EXPECT_EQ(ViewPosition, Point(51, 82));
	TestCreateDungeon(1, 743271966, ENTRY_PREV);
	EXPECT_EQ(ViewPosition, Point(81, 47));
}

TEST(Drlg_l1, CreateL5Dungeon_diablo_2_1383137027)
{
	LoadExpectedLevelData("diablo/2-1383137027.dun");

	TestInitGame();
	Quests[Q_PWATER]._qactive = QUEST_INIT;
	Quests[Q_BUTCHER]._qactive = QUEST_NOTAVAIL;

	TestCreateDungeon(2, 1383137027, ENTRY_MAIN);
	EXPECT_EQ(ViewPosition, Point(57, 74));
	TestCreateDungeon(2, 1383137027, ENTRY_PREV);
	EXPECT_EQ(ViewPosition, Point(57, 79));
	TestCreateDungeon(2, 1383137027, ENTRY_RTNLVL);
	EXPECT_EQ(ViewPosition, Point(49, 88));
}

TEST(Drlg_l1, CreateL5Dungeon_diablo_3_844660068)
{
	LoadExpectedLevelData("diablo/3-844660068.dun");

	TestInitGame();

	TestCreateDungeon(3, 844660068, ENTRY_MAIN);
	EXPECT_EQ(ViewPosition, Point(67, 52));
	TestCreateDungeon(3, 844660068, ENTRY_PREV);
	EXPECT_EQ(ViewPosition, Point(85, 45));
}

TEST(Drlg_l1, CreateL5Dungeon_diablo_4_609325643)
{
	LoadExpectedLevelData("diablo/4-609325643.dun");

	TestInitGame();
	Quests[Q_LTBANNER]._qactive = QUEST_NOTAVAIL;

	TestCreateDungeon(4, 609325643, ENTRY_MAIN);
	EXPECT_EQ(ViewPosition, Point(85, 78));
	TestCreateDungeon(4, 609325643, ENTRY_PREV);
	EXPECT_EQ(ViewPosition, Point(55, 47));
}

TEST(Drlg_l1, CreateL5Dungeon_diablo_4_902156014)
{
	LoadExpectedLevelData("diablo/4-902156014.dun");

	TestInitGame();
	Quests[Q_LTBANNER]._qactive = QUEST_INIT;

	TestCreateDungeon(4, 902156014, ENTRY_MAIN);
	EXPECT_EQ(ViewPosition, Point(75, 56));
	TestCreateDungeon(4, 902156014, ENTRY_PREV);
	EXPECT_EQ(ViewPosition, Point(51, 59));
}

TEST(Drlg_l1, CreateL5Dungeon_hellfire_1_401921334)
{
	LoadExpectedLevelData("hellfire/1-401921334.dun");

	TestInitGame(true, false);

	TestCreateDungeon(1, 401921334, ENTRY_MAIN);
	EXPECT_EQ(ViewPosition, Point(79, 80));
	TestCreateDungeon(1, 401921334, ENTRY_PREV);
	EXPECT_EQ(ViewPosition, Point(49, 63));
}

TEST(Drlg_l1, CreateL5Dungeon_hellfire_1_536340718)
{
	LoadExpectedLevelData("hellfire/1-536340718.dun");

	TestInitGame(true, false);

	TestCreateDungeon(1, 536340718, ENTRY_MAIN);
	EXPECT_EQ(ViewPosition, Point(55, 72));
	TestCreateDungeon(1, 536340718, ENTRY_PREV);
	EXPECT_EQ(ViewPosition, Point(49, 63));
}

TEST(Drlg_l1, CreateL5Dungeon_hellfire_2_128964898)
{
	LoadExpectedLevelData("hellfire/2-128964898.dun");

	TestInitGame(true, false);
	Quests[Q_PWATER]._qactive = QUEST_NOTAVAIL;
	Quests[Q_BUTCHER]._qactive = QUEST_NOTAVAIL;

	TestCreateDungeon(2, 128964898, ENTRY_MAIN);
	EXPECT_EQ(ViewPosition, Point(55, 68));
	TestCreateDungeon(2, 128964898, ENTRY_PREV);
	EXPECT_EQ(ViewPosition, Point(49, 63));
}

TEST(Drlg_l1, CreateL5Dungeon_hellfire_2_1180526547)
{
	LoadExpectedLevelData("hellfire/2-1180526547.dun");

	TestInitGame(true, false);
	Quests[Q_PWATER]._qactive = QUEST_NOTAVAIL;
	Quests[Q_BUTCHER]._qactive = QUEST_INIT;

	TestCreateDungeon(2, 1180526547, ENTRY_MAIN);
	EXPECT_EQ(ViewPosition, Point(83, 68));
	TestCreateDungeon(2, 1180526547, ENTRY_PREV);
	EXPECT_EQ(ViewPosition, Point(81, 45));
}

TEST(Drlg_l1, CreateL5Dungeon_hellfire_3_1369955278)
{
	LoadExpectedLevelData("hellfire/3-1369955278.dun");

	TestInitGame(true, false);
	Quests[Q_SKELKING]._qactive = QUEST_INIT;

	TestCreateDungeon(3, 1369955278, ENTRY_MAIN);
	EXPECT_EQ(ViewPosition, Point(63, 86));
	TestCreateDungeon(3, 1369955278, ENTRY_PREV);
	EXPECT_EQ(ViewPosition, Point(75, 69));
}

TEST(Drlg_l1, CreateL5Dungeon_hellfire_3_1799396623)
{
	LoadExpectedLevelData("hellfire/3-1799396623.dun");

	TestInitGame(true, false);
	Quests[Q_SKELKING]._qactive = QUEST_NOTAVAIL;

	TestCreateDungeon(3, 1799396623, ENTRY_MAIN);
	EXPECT_EQ(ViewPosition, Point(59, 68));
	TestCreateDungeon(3, 1799396623, ENTRY_PREV);
	EXPECT_EQ(ViewPosition, Point(47, 55));
}

TEST(Drlg_l1, CreateL5Dungeon_hellfire_3_1512491184)
{
	LoadExpectedLevelData("hellfire/3-1512491184.dun");

	TestInitGame(true, false);
	Quests[Q_SKELKING]._qactive = QUEST_INIT;

	TestCreateDungeon(3, 1512491184, ENTRY_MAIN);
	EXPECT_EQ(ViewPosition, Point(47, 72));
	TestCreateDungeon(3, 1512491184, ENTRY_PREV);
	EXPECT_EQ(ViewPosition, Point(79, 45));
}

TEST(Drlg_l1, CreateL5Dungeon_hellfire_4_1190318991)
{
	LoadExpectedLevelData("hellfire/4-1190318991.dun");

	TestInitGame(true, false);
	Quests[Q_LTBANNER]._qactive = QUEST_NOTAVAIL;

	TestCreateDungeon(4, 1190318991, ENTRY_MAIN);
	EXPECT_EQ(ViewPosition, Point(67, 80));
	TestCreateDungeon(4, 1190318991, ENTRY_PREV);
	EXPECT_EQ(ViewPosition, Point(77, 45));
}

TEST(Drlg_l1, CreateL5Dungeon_hellfire_4_1924296259)
{
	LoadExpectedLevelData("hellfire/4-1924296259.dun");

	TestInitGame(true, false);
	Quests[Q_LTBANNER]._qactive = QUEST_INIT;

	TestCreateDungeon(4, 1924296259, ENTRY_MAIN);
	EXPECT_EQ(ViewPosition, Point(83, 54));
	TestCreateDungeon(4, 1924296259, ENTRY_PREV);
	EXPECT_EQ(ViewPosition, Point(51, 87));
}

TEST(Drlg_l1, CreateL5Dungeon_crypt_1_2122696790)
{
	LoadExpectedLevelData("hellfire/21-2122696790.dun");

	TestInitGame(true, true, true);

	TestCreateDungeon(21, 2122696790, ENTRY_TWARPDN);
	EXPECT_EQ(ViewPosition, Point(61, 81));
	TestCreateDungeon(21, 2122696790, ENTRY_PREV);
	EXPECT_EQ(ViewPosition, Point(53, 67));
}

TEST(Drlg_l1, CreateL5Dungeon_crypt_2_1191662129)
{
	LoadExpectedLevelData("hellfire/22-1191662129.dun");

	TestInitGame(true, true, true);
	Quests[Q_PWATER]._qactive = QUEST_NOTAVAIL;
	Quests[Q_BUTCHER]._qactive = QUEST_NOTAVAIL;

	TestCreateDungeon(22, 1191662129, ENTRY_MAIN);
	EXPECT_EQ(ViewPosition, Point(71, 47));
	TestCreateDungeon(22, 1191662129, ENTRY_PREV);
	EXPECT_EQ(ViewPosition, Point(85, 71));
}

TEST(Drlg_l1, CreateL5Dungeon_crypt_3_97055268)
{
	LoadExpectedLevelData("hellfire/23-97055268.dun");

	TestInitGame(true, true, true);

	TestCreateDungeon(23, 97055268, ENTRY_MAIN);
	EXPECT_EQ(ViewPosition, Point(71, 57));
	TestCreateDungeon(23, 97055268, ENTRY_PREV);
	EXPECT_EQ(ViewPosition, Point(81, 59));
}

TEST(Drlg_l1, CreateL5Dungeon_crypt_4_1324803725)
{
	LoadExpectedLevelData("hellfire/24-1324803725.dun");

	TestInitGame(true, true, true);

	TestCreateDungeon(24, 1324803725, ENTRY_MAIN);
	EXPECT_EQ(ViewPosition, Point(79, 47));
}

} // namespace
