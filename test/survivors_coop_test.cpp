#include <gtest/gtest.h>

#include "player.h"
#include "survivors_arsenal.h"
#include "survivors_coop.h"

namespace devilution {
namespace {
TEST(SurvivorsCoopProtocol, FourIndependentDraftsRoundTripWithinTransportBudget)
{
	SurvivorsCoopState state;
	state.sequence = 42;
	state.ticks = 8000;
	state.actTicks = 1200;
	state.kills = 534;
	state.sectorEpoch = 5;
	state.sector = { -17, 31 };
	state.act = 2;
	state.actsCleared = 2;
	state.bosses = 2;
	state.boss = 17;
	for (unsigned peer = 0; peer < state.peers.size(); ++peer) {
		auto &entry = state.peers[peer];
		entry.started = true;
		entry.admissionNonce = 0x918200000ULL + peer;
		entry.ledgerGeneration = 0x719300000ULL + peer;
		entry.durableLedger = peer % 2 == 0;
		entry.upgradePending = true;
		entry.level = 10 + peer;
		entry.experience = 100 + peer;
		entry.draftSequence = peer + 4;
		entry.acknowledgedRequest = peer + 1;
		entry.arsenal.ranks[peer] = peer + 1;
		entry.arsenal.passives[peer] = 2;
		entry.arsenal.offers = { peer, 10 + peer, 16 };
		entry.arsenal.drafted = true;
	}
	const auto encoded = EncodeSurvivorsCoopState(state);
	ASSERT_LE(encoded.size(), 384U);
	const auto decoded = DecodeSurvivorsCoopState(encoded);
	ASSERT_TRUE(decoded);
	EXPECT_EQ(decoded->sequence, state.sequence);
	EXPECT_EQ(decoded->sectorEpoch, state.sectorEpoch);
	EXPECT_EQ(decoded->sector, state.sector);
	EXPECT_EQ(decoded->act, 2U);
	EXPECT_EQ(decoded->boss, 17);
	for (unsigned peer = 0; peer < state.peers.size(); ++peer) {
		EXPECT_EQ(decoded->peers[peer].level, 10 + peer);
		EXPECT_EQ(decoded->peers[peer].admissionNonce, state.peers[peer].admissionNonce);
		EXPECT_EQ(decoded->peers[peer].ledgerGeneration, state.peers[peer].ledgerGeneration);
		EXPECT_EQ(decoded->peers[peer].durableLedger, state.peers[peer].durableLedger);
		EXPECT_EQ(decoded->peers[peer].experience, 100 + peer);
		EXPECT_EQ(decoded->peers[peer].arsenal.ranks, state.peers[peer].arsenal.ranks);
		EXPECT_EQ(decoded->peers[peer].arsenal.offers, state.peers[peer].arsenal.offers);
	}
}

TEST(SurvivorsCoopProtocol, ReconnectingAdmissionRejectsOldInputNonce)
{
	constexpr uint64_t oldAdmission = 0x219a36bc1234ULL;
	constexpr uint64_t newAdmission = 0x821033cc9876ULL;
	EXPECT_TRUE(SurvivorsCoopAdmissionMatches(oldAdmission, oldAdmission));
	EXPECT_FALSE(SurvivorsCoopAdmissionMatches(newAdmission, oldAdmission));
	EXPECT_TRUE(SurvivorsCoopAdmissionMatches(newAdmission, newAdmission));
	EXPECT_FALSE(SurvivorsCoopAdmissionMatches(0, 0));
}

TEST(SurvivorsCoopProtocol, SessionOnlyFallbackNeverUsesTheDurableLedger)
{
	EXPECT_TRUE(SurvivorsCoopUsesDurableLedger(true, false));
	EXPECT_FALSE(SurvivorsCoopUsesDurableLedger(true, true));
	EXPECT_FALSE(SurvivorsCoopUsesDurableLedger(false, false));
	EXPECT_FALSE(SurvivorsCoopUsesDurableLedger(false, true));
}

TEST(SurvivorsCoopProtocol, GroundTransfersRequireDurablePeerOnDurableHost)
{
	EXPECT_TRUE(SurvivorsCoopGroundTransferAllowed(true, true));
	EXPECT_FALSE(SurvivorsCoopGroundTransferAllowed(true, false));
	EXPECT_TRUE(SurvivorsCoopGroundTransferAllowed(false, false));
	EXPECT_TRUE(SurvivorsCoopGroundTransferAllowed(false, true));
}

TEST(SurvivorsCoopProtocol, CheckpointIndicatorRequiresCurrentSavedRevisionAndNoPendingInput)
{
	EXPECT_TRUE(SurvivorsCoopCheckpointIsCurrent(7, 7, 0, 100));
	EXPECT_FALSE(SurvivorsCoopCheckpointIsCurrent(0, 0, 0, 0));
	EXPECT_FALSE(SurvivorsCoopCheckpointIsCurrent(8, 7, 0, 100));
	EXPECT_FALSE(SurvivorsCoopCheckpointIsCurrent(7, 8, 0, 100));
	EXPECT_FALSE(SurvivorsCoopCheckpointIsCurrent(7, 7, 1, 100));
	EXPECT_FALSE(SurvivorsCoopCheckpointIsCurrent(7, 7, 0, 2000));
}

TEST(SurvivorsCoopProtocol, FastForwardedActClockKeepsSnapshotsDecodable)
{
	// Thirteen seconds into a run, jump the act clock to the boss gate.
	SurvivorsCoopState state;
	state.sequence = 1;
	state.tickRate = 20;
	state.ticks = 13 * 20;
	state.actTicks = 13 * 20;
	const uint32_t gate = 239 * 20;
	// Moving only the act clock produces a snapshot every peer discards.
	SurvivorsCoopState actOnly = state;
	actOnly.actTicks = gate;
	EXPECT_FALSE(DecodeSurvivorsCoopState(EncodeSurvivorsCoopState(actOnly)));

	const auto clock = SurvivorsCoopAdvanceActClock(state.ticks, state.actTicks, gate);
	EXPECT_EQ(clock.actTicks, gate);
	EXPECT_EQ(clock.ticks, gate);
	state.ticks = clock.ticks;
	state.actTicks = clock.actTicks;
	const auto decoded = DecodeSurvivorsCoopState(EncodeSurvivorsCoopState(state));
	ASSERT_TRUE(decoded);
	EXPECT_EQ(decoded->actTicks, gate);

	// A later act keeps its earlier run time; never move either clock back.
	const auto later = SurvivorsCoopAdvanceActClock(9000, 100, gate);
	EXPECT_EQ(later.ticks, 9000U + gate - 100);
	EXPECT_EQ(later.actTicks, gate);
	const auto past = SurvivorsCoopAdvanceActClock(9000, gate + 5, gate);
	EXPECT_EQ(past.ticks, 9000U);
	EXPECT_EQ(past.actTicks, gate + 5);
	EXPECT_EQ(SurvivorsCoopAdvanceActClock(UINT32_MAX - 1, 0, 10).ticks, UINT32_MAX);
}

TEST(SurvivorsCoopProtocol, ActionOverlaysRoundTripCompactlyAndRejectMalformedPackets)
{
	static_assert(IsSurvivorsCoopActionGraphic(static_cast<unsigned>(player_graphic::Attack)));
	static_assert(IsSurvivorsCoopActionGraphic(static_cast<unsigned>(player_graphic::Lightning)));
	static_assert(IsSurvivorsCoopActionGraphic(static_cast<unsigned>(player_graphic::Fire)));
	static_assert(IsSurvivorsCoopActionGraphic(static_cast<unsigned>(player_graphic::Magic)));
	static_assert(!IsSurvivorsCoopActionGraphic(static_cast<unsigned>(player_graphic::Walk)));
	SurvivorsCoopActions packet;
	packet.ticks = 9001;
	packet.count = 4;
	packet.actions = { { { 0, 2, 3 }, { 1, 5, 7 }, { 2, 4, 0 }, { 3, 6, 1 } } };
	const auto encoded = EncodeSurvivorsCoopActions(packet);
	EXPECT_EQ(encoded[0], std::byte { SurvivorsCoopVersion });
	EXPECT_EQ(encoded.size(), 2U + 4U + 1U + 4U * 3U);
	const auto decoded = DecodeSurvivorsCoopActions(encoded);
	ASSERT_TRUE(decoded);
	EXPECT_EQ(decoded->ticks, 9001U);
	ASSERT_EQ(decoded->count, 4U);
	for (unsigned index = 0; index < 4; ++index) {
		EXPECT_EQ(decoded->actions[index].player, packet.actions[index].player);
		EXPECT_EQ(decoded->actions[index].graphic, packet.actions[index].graphic);
		EXPECT_EQ(decoded->actions[index].direction, packet.actions[index].direction);
	}
	// A state decoder never mistakes an action packet for a snapshot, and vice versa.
	EXPECT_FALSE(DecodeSurvivorsCoopState(encoded));
	SurvivorsCoopState state;
	state.sequence = 1;
	EXPECT_FALSE(DecodeSurvivorsCoopActions(EncodeSurvivorsCoopState(state)));
	for (size_t length = 0; length < encoded.size(); ++length)
		EXPECT_FALSE(DecodeSurvivorsCoopActions(std::span(encoded).first(length))) << length;
	auto trailing = encoded;
	trailing.push_back(std::byte { 0 });
	EXPECT_FALSE(DecodeSurvivorsCoopActions(trailing));
	const auto rejects = [&](auto mutate) {
		SurvivorsCoopActions bad = packet;
		mutate(bad);
		return !DecodeSurvivorsCoopActions(EncodeSurvivorsCoopActions(bad));
	};
	EXPECT_TRUE(rejects([](SurvivorsCoopActions &p) { p.count = 0; }));
	EXPECT_TRUE(rejects([](SurvivorsCoopActions &p) { p.actions[1].player = 4; }));
	EXPECT_TRUE(rejects([](SurvivorsCoopActions &p) { p.actions[1].player = 0; }));
	EXPECT_TRUE(rejects([](SurvivorsCoopActions &p) { p.actions[2].graphic = static_cast<uint8_t>(player_graphic::Death); }));
	EXPECT_TRUE(rejects([](SurvivorsCoopActions &p) { p.actions[3].direction = 8; }));
}

TEST(SurvivorsCoopProtocol, ReplayedActionOverridesTheLocalThrottleButNotItsOwnSwing)
{
	Player hero { };
	hero._pHitPoints = 10 << 6;
	hero._pmode = PM_WALK_SOUTHWARDS;
	hero._pAFrames = 16;
	hero._pAFNum = 9;
	hero._pSFrames = 14;
	hero._pSFNum = 8;
	hero.position.tile = { 50, 50 };
	StartSurvivorsActionAnimation(hero, player_graphic::Attack, { 51, 50 });
	ASSERT_TRUE(hero.survivorsActionActive);
	const uint8_t serial = hero.survivorsActionSerial;
	hero.survivorsActionAnim.currentFrame = 3;
	// Replaying the same swing the client already shows keeps it running.
	ReplaySurvivorsActionAnimation(hero, player_graphic::Attack, hero.survivorsActionDirection);
	EXPECT_EQ(hero.survivorsActionAnim.currentFrame, 3);
	EXPECT_EQ(hero.survivorsActionSerial, serial);
	// A different authoritative action replaces it even before the release frame.
	ReplaySurvivorsActionAnimation(hero, player_graphic::Fire, Direction::North);
	EXPECT_EQ(hero.getDisplayedGraphic(), player_graphic::Fire);
	EXPECT_EQ(hero.getDisplayedDirection(), Direction::North);
	EXPECT_EQ(hero.getDisplayedAnimation().numberOfFrames, 14);
	EXPECT_EQ(hero._pmode, PM_WALK_SOUTHWARDS);
	EXPECT_NE(hero.survivorsActionSerial, serial);
	// Native modes (here death) own the sprite: a replayed action never covers them.
	hero.survivorsActionActive = false;
	hero._pmode = PM_DEATH;
	ReplaySurvivorsActionAnimation(hero, player_graphic::Magic, Direction::East);
	EXPECT_FALSE(hero.survivorsActionActive);
}

TEST(SurvivorsCoopProtocol, TruncatedAndTrailingPacketsCannotPartiallyApplyState)
{
	SurvivorsCoopState state;
	state.sequence = 1;
	const auto encoded = EncodeSurvivorsCoopState(state);
	for (size_t length = 0; length < encoded.size(); ++length)
		EXPECT_FALSE(DecodeSurvivorsCoopState(std::span(encoded).first(length))) << length;
	auto oversized = encoded;
	oversized.push_back(std::byte { 0 });
	EXPECT_FALSE(DecodeSurvivorsCoopState(oversized));
	state.peers[2].arsenal.ranks[0] = SurvivorsWeaponMaxRank + 1;
	EXPECT_FALSE(DecodeSurvivorsCoopState(EncodeSurvivorsCoopState(state)));
	state.peers[2].arsenal.ranks[0] = 0;
	state.sector.x = SurvivorsSectorLimit + 1;
	EXPECT_FALSE(DecodeSurvivorsCoopState(EncodeSurvivorsCoopState(state)));
}

TEST(SurvivorsCoopProtocol, SectorSeedsAreStableAndReturnToOriginalAtOrigin)
{
	EXPECT_EQ(SurvivorsSectorSeed(1234, 0, {}), 1234U);
	EXPECT_EQ(SurvivorsSectorSeed(1234, 2, { -4, 7 }), SurvivorsSectorSeed(1234, 2, { -4, 7 }));
	EXPECT_NE(SurvivorsSectorSeed(1234, 2, { -4, 7 }), SurvivorsSectorSeed(1234, 2, { -3, 7 }));
	EXPECT_NE(SurvivorsSectorSeed(1234, 2, { -4, 7 }), SurvivorsSectorSeed(1234, 3, { -4, 7 }));
}

TEST(SurvivorsCoopProtocol, SnapshotMustMatchBothLoadedAndRequestedSector)
{
	EXPECT_TRUE(SurvivorsSectorSnapshotIsCurrent(8, 8, 8));
	EXPECT_FALSE(SurvivorsSectorSnapshotIsCurrent(7, 8, 8));
	EXPECT_FALSE(SurvivorsSectorSnapshotIsCurrent(8, 8, 9));
	EXPECT_FALSE(SurvivorsSectorSnapshotIsCurrent(9, 8, 9));
}

TEST(SurvivorsCoopProtocol, DuplicateAndStaleChoicesCannotSpendASecondLevel)
{
	SurvivorsCoopPeerState peer;
	peer.started = true;
	peer.upgradePending = true;
	peer.arsenal.drafted = true;
	peer.draftSequence = 7;
	peer.acknowledgedRequest = 9;
	SurvivorsCoopDraftRequest request { 10, 7, SurvivorsCoopDraftAction::Choose, 2 };
	const auto decoded = DecodeSurvivorsCoopDraft(EncodeSurvivorsCoopDraft(request));
	ASSERT_TRUE(decoded);
	EXPECT_TRUE(SurvivorsCoopDraftRequestIsFresh(peer, *decoded));
	peer.acknowledgedRequest = 10;
	EXPECT_FALSE(SurvivorsCoopDraftRequestIsFresh(peer, *decoded));
	peer.acknowledgedRequest = 9;
	++peer.draftSequence; // Reroll or banish produced a different authoritative draft.
	EXPECT_FALSE(SurvivorsCoopDraftRequestIsFresh(peer, *decoded));
	peer.draftSequence = 7;
	peer.upgradePending = false;
	EXPECT_FALSE(SurvivorsCoopDraftRequestIsFresh(peer, *decoded));
	request.choice = 3;
	EXPECT_FALSE(DecodeSurvivorsCoopDraft(EncodeSurvivorsCoopDraft(request)));
}

TEST(SurvivorsCoopArsenal, ReplicaDraftsAndResourcesStayIsolatedPerPlayer)
{
	ResetAllSurvivorsArsenals();
	SurvivorsArsenalSnapshot first;
	first.ranks[0] = 4;
	first.offers = { 0, 10, 16 };
	first.drafted = true;
	SurvivorsArsenalSnapshot second;
	second.ranks[2] = 3;
	second.offers = { 2, 11, 17 };
	second.rerolls = 1;
	second.drafted = true;
	ASSERT_TRUE(ApplySurvivorsArsenalReplica(0, first));
	ASSERT_TRUE(ApplySurvivorsArsenalReplica(1, second));
	SelectSurvivorsArsenalPlayer(0);
	EXPECT_EQ(GetSurvivorsArsenalSnapshot().ranks[0], 4U);
	EXPECT_EQ(GetSurvivorsArsenalSnapshot().ranks[2], 0U);
	SelectSurvivorsArsenalPlayer(1);
	EXPECT_EQ(GetSurvivorsArsenalSnapshot().ranks[0], 0U);
	EXPECT_EQ(GetSurvivorsArsenalSnapshot().ranks[2], 3U);
	EXPECT_EQ(GetSurvivorsArsenalSnapshot().rerolls, 1U);
	second.ranks[2] = 7;
	EXPECT_FALSE(ApplySurvivorsArsenalReplica(1, second));
	EXPECT_EQ(GetSurvivorsArsenalSnapshot().ranks[2], 3U);
	EXPECT_FALSE(ApplySurvivorsArsenalReplica(4, first));
	ResetAllSurvivorsArsenals();
	SelectSurvivorsArsenalPlayer(0);
}
} // namespace
} // namespace devilution
