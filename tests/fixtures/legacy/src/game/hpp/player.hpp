#pragma once
#include "../../util/math.h"
#include "inventory.hpp"

class World;

// The player: walks on walkable tiles and carries an inventory.
class Player {
public:
    void Update(float dt, const World& world);
    Vec2 Position() const { return pos_; }
    const Inventory& Bag() const { return bag_; }

private:
    Vec2 pos_{0, 0};
    Inventory bag_;
};
