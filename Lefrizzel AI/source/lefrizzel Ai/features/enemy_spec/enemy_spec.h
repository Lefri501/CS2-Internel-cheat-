#pragma once

// Client-side enemy spectate while dead (comp/MM normally blocks enemy cams).
// Forces CPlayer_ObserverServices target + mode each frame.

namespace EnemySpec {

bool Init();
// Call when dead — FRAME_RENDER_START preferred (camera sample).
void OnFrame();

} // namespace EnemySpec
