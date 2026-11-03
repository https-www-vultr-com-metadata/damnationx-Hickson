#include "survivors.h"

#include <array>
#include <cstdint>
#include <chrono>
#include <cstdlib>
#include <fstream>
#include <format>
#include <string>
#include <string_view>
#include <vector>
#ifdef PACKET_ENCRYPTION
#include <sodium.h>
#endif

#include "diablo.h"
#include "dvlnet/async_runtime.h"
#include "controls/plrctrls.h"
#include "engine/path.h"
#include "engine/palette.h"
#include "engine/random.hpp"
#include "engine/render/primitive_render.hpp"
#include "engine/render/scrollrt.h"
#include "engine/render/text_render.hpp"
#include "engine/render/ui_scale.hpp"
#include "fpv/fpv.hpp"
#include "game_mode.hpp"
#include "gmenu.h"
#include "host/host_hooks.hpp"
#include "interfac.h"
#include "levels/dun_tile_data.hpp"
#include "levels/tile_properties.hpp"
#include "levels/survivors_layout.hpp"
#include "levels/survivors_rolling_runtime.hpp"
#include "missiles.h"
#include "monster.h"
#include "msg.h"
#include "multi.h"
#include "options.h"
#include "pack.h"
#include "pfile.h"
#include "player.h"
#include "plrmsg.h"
#include "qol/visual_store.h"
#include "survivors_arsenal.h"
#include "survivors_coop.h"
#include "survivors_encircle.h"
#include "survivors_progression.h"
#include "storm/storm_net.hpp"
#include "utils/log.hpp"
#include "utils/paths.h"

namespace devilution {
namespace {
constexpr std::array<std::string_view, 5> ActNames { "CATHEDRAL", "CATACOMBS", "CAVERNS", "BURNING HELL", "THRONE OF DAMNATION" };
constexpr std::array<std::string_view, 5> BossNames { "THE GRAVEWARDEN", "THE OSSUARY TYRANT", "THE MAGMA DEVOURER", "THE ABYSSAL MARSHAL", "THE LORD OF DAMNATION" };
constexpr std::array<std::string_view, 3> DifficultyNames { "NORMAL", "NIGHTMARE", "HELL" };
using Soul = SurvivorsRollingSoul;
struct RunPeer : SurvivorsCoopPeerState {
	SurvivorsArsenalBonuses bonuses;
	SurvivorsDamageGrace damageGrace;
	bool graceOwnsInvulnerability = false;
	std::optional<SurvivorsIdentity> ledgerIdentity;
	uint64_t ledgerFingerprint = 0;
	bool ledgerReady = false;
	bool helloReceived = false;
	SurvivorsIdentity incomingIdentity {};
	uint32_t incomingSize = 0;
	uint64_t incomingGeneration = 0;
	std::vector<std::byte> incomingCheckpoint;
	std::vector<std::byte> outgoingCheckpoint;
	size_t outgoingOffset = 0;
	uint64_t outgoingGeneration = 0;
	uint64_t mirroredGeneration = 0;
	uint64_t checkpointAckGeneration = 0;
};
struct Run {
	bool enabled = false;
	bool initialized = false;
	bool arenaInitialized = false;
	bool finished = false;
	bool settled = false;
	bool transitionPending = false;
	bool sectorMove = false;
	bool sectorTravelRequested = false;
	SurvivorsSector sector;
	SurvivorsSector sectorDelta;
	uint32_t sectorEpoch = 0;
	uint32_t loadedSectorEpoch = 0;
	std::array<uint32_t, MAX_PLRS> readySectorEpoch {};
	unsigned sectorArrivalTick = 0;
	bool bossDefeated = false;
	unsigned ticks = 0;
	unsigned actTicks = 0;
	unsigned tickRate = SurvivorsTicksPerSecond;
	unsigned difficulty = 0;
	unsigned wave = 1;
	unsigned kills = 0;
	unsigned bosses = 0;
	bool nextElite = false;
	int elite = -1;
	int boss = -1;
	unsigned spawnSequence = 0;
	SurvivorsEncircleDirector encircle;
	SurvivorsCampaignDirector campaign;
	std::array<bool, MaxMonsters> dead { };
	std::vector<Soul> souls;
	uint64_t soulEpoch = 0;
	std::array<RunPeer, MAX_PLRS> peers;
	std::optional<SurvivorsIdentity> localIdentity;
	bool helloQueued = false;
	bool uploadPrepared = false;
	uint64_t nextHelloRetry = 0;
	std::vector<std::byte> uploadCheckpoint;
	size_t uploadOffset = 0;
	std::vector<std::byte> receivedCheckpoint;
	uint32_t receivedCheckpointSize = 0;
	uint64_t receivedCheckpointGeneration = 0;
	uint64_t savedCheckpointGeneration = 0;
	uint64_t observedLedgerGeneration = 0;
	uint64_t sentCheckpointAckGeneration = 0;
	bool ledgerFailure = false;
	bool groundNeedsRestore = true;
	bool groundReady = false;
	bool groundFingerprintValid = false;
	uint64_t groundFingerprint = 0;
	std::vector<Item> groundUnplaced;
	bool sessionOnlyWarningShown = false;
	uint32_t snapshotSequence = 0;
	unsigned reliableMilestone = 0;
	uint32_t receivedSequence = 0;
	std::array<uint8_t, MAX_PLRS> sentActionSerial {};
	uint32_t receivedActionTicks = 0;
	uint64_t lastCoopStateAt = 0;
	uint32_t nextRequest = 0;
	std::optional<SurvivorsCoopDraftRequest> pendingRequest;
	uint64_t nextNetworkRetry = 0;
};
Run CurrentRun;
unsigned SelectedRunPeer = 0;
RunPeer &CurrentPeer() { return CurrentRun.peers[SelectedRunPeer]; }
void SelectPeer(unsigned peer)
{
	SelectedRunPeer = std::min(peer, static_cast<unsigned>(MAX_PLRS - 1));
	SelectSurvivorsArsenalPlayer(SelectedRunPeer);
}
Player &PeerPlayer(unsigned peer) { return peer == MyPlayerId && MyPlayer != nullptr ? *MyPlayer : Players[peer]; }
bool Cooperative() { return gbIsMultiplayer && AsyncNetEnabled(); }
bool Authority() { return !Cooperative() || AsyncNetIsAuthority(); }
bool ActivePeer(unsigned peer)
{
	if (peer == MyPlayerId && MyPlayer != nullptr && !gbIsMultiplayer) return true;
	return peer < Players.size() && Players[peer].plractive && Players[peer].isOnActiveLevel();
}

bool SupportsPersistentCoopGear()
{
#if defined(__EMSCRIPTEN__) || !defined(PACKET_ENCRYPTION) || defined(UNPACKED_SAVES)
	return false;
#elif defined(DAMNATION_S1_BRIDGE)
	// Headless bridge builds can exercise mixed-capability peers without
	// changing the behavior of released game builds.
	return std::getenv("DAMNATION_QA_SESSION_ONLY_GEAR") == nullptr;
#else
	return true;
#endif
}

// Hash only the persisted inventory and buyback state.  In particular, player
// position and life are deliberately excluded so ordinary movement does not
// rewrite the archive on every network snapshot.
uint64_t LedgerFingerprint(unsigned peer)
{
	const Player &player = PeerPlayer(peer);
	uint64_t hash = 14695981039346656037ULL;
	auto add = [&hash](const void *data, size_t size) {
		const auto *bytes = static_cast<const uint8_t *>(data);
		for (size_t i = 0; i < size; ++i) hash = (hash ^ bytes[i]) * 1099511628211ULL;
	};
	auto addItem = [&](const Item &item) {
		ItemNetPack packed {};
		PackNetItem(item, packed);
		add(&packed, sizeof(packed));
		if (item.isEmpty()) return;
		// SaveItem writes the full native item, not just the compact network
		// record. Include each persisted gameplay field so repairs, affixes,
		// requirements and other extended changes cannot evade the barrier.
#define HASH_ITEM_FIELD(field) add(&item.field, sizeof(item.field))
		HASH_ITEM_FIELD(_iSeed); HASH_ITEM_FIELD(_iCreateInfo); HASH_ITEM_FIELD(_itype);
		HASH_ITEM_FIELD(position); HASH_ITEM_FIELD(_iAnimFlag);
		HASH_ITEM_FIELD(AnimInfo.numberOfFrames); HASH_ITEM_FIELD(AnimInfo.currentFrame);
		HASH_ITEM_FIELD(selectionRegion); HASH_ITEM_FIELD(_iPostDraw); HASH_ITEM_FIELD(_iIdentified);
		HASH_ITEM_FIELD(_iMagical); HASH_ITEM_FIELD(_iName); HASH_ITEM_FIELD(_iIName);
		HASH_ITEM_FIELD(_iLoc); HASH_ITEM_FIELD(_iClass); HASH_ITEM_FIELD(_iCurs);
		HASH_ITEM_FIELD(_ivalue); HASH_ITEM_FIELD(_iIvalue); HASH_ITEM_FIELD(_iMinDam); HASH_ITEM_FIELD(_iMaxDam);
		HASH_ITEM_FIELD(_iAC); HASH_ITEM_FIELD(_iFlags); HASH_ITEM_FIELD(_iMiscId); HASH_ITEM_FIELD(_iSpell);
		HASH_ITEM_FIELD(_iCharges); HASH_ITEM_FIELD(_iMaxCharges); HASH_ITEM_FIELD(_iDurability); HASH_ITEM_FIELD(_iMaxDur);
		HASH_ITEM_FIELD(_iPLDam); HASH_ITEM_FIELD(_iPLToHit); HASH_ITEM_FIELD(_iPLAC); HASH_ITEM_FIELD(_iPLStr);
		HASH_ITEM_FIELD(_iPLMag); HASH_ITEM_FIELD(_iPLDex); HASH_ITEM_FIELD(_iPLVit); HASH_ITEM_FIELD(_iPLFR);
		HASH_ITEM_FIELD(_iPLLR); HASH_ITEM_FIELD(_iPLMR); HASH_ITEM_FIELD(_iPLMana); HASH_ITEM_FIELD(_iPLHP);
		HASH_ITEM_FIELD(_iPLDamMod); HASH_ITEM_FIELD(_iPLGetHit); HASH_ITEM_FIELD(_iPLLight); HASH_ITEM_FIELD(_iSplLvlAdd);
		HASH_ITEM_FIELD(_iRequest); HASH_ITEM_FIELD(_iUid); HASH_ITEM_FIELD(_iFMinDam); HASH_ITEM_FIELD(_iFMaxDam);
		HASH_ITEM_FIELD(_iLMinDam); HASH_ITEM_FIELD(_iLMaxDam); HASH_ITEM_FIELD(_iPLEnAc); HASH_ITEM_FIELD(_iPrePower);
		HASH_ITEM_FIELD(_iSufPower); HASH_ITEM_FIELD(_iVAdd1); HASH_ITEM_FIELD(_iVMult1); HASH_ITEM_FIELD(_iVAdd2);
		HASH_ITEM_FIELD(_iVMult2); HASH_ITEM_FIELD(_iMinStr); HASH_ITEM_FIELD(_iMinMag); HASH_ITEM_FIELD(_iMinDex);
		HASH_ITEM_FIELD(_iStatFlag); HASH_ITEM_FIELD(IDidx); HASH_ITEM_FIELD(dwBuff); HASH_ITEM_FIELD(_iDamAcFlags);
#undef HASH_ITEM_FIELD
	};
	for (const Item &item : player.InvBody) addItem(item);
	for (const Item &item : player.InvList) addItem(item);
	for (const Item &item : player.SpdList) addItem(item);
	addItem(player.HoldItem);
	add(player.InvGrid, sizeof(player.InvGrid));
	add(&player._pNumInv, sizeof(player._pNumInv));
	add(&player._pGold, sizeof(player._pGold));
	const auto items = GetSurvivorsBuybackItems(peer);
	const auto prices = GetSurvivorsBuybackPrices(peer);
	const size_t count = items.size();
	add(&count, sizeof(count));
	for (size_t i = 0; i < items.size(); ++i) {
		addItem(items[i]);
		if (i < prices.size()) add(&prices[i], sizeof(prices[i]));
	}
	return hash;
}

// Checkpoint ground with player records so a crash cannot restore both an
// item on the floor and the same item in a hunter's saved inventory.
std::vector<Item> CoopGroundItems()
{
	std::vector<Item> result = CurrentRun.groundUnplaced;
	SurvivorsRollingAppendParkedItems(result);
	SurvivorsAppendLocalDropEscrow(result);
	result.reserve(result.size() + ActiveItemCount);
	for (unsigned i = 0; i < ActiveItemCount; ++i) {
		const Item &item = Items[ActiveItems[i]];
		if (!item.isEmpty()) result.push_back(item);
	}
	return result;
}

uint64_t CoopGroundFingerprint(std::span<const Item> items)
{
	uint64_t hash = 14695981039346656037ULL;
	for (const Item &item : items) {
		ItemNetPack packed {};
		PackNetItem(item, packed);
		const auto *bytes = reinterpret_cast<const uint8_t *>(&packed);
		for (size_t i = 0; i < sizeof(packed); ++i) hash = (hash ^ bytes[i]) * 1099511628211ULL;
	}
	return (hash ^ items.size()) * 1099511628211ULL;
}

void PlaceRecoveredGround()
{
	if (MyPlayer == nullptr) return;
	while (!CurrentRun.groundUnplaced.empty() && ActiveItemCount < MAXITEMS) {
		const auto tile = FindClosestValidPosition(ItemSpaceOk, MyPlayer->position.tile, 1, 50);
		if (!tile) break;
		PlaceItemInWorld(std::move(CurrentRun.groundUnplaced.back()), *tile);
		CurrentRun.groundUnplaced.pop_back();
	}
}

bool RestoreCoopGround()
{
	if (!SupportsPersistentCoopGear()) return true;
	if (MyPlayer == nullptr || leveltype == DTYPE_TOWN) return false;
	if (CurrentRun.groundNeedsRestore) {
		std::vector<Item> recovered;
		if (pfile_load_survivors_ground(recovered) == SurvivorsLedgerLoad::Corrupt) return false;
		CurrentRun.groundUnplaced = std::move(recovered);
		CurrentRun.groundNeedsRestore = false;
		CurrentRun.groundReady = true;
		CurrentRun.groundFingerprintValid = false;
		if (!CurrentRun.groundUnplaced.empty()) EventPlrMsg("Recovered ground items are appearing near the host.");
	}
	PlaceRecoveredGround();
	return true;
}

bool AdmitCoopLedger(unsigned peer, const SurvivorsIdentity &identity)
{
	if (peer >= Players.size() || peer >= MAX_PLRS) return false;
	if (SupportsPersistentCoopGear() && !pfile_begin_survivors_shared_authority()) return false;
	const bool emptyIdentity = std::all_of(identity.begin(), identity.end(), [](uint8_t byte) { return byte == 0; });
	const bool sessionOnly = !SurvivorsCoopUsesDurableLedger(SupportsPersistentCoopGear(), emptyIdentity);
	for (unsigned i = 0; i < MAX_PLRS; ++i) {
		if (!sessionOnly && i != peer && CurrentRun.peers[i].ledgerReady && CurrentRun.peers[i].ledgerIdentity == identity)
			return false;
	}
	RunPeer &state = CurrentRun.peers[peer];
	if (state.ledgerReady) return sessionOnly ? !state.durableLedger : state.ledgerIdentity == identity;
	uint64_t generation = 0;
	if (!sessionOnly) {
		const auto loaded = pfile_load_survivors_ledger(PeerPlayer(peer), identity, generation);
		if (loaded == SurvivorsLedgerLoad::Corrupt) return false;
		if (loaded == SurvivorsLedgerLoad::Missing) {
			generation = 1;
			if (!pfile_save_survivors_ledger(PeerPlayer(peer), identity, generation)) return false;
		}
		state.ledgerIdentity = identity;
	}
	state.durableLedger = !sessionOnly;
	state.ledgerGeneration = generation;
	state.ledgerFingerprint = LedgerFingerprint(peer);
	// A fresh connection receives a fresh nonce. A retransmission from a prior
	// admission cannot become a new action when the transport slot is reused.
#ifdef PACKET_ENCRYPTION
	do { randombytes_buf(&state.admissionNonce, sizeof(state.admissionNonce)); } while (state.admissionNonce == 0);
#else
	do {
		const uint64_t high = GenerateSeed();
		const uint64_t low = GenerateSeed();
		state.admissionNonce = (high << 32) | low;
	} while (state.admissionNonce == 0);
#endif
	state.ledgerReady = true;
	if (sessionOnly && peer != MyPlayerId) EventPlrMsg("A teammate's cooperative gear is session-only and will not persist.");
	return true;
}

bool EnsureLocalCoopLedger()
{
	if (MyPlayer == nullptr) return false;
	if (Authority() && SupportsPersistentCoopGear() && !pfile_begin_survivors_shared_authority()) return false;
	if (!CurrentRun.localIdentity) {
		if (SupportsPersistentCoopGear()) CurrentRun.localIdentity = pfile_survivors_identity(*MyPlayer);
		else {
			CurrentRun.localIdentity = SurvivorsIdentity {};
			if (!CurrentRun.sessionOnlyWarningShown) {
				EventPlrMsg("Cooperative gear is session-only; ground transfers may be restricted on this host.");
				CurrentRun.sessionOnlyWarningShown = true;
			}
		}
	}
	if (!CurrentRun.localIdentity) return false;
	return !Authority() || AdmitCoopLedger(MyPlayerId, *CurrentRun.localIdentity);
}

const char *CoopLedgerUnavailableMessage()
{
#if defined(__EMSCRIPTEN__)
	return "Persistent cooperative gear is unavailable in browser builds. Use a native build.";
#elif !defined(PACKET_ENCRYPTION)
	return "Persistent cooperative gear requires network packet encryption in this build.";
#elif defined(UNPACKED_SAVES)
	return "Persistent cooperative gear is unavailable with unpacked saves in this build.";
#else
	if (const char *shared = std::getenv("DAMNATION_SURVIVORS_SHARED_DIR"); shared != nullptr && *shared != '\0')
		return "Shared cooperative storage is unavailable or in use. Check the shared folder and host lease.";
	return "Cooperative gear could not be loaded or saved. Check the save folder and restart.";
#endif
}
void ConsumeUpgrade();
void SendCoopState();
void HandleCoopPacket(uint8_t sender, std::span<const std::byte> bytes);
void ProcessCoopNetworking();
bool SubmitCoopDraft(SurvivorsCoopDraftAction action, unsigned choice = 0);

struct NativePlaytest {
	bool configured = false;
	bool enabled = false;
	bool ended = false;
	std::optional<bool> originalFocusPause;
	int previousHitPoints = -1;
	std::chrono::steady_clock::time_point started;
	std::chrono::steady_clock::time_point nextMove;
	std::chrono::steady_clock::time_point nextReport;
	std::ofstream telemetry;
};
NativePlaytest Playtest;
bool CampShop = false;


unsigned Seconds() { return CurrentRun.ticks / CurrentRun.tickRate; }
unsigned ActSeconds() { return CurrentRun.actTicks / CurrentRun.tickRate; }

void SettleRun()
{
	SelectPeer(MyPlayerId);
	if (!CurrentRun.initialized || CurrentRun.settled)
		return;
	CurrentRun.settled = true;
	if (!RecordSurvivorsRunProgress(CurrentRun.kills - std::min(CurrentRun.kills, CurrentPeer().startingKills), CurrentPeer().level, CurrentRun.campaign.actsCleared - std::min(CurrentRun.campaign.actsCleared, CurrentPeer().startingActs), CurrentRun.bosses - std::min(CurrentRun.bosses, CurrentPeer().startingBosses), Seconds() - std::min(Seconds(), CurrentPeer().startingSeconds), CurrentRun.campaign.victory && CurrentPeer().startingActs == 0, CurrentRun.difficulty))
		EventPlrMsg("Survivors progress could not be saved. Check the Survivors Camp status.");
}

void CheckUpgrade()
{
	if (!CurrentPeer().upgradePending && CurrentPeer().experience >= SurvivorsExperienceForLevel(CurrentPeer().level)) {
		CurrentPeer().upgradePending = true;
		++CurrentPeer().draftSequence;
		DraftSurvivorsArsenal();
	}
}

void GrantTeamExperience(unsigned amount)
{
	for (unsigned peer = 0; peer < MAX_PLRS; ++peer) {
		if (CurrentRun.peers[peer].started)
			CurrentRun.peers[peer].experience += amount;
	}
}

void CollectSouls()
{
	for (auto it = CurrentRun.souls.begin(); it != CurrentRun.souls.end();) {
		bool collected = false;
		for (unsigned peer = 0; peer < MAX_PLRS; ++peer) {
			if (!CurrentRun.peers[peer].started || !ActivePeer(peer) || PeerPlayer(peer)._pHitPoints <= 0) continue;
			SelectPeer(peer);
			if (PeerPlayer(peer).position.tile.WalkingDistance(it->tile) <= static_cast<int>(2 + SurvivorsArsenalMagnet())) { collected = true; break; }
		}
		if (collected) { GrantTeamExperience(it->value); it = CurrentRun.souls.erase(it); }
		else ++it;
	}
	for (unsigned peer = 0; peer < MAX_PLRS; ++peer) {
		if (!CurrentRun.peers[peer].started) continue;
		SelectPeer(peer);
		CheckUpgrade();
	}
	SelectPeer(MyPlayerId);
}

void TrackKills()
{
	for (size_t i = 0; i < ActiveMonsterCount; ++i) {
		const unsigned id = ActiveMonsters[i];
		const Monster &monster = Monsters[id];
		if ((monster.flags & MFLAG_GOLEM) != 0 || monster.isInvalid)
			continue;
		const bool dead = monster.hitPoints <= 0 || monster.mode == MonsterMode::Death;
		if (dead && !CurrentRun.dead[id]) {
			++CurrentRun.kills;
			const bool boss = static_cast<int>(id) == CurrentRun.boss;
			const unsigned value = boss ? 60 : (static_cast<int>(id) == CurrentRun.elite ? 20 : 2);
			if (static_cast<int>(id) == CurrentRun.elite)
				CurrentRun.elite = -1;
			if (boss) {
				CurrentRun.boss = -1;
				CurrentRun.bossDefeated = true;
				++CurrentRun.bosses;
				GrantTeamExperience(value);
				// Victory vacuums every remaining soul before the next arena loads.
				for (const Soul &soul : CurrentRun.souls)
					GrantTeamExperience(soul.value);
				GrantTeamExperience(SurvivorsRollingTakeParkedSouls());
				CurrentRun.souls.clear();
			} else {
				auto existing = std::find_if(CurrentRun.souls.begin(), CurrentRun.souls.end(), [&](const Soul &soul) { return soul.tile == monster.position.tile; });
				if (existing != CurrentRun.souls.end())
					existing->value += value;
				else if (CurrentRun.souls.size() < 1024)
					CurrentRun.souls.push_back({ monster.position.tile, value });
				else
					CurrentRun.souls.front().value += value;
			}
		}
		CurrentRun.dead[id] = dead;
	}
}

size_t WaveMonsterTypes(std::array<size_t, MaxLvlMTypes> &types)
{
	size_t count = 0;
	for (size_t i = 0; i < LevelMonsterTypeCount; ++i) {
		const auto type = LevelMonsterTypes[i].type;
		if (type != MT_GOLEM && type != MT_DIABLO && type != MT_SKING)
			types[count++] = i;
	}
	return count;
}

unsigned LivingParty(std::array<Player *, MAX_PLRS> &party)
{
	unsigned partySize = 0;
	for (unsigned peer = 0; peer < MAX_PLRS; ++peer)
		if (CurrentRun.peers[peer].started && ActivePeer(peer) && PeerPlayer(peer)._pHitPoints > 0) party[partySize++] = &PeerPlayer(peer);
	return partySize;
}

void ApplyWaveMonsterStats(Monster &monster, bool boss, bool elite, unsigned partySize)
{
	CurrentRun.dead[monster.getId()] = false;
	const unsigned scale = 1 + CurrentRun.difficulty * 2;
	monster.maxHitPoints = static_cast<int>(boss ? SurvivorsBossHitPoints(CurrentRun.campaign.act, CurrentRun.difficulty) : (12 + CurrentRun.wave * 5 + CurrentRun.campaign.act * 12) * scale * (elite ? 6 : 1)) << 6;
	monster.maxHitPoints = monster.maxHitPoints * static_cast<int>(100 + (boss ? 75 : 60) * (partySize - 1)) / 100;
	monster.hitPoints = monster.maxHitPoints;
	monster.minDamage = static_cast<uint8_t>(std::min(100U, (2 + CurrentRun.wave / 2 + CurrentRun.campaign.act) * (1 + CurrentRun.difficulty)));
	monster.maxDamage = static_cast<uint8_t>(std::min(160U, (4 + CurrentRun.wave + CurrentRun.campaign.act * 2) * (1 + CurrentRun.difficulty)));
	monster.minDamageSpecial = monster.minDamage;
	monster.maxDamageSpecial = monster.maxDamage;
	// The arsenal contains every damage type; bosses are durable rather than immune.
	monster.resistance = 0;
	monster.flags &= ~(MFLAG_HIDDEN | MFLAG_NO_ENEMY);
}

void SpawnWaveMonster(bool boss = false)
{
	if (ActiveMonsterCount >= MaxMonsters - (boss ? 1 : 12) || LevelMonsterTypeCount == 0)
		return;
	std::array<size_t, MaxLvlMTypes> types { };
	const size_t count = WaveMonsterTypes(types);
	if (count == 0)
		return;
	const unsigned sequence = CurrentRun.spawnSequence++;
	std::array<Player *, MAX_PLRS> party { };
	const unsigned partySize = LivingParty(party);
	if (partySize == 0) return;
	Player &focus = *party[sequence % partySize];
	for (unsigned attempt = 0; attempt < 128; ++attempt) {
		const unsigned sample = sequence * 37 + attempt * 17;
		const int radius = (boss ? 7 : 9) + static_cast<int>((sample / 8) % 5);
		const int edge = static_cast<int>(sample % (radius * 8));
		int dx;
		int dy;
		if (edge < radius * 2) { dx = edge - radius; dy = -radius; }
		else if (edge < radius * 4) { dx = radius; dy = edge - radius * 3; }
		else if (edge < radius * 6) { dx = radius * 5 - edge; dy = radius; }
		else { dx = -radius; dy = radius * 7 - edge; }
		const Point tile = focus.position.tile + Displacement { dx, dy };
		if (!InDungeonBounds(tile) || dPiece[tile.x][tile.y] == 0 || IsTileOccupied(tile))
			continue;
		Monster *monster = AddMonster(tile, GetDirection(tile, focus.position.tile), types[(sequence + CurrentRun.wave / 3) % count], true);
		if (monster == nullptr)
			return;
		const bool elite = !boss && CurrentRun.nextElite;
		if (elite)
			CurrentRun.nextElite = false;
		ApplyWaveMonsterStats(*monster, boss, elite, partySize);
		if (boss) {
			// A stable native pursuer avoids a randomly selected Gargoyle healing
			// through the entire boss gate or a Counselor repeatedly teleporting.
			// The act's loaded monster art still supplies its walk/attack frames.
			monster->ai = MonsterAIID::SkeletonMelee;
			monster->flags &= ~MFLAG_ALLOW_SPECIAL;
			M_StartStand(*monster, GetDirection(tile, focus.position.tile));
		}
		monster->activeForTicks = 255;
		monster->enemy = focus.getId();
		monster->enemyPosition = focus.position.tile;
		if (boss) {
			CurrentRun.boss = static_cast<int>(monster->getId());
			EventPlrMsg(std::format("{} blocks the way. Defeat the act boss!", BossNames[CurrentRun.campaign.act]));
		} else if (elite) {
			CurrentRun.elite = static_cast<int>(monster->getId());
			EventPlrMsg("A Soul Reaper enters the hunt!");
		}
		return;
	}
}

uint32_t EncircleSeed() { return sgGameInitInfo.gameSeed[0] ^ sgGameInitInfo.gameSeed[1]; }

// Encircle events (Source/survivors_encircle.h): a full ring of one monster
// type from the act's pool closes in on one hunter. Authority only; the ring
// reaches cooperative peers through the regular world snapshots.
void ProcessEncircle()
{
	using namespace SurvivorsEncircle;
	if (CurrentRun.campaign.bossGate || CurrentRun.boss >= 0) return;
	if (CurrentRun.sectorArrivalTick != 0 && CurrentRun.ticks - CurrentRun.sectorArrivalTick < SectorArrivalGraceSeconds * CurrentRun.tickRate) return;
	const uint32_t seed = EncircleSeed();
	const unsigned act = CurrentRun.campaign.act;
	auto &director = CurrentRun.encircle;
	if (!director.Due(act, ActSeconds(), seed)) return;
	std::array<size_t, MaxLvlMTypes> types { };
	const size_t typeCount = WaveMonsterTypes(types);
	std::array<Player *, MAX_PLRS> party { };
	const unsigned partySize = LivingParty(party);
	const unsigned count = SurvivorsEncircleCapacity(SurvivorsEncircleCount(act, CurrentRun.difficulty, director.fired, partySize), static_cast<unsigned>(ActiveMonsterCount), MaxMonsters);
	if (typeCount == 0 || partySize == 0 || count == 0) {
		director.Retry(ActSeconds());
		return;
	}
	// Co-op: each ring traps one hunter, rotating through the living party.
	const uint32_t roll = SurvivorsEncircleHash(seed ^ CurrentRun.sectorEpoch, act, 0x100U + director.fired);
	Player &focus = *party[(roll + director.fired) % partySize];
	const unsigned placed = SpawnSurvivorsEncircle({
	    .focus = focus,
	    .typeIndex = types[(roll >> 8) % typeCount],
	    .count = count,
	    .phase = static_cast<double>((roll >> 16) & 0xffffU) / 65536.0,
	    .tick = CurrentRun.ticks,
	    .tickRate = CurrentRun.tickRate,
	    .configure = [partySize](Monster &monster) { ApplyWaveMonsterStats(monster, false, false, partySize); },
	});
	if (placed == 0) {
		director.Retry(ActSeconds());
		return;
	}
	director.Fired(ActSeconds(), seed);
	EventPlrMsg(partySize > 1 ? std::format("{} is surrounded! Break through the ring!", focus._pName) : std::string("SURROUNDED! Break through the ring!"));
}

void ProcessBoss()
{
	if (CurrentRun.boss < 0) return;
	Monster &boss = Monsters[CurrentRun.boss];
	if (boss.isInvalid || boss.hitPoints <= 0) return;
	const bool ring = CurrentRun.ticks % (CurrentRun.tickRate * 3) == 0;
	const bool aimed = CurrentRun.ticks % (CurrentRun.tickRate * 2) == 0;
	if (!ring && !aimed) return;
	const int damage = static_cast<int>(6 + CurrentRun.campaign.act * 2 + CurrentRun.difficulty * 4);
	if (ring) {
		for (unsigned index = 0; index < 8; ++index) {
			const Direction direction = static_cast<Direction>(index);
			if (Missile *projectile = AddMissile(boss.position.tile, boss.position.tile + direction, direction, MissileID::Firebolt, TARGET_PLAYERS, boss, damage, 1)) projectile->_midam = damage;
		}
	}
	if (aimed) {
		Player *focus = nullptr;
		int distance = 1000;
		for (unsigned peer = 0; peer < MAX_PLRS; ++peer) {
			if (!CurrentRun.peers[peer].started || !ActivePeer(peer) || PeerPlayer(peer)._pHitPoints <= 0) continue;
			const int next = boss.position.tile.WalkingDistance(PeerPlayer(peer).position.future);
			if (next < distance) { distance = next; focus = &PeerPlayer(peer); }
		}
		if (focus != nullptr) {
			const Direction facing = GetDirection(boss.position.tile, focus->position.future);
			const auto missile = CurrentRun.campaign.act >= 2 ? MissileID::Fireball : MissileID::Firebolt;
			for (const auto offset : { Direction::NoDirection, Left(facing), Right(facing) }) {
				const Point target = focus->position.future + offset;
				if (Missile *projectile = AddMissile(boss.position.tile, target, facing, missile, TARGET_PLAYERS, boss, damage, 1)) projectile->_midam = damage;
			}
		}
	}
}

void ConsumeUpgrade()
{
	CurrentPeer().experience -= SurvivorsExperienceForLevel(CurrentPeer().level);
	++CurrentPeer().level;
	CurrentPeer().upgradePending = false;
	// Drafts are resolved by the host in co-op. Award points there as well so
	// the authoritative player snapshot and stat-spending validation agree.
	Player &player = PeerPlayer(SelectedRunPeer);
	player._pStatPts = SurvivorsStatPointsAfterLevel(player._pStatPts, CalcStatDiff(player));
	if (!Cooperative() || PeerPlayer(SelectedRunPeer)._pHitPoints > 0)
		PeerPlayer(SelectedRunPeer).RestoreFullLife();
	CheckUpgrade();
}

void InitializeArena()
{
	CurrentRun.arenaInitialized = true;
	CurrentRun.souls.clear();
	ResetSurvivorsEncircleMarch();
	CurrentRun.dead.fill(false);
	CurrentRun.elite = -1;
	CurrentRun.boss = -1;
	for (size_t i = 0; i < ActiveMonsterCount; ++i) {
		Monster &monster = Monsters[ActiveMonsters[i]];
		if ((monster.flags & MFLAG_GOLEM) == 0) {
			M_ClearSquares(monster);
			monster.isInvalid = true;
		}
	}
	for (unsigned peer = 0; peer < MAX_PLRS; ++peer) {
		if (!CurrentRun.peers[peer].started || !ActivePeer(peer)) continue;
		Player &player = PeerPlayer(peer);
		if (CurrentRun.sectorMove) continue;
		const bool fallen = player._pHitPoints <= 0;
		player.RestoreFullLife();
		if (fallen && player.AnimationData[static_cast<size_t>(player_graphic::Stand)].sprites.has_value())
			StartStand(player, player._pdir);
		if (peer == MyPlayerId) MyPlayerIsDead = false;
	}
	if (CurrentRun.sectorMove) {
		CurrentRun.sectorMove = false;
		CurrentRun.sectorArrivalTick = CurrentRun.ticks;
		EventPlrMsg(std::format("Exploring sector {}, {}.", CurrentRun.sector.x, CurrentRun.sector.y));
	} else {
		EventPlrMsg(std::format("Act {}: {} - {}. Survive four minutes, then defeat the guardian.", CurrentRun.campaign.act + 1, ActNames[CurrentRun.campaign.act], DifficultyNames[CurrentRun.difficulty]));
	}
}

bool TrySurvivorsSectorTransition()
{
	if (!CurrentRun.sectorTravelRequested) return false;
	CurrentRun.sectorTravelRequested = false;
	if (CurrentRun.campaign.bossGate || CurrentRun.boss >= 0 || CurrentRun.transitionPending || IsVisualStoreOpen
	    || CurrentRun.ticks < CurrentRun.sectorArrivalTick + CurrentRun.tickRate * 3) {
		EventPlrMsg("Cannot travel yet. Defeat the guardian or wait after arriving.");
		return false;
	}
	std::array<bool, 4> candidates { true, true, true, true };
	bool any = false;
	for (unsigned peer = 0; peer < MAX_PLRS; ++peer) {
		if (!CurrentRun.peers[peer].started || !ActivePeer(peer) || PeerPlayer(peer).hasNoLife()) continue;
		if (CurrentRun.peers[peer].upgradePending) { EventPlrMsg("Wait for every hunter to finish choosing a gift."); return false; }
		any = true;
		const Point tile = PeerPlayer(peer).position.tile;
		candidates[0] &= tile.x <= 42;
		candidates[1] &= tile.x >= 70;
		candidates[2] &= tile.y <= 36;
		candidates[3] &= tile.y >= 76;
	}
	if (!any) return false;
	if (Cooperative() && !SurvivorsCommitCoopLedgers()) return false;
	SurvivorsSector next = CurrentRun.sector;
	if (candidates[0]) --next.x;
	else if (candidates[1]) ++next.x;
	else if (candidates[2]) --next.y;
	else if (candidates[3]) ++next.y;
	else { EventPlrMsg(Cooperative() ? "Gather all living hunters at the same edge to travel." : "Move near an edge to travel."); return false; }
	if (!IsValidSurvivorsSector(next) || CurrentRun.sectorEpoch == UINT32_MAX) return false;
	for (const Soul &soul : CurrentRun.souls) GrantTeamExperience(soul.value);
	CurrentRun.souls.clear();
	CurrentRun.sectorDelta = { next.x - CurrentRun.sector.x, next.y - CurrentRun.sector.y };
	CurrentRun.sector = next;
	++CurrentRun.sectorEpoch;
	CurrentRun.sectorMove = true;
	CurrentRun.transitionPending = true;
	CurrentRun.elite = -1;
	CurrentRun.reliableMilestone = 0;
	const unsigned level = SurvivorsActLevel(CurrentRun.campaign.act);
	MyPlayer->_pLvlVisited[level] = false;
	if (Cooperative()) SendCoopState();
	MyPlayer->Stop();
	StartNewLvl(*MyPlayer, WM_DIABNEXTLVL, static_cast<int>(level));
	return true;
}

void AdvanceCampaign()
{
	// Prepare the transition without consuming the boss or advancing the act.
	// A joining peer can temporarily hold the persistence barrier closed, so
	// mutating the live director first would lose the transition on retry.
	auto nextCampaign = CurrentRun.campaign;
	if (!nextCampaign.Update(ActSeconds(), CurrentRun.bossDefeated)) {
		CurrentRun.campaign = nextCampaign;
		return;
	}
	if (Cooperative() && !SurvivorsCommitCoopLedgers()) return;
	CurrentRun.campaign = nextCampaign;
	CurrentRun.bossDefeated = false;
	if (CurrentRun.campaign.victory) {
		CurrentRun.finished = true;
		SettleRun();
		EventPlrMsg("The Lord of Damnation has fallen. Return to Survivors Camp for your rewards.");
		return;
	}
	for (unsigned peer = 0; peer < MAX_PLRS; ++peer) {
		if (!CurrentRun.peers[peer].started) continue;
		SelectPeer(peer);
		GrantSurvivorsArsenalLife(SurvivorsActLifeBonus);
	}
	SelectPeer(MyPlayerId);
	EventPlrMsg("Act cleared: expedition vigor grants +15 maximum life. Your build carries forward.");
	CurrentRun.actTicks = 0;
	CurrentRun.nextElite = false;
	SurvivorsRollingPrepareLevelChange();
	CurrentRun.transitionPending = true;
	CurrentRun.sectorMove = false;
	CurrentRun.sector = {};
	CurrentRun.sectorDelta = {};
	++CurrentRun.sectorEpoch;
	const unsigned level = SurvivorsActLevel(CurrentRun.campaign.act);
	MyPlayer->_pLvlVisited[level] = false;
	if (Cooperative()) {
		for (unsigned peer = 0; peer < MAX_PLRS; ++peer) {
			if (peer != MyPlayerId && CurrentRun.peers[peer].started && Players[peer].plractive)
				Players[peer].setLevel(level);
		}
		SendCoopState();
	}
	MyPlayer->Stop();
	StartNewLvl(*MyPlayer, WM_DIABNEXTLVL, static_cast<int>(level));
}
uint64_t NetworkMilliseconds()
{
	return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
}

void SendCoopSouls(unsigned peer)
{
	std::vector<const Soul *> nearby;
	for (const Soul &soul : CurrentRun.souls)
		if (PeerPlayer(peer).position.tile.WalkingDistance(soul.tile) <= 25) nearby.push_back(&soul);
	std::sort(nearby.begin(), nearby.end(), [peer](const Soul *a, const Soul *b) { return PeerPlayer(peer).position.tile.WalkingDistance(a->tile) < PeerPlayer(peer).position.tile.WalkingDistance(b->tile); });
	if (nearby.size() > 60) nearby.resize(60);
	SurvivorsCoopWriter out;
	out.Byte(SurvivorsCoopVersion); out.Byte(static_cast<unsigned>(SurvivorsCoopPacket::Souls)); out.Byte(peer); out.Byte(CurrentRun.campaign.act); out.Byte(nearby.size());
	const uint64_t epoch = SurvivorsRollingEpoch();
	out.Dword(static_cast<uint32_t>(epoch)); out.Dword(static_cast<uint32_t>(epoch >> 32));
	for (const Soul *soul : nearby) { out.Byte(soul->tile.x); out.Byte(soul->tile.y); out.Word(std::min(soul->value, 65535U)); }
	AsyncNetSendExtension(SurvivorsCoopChannel, out.data);
}

// Attack/cast overlays started since the last tick, so clients animate them too.
void SendCoopActions()
{
	if (!Cooperative() || !Authority()) return;
	SurvivorsCoopActions packet;
	packet.ticks = CurrentRun.ticks;
	for (unsigned peer = 0; peer < MAX_PLRS && peer < Players.size(); ++peer) {
		const Player &player = Players[peer];
		if (CurrentRun.sentActionSerial[peer] == player.survivorsActionSerial) continue;
		CurrentRun.sentActionSerial[peer] = player.survivorsActionSerial;
		if (!player.plractive || !player.survivorsActionActive) continue;
		packet.actions[packet.count++] = { static_cast<uint8_t>(peer), static_cast<uint8_t>(player.survivorsActionGraphic), static_cast<uint8_t>(player.survivorsActionDirection) };
	}
	if (packet.count != 0) AsyncNetSendExtension(SurvivorsCoopChannel, EncodeSurvivorsCoopActions(packet));
}

void SendCoopState()
{
	if (!Cooperative() || !Authority() || !CurrentRun.initialized) return;
	SurvivorsCoopState state;
	state.sequence = ++CurrentRun.snapshotSequence;
	state.sectorEpoch = CurrentRun.sectorEpoch; state.sector = CurrentRun.sector;
	state.ticks = CurrentRun.ticks; state.actTicks = CurrentRun.actTicks; state.kills = CurrentRun.kills;
	state.tickRate = CurrentRun.tickRate; state.difficulty = CurrentRun.difficulty;
	state.act = CurrentRun.campaign.act; state.actsCleared = CurrentRun.campaign.actsCleared; state.bosses = CurrentRun.bosses;
	state.boss = CurrentRun.boss; state.elite = CurrentRun.elite;
	state.flags = (CurrentRun.transitionPending ? 1 : 0) | (CurrentRun.finished ? 2 : 0) | (CurrentRun.campaign.victory ? 4 : 0) | (CurrentRun.campaign.bossGate ? 8 : 0) | (CurrentRun.bossDefeated ? 16 : 0);
	for (unsigned peer = 0; peer < MAX_PLRS; ++peer) {
		SelectPeer(peer);
		CurrentPeer().arsenal = GetSurvivorsArsenalSnapshot();
		state.peers[peer] = CurrentPeer();
	}
	SelectPeer(MyPlayerId);
	const auto encoded = EncodeSurvivorsCoopState(state);
	const unsigned milestone = (CurrentRun.campaign.act + 1) * 2 + (CurrentRun.finished ? 1 : 0);
	if ((CurrentRun.finished || CurrentRun.transitionPending) && CurrentRun.reliableMilestone != milestone) {
		if (AsyncNetSendReliableExtension(SurvivorsCoopChannel, encoded))
			CurrentRun.reliableMilestone = milestone;
		else
			AsyncNetSendExtension(SurvivorsCoopChannel, encoded);
	} else {
		AsyncNetSendExtension(SurvivorsCoopChannel, encoded);
	}
	for (unsigned peer = 0; peer < MAX_PLRS; ++peer)
		if (CurrentRun.peers[peer].started && ActivePeer(peer)) SendCoopSouls(peer);
}

bool SubmitCoopDraft(SurvivorsCoopDraftAction action, unsigned choice)
{
	if (!Cooperative() || Authority() || CurrentRun.pendingRequest || choice > 2) return false;
	CurrentRun.pendingRequest = SurvivorsCoopDraftRequest { ++CurrentRun.nextRequest, CurrentPeer().draftSequence, action, choice };
	CurrentRun.nextNetworkRetry = 0;
	ProcessCoopNetworking();
	return true;
}

// Checkpoints use ordered reliable extension messages. Only a complete,
// validated archive is installed; an interrupted transfer leaves the old copy.
constexpr size_t CheckpointChunkSize = 320;
constexpr size_t CheckpointMaximumSize = 1024 * 1024;
bool QueueCheckpointChunk(unsigned peer, std::span<const std::byte> bytes, size_t &offset, uint64_t generation)
{
	if (offset >= bytes.size()) return true;
	SurvivorsCoopWriter out;
	out.Byte(SurvivorsCoopVersion); out.Byte(static_cast<unsigned>(SurvivorsCoopPacket::Checkpoint));
	out.Byte(peer); out.Dword(static_cast<uint32_t>(generation)); out.Dword(static_cast<uint32_t>(generation >> 32));
	out.Dword(static_cast<uint32_t>(bytes.size())); out.Dword(static_cast<uint32_t>(offset));
	const size_t length = std::min(CheckpointChunkSize, bytes.size() - offset);
	out.data.insert(out.data.end(), bytes.begin() + offset, bytes.begin() + offset + length);
	if (!AsyncNetSendReliableExtension(SurvivorsCoopChannel, out.data)) return false;
	offset += length;
	return true;
}

bool AppendCheckpointChunk(SurvivorsCoopReader &in, uint32_t &size, std::vector<std::byte> &data, uint64_t &generation)
{
	const uint64_t low = in.Dword();
	const uint64_t incomingGeneration = low | (static_cast<uint64_t>(in.Dword()) << 32);
	const uint32_t total = in.Dword();
	const uint32_t offset = in.Dword();
	if (!in.valid || total == 0 || total > CheckpointMaximumSize || offset > total
	    || in.data.empty() || in.data.size() > CheckpointChunkSize || in.data.size() > total - offset) return false;
	if (offset == 0) {
		if (size != 0 && size != total) return false;
		size = total; generation = incomingGeneration; data.clear(); data.reserve(total);
	}
	if (size != total || generation != incomingGeneration || offset != data.size()) return false;
	data.insert(data.end(), in.data.begin(), in.data.end());
	return true;
}

void StartCoopPeer(unsigned sender)
{
	RunPeer &peer = CurrentRun.peers[sender];
	if (peer.started) return;
	if (SupportsPersistentCoopGear() && !pfile_begin_survivors_shared_authority()) {
		CurrentRun.ledgerFailure = true;
		EventPlrMsg("The shared cooperative save is unavailable or in use. The hunt is paused.");
		SNetDropPlayer(sender, net::leaveinfo_t::LEAVE_DROP);
		return;
	}
	for (unsigned i = 0; i < MAX_PLRS; ++i) {
		if (i != sender && CurrentRun.peers[i].ledgerReady && CurrentRun.peers[i].ledgerIdentity == peer.incomingIdentity) {
			SNetDropPlayer(sender, net::leaveinfo_t::LEAVE_DROP);
			return;
		}
	}
	if (!peer.incomingCheckpoint.empty() && SupportsPersistentCoopGear()) {
		const auto result = pfile_import_survivors_ledger(PeerPlayer(sender), peer.incomingIdentity, peer.incomingCheckpoint);
		if (result == SurvivorsLedgerImport::Conflict || result == SurvivorsLedgerImport::Invalid) {
			EventPlrMsg("A teammate's cooperative checkpoint conflicts or is invalid.");
			SNetDropPlayer(sender, net::leaveinfo_t::LEAVE_DROP);
			return;
		}
	}
	peer.incomingCheckpoint.clear();
	if (!AdmitCoopLedger(sender, peer.incomingIdentity)) {
		SNetDropPlayer(sender, net::leaveinfo_t::LEAVE_DROP);
		return;
	}
	peer.started = true;
	peer.startingKills = CurrentRun.kills;
	peer.startingActs = CurrentRun.campaign.actsCleared;
	peer.startingBosses = CurrentRun.bosses;
	peer.startingSeconds = Seconds();
	const auto &leader = CurrentRun.peers[MyPlayerId];
	peer.experience = leader.experience;
	for (unsigned level = 1; level < leader.level; ++level) peer.experience += SurvivorsExperienceForLevel(level);
	StartSurvivorsArsenal(Players[sender], peer.bonuses, sgGameInitInfo.gameSeed[0] ^ (0x53555256U + sender));
	GrantSurvivorsArsenalLife(SurvivorsStartingLifeBonus + CurrentRun.campaign.actsCleared * SurvivorsActLifeBonus);
	CheckUpgrade();
}

void HandleCoopPacket(uint8_t sender, std::span<const std::byte> bytes)
{
	if (!CurrentRun.enabled || !Cooperative() || bytes.size() < 2 || bytes.size() > 384 || sender >= MAX_PLRS || std::to_integer<unsigned>(bytes[0]) != SurvivorsCoopVersion) return;
	const auto kind = static_cast<SurvivorsCoopPacket>(std::to_integer<uint8_t>(bytes[1]));
	if (!Authority()) {
		if (sender != 0) return;
		if (kind == SurvivorsCoopPacket::Checkpoint) {
			SurvivorsCoopReader in(bytes.subspan(2));
			if (in.Byte() != MyPlayerId || !CurrentRun.localIdentity || MyPlayer == nullptr) return;
			if (!AppendCheckpointChunk(in, CurrentRun.receivedCheckpointSize, CurrentRun.receivedCheckpoint, CurrentRun.receivedCheckpointGeneration)) return;
			if (CurrentRun.receivedCheckpoint.size() == CurrentRun.receivedCheckpointSize) {
				const auto result = CurrentRun.receivedCheckpointGeneration == 0 ? SurvivorsLedgerImport::Invalid
				    : pfile_import_survivors_ledger(*MyPlayer, *CurrentRun.localIdentity, CurrentRun.receivedCheckpoint, CurrentRun.receivedCheckpointGeneration);
				if (result == SurvivorsLedgerImport::Conflict || result == SurvivorsLedgerImport::Invalid || result == SurvivorsLedgerImport::Older) {
					EventPlrMsg("Cooperative checkpoint could not be saved; do not change hosts.");
				} else {
					CurrentRun.savedCheckpointGeneration = CurrentRun.receivedCheckpointGeneration;
				}
				CurrentRun.receivedCheckpoint.clear();
				CurrentRun.receivedCheckpointSize = 0;
			}
		} else if (kind == SurvivorsCoopPacket::State) {
			const auto state = DecodeSurvivorsCoopState(bytes);
			if (!state || state->sequence <= CurrentRun.receivedSequence) return;
			CurrentRun.receivedSequence = state->sequence;
			CurrentRun.lastCoopStateAt = NetworkMilliseconds();
			const bool needLoad = MyPlayer != nullptr && (MyPlayer->plrlevel != SurvivorsActLevel(state->act) || CurrentRun.loadedSectorEpoch != state->sectorEpoch);
			const bool sameAct = CurrentRun.campaign.act == state->act;
			if (state->sectorEpoch < CurrentRun.loadedSectorEpoch) return;
			if (needLoad && sameAct) CurrentRun.sectorDelta = { state->sector.x - CurrentRun.sector.x, state->sector.y - CurrentRun.sector.y };
			CurrentRun.sector = state->sector; CurrentRun.sectorEpoch = state->sectorEpoch;
			CurrentRun.ticks = state->ticks; CurrentRun.actTicks = state->actTicks; CurrentRun.tickRate = state->tickRate;
			CurrentRun.kills = state->kills; CurrentRun.difficulty = state->difficulty; CurrentRun.bosses = state->bosses;
			CurrentRun.campaign.act = state->act; CurrentRun.campaign.actsCleared = state->actsCleared;
			CurrentRun.campaign.victory = (state->flags & 4) != 0; CurrentRun.campaign.bossGate = (state->flags & 8) != 0;
			CurrentRun.bossDefeated = (state->flags & 16) != 0; CurrentRun.finished = (state->flags & 2) != 0;
			CurrentRun.boss = state->boss; CurrentRun.elite = state->elite;
			for (unsigned peer = 0; peer < MAX_PLRS; ++peer) {
				static_cast<SurvivorsCoopPeerState &>(CurrentRun.peers[peer]) = state->peers[peer];
				ApplySurvivorsArsenalReplica(peer, state->peers[peer].arsenal);
			}
			if (MyPlayerId < MAX_PLRS) SurvivorsObserveLedgerGeneration(state->peers[MyPlayerId].ledgerGeneration);
			SelectPeer(MyPlayerId);
			if (CurrentPeer().started && !CurrentPeer().durableLedger && !CurrentRun.sessionOnlyWarningShown) {
				EventPlrMsg("Your cooperative gear is session-only; ground transfers may be restricted on this host.");
				CurrentRun.sessionOnlyWarningShown = true;
			}
			CurrentRun.initialized = CurrentPeer().started;
			CurrentRun.nextRequest = std::max(CurrentRun.nextRequest, CurrentPeer().acknowledgedRequest);
			if (CurrentRun.pendingRequest && CurrentPeer().acknowledgedRequest >= CurrentRun.pendingRequest->request) CurrentRun.pendingRequest.reset();
			if (needLoad && !CurrentRun.transitionPending && !CurrentRun.finished) {
				CurrentRun.sectorMove = sameAct;
				CurrentRun.transitionPending = true;
				MyPlayer->_pLvlVisited[SurvivorsActLevel(state->act)] = false;
				MyPlayer->Stop();
				StartNewLvl(*MyPlayer, WM_DIABNEXTLVL, static_cast<int>(SurvivorsActLevel(state->act)));
			}
			if (!needLoad && (state->flags & 1) == 0) CurrentRun.transitionPending = false;
			if (CurrentRun.finished) SettleRun();
		} else if (kind == SurvivorsCoopPacket::Souls) {
			SurvivorsCoopReader in(bytes.subspan(2));
			const unsigned peer = in.Byte(); const unsigned act = in.Byte(); const unsigned count = in.Byte();
			const uint64_t low = in.Dword();
			const uint64_t epoch = low | (static_cast<uint64_t>(in.Dword()) << 32);
			if (peer != MyPlayerId || act != CurrentRun.campaign.act || count > 60 || epoch != SurvivorsRollingEpoch()) return;
			std::vector<Soul> souls;
			for (unsigned index = 0; index < count; ++index) {
				const Point tile { static_cast<int>(in.Byte()), static_cast<int>(in.Byte()) }; const unsigned value = in.Word();
				if (!InDungeonBounds(tile) || value == 0) return;
				souls.push_back({ tile, value });
			}
			if (in.Complete()) { CurrentRun.souls = std::move(souls); CurrentRun.soulEpoch = epoch; }
		} else if (kind == SurvivorsCoopPacket::Actions) {
			const auto packet = DecodeSurvivorsCoopActions(bytes);
			// Unreliable delivery: drop late duplicates. A new run restarts the clock.
			if (!packet || (packet->ticks <= CurrentRun.receivedActionTicks && packet->ticks + CurrentRun.tickRate * 5 > CurrentRun.receivedActionTicks)) return;
			CurrentRun.receivedActionTicks = packet->ticks;
			for (unsigned index = 0; index < packet->count; ++index) {
				const SurvivorsCoopAction &action = packet->actions[index];
				if (action.player >= Players.size()) continue;
				Player &player = Players[action.player];
				if (!player.plractive || !player.isOnActiveLevel()) continue;
				ReplaySurvivorsActionAnimation(player, static_cast<player_graphic>(action.graphic), static_cast<Direction>(action.direction));
			}
		}
		return;
	}
	if (!ActivePeer(sender) || sender == MyPlayerId) return;
	SelectPeer(sender);
	if (kind == SurvivorsCoopPacket::Checkpoint) {
		SurvivorsCoopReader in(bytes.subspan(2));
		if (in.Byte() == sender && CurrentPeer().helloReceived && !CurrentPeer().started
		    && AppendCheckpointChunk(in, CurrentPeer().incomingSize, CurrentPeer().incomingCheckpoint, CurrentPeer().incomingGeneration)
		    && CurrentPeer().incomingCheckpoint.size() == CurrentPeer().incomingSize)
			StartCoopPeer(sender);
	} else if (kind == SurvivorsCoopPacket::CheckpointAck) {
		SurvivorsCoopReader in(bytes.subspan(2));
		const uint64_t low = in.Dword();
		const uint64_t generation = low | (static_cast<uint64_t>(in.Dword()) << 32);
		if (in.Complete() && generation <= CurrentPeer().mirroredGeneration)
			CurrentPeer().checkpointAckGeneration = std::max(CurrentPeer().checkpointAckGeneration, generation);
	} else if (kind == SurvivorsCoopPacket::SectorReady) {
		SurvivorsCoopReader in(bytes.subspan(2));
		const uint32_t epoch = in.Dword();
		if (in.Complete() && epoch == CurrentRun.sectorEpoch && sender < MAX_PLRS)
			CurrentRun.readySectorEpoch[sender] = epoch;
	} else if (kind == SurvivorsCoopPacket::Hello) {
		SurvivorsCoopReader in(bytes.subspan(2));
		SurvivorsArsenalBonuses bonuses;
		bonuses.damage = in.Byte(); bonuses.vitality = in.Byte(); bonuses.haste = in.Byte(); bonuses.magnet = in.Byte(); bonuses.rerolls = in.Byte();
		const unsigned autoMelee = in.Byte();
		SurvivorsIdentity identity {};
		for (auto &byte : identity) byte = in.Byte();
		const uint32_t checkpointSize = in.Dword();
		bonuses.autoMelee = autoMelee != 0;
		if (in.Complete() && bonuses.damage <= 20 && bonuses.vitality <= 50 && bonuses.haste <= 10 && bonuses.magnet <= 10 && bonuses.rerolls <= 5 && autoMelee <= 1 && checkpointSize <= CheckpointMaximumSize && !CurrentPeer().helloReceived) {
			CurrentPeer().helloReceived = true;
			CurrentPeer().incomingIdentity = identity;
			CurrentPeer().incomingSize = checkpointSize;
			CurrentPeer().bonuses = bonuses;
			if (checkpointSize == 0) StartCoopPeer(sender);
		}
	} else if (kind == SurvivorsCoopPacket::Draft) {
		const auto request = DecodeSurvivorsCoopDraft(bytes);
		CurrentPeer().arsenal = GetSurvivorsArsenalSnapshot();
		if (request && request->request > CurrentPeer().acknowledgedRequest && request->request - CurrentPeer().acknowledgedRequest <= 1024) {
			const bool fresh = SurvivorsCoopDraftRequestIsFresh(CurrentPeer(), *request);
			CurrentPeer().acknowledgedRequest = request->request;
			if (fresh && PeerPlayer(sender)._pHitPoints > 0) {
				bool consumed = false;
				switch (request->action) {
				case SurvivorsCoopDraftAction::Choose: consumed = ChooseSurvivorsArsenalUpgrade(request->choice); break;
				case SurvivorsCoopDraftAction::Skip: if (CanSkipSurvivorsArsenalDraft()) { SkipSurvivorsArsenalDraft(); consumed = true; } break;
				case SurvivorsCoopDraftAction::Reroll: if (RerollSurvivorsArsenal()) ++CurrentPeer().draftSequence; break;
				case SurvivorsCoopDraftAction::Banish: if (BanishSurvivorsArsenalOffer(request->choice)) ++CurrentPeer().draftSequence; break;
				}
				if (consumed) ConsumeUpgrade();
			}
		}
	}
	SelectPeer(MyPlayerId);
	SendCoopState();
}

void ProcessCoopNetworking()
{
	if (!Cooperative()) return;
	AsyncNetSetExtensionHandler(SurvivorsCoopChannel, HandleCoopPacket);
	const uint64_t now = NetworkMilliseconds();
	if (now < CurrentRun.nextNetworkRetry) return;
	CurrentRun.nextNetworkRetry = now + 250;
	if (!Authority() && CurrentRun.savedCheckpointGeneration > CurrentRun.sentCheckpointAckGeneration) {
		SurvivorsCoopWriter ack;
		ack.Byte(SurvivorsCoopVersion); ack.Byte(static_cast<unsigned>(SurvivorsCoopPacket::CheckpointAck));
		ack.Dword(static_cast<uint32_t>(CurrentRun.savedCheckpointGeneration)); ack.Dword(static_cast<uint32_t>(CurrentRun.savedCheckpointGeneration >> 32));
		if (AsyncNetSendReliableExtension(SurvivorsCoopChannel, ack.data)) CurrentRun.sentCheckpointAckGeneration = CurrentRun.savedCheckpointGeneration;
	}
	if (Authority()) {
		for (unsigned peer = 0; peer < MAX_PLRS; ++peer) {
			RunPeer &state = CurrentRun.peers[peer];
			if (peer == MyPlayerId || !state.started || !state.durableLedger || !state.ledgerIdentity || !ActivePeer(peer)) continue;
			if (state.outgoingCheckpoint.empty() && state.mirroredGeneration != state.ledgerGeneration) {
				const auto archive = pfile_export_survivors_ledger(*state.ledgerIdentity);
				if (!archive || archive->empty()) continue;
				state.outgoingCheckpoint = *archive;
				state.outgoingOffset = 0;
				state.outgoingGeneration = state.ledgerGeneration;
			}
			for (unsigned chunk = 0; chunk < 12 && state.outgoingOffset < state.outgoingCheckpoint.size(); ++chunk)
				if (!QueueCheckpointChunk(peer, state.outgoingCheckpoint, state.outgoingOffset, state.outgoingGeneration)) break;
			if (!state.outgoingCheckpoint.empty() && state.outgoingOffset == state.outgoingCheckpoint.size()) {
				state.mirroredGeneration = state.outgoingGeneration;
				state.outgoingCheckpoint.clear();
			}
		}
		if (CurrentRun.transitionPending && CurrentRun.loadedSectorEpoch == CurrentRun.sectorEpoch) {
			bool ready = true;
			for (unsigned peer = 0; peer < MAX_PLRS; ++peer)
				if (peer != MyPlayerId && CurrentRun.peers[peer].started && peer < Players.size() && Players[peer].plractive && CurrentRun.readySectorEpoch[peer] != CurrentRun.sectorEpoch) ready = false;
			if (ready) CurrentRun.transitionPending = false;
		}
		if (CurrentRun.initialized && (CurrentRun.finished || CurrentRun.transitionPending)) SendCoopState();
	} else if (CurrentRun.transitionPending && CurrentRun.loadedSectorEpoch == CurrentRun.sectorEpoch) {
		SurvivorsCoopWriter out;
		out.Byte(SurvivorsCoopVersion); out.Byte(static_cast<unsigned>(SurvivorsCoopPacket::SectorReady)); out.Dword(CurrentRun.sectorEpoch);
		AsyncNetSendExtension(SurvivorsCoopChannel, out.data);
	} else if (CurrentRun.pendingRequest) {
		AsyncNetSendExtension(SurvivorsCoopChannel, EncodeSurvivorsCoopDraft(*CurrentRun.pendingRequest));
	} else if (!CurrentPeer().started) {
		if (!EnsureLocalCoopLedger()) {
			if (!CurrentRun.ledgerFailure) EventPlrMsg(CoopLedgerUnavailableMessage());
			CurrentRun.ledgerFailure = true;
			return;
		}
		if (SurvivorsCoopShouldRetryAdmission(CurrentRun.helloQueued, CurrentRun.uploadOffset, CurrentRun.uploadCheckpoint.size(), AsyncNetHasReliablePending(), now, CurrentRun.nextHelloRetry)) {
			CurrentRun.helloQueued = false;
			CurrentRun.uploadOffset = 0;
		}
		if (!CurrentRun.helloQueued) {
			if (!CurrentRun.uploadPrepared && SupportsPersistentCoopGear()) {
				const auto archive = pfile_export_survivors_ledger(*CurrentRun.localIdentity);
				if (!archive) {
					if (!CurrentRun.ledgerFailure) EventPlrMsg(CoopLedgerUnavailableMessage());
					CurrentRun.ledgerFailure = true;
					return;
				}
				CurrentRun.uploadCheckpoint = *archive;
			}
			CurrentRun.uploadPrepared = true;
			const auto bonuses = GetSurvivorsRunBonuses();
			SurvivorsCoopWriter out;
			out.Byte(SurvivorsCoopVersion); out.Byte(static_cast<unsigned>(SurvivorsCoopPacket::Hello));
			out.Byte(bonuses.damage); out.Byte(bonuses.vitality); out.Byte(bonuses.haste); out.Byte(bonuses.magnet); out.Byte(bonuses.rerolls); out.Byte(bonuses.autoMelee);
			for (uint8_t byte : *CurrentRun.localIdentity) out.Byte(byte);
			out.Dword(static_cast<uint32_t>(CurrentRun.uploadCheckpoint.size()));
			CurrentRun.helloQueued = AsyncNetSendReliableExtension(SurvivorsCoopChannel, out.data);
			if (CurrentRun.helloQueued) CurrentRun.nextHelloRetry = now + 1000;
		}
		if (CurrentRun.helloQueued)
			for (unsigned chunk = 0; chunk < 12 && CurrentRun.uploadOffset < CurrentRun.uploadCheckpoint.size(); ++chunk)
				if (!QueueCheckpointChunk(MyPlayerId, CurrentRun.uploadCheckpoint, CurrentRun.uploadOffset, 0)) break;
	}
}

} // namespace

bool SurvivorsCoopPeerLedgerReady(unsigned peer)
{
	return !IsSurvivorsMode() || !Cooperative() || !Authority()
	    || (peer < MAX_PLRS && CurrentRun.peers[peer].ledgerReady && !CurrentRun.ledgerFailure);
}

bool SurvivorsGroundTransferBlocked() { return IsSurvivorsTransitionPending(); }

bool SurvivorsCanTransferCoopGround(unsigned peer)
{
	if (SurvivorsGroundTransferBlocked()) return false;
	if (!IsSurvivorsMode() || !Cooperative() || !Authority()) return true;
	if (peer >= MAX_PLRS || !CurrentRun.peers[peer].ledgerReady || CurrentRun.ledgerFailure) return false;
	return SurvivorsCoopGroundTransferAllowed(SupportsPersistentCoopGear(), CurrentRun.peers[peer].durableLedger);
}

uint64_t SurvivorsCoopAdmissionNonce(unsigned peer)
{
	return peer < MAX_PLRS ? CurrentRun.peers[peer].admissionNonce : 0;
}

uint64_t SurvivorsCoopLedgerGeneration(unsigned peer)
{
	return peer < MAX_PLRS && CurrentRun.peers[peer].ledgerReady ? CurrentRun.peers[peer].ledgerGeneration : 0;
}

uint64_t SurvivorsCoopCheckpointGeneration(unsigned peer)
{
	if (peer >= MAX_PLRS) return 0;
	return Authority() ? CurrentRun.peers[peer].checkpointAckGeneration : (peer == MyPlayerId ? CurrentRun.savedCheckpointGeneration : 0);
}

void SurvivorsObserveLedgerGeneration(uint64_t generation)
{
	if (Cooperative() && !Authority()) CurrentRun.observedLedgerGeneration = std::max(CurrentRun.observedLedgerGeneration, generation);
}

unsigned SurvivorsCoopGearSyncStatus()
{
	if (!IsSurvivorsMode() || !Cooperative() || MyPlayerId >= MAX_PLRS) return 0;
	const RunPeer &peer = CurrentRun.peers[MyPlayerId];
	if (!peer.started || CurrentRun.ledgerFailure) return 1;
	if (!peer.durableLedger) return 3;
	if (Authority()) return peer.ledgerReady && LedgerFingerprint(MyPlayerId) == peer.ledgerFingerprint ? 4 : 1;
	const uint64_t now = NetworkMilliseconds();
	return CurrentRun.lastCoopStateAt != 0 && now >= CurrentRun.lastCoopStateAt
	    && SurvivorsCoopCheckpointIsCurrent(CurrentRun.observedLedgerGeneration, CurrentRun.savedCheckpointGeneration,
	        GetAsyncNetStatus().pendingInputs, now - CurrentRun.lastCoopStateAt) ? 2 : 1;
}

void SurvivorsForgetCoopLedger(unsigned peer)
{
	if (peer >= MAX_PLRS || peer == MyPlayerId) return;
	// Reset the old draft and readiness before this transport slot can be
	// reused, including a reconnect occurring before the next game tick.
	SelectPeer(peer);
	ResetSurvivorsArsenal();
	CurrentRun.peers[peer] = {};
	CurrentRun.readySectorEpoch[peer] = 0;
	SetSurvivorsBuyback(peer, {}, {});
	SelectPeer(MyPlayerId);
}

bool SurvivorsCommitCoopLedgers()
{
	if (!IsSurvivorsMode() || !Cooperative() || !Authority()) return true;
	if (CurrentRun.ledgerFailure) return false;
	if (!EnsureLocalCoopLedger()) {
		CurrentRun.ledgerFailure = true;
		EventPlrMsg(CoopLedgerUnavailableMessage());
		return false;
	}
	if (SupportsPersistentCoopGear() && CurrentRun.groundNeedsRestore && leveltype != DTYPE_TOWN && !RestoreCoopGround()) {
		CurrentRun.ledgerFailure = true;
		EventPlrMsg("Cooperative ground recovery failed. The hunt is paused; check the save folder.");
		return false;
	}
	std::array<SurvivorsLedgerEntry, MAX_PLRS> pending {};
	std::array<uint64_t, MAX_PLRS> fingerprints {};
	std::array<unsigned, MAX_PLRS> peers {};
	size_t pendingCount = 0;
	for (unsigned peer = 0; peer < Players.size() && peer < MAX_PLRS; ++peer) {
		if (!Players[peer].plractive) continue;
		RunPeer &state = CurrentRun.peers[peer];
		// A joining player must prove its identity and load its ledger before
		// any world snapshot can acknowledge input for that player.
		if (!state.ledgerReady) return false;
		if (!state.durableLedger) continue;
		if (!state.ledgerIdentity) return false;
		const uint64_t fingerprint = LedgerFingerprint(peer);
		if (fingerprint == state.ledgerFingerprint) continue;
		if (state.ledgerGeneration == UINT64_MAX) {
			CurrentRun.ledgerFailure = true;
			EventPlrMsg("Cooperative gear ledger is full. The hunt is paused.");
			return false;
		}
		pending[pendingCount] = { &PeerPlayer(peer), *state.ledgerIdentity, state.ledgerGeneration + 1 };
		fingerprints[pendingCount] = fingerprint;
		peers[pendingCount++] = peer;
	}
	// A level being unloaded must not replace its only durable ground copy.
	std::vector<Item> ground;
	std::optional<std::span<const Item>> groundChange;
	uint64_t groundFingerprint = 0;
	if (SupportsPersistentCoopGear() && CurrentRun.groundReady && !CurrentRun.transitionPending) {
		SurvivorsMaterializeLocalDrops();
		PlaceRecoveredGround();
		ground = CoopGroundItems();
		groundFingerprint = CoopGroundFingerprint(ground);
		if (!CurrentRun.groundFingerprintValid || groundFingerprint != CurrentRun.groundFingerprint)
			groundChange = std::span<const Item>(ground);
	}
	if ((pendingCount != 0 || groundChange) && !pfile_commit_survivors_ledgers(std::span(pending).first(pendingCount), groundChange)) {
			// A failed atomic replacement can have an ambiguous outcome. Never
			// publish an acknowledgement or roll back to a potentially stale copy.
			CurrentRun.ledgerFailure = true;
			EventPlrMsg("Cooperative gear save failed. The hunt is paused; restart after checking the save folder.");
			return false;
	}
	if (groundChange) { CurrentRun.groundFingerprint = groundFingerprint; CurrentRun.groundFingerprintValid = true; }
	for (size_t i = 0; i < pendingCount; ++i) {
		RunPeer &state = CurrentRun.peers[peers[i]];
		++state.ledgerGeneration;
		state.ledgerFingerprint = fingerprints[i];
	}
	return true;
}

void FinalizeSurvivorsRun()
{
	if (CurrentRun.enabled)
		SettleRun();
}

void SetSurvivorsMode(bool enabled)
{
	ResetSurvivorsRollingWorld();
	CampShop = false;
	if (!enabled && Playtest.configured) {
		if (Playtest.originalFocusPause) GetOptions().Gameplay.pauseOnFocusLoss.SetValue(*Playtest.originalFocusPause);
		if (Playtest.enabled && !Playtest.ended) {
			Playtest.telemetry << std::format("{{\"kind\":\"abandoned\",\"sim_seconds\":{}}}\n", Seconds());
			Playtest.telemetry.flush();
		}
		Playtest = { };
	}
	if (CurrentRun.enabled) {
		SettleRun();
		for (unsigned peer = 0; peer < MAX_PLRS; ++peer) {
			if (!CurrentRun.peers[peer].graceOwnsInvulnerability) continue;
			if ((peer == MyPlayerId && MyPlayer != nullptr) || peer < Players.size()) PeerPlayer(peer)._pInvincible = false;
		}
	}
	// Settlement and the final in-game checkpoint finish before a different
	// host may acquire this authority's shared lease.
	if (!enabled) pfile_end_survivors_shared_authority();
	SurvivorsClearLocalDropEscrow();
	CurrentRun = { };
	ResetSurvivorsEncircleMarch();
	ResetSurvivorsManualActions();
	CurrentRun.enabled = enabled;
	ResetAllSurvivorsArsenals();
	SelectPeer(MyPlayerId);
	if (enabled) {
		LoadSurvivorsProfile();
		CurrentRun.difficulty = std::min(2U, SurvivorsSelectedDifficulty());
	}
}

void SetSurvivorsCampShop(bool enabled) { CampShop = enabled && CurrentRun.enabled; }
bool IsSurvivorsCampShop() { return CurrentRun.enabled && CampShop; }
#ifdef DAMNATION_S1_BRIDGE
bool SurvivorsQaAdvanceBossGate()
{
	if (!CurrentRun.enabled || CampShop || !CurrentRun.initialized || CurrentRun.finished
	    || CurrentRun.transitionPending || CurrentRun.campaign.act != 0 || CurrentRun.campaign.bossGate
	    || CurrentRun.boss >= 0 || CurrentRun.bossDefeated || !Authority() || CurrentRun.tickRate == 0)
		return false;
	// Advance the run clock too. A snapshot whose act clock exceeds the run
	// clock is rejected by every peer, which froze their co-op state (and the
	// gear sync indicator) until the act changed.
	const auto clock = SurvivorsCoopAdvanceActClock(CurrentRun.ticks, CurrentRun.actTicks, (SurvivorsActDurationSeconds - 1) * CurrentRun.tickRate);
	CurrentRun.ticks = clock.ticks;
	CurrentRun.actTicks = clock.actTicks;
	return true;
}
bool SurvivorsQaTriggerEncircle()
{
	if (!CurrentRun.enabled || CampShop || !CurrentRun.initialized || CurrentRun.finished || CurrentRun.transitionPending
	    || CurrentRun.campaign.bossGate || CurrentRun.boss >= 0 || !Authority() || ActSeconds() > SurvivorsEncircle::LastStartSecond)
		return false;
	CurrentRun.encircle.Due(CurrentRun.campaign.act, ActSeconds(), EncircleSeed());
	CurrentRun.encircle.nextSecond = ActSeconds();
	return true;
}
#endif
bool RequestSurvivorsSectorTravel()
{
	if (!CurrentRun.enabled || CampShop || CurrentRun.finished || CurrentRun.transitionPending || IsVisualStoreOpen) return false;
	if (SurvivorsRollingActive()) return true;
	if (Cooperative() && !Authority()) { EventPlrMsg("The party leader chooses when to travel."); return true; }
	CurrentRun.sectorTravelRequested = true;
	return true;
}
uint32_t SurvivorsLoadedSectorEpoch() { return CurrentRun.enabled ? CurrentRun.loadedSectorEpoch : 0; }
Point SurvivorsSectorPosition() { return { CurrentRun.sector.x, CurrentRun.sector.y }; }
uint32_t SurvivorsSectorEpoch() { return CurrentRun.enabled ? CurrentRun.sectorEpoch : 0; }
uint32_t SurvivorsLevelSeed(uint32_t baseSeed) { return CurrentRun.enabled ? SurvivorsSectorSeed(baseSeed, CurrentRun.campaign.act, CurrentRun.sector) : baseSeed; }
Point SurvivorsSectorArrival(Point fallback, uint32_t seed)
{
	if (!IsSurvivorsMode() || !CurrentRun.sectorMove) return fallback;
	const auto layout = GenerateSurvivorsLayout(seed);
	const auto tile = SurvivorsArrivalMega(layout, CurrentRun.sectorDelta.x, CurrentRun.sectorDelta.y);
	return { 16 + 2 * tile[0], 16 + 2 * tile[1] };
}

bool IsSurvivorsPlaytestEnabled()
{
	if (!Playtest.configured) {
		Playtest.configured = true;
		const char *enabled = std::getenv("DAMNATIONX_SURVIVORS_PLAYTEST");
		const char *logPath = std::getenv("DAMNATIONX_SURVIVORS_PLAYTEST_LOG");
		// Two explicit settings prevent incidental environment flags from taking
		// control. The caller chooses an isolated --save-dir for profile writes.
		Playtest.enabled = enabled != nullptr && std::string_view(enabled) == "1" && logPath != nullptr && *logPath != '\0';
		if (Playtest.enabled) {
			Playtest.telemetry.open(logPath, std::ios::out | std::ios::app);
			Playtest.enabled = Playtest.telemetry.good();
			Playtest.originalFocusPause = *GetOptions().Gameplay.pauseOnFocusLoss;
			GetOptions().Gameplay.pauseOnFocusLoss.SetValue(false);
			Playtest.started = std::chrono::steady_clock::now();
			Playtest.nextMove = Playtest.nextReport = Playtest.started;
			Playtest.telemetry << "{\"kind\":\"native_production_playtest\",\"clock\":\"real_time\",\"cheats\":false}\n";
			Playtest.telemetry.flush();
			LogInfo("Survivors autonomous playtest enabled; normal movement, gifts and belt potions only.");
		}
	}
	return Playtest.enabled;
}

void ProcessSurvivorsPlaytestInput()
{
	SelectPeer(MyPlayerId);
	if (!IsSurvivorsMode() || MyPlayer == nullptr || !IsSurvivorsPlaytestEnabled() || !CurrentRun.initialized)
		return;
	const auto now = std::chrono::steady_clock::now();
	if (Playtest.previousHitPoints >= 0 && MyPlayer->_pHitPoints < Playtest.previousHitPoints) {
		Playtest.telemetry << std::format("{{\"kind\":\"damage\",\"sim_seconds\":{},\"before_fp\":{},\"after_fp\":{},\"missiles\":{}}}\n", Seconds(), Playtest.previousHitPoints, MyPlayer->_pHitPoints, Missiles.size());
		Playtest.telemetry.flush();
	}
	Playtest.previousHitPoints = MyPlayer->_pHitPoints;
	if (now >= Playtest.nextReport || (CurrentRun.finished && !Playtest.ended)) {
		Playtest.ended = CurrentRun.finished;
		Playtest.nextReport = now + std::chrono::seconds(10);
		const auto wall = std::chrono::duration_cast<std::chrono::milliseconds>(now - Playtest.started).count();
		Playtest.telemetry << std::format("{{\"kind\":\"{}\",\"wall_ms\":{},\"sim_seconds\":{},\"tick_rate\":{},\"class_id\":{},\"act\":{},\"act_seconds\":{},\"hp\":{},\"max_hp\":{},\"level\":{},\"kills\":{},\"monsters\":{},\"souls\":{},\"x\":{},\"y\":{},\"boss\":{},\"victory\":{}}}\n", CurrentRun.finished ? "finished" : "tick", wall, Seconds(), CurrentRun.tickRate, static_cast<unsigned>(MyPlayer->_pClass), CurrentRun.campaign.act + 1, ActSeconds(), MyPlayer->_pHitPoints >> 6, MyPlayer->_pMaxHP >> 6, CurrentPeer().level, CurrentRun.kills, ActiveMonsterCount, CurrentRun.souls.size(), MyPlayer->position.tile.x, MyPlayer->position.tile.y, CurrentRun.boss, CurrentRun.campaign.victory);
		Playtest.telemetry.flush();
	}
	if (CurrentRun.finished || CurrentRun.transitionPending || leveltype == DTYPE_TOWN || gmenu_is_active())
		return;
	if (CurrentPeer().upgradePending) {
		const auto arsenal = GetSurvivorsArsenalSnapshot();
		int bestScore = -10000;
		unsigned best = 0;
		for (unsigned choice = 0; choice < arsenal.offers.size(); ++choice) {
			const unsigned item = arsenal.offers[choice];
			int score = 0;
			if (item < SurvivorsWeaponCount) {
				score = 70 + static_cast<int>(arsenal.ranks[item]) * 9;
				if (item == 2 || item == 4 || item == 8) score += 25;
				if (arsenal.ranks[item] == 5) score += 80;
			} else if (item < SurvivorsArsenalFirstFallback) {
				const unsigned passive = item - SurvivorsWeaponCount;
				constexpr std::array<int, 6> priorities { 70, 75, 95, 65, 105, 85 };
				score = priorities[passive] - static_cast<int>(arsenal.passives[passive]) * 10;
				if ((passive == 3 || passive == 4) && MyPlayer->_pHitPoints * 2 < MyPlayer->_pMaxHP) score += 60;
				// The baseline glass-cannon build reached Hell with 46 life and died
				// to its first ranged pack. Buy real Fortitude gifts before that act.
				if (passive == 3 && MyPlayer->_pMaxHP < static_cast<int>(65 + CurrentRun.campaign.act * 15) * 64) score += 90;
			} else {
				score = 30;
			}
			if (score > bestScore) { bestScore = score; best = choice; }
		}
			if (ChooseSurvivorsUpgrade(best)) {
			Playtest.telemetry << std::format("{{\"kind\":\"gift\",\"sim_seconds\":{},\"choice\":{},\"item\":{},\"level\":{}}}\n", Seconds(), best, arsenal.offers[best], CurrentPeer().level);
			Playtest.telemetry.flush();
		}
		return;
	}
	if (now < Playtest.nextMove)
		return;
	Playtest.nextMove = now + std::chrono::milliseconds(250);
	if (MyPlayer->_pHitPoints * 3 < MyPlayer->_pMaxHP)
		UseBeltItem(BeltItemType::Healing);
	const Point origin = MyPlayer->position.future;
	Point preferred { 56, 56 };
	// Follow souls while maintaining room to kite. The bot receives no damage,
	// speed, vision, XP or target accuracy bonuses from this observer.
	int soulDistance = 1000;
	for (const Soul &soul : CurrentRun.souls) {
		const int distance = origin.WalkingDistance(soul.tile);
		if (distance < soulDistance) { soulDistance = distance; preferred = soul.tile; }
	}
	if (CurrentRun.boss >= 0 && !Monsters[CurrentRun.boss].isInvalid && Monsters[CurrentRun.boss].hitPoints > 0) {
		preferred = Monsters[CurrentRun.boss].position.tile;
		soulDistance = 0;
	}
	const unsigned quadrant = (Seconds() / 9) % 4;
	constexpr std::array<Displacement, 4> patrol { Displacement { -12, -12 }, Displacement { 12, -12 }, Displacement { 12, 12 }, Displacement { -12, 12 } };
	if (soulDistance > 16)
		preferred += patrol[quadrant];
	Point destination = origin;
	int bestScore = -1000000;
	for (unsigned direction = 0; direction < 8; ++direction) {
		const Direction facing = static_cast<Direction>(direction);
		const Point first = origin + facing;
		if (!PosOkPlayer(*MyPlayer, first) || !CanStep(origin, first))
			continue;
		Point candidate = first;
		for (unsigned step = 1; step < 4; ++step) {
			const Point next = candidate + facing;
			if (!PosOkPlayer(*MyPlayer, next) || !CanStep(candidate, next)) break;
			candidate = next;
		}
		int score = -candidate.WalkingDistance(preferred) * 12 - candidate.WalkingDistance(Point { 56, 56 });
		for (size_t index = 0; index < ActiveMonsterCount; ++index) {
			const Monster &monster = Monsters[ActiveMonsters[index]];
			if (monster.isInvalid || monster.hitPoints <= 0 || (monster.flags & MFLAG_GOLEM) != 0) continue;
			const int distance = candidate.WalkingDistance(monster.position.future);
			if (distance < 6) score -= (6 - distance) * (6 - distance) * 25;
			const int firstDistance = first.WalkingDistance(monster.position.future);
			if (firstDistance < 3) score -= (3 - firstDistance) * 150;
		}
		for (const Missile &missile : Missiles) {
			if (missile._miDelFlag || missile._micaster == TARGET_MONSTERS) continue;
			const int distance = candidate.WalkingDistance(missile.position.tile);
			if (distance < 4) score -= (4 - distance) * 125;
			const int firstDistance = first.WalkingDistance(missile.position.tile);
			if (firstDistance < 2) score -= 300;
		}
		if (score > bestScore) { bestScore = score; destination = candidate; }
	}
	if (destination != origin)
		NetSendCmdLoc(MyPlayerId, true, CMD_WALKXY, destination);
}

bool SurvivorsAllowPlayerDamage(Player &player)
{
	if (!CurrentRun.enabled || !CurrentRun.initialized || !Authority()) return true;
	unsigned peer = MAX_PLRS;
	if (&player == MyPlayer) peer = MyPlayerId;
	else for (unsigned index = 0; index < Players.size(); ++index) if (&Players[index] == &player) { peer = index; break; }
	// A (re)joining teammate is still being admitted: the authority neither
	// executes its input nor has restored its portable checkpoint (life, gear).
	// It cannot move, drink or fight back, so monsters may not damage it yet.
	if (peer < MAX_PLRS && peer != MyPlayerId && !SurvivorsCoopPeerLedgerReady(peer)) return false;
	if (peer >= MAX_PLRS || !CurrentRun.peers[peer].started) return true;
	auto &state = CurrentRun.peers[peer];
	if (!state.damageGrace.Accept(CurrentRun.ticks, CurrentRun.tickRate)) return false;
	// This is the production mode's contact grace, not a playtest exception.
	// Native melee/projectile hit checks also respect this flag, avoiding
	// repeated hit-recovery animations while the player escapes a dense pack.
	if (!player._pInvincible) {
		player._pInvincible = true;
		state.graceOwnsInvulnerability = true;
	}
	return true;
}

void SurvivorsOpenEscapeLane(Player &player)
{
	if (!CurrentRun.enabled || !CurrentRun.initialized || !Authority() || player.hasNoLife())
		return;
	// Monster and player occupancy use separate tile maps. Moving the hunter
	// through a living monster would make client path prediction disagree with
	// the host, so use the existing collision-checked monster knockback instead.
	// Only an accepted damaging hit reaches this function; contact grace keeps
	// a crowded pack from opening/closing the lane several times per tick.
	for (size_t index = 0; index < ActiveMonsterCount; ++index) {
		Monster &monster = Monsters[ActiveMonsters[index]];
		if (monster.isInvalid || monster.hasNoLife() || (monster.flags & MFLAG_GOLEM) != 0 || monster.getId() == CurrentRun.boss)
			continue;
		if (player.position.tile.WalkingDistance(monster.position.tile) == 1)
			M_GetKnockback(monster, player.position.tile);
	}
}

bool IsSurvivorsMode() { return CurrentRun.enabled; }
int SurvivorsBossMonsterId() { return CurrentRun.enabled ? CurrentRun.boss : -1; }
bool IsSurvivorsUpgradePending() { return CurrentRun.enabled && CurrentRun.peers[std::min(static_cast<unsigned>(MyPlayerId), 3U)].upgradePending; }
bool IsSurvivorsDraftRequestPending() { return Cooperative() && !Authority() && CurrentRun.pendingRequest.has_value(); }
bool ShouldPauseSurvivorsSimulation() { return IsSurvivorsRunFinished() || IsSurvivorsTransitionPending() || (!Cooperative() && IsSurvivorsUpgradePending()); }
bool IsSurvivorsRunFinished() { return CurrentRun.enabled && CurrentRun.finished; }
bool IsSurvivorsTransitionPending() { return CurrentRun.enabled && CurrentRun.transitionPending; }

SurvivorsRunStatus GetSurvivorsRunStatus()
{
	SelectPeer(MyPlayerId);
	return { CurrentRun.campaign.act + 1, CurrentRun.difficulty, ActSeconds(), Seconds(), CurrentPeer().level, CurrentRun.kills, CurrentRun.campaign.actsCleared, CurrentRun.bosses, CurrentRun.campaign.bossGate && !CurrentRun.bossDefeated, CurrentRun.transitionPending, CurrentRun.campaign.victory, CurrentRun.boss };
}

void SurvivorsLevelLoaded()
{
	if (!CurrentRun.enabled)
		return;
	InitSurvivorsRollingWorld();
	if (Cooperative() && Authority() && SupportsPersistentCoopGear()) {
		SurvivorsClearLocalDropEscrow();
		CurrentRun.groundNeedsRestore = true;
		CurrentRun.groundReady = false;
		CurrentRun.groundUnplaced.clear();
	}
	if (CurrentRun.sectorMove && Cooperative() && Authority() && MyPlayer != nullptr) {
		for (unsigned peer = 0; peer < MAX_PLRS && peer < Players.size(); ++peer) {
			if (peer == MyPlayerId || !CurrentRun.peers[peer].started || !Players[peer].plractive || Players[peer].hasNoLife()) continue;
			Player &player = Players[peer];
			FixPlrWalkTags(player);
			player.position.tile = MyPlayer->position.tile;
			SyncInitPlrPos(player);
		}
	}
	CurrentRun.loadedSectorEpoch = CurrentRun.sectorEpoch;
	CurrentRun.readySectorEpoch[MyPlayerId] = CurrentRun.sectorEpoch;
	if (!Cooperative()) CurrentRun.transitionPending = false;
	CurrentRun.arenaInitialized = false;
	CurrentRun.souls.clear();
	ResetSurvivorsEncircleMarch();
	CurrentRun.dead.fill(false);
	CurrentRun.elite = -1;
	CurrentRun.boss = -1;
	for (auto &peer : CurrentRun.peers) {
		peer.damageGrace = { };
		peer.graceOwnsInvulnerability = false;
	}
}

bool ChooseSurvivorsUpgrade(unsigned choice)
{
	SelectPeer(MyPlayerId);
	if (!IsSurvivorsUpgradePending() || CurrentRun.finished || choice > 2) return false;
	if (Cooperative() && !Authority()) return SubmitCoopDraft(SurvivorsCoopDraftAction::Choose, choice);
	if (Cooperative() && MyPlayer != nullptr && MyPlayer->_pHitPoints <= 0) return false;
	if (!ChooseSurvivorsArsenalUpgrade(choice)) return false;
	ConsumeUpgrade();
	return true;
}

bool RerollSurvivorsUpgrades()
{
	SelectPeer(MyPlayerId);
	if (!IsSurvivorsUpgradePending()) return false;
	if (Cooperative() && !Authority()) return SubmitCoopDraft(SurvivorsCoopDraftAction::Reroll);
	if (!RerollSurvivorsArsenal()) return false;
	++CurrentPeer().draftSequence;
	return true;
}
bool BanishSurvivorsUpgrade(unsigned choice)
{
	SelectPeer(MyPlayerId);
	if (!IsSurvivorsUpgradePending() || choice > 2) return false;
	if (Cooperative() && !Authority()) return SubmitCoopDraft(SurvivorsCoopDraftAction::Banish, choice);
	if (!BanishSurvivorsArsenalOffer(choice)) return false;
	++CurrentPeer().draftSequence;
	return true;
}
bool SkipSurvivorsUpgrade()
{
	SelectPeer(MyPlayerId);
	if (!IsSurvivorsUpgradePending()) return false;
	if (Cooperative() && !Authority()) return SubmitCoopDraft(SurvivorsCoopDraftAction::Skip);
	if (!CanSkipSurvivorsArsenalDraft()) return false;
	SkipSurvivorsArsenalDraft();
	ConsumeUpgrade();
	return true;
}

bool ChooseSurvivorsUpgradeAt(Point position, Size screenSize)
{
	const int choice = SurvivorsUpgradeChoiceAt(position, screenSize);
	return choice >= 0 && ChooseSurvivorsUpgrade(static_cast<unsigned>(choice));
}

void ProcessSurvivorsMode()
{
	SelectPeer(MyPlayerId);
	if (!CurrentRun.enabled || CampShop || MyPlayer == nullptr || (gbIsMultiplayer && !Cooperative())) return;
	ProcessCoopNetworking();
	if (Cooperative() && !Authority()) {
		if (CurrentRun.soulEpoch != SurvivorsRollingEpoch()) { CurrentRun.souls.clear(); CurrentRun.soulEpoch = SurvivorsRollingEpoch(); }
		SurvivorsRollingWarnIfTethered();
		if (CurrentRun.finished) SettleRun();
		return;
	}
	if (leveltype == DTYPE_TOWN || CurrentRun.finished || CurrentRun.transitionPending || (!Cooperative() && CurrentPeer().upgradePending)) return;
	if (!CurrentRun.initialized) {
		CurrentRun.initialized = true;
		CurrentRun.tickRate = std::max(1U, static_cast<unsigned>(sgGameInitInfo.nTickRate));
		CurrentPeer().bonuses = GetSurvivorsRunBonuses();
		StartSurvivorsArsenal(*MyPlayer, CurrentPeer().bonuses, sgGameInitInfo.gameSeed[0] ^ sgGameInitInfo.gameSeed[1] ^ 0x53555256U);
		GrantSurvivorsArsenalLife(SurvivorsStartingLifeBonus);
		CurrentPeer().started = true;
	}
	if (!CurrentRun.arenaInitialized) InitializeArena();
	if (Cooperative()) {
		for (unsigned peer = 0; peer < MAX_PLRS; ++peer) {
			if (peer == MyPlayerId || !CurrentRun.peers[peer].started || (peer < Players.size() && Players[peer].plractive)) continue;
			SelectPeer(peer);
			ResetSurvivorsArsenal();
			CurrentRun.peers[peer] = { };
		}
		SelectPeer(MyPlayerId);
	}
	bool living = false;
	for (unsigned peer = 0; peer < MAX_PLRS; ++peer) {
		if (CurrentRun.peers[peer].started && ActivePeer(peer) && PeerPlayer(peer)._pHitPoints > 0) living = true;
	}
	if (!living) {
		CurrentRun.finished = true;
		SendCoopState();
		SettleRun();
		return;
	}
	// The authoritative host's viewport must not put remote players' hunters
	// to sleep. Survivors enemies keep pursuing their current living target.
	for (size_t index = 0; index < ActiveMonsterCount; ++index) {
		Monster &monster = Monsters[ActiveMonsters[index]];
		if (monster.isInvalid || monster.hitPoints <= 0 || (monster.flags & MFLAG_GOLEM) != 0) continue;
		monster.activeForTicks = 255;
		if ((monster.flags & MFLAG_TARGETS_MONSTER) == 0 && monster.enemy < Players.size())
			monster.position.last = Players[monster.enemy].position.future;
	}
	TrackKills();
	// Credit deaths before rebasing can park or retire their monster slots.
	UpdateSurvivorsRollingWorld(CurrentRun.souls, CurrentRun.boss, CurrentRun.elite);
	CollectSouls();
	if (!Cooperative() && CurrentPeer().upgradePending) return;
	if (TrySurvivorsSectorTransition()) return;
	++CurrentRun.ticks;
	++CurrentRun.actTicks;
	for (unsigned peer = 0; peer < MAX_PLRS; ++peer) {
		if (!CurrentRun.peers[peer].started || !ActivePeer(peer)) continue;
		auto &state = CurrentRun.peers[peer];
		Player &player = PeerPlayer(peer);
		if (CurrentRun.ticks < state.damageGrace.until) {
			player._pInvincible = true;
			state.graceOwnsInvulnerability = true;
		} else if (state.graceOwnsInvulnerability) {
			player._pInvincible = false;
			state.graceOwnsInvulnerability = false;
		}
	}
	const unsigned wave = SurvivorsWaveForSeconds(CurrentRun.campaign.act * SurvivorsActDurationSeconds + std::min(ActSeconds(), SurvivorsActDurationSeconds - 1));
	if (wave != CurrentRun.wave) {
		CurrentRun.wave = wave;
		CurrentRun.nextElite = true;
		EventPlrMsg(std::format("Wave {}: the darkness grows stronger.", wave));
	}
	AdvanceCampaign();
	if (CurrentRun.finished || CurrentRun.transitionPending) { SendCoopState(); return; }
	if (CurrentRun.campaign.bossGate && CurrentRun.boss < 0 && !CurrentRun.bossDefeated) SpawnWaveMonster(true);
	const unsigned interval = std::max(1U, SurvivorsSpawnInterval(CurrentRun.wave) * CurrentRun.tickRate / SurvivorsTicksPerSecond);
	if (ActSeconds() > 15 && CurrentRun.actTicks % (CurrentRun.tickRate * 30) == 0) CurrentRun.nextElite = true;
	if (ActSeconds() >= 5 && CurrentRun.ticks % interval == 0)
		for (unsigned count = 0; count < SurvivorsSpawnBatch(CurrentRun.wave); ++count) SpawnWaveMonster();
	ProcessEncircle();
	ProcessSurvivorsEncircleMarch(CurrentRun.ticks);
	ProcessBoss();
	for (unsigned peer = 0; peer < MAX_PLRS; ++peer) {
		if (!CurrentRun.peers[peer].started || !ActivePeer(peer)) continue;
		SelectPeer(peer);
		ProcessSurvivorsArsenal(CurrentRun.ticks, CurrentRun.tickRate, CurrentPeer().level);
	}
	SelectPeer(MyPlayerId);
	SendCoopActions();
	if (CurrentRun.ticks % std::max(1U, CurrentRun.tickRate / 5) == 0) SendCoopState();
}

void DrawSurvivorsMode(const Surface &out)
{
	SelectPeer(MyPlayerId);
	if (!CurrentRun.enabled)
		return;
	if (CampShop) {
		const int width = std::min(420, out.w() - 24);
		const int left = (out.w() - width) / 2;
		DrawHalfTransparentRectTo(out, left, 8, width, 46);
		DrawString(out, "SURVIVORS CAMP SHOP", Rectangle { { left + 8, 12 }, { width - 16, 18 } }, { .flags = UiFlags::ColorGold | UiFlags::AlignCenter });
		DrawString(out, "V or Esc: return to camp", Rectangle { { left + 8, 30 }, { width - 16, 18 } }, { .flags = UiFlags::AlignCenter });
		return;
	}
	// Soul marks are anchored to the world. The isometric projection only
	// matches the image under the UI layer while the game draws the isometric
	// world itself; under a 3D view (embedded World3D, first/third person) those
	// positions float as stray palette-coloured dots that slide while moving.
	// Embedded World3D projects through the host camera instead.
	const bool isometricWorld = !fpv::SkipIsometricWorld();
	for (const Soul &soul : CurrentRun.souls) {
		if (!IsTileVisible(soul.tile))
			continue;
		Point projected;
		if (isometricWorld)
			projected = GetScreenPosition(soul.tile) + Displacement { 32, -16 };
		else if (!host::DrawsWorld3D() || !host::ProjectWorldTile(soul.tile, 0.25F, projected))
			continue;
		const Point screen = ScreenToUi(projected);
		if (screen.x < 4 || screen.y < 4 || screen.x >= out.w() - 4 || screen.y >= out.h() - 4)
			continue;
		for (int row = -3; row <= 3; ++row) {
			const int half = 3 - std::abs(row);
			DrawHorizontalLine(out, { screen.x - half, screen.y + row }, half * 2 + 1, PAL8_BLUE + 2);
		}
	}
	const int width = std::min(600, out.w() - 24);
	const int left = (out.w() - width) / 2;
	const unsigned gearSync = SurvivorsCoopGearSyncStatus();
	if (!host::HideHud) {
		DrawHalfTransparentRectTo(out, left, 8, width, gearSync != 0 ? 88 : 70);
		DrawString(out, std::format("ACT {}/5: {} - {}", CurrentRun.campaign.act + 1, ActNames[CurrentRun.campaign.act], DifficultyNames[CurrentRun.difficulty]), Rectangle { { left + 8, 12 }, { width - 16, 18 } }, { .flags = UiFlags::ColorGold | UiFlags::AlignCenter });
		DrawString(out, std::format("{:02}:{:02} / 04:00{}   {} kills   Level {}   Total {:02}:{:02}{}", ActSeconds() / 60, ActSeconds() % 60, CurrentRun.campaign.bossGate ? " BOSS" : "", CurrentRun.kills, CurrentPeer().level, Seconds() / 60, Seconds() % 60, MyPlayer->_pStatPts > 0 ? std::format("   C: {} stats", MyPlayer->_pStatPts) : ""), Rectangle { { left + 8, 32 }, { width - 16, 18 } }, { .flags = UiFlags::AlignCenter });
		const unsigned goal = SurvivorsExperienceForLevel(CurrentPeer().level);
		FillRect(out, left + 8, 52, width - 16, 4, PAL8_BLUE + 7);
		FillRect(out, left + 8, 52, static_cast<int>((width - 16) * std::min(CurrentPeer().experience, goal) / goal), 4, PAL8_BLUE + 1);
		DrawString(out, std::format("{}{}{}", SurvivorsArsenalSummary(), SurvivorsRollingActive() ? "" : "   N: Travel at edge", gbIsMultiplayer ? "" : "   V: Shop"), Rectangle { { left + 8, 60 }, { width - 16, 18 } }, { .flags = UiFlags::AlignCenter });
		if (gearSync != 0) {
			const char *status = gearSync == 1 ? "Gear: syncing - wait before changing hosts" : gearSync == 2 ? "Gear: latest received checkpoint saved" : gearSync == 3 ? "Gear: session only" : "Gear: saved on this host";
			DrawString(out, status, Rectangle { { left + 8, 78 }, { width - 16, 18 } }, { .flags = UiFlags::AlignCenter | (gearSync == 1 ? UiFlags::ColorGold : UiFlags::ColorUiSilver) });
		}
	}
	const int bossId = CurrentRun.boss >= 0 ? CurrentRun.boss : CurrentRun.elite;
	if (bossId >= 0) {
		const Monster &boss = Monsters[bossId];
		if (!boss.isInvalid && boss.hitPoints > 0)
			// The imported host HUD has its own top banner. Put the boss label
			// immediately below it instead of drawing through its title.
			DrawString(out, std::format("{} {}/{} HP", CurrentRun.boss >= 0 ? BossNames[CurrentRun.campaign.act] : "SOUL REAPER", boss.hitPoints >> 6, boss.maxHitPoints >> 6), Rectangle { { left, host::HideHud ? std::max(96, out.h() / 8 + 8) : (gearSync != 0 ? 100 : 82) }, { width, 20 } }, { .flags = UiFlags::ColorRed | UiFlags::AlignCenter });
	}
	if (gmenu_is_active() || IsVisualStoreOpen || (!CurrentPeer().upgradePending && !CurrentRun.finished))
		return;
	const int top = std::max(70, out.h() / 2 - 85);
	if (CurrentRun.finished) DrawHalfTransparentRectTo(out, left, top, width, 190);
	if (CurrentRun.finished) {
		DrawString(out, CurrentRun.campaign.victory ? "DAMNATION CONQUERED" : "YOUR WATCH HAS ENDED", Rectangle { { left + 8, top + 14 }, { width - 16, 30 } }, { .flags = UiFlags::ColorGold | UiFlags::AlignCenter });
		DrawString(out, std::format("{} acts   {} bosses   {} kills   Level {}\n{}\n{}Esc or controller A/B/Start: Survivors Camp.", CurrentRun.campaign.actsCleared, CurrentRun.bosses, CurrentRun.kills, CurrentPeer().level, SurvivorsProgressionStatus(), gbIsMultiplayer ? "" : "V: Shop   "), Rectangle { { left + 16, top + 58 }, { width - 32, 120 } }, { .flags = UiFlags::AlignCenter });
	} else {
		const Size screen { out.w(), out.h() };
		const auto panel = SurvivorsUpgradePanelRect(screen);
		DrawHalfTransparentRectTo(out, panel.x, panel.y, panel.width, panel.height);
		DrawString(out, std::format("SOUL ASCENSION - LEVEL {}", CurrentPeer().level + 1), Rectangle { { panel.x + 12, panel.y + 8 }, { panel.width - 24, 20 } }, { .flags = UiFlags::ColorGold | UiFlags::AlignCenter });
		DrawString(out, "Choose a weapon or passive. Gifts restore life.", Rectangle { { panel.x + 12, panel.y + 28 }, { panel.width - 24, 16 } }, { .flags = UiFlags::AlignCenter });
		const auto offers = SurvivorsArsenalOfferDetails();
		const auto arsenal = GetSurvivorsArsenalSnapshot();
		const bool busy = IsSurvivorsDraftRequestPending();
		auto box = [&](SurvivorsUpgradeRect rect, bool enabled) {
			FillRect(out, rect.x, rect.y, rect.width, rect.height, PAL8_BLUE + 7);
			const uint8_t border = enabled ? PAL8_YELLOW + 2 : PAL8_BLUE + 5;
			DrawHorizontalLine(out, { rect.x, rect.y }, rect.width, border);
			DrawHorizontalLine(out, { rect.x, rect.y + rect.height - 1 }, rect.width, border);
			DrawVerticalLine(out, { rect.x, rect.y }, rect.height, border);
			DrawVerticalLine(out, { rect.x + rect.width - 1, rect.y }, rect.height, border);
		};
		for (unsigned choice = 0; choice < offers.size(); ++choice) {
			const auto &offer = offers[choice];
			const auto card = SurvivorsUpgradeCardRect(choice, screen);
			box(card, offer.enabled && !busy);
			DrawString(out, WordWrapString(offer.name, card.width - 16), Rectangle { { card.x + 8, card.y + 10 }, { card.width - 16, 32 } }, { .flags = UiFlags::ColorGold | UiFlags::AlignCenter });
			const std::string rank = offer.evolution ? "EVOLUTION" : std::format("{}  {} -> {} / {}", offer.isWeapon ? "WEAPON" : "PASSIVE", offer.currentRank, offer.nextRank, offer.maxRank);
			DrawString(out, rank, Rectangle { { card.x + 8, card.y + 46 }, { card.width - 16, 18 } }, { .flags = UiFlags::ColorGold | UiFlags::AlignCenter });
			DrawString(out, WordWrapString(offer.description, card.width - 20), Rectangle { { card.x + 10, card.y + 70 }, { card.width - 20, 50 } });
			DrawString(out, WordWrapString(offer.requirements, card.width - 20), Rectangle { { card.x + 10, card.y + 123 }, { card.width - 20, 42 } }, { .flags = UiFlags::ColorWhitegold });
			const auto banish = SurvivorsUpgradeBanishRect(choice, screen);
			const bool canBanish = arsenal.banishes > 0 && offer.item < SurvivorsArsenalFirstFallback && !busy;
			box(banish, canBanish);
			DrawString(out, std::format("Banish [{}]  Shift+{}", arsenal.banishes, choice + 1), Rectangle { { banish.x + 2, banish.y + 5 }, { banish.width - 4, 16 } }, { .flags = (canBanish ? UiFlags::ColorGold : UiFlags::ColorUiSilverDark) | UiFlags::AlignCenter });
		}
		const auto reroll = SurvivorsUpgradeRerollRect(screen);
		const auto skip = SurvivorsUpgradeSkipRect(screen);
		box(reroll, arsenal.rerolls > 0 && !busy);
		box(skip, arsenal.skips > 0 && !busy);
		DrawString(out, std::format("Reroll [{}]", arsenal.rerolls), Rectangle { { reroll.x + 2, reroll.y + 8 }, { reroll.width - 4, 18 } }, { .flags = (arsenal.rerolls > 0 && !busy ? UiFlags::ColorGold : UiFlags::ColorUiSilverDark) | UiFlags::AlignCenter });
		DrawString(out, std::format("Skip [{}]", arsenal.skips), Rectangle { { skip.x + 2, skip.y + 8 }, { skip.width - 4, 18 } }, { .flags = (arsenal.skips > 0 && !busy ? UiFlags::ColorGold : UiFlags::ColorUiSilverDark) | UiFlags::AlignCenter });
		DrawString(out, busy ? "Waiting for the host to confirm your choice..." : "1/2/3 or A/X/Y  |  R/RB reroll  |  Space/B skip", Rectangle { { panel.x + 12, panel.y + 308 }, { panel.width - 24, 18 } }, { .flags = UiFlags::AlignCenter });
	}
}

} // namespace devilution
