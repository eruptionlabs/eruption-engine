#pragma once

namespace eruption {

// Concept-like documentation. Any type passed to Engine::run() must provide:
//   void onInit(Engine&)
//   void onUpdate(float dt)
//   void onRender(Engine&)
//   void onShutdown()
// No inheritance required — compile-time duck typing via template.

} // namespace eruption
