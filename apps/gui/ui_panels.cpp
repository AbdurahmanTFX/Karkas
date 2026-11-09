// The bottom panel of karkas-gui (14.4): tickets, agents, build & tests, costs, drift.
// Rows can be removed from a panel and a panel cleared — display only, the ticket files,
// run journals, test results and karkas.log stay — and running agents can be stopped
// (docs/adr/0019).
#include <fmt/format.h>
#include <imgui.h>
#include <unistd.h>

#include <algorithm>
#include <csignal>
#include <set>

#include "i18n.hpp"
#include "icons.hpp"
#include "kk/core/paths.hpp"
#include "theme.hpp"
#include "ui.hpp"
#include "widgets.hpp"

namespace kk::gui {

namespace {

constexpr double kKillAfterS = 5.0;  // "Kill" appears when a stopped command still runs

ImGuiTableFlags table_flags() {
  return ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_Resizable;
}

// Narrow last column of a table with the "remove from this list" button.
void remove_column() { ImGui::TableSetupColumn("##rm", ImGuiTableColumnFlags_WidthFixed, ImGui::GetFrameHeight() * 1.2f); }

bool remove_button(const std::string& id) {
  return ui::icon_button(("rm" + id).c_str(), Icon::Close, tr("Remove from this list (the files and logs are kept)"));
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
  if (pid <= 0 || pid == ::getpid()) return;
  ::kill(pid, SIGINT);
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
  if (!ui::begin_docked("Panel###panel", theme::kSideBg, ImVec2(10, 4))) {
    ImGui::End();
    return;
  }
  int errors = 0;
  int warnings = 0;
  for (const auto& d : m.snap->drift) (d.severity == "E" ? errors : warnings)++;
  std::size_t tickets = 0;
  for (const auto& t : m.snap->tickets) tickets += cleared_.hides(t) ? 0 : 1;
  auto tab = [&](const char* key, Icon ic, const std::string& text) {
    ImGuiTabItemFlags f = bottom_select_ == key ? ImGuiTabItemFlags_SetSelected : 0;
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(10, 5));
    bool r = ImGui::BeginTabItem((with_icon(ic, text) + "###" + key).c_str(), nullptr, f);
    ImGui::PopStyleVar();
    return r;
  };
  ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(10, 5));
  bool tabs = ImGui::BeginTabBar("##panel_tabs");
  ImGui::PopStyleVar();
  if (tabs) {
    if (tab("tickets", Icon::Ticket, fmt::format("{} {}", tr("Tickets"), tickets))) {
      bottom_tickets(m);
      ImGui::EndTabItem();
    }
    if (tab("agents", Icon::Agent, m.active.empty() ? std::string(tr("Agents")) : fmt::format("{} {}", tr("Agents"), m.active.size()))) {
      bottom_agents(m);
      ImGui::EndTabItem();
    }
    if (tab("build", Icon::Hammer, tr("Build & tests"))) {
      bottom_tests(m);
      ImGui::EndTabItem();
    }
    if (tab("costs", Icon::Coins, tr("Costs"))) {
      bottom_costs(m);
      ImGui::EndTabItem();
    }
    std::string drift_label = m.snap->drift.empty() ? std::string(tr("Drift")) : fmt::format("{} {}·{}", tr("Drift"), errors, warnings);
    if (tab("drift", Icon::Wave, drift_label)) {
      bottom_drift(m);
      ImGui::EndTabItem();
    }
    ImGui::EndTabBar();
  }
  bottom_select_.clear();
  ImGui::End();
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
  ui::muted("%s", fmt::format(fmt::runtime(tr("{} tickets")), rows.size()).c_str());
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
  if (ImGui::BeginTable("t", 5, table_flags(), ImVec2(ImGui::GetContentRegionAvail().x * 0.48f, 0))) {
    ImGui::TableSetupScrollFreeze(0, 1);
    for (const char* h : {"id", "kind", "status", "title"}) ImGui::TableSetupColumn(tr(h));
    remove_column();
    ImGui::TableHeadersRow();
    for (const auto* t : rows) {
      ImGui::PushID(t->id.c_str());
      ImGui::TableNextRow();
      ImGui::TableNextColumn();
      if (ImGui::Selectable(t->id.c_str(), req_.ticket == t->id, ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowOverlap)) {
        req_.ticket = t->id;
      }
      bool remove = false;
      if (ImGui::BeginPopupContextItem("ctx")) {
        ImGui::TextDisabled("%s", t->id.c_str());
        ImGui::Separator();
        if (t->status != "closed" && ImGui::MenuItem(with_icon(Icon::Chat, tr("Continue in the chat")).c_str())) {
          show_chat_ = true;
          chat_focus_ = true;
          std::snprintf(chat_input_, sizeof chat_input_, "/ticket run %s", t->id.c_str());
        }
        if (ImGui::MenuItem(with_icon(Icon::Close, tr("Remove from this list")).c_str())) remove = true;
        ImGui::EndPopup();
      }
      ImGui::TableNextColumn();
      ImGui::TextUnformatted(t->kind.c_str());
      ImGui::TableNextColumn();
      ImGui::TextColored(t->status == "closed" ? rgb(115, 201, 145) : rgb(230, 180, 60), "%s", t->status.c_str());
      ImGui::TableNextColumn();
      ImGui::TextUnformatted(t->title.c_str());
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
    ImGui::SameLine();
    ImGui::BeginChild("ticket", ImVec2(0, 0), ImGuiChildFlags_Borders);
    if (m.ticket_text.empty() || req_.ticket.empty()) {
      ui::muted("%s", tr("(select a ticket)"));
    } else {
      ui::ksl_text(m.ticket_text);
    }
    ImGui::EndChild();
  }
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
    ImGui::Text("%s  %s  (%s)  %s", a.role.c_str(), a.block.c_str(), a.model.c_str(), a.ticket.c_str());
    ImGui::SameLine();
    ui::muted("%s", elapsed(a.started).c_str());
    ImGui::SameLine(0, 16);
    if (a.stopping) {
      ImGui::TextColored(rgb(240, 200, 30), "%s", tr("stopping after the current step..."));
    } else if (ui::icon_button("stop", Icon::Stop, tr("Stop this agent: it ends after its current step, its journal is finished, "
                                                       "the ticket stays open"),
                               false, tr("Stop"))) {
      w_.stop_run(a.journal);
    }
    if (a.pid > 0 && a.pid != ::getpid()) {
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
          ::kill(a.pid, SIGKILL);
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
  ui::muted("%s", fmt::format(fmt::runtime(tr("recent runs: {}")), rows.size()).c_str());
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
  if (ImGui::BeginTable("runs", 8, table_flags())) {
    ImGui::TableSetupScrollFreeze(0, 1);
    for (const char* h : {"role", "block", "tier", "outcome", "tokens in/out", "cache read", "usd"}) ImGui::TableSetupColumn(tr(h));
    remove_column();
    ImGui::TableHeadersRow();
    for (const auto* r : rows) {
      ImGui::PushID(r->journal.empty() ? std::to_string(r->id).c_str() : r->journal.c_str());
      ImGui::TableNextRow();
      for (const auto& cell : {r->role, r->block, r->tier, r->outcome, fmt::format("{}/{}", r->input_tokens, r->output_tokens),
                               std::to_string(r->cache_read_tokens), fmt::format("{:.4f}", r->cost_usd)}) {
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
  if (ImGui::BeginTable("tests", 6, table_flags())) {
    ImGui::TableSetupScrollFreeze(0, 1);
    for (const char* h : {"block", "contract", "unit", "integration", "failures"}) ImGui::TableSetupColumn(tr(h));
    remove_column();
    ImGui::TableHeadersRow();
    for (const auto* t : rows) {
      ImGui::PushID(t->block.c_str());
      ImGui::TableNextRow();
      ImGui::TableNextColumn();
      ImGui::TextColored(t->fresh ? ImGui::GetStyleColorVec4(ImGuiCol_Text) : rgb(140, 140, 140), "%s%s%s", t->block.c_str(),
                         t->fresh ? "" : " ", t->fresh ? "" : tr("(stale)"));
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
  if (m.snap->drift.empty()) ui::muted("%s", tr("no drift"));
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
