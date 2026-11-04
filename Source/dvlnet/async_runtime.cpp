#include "dvlnet/async_runtime.h"
#include "survivors.h"
#include "levels/survivors_sector.hpp"
#include "levels/survivors_rolling_runtime.hpp"
#include "survivors_coop.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cstring>
#include <cstdlib>
#include <deque>
#include <map>
#include <type_traits>
#include <vector>

#include <zlib.h>
#ifdef USE_SDL3
#include <SDL3/SDL_timer.h>
#else
#include <SDL.h>
#endif

#include "control/control.hpp"
#include "cursor.h"
#include "diablo.h"
#ifdef DAMNATION_S1_BRIDGE
#include "s1_headless.hpp"
#endif
#include "dvlnet/async_item_animation.hpp"
#include "dvlnet/async_session.h"
#include "engine/random.hpp"
#include "game_mode.hpp"
#include "gamemenu.h"
#include "interfac.h"
#include "inv.h"
#include "items.h"
#include "levels/dun_tile_data.hpp"
#include "levels/tile_properties.hpp"
#include "lighting.h"
#include "missiles.h"
#include "monster.h"
#include "msg.h"
#include "objects.h"
#include "pack.h"
#include "panels/spell_book.hpp"
#include "pfile.h"
#include "player.h"
#include "plrmsg.h"
#include "portal.h"
#include "quests.h"
#include "spells.h"
#include "stores.h"
#include "qol/visual_store.h"
#include "storm/storm_net.hpp"
#include "towners.h"
#include "utils/endian_swap.hpp"
#include "utils/log.hpp"

namespace devilution {
namespace {
using net::async::Bytes;
constexpr unsigned MaxWorldMissiles = 2048;
constexpr size_t MaxRawWorldBytes = 1024 * 1024;
constexpr uint32_t WorldSnapshotVersion = 5;
net::async::Session Session;
bool Running = false;
bool Installing = false;
bool ApplyingCommand = false;
bool RemoteInteraction = false;
constexpr uint8_t UseInventoryIntent = 0xF0;
constexpr uint8_t StoreIntent = 0xF1;
constexpr uint8_t TargetItemIntent = 0xF2;
constexpr uint8_t TargetWorldIntent = 0xF3;
constexpr uint8_t SurvivorsEquipIntent = 0xF4;
std::array<uint32_t, MAX_PLRS> ItemHashes { };
std::array<bool, MAX_PLRS> ItemHashValid { };
constexpr uint8_t SurvivorsEpochIntent = 0xF5;
// A command carries its original rolling frame, so a command already in flight
// can still execute after the authority shifts its local simulation window.
Bytes EncodeSurvivorsInput(std::span<const std::byte> payload, uint32_t epoch, uint64_t nonce, uint64_t rollingEpoch, int64_t originX, int64_t originY, uint64_t targetIdentity = 0)
{
	Bytes wrapped;
	wrapped.reserve(payload.size() + 45);
	wrapped.push_back(static_cast<std::byte>(SurvivorsEpochIntent));
	for (unsigned shift = 0; shift < 32; shift += 8) wrapped.push_back(static_cast<std::byte>((epoch >> shift) & 255));
	for (unsigned shift = 0; shift < 64; shift += 8) wrapped.push_back(static_cast<std::byte>((nonce >> shift) & 255));
	for (unsigned shift = 0; shift < 64; shift += 8) wrapped.push_back(static_cast<std::byte>((rollingEpoch >> shift) & 255));
	for (uint64_t value : { std::bit_cast<uint64_t>(originX), std::bit_cast<uint64_t>(originY), targetIdentity })
		for (unsigned shift = 0; shift < 64; shift += 8) wrapped.push_back(static_cast<std::byte>((value >> shift) & 255));
	wrapped.insert(wrapped.end(), payload.begin(), payload.end());
	return wrapped;
}
// All coordinate-bearing native commands begin with command,x,y, including
// item pickup/drop records. TargetWorldIntent wraps that record after a token.
bool InputHasCoordinates(_cmd_id type)
{
	switch (type) {
	case CMD_WALKXY: case CMD_SATTACKXY: case CMD_RATTACKXY: case CMD_SPELLXY: case CMD_SPELLXYD:
	case CMD_OPOBJXY: case CMD_OPOBJT: case CMD_DISARMXY: case CMD_TALKXY:
	case CMD_GOTOGETITEM: case CMD_GOTOAGETITEM: case CMD_REQUESTGITEM: case CMD_REQUESTAGITEM:
	case CMD_PUTITEM: return true;
	default: return false;
	}
}

bool InputTargetsMonster(_cmd_id type)
{
	return type == CMD_ATTACKID || type == CMD_RATTACKID || type == CMD_SPELLID || type == CMD_KNOCKBACK;
}

size_t InputCommandOffset(std::span<const std::byte> payload)
{
	return !payload.empty() && std::to_integer<uint8_t>(payload[0]) == TargetWorldIntent ? 5 : 0;
}

uint64_t InputTargetIdentity(std::span<const std::byte> payload)
{
	const size_t offset = InputCommandOffset(payload);
	if (payload.size() < offset + 3) return 0;
	const auto type = static_cast<_cmd_id>(payload[offset]);
	if (InputTargetsMonster(type)) {
		const unsigned id = std::to_integer<unsigned>(payload[offset + 1]) | (std::to_integer<unsigned>(payload[offset + 2]) << 8);
		if (id < MaxMonsters && !Monsters[id].isInvalid)
			return (uint64_t { 1 } << 32) | Monsters[id].rndItemSeed;
	} else if ((type == CMD_GOTOGETITEM || type == CMD_GOTOAGETITEM) && payload.size() >= offset + sizeof(TCmdLocParam1)) {
		const unsigned id = std::to_integer<unsigned>(payload[offset + 3]) | (std::to_integer<unsigned>(payload[offset + 4]) << 8);
		if (id < MAXITEMS && std::find(ActiveItems, ActiveItems + ActiveItemCount, id) != ActiveItems + ActiveItemCount)
			return (uint64_t { 1 } << 32) | Items[id]._iSeed;
	}
	return 0;
}

Bytes DecodeSurvivorsInput(std::span<const std::byte> payload, uint32_t expectedEpoch, uint64_t expectedNonce, uint64_t expectedRollingEpoch, int64_t currentX, int64_t currentY)
{
	if (payload.size() < 46 || std::to_integer<uint8_t>(payload[0]) != SurvivorsEpochIntent) return {};
	uint32_t epoch = 0;
	for (unsigned i = 0; i < 4; ++i) epoch |= std::to_integer<uint32_t>(payload[i + 1]) << (8 * i);
	uint64_t nonce = 0;
	for (unsigned i = 0; i < 8; ++i) nonce |= std::to_integer<uint64_t>(payload[i + 5]) << (8 * i);
	uint64_t rollingEpoch = 0;
	for (unsigned i = 0; i < 8; ++i) rollingEpoch |= std::to_integer<uint64_t>(payload[i + 13]) << (8 * i);
	if (epoch != expectedEpoch || rollingEpoch > expectedRollingEpoch || !SurvivorsCoopAdmissionMatches(expectedNonce, nonce)) return {};
	auto read64 = [&payload](size_t offset) {
		uint64_t value = 0;
		for (unsigned i = 0; i < 8; ++i) value |= std::to_integer<uint64_t>(payload[offset + i]) << (8 * i);
		return value;
	};
	const int64_t originX = std::bit_cast<int64_t>(read64(21));
	const int64_t originY = std::bit_cast<int64_t>(read64(29));
	const uint64_t identity = read64(37);
	if (rollingEpoch == expectedRollingEpoch && (originX != currentX || originY != currentY)) return {};
	Bytes command(payload.begin() + 45, payload.end());
	if (rollingEpoch == expectedRollingEpoch) return command;
	const size_t offset = InputCommandOffset(command);
	if (command.size() <= offset) return {};
	const auto type = static_cast<_cmd_id>(command[offset]);
	if (InputHasCoordinates(type)) {
		if (command.size() < offset + 3) return {};
		// Unsigned subtraction avoids signed overflow for hostile input.
		auto delta = [](int64_t origin, int64_t current, int &out) {
			const bool positive = origin >= current;
			const uint64_t distance = positive ? uint64_t(origin) - uint64_t(current) : uint64_t(current) - uint64_t(origin);
			if (distance >= MAXDUNX) return false;
			out = positive ? static_cast<int>(distance) : -static_cast<int>(distance);
			return true;
		};
		int dx = 0, dy = 0;
		if (!delta(originX, currentX, dx) || !delta(originY, currentY, dy)) return {};
		const Point target { static_cast<int>(std::to_integer<uint8_t>(command[offset + 1])) + dx,
			static_cast<int>(std::to_integer<uint8_t>(command[offset + 2])) + dy };
		if (!InDungeonBounds(target)) return {};
		command[offset + 1] = static_cast<std::byte>(target.x);
		command[offset + 2] = static_cast<std::byte>(target.y);
	}
	if (InputTargetsMonster(type)) {
		if (command.size() < offset + 3 || (identity >> 32) != 1) return {};
		const unsigned id = std::to_integer<unsigned>(command[offset + 1]) | (std::to_integer<unsigned>(command[offset + 2]) << 8);
		if (id >= MaxMonsters || Monsters[id].isInvalid || Monsters[id].rndItemSeed != static_cast<uint32_t>(identity)) return {};
	} else if (type == CMD_GOTOGETITEM || type == CMD_GOTOAGETITEM) {
		if (command.size() < offset + sizeof(TCmdLocParam1) || (identity >> 32) != 1) return {};
		const Point target { std::to_integer<uint8_t>(command[offset + 1]), std::to_integer<uint8_t>(command[offset + 2]) };
		const auto it = std::find_if(ActiveItems, ActiveItems + ActiveItemCount, [&](unsigned id) {
			return Items[id]._iSeed == static_cast<uint32_t>(identity) && Items[id].position == target;
		});
		if (it == ActiveItems + ActiveItemCount) return {};
		command[offset + 3] = static_cast<std::byte>(*it & 255);
		command[offset + 4] = static_cast<std::byte>(*it >> 8);
	}
	return command;
}
bool InventoryMutationIntent(std::span<const std::byte> payload)
{
	if (payload.empty()) return false;
	if (std::to_integer<uint8_t>(payload[0]) == SurvivorsEpochIntent) {
		if (payload.size() <= 45) return false;
		payload = payload.subspan(45);
	}
	const unsigned first = std::to_integer<uint8_t>(payload[0]);
	if (first == UseInventoryIntent || first == StoreIntent || first == TargetItemIntent || first == SurvivorsEquipIntent) return true;
	const size_t offset = InputCommandOffset(payload);
	if (payload.size() <= offset) return false;
	switch (static_cast<_cmd_id>(payload[offset])) {
	case CMD_PUTITEM: case CMD_CHANGEPLRITEMS: case CMD_DELPLRITEMS:
	case CMD_CHANGEINVITEMS: case CMD_DELINVITEMS: case CMD_CHANGEBELTITEMS: case CMD_DELBELTITEMS:
	case CMD_GOTOGETITEM: case CMD_GOTOAGETITEM: case CMD_REQUESTGITEM: case CMD_REQUESTAGITEM:
	case CMD_SPELLXY: case CMD_SPELLXYD: case CMD_SPELLID: case CMD_SPELLPID:
		return true;
	default: return false;
	}
}

bool QueueRuntimeInput(std::span<const std::byte> payload)
{
	if (MyPlayerId < MAX_PLRS && MyPlayerId != 0 && InventoryMutationIntent(payload)) {
		// The client UI may already be changing inventory optimistically. The
		// authority can reject that change and send the same bytes as its last
		// snapshot, so comparing only against the old hash would skip rollback.
		ItemHashValid[MyPlayerId] = false;
	}
	if (!IsSurvivorsMode()) return Session.QueueInput(payload);
	const uint64_t nonce = SurvivorsCoopAdmissionNonce(MyPlayerId);
	if (nonce == 0) return false;
	const auto wrapped = EncodeSurvivorsInput(payload, SurvivorsLoadedSectorEpoch(), nonce,
	    SurvivorsRollingActive() ? SurvivorsRollingEpoch() : 0, SurvivorsRollingOriginX(), SurvivorsRollingOriginY(), InputTargetIdentity(payload));
	return Session.QueueInput(wrapped);
}
Bytes UnwrapRuntimeInput(std::span<const std::byte> payload, unsigned peer)
{
	if (!IsSurvivorsMode()) return Bytes(payload.begin(), payload.end());
	if (IsSurvivorsTransitionPending() || SurvivorsLoadedSectorEpoch() != SurvivorsSectorEpoch()) return {};
	return DecodeSurvivorsInput(payload, SurvivorsLoadedSectorEpoch(), SurvivorsCoopAdmissionNonce(peer),
	    SurvivorsRollingActive() ? SurvivorsRollingEpoch() : 0, SurvivorsRollingOriginX(), SurvivorsRollingOriginY());
}
std::array<AsyncTargetedSpellStatus, MAX_PLRS> TargetSpells { };
std::array<uint32_t, MAX_PLRS> TargetSpellWorld { };
uint32_t WorldContextKey() { return (static_cast<uint32_t>(currlevel) | (setlevel ? 1U << 8 : 0) | (static_cast<uint32_t>(setlvlnum) << 16)) ^ (SurvivorsLoadedSectorEpoch() * 0x9e3779b9U); }
uint32_t LastUiSpellToken = 0;
int PendingTowner = -1;
uint32_t LastReceive = 0;
uint32_t LastPublish = 0;
uint32_t StockHash = 0;
uint32_t PredictedTick = 0;
uint32_t InstalledTick = 0;
uint64_t InstalledSnapshots = 0;
std::array<AsyncNetExtensionHandler, 256> Extensions;
std::array<uint32_t, MAX_PLRS> RewindTicks { };
struct History {
	uint32_t tick;
	std::array<Point, MAX_PLRS> players;
	std::array<Point, MaxMonsters> monsters;
};
std::deque<History> RewindHistory;
struct VisualState {
	Point previous;
	Point current;
	uint32_t received = 0;
	bool valid = false;
};
std::array<VisualState, MAX_PLRS> Visuals;
std::array<VisualState, MaxMonsters> MonsterVisuals;
std::array<bool, MAX_PLRS> Negotiated { };
std::array<bool, MAX_PLRS> ConnectedPeers { };
std::array<uint32_t, MAX_PLRS> NegotiationStarted { };
uint32_t LastHello = 0;
Point ActorPixels(const ActorPosition &position, bool walking, Direction direction, const AnimationInfo &animation)
{
	const auto offset = position.GetDrawnScreenOffset(walking, direction, animation);
	return { (position.tile.x - position.tile.y) * 32 + offset.deltaX, (position.tile.x + position.tile.y) * 16 + offset.deltaY };
}
struct LocalPrediction {
	uint32_t sequence;
	uint32_t tick;
	Bytes command;
};
std::deque<LocalPrediction> Predictions;

// All scalar fields use a defined 32-bit little-endian representation; the only
// raw records are the game's already packed, endian-defined PlayerNetPack and
// ItemNetPack. Native pointers, sprite caches and container internals never cross
// the wire. Decoding builds a complete temporary world before touching globals.
struct Codec {
	Bytes bytes;
	std::span<const std::byte> input;
	size_t offset = 0;
	bool reading = false;
	bool valid = true;
	template <typename T>
	void Scalar(T &value)
	{
		static_assert(std::is_integral_v<T> || std::is_enum_v<T>);
		uint32_t wire = 0;
		if (reading) {
			if (input.size() - offset < 4) {
				valid = false;
				return;
			}
			wire = 0;
			for (unsigned shift = 0; shift < 32; shift += 8)
				wire |= std::to_integer<uint32_t>(input[offset++]) << shift;
			value = static_cast<T>(wire);
		} else {
			wire = static_cast<uint32_t>(value);
			for (unsigned shift = 0; shift < 32; shift += 8)
				bytes.push_back(static_cast<std::byte>((wire >> shift) & 255));
		}
	}
	template <typename... T>
	void Fields(T &...values) { (Scalar(values), ...); }
	void Raw(void *data, size_t count)
	{
		if (reading) {
			if (input.size() - offset < count) {
				valid = false;
				return;
			}
			std::memcpy(data, input.data() + offset, count);
			offset += count;
		} else {
			const auto *first = static_cast<const std::byte *>(data);
			bytes.insert(bytes.end(), first, first + count);
		}
	}
};
template <typename T>
void Transfer(Codec &c, PointOf<T> &p) { c.Fields(p.x, p.y); }
template <typename T>
void Transfer(Codec &c, DisplacementOf<T> &p) { c.Fields(p.deltaX, p.deltaY); }
void Transfer(Codec &c, ActorPosition &p)
{
	Transfer(c, p.tile);
	Transfer(c, p.future);
	Transfer(c, p.last);
	Transfer(c, p.old);
	Transfer(c, p.temp);
	Transfer(c, p.freeform.start);
	Transfer(c, p.freeform.end);
	c.Scalar(p.freeform.active);
	Transfer(c, p.freeformCarry);
}
void Transfer(Codec &c, AnimationInfo &a)
{
	c.Fields(a.ticksPerFrame, a.tickCounterOfCurrentFrame, a.numberOfFrames, a.currentFrame, a.isPetrified);
}
void Transfer(Codec &c, Monster &m)
{
	Transfer(c, m.position);
	Transfer(c, m.enemyPosition);
	Transfer(c, m.animInfo);
	c.Fields(m.maxHitPoints, m.hitPoints, m.flags, m.rndItemSeed, m.aiSeed, m.golemToHit, m.resistance, m.talkMsg,
	    m.goalVar1, m.goalVar2, m.goalVar3, m.var1, m.var2, m.var3, m.goal, m.levelType, m.mode, m.pathCount,
	    m.direction, m.walkFacing, m.enemy, m.isInvalid, m.ai, m.intelligence, m.activeForTicks, m.uniqueType,
	    m.uniqTrans, m.corpseId, m.whoHit, m.minDamage, m.maxDamage, m.minDamageSpecial, m.maxDamageSpecial,
	    m.armorClass, m.reducePlayerStrength, m.reducePlayerMagic, m.reducePlayerDexterity, m.reducePlayerVitality,
	    m.reducePlayerMaxHP, m.reducePlayerMaxMana, m.leader, m.leaderRelation, m.packSize, m.lightId);
}
void Transfer(Codec &c, Missile &m)
{
	Transfer(c, m.position.tile);
	Transfer(c, m.position.start);
	Transfer(c, m.position.tileForRendering);
	Transfer(c, m.position.offset);
	Transfer(c, m.position.velocity);
	Transfer(c, m.position.traveled);
	Transfer(c, m.position.offsetForRendering);
	int group = c.reading ? 0 : m.getFrameGroupRaw();
	c.Fields(group, m._mitype, m._mispllvl, m._miDelFlag, m._miAnimType, m._miAnimFlags, m._miAnimDelay, m._miAnimLen,
	    m._miAnimWidth, m._miAnimWidth2, m._miAnimCnt, m._miAnimAdd, m._miAnimFrame, m._miDrawFlag, m._miLightFlag,
	    m._miPreFlag, m._miUniqTrans, m.duration, m._misource, m._micaster, m._midam, m._miHitFlag, m._midist,
	    m._mlid, m._mirnd, m.var1, m.var2, m.var3, m.var4, m.var5, m.var6, m.var7, m.limitReached, m.lastCollisionTargetHash);
	if (c.reading && c.valid) {
		const bool invisible = m._miAnimType == MissileGraphicID::None;
		if (group < 0 || group >= 16 || (m._mitype == MissileID::Rhino && group >= 8) || (!invisible && (static_cast<size_t>(m._miAnimType) >= GetMissileSpriteDataCount() || group >= GetMissileSpriteData(m._miAnimType).animFAmt))) {
			c.valid = false;
			return;
		}
		// Rhino borrows monster animation data while its graphic ID is None.
		// Native setFrameGroupRaw resets the animation; preserve all numeric
		// state and rebind the correct monster sprites after world installation.
		const auto flags = m._miAnimFlags;
		const int delay = m._miAnimDelay, length = m._miAnimLen, width = m._miAnimWidth, width2 = m._miAnimWidth2, frame = m._miAnimFrame, counter = m._miAnimCnt;
		m.setFrameGroupRaw(group);
		m._miAnimFlags = flags;
		m._miAnimDelay = delay;
		m._miAnimLen = length;
		m._miAnimWidth = width;
		m._miAnimWidth2 = width2;
		m._miAnimFrame = frame;
		m._miAnimCnt = counter;
	}
}
void Transfer(Codec &c, Object &o)
{
	Transfer(c, o.position);
	c.Fields(o._otype, o.applyLighting, o._oTrapFlag, o._oDoorFlag, o._oAnimFlag, o._oAnimDelay, o._oAnimCnt, o._oAnimLen,
	    o._oAnimFrame, o._oAnimWidth, o._oDelFlag, o._oBreak, o._oSolidFlag, o._oMissFlag, o.selectionRegion,
	    o._oPreFlag, o._olid, o._oRndSeed, o._oVar1, o._oVar2, o._oVar3, o._oVar4, o._oVar5, o._oVar6, o._oVar8, o.bookMessage);
}
struct PlayerReplica {
	PlayerNetPack packed { };
	ItemNetPack held { };
	ActorPosition position;
	AnimationInfo animation { };
	PLR_MODE mode = PM_STAND;
	int gold = 0;
	int action = ACTION_NONE;
	int parameter1 = 0, parameter2 = 0, parameter3 = 0;
	bool active = false;
	bool invincible = false;
	bool changingLevel = false;
	AsyncTargetedSpellStatus targetedSpell { };
	int8_t resistFire = 0, resistLightning = 0, resistMagic = 0;
	uint8_t graphics = 0;
	uint32_t ledgerGenerationLow = 0, ledgerGenerationHigh = 0;
};
void Transfer(Codec &c, PlayerReplica &p)
{
	c.Raw(&p.packed, sizeof(p.packed));
	c.Raw(&p.held, sizeof(p.held));
	Transfer(c, p.position);
	Transfer(c, p.animation);
	c.Fields(p.mode, p.gold, p.action, p.parameter1, p.parameter2, p.parameter3, p.active, p.invincible, p.graphics, p.changingLevel, p.resistFire, p.resistLightning, p.resistMagic, p.targetedSpell.spell, p.targetedSpell.token, p.targetedSpell.ready, p.ledgerGenerationLow, p.ledgerGenerationHigh);
}
struct DroppedItem {
	unsigned id;
	ItemNetPack packed { };
	Point position;
	int frame;
	bool animate;
};
struct StockItem {
	ItemNetPack packed { };
	int price = 0;
};
struct World {
	uint32_t version = WorldSnapshotVersion;
	uint32_t level = 0;
	uint32_t sectorEpoch = 0;
	bool rolling = false;
	uint32_t rollingEpochLow = 0, rollingEpochHigh = 0;
	uint32_t originXLow = 0, originXHigh = 0, originYLow = 0, originYHigh = 0;
	bool setLevel = false;
	int setNumber = 0;
	dungeon_type dungeonType = DTYPE_TOWN;
	uint32_t rng = 0;
	std::array<std::vector<StockItem>, 5> stock;
	std::array<std::vector<StockItem>, MAX_PLRS> buyback;
	std::array<PlayerReplica, MAX_PLRS> players;
	std::vector<std::pair<unsigned, Monster>> monsters;
	std::vector<Missile> missiles;
	std::vector<std::pair<unsigned, Object>> objects;
	std::vector<DroppedItem> items;
	std::array<Quest, MAXQUESTS> quests;
	std::array<Portal, MAXPORTAL> portals;
	std::vector<std::byte> tiles;
};
void TransferWorld(Codec &c, World &w)
{
	c.Fields(w.version, w.level, w.sectorEpoch, w.rng, w.setLevel, w.setNumber, w.dungeonType,
	    w.rolling, w.rollingEpochLow, w.rollingEpochHigh, w.originXLow, w.originXHigh, w.originYLow, w.originYHigh);
	for (auto &p : w.players)
		Transfer(c, p);
	for (auto &stock : w.stock) {
		uint32_t count = stock.size();
		c.Scalar(count);
		if (count > 100) {
			c.valid = false;
			return;
		}
		if (c.reading) stock.resize(count);
		for (auto &item : stock) {
			c.Raw(&item.packed, sizeof(item.packed));
			c.Scalar(item.price);
		}
	}
	for (auto &entries : w.buyback) {
		uint32_t count = entries.size();
		c.Scalar(count);
		if (count > SurvivorsBuybackLimit) { c.valid = false; return; }
		if (c.reading) entries.resize(count);
		for (auto &entry : entries) { c.Raw(&entry.packed, sizeof(entry.packed)); c.Scalar(entry.price); }
	}
	uint32_t count = static_cast<uint32_t>(w.monsters.size());
	c.Scalar(count);
	if (count > MaxMonsters) {
		c.valid = false;
		return;
	}
	if (c.reading) w.monsters.resize(count);
	for (auto &[id, monster] : w.monsters) {
		c.Scalar(id);
		Transfer(c, monster);
	}
	count = static_cast<uint32_t>(w.missiles.size());
	c.Scalar(count);
	if (count > MaxWorldMissiles) {
		c.valid = false;
		return;
	}
	if (c.reading) w.missiles.resize(count);
	for (auto &missile : w.missiles)
		Transfer(c, missile);
	count = static_cast<uint32_t>(w.objects.size());
	c.Scalar(count);
	if (count > MAXOBJECTS) {
		c.valid = false;
		return;
	}
	if (c.reading) w.objects.resize(count);
	for (auto &[id, object] : w.objects) {
		c.Scalar(id);
		Transfer(c, object);
	}
	count = static_cast<uint32_t>(w.items.size());
	c.Scalar(count);
	if (count > MAXITEMS) {
		c.valid = false;
		return;
	}
	if (c.reading) w.items.resize(count);
	for (auto &item : w.items) {
		c.Scalar(item.id);
		c.Raw(&item.packed, sizeof(item.packed));
		Transfer(c, item.position);
		c.Fields(item.frame, item.animate);
	}
	for (auto &q : w.quests) {
		Transfer(c, q.position);
		c.Fields(q._qidx, q._qactive, q._qlevel, q._qlvltype, q._qslvl, q._qlog, q._qmsg, q._qvar1, q._qvar2);
	}
	for (auto &p : w.portals) {
		Transfer(c, p.position);
		c.Fields(p.open, p.level, p.ltype, p.setlvl);
	}
	count = static_cast<uint32_t>(w.tiles.size());
	c.Scalar(count);
	if (count > 400 * 1024) {
		c.valid = false;
		return;
	}
	if (c.reading) w.tiles.resize(count);
	c.Raw(w.tiles.data(), count);
}

void Send(uint8_t destination, std::span<const std::byte> bytes)
{
	if (destination == 0xFF) {
		for (uint8_t peer = 0; peer < Players.size(); ++peer)
			if (peer != MyPlayerId && Players[peer].plractive)
				SNetSendMessage(peer, const_cast<std::byte *>(bytes.data()), bytes.size());
	} else if (destination < Players.size() && destination != MyPlayerId) {
		SNetSendMessage(destination, const_cast<std::byte *>(bytes.data()), bytes.size());
	}
}

// Snapshot all mutable collision tiles rather than trusting a client's local
// object/quest simulation. Occupancy is rebuilt from authoritative actors below.
template <typename T, size_t Width, size_t Height>
void TransferGrid(Codec &codec, T (&grid)[Width][Height])
{
	for (auto &row : grid)
		for (auto &value : row)
			codec.Scalar(value);
}
void TransferTiles(Codec &codec)
{
	TransferGrid(codec, dPiece);
	TransferGrid(codec, dFlags);
	TransferGrid(codec, dLight);
	TransferGrid(codec, dPreLight);
	TransferGrid(codec, dCorpse);
	TransferGrid(codec, dTransVal);
}
Bytes CaptureTiles()
{
	Codec codec;
	TransferTiles(codec);
	return std::move(codec.bytes);
}

Bytes CaptureWorld()
{
	World world;
	world.level = currlevel;
	world.sectorEpoch = SurvivorsLoadedSectorEpoch();
	world.rolling = SurvivorsRollingActive();
	if (world.rolling) {
		const uint64_t epoch = SurvivorsRollingEpoch();
		const uint64_t x = std::bit_cast<uint64_t>(SurvivorsRollingOriginX());
		const uint64_t y = std::bit_cast<uint64_t>(SurvivorsRollingOriginY());
		world.rollingEpochLow = static_cast<uint32_t>(epoch);
		world.rollingEpochHigh = static_cast<uint32_t>(epoch >> 32);
		world.originXLow = static_cast<uint32_t>(x);
		world.originXHigh = static_cast<uint32_t>(x >> 32);
		world.originYLow = static_cast<uint32_t>(y);
		world.originYHigh = static_cast<uint32_t>(y >> 32);
	}
	world.setLevel = setlevel;
	world.setNumber = setlvlnum;
	world.dungeonType = leveltype;
	world.rng = GetLCGEngineState();
	auto captureStock = [&](unsigned id, const auto &items) { for (const Item &i:items) { StockItem stock; PackNetItem(i,stock.packed); stock.price=i._iIvalue; world.stock[id].push_back(stock); } };
	captureStock(0, SmithItems);
	captureStock(1, PremiumItems);
	captureStock(2, WitchItems);
	captureStock(3, HealerItems);
	StockItem boy;
	PackNetItem(BoyItem, boy.packed);
	boy.price = BoyItem._iIvalue;
	world.stock[4].push_back(boy);
	for (unsigned peer = 0; peer < MAX_PLRS; ++peer) {
		const auto items = GetSurvivorsBuybackItems(peer);
		const auto prices = GetSurvivorsBuybackPrices(peer);
		for (size_t i = 0; i < items.size() && i < prices.size(); ++i) {
			StockItem entry;
			PackNetItem(items[i], entry.packed);
			entry.price = prices[i];
			world.buyback[peer].push_back(entry);
		}
	}
	for (unsigned peer = 0; peer < Players.size(); ++peer) {
		Player &player = Players[peer];
		auto &out = world.players[peer];
		PackNetPlayer(out.packed, player);
		PackNetItem(player.HoldItem, out.held);
		out.position = player.position;
		out.animation = player.AnimInfo;
		out.mode = player._pmode;
		out.gold = player._pGold;
		out.active = player.plractive;
		out.action = player.destAction;
		out.parameter1 = player.destParam1;
		out.parameter2 = player.destParam2;
		out.parameter3 = player.destParam3;
		out.invincible = player._pInvincible;
		out.changingLevel = player._pLvlChanging;
		out.targetedSpell = TargetSpells[peer];
		out.resistFire = player._pFireResist;
		out.resistLightning = player._pLghtResist;
		out.resistMagic = player._pMagResist;
		out.graphics = player._pgfxnum;
		const uint64_t generation = SurvivorsCoopLedgerGeneration(peer);
		out.ledgerGenerationLow = static_cast<uint32_t>(generation);
		out.ledgerGenerationHigh = static_cast<uint32_t>(generation >> 32);
	}
	// Monster is movable but not copyable because of its palette ownership.
	// Round-trip only the wire fields to make an independent pointer-free copy.
	for (size_t i = 0; leveltype != DTYPE_TOWN && i < ActiveMonsterCount; ++i) {
		const unsigned id = ActiveMonsters[i];
		// The rolling authority reserves parked boss/source IDs in its active
		// pool, but their holding-cell placeholders are not visible actors and
		// must not be installed into a peer's simulated viewport.
		if (SurvivorsRollingMonsterParked(id) || Monsters[id].isInvalid) continue;
		Codec encoded;
		Transfer(encoded, Monsters[id]);
		Codec decoded { { }, encoded.bytes, 0, true };
		world.monsters.emplace_back();
		world.monsters.back().first = id;
		Transfer(decoded, world.monsters.back().second);
	}
	auto missileEnd = Missiles.begin();
	std::advance(missileEnd, std::min<size_t>(Missiles.size(), MaxWorldMissiles));
	world.missiles.assign(Missiles.begin(), missileEnd);
	for (int i = 0; i < ActiveObjectCount; ++i)
		world.objects.emplace_back(static_cast<unsigned>(ActiveObjects[i]), Objects[ActiveObjects[i]]);
	for (unsigned i = 0; i < ActiveItemCount; ++i) {
		const unsigned id = ActiveItems[i];
		DroppedItem item { id, { }, Items[id].position, Items[id].AnimInfo.currentFrame, Items[id]._iAnimFlag };
		PackNetItem(Items[id], item.packed);
		world.items.push_back(item);
	}
	std::copy(std::begin(Quests), std::end(Quests), world.quests.begin());
	std::copy(std::begin(Portals), std::end(Portals), world.portals.begin());
	world.tiles = CaptureTiles();
	Codec codec;
	TransferWorld(codec, world);
	if (codec.bytes.size() > MaxRawWorldBytes) return { };
	Bytes compressed(compressBound(codec.bytes.size()) + 4);
	uint32_t size = static_cast<uint32_t>(codec.bytes.size());
	for (unsigned i = 0; i < 4; ++i)
		compressed[i] = static_cast<std::byte>((size >> (i * 8)) & 255);
	uLongf length = compressed.size() - 4;
	if (compress2(reinterpret_cast<Bytef *>(compressed.data() + 4), &length, reinterpret_cast<const Bytef *>(codec.bytes.data()), codec.bytes.size(), Z_BEST_SPEED) != Z_OK)
		return { };
	compressed.resize(length + 4);
	return compressed;
}

void RestorePlayer(Player &player, const PlayerReplica &replica, bool deferInventory)
{
	const auto &p = replica.packed;
	if (!deferInventory) {
		UnPackNetItem(player, replica.held, player.HoldItem);
		if (&player == MyPlayer && !player.HoldItem.isEmpty())
			NewCursor(player.HoldItem);
		else if (&player == MyPlayer && pcurs >= CURSOR_FIRSTITEM)
			NewCursor(CURSOR_HAND);
	}

	const auto inventory = std::span<const std::byte>(reinterpret_cast<const std::byte *>(&p.InvBody), sizeof(p.InvBody) + sizeof(p.InvList) + sizeof(p.InvGrid) + sizeof(p._pNumInv) + sizeof(p.SpdList));
	const uint32_t hash = net::async::PayloadHash(inventory);
	if (deferInventory) {
		ItemHashValid[player.getId()] = false;
	} else if (!ItemHashValid[player.getId()] || ItemHashes[player.getId()] != hash) {
		for (unsigned i = 0; i < NUM_INVLOC; ++i)
			UnPackNetItem(player, p.InvBody[i], player.InvBody[i]);
		for (unsigned i = 0; i < InventoryGridCells; ++i)
			UnPackNetItem(player, p.InvList[i], player.InvList[i]);
		for (unsigned i = 0; i < MaxBeltItems; ++i)
			UnPackNetItem(player, p.SpdList[i], player.SpdList[i]);
		std::copy(std::begin(p.InvGrid), std::end(p.InvGrid), player.InvGrid);
		player._pNumInv = p._pNumInv;
		CalcPlrInv(player, false);
		ItemHashes[player.getId()] = hash;
		ItemHashValid[player.getId()] = true;
	}
	player._pBaseStr = p.pBaseStr;
	player._pBaseMag = p.pBaseMag;
	player._pBaseDex = p.pBaseDex;
	player._pBaseVit = p.pBaseVit;
	player._pStrength = Swap32LE(p.pStrength);
	player._pMagic = Swap32LE(p.pMagic);
	player._pDexterity = Swap32LE(p.pDexterity);
	player._pVitality = Swap32LE(p.pVitality);
	player._pDamageMod = SwapSigned32LE(p.pDamageMod);
	player._pIMinDam = SwapSigned32LE(p.pIMinDam);
	player._pIMaxDam = SwapSigned32LE(p.pIMaxDam);
	player._pIAC = SwapSigned32LE(p.pIAC);
	player._pIBonusDam = SwapSigned32LE(p.pIBonusDam);
	player._pIBonusToHit = SwapSigned32LE(p.pIBonusToHit);
	player._pIBonusAC = SwapSigned32LE(p.pIBonusAC);
	player._pIBonusDamMod = SwapSigned32LE(p.pIBonusDamMod);
	player._pIGetHit = SwapSigned32LE(p.pIGetHit);
	player._pIEnAc = SwapSigned32LE(p.pIEnAc);
	player._pIFMinDam = SwapSigned32LE(p.pIFMinDam);
	player._pIFMaxDam = SwapSigned32LE(p.pIFMaxDam);
	player._pILMinDam = SwapSigned32LE(p.pILMinDam);
	player._pILMaxDam = SwapSigned32LE(p.pILMaxDam);

	player.setCharacterLevel(p.pLevel);
	player._pStatPts = p.pStatPts;
	player._pExperience = Swap32LE(p.pExperience);
	player._pHitPoints = Swap32LE(p.pHitPoints);
	player._pMaxHP = Swap32LE(p.pMaxHP);
	player._pHPBase = Swap32LE(p.pHPBase);
	player._pMaxHPBase = Swap32LE(p.pMaxHPBase);
	player._pMana = Swap32LE(p.pMana);
	player._pMaxMana = Swap32LE(p.pMaxMana);
	player._pManaBase = Swap32LE(p.pManaBase);
	player._pMaxManaBase = Swap32LE(p.pMaxManaBase);
	player._pMemSpells = Swap64LE(p.pMemSpells);
	std::copy(std::begin(p.pSplLvl), std::end(p.pSplLvl), player._pSplLvl);
	player.wReflections = Swap16LE(p.wReflections);
	player.pManaShield = p.pManaShield != 0;
	player.friendlyMode = p.friendlyMode != 0;
	player.position = replica.position;
	player._pmode = replica.mode;
	if (!deferInventory) player._pGold = replica.gold;
	player.plractive = replica.active;
	player._pdir = static_cast<Direction>(p.pdir);
	player._pInvincible = replica.invincible;
	player._pLvlChanging = replica.changingLevel;
	TargetSpells[player.getId()] = replica.targetedSpell;
	if (&player == MyPlayer && replica.targetedSpell.ready && replica.targetedSpell.token != LastUiSpellToken) {
		LastUiSpellToken = replica.targetedSpell.token;
		int cursor = CURSOR_HAND;
		switch (static_cast<SpellID>(replica.targetedSpell.spell)) {
		case SpellID::Identify: cursor = CURSOR_IDENTIFY; break;
		case SpellID::ItemRepair: cursor = CURSOR_REPAIR; break;
		case SpellID::StaffRecharge: cursor = CURSOR_RECHARGE; break;
		case SpellID::Resurrect: cursor = CURSOR_RESURRECT; break;
		case SpellID::HealOther: cursor = CURSOR_HEALOTHER; break;
		case SpellID::Telekinesis: cursor = CURSOR_TELEKINESIS; break;
		case SpellID::TrapDisarm: cursor = CURSOR_DISARM; break;
		default: break;
		}
		if (cursor == CURSOR_IDENTIFY || cursor == CURSOR_REPAIR || cursor == CURSOR_RECHARGE) {
			invflag = true;
			SpellbookFlag = false;
		}
		NewCursor(cursor);
	}

	if (&player == MyPlayer && !replica.targetedSpell.ready && replica.targetedSpell.token == LastUiSpellToken) {
		const auto spell = static_cast<SpellID>(replica.targetedSpell.spell);
		if ((spell == SpellID::Identify && pcurs == CURSOR_IDENTIFY) || (spell == SpellID::ItemRepair && pcurs == CURSOR_REPAIR)
		    || (spell == SpellID::StaffRecharge && pcurs == CURSOR_RECHARGE) || (spell == SpellID::Resurrect && pcurs == CURSOR_RESURRECT)
		    || (spell == SpellID::HealOther && pcurs == CURSOR_HEALOTHER) || (spell == SpellID::Telekinesis && pcurs == CURSOR_TELEKINESIS)
		    || (spell == SpellID::TrapDisarm && pcurs == CURSOR_DISARM)) NewCursor(CURSOR_HAND);
	}
	player._pFireResist = replica.resistFire;
	player._pLghtResist = replica.resistLightning;
	player._pMagResist = replica.resistMagic;
	player._pgfxnum = replica.graphics;
	player.destAction = static_cast<action_id>(replica.action);
	player.destParam1 = replica.parameter1;
	player.destParam2 = replica.parameter2;
	player.destParam3 = replica.parameter3;
	player.setLevel(p.plrlevel);
	player.plrIsOnSetLevel = p.isOnSetLevel != 0;
	// Reconciliation replaces the animation timeline too. Retaining private
	// distribution counters while moving currentFrame backwards can advance
	// walking progress beyond one tile and assert during prediction/rendering.
	player.AnimInfo = replica.animation;
	player.AnimInfo.numberOfFrames = std::max<int8_t>(1, player.AnimInfo.numberOfFrames);
	player.AnimInfo.ticksPerFrame = std::max<int8_t>(1, player.AnimInfo.ticksPerFrame);
	player.AnimInfo.currentFrame = std::clamp<int8_t>(player.AnimInfo.currentFrame, 0, player.AnimInfo.numberOfFrames - 1);
	player.AnimInfo.tickCounterOfCurrentFrame = std::clamp<int8_t>(player.AnimInfo.tickCounterOfCurrentFrame, 0, player.AnimInfo.ticksPerFrame - 1);
	SyncPlrAnim(player);
	if (&player == MyPlayer) {
		const bool wasDead = MyPlayerIsDead;
		MyPlayerIsDead = player._pHitPoints <= 0;
		if (wasDead && !MyPlayerIsDead) gamemenu_off();
	}
}

bool ValidateWorld(const World &world)
{
	// Keep malformed snapshot diagnostics bounded; failures must identify the
	// offending record so a rejected rebase doesn't look like a generic timeout.
	auto reject = [](std::string_view reason) {
		static unsigned reports = 0;
		if (reports++ < 8) LogInfo("Rejected authoritative world field: {}", reason);
		return false;
	};
	auto validItem = [](const ItemNetPack &p) { return static_cast<_item_indexes>(Swap16LE(p.def.wIndx)) != IDI_EAR || std::memchr(p.ear.heroname, 0, sizeof(p.ear.heroname)) != nullptr; };
	for (const auto &stock : world.stock)
		for (const auto &item : stock)
			if (!validItem(item.packed) || item.price < 0 || item.price > 100000000) return reject("stock");
	for (const auto &entries : world.buyback)
		for (const auto &item : entries)
			if (!validItem(item.packed) || item.price <= 0 || item.price > 100000000) return reject("buyback");
	for (const auto &item : world.items)
		if (!validItem(item.packed)) return reject("ground item");
	if (world.version != WorldSnapshotVersion || world.level >= NUMLEVELS || world.monsters.size() > MaxMonsters)
		return reject("version, level, or monster count");
	for (const auto &p : world.players) {
		if (!p.active) continue;
		if (!validItem(p.held) || std::memchr(p.packed.pName, 0, sizeof(p.packed.pName)) == nullptr) return reject("player name or held item");
		for (const auto &item : p.packed.InvBody)
			if (!validItem(item)) return reject("equipment item");
		for (const auto &item : p.packed.InvList)
			if (!validItem(item)) return reject("inventory item");
		for (const auto &item : p.packed.SpdList)
			if (!validItem(item)) return reject("belt item");
		if (p.packed.pClass >= GetNumPlayerClasses() || p.packed.pLevel < 1 || p.packed.pLevel > 50 || p.packed._pNumInv > InventoryGridCells
		    || p.packed.pdir >= 8 || p.mode > PM_QUIT || p.packed.plrlevel >= NUMLEVELS || !InDungeonBounds(p.position.tile)
		    || !InDungeonBounds(p.position.future) || !InDungeonBounds(p.position.last)) return reject("player class, mode, or position");
		for (const int8_t cell : p.packed.InvGrid)
			if (cell < -static_cast<int>(InventoryGridCells) || cell > static_cast<int>(InventoryGridCells)) return reject("inventory grid");
	}
	std::array<bool, MaxMonsters> ids { };
	for (const auto &[id, m] : world.monsters) {
		if (id >= MaxMonsters || ids[id] || m.levelType >= LevelMonsterTypeCount || m.mode > MonsterMode::Talk || static_cast<unsigned>(m.direction) >= 8 || !InDungeonBounds(m.position.tile)) {
			static unsigned monsterReports = 0;
			if (monsterReports++ < 4)
				LogInfo("Invalid monster: id {}, duplicate {}, type {}/{}, mode {}, direction {}, tile {},{}", id,
				    id < MaxMonsters && ids[id], m.levelType, LevelMonsterTypeCount, static_cast<unsigned>(m.mode),
				    static_cast<unsigned>(m.direction), m.position.tile.x, m.position.tile.y);
			return reject("monster id, type, mode, direction, or position");
		}
		if (m.isUnique() && static_cast<size_t>(m.uniqueType) >= UniqueMonstersData.size()) return reject("unique monster type");
		ids[id] = true;
	}
	for (const auto &[id, object] : world.objects)
		if (id >= MAXOBJECTS || static_cast<size_t>(object._otype) >= AllObjects.size() || !InDungeonBounds(object.position)) return reject("object");
	for (const auto &item : world.items)
		if (item.id >= MAXITEMS || !InDungeonBounds(item.position)) return reject("ground item id or position");
	for (const auto &m : world.missiles) {
		if (static_cast<size_t>(m._miAnimType) >= GetMissileSpriteDataCount() && m._miAnimType != MissileGraphicID::None) return reject("missile animation");
		if (static_cast<unsigned>(m._mitype) > static_cast<unsigned>(gbIsHellfire ? MissileID::LAST : MissileID::LastDiablo) || static_cast<unsigned>(m._micaster) > TARGET_BOTH || m._misource < -1) return reject("missile type or caster");
		if (m._misource >= 0 && static_cast<unsigned>(m._misource) >= (m._micaster == TARGET_PLAYERS ? MaxMonsters : MAX_PLRS)) return reject("missile source");
	}
	return true;
}

void Install(const net::async::State &state, std::span<const net::async::Command> pending)
{
	if (state.payload.size() < 4) return;
	uint32_t size = 0;
	for (unsigned i = 0; i < 4; ++i)
		size |= std::to_integer<uint32_t>(state.payload[i]) << (i * 8);
	if (size == 0 || size > MaxRawWorldBytes) return;
	Bytes raw(size);
	uLongf rawSize = size;
	if (uncompress(reinterpret_cast<Bytef *>(raw.data()), &rawSize, reinterpret_cast<const Bytef *>(state.payload.data() + 4), state.payload.size() - 4) != Z_OK || rawSize != size) return;
	World world;
	Codec decoder { { }, raw, 0, true };
	TransferWorld(decoder, world);
	if (!decoder.valid || decoder.offset != raw.size() || world.version != WorldSnapshotVersion || world.level >= NUMLEVELS || (world.setLevel && (world.setNumber < 1 || world.setNumber > SL_ARENA_CIRCLE_OF_LIFE || world.dungeonType < DTYPE_CATHEDRAL || world.dungeonType > DTYPE_CRYPT))) {
		static bool warnedDecode = false;
		if (!warnedDecode) {
			LogInfo("Rejected authoritative world decoding: level {}, offset {} of {}, valid {}", world.level, decoder.offset, raw.size(), decoder.valid);
			warnedDecode = true;
		}
		return;
	}
	if (IsSurvivorsMode() && !SurvivorsSectorSnapshotIsCurrent(world.sectorEpoch, SurvivorsLoadedSectorEpoch(), SurvivorsSectorEpoch())) return;
	if (world.level != static_cast<unsigned>(currlevel) || world.setLevel != setlevel || (world.setLevel && world.setNumber != setlvlnum)) {
		if (MyPlayer->_pLvlChanging && MyPlayer->plrlevel == world.level && MyPlayer->plrIsOnSetLevel == world.setLevel) return;
		Installing = true;
		MyPlayer->_pLvlVisited[world.level] = false;
		if (world.setLevel) {
			setlvltype = world.dungeonType;
			StartNewLvl(*MyPlayer, WM_DIABSETLVL, world.setNumber);
		} else if (setlevel && static_cast<unsigned>(GetMapReturnLevel()) == world.level)
			StartNewLvl(*MyPlayer, WM_DIABRTNLVL, static_cast<int>(world.level));
		else {
			setlevel = false;
			StartNewLvl(*MyPlayer, WM_DIABNEXTLVL, static_cast<int>(world.level));
		}
		Installing = false;
		return;
	}
	if (!ValidateWorld(world)) {
		static bool warned = false;
		if (!warned) {
			LogInfo("Rejected authoritative world: level {}, monsters {}, local monster types {}", world.level, world.monsters.size(), LevelMonsterTypeCount);
			warned = true;
		}
		return;
	}
	if (world.stock[0].size() > NumSmithBasicItemsHf || world.stock[1].size() > NumSmithItemsHf || world.stock[2].size() > NumWitchItemsHf || world.stock[3].size() > NumHealerItemsHf || world.stock[4].size() != 1) return;
	const auto tiles = CaptureTiles();
	if (world.tiles.size() != tiles.size()) return;
	bool rebased = false;
	if (world.rolling != SurvivorsRollingActive()) {
		static bool warnedRollingMismatch = false;
		if (!warnedRollingMismatch) {
			EventPlrMsg("This hunt requires matching procedural-world settings on every player.");
			warnedRollingMismatch = true;
		}
		return;
	}
	if (world.rolling) {
		const uint64_t epoch = static_cast<uint64_t>(world.rollingEpochLow) | (static_cast<uint64_t>(world.rollingEpochHigh) << 32);
		const uint64_t xBits = static_cast<uint64_t>(world.originXLow) | (static_cast<uint64_t>(world.originXHigh) << 32);
		const uint64_t yBits = static_cast<uint64_t>(world.originYLow) | (static_cast<uint64_t>(world.originYHigh) << 32);
		const int64_t x = std::bit_cast<int64_t>(xBits);
		const int64_t y = std::bit_cast<int64_t>(yBits);
		if (epoch < SurvivorsRollingEpoch()) return;
		const bool shifted = epoch != SurvivorsRollingEpoch() || x != SurvivorsRollingOriginX() || y != SurvivorsRollingOriginY();
		if (!SurvivorsRollingApplyOrigin(x, y, epoch)) return;
		if (shifted) {
			rebased = true;
			// Snapshot positions are in the new local frame. Interpolating against
			// the previous frame would visibly fling actors across the window.
			Visuals = {};
			MonsterVisuals = {};
		}
	}
	Installing = true;
	const uint32_t now = SDL_GetTicks();
	Codec encodedStock;
	for (auto &stock : world.stock)
		for (auto &item : stock) {
			encodedStock.Raw(&item.packed, sizeof(item.packed));
			encodedStock.Scalar(item.price);
		}
	const uint32_t stockHash = net::async::PayloadHash(encodedStock.bytes);
	const bool stockChanged = stockHash != StockHash;
	StockHash = stockHash;
	auto restoreStock = [&](unsigned id, auto &items) { items.clear(); for(const auto &stock:world.stock[id]) { Item item; UnPackNetItem(*MyPlayer,stock.packed,item); item._iIvalue=stock.price; items.push_back(item); } };
	restoreStock(0, SmithItems);
	restoreStock(1, PremiumItems);
	restoreStock(2, WitchItems);
	restoreStock(3, HealerItems);
	UnPackNetItem(*MyPlayer, world.stock[4][0].packed, BoyItem);
	BoyItem._iIvalue = world.stock[4][0].price;
	if (stockChanged && (ActiveStore == TalkID::SmithBuy || ActiveStore == TalkID::SmithPremiumBuy || ActiveStore == TalkID::WitchBuy || ActiveStore == TalkID::HealerBuy)) {
		const int line = CurrentTextLine, scroll = ScrollPos;
		StartStore(ActiveStore);
		CurrentTextLine = line;
		ScrollPos = scroll;
	}

	const bool localInventoryPending = std::any_of(pending.begin(), pending.end(), [](const net::async::Command &command) {
		return InventoryMutationIntent(command.payload);
	});
	for (unsigned i = 0; i < Players.size(); ++i) {
		if (!world.players[i].active) {
			Players[i].plractive = false;
			continue;
		}
		const Point old = ActorPixels(Players[i].position, Players[i].isWalking(), Players[i]._pdir, Players[i].AnimInfo);
		RestorePlayer(Players[i], world.players[i], i == MyPlayerId && localInventoryPending);
		if (i == MyPlayerId)
			SurvivorsObserveLedgerGeneration(static_cast<uint64_t>(world.players[i].ledgerGenerationLow) | (static_cast<uint64_t>(world.players[i].ledgerGenerationHigh) << 32));
		const Point current = ActorPixels(Players[i].position, Players[i].isWalking(), Players[i]._pdir, Players[i].AnimInfo);
		Visuals[i] = { rebased ? current : (Visuals[i].valid ? Visuals[i].current : old), current, now, true };
	}
	for (unsigned peer = 0; peer < Players.size(); ++peer) {
		std::vector<Item> items;
		std::vector<int> prices;
		for (const auto &entry : world.buyback[peer]) {
			Item item;
			UnPackNetItem(Players[peer], entry.packed, item);
			if (item.isEmpty()) { items.clear(); prices.clear(); break; }
			items.push_back(item);
			prices.push_back(entry.price);
		}
		SetSurvivorsBuyback(peer, items, prices);
	}
	for (unsigned i = 0; i < MaxMonsters; ++i)
		Monsters[i].isInvalid = true;
	ActiveMonsterCount = world.monsters.size();
	for (size_t i = 0; i < world.monsters.size(); ++i) {
		auto &[id, monster] = world.monsters[i];
		monster.uniqueMonsterTRN = std::move(Monsters[id].uniqueMonsterTRN);
		const Point current = ActorPixels(monster.position, IsMonsterModeMove(monster.mode), monster.direction, monster.animInfo);
		MonsterVisuals[id] = { MonsterVisuals[id].valid ? MonsterVisuals[id].current : current, current, now, true };
		Monsters[id] = std::move(monster);
		ActiveMonsters[i] = id;
		(void)SyncMonsterAnim(Monsters[id]);
	}
	std::array<bool, MaxMonsters> active { };
	for (size_t i = 0; i < ActiveMonsterCount; ++i)
		active[ActiveMonsters[i]] = true;
	size_t next = ActiveMonsterCount;
	for (unsigned id = 0; id < MaxMonsters; ++id)
		if (!active[id]) ActiveMonsters[next++] = id;
	Missiles.assign(world.missiles.begin(), world.missiles.end());
	SetUpMissileAnimationData();
	ActiveObjectCount = static_cast<int>(world.objects.size());
	for (size_t i = 0; i < world.objects.size(); ++i) {
		auto &[id, object] = world.objects[i];
		Objects[id] = object;
		ActiveObjects[i] = id;
		SyncObjectAnim(Objects[id]);
	}
	for (auto &item : Items)
		item.clear();
	ActiveItemCount = static_cast<uint8_t>(world.items.size());
	for (size_t i = 0; i < world.items.size(); ++i) {
		const auto &item = world.items[i];
		ActiveItems[i] = item.id;
		ItemNetPack ground = item.packed;
		const uint16_t creationFlags = Swap16LE(ground.def.wCI);
		// Inventory packets reject CF_PREGEN because pickup clears it. Ground
		// replicas must retain it so their pickup identity matches the host.
		ground.def.wCI = Swap16LE(static_cast<uint16_t>(creationFlags & ~CF_PREGEN));
		UnPackNetItem(*MyPlayer, ground, Items[item.id]);
		Items[item.id]._iCreateInfo = creationFlags;
		Items[item.id].position = item.position;
		// This client does not simulate ProcessItems. SetupItem expects that
		// tick and would leave a pending animation at its initial frame forever.
		// Preserve the packed identification state and install the host's frame.
		AsyncNetRestoreItemAnimation(Items[item.id], item.frame, item.animate);
	}
	std::copy(world.quests.begin(), world.quests.end(), std::begin(Quests));
	std::copy(world.portals.begin(), world.portals.end(), std::begin(Portals));
	Codec grids { { }, world.tiles, 0, true };
	TransferTiles(grids);
	std::memset(dPlayer, 0, sizeof(dPlayer));
	std::memset(dMonster, 0, sizeof(dMonster));
	std::memset(dObject, 0, sizeof(dObject));
	std::memset(dItem, 0, sizeof(dItem));
	for (auto &player : Players)
		if (player.plractive && player.isOnActiveLevel()) player.occupyTile(player.position.tile, false);
	for (size_t i = 0; i < ActiveMonsterCount; ++i) {
		Monster &m = Monsters[ActiveMonsters[i]];
		if (!m.isInvalid) m.occupyTile(m.position.tile, false);
	}
	for (int i = 0; i < ActiveObjectCount; ++i) {
		const Object &o = Objects[ActiveObjects[i]];
		dObject[o.position.x][o.position.y] = static_cast<int8_t>(ActiveObjects[i] + 1);
	}
	for (unsigned i = 0; i < ActiveItemCount; ++i) {
		const auto id = ActiveItems[i];
		const auto pos = Items[id].position;
		dItem[pos.x][pos.y] = static_cast<int8_t>(id + 1);
	}
	RedoMissileFlags();
	// Visibility is a local presentation projection. The server's tile copy
	// reflects player 0's view and cannot hide another peer's nearby world.
	if (leveltype != DTYPE_TOWN) {
		for (auto &row : dFlags)
			for (auto &flags : row)
				flags &= ~DungeonFlag::Visible;
		TransList = { };
		DoVision(MyPlayer->position.tile, MyPlayer->_pLightRad, MAP_EXP_SELF, true);
		DoLighting(MyPlayer->position.tile, MyPlayer->_pLightRad, { });
	}

	SetRndSeed(world.rng);
	// Reapply only unacknowledged movement intentions. Combat stays unpredicted;
	// no speculative missiles, damage, loot or rewards can enter the world.
	for (const auto &command : pending) {
		const auto payload = UnwrapRuntimeInput(command.payload, MyPlayerId);
		if (!payload.empty() && static_cast<_cmd_id>(payload[0]) == CMD_WALKXY)
			AsyncNetApplyNativeCommands(MyPlayerId, payload.data(), payload.size());
	}
	const unsigned replayTicks = std::min(16U, PredictedTick > state.tick ? PredictedTick - state.tick : 0U);
	for (unsigned tick = 0; tick < replayTicks; ++tick)
		AsyncNetAdvancePredictedPlayer(*MyPlayer);
	ViewPosition = MyPlayer->position.tile;
	PredictedTick = std::max(PredictedTick, state.tick);
	SetRndSeed(world.rng);
	LastReceive = now;
	InstalledTick = state.tick;
	++InstalledSnapshots;
	Installing = false;
}

bool IsValidSpellIntent(const Player &player, _cmd_id type, std::span<const std::byte> payload)
{
	unsigned spell = 0, source = 0;
	if (type == CMD_SPELLXY && payload.size() == sizeof(TCmdLocParam3)) {
		TCmdLocParam3 m;
		std::memcpy(&m, payload.data(), sizeof(m));
		spell = Swap16LE(m.wParam1);
		source = Swap16LE(m.wParam2);
	} else if (type == CMD_SPELLXYD && payload.size() == sizeof(TCmdLocParam4)) {
		TCmdLocParam4 m;
		std::memcpy(&m, payload.data(), sizeof(m));
		spell = Swap16LE(m.wParam1);
		source = Swap16LE(m.wParam2);
	} else if ((type == CMD_SPELLID || type == CMD_SPELLPID) && payload.size() == sizeof(TCmdParam4)) {
		TCmdParam4 m;
		std::memcpy(&m, payload.data(), sizeof(m));
		spell = Swap16LE(m.wParam2);
		source = Swap16LE(m.wParam3);
	} else
		return type != CMD_SPELLXY && type != CMD_SPELLXYD && type != CMD_SPELLID && type != CMD_SPELLPID;
	if (spell > static_cast<unsigned>(SpellID::LAST) || source > static_cast<unsigned>(SpellType::Charges)) return false;
	const auto id = static_cast<SpellID>(spell);
	if (!IsValidSpell(id) || (leveltype == DTYPE_TOWN && !GetSpellData(id).isAllowedInTown())) return false;
	const auto kind = static_cast<SpellType>(source);
	if (kind == SpellType::Skill) return (player._pAblSpells & GetSpellBitmask(id)) != 0;
	if (kind == SpellType::Spell) return (player._pMemSpells & GetSpellBitmask(id)) != 0 && CheckSpell(player, id, kind, true) == SpellCheckResult::Success;
	if (kind == SpellType::Charges) return CanUseStaff(const_cast<Player &>(player), id);
	return CanUseScroll(const_cast<Player &>(player), id);
}

bool IsIntent(_cmd_id cmd)
{
	if (cmd == CMD_SATTACKXY) return IsSurvivorsMode();
	switch (cmd) {
	case CMD_PLAYER_JOINLEVEL:
	case CMD_STAND:
	case CMD_WALKXY:
	case CMD_ADDSTR:
	case CMD_ADDMAG:
	case CMD_ADDDEX:
	case CMD_ADDVIT:
	case CMD_PUTITEM:
	case CMD_RATTACKXY:
	case CMD_SPELLXY:
	case CMD_OPOBJXY:
	case CMD_GOTOGETITEM:
	case CMD_GOTOAGETITEM:
	case CMD_REQUESTGITEM:
	case CMD_REQUESTAGITEM:
	case CMD_CHANGEPLRITEMS:
	case CMD_DELPLRITEMS:
	case CMD_CHANGEINVITEMS:
	case CMD_DELINVITEMS:
	case CMD_CHANGEBELTITEMS:
	case CMD_DELBELTITEMS:
	case CMD_FRIENDLYMODE:
	case CMD_HEALOTHER:
	case CMD_SPELLXYD:
	case CMD_DISARMXY:
	case CMD_ATTACKID:
	case CMD_ATTACKPID:
	case CMD_RATTACKID:
	case CMD_RATTACKPID:
	case CMD_SPELLID:
	case CMD_SPELLPID:
	case CMD_RESURRECT:
	case CMD_OPOBJT:
	case CMD_KNOCKBACK:
	case CMD_TALKXY:
	case CMD_NEWLVL:
	case CMD_WARP:
	case CMD_RETOWN:
		return true;
	default: return false;
	}
}

bool IsDeadForRestart(const Player &player)
{
	// hasNoLife() is always false in town, so test the life and mode directly.
	return player._pmode == PM_DEATH || (player._pHitPoints >> 6) <= 0;
}

// Peers share the host's level (see the CMD_NEWLVL rule in AsyncNetBeforeTick),
// so a dead peer cannot restart in town by itself. It comes back beside the
// host with the life and mana that Restart in Town gives. Items dropped at
// the corpse stay on the ground for it to collect.
void RespawnPeerBesideAuthority(Player &player)
{
	RemoveEnemyReferences(player);
	RemovePlrMissiles(player);
	FixPlrWalkTags(player);
	ClrPlrPath(player);
	player.destAction = ACTION_NONE;
	player._pLvlChanging = false;
	SetPlayerHitPoints(player, 64);
	player._pMana = 0;
	player._pManaBase = player._pMana - (player._pMaxMana - player._pMaxManaBase);
	CalcPlrInv(player, false);
	TargetSpells[player.getId()].ready = false;
	player.position.tile = MyPlayer->position.tile;
	player.position.future = player.position.tile;
	InitPlayer(player, false);
	SyncInitPlrPos(player);
}
} // namespace

bool AsyncNetEnabled() { return Running && gbIsMultiplayer && sgGameInitInfo.reserved[0] == net::async::GameProtocolTag; }
bool AsyncNetIsAuthority() { return !AsyncNetEnabled() || MyPlayerId == 0; }
bool AsyncNetAwaitingInitialSnapshot() { return AsyncNetNeedsInitialSnapshot(AsyncNetEnabled(), AsyncNetIsAuthority(), InstalledSnapshots); }
bool AsyncNetOwnsPlayer(const Player &player) { return AsyncNetEnabled() ? AsyncNetIsAuthority() : &player == MyPlayer; }
bool AsyncNetSimulateWorld() { return !AsyncNetEnabled() || (AsyncNetIsAuthority() && SurvivorsCommitCoopLedgers()); }
bool AsyncNetAllowLevelChange() { return !AsyncNetEnabled() || AsyncNetIsAuthority() || Installing; }
bool AsyncNetIsRemoteInteraction() { return RemoteInteraction; }
void AsyncNetInteractWithTowner(Player &player, int npc)
{
	if (!AsyncNetEnabled() || !AsyncNetIsAuthority() || npc < 0 || static_cast<size_t>(npc) >= Towners.size()) return;
	RemoteInteraction = true;
	TalkToTowner(player, npc);
	if (Towners[npc]._ttype == TOWN_HEALER && player.position.tile.WalkingDistance(Towners[npc].position) < 2) player.RestoreFullLife();
	RemoteInteraction = false;
}
void AsyncNetInitialize()
{
	Session.Reset(MyPlayerId, sgGameInitInfo.gameSeed[0] ^ sgGameInitInfo.gameSeed[2]);
	Running = gbIsMultiplayer && sgGameInitInfo.reserved[0] == net::async::GameProtocolTag;
	LastReceive = SDL_GetTicks();
	TargetSpells = { };
	TargetSpellWorld = { };
	LastUiSpellToken = 0;
	LastPublish = 0;
	StockHash = 0;
	PredictedTick = 0;
	PendingTowner = -1;
	InstalledTick = 0;
	InstalledSnapshots = 0;
	LastHello = 0;
	Negotiated.fill(false);
	ConnectedPeers.fill(false);
	NegotiationStarted.fill(0);
	MonsterVisuals = { };
	ItemHashes.fill(0);
	ItemHashValid.fill(false);
	Visuals = { };
	RewindTicks.fill(0);
	RewindHistory.clear();
	Predictions.clear();
	if (Running) {
		const std::array<std::byte, 1> version { static_cast<std::byte>(net::async::ProtocolVersion) };
		Session.SendExtension(255, version, Send);
	}
}
void AsyncNetShutdown()
{
	// The final Survivors checkpoint is committed before NetClose reaches
	// here. Release at session end, including headless hosts that stay in the
	// same mode and never return through the camp menu.
	pfile_end_survivors_shared_authority();
	Running = false;
	Installing = false;
	RewindHistory.clear();
	Predictions.clear();
}

bool AsyncNetSendCommand(std::span<const std::byte> command)
{
	if (!AsyncNetEnabled() || Installing) return false;
	if (command.empty() || ApplyingCommand) return true;
	const auto type = static_cast<_cmd_id>(command[0]);

	const bool targeted = type == CMD_RESURRECT || type == CMD_HEALOTHER || type == CMD_OPOBJT || type == CMD_KNOCKBACK || type == CMD_DISARMXY
	    || ((type == CMD_REQUESTAGITEM || type == CMD_REQUESTGITEM) && pcurs == CURSOR_TELEKINESIS);
	if (targeted) {
		const auto lease = TargetSpells[MyPlayerId];
		if (!lease.ready) return true;
		Codec request;
		request.bytes.push_back(static_cast<std::byte>(TargetWorldIntent));
		unsigned token = lease.token;
		request.Scalar(token);
		request.bytes.insert(request.bytes.end(), command.begin(), command.end());
		if (QueueRuntimeInput(request.bytes)) Session.PumpInputs(SDL_GetTicks(), Send);
		return true;
	}
	if (!IsIntent(type)) return true; // Ignore peer-authored damage/state messages; snapshots own those facts.
	if (!QueueRuntimeInput(command)) {
		EventPlrMsg("Network input backlog is full; awaiting authority acknowledgement.");
		return true;
	}
	if (type == CMD_TALKXY && command.size() == sizeof(TCmdLocParam1)) {
		TCmdLocParam1 talk;
		std::memcpy(&talk, command.data(), sizeof(talk));
		PendingTowner = Swap16LE(talk.wParam1);
	}
	Session.PumpInputs(SDL_GetTicks(), Send);
	// Senders predict their own movement immediately; other commands run only on the authority.
	if (MyPlayerId != 0 && type == CMD_WALKXY) AsyncNetApplyNativeCommands(MyPlayerId, command.data(), command.size());
	return true;
}

bool AsyncNetUseInventoryItem(unsigned slot)
{
	if (!AsyncNetEnabled() || ApplyingCommand || MyPlayer == nullptr || slot < INVITEM_INV_FIRST || slot > INVITEM_BELT_LAST)
		return false;
	const Item &item = GetInventoryItem(*MyPlayer, static_cast<int>(slot));
	if (item.isScroll() || item.isRune() || item._itype == ItemType::Gold || item._iMiscId == IMISC_MAPOFDOOM || item._iMiscId == IMISC_NOTE)
		return false;
	Codec request;
	request.bytes.push_back(static_cast<std::byte>(UseInventoryIntent));
	unsigned seed = item._iSeed;
	request.Fields(slot, seed);
	QueueRuntimeInput(request.bytes);
	Session.PumpInputs(SDL_GetTicks(), Send);
	return true;
}

bool AsyncNetHandlePacket(uint8_t sender, std::span<const std::byte> packet)
{
	if (packet.size() < 4) return false;
	if (packet[0] != std::byte { 'A' } || packet[1] != std::byte { 'N' } || packet[2] != std::byte { 'E' } || packet[3] != std::byte { 'T' }) return false;
	if (!AsyncNetEnabled()) return true;
#ifdef DAMNATION_S1_BRIDGE
	// Isolated rendered-bot QA can delay the first authority snapshot so the
	// joining overlay is observable. Other protocol traffic still proceeds.
	if (s1_headless::Rendered() && std::getenv("DAMNATIONX_QA_DELAY_INITIAL_SNAPSHOT") != nullptr
	    && !AsyncNetIsAuthority() && InstalledSnapshots == 0 && sender == 0
	    && packet.size() >= 8 && packet[5] == std::byte { 2 }
	    && SDL_GetTicks() - LastReceive < 8000U)
		return true;
#endif
	if (Session.Receive(sender, packet, Send, Install, [](uint8_t peer, uint8_t channel, auto bytes) {
		    if (channel == 255) return;
		    if (Extensions[channel]) Extensions[channel](peer, bytes);
	    })) Negotiated[sender] = true;
	return true;
}

void AsyncNetBeforeTick()
{
	if (!AsyncNetEnabled()) return;
	const uint32_t now = SDL_GetTicks();
	for (uint8_t peer = 0; peer < Players.size(); ++peer) {
		Session.SetPeerConnected(peer, Players[peer].plractive);
		if (!Players[peer].plractive) {
			if (ConnectedPeers[peer]) SurvivorsForgetCoopLedger(peer);
			if (ConnectedPeers[peer]) RewindHistory.clear();
			ConnectedPeers[peer] = false;
			Negotiated[peer] = false;
			NegotiationStarted[peer] = 0;
			RewindTicks[peer] = 0;
			TargetSpells[peer] = { };
			ItemHashes[peer] = 0;
			ItemHashValid[peer] = false;
			Visuals[peer] = { };
			continue;
		}
		ConnectedPeers[peer] = true;
		if (peer == MyPlayerId) continue;
		if (NegotiationStarted[peer] == 0) NegotiationStarted[peer] = now;
		if (AsyncNetIsAuthority() && !Negotiated[peer] && now - NegotiationStarted[peer] > (Players[peer]._pLvlChanging ? 30000U : 5000U))
			SNetDropPlayer(peer, net::leaveinfo_t::LEAVE_DROP);
	}
	if (now - LastHello >= 250) {
		const std::array<std::byte, 1> version { static_cast<std::byte>(net::async::ProtocolVersion) };
		Session.SendExtension(255, version, Send);
		LastHello = now;
	}
	Session.PumpInputs(SDL_GetTicks(), Send);
	if (AsyncNetIsAuthority()) {
		for (auto &player : Players) {
			if (!player.plractive || &player == MyPlayer || player.isOnActiveLevel()) continue;
			if (setlevel) {
				player.setLevel(setlvlnum);
				player._pSLvlVisited[setlvlnum] = true;
			} else {
				player.setLevel(currlevel);
				player._pLvlVisited[currlevel] = true;
			}
			player._pLvlChanging = false;
			TargetSpells[player.getId()].ready = false;
			player.position.tile = MyPlayer->position.tile;
			player.position.future = player.position.tile;
			InitPlayer(player, false);
			SyncInitPlrPos(player);
		}
		// Do not execute or acknowledge gameplay input for an unadmitted peer,
		// or while any prior authoritative gear change cannot be committed.
		if (!SurvivorsCommitCoopLedgers()) return;
		Session.AdvanceAuthority([](uint8_t peer, const net::async::Command &command, uint32_t rewind) {
			if (peer >= Players.size() || !Players[peer].plractive || command.payload.empty() || !SurvivorsCoopPeerLedgerReady(peer)) return;
			const Bytes decodedInput = UnwrapRuntimeInput(command.payload, peer);
			std::span<const std::byte> payload = decodedInput;
			if (payload.empty()) return;
			bool targeted = false;
			unsigned targetToken = 0;
			if (std::to_integer<uint8_t>(payload[0]) == TargetWorldIntent) {
				if (payload.size() < 6) return;
				Codec envelope { { }, payload.subspan(1, 4), 0, true };
				envelope.Scalar(targetToken);
				payload = payload.subspan(5);
				targeted = true;
			}
			const auto type = static_cast<_cmd_id>(payload[0]);
			Player &player = Players[peer];
			if (payload.size() == 17 && std::to_integer<uint8_t>(payload[0]) == TargetItemIntent) {
				Codec request { { }, payload.subspan(1), 0, true };
				unsigned token = 0, spell = 0, slot = 0, seed = 0;
				request.Fields(token, spell, slot, seed);
				auto &lease = TargetSpells[peer];
				if (!lease.ready || TargetSpellWorld[peer] != WorldContextKey() || lease.token != token || lease.spell != spell || player.hasNoLife() || slot >= static_cast<unsigned>(NUM_INVLOC + player._pNumInv)) return;
				Item &item = GetInventoryItem(player, slot);
				if (item.isEmpty() || item._iSeed != seed) return;
				const SpellID action = static_cast<SpellID>(spell);
				if (action != SpellID::Identify && action != SpellID::ItemRepair && action != SpellID::StaffRecharge) return;
				if (action == SpellID::Identify && (item._iIdentified || item._iMagical == ITEM_QUALITY_NORMAL)) return;
				if (action == SpellID::ItemRepair && (item._iMaxDur == DUR_INDESTRUCTIBLE || item._iDurability >= item._iMaxDur)) return;
				if (action == SpellID::StaffRecharge && (item._itype != ItemType::Staff || !IsValidSpell(item._iSpell) || item._iCharges >= item._iMaxCharges)) return;
				lease.ready = false;
				ApplyingCommand = true;
				if (action == SpellID::Identify) CheckIdentify(player, slot);
				if (action == SpellID::ItemRepair) DoRepair(player, slot);
				if (action == SpellID::StaffRecharge) DoRecharge(player, slot);
				ApplyingCommand = false;
				return;
			}
			if (targeted) {
				auto &lease = TargetSpells[peer];
				if (!lease.ready || TargetSpellWorld[peer] != WorldContextKey() || lease.token != targetToken || player.hasNoLife() || !player.isOnActiveLevel()) return;
				SpellID expected = SpellID::Null;
				if (type == CMD_RESURRECT) expected = SpellID::Resurrect;
				if (type == CMD_HEALOTHER) expected = SpellID::HealOther;
				if (type == CMD_DISARMXY) expected = SpellID::TrapDisarm;
				if (type == CMD_OPOBJT || type == CMD_KNOCKBACK || type == CMD_REQUESTAGITEM || type == CMD_REQUESTGITEM) expected = SpellID::Telekinesis;
				if (expected == SpellID::Null || lease.spell != static_cast<uint8_t>(expected)) return;
				if (type == CMD_RESURRECT || type == CMD_HEALOTHER) {
					if (payload.size() != sizeof(TCmdParam1)) return;
					TCmdParam1 request;
					std::memcpy(&request, payload.data(), sizeof(request));
					const unsigned id = Swap16LE(request.wParam1);
					if (id >= Players.size() || id == peer || !Players[id].plractive || !Players[id].isOnActiveLevel() || player.position.tile.WalkingDistance(Players[id].position.tile) > 12 || !LineClearMissile(player.position.tile, Players[id].position.tile)) return;
					if ((type == CMD_RESURRECT) != (Players[id]._pHitPoints <= 0)) return;
				} else if (type == CMD_OPOBJT || type == CMD_DISARMXY) {
					if (payload.size() != sizeof(TCmdLoc)) return;
					TCmdLoc request;
					std::memcpy(&request, payload.data(), sizeof(request));
					const Point tile { request.x, request.y };
					if (!InDungeonBounds(tile) || FindObjectAtPosition(tile) == nullptr || player.position.tile.WalkingDistance(tile) > 12 || !LineClearMissile(player.position.tile, tile)) return;
				} else if (type == CMD_KNOCKBACK) {
					if (payload.size() != sizeof(TCmdParam1)) return;
					TCmdParam1 request;
					std::memcpy(&request, payload.data(), sizeof(request));
					const unsigned id = Swap16LE(request.wParam1);
					if (id >= MaxMonsters || Monsters[id].isInvalid || Monsters[id].hitPoints <= 0 || M_Talker(Monsters[id]) || Monsters[id].talkMsg != TEXT_NONE || player.position.tile.WalkingDistance(Monsters[id].position.tile) > 12 || !LineClearMissile(player.position.tile, Monsters[id].position.tile)) return;
				}
				if (type == CMD_REQUESTGITEM || type == CMD_REQUESTAGITEM) {
					if (payload.size() != sizeof(TCmdGItem)) return;
					TCmdGItem request;
					std::memcpy(&request, payload.data(), sizeof(request));
					if (request.bPnum != peer) return;
					bool found = false;
					for (unsigned i = 0; i < ActiveItemCount; ++i) {
						const Item &item = Items[ActiveItems[i]];
						if (item.keyAttributesMatch(Swap32LE(request.def.dwSeed), static_cast<_item_indexes>(Swap16LE(request.def.wIndx)), Swap16LE(request.def.wCI)) && player.position.tile.WalkingDistance(item.position) <= 12 && LineClearMissile(player.position.tile, item.position)) found = true;
					}
					if (!found) return;
				}
				lease.ready = false;
			} else if (type == CMD_RESURRECT || type == CMD_HEALOTHER || type == CMD_OPOBJT || type == CMD_KNOCKBACK || type == CMD_DISARMXY)
				return;

			if (payload.size() == 9 && std::to_integer<uint8_t>(payload[0]) == UseInventoryIntent) {
				Codec request { { }, std::span<const std::byte>(payload).subspan(1), 0, true };
				unsigned slot = 0, seed = 0;
				request.Fields(slot, seed);
				if (slot < INVITEM_INV_FIRST || slot > INVITEM_BELT_LAST || player.hasNoLife()) return;
				Item &item = GetInventoryItem(player, static_cast<int>(slot));
				if (item._iSeed != seed || item.isEmpty() || !item.isUsable() || !player.CanUseItem(item) || item.isScroll() || item.isRune()) return;
				switch (item._iMiscId) {
				case IMISC_HEAL:
				case IMISC_FULLHEAL:
				case IMISC_MANA:
				case IMISC_FULLMANA:
				case IMISC_REJUV:
				case IMISC_FULLREJUV:
				case IMISC_ELIXSTR:
				case IMISC_ELIXMAG:
				case IMISC_ELIXDEX:
				case IMISC_ELIXVIT:
				case IMISC_BOOK: break;
				default: return;
				}
				ApplyingCommand = true;
				UseItem(player, item._iMiscId, item._iSpell, slot);
				if (item._iMiscId == IMISC_BOOK) player._pMemSpells |= GetSpellBitmask(item._iSpell);
				if (slot <= INVITEM_INV_LAST)
					player.RemoveInvItem(slot - INVITEM_INV_FIRST);
				else
					player.RemoveSpdBarItem(slot - INVITEM_BELT_FIRST);
				ApplyingCommand = false;
				return;
			}
			if (payload.size() == 9 && std::to_integer<uint8_t>(payload[0]) == SurvivorsEquipIntent) {
				Codec request { { }, std::span<const std::byte>(payload).subspan(1), 0, true };
				unsigned index = 0, seed = 0;
				request.Fields(index, seed);
				if (!IsSurvivorsMode() || !player.isOnActiveLevel() || player.hasNoLife()) return;
				ApplyingCommand = true;
				SurvivorsEquipInventoryItem(player, index, seed);
				ApplyingCommand = false;
				return;
			}
			if (payload.size() == 13 && std::to_integer<uint8_t>(payload[0]) == StoreIntent) {
				Codec request { { }, std::span<const std::byte>(payload).subspan(1), 0, true };
				unsigned action = 0, index = 0, seed = 0;
				request.Fields(action, index, seed);
				if (action > static_cast<unsigned>(AsyncStoreAction::SurvivorsBuyback)) return;
				ApplyingCommand = true;
				ApplyAsyncStoreTransaction(player, static_cast<AsyncStoreAction>(action), index, seed);
				ApplyingCommand = false;
				return;
			}
			if (!IsIntent(type)) return;
			if (!IsValidSpellIntent(player, type, payload)) return;
			if (peer != MyPlayerId && (type == CMD_ADDSTR || type == CMD_ADDMAG || type == CMD_ADDDEX || type == CMD_ADDVIT)) {
				if (payload.size() != sizeof(TCmdParam1)) return;
				TCmdParam1 message;
				std::memcpy(&message, payload.data(), sizeof(message));
				const unsigned points = Swap16LE(message.wParam1);
				if (points == 0 || player._pStatPts < 0 || points > static_cast<unsigned>(player._pStatPts)) return;
				player._pStatPts -= points;
			}

			if ((type == CMD_REQUESTGITEM || type == CMD_REQUESTAGITEM) && payload.size() == sizeof(TCmdGItem)) {
				TCmdGItem request;
				std::memcpy(&request, payload.data(), sizeof(request));
				if (request.bPnum != peer) return;
				for (unsigned i = 0; i < ActiveItemCount; ++i) {
					const unsigned id = ActiveItems[i];
					Item &item = Items[id];
					if (item.keyAttributesMatch(Swap32LE(request.def.dwSeed), static_cast<_item_indexes>(Swap16LE(request.def.wIndx)), Swap16LE(request.def.wCI)) && player.position.tile.WalkingDistance(item.position) <= (targeted ? 12 : 2) && (!targeted || LineClearMissile(player.position.tile, item.position))) {
						ApplyingCommand = true;
						if (type == CMD_REQUESTGITEM)
							InvGetItem(player, id);
						else
							AutoGetItem(player, &item, id);
						ApplyingCommand = false;
						break;
					}
				}
				return;
			}
			// Until independent per-level authority is supported, the party travels
			// with the host. A peer cannot switch its collision world independently.
			if (peer != 0 && (type == CMD_NEWLVL || type == CMD_WARP)) return;
			// Esc or the Menu button on the death screen. The host restarts in
			// town through the normal handler and the party follows it there; a
			// peer respawns beside the host instead.
			if (type == CMD_RETOWN) {
				if (payload.size() != sizeof(TCmd) || !IsDeadForRestart(player)) return;
				if (peer != 0) {
					ApplyingCommand = true;
					RespawnPeerBesideAuthority(player);
					ApplyingCommand = false;
					return;
				}
			}
			RewindTicks[peer] = std::min({ net::async::MaxRewindTicks, static_cast<unsigned>(sgGameInitInfo.nTickRate) / 5U, Session.Tick() - rewind });
			ApplyingCommand = true;
			if (peer != MyPlayerId) {
				if (type == CMD_DELPLRITEMS && payload.size() == sizeof(TCmdDelItem)) {
					TCmdDelItem message;
					std::memcpy(&message, payload.data(), sizeof(message));
					if (message.bLoc < NUM_INVLOC) player.HoldItem = player.InvBody[message.bLoc];
				} else if ((type == CMD_DELINVITEMS || type == CMD_DELBELTITEMS) && payload.size() == sizeof(TCmdParam1)) {
					TCmdParam1 message;
					std::memcpy(&message, payload.data(), sizeof(message));
					const unsigned slot = Swap16LE(message.wParam1);
					if (type == CMD_DELBELTITEMS && slot < MaxBeltItems) player.HoldItem = player.SpdList[slot];
					if (type == CMD_DELINVITEMS && slot < InventoryGridCells) {
						const int id = std::abs(player.InvGrid[slot]) - 1;
						if (id >= 0 && id < player._pNumInv) player.HoldItem = player.InvList[id];
					}
				}
			}
			if (peer != MyPlayerId && (type == CMD_CHANGEPLRITEMS || type == CMD_CHANGEINVITEMS || type == CMD_CHANGEBELTITEMS) && payload.size() == sizeof(TCmdChItem)) {
				TCmdChItem message;
				std::memcpy(&message, payload.data(), sizeof(message));
				auto matches = [&](const Item &item) { return !item.isEmpty() && item.keyAttributesMatch(Swap32LE(message.def.dwSeed), static_cast<_item_indexes>(Swap16LE(message.def.wIndx)), Swap16LE(message.def.wCI)); };
				Item *owned = matches(player.HoldItem) ? &player.HoldItem : nullptr;
				for (auto &item : player.InvBody)
					if (matches(item)) owned = &item;
				for (auto &item : player.InvList)
					if (matches(item)) owned = &item;
				for (auto &item : player.SpdList)
					if (matches(item)) owned = &item;
				if (owned) {
					Item moved = *owned, replaced;
					if (type == CMD_CHANGEPLRITEMS && message.bLoc < NUM_INVLOC) {
						replaced = player.InvBody[message.bLoc];
						player.InvBody[message.bLoc] = moved;
						CheckInvSwap(player, static_cast<inv_body_loc>(message.bLoc));
					} else if (type == CMD_CHANGEBELTITEMS && message.bLoc < MaxBeltItems) {
						replaced = player.SpdList[message.bLoc];
						player.SpdList[message.bLoc] = moved;
					} else if (type == CMD_CHANGEINVITEMS && message.bLoc < InventoryGridCells) {
						const Size size = GetInventorySize(moved);
						const unsigned x = message.bLoc % 10, y = message.bLoc / 10;
						if (x + size.width <= 10 && y + size.height <= 4) {
							const int id = std::abs(player.InvGrid[message.bLoc]) - 1;
							if (id >= 0 && id < player._pNumInv) replaced = player.InvList[id];
							CheckInvSwap(player, moved, message.bLoc);
						}
					}
					if (owned == &player.HoldItem) {
						if (matches(replaced))
							player.HoldItem.clear();
						else
							player.HoldItem = replaced;
					}
					CalcPlrInv(player, false);
				}
			} else if (peer != MyPlayerId && type == CMD_PUTITEM && payload.size() == sizeof(TCmdPItem)) {
				TCmdPItem message;
				std::memcpy(&message, payload.data(), sizeof(message));
				if (player.HoldItem.keyAttributesMatch(Swap32LE(message.def.dwSeed), static_cast<_item_indexes>(Swap16LE(message.def.wIndx)), Swap16LE(message.def.wCI))) {
					if (!SurvivorsCanTransferCoopGround(peer)) {
						// The pickup that preceded this drop already removed the item
						// from its old slot. Return it if there is room; otherwise retain
						// it in the authoritative cursor rather than losing it.
						if (AutoPlaceItemInInventory(player, player.HoldItem) || AutoPlaceItemInBelt(player, player.HoldItem, true)) {
							player.HoldItem.clear();
							player._pGold = CalculateGold(player);
						}
					} else {
						const auto tile = FindAdjacentPositionForItem(player.position.tile, GetDirection(player.position.tile, { message.x, message.y }));
						if (tile) {
							PlaceItemInWorld(std::move(player.HoldItem), *tile);
							player.HoldItem.clear();
						}
					}
				}
			} else {
				AsyncNetApplyNativeCommands(peer, payload.data(), payload.size());
			}

			ApplyingCommand = false;
		});
		// Commit before another network pump can remove a disconnected player
		// slot (which clears its name and ledger identity).
		SurvivorsCommitCoopLedgers();
	} else if (SDL_GetTicks() - LastReceive > AsyncNetAuthorityTimeoutMs(InstalledSnapshots, MyPlayer != nullptr && MyPlayer->_pLvlChanging)) {
		gbRunGame = false;
		EventPlrMsg("Connection to the authority was lost.");
	}
}

void AsyncNetAfterTick()
{
	if (!AsyncNetEnabled() || !AsyncNetIsAuthority()) return;
	for (const auto &player : Players)
		if (TargetSpells[player.getId()].ready && (TargetSpellWorld[player.getId()] != WorldContextKey() || player._pHitPoints <= 0)) TargetSpells[player.getId()].ready = false;
	History history { };
	history.tick = Session.Tick();
	for (size_t i = 0; i < Players.size(); ++i)
		history.players[i] = Players[i].position.tile;
	for (size_t i = 0; i < MaxMonsters; ++i)
		history.monsters[i] = Monsters[i].position.tile;
	RewindHistory.push_back(history);
	while (RewindHistory.size() > 16)
		RewindHistory.pop_front();
	// Session.Publish carries input ACKs. Commit inventory and buyback first;
	// failed or ambiguous writes withhold ACKs and freeze subsequent ticks. Do
	// this on every simulation tick, even when publication is rate limited, so
	// a disconnect cannot discard the latest durable item changes.
	if (!SurvivorsCommitCoopLedgers()) return;
	if (SDL_GetTicks() - LastPublish < 100) return;
	LastPublish = SDL_GetTicks();
	const Bytes world = CaptureWorld();
	Session.Publish(world, Send);
}

void AsyncNetOnRollingOriginChanged()
{
	if (!AsyncNetEnabled()) return;
	RewindHistory.clear();
	RewindTicks = {};
	// Target-spell leases identify the current act, not local coordinates.
	// Preserve them: an already-issued targeted action may arrive after a
	// rolling shift and is translated/revalidated in its new local frame.
	Visuals = {};
	MonsterVisuals = {};
}

void AsyncNetOfferTargetedSpell(Player &player, SpellID spell)
{
	if (!AsyncNetEnabled() || !AsyncNetIsAuthority() || player.getId() >= MAX_PLRS) return;
	auto &lease = TargetSpells[player.getId()];
	++lease.token;
	if (lease.token == 0) ++lease.token;
	lease.spell = static_cast<uint8_t>(spell);
	lease.ready = true;
	TargetSpellWorld[player.getId()] = WorldContextKey();
}
AsyncTargetedSpellStatus GetAsyncTargetedSpellStatus(const Player &player)
{
	return AsyncNetEnabled() && player.getId() < MAX_PLRS ? TargetSpells[player.getId()] : AsyncTargetedSpellStatus { };
}
bool AsyncNetTargetItem(Player &player, SpellID spell, unsigned index)
{
	if (!AsyncNetEnabled() || ApplyingCommand || &player != MyPlayer) return false;
	const auto lease = TargetSpells[player.getId()];
	if (!lease.ready || lease.spell != static_cast<uint8_t>(spell) || index >= static_cast<unsigned>(NUM_INVLOC + player._pNumInv)) return true;
	const Item &item = GetInventoryItem(player, index);
	Codec request;
	request.bytes.push_back(static_cast<std::byte>(TargetItemIntent));
	unsigned token = lease.token, action = static_cast<unsigned>(spell), seed = item._iSeed;
	request.Fields(token, action, index, seed);
	if (QueueRuntimeInput(request.bytes)) Session.PumpInputs(SDL_GetTicks(), Send);
	return true;
}

bool AsyncNetSurvivorsEquip(unsigned index, uint32_t seed)
{
	if (!AsyncNetEnabled() || !IsSurvivorsMode()) return false;
	Codec encoded;
	encoded.bytes.push_back(static_cast<std::byte>(SurvivorsEquipIntent));
	encoded.Fields(index, seed);
	if (QueueRuntimeInput(encoded.bytes)) Session.PumpInputs(SDL_GetTicks(), Send);
	else EventPlrMsg("Network input backlog is full; awaiting authority acknowledgement.");
	return true;
}

bool AsyncNetStoreTransaction(AsyncStoreAction action, unsigned index, uint32_t seed)
{
	if (!AsyncNetEnabled()) return false;
	Codec encoded;
	unsigned a = static_cast<unsigned>(action);
	encoded.bytes.push_back(static_cast<std::byte>(StoreIntent));
	encoded.Fields(a, index, seed);
	if (QueueRuntimeInput(encoded.bytes)) Session.PumpInputs(SDL_GetTicks(), Send);
	return true;
}

void AsyncNetPredictLocalPlayer()
{
	if (!AsyncNetEnabled() || AsyncNetIsAuthority() || MyPlayer == nullptr) return;
	// The movement-only wrapper deliberately excludes combat/death/inventory.
	const uint32_t rng = GetLCGEngineState();
	if (PendingTowner >= 0 && static_cast<size_t>(PendingTowner) < Towners.size() && MyPlayer->position.tile.WalkingDistance(Towners[PendingTowner].position) < 2) {
		const int npc = PendingTowner;
		PendingTowner = -1;
		TalkToTowner(*MyPlayer, npc);
	}
	AsyncNetAdvancePredictedPlayer(*MyPlayer);
	++PredictedTick;
	SetRndSeed(rng);
}

Displacement AsyncNetRenderOffset(const Player &player)
{
	if (!AsyncNetEnabled() || AsyncNetIsAuthority() || &player == MyPlayer || player.getId() >= MAX_PLRS) return { };
	const auto &visual = Visuals[player.getId()];
	if (!visual.valid) return { };
	const int remaining = static_cast<int>(100 - std::min<uint64_t>(100, SDL_GetTicks() - visual.received));
	const Displacement distance = visual.previous - visual.current;
	return { distance.deltaX * remaining / 100, distance.deltaY * remaining / 100 };
}

bool AsyncNetMeleeReach(const Player &player, const Monster &monster, unsigned range)
{
	if (player.position.tile.WalkingDistance(monster.position.tile) <= static_cast<int>(range)) return true;
	if (!AsyncNetEnabled() || !AsyncNetIsAuthority() || player.getId() >= MAX_PLRS || RewindTicks[player.getId()] == 0) return false;
	const uint32_t requested = Session.Tick() - std::min(Session.Tick(), RewindTicks[player.getId()]);
	const uint32_t oldest = Session.Tick() > net::async::MaxRewindTicks ? Session.Tick() - net::async::MaxRewindTicks : 0;
	for (auto it = RewindHistory.rbegin(); it != RewindHistory.rend(); ++it) {
		if (it->tick < oldest) break;
		if (it->tick <= requested)
			return it->players[player.getId()].WalkingDistance(it->monsters[monster.getId()]) <= static_cast<int>(range);
	}
	return false;
}

bool AsyncNetSendExtension(uint8_t channel, std::span<const std::byte> payload)
{
	return AsyncNetEnabled() && (!AsyncNetIsAuthority() || SurvivorsCommitCoopLedgers()) && Session.SendExtension(channel, payload, Send);
}
bool AsyncNetSendReliableExtension(uint8_t channel, std::span<const std::byte> payload)
{
	if (!AsyncNetEnabled() || (AsyncNetIsAuthority() && !SurvivorsCommitCoopLedgers()) || !Session.QueueReliableExtension(channel, payload)) return false;
	Session.PumpInputs(SDL_GetTicks(), Send);
	return true;
}
bool AsyncNetHasReliablePending() { return AsyncNetEnabled() && Session.HasReliablePending(); }
Displacement AsyncNetRenderOffset(const Monster &monster)
{
	if (!AsyncNetEnabled() || AsyncNetIsAuthority() || monster.getId() >= MaxMonsters) return { };
	const auto &visual = MonsterVisuals[monster.getId()];
	if (!visual.valid) return { };
	const int remaining = static_cast<int>(100 - std::min<uint64_t>(100, SDL_GetTicks() - visual.received));
	const Displacement delta = visual.previous - visual.current;
	return { delta.deltaX * remaining / 100, delta.deltaY * remaining / 100 };
}
void AsyncNetSetExtensionHandler(uint8_t channel, AsyncNetExtensionHandler handler) { Extensions[channel] = std::move(handler); }
AsyncNetStatus GetAsyncNetStatus()
{
	return { AsyncNetEnabled(), AsyncNetIsAuthority(), Session.Healthy(), Session.Tick(), InstalledTick, static_cast<uint32_t>(Session.PendingInputs().size()), InstalledSnapshots, Session.Statistics().retransmits };
}
#ifdef BUILD_TESTING
bool AsyncNetInventoryReconciliationForTesting(Player &player)
{
	if (player._pNumInv != 1 || player.InvList[0].isEmpty()) return false;
	const Item original = player.InvList[0];
	const int originalGold = player._pGold;
	PlayerReplica snapshot;
	PackNetPlayer(snapshot.packed, player);
	PackNetItem(player.HoldItem, snapshot.held);
	snapshot.position = player.position; snapshot.animation = player.AnimInfo;
	snapshot.mode = player._pmode; snapshot.gold = originalGold; snapshot.active = player.plractive;
	snapshot.graphics = player._pgfxnum;
	const auto inventory = std::span<const std::byte>(reinterpret_cast<const std::byte *>(&snapshot.packed.InvBody), sizeof(snapshot.packed.InvBody) + sizeof(snapshot.packed.InvList) + sizeof(snapshot.packed.InvGrid) + sizeof(snapshot.packed._pNumInv) + sizeof(snapshot.packed.SpdList));
	ItemHashes[player.getId()] = net::async::PayloadHash(inventory);
	ItemHashValid[player.getId()] = true;
	const std::array<std::byte, 3> removal { static_cast<std::byte>(CMD_DELINVITEMS), std::byte {}, std::byte {} };
	const auto wrapped = EncodeSurvivorsInput(removal, 0, 1, 0, 0, 0);
	const std::array<std::byte, 3> movement { static_cast<std::byte>(CMD_WALKXY), std::byte {}, std::byte {} };
	if (!InventoryMutationIntent(removal) || !InventoryMutationIntent(wrapped) || InventoryMutationIntent(movement)) return false;
	player.InvList[0].clear(); player._pNumInv = 0;
	std::fill(std::begin(player.InvGrid), std::end(player.InvGrid), 0);
	player.HoldItem = original; player._pGold = 7;
	// An older snapshot received before ACK must preserve the local edit.
	RestorePlayer(player, snapshot, true);
	if (player._pNumInv != 0 || player.HoldItem._iSeed != original._iSeed || player._pGold != 7) return false;
	// The ACK can carry the same bytes as the original cached inventory.
	RestorePlayer(player, snapshot, false);
	return player._pNumInv == 1 && player.InvList[0]._iSeed == original._iSeed
	    && player.HoldItem.isEmpty() && player._pGold == originalGold;
}

bool AsyncNetRollingInputEpochForTesting()
{
	const std::array<std::byte, 3> walk { static_cast<std::byte>(CMD_WALKXY), std::byte { 71 }, std::byte { 52 } };
	constexpr uint32_t sector = 9;
	constexpr uint64_t nonce = 0x123456789abcdef0ULL;
	constexpr uint64_t originEpoch = 0x100000001ULL;
	const auto encoded = EncodeSurvivorsInput(walk, sector, nonce, originEpoch, 0, 0);
	const auto accepted = DecodeSurvivorsInput(encoded, sector, nonce, originEpoch, 0, 0);
	const auto translated = DecodeSurvivorsInput(encoded, sector, nonce, originEpoch + 1, 32, 0);
	const std::array<std::byte, 3> store { static_cast<std::byte>(StoreIntent), std::byte { 7 }, std::byte { 1 } };
	const auto storeEncoded = EncodeSurvivorsInput(store, sector, nonce, originEpoch, 0, 0);
	const auto retained = DecodeSurvivorsInput(storeEncoded, sector, nonce, originEpoch + 1, 32, 0);
	const std::array<std::byte, 8> targetSpell { static_cast<std::byte>(TargetWorldIntent), std::byte { 1 }, std::byte {}, std::byte {}, std::byte {}, static_cast<std::byte>(CMD_DISARMXY), std::byte { 71 }, std::byte { 52 } };
	const auto targetEncoded = EncodeSurvivorsInput(targetSpell, sector, nonce, originEpoch, 0, 0);
	const auto targetTranslated = DecodeSurvivorsInput(targetEncoded, sector, nonce, originEpoch + 1, 32, 0);
	const std::array<std::byte, 3> attack { static_cast<std::byte>(CMD_ATTACKID), std::byte { 0 }, std::byte { 0 } };
	Monster &monster = Monsters[0];
	const bool oldInvalid = monster.isInvalid;
	const uint32_t oldSeed = monster.rndItemSeed;
	monster.isInvalid = false;
	monster.rndItemSeed = 123456;
	const auto attackEncoded = EncodeSurvivorsInput(attack, sector, nonce, originEpoch, 0, 0, InputTargetIdentity(attack));
	const bool retainedTarget = !DecodeSurvivorsInput(attackEncoded, sector, nonce, originEpoch + 1, 32, 0).empty();
	monster.rndItemSeed = 654321;
	const bool replacementRejected = DecodeSurvivorsInput(attackEncoded, sector, nonce, originEpoch + 1, 32, 0).empty();
	monster.rndItemSeed = oldSeed;
	monster.isInvalid = oldInvalid;
	return std::equal(walk.begin(), walk.end(), accepted.begin(), accepted.end())
	    && translated.size() == 3 && translated[1] == std::byte { 39 } && translated[2] == std::byte { 52 }
	    && std::equal(store.begin(), store.end(), retained.begin(), retained.end())
	    && targetTranslated.size() == targetSpell.size() && targetTranslated[6] == std::byte { 39 } && targetTranslated[7] == std::byte { 52 }
	    && retainedTarget && replacementRejected
	    && DecodeSurvivorsInput(encoded, sector, nonce, originEpoch - 1, 0, 0).empty()
	    && DecodeSurvivorsInput(encoded, sector, nonce, originEpoch, 32, 0).empty()
	    && DecodeSurvivorsInput(encoded, sector, nonce, originEpoch + 1, 96, 0).empty()
	    && DecodeSurvivorsInput(encoded, sector + 1, nonce, originEpoch, 0, 0).empty()
	    && DecodeSurvivorsInput(encoded, sector, nonce + 1, originEpoch, 0, 0).empty()
	    && DecodeSurvivorsInput(std::span(encoded).first(45), sector, nonce, originEpoch, 0, 0).empty();
}

bool AsyncNetWorldVersionForTesting()
{
	World source {};
	source.players[0].ledgerGenerationLow = 0xabcdef01;
	source.players[0].ledgerGenerationHigh = 0x12345678;
	// Preserve negative 64-bit world origins and epochs above UINT32_MAX;
	// truncating either would install a peer into the wrong terrain window.
	source.rolling = true;
	source.rollingEpochLow = 5;
	source.rollingEpochHigh = 1;
	source.originXLow = 0xffffffe0;
	source.originXHigh = 0xffffffff;
	source.originYLow = 0x00000040;
	source.originYHigh = 0x00000002;
	Codec encoder;
	TransferWorld(encoder, source);
	World restored {};
	Codec decoder { {}, encoder.bytes, 0, true };
	TransferWorld(decoder, restored);
	if (!decoder.valid || decoder.offset != encoder.bytes.size() || !ValidateWorld(restored)
	    || restored.players[0].ledgerGenerationLow != source.players[0].ledgerGenerationLow
	    || restored.players[0].ledgerGenerationHigh != source.players[0].ledgerGenerationHigh
	    || !restored.rolling || restored.rollingEpochLow != source.rollingEpochLow || restored.rollingEpochHigh != source.rollingEpochHigh
	    || restored.originXLow != source.originXLow || restored.originXHigh != source.originXHigh
	    || restored.originYLow != source.originYLow || restored.originYHigh != source.originYHigh) return false;
	++restored.version;
	return !ValidateWorld(restored);
}

bool AsyncNetRoundTripMissileForTesting(const Missile &input, Missile &output)
{
	Missile copy = input;
	Codec encoder;
	Transfer(encoder, copy);
	Codec decoder { { }, encoder.bytes, 0, true };
	output = { };
	Transfer(decoder, output);
	return decoder.valid && decoder.offset == encoder.bytes.size();
}
bool AsyncNetRoundTripMissileBudgetForTesting(unsigned count)
{
	World source { };
	source.tiles = CaptureTiles();
	source.missiles.resize(count);
	for (auto &missile : source.missiles) {
		missile._mitype = MissileID::Arrow;
		missile._miAnimType = MissileGraphicID::None;
	}
	Codec encoder;
	TransferWorld(encoder, source);
	if (!encoder.valid || encoder.bytes.size() > MaxRawWorldBytes) return false;
	World restored { };
	Codec decoder { { }, encoder.bytes, 0, true };
	TransferWorld(decoder, restored);
	return decoder.valid && decoder.offset == encoder.bytes.size() && restored.missiles.size() == count;
}

// Exercise the compressed transport limit with a full party's buyback and a
// crowded arena, rather than only checking the uncompressed missile encoder.
bool AsyncNetDenseBuybackSnapshotForTesting()
{
	World source { };
	source.tiles = CaptureTiles();
	source.missiles.resize(MaxWorldMissiles);
	for (unsigned i = 0; i < source.missiles.size(); ++i) {
		auto &m = source.missiles[i];
		m._mitype = MissileID::Arrow;
		m._miAnimType = MissileGraphicID::None;
		m.position.tile = { static_cast<uint8_t>(i % 112), static_cast<uint8_t>((i * 17) % 112) };
		m.duration = i % 120;
		m._mirnd = i * 2654435761U;
	}
	for (unsigned peer = 0; peer < MAX_PLRS; ++peer) {
		for (unsigned i = 0; i < SurvivorsBuybackLimit; ++i) {
			StockItem item;
			item.packed.def.wIndx = IDI_BARDSWORD;
			item.packed.def.dwSeed = (peer * SurvivorsBuybackLimit + i + 1) * 2654435761U;
			item.price = static_cast<int>(1 + i * 173);
			source.buyback[peer].push_back(item);
		}
	}
	Codec encoder;
	TransferWorld(encoder, source);
	if (!encoder.valid || encoder.bytes.size() > MaxRawWorldBytes) return false;
	Bytes compressed(compressBound(encoder.bytes.size()));
	uLongf length = compressed.size();
	if (compress2(reinterpret_cast<Bytef *>(compressed.data()), &length,
	        reinterpret_cast<const Bytef *>(encoder.bytes.data()), encoder.bytes.size(), Z_BEST_SPEED) != Z_OK
	    || length + 4 > net::async::MaxSnapshotBytes) return false;
	World restored { };
	Codec decoder { { }, encoder.bytes, 0, true };
	TransferWorld(decoder, restored);
	if (!decoder.valid || decoder.offset != encoder.bytes.size() || restored.missiles.size() != MaxWorldMissiles) return false;
	for (unsigned peer = 0; peer < MAX_PLRS; ++peer) {
		if (restored.buyback[peer].size() != SurvivorsBuybackLimit) return false;
		for (unsigned i = 0; i < SurvivorsBuybackLimit; ++i)
			if (restored.buyback[peer][i].packed.def.dwSeed != source.buyback[peer][i].packed.def.dwSeed
			    || restored.buyback[peer][i].price != source.buyback[peer][i].price) return false;
	}
	return true;
}
#endif
} // namespace devilution
