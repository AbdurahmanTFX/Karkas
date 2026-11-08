#include <doctest/doctest.h>

#include "kk/code/init.hpp"
#include "kk/core/config.hpp"
#include "kk/core/schema.hpp"
#include "kk/core/text.hpp"
#include "kk/test/support.hpp"

using namespace kk;
namespace fs = std::filesystem;

TEST_CASE("init: project ids from folder names") {
  CHECK(code::project_id_from("Flappy-Birds") == "flappy_birds");
  CHECK(code::project_id_from("wavkit") == "wavkit");
  CHECK(code::project_id_from("2048") == "");
  CHECK(code::project_id_from("") == "");
}

TEST_CASE("init: a new folder gets the project frame") {
  test::TempDir tmp;
  auto root = tmp.path() / "My Game";
  auto r = code::init_project(root, "a tiny arcade game");
  REQUIRE_MESSAGE(r, (r ? "" : r.error().str()));
  CHECK(r->name == "my_game");
  CHECK_FALSE(r->existing_code);
  CHECK(r->written == std::vector<std::string>{"project.ksl", "karkas.toml", "cmake/user.cmake", ".gitignore"});
  CHECK(fs::is_directory(root / "blocks"));
  CHECK(fs::is_directory(root / ".karkas"));
  auto s = core::load_schema(root);
  REQUIRE_MESSAGE(s, (s ? "" : s.error().str()));
  CHECK(s->project.name == "my_game");
  CHECK(s->project.idea.value == "a tiny arcade game");
  CHECK(core::load_config(root));
  // A second init in the same folder is refused.
  CHECK_FALSE(code::init_project(root));
}

TEST_CASE("init: a folder with code keeps its standard and gets no rules") {
  test::TempDir tmp;
  auto root = tmp.path() / "game";
  fs::create_directories(root / "src");
  test::write(root / "src/main.cpp", "int main() { return 0; }\n");
  test::write(root / "CMakeLists.txt", "set(CMAKE_CXX_STANDARD 17)\n");
  auto r = code::init_project(root);
  REQUIRE(r);
  CHECK(r->existing_code);
  auto text = test::read(root / "project.ksl");
  CHECK(core::contains(text, "std     c++17"));
  CHECK_FALSE(core::contains(text, "rule"));
  CHECK(test::read(root / "src/main.cpp") == "int main() { return 0; }\n");
}
