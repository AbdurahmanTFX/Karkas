#include "hpp/player.hpp"

#include "hpp/world.hpp"

void Player::Update(float dt, const World& world) {
    Vec2 next{pos_.x + dt, pos_.y};
    if (world.IsWalkable(next)) pos_ = next;
}
