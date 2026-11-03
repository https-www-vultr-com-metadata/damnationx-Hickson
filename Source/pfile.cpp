/**
 * @file pfile.cpp
 *
 * Implementation of the save game encoding functionality.
 */
#include "pfile.h"
#include "survivors.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <expected>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#ifdef PACKET_ENCRYPTION
#include <sodium.h>
#endif
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>
#endif

#include <ankerl/unordered_dense.h>

#ifdef USE_SDL3
#include <SDL3/SDL_iostream.h>
#include <SDL3/SDL_timer.h>
#else
#include <SDL.h>
#endif

#include "appfat.h"
#include "codec.h"
#include "engine/load_file.hpp"
#include "engine/render/primitive_render.hpp"
#include "game_mode.hpp"
#include "loadsave.h"
#include "inv.h"
#include "qol/visual_store.h"
#include "menu.h"
#include "mods/mod_identity.h"
#include "mpq/mpq_common.hpp"
#include "pack.h"
#include "qol/stash.h"
#include "tables/playerdat.hpp"
#include "utils/endian_read.hpp"
#include "utils/endian_swap.hpp"
#include "utils/file_util.h"
#include "utils/language.h"
#include "utils/parse_int.hpp"
#include "utils/paths.h"
#include "utils/sdl_compat.h"
#include "utils/stdcompat/filesystem.hpp"
#include "utils/str_cat.hpp"
#include "utils/str_split.hpp"
#include "utils/utf8.hpp"

#ifdef UNPACKED_SAVES
#include "utils/file_util.h"
#else
#include "mpq/mpq_reader.hpp"
#endif

#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#endif

namespace devilution {

#define PASSWORD_SPAWN_SINGLE "adslhfb1"
#define PASSWORD_SPAWN_MULTI "lshbkfg1"
#define PASSWORD_SINGLE "xrgyrkj1"
#define PASSWORD_MULTI "szqnlsk1"

bool gbValidSaveFile;

namespace {

/** List of character names for the character selection screen. */
char hero_names[MAX_CHARACTERS][PlayerNameLength];

void RequestBrowserSave()
{
#ifdef __EMSCRIPTEN__
	emscripten_run_script("if (typeof Module !== 'undefined' && Module.saveToIndexedDB) Module.saveToIndexedDB();");
#endif
}

// Effective save-file extension token (no leading dot). Defaults to "sv"; an active mod may
// override the save namespace by declaring `saveExtension` in its manifest.
std::string_view GetSaveExtension()
{
	const std::string_view modExtension = GetActiveModSaveExtension();
	return modExtension.empty() ? std::string_view("sv") : modExtension;
}

/** Embedded hosts: while set, the hero's save archive is this file instead (pfile_set_save_path_override). */
std::optional<std::string> SavePathOverride;
std::string SurvivorsInventoryPath;
bool SurvivorsInventoryWritable = false;

// The character selector does not expose campaign save slots in this mode.
// Derive a separate, path-safe identity from class and the exact hero name.
std::string SurvivorsInventoryFile(const Player &player)
{
	uint64_t hash = 14695981039346656037ULL;
	for (const unsigned char c : std::string_view(player._pName))
		hash = (hash ^ c) * 1099511628211ULL;
	return StrCat(paths::PrefPath(), "survivors-gear-", static_cast<unsigned>(player._pClass), "-", hash, ".sv");
}

std::string GetSavePath(uint32_t saveNum, std::string_view savePrefix = {})
{
	if (SavePathOverride && savePrefix.empty() && saveNum == gSaveNumber)
		return *SavePathOverride;
	const std::string_view ext = GetSaveExtension();
	return StrCat(paths::PrefPath(), savePrefix,
	    gbIsSpawn
	        ? (gbIsMultiplayer ? "share_" : "spawn_")
	        : (gbIsMultiplayer ? "multi_" : "single_"),
	    saveNum,
#ifdef UNPACKED_SAVES
	    "_", ext, DIRECTORY_SEPARATOR_STR
#else
	    ".", ext
#endif
	);
}

std::string GetStashSavePath()
{
	const std::string_view ext = GetSaveExtension();
	return StrCat(paths::PrefPath(),
	    gbIsSpawn ? "stash_spawn" : "stash",
#ifdef UNPACKED_SAVES
	    "_", ext, DIRECTORY_SEPARATOR_STR
#else
	    ".", ext
#endif
	);
}

bool GetSaveNames(uint8_t index, std::string_view prefix, char *out)
{
	char suf;
	if (index < giNumberOfLevels)
		suf = 'l';
	else if (index < giNumberOfLevels * 2) {
		index -= giNumberOfLevels;
		suf = 's';
	} else {
		return false;
	}

	*BufCopy(out, prefix, std::string_view(&suf, 1), LeftPad(index, 2, '0')) = '\0';
	return true;
}

bool GetPermSaveNames(uint8_t dwIndex, char *szPerm)
{
	return GetSaveNames(dwIndex, "perm", szPerm);
}

bool GetTempSaveNames(uint8_t dwIndex, char *szTemp)
{
	return GetSaveNames(dwIndex, "temp", szTemp);
}

void RenameTempToPerm(SaveWriter &saveWriter)
{
	char szTemp[MaxMpqPathSize];
	char szPerm[MaxMpqPathSize];

	uint32_t dwIndex = 0;
	while (GetTempSaveNames(dwIndex, szTemp)) {
		[[maybe_unused]] const bool result = GetPermSaveNames(dwIndex, szPerm); // DO NOT PUT DIRECTLY INTO ASSERT!
		assert(result);
		dwIndex++;
		if (saveWriter.HasFile(szTemp)) {
			if (saveWriter.HasFile(szPerm))
				saveWriter.RemoveHashEntry(szPerm);
			saveWriter.RenameFile(szTemp, szPerm);
		}
	}
	assert(!GetPermSaveNames(dwIndex, szPerm));
}

bool ReadHero(SaveReader &archive, PlayerPack *pPack)
{
	size_t read;

	auto buf = ReadArchive(archive, "hero", &read);
	if (buf == nullptr)
		return false;

	bool ret = false;
	if (read == sizeof(*pPack)) {
		memcpy(pPack, buf.get(), sizeof(*pPack));
		ret = true;
	}

	return ret;
}

void EncodeHero(SaveWriter &saveWriter, const PlayerPack *pack)
{
	const size_t packedLen = codec_get_encoded_len(sizeof(*pack));
	const std::unique_ptr<std::byte[]> packed { new std::byte[packedLen] };

	memcpy(packed.get(), pack, sizeof(*pack));
	codec_encode(packed.get(), sizeof(*pack), packedLen, pfile_get_password());
	saveWriter.WriteFile("hero", packed.get(), packedLen);
}

SaveWriter GetSaveWriter(uint32_t saveNum, bool carryForward = true)
{
	return SaveWriter(GetSavePath(saveNum), carryForward);
}

SaveWriter GetStashWriter()
{
	return SaveWriter(GetStashSavePath(), /*carryForward=*/true);
}

#ifndef DISABLE_DEMOMODE
void CopySaveFile(uint32_t saveNum, std::string targetPath)
{
	const std::string savePath = GetSavePath(saveNum);
#if defined(UNPACKED_SAVES)
#ifdef DVL_NO_FILESYSTEM
#error "UNPACKED_SAVES requires either DISABLE_DEMOMODE or C++17 <filesystem>"
#endif
	if (!targetPath.empty()) {
		CreateDir(targetPath.c_str());
	}
	for (const std::filesystem::directory_entry &entry : std::filesystem::directory_iterator(savePath)) {
		CopyFileOverwrite(entry.path().string().c_str(), (targetPath + entry.path().filename().string()).c_str());
	}
#else
	CopyFileOverwrite(savePath.c_str(), targetPath.c_str());
#endif
}
#endif

void Game2UiPlayer(const Player &player, _uiheroinfo *heroinfo, bool bHasSaveFile)
{
	CopyUtf8(heroinfo->name, player._pName, sizeof(heroinfo->name));
	heroinfo->level = player.getCharacterLevel();
	heroinfo->heroclass = player._pClass;
	heroinfo->strength = player._pStrength;
	heroinfo->magic = player._pMagic;
	heroinfo->dexterity = player._pDexterity;
	heroinfo->vitality = player._pVitality;
	heroinfo->hassaved = bHasSaveFile;
	heroinfo->herorank = player.pDiabloKillLevel;
	heroinfo->spawned = gbIsSpawn;
}

bool GetFileName(uint8_t lvl, char *dst)
{
	if (gbIsMultiplayer) {
		if (lvl != 0)
			return false;
		memcpy(dst, "hero", 5);
		return true;
	}
	if (GetPermSaveNames(lvl, dst)) {
		return true;
	}
	if (lvl == giNumberOfLevels * 2) {
		memcpy(dst, "game", 5);
		return true;
	}
	if (lvl == giNumberOfLevels * 2 + 1) {
		memcpy(dst, "hero", 5);
		return true;
	}
	return false;
}

bool ArchiveContainsGame(SaveReader &hsArchive)
{
	if (gbIsMultiplayer)
		return false;

	auto gameData = ReadArchive(hsArchive, "game");
	if (gameData == nullptr)
		return false;

	const uint32_t hdr = LoadLE32(gameData.get());

	return IsHeaderValid(hdr);
}

std::optional<SaveReader> CreateSaveReader(std::string &&path)
{
#ifdef UNPACKED_SAVES
	if (!FileExists(path))
		return std::nullopt;
	return SaveReader(std::move(path));
#else
	std::expected<MpqArchive, std::string> opened = MpqArchive::Open(path.c_str());
	if (!opened.has_value()) return std::nullopt;
	return std::move(*opened);
#endif
}

#ifndef DISABLE_DEMOMODE
struct CompareInfo {
	std::unique_ptr<std::byte[]> &data;
	size_t currentPosition;
	size_t size;
	bool isTownLevel;
	bool dataExists;
};

struct CompareCounter {
	int reference;
	int actual;
	[[nodiscard]] int max() const
	{
		return std::max(reference, actual);
	}
	void checkIfDataExists(int count, CompareInfo &compareInfoReference, CompareInfo &compareInfoActual) const
	{
		if (reference == count)
			compareInfoReference.dataExists = false;
		if (actual == count)
			compareInfoActual.dataExists = false;
	}
};

inline bool string_ends_with(std::string_view value, std::string_view suffix)
{
	if (suffix.size() > value.size())
		return false;
	return std::equal(suffix.rbegin(), suffix.rend(), value.rbegin());
}

void CreateDetailDiffs(std::string_view prefix, std::string_view memoryMapFile, CompareInfo &compareInfoReference, CompareInfo &compareInfoActual, ankerl::unordered_dense::segmented_map<std::string, size_t> &foundDiffs)
{
	// Note: Detail diffs are currently only supported in unit tests
	const std::string memoryMapFileAssetName = StrCat(paths::BasePath(), "/test/fixtures/memory_map/", memoryMapFile, ".txt");

	SDL_IOStream *handle = SDL_IOFromFile(memoryMapFileAssetName.c_str(), "r");
	if (handle == nullptr) {
		app_fatal(StrCat("MemoryMapFile ", memoryMapFile, " is missing"));
		return;
	}

	const auto readBytes = static_cast<size_t>(SDL_GetIOSize(handle));
	const std::unique_ptr<std::byte[]> memoryMapFileData { new std::byte[readBytes] };
	SDL_ReadIO(handle, memoryMapFileData.get(), readBytes);
	SDL_CloseIO(handle);

	const std::string_view buffer(reinterpret_cast<const char *>(memoryMapFileData.get()), readBytes);

	ankerl::unordered_dense::segmented_map<std::string, CompareCounter> counter;

	auto getCounter = [&](const std::string &counterAsString) {
		auto it = counter.find(counterAsString);
		if (it != counter.end())
			return it->second;
		const ParseIntResult<int> countFromMapFile = ParseInt<int>(counterAsString);
		if (!countFromMapFile.has_value())
			app_fatal(StrCat("Failed to parse ", counterAsString, " as int"));
		return CompareCounter { countFromMapFile.value(), countFromMapFile.value() };
	};
	auto addDiff = [&](const std::string &diffKey) {
		auto it = foundDiffs.find(diffKey);
		if (it == foundDiffs.end()) {
			foundDiffs.insert_or_assign(diffKey, 1);
		} else {
			foundDiffs.insert_or_assign(diffKey, it->second + 1);
		}
	};

	auto compareBytes = [&](size_t countBytes) {
		if (compareInfoReference.dataExists && compareInfoReference.currentPosition + countBytes > compareInfoReference.size)
			app_fatal(StrCat("Comparison failed. Not enough bytes in reference to compare. Location: ", prefix));
		if (compareInfoActual.dataExists && compareInfoActual.currentPosition + countBytes > compareInfoActual.size)
			app_fatal(StrCat("Comparison failed. Not enough bytes in actual to compare. Location: ", prefix));
		bool result = true;
		if (compareInfoReference.dataExists && compareInfoActual.dataExists)
			result = memcmp(compareInfoReference.data.get() + compareInfoReference.currentPosition, compareInfoActual.data.get() + compareInfoActual.currentPosition, countBytes) == 0;
		if (compareInfoReference.dataExists)
			compareInfoReference.currentPosition += countBytes;
		if (compareInfoActual.dataExists)
			compareInfoActual.currentPosition += countBytes;
		return result;
	};

	auto read32BitInt = [&](CompareInfo &compareInfo, bool useLE) {
		int32_t value = 0;
		if (!compareInfo.dataExists)
			return value;
		if (compareInfo.currentPosition + sizeof(value) > compareInfo.size)
			app_fatal("read32BitInt failed. Too less bytes to read.");
		memcpy(&value, compareInfo.data.get() + compareInfo.currentPosition, sizeof(value));
		if (useLE)
			value = Swap32LE(value);
		else
			value = Swap32BE(value);
		return value;
	};

	for (std::string_view line : SplitByChar(buffer, '\n')) {
		if (!line.empty() && line.back() == '\r')
			line.remove_suffix(1);
		if (line.empty())
			continue;
		const auto tokens = SplitByChar(line, ' ');
		auto it = tokens.begin();
		const auto end = tokens.end();
		if (it == end)
			continue;

		std::string_view command = *it;

		const bool dataExistsReference = compareInfoReference.dataExists;
		const bool dataExistsActual = compareInfoActual.dataExists;

		if (string_ends_with(command, "_HF")) {
			if (!gbIsHellfire)
				continue;
			command.remove_suffix(3);
		}
		if (string_ends_with(command, "_DA")) {
			if (gbIsHellfire)
				continue;
			command.remove_suffix(3);
		}
		if (string_ends_with(command, "_DL")) {
			if (compareInfoReference.isTownLevel && compareInfoActual.isTownLevel)
				continue;
			if (compareInfoReference.isTownLevel)
				compareInfoReference.dataExists = false;
			if (compareInfoActual.isTownLevel)
				compareInfoActual.dataExists = false;
			command.remove_suffix(3);
		}
		if (command == "R" || command == "LT" || command == "LC" || command == "LC_LE") {
			const auto bitsAsString = std::string(*++it);
			const auto comment = std::string(*++it);
			const ParseIntResult<size_t> parsedBytes = ParseInt<size_t>(bitsAsString);
			if (!parsedBytes.has_value())
				app_fatal(StrCat("Failed to parse ", bitsAsString, " as size_t"));
			const auto bytes = static_cast<size_t>(parsedBytes.value() / 8);

			if (command == "LT") {
				const int32_t valueReference = read32BitInt(compareInfoReference, false);
				const int32_t valueActual = read32BitInt(compareInfoActual, false);
				assert(sizeof(valueReference) == bytes);
				compareInfoReference.isTownLevel = valueReference == 0;
				compareInfoActual.isTownLevel = valueActual == 0;
			}
			if (command == "LC" || command == "LC_LE") {
				const int32_t valueReference = read32BitInt(compareInfoReference, command == "LC_LE");
				const int32_t valueActual = read32BitInt(compareInfoActual, command == "LC_LE");
				assert(sizeof(valueReference) == bytes);
				counter.insert_or_assign(std::string(comment), CompareCounter { valueReference, valueActual });
			}

			if (!compareBytes(bytes)) {
				const std::string diffKey = StrCat(prefix, ".", comment);
				addDiff(diffKey);
			}
		} else if (command == "M") {
			const auto countAsString = std::string(*++it);
			const auto bitsAsString = std::string(*++it);
			const std::string_view comment = *++it;

			const CompareCounter count = getCounter(countAsString);
			const ParseIntResult<size_t> parsedBytes = ParseInt<size_t>(bitsAsString);
			if (!parsedBytes.has_value())
				app_fatal(StrCat("Failed to parse ", bitsAsString, " as size_t"));
			const auto bytes = static_cast<size_t>(parsedBytes.value() / 8);
			for (int i = 0; i < count.max(); i++) {
				count.checkIfDataExists(i, compareInfoReference, compareInfoActual);
				if (!compareBytes(bytes)) {
					const std::string diffKey = StrCat(prefix, ".", comment);
					addDiff(diffKey);
				}
			}
		} else if (command == "C") {
			const auto countAsString = std::string(*++it);
			auto subMemoryMapFile = std::string(*++it);
			const auto comment = std::string(*++it);

			const CompareCounter count = getCounter(countAsString);
			subMemoryMapFile.erase(std::remove(subMemoryMapFile.begin(), subMemoryMapFile.end(), '\r'), subMemoryMapFile.end());
			for (int i = 0; i < count.max(); i++) {
				count.checkIfDataExists(i, compareInfoReference, compareInfoActual);
				const std::string subPrefix = StrCat(prefix, ".", comment);
				CreateDetailDiffs(subPrefix, subMemoryMapFile, compareInfoReference, compareInfoActual, foundDiffs);
			}
		}

		compareInfoReference.dataExists = dataExistsReference;
		compareInfoActual.dataExists = dataExistsActual;
	}
}

struct CompareTargets {
	std::string fileName;
	std::string memoryMapFileName;
	bool isTownLevel;
};

HeroCompareResult CompareSaves(const std::string &actualSavePath, const std::string &referenceSavePath, bool logDetails)
{
	std::vector<CompareTargets> possibleFileToCheck;
	possibleFileToCheck.push_back({ "hero", "hero", false });
	possibleFileToCheck.push_back({ "game", "game", false });
	possibleFileToCheck.push_back({ "additionalMissiles", "additionalMissiles", false });
	char szPerm[MaxMpqPathSize];
	for (int i = 0; GetPermSaveNames(i, szPerm); i++) {
		possibleFileToCheck.push_back({ std::string(szPerm), "level", i == 0 });
	}

	SaveReader actualArchive = *CreateSaveReader(std::string(actualSavePath));
	SaveReader referenceArchive = *CreateSaveReader(std::string(referenceSavePath));

	bool compareResult = true;
	std::string message;
	for (const auto &compareTarget : possibleFileToCheck) {
		size_t fileSizeActual = 0;
		auto fileDataActual = ReadArchive(actualArchive, compareTarget.fileName.c_str(), &fileSizeActual);
		size_t fileSizeReference = 0;
		auto fileDataReference = ReadArchive(referenceArchive, compareTarget.fileName.c_str(), &fileSizeReference);
		if (fileDataActual.get() == nullptr && fileDataReference.get() == nullptr) {
			continue;
		}
		if (fileSizeActual == fileSizeReference && memcmp(fileDataReference.get(), fileDataActual.get(), fileSizeActual) == 0)
			continue;
		compareResult = false;
		if (!message.empty())
			message.append("\n");
		if (fileSizeActual != fileSizeReference)
			StrAppend(message, "file \"", compareTarget.fileName, "\" is different size. Expected: ", fileSizeReference, " Actual: ", fileSizeActual);
		else
			StrAppend(message, "file \"", compareTarget.fileName, "\" has different content.");
		if (!logDetails)
			continue;
		ankerl::unordered_dense::segmented_map<std::string, size_t> foundDiffs;
		CompareInfo compareInfoReference = { fileDataReference, 0, fileSizeReference, compareTarget.isTownLevel, fileSizeReference != 0 };
		CompareInfo compareInfoActual = { fileDataActual, 0, fileSizeActual, compareTarget.isTownLevel, fileSizeActual != 0 };
		CreateDetailDiffs(compareTarget.fileName, compareTarget.memoryMapFileName, compareInfoReference, compareInfoActual, foundDiffs);
		if (compareInfoReference.currentPosition != fileSizeReference)
			app_fatal(StrCat("Comparison failed. Uncompared bytes in reference. File: ", compareTarget.fileName));
		if (compareInfoActual.currentPosition != fileSizeActual)
			app_fatal(StrCat("Comparison failed. Uncompared bytes in actual. File: ", compareTarget.fileName));
		for (const auto &[location, count] : foundDiffs) {
			StrAppend(message, "\nDiff found in ", location, " count: ", count);
		}
	}
	return { compareResult ? HeroCompareResult::Same : HeroCompareResult::Difference, message };
}
#endif // !DISABLE_DEMOMODE

void pfile_write_hero(SaveWriter &saveWriter, bool writeGameData)
{
	if (writeGameData) {
		SaveGameData(saveWriter);
		RenameTempToPerm(saveWriter);
	}
	PlayerPack pkplr;
	Player &myPlayer = *MyPlayer;

	PackPlayer(pkplr, myPlayer);
	EncodeHero(saveWriter, &pkplr);
	if (!gbVanilla) {
		SaveHotkeys(saveWriter, myPlayer);
		SaveHeroItems(saveWriter, myPlayer);
	}
}

void RemoveAllInvalidItems(Player &player)
{
	for (int i = 0; i < NUM_INVLOC; i++)
		RemoveInvalidItem(player.InvBody[i]);
	for (int i = 0; i < player._pNumInv; i++)
		RemoveInvalidItem(player.InvList[i]);
	for (int i = 0; i < MaxBeltItems; i++)
		RemoveInvalidItem(player.SpdList[i]);
	RemoveEmptyInventory(player);
}

} // namespace

#ifdef UNPACKED_SAVES
std::unique_ptr<std::byte[]> SaveReader::ReadFile(const char *filename, std::size_t &fileSize, int32_t &error)
{
	std::unique_ptr<std::byte[]> result;
	error = 0;
	const std::string path = dir_ + filename;
	uintmax_t size;
	if (!GetFileSize(path.c_str(), &size)) {
		error = 1;
		return nullptr;
	}
	fileSize = size;
	FILE *file = OpenFile(path.c_str(), "rb");
	if (file == nullptr) {
		error = 1;
		return nullptr;
	}
	result.reset(new std::byte[size]);
	if (std::fread(result.get(), size, 1, file) != 1) {
		std::fclose(file);
		error = 1;
		return nullptr;
	}
	std::fclose(file);
	return result;
}

bool SaveWriter::WriteFile(const char *filename, const std::byte *data, size_t size)
{
	const std::string path = dir_ + filename;
	FILE *file = OpenFile(path.c_str(), "wb");
	if (file == nullptr) {
		return false;
	}
	if (std::fwrite(data, size, 1, file) != 1) {
		std::fclose(file);
		return false;
	}
	std::fclose(file);
	return true;
}

void SaveWriter::RemoveHashEntries(bool (*fnGetName)(uint8_t, char *))
{
	char pszFileName[MaxMpqPathSize];

	for (uint8_t i = 0; fnGetName(i, pszFileName); i++) {
		RemoveHashEntry(pszFileName);
	}
}
#endif

std::optional<SaveReader> OpenSaveArchive(uint32_t saveNum)
{
	return CreateSaveReader(GetSavePath(saveNum));
}

std::optional<SaveReader> OpenStashArchive()
{
	return CreateSaveReader(GetStashSavePath());
}

std::unique_ptr<std::byte[]> ReadArchive(SaveReader &archive, const char *pszName, size_t *pdwLen)
{
	int32_t error;
	std::size_t length;

	std::unique_ptr<std::byte[]> result = archive.ReadFile(pszName, length, error);
	if (error != 0)
		return nullptr;

	const std::size_t decodedLength = codec_decode(result.get(), length, pfile_get_password());
	if (decodedLength == 0)
		return nullptr;

	if (pdwLen != nullptr)
		*pdwLen = decodedLength;

	return result;
}

const char *pfile_get_password()
{
	if (gbIsSpawn)
		return gbIsMultiplayer ? PASSWORD_SPAWN_MULTI : PASSWORD_SPAWN_SINGLE;
	return gbIsMultiplayer ? PASSWORD_MULTI : PASSWORD_SINGLE;
}

void pfile_write_hero(bool writeGameData)
{
	if (IsSurvivorsMode())
		return;
	{
		SaveWriter saveWriter = GetSaveWriter(gSaveNumber, /*carryForward=*/writeGameData);
		pfile_write_hero(saveWriter, writeGameData);
	}
	RequestBrowserSave();
}

#ifndef DISABLE_DEMOMODE
void pfile_write_hero_demo(int demo)
{
	const std::string savePath = GetSavePath(gSaveNumber, StrCat("demo_", demo, "_reference_"));
	CopySaveFile(gSaveNumber, savePath);
	auto saveWriter = SaveWriter(savePath.c_str());
	pfile_write_hero(saveWriter, true);
}

HeroCompareResult pfile_compare_hero_demo(int demo, bool logDetails)
{
	const std::string referenceSavePath = GetSavePath(gSaveNumber, StrCat("demo_", demo, "_reference_"));

	if (!FileExists(referenceSavePath.c_str()))
		return { HeroCompareResult::ReferenceNotFound, {} };

	const std::string actualSavePath = GetSavePath(gSaveNumber, StrCat("demo_", demo, "_actual_"));
	{
		CopySaveFile(gSaveNumber, actualSavePath);
		SaveWriter saveWriter(actualSavePath.c_str());
		pfile_write_hero(saveWriter, true);
	}

	return CompareSaves(actualSavePath, referenceSavePath, logDetails);
}
#endif

void sfile_write_stash()
{
	if (IsSurvivorsMode())
		return;
	if (!Stash.dirty)
		return;

	SaveWriter stashWriter = GetStashWriter();

	SaveStash(stashWriter);

	Stash.dirty = false;
}

bool pfile_ui_set_hero_infos(bool (*uiAddHeroInfo)(_uiheroinfo *))
{
	memset(hero_names, 0, sizeof(hero_names));

	for (uint32_t i = 0; i < MAX_CHARACTERS; i++) {
		std::optional<SaveReader> archive = OpenSaveArchive(i);
		if (archive) {
			PlayerPack pkplr;
			if (ReadHero(*archive, &pkplr)) {
				_uiheroinfo uihero;
				uihero.saveNumber = i;
				CopyUtf8(hero_names[i], std::string_view(pkplr.pName, PlayerNameLength), sizeof(hero_names[i]));
				const bool hasSaveGame = ArchiveContainsGame(*archive);
				if (hasSaveGame)
					pkplr.bIsHellfire = gbIsHellfireSaveGame ? 1 : 0;

				Player &player = Players[0];

				UnPackPlayer(pkplr, player);
				LoadHeroItems(player);
				RemoveAllInvalidItems(player);
				CalcPlrInv(player, false);

				Game2UiPlayer(player, &uihero, hasSaveGame);
				uiAddHeroInfo(&uihero);
			}
		}
	}

	return true;
}

void pfile_ui_set_class_stats(HeroClass playerClass, _uidefaultstats *classStats)
{
	const ClassAttributes &classAttributes = GetClassAttributes(playerClass);
	classStats->strength = classAttributes.baseStr;
	classStats->magic = classAttributes.baseMag;
	classStats->dexterity = classAttributes.baseDex;
	classStats->vitality = classAttributes.baseVit;
}

uint32_t pfile_ui_get_first_unused_save_num()
{
	uint32_t saveNum;
	for (saveNum = 0; saveNum < MAX_CHARACTERS; saveNum++) {
		if (hero_names[saveNum][0] == '\0')
			break;
	}
	return saveNum;
}

bool pfile_ui_save_create(_uiheroinfo *heroinfo)
{
	PlayerPack pkplr;

	const uint32_t saveNum = heroinfo->saveNumber;
	if (saveNum >= MAX_CHARACTERS)
		return false;
	heroinfo->saveNumber = saveNum;

	giNumberOfLevels = gbIsHellfire ? 25 : 17;

	Player &player = Players[0];
	{
		SaveWriter saveWriter = GetSaveWriter(saveNum, /*carryForward=*/false);
		saveWriter.RemoveHashEntries(GetFileName);
		CopyUtf8(hero_names[saveNum], heroinfo->name, sizeof(hero_names[saveNum]));

		CreatePlayer(player, heroinfo->heroclass);
		CopyUtf8(player._pName, heroinfo->name, PlayerNameLength);
		PackPlayer(pkplr, player);
		EncodeHero(saveWriter, &pkplr);
		Game2UiPlayer(player, heroinfo, false);
		if (!gbVanilla) {
			SaveHotkeys(saveWriter, player);
			SaveHeroItems(saveWriter, player);
		}
	}
	RequestBrowserSave();

	return true;
}

bool pfile_delete_save(_uiheroinfo *heroInfo)
{
	const uint32_t saveNum = heroInfo->saveNumber;
	if (saveNum < MAX_CHARACTERS) {
		hero_names[saveNum][0] = '\0';
		RemoveFile(GetSavePath(saveNum).c_str());
		RequestBrowserSave();
	}
	return true;
}

namespace {
bool ReadSurvivorsGear(const std::string &path, const Player &hero, PlayerPack &packed)
{
	auto archive = CreateSaveReader(std::string(path));
	if (!archive || !ReadHero(*archive, &packed))
		return false;
	const auto end = std::find(std::begin(packed.pName), std::end(packed.pName), '\0');
	const auto heroEnd = std::find(std::begin(hero._pName), std::end(hero._pName), '\0');
	if (packed.pClass != static_cast<uint8_t>(hero._pClass)
	    || end == std::end(packed.pName) || heroEnd == std::end(hero._pName)
	    || std::string_view(packed.pName, end - std::begin(packed.pName)) != std::string_view(hero._pName, heroEnd - std::begin(hero._pName)))
		return false;
	if (packed._pNumInv > InventoryGridCells || packed.pLevel == 0 || packed.pLevel > 50
	    || packed.bIsHellfire != static_cast<int>(gbIsHellfire))
		return false;
	for (const int cell : packed.InvGrid)
		if (cell < -static_cast<int>(packed._pNumInv) || cell > packed._pNumInv) return false;
	// Both entries are required. The second preserves fields not in PlayerPack.
	size_t itemBytes = 0;
	const auto items = ReadArchive(*archive, "heroitems", &itemBytes);
	const size_t itemCount = NUM_INVLOC + InventoryGridCells + MaxBeltItems;
	if (items == nullptr || itemBytes != 1 + itemCount * (gbIsHellfire ? 372 : 368)
	    || std::to_integer<unsigned>(items[0]) != static_cast<unsigned>(gbIsHellfire))
		return false;
	std::vector<Item> buybackItems;
	std::vector<int> buybackPrices;
	return LoadSurvivorsBuyback(CreateSaveReader(std::string(path)), buybackItems, buybackPrices);
}
}

bool pfile_survivors_inventory_available()
{
	return SurvivorsInventoryWritable && !SurvivorsInventoryPath.empty();
}

bool pfile_save_survivors_inventory()
{
#ifdef UNPACKED_SAVES
	return false;
#else
	if (!IsSurvivorsMode() || gbIsMultiplayer || MyPlayer == nullptr || !SurvivorsInventoryWritable
	    || SurvivorsInventoryPath.empty() || !MyPlayer->HoldItem.isEmpty())
		return false;
	const std::string temporary = SurvivorsInventoryPath + ".tmp";
	std::error_code error;
	std::filesystem::remove(temporary, error);
	{
		SaveWriter writer(std::string(temporary), false);
		PlayerPack packed;
		PackPlayer(packed, *MyPlayer);
		EncodeHero(writer, &packed);
		SaveHeroItems(writer, *MyPlayer);
		SaveSurvivorsBuyback(writer, GetSurvivorsBuybackItems(MyPlayerId), GetSurvivorsBuybackPrices(MyPlayerId));
	}
	PlayerPack verified;
	if (!ReadSurvivorsGear(temporary, *MyPlayer, verified))
		return false;
	const std::string backup = SurvivorsInventoryPath + ".bak";
	if (FileExists(SurvivorsInventoryPath.c_str())) {
		std::filesystem::remove(backup, error);
		error.clear();
		std::filesystem::rename(SurvivorsInventoryPath, backup, error);
		if (error) return false;
	}
	error.clear();
	std::filesystem::rename(temporary, SurvivorsInventoryPath, error);
	if (error) {
		std::error_code ignored;
		if (FileExists(backup.c_str())) std::filesystem::rename(backup, SurvivorsInventoryPath, ignored);
		return false;
	}
	RequestBrowserSave();
	return true;
#endif
}

namespace {
#ifdef PACKET_ENCRYPTION
using LedgerDigest = std::array<uint8_t, crypto_hash_sha256_BYTES>;

std::filesystem::path LedgerUtf8Path(std::string_view text)
{
	return std::filesystem::path(std::u8string_view(reinterpret_cast<const char8_t *>(text.data()), text.size()));
}

// Only an authority that has acquired the exclusive shared lease may change
// its ledger root. Client mirrors and per-hero identities remain local.
std::optional<std::filesystem::path> SharedLedgerRoot;
std::string SharedLedgerConfiguration;
#ifdef _WIN32
HANDLE SharedLedgerLease = INVALID_HANDLE_VALUE;
#else
int SharedLedgerLease = -1;
#endif

std::filesystem::path LedgerRoot()
{
	return SharedLedgerRoot ? *SharedLedgerRoot : LedgerUtf8Path(paths::PrefPath());
}

LedgerDigest LedgerHash(const uint8_t *bytes, size_t size)
{
	LedgerDigest digest {};
	crypto_hash_sha256(digest.data(), bytes, size);
	return digest;
}

std::string LedgerHex(const LedgerDigest &digest)
{
	constexpr char Hex[] = "0123456789abcdef";
	std::string result;
	result.reserve(digest.size() * 2);
	for (uint8_t value : digest) { result.push_back(Hex[value >> 4]); result.push_back(Hex[value & 15]); }
	return result;
}

std::filesystem::path LedgerPath(const SurvivorsIdentity &identity)
{
	return LedgerRoot() / ("survivors-coop-ledger-" + LedgerHex(LedgerHash(identity.data(), identity.size())) + ".sv");
}

std::filesystem::path LedgerPathForDigest(const LedgerDigest &digest)
{
	// The all-zero digest is reserved for the shared ground checkpoint. It is
	// included in the same journal as player inventories on drops and pickups.
	if (std::all_of(digest.begin(), digest.end(), [](uint8_t value) { return value == 0; }))
		return LedgerRoot() / "survivors-coop-ground.sv";
	return LedgerRoot() / ("survivors-coop-ledger-" + LedgerHex(digest) + ".sv");
}

std::filesystem::path LedgerJournalPath()
{
	return LedgerRoot() / "survivors-coop-checkpoint.journal";
}

bool HashLedgerFile(const std::filesystem::path &path, LedgerDigest &digest)
{
	std::ifstream file(path, std::ios::binary);
	if (!file) return false;
	crypto_hash_sha256_state state;
	crypto_hash_sha256_init(&state);
	std::array<char, 16384> buffer {};
	size_t size = 0;
	while (file) {
		file.read(buffer.data(), buffer.size());
		const auto count = static_cast<size_t>(file.gcount());
		size += count;
		if (size > 4 * 1024 * 1024) return false;
		crypto_hash_sha256_update(&state, reinterpret_cast<const uint8_t *>(buffer.data()), count);
	}
	if (!file.eof() || size == 0) return false;
	crypto_hash_sha256_final(&state, digest.data());
	return true;
}

std::filesystem::path IdentityPath(const Player &player)
{
	std::string key = std::to_string(static_cast<unsigned>(player._pClass));
	key += ':';
	const auto *end = std::find(std::begin(player._pName), std::end(player._pName), '\0');
	key.append(player._pName, end);
	return LedgerUtf8Path(paths::PrefPath()) / ("survivors-coop-identity-" + LedgerHex(LedgerHash(reinterpret_cast<const uint8_t *>(key.data()), key.size())) + ".dat");
}

bool FlushLedgerFile(const std::filesystem::path &path)
{
#ifdef _WIN32
	const HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
	if (file == INVALID_HANDLE_VALUE) return false;
	const bool ok = FlushFileBuffers(file) != 0;
	CloseHandle(file);
	return ok;
#else
	const int file = open(path.c_str(), O_RDONLY);
	if (file < 0) return false;
	int result;
	do { result = fsync(file); } while (result < 0 && errno == EINTR);
	close(file);
	return result == 0;
#endif
}

bool FlushLedgerDirectory(const std::filesystem::path &path)
{
#ifdef _WIN32
	// Win32 has no portable directory fsync. Replacements use WRITE_THROUGH,
	// but the configured shared store must provide durable directory metadata.
	(void)path;
	return true;
#else
	const int dir = open(path.parent_path().c_str(), O_RDONLY);
	if (dir < 0) return false;
	int result;
	do { result = fsync(dir); } while (result < 0 && errno == EINTR);
	close(dir);
	return result == 0;
#endif
}

bool ReplaceLedgerFile(const std::filesystem::path &temporary, const std::filesystem::path &path)
{
#ifdef _WIN32
	return MoveFileExW(temporary.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
#else
	return std::rename(temporary.c_str(), path.c_str()) == 0;
#endif
}

// Never overwrite an existing identity. A partial/corrupt identity fails closed.
bool CreateIdentityFile(const std::filesystem::path &path, std::span<const uint8_t> record)
{
#ifdef _WIN32
	const HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
	if (file == INVALID_HANDLE_VALUE) return false;
	DWORD written = 0;
	const bool ok = WriteFile(file, record.data(), static_cast<DWORD>(record.size()), &written, nullptr)
	    && written == record.size() && FlushFileBuffers(file);
	CloseHandle(file);
	return ok;
#else
	const int file = open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL, 0600);
	if (file < 0) return false;
	size_t offset = 0;
	while (offset < record.size()) {
		const ssize_t n = write(file, record.data() + offset, record.size() - offset);
		if (n < 0 && errno == EINTR) continue;
		if (n <= 0) { close(file); return false; }
		offset += static_cast<size_t>(n);
	}
	int result;
	do { result = fsync(file); } while (result < 0 && errno == EINTR);
	close(file);
	return result == 0 && FlushLedgerDirectory(path);
#endif
}

constexpr size_t LedgerMetadataSize = 8 + crypto_hash_sha256_BYTES + sizeof(uint64_t);
constexpr std::array<uint8_t, 8> LedgerMagic { 'D', 'X', 'L', 'E', 'D', 'G', '0', '1' };

bool ReadLedgerMetadata(const std::string &path, const SurvivorsIdentity &identity, uint64_t &committed)
{
	auto archive = CreateSaveReader(std::string(path));
	if (!archive) return false;
	size_t size = 0;
	const auto data = ReadArchive(*archive, "survivors-ledger", &size);
	if (!data || size != LedgerMetadataSize || std::memcmp(data.get(), LedgerMagic.data(), LedgerMagic.size()) != 0) return false;
	const auto digest = LedgerHash(identity.data(), identity.size());
	if (sodium_memcmp(data.get() + LedgerMagic.size(), digest.data(), digest.size()) != 0) return false;
	committed = 0;
	for (unsigned i = 0; i < 8; ++i) committed |= std::to_integer<uint64_t>(data[8 + digest.size() + i]) << (i * 8);
	return true;
}

bool WriteLedgerMetadata(SaveWriter &writer, const SurvivorsIdentity &identity, uint64_t committed)
{
	const auto digest = LedgerHash(identity.data(), identity.size());
	const size_t encodedSize = codec_get_encoded_len(LedgerMetadataSize);
	std::vector<std::byte> data(encodedSize);
	std::memcpy(data.data(), LedgerMagic.data(), LedgerMagic.size());
	std::memcpy(data.data() + 8, digest.data(), digest.size());
	for (unsigned i = 0; i < 8; ++i) data[8 + digest.size() + i] = static_cast<std::byte>((committed >> (i * 8)) & 0xff);
	codec_encode(data.data(), LedgerMetadataSize, encodedSize, pfile_get_password());
	return writer.WriteFile("survivors-ledger", data.data(), data.size());
}

#if !defined(UNPACKED_SAVES) && !defined(__EMSCRIPTEN__)
// Check the native MPQ header before opening an untrusted transfer: mpqfs
// allocates its hash and block tables using the counts in that header. Ledger
// archives produced by the native writer have their header at offset zero.
bool BoundedPortableLedgerHeader(std::span<const std::byte> bytes)
{
	if (bytes.size() < 32 || bytes[0] != std::byte { 'M' } || bytes[1] != std::byte { 'P' }
	    || bytes[2] != std::byte { 'Q' } || bytes[3] != std::byte { 0x1a }) return false;
	auto word = [&bytes](size_t offset) {
		return std::to_integer<uint32_t>(bytes[offset]) | (std::to_integer<uint32_t>(bytes[offset + 1]) << 8);
	};
	auto dword = [&word](size_t offset) { return word(offset) | (word(offset + 2) << 16); };
	if (dword(4) != 32 || word(12) != 0 || word(14) > 7) return false;
	auto tableFits = [&bytes, &dword](size_t offsetField, size_t countField) {
		const size_t offset = dword(offsetField);
		const size_t count = dword(countField);
		return count != 0 && offset <= bytes.size() && count <= (bytes.size() - offset) / 16;
	};
	return tableFits(16, 24) && tableFits(20, 28);
}

// Portable checkpoints arrive from peers. The archive's compressed size does
// not bound its members' declared sizes, and ReadFile allocates a member before
// its decoded format can be checked. Inspect every member used by the loader
// before any of those allocations. Buyback is optional for older ledgers.
bool BoundedPortableLedgerMembers(SaveReader &archive)
{
	const size_t itemSize = gbIsHellfire ? 372 : 368;
	const size_t itemCount = NUM_INVLOC + InventoryGridCells + MaxBeltItems;
	auto bounded = [&archive](const char *name, size_t maximum, bool required) {
		if (!archive.HasFile(name)) return !required;
		const size_t size = archive.GetFileSize(name);
		return size != 0 && size <= maximum;
	};
	return bounded("hero", codec_get_encoded_len(sizeof(PlayerPack)), true)
	    && bounded("heroitems", codec_get_encoded_len(1 + itemCount * itemSize), true)
	    && bounded("survivors-ledger", codec_get_encoded_len(LedgerMetadataSize), true)
	    && bounded("survivors-held", codec_get_encoded_len(2 + itemSize), true)
	    && bounded("survivors-buyback", codec_get_encoded_len(4 + SurvivorsBuybackLimit * (4 + itemSize)), false);
}
#endif

constexpr std::array<uint8_t, 8> LedgerJournalMagic { 'D', 'X', 'T', 'X', '0', '0', '0', '1' };
struct LedgerJournalItem { LedgerDigest identity; LedgerDigest contents; };
#ifdef BUILD_TESTING
unsigned LedgerTestInterruptAfter = 0;
#endif

bool ReadLedgerJournal(std::vector<LedgerJournalItem> &entries)
{
	std::ifstream file(LedgerJournalPath(), std::ios::binary);
	if (!file) return false;
	std::array<uint8_t, 8 + 4 + (MAX_PLRS + 1) * 64 + crypto_hash_sha256_BYTES + 1> bytes {};
	file.read(reinterpret_cast<char *>(bytes.data()), bytes.size());
	const size_t size = static_cast<size_t>(file.gcount());
	if (!file.eof() || size < 8 + 4 + crypto_hash_sha256_BYTES || std::memcmp(bytes.data(), LedgerJournalMagic.data(), 8) != 0) return false;
	const uint32_t count = static_cast<uint32_t>(bytes[8]) | (static_cast<uint32_t>(bytes[9]) << 8)
	    | (static_cast<uint32_t>(bytes[10]) << 16) | (static_cast<uint32_t>(bytes[11]) << 24);
	if (count == 0 || count > MAX_PLRS + 1 || size != 12 + count * 64 + crypto_hash_sha256_BYTES) return false;
	const auto checksum = LedgerHash(bytes.data(), size - crypto_hash_sha256_BYTES);
	if (sodium_memcmp(checksum.data(), bytes.data() + size - checksum.size(), checksum.size()) != 0) return false;
	entries.clear();
	for (unsigned i = 0; i < count; ++i) {
		LedgerJournalItem entry {};
		std::copy_n(bytes.begin() + 12 + i * 64, 32, entry.identity.begin());
		std::copy_n(bytes.begin() + 12 + i * 64 + 32, 32, entry.contents.begin());
		if (std::any_of(entries.begin(), entries.end(), [&](const LedgerJournalItem &old) { return old.identity == entry.identity; })) return false;
		entries.push_back(entry);
	}
	return true;
}

bool ReplayLedgerJournal(bool allowTestInterrupt)
{
	const auto journal = LedgerJournalPath();
	std::error_code error;
	if (!std::filesystem::exists(journal, error)) return !error;
	std::vector<LedgerJournalItem> entries;
	if (!ReadLedgerJournal(entries)) return false;
	unsigned replaced = 0;
	for (const auto &entry : entries) {
		const auto destination = LedgerPathForDigest(entry.identity);
		auto stage = destination;
		stage += ".stage";
		LedgerDigest actual {};
		if (std::filesystem::exists(stage, error)) {
			if (error || !HashLedgerFile(stage, actual) || sodium_memcmp(actual.data(), entry.contents.data(), actual.size()) != 0
			    || !ReplaceLedgerFile(stage, destination)) return false;
		} else {
			if (error || !HashLedgerFile(destination, actual) || sodium_memcmp(actual.data(), entry.contents.data(), actual.size()) != 0) return false;
		}
		++replaced;
#ifdef BUILD_TESTING
		if (allowTestInterrupt && LedgerTestInterruptAfter != 0 && replaced >= LedgerTestInterruptAfter) {
			LedgerTestInterruptAfter = 0;
			return false;
		}
#else
		(void)allowTestInterrupt;
#endif
	}
	if (!FlushLedgerDirectory(journal)) return false;
	if (!std::filesystem::remove(journal, error) || error) return false;
	return FlushLedgerDirectory(journal);
}
#endif
} // namespace

bool pfile_begin_survivors_shared_authority()
{
#if !defined(PACKET_ENCRYPTION) || defined(UNPACKED_SAVES) || defined(__EMSCRIPTEN__)
	return false;
#else
	const char *configured = std::getenv("DAMNATION_SURVIVORS_SHARED_DIR");
	if (configured == nullptr || *configured == '\0') return !SharedLedgerRoot.has_value();
	if (SharedLedgerRoot) return SharedLedgerConfiguration == configured;
	const auto requested = LedgerUtf8Path(configured);
	if (!requested.is_absolute()) return false;
	std::error_code error;
	const auto root = std::filesystem::canonical(requested, error);
	if (error || !std::filesystem::is_directory(root, error) || error) return false;
	const auto path = root / "survivors-coop-authority.lock";
#ifdef _WIN32
	const HANDLE lease = CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
	    nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
	if (lease == INVALID_HANDLE_VALUE) return false;
	OVERLAPPED overlap {};
	if (!LockFileEx(lease, LOCKFILE_EXCLUSIVE_LOCK | LOCKFILE_FAIL_IMMEDIATELY, 0, 1, 0, &overlap)
	    || !FlushFileBuffers(lease) || !FlushLedgerDirectory(path)) {
		UnlockFileEx(lease, 0, 1, 0, &overlap);
		CloseHandle(lease);
		return false;
	}
	SharedLedgerLease = lease;
#else
	const int lease = open(path.c_str(), O_RDWR | O_CREAT | O_NOFOLLOW | O_CLOEXEC, 0600);
	if (lease < 0) return false;
	if (flock(lease, LOCK_EX | LOCK_NB) != 0 || fsync(lease) != 0 || !FlushLedgerDirectory(path)) {
		flock(lease, LOCK_UN);
		close(lease);
		return false;
	}
	SharedLedgerLease = lease;
#endif
	SharedLedgerRoot = root;
	SharedLedgerConfiguration = configured;
	return true;
#endif
}

void pfile_end_survivors_shared_authority()
{
#if defined(PACKET_ENCRYPTION) && !defined(UNPACKED_SAVES) && !defined(__EMSCRIPTEN__)
	if (!SharedLedgerRoot) return;
#ifdef _WIN32
	OVERLAPPED overlap {};
	UnlockFileEx(SharedLedgerLease, 0, 1, 0, &overlap);
	CloseHandle(SharedLedgerLease);
	SharedLedgerLease = INVALID_HANDLE_VALUE;
#else
	flock(SharedLedgerLease, LOCK_UN);
	close(SharedLedgerLease);
	SharedLedgerLease = -1;
#endif
	SharedLedgerRoot.reset();
	SharedLedgerConfiguration.clear();
#endif
}

std::optional<SurvivorsIdentity> pfile_survivors_identity(const Player &player)
{
#if !defined(PACKET_ENCRYPTION) || defined(UNPACKED_SAVES) || defined(__EMSCRIPTEN__)
	(void)player;
	return std::nullopt;
#else
	if (sodium_init() < 0 || player._pName[0] == '\0') return std::nullopt;
	const auto path = IdentityPath(player);
	constexpr size_t prefix = 8 + 1 + PlayerNameLength;
	std::array<uint8_t, prefix + 32 + crypto_hash_sha256_BYTES> record {};
	std::ifstream existing(path, std::ios::binary);
	if (existing) {
		existing.read(reinterpret_cast<char *>(record.data()), record.size());
		if (existing.gcount() != static_cast<std::streamsize>(record.size()) || existing.peek() != std::char_traits<char>::eof()) return std::nullopt;
	} else {
		std::error_code error;
		if (std::filesystem::exists(path, error) || error) return std::nullopt;
		std::memcpy(record.data(), "DXID0001", 8);
		record[8] = static_cast<uint8_t>(player._pClass);
		std::strncpy(reinterpret_cast<char *>(record.data() + 9), player._pName, PlayerNameLength);
		randombytes_buf(record.data() + prefix, 32);
		const auto checksum = LedgerHash(record.data(), prefix + 32);
		std::copy(checksum.begin(), checksum.end(), record.begin() + prefix + 32);
		if (!CreateIdentityFile(path, record)) return std::nullopt;
	}
	const auto checksum = LedgerHash(record.data(), prefix + 32);
	if (std::memcmp(record.data(), "DXID0001", 8) != 0 || record[8] != static_cast<uint8_t>(player._pClass)
	    || std::strncmp(reinterpret_cast<const char *>(record.data() + 9), player._pName, PlayerNameLength) != 0
	    || sodium_memcmp(checksum.data(), record.data() + prefix + 32, checksum.size()) != 0
	    || !FlushLedgerFile(path) || !FlushLedgerDirectory(path)) return std::nullopt;
	SurvivorsIdentity identity {};
	std::copy_n(record.begin() + prefix, identity.size(), identity.begin());
	return identity;
#endif
}

SurvivorsLedgerLoad pfile_load_survivors_ledger(Player &player, const SurvivorsIdentity &identity, uint64_t &committed)
{
#if !defined(PACKET_ENCRYPTION) || defined(UNPACKED_SAVES) || defined(__EMSCRIPTEN__)
	(void)player; (void)identity; (void)committed;
	return SurvivorsLedgerLoad::Corrupt;
#else
	if (sodium_init() < 0 || sodium_is_zero(identity.data(), identity.size()) || !pfile_recover_survivors_ledger_journal()) return SurvivorsLedgerLoad::Corrupt;
	const auto path = LedgerPath(identity);
	std::error_code error;
	if (!std::filesystem::exists(path, error)) return error ? SurvivorsLedgerLoad::Corrupt : SurvivorsLedgerLoad::Missing;
	const std::string source = path.string();
	PlayerPack packed;
	std::vector<Item> items;
	std::vector<int> prices;
	Item held;
	uint64_t sequence = 0;
	if (!ReadSurvivorsGear(source, player, packed) || !ReadLedgerMetadata(source, identity, sequence)
	    || !LoadSurvivorsBuyback(CreateSaveReader(std::string(source)), items, prices)
	    || !LoadSurvivorsLedgerHeldItem(CreateSaveReader(std::string(source)), held)) return SurvivorsLedgerLoad::Corrupt;
	Player stored;
	UnPackPlayer(packed, stored);
	const auto oldOverride = SavePathOverride;
	SavePathOverride = source;
	LoadHeroItems(stored);
	SavePathOverride = oldOverride;
	RemoveAllInvalidItems(stored);
	if (!SetSurvivorsBuyback(player.getId(), items, prices)) return SurvivorsLedgerLoad::Corrupt;
	std::copy(std::begin(stored.InvBody), std::end(stored.InvBody), std::begin(player.InvBody));
	std::copy(std::begin(stored.InvList), std::end(stored.InvList), std::begin(player.InvList));
	std::copy(std::begin(stored.InvGrid), std::end(stored.InvGrid), std::begin(player.InvGrid));
	std::copy(std::begin(stored.SpdList), std::end(stored.SpdList), std::begin(player.SpdList));
	player._pNumInv = stored._pNumInv;
	player._pGold = stored._pGold;
	player.HoldItem = held;
	CalcPlrInv(player, false);
	committed = sequence;
	return SurvivorsLedgerLoad::Loaded;
#endif
}

bool pfile_recover_survivors_ledger_journal()
{
#if !defined(PACKET_ENCRYPTION) || defined(UNPACKED_SAVES) || defined(__EMSCRIPTEN__)
	return false;
#else
	return sodium_init() >= 0 && ReplayLedgerJournal(false);
#endif
}

#ifdef BUILD_TESTING
void pfile_test_survivors_ledger_interrupt_after(unsigned replacements)
{
#if defined(PACKET_ENCRYPTION) && !defined(UNPACKED_SAVES) && !defined(__EMSCRIPTEN__)
	LedgerTestInterruptAfter = replacements;
#else
	(void)replacements;
#endif
}
#endif

SurvivorsLedgerLoad pfile_load_survivors_ground(std::vector<Item> &items)
{
#if !defined(PACKET_ENCRYPTION) || defined(UNPACKED_SAVES) || defined(__EMSCRIPTEN__)
	(void)items;
	return SurvivorsLedgerLoad::Corrupt;
#else
	items.clear();
	if (sodium_init() < 0 || !pfile_recover_survivors_ledger_journal()) return SurvivorsLedgerLoad::Corrupt;
	const auto path = LedgerPathForDigest({});
	std::error_code error;
	if (!std::filesystem::exists(path, error)) return error ? SurvivorsLedgerLoad::Corrupt : SurvivorsLedgerLoad::Missing;
	return LoadSurvivorsGroundItems(CreateSaveReader(path.string()), items) ? SurvivorsLedgerLoad::Loaded : SurvivorsLedgerLoad::Corrupt;
#endif
}

bool pfile_commit_survivors_ledgers(std::span<const SurvivorsLedgerEntry> entries, std::optional<std::span<const Item>> ground)
{
#if !defined(PACKET_ENCRYPTION) || defined(UNPACKED_SAVES) || defined(__EMSCRIPTEN__)
	(void)entries; (void)ground;
	return false;
#else
	if (!IsSurvivorsMode() || !gbIsMultiplayer || sodium_init() < 0 || (entries.empty() && !ground) || entries.size() > MAX_PLRS
	    || !pfile_recover_survivors_ledger_journal()) return false;
	std::vector<LedgerJournalItem> journalEntries;
	journalEntries.reserve(entries.size() + (ground ? 1 : 0));
	std::vector<Player *> seenPlayers;
	std::error_code error;
	for (const auto &entry : entries) {
		if (entry.player == nullptr || sodium_is_zero(entry.identity.data(), entry.identity.size())) return false;
		if (std::none_of(Players.begin(), Players.end(), [&](Player &player) { return &player == entry.player; })
		    || std::find(seenPlayers.begin(), seenPlayers.end(), entry.player) != seenPlayers.end()) return false;
		seenPlayers.push_back(entry.player);
		Player &player = *entry.player;
		const auto digest = LedgerHash(entry.identity.data(), entry.identity.size());
		if (sodium_is_zero(digest.data(), digest.size())) return false;
		if (std::any_of(journalEntries.begin(), journalEntries.end(), [&](const LedgerJournalItem &old) { return old.identity == digest; })) return false;
		const auto path = LedgerPathForDigest(digest);
		// Never overwrite a corrupt or newer authority record. An uncertain
		// write must be reconciled before a stale state can be published.
		if (std::filesystem::exists(path, error)) {
			const std::string current = path.string();
			PlayerPack previous;
			uint64_t sequence = 0;
			Item held;
			if (!ReadSurvivorsGear(current, player, previous) || !ReadLedgerMetadata(current, entry.identity, sequence)
			    || sequence > entry.committed || !LoadSurvivorsLedgerHeldItem(CreateSaveReader(std::string(current)), held)) return false;
		} else if (error) {
			return false;
		}
		auto stage = path;
		stage += ".stage";
		std::filesystem::remove(stage, error);
		if (error) return false;
		{
			SaveWriter writer(stage.string(), false);
			PlayerPack packed;
			PackPlayer(packed, player);
			EncodeHero(writer, &packed);
			SaveHeroItems(writer, player);
			SaveSurvivorsBuyback(writer, GetSurvivorsBuybackItems(player.getId()), GetSurvivorsBuybackPrices(player.getId()));
			SaveSurvivorsLedgerHeldItem(writer, player.HoldItem);
			if (!WriteLedgerMetadata(writer, entry.identity, entry.committed)) return false;
		}
		const std::string source = stage.string();
		PlayerPack packed;
		uint64_t sequence = 0;
		Item held;
		LedgerJournalItem saved { digest, {} };
		if (!ReadSurvivorsGear(source, player, packed) || !ReadLedgerMetadata(source, entry.identity, sequence) || sequence != entry.committed
		    || !LoadSurvivorsLedgerHeldItem(CreateSaveReader(std::string(source)), held) || !FlushLedgerFile(stage)
		    || !HashLedgerFile(stage, saved.contents)) return false;
		journalEntries.push_back(saved);
	}
	if (ground) {
		if (ground->size() > 4096 || std::any_of(ground->begin(), ground->end(), [](const Item &item) { return item.isEmpty(); })) return false;
		const LedgerDigest digest {};
		const auto path = LedgerPathForDigest(digest);
		// A damaged or ambiguous committed checkpoint must not be silently
		// replaced by a snapshot that might already have lost an item.
		if (std::filesystem::exists(path, error)) {
			std::vector<Item> previous;
			if (!LoadSurvivorsGroundItems(CreateSaveReader(path.string()), previous)) return false;
		} else if (error) return false;
		auto stage = path;
		stage += ".stage";
		std::filesystem::remove(stage, error);
		if (error) return false;
		{
			SaveWriter writer(stage.string(), false);
			SaveSurvivorsGroundItems(writer, *ground);
		}
		std::vector<Item> validated;
		LedgerJournalItem saved { digest, {} };
		if (!LoadSurvivorsGroundItems(CreateSaveReader(stage.string()), validated) || validated.size() != ground->size()
		    || !FlushLedgerFile(stage) || !HashLedgerFile(stage, saved.contents)) return false;
		journalEntries.push_back(saved);
	}
	const auto journal = LedgerJournalPath();
	if (!FlushLedgerDirectory(journal)) return false;
	std::vector<uint8_t> manifest(12 + journalEntries.size() * 64 + crypto_hash_sha256_BYTES);
	std::copy(LedgerJournalMagic.begin(), LedgerJournalMagic.end(), manifest.begin());
	const auto count = static_cast<uint32_t>(journalEntries.size());
	for (unsigned i = 0; i < 4; ++i) manifest[8 + i] = (count >> (i * 8)) & 0xff;
	for (size_t i = 0; i < journalEntries.size(); ++i) {
		std::copy(journalEntries[i].identity.begin(), journalEntries[i].identity.end(), manifest.begin() + 12 + i * 64);
		std::copy(journalEntries[i].contents.begin(), journalEntries[i].contents.end(), manifest.begin() + 12 + i * 64 + 32);
	}
	const auto checksum = LedgerHash(manifest.data(), manifest.size() - crypto_hash_sha256_BYTES);
	std::copy(checksum.begin(), checksum.end(), manifest.end() - checksum.size());
	auto temporary = journal;
	temporary += ".tmp";
	std::filesystem::remove(temporary, error);
	if (error || !CreateIdentityFile(temporary, manifest) || !ReplaceLedgerFile(temporary, journal)
	    || !FlushLedgerDirectory(journal) || !ReplayLedgerJournal(true)) return false;
	RequestBrowserSave();
	return true;
#endif
}

bool pfile_save_survivors_ledger(Player &player, const SurvivorsIdentity &identity, uint64_t committed)
{
	const SurvivorsLedgerEntry entry { &player, identity, committed };
	return pfile_commit_survivors_ledgers(std::span<const SurvivorsLedgerEntry>(&entry, 1));
}

std::optional<std::vector<std::byte>> pfile_export_survivors_ledger(const SurvivorsIdentity &identity)
{
#if !defined(PACKET_ENCRYPTION) || defined(UNPACKED_SAVES) || defined(__EMSCRIPTEN__)
	(void)identity;
	return std::nullopt;
#else
	if (sodium_init() < 0 || sodium_is_zero(identity.data(), identity.size()) || !pfile_recover_survivors_ledger_journal()) return std::nullopt;
	const auto path = LedgerPath(identity);
	std::error_code error;
	if (!std::filesystem::exists(path, error)) return error ? std::nullopt : std::optional<std::vector<std::byte>>(std::in_place);
	const auto size = std::filesystem::file_size(path, error);
	uint64_t revision = 0;
	if (error || size == 0 || size > 1024 * 1024 || !ReadLedgerMetadata(path.string(), identity, revision)) return std::nullopt;
	std::vector<std::byte> bytes(size);
	std::ifstream file(path, std::ios::binary);
	if (!file.read(reinterpret_cast<char *>(bytes.data()), size) || file.peek() != std::char_traits<char>::eof()) return std::nullopt;
	return bytes;
#endif
}

#ifdef BUILD_TESTING
bool pfile_test_survivors_portable_header(std::span<const std::byte> bytes)
{
#if !defined(PACKET_ENCRYPTION) || defined(UNPACKED_SAVES) || defined(__EMSCRIPTEN__)
	(void)bytes;
	return false;
#else
	return BoundedPortableLedgerHeader(bytes);
#endif
}

bool pfile_test_survivors_portable_members(const std::string &path)
{
#if !defined(PACKET_ENCRYPTION) || defined(UNPACKED_SAVES) || defined(__EMSCRIPTEN__)
	(void)path;
	return false;
#else
	auto archive = CreateSaveReader(std::string(path));
	return archive && BoundedPortableLedgerMembers(*archive);
#endif
}
#endif

SurvivorsLedgerImport pfile_import_survivors_ledger(const Player &player, const SurvivorsIdentity &identity, std::span<const std::byte> bytes, uint64_t expectedRevision)
{
#if !defined(PACKET_ENCRYPTION) || defined(UNPACKED_SAVES) || defined(__EMSCRIPTEN__)
	(void)player; (void)identity; (void)bytes; (void)expectedRevision;
	return SurvivorsLedgerImport::Invalid;
#else
	if (bytes.empty() || bytes.size() > 1024 * 1024 || sodium_init() < 0 || sodium_is_zero(identity.data(), identity.size())
	    || !pfile_recover_survivors_ledger_journal()) return SurvivorsLedgerImport::Invalid;
	if (!BoundedPortableLedgerHeader(bytes)) return SurvivorsLedgerImport::Invalid;
	const auto path = LedgerPath(identity);
	auto temporary = path;
	temporary += ".incoming";
	std::error_code error;
	std::filesystem::remove(temporary, error);
	if (error) return SurvivorsLedgerImport::Invalid;
	{
		std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
		if (!output.write(reinterpret_cast<const char *>(bytes.data()), bytes.size()) || !output.flush()) return SurvivorsLedgerImport::Invalid;
	}
	const auto source = temporary.string();
	auto archive = CreateSaveReader(std::string(source));
	if (!archive || !BoundedPortableLedgerMembers(*archive)) {
		archive.reset();
		std::filesystem::remove(temporary, error);
		return SurvivorsLedgerImport::Invalid;
	}
	archive.reset();
	PlayerPack packed;
	Item held;
	uint64_t incoming = 0;
	if (!ReadSurvivorsGear(source, player, packed) || !ReadLedgerMetadata(source, identity, incoming)
	    || (expectedRevision != 0 && incoming != expectedRevision)
	    || !LoadSurvivorsLedgerHeldItem(CreateSaveReader(std::string(source)), held) || !FlushLedgerFile(temporary)) {
		std::filesystem::remove(temporary, error);
		return SurvivorsLedgerImport::Invalid;
	}
	if (std::filesystem::exists(path, error)) {
		uint64_t current = 0;
		PlayerPack previous;
		if (!ReadLedgerMetadata(path.string(), identity, current) || !ReadSurvivorsGear(path.string(), player, previous)) return SurvivorsLedgerImport::Invalid;
		if (current >= incoming) {
			LedgerDigest oldHash {}, newHash {};
			const bool same = current == incoming && HashLedgerFile(path, oldHash) && HashLedgerFile(temporary, newHash) && oldHash == newHash;
			std::filesystem::remove(temporary, error);
			if (current > incoming) return SurvivorsLedgerImport::Older;
			return same ? SurvivorsLedgerImport::Unchanged : SurvivorsLedgerImport::Conflict;
		}
	} else if (error) return SurvivorsLedgerImport::Invalid;
	if (!ReplaceLedgerFile(temporary, path) || !FlushLedgerDirectory(path)) return SurvivorsLedgerImport::Invalid;
	return SurvivorsLedgerImport::Imported;
#endif
}

void pfile_load_survivors_inventory(Player &player)
{
		SetSurvivorsBuyback(player.getId(), {}, {});
		SurvivorsInventoryPath.clear();
		SurvivorsInventoryWritable = false;
#ifndef UNPACKED_SAVES
		if (!gbIsMultiplayer) {
			SurvivorsInventoryPath = SurvivorsInventoryFile(player);
			PlayerPack packed;
			std::string source = SurvivorsInventoryPath;
			bool valid = ReadSurvivorsGear(source, player, packed);
			if (!valid) {
				source += ".bak";
				valid = ReadSurvivorsGear(source, player, packed);
			}
			if (valid) {
				std::vector<Item> buybackItems;
				std::vector<int> buybackPrices;
				if (!LoadSurvivorsBuyback(CreateSaveReader(std::string(source)), buybackItems, buybackPrices)
				    || !SetSurvivorsBuyback(player.getId(), buybackItems, buybackPrices))
					return;
				Player stored;
				UnPackPlayer(packed, stored);
				const auto oldOverride = SavePathOverride;
				SavePathOverride = source;
				LoadHeroItems(stored);
				SavePathOverride = oldOverride;
				RemoveAllInvalidItems(stored);
				std::copy(std::begin(stored.InvBody), std::end(stored.InvBody), std::begin(player.InvBody));
				std::copy(std::begin(stored.InvList), std::end(stored.InvList), std::begin(player.InvList));
				std::copy(std::begin(stored.InvGrid), std::end(stored.InvGrid), std::begin(player.InvGrid));
				std::copy(std::begin(stored.SpdList), std::end(stored.SpdList), std::begin(player.SpdList));
				player._pNumInv = stored._pNumInv;
				player._pGold = stored._pGold;
				CalcPlrInv(player, false);
			}
			// Preserve an unreadable archive instead of overwriting it.
			SurvivorsInventoryWritable = (valid && source == SurvivorsInventoryPath)
			    || (!FileExists(SurvivorsInventoryPath.c_str()) && !FileExists(source.c_str()));
		}
#endif
	}

void pfile_read_player_from_save(uint32_t saveNum, Player &player)
{
	// The Survivors class selector creates this hero in memory.
	if (IsSurvivorsMode()) {
		mainmenu_initialize_survivors_hero(player);
		pfile_load_survivors_inventory(player);
		gbValidSaveFile = false;
		return;
	}
	PlayerPack pkplr;
	{
		std::optional<SaveReader> archive = OpenSaveArchive(saveNum);
		if (!archive)
			app_fatal(_("Unable to open archive"));
		if (!ReadHero(*archive, &pkplr))
			app_fatal(_("Unable to load character"));

		gbValidSaveFile = ArchiveContainsGame(*archive);
		if (gbValidSaveFile)
			pkplr.bIsHellfire = gbIsHellfireSaveGame ? 1 : 0;
	}

	UnPackPlayer(pkplr, player);
	LoadHeroItems(player);
	RemoveAllInvalidItems(player);
	CalcPlrInv(player, false);
}

void pfile_save_level()
{
	if (IsSurvivorsMode())
		return;
	SaveWriter saveWriter = GetSaveWriter(gSaveNumber);
	SaveLevel(saveWriter);
}

std::expected<void, std::string> pfile_convert_levels()
{
	SaveWriter saveWriter = GetSaveWriter(gSaveNumber);
	return ConvertLevels(saveWriter);
}

void pfile_remove_temp_files()
{
	if (IsSurvivorsMode())
		return;
	if (gbIsMultiplayer)
		return;

	SaveWriter saveWriter = GetSaveWriter(gSaveNumber, /*carryForward=*/true);
	saveWriter.RemoveHashEntries(GetTempSaveNames);
}

void pfile_update(bool forceSave)
{
	static Uint32 prevTick;

	if (!gbIsMultiplayer)
		return;

	const Uint32 tick = SDL_GetTicks();
	if (!forceSave && tick - prevTick <= 60000)
		return;

	prevTick = tick;
	pfile_write_hero();
	sfile_write_stash();
}

/** Embedded hosts (Source/host/): the hero's save archive. */
std::string pfile_get_save_path()
{
	return GetSavePath(gSaveNumber);
}

/** Embedded hosts: reads and writes of the hero's save archive go to `path` until called with nullptr. */
void pfile_set_save_path_override(const std::string *path)
{
	if (path != nullptr)
		SavePathOverride = *path;
	else
		SavePathOverride.reset();
}

/** Embedded hosts: writes the whole game (hero, current and visited levels) into a separate archive at `path`. */
bool pfile_write_game_snapshot(const std::string &path)
{
#ifdef UNPACKED_SAVES
	(void)path;
	return false;
#else
	CopyFileOverwrite(GetSavePath(gSaveNumber).c_str(), path.c_str());
	SaveWriter saveWriter(path.c_str());
	pfile_write_hero(saveWriter, true);
	return true;
#endif
}

} // namespace devilution
