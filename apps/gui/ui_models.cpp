// Models and APIs per level of the block tree and per block (ADR 0011): the settings page,
// the model chip of the schema toolbar and the model section of the inspector. Everything is
// written to <project>/karkas.toml ([models.*], [levels], [blocks], [roles]).
#include <fmt/format.h>
#include <imgui.h>

#include <algorithm>
#include <cstdlib>
#include <cstring>

#include "i18n.hpp"
#include "kk/core/text.hpp"
#include "theme.hpp"
#include "ui.hpp"
#include "widgets.hpp"

namespace kk::gui {
namespace {

const char* const kProviders[] = {"anthropic", "openai_compat"};
const char* const kClaudeModels[] = {"claude-opus-5-5", "claude-sonnet-5-5", "claude-haiku-5-5", "claude-fable-5-1"};
const char* const kRoles[] = {"orchestrator", "architect", "navigator", "worker", "tester", "composer", "ask"};

const char* provider_name(const std::string& p) {
  return p == "openai_compat" ? tr("OpenAI-compatible (OpenRouter, DeepSeek, Ollama, LM Studio...)") : tr("Anthropic (Claude)");
}

bool valid_tier_name(const std::string& n) {
  return !n.empty() && n.size() < 32 && std::all_of(n.begin(), n.end(), [](char c) {
    return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '-';
  });
}

void copy_to(char* buf, std::size_t size, const std::string& s) {
  std::snprintf(buf, size, "%s", s.c_str());
}

// "level 2" / "block game" (Pin::by) in the interface language.
std::string by_text(const std::string& by) {
  if (by.starts_with("level ")) return fmt::format(fmt::runtime(tr("level {}")), by.substr(6));
  if (by.starts_with("block ")) return fmt::format(fmt::runtime(tr("block {}")), by.substr(6));
  return by;
}

}  // namespace

void Ui::reload_models() {
  auto c = core::load_config(root_);
  if (c) {
    cfg_ = std::move(*c);
    cfg_error_.clear();
  } else {
    cfg_error_ = c.error().str();
  }
}

std::string Ui::tier_label(const std::string& tier) const {
  const auto* t = cfg_.tier(tier);
  if (t == nullptr) return tier + " (" + tr("not defined") + ")";
  std::string s = tier + "  ·  " + t->model;
  if (!t->base_url.empty()) {
    auto host = t->base_url;
    if (auto p = host.find("://"); p != std::string::npos) host = host.substr(p + 3);
    if (auto p = host.find('/'); p != std::string::npos) host = host.substr(0, p);
    s += "  @ " + host;
  }
  return s;
}

bool Ui::tier_combo(const char* id, const std::string& current, const char* none_label, std::string* out) {
  bool changed = false;
  std::string preview = current.empty() ? none_label : tier_label(current);
  if (ImGui::BeginCombo(id, preview.c_str())) {
    if (ImGui::Selectable(none_label, current.empty())) {
      *out = "";
      changed = !current.empty();
    }
    ImGui::Separator();
    for (const auto& [name, t] : cfg_.models) {
      if (ImGui::Selectable(tier_label(name).c_str(), current == name)) {
        *out = name;
        changed = current != name;
      }
    }
    ImGui::EndCombo();
  }
  return changed;
}

void Ui::set_pin(const std::string& section, const std::string& key, const std::string& tier) {
  auto r = core::write_config_value(root_, section, key, tier.empty() ? std::nullopt : std::optional<std::string>(tier));
  reload_models();
  // Still pinned after erasing it from the project: the user file pins it; override with "".
  if (r && tier.empty() && section != "roles") {
    bool still = section == "levels" ? cfg_.levels.contains(std::atoi(key.c_str())) : cfg_.block_tiers.contains(key);
    if (still) {
      r = core::write_config_value(root_, section, key, std::string());
      reload_models();
    }
  }
  models_msg_ = r ? fmt::format(fmt::runtime(tr("saved to {}")), (root_ / "karkas.toml").string()) : r.error().message;
  w_.reload_config();
}

std::string Ui::effective_model(const Model& m, const std::string& block) const {
  auto p = cfg_.pin(block);
  if (!p.tier.empty()) return fmt::format(fmt::runtime(tr("{} (pinned: {})")), p.tier, by_text(p.by));
  std::string role = "orchestrator";
  if (!block.empty() && block != "root") {
    const auto* b = m.snap->block(block);
    role = b != nullptr && b->kind == "group" ? "navigator" : "worker";
  }
  auto it = cfg_.roles.find(role);
  std::string t = it == cfg_.roles.end() ? "auto" : it->second;
  if (t == "auto") return fmt::format(fmt::runtime(tr("by size of the task (role {})")), role);
  return fmt::format(fmt::runtime(tr("{} (role {})")), t, role);
}

// --- toolbar chip -------------------------------------------------------------------------------

void Ui::draw_model_chip(const Model& m) {
  int level = core::block_level(req_.scope) + 1;
  bool sel = !req_.selected.empty() && m.snap->block(req_.selected) != nullptr;
  std::string cur;
  if (sel) {
    auto p = cfg_.pin(req_.selected);
    cur = fmt::format("{}: {}", core::last_segment(req_.selected), p.tier.empty() ? tr("auto") : p.tier.c_str());
  } else {
    auto it = cfg_.levels.find(level);
    cur = fmt::format(fmt::runtime(tr("level {}: {}")), level, it == cfg_.levels.end() ? tr("by role") : it->second.c_str());
  }
  if (ui::icon_button("tb_model", Icon::Agent, tr("Model and API for this level or the selected block"), false, cur.c_str())) {
    ImGui::OpenPopup("model_popup");
  }
  if (!ImGui::BeginPopup("model_popup")) return;
  ImGui::PushItemWidth(ImGui::GetFontSize() * 22);
  ui::panel_title(tr("MODEL AND API"));
  ImGui::Spacing();
  ImGui::TextUnformatted(fmt::format(fmt::runtime(tr("Level {}: every block at this depth")), level).c_str());
  auto lit = cfg_.levels.find(level);
  std::string pick;
  if (tier_combo("##lvl", lit == cfg_.levels.end() ? "" : lit->second, tr("by role (default)"), &pick)) {
    set_pin("levels", std::to_string(level), pick);
  }
  if (sel) {
    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();
    ImGui::TextUnformatted(fmt::format(fmt::runtime(tr("Block {} and everything inside it")), req_.selected).c_str());
    auto bit = cfg_.block_tiers.find(req_.selected);
    if (tier_combo("##blk", bit == cfg_.block_tiers.end() ? "" : bit->second, tr("as its level"), &pick)) {
      set_pin("blocks", req_.selected, pick);
    }
    ui::muted("%s", fmt::format(fmt::runtime(tr("now: {}")), effective_model(m, req_.selected)).c_str());
  }
  ImGui::Spacing();
  ImGui::Separator();
  if (ImGui::Selectable(with_icon(Icon::Gear, tr("All model settings...")).c_str())) {
    show_settings_ = true;
    settings_page_ = 2;
  }
  ImGui::PopItemWidth();
  ImGui::EndPopup();
}

// --- inspector ---------------------------------------------------------------------------------

void Ui::inspector_model(const Model& m, const std::string& block) {
  if (!ui::section((std::string(tr("MODEL")) + "###model").c_str())) return;
  auto bit = cfg_.block_tiers.find(block);
  std::string pick;
  ImGui::SetNextItemWidth(-1);
  if (tier_combo("##insp_model", bit == cfg_.block_tiers.end() ? "" : bit->second, tr("as its level"), &pick)) {
    set_pin("blocks", block, pick);
  }
  ui::muted("%s", fmt::format(fmt::runtime(tr("now: {}")), effective_model(m, block)).c_str());
}

// --- settings page -----------------------------------------------------------------------------

void Ui::start_api_edit(const std::string& tier) {
  api_edit_ = tier;
  const core::ModelTier* t = tier == "+" ? nullptr : cfg_.tier(tier);
  copy_to(api_name_, sizeof api_name_, tier == "+" ? "" : tier);
  copy_to(api_model_, sizeof api_model_, t ? t->model : "");
  copy_to(api_url_, sizeof api_url_, t ? t->base_url : "");
  copy_to(api_env_, sizeof api_env_, t ? t->api_key_env : "");
  api_provider_ = t != nullptr && t->provider == "openai_compat" ? 1 : 0;
}

void Ui::draw_api_form() {
  bool is_new = api_edit_ == "+";
  ImGui::PushStyleColor(ImGuiCol_ChildBg, theme::col(theme::kEditorBg));
  ImGui::BeginChild("api_form", ImVec2(0, 0), ImGuiChildFlags_AutoResizeY | ImGuiChildFlags_Borders | ImGuiChildFlags_AlwaysUseWindowPadding);
  ImGui::TextUnformatted(is_new ? tr("New API") : fmt::format(fmt::runtime(tr("API \"{}\"")), api_edit_).c_str());
  ImGui::Spacing();
  if (ImGui::BeginTable("api_fields", 2, ImGuiTableFlags_SizingStretchProp)) {
    ImGui::TableSetupColumn("k", ImGuiTableColumnFlags_WidthFixed, ImGui::GetFontSize() * 9);
    ImGui::TableSetupColumn("v", ImGuiTableColumnFlags_WidthStretch);
    auto row = [](const char* k) {
      ImGui::TableNextRow();
      ImGui::TableNextColumn();
      ImGui::AlignTextToFramePadding();
      ui::muted("%s", k);
      ImGui::TableNextColumn();
      ImGui::SetNextItemWidth(-1);
    };
    row(tr("Name"));
    ImGui::BeginDisabled(!is_new);
    ImGui::InputTextWithHint("##api_name", "local, deepseek, openrouter...", api_name_, sizeof api_name_);
    ImGui::EndDisabled();
    row(tr("Provider"));
    if (ImGui::BeginCombo("##api_prov", provider_name(kProviders[api_provider_]))) {
      for (int i = 0; i < 2; i++) {
        if (ImGui::Selectable(provider_name(kProviders[i]), api_provider_ == i)) api_provider_ = i;
      }
      ImGui::EndCombo();
    }
    row(tr("Model"));
    if (api_provider_ == 0) {
      ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - ImGui::GetFrameHeight() - 4);
      ImGui::InputTextWithHint("##api_model", "claude-sonnet-5-5", api_model_, sizeof api_model_);
      ImGui::SameLine(0, 4);
      if (ImGui::BeginCombo("##api_models", "", ImGuiComboFlags_NoPreview | ImGuiComboFlags_PopupAlignLeft)) {
        for (const char* mdl : kClaudeModels) {
          if (ImGui::Selectable(mdl)) copy_to(api_model_, sizeof api_model_, mdl);
        }
        ImGui::EndCombo();
      }
    } else {
      ImGui::InputTextWithHint("##api_model", "deepseek-chat, qwen3-coder...", api_model_, sizeof api_model_);
    }
    row(tr("Address"));
    ImGui::InputTextWithHint("##api_url", api_provider_ == 0 ? tr("empty = https://api.anthropic.com") : "http://localhost:11434/v1",
                             api_url_, sizeof api_url_);
    row(tr("Key variable"));
    std::string def = api_provider_ == 0 ? "ANTHROPIC_API_KEY" : "OPENAI_API_KEY";
    ImGui::InputTextWithHint("##api_env", (tr("empty = ") + def).c_str(), api_env_, sizeof api_env_);
    std::string env = api_env_[0] != '\0' ? api_env_ : def;
    const char* v = std::getenv(env.c_str());
    ImGui::TableNextRow();
    ImGui::TableNextColumn();
    ImGui::TableNextColumn();
    if (v != nullptr && *v != '\0') {
      ImGui::TextColored(rgb(115, 201, 145), "%s", with_icon(Icon::Check, fmt::format(fmt::runtime(tr("{} is set")), env)).c_str());
    } else {
      ImGui::TextColored(rgb(204, 167, 0), "%s",
                         with_icon(Icon::Warning, fmt::format(fmt::runtime(tr("{} is not set in this environment")), env)).c_str());
    }
    ImGui::EndTable();
  }
  ImGui::Spacing();
  std::string name = api_name_;
  bool name_ok = valid_tier_name(name) && (!is_new || cfg_.tier(name) == nullptr);
  bool ok = name_ok && api_model_[0] != '\0';
  ImGui::BeginDisabled(!ok);
  ImGui::PushStyleColor(ImGuiCol_Button, theme::col(theme::kAccent));
  if (ImGui::Button(with_icon(Icon::Check, tr("Save")).c_str())) {
    std::string sec = "models." + name;
    auto put = [&](const char* key, const std::string& val) -> bool {
      auto r = core::write_config_value(root_, sec, key, val.empty() ? std::nullopt : std::optional<std::string>(val));
      if (!r) models_msg_ = r.error().message;
      return static_cast<bool>(r);
    };
    if (put("provider", kProviders[api_provider_]) && put("model", api_model_) && put("base_url", api_url_) &&
        put("api_key_env", api_env_)) {
      models_msg_ = fmt::format(fmt::runtime(tr("saved to {}")), (root_ / "karkas.toml").string());
      api_edit_.clear();
    }
    reload_models();
    w_.reload_config();
  }
  ImGui::PopStyleColor();
  ImGui::EndDisabled();
  ImGui::SameLine();
  if (ImGui::Button(tr("Cancel"))) api_edit_.clear();
  if (!is_new) {
    // An API that levels, blocks or roles still use cannot be removed.
    std::vector<std::string> users;
    for (const auto& [lv, t] : cfg_.levels) {
      if (t == api_edit_) users.push_back(fmt::format(fmt::runtime(tr("level {}")), lv));
    }
    for (const auto& [b, t] : cfg_.block_tiers) {
      if (t == api_edit_) users.push_back(b);
    }
    for (const auto& [r, t] : cfg_.roles) {
      if (t == api_edit_) users.push_back(r);
    }
    ImGui::SameLine();
    ImGui::BeginDisabled(!users.empty());
    ImGui::PushStyleColor(ImGuiCol_Text, rgb(241, 110, 110));
    if (ImGui::Button(with_icon(Icon::Close, tr("Delete")).c_str())) {
      auto r = core::erase_config_section(root_, "models." + api_edit_);
      reload_models();
      if (!r) models_msg_ = r.error().message;
      else if (cfg_.tier(api_edit_) != nullptr) models_msg_ = tr("it is also defined in ~/.config/karkas/config.toml");
      else models_msg_ = fmt::format(fmt::runtime(tr("saved to {}")), (root_ / "karkas.toml").string());
      api_edit_.clear();
      w_.reload_config();
    }
    ImGui::PopStyleColor();
    ImGui::EndDisabled();
    if (!users.empty()) {
      ImGui::SameLine();
      ui::muted("%s", fmt::format(fmt::runtime(tr("used by: {}")), core::join(users, ", ")).c_str());
    }
  }
  if (!name.empty() && !name_ok) {
    ImGui::TextColored(rgb(241, 76, 76), "%s", tr("name: a-z, 0-9, _ or -, not taken"));
  }
  ImGui::EndChild();
  ImGui::PopStyleColor();
}

void Ui::settings_models(const Model* m) {
  if (root_.empty()) {
    ui::muted("%s", tr("Open a project to choose its models."));
    return;
  }
  if (!cfg_error_.empty()) {
    ImGui::TextColored(rgb(241, 76, 76), "%s", cfg_error_.c_str());
    return;
  }
  ImGui::PushTextWrapPos(0);
  ui::muted("%s", tr("Each part of the block tree can work with its own model and API. A block's own choice beats its "
                     "level; a level beats the role. Karkas never stores keys: an API reads its key from an environment "
                     "variable."));
  ImGui::PopTextWrapPos();
  ImGui::Spacing();
  float combo_w = ImGui::GetFontSize() * 20;

  // APIs ([models.*])
  ImGui::SeparatorText(tr("APIs and models"));
  if (ImGui::BeginTable("apis", 4, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_BordersInnerH)) {
    ImGui::TableSetupColumn(tr("Name"));
    ImGui::TableSetupColumn(tr("Provider"));
    ImGui::TableSetupColumn(tr("Model"));
    ImGui::TableSetupColumn(tr("Key"));
    ImGui::TableHeadersRow();
    for (const auto& [name, t] : cfg_.models) {
      ImGui::TableNextRow();
      ImGui::TableNextColumn();
      if (ImGui::Selectable((name + "##api_" + name).c_str(), api_edit_ == name, ImGuiSelectableFlags_SpanAllColumns)) {
        start_api_edit(name);
      }
      ImGui::TableNextColumn();
      ui::muted("%s", t.provider == "openai_compat" ? "OpenAI-compat" : "Anthropic");
      ImGui::TableNextColumn();
      ImGui::TextUnformatted(t.model.c_str());
      if (!t.base_url.empty()) {
        ImGui::SameLine();
        ui::muted("@ %s", t.base_url.c_str());
      }
      ImGui::TableNextColumn();
      const char* v = std::getenv(t.key_env().c_str());
      bool set = v != nullptr && *v != '\0';
      ImGui::TextColored(set ? rgb(115, 201, 145) : rgb(204, 167, 0), "%s", with_icon(set ? Icon::Check : Icon::Warning, t.key_env()).c_str());
    }
    ImGui::EndTable();
  }
  if (api_edit_.empty() && ui::icon_button("api_add", Icon::Plus, tr("Add an API (another provider or model)"), false, tr("Add API"))) {
    start_api_edit("+");
  }
  if (!api_edit_.empty()) draw_api_form();
  ImGui::Spacing();

  // Levels ([levels])
  ImGui::SeparatorText(tr("By level of the tree"));
  int max_level = 1;
  std::map<int, std::vector<std::string>> at_level;
  if (m != nullptr && m->snap) {
    for (const auto& b : m->snap->blocks) {
      int lv = core::block_level(b.id);
      max_level = std::max(max_level, lv);
      at_level[lv].push_back(core::last_segment(b.id));
    }
  }
  for (const auto& [lv, t] : cfg_.levels) max_level = std::max(max_level, lv);
  if (ImGui::BeginTable("levels", 2, ImGuiTableFlags_SizingStretchProp)) {
    ImGui::TableSetupColumn("l", ImGuiTableColumnFlags_WidthStretch);
    ImGui::TableSetupColumn("c", ImGuiTableColumnFlags_WidthFixed, combo_w);
    for (int lv = 0; lv <= max_level; ++lv) {
      ImGui::TableNextRow();
      ImGui::TableNextColumn();
      ImGui::AlignTextToFramePadding();
      ImGui::TextUnformatted(fmt::format(fmt::runtime(tr("Level {}")), lv).c_str());
      ImGui::SameLine();
      std::string what;
      if (lv == 0) {
        what = tr("the project: orchestrator, architect of the top level");
      } else {
        auto& names = at_level[lv];
        std::sort(names.begin(), names.end());
        names.erase(std::unique(names.begin(), names.end()), names.end());
        std::vector<std::string> shown(names.begin(), names.begin() + static_cast<long>(std::min<std::size_t>(names.size(), 5)));
        what = core::join(shown, ", ") + (names.size() > 5 ? fmt::format(" +{}", names.size() - 5) : "");
      }
      ui::muted("%s", what.c_str());
      ImGui::TableNextColumn();
      ImGui::SetNextItemWidth(-1);
      auto it = cfg_.levels.find(lv);
      std::string pick;
      if (tier_combo(("##lv" + std::to_string(lv)).c_str(), it == cfg_.levels.end() ? "" : it->second, tr("by role (default)"), &pick)) {
        set_pin("levels", std::to_string(lv), pick);
      }
    }
    ImGui::EndTable();
  }
  ImGui::Spacing();

  // Blocks ([blocks])
  ImGui::SeparatorText(tr("By block (with everything inside it)"));
  if (cfg_.block_tiers.empty()) {
    ui::muted("%s", tr("No block has its own model. Select a block and choose one in the toolbar or the inspector."));
  } else if (ImGui::BeginTable("blocks", 3, ImGuiTableFlags_SizingStretchProp)) {
    ImGui::TableSetupColumn("b", ImGuiTableColumnFlags_WidthStretch);
    ImGui::TableSetupColumn("c", ImGuiTableColumnFlags_WidthFixed, combo_w);
    ImGui::TableSetupColumn("x", ImGuiTableColumnFlags_WidthFixed, ImGui::GetFrameHeight());
    auto pins = cfg_.block_tiers;  // set_pin reloads cfg_
    for (const auto& [b, t] : pins) {
      ImGui::TableNextRow();
      ImGui::TableNextColumn();
      ImGui::AlignTextToFramePadding();
      bool known = m == nullptr || !m->snap || m->snap->block(b) != nullptr;
      ImGui::TextUnformatted(b.c_str());
      if (!known) {
        ImGui::SameLine();
        ImGui::TextColored(rgb(204, 167, 0), "%s", tr("(no such block)"));
      }
      ImGui::TableNextColumn();
      ImGui::SetNextItemWidth(-1);
      std::string pick;
      if (tier_combo(("##bk" + b).c_str(), t, tr("as its level"), &pick)) set_pin("blocks", b, pick);
      ImGui::TableNextColumn();
      if (ui::icon_button(("bk_x" + b).c_str(), Icon::Close, tr("Remove"))) set_pin("blocks", b, "");
    }
    ImGui::EndTable();
  }
  ImGui::Spacing();

  // Roles ([roles])
  ImGui::SeparatorText(tr("By role (where nothing is pinned)"));
  if (ImGui::BeginTable("roles", 2, ImGuiTableFlags_SizingStretchProp)) {
    ImGui::TableSetupColumn("r", ImGuiTableColumnFlags_WidthStretch);
    ImGui::TableSetupColumn("c", ImGuiTableColumnFlags_WidthFixed, combo_w);
    for (const char* role : kRoles) {
      ImGui::TableNextRow();
      ImGui::TableNextColumn();
      ImGui::AlignTextToFramePadding();
      ImGui::TextUnformatted(role);
      ImGui::TableNextColumn();
      ImGui::SetNextItemWidth(-1);
      auto it = cfg_.roles.find(role);
      std::string cur = it == cfg_.roles.end() || it->second == "auto" ? "" : it->second;
      std::string pick;
      if (tier_combo((std::string("##role_") + role).c_str(), cur, tr("auto (default)"), &pick)) {
        set_pin("roles", role, pick.empty() ? "auto" : pick);
      }
    }
    ImGui::EndTable();
  }
  if (!models_msg_.empty()) {
    ImGui::Spacing();
    ui::muted("%s", models_msg_.c_str());
  }
}

}  // namespace kk::gui
