#include <algorithm>
#include <deque>
#include <limits>

#include <gtest/gtest.h>

#include "dvlnet/async_session.h"
#include "dvlnet/async_runtime.h"
#include "survivors_coop.h"

namespace devilution::net::async {
namespace {
TEST(AsyncRuntimeTimeout, FirstSnapshotHasLoadingGraceAfterLocalLevelIsReady)
{
	EXPECT_EQ(AsyncNetAuthorityTimeoutMs(0, false), 30000U);
	EXPECT_EQ(AsyncNetAuthorityTimeoutMs(1, false), 10000U);
	EXPECT_EQ(AsyncNetAuthorityTimeoutMs(1, true), 30000U);
}

TEST(AsyncRuntimeReadiness, JoiningClientWaitsForAuthoritySnapshot)
{
	EXPECT_TRUE(AsyncNetNeedsInitialSnapshot(true, false, 0));
	EXPECT_FALSE(AsyncNetNeedsInitialSnapshot(true, false, 1));
	EXPECT_FALSE(AsyncNetNeedsInitialSnapshot(true, true, 0));
	EXPECT_FALSE(AsyncNetNeedsInitialSnapshot(false, false, 0));
}

struct Link {
	struct Packet {
		uint8_t source;
		uint8_t destination;
		Bytes bytes;
		unsigned deliverAt;
	};
	Session host, client;
	std::deque<Packet> packets;
	unsigned now = 0;
	unsigned sent = 0;
	bool impair = false;
	unsigned applied = 0;
	unsigned installed = 0;
	uint32_t maximumRewind = 0;
	Bytes received;
	Link()
	{
		host.Reset(0, 123);
		client.Reset(1, 123);
		host.SetPeerConnected(1, true);
	}
	Session::Send Sender(uint8_t source)
	{
		return [this, source](uint8_t destination, std::span<const std::byte> bytes) {
			++sent;
			if (impair && sent % 7 == 0) return;
			Packet packet { source, destination, Bytes(bytes.begin(), bytes.end()), now + (impair ? 100U + (sent % 4) * 25U : 0U) };
			if (sent % 3 == 0)
				packets.push_front(std::move(packet));
			else
				packets.push_back(std::move(packet));
		};
	}
	void Deliver()
	{
		std::deque<Packet> due;
		for (auto it = packets.begin(); it != packets.end();) {
			if (it->deliverAt <= now) {
				due.push_back(std::move(*it));
				it = packets.erase(it);
			} else
				++it;
		}
		for (const auto &packet : due) {
			Session &destination = packet.destination == 0 ? host : client;
			destination.Receive(packet.source, packet.bytes, Sender(packet.destination),
			    [this](const State &state, std::span<const Command>) { ++installed; received = state.payload; }, { });
		}
	}
	void Tick()
	{
		now += 50;
		client.PumpInputs(now, Sender(1));
		host.PumpInputs(now, Sender(0));
		Deliver();
		host.AdvanceAuthority([this](uint8_t, const Command &command, uint32_t rewind) {
			EXPECT_EQ(command.sequence, applied + 1);
			++applied;
			maximumRewind = std::max(maximumRewind, host.Tick() - rewind);
		});
	}
};

TEST(AsyncNetworkSession, AuthorityNeverWaitsForAStalledPeer)
{
	Link link;
	for (unsigned tick = 0; tick < 100; ++tick)
		link.Tick();
	EXPECT_EQ(link.host.Tick(), 100U);
	EXPECT_EQ(link.applied, 0U);
}

TEST(AsyncNetworkSession, DelayedLostAndReorderedInputsApplyExactlyOnceAndAckCleansReplayQueue)
{
	Link link;
	link.impair = true;
	const Bytes command { std::byte { 1 }, std::byte { 70 }, std::byte { 71 } };
	for (unsigned index = 0; index < 30; ++index)
		ASSERT_TRUE(link.client.QueueInput(command));
	const Bytes world(2048, std::byte { 9 });
	for (unsigned tick = 0; tick < 400; ++tick) {
		link.Tick();
		if (tick % 2 == 0) link.host.Publish(world, link.Sender(0));
	}
	EXPECT_EQ(link.applied, 30U);
	EXPECT_TRUE(link.client.PendingInputs().empty());
	EXPECT_GT(link.client.Statistics().retransmits, 0U);
	EXPECT_GT(link.installed, 0U);
	EXPECT_LE(link.maximumRewind, MaxRewindTicks);
	EXPECT_EQ(link.received, world);
}

TEST(AsyncNetworkSession, LostFragmentsCannotBeStarvedByNewerWorldStates)
{
	Link link;
	link.impair = true;
	Bytes world(FragmentBytes * 40 + 37);
	for (size_t i = 0; i < world.size(); ++i)
		world[i] = static_cast<std::byte>(i & 255);
	for (unsigned tick = 0; tick < 300; ++tick) {
		link.Tick();
		if (tick % 2 == 0) link.host.Publish(world, link.Sender(0));
	}
	EXPECT_GT(link.installed, 10U);
	EXPECT_EQ(link.received, world);
}

TEST(AsyncNetworkSession, StaleEpochAndForgedAuthoritySnapshotsCannotMutateClient)
{
	Link link;
	link.Tick();
	link.host.Publish(Bytes(2000, std::byte { 4 }), link.Sender(0));
	ASSERT_FALSE(link.packets.empty());
	const auto packet = link.packets.front().bytes;
	bool installed = false;
	auto install = [&](const State &, std::span<const Command>) { installed = true; };
	EXPECT_FALSE(link.client.Receive(2, packet, link.Sender(1), install, { }));
	Session anotherRoom;
	anotherRoom.Reset(1, 124);
	EXPECT_FALSE(anotherRoom.Receive(0, packet, link.Sender(1), install, { }));
	EXPECT_FALSE(installed);
}

TEST(AsyncNetworkSession, ReliableFinalStateRetriesAndExecutesExactlyOnce)
{
	Link link;
	link.impair = true;
	const Bytes finalState { std::byte { 5 }, std::byte { 1 } };
	ASSERT_TRUE(link.host.QueueReliableExtension(5, finalState));
	unsigned deliveries = 0;
	for (unsigned tick = 0; tick < 100; ++tick) {
		link.now += 50;
		link.host.PumpInputs(link.now, link.Sender(0));
		std::deque<Link::Packet> due;
		for (auto it = link.packets.begin(); it != link.packets.end();) {
			if (it->deliverAt <= link.now) {
				due.push_back(std::move(*it));
				it = link.packets.erase(it);
			} else
				++it;
		}
		for (const auto &packet : due) {
			Session &destination = packet.destination == 0 ? link.host : link.client;
			destination.Receive(packet.source, packet.bytes, link.Sender(packet.destination), { }, [&](uint8_t, uint8_t channel, std::span<const std::byte> payload) {
				EXPECT_EQ(channel, 5);
				EXPECT_EQ(Bytes(payload.begin(), payload.end()), finalState);
				++deliveries;
			});
		}
	}
	EXPECT_EQ(deliveries, 1U);
	EXPECT_FALSE(link.host.HasReliablePending());
}

TEST(AsyncNetworkSession, SurvivorsAdmissionRetriesAfterEarlyHelloWasAcknowledgedButIgnored)
{
	Link link;
	const Bytes hello { std::byte { 2 } };
	const Bytes checkpoint { std::byte { 6 }, std::byte { 42 } };
	bool handlerReady = false;
	bool sawHello = false;
	unsigned admissions = 0;
	auto pump = [&] {
		for (unsigned i = 0; i < 10; ++i) {
			link.now += 50;
			link.client.PumpInputs(link.now, link.Sender(1));
			link.host.PumpInputs(link.now, link.Sender(0));
			std::deque<Link::Packet> due;
			due.swap(link.packets);
			for (const auto &packet : due) {
				Session &destination = packet.destination == 0 ? link.host : link.client;
				destination.Receive(packet.source, packet.bytes, link.Sender(packet.destination), { },
				    [&](uint8_t, uint8_t, std::span<const std::byte> payload) {
					    if (!handlerReady || admissions != 0) return;
					    const Bytes message(payload.begin(), payload.end());
					    if (message == hello) sawHello = true;
					    if (message == checkpoint && sawHello) ++admissions;
				    });
			}
		}
	};
	ASSERT_TRUE(link.client.QueueReliableExtension(SurvivorsCoopChannel, hello));
	ASSERT_TRUE(link.client.QueueReliableExtension(SurvivorsCoopChannel, checkpoint));
	pump();
	ASSERT_FALSE(link.client.HasReliablePending()); // Transport ACKed both before the handler was ready.
	EXPECT_EQ(admissions, 0U);
	EXPECT_FALSE(SurvivorsCoopShouldRetryAdmission(true, 0, 1, false, 1000, 1000));
	EXPECT_FALSE(SurvivorsCoopShouldRetryAdmission(true, 1, 1, true, 1000, 1000));
	EXPECT_FALSE(SurvivorsCoopShouldRetryAdmission(true, 1, 1, false, 999, 1000));
	ASSERT_TRUE(SurvivorsCoopShouldRetryAdmission(true, 1, 1, false, 1000, 1000));
	handlerReady = true;
	ASSERT_TRUE(link.client.QueueReliableExtension(SurvivorsCoopChannel, hello));
	ASSERT_TRUE(link.client.QueueReliableExtension(SurvivorsCoopChannel, checkpoint));
	pump();
	EXPECT_EQ(admissions, 1U);
	EXPECT_FALSE(link.client.HasReliablePending());
}

TEST(AsyncNetworkSession, DisconnectedSlotCanAcceptANewPlayersSequenceOne)
{
	Link link;
	for (unsigned i = 0; i < 3; ++i)
		ASSERT_TRUE(link.client.QueueInput(Bytes { std::byte { 1 } }));
	for (unsigned i = 0; i < 10; ++i)
		link.Tick();
	ASSERT_EQ(link.applied, 3U);
	link.host.SetPeerConnected(1, false);
	link.client.Reset(1, 123);
	link.host.SetPeerConnected(1, true);
	link.applied = 0;
	ASSERT_TRUE(link.client.QueueInput(Bytes { std::byte { 2 } }));
	for (unsigned i = 0; i < 10; ++i)
		link.Tick();
	EXPECT_EQ(link.applied, 1U);
	EXPECT_EQ(link.host.Statistics().inputsApplied, 4U);
}

TEST(AsyncNetworkSession, InputAndSnapshotBudgetsAreBounded)
{
	Link link;
	EXPECT_FALSE(link.client.QueueInput({ }));
	EXPECT_FALSE(link.client.QueueInput(Bytes(MaxCommandBytes + 1)));
	for (unsigned i = 0; i < MaxPendingInputs; ++i)
		ASSERT_TRUE(link.client.QueueInput(Bytes { std::byte { 1 } }));
	EXPECT_FALSE(link.client.QueueInput(Bytes { std::byte { 1 } }));
	link.host.Publish(Bytes(MaxSnapshotBytes + 1), link.Sender(0));
	EXPECT_TRUE(link.packets.empty());
	Session authority;
	authority.Reset(0, 123);
	for (uint8_t peer = 1; peer < MaxPeers; ++peer)
		authority.SetPeerConnected(peer, true);
	for (unsigned i = 0; i < MaxPendingInputs / (MaxPeers - 1); ++i)
		ASSERT_TRUE(authority.QueueReliableExtension(5, Bytes { std::byte { 1 } }));
	EXPECT_FALSE(authority.QueueReliableExtension(5, Bytes { std::byte { 1 } }));
}
} // namespace
} // namespace devilution::net::async
