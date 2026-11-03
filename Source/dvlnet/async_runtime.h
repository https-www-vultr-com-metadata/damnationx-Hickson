#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <span>

#include "engine/displacement.hpp"
#include "engine/point.hpp"

namespace devilution {
// A joining client needs time to receive and install its first authority
// snapshot even after its local level loader has finished.
constexpr uint32_t AsyncNetAuthorityTimeoutMs(uint64_t installedSnapshots, bool changingLevel)
{
	return installedSnapshots == 0 || changingLevel ? 30000U : 10000U;
}
constexpr bool AsyncNetNeedsInitialSnapshot(bool enabled, bool authority, uint64_t installedSnapshots)
{
	return enabled && !authority && installedSnapshots == 0;
}
struct Player;
struct Monster;
enum class SpellID : int8_t;
struct AsyncTargetedSpellStatus {
	uint8_t spell = 0;
	uint32_t token = 0;
	bool ready = false;
};
// Native instant spell effects issue one lease after casting is validated.
void AsyncNetOfferTargetedSpell(Player &player, SpellID spell);
AsyncTargetedSpellStatus GetAsyncTargetedSpellStatus(const Player &player);
bool AsyncNetTargetItem(Player &player, SpellID spell, unsigned inventoryIndex);

// Negotiated through GameData::reserved[0]. Legacy rooms retain lockstep.
bool AsyncNetEnabled();
bool AsyncNetIsAuthority();
bool AsyncNetOwnsPlayer(const Player &player);
bool AsyncNetSimulateWorld();
bool AsyncNetAllowLevelChange();
bool AsyncNetIsRemoteInteraction();
void AsyncNetInteractWithTowner(Player &player, int npc);
void AsyncNetInitialize();
void AsyncNetShutdown();
// Called once before input and once after an authoritative simulation tick.
void AsyncNetBeforeTick();
void AsyncNetAfterTick();
// A Survivors rolling-world rebase invalidates local-coordinate rewind data.
// Called by the authority immediately after a shift.
void AsyncNetOnRollingOriginChanged();
// Client-only walking prediction; never advances combat or world RNG.
void AsyncNetPredictLocalPlayer();
void AsyncNetAdvancePredictedPlayer(Player &player);
bool AsyncNetSendCommand(std::span<const std::byte> command);
bool AsyncNetUseInventoryItem(unsigned slot);
bool AsyncNetSurvivorsEquip(unsigned index, uint32_t seed);
enum class AsyncStoreAction : uint8_t { SmithBuy,
	PremiumBuy,
	WitchBuy,
	HealerBuy,
	BoyBuy,
	SmithSell,
	WitchSell,
	Repair,
	Recharge,
	Identify,
	RepairAll,
	SellHeldSmith,
	SellHeldWitch,
	SurvivorsBuyback };
bool AsyncNetStoreTransaction(AsyncStoreAction action, unsigned index, uint32_t seed);
// Called only by the authority after ordered input validation.
bool ApplyAsyncStoreTransaction(Player &player, AsyncStoreAction action, unsigned index, uint32_t seed);

bool AsyncNetHandlePacket(uint8_t sender, std::span<const std::byte> packet);
// Render-only offset; simulation and collision always use authoritative state.
Displacement AsyncNetRenderOffset(const Player &player);
Displacement AsyncNetRenderOffset(const Monster &monster);
bool AsyncNetMeleeReach(const Player &player, const Monster &monster, unsigned range);

using AsyncNetExtensionHandler = std::function<void(uint8_t sender, std::span<const std::byte>)>;
// Extension payload limit: 384 bytes; host broadcasts, clients send only to host.
bool AsyncNetSendExtension(uint8_t channel, std::span<const std::byte> payload);
bool AsyncNetSendReliableExtension(uint8_t channel, std::span<const std::byte> payload);
bool AsyncNetHasReliablePending();
void AsyncNetSetExtensionHandler(uint8_t channel, AsyncNetExtensionHandler handler);

struct AsyncNetStatus {
	bool enabled;
	bool authority;
	bool healthy;
	uint32_t tick;
	uint32_t latestSnapshot;
	uint32_t pendingInputs;
	uint64_t snapshots;
	uint64_t retransmits;
};
AsyncNetStatus GetAsyncNetStatus();
bool AsyncNetAwaitingInitialSnapshot();
} // namespace devilution
