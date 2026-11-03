#include <gtest/gtest.h>

#include "automap.h"
#include "engine/render/scrollrt.h"
#include "levels/survivors_exploration.hpp"
#include "options.h"

using namespace devilution;

TEST(SurvivorsExploration, KeepsWorldCellsAcrossRebasesAndReset)
{
	SurvivorsExploration explored;
	explored.Mark(0, 0, { 56, 56 }, MAP_EXP_SELF);
	EXPECT_EQ(explored.At(0, 0, { 20, 20 }), MAP_EXP_SELF);
	EXPECT_EQ(explored.At(32, 0, { 4, 20 }), MAP_EXP_SELF);
	EXPECT_EQ(explored.At(-32, 0, { 36, 20 }), MAP_EXP_SELF);
	EXPECT_EQ(explored.At(0, 32, { 20, 4 }), MAP_EXP_SELF);
	explored.Mark(0, 0, { 57, 57 }, MAP_EXP_OTHERS);
	EXPECT_EQ(explored.At(0, 0, { 20, 20 }), MAP_EXP_SELF);
	explored.Mark(-32, -32, { 15, 15 }, MAP_EXP_OTHERS);
	EXPECT_EQ(explored.At(-32, -32, { -1, -1 }), MAP_EXP_OTHERS);
	EXPECT_EQ(explored.At(0, 0, { -17, -17 }), MAP_EXP_OTHERS);
	EXPECT_EQ(explored.At(0, 0, { 0, 0 }), MAP_EXP_NONE);
	explored.Clear();
	EXPECT_EQ(explored.At(0, 0, { 20, 20 }), MAP_EXP_NONE);
	EXPECT_EQ(explored.At(0, 0, { -17, -17 }), MAP_EXP_NONE);
}

TEST(SurvivorsExploration, DiscardsLeastRecentlySeenCellsAtCapacity)
{
	SurvivorsExploration explored;
	for (size_t i = 0; i < SurvivorsExploration::MaxCells; ++i)
		explored.Mark(0, 0, { static_cast<int>(16 + 2 * i), 16 }, MAP_EXP_SELF);
	// Seeing an older cell again protects it from the next eviction.
	explored.Mark(0, 0, { 16, 16 }, MAP_EXP_OTHERS);
	explored.Mark(0, 0, { static_cast<int>(16 + 2 * SurvivorsExploration::MaxCells), 16 }, MAP_EXP_SELF);
	EXPECT_EQ(explored.At(0, 0, { 0, 0 }), MAP_EXP_SELF);
	EXPECT_EQ(explored.At(0, 0, { 1, 0 }), MAP_EXP_NONE);
	EXPECT_EQ(explored.At(0, 0, { static_cast<int>(SurvivorsExploration::MaxCells), 0 }), MAP_EXP_SELF);
}

TEST(Automap, InitAutomap)
{
	InitAutomapOnce();
	EXPECT_EQ(AutomapActive, false);
	EXPECT_EQ(AutoMapScale, 50);
	EXPECT_EQ(AmLine(AmLineLength::DoubleTile), static_cast<int>(AmLineLength::FullTile));
	EXPECT_EQ(AmLine(AmLineLength::FullAndHalfTile), 6);
	EXPECT_EQ(AmLine(AmLineLength::FullTile), static_cast<int>(AmLineLength::HalfTile));
	EXPECT_EQ(AmLine(AmLineLength::HalfTile), static_cast<int>(AmLineLength::QuarterTile));
	EXPECT_EQ(AmLine(AmLineLength::QuarterTile), 1);
}

TEST(Automap, StartAutomap)
{
	StartAutomap();
	EXPECT_EQ(AutomapOffset.deltaX, 0);
	EXPECT_EQ(AutomapOffset.deltaY, 0);
	EXPECT_EQ(AutomapActive, true);
}

TEST(Automap, AutomapUp)
{
	AutomapOffset.deltaX = 1;
	AutomapOffset.deltaY = 1;
	AutomapUp();
	EXPECT_EQ(AutomapOffset.deltaX, 0);
	EXPECT_EQ(AutomapOffset.deltaY, 0);
}

TEST(Automap, AutomapDown)
{
	AutomapOffset.deltaX = 1;
	AutomapOffset.deltaY = 1;
	AutomapDown();
	EXPECT_EQ(AutomapOffset.deltaX, 2);
	EXPECT_EQ(AutomapOffset.deltaY, 2);
}

TEST(Automap, AutomapLeft)
{
	AutomapOffset.deltaX = 1;
	AutomapOffset.deltaY = 1;
	AutomapLeft();
	EXPECT_EQ(AutomapOffset.deltaX, 0);
	EXPECT_EQ(AutomapOffset.deltaY, 2);
}

TEST(Automap, AutomapRight)
{
	AutomapOffset.deltaX = 1;
	AutomapOffset.deltaY = 1;
	AutomapRight();
	EXPECT_EQ(AutomapOffset.deltaX, 2);
	EXPECT_EQ(AutomapOffset.deltaY, 0);
}

TEST(Automap, AutomapZoomIn)
{
	AutoMapScale = 50;
	AutomapZoomIn();
	EXPECT_EQ(AutoMapScale, 75);
	EXPECT_EQ(AmLine(AmLineLength::DoubleTile), static_cast<int>(AmLineLength::FullAndHalfTile));
	EXPECT_EQ(AmLine(AmLineLength::FullTile), 6);
	EXPECT_EQ(AmLine(AmLineLength::HalfTile), 3);
	EXPECT_EQ(AmLine(AmLineLength::QuarterTile), 1);
}

TEST(Automap, AutomapZoomIn_Max)
{
	AutoMapScale = 175;
	AutoMapScale = 175;
	AutomapZoomIn();
	AutomapZoomIn();
	EXPECT_EQ(AutoMapScale, 200);
	EXPECT_EQ(AmLine(AmLineLength::DoubleTile), 32);
	EXPECT_EQ(AmLine(AmLineLength::FullTile), static_cast<int>(AmLineLength::DoubleTile));
	EXPECT_EQ(AmLine(AmLineLength::HalfTile), static_cast<int>(AmLineLength::FullTile));
	EXPECT_EQ(AmLine(AmLineLength::QuarterTile), static_cast<int>(AmLineLength::HalfTile));
}

TEST(Automap, AutomapZoomOut)
{
	AutoMapScale = 200;
	AutomapZoomOut();
	EXPECT_EQ(AutoMapScale, 175);
	EXPECT_EQ(AmLine(AmLineLength::DoubleTile), 28);
	EXPECT_EQ(AmLine(AmLineLength::FullTile), 14);
	EXPECT_EQ(AmLine(AmLineLength::HalfTile), 7);
	EXPECT_EQ(AmLine(AmLineLength::QuarterTile), 3);
}

TEST(Automap, AutomapZoomOut_Min)
{
	AutoMapScale = 50;
	AutomapZoomOut();
	AutomapZoomOut();
	EXPECT_EQ(AutoMapScale, 25);
	EXPECT_EQ(AmLine(AmLineLength::DoubleTile), static_cast<int>(AmLineLength::HalfTile));
	EXPECT_EQ(AmLine(AmLineLength::FullTile), static_cast<int>(AmLineLength::QuarterTile));
	EXPECT_EQ(AmLine(AmLineLength::HalfTile), 1);
	EXPECT_EQ(AmLine(AmLineLength::QuarterTile), 0);
}

TEST(Automap, AutomapZoomReset)
{
	AutoMapScale = 50;
	AutomapOffset.deltaX = 1;
	AutomapOffset.deltaY = 1;
	AutomapZoomReset();
	EXPECT_EQ(AutomapOffset.deltaX, 0);
	EXPECT_EQ(AutomapOffset.deltaY, 0);
	EXPECT_EQ(AutoMapScale, 50);
	EXPECT_EQ(AmLine(AmLineLength::DoubleTile), static_cast<int>(AmLineLength::FullTile));
	EXPECT_EQ(AmLine(AmLineLength::FullTile), static_cast<int>(AmLineLength::HalfTile));
	EXPECT_EQ(AmLine(AmLineLength::HalfTile), static_cast<int>(AmLineLength::QuarterTile));
	EXPECT_EQ(AmLine(AmLineLength::QuarterTile), 1);
}

TEST(Automap, ScaleFollowsViewZoom)
{
	SetFractionalZoomForTesting(true);
	CurrentAutomapType = AutomapType::Opaque;
	AutoMapScale = 50;

	GetOptions().Graphics.zoomLevel.SetValue(4);
	EXPECT_EQ(GetAutomapScale(), 50);
	EXPECT_EQ(AmLine(AmLineLength::DoubleTile), static_cast<int>(AmLineLength::FullTile));

	// Each view zoom step scales the map by the same factor as the world
	StepZoomLevel(1);
	EXPECT_EQ(*GetOptions().Graphics.zoomLevel, 5);
	EXPECT_EQ(GetAutomapScale(), 62);
	StepZoomLevel(3);
	EXPECT_EQ(GetAutomapScale(), 100);
	EXPECT_EQ(AmLine(AmLineLength::DoubleTile), static_cast<int>(AmLineLength::DoubleTile));
	GetOptions().Graphics.zoomLevel.SetValue(16);
	EXPECT_EQ(GetAutomapScale(), 200);

	// The automap's own zoom keeps its 1x scale and stacks with the view zoom
	GetOptions().Graphics.zoomLevel.SetValue(8);
	AutomapZoomIn();
	EXPECT_EQ(AutoMapScale, 75);
	EXPECT_EQ(GetAutomapScale(), 150);

	// Back at 1x the map is where it was: no drift from zooming the view
	StepZoomLevel(-4);
	EXPECT_EQ(*GetOptions().Graphics.zoomLevel, 4);
	EXPECT_EQ(GetAutomapScale(), 75);

	SetFractionalZoomForTesting(std::nullopt);
}

TEST(Automap, MinimapKeepsItsScale)
{
	SetFractionalZoomForTesting(true);
	CurrentAutomapType = AutomapType::Minimap;
	MinimapScale = 50;
	GetOptions().Graphics.zoomLevel.SetValue(12);
	EXPECT_EQ(GetAutomapScale(), 50);
	GetOptions().Graphics.zoomLevel.SetValue(4);
	CurrentAutomapType = AutomapType::Opaque;
	SetFractionalZoomForTesting(std::nullopt);
}
