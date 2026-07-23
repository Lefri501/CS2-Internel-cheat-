#pragma once

// Enemy walk/run ground rings (Present).
// CS2 serverside footsteps — no client footstep hook for enemies.
// Detects FL_ONGROUND + horizontal speed, draws expanding floor rings.

namespace SoundEsp {

void Install();
void OnGameEvent(void* gameEvent); // no-op
void Draw();
// Map unload — drop rings / track handles
void Clear();

} // namespace SoundEsp
