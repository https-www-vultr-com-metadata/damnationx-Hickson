#pragma once

#include <cstdint>
#include <string_view>

// Copied values only. The game and embedding host take turns; queued commands
// are applied on the game thread, and a menu revision rejects stale clicks.
struct DxpFrontendEntry {
	char label[160];
	int32_t value;
	uint32_t enabled;
};
struct DxpFrontendSnapshot {
	uint32_t version;
	uint32_t stage; // 0 inactive, 1 main, 2 heroes, 3 classes, 4 lobby, 5 camp, 6 native dialog, 7 game launch
	uint32_t revision;
	uint32_t selected;
	uint32_t heroClass;
	uint32_t heroLevel;
	uint32_t count;
	char heroName[64];
	char title[160];
	char subtitle[512];
	DxpFrontendEntry entries[64];
};

namespace devilution::host {
#ifdef DAMNATIONX_HOST_LIB
uint32_t SetFrontendStage(uint32_t stage);
void SetFrontendHero(uint32_t heroClass, uint32_t level, std::string_view name);
#else
inline uint32_t SetFrontendStage(uint32_t) { return 0; }
inline void SetFrontendHero(uint32_t, uint32_t, std::string_view) { }
#endif
struct FrontendScope {
	uint32_t previous;
	explicit FrontendScope(uint32_t stage) : previous(SetFrontendStage(stage)) { }
	~FrontendScope() { SetFrontendStage(previous); }
};
} // namespace devilution::host

extern "C" {
bool dxp_frontend_snapshot(DxpFrontendSnapshot *output, uint32_t size);
bool dxp_frontend_command(uint32_t revision, uint32_t action, uint32_t index);
}

static_assert(sizeof(DxpFrontendEntry) == 168);
static_assert(sizeof(DxpFrontendSnapshot) == 11516);
