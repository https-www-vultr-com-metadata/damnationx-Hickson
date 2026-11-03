#include "host/host_frontend.hpp"
/**
 * @file mainmenu.cpp
 *
 * Implementation of functions for interacting with the main menu.
 */

#include <algorithm>
#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include <format>

#ifdef USE_SDL3
#include <SDL3/SDL_surface.h>
#include <SDL3/SDL_timer.h>
#else
#include <SDL.h>
#endif

#include "DiabloUI/diabloui.h"
#include "DiabloUI/ui_flags.hpp"
#include "DiabloUI/ui_item.h"
#ifdef DEVILUTIONX_LOBBY
#include "dvlnet/lobby_api.h"
#endif
#include "DiabloUI/selok.h"
#include "DiabloUI/settingsmenu.h"
#ifdef DEVILUTIONX_D2_SOUND_IMPORT
#include "d2sound/d2sound_ui.hpp"
#endif
#include "engine/assets.hpp"
#include "engine/demomode.h"
#include "game_mode.hpp"
#include "host/host_hooks.hpp"
#include "init.hpp"
#include "movie.h"
#include "options.h"
#include "pfile.h"
#ifdef DAMNATION_S1_BRIDGE
#include "s1_bridge.hpp"
#endif
#include "storm/storm_net.hpp"
#include "survivors.h"
#include "survivors_progression.h"
#include "utils/language.h"
#include "utils/ui_fwd.h"
#include "utils/utf8.hpp"

namespace devilution {

uint32_t gSaveNumber;

namespace {

_music_id NextTrack()
{
	if (gbIsSpawn) {
		return TMUSIC_INTRO;
	}

	switch (sgnMusicTrack) {
	case TMUSIC_INTRO:
		return TMUSIC_CATACOMBS;
	case TMUSIC_CATACOMBS:
		return TMUSIC_CAVES;
	case TMUSIC_CAVES:
		return TMUSIC_HELL;
	case TMUSIC_HELL:
		return gbIsHellfire ? TMUSIC_NEST : TMUSIC_INTRO;
	case TMUSIC_NEST:
		return gbIsHellfire ? TMUSIC_CRYPT : TMUSIC_INTRO;
	default:
		return TMUSIC_INTRO;
	}
}

void RefreshMusic()
{
	music_start(NextTrack());
}

bool InitMenu(_selhero_selections type)
{
	bool success;

	if (type == SELHERO_PREVIOUS)
		return true;

	success = StartGame(type != SELHERO_CONTINUE, type != SELHERO_CONNECT);
	host::SetFrontendStage(0);
	if (success)
		RefreshMusic();

	return success;
}

bool InitSinglePlayerMenu()
{
	gbIsMultiplayer = false;
	return InitMenu(SELHERO_NEW_DUNGEON);
}

int SurvivorsCampSelection = -1;
void SelectSurvivorsCamp(size_t selection) { SurvivorsCampSelection = selection == 1 ? 8 : selection == 2 ? 9 : static_cast<int>(selection > 2 ? selection - 2 : selection); }
void LeaveSurvivorsCamp() { SurvivorsCampSelection = 7; }

bool ShowSurvivorsCamp()
{
	host::FrontendScope frontendScope(5);
	while (true) {
		const auto &profile = GetSurvivorsProfile();
		const std::array<const char *, 3> difficulties { "Normal", "Nightmare", "Hell" };
		std::vector<std::unique_ptr<UiListItem>> choices;
		std::vector<std::unique_ptr<UiItemBase>> dialog;
		LoadBackgroundArt(gbIsSpawn ? "ui_art\\swmmenu" : "ui_art\\mainmenu");
		UiAddBackground(&dialog);
		UiAddLogo(&dialog);
		const Point origin = GetUIRectangle().position;
		const std::string stats = std::format("Soul Shards: {}   Hunts: {}   Victories: {}   Kills: {}\nBest: {:02}:{:02}   Level {}   Acts: {}   Bosses: {}",
		    profile.soulShards, profile.runs, profile.victories, profile.totalKills,
		    profile.bestSeconds / 60, profile.bestSeconds % 60, profile.bestLevel, profile.clearedActs, profile.slainBosses);
		const std::string status = SurvivorsProgressionStatus();
		const auto rect = [&](int y, int height) { return SDL_Rect { static_cast<Sint16>(origin.x + 20), static_cast<Sint16>(origin.y + y), 600, static_cast<Sint16>(height) }; };
		dialog.push_back(std::make_unique<UiArtText>("SURVIVORS - THE CAMPFIRE", rect(160, 30), UiFlags::FontSize24 | UiFlags::AlignCenter | UiFlags::ColorUiGold));
		dialog.push_back(std::make_unique<UiArtText>(stats.c_str(), rect(196, 40), UiFlags::FontSize12 | UiFlags::AlignCenter | UiFlags::ColorUiSilver));
		choices.push_back(std::make_unique<UiListItem>(std::format("Start hunt - {}", difficulties[profile.selectedDifficulty]), 0));
		choices.push_back(std::make_unique<UiListItem>(std::string("Cooperative hunt - host / join"), 8));
		choices.push_back(std::make_unique<UiListItem>(std::string("Shop gear - select hero"), 9));
		const std::array<const char *, 4> names { "Might", "Vitality", "Quickening", "Soul Reach" };
		const std::array<std::string, 4> effects {
			std::format("+{} damage", std::min(profile.ranks[0] + 1, SurvivorsMaxPermanentRank) * 2),
			std::format("+{} max life", std::min(profile.ranks[1] + 1, SurvivorsMaxPermanentRank) * 5),
			std::format("-{}% cooldown", std::min(profile.ranks[2] + 1, SurvivorsMaxPermanentRank) * 5),
			std::format("+{} pickup radius", std::min(profile.ranks[3] + 1, SurvivorsMaxPermanentRank))
		};
		for (unsigned gift = 0; gift < names.size(); ++gift) {
			const auto cost = SurvivorsPermanentUpgradeCost(profile, static_cast<SurvivorsPermanentUpgrade>(gift));
			const bool affordable = cost != 0 && profile.soulShards >= cost;
			std::string label = cost == 0 ? std::format("{} {}/10 - mastered", names[gift], profile.ranks[gift])
			    : std::format("{} {}/10 - {} - {} shards", names[gift], profile.ranks[gift], effects[gift], cost);
			choices.push_back(std::make_unique<UiListItem>(std::move(label), static_cast<int>(gift + 1), affordable ? UiFlags::None : UiFlags::ElementDisabled | UiFlags::ColorUiSilver));
		}
		const bool difficultyChoice = profile.unlockedDifficulty > 0;
		choices.push_back(std::make_unique<UiListItem>(difficultyChoice
		    ? std::format("Change difficulty: {} (unlocked through {})", difficulties[profile.selectedDifficulty], difficulties[profile.unlockedDifficulty])
		    : "Difficulty: Normal - win a hunt to unlock Nightmare", 5,
		    difficultyChoice ? UiFlags::None : UiFlags::ElementDisabled | UiFlags::ColorUiSilver));
		choices.push_back(std::make_unique<UiListItem>(std::format("Auto melee: {} - {}", profile.autoMelee ? "ON" : "OFF",
		    profile.autoMelee ? "Edge strikes nearby" : "left click to strike"), 6));
		choices.push_back(std::make_unique<UiListItem>(std::string("Back to main menu"), 7));
		dialog.push_back(std::make_unique<UiList>(choices, choices.size(), origin.x + 20, origin.y + 240, 600, 19, UiFlags::FontSize12 | UiFlags::AlignCenter | UiFlags::ColorUiGold, 1));
		dialog.push_back(std::make_unique<UiArtText>(status.c_str(), rect(436, 30), UiFlags::FontSize12 | UiFlags::AlignCenter | UiFlags::ColorUiSilver));
		SurvivorsCampSelection = -1;
		UiInitList(nullptr, SelectSurvivorsCamp, LeaveSurvivorsCamp, dialog, false);
		while (SurvivorsCampSelection < 0) {
			UiClearScreen();
			UiRenderItems(dialog);
			UiPollAndRender();
		}
		const int selected = SurvivorsCampSelection;
		dialog.clear();
		choices.clear();
		ArtBackground = std::nullopt;
		if (selected == 0 || selected == 8 || selected == 9)
			return true;
		if (selected == 7)
			return false;
		if (selected >= 1 && selected <= 4)
			PurchaseSurvivorsPermanentUpgrade(static_cast<SurvivorsPermanentUpgrade>(selected - 1));
		else if (selected == 5)
			SelectSurvivorsDifficulty((profile.selectedDifficulty + 1) % (profile.unlockedDifficulty + 1));
		else if (selected == 6)
			SetSurvivorsAutoMelee(!profile.autoMelee);
	}
}

bool InitMultiPlayerMenu();

bool InitSurvivorsMenu()
{
	LoadSurvivorsProfile();
	while (ShowSurvivorsCamp()) {
		// Keep the mode active through shutdown so campaign and stash stay separate.
		SetSurvivorsMode(true);
		SetSurvivorsCampShop(SurvivorsCampSelection == 9);
		const bool success = SurvivorsCampSelection == 8 ? InitMultiPlayerMenu() : InitSinglePlayerMenu();
		SetSurvivorsMode(false);
		if (!success)
			return false;
	}
	return true;
}

bool InitMultiPlayerMenu()
{
#ifndef _DEBUG
	if (IsAssetIntegrityViolated || HasLooseLogicAssets()) {
		UiSelOkDialog(_("Multi Player Game").data(), _("Cannot play Multiplayer with overridden *.lua, *.tsv, or *.sol assets.").data(), false);
		return true;
	}
#endif
	gbIsMultiplayer = true;
	return InitMenu(SELHERO_CONNECT);
}

void PlayIntro()
{
	music_stop();
	if (gbIsHellfire)
		play_movie("gendata\\Hellfire.smk", true);
	else
		play_movie("gendata\\diablo1.smk", true);
	RefreshMusic();
}

bool SurvivorsHeroInfo(bool (* /*addHero*/)(_uiheroinfo *))
{
	// An empty list opens class selection directly; campaign heroes stay separate.
	return true;
}

HeroClass SelectedSurvivorsClass = HeroClass::Warrior;
std::string SelectedSurvivorsName;

bool CreateSurvivorsHero(_uiheroinfo *heroInfo)
{
	SelectedSurvivorsClass = heroInfo->heroclass;
	SelectedSurvivorsName = heroInfo->name;
	CreatePlayer(Players[0], heroInfo->heroclass);
	CopyUtf8(Players[0]._pName, heroInfo->name, PlayerNameLength);
	gbValidSaveFile = false;
	return true;
}

bool DeleteSurvivorsHero(_uiheroinfo * /*heroInfo*/)
{
	return true;
}

bool DummyGetHeroInfo(_uiheroinfo * /*pInfo*/)
{
	return true;
}

} // namespace

void mainmenu_initialize_survivors_hero(Player &player)
{
	if (!IsSurvivorsMode() || SelectedSurvivorsName.empty()) return;
	CreatePlayer(player, SelectedSurvivorsClass);
	CopyUtf8(player._pName, SelectedSurvivorsName, PlayerNameLength);
}

bool mainmenu_select_hero_dialog(GameData *gameData)
{
	OptionEntryInt<uint32_t> *pSaveNumberFromOptions = nullptr;
	_selhero_selections dlgresult = SELHERO_NEW_DUNGEON;
	if (IsSurvivorsMode()) {
		// Use the normal class/name UI with entirely in-memory hero callbacks.
		if (gbIsMultiplayer) {
			UiSelHeroMultDialog(SurvivorsHeroInfo, CreateSurvivorsHero, DeleteSurvivorsHero,
			    pfile_ui_set_class_stats, &dlgresult, &gSaveNumber);
		} else {
			UiSelHeroSingDialog(SurvivorsHeroInfo, CreateSurvivorsHero, DeleteSurvivorsHero,
			    pfile_ui_set_class_stats, &dlgresult, &gSaveNumber, &gameData->nDifficulty);
		}
		// The camp controls unlocks and the director applies difficulty scaling.
		gameData->nDifficulty = DIFF_NORMAL;
		gbLoadGame = false;
	} else if (demo::IsRunning()) {
		pfile_ui_set_hero_infos(DummyGetHeroInfo);
		gbLoadGame = true;
	} else if (host::SelectAutoStartHero(gameData)) {
		// Embedded with --host-hero: the host picked (or created) the hero.
	} else if (!gbIsMultiplayer) {
		pSaveNumberFromOptions = gbIsHellfire ? &GetOptions().Hellfire.lastSinglePlayerHero : &GetOptions().Diablo.lastSinglePlayerHero;
		gSaveNumber = **pSaveNumberFromOptions;
		UiSelHeroSingDialog(
		    pfile_ui_set_hero_infos,
		    pfile_ui_save_create,
		    pfile_delete_save,
		    pfile_ui_set_class_stats,
		    &dlgresult,
		    &gSaveNumber,
		    &gameData->nDifficulty
#ifdef DAMNATION_S1_BRIDGE
		    ,
		    s1_bridge::HeroTick
#endif
		);

		gbLoadGame = (dlgresult == SELHERO_CONTINUE);
	} else {
		pSaveNumberFromOptions = gbIsHellfire ? &GetOptions().Hellfire.lastMultiplayerHero : &GetOptions().Diablo.lastMultiplayerHero;
		gSaveNumber = **pSaveNumberFromOptions;
		UiSelHeroMultDialog(
		    pfile_ui_set_hero_infos,
		    pfile_ui_save_create,
		    pfile_delete_save,
		    pfile_ui_set_class_stats,
		    &dlgresult,
		    &gSaveNumber);
	}
	if (dlgresult == SELHERO_PREVIOUS) {
		return false;
	}

	if (pSaveNumberFromOptions != nullptr)
		pSaveNumberFromOptions->SetValue(gSaveNumber);
	// Expose the committed start to the host so its loading card cannot appear
	// while ordinary native screens (Settings, credits, confirmations) are open.
	host::SetFrontendStage(7);

	return true;
}

void mainmenu_wait_for_button_sound()
{
	SDL_FillSurfaceRect(DiabloUiSurface(), nullptr, 0);
	UiFadeIn();
	SDL_Delay(350); // delay to let button pressed sound finish playing
}

void mainmenu_loop()
{
	bool done;

	RefreshMusic();
	done = false;

	do {
#ifdef DEVILUTIONX_D2_SOUND_IMPORT
		ApplyPendingSoundPack();
#endif
		_mainmenu_selections menu = MAINMENU_NONE;
		if (demo::IsRunning() || host::TakeAutoStart())
			menu = MAINMENU_SINGLE_PLAYER;
#ifdef DEVILUTIONX_LOBBY
		else if (static bool deepLinkAttempted = false; !deepLinkAttempted && !net::lobby::DeepLinkGameId().empty()) {
			deepLinkAttempted = true;
			menu = MAINMENU_MULTIPLAYER;
		}
#endif
		else if (!UiMainMenuDialog(gszProductName, &menu, 30
#ifdef DAMNATION_S1_BRIDGE
		             ,
		             s1_bridge::MenuTick
#endif
		             ))
			app_fatal(_("Unable to display mainmenu"));

		switch (menu) {
		case MAINMENU_NONE:
			break;
		case MAINMENU_SINGLE_PLAYER:
			if (!InitSinglePlayerMenu())
				done = true;
			break;
		case MAINMENU_SURVIVORS:
			if (!InitSurvivorsMenu())
				done = true;
			break;
		case MAINMENU_MULTIPLAYER:
			if (!InitMultiPlayerMenu())
				done = true;
			break;
		case MAINMENU_ATTRACT_MODE:
			if (gbIsSpawn && !HaveIntro())
				done = false;
			else if (gbActive)
				PlayIntro();
			break;
		case MAINMENU_SHOW_CREDITS:
			UiCreditsDialog();
			break;
		case MAINMENU_SHOW_SUPPORT:
			UiSupportDialog();
			break;
		case MAINMENU_EXIT_DIABLO:
			mainmenu_wait_for_button_sound();
			done = true;
			break;
		case MAINMENU_SETTINGS:
			UiSettingsMenu();
			break;
		}
	} while (!done);

	music_stop();
}

} // namespace devilution
