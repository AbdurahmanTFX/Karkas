#pragma once
// Files dropped on the window (docs/adr/0041): GLFW's drop callback (and the script's "drop <path>") put
// them here, the UI takes them at its next frame.
#include <string>
#include <vector>

namespace kk::gui {

void push_dropped(std::vector<std::string> paths);
std::vector<std::string> take_dropped();

}  // namespace kk::gui
