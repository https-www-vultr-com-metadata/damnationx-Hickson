#include "survivors_progression.h"

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif
#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#endif

#include "survivors_arsenal.h"
#include "utils/paths.h"

namespace devilution {
namespace {
constexpr uint32_t CounterLimit = 100000000;
SurvivorsProfile Profile;
std::string LoadedPath;
std::string Status;
bool Dirty = false;
bool ReadOnlyProfile = false;

std::filesystem::path Utf8Path(std::string_view value)
{
	return std::filesystem::path(std::u8string_view(reinterpret_cast<const char8_t *>(value.data()), value.size()));
}
std::filesystem::path SuffixedPath(const std::filesystem::path &path, const char *suffix)
{
	auto result = path;
	result += suffix;
	return result;
}
std::string ProfilePath()
{
	const auto text = (Utf8Path(paths::PrefPath()) / "survivors-profile.dat").u8string();
	return std::string(reinterpret_cast<const char *>(text.data()), text.size());
}
bool FlushToDisk(const std::filesystem::path &path)
{
#ifdef _WIN32
	const HANDLE handle = CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
	if (handle == INVALID_HANDLE_VALUE)
		return false;
	const bool success = FlushFileBuffers(handle) != 0;
	CloseHandle(handle);
	return success;
#else
	const int handle = open(path.c_str(), O_RDONLY);
	if (handle < 0)
		return false;
	int result;
	do {
		result = fsync(handle);
	} while (result < 0 && errno == EINTR);
	close(handle);
	return result == 0;
#endif
}
uint32_t Checksum(std::string_view text)
{
	uint32_t hash = 2166136261U;
	for (const unsigned char value : text)
		hash = (hash ^ value) * 16777619U;
	return hash;
}
uint32_t AddBounded(uint32_t value, uint64_t amount, uint32_t limit = CounterLimit)
{
	return static_cast<uint32_t>(std::min<uint64_t>(static_cast<uint64_t>(value) + amount, limit));
}
std::optional<SurvivorsProfile> ReadProfile(const std::filesystem::path &path)
{
	std::ifstream file(path, std::ios::binary);
	if (!file)
		return std::nullopt;
	std::string data(4097, '\0');
	file.read(data.data(), static_cast<std::streamsize>(data.size()));
	data.resize(static_cast<size_t>(file.gcount()));
	if (data.size() > 4096)
		return std::nullopt;
	return ParseSurvivorsProfile(data);
}
bool AtomicReplace(const std::filesystem::path &source, const std::filesystem::path &destination)
{
#ifdef _WIN32
	return MoveFileExW(source.c_str(), destination.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
#else
	return std::rename(source.c_str(), destination.c_str()) == 0;
#endif
}
bool WriteAtomic(const std::filesystem::path &path, std::string_view content)
{
	const auto temporary = SuffixedPath(path, ".tmp");
	{
		std::ofstream stream(temporary, std::ios::binary | std::ios::trunc);
		if (!stream)
			return false;
		stream.write(content.data(), static_cast<std::streamsize>(content.size()));
		stream.flush();
		if (!stream)
			return false;
		stream.close();
		if (!stream)
			return false;
	}
	return FlushToDisk(temporary) && AtomicReplace(temporary, path);
}
} // namespace

bool IsValidSurvivorsProfile(const SurvivorsProfile &p)
{
	return p.soulShards <= SurvivorsMaxShards && p.totalKills <= CounterLimit && p.runs <= CounterLimit
	    && p.clearedActs <= CounterLimit && p.slainBosses <= CounterLimit && p.victories <= p.runs
	    && p.bestSeconds <= 86400 && p.bestLevel <= 1000 && p.unlockedDifficulty <= 2
	    && p.selectedDifficulty <= p.unlockedDifficulty
	    && std::all_of(p.ranks.begin(), p.ranks.end(), [](uint32_t rank) { return rank <= SurvivorsMaxPermanentRank; });
}

std::string SerializeSurvivorsProfile(const SurvivorsProfile &p)
{
	if (!IsValidSurvivorsProfile(p))
		return { };
	std::ostringstream stream;
	stream << "DamnationXSurvivors 2\n"
	       << p.soulShards << ' ' << p.totalKills << ' ' << p.runs << ' '
	       << p.clearedActs << ' ' << p.slainBosses << ' ' << p.victories << ' ' << p.bestSeconds << ' '
	       << p.bestLevel << ' ' << p.unlockedDifficulty << ' ' << p.selectedDifficulty;
	for (const auto rank : p.ranks)
		stream << ' ' << rank;
	stream << ' ' << static_cast<unsigned>(p.autoMelee);
	stream << '\n';
	const std::string payload = stream.str();
	return payload + "checksum " + std::to_string(Checksum(payload)) + "\n";
}

std::optional<SurvivorsProfile> ParseSurvivorsProfile(std::string_view data)
{
	if (data.size() > 4096)
		return std::nullopt;
	const auto checksumPosition = data.find("checksum ");
	if (checksumPosition == std::string_view::npos)
		return std::nullopt;
	const auto payload = data.substr(0, checksumPosition);
	std::istringstream checksumStream { std::string(data.substr(checksumPosition + 9)) };
	uint64_t storedChecksum;
	if (!(checksumStream >> storedChecksum) || storedChecksum != Checksum(payload))
		return std::nullopt;
	checksumStream >> std::ws;
	if (!checksumStream.eof())
		return std::nullopt;
	std::istringstream stream { std::string(payload) };
	std::string magic;
	unsigned version;
	SurvivorsProfile p;
	if (!(stream >> magic >> version) || magic != "DamnationXSurvivors" || (version != 1 && version != 2)
	    || !(stream >> p.soulShards >> p.totalKills >> p.runs >> p.clearedActs >> p.slainBosses >> p.victories
	        >> p.bestSeconds >> p.bestLevel >> p.unlockedDifficulty >> p.selectedDifficulty))
		return std::nullopt;
	for (auto &rank : p.ranks)
		if (!(stream >> rank))
			return std::nullopt;
	if (version == 2) {
		unsigned autoMelee;
		if (!(stream >> autoMelee) || autoMelee > 1)
			return std::nullopt;
		p.autoMelee = autoMelee != 0;
	}
	stream >> std::ws;
	if (!stream.eof() || !IsValidSurvivorsProfile(p))
		return std::nullopt;
	return p;
}

bool LoadSurvivorsProfileFromPath(std::string_view path, SurvivorsProfile &result)
{
	const auto primary = Utf8Path(path);
	if (const auto loaded = ReadProfile(primary)) {
		result = *loaded;
		return true;
	}
	if (const auto backup = ReadProfile(SuffixedPath(primary, ".bak"))) {
		result = *backup;
		return true;
	}
	return false;
}

bool SaveSurvivorsProfileToPath(std::string_view path, const SurvivorsProfile &profile)
{
	const std::string content = SerializeSurvivorsProfile(profile);
	if (content.empty())
		return false;
	const auto primary = Utf8Path(path);
	std::error_code error;
	if (!primary.parent_path().empty())
		std::filesystem::create_directories(primary.parent_path(), error);
	if (error)
		return false;
	// Never replace a valid backup with a malformed/truncated primary.
	if (const auto previous = ReadProfile(primary)) {
		if (!WriteAtomic(SuffixedPath(primary, ".bak"), SerializeSurvivorsProfile(*previous)))
			return false;
	}
	if (!WriteAtomic(primary, content))
		return false;
#ifdef __EMSCRIPTEN__
	emscripten_run_script("if(typeof Module !== 'undefined' && Module.saveToIndexedDB) Module.saveToIndexedDB();");
#endif
	return true;
}

unsigned SurvivorsPermanentUpgradeCost(const SurvivorsProfile &profile, SurvivorsPermanentUpgrade upgrade)
{
	const unsigned index = static_cast<unsigned>(upgrade);
	if (index >= profile.ranks.size() || profile.ranks[index] >= SurvivorsMaxPermanentRank)
		return 0;
	const auto rank = profile.ranks[index];
	return 25 * (rank + 1) * (rank + 1);
}

bool ApplySurvivorsPermanentPurchase(SurvivorsProfile &profile, SurvivorsPermanentUpgrade upgrade)
{
	if (!IsValidSurvivorsProfile(profile))
		return false;
	const auto cost = SurvivorsPermanentUpgradeCost(profile, upgrade);
	if (cost == 0 || profile.soulShards < cost)
		return false;
	profile.soulShards -= cost;
	++profile.ranks[static_cast<unsigned>(upgrade)];
	return true;
}

unsigned SurvivorsRunShardReward(unsigned kills, unsigned acts, unsigned bosses, bool victory)
{
	return static_cast<unsigned>(std::min<uint64_t>(SurvivorsMaxShards,
	    static_cast<uint64_t>(kills) / 5 + static_cast<uint64_t>(acts) * 25 + static_cast<uint64_t>(bosses) * 30 + (victory ? 100 : 0)));
}

const SurvivorsProfile &GetSurvivorsProfile()
{
	const std::string path = ProfilePath();
	if (LoadedPath != path)
		LoadSurvivorsProfile();
	return Profile;
}

bool LoadSurvivorsProfile()
{
	const std::string path = ProfilePath();
	if (LoadedPath == path && Dirty)
		return false;
	SurvivorsProfile loaded;
	const bool found = LoadSurvivorsProfileFromPath(path, loaded);
	std::error_code error;
	const bool exists = std::filesystem::exists(Utf8Path(path), error)
	    || std::filesystem::exists(Utf8Path(path + ".bak"), error);
	if (!found && exists) {
		Status = "Profile unreadable. Files preserved; restore a backup or retry loading.";
		if (LoadedPath != path)
			Profile = { };
		ReadOnlyProfile = true;
		LoadedPath = path;
		return false;
	}
	Profile = loaded;
	ReadOnlyProfile = false;
	LoadedPath = path;
	Status = found ? "Progress loaded. Campaign saves remain separate." : "New camp. Earn Soul Shards by hunting.";
	return true;
}

bool SaveSurvivorsProfile()
{
	GetSurvivorsProfile();
	if (ReadOnlyProfile)
		return false;
	if (!SaveSurvivorsProfileToPath(LoadedPath, Profile)) {
		Status = "Progress could not be saved. Retry before closing the game.";
		Dirty = true;
		return false;
	}
	Dirty = false;
	Status = "Progress saved.";
	return true;
}

bool PurchaseSurvivorsPermanentUpgrade(SurvivorsPermanentUpgrade upgrade)
{
	GetSurvivorsProfile();
	if (ReadOnlyProfile)
		return false;
	SurvivorsProfile next = Profile;
	if (!ApplySurvivorsPermanentPurchase(next, upgrade)) {
		Status = "Not enough Soul Shards, or this gift is already mastered.";
		return false;
	}
	if (!SaveSurvivorsProfileToPath(LoadedPath, next)) {
		Status = "Purchase canceled: profile could not be saved. No shards spent.";
		return false;
	}
	Profile = next;
	Dirty = false;
	Status = "Permanent gift purchased and saved.";
	return true;
}

unsigned SurvivorsPermanentRank(SurvivorsPermanentUpgrade upgrade)
{
	const auto &profile = GetSurvivorsProfile();
	const unsigned index = static_cast<unsigned>(upgrade);
	return index < profile.ranks.size() ? profile.ranks[index] : 0;
}

SurvivorsArsenalBonuses GetSurvivorsRunBonuses()
{
	const auto &profile = GetSurvivorsProfile();
	SurvivorsArsenalBonuses result;
	result.damage = profile.ranks[0] * 2;
	result.vitality = profile.ranks[1] * 5;
	result.haste = profile.ranks[2];
	result.magnet = profile.ranks[3];
	result.rerolls = std::min(5U, 1 + profile.clearedActs / 5);
	result.autoMelee = profile.autoMelee;
	return result;
}
unsigned SurvivorsSelectedDifficulty() { return GetSurvivorsProfile().selectedDifficulty; }

bool SelectSurvivorsDifficulty(unsigned difficulty)
{
	GetSurvivorsProfile();
	if (difficulty > Profile.unlockedDifficulty)
		return false;
	const auto previous = Profile.selectedDifficulty;
	Profile.selectedDifficulty = difficulty;
	if (SaveSurvivorsProfile())
		return true;
	Profile.selectedDifficulty = previous;
	return false;
}

bool SetSurvivorsAutoMelee(bool enabled)
{
	GetSurvivorsProfile();
	const bool previous = Profile.autoMelee;
	Profile.autoMelee = enabled;
	if (SaveSurvivorsProfile())
		return true;
	Profile.autoMelee = previous;
	return false;
}

bool RecordSurvivorsRunProgress(unsigned kills, unsigned runLevel, unsigned actsCleared, unsigned bosses, unsigned seconds, bool victory, unsigned difficulty)
{
	GetSurvivorsProfile();
	if (ReadOnlyProfile)
		return false;
	const auto reward = SurvivorsRunShardReward(kills, actsCleared, bosses, victory);
	Profile.soulShards = AddBounded(Profile.soulShards, reward, SurvivorsMaxShards);
	Profile.totalKills = AddBounded(Profile.totalKills, kills);
	Profile.runs = AddBounded(Profile.runs, 1);
	Profile.clearedActs = AddBounded(Profile.clearedActs, actsCleared);
	Profile.slainBosses = AddBounded(Profile.slainBosses, bosses);
	Profile.bestSeconds = std::max(Profile.bestSeconds, std::min(seconds, 86400U));
	Profile.bestLevel = std::max(Profile.bestLevel, std::min(runLevel, 1000U));
	if (victory) {
		Profile.victories = AddBounded(Profile.victories, 1);
		Profile.unlockedDifficulty = std::max(Profile.unlockedDifficulty, std::min(2U, std::min(difficulty, 2U) + 1));
	}
	Dirty = true;
	return SaveSurvivorsProfile();
}

const std::string &SurvivorsProgressionStatus() { return Status; }

} // namespace devilution
