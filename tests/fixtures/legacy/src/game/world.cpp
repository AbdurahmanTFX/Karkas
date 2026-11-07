#include "hpp/world.hpp"

void World::Update(float dt) { time_ += dt; Helper(); }
bool World::IsWalkable(Vec2 p) const { return clampf(p.x, 0.0f, 100.0f) == p.x; }
void World::Draw() const {}
void World::Helper() {}
