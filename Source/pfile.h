/**
 * @file pfile.h
 *
 * Interface of the save game encoding functionality.
 */
#pragma once

#include "utils/attributes.h"

#include <cstdint>
#include <array>
#include <span>
#include <expected>
#include <string>
#include <vector>

#include "DiabloUI/diabloui.h"
#include "player.h"

#ifdef UNPACKED_SAVES
#include "utils/file_util.h"
#else
#include "mpq/mpq_reader.hpp"
#include "mpq/mpq_writer.hpp"
#endif

namespace devilution {

#define MAX_CHARACTERS 99

extern bool gbValidSaveFile;

#ifdef UNPACKED_SAVES
struct SaveReader {
	explicit SaveReader(std::string &&dir)
	    : dir_(std::move(dir))
	{
	}

	const std::string &dir() const
	{
		return dir_;
	}

	std::unique_ptr<std::byte[]> ReadFile(const char *filename, std::size_t &fileSize, int32_t &error);

	bool HasFile(const char *path)
	{
		return ::devilution::FileExists((dir_ + path).c_str());
	}

private:
	std::string dir_;
};

struct SaveWriter {
	explicit SaveWriter(std::string &&dir)
	    : dir_(std::move(dir))
	{
	}

	bool WriteFile(const char *filename, const std::byte *data, size_t size);

	bool HasFile(const char *path)
	{
		return ::devilution::FileExists((dir_ + path).c_str());
	}

	void RenameFile(const char *from, const char *to)
	{
		::devilution::RenameFile((dir_ + from).c_str(), (dir_ + to).c_str());
	}

	void RemoveHashEntry(const char *path)
	{
		RemoveFile((dir_ + path).c_str());
	}

	void RemoveHashEntries(bool (*fnGetName)(uint8_t, char *));

private:
	std::string dir_;
};

#else
using SaveReader = MpqArchive;
using SaveWriter = MpqWriter;
#endif

/**
 * @brief Comparison result of pfile_compare_hero_demo
 */
struct HeroCompareResult {
	enum Status : uint8_t {
		ReferenceNotFound,
		Same,
		Difference,
	};
	Status status;
	std::string message;
};

std::optional<SaveReader> OpenSaveArchive(uint32_t saveNum);
std::optional<SaveReader> OpenStashArchive();
const char *pfile_get_password();
std::unique_ptr<std::byte[]> ReadArchive(SaveReader &archive, const char *pszName, size_t *pdwLen = nullptr);
void pfile_write_hero(bool writeGameData = false);

#ifndef DISABLE_DEMOMODE
/**
 * @brief Save a reference game-state (save game) for the demo recording
 * @param demo that is recorded
 */
void pfile_write_hero_demo(int demo);
/**
 * @brief Compares the actual game-state (savegame) with a reference game-state (save game from demo recording)
 * @param demo for the comparison
 * @param logDetails in case of a difference log details
 * @return The comparison result.
 */
HeroCompareResult pfile_compare_hero_demo(int demo, bool logDetails);
#endif

void sfile_write_stash();
bool pfile_ui_set_hero_infos(bool (*uiAddHeroInfo)(_uiheroinfo *));
void pfile_ui_set_class_stats(HeroClass playerClass, _uidefaultstats *classStats);
uint32_t pfile_ui_get_first_unused_save_num();
bool pfile_ui_save_create(_uiheroinfo *heroinfo);
bool pfile_delete_save(_uiheroinfo *heroInfo);
void pfile_read_player_from_save(uint32_t saveNum, Player &player);
DVL_API_FOR_TEST bool pfile_save_survivors_inventory();
DVL_API_FOR_TEST bool pfile_survivors_inventory_available();
DVL_API_FOR_TEST void pfile_load_survivors_inventory(Player &player);

using SurvivorsIdentity = std::array<uint8_t, 32>;
enum class SurvivorsLedgerLoad { Missing, Loaded, Corrupt };
// Optional cross-host authority. When configured, hold the exclusive shared
// lease across journal recovery and all commits, until the hunt ends.
DVL_API_FOR_TEST bool pfile_begin_survivors_shared_authority();
DVL_API_FOR_TEST void pfile_end_survivors_shared_authority();
// The host owns these records. A corrupt primary is never replaced by a stale
// backup automatically, since doing so could replay an acknowledged purchase.
DVL_API_FOR_TEST std::optional<SurvivorsIdentity> pfile_survivors_identity(const Player &player);
DVL_API_FOR_TEST SurvivorsLedgerLoad pfile_load_survivors_ledger(Player &player, const SurvivorsIdentity &identity, uint64_t &committed);
DVL_API_FOR_TEST bool pfile_save_survivors_ledger(Player &player, const SurvivorsIdentity &identity, uint64_t committed);
// Portable copies are validated against the hero identity before replacing the
// local checkpoint. Equal revisions with different bytes are a conflict.
enum class SurvivorsLedgerImport { Imported, Unchanged, Older, Conflict, Invalid };
DVL_API_FOR_TEST std::optional<std::vector<std::byte>> pfile_export_survivors_ledger(const SurvivorsIdentity &identity);
DVL_API_FOR_TEST SurvivorsLedgerImport pfile_import_survivors_ledger(const Player &player, const SurvivorsIdentity &identity, std::span<const std::byte> bytes, uint64_t expectedRevision = 0);
struct SurvivorsLedgerEntry { Player *player; SurvivorsIdentity identity; uint64_t committed; };
// A committed journal must be replayed before admission or ACK publication.
DVL_API_FOR_TEST bool pfile_recover_survivors_ledger_journal();
DVL_API_FOR_TEST bool pfile_commit_survivors_ledgers(std::span<const SurvivorsLedgerEntry> entries, std::optional<std::span<const Item>> ground = std::nullopt);
DVL_API_FOR_TEST SurvivorsLedgerLoad pfile_load_survivors_ground(std::vector<Item> &items);
#ifdef BUILD_TESTING
// Verify the same bounded member preflight used before importing peer data.
DVL_API_FOR_TEST bool pfile_test_survivors_portable_members(const std::string &path);
DVL_API_FOR_TEST bool pfile_test_survivors_portable_header(std::span<const std::byte> bytes);
// Simulate a power loss after this many committed archive replacements.
DVL_API_FOR_TEST void pfile_test_survivors_ledger_interrupt_after(unsigned replacements);
#endif
void pfile_save_level();
std::expected<void, std::string> pfile_convert_levels();
void pfile_remove_temp_files();
std::unique_ptr<std::byte[]> pfile_read(const char *pszName, size_t *pdwLen);
void pfile_update(bool forceSave);
std::string pfile_get_save_path();
bool pfile_write_game_snapshot(const std::string &path);
void pfile_set_save_path_override(const std::string *path);

} // namespace devilution
