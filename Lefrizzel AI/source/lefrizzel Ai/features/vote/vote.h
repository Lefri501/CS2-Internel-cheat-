#pragma once

// Vote reveal + auto-vote (Misc).
// Events: vote_cast / vote_changed via FireEventClientSide.
// Auto-vote: engine ExecuteStringCommand "vote option1/2".

namespace Vote {

void Install();
void OnGameEvent(void* gameEvent);
void OnFrame(); // delayed auto-vote

} // namespace Vote
