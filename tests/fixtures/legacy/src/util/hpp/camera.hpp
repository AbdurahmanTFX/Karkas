#pragma once
#include "../math.h"

// Camera wrapper: smooth follow and world bounds. Second sentence is not the purpose.
class GameCamera {
public:
    void Follow(Vec2 target, float dt);  // smooth follow
    Vec2 ScreenToWorld(Vec2 screen) const;
    float Zoom() const { return zoom_; }

private:
    void Clamp();
    Vec2 pos_{0, 0};
    float zoom_ = 1.0f;
};
