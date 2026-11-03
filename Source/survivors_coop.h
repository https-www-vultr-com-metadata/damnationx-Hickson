#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

#include "survivors_arsenal.h"
#include "levels/survivors_sector.hpp"

namespace devilution {

inline constexpr uint8_t SurvivorsCoopChannel = 5;
inline constexpr uint8_t SurvivorsCoopVersion = 7;
constexpr bool SurvivorsCoopAdmissionMatches(uint64_t expected, uint64_t submitted)
{
	return expected != 0 && expected == submitted;
}
constexpr bool SurvivorsCoopUsesDurableLedger(bool authoritySupportsPersistence, bool emptyIdentity)
{
	return authoritySupportsPersistence && !emptyIdentity;
}
// A durable ground checkpoint must never transfer an item into or out of a
// session-only inventory: the two sides could not be recovered atomically.
constexpr bool SurvivorsCoopGroundTransferAllowed(bool authoritySupportsPersistence, bool peerHasDurableLedger)
{
	return !authoritySupportsPersistence || peerHasDurableLedger;
}
constexpr bool SurvivorsCoopCheckpointIsCurrent(uint64_t authorityRevision, uint64_t savedRevision, uint32_t pendingInputs, uint64_t stateAgeMilliseconds)
{
	return authorityRevision != 0 && savedRevision == authorityRevision && pendingInputs == 0 && stateAgeMilliseconds < 2000;
}
// Moving the act clock forward must move the run clock with it: an act can
// never have lasted longer than the whole run, and peers discard any state
// snapshot claiming otherwise (leaving their sync indicators stale).
struct SurvivorsCoopClock {
	uint32_t ticks;
	uint32_t actTicks;
};
constexpr SurvivorsCoopClock SurvivorsCoopAdvanceActClock(uint32_t ticks, uint32_t actTicks, uint32_t targetActTicks)
{
	if (targetActTicks <= actTicks) return { ticks, actTicks };
	const uint32_t delta = targetActTicks - actTicks;
	return { ticks > UINT32_MAX - delta ? UINT32_MAX : ticks + delta, targetActTicks };
}
// Transport acknowledgement does not mean the authority's game state was
// ready to accept the admission. Replay the whole upload at a bounded rate
// until the authority reports that the peer has started.
constexpr bool SurvivorsCoopShouldRetryAdmission(bool helloQueued, size_t uploaded, size_t total, bool reliablePending, uint64_t now, uint64_t retryAt)
{
	return helloQueued && uploaded == total && !reliablePending && now >= retryAt;
}
// New kinds keep SurvivorsCoopVersion: older peers ignore packet kinds they do not know.
enum class SurvivorsCoopPacket : uint8_t { State = 1, Hello = 2, Draft = 3, Souls = 4, SectorReady = 5, Checkpoint = 6, CheckpointAck = 7, Actions = 8 };
enum class SurvivorsCoopDraftAction : uint8_t { Choose, Reroll, Skip, Banish };
struct SurvivorsCoopPeerState {
	bool started = false;
	bool upgradePending = false;
	bool durableLedger = false;
	uint64_t admissionNonce = 0;
	uint64_t ledgerGeneration = 0;
	unsigned level = 1;
	unsigned experience = 0;
	uint32_t draftSequence = 0;
	uint32_t acknowledgedRequest = 0;
	uint32_t startingKills = 0;
	uint32_t startingActs = 0;
	uint32_t startingBosses = 0;
	uint32_t startingSeconds = 0;
	SurvivorsArsenalSnapshot arsenal;
};
struct SurvivorsCoopState {
	uint32_t sequence = 0;
	uint32_t ticks = 0;
	uint32_t actTicks = 0;
	uint32_t kills = 0;
	uint32_t sectorEpoch = 0;
	SurvivorsSector sector;
	unsigned tickRate = 20;
	unsigned difficulty = 0;
	unsigned act = 0;
	unsigned actsCleared = 0;
	unsigned bosses = 0;
	int boss = -1;
	int elite = -1;
	uint8_t flags = 0;
	std::array<SurvivorsCoopPeerState, 4> peers;
};
struct SurvivorsCoopDraftRequest {
	uint32_t request = 0;
	uint32_t draftSequence = 0;
	SurvivorsCoopDraftAction action = SurvivorsCoopDraftAction::Choose;
	unsigned choice = 0;
};

// Little-endian, fixed-width wire fields; never serialize native object padding.
class SurvivorsCoopWriter {
public:
	std::vector<std::byte> data;
	void Byte(unsigned value) { data.push_back(static_cast<std::byte>(value & 0xff)); }
	void Word(unsigned value) { Byte(value); Byte(value >> 8); }
	void Dword(uint32_t value) { Word(value); Word(value >> 16); }
};
class SurvivorsCoopReader {
public:
	explicit SurvivorsCoopReader(std::span<const std::byte> bytes) : data(bytes) { }
	std::span<const std::byte> data;
	bool valid = true;
	unsigned Byte() { if (data.empty()) { valid = false; return 0; } const unsigned result = std::to_integer<unsigned>(data.front()); data = data.subspan(1); return result; }
	unsigned Word() { const unsigned low = Byte(); return low | (Byte() << 8); }
	uint32_t Dword() { const uint32_t low = Word(); return low | (static_cast<uint32_t>(Word()) << 16); }
	bool Complete() const { return valid && data.empty(); }
};

inline bool IsValidSurvivorsCoopArsenal(const SurvivorsArsenalSnapshot &state)
{
	if (state.rerolls > 13 || state.skips > 2 || state.banishes > 2) return false;
	unsigned weapons = 0;
	for (unsigned rank : state.ranks) { if (rank > SurvivorsWeaponMaxRank) return false; if (rank != 0) ++weapons; }
	if (weapons > SurvivorsWeaponSlots) return false;
	for (unsigned rank : state.passives) if (rank > SurvivorsPassiveMaxRank) return false;
	for (unsigned offer : state.offers) if (offer > SurvivorsArsenalInvalidOffer) return false;
	return true;
}

inline std::vector<std::byte> EncodeSurvivorsCoopState(const SurvivorsCoopState &state)
{
	SurvivorsCoopWriter out;
	out.Byte(SurvivorsCoopVersion); out.Byte(static_cast<unsigned>(SurvivorsCoopPacket::State));
	out.Dword(state.sequence); out.Dword(state.ticks); out.Dword(state.actTicks); out.Dword(state.kills);
	out.Dword(state.sectorEpoch); out.Dword(static_cast<uint32_t>(state.sector.x)); out.Dword(static_cast<uint32_t>(state.sector.y));
	out.Word(state.tickRate); out.Byte(state.difficulty); out.Byte(state.act); out.Byte(state.actsCleared); out.Byte(state.bosses);
	out.Word(static_cast<unsigned>(state.boss + 1)); out.Word(static_cast<unsigned>(state.elite + 1)); out.Byte(state.flags);
	for (const auto &peer : state.peers) {
		out.Byte((peer.started ? 1 : 0) | (peer.upgradePending ? 2 : 0) | (peer.durableLedger ? 4 : 0)); out.Word(peer.level); out.Dword(peer.experience);
		out.Dword(static_cast<uint32_t>(peer.admissionNonce)); out.Dword(static_cast<uint32_t>(peer.admissionNonce >> 32));
		out.Dword(static_cast<uint32_t>(peer.ledgerGeneration)); out.Dword(static_cast<uint32_t>(peer.ledgerGeneration >> 32));
		out.Dword(peer.draftSequence); out.Dword(peer.acknowledgedRequest);
		out.Dword(peer.startingKills); out.Dword(peer.startingActs); out.Dword(peer.startingBosses); out.Dword(peer.startingSeconds);
		for (unsigned rank : peer.arsenal.ranks) out.Byte(rank);
		for (unsigned rank : peer.arsenal.passives) out.Byte(rank);
		unsigned banished = 0;
		for (unsigned index = 0; index < peer.arsenal.banished.size(); ++index) if (peer.arsenal.banished[index]) banished |= 1U << index;
		out.Word(banished);
		for (unsigned offer : peer.arsenal.offers) out.Byte(offer);
		out.Byte(peer.arsenal.rerolls); out.Byte(peer.arsenal.skips); out.Byte(peer.arsenal.banishes); out.Byte(peer.arsenal.drafted ? 1 : 0);
	}
	return out.data;
}

inline std::optional<SurvivorsCoopState> DecodeSurvivorsCoopState(std::span<const std::byte> bytes)
{
	SurvivorsCoopReader in(bytes);
	if (in.Byte() != SurvivorsCoopVersion || in.Byte() != static_cast<unsigned>(SurvivorsCoopPacket::State)) return std::nullopt;
	SurvivorsCoopState state;
	state.sequence = in.Dword(); state.ticks = in.Dword(); state.actTicks = in.Dword(); state.kills = in.Dword();
	state.sectorEpoch = in.Dword(); state.sector.x = static_cast<int32_t>(in.Dword()); state.sector.y = static_cast<int32_t>(in.Dword());
	state.tickRate = in.Word(); state.difficulty = in.Byte(); state.act = in.Byte(); state.actsCleared = in.Byte(); state.bosses = in.Byte();
	state.boss = static_cast<int>(in.Word()) - 1; state.elite = static_cast<int>(in.Word()) - 1; state.flags = static_cast<uint8_t>(in.Byte());
	if (!IsValidSurvivorsSector(state.sector) || state.sequence == 0 || state.tickRate == 0 || state.tickRate > 120 || state.difficulty > 2 || state.act >= 5 || state.actsCleared > 5 || state.bosses > 5 || state.boss < -1 || state.boss >= 200 || state.elite < -1 || state.elite >= 200 || state.flags > 31 || state.kills > 1000000 || state.ticks > state.tickRate * 86400 || state.actTicks > state.ticks) return std::nullopt;
	for (auto &peer : state.peers) {
		const unsigned flags = in.Byte(); if (flags > 7) return std::nullopt;
		peer.started = (flags & 1) != 0; peer.upgradePending = (flags & 2) != 0; peer.level = in.Word(); peer.experience = in.Dword();
		peer.durableLedger = (flags & 4) != 0;
		const uint64_t lowNonce = in.Dword(); peer.admissionNonce = lowNonce | (static_cast<uint64_t>(in.Dword()) << 32);
		const uint64_t lowGeneration = in.Dword(); peer.ledgerGeneration = lowGeneration | (static_cast<uint64_t>(in.Dword()) << 32);
		peer.draftSequence = in.Dword(); peer.acknowledgedRequest = in.Dword();
		peer.startingKills = in.Dword(); peer.startingActs = in.Dword(); peer.startingBosses = in.Dword(); peer.startingSeconds = in.Dword();
		for (auto &rank : peer.arsenal.ranks) rank = in.Byte();
		for (auto &rank : peer.arsenal.passives) rank = in.Byte();
		const unsigned banished = in.Word();
		for (unsigned index = 0; index < peer.arsenal.banished.size(); ++index) peer.arsenal.banished[index] = (banished & (1U << index)) != 0;
		for (auto &offer : peer.arsenal.offers) offer = in.Byte();
		peer.arsenal.rerolls = in.Byte(); peer.arsenal.skips = in.Byte(); peer.arsenal.banishes = in.Byte();
		const unsigned drafted = in.Byte(); peer.arsenal.drafted = drafted != 0;
		if ((peer.started && peer.admissionNonce == 0) || peer.startingKills > state.kills || peer.startingActs > state.actsCleared || peer.startingBosses > state.bosses || peer.startingSeconds > state.ticks / state.tickRate || drafted > 1 || peer.level == 0 || peer.level > 1000 || peer.experience > 1000000 || !IsValidSurvivorsCoopArsenal(peer.arsenal)) return std::nullopt;
	}
	return in.Complete() ? std::optional { state } : std::nullopt;
}

inline std::vector<std::byte> EncodeSurvivorsCoopDraft(const SurvivorsCoopDraftRequest &request)
{
	SurvivorsCoopWriter out;
	out.Byte(SurvivorsCoopVersion); out.Byte(static_cast<unsigned>(SurvivorsCoopPacket::Draft));
	out.Dword(request.request); out.Dword(request.draftSequence); out.Byte(static_cast<unsigned>(request.action)); out.Byte(request.choice);
	return out.data;
}
inline std::optional<SurvivorsCoopDraftRequest> DecodeSurvivorsCoopDraft(std::span<const std::byte> bytes)
{
	SurvivorsCoopReader in(bytes);
	if (in.Byte() != SurvivorsCoopVersion || in.Byte() != static_cast<unsigned>(SurvivorsCoopPacket::Draft)) return std::nullopt;
	SurvivorsCoopDraftRequest request;
	request.request = in.Dword(); request.draftSequence = in.Dword();
	const unsigned action = in.Byte(); request.action = static_cast<SurvivorsCoopDraftAction>(action); request.choice = in.Byte();
	if (!in.Complete() || request.request == 0 || action > 3 || request.choice > 2) return std::nullopt;
	return request;
}
inline bool SurvivorsCoopDraftRequestIsFresh(const SurvivorsCoopPeerState &peer, const SurvivorsCoopDraftRequest &request)
{
	return peer.started && peer.upgradePending && peer.arsenal.drafted && request.request > peer.acknowledgedRequest && request.draftSequence == peer.draftSequence;
}

// Survivors attacks and casts are resolved by the authority without a player
// mode, so clients learn of them only through this cosmetic packet: which
// player started which animation (player_graphic Attack/Lightning/Fire/Magic)
// facing which direction, stamped with the authority tick that started it.
struct SurvivorsCoopAction {
	uint8_t player = 0;
	uint8_t graphic = 0;
	uint8_t direction = 0;
};
struct SurvivorsCoopActions {
	uint32_t ticks = 0;
	uint8_t count = 0;
	std::array<SurvivorsCoopAction, 4> actions;
};
constexpr bool IsSurvivorsCoopActionGraphic(unsigned graphic)
{
	// player_graphic::Attack, Lightning, Fire, Magic.
	return graphic == 2 || graphic == 4 || graphic == 5 || graphic == 6;
}
inline std::vector<std::byte> EncodeSurvivorsCoopActions(const SurvivorsCoopActions &packet)
{
	SurvivorsCoopWriter out;
	out.Byte(SurvivorsCoopVersion); out.Byte(static_cast<unsigned>(SurvivorsCoopPacket::Actions));
	out.Dword(packet.ticks); out.Byte(packet.count);
	for (unsigned index = 0; index < packet.count && index < packet.actions.size(); ++index) {
		const SurvivorsCoopAction &action = packet.actions[index];
		out.Byte(action.player); out.Byte(action.graphic); out.Byte(action.direction);
	}
	return out.data;
}
inline std::optional<SurvivorsCoopActions> DecodeSurvivorsCoopActions(std::span<const std::byte> bytes)
{
	SurvivorsCoopReader in(bytes);
	if (in.Byte() != SurvivorsCoopVersion || in.Byte() != static_cast<unsigned>(SurvivorsCoopPacket::Actions)) return std::nullopt;
	SurvivorsCoopActions packet;
	packet.ticks = in.Dword();
	const unsigned count = in.Byte();
	if (count == 0 || count > packet.actions.size()) return std::nullopt;
	packet.count = static_cast<uint8_t>(count);
	for (unsigned index = 0; index < count; ++index) {
		SurvivorsCoopAction &action = packet.actions[index];
		action.player = static_cast<uint8_t>(in.Byte()); action.graphic = static_cast<uint8_t>(in.Byte());
		action.direction = static_cast<uint8_t>(in.Byte());
		if (action.player >= packet.actions.size() || !IsSurvivorsCoopActionGraphic(action.graphic) || action.direction >= 8) return std::nullopt;
		for (unsigned previous = 0; previous < index; ++previous)
			if (packet.actions[previous].player == action.player) return std::nullopt;
	}
	return in.Complete() ? std::optional { packet } : std::nullopt;
}

} // namespace devilution
