#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <span>
#include <vector>

namespace devilution::net::async {

inline constexpr uint8_t ProtocolVersion = 5;
inline constexpr uint8_t GameProtocolTag = 0xA7;
inline constexpr size_t MaxPeers = 4;
inline constexpr size_t MaxCommandBytes = 384;
inline constexpr size_t MaxSnapshotBytes = 512 * 1024;
inline constexpr size_t FragmentBytes = 384;
inline constexpr size_t MaxPendingInputs = 128;
inline constexpr uint32_t MaxRewindTicks = 4;
using Bytes = std::vector<std::byte>;

struct Command {
	uint32_t sequence = 0;
	uint32_t observedTick = 0;
	Bytes payload;
};
struct State {
	uint32_t tick = 0;
	std::array<uint32_t, MaxPeers> acknowledged { };
	Bytes payload;
};
struct Metrics {
	uint64_t inputsApplied = 0;
	uint64_t duplicateInputs = 0;
	uint64_t malformed = 0;
	uint64_t snapshotsApplied = 0;
	uint64_t retransmits = 0;
};

// A session transports native commands and complete authoritative world states.
// No tick waits for another peer: missing input keeps the actor's last command.
// Ordered, exactly-once commands are retransmitted until a state ACKs execution.
class Session {
public:
	using Send = std::function<void(uint8_t destination, std::span<const std::byte>)>;
	using Apply = std::function<void(uint8_t peer, const Command &, uint32_t rewindTick)>;
	using Install = std::function<void(const State &, std::span<const Command> pending)>;
	using Extension = std::function<void(uint8_t peer, uint8_t channel, std::span<const std::byte>)>;

	void Reset(uint8_t self, uint32_t epoch);
	void SetPeerConnected(uint8_t peer, bool connected);
	bool IsAuthority() const { return self_ == 0; }
	bool QueueInput(std::span<const std::byte> payload);
	bool Receive(uint8_t sender, std::span<const std::byte> packet, const Send &send, const Install &install, const Extension &extension);
	void PumpInputs(uint32_t nowMilliseconds, const Send &send);
	void AdvanceAuthority(const Apply &apply);
	void Publish(std::span<const std::byte> world, const Send &send);
	bool SendExtension(uint8_t channel, std::span<const std::byte> payload, const Send &send);
	bool QueueReliableExtension(uint8_t channel, std::span<const std::byte> payload);
	bool HasReliablePending() const { return !reliablePending_.empty(); }
	uint32_t Tick() const { return tick_; }
	uint32_t LastSnapshotTick() const { return lastSnapshotTick_; }
	const Metrics &Statistics() const { return metrics_; }
	std::vector<Command> PendingInputs() const;
	bool Healthy() const { return healthy_; }

private:
	struct Pending {
		Command command;
		uint32_t sentAt = 0;
		bool sent = false;
	};
	struct Assembly {
		uint32_t tick = 0;
		uint32_t size = 0;
		uint32_t hash = 0;
		std::array<uint32_t, MaxPeers> acknowledged { };
		Bytes bytes;
		std::vector<bool> fragments;
		size_t received = 0;
	};
	uint8_t self_ = 0;
	uint32_t epoch_ = 0;
	uint32_t tick_ = 0;
	uint32_t nextInput_ = 1;
	uint32_t lastSnapshotTick_ = 0;
	std::array<uint32_t, MaxPeers> applied_ { };
	std::array<std::map<uint32_t, Command>, MaxPeers> queued_;
	std::deque<Pending> pending_;
	Assembly assembly_;
	std::array<bool, MaxPeers> connected_ { };
	std::array<State, MaxPeers> outgoing_;
	struct Reliable {
		uint8_t destination;
		uint8_t channel;
		uint32_t sequence;
		Bytes payload;
		uint32_t sentAt = 0;
		bool sent = false;
	};
	std::deque<Reliable> reliablePending_;
	std::array<uint32_t, MaxPeers> nextReliable_ { 1, 1, 1, 1 };
	std::array<uint32_t, MaxPeers> appliedReliable_ { };
	std::array<std::map<uint32_t, std::pair<uint8_t, Bytes>>, MaxPeers> queuedReliable_;
	Metrics metrics_;
	bool healthy_ = true;
};

uint32_t PayloadHash(std::span<const std::byte> payload);

} // namespace devilution::net::async
