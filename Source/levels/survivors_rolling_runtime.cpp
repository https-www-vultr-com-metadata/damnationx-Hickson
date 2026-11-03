#include "levels/survivors_rolling_runtime.hpp"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <initializer_list>
#include <limits>
#include <optional>
#include <string_view>
#include <unordered_set>
#include <utility>
#include <vector>

#include "cursor.h"
#include "dvlnet/async_runtime.h"
#include "engine/actor_position.hpp"
#include "engine/path.h"
#include "engine/random.hpp"
#include "host/host_hooks.hpp"
#include "items.h"
#include "levels/dun_tile_data.hpp"
#include "levels/gendung.h"
#include "levels/survivors_rolling_world.hpp"
#include "levels/tile_properties.hpp"
#include "lighting.h"
#include "missiles.h"
#include "monster.h"
#include "objects.h"
#include "player.h"
#include "plrmsg.h"
#include "survivors.h"
#include "utils/enum_traits.h"
#include "utils/is_of.hpp"

namespace devilution {
namespace {

constexpr int RollDistance = 32;
constexpr int RollLower = 39;
constexpr int RollUpper = 73;

struct StoredItem { Item item; int64_t x, y; };
struct StoredObject { Object object; int64_t x, y; };
struct StoredSoul { unsigned value; int64_t x, y; };
struct StoredMissile { Missile missile; SurvivorsWorldOrigin origin; std::optional<Light> light; };
struct StoredMonster { ActorPosition position; WorldTilePosition enemyPosition; SurvivorsWorldOrigin origin; std::optional<Light> light; bool retireWhenUnused; };
struct StoredDeadPlayer { ActorPosition position; SurvivorsWorldOrigin origin; };
struct WorldKey {
	int64_t x, y;
	bool operator==(const WorldKey &) const = default;
};
struct WorldKeyHash {
	size_t operator()(const WorldKey &key) const noexcept
	{
		uint64_t value = static_cast<uint64_t>(key.x) * 0x9e3779b97f4a7c15ULL ^ (static_cast<uint64_t>(key.y) + 0x632be59bd9b4e019ULL);
		value ^= value >> 29;
		value *= 0xbf58476d1ce4e5b9ULL;
		return static_cast<size_t>(value ^ (value >> 32));
	}
};

struct RollingState {
	bool active = false;
	bool carryItemsToNextAct = false;
	unsigned pursuitTick = 0;
	unsigned warningTicks = 0;
	uint64_t epoch = 0;
	uint32_t seed = 0;
	SurvivorsRollingTerrain terrain;
	std::vector<StoredItem> items;
	std::vector<Item> carriedItems;
	std::vector<StoredObject> objects;
	// World tiles of parked objects, so generation never duplicates them.
	std::unordered_set<WorldKey, WorldKeyHash> parkedObjectTiles;
	// Generated props delayed by a transient occupant (hunter, monster, item).
	std::vector<SurvivorsRollingProp> pendingProps;
	bool restoreObjects = false;
	std::vector<StoredSoul> souls;
	std::vector<StoredMissile> missiles;
	std::array<std::optional<StoredMonster>, MaxMonsters> monsters;
	std::array<std::optional<StoredDeadPlayer>, MAX_PLRS> deadPlayers;
};
RollingState State;

bool Inside(int x, int y) { return x >= 0 && x < MAXDUNX && y >= 0 && y < MAXDUNY; }
bool IsDormantGolem(unsigned id, const Monster &monster)
{
	// The first MAX_PLRS slots are reserved by InitGolems even before their
	// MFLAG_GOLEM flag is set. Their holding cell is an engine sentinel, not a
	// world tile, and must never be translated when the window moves.
	return id < MAX_PLRS && monster.position.tile == GolemHoldingCell;
}

bool CanPlaceRollingObject(Point tile)
{
	return Inside(tile.x, tile.y) && dObject[tile.x][tile.y] == 0 && dItem[tile.x][tile.y] == 0
	    && dPlayer[tile.x][tile.y] == 0 && dMonster[tile.x][tile.y] == 0
	    && dPiece[tile.x][tile.y] != 0 && !TileHasAny(tile, TileProperties::Solid);
}

// Native objects for each generated role, per tileset. Only single-tile
// objects whose operation does not rewrite the map are used: large fountains
// claim neighbouring dObject cells that window eviction cannot release, and
// crucifixes and levers change dungeon tiles when used.
_object_id RollingSceneryObject(SurvivorsPropRole role, uint8_t variant)
{
	const bool hell = leveltype == DTYPE_HELL;
	const bool caves = leveltype == DTYPE_CAVES || leveltype == DTYPE_NEST;
	auto pick = [variant](std::initializer_list<_object_id> choices) { return *(choices.begin() + variant % choices.size()); };
	switch (role) {
	case SurvivorsPropRole::Chest:
		// Larger chests are rarer: 70% small, 22% medium, 8% large.
		return variant < 179 ? OBJ_CHEST1 : variant < 235 ? OBJ_CHEST2 : OBJ_CHEST3;
	case SurvivorsPropRole::Shrine: return variant % 2 == 0 ? OBJ_SHRINEL : OBJ_SHRINER;
	// Fires must read in the D2R view: the skull fire imports as a burning body
	// on a square ember bed and the cauldron as a wide ash disc, both without
	// their flames. Candle stands import cleanly (candles_1 / candles_2), and
	// the host gives every one a flame and a broad pool (host_snapshot.cpp).
	case SurvivorsPropRole::Brazier: return OBJ_L1LIGHT;
	case SurvivorsPropRole::Candle: return OBJ_CANDLE2;
	case SurvivorsPropRole::Firepit: return OBJ_BOOKCANDLE;
	case SurvivorsPropRole::Bones:
		if (hell) return pick({ OBJ_TNUDEM1, OBJ_TNUDEM2, OBJ_TNUDEM3, OBJ_TNUDEM4, OBJ_TNUDEW1, OBJ_TNUDEW2, OBJ_TNUDEW3, OBJ_SKPILE });
		return variant < 200 ? OBJ_SKPILE : OBJ_DECAP;
	case SurvivorsPropRole::Barrel:
		if (hell) return variant % 2 == 0 ? OBJ_BARRELEX : OBJ_SKPILE;
		return variant < 190 ? OBJ_BARREL : OBJ_BARRELEX;
	case SurvivorsPropRole::Banner:
		if (hell) return pick({ OBJ_NUDEW2R, OBJ_TNUDEM2, OBJ_TNUDEW1 });
		if (caves) return OBJ_SKPILE;
		return pick({ OBJ_BANNERL, OBJ_BANNERM, OBJ_BANNERR });
	case SurvivorsPropRole::Altar:
		if (hell) return pick({ OBJ_GOATSHRINE, OBJ_BLOODFTN });
		if (caves) return pick({ OBJ_GOATSHRINE, OBJ_TEARFTN });
		return pick({ OBJ_BLOODFTN, OBJ_GOATSHRINE });
	}
	return OBJ_SKPILE;
}

void ResetRollingObjectLight(Object &object)
{
	// A parked light's engine slot was released; its next window allocates one
	// again when a hunter comes into range (UpdateObjectLight).
	if (GetObjectLightRadius(object._otype) == 0) return;
	object._oVar1 = 0;
	object._olid = 0;
}

bool AnyHunterNear(Point tile, int distance)
{
	return std::any_of(Players.begin(), Players.end(), [&](const Player &player) {
		return player.plractive && player.isOnActiveLevel() && !player.hasNoLife()
		    && player.position.tile.WalkingDistance(tile) < distance;
	});
}

enum class PlaceResult { Placed, Skipped, Deferred };
// Keep a few object slots free for restored props and native interactions.
constexpr int RollingObjectHeadroom = 6;

PlaceResult PlaceRollingProp(const SurvivorsRollingProp &prop, SurvivorsWorldOrigin origin)
{
	const int64_t localX = prop.x - origin.x, localY = prop.y - origin.y;
	if (localX < 4 || localX >= MAXDUNX - 4 || localY < 4 || localY >= MAXDUNY - 4) return PlaceResult::Skipped;
	if (State.parkedObjectTiles.contains({ prop.x, prop.y })) return PlaceResult::Skipped;
	const Point tile { static_cast<int>(localX), static_cast<int>(localY) };
	// An existing object here, or touching this tile, is either this prop
	// itself or a permanent neighbour: keep a walkable gap around every prop.
	for (int dx = -1; dx <= 1; ++dx)
		for (int dy = -1; dy <= 1; ++dy)
			if (dObject[tile.x + dx][tile.y + dy] != 0) return PlaceResult::Skipped;
	if (dPiece[tile.x][tile.y] == 0 || TileHasAny(tile, TileProperties::Solid)) return PlaceResult::Skipped;
	// Do not materialize a prop under or immediately next to a hunter, or on
	// top of a transient occupant; try again once they have moved on.
	if (dItem[tile.x][tile.y] != 0 || dPlayer[tile.x][tile.y] != 0 || dMonster[tile.x][tile.y] != 0 || AnyHunterNear(tile, 7)
	    || ActiveObjectCount >= MAXOBJECTS - RollingObjectHeadroom)
		return PlaceResult::Deferred;
	const _object_id type = RollingSceneryObject(prop.role, prop.variant);
	// Unit fixtures run the rolling world without the object table loaded.
	if (static_cast<size_t>(type) >= AllObjects.size()) return PlaceResult::Skipped;
	// Seed locally without perturbing monster, loot or combat RNG.
	const auto rng = GetLCGEngineState();
	const uint64_t ux = static_cast<uint64_t>(prop.x), uy = static_cast<uint64_t>(prop.y);
	SetRndSeed(State.seed ^ static_cast<uint32_t>(ux ^ (ux >> 32)) ^ static_cast<uint32_t>(uy ^ (uy >> 32)) * 0x9e3779b9U);
	Object *object = AddObject(type, tile);
	SetRndSeed(rng);
	return object != nullptr ? PlaceResult::Placed : PlaceResult::Deferred;
}

bool PropPending(const SurvivorsRollingProp &prop)
{
	return std::any_of(State.pendingProps.begin(), State.pendingProps.end(), [&](const SurvivorsRollingProp &other) { return other.x == prop.x && other.y == prop.y; });
}

// `generate` lays out every chunk overlapping the window (at initialization
// and after a rebase). Otherwise only delayed props and objects are retried,
// which keeps the per-tick cost independent of how far the hunters roamed.
void PopulateRollingObjects(bool generate)
{
	if (!State.active || (gbIsMultiplayer && !AsyncNetIsAuthority())) return;
	const auto origin = State.terrain.Origin();
	// Restore previously visited objects before generating new ones. Their
	// opened/broken state and native loot seed survive window eviction.
	if (generate || State.restoreObjects) {
		State.restoreObjects = false;
		for (auto it = State.objects.begin(); it != State.objects.end() && ActiveObjectCount < MAXOBJECTS;) {
			const int64_t x = it->x - origin.x, y = it->y - origin.y;
			if (x < 0 || x >= MAXDUNX || y < 0 || y >= MAXDUNY) { ++it; continue; }
			if (!CanPlaceRollingObject({ static_cast<int>(x), static_cast<int>(y) })) {
				State.restoreObjects = true;
				++it;
				continue;
			}
			const auto rng = GetLCGEngineState();
			Object *object = AddObject(it->object._otype, { static_cast<int>(x), static_cast<int>(y) });
			SetRndSeed(rng);
			if (object == nullptr) break;
			const auto animation = object->_oAnimData;
			*object = it->object;
			object->position = { static_cast<int>(x), static_cast<int>(y) };
			object->_oAnimData = animation;
			ResetRollingObjectLight(*object);
			State.parkedObjectTiles.erase({ it->x, it->y });
			it = State.objects.erase(it);
		}
	}
	if (!generate) {
		for (auto it = State.pendingProps.begin(); it != State.pendingProps.end();) {
			if (PlaceRollingProp(*it, origin) == PlaceResult::Deferred) ++it;
			else it = State.pendingProps.erase(it);
		}
		return;
	}
	// Props outside this window are regenerated when a window covers them again.
	State.pendingProps.clear();
	for (int64_t chunkX = origin.x - 16; chunkX < origin.x + MAXDUNX; chunkX += 16) {
		for (int64_t chunkY = origin.y - 16; chunkY < origin.y + MAXDUNY; chunkY += 16) {
			for (const SurvivorsRollingProp &prop : SurvivorsRollingChunkScenery(State.seed, chunkX, chunkY).view()) {
				if (PlaceRollingProp(prop, origin) == PlaceResult::Deferred && !PropPending(prop))
					State.pendingProps.push_back(prop);
			}
		}
	}
}
bool CanShift(WorldTilePosition point, int dx, int dy)
{
	// Unused actor fields use the all-zero sentinel.
	return point == Point { 0, 0 } || Inside(static_cast<int>(point.x) - dx, static_cast<int>(point.y) - dy);
}
void ShiftPoint(WorldTilePosition &point, int dx, int dy)
{
	if (point != Point { 0, 0 }) point = Point { static_cast<int>(point.x) - dx, static_cast<int>(point.y) - dy };
}
bool CanShiftActor(const ActorPosition &position, int dx, int dy)
{
	// The actor's occupied and future tiles are real coordinates, even at
	// (0, 0); only the auxiliary fields can use that value as a sentinel.
	return Inside(static_cast<int>(position.tile.x) - dx, static_cast<int>(position.tile.y) - dy)
	    && Inside(static_cast<int>(position.future.x) - dx, static_cast<int>(position.future.y) - dy)
	    && CanShift(position.old, dx, dy) && CanShift(position.last, dx, dy) && CanShift(position.temp, dx, dy);
}
void ShiftActor(ActorPosition &position, int dx, int dy)
{
	position.tile = Point { static_cast<int>(position.tile.x) - dx, static_cast<int>(position.tile.y) - dy };
	position.future = Point { static_cast<int>(position.future.x) - dx, static_cast<int>(position.future.y) - dy };
	ShiftPoint(position.old, dx, dy); ShiftPoint(position.last, dx, dy); ShiftPoint(position.temp, dx, dy);
}
template <typename T>
void ShiftRollingGrid(T (&grid)[MAXDUNX][MAXDUNY], int dx, int dy, T fill = T {})
{
	std::array<std::array<T, MAXDUNY>, MAXDUNX> before {};
	for (int x = 0; x < MAXDUNX; ++x) std::copy_n(grid[x], MAXDUNY, before[x].begin());
	for (int x = 0; x < MAXDUNX; ++x)
		for (int y = 0; y < MAXDUNY; ++y)
			grid[x][y] = Inside(x + dx, y + dy) ? before[x + dx][y + dy] : fill;
}

template <typename T>
std::optional<Light> SuspendLight(T &id)
{
	if (id == NO_LIGHT) return std::nullopt;
	const Light light = Lights[id];
	AddUnLight(id);
	id = NO_LIGHT;
	return light;
}
template <typename T>
void RestoreLight(T &id, std::optional<Light> &light, Point position)
{
	if (!light) return;
	id = AddLight(position, light->radius);
	if (id != NO_LIGHT) {
		ChangeLightOffset(id, light->position.offset);
		light.reset();
	}
}

// Each supported effect's variable layout is explicitly enumerated. Effects
// with opaque positional variables remain suspended until their original
// window returns; they never hold up the player's movement.
enum class MissileVars { None, XY12, XY12_45, XY12_56, Apocalypse, FollowPlayer, Unsupported };
MissileVars Vars(MissileID type)
{
	switch (type) {
	case MissileID::Arrow: case MissileID::ChargedBolt: case MissileID::FlameWave:
	case MissileID::Lightning: case MissileID::ThinLightning: case MissileID::FireWall:
	case MissileID::WeaponExplosion: case MissileID::MagmaBallExplosion:
	case MissileID::FlashBottom: case MissileID::FlashTop: case MissileID::Guardian:
	case MissileID::AcidPuddle: case MissileID::AcidSplat: case MissileID::BloodStarExplosion:
	case MissileID::BigExplosion: case MissileID::ApocalypseBoom: case MissileID::DiabloApocalypseBoom:
	case MissileID::Inferno: case MissileID::ResurrectBeam: case MissileID::Phasing:
	case MissileID::Teleport: case MissileID::Warp: case MissileID::Rhino:
	case MissileID::ChargedBoltBow: case MissileID::HorkSpawn:
	case MissileID::RuneOfFire: case MissileID::RuneOfLight: case MissileID::RuneOfNova:
	case MissileID::RuneOfImmolation: case MissileID::RuneOfStone:
	case MissileID::YellowExplosion: case MissileID::RedExplosion: case MissileID::BlueExplosion:
	case MissileID::BlueExplosion2: case MissileID::OrangeExplosion:
		return MissileVars::None;
	case MissileID::Rage: case MissileID::Search: case MissileID::Infravision: case MissileID::StoneCurse:
		return MissileVars::FollowPlayer;
	case MissileID::Firebolt: case MissileID::Fireball: case MissileID::MagmaBall:
	case MissileID::BloodStar: case MissileID::Acid: case MissileID::FireArrow:
	case MissileID::LightningArrow: case MissileID::LightningControl:
	case MissileID::ThinLightningControl: case MissileID::HolyBolt:
	case MissileID::InfernoControl: case MissileID::SpectralArrow:
	case MissileID::ChainLightning: case MissileID::Nova: case MissileID::NovaBall: case MissileID::RingOfFire:
	case MissileID::FlameWaveControl: case MissileID::LightningWall: case MissileID::Immolation:
	case MissileID::FireballBow: case MissileID::LightningBow: case MissileID::HolyBoltBow:
	case MissileID::OrangeFlare: case MissileID::BlueFlare: case MissileID::RedFlare:
	case MissileID::YellowFlare: case MissileID::BlueFlare2:
		return MissileVars::XY12;
	case MissileID::Elemental: case MissileID::BoneSpirit: return MissileVars::XY12_45;
	case MissileID::FireWallControl: case MissileID::LightningWallControl: return MissileVars::XY12_56;
	case MissileID::Apocalypse: return MissileVars::Apocalypse;
	default: return MissileVars::Unsupported;
	}
}

bool CanMoveMissile(const Missile &missile, int dx, int dy)
{
	const auto vars = Vars(missile._mitype);
	if (vars == MissileVars::FollowPlayer) return true;
	if (vars == MissileVars::Unsupported
	    || !Inside(static_cast<int>(missile.position.tile.x) - dx, static_cast<int>(missile.position.tile.y) - dy)
	    || !Inside(static_cast<int>(missile.position.start.x) - dx, static_cast<int>(missile.position.start.y) - dy)
	    || !Inside(static_cast<int>(missile.position.tileForRendering.x) - dx, static_cast<int>(missile.position.tileForRendering.y) - dy)) return false;
	if ((vars == MissileVars::XY12 || vars == MissileVars::XY12_45 || vars == MissileVars::XY12_56)
	    && (missile.var1 != 0 || missile.var2 != 0) && !Inside(missile.var1 - dx, missile.var2 - dy)) return false;
	// Elemental and Bone Spirit use var4/var5 only for a coordinate comparison,
	// never as a grid index. Their target may be just outside this window while
	// the projectile is still visible; keep its signed translated coordinates.
	if (vars == MissileVars::XY12_56 && !Inside(missile.var5 - dx, missile.var6 - dy)) return false;
	return true;
}
// Straight moving projectiles can keep their current sub-tile position and
// velocity when their original firing tile leaves the local window.
void ReanchorMovingProjectile(Missile &missile, int dx, int dy)
{
	// Headless simulation does not run the render interpolator, so its render
	// tile may never have been initialized. Likewise an interpolated previous
	// render tile may leave the window before the actual missile does.
	const Point current = missile.position.tile;
	const Point rendered = missile.position.tileForRendering;
	if (Inside(current.x - dx, current.y - dy)
	    && (rendered == Point { 0, 0 } || !Inside(rendered.x - dx, rendered.y - dy)))
		missile.position.tileForRendering = current;
	if (IsAnyOf(missile._mitype, MissileID::Lightning, MissileID::ThinLightning)) {
		// Lightning segments are stationary, and only compare the original
		// source against their tile to avoid hitting at the source itself.
		// If the source left the window it is necessarily different; retain
		// that distinction with an in-window adjacent tile.
		if (Inside(current.x - dx, current.y - dy)
		    && !Inside(static_cast<int>(missile.position.start.x) - dx, static_cast<int>(missile.position.start.y) - dy)) {
			const int x = Inside(current.x + 1 - dx, current.y - dy) ? current.x + 1 : current.x - 1;
			missile.position.start = Point { x, current.y };
		}
		return;
	}
	switch (missile._mitype) {
	case MissileID::Arrow: case MissileID::Firebolt: case MissileID::Fireball:
	case MissileID::HolyBolt: case MissileID::ChargedBolt: case MissileID::MagmaBall:
	case MissileID::BloodStar: case MissileID::Acid: case MissileID::FireArrow:
	case MissileID::LightningArrow: case MissileID::SpectralArrow: case MissileID::NovaBall: case MissileID::FlameWave:
	case MissileID::LightningControl: case MissileID::ThinLightningControl:
	case MissileID::InfernoControl: case MissileID::LightningBow:
	case MissileID::FireballBow: case MissileID::HolyBoltBow:
	case MissileID::Elemental: case MissileID::BoneSpirit:
	case MissileID::OrangeFlare: case MissileID::BlueFlare: case MissileID::RedFlare:
	case MissileID::YellowFlare: case MissileID::BlueFlare2:
		break;
	default: return;
	}
	const Point tile = missile.position.tile;
	if (!Inside(tile.x - dx, tile.y - dy)) return;
	const Point start = missile.position.start;
	if (!Inside(start.x - dx, start.y - dy)) {
		// Flame waves store the rendered tile one step south of the tile used
		// by their movement processor. Reanchor to the latter so their next
		// tick does not add an extra southward jump.
		const Point anchor = missile._mitype == MissileID::FlameWave ? tile + Direction::North : tile;
		if (!Inside(anchor.x - dx, anchor.y - dy)) return;
		const Displacement tileOffset = anchor - start;
		missile.position.traveled += tileOffset.worldToScreen() * 65536;
		missile.position.start = anchor;
	}
	const Point render = missile.position.tileForRendering;
	if (!Inside(render.x - dx, render.y - dy)) missile.position.tileForRendering = tile;
	if ((Vars(missile._mitype) == MissileVars::XY12 || Vars(missile._mitype) == MissileVars::XY12_45)
	    && (missile.var1 != 0 || missile.var2 != 0)
	    && !Inside(missile.var1 - dx, missile.var2 - dy)) {
		missile.var1 = tile.x;
		missile.var2 = tile.y;
	}
}
void MoveMissile(Missile &missile, int dx, int dy)
{
	if (Vars(missile._mitype) == MissileVars::FollowPlayer) {
		if (missile._mitype == MissileID::StoneCurse
		    && Inside(static_cast<int>(missile.position.tile.x) - dx, static_cast<int>(missile.position.tile.y) - dy)
		    && Inside(static_cast<int>(missile.position.start.x) - dx, static_cast<int>(missile.position.start.y) - dy)
		    && Inside(static_cast<int>(missile.position.tileForRendering.x) - dx, static_cast<int>(missile.position.tileForRendering.y) - dy)) {
			missile.position.tile = Point { static_cast<int>(missile.position.tile.x) - dx, static_cast<int>(missile.position.tile.y) - dy };
			missile.position.start = Point { static_cast<int>(missile.position.start.x) - dx, static_cast<int>(missile.position.start.y) - dy };
			missile.position.tileForRendering = Point { static_cast<int>(missile.position.tileForRendering.x) - dx, static_cast<int>(missile.position.tileForRendering.y) - dy };
			return;
		}
		// Player-owned effects follow their caster, which may be a remote
		// hunter on the authority rather than the host's local player.
		const Player *owner = MyPlayer;
		if (missile.sourceType() == MissileSource::Player && missile._misource >= 0
		    && static_cast<size_t>(missile._misource) < Players.size() && Players[missile._misource].plractive)
			owner = &Players[missile._misource];
		const Point player = Point(owner->position.tile) - Displacement { dx, dy };
		missile.position.tile = player; missile.position.start = player; missile.position.tileForRendering = player;
		return;
	}
	missile.position.tile = Point { static_cast<int>(missile.position.tile.x) - dx, static_cast<int>(missile.position.tile.y) - dy };
	missile.position.start = Point { static_cast<int>(missile.position.start.x) - dx, static_cast<int>(missile.position.start.y) - dy };
	missile.position.tileForRendering = Point { static_cast<int>(missile.position.tileForRendering.x) - dx, static_cast<int>(missile.position.tileForRendering.y) - dy };
	const auto vars = Vars(missile._mitype);
	if (vars == MissileVars::XY12 || vars == MissileVars::XY12_45 || vars == MissileVars::XY12_56) {
		// Some children (notably Immolation's fireballs) initialize their
		// previous-tile cache on their first processing tick.
		if (missile.var1 == 0 && missile.var2 == 0) {
			missile.var1 = missile.position.tile.x;
			missile.var2 = missile.position.tile.y;
		} else { missile.var1 -= dx; missile.var2 -= dy; }
	}
	if (vars == MissileVars::XY12_45) { missile.var4 -= dx; missile.var5 -= dy; }
	if (vars == MissileVars::XY12_56) { missile.var5 -= dx; missile.var6 -= dy; }
	if (vars == MissileVars::Apocalypse) {
		missile.var2 = std::clamp(missile.var2 - dy, 1, MAXDUNY - 1);
		missile.var3 = std::clamp(missile.var3 - dy, 1, MAXDUNY - 1);
		missile.var4 = std::clamp(missile.var4 - dx, 1, MAXDUNX - 1);
		missile.var5 = std::clamp(missile.var5 - dx, 1, MAXDUNX - 1);
		missile.var6 = std::clamp(missile.var6 - dx, 1, MAXDUNX - 1);
	}
}

void SuspendMissile(std::list<Missile>::iterator &it, SurvivorsWorldOrigin origin)
{
	StoredMissile stored { std::move(*it), origin, std::nullopt };
	// A placed rune no longer uses its cast location; anchoring its start to
	// the rune lets it reappear in any window containing the rune itself.
	if (IsAnyOf(stored.missile._mitype, MissileID::RuneOfFire, MissileID::RuneOfLight, MissileID::RuneOfNova,
	        MissileID::RuneOfImmolation, MissileID::RuneOfStone))
		stored.missile.position.start = stored.missile.position.tile;
	stored.light = SuspendLight(stored.missile._mlid);
	it = Missiles.erase(it);
	State.missiles.push_back(std::move(stored));
}

void RestoreSuspendedMissiles()
{
	const auto origin = State.terrain.Origin();
	for (auto it = State.missiles.begin(); it != State.missiles.end();) {
		if (origin.x < it->origin.x - MAXDUNX || origin.x > it->origin.x + MAXDUNX
		    || origin.y < it->origin.y - MAXDUNY || origin.y > it->origin.y + MAXDUNY) { ++it; continue; }
		Missile translated = it->missile;
		if (!SurvivorsRollingRebaseSuspendedMissile(translated, static_cast<int>(origin.x - it->origin.x), static_cast<int>(origin.y - it->origin.y))) { ++it; continue; }
		RestoreLight(translated._mlid, it->light, translated.position.tile);
		if (it->light) { ++it; continue; }
		Missiles.push_back(std::move(translated));
		it = State.missiles.erase(it);
	}
}

void AdvanceParkedMonsters(int boss, int elite)
{
	const auto origin = State.terrain.Origin();
	++State.pursuitTick;
	for (unsigned id = 0; id < State.monsters.size(); ++id) {
		if (!State.monsters[id]) continue;
		auto &parked = *State.monsters[id];
		if (parked.retireWhenUnused) {
			const auto references = [id](const Missile &missile) {
				return (missile._micaster == TARGET_PLAYERS && missile._misource == static_cast<int>(id))
				    || (missile._mitype == MissileID::StoneCurse && missile.var2 == static_cast<int>(id));
			};
			const bool liveReference = std::any_of(Missiles.begin(), Missiles.end(), references)
			    || std::any_of(State.missiles.begin(), State.missiles.end(), [&](const StoredMissile &effect) { return references(effect.missile); });
			if (!liveReference) { State.monsters[id].reset(); continue; }
		}
		// Keep a named enemy pursuing while it is outside the engine window.
		// Its active AI and animation resume when it re-enters the window.
		if ((static_cast<int>(id) == boss || static_cast<int>(id) == elite || (Monsters[id].flags & MFLAG_GOLEM) != 0) && State.pursuitTick % 5 == 0) {
			const Player *target = nullptr;
			const auto enemy = Monsters[id].enemy;
			if (enemy < Players.size() && Players[enemy].plractive && Players[enemy].isOnActiveLevel() && !Players[enemy].hasNoLife())
				target = &Players[enemy];
			if (target == nullptr)
				for (const Player &player : Players)
					if (player.plractive && player.isOnActiveLevel() && !player.hasNoLife()) { target = &player; break; }
			if (target == nullptr) continue;
			const int64_t playerX = origin.x + target->position.tile.x;
			const int64_t playerY = origin.y + target->position.tile.y;
			const int64_t x = parked.origin.x + parked.position.tile.x;
			const int64_t y = parked.origin.y + parked.position.tile.y;
			parked.origin.x += (playerX > x) - (playerX < x);
			parked.origin.y += (playerY > y) - (playerY < y);
		}
		const int64_t shiftX = origin.x - parked.origin.x, shiftY = origin.y - parked.origin.y;
		if (shiftX < -MAXDUNX || shiftX > MAXDUNX || shiftY < -MAXDUNY || shiftY > MAXDUNY
		    || !CanShiftActor(parked.position, static_cast<int>(shiftX), static_cast<int>(shiftY))) continue;
		ActorPosition position = parked.position;
		ShiftActor(position, static_cast<int>(shiftX), static_cast<int>(shiftY));
		if (dMonster[position.tile.x][position.tile.y] != 0 || dPlayer[position.tile.x][position.tile.y] != 0) continue;
		Monster &monster = Monsters[id];
		RestoreLight(monster.lightId, parked.light, position.tile);
		if (parked.light) continue;
		monster.position = position;
		monster.enemyPosition = MyPlayer->position.future;
		if (MyPlayer->hasNoLife()) {
			for (const Player &player : Players) {
				if (player.plractive && player.isOnActiveLevel() && !player.hasNoLife()) {
					monster.enemy = player.getId();
					monster.enemyPosition = player.position.future;
					break;
				}
			}
		}
		monster.isInvalid = false;
		// A charge is driven by a Rhino missile. Its finite lifetime may
		// elapse while the named monster is parked; don't restore a permanent
		// Charge mode after the driving missile has disappeared.
		if (monster.mode == MonsterMode::Charge && std::none_of(Missiles.begin(), Missiles.end(), [id](const Missile &missile) {
			    return missile._mitype == MissileID::Rhino && missile._misource == static_cast<int>(id);
		    })) M_StartStand(monster, monster.direction);
		monster.occupyTile(monster.position.tile, false);
		State.monsters[id].reset();
	}
}

void RestoreParkedDeadPlayers()
{
	const auto origin = State.terrain.Origin();
	for (unsigned id = 0; id < State.deadPlayers.size() && id < Players.size(); ++id) {
		if (!State.deadPlayers[id]) continue;
		Player &player = Players[id];
		if (!player.plractive || !player.isOnActiveLevel()) { State.deadPlayers[id].reset(); continue; }
		if (!player.hasNoLife()) {
			// A checkpoint or external revival can make a parked hunter alive.
			// Place them beside a living teammate rather than at the holding cell.
			const Player *anchor = nullptr;
			for (const Player &other : Players)
				if (&other != &player && other.plractive && other.isOnActiveLevel() && !other.hasNoLife()) { anchor = &other; break; }
			if (anchor == nullptr) continue;
			const auto tile = FindClosestValidPosition([&](Point p) { return PosOkPlayer(player, p); }, anchor->position.tile, 1, 50);
			if (!tile) continue;
			FixPlrWalkTags(player);
			player.position = {};
			player.position.tile = player.position.future = player.position.old = player.position.last = *tile;
			player.occupyTile(*tile, false);
			State.deadPlayers[id].reset();
			continue;
		}
		auto &stored = *State.deadPlayers[id];
		if (origin.x < stored.origin.x - MAXDUNX || origin.x > stored.origin.x + MAXDUNX
		    || origin.y < stored.origin.y - MAXDUNY || origin.y > stored.origin.y + MAXDUNY) continue;
		const int dx = static_cast<int>(origin.x - stored.origin.x);
		const int dy = static_cast<int>(origin.y - stored.origin.y);
		if (!CanShiftActor(stored.position, dx, dy)) continue;
		ActorPosition position = stored.position;
		ShiftActor(position, dx, dy);
		if (dPlayer[position.tile.x][position.tile.y] != 0) continue;
		FixPlrWalkTags(player);
		player.position = position;
		player.occupyTile(player.position.tile, false);
		State.deadPlayers[id].reset();
	}
}

} // namespace

bool SurvivorsRollingRequested()
{
	const char *legacy = std::getenv("DAMNATIONX_LEGACY_SURVIVORS_SECTORS");
	return (legacy == nullptr || std::string_view(legacy) != "1") && IsSurvivorsMode()
	    && (!gbIsMultiplayer || AsyncNetEnabled());
}
bool SurvivorsRollingActive() { return State.active; }
std::span<const _object_id> SurvivorsRollingSceneryObjects()
{
	static constexpr _object_id Types[] {
		OBJ_CHEST1, OBJ_CHEST2, OBJ_CHEST3, OBJ_SHRINEL, OBJ_SHRINER, OBJ_L1LIGHT, OBJ_CANDLE2, OBJ_BOOKCANDLE,
		OBJ_SKPILE, OBJ_DECAP, OBJ_BARREL, OBJ_BARRELEX, OBJ_BANNERL, OBJ_BANNERM, OBJ_BANNERR,
		OBJ_TNUDEM1, OBJ_TNUDEM2, OBJ_TNUDEM3, OBJ_TNUDEM4, OBJ_TNUDEW1, OBJ_TNUDEW2, OBJ_TNUDEW3, OBJ_NUDEW2R,
		OBJ_BLOODFTN, OBJ_TEARFTN, OBJ_GOATSHRINE, OBJ_CAULDRON,
	};
	return Types;
}
void SurvivorsRollingCancelTarget(Player &player, SurvivorsRollingTarget kind, unsigned id)
{
	if (player.destParam1 != static_cast<int>(id)) return;
	const auto action = player.destAction;
	const bool match = (kind == SurvivorsRollingTarget::Item && (action == ACTION_PICKUPITEM || action == ACTION_PICKUPAITEM))
	    || (kind == SurvivorsRollingTarget::Object && (action == ACTION_OPERATE || action == ACTION_DISARM || action == ACTION_OPERATETK))
	    || (kind == SurvivorsRollingTarget::Monster && (action == ACTION_ATTACKMON || action == ACTION_RATTACKMON || action == ACTION_SPELLMON));
	if (match) player.Stop();
}
bool SurvivorsRollingRebaseSuspendedMissile(Missile &missile, int dx, int dy)
{
	ReanchorMovingProjectile(missile, dx, dy);
	if (!CanMoveMissile(missile, dx, dy)) return false;
	MoveMissile(missile, dx, dy);
	return true;
}
bool SurvivorsRollingRebaseMonsterPosition(Monster &monster, unsigned id, int dx, int dy)
{
	if (IsDormantGolem(id, monster) || !CanShiftActor(monster.position, dx, dy)) return false;
	ShiftActor(monster.position, dx, dy);
	return true;
}
int64_t SurvivorsRollingOriginX() { return State.active ? State.terrain.Origin().x : 0; }
int64_t SurvivorsRollingOriginY() { return State.active ? State.terrain.Origin().y : 0; }
uint32_t SurvivorsRollingSeed() { return State.active ? State.seed : 0; }
uint64_t SurvivorsRollingEpoch() { return State.active ? State.epoch : 0; }
bool SurvivorsRollingApplyOrigin(int64_t x, int64_t y, uint64_t epoch)
{
	if (!State.active) return x == 0 && y == 0 && epoch == 0;
	if (epoch < State.epoch || (epoch == State.epoch && (x != State.terrain.Origin().x || y != State.terrain.Origin().y))) return false;
	if (!State.terrain.SetOrigin(x, y)) return false;
	State.epoch = epoch;
	return true;
}
size_t SurvivorsRollingSuspendedMissileCount() { return State.missiles.size(); }
bool SurvivorsRollingMonsterParked(unsigned id) { return id < State.monsters.size() && State.monsters[id].has_value(); }
bool SurvivorsRollingDeadPlayerParked(unsigned id) { return id < State.deadPlayers.size() && State.deadPlayers[id].has_value(); }
bool SurvivorsRollingMissileSupported(MissileID type)
{
	if (!State.active || Vars(type) != MissileVars::Unsupported) return true;
	// Immediate effects have no live projectile to rebase.
	switch (type) {
	case MissileID::Healing: case MissileID::Identify: case MissileID::ItemRepair:
	case MissileID::StaffRecharge: case MissileID::TrapDisarm: case MissileID::Telekinesis:
	case MissileID::HealOther: case MissileID::Resurrect: case MissileID::ManaShield:
	case MissileID::Golem: case MissileID::Mana: case MissileID::Magi:
	case MissileID::Reflect: case MissileID::Berserk: case MissileID::Jester:
	case MissileID::StealPotions: case MissileID::StealMana: case MissileID::OpenNest:
	case MissileID::DiabloApocalypse:
		return true;
	default: return false;
	}
}

void SurvivorsRollingWarnIfTethered()
{
	if (!State.active || !gbIsMultiplayer || AsyncNetIsAuthority() || MyPlayer == nullptr || MyPlayer->hasNoLife()) return;
	if (++State.warningTicks % 100 != 0) return;
	const Point tile = MyPlayer->position.tile;
	const int dx = tile.x >= 90 ? RollDistance : tile.x <= 22 ? -RollDistance : 0;
	const int dy = tile.y >= 90 ? RollDistance : tile.y <= 22 ? -RollDistance : 0;
	if (dx == 0 && dy == 0) return;
	for (const Player &player : Players) {
		if (!player.plractive || !player.isOnActiveLevel() || player.hasNoLife()) continue;
		if (!CanShiftActor(player.position, dx, dy)) {
			EventPlrMsg("Hunters are too far apart to travel farther. Regroup to continue.");
			return;
		}
	}
}

void ResetSurvivorsRollingWorld()
{
	State = {};
}

void InitSurvivorsRollingWorld()
{
	const bool carryItems = State.carryItemsToNextAct;
	auto carriedItems = std::move(State.carriedItems);
	State = {};
	if (carryItems) State.carriedItems = std::move(carriedItems);
	if (!SurvivorsRollingRequested()) return;
	std::vector<SurvivorsFloorTile> floors;
	for (int x = 16; x < 96; x += 2) {
		for (int y = 16; y < 96; y += 2) {
			const SurvivorsFloorTile floor { dPiece[x][y], dPiece[x + 1][y], dPiece[x][y + 1], dPiece[x + 1][y + 1] };
			if (std::all_of(floor.begin(), floor.end(), [](uint16_t piece) {
				return piece != 0 && piece < MAXTILES && HasNoneOf(SOLData[piece], TileProperties::Solid | TileProperties::BlockMissile);
			})) floors.push_back(floor);
		}
	}
	if (!State.terrain.Initialize(DungeonSeeds[currlevel], floors)) return;
	State.seed = DungeonSeeds[currlevel];
	State.terrain.Fill(dPiece);
	for (int x = 0; x < MAXDUNX; ++x) {
		std::fill_n(dSpecial[x], MAXDUNY, 0);
		std::fill_n(dTransVal[x], MAXDUNY, 0);
	}
	State.carryItemsToNextAct = carryItems;
	State.active = true;
	PopulateRollingObjects(true);
}

void SurvivorsRollingPrepareLevelChange()
{
	if (!State.active) return;
	// Cooperative ground is durably journalled and restored on level load;
	// carrying a second copy here would duplicate it at the next act.
	if (gbIsMultiplayer) {
		const Player *anchor = nullptr;
		for (const Player &player : Players)
			if (player.plractive && player.isOnActiveLevel() && !player.hasNoLife()) { anchor = &player; break; }
		if (anchor != nullptr) {
			for (unsigned id = 0; id < State.deadPlayers.size() && id < Players.size(); ++id) {
				if (!State.deadPlayers[id]) continue;
				Player &player = Players[id];
				const auto tile = FindClosestValidPosition([&](Point p) { return PosOkPlayer(player, p); }, anchor->position.tile, 1, 50);
				if (!tile) continue;
				FixPlrWalkTags(player);
				player.position = {};
				player.position.tile = player.position.future = player.position.old = player.position.last = *tile;
				player.occupyTile(*tile, false);
				State.deadPlayers[id].reset();
			}
		}
		return;
	}
	for (auto &item : State.items) State.carriedItems.push_back(std::move(item.item));
	State.items.clear();
	for (unsigned i = 0; i < ActiveItemCount; ++i)
		State.carriedItems.push_back(Items[ActiveItems[i]]);
	State.carryItemsToNextAct = true;
}

void SurvivorsRollingAppendParkedItems(std::vector<Item> &items)
{
	if (!State.active) return;
	for (const StoredItem &item : State.items) items.push_back(item.item);
	for (const Item &item : State.carriedItems) items.push_back(item);
}

unsigned SurvivorsRollingTakeParkedSouls()
{
	unsigned total = 0;
	for (const auto &soul : State.souls)
		total += std::min(soul.value, std::numeric_limits<unsigned>::max() - total);
	State.souls.clear();
	return total;
}

void UpdateSurvivorsRollingWorld(std::vector<SurvivorsRollingSoul> &souls, int boss, int elite)
{
	if (!State.active || MyPlayer == nullptr) return;
	if (MyPlayer->hasNoLife() && (!gbIsMultiplayer || std::none_of(Players.begin(), Players.end(), [](const Player &player) {
		    return player.plractive && player.isOnActiveLevel() && !player.hasNoLife();
	    }))) return;
	// Effects that left the simulation window keep their finite lifetime. This
	// bounds storage and prevents a returned projectile from finding a reused
	// source monster ID long after the original effect would have ended.
	for (auto it = State.missiles.begin(); it != State.missiles.end();) {
		// Runes have no countdown: they remain armed until triggered. An
		// off-window rune must still be there when its owner returns.
		if (it->missile._mitype == MissileID::RuneOfFire || it->missile._mitype == MissileID::RuneOfLight
		    || it->missile._mitype == MissileID::RuneOfNova || it->missile._mitype == MissileID::RuneOfImmolation
		    || it->missile._mitype == MissileID::RuneOfStone) { ++it; continue; }
		if (it->missile.duration > 0) --it->missile.duration;
		if (it->missile.duration <= 0) it = State.missiles.erase(it);
		else ++it;
	}
	AdvanceParkedMonsters(boss, elite);
	RestoreParkedDeadPlayers();
	// Retry props and parked objects whose tile was temporarily occupied by an
	// actor or dropped item; no extra terrain rebase should be required.
	PopulateRollingObjects(false);
	if (State.carryItemsToNextAct) {
		while (!State.carriedItems.empty() && ActiveItemCount < MAXITEMS) {
			const auto position = FindClosestValidPosition(ItemSpaceOk, MyPlayer->position.tile, 1, 50);
			if (!position) break;
			PlaceItemInWorld(std::move(State.carriedItems.back()), *position);
			State.carriedItems.pop_back();
		}
		State.carryItemsToNextAct = !State.carriedItems.empty();
	}
	// A shared authoritative window follows the party's midpoint, not just the
	// host. Otherwise a peer can hit the boundary while the host stays still.
	// Midpoint hysteresis also prevents two separated players from immediately
	// shifting the window back and forth on consecutive simulation ticks.
	Point minimum = MyPlayer->position.tile;
	Point maximum = minimum;
	if (gbIsMultiplayer) {
		bool haveLiving = false;
		for (const Player &player : Players) {
			if (!player.plractive || !player.isOnActiveLevel() || player.hasNoLife()
			    || (player.getId() < State.deadPlayers.size() && State.deadPlayers[player.getId()])) continue;
			if (!haveLiving) { minimum = maximum = player.position.tile; haveLiving = true; continue; }
			minimum.x = std::min<int>(minimum.x, player.position.tile.x);
			minimum.y = std::min<int>(minimum.y, player.position.tile.y);
			maximum.x = std::max<int>(maximum.x, player.position.tile.x);
			maximum.y = std::max<int>(maximum.y, player.position.tile.y);
		}
	}
	const Point focus { (minimum.x + maximum.x) / 2, (minimum.y + maximum.y) / 2 };
	const int dx = focus.x >= RollUpper ? RollDistance : focus.x <= RollLower ? -RollDistance : 0;
	const int dy = focus.y >= RollUpper ? RollDistance : focus.y <= RollLower ? -RollDistance : 0;
	if (gbIsMultiplayer && dx == 0 && dy == 0 && State.pursuitTick % 100 == 0
	    && (minimum.x <= 22 || minimum.y <= 22 || maximum.x >= 90 || maximum.y >= 90))
		EventPlrMsg("Hunters are too far apart to travel farther. Regroup to continue.");
	if (dx == 0 && dy == 0) return;
	if (!MyPlayer->hasNoLife() && !State.deadPlayers[MyPlayerId] && !CanShiftActor(MyPlayer->position, dx, dy)) return;
	if (State.epoch == std::numeric_limits<uint64_t>::max()) return;
	if (gbIsMultiplayer) {
		for (const Player &player : Players) {
			if (!player.plractive || !player.isOnActiveLevel() || player.hasNoLife()
			    || (player.getId() < State.deadPlayers.size() && State.deadPlayers[player.getId()])) continue;
			if (!CanShiftActor(player.position, dx, dy)) {
				if (State.pursuitTick % 100 == 0) EventPlrMsg("Hunters are too far apart to travel farther. Regroup to continue.");
				return;
			}
		}
	}
	const SurvivorsWorldOrigin oldOrigin = State.terrain.Origin();
	if (gbIsMultiplayer) {
		for (Player &player : Players) {
			const unsigned id = player.getId();
			if (id >= State.deadPlayers.size() || !player.plractive || !player.isOnActiveLevel()
			    || !player.hasNoLife() || State.deadPlayers[id] || CanShiftActor(player.position, dx, dy)) continue;
			State.deadPlayers[id] = StoredDeadPlayer { player.position, oldOrigin };
			FixPlrWalkTags(player);
			player.position = {};
		}
	}
	std::array<bool, MaxMonsters> retired {};
	for (size_t i = 0; i < ActiveMonsterCount; ++i) {
		const unsigned id = ActiveMonsters[i];
		Monster &monster = Monsters[id];
		if (IsDormantGolem(id, monster)) continue;
		if (State.monsters[id] || monster.isInvalid || CanShiftActor(monster.position, dx, dy)) continue;
		for (Player &player : Players) SurvivorsRollingCancelTarget(player, SurvivorsRollingTarget::Monster, id);
		M_ClearSquares(monster);
		const bool important = static_cast<int>(id) == boss || static_cast<int>(id) == elite || (monster.flags & MFLAG_GOLEM) != 0;
		const bool referenced = std::any_of(Missiles.begin(), Missiles.end(), [id](const Missile &effect) {
			return (effect._micaster == TARGET_PLAYERS && effect._misource == static_cast<int>(id))
			    || (effect._mitype == MissileID::StoneCurse && effect.var2 == static_cast<int>(id));
		});
		if (important || referenced) {
			State.monsters[id] = StoredMonster { monster.position, monster.enemyPosition, oldOrigin, SuspendLight(monster.lightId), !important };
			monster.position.tile = GolemHoldingCell;
			monster.position.future = GolemHoldingCell;
			monster.position.old = GolemHoldingCell;
			monster.position.last = GolemHoldingCell;
			monster.position.temp = {};
			monster.isInvalid = true;
		} else {
			retired[id] = true;
			if (monster.lightId != NO_LIGHT) AddUnLight(monster.lightId);
			monster.isInvalid = true;
		}
	}
	for (auto it = Missiles.begin(); it != Missiles.end();) {
		const bool retiredSource = it->sourceType() == MissileSource::Monster && it->_misource >= 0
		    && static_cast<unsigned>(it->_misource) < MaxMonsters && retired[it->_misource];
		ReanchorMovingProjectile(*it, dx, dy);
		if (retiredSource || !CanMoveMissile(*it, dx, dy)) SuspendMissile(it, oldOrigin);
		else { MoveMissile(*it, dx, dy); ++it; }
	}
	for (int i = 0; i < ActiveItemCount;) {
		const unsigned id = ActiveItems[i];
		Item &item = Items[id];
		if (Inside(item.position.x - dx, item.position.y - dy)) { item.position -= Displacement { dx, dy }; ++i; continue; }
		for (Player &player : Players) SurvivorsRollingCancelTarget(player, SurvivorsRollingTarget::Item, id);
		const Point old = item.position;
		State.items.push_back({ std::move(item), oldOrigin.x + old.x, oldOrigin.y + old.y });
		dItem[old.x][old.y] = 0;
		DeleteItem(i);
	}
	for (auto it = souls.begin(); it != souls.end();) {
		if (Inside(it->tile.x - dx, it->tile.y - dy)) { it->tile -= Displacement { dx, dy }; ++it; continue; }
		State.souls.push_back({ it->value, oldOrigin.x + it->tile.x, oldOrigin.y + it->tile.y });
		it = souls.erase(it);
	}
	for (int i = 0; i < ActiveObjectCount;) {
		Object &object = Objects[ActiveObjects[i]];
		if (Inside(object.position.x - dx, object.position.y - dy)) {
			object.position -= Displacement { dx, dy };
			++i;
			continue;
		}
		for (Player &player : Players) SurvivorsRollingCancelTarget(player, SurvivorsRollingTarget::Object, ActiveObjects[i]);
		// Release a lit prop's engine light before parking it; the slot would
		// otherwise leak and its stale id would be restored with the object.
		if (GetObjectLightRadius(object._otype) != 0 && object._oVar1 == 1) AddUnLight(object._olid);
		ResetRollingObjectLight(object);
		State.objects.push_back({ object, oldOrigin.x + object.position.x, oldOrigin.y + object.position.y });
		State.parkedObjectTiles.insert({ oldOrigin.x + object.position.x, oldOrigin.y + object.position.y });
		RemoveSurvivorsRollingObject(i);
	}
	DeleteMonsterList();
	// Retired/parked owners may have invalidated lights. Flush them while their
	// old coordinates still refer to the old grid, before translating the grid.
	ProcessLightList();
	ShiftRollingGrid(dFlags, dx, dy); ShiftRollingGrid(dLight, dx, dy, uint8_t { 15 }); ShiftRollingGrid(dPreLight, dx, dy, uint8_t { 15 });
	ShiftRollingGrid(dPlayer, dx, dy); ShiftRollingGrid(dMonster, dx, dy); ShiftRollingGrid(dItem, dx, dy);
	ShiftRollingGrid(dCorpse, dx, dy); ShiftRollingGrid(dObject, dx, dy); ShiftRollingGrid(dSpecial, dx, dy); ShiftRollingGrid(dTransVal, dx, dy);
	if (!State.terrain.Shift(dx, dy, dPiece)) return;
	++State.epoch;
	if (gbIsMultiplayer && AsyncNetIsAuthority()) AsyncNetOnRollingOriginChanged();
	for (Player &player : Players) {
		if (&player != MyPlayer && (!player.plractive || !player.isOnActiveLevel())) continue;
		if (player.getId() < State.deadPlayers.size() && State.deadPlayers[player.getId()]) continue;
		ShiftActor(player.position, dx, dy);
		if (player.destAction == ACTION_ATTACK || player.destAction == ACTION_RATTACK
		    || player.destAction == ACTION_SPELL || player.destAction == ACTION_SPELLWALL) {
			const int x = player.destParam1 - dx;
			const int y = player.destParam2 - dy;
			if (Inside(x, y)) { player.destParam1 = x; player.destParam2 = y; }
			else player.destAction = ACTION_NONE;
		}
	}
	ViewPosition -= Displacement { dx, dy };
	cursPosition -= Displacement { dx, dy };
	for (size_t i = 0; i < ActiveMonsterCount; ++i) {
		const unsigned id = ActiveMonsters[i];
		Monster &monster = Monsters[id];
		if (State.monsters[id] || monster.isInvalid || !SurvivorsRollingRebaseMonsterPosition(monster, id, dx, dy)) continue;
		if (CanShift(monster.enemyPosition, dx, dy)) ShiftPoint(monster.enemyPosition, dx, dy);
		else monster.enemyPosition = MyPlayer->position.future;
		if (monster.ai == MonsterAIID::Scavenger && monster.goal == MonsterGoal::Healing && monster.goalVar1 != 0) {
			monster.goalVar1 -= dx; monster.goalVar2 -= dy;
			if (!Inside(monster.goalVar1 - 1, monster.goalVar2 - 1)) monster.goalVar1 = monster.goalVar2 = 0;
		}
	}
	for (int i = 0; i < ActiveLightCount; ++i) {
		Light &light = Lights[ActiveLights[i]];
		if (light.isInvalid) continue;
		if (!CanShift(light.position.tile, dx, dy)) { light.isInvalid = true; continue; }
		ShiftPoint(light.position.tile, dx, dy);
		light.position.old = light.position.tile;
		light.oldRadius = light.radius;
		light.hasChanged = true;
	}
	UpdateLighting = true;
	for (size_t i = 0; i < VisionActive.size(); ++i) {
		if (!VisionActive[i]) continue;
		auto &vision = VisionList[i];
		if (!CanShift(vision.position.tile, dx, dy)) continue;
		ShiftPoint(vision.position.tile, dx, dy);
		ChangeVisionXY(i, vision.position.tile);
	}
	const SurvivorsWorldOrigin origin = State.terrain.Origin();
	for (auto it = State.items.begin(); it != State.items.end() && ActiveItemCount < MAXITEMS;) {
		const int64_t x = it->x - origin.x, y = it->y - origin.y;
		if (x < 0 || x >= MAXDUNX || y < 0 || y >= MAXDUNY || dItem[x][y] != 0) { ++it; continue; }
		PlaceItemInWorld(std::move(it->item), Point { static_cast<int>(x), static_cast<int>(y) });
		it = State.items.erase(it);
	}
	PopulateRollingObjects(true);
	for (auto it = State.souls.begin(); it != State.souls.end();) {
		const int64_t x = it->x - origin.x, y = it->y - origin.y;
		if (x < 0 || x >= MAXDUNX || y < 0 || y >= MAXDUNY) { ++it; continue; }
		souls.push_back({ Point { static_cast<int>(x), static_cast<int>(y) }, it->value });
		it = State.souls.erase(it);
	}
	AdvanceParkedMonsters(boss, elite);
	RestoreSuspendedMissiles();
	RedoMissileFlags();
}

} // namespace devilution
