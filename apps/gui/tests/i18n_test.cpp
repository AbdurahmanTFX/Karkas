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
  // tr("...") / title("...") with one or more adjacent literals ("a" "b" → "ab").
  std::regex call(R"re(\b(?:tr|title)\(((?:\s*"(?:[^"\\]|\\.)*")+)\s*\))re");
  std::regex literal(R"re("((?:[^"\\]|\\.)*)")re");
  auto unescape = [](const std::string& s) {
    std::string out;
    for (std::size_t i = 0; i < s.size(); ++i) {
      if (s[i] == '\\' && i + 1 < s.size()) ++i;
      out += s[i];
    }
    return out;
  };
  int checked = 0;
  for (const char* f : {"ui.cpp", "ui_shell.cpp", "ui_models.cpp", "ui_panels.cpp", "ui_chat.cpp", "ui_prototype.cpp", "ui_run.cpp", "worker.cpp", "picker.cpp"}) {
    auto text = kk::core::read_file(std::string(KK_GUI_SRC) + "/" + f);
    REQUIRE(text);
    for (std::sregex_iterator it(text->begin(), text->end(), call), end; it != end; ++it) {
      std::string lits = (*it)[1].str();
      std::string en;
      for (std::sregex_iterator l(lits.begin(), lits.end(), literal), le; l != le; ++l) en += unescape((*l)[1].str());
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

TEST_CASE("the layout file loses values that are not numbers (a saved Weight=-nan aborted the next column resize)") {
  std::string ini =
      "[Window][###tile_agents]\nPos=60,60\nSize=400,300\n\n"
      "[Table][0xF5B0B96A,11]\nRefScale=15\nColumn 0  Weight=-nan\nColumn 1  Weight=NaN\nColumn 2  Weight=inf\n"
      "Column 3  Weight=1.2000\nColumn 10 Width=27\n\n[Docking][Data]\nDockSpace ID=0x6B61726B Pos=45,27 Size=1321,716\n";
  auto clean = clean_layout_ini(ini);
  CHECK(clean.find("nan") == std::string::npos);
  CHECK(clean.find("NaN") == std::string::npos);
  CHECK(clean.find("inf") == std::string::npos);
  CHECK(clean.find("Column 3  Weight=1.2000\n") != std::string::npos);
  CHECK(clean.find("Column 10 Width=27\n") != std::string::npos);
  CHECK(clean.find("[Docking][Data]\nDockSpace ID=0x6B61726B Pos=45,27 Size=1321,716\n") != std::string::npos);
  CHECK(clean.find("Pos=60,60\n") != std::string::npos);  // "Pos=" is no "inf": only values are looked at
  CHECK(clean_layout_ini("Window=0xF98418C4 Info=1\n") == "Window=0xF98418C4 Info=1\n");
}
