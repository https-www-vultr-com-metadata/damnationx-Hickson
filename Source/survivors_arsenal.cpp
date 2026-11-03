#include "survivors_arsenal.h"

#include <array>
#include <cstddef>
#include <format>
#include <string_view>
#include <vector>

#include "control/control.hpp"
#include "engine/backbuffer_state.hpp"
#include "engine/direction.hpp"
#include "levels/dun_tile_data.hpp"
#include "levels/tile_properties.hpp"
#include "missiles.h"
#include "monster.h"
#include "multi.h"
#include "player.h"
#include "survivors.h"
#include "tables/playerdat.hpp"

namespace devilution {
namespace {

enum Weapon : unsigned {
	Ember,
	Warbow,
	Fireball,
	ChargedBolts,
	StormRing,
	HolyBolt,
	FireWall,
	FlameWave,
	BloodStar,
	SanctifiedEdge,
};
enum Passive : unsigned {
	Might,
	Quickening,
	SoulReach,
	Fortitude,
	Renewal,
	Multiplicity,
};
struct WeaponDefinition {
	std::string_view name;
	std::string_view evolution;
	std::string_view effect;
	Passive catalyst;
};
constexpr std::array<WeaponDefinition, SurvivorsWeaponCount> WeaponDefinitions { {
	{ "Ember", "HELLFIRE COVENANT", "homing aim, rapid fire bolts", Might },
	{ "Warbow", "WINDFORCE", "arrows seek separate enemies", Multiplicity },
	{ "Fireball", "ARMAGEDDON", "explosions strike clustered foes", Might },
	{ "Charged Bolts", "THUNDERSTORM", "wandering bolts spread in a fan", Quickening },
	{ "Storm Ring", "EYE OF THE STORM", "piercing lightning in all directions", SoulReach },
	{ "Holy Bolt", "HEAVEN'S JUDGMENT", "slays undead; evolved adds lightning", Renewal },
	{ "Fire Wall", "INFERNAL PRISON", "burning ground under pursuing foes", Fortitude },
	{ "Flame Wave", "SEA OF FLAMES", "broad waves pass through enemies", Multiplicity },
	{ "Blood Star", "BLOOD MOON", "magic stars hit several targets", Renewal },
	{ "Sanctified Edge", "AEGIS OF TRISTRAM", "automatic melee swing at nearby foes", Fortitude },
} };
constexpr std::array<std::string_view, SurvivorsPassiveCount> PassiveNames { "Might", "Quickening", "Soul Reach", "Fortitude", "Renewal", "Multiplicity" };
constexpr std::array<std::string_view, SurvivorsPassiveCount> PassiveEffects { "+4 damage to every weapon", "5% faster independent cooldowns", "+1 soul pickup range", "+10 maximum life, +4% elemental ward", "regenerate 1 HP each second", "+1 projectile to aimed weapons" };
constexpr unsigned FirstFallback = SurvivorsWeaponCount + SurvivorsPassiveCount;
constexpr unsigned InvalidOffer = FirstFallback + 3;
constexpr int FlameWaveTag = 0x535652;
constexpr size_t MissileBudget = 320;

struct Arsenal : SurvivorsArsenalSnapshot {
	Player *player = nullptr;
	SurvivorsArsenalBonuses bonuses;
	std::array<unsigned, SurvivorsWeaponCount> readyAt { };
	unsigned bonusDamage = 0;
	unsigned bonusMagnet = 0;
	unsigned rotation = 0;
	uint32_t random = 1;
};
std::array<Arsenal, MAX_PLRS> PlayerArsenals;
unsigned SelectedArsenalPlayer = 0;
Arsenal &ActiveArsenal() { return PlayerArsenals[SelectedArsenalPlayer]; }

uint32_t NextRandom()
{
	uint32_t &state = ActiveArsenal().random;
	state ^= state << 13;
	state ^= state >> 17;
	state ^= state << 5;
	return state;
}

bool Eligible(unsigned item)
{
	return SurvivorsArsenalCanOffer(ActiveArsenal(), item);
}

std::vector<unsigned> EligiblePool()
{
	std::vector<unsigned> pool;
	for (unsigned item = 0; item < FirstFallback; ++item) {
		if (Eligible(item))
			pool.push_back(item);
	}
	return pool;
}

unsigned SelectReplacement(const std::array<unsigned, 3> &excluded)
{
	auto pool = EligiblePool();
	for (unsigned fallback = FirstFallback; fallback < InvalidOffer; ++fallback)
		pool.push_back(fallback);
	pool.erase(std::remove_if(pool.begin(), pool.end(), [&](unsigned item) { return std::find(excluded.begin(), excluded.end(), item) != excluded.end(); }), pool.end());
	return pool.empty() ? FirstFallback : pool[NextRandom() % pool.size()];
}

void AddMaximumLife(unsigned hitPoints)
{
	Player &player = *ActiveArsenal().player;
	const int life = static_cast<int>(std::min(hitPoints, 1000U)) << 6;
	player._pMaxHPBase += life;
	player._pMaxHP += life;
	player._pHPBase = std::min(player._pMaxHPBase, player._pHPBase + life);
	player._pHitPoints = std::min(player._pMaxHP, player._pHitPoints + life);
	RedrawComponent(PanelDrawComponent::Health);
}

bool ValidTarget(const Monster &monster)
{
	return !monster.isInvalid && monster.hitPoints > 0 && monster.mode != MonsterMode::Death && (monster.flags & (MFLAG_GOLEM | MFLAG_HIDDEN)) == 0;
}

std::vector<Monster *> FindTargets(bool undeadOnly)
{
	std::vector<Monster *> targets;
	const Point source = ActiveArsenal().player->position.tile;
	for (size_t i = 0; i < ActiveMonsterCount; ++i) {
		Monster &monster = Monsters[ActiveMonsters[i]];
		if (!ValidTarget(monster) || (undeadOnly && monster.data().monsterClass != MonsterClass::Undead && monster.type().type != MT_DIABLO))
			continue;
		if (source.WalkingDistance(monster.position.tile) > 14 || !LineClearMissile(source, monster.position.tile))
			continue;
		targets.push_back(&monster);
	}
	std::sort(targets.begin(), targets.end(), [source](const Monster *a, const Monster *b) {
		const int distanceA = source.WalkingDistance(a->position.tile);
		const int distanceB = source.WalkingDistance(b->position.tile);
		const int boss = SurvivorsBossMonsterId();
		const bool priorityA = static_cast<int>(a->getId()) == boss && distanceA <= 9;
		const bool priorityB = static_cast<int>(b->getId()) == boss && distanceB <= 9;
		if (priorityA != priorityB) return priorityA;
		return distanceA == distanceB ? a->getId() < b->getId() : distanceA < distanceB;
	});
	return targets;
}

// Bypass spell casting: direct native projectiles neither consume mana nor run
// spell animation locks. Several native add callbacks replace their input damage,
// so set the authoritative damage AFTER AddMissile has initialized each missile.
Missile *Projectile(MissileID type, Point source, Point target, int damage, unsigned rank)
{
	if (!InDungeonBounds(source) || !InDungeonBounds(target))
		return nullptr;
	ReclaimSurvivorsArsenalProjectiles(ActiveArsenal().player->getId());
	Missile *missile = AddMissile(source, target, GetDirection(source, target), type, TARGET_MONSTERS, *ActiveArsenal().player, damage, std::min(rank, 5U));
	if (missile != nullptr)
		missile->_midam = damage;
	return missile;
}

bool CastWeapon(unsigned weapon, unsigned level, unsigned tickRate)
{
	const unsigned rank = ActiveArsenal().ranks[weapon];
	const bool evolved = rank == SurvivorsWeaponMaxRank;
	Player &player = *ActiveArsenal().player;
	const Point source = player.position.tile;
	const unsigned bonus = ActiveArsenal().bonuses.damage + ActiveArsenal().passives[Might] * 4 + ActiveArsenal().bonusDamage;
	const int damage = static_cast<int>(SurvivorsWeaponStatDamage(weapon,
	    SurvivorsWeaponImpactDamage(weapon, level, rank, bonus), player._pMagic,
	    GetClassAttributes(player._pClass).baseMag));
	const unsigned multiplicity = ActiveArsenal().passives[Multiplicity];
	auto targets = FindTargets(weapon == HolyBolt && !evolved);
	if (targets.empty())
		return false;
	const Point nearest = targets.front()->position.tile;
	const Direction facing = GetDirection(source, nearest);
	switch (weapon) {
	case Ember:
	case Warbow:
	case Fireball:
	case BloodStar:
	case HolyBolt: {
		const MissileID type = weapon == Ember ? MissileID::Firebolt : weapon == Warbow ? MissileID::Arrow : weapon == Fireball ? MissileID::Fireball : weapon == BloodStar ? MissileID::BloodStar : MissileID::HolyBolt;
		const unsigned count = std::min(8U, 1 + multiplicity + (rank >= 3 ? 1U : 0U) + (evolved ? 2U : 0U));
		for (unsigned shot = 0; shot < count; ++shot) {
			const Point destination = targets[shot % targets.size()]->position.tile;
			if (weapon == HolyBolt && evolved) {
				// Holy missiles retain Diablo's undead-only rule. Evolved lightning
				// covers living enemies without silently changing that core rule.
				if (Missile *strike = Projectile(MissileID::Lightning, destination, source, damage << 3, rank))
					strike->duration = 6;
			}
			Projectile(type, source, destination, damage, rank);
			if (weapon == Ember && evolved)
				Projectile(MissileID::Fireball, source, destination, damage / 2, 2);
		}
	} break;
	case ChargedBolts: {
		const unsigned count = std::min(12U, 3 + rank + multiplicity);
		for (unsigned shot = 0; shot < count; ++shot) {
			const int turn = static_cast<int>(shot % 3) - 1;
			const Direction direction = static_cast<Direction>((static_cast<int>(facing) + turn + 8) % 8);
			Projectile(MissileID::ChargedBolt, source, source + direction, damage, rank);
		}
		if (evolved)
			Projectile(MissileID::NovaBall, source, nearest, damage * 2, rank);
	} break;
	case StormRing:
		for (unsigned shot = 0; shot < 8; ++shot) {
			const Direction direction = static_cast<Direction>((shot + ActiveArsenal().rotation) % 8);
			Projectile(MissileID::NovaBall, source, source + direction, damage, rank);
			if (evolved)
				Projectile(MissileID::ChargedBolt, source, source + direction, damage, rank);
		}
		++ActiveArsenal().rotation;
		break;
	case FireWall: {
		const unsigned count = std::min(5U, 1 + rank / 2 + (evolved ? 2U : 0U));
		for (unsigned patch = 0; patch < count && patch < targets.size(); ++patch) {
			const Point tile = targets[patch]->position.tile;
			if (IsMissileBlockedByTile(tile))
				continue;
			// ProcessFireWall takes already shifted (1/64 HP) damage each tick.
			if (Missile *wall = Projectile(MissileID::FireWall, tile, tile, damage << 2, rank)) {
				wall->duration = static_cast<int>(tickRate * (evolved ? 4U : 2U));
				wall->var1 = wall->duration - wall->_miAnimLen;
			}
		}
	} break;
	case FlameWave: {
		const unsigned count = std::min(7U, 3 + multiplicity + (evolved ? 2U : 0U));
		for (unsigned shot = 0; shot < count; ++shot) {
			const Direction direction = static_cast<Direction>((static_cast<unsigned>(facing) + shot + 8 - count / 2) % 8);
			if (Missile *wave = Projectile(MissileID::FlameWave, source, source + direction, damage, rank)) {
				// Native waves travel until blocked, so bound our waves explicitly.
				// These variables are unused by the native FlameWave processor.
				wave->var7 = FlameWaveTag;
				wave->var6 = static_cast<int>(tickRate * 2);
			}
		}
	} break;
	case SanctifiedEdge:
		// Boss priority must not hide an adjacent target from this short-range weapon.
		for (const Monster *target : targets) {
			if (source.WalkingDistance(target->position.tile) == 1)
				return StartSurvivorsAutoMelee(player, target->position.tile, damage, evolved);
		}
		return false;
	}
	// Arsenal projectiles bypass the native attack/spell modes; still show the
	// class's shot or cast over the current stand/walk.
	player_graphic graphic = player_graphic::Fire;
	if (weapon == Warbow) graphic = player_graphic::Attack;
	else if (weapon == ChargedBolts || weapon == StormRing) graphic = player_graphic::Lightning;
	else if (weapon == BloodStar || weapon == HolyBolt) graphic = player_graphic::Magic;
	StartSurvivorsActionAnimation(player, graphic, nearest);
	return true;
}

std::string OfferLabel(unsigned item)
{
	if (item < SurvivorsWeaponCount) {
		const auto &definition = WeaponDefinitions[item];
		const unsigned rank = ActiveArsenal().ranks[item];
		if (rank == 5) {
			constexpr std::array<std::string_view, SurvivorsWeaponCount> effects {
				"each bolt adds a fireball", "+2 aimed arrows", "+2 explosions", "adds piercing lightning",
				"adds 8 charged bolts", "lightning strikes living foes", "more patches, double duration",
				"wider flame fan", "+2 magic stars", "cleaves adjacent foes",
			};
			return std::format("EVOLVE {} - {}", definition.evolution, effects[item]);
		}
		if (rank == 4)
			return std::format("{} 5/5 - evolve with {} II", definition.name, PassiveNames[definition.catalyst]);
		return std::format("{} {} {}/5 - {}", rank == 0 ? "NEW" : "UPGRADE", definition.name, rank + 1, rank == 0 ? definition.effect : std::string_view("+5 base damage, faster cooldown"));
	}
	if (item < FirstFallback) {
		const unsigned passive = item - SurvivorsWeaponCount;
		return std::format("{} {}/5 - {}", PassiveNames[passive], ActiveArsenal().passives[passive] + 1, PassiveEffects[passive]);
	}
	if (item == FirstFallback)
		return "Ash Offering - +3 damage to every weapon";
	if (item == FirstFallback + 1)
		return "Rite of Blood - +5 maximum life";
	if (item == FirstFallback + 2)
		return "Soul Harvest - +1 soul pickup range";
	return "";
}

} // namespace

unsigned ReclaimSurvivorsArsenalProjectiles(unsigned owner)
{
	// Enemy volleys must never silence the player's weapons. Recycle old
	// friendly effects at this player's budget instead of refusing every cast
	// once the combined enemy/friendly list passes a global threshold. Native
	// Fire Wall descendants use TARGET_BOTH but retain their player source.
	const auto ownedProjectile = [owner](const Missile &missile) {
		return !missile._miDelFlag
		    && (missile._micaster == TARGET_MONSTERS || missile._micaster == TARGET_BOTH)
		    && missile._misource == static_cast<int>(owner)
		    && missile._mitype != MissileID::FlashBottom && missile._mitype != MissileID::FlashTop;
	};
	size_t count = std::count_if(Missiles.begin(), Missiles.end(), ownedProjectile);
	unsigned retired = 0;
	for (Missile &missile : Missiles) {
		if (count < MissileBudget) break;
		if (!ownedProjectile(missile)) continue;
		// ProcessMissiles performs normal light/animation cleanup next tick.
		missile._miDelFlag = true;
		--count;
		++retired;
	}
	return retired;
}

bool SurvivorsArsenalCanOffer(const SurvivorsArsenalSnapshot &state, unsigned item)
{
	if (item >= FirstFallback)
		return item < InvalidOffer;
	if (state.banished[item])
		return false;
	if (item >= SurvivorsWeaponCount)
		return state.passives[item - SurvivorsWeaponCount] < SurvivorsPassiveMaxRank;
	const unsigned rank = state.ranks[item];
	const size_t slots = static_cast<size_t>(std::count_if(state.ranks.begin(), state.ranks.end(), [](unsigned value) { return value != 0; }));
	if (rank == 0 && slots >= SurvivorsWeaponSlots)
		return false;
	return SurvivorsWeaponCanAdvance(rank, state.passives[WeaponDefinitions[item].catalyst]);
}

unsigned SurvivorsArsenalEvolutionCatalyst(unsigned weapon)
{
	return weapon < SurvivorsWeaponCount ? static_cast<unsigned>(WeaponDefinitions[weapon].catalyst) : SurvivorsPassiveCount;
}

SurvivorsArsenalSnapshot GetSurvivorsArsenalSnapshot()
{
	return ActiveArsenal();
}

void SelectSurvivorsArsenalPlayer(unsigned playerId)
{
	SelectedArsenalPlayer = std::min(playerId, static_cast<unsigned>(MAX_PLRS - 1));
}

void ResetAllSurvivorsArsenals()
{
	PlayerArsenals = { };
	SelectedArsenalPlayer = std::min(static_cast<unsigned>(MyPlayerId), static_cast<unsigned>(MAX_PLRS - 1));
}

bool ApplySurvivorsArsenalReplica(unsigned playerId, const SurvivorsArsenalSnapshot &snapshot)
{
	if (playerId >= MAX_PLRS || snapshot.rerolls > 13 || snapshot.skips > 2 || snapshot.banishes > 2)
		return false;
	unsigned weapons = 0;
	for (unsigned rank : snapshot.ranks) { if (rank > SurvivorsWeaponMaxRank) return false; if (rank != 0) ++weapons; }
	if (weapons > SurvivorsWeaponSlots) return false;
	for (unsigned rank : snapshot.passives)
		if (rank > SurvivorsPassiveMaxRank) return false;
	for (unsigned offer : snapshot.offers)
		if (offer > SurvivorsArsenalInvalidOffer) return false;
	static_cast<SurvivorsArsenalSnapshot &>(PlayerArsenals[playerId]) = snapshot;
	PlayerArsenals[playerId].player = playerId < Players.size() ? &Players[playerId] : nullptr;
	return true;
}

void ResetSurvivorsArsenal()
{
	ActiveArsenal() = { };
}

void StartSurvivorsArsenal(Player &player, const SurvivorsArsenalBonuses &bonuses, uint32_t seed)
{
	ResetSurvivorsArsenal();
	ActiveArsenal().player = &player;
	ActiveArsenal().bonuses = bonuses;
	ActiveArsenal().random = seed == 0 ? 0x9e3779b9U : seed;
	ActiveArsenal().rerolls += std::min(bonuses.rerolls, 10U);
	AddMaximumLife(bonuses.vitality);
	switch (player._pClass) {
	case HeroClass::Warrior:
	case HeroClass::Barbarian:
		ActiveArsenal().ranks[SanctifiedEdge] = 1;
		break;
	case HeroClass::Rogue:
		ActiveArsenal().ranks[Warbow] = 1;
		ActiveArsenal().ranks[HolyBolt] = 1;
		break;
	case HeroClass::Sorcerer:
		ActiveArsenal().ranks[Ember] = 1;
		ActiveArsenal().ranks[ChargedBolts] = 1;
		break;
	case HeroClass::Monk:
		ActiveArsenal().ranks[SanctifiedEdge] = 1;
		ActiveArsenal().ranks[StormRing] = 1;
		break;
	case HeroClass::Bard:
		ActiveArsenal().ranks[Warbow] = 1;
		ActiveArsenal().ranks[BloodStar] = 1;
		break;
	default:
		ActiveArsenal().ranks[Ember] = 1;
		break;
	}
}

void GrantSurvivorsArsenalLife(unsigned hitPoints)
{
	if (ActiveArsenal().player != nullptr) AddMaximumLife(hitPoints);
}

void ProcessSurvivorsArsenal(unsigned ticks, unsigned tickRate, unsigned level)
{
	if (ActiveArsenal().player == nullptr)
		return;
	tickRate = std::clamp(tickRate, 1U, 120U);
	Player &player = *ActiveArsenal().player;
	if (player._pHitPoints <= 0)
		return;
	// Survivors starts with class equipment rather than campaign resistance
	// gear. Fortitude provides an explicit defense against later caster packs.
	const int8_t ward = static_cast<int8_t>(ActiveArsenal().passives[Fortitude] * 4);
	player._pFireResist = std::max(player._pFireResist, ward);
	player._pLghtResist = std::max(player._pLghtResist, ward);
	player._pMagResist = std::max(player._pMagResist, ward);
	for (Missile &missile : Missiles) {
		if (missile._misource == player.getId() && missile._mitype == MissileID::FlameWave && missile.var7 == FlameWaveTag && --missile.var6 <= 0)
			missile._miDelFlag = true;
	}
	if (ActiveArsenal().passives[Renewal] != 0 && ticks % tickRate == 0) {
		const int healing = static_cast<int>(ActiveArsenal().passives[Renewal]) << 6;
		player._pHPBase = std::min(player._pMaxHPBase, player._pHPBase + healing);
		player._pHitPoints = std::min(player._pMaxHP, player._pHitPoints + healing);
		RedrawComponent(PanelDrawComponent::Health);
	}
	// Native arrows read damage from their source player on EVERY collision.
	// Refresh it each tick rather than relying on AddMissile's damage argument.
	if (ActiveArsenal().ranks[Warbow] != 0) {
		const unsigned bonus = ActiveArsenal().bonuses.damage + ActiveArsenal().passives[Might] * 4 + ActiveArsenal().bonusDamage;
		player._pIMinDam = static_cast<int>(SurvivorsWeaponDamage(level, ActiveArsenal().ranks[Warbow], bonus));
		player._pIMaxDam = player._pIMinDam + 3;
	}
	for (unsigned weapon = 0; weapon < SurvivorsWeaponCount; ++weapon) {
		const unsigned rank = ActiveArsenal().ranks[weapon];
		if (rank == 0 || ticks < ActiveArsenal().readyAt[weapon]
		    || (weapon == SanctifiedEdge && !ActiveArsenal().bonuses.autoMelee))
			continue;
		if (!CastWeapon(weapon, level, tickRate))
			continue;
		const unsigned haste = ActiveArsenal().bonuses.haste + ActiveArsenal().passives[Quickening];
		ActiveArsenal().readyAt[weapon] = ticks + SurvivorsWeaponCooldown(SurvivorsWeaponBalance[weapon].cooldownMilliseconds, rank, haste, tickRate);
	}
}

void DraftSurvivorsArsenal()
{
	if (ActiveArsenal().player == nullptr)
		return;
	auto pool = EligiblePool();
	for (unsigned fallback = FirstFallback; pool.size() < 3 && fallback < InvalidOffer; ++fallback)
		pool.push_back(fallback);
	for (unsigned choice = 0; choice < 3; ++choice) {
		const size_t selected = NextRandom() % pool.size();
		ActiveArsenal().offers[choice] = pool[selected];
		pool.erase(pool.begin() + static_cast<std::ptrdiff_t>(selected));
	}
	ActiveArsenal().drafted = true;
}

std::string SurvivorsArsenalItemName(unsigned item)
{
	if (item < SurvivorsWeaponCount) return std::string(WeaponDefinitions[item].name);
	if (item < FirstFallback) return std::string(PassiveNames[item - SurvivorsWeaponCount]);
	constexpr std::array<std::string_view, 3> rites { "Blood Rite", "Life Rite", "Soul Rite" };
	return item < InvalidOffer ? std::string(rites[item - FirstFallback]) : "Unavailable";
}

std::array<SurvivorsArsenalOfferInfo, 3> SurvivorsArsenalOfferDetails()
{
	std::array<SurvivorsArsenalOfferInfo, 3> result;
	for (unsigned choice = 0; choice < result.size(); ++choice) {
		auto &info = result[choice];
		const unsigned item = ActiveArsenal().offers[choice];
		info.item = item;
		info.name = SurvivorsArsenalItemName(item);
		info.enabled = ActiveArsenal().drafted && Eligible(item);
		if (item < SurvivorsWeaponCount) {
			const auto &definition = WeaponDefinitions[item];
			info.isWeapon = true;
			info.currentRank = ActiveArsenal().ranks[item];
			info.nextRank = std::min(info.currentRank + 1, SurvivorsWeaponMaxRank);
			info.maxRank = SurvivorsWeaponMaxRank;
			info.evolution = info.nextRank == SurvivorsWeaponMaxRank;
			info.description = std::string(definition.effect);
			const unsigned catalyst = ActiveArsenal().passives[definition.catalyst];
			info.requirements = std::format("Evolution: weapon V + {} II ({}/2)", PassiveNames[definition.catalyst], catalyst);
			if (info.evolution) {
				info.name = std::string(definition.evolution);
				info.description = OfferLabel(item);
				info.requirements = std::format("Evolution ready: {} II", PassiveNames[definition.catalyst]);
			}
		} else if (item < FirstFallback) {
			const unsigned passive = item - SurvivorsWeaponCount;
			info.currentRank = ActiveArsenal().passives[passive];
			info.nextRank = std::min(info.currentRank + 1, SurvivorsPassiveMaxRank);
			info.maxRank = SurvivorsPassiveMaxRank;
			info.description = std::string(PassiveEffects[passive]);
			info.requirements = "Rank II evolves: ";
			bool first = true;
			for (unsigned weapon = 0; weapon < SurvivorsWeaponCount; ++weapon) {
				if (WeaponDefinitions[weapon].catalyst != passive) continue;
				if (!first) info.requirements += ", ";
				info.requirements += WeaponDefinitions[weapon].name;
				first = false;
			}
		} else if (item < InvalidOffer) {
			constexpr std::array<std::string_view, 3> effects { "+3 permanent damage for this run", "+5 maximum life for this run", "+1 pickup range for this run" };
			info.nextRank = info.maxRank = 1;
			info.description = std::string(effects[item - FirstFallback]);
			info.requirements = "Repeatable rite. No weapon slot required.";
		} else {
			info.description = "Waiting for an authoritative draft.";
		}
	}
	return result;
}

std::array<std::string, 3> SurvivorsArsenalOfferLabels()
{
	return { OfferLabel(ActiveArsenal().offers[0]), OfferLabel(ActiveArsenal().offers[1]), OfferLabel(ActiveArsenal().offers[2]) };
}

bool ChooseSurvivorsArsenalUpgrade(unsigned choice)
{
	if (!ActiveArsenal().drafted || choice >= ActiveArsenal().offers.size())
		return false;
	const unsigned item = ActiveArsenal().offers[choice];
	if (!Eligible(item))
		return false;
	if (item < SurvivorsWeaponCount) {
		++ActiveArsenal().ranks[item];
		ActiveArsenal().readyAt[item] = 0;
	} else if (item < FirstFallback) {
		const unsigned passive = item - SurvivorsWeaponCount;
		++ActiveArsenal().passives[passive];
		if (passive == Fortitude)
			AddMaximumLife(10);
	} else if (item == FirstFallback) {
		ActiveArsenal().bonusDamage = std::min(1000U, ActiveArsenal().bonusDamage + 3);
	} else if (item == FirstFallback + 1) {
		AddMaximumLife(5);
	} else {
		ActiveArsenal().bonusMagnet = std::min(20U, ActiveArsenal().bonusMagnet + 1);
	}
	ActiveArsenal().drafted = false;
	return true;
}

bool RerollSurvivorsArsenal()
{
	if (!ActiveArsenal().drafted || ActiveArsenal().rerolls == 0)
		return false;
	--ActiveArsenal().rerolls;
	const auto previous = ActiveArsenal().offers;
	DraftSurvivorsArsenal();
	// If the random draw reproduced the same cards, change at least one card.
	if (previous == ActiveArsenal().offers)
		ActiveArsenal().offers[0] = SelectReplacement(ActiveArsenal().offers);
	return true;
}

bool BanishSurvivorsArsenalOffer(unsigned choice)
{
	if (!ActiveArsenal().drafted || ActiveArsenal().banishes == 0 || choice >= ActiveArsenal().offers.size())
		return false;
	const unsigned item = ActiveArsenal().offers[choice];
	if (item >= FirstFallback)
		return false;
	--ActiveArsenal().banishes;
	ActiveArsenal().banished[item] = true;
	ActiveArsenal().offers[choice] = SelectReplacement(ActiveArsenal().offers);
	return true;
}

bool CanSkipSurvivorsArsenalDraft()
{
	return ActiveArsenal().drafted && ActiveArsenal().skips > 0;
}

void SkipSurvivorsArsenalDraft()
{
	if (CanSkipSurvivorsArsenalDraft()) {
		--ActiveArsenal().skips;
		ActiveArsenal().drafted = false;
	}
}

unsigned SurvivorsArsenalMagnet()
{
	return std::min(24U, ActiveArsenal().bonuses.magnet + ActiveArsenal().passives[SoulReach] + ActiveArsenal().bonusMagnet);
}

std::string SurvivorsArsenalSummary()
{
	// Compact base names keep a six-weapon loadout inside the 600px HUD.
	// Full weapon/evolution names remain on the actual draft cards.
	constexpr std::array<std::string_view, SurvivorsWeaponCount> names {
		"Ember", "Warbow", "Fireball", "Bolts", "Storm", "Holy", "Wall", "Flame", "Stars", "Edge",
	};
	std::string result;
	for (unsigned weapon = 0; weapon < SurvivorsWeaponCount; ++weapon) {
		const unsigned rank = ActiveArsenal().ranks[weapon];
		if (rank == 0)
			continue;
		if (!result.empty())
			result += " | ";
		result += rank == SurvivorsWeaponMaxRank ? std::format("{} VI*", names[weapon]) : std::format("{} {}", names[weapon], rank);
	}
	return result;
}

std::string SurvivorsArsenalDraftResources()
{
	return std::format("R / RB: reroll {}   Space / B: skip {}   Shift+1/2/3 or right-click: banish {}", ActiveArsenal().rerolls, ActiveArsenal().skips, ActiveArsenal().banishes);
}

} // namespace devilution
