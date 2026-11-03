#pragma once

#include <algorithm>

#include "items.h"

namespace devilution {

// Replica clients do not run ProcessItems. Rebind their ground-item animation
// to the authoritative frame without leaving a pending local simulation tick.
inline void AsyncNetRestoreItemAnimation(Item &item, int frame, bool animated)
{
	// Do not call SetupItem: it also clears the identification bit that was
	// just restored from the authority's packed item.
	item.setNewAnimation(animated);
	auto &animation = item.AnimInfo;
	const auto sprites = animation.sprites;
	const auto frames = std::max<int8_t>(1, animation.numberOfFrames);
	const auto ticks = std::max<int8_t>(1, animation.ticksPerFrame);
	animation.setNewAnimation(sprites, frames, ticks);
	animation.currentFrame = static_cast<int8_t>(std::clamp(frame, 0, static_cast<int>(frames) - 1));
}

} // namespace devilution
