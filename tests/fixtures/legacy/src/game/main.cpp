#include "hpp/player.hpp"
#include "hpp/world.hpp"
#include "../util/hpp/camera.hpp"

int main() {
    World world;
    Player player;
    GameCamera camera;
    for (int i = 0; i < 3; ++i) {
        world.Update(0.016f);
        player.Update(0.016f, world);
        camera.Follow(player.Position(), 0.016f);
        world.Draw();
    }
    return static_cast<int>(player.Bag().items.size());
}
