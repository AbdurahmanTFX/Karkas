#include "hpp/camera.hpp"

void GameCamera::Follow(Vec2 target, float dt) {
    pos_.x += (target.x - pos_.x) * dt;
    pos_.y += (target.y - pos_.y) * dt;
    Clamp();
}

Vec2 GameCamera::ScreenToWorld(Vec2 screen) const { return Vec2{screen.x / zoom_ + pos_.x, screen.y / zoom_ + pos_.y}; }

void GameCamera::Clamp() { pos_.x = clampf(pos_.x, 0.0f, 1000.0f); }
