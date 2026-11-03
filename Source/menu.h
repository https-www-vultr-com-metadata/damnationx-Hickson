/**
 * @file menu.h
 *
 * Interface of functions for interacting with the main menu.
 */
#pragma once

#include <cstdint>

#include "multi.h"

namespace devilution {

struct Player;
extern uint32_t gSaveNumber;
void mainmenu_initialize_survivors_hero(Player &player);

bool mainmenu_select_hero_dialog(GameData *gameData);
void mainmenu_loop();

} // namespace devilution
