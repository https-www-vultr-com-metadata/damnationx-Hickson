/**
 * @file inventory_ui_test.cpp
 *
 * Tests for inventory operations that are not covered by inv_test.cpp.
 *
 * Covers: AutoEquip, AutoPlaceItemInInventory, CanFitItemInInventory,
 * belt placement, RemoveEquipment, ReorganizeInventory, and
 * TransferItemToStash.
 */

#include <gtest/gtest.h>
#include <cstring>
#include <cstdlib>
#include <filesystem>
#include <fstream>

#include "ui_test.hpp"

#include "inv.h"
#include "game_mode.hpp"
#include "headless_mode.hpp"
#include "items.h"
#include "player.h"
#include "pfile.h"
#include "qol/stash.h"
#include "survivors.h"
#include "utils/paths.h"

namespace devilution {
bool AsyncNetInventoryReconciliationForTesting(Player &player);
namespace {

// ---------------------------------------------------------------------------
// Test fixture
// ---------------------------------------------------------------------------

class InventoryUITest : public UITest {
protected:
	void SetUp() override
	{
		UITest::SetUp();

		// Each test starts with a completely stripped player and clean stash.
		StripPlayer();
		Stash = {};
		Stash.gold = 0;
		Stash.dirty = false;
	}

	// --- helpers ---

	/** @brief Create a sword (2×1 one-hand weapon, requires 15 Str). */
	static Item MakeSword()
	{
		Item item {};
		InitializeItem(item, IDI_BARDSWORD);
		item._iIdentified = true;
		return item;
	}

	/** @brief Create a healing potion (1×1, beltable). */
	static Item MakePotion()
	{
		Item item {};
		InitializeItem(item, IDI_HEAL);
		return item;
	}

	/** @brief Create a short staff (1×3 two-hand weapon, requires 0 Str). */
	static Item MakeStaff()
	{
		Item item {};
		InitializeItem(item, IDI_SHORTSTAFF);
		item._iIdentified = true;
		return item;
	}

	/**
	 * @brief Fill the entire inventory grid with 1×1 healing potions.
	 *
	 * After this call every one of the 40 inventory cells is occupied.
	 */
	void FillInventory()
	{
		for (int i = 0; i < InventoryGridCells; i++) {
			Item potion = MakePotion();
			ASSERT_TRUE(AutoPlaceItemInInventory(*MyPlayer, potion))
			    << "Failed to place potion at cell " << i;
		}
	}

	/** @brief Fill all 8 belt slots with healing potions. */
	void FillBelt()
	{
		for (int i = 0; i < MaxBeltItems; i++) {
			Item potion = MakePotion();
			ASSERT_TRUE(AutoPlaceItemInBelt(*MyPlayer, potion, /*persistItem=*/true))
			    << "Failed to place potion in belt slot " << i;
		}
	}
};

#ifndef UNPACKED_SAVES
#ifdef PACKET_ENCRYPTION
TEST_F(InventoryUITest, PendingInventoryWaitsForAckAndRejectedEditRollsBack)
{
	const bool oldHeadless = HeadlessMode;
	struct RestoreHeadless { bool value; ~RestoreHeadless() { HeadlessMode = value; } } restore { oldHeadless };
	HeadlessMode = true;
	MyPlayer->HoldItem.clear();
	Item sword {};
	InitializeItem(sword, IDI_WARRIOR);
	sword._iSeed = 1432657;
	ASSERT_TRUE(AutoPlaceItemInInventory(*MyPlayer, sword));
	EXPECT_TRUE(AsyncNetInventoryReconciliationForTesting(*MyPlayer));
}

TEST_F(InventoryUITest, CooperativeLedgerPersistsHeldGearGoldBuybackAndSequence)
{
	const std::string oldPref = paths::PrefPath();
	const bool oldMultiplayer = gbIsMultiplayer;
	const auto folder = std::filesystem::temp_directory_path() / "damnationx-coop-ledger-test";
	std::filesystem::remove_all(folder);
	std::filesystem::create_directories(folder);
	struct Cleanup {
		std::string oldPref; bool oldMultiplayer; std::filesystem::path folder;
		~Cleanup() { SetSurvivorsMode(false); gbIsMultiplayer = oldMultiplayer; paths::SetPrefPath(oldPref); ResetSurvivorsBuyback(); std::filesystem::remove_all(folder); }
	} cleanup { oldPref, oldMultiplayer, folder };
	paths::SetPrefPath(folder.string() + "/");
	SetSurvivorsMode(true);
	gbIsMultiplayer = true;
	ResetSurvivorsBuyback();
	std::strcpy(MyPlayer->_pName, "LedgerHero");
	const auto identity = pfile_survivors_identity(*MyPlayer);
	ASSERT_TRUE(identity);
	EXPECT_EQ(identity, pfile_survivors_identity(*MyPlayer));
	uint64_t committed = 99;
	EXPECT_EQ(pfile_load_survivors_ledger(*MyPlayer, *identity, committed), SurvivorsLedgerLoad::Missing);
	ASSERT_EQ(AddGoldToInventory(*MyPlayer, 10000), 0);
	MyPlayer->_pGold = CalculateGold(*MyPlayer);
	Item sword {};
	InitializeItem(sword, IDI_WARRIOR);
	sword._iSeed = 214365;
	sword._iDurability = std::max(1, sword._iMaxDur - 1);
	ASSERT_TRUE(AutoPlaceItemInInventory(*MyPlayer, sword));
	Item held {};
	InitializeItem(held, IDI_WARRIOR);
	held._iSeed = 654321;
	held._iDurability = std::max(1, held._iMaxDur - 2);
	MyPlayer->HoldItem = held;
	RememberSurvivorsSale(MyPlayerId, sword, 123);
	ASSERT_TRUE(pfile_save_survivors_ledger(*MyPlayer, *identity, 42));
	ResetSurvivorsBuyback();
	MyPlayer->_pGold = 0;
	MyPlayer->HoldItem.clear();
	committed = 0;
	ASSERT_EQ(pfile_load_survivors_ledger(*MyPlayer, *identity, committed), SurvivorsLedgerLoad::Loaded);
	EXPECT_EQ(committed, 42U);
	EXPECT_EQ(MyPlayer->_pGold, 10000);
	EXPECT_EQ(MyPlayer->HoldItem._iSeed, held._iSeed);
	EXPECT_EQ(MyPlayer->HoldItem._iDurability, held._iDurability);
	ASSERT_EQ(GetSurvivorsBuybackItems(MyPlayerId).size(), 1U);
	EXPECT_EQ(GetSurvivorsBuybackItems(MyPlayerId)[0]._iSeed, sword._iSeed);
	EXPECT_EQ(GetSurvivorsBuybackItems(MyPlayerId)[0]._iDurability, sword._iDurability);
	EXPECT_EQ(GetSurvivorsBuybackPrices(MyPlayerId)[0], 123);
	Item redeemed; int price = 0;
	ASSERT_TRUE(RemoveSurvivorsBuyback(MyPlayerId, 0, sword._iSeed, redeemed, price));
	ASSERT_TRUE(pfile_save_survivors_ledger(*MyPlayer, *identity, 43));
	EXPECT_FALSE(pfile_save_survivors_ledger(*MyPlayer, *identity, 42));
	ASSERT_EQ(pfile_load_survivors_ledger(*MyPlayer, *identity, committed), SurvivorsLedgerLoad::Loaded);
	EXPECT_EQ(committed, 43U);
	EXPECT_TRUE(GetSurvivorsBuybackItems(MyPlayerId).empty());
	for (const auto &entry : std::filesystem::directory_iterator(folder)) {
		if (entry.path().extension() == ".sv") { std::ofstream corrupt(entry.path(), std::ios::binary | std::ios::trunc); corrupt << "bad"; }
	}
	EXPECT_EQ(pfile_load_survivors_ledger(*MyPlayer, *identity, committed), SurvivorsLedgerLoad::Corrupt);
	EXPECT_FALSE(pfile_save_survivors_ledger(*MyPlayer, *identity, 44));
}

TEST_F(InventoryUITest, PortableCooperativeCheckpointRejectsConflictsAndImportsNewerRevision)
{
	const std::string oldPref = paths::PrefPath();
	const bool oldMultiplayer = gbIsMultiplayer;
	const auto folder = std::filesystem::temp_directory_path() / "damnationx-coop-portable-test";
	std::filesystem::remove_all(folder);
	std::filesystem::create_directories(folder / "a");
	std::filesystem::create_directories(folder / "b");
	struct Cleanup {
		std::string oldPref; bool oldMultiplayer; std::filesystem::path folder;
		~Cleanup() { SetSurvivorsMode(false); gbIsMultiplayer = oldMultiplayer; paths::SetPrefPath(oldPref); ResetSurvivorsBuyback(); std::filesystem::remove_all(folder); }
	} cleanup { oldPref, oldMultiplayer, folder };
	paths::SetPrefPath((folder / "a").string() + "/");
	SetSurvivorsMode(true); gbIsMultiplayer = true; ResetSurvivorsBuyback();
	std::strcpy(MyPlayer->_pName, "PortableHero");
	const auto identity = pfile_survivors_identity(*MyPlayer);
	ASSERT_TRUE(identity);
	ASSERT_EQ(AddGoldToInventory(*MyPlayer, 1000), 0);
	MyPlayer->_pGold = CalculateGold(*MyPlayer);
	ASSERT_TRUE(pfile_save_survivors_ledger(*MyPlayer, *identity, 1));
	const auto first = pfile_export_survivors_ledger(*identity);
	ASSERT_TRUE(first); ASSERT_FALSE(first->empty());
	ASSERT_TRUE(pfile_test_survivors_portable_header(*first));
	for (size_t countOffset : { 24U, 28U }) {
		auto malformed = *first;
		std::fill_n(malformed.begin() + countOffset, 4, std::byte { 0xff });
		EXPECT_FALSE(pfile_test_survivors_portable_header(malformed));
		EXPECT_EQ(pfile_import_survivors_ledger(*MyPlayer, *identity, malformed), SurvivorsLedgerImport::Invalid);
	}
	// A small MPQ can declare members much larger than its transport size.
	// Check each member consumed by import before it can be allocated.
	const std::vector<std::byte> oversized(128 * 1024);
	for (const char *name : { "hero", "heroitems", "survivors-ledger", "survivors-held", "survivors-buyback" }) {
		const auto malicious = folder / "malicious.sv";
		{
			std::ofstream file(malicious, std::ios::binary | std::ios::trunc);
			ASSERT_TRUE(file.write(reinterpret_cast<const char *>(first->data()), first->size()));
		}
		ASSERT_TRUE(pfile_test_survivors_portable_members(malicious.string()));
		{
			SaveWriter writer(malicious.string(), true);
			ASSERT_TRUE(writer.WriteFile(name, oversized.data(), oversized.size()));
		}
		const auto archive = MpqArchive::Open(malicious.string().c_str());
		ASSERT_TRUE(archive.has_value());
		ASSERT_EQ(archive->GetFileSize(name), oversized.size());
		const auto fileSize = std::filesystem::file_size(malicious);
		ASSERT_LT(fileSize, 1024U * 1024U);
		EXPECT_FALSE(pfile_test_survivors_portable_members(malicious.string())) << name;
		std::vector<std::byte> bytes(fileSize);
		std::ifstream file(malicious, std::ios::binary);
		ASSERT_TRUE(file.read(reinterpret_cast<char *>(bytes.data()), bytes.size()));
		EXPECT_EQ(pfile_import_survivors_ledger(*MyPlayer, *identity, bytes), SurvivorsLedgerImport::Invalid) << name;
	}
	paths::SetPrefPath((folder / "b").string() + "/");
	EXPECT_EQ(pfile_import_survivors_ledger(*MyPlayer, *identity, *first), SurvivorsLedgerImport::Imported);
	EXPECT_EQ(pfile_import_survivors_ledger(*MyPlayer, *identity, *first), SurvivorsLedgerImport::Unchanged);
	ASSERT_EQ(AddGoldToInventory(*MyPlayer, 500), 0);
	MyPlayer->_pGold = CalculateGold(*MyPlayer);
	ASSERT_TRUE(pfile_save_survivors_ledger(*MyPlayer, *identity, 1));
	const auto conflicting = pfile_export_survivors_ledger(*identity);
	ASSERT_TRUE(conflicting);
	paths::SetPrefPath((folder / "a").string() + "/");
	EXPECT_EQ(pfile_import_survivors_ledger(*MyPlayer, *identity, *conflicting), SurvivorsLedgerImport::Conflict);
	uint64_t revision = 0;
	ASSERT_EQ(pfile_load_survivors_ledger(*MyPlayer, *identity, revision), SurvivorsLedgerLoad::Loaded);
	EXPECT_EQ(MyPlayer->_pGold, 1000);
	paths::SetPrefPath((folder / "b").string() + "/");
	ASSERT_EQ(pfile_load_survivors_ledger(*MyPlayer, *identity, revision), SurvivorsLedgerLoad::Loaded);
	ASSERT_TRUE(pfile_save_survivors_ledger(*MyPlayer, *identity, 2));
	const auto newer = pfile_export_survivors_ledger(*identity);
	ASSERT_TRUE(newer);
	paths::SetPrefPath((folder / "a").string() + "/");
	EXPECT_EQ(pfile_import_survivors_ledger(*MyPlayer, *identity, *newer, 3), SurvivorsLedgerImport::Invalid);
	EXPECT_EQ(pfile_import_survivors_ledger(*MyPlayer, *identity, *newer), SurvivorsLedgerImport::Imported);
	EXPECT_EQ(pfile_import_survivors_ledger(*MyPlayer, *identity, *first), SurvivorsLedgerImport::Older);
	ASSERT_EQ(pfile_load_survivors_ledger(*MyPlayer, *identity, revision), SurvivorsLedgerLoad::Loaded);
	EXPECT_EQ(revision, 2U); EXPECT_EQ(MyPlayer->_pGold, 1500);
	std::vector<std::byte> broken(24);
	EXPECT_EQ(pfile_import_survivors_ledger(*MyPlayer, *identity, broken), SurvivorsLedgerImport::Invalid);
	ASSERT_EQ(pfile_load_survivors_ledger(*MyPlayer, *identity, revision), SurvivorsLedgerLoad::Loaded);
	EXPECT_EQ(revision, 2U);
}

TEST_F(InventoryUITest, CooperativeIdentityRejectsCorruptTokenWithoutReplacingIt)
{
	const std::string oldPref = paths::PrefPath();
	const auto folder = std::filesystem::temp_directory_path() / "damnationx-coop-identity-test";
	std::filesystem::remove_all(folder);
	std::filesystem::create_directories(folder);
	struct Cleanup { std::string oldPref; std::filesystem::path folder; ~Cleanup() { paths::SetPrefPath(oldPref); std::filesystem::remove_all(folder); } } cleanup { oldPref, folder };
	paths::SetPrefPath(folder.string() + "/");
	std::strcpy(MyPlayer->_pName, "LedgerHero");
	ASSERT_TRUE(pfile_survivors_identity(*MyPlayer));
	std::filesystem::path token;
	for (const auto &entry : std::filesystem::directory_iterator(folder)) if (entry.path().extension() == ".dat") token = entry.path();
	ASSERT_FALSE(token.empty());
	{ std::ofstream corrupt(token, std::ios::binary | std::ios::trunc); corrupt << "broken"; }
	EXPECT_FALSE(pfile_survivors_identity(*MyPlayer));
	EXPECT_EQ(std::filesystem::file_size(token), 6U);
}

TEST_F(InventoryUITest, CooperativeJournalRollsForwardAnInterruptedTwoPlayerTransfer)
{
	const std::string oldPref = paths::PrefPath();
	const bool oldMultiplayer = gbIsMultiplayer;
	const auto folder = std::filesystem::temp_directory_path() / "damnationx-coop-journal-test";
	std::filesystem::remove_all(folder);
	std::filesystem::create_directories(folder);
	struct Cleanup {
		std::string oldPref; bool oldMultiplayer; std::filesystem::path folder;
		~Cleanup() { pfile_test_survivors_ledger_interrupt_after(0); SetSurvivorsMode(false); gbIsMultiplayer = oldMultiplayer; paths::SetPrefPath(oldPref); ResetSurvivorsBuyback(); std::filesystem::remove_all(folder); }
	} cleanup { oldPref, oldMultiplayer, folder };
	paths::SetPrefPath(folder.string() + "/");
	SetSurvivorsMode(true);
	gbIsMultiplayer = true;
	Players.resize(2);
	MyPlayer = &Players[0];
	CreatePlayer(Players[1], HeroClass::Warrior);
	InitCursor(); // CreatePlayer owns and frees its temporary cursor data.
	for (auto &item : Players[1].InvList) item.clear();
	std::fill(std::begin(Players[1].InvGrid), std::end(Players[1].InvGrid), 0);
	Players[1]._pNumInv = 0;
	Players[1]._pGold = 0;
	std::strcpy(Players[0]._pName, "FirstHero");
	std::strcpy(Players[1]._pName, "SecondHero");
	const auto first = pfile_survivors_identity(Players[0]);
	const auto second = pfile_survivors_identity(Players[1]);
	ASSERT_TRUE(first); ASSERT_TRUE(second); ASSERT_NE(*first, *second);
	ResetSurvivorsBuyback();
	ASSERT_EQ(AddGoldToInventory(Players[0], 1000), 0);
	ASSERT_EQ(AddGoldToInventory(Players[1], 1000), 0);
	Players[0]._pGold = CalculateGold(Players[0]);
	Players[1]._pGold = CalculateGold(Players[1]);
	const std::array initial { SurvivorsLedgerEntry { &Players[0], *first, 1 }, SurvivorsLedgerEntry { &Players[1], *second, 1 } };
	ASSERT_TRUE(pfile_commit_survivors_ledgers(initial));
	// Simulate a transfer in one authority tick and power loss after one of the
	// committed journal's replacements. Replay must install both or block all.
	ASSERT_EQ(Players[0]._pNumInv, 1);
	Players[0].InvList[0]._ivalue = 700;
	SetPlrHandGoldCurs(Players[0].InvList[0]);
	ASSERT_EQ(AddGoldToInventory(Players[1], 300), 0);
	Players[0]._pGold = CalculateGold(Players[0]);
	Players[1]._pGold = CalculateGold(Players[1]);
	const std::array transfer { SurvivorsLedgerEntry { &Players[0], *first, 2 }, SurvivorsLedgerEntry { &Players[1], *second, 2 } };
	pfile_test_survivors_ledger_interrupt_after(1);
	ASSERT_FALSE(pfile_commit_survivors_ledgers(transfer));
	ASSERT_TRUE(pfile_recover_survivors_ledger_journal());
	uint64_t firstRevision = 0, secondRevision = 0;
	ASSERT_EQ(pfile_load_survivors_ledger(Players[0], *first, firstRevision), SurvivorsLedgerLoad::Loaded);
	ASSERT_EQ(pfile_load_survivors_ledger(Players[1], *second, secondRevision), SurvivorsLedgerLoad::Loaded);
	EXPECT_EQ(firstRevision, 2U); EXPECT_EQ(secondRevision, 2U);
	EXPECT_EQ(Players[0]._pGold, 700); EXPECT_EQ(Players[1]._pGold, 1300);
}

TEST_F(InventoryUITest, CooperativeGroundAndInventoryRecoverTogetherAfterInterruptedDropAndPickup)
{
	const std::string oldPref = paths::PrefPath();
	const bool oldMultiplayer = gbIsMultiplayer;
	const auto folder = std::filesystem::temp_directory_path() / "damnationx-coop-ground-journal-test";
	std::filesystem::remove_all(folder);
	std::filesystem::create_directories(folder);
	struct Cleanup {
		std::string oldPref; bool oldMultiplayer; std::filesystem::path folder;
		~Cleanup() { pfile_test_survivors_ledger_interrupt_after(0); SetSurvivorsMode(false); gbIsMultiplayer = oldMultiplayer; paths::SetPrefPath(oldPref); ResetSurvivorsBuyback(); std::filesystem::remove_all(folder); }
	} cleanup { oldPref, oldMultiplayer, folder };
	paths::SetPrefPath(folder.string() + "/");
	SetSurvivorsMode(true);
	gbIsMultiplayer = true;
	std::strcpy(MyPlayer->_pName, "GroundTest");
	const auto identity = pfile_survivors_identity(*MyPlayer);
	ASSERT_TRUE(identity);
	Item sword {};
	InitializeItem(sword, IDI_WARRIOR);
	sword._iSeed = 1234567;
	ASSERT_TRUE(AutoPlaceItemInInventory(*MyPlayer, sword));
	const std::array first { SurvivorsLedgerEntry { MyPlayer, *identity, 1 } };
	const std::vector<Item> empty;
	ASSERT_TRUE(pfile_commit_survivors_ledgers(first, std::span<const Item>(empty)));
	MyPlayer->RemoveInvItem(MyPlayer->_pNumInv - 1);
	const std::vector<Item> dropped { sword };
	const std::array second { SurvivorsLedgerEntry { MyPlayer, *identity, 2 } };
	pfile_test_survivors_ledger_interrupt_after(1);
	ASSERT_FALSE(pfile_commit_survivors_ledgers(second, std::span<const Item>(dropped)));
	std::vector<Item> recovered;
	ASSERT_EQ(pfile_load_survivors_ground(recovered), SurvivorsLedgerLoad::Loaded);
	ASSERT_EQ(recovered.size(), 1U);
	EXPECT_EQ(recovered[0]._iSeed, sword._iSeed);
	uint64_t revision = 0;
	ASSERT_EQ(pfile_load_survivors_ledger(*MyPlayer, *identity, revision), SurvivorsLedgerLoad::Loaded);
	EXPECT_EQ(revision, 2U);
	EXPECT_EQ(MyPlayer->_pNumInv, 0);
	ASSERT_TRUE(AutoPlaceItemInInventory(*MyPlayer, recovered[0]));
	const std::array third { SurvivorsLedgerEntry { MyPlayer, *identity, 3 } };
	pfile_test_survivors_ledger_interrupt_after(1);
	ASSERT_FALSE(pfile_commit_survivors_ledgers(third, std::span<const Item>(empty)));
	ASSERT_EQ(pfile_load_survivors_ground(recovered), SurvivorsLedgerLoad::Loaded);
	EXPECT_TRUE(recovered.empty());
	ASSERT_EQ(pfile_load_survivors_ledger(*MyPlayer, *identity, revision), SurvivorsLedgerLoad::Loaded);
	EXPECT_EQ(revision, 3U);
	ASSERT_EQ(MyPlayer->_pNumInv, 1);
	EXPECT_EQ(MyPlayer->InvList[0]._iSeed, sword._iSeed);
}

#ifndef _WIN32
TEST_F(InventoryUITest, SharedAuthorityLoadsGroundAcrossDifferentHostProfiles)
{
	const std::string oldPref = paths::PrefPath();
	const bool oldMultiplayer = gbIsMultiplayer;
	const char *oldValue = std::getenv("DAMNATION_SURVIVORS_SHARED_DIR");
	const std::optional<std::string> oldShared = oldValue ? std::optional<std::string>(oldValue) : std::nullopt;
	const auto folder = std::filesystem::temp_directory_path() / "damnationx-shared-ground-test";
	std::filesystem::remove_all(folder);
	std::filesystem::create_directories(folder / "shared");
	std::filesystem::create_directories(folder / "first");
	std::filesystem::create_directories(folder / "second");
	struct Cleanup {
		std::string oldPref; bool oldMultiplayer; std::optional<std::string> oldShared; std::filesystem::path folder;
		~Cleanup() {
			pfile_end_survivors_shared_authority(); SetSurvivorsMode(false); gbIsMultiplayer = oldMultiplayer;
			paths::SetPrefPath(oldPref); ResetSurvivorsBuyback();
			if (oldShared) setenv("DAMNATION_SURVIVORS_SHARED_DIR", oldShared->c_str(), 1);
			else unsetenv("DAMNATION_SURVIVORS_SHARED_DIR");
			std::filesystem::remove_all(folder);
		}
	} cleanup { oldPref, oldMultiplayer, oldShared, folder };
	paths::SetPrefPath((folder / "first").string() + "/");
	SetSurvivorsMode(true); gbIsMultiplayer = true;
	ASSERT_EQ(setenv("DAMNATION_SURVIVORS_SHARED_DIR", (folder / "shared").c_str(), 1), 0);
	ASSERT_TRUE(pfile_begin_survivors_shared_authority());
	Item sword {};
	InitializeItem(sword, IDI_WARRIOR); sword._iSeed = 0x12345678;
	const std::vector<Item> ground { sword };
	ASSERT_TRUE(pfile_commit_survivors_ledgers({}, std::span<const Item>(ground)));
	EXPECT_FALSE(std::filesystem::exists(folder / "first" / "survivors-coop-ground.sv"));
	EXPECT_TRUE(std::filesystem::exists(folder / "shared" / "survivors-coop-ground.sv"));
	pfile_end_survivors_shared_authority();
	paths::SetPrefPath((folder / "second").string() + "/");
	ASSERT_TRUE(pfile_begin_survivors_shared_authority());
	std::vector<Item> recovered;
	ASSERT_EQ(pfile_load_survivors_ground(recovered), SurvivorsLedgerLoad::Loaded);
	ASSERT_EQ(recovered.size(), 1U);
	EXPECT_EQ(recovered[0]._iSeed, sword._iSeed);
	pfile_end_survivors_shared_authority();
	ASSERT_EQ(setenv("DAMNATION_SURVIVORS_SHARED_DIR", "relative-folder", 1), 0);
	EXPECT_FALSE(pfile_begin_survivors_shared_authority());
}
#endif
#endif
TEST_F(InventoryUITest, SurvivorsGearRoundTripPreservesBackupWhenPrimaryIsCorrupt)
{
	const std::string oldPref = paths::PrefPath();
	const auto folder = std::filesystem::temp_directory_path() / "damnationx-survivors-gear-test";
	std::filesystem::remove_all(folder);
	std::filesystem::create_directories(folder);
	struct Cleanup {
		std::string oldPref;
		std::filesystem::path folder;
		~Cleanup() { SetSurvivorsMode(false); paths::SetPrefPath(oldPref); std::filesystem::remove_all(folder); }
	} cleanup { oldPref, folder };
	paths::SetPrefPath(folder.string() + "/");
	SetSurvivorsMode(true);
	std::strcpy(MyPlayer->_pName, "GearTest");
	ASSERT_EQ(AddGoldToInventory(*MyPlayer, 10000), 0);
	MyPlayer->_pGold = CalculateGold(*MyPlayer);
	Item sword {};
	InitializeItem(sword, IDI_WARRIOR);
	sword._iSeed = 123456;
	sword._iDurability = std::max(1, sword._iMaxDur - 1);
	const auto seed = sword._iSeed;
	const auto durability = sword._iDurability;
	ASSERT_TRUE(AutoPlaceItemInInventory(*MyPlayer, sword));
	pfile_load_survivors_inventory(*MyPlayer);
	ASSERT_TRUE(pfile_survivors_inventory_available());
	RememberSurvivorsSale(MyPlayerId, sword, 123);
	const int gold = MyPlayer->_pGold;
	ASSERT_TRUE(pfile_save_survivors_inventory());
	ASSERT_TRUE(pfile_save_survivors_inventory());
	std::filesystem::path primary;
	for (const auto &entry : std::filesystem::directory_iterator(folder)) {
		if (entry.path().extension() == ".sv") primary = entry.path();
	}
	ASSERT_FALSE(primary.empty());
	ResetSurvivorsBuyback();
	MyPlayer->_pGold = 0;
	pfile_load_survivors_inventory(*MyPlayer);
	EXPECT_EQ(MyPlayer->_pGold, gold);
	ASSERT_EQ(GetSurvivorsBuybackItems(MyPlayerId).size(), 1U);
	ASSERT_EQ(GetSurvivorsBuybackPrices(MyPlayerId).size(), 1U);
	EXPECT_EQ(GetSurvivorsBuybackItems(MyPlayerId)[0]._iSeed, seed);
	EXPECT_EQ(GetSurvivorsBuybackItems(MyPlayerId)[0]._iDurability, durability);
	EXPECT_EQ(GetSurvivorsBuybackPrices(MyPlayerId)[0], 123);
	const auto loaded = std::find_if(std::begin(MyPlayer->InvList), std::begin(MyPlayer->InvList) + MyPlayer->_pNumInv,
	    [seed](const Item &item) { return item._iSeed == seed; });
	ASSERT_NE(loaded, std::begin(MyPlayer->InvList) + MyPlayer->_pNumInv);
	EXPECT_EQ(loaded->_iDurability, durability);
	Item redeemed;
	int redeemedPrice = 0;
	ASSERT_TRUE(RemoveSurvivorsBuyback(MyPlayerId, 0, seed, redeemed, redeemedPrice));
	EXPECT_EQ(redeemedPrice, 123);
	ASSERT_TRUE(pfile_save_survivors_inventory());
	pfile_load_survivors_inventory(*MyPlayer);
	EXPECT_TRUE(GetSurvivorsBuybackItems(MyPlayerId).empty());
	{
		std::ofstream corrupt(primary, std::ios::binary | std::ios::trunc);
		corrupt << "corrupt";
	}
	MyPlayer->_pGold = 0;
	ResetSurvivorsBuyback();
	pfile_load_survivors_inventory(*MyPlayer);
	EXPECT_EQ(MyPlayer->_pGold, gold);
	ASSERT_EQ(GetSurvivorsBuybackItems(MyPlayerId).size(), 1U);
	EXPECT_EQ(GetSurvivorsBuybackItems(MyPlayerId)[0]._iSeed, seed);
	EXPECT_EQ(GetSurvivorsBuybackPrices(MyPlayerId)[0], 123);
	EXPECT_FALSE(pfile_survivors_inventory_available());
	EXPECT_FALSE(pfile_save_survivors_inventory());
	EXPECT_EQ(std::filesystem::file_size(primary), 7U);
}

TEST_F(InventoryUITest, SurvivorsBuybackRestoresTheSoldItemAndItsGold)
{
	SetSurvivorsMode(true);
	struct Cleanup { ~Cleanup() { IsVisualStoreOpen = false; ResetSurvivorsBuyback(); SetSurvivorsMode(false); } } cleanup;
	ResetSurvivorsBuyback();
	VisualStore.vendor = VisualStoreVendor::Smith;
	VisualStore.activeTab = VisualStoreTab::Basic;
	IsVisualStoreOpen = true;
	Item sword {};
	InitializeItem(sword, IDI_WARRIOR);
	sword._iSeed = 654321;
	sword._iStatFlag = true;
	sword._iDurability = std::max(1, sword._iMaxDur - 1);
	const auto seed = sword._iSeed;
	const auto durability = sword._iDurability;
	ASSERT_TRUE(AutoPlaceItemInInventory(*MyPlayer, sword));
	const int originalGold = MyPlayer->_pGold;
	const int index = MyPlayer->_pNumInv - 1;
	SellItemToVisualStore(index);
	ASSERT_GT(MyPlayer->_pGold, originalGold);
	ToggleSurvivorsBuyback();
	ASSERT_EQ(VisualStore.activeTab, VisualStoreTab::Buyback);
	ASSERT_FALSE(VisualStore.pages.empty());
	ASSERT_EQ(VisualStore.pages.front().items.size(), 1U);
	const Point position = GetVisualStoreSlotCoord(VisualStore.pages.front().items.front().position) + Displacement { 1, 1 };
	CheckVisualStoreItem(position, false, false);
	EXPECT_EQ(MyPlayer->_pGold, originalGold);
	const auto found = std::find_if(std::begin(MyPlayer->InvList), std::begin(MyPlayer->InvList) + MyPlayer->_pNumInv,
	    [seed](const Item &item) { return item._iSeed == seed; });
	const auto equipped = std::find_if(std::begin(MyPlayer->InvBody), std::end(MyPlayer->InvBody),
	    [seed](const Item &item) { return item._iSeed == seed && !item.isEmpty(); });
	ASSERT_TRUE(found != std::begin(MyPlayer->InvList) + MyPlayer->_pNumInv || equipped != std::end(MyPlayer->InvBody));
	EXPECT_EQ(found != std::begin(MyPlayer->InvList) + MyPlayer->_pNumInv ? found->_iDurability : equipped->_iDurability, durability);
	EXPECT_EQ(GetVisualStoreItemCount(), 0);
}

TEST_F(InventoryUITest, ReplacedShopStockUsesTheLocalPlayersRequirements)
{
	VisualStore.vendor = VisualStoreVendor::Smith;
	VisualStore.activeTab = VisualStoreTab::Basic;
	SmithItems.clear();
	Item sword { };
	InitializeItem(sword, IDI_WARRIOR);
	ASSERT_TRUE(MyPlayer->CanUseItem(sword));
	sword._iStatFlag = false; // Like an item reconstructed by a world snapshot.
	SmithItems.push_back(sword);
	RefreshVisualStoreItemEligibility();
	EXPECT_TRUE(SmithItems.front()._iStatFlag);
	MyPlayer->_pStrength = 0;
	RefreshVisualStoreItemEligibility();
	EXPECT_FALSE(SmithItems.front()._iStatFlag);
	SmithItems.clear();
}
#endif

// ===========================================================================
// AutoEquip tests
// ===========================================================================

TEST_F(InventoryUITest, AutoEquip_SwordGoesToHand)
{
	Item sword = MakeSword();
	sword._iStatFlag = true; // Player meets stat requirements

	bool equipped = AutoEquip(*MyPlayer, sword);

	EXPECT_TRUE(equipped) << "AutoEquip should succeed for a usable sword";
	EXPECT_FALSE(MyPlayer->InvBody[INVLOC_HAND_LEFT].isEmpty())
	    << "Sword should be placed in the left hand slot";
	EXPECT_EQ(MyPlayer->InvBody[INVLOC_HAND_LEFT].IDidx, sword.IDidx)
	    << "Equipped item should match the sword we created";
}

TEST_F(InventoryUITest, AutoEquip_ReturnsFalseForUnusableItem)
{
	Item sword = MakeSword();
	sword._iStatFlag = false; // Player does NOT meet stat requirements

	bool equipped = AutoEquip(*MyPlayer, sword);

	EXPECT_FALSE(equipped)
	    << "AutoEquip should return false when _iStatFlag is false";
	EXPECT_TRUE(MyPlayer->InvBody[INVLOC_HAND_LEFT].isEmpty())
	    << "Left hand slot should remain empty";
	EXPECT_TRUE(MyPlayer->InvBody[INVLOC_HAND_RIGHT].isEmpty())
	    << "Right hand slot should remain empty";
}

TEST_F(InventoryUITest, AutoEquip_FailsWhenSlotOccupied)
{
	// Equip the first sword.
	Item sword1 = MakeSword();
	sword1._iStatFlag = true;
	ASSERT_TRUE(AutoEquip(*MyPlayer, sword1));

	// Also fill the right hand so neither hand slot is free.
	Item sword2 = MakeSword();
	sword2._iStatFlag = true;
	MyPlayer->InvBody[INVLOC_HAND_RIGHT] = sword2;

	// Now try to auto-equip a third sword — both hand slots are occupied,
	// so CanEquip should reject it (AutoEquip does NOT swap).
	Item sword3 = MakeSword();
	sword3._iStatFlag = true;

	bool equipped = AutoEquip(*MyPlayer, sword3);

	EXPECT_FALSE(equipped)
	    << "AutoEquip should return false when all valid body slots are occupied";
}

// ===========================================================================
// AutoPlaceItemInInventory / CanFitItemInInventory tests
// ===========================================================================

TEST_F(InventoryUITest, AutoPlaceItem_EmptyInventory)
{
	Item sword = MakeSword();

	bool placed = AutoPlaceItemInInventory(*MyPlayer, sword);

	EXPECT_TRUE(placed) << "Should be able to place an item in an empty inventory";
	EXPECT_EQ(MyPlayer->_pNumInv, 1)
	    << "Inventory count should be 1 after placing one item";

	// Verify the item is actually stored.
	EXPECT_EQ(MyPlayer->InvList[0].IDidx, sword.IDidx)
	    << "InvList[0] should contain the sword";

	// Verify at least one grid cell is set (non-zero).
	bool gridSet = false;
	for (int i = 0; i < InventoryGridCells; i++) {
		if (MyPlayer->InvGrid[i] != 0) {
			gridSet = true;
			break;
		}
	}
	EXPECT_TRUE(gridSet) << "At least one InvGrid cell should be non-zero";
}

TEST_F(InventoryUITest, AutoPlaceItem_FullInventory)
{
	FillInventory();

	// Inventory is full — a new item should not fit.
	Item extraPotion = MakePotion();
	bool placed = AutoPlaceItemInInventory(*MyPlayer, extraPotion);

	EXPECT_FALSE(placed)
	    << "Should not be able to place an item in a completely full inventory";
}

TEST_F(InventoryUITest, CanFitItem_EmptyInventory)
{
	Item sword = MakeSword();

	EXPECT_TRUE(CanFitItemInInventory(*MyPlayer, sword))
	    << "An empty inventory should have room for a sword";
}

TEST_F(InventoryUITest, CanFitItem_FullInventory)
{
	FillInventory();

	Item extraPotion = MakePotion();

	EXPECT_FALSE(CanFitItemInInventory(*MyPlayer, extraPotion))
	    << "A completely full inventory should report no room";
}

// ===========================================================================
// Belt tests
// ===========================================================================

TEST_F(InventoryUITest, CanBePlacedOnBelt_Potion)
{
	Item potion = MakePotion();

	EXPECT_TRUE(CanBePlacedOnBelt(*MyPlayer, potion))
	    << "A healing potion (1×1) should be placeable on the belt";
}

TEST_F(InventoryUITest, CanBePlacedOnBelt_Sword)
{
	Item sword = MakeSword();

	EXPECT_FALSE(CanBePlacedOnBelt(*MyPlayer, sword))
	    << "A sword (2×1 weapon) should NOT be placeable on the belt";
}

TEST_F(InventoryUITest, AutoPlaceBelt_Success)
{
	Item potion = MakePotion();

	bool placed = AutoPlaceItemInBelt(*MyPlayer, potion, /*persistItem=*/true);

	EXPECT_TRUE(placed) << "Should be able to place a potion in an empty belt";

	// Verify at least one belt slot contains the item.
	bool found = false;
	for (int i = 0; i < MaxBeltItems; i++) {
		if (!MyPlayer->SpdList[i].isEmpty()) {
			found = true;
			break;
		}
	}
	EXPECT_TRUE(found) << "Potion should appear in one of the belt slots";
}

TEST_F(InventoryUITest, AutoPlaceBelt_Full)
{
	FillBelt();

	Item extraPotion = MakePotion();
	bool placed = AutoPlaceItemInBelt(*MyPlayer, extraPotion, /*persistItem=*/true);

	EXPECT_FALSE(placed)
	    << "Should not be able to place a potion when all 8 belt slots are occupied";
}

// ===========================================================================
// RemoveEquipment test
// ===========================================================================

TEST_F(InventoryUITest, RemoveEquipment_ClearsBodySlot)
{
	// Equip a sword in the left hand.
	Item sword = MakeSword();
	sword._iStatFlag = true;
	ASSERT_TRUE(AutoEquip(*MyPlayer, sword));
	ASSERT_FALSE(MyPlayer->InvBody[INVLOC_HAND_LEFT].isEmpty())
	    << "Precondition: left hand should have the sword";

	RemoveEquipment(*MyPlayer, INVLOC_HAND_LEFT, false);

	EXPECT_TRUE(MyPlayer->InvBody[INVLOC_HAND_LEFT].isEmpty())
	    << "Left hand slot should be empty after RemoveEquipment";
}

// ===========================================================================
// ReorganizeInventory test
// ===========================================================================

TEST_F(InventoryUITest, ReorganizeInventory_DefragmentsGrid)
{
	// Place three potions via AutoPlace so the grid is properly populated.
	Item p1 = MakePotion();
	Item p2 = MakePotion();
	Item p3 = MakePotion();

	ASSERT_TRUE(AutoPlaceItemInInventory(*MyPlayer, p1));
	ASSERT_TRUE(AutoPlaceItemInInventory(*MyPlayer, p2));
	ASSERT_TRUE(AutoPlaceItemInInventory(*MyPlayer, p3));
	ASSERT_EQ(MyPlayer->_pNumInv, 3);

	// Remove the middle item to create a gap.
	MyPlayer->RemoveInvItem(1);
	ASSERT_EQ(MyPlayer->_pNumInv, 2);

	// Reorganize should keep all remaining items and defragment the grid.
	ReorganizeInventory(*MyPlayer);

	EXPECT_EQ(MyPlayer->_pNumInv, 2)
	    << "Item count should be preserved after reorganization";

	// After reorganization, a potion should still fit (there are 38 free cells).
	Item extra = MakePotion();
	EXPECT_TRUE(CanFitItemInInventory(*MyPlayer, extra))
	    << "Should be able to fit another item after reorganization";

	// Verify no InvList entries in the active range are empty.
	for (int i = 0; i < MyPlayer->_pNumInv; i++) {
		EXPECT_FALSE(MyPlayer->InvList[i].isEmpty())
		    << "InvList[" << i << "] should not be empty within active range";
	}
}

// ===========================================================================
// TransferItemToStash tests
// ===========================================================================

TEST_F(InventoryUITest, TransferToStash_FromInventory)
{
	IsStashOpen = true;

	// Place a sword in the inventory.
	Item sword = MakeSword();
	int idx = PlaceItemInInventory(sword);
	ASSERT_GE(idx, 0) << "Failed to place sword in inventory";
	ASSERT_EQ(MyPlayer->_pNumInv, 1);

	int invLocation = INVITEM_INV_FIRST + idx;

	TransferItemToStash(*MyPlayer, invLocation);

	// Item should now be in the stash.
	EXPECT_FALSE(Stash.stashList.empty())
	    << "Stash should contain the transferred item";

	// Item should be removed from inventory.
	EXPECT_EQ(MyPlayer->_pNumInv, 0)
	    << "Inventory should be empty after transferring the only item";
}

TEST_F(InventoryUITest, TransferToStash_InvalidLocation)
{
	IsStashOpen = true;

	size_t stashSizeBefore = Stash.stashList.size();
	int invCountBefore = MyPlayer->_pNumInv;

	// Passing -1 should be a no-op (early return), not a crash.
	TransferItemToStash(*MyPlayer, -1);

	EXPECT_EQ(Stash.stashList.size(), stashSizeBefore)
	    << "Stash should be unchanged after invalid transfer";
	EXPECT_EQ(MyPlayer->_pNumInv, invCountBefore)
	    << "Inventory should be unchanged after invalid transfer";
}

} // namespace
} // namespace devilution
