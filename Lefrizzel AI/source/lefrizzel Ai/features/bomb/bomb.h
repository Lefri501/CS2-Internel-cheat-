#pragma once

#include "../../utils/math/vector/vector.h"

// Planted C4 list + bombsite centers from client patterns (pPlantedC4s / GetBombsite*).

namespace Bomb {

bool Init();

// Refresh site centers (map load + each round). Never clears a good cache on fail.
void RefreshSites();

bool GetSiteCenter(int site /*0=A,1=B*/, Vector_t& out);
// Classify world pos by nearest site center; -1 if unknown.
int ClassifySite(const Vector_t& pos);

// First ticking planted C4 entity pointer (may be null).
void* PlantedC4Entity();
int PlantedC4Count();

// Optional: invoke game StartDefuse on planted C4 (CT, in range).
bool TryStartDefuse(void* plantedC4, void* localPawn);

} // namespace Bomb
