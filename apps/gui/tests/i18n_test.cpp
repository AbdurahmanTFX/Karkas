// Every interface text of karkas-gui has a Russian translation; settings round-trip.
#include <doctest/doctest.h>

#include <cstdlib>
#include <regex>
#include <string>

#include "i18n.hpp"
#include "kk/core/paths.hpp"
#include "kk/test/support.hpp"
#include "settings.hpp"

using namespace kk::gui;

TEST_CASE("every tr()/title() text in the GUI sources has a Russian translation") {
  set_lang(Lang::Ru);
  std::regex call(R"re(\b(?:tr|title)\("((?:[^"\\]|\\.)*)"\))re");
  int checked = 0;
  for (const char* f : {"ui.cpp", "worker.cpp"}) {
    auto text = kk::core::read_file(std::string(KK_GUI_SRC) + "/" + f);
    REQUIRE(text);
    for (std::sregex_iterator it(text->begin(), text->end(), call), end; it != end; ++it) {
      std::string en = (*it)[1].str();
      INFO(f << ": " << en);
      CHECK(std::string(tr(en.c_str())) != en);
      ++checked;
    }
  }
  CHECK(checked > 60);
  set_lang(Lang::En);
  CHECK(std::string(tr("Settings")) == "Settings");
}

TEST_CASE("window titles keep a language-independent id") {
  set_lang(Lang::Ru);
  CHECK(title("Blocks") == "Блоки###Blocks");
  CHECK(std::string(tr_status("passing")) == "проходит");
  set_lang(Lang::En);
  CHECK(title("Blocks") == "Blocks###Blocks");
  CHECK(std::string(tr_kind("leaf")) == "leaf");
}

TEST_CASE("language codes") {
  CHECK(lang_from_code("ru_RU.UTF-8") == Lang::Ru);
  CHECK(lang_from_code("en_US.UTF-8") == Lang::En);
  CHECK(lang_from_code("C") == Lang::En);
}

TEST_CASE("settings are saved and loaded, sizes clamped") {
  kk::test::TempDir dir;
  setenv("XDG_CONFIG_HOME", dir.path().c_str(), 1);
  GuiSettings s;
  s.lang = Lang::Ru;
  s.block_scale = 1.75f;
  REQUIRE(save_settings(s));
  CHECK(settings_path() == dir.path() / "karkas" / "gui.json");
  auto l = load_settings();
  CHECK(l.lang == Lang::Ru);
  CHECK(l.block_scale == doctest::Approx(1.75f));
  REQUIRE(kk::core::write_file(settings_path(), R"({"language":"en","block_scale":0.01})"));
  l = load_settings();
  CHECK(l.lang == Lang::En);
  CHECK(l.block_scale == doctest::Approx(kMinBlockScale));  // never shrinks to a dot
  unsetenv("XDG_CONFIG_HOME");
}
