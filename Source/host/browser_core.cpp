// Browser C ABI over the same real-core contract used by the native host.
#include "host/host_api.hpp"
#include "host/host_menu.hpp"
#include "options.h"
#include <emscripten/emscripten.h>
using namespace devilution::host;
// Emscripten's shared heap can grow on the simulation worker. Refresh its
// main-thread views before copying a completed snapshot into the Rust heap.
EM_JS(void, RefreshBrowserHeapViews, (), { growMemViews(); });
static_assert(sizeof(HostFrame) == 116);
static_assert(sizeof(HostViewControl) == 24);
static_assert(sizeof(HostCameraSettings) == 20);
static_assert(sizeof(HostVrSettings) == 44);
static_assert(sizeof(HostViewFeedback) == 12);
static_assert(sizeof(HostInitParams) == 20);
static_assert(sizeof(HostTickResult) == 8);
static_assert(sizeof(HostHudState) == 212);
static_assert(sizeof(HostHdFrame) == 132);
static_assert(sizeof(HostWorldFrame) == 56);
static_assert(sizeof(HostEntityArt) == 60);
static_assert(sizeof(HostEntityHdArt) == 12);
static_assert(sizeof(HostView2DFrame) == 60);
namespace {
bool BrowserInitResult = false;
HostTickResult BrowserTickResult {};
}
extern "C" {
EMSCRIPTEN_KEEPALIVE void dx_host_set_view_control(const HostViewControl *control) { HostSetViewControl(*control); }
EMSCRIPTEN_KEEPALIVE const HostCameraSettings *dx_host_camera_settings_snapshot() { static HostCameraSettings value; value = HostCameraSettingsSnapshot(); return &value; }
EMSCRIPTEN_KEEPALIVE const HostVrSettings *dx_host_vr_settings_snapshot() { static HostVrSettings value; value = HostVrSettingsSnapshot(); return &value; }
EMSCRIPTEN_KEEPALIVE const HostViewFeedback *dx_host_view_feedback_snapshot() { static HostViewFeedback value; value = HostViewFeedbackSnapshot(); return &value; }
EMSCRIPTEN_KEEPALIVE void dx_host_set_vr_enabled(bool enabled) { devilution::GetOptions().Vr.enabled.SetValue(enabled); }
EMSCRIPTEN_KEEPALIVE void dx_host_set_vr_support(bool available) { HostSetVrSupport(available); }
EMSCRIPTEN_KEEPALIVE uint32_t dx_host_bound_key(uint32_t action) { return HostBoundKey(action); }
EMSCRIPTEN_KEEPALIVE void dx_host_set_chase_settings(float distance, uint32_t style) { HostSetChaseSettings(distance, style); }
EMSCRIPTEN_KEEPALIVE void dx_host_set_camera_settings(const HostCameraSettings *settings) { HostSetCameraSettings(*settings); }
EMSCRIPTEN_KEEPALIVE void dx_host_set_camera_mode(uint32_t mode) { HostSetCameraMode(mode); }

EMSCRIPTEN_KEEPALIVE void dx_host_refresh_heap() { RefreshBrowserHeapViews(); }
EMSCRIPTEN_KEEPALIVE const HostHudState * dx_host_hud_state() { static HostHudState value; value = HostHudStateSnapshot(); return &value; }
EMSCRIPTEN_KEEPALIVE uint32_t dx_host_hover_info(uint8_t * text, uint32_t capacity) { return HostHoverInfo(text, capacity); }
EMSCRIPTEN_KEEPALIVE void dx_host_set_hud_visible(bool visible) { HostSetHudVisible(visible); }
EMSCRIPTEN_KEEPALIVE bool dx_host_toggle_run() { return HostToggleRun(); }
EMSCRIPTEN_KEEPALIVE void dx_host_set_hero_voice_d2(bool d2) { HostSetHeroVoiceD2(d2); }
EMSCRIPTEN_KEEPALIVE bool dx_host_init(const HostInitParams * params) { BrowserInitResult = HostInit(*params); return BrowserInitResult; }
EMSCRIPTEN_KEEPALIVE int32_t dx_host_import_d2_sounds(const char * install, const char * output, const char * maps) { return HostImportD2Sounds(install, output, maps); }
EMSCRIPTEN_KEEPALIVE void dx_host_submit_input(const HostInputFrame * input) { HostSubmitInput(*input); }
EMSCRIPTEN_KEEPALIVE const HostTickResult * dx_host_tick(uint32_t elapsed_ms) { BrowserTickResult = HostTick(elapsed_ms); return &BrowserTickResult; }
EMSCRIPTEN_KEEPALIVE const HostFrame * dx_host_snapshot() { return &HostSnapshot(); }
// Completed results remain addressable across the two independent WASM heaps.
// The thread rendezvous finishes before these getters are read by the frontend.
EMSCRIPTEN_KEEPALIVE bool dx_host_init_result() { return BrowserInitResult; }
EMSCRIPTEN_KEEPALIVE const HostTickResult * dx_host_tick_result() { return &BrowserTickResult; }
EMSCRIPTEN_KEEPALIVE void dx_host_shutdown() { HostShutdown(); }
EMSCRIPTEN_KEEPALIVE const char * dx_host_sound_path(uint32_t sound_id) { return HostSoundPath(sound_id); }
EMSCRIPTEN_KEEPALIVE uint32_t dx_host_read_asset(const char * path, uint8_t * buffer, uint32_t capacity) { return HostReadAsset(path, buffer, capacity); }
EMSCRIPTEN_KEEPALIVE void dx_host_set_external_door_models(uint32_t mask) { HostSetExternalDoorModels(mask); }
EMSCRIPTEN_KEEPALIVE void dx_host_set_external_door_aperture(uint32_t family, uint32_t width, uint32_t height) { HostSetExternalDoorAperture(family, width, height); }
EMSCRIPTEN_KEEPALIVE void dx_host_set_render_mode(HostRenderMode mode) { HostSetRenderMode(mode); }
EMSCRIPTEN_KEEPALIVE HostRenderMode dx_host_get_render_mode() { return HostGetRenderMode(); }
EMSCRIPTEN_KEEPALIVE uint32_t dx_host_installed_packs() { return HostInstalledPacks(); }
EMSCRIPTEN_KEEPALIVE const char * dx_host_hd_directory() { return HostHdDirectory(); }
EMSCRIPTEN_KEEPALIVE const HostHdFrame * dx_host_hd_snapshot() { return &HostHdSnapshot(); }
EMSCRIPTEN_KEEPALIVE const HostWorldFrame * dx_host_world_snapshot() { return &HostWorldSnapshot(); }
EMSCRIPTEN_KEEPALIVE const HostEntityArt * dx_host_entity_art_snapshot() { return &HostEntityArtSnapshot(); }
EMSCRIPTEN_KEEPALIVE const HostEntityHdArt * dx_host_entity_hd_art_snapshot() { return &HostEntityHdArtSnapshot(); }
EMSCRIPTEN_KEEPALIVE const HostUiState * dx_host_ui_state_snapshot() { return &HostUiStateSnapshot(); }
EMSCRIPTEN_KEEPALIVE const HostView2DFrame * dx_host_view2d_snapshot() { return &HostView2DSnapshot(); }
EMSCRIPTEN_KEEPALIVE void dx_host_set_world_view(const float * screen_from_world) { HostSetWorldView(screen_from_world); }
EMSCRIPTEN_KEEPALIVE const HostAudioState * dx_host_audio_snapshot() { return &HostAudioSnapshot(); }
EMSCRIPTEN_KEEPALIVE uint32_t dx_host_save_state(uint8_t * buffer, uint32_t capacity) { return HostSaveState(buffer, capacity); }
EMSCRIPTEN_KEEPALIVE bool dx_host_load_state(const uint8_t * data, uint32_t size) { return HostLoadState(data, size); }
EMSCRIPTEN_KEEPALIVE const char * dx_host_hero_name() { return HostHeroName(); }
EMSCRIPTEN_KEEPALIVE const char * dx_host_take_clipboard_text() { return HostTakeClipboardText(); }
EMSCRIPTEN_KEEPALIVE const char * dx_host_player_equipment_name(uint32_t entity_id, uint32_t slot) { return HostPlayerEquipmentName(entity_id, slot); }
EMSCRIPTEN_KEEPALIVE uint32_t dx_host_reachable_object(uint32_t type_id) { return HostReachableObject(type_id); }
}
