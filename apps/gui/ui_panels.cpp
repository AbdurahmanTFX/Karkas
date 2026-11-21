// The bottom panel of karkas-gui (14.4): tickets, agents, build & tests, costs, drift.
// Rows can be removed from a panel and a panel cleared — display only, the ticket files,
// run journals, test results and karkas.log stay — and running agents can be stopped
// (docs/adr/0019).
#include <fmt/format.h>
#include <imgui.h>
#include <imgui_internal.h>
#include <misc/cpp/imgui_stdlib.h>

#include <algorithm>
#include <array>
#include <csignal>
#include <map>
#include <set>

#include "i18n.hpp"
#include "icons.hpp"
#include "kk/core/paths.hpp"
#include "kk/core/platform.hpp"
#include "theme.hpp"
#include "ui.hpp"
#include "widgets.hpp"

namespace kk::gui {

namespace {

constexpr double kKillAfterS = 5.0;  // "Kill" appears when a stopped command still runs

ImGuiTableFlags table_flags() {
  return ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_Resizable;
}

// A proportional column with its own initial weight. Without one ImGui derives the weights from the
// measured content (SizingStretchProp): a table first laid out while its tile had no room measured
// nothing, the weights became 0/0 = NaN, were saved in gui_layout.ini, and the next resize of a
// column aborted (TableUpdateColumnsWeightFromWidth).
void stretch_column(const char* label, float weight) { ImGui::TableSetupColumn(label, ImGuiTableColumnFlags_WidthStretch, weight); }

// Narrow last column of a table with the "remove from this list" button.
void remove_column() { ImGui::TableSetupColumn("##rm", ImGuiTableColumnFlags_WidthFixed, ImGui::GetFrameHeight() * 1.2f); }

bool remove_button(const std::string& id) {
  return ui::icon_button(("rm" + id).c_str(), Icon::Close, tr("Remove from this list (the files and logs are kept)"));
}

ImVec4 ticket_status_color(const std::string& s) {
  if (s == "closed") return rgb(115, 201, 145);
  if (s == "escalated") return rgb(241, 120, 90);
  if (s == "waiting") return rgb(230, 180, 60);
  if (s == "open") return rgb(160, 160, 160);
  return rgb(90, 170, 255);  // descending, fixing, verifying: agents at work
}

std::string elapsed(std::int64_t since) {
  auto s = std::max<std::int64_t>(0, core::unix_now() - since);
  return s < 120 ? fmt::format("{} s", s) : fmt::format("{} min", s / 60);
}

}  // namespace

void Ui::cleared_changed() { w_.save_cleared(cleared_); }

void Ui::interrupt_process(int pid) {
  // SIGINT to the command only (not its process group): the CLI stops its agents
  // gracefully; builds it started finish on their own.
  if (pid <= 0 || pid == core::current_pid()) return;
  core::pid_interrupt(pid);  // Windows: ends it (no Ctrl+C for another console)
  interrupted_[pid] = ImGui::GetTime();
}

void Ui::panel_actions(const char* key, int shown, int hidden, const std::function<void()>& clear_all,
                       const std::function<void()>& restore) {
  std::string clear_label = with_icon(Icon::Trash, tr("Clear list"));
  std::string restore_label = with_icon(Icon::Undo, fmt::format(fmt::runtime(tr("Restore ({})")), hidden));
  auto bw = [](const std::string& l) { return ImGui::CalcTextSize(l.c_str()).x + 12 + 6; };
  float w = (shown > 0 ? bw(clear_label) : 0) + (hidden > 0 ? bw(restore_label) : 0);
  ImGui::SameLine(std::max(ImGui::GetCursorPosX() + 12, ImGui::GetContentRegionMax().x - w));
  std::string popup = std::string("confirm_") + key;
  if (shown > 0) {
    if (ui::icon_button((std::string("clear_") + key).c_str(), Icon::Trash, tr("Remove every row of this panel from the view"), false,
                        tr("Clear list"))) {
      ImGui::OpenPopup(popup.c_str());
    }
    if (hidden > 0) ImGui::SameLine(0, 6);
  }
  if (hidden > 0 && ui::icon_button((std::string("restore_") + key).c_str(), Icon::Undo, tr("Show the cleared rows again"), false,
                                    fmt::format(fmt::runtime(tr("Restore ({})")), hidden).c_str())) {
    restore();
  }
  if (ImGui::BeginPopup(popup.c_str())) {
    ImGui::Text(tr("Remove %d rows from this panel?"), shown);
    ImGui::PushTextWrapPos(ImGui::GetFontSize() * 26);
    ui::muted("%s", tr("Only the view is cleared: tickets, run journals, test results and karkas.log stay; new "
                       "activity shows again, Restore brings everything back."));
    ImGui::PopTextWrapPos();
    ImGui::Spacing();
    ImGui::PushStyleColor(ImGuiCol_Button, theme::accent(0.9f));
    if (ImGui::Button(tr("Clear list"))) {
      clear_all();
      ImGui::CloseCurrentPopup();
    }
    ImGui::PopStyleColor();
    ImGui::SameLine();
    if (ImGui::Button(tr("Cancel"))) ImGui::CloseCurrentPopup();
    ImGui::EndPopup();
  }
}

void Ui::draw_bottom(const Model& m) {
  // Every tab of the bottom panel is a tile of its own (ADR 0032): docked together under the
  // schema by default, dragged by its tab anywhere, floating or a window of the desktop.
  if (!settings_.show_bottom) return;
  int errors = 0;
  int warnings = 0;
  for (const auto& d : m.snap->drift) (d.severity == "E" ? errors : warnings)++;
  std::size_t tickets = 0;
  for (const auto& t : m.snap->tickets) tickets += cleared_.hides(t) ? 0 : 1;
  // Notes (ADR 0038): broken [[links]] are their problems; no tickets, builds or runs.
  const char* drift_name = notes_ ? tr("Problems") : tr("Drift");
  std::string drift_label = m.snap->drift.empty() ? std::string(drift_name) : fmt::format("{} {}·{}", drift_name, errors, warnings);
  struct Tab {
    Tile tile;
    const char* key;
    Icon ic;
    std::string label;
    void (Ui::*body)(const Model&);
  };
  std::string run_label = tr("Run");
  if (run_ && run_->busy()) run_label += run_->phase() == RunPhase::Building ? std::string(" · ") + tr("building") : std::string(" ●");
  const std::array<Tab, 6> tabs = {{
      {Tile::Tickets, "tickets", Icon::Ticket, fmt::format("{} {}", tr("Tickets"), tickets), &Ui::bottom_tickets},
      {Tile::Agents, "agents", Icon::Agent, m.active.empty() ? std::string(tr("Agents")) : fmt::format("{} {}", tr("Agents"), m.active.size()),
       &Ui::bottom_agents},
      {Tile::Build, "build", Icon::Hammer, tr("Build & tests"), &Ui::bottom_tests},
      {Tile::Costs, "costs", Icon::Coins, tr("Costs"), &Ui::bottom_costs},
      {Tile::Drift, "drift", Icon::Wave, drift_label, &Ui::bottom_drift},
      {Tile::Run, "run", Icon::Play, run_label, &Ui::bottom_run},
  }};
  if (!bottom_select_.empty() && !tab_visible(bottom_select_.c_str())) set_tab_visible(bottom_select_.c_str(), true);
  bool keep_select = false;
  for (const auto& tab : tabs) {
    if (!tab_visible(tab.key)) continue;
    // Notes (ADR 0038, 0039): problems, and the assistant's runs and costs.
    if (notes_ && tab.tile != Tile::Drift && tab.tile != Tile::Agents && tab.tile != Tile::Costs) continue;
    // Brings its tab forward; kept until its window exists (the first frames of a new layout).
    bool select = bottom_select_ == tab.key;
    if (select) ImGui::SetNextWindowFocus();
    if (select && ImGui::FindWindowByName((std::string("###tile_") + tab.key).c_str()) == nullptr) keep_select = true;
    // A tile added after the layout was saved (the Run tab, ADR 0037): docked with the bottom tabs
    // the first time it is drawn, not floating in the middle.
    std::string window = std::string("###tile_") + tab.key;
    if (ImGui::FindWindowByName(window.c_str()) == nullptr && ImGui::FindWindowSettingsByID(ImHashStr(window.c_str())) == nullptr &&
        tile_request_[static_cast<std::size_t>(tab.tile)] == 0 && tile_mode(tab.tile) == TileMode::Docked) {
      tile_request_[static_cast<std::size_t>(tab.tile)] = 1;
    }
    bool open = true;
    bool visible = begin_tile(tab.tile, with_icon(tab.ic, tab.label), &open, theme::kSideBg, ImVec2(10, 6));
    if (visible) {
      ImGui::PushID(tab.key);
      (this->*tab.body)(m);
      ImGui::PopID();
    }
    ImGui::End();
    if (!open) set_tab_visible(tab.key, false);  // closed by its tab: the Windows menu shows it again
  }
  if (!keep_select) bottom_select_.clear();
}

// --- tickets ---------------------------------------------------------------------------------

void Ui::bottom_tickets(const Model& m) {
  std::vector<const index::TicketRow*> rows;
  int hidden = 0;
  for (const auto& t : m.snap->tickets) {
    if (cleared_.hides(t)) {
      ++hidden;
    } else {
      rows.push_back(&t);
    }
  }
  std::map<std::string, double> ticket_cost;  // every run of the ticket, also cleared ones
  double cost_total = 0;
  for (const auto& r : m.runs) {
    if (!r.ticket.empty()) ticket_cost[r.ticket] += r.cost_usd;
  }
  for (const auto* t : rows) cost_total += ticket_cost[t->id];
  ui::muted("%s", fmt::format(fmt::runtime(tr("{} tickets · ${:.4f}")), rows.size(), cost_total).c_str());
  panel_actions(
      "tickets", static_cast<int>(rows.size()), hidden,
      [&] {
        cleared_.clear_all_tickets(core::unix_now());
        req_.ticket.clear();
        cleared_changed();
      },
      [&] {
        cleared_.restore_tickets();
        cleared_changed();
      });
  if (rows.empty()) {
    ui::muted("%s", hidden > 0 ? tr("the list is cleared; new tickets show here")
                               : tr("no tickets yet: write to the orchestrator (chat), or karkas task / karkas debug"));
    return;
  }
  // The list and the selected ticket side by side; the splitter between them is dragged.
  float avail_w = ImGui::GetContentRegionAvail().x;
  float avail_h = ImGui::GetContentRegionAvail().y;
  constexpr float kMinList = 260.0f;
  constexpr float kMinText = 240.0f;
  float& list_w = settings_.tickets_list_w;
  if (list_w <= 0) list_w = avail_w * 0.62f;
  list_w = std::clamp(list_w, kMinList, std::max(kMinList, avail_w - kMinText));
  ImGui::BeginChild("tlist", ImVec2(list_w, avail_h));
  const ImGuiTableFlags flags = ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_BordersInnerH |
                                ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_PadOuterX;
  // The title keeps at least kTitleMin: time, cost and kind are shown only when they fit
  // beside it (the selected ticket's details show them all).
  float status_w = 0;
  for (const auto* t : rows) status_w = std::max(status_w, ImGui::CalcTextSize(tr_ticket_status(t->status)).x);
  float dot_w = ImGui::CalcTextSize(icon(Icon::Dot)).x + 4;
  const float id_w = ImGui::CalcTextSize("T-0000").x + 4;
  const float kind_w = std::max(ImGui::CalcTextSize(tr("feature")).x, ImGui::CalcTextSize(tr("bug")).x) + 4;
  const float st_w = status_w + dot_w + 4;
  const float time_w = ImGui::CalcTextSize("00.00 00:00").x + 4;
  const float cost_w = ImGui::CalcTextSize("0.0000").x + 4;
  const float pad = ImGui::GetStyle().CellPadding.x * 2 + 2;
  constexpr float kTitleMin = 170.0f;
  float room = list_w - (id_w + st_w + ImGui::GetFrameHeight() * 1.2f + pad * 4) - ImGui::GetStyle().ScrollbarSize;
  auto fits = [&](float w) {
    if (room - (w + pad) < kTitleMin) return false;
    room -= w + pad;
    return true;
  };
  const bool show_time = fits(time_w);
  const bool show_cost = fits(cost_w);
  const bool show_kind = fits(kind_w);
  const int title_col = show_kind ? 3 : 2;
  if (ImGui::BeginTable("t", 4 + (show_time ? 1 : 0) + (show_cost ? 1 : 0) + (show_kind ? 1 : 0), flags, ImVec2(0, 0))) {
    ImGui::TableSetupScrollFreeze(0, 1);
    ImGui::TableSetupColumn("id", ImGuiTableColumnFlags_WidthFixed, id_w);
    if (show_kind) ImGui::TableSetupColumn(tr("kind"), ImGuiTableColumnFlags_WidthFixed, kind_w);
    ImGui::TableSetupColumn(tr("status"), ImGuiTableColumnFlags_WidthFixed, st_w);
    ImGui::TableSetupColumn(tr("title"), ImGuiTableColumnFlags_WidthStretch);
    if (show_time) ImGui::TableSetupColumn(tr("time"), ImGuiTableColumnFlags_WidthFixed, time_w);
    if (show_cost) ImGui::TableSetupColumn("$", ImGuiTableColumnFlags_WidthFixed, cost_w);
    remove_column();
    ImGui::TableHeadersRow();
    ImGuiTable* table = ImGui::GetCurrentTable();
    for (const auto* t : rows) {
      ImGui::PushID(t->id.c_str());
      bool sel = req_.ticket == t->id;
      // The selected row opens up: its whole title, wrapped in the title column.
      float title_w = table != nullptr ? table->Columns[title_col].WidthGiven : 200.0f;
      float line = ImGui::GetTextLineHeight();
      float row_h = sel ? std::max(line, ImGui::CalcTextSize(t->title.c_str(), nullptr, false, title_w).y) : line;
      ImGui::TableNextRow(0, row_h + ImGui::GetStyle().CellPadding.y * 2);
      ImGui::TableNextColumn();
      if (ImGui::Selectable(t->id.c_str(), sel, ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowOverlap,
                            ImVec2(0, row_h))) {
        req_.ticket = sel ? std::string() : t->id;  // a second click folds the row again
      }
      bool hot = ImGui::IsItemHovered();
      ui::hand_on_hover();
      bool remove = false;
      if (ImGui::BeginPopupContextItem("ctx")) {
        ImGui::TextDisabled("%s", t->id.c_str());
        ImGui::Separator();
        if (ImGui::MenuItem(with_icon(Icon::Chat, tr("Write about this ticket in the chat")).c_str())) {
          chat_scope_ = {ChatScope::Kind::Ticket, t->id};  // a message is added to the ticket, which runs again
          show_chat_ = true;
          chat_focus_ = true;
          chat_window_focus_ = true;
        }
        if (ImGui::MenuItem(with_icon(Icon::Copy, tr("Copy")).c_str())) ImGui::SetClipboardText((t->id + " " + t->title).c_str());
        if (ImGui::MenuItem(with_icon(Icon::Close, tr("Remove from this list")).c_str())) remove = true;
        ImGui::EndPopup();
      }
      if (show_kind) {
        ImGui::TableNextColumn();
        ImGui::TextUnformatted(tr(t->kind.c_str()));
      }
      ImGui::TableNextColumn();
      ui::status_pill(tr_ticket_status(t->status), ticket_status_color(t->status));
      ImGui::TableNextColumn();
      if (sel) {
        ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + title_w);
        ImGui::TextUnformatted(t->title.c_str());
        ImGui::PopTextWrapPos();
      } else {
        auto shown = ui::ellipsize(t->title, title_w);
        ImGui::TextUnformatted(shown.c_str());
        if (hot && shown != t->title) ImGui::SetItemTooltip("%s", t->title.c_str());
      }
      if (show_time) {
        ImGui::TableNextColumn();
        // When it was done (closed) or asked (created); both in the tooltip.
        bool closed = t->status == "closed" && t->closed_at > 0;
        std::string at = when(closed ? t->closed_at : t->created_at);
        ImGui::TextDisabled("%s", at.c_str());
        if (ImGui::IsItemHovered()) {
          std::string tip = fmt::format(fmt::runtime(tr("created {}")), when(t->created_at));
          if (t->closed_at > 0) tip += "\n" + fmt::format(fmt::runtime(tr("closed {} (after {})")), when(t->closed_at), duration_text(t->closed_at - t->created_at));
          ImGui::SetTooltip("%s", tip.c_str());
        }
      }
      if (show_cost) {
        ImGui::TableNextColumn();
        if (auto it = ticket_cost.find(t->id); it != ticket_cost.end() && it->second > 0) ImGui::TextDisabled("%.4f", it->second);
      }
      ImGui::TableNextColumn();
      remove = remove_button(t->id) || remove;
      if (remove) {
        cleared_.clear_ticket(*t);
        if (req_.ticket == t->id) req_.ticket.clear();
        cleared_changed();
      }
      ImGui::PopID();
    }
    ImGui::EndTable();
  }
  ImGui::EndChild();
  ImGui::SameLine(0, 2);
  ui::vsplitter("tsplit", &list_w, kMinList, kMinText, avail_w, avail_h);
  if (ImGui::IsItemDeactivated()) settings_changed();
  ImGui::SameLine(0, 2);
  ImGui::BeginChild("ticket", ImVec2(0, avail_h), ImGuiChildFlags_Borders);
  const index::TicketRow* cur = nullptr;
  for (const auto* t : rows) {
    if (t->id == req_.ticket) cur = t;
  }
  if (m.ticket_text.empty() || cur == nullptr) {
    ui::muted("%s", tr("(select a ticket)"));
  } else {
    // Actions first (always in reach), then id, kind, status, the title and the ticket.
    bool editing = ticket_edit_id_ == cur->id;
    ui::copy_button("tcopy", m.ticket_text);
    ImGui::SameLine(0, 4);
    if (!editing && ui::icon_button("tedit", Icon::Pencil, tr("Edit the ticket by hand: the task or symptom, facts, status, trail (checked before it is written)"),
                                    false, tr("Edit"))) {
      ticket_edit_id_ = cur->id;
      ticket_edit_buf_ = m.ticket_text;
      ticket_edit_seen_ = m.ticket_edit_saved;
      ticket_edit_wait_ = false;
      editing = true;
    }
    if (cur->status != "closed" && chat_) {
      ImGui::SameLine(0, 4);
      if (ui::icon_button("trun", Icon::Play, tr("Run the ticket again (karkas ticket run)"), false, tr("Run again"))) {
        show_chat_ = true;
        chat_->send(ChatMode::Task, "/ticket run " + cur->id);
      }
      ImGui::SameLine(0, 4);
      if (ui::icon_button("tclose", Icon::Check, tr("Close the ticket by hand (karkas ticket close)"), false, tr("Close ticket"))) {
        chat_->send(ChatMode::Task, "/ticket close " + cur->id);
      }
    }
    ImGui::PushFont(theme::heading_font());
    ImGui::TextUnformatted(cur->id.c_str());
    ImGui::PopFont();
    ImGui::SameLine();
    ui::badge(tr(cur->kind.c_str()), with_alpha(theme::accent(1.0f), 0.25f));
    ImGui::SameLine();
    ui::status_pill(tr_ticket_status(cur->status), ticket_status_color(cur->status));
    ImGui::PushTextWrapPos(0.0f);
    ImGui::TextUnformatted(cur->title.c_str());
    ImGui::PopTextWrapPos();
    ImGui::Separator();
    if (editing) {
      ticket_editor(m, cur->id);
    } else {
      ImGui::BeginChild("ttext", ImVec2(0, 0));
      ui::ksl_wrapped(m.ticket_text);
      ImGui::EndChild();
    }
  }
  ImGui::EndChild();
}

void Ui::ticket_editor(const Model& m, const std::string& id) {
  if (ticket_edit_wait_ && m.ticket_edit_saved != ticket_edit_seen_) {
    ticket_edit_id_.clear();  // written: the ticket shows as it is on disk now
    ticket_edit_wait_ = false;
    return;
  }
  // The buttons first: always in reach in a small panel.
  ImGui::BeginDisabled(ticket_edit_buf_ == m.ticket_text);
  ImGui::PushStyleColor(ImGuiCol_Button, theme::accent(0.9f));
  if (ImGui::Button(with_icon(Icon::Check, tr("Check and save")).c_str())) {
    w_.save_ticket(id, ticket_edit_buf_);
    ticket_edit_seen_ = m.ticket_edit_saved;
    ticket_edit_wait_ = true;
  }
  ImGui::PopStyleColor();
  ImGui::EndDisabled();
  ImGui::SetItemTooltip("%s", tr("The agents read the ticket as it is written here at their next run. Its id stays; a ticket an agent works on "
                                 "cannot be saved (stop it first)."));
  ImGui::SameLine();
  if (ImGui::Button(tr("Cancel"))) {
    ticket_edit_id_.clear();
    return;
  }
  if (ticket_edit_wait_ && !m.ticket_edit_error.empty()) {
    ImGui::PushTextWrapPos(0);
    ImGui::TextColored(rgb(241, 96, 96), "%s", m.ticket_edit_error.c_str());
    ImGui::PopTextWrapPos();
  }
  ImGui::PushFont(theme::mono_font());
  ImGui::PushStyleColor(ImGuiCol_FrameBg, theme::col(theme::kEditorBg));
  if (ImGui::InputTextMultiline("##ticket_text", &ticket_edit_buf_, ImVec2(-1, std::max(60.0f, ImGui::GetContentRegionAvail().y)),
                                ImGuiInputTextFlags_AllowTabInput)) {
    ticket_edit_wait_ = false;  // a new attempt: the old error no longer applies
  }
  ImGui::PopStyleColor();
  ImGui::PopFont();
}

// --- agents ----------------------------------------------------------------------------------

void Ui::bottom_agents(const Model& m) {
  std::set<int> live;
  if (m.active.empty()) ui::muted("%s", tr("no agent is working now"));
  for (std::size_t i = 0; i < m.active.size(); ++i) {
    const auto& a = m.active[i];
    live.insert(a.pid);
    ImGui::PushID(static_cast<int>(i));
    ImGui::AlignTextToFramePadding();
    status_dot("", true, pulse_);
    std::string label = fmt::format("{}  {}  ({})  {}", a.role, a.block, a.model, a.ticket);
    if (ImGui::Selectable(label.c_str(), false, ImGuiSelectableFlags_AllowOverlap,
                          ImVec2(ImGui::CalcTextSize(label.c_str()).x, 0))) {
      open_live(a.journal.filename().string());
    }
    ui::hand_on_hover();
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("Watch every step of this agent"));
    ImGui::SameLine();
    ui::muted("%s", fmt::format("{} · {}", when(a.started), elapsed(a.started)).c_str());
    ImGui::SameLine(0, 16);
    if (ui::icon_button("watch", Icon::Eye, tr("Watch every step of this agent"), false, tr("Watch"))) open_live(a.journal.filename().string());
    ImGui::SameLine(0, 6);
    if (a.stopping) {
      ImGui::TextColored(rgb(240, 200, 30), "%s", tr("stopping after the current step..."));
    } else if (ui::icon_button("stop", Icon::Stop, tr("Stop this agent: it ends after its current step, its journal is finished, "
                                                       "the ticket stays open"),
                               false, tr("Stop"))) {
      w_.stop_run(a.journal);
    }
    if (a.pid > 0 && a.pid != core::current_pid()) {
      ImGui::SameLine(0, 6);
      auto it = interrupted_.find(a.pid);
      if (it == interrupted_.end()) {
        if (ui::icon_button("int", Icon::Stop, fmt::format(fmt::runtime(tr("Stop the whole command (pid {}) like Ctrl+C: all its agents")), a.pid).c_str(),
                            false, tr("Stop command"))) {
          interrupt_process(a.pid);
        }
      } else if (ImGui::GetTime() - it->second > kKillAfterS) {
        if (ui::icon_button("kill", Icon::Error, tr("The command does not stop: end it at once (SIGKILL); its current step is lost"), false,
                            tr("Kill"))) {
          core::pid_kill(a.pid);
        }
      } else {
        ui::muted("%s", tr("stopping the command..."));
      }
    }
    ImGui::PopID();
  }
  std::erase_if(interrupted_, [&](const auto& kv) { return !live.contains(kv.first); });
  ImGui::Spacing();
  ImGui::Separator();
  std::vector<const index::RunRow*> rows;
  int hidden = 0;
  for (auto it = m.runs.rbegin(); it != m.runs.rend(); ++it) {
    if (cleared_.hides(*it)) {
      ++hidden;
    } else if (rows.size() < 200) {
      rows.push_back(&*it);
    }
  }
  double runs_cost = 0;
  for (const auto* r : rows) runs_cost += r->cost_usd;
  ui::muted("%s", fmt::format(fmt::runtime(tr("recent runs: {} · ${:.4f}")), rows.size(), runs_cost).c_str());
  panel_actions(
      "runs", static_cast<int>(rows.size()), hidden,
      [&] {
        cleared_.clear_all_runs(core::unix_now());
        cleared_changed();
      },
      [&] {
        cleared_.restore_runs();
        cleared_changed();
      });
  if (rows.empty()) return;
  if (ImGui::BeginTable("runs", 11, table_flags())) {
    ImGui::TableSetupScrollFreeze(0, 1);
    for (const auto& [h, w] : {std::pair{"time", 1.2f}, {"duration", 0.9f}, {"ticket", 0.9f}, {"role", 1.0f}, {"block", 1.6f}, {"tier", 0.8f},
                               {"outcome", 1.0f}, {"tokens in/out", 1.3f}, {"cache read", 0.9f}, {"usd", 0.8f}}) {
      stretch_column(tr(h), w);
    }
    remove_column();
    ImGui::TableHeadersRow();
    for (const auto* r : rows) {
      ImGui::PushID(r->journal.empty() ? std::to_string(r->id).c_str() : r->journal.c_str());
      ImGui::TableNextRow();
      ImGui::TableNextColumn();
      // The whole row opens the run in the live feed of the chat window.
      if (ImGui::Selectable(when(r->ts).c_str(), false, ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowOverlap) &&
          !r->journal.empty()) {
        open_live(r->journal);
      }
      ui::hand_on_hover();
      for (const auto& cell : {duration_text(r->duration_ms / 1000), r->ticket, r->role, r->block, r->tier, r->outcome,
                               fmt::format("{}/{}", r->input_tokens, r->output_tokens), std::to_string(r->cache_read_tokens),
                               fmt::format("{:.4f}", r->cost_usd)}) {
        ImGui::TableNextColumn();
        ImGui::TextUnformatted(cell.c_str());
      }
      ImGui::TableNextColumn();
      if (!r->journal.empty() && remove_button(r->journal)) {
        cleared_.clear_run(*r);
        cleared_changed();
      }
      ImGui::PopID();
    }
    ImGui::EndTable();
  }
}

// --- build & tests ----------------------------------------------------------------------------

void Ui::bottom_tests(const Model& m) {
  std::vector<const index::TestRow*> rows;
  int hidden = 0;
  for (const auto& t : m.snap->tests) {
    if (cleared_.hides(t)) {
      ++hidden;
    } else {
      rows.push_back(&t);
    }
  }
  ui::muted("%s", fmt::format(fmt::runtime(tr("{} blocks with test results")), rows.size()).c_str());
  panel_actions(
      "tests", static_cast<int>(rows.size()), hidden,
      [&] {
        cleared_.clear_all_tests(core::unix_now());
        cleared_changed();
      },
      [&] {
        cleared_.restore_tests();
        cleared_changed();
      });
  if (rows.empty()) {
    ui::muted("%s", hidden > 0 ? tr("the list is cleared; the next test run shows here") : tr("no test results yet: karkas test"));
    return;
  }
  if (ImGui::BeginTable("tests", 7, table_flags())) {
    ImGui::TableSetupScrollFreeze(0, 1);
    for (const auto& [h, w] : {std::pair{"block", 1.6f}, {"time", 1.0f}, {"contract", 0.8f}, {"unit", 0.8f}, {"integration", 0.9f}, {"failures", 2.4f}}) {
      stretch_column(tr(h), w);
    }
    remove_column();
    ImGui::TableHeadersRow();
    for (const auto* t : rows) {
      ImGui::PushID(t->block.c_str());
      ImGui::TableNextRow();
      ImGui::TableNextColumn();
      ImGui::TextColored(t->fresh ? ImGui::GetStyleColorVec4(ImGuiCol_Text) : rgb(140, 140, 140), "%s%s%s", t->block.c_str(),
                         t->fresh ? "" : " ", t->fresh ? "" : tr("(stale)"));
      ImGui::TableNextColumn();
      ImGui::TextDisabled("%s", when(t->ts).c_str());
      for (const auto& c : {t->contract, t->unit, t->integration}) {
        ImGui::TableNextColumn();
        if (c.total > 0) {
          ImGui::TextColored(c.pass == c.total ? rgb(115, 201, 145) : rgb(241, 76, 76), "%s %d/%d", icon(c.pass == c.total ? Icon::Check : Icon::Close),
                             c.pass, c.total);
        }
      }
      ImGui::TableNextColumn();
      ImGui::TextUnformatted(t->build_failed ? tr("build failed") : (t->failures.empty() ? "" : t->failures.front().c_str()));
      ImGui::TableNextColumn();
      if (remove_button(t->block)) {
        cleared_.clear_test(*t);
        cleared_changed();
      }
      ImGui::PopID();
    }
    ImGui::EndTable();
  }
}

// --- costs ------------------------------------------------------------------------------------

void Ui::bottom_costs(const Model& m) {
  double total = 0;
  std::int64_t in = 0, cr = 0, cw = 0;
  std::size_t n = 0;
  int hidden = 0;
  std::map<std::string, double> by_block, by_model, by_ticket;
  for (const auto& r : m.runs) {
    if (cleared_.hides(r)) {
      ++hidden;
      continue;
    }
    ++n;
    total += r.cost_usd;
    in += r.input_tokens;
    cr += r.cache_read_tokens;
    cw += r.cache_write_tokens;
    by_block[r.block] += r.cost_usd;
    by_model[r.model] += r.cost_usd;
    if (!r.ticket.empty()) by_ticket[r.ticket] += r.cost_usd;
  }
  double share = in + cr + cw > 0 ? 100.0 * static_cast<double>(cr) / static_cast<double>(in + cr + cw) : 0.0;
  ImGui::AlignTextToFramePadding();
  ImGui::Text(tr("total $%.4f · %zu runs · cache read share %.1f%%"), total, n, share);
  if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal) && hidden > 0) {
    ImGui::SetTooltip("%s", tr("Cleared runs are not counted here; karkas cost still counts every run."));
  }
  panel_actions(
      "costs", static_cast<int>(n), hidden,
      [&] {
        cleared_.clear_all_runs(core::unix_now());
        cleared_changed();
      },
      [&] {
        cleared_.restore_runs();
        cleared_changed();
      });
  ImGui::Spacing();
  if (ImGui::BeginTable("costs", 3, ImGuiTableFlags_SizingStretchSame | ImGuiTableFlags_BordersInnerV)) {
    ImGui::TableNextRow();
    for (const auto& [name, v] : {std::pair{"by block", &by_block}, std::pair{"by model", &by_model}, std::pair{"by ticket", &by_ticket}}) {
      ImGui::TableNextColumn();
      ui::panel_title(tr(name));
      for (const auto& [k, c] : *v) {
        ImGui::TextUnformatted(k.c_str());
        ImGui::SameLine(ImGui::GetColumnWidth() - ImGui::CalcTextSize("$0.0000").x - 4);
        ImGui::Text("$%.4f", c);
      }
    }
    ImGui::EndTable();
  }
}

// --- drift ------------------------------------------------------------------------------------

void Ui::bottom_drift(const Model& m) {
  // Drift is the state of the code now (recomputed on every change): it cannot be cleared,
  // only fixed.
  if (m.snap->drift.empty()) ui::muted("%s", (notes_ ? tr("no problems") : tr("no drift")));
  for (std::size_t i = 0; i < m.snap->drift.size(); ++i) {
    const auto& d = m.snap->drift[i];
    bool err = d.severity == "E";
    ImGui::PushID(static_cast<int>(i));
    ImGui::TextColored(err ? rgb(241, 76, 76) : rgb(230, 180, 60), "%s", icon(err ? Icon::Error : Icon::Warning));
    ImGui::SameLine();
    ImGui::TextDisabled("%s", d.code.c_str());
    ImGui::SameLine();
    ImGui::TextUnformatted(d.message.c_str());
    if (!d.block.empty()) {
      ImGui::SameLine();
      ImGui::TextDisabled("%s", d.block.c_str());
    }
    if (!d.file.empty()) {
      ImGui::SameLine();
      std::string where = d.line > 0 ? fmt::format("{}:{}", d.file, d.line) : d.file;
      if (ImGui::TextLink(where.c_str())) w_.open_in_editor(d.file, d.line);
    }
    ImGui::PopID();
  }
}

}  // namespace kk::gui
