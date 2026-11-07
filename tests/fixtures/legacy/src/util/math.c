#include "math.h"

float clampf(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }
