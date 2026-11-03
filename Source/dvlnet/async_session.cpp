#include "dvlnet/async_session.h"

#include <algorithm>
#include <limits>

namespace devilution::net::async {
namespace {
constexpr uint32_t Magic = 0x54454E41; // ANET, little endian.
enum Kind : uint8_t { Input = 1,
	Snapshot = 2,
	Extra = 3,
	StateAck = 4,
	ReliableExtra = 5,
	ReliableAck = 6 };
void Put(Bytes &bytes, uint32_t value)
{
	for (unsigned shift = 0; shift != 32; shift += 8)
		bytes.push_back(static_cast<std::byte>((value >> shift) & 255));
}
struct Reader {
	std::span<const std::byte> bytes;
	size_t offset = 0;
	bool valid = true;
	uint32_t Get()
	{
		if (bytes.size() - offset < 4) {
			valid = false;
			return 0;
		}
		uint32_t value = 0;
		for (unsigned shift = 0; shift != 32; shift += 8)
			value |= std::to_integer<uint32_t>(bytes[offset++]) << shift;
		return value;
	}
	std::span<const std::byte> Rest() const { return bytes.subspan(offset); }
};
Bytes Header(uint32_t epoch, Kind kind)
{
	Bytes result;
	Put(result, Magic);
	Put(result, ProtocolVersion | (static_cast<uint32_t>(kind) << 8));
	Put(result, epoch);
	return result;
}
} // namespace

uint32_t PayloadHash(std::span<const std::byte> payload)
{
	uint32_t result = 2166136261U;
	for (const auto value : payload)
		result = (result ^ std::to_integer<uint8_t>(value)) * 16777619U;
	return result;
}

void Session::Reset(uint8_t self, uint32_t epoch)
{
	*this = { };
	self_ = self;
	epoch_ = epoch;
	healthy_ = self < MaxPeers;
}

bool Session::QueueInput(std::span<const std::byte> payload)
{
	if (!healthy_ || payload.empty() || payload.size() > MaxCommandBytes || pending_.size() >= MaxPendingInputs || nextInput_ == std::numeric_limits<uint32_t>::max())
		return false;
	pending_.push_back({ { nextInput_++, lastSnapshotTick_, Bytes(payload.begin(), payload.end()) }, 0, false });
	return true;
}

std::vector<Command> Session::PendingInputs() const
{
	std::vector<Command> result;
	result.reserve(pending_.size());
	for (const auto &pending : pending_)
		result.push_back(pending.command);
	return result;
}

void Session::PumpInputs(uint32_t now, const Send &send)
{
	unsigned sent = 0;
	for (auto &pending : pending_) {
		if (pending.sent && now - pending.sentAt < 100)
			continue;
		if (sent++ >= 16)
			break;
		Bytes bytes = Header(epoch_, Input);
		Put(bytes, pending.command.sequence);
		Put(bytes, pending.command.observedTick);
		bytes.insert(bytes.end(), pending.command.payload.begin(), pending.command.payload.end());
		if (IsAuthority()) {
			queued_[self_].try_emplace(pending.command.sequence, pending.command);
		} else {
			send(0, bytes);
		}
		if (pending.sent)
			++metrics_.retransmits;
		pending.sent = true;
		pending.sentAt = now;
	}
	for (auto &pending : reliablePending_) {
		if (pending.sent && now - pending.sentAt < 100) continue;
		Bytes bytes = Header(epoch_, ReliableExtra);
		Put(bytes, pending.sequence);
		Put(bytes, pending.channel);
		bytes.insert(bytes.end(), pending.payload.begin(), pending.payload.end());
		send(pending.destination, bytes);
		pending.sent = true;
		pending.sentAt = now;
	}
}

void Session::AdvanceAuthority(const Apply &apply)
{
	if (!healthy_ || !IsAuthority())
		return;
	++tick_;
	for (uint8_t peer = 0; peer < MaxPeers; ++peer) {
		auto &queue = queued_[peer];
		unsigned budget = 16;
		while (budget-- != 0) {
			auto next = queue.find(applied_[peer] + 1);
			if (next == queue.end())
				break;
			const uint32_t observed = std::min(next->second.observedTick, tick_);
			const uint32_t oldest = tick_ > MaxRewindTicks ? tick_ - MaxRewindTicks : 0;
			apply(peer, next->second, std::max(oldest, observed));
			applied_[peer] = next->first;
			queue.erase(next);
			++metrics_.inputsApplied;
		}
	}
	while (!pending_.empty() && pending_.front().command.sequence <= applied_[self_])
		pending_.pop_front();
}

void Session::SetPeerConnected(uint8_t peer, bool connected)
{
	if (peer >= MaxPeers)
		return;
	const bool wasConnected = connected_[peer];
	connected_[peer] = connected;
	if (!connected) {
		if (wasConnected) {
			applied_[peer] = 0;
			nextReliable_[peer] = 1;
			appliedReliable_[peer] = 0;
			queuedReliable_[peer].clear();
		}
		queued_[peer].clear();
		outgoing_[peer] = { };
		std::erase_if(reliablePending_, [peer](const Reliable &pending) { return pending.destination == peer; });
	}
}

void Session::Publish(std::span<const std::byte> world, const Send &send)
{
	if (!IsAuthority() || !healthy_ || world.empty() || world.size() > MaxSnapshotBytes)
		return;
	for (uint8_t peer = 1; peer < MaxPeers; ++peer) {
		if (!connected_[peer])
			continue;
		State &state = outgoing_[peer];
		// Keep a complete baseline alive until ACKed. Under loss the next publish
		// retransmits its fragments; it cannot be starved by newer snapshots.
		if (state.payload.empty())
			state = { tick_, applied_, Bytes(world.begin(), world.end()) };
		const uint32_t hash = PayloadHash(state.payload);
		for (size_t offset = 0; offset < state.payload.size(); offset += FragmentBytes) {
			Bytes bytes = Header(epoch_, Snapshot);
			Put(bytes, state.tick);
			Put(bytes, static_cast<uint32_t>(state.payload.size()));
			Put(bytes, static_cast<uint32_t>(offset));
			Put(bytes, hash);
			for (const auto ack : state.acknowledged)
				Put(bytes, ack);
			const auto part = std::span<const std::byte>(state.payload).subspan(offset, std::min(FragmentBytes, state.payload.size() - offset));
			bytes.insert(bytes.end(), part.begin(), part.end());
			send(peer, bytes);
		}
	}
}

bool Session::SendExtension(uint8_t channel, std::span<const std::byte> payload, const Send &send)
{
	if (!healthy_ || payload.empty() || payload.size() > MaxCommandBytes)
		return false;
	Bytes bytes = Header(epoch_, Extra);
	Put(bytes, channel);
	bytes.insert(bytes.end(), payload.begin(), payload.end());
	send(IsAuthority() ? 0xFF : 0, bytes);
	return true;
}

bool Session::QueueReliableExtension(uint8_t channel, std::span<const std::byte> payload)
{
	const size_t recipients = IsAuthority() ? static_cast<size_t>(std::count(connected_.begin() + 1, connected_.end(), true)) : 1;
	if (!healthy_ || payload.empty() || payload.size() > MaxCommandBytes || reliablePending_.size() + recipients > MaxPendingInputs) return false;
	for (uint8_t peer = 0; peer < MaxPeers; ++peer) {
		if (peer == self_ || (!IsAuthority() && peer != 0) || (IsAuthority() && !connected_[peer])) continue;
		reliablePending_.push_back({ peer, channel, nextReliable_[peer]++, Bytes(payload.begin(), payload.end()) });
	}
	return true;
}

bool Session::Receive(uint8_t sender, std::span<const std::byte> bytes, const Send &send, const Install &install, const Extension &extension)
{

	if (sender >= MaxPeers || bytes.size() < 12 || bytes.size() > FragmentBytes + 44) {
		++metrics_.malformed;
		return false;
	}
	Reader reader { bytes };
	if (reader.Get() != Magic)
		return false; // Legacy packets are handled by the legacy receive path.
	const uint32_t descriptor = reader.Get();
	const uint32_t epoch = reader.Get();
	if ((descriptor & 255) != ProtocolVersion || epoch != epoch_ || !healthy_)
		return false;
	const auto kind = static_cast<Kind>(descriptor >> 8);
	if (kind == ReliableAck) {
		const uint32_t sequence = reader.Get();
		if (!reader.valid || !reader.Rest().empty()) return false;
		std::erase_if(reliablePending_, [=](const Reliable &pending) { return pending.destination == sender && pending.sequence <= sequence; });
		return true;
	}
	if (kind == ReliableExtra) {
		const uint32_t sequence = reader.Get();
		const uint32_t channel = reader.Get();
		if (!reader.valid || channel > 255 || reader.Rest().empty() || reader.Rest().size() > MaxCommandBytes || (!IsAuthority() && sender != 0) || sequence == 0) return false;
		if (sequence > appliedReliable_[sender] && sequence - appliedReliable_[sender] <= MaxPendingInputs)
			queuedReliable_[sender].try_emplace(sequence, static_cast<uint8_t>(channel), Bytes(reader.Rest().begin(), reader.Rest().end()));
		auto &queue = queuedReliable_[sender];
		while (true) {
			auto next = queue.find(appliedReliable_[sender] + 1);
			if (next == queue.end()) break;
			if (extension) extension(sender, next->second.first, next->second.second);
			appliedReliable_[sender] = next->first;
			queue.erase(next);
		}
		Bytes ack = Header(epoch_, ReliableAck);
		Put(ack, appliedReliable_[sender]);
		send(sender, ack);
		return true;
	}
	if (kind == StateAck) {
		const uint32_t acknowledged = reader.Get();
		if (!IsAuthority() || !reader.valid || !reader.Rest().empty())
			return false;
		if (outgoing_[sender].tick == acknowledged)
			outgoing_[sender] = { };
		return true;
	}
	if (kind == Input) {
		const uint32_t sequence = reader.Get();
		const uint32_t observed = reader.Get();
		if (!IsAuthority() || !reader.valid || sequence == 0 || reader.Rest().empty() || reader.Rest().size() > MaxCommandBytes)
			return false;
		if (sequence <= applied_[sender]) {
			++metrics_.duplicateInputs;
			return true;
		}
		if (sequence - applied_[sender] > MaxPendingInputs || queued_[sender].size() >= MaxPendingInputs)
			return false;
		connected_[sender] = true;
		queued_[sender].try_emplace(sequence, Command { sequence, observed, Bytes(reader.Rest().begin(), reader.Rest().end()) });
		return true;
	}
	if (kind == Extra) {
		const uint32_t channel = reader.Get();
		if (!reader.valid || channel > 255 || reader.Rest().empty() || reader.Rest().size() > MaxCommandBytes || (!IsAuthority() && sender != 0))
			return false;
		if (extension)
			extension(sender, static_cast<uint8_t>(channel), reader.Rest());
		return true;
	}
	if (kind != Snapshot || IsAuthority() || sender != 0)
		return false;
	const uint32_t tick = reader.Get();
	const uint32_t size = reader.Get();
	const uint32_t offset = reader.Get();
	const uint32_t hash = reader.Get();
	std::array<uint32_t, MaxPeers> acks;
	for (auto &ack : acks)
		ack = reader.Get();
	if (reader.valid && tick == lastSnapshotTick_ && tick != 0) {
		Bytes ack = Header(epoch_, StateAck);
		Put(ack, tick);
		send(0, ack);
		return true;
	}
	if (!reader.valid || tick <= lastSnapshotTick_ || size == 0 || size > MaxSnapshotBytes || offset >= size || offset % FragmentBytes != 0 || reader.Rest().size() != std::min<size_t>(FragmentBytes, size - offset))
		return false;
	if (assembly_.tick > tick)
		return false;
	if (assembly_.tick != tick) {
		assembly_ = { tick, size, hash, acks, Bytes(size), std::vector<bool>((size + FragmentBytes - 1) / FragmentBytes), 0 };
	}
	if (assembly_.size != size || assembly_.hash != hash || assembly_.acknowledged != acks)
		return false;
	const size_t fragment = offset / FragmentBytes;
	if (!assembly_.fragments[fragment]) {
		std::copy(reader.Rest().begin(), reader.Rest().end(), assembly_.bytes.begin() + offset);
		assembly_.fragments[fragment] = true;
		++assembly_.received;
	}
	if (assembly_.received != assembly_.fragments.size())
		return true;
	if (PayloadHash(assembly_.bytes) != hash) {
		assembly_ = { };
		++metrics_.malformed;
		return false;
	}
	if (acks[self_] >= nextInput_)
		return false; // An authority cannot ACK an input this session never issued.
	while (!pending_.empty() && pending_.front().command.sequence <= acks[self_])
		pending_.pop_front();
	State state { tick, acks, std::move(assembly_.bytes) };
	lastSnapshotTick_ = tick;
	tick_ = tick;
	assembly_ = { };
	const auto pending = PendingInputs();
	install(state, pending);
	++metrics_.snapshotsApplied;
	Bytes ack = Header(epoch_, StateAck);
	Put(ack, tick);
	send(0, ack);
	return true;
}

} // namespace devilution::net::async
