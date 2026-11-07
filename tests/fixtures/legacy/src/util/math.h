#pragma once
/* Small math helpers shared by the game. */
typedef struct {
  float x, y;
} Vec2;

float clampf(float v, float lo, float hi);
