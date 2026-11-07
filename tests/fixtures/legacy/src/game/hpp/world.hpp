#pragma once
#include "../../util/math.h"

class World {
public:
    void Update(float dt);
    bool IsWalkable(Vec2 p) const;
    void Draw() const;

private:
    void Helper();
    float time_ = 0;
};
