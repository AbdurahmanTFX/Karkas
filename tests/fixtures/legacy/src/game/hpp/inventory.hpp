#pragma once
#include <string>
#include <vector>

// Items the player carries.
struct Item {
    std::string name;
    int count = 0;
};

struct Inventory {
    std::vector<Item> items;
};
