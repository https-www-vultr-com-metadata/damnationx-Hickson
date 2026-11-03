/**
 * @file loadsave.h
 *
 * Interface of save game functionality.
 */
#pragma once

#include <cstdint>
#include <expected>
#include <span>
#include <vector>

#include "pfile.h"
#include "player.h"
#include "utils/attributes.h"

namespace devilution {

extern DVL_API_FOR_TEST bool gbIsHellfireSaveGame;
extern DVL_API_FOR_TEST uint8_t giNumberOfLevels;

void RemoveInvalidItem(Item &pItem);
_item_indexes RemapItemIdxFromDiablo(_item_indexes i);
_item_indexes RemapItemIdxToDiablo(_item_indexes i);
_item_indexes RemapItemIdxFromSpawn(_item_indexes i);
_item_indexes RemapItemIdxToSpawn(_item_indexes i);
bool IsHeaderValid(uint32_t magicNumber);
void LoadHotkeys();
void LoadHeroItems(Player &player);
/**
 * @brief Remove invalid inventory items from the inventory grid
 * @param player The player to remove invalid items from
 */
void RemoveEmptyInventory(Player &player);

/**
 * @brief Load game state
 * @param firstflag Can be set to false if we are simply reloading the current game
 */
std::expected<void, std::string> LoadGame(bool firstflag);
void SaveHotkeys(SaveWriter &saveWriter, const Player &player);
void SaveHeroItems(SaveWriter &saveWriter, Player &player);
void SaveSurvivorsBuyback(SaveWriter &writer, std::span<const Item> items, std::span<const int> prices);
bool LoadSurvivorsBuyback(std::optional<SaveReader> archive, std::vector<Item> &items, std::vector<int> &prices);
void SaveSurvivorsLedgerHeldItem(SaveWriter &writer, const Item &item);
bool LoadSurvivorsLedgerHeldItem(std::optional<SaveReader> archive, Item &item);
void SaveSurvivorsGroundItems(SaveWriter &writer, std::span<const Item> items);
bool LoadSurvivorsGroundItems(std::optional<SaveReader> archive, std::vector<Item> &items);
void SaveGameData(SaveWriter &saveWriter);
void SaveGame();
void SaveLevel(SaveWriter &saveWriter);
std::expected<void, std::string> LoadLevel();
std::expected<void, std::string> ConvertLevels(SaveWriter &saveWriter);
void LoadStash();
void SaveStash(SaveWriter &stashWriter);

} // namespace devilution
