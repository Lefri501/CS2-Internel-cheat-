#pragma once

// Celerity fog_handler: spawn env_gradient_fog, drive C_GradientFog schema fields.
// CreateEntityByClassName IDA-verified @ client 0x1814E9A40.

namespace World {
namespace Fog {
	void Update();       // call from World::Update / FSN
	void Shutdown();     // LevelInit / unload — disable + drop ptr
}
}
