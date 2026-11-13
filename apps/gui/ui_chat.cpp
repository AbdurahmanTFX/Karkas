// The orchestrator chat window of karkas-gui (docs/adr/0019, 0021). Two tabs:
// - Dialog: a message becomes `karkas task` / `karkas debug` / a "/command" run by the Chat
//   (chat.hpp); its progress, the ticket it ends with and the next steps (continue, show a
//   proposed change on the schema, edit it, approve or reject it) show here.
// - Live: every step of the agents (orchestrator, navigators, workers, testers) read from the
//   run journals as they are written — also for commands started from a terminal.
// Texts are selected with the mouse like in a browser (selectable.hpp) and copied with Ctrl+C
// or the context menu.
#include <fmt/format.h>
#include <imgui.h>

#include <algorithm>
#include <cstdio>

#include "i18n.hpp"
#include "icons.hpp"
#include "kk/code/init.hpp"
#include "kk/core/paths.hpp"
#include "kk/core/config.hpp"
#include "kk/core/text.hpp"
#include "theme.hpp"
#include "ui.hpp"
#include "selectable.hpp"
#include "widgets.hpp"

namespace kk::gui {

namespace {

constexpr double kKillAfterS = 5.0;

const ImVec4 kUser = rgb(55, 148, 255);
const ImVec4 kAgent = rgb(78, 201, 176);
const ImVec4 kThink = rgb(150, 150, 165);
const ImVec4 kCall = rgb(120, 180, 255);
const ImVec4 kResult = rgb(170, 170, 170);

// A rounded background behind the group drawn by `body`.
void bubble(ImVec4 bg, float wrap, const std::function<void()>& body) {
  auto* dl = ImGui::GetWindowDrawList();
  dl->ChannelsSplit(2);
  dl->ChannelsSetCurrent(1);
  ImVec2 p = ImGui::GetCursorScreenPos();
  ImGui::SetCursorScreenPos(ImVec2(p.x + 8, p.y + 6));
  ImGui::BeginGroup();
  ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + wrap);
  body();
  ImGui::PopTextWrapPos();
  ImGui::EndGroup();
  ImVec2 mx = ImGui::GetItemRectMax();
  dl->ChannelsSetCurrent(0);
  dl->AddRectFilled(p, ImVec2(std::max(mx.x + 8, p.x + 40), mx.y + 6), ImGui::GetColorU32(bg), 6.0f);
  dl->ChannelsMerge();
  ImGui::SetCursorScreenPos(ImVec2(p.x, mx.y + 6));
  ImGui::Dummy(ImVec2(1, 4));
}

std::string role_label(const std::string& role) {
  if (role == "orchestrator") return tr("orchestrator");
  if (role == "navigator") return tr("navigator");
  if (role == "worker") return tr("worker");
  if (role == "tester") return tr("tester");
  if (role == "architect") return tr("architect");
  if (role == "ask") return tr("question");
  return role;
}

// The first `n` lines of a text (and whether there is more).
std::pair<std::string, bool> head_lines(const std::string& s, int n) {
  std::size_t pos = 0;
  for (int i = 0; i < n; ++i) {
    pos = s.find('\n', pos);
    if (pos == std::string::npos) return {s, false};
    ++pos;
  }
  return {s.substr(0, pos > 0 ? pos - 1 : 0), true};
}

}  // namespace

// --- shared -----------------------------------------------------------------------------------

void Ui::set_auto(bool on) {
  auto r = core::write_config_value(root_, "agents", "auto_approve", std::optional<std::string>(on ? "true" : "false"));
  reload_models();
  w_.reload_config();
  (void)r;
}

void Ui::set_mode(const std::string& mode) {
  auto r = core::write_config_value(root_, "agents", "mode", std::optional<std::string>(mode));
  reload_models();
  w_.reload_config();
  if (r && cfg_.mcp() && !std::filesystem::exists(root_ / ".claude" / "agents")) (void)code::write_claude_agents(root_, cfg_);
  models_msg_ = r ? fmt::format(fmt::runtime(tr("saved to {}")), (root_ / "karkas.toml").string()) : r.error().message;
}

void Ui::fit_mcp_models() {
  auto text = core::read_file(root_ / "karkas.toml");
  kk::Result<std::string> out = text ? core::write_mcp_maps(*text, core::mcp_maps_from_api(cfg_)) : kk::Result<std::string>(std::unexpected(text.error()));
  if (out) {
    auto w = core::write_file(root_ / "karkas.toml", *out);
    models_msg_ = w ? std::string(tr("The MCP models now follow the API ones: Claude tiers of the same strength per level, role and block."))
                    : w.error().message;
  } else {
    models_msg_ = out.error().message;
  }
  reload_models();
  w_.reload_config();
}

const std::vector<llm::McpTurn>& Ui::mcp_waiting() {
  if (pulse_ - mcp_polled_ >= 1.0) {
    mcp_polled_ = pulse_;
    mcp_waiting_ = root_.empty() ? std::vector<llm::McpTurn>{} : llm::McpQueue(root_ / ".karkas" / "mcp").waiting();
  }
  return mcp_waiting_;
}

void Ui::mode_switch(const char* id) {
  ImGui::PushID(id);
  ImGui::AlignTextToFramePadding();
  ui::muted("%s", tr("Model:"));
  ImGui::SameLine();
  bool api = !cfg_.mcp();
  if (ui::toggle("api", Icon::Coins, "API", &api, tr("Agents' model through the API: Karkas calls the models of karkas.toml itself"))) set_mode("api");
  ImGui::SameLine(0, 2);
  bool mcp = cfg_.mcp();
  if (ui::toggle("mcp", Icon::Agent, "MCP", &mcp,
                 tr("Agents' model through MCP: Claude Code connected to `karkas mcp` answers every turn of the same agents "
                    "(its subagents per level); nothing is billed by the API"))) {
    set_mode("mcp");
  }
  ImGui::PopID();
}

void Ui::open_live(const std::string& journal) {
  show_chat_ = true;
  chat_tab_select_ = 1;
  chat_window_focus_ = true;
  live_journal_ = journal;
  live_follow_ = journal.empty();
  live_seen_ = 0;
}

void Ui::change_buttons(const Model* m, const std::string& c) {
  bool pending = false;
  if (m != nullptr) {
    for (const auto& ch : m->changes) pending |= ch.id == c && ch.status.value == "pending";
  }
  ImGui::PushID(c.c_str());
  if (ui::icon_button("show_c", Icon::Ghost, tr("Show what the change does on the schema (ghost blocks and links)"), false, c.c_str())) {
    side_ = SideView::Changes;
    settings_.show_side = true;
    req_.change = c;
    ghost_on_ = true;
    ghost_shown_.clear();
  }
  if (pending) {
    ImGui::SameLine(0, 4);
    if (ui::icon_button("edit_c", Icon::Pencil, tr("Edit the KSL diff of this change"))) {
      side_ = SideView::Changes;
      settings_.show_side = true;
      req_.change = c;
      edit_change_next_ = c;
    }
    ImGui::SameLine(0, 4);
    if (ui::icon_button("approve_c", Icon::Check, tr("Approve the change: the cards are written"), false, tr("Approve"))) w_.approve_change(c);
    ImGui::SameLine(0, 4);
    if (ui::icon_button("reject_c", Icon::Close, tr("Reject"), false, tr("Reject"))) w_.reject_change(c);
  }
  ImGui::PopID();
}

// --- dialog -----------------------------------------------------------------------------------

void Ui::chat_entry(const Model* m, const ChatEntry& e, std::size_t i) {
  ImGui::PushID(static_cast<int>(i));
  float wrap = ImGui::GetContentRegionAvail().x - 24;
  std::string at = when(e.ts);
  switch (e.kind) {
    case ChatEntry::Kind::User: {
      ImGui::Spacing();
      bubble(with_alpha(kUser, 0.16f), wrap, [&] {
        const char* mode = e.mode == "bug" ? tr("bug") : e.mode == "command" ? tr("command") : tr("task");
        ImGui::TextColored(kUser, "%s", with_icon(Icon::Chat, fmt::format("{} · {}", tr("you"), mode)).c_str());
        if (!at.empty()) {
          ImGui::SameLine();
          ui::muted("%s", at.c_str());
        }
        chat_sel_.text("msg", e.text, ImGui::GetStyleColorVec4(ImGuiCol_Text));
      });
      break;
    }
    case ChatEntry::Kind::Progress: {
      chat_sel_.text("prog", "· " + e.text, rgb(150, 150, 150), theme::mono_font());
      break;
    }
    case ChatEntry::Kind::Log: {
      bool warn = core::contains(e.text, " warn") || core::contains(e.text, " error");
      chat_sel_.text("log", e.text, warn ? rgb(230, 160, 60) : rgb(120, 120, 120), theme::mono_font());
      break;
    }
    case ChatEntry::Kind::Result: {
      ImGui::Spacing();
      ImGui::TextColored(kAgent, "%s", with_icon(Icon::Agent, tr("orchestrator")).c_str());
      if (!at.empty()) {
        ImGui::SameLine();
        ui::muted("%s", at.c_str());
      }
      ImGui::SameLine();
      ui::copy_button("rcopy", e.text);
      ImGui::PushStyleColor(ImGuiCol_ChildBg, theme::col(theme::kEditorBg));
      ImGui::BeginChild("result", ImVec2(0, 0), ImGuiChildFlags_AutoResizeY | ImGuiChildFlags_Borders);
      auto spans = ui::ksl_spans(e.text);
      chat_sel_.text("ksl", e.text, ImGui::GetStyleColorVec4(ImGuiCol_Text), theme::mono_font(), &spans);
      ImGui::EndChild();
      ImGui::PopStyleColor();
      // Next steps of the protocol: the ticket, its continuation, changes waiting for approval.
      if (!e.ticket.empty()) {
        if (ui::icon_button("open_t", Icon::Ticket, tr("Show the ticket in the panel below"), false, e.ticket.c_str())) {
          req_.ticket = e.ticket;
          bottom_select_ = "tickets";
          settings_.show_bottom = true;
        }
        std::string status;
        if (m != nullptr) {
          for (const auto& t : m->snap->tickets) {
            if (t.id == e.ticket) status = t.status;
          }
        }
        if (!status.empty() && status != "closed" && !(chat_ && chat_->busy())) {
          ImGui::SameLine(0, 4);
          if (ui::icon_button("cont", Icon::Play, tr("Run the ticket again from the orchestrator (karkas ticket run)"), false,
                              tr("Continue"))) {
            chat_->send(ChatMode::Task, "/ticket run " + e.ticket);
          }
        }
      }
      for (const auto& c : changes_in(e.text)) {
        if (!e.ticket.empty()) ImGui::SameLine(0, 4);
        change_buttons(m, c);
      }
      break;
    }
    case ChatEntry::Kind::Error: {
      ImGui::TextColored(rgb(241, 96, 96), "%s", icon(Icon::Error));
      ImGui::SameLine();
      chat_sel_.text("err", e.text, rgb(241, 96, 96));
      break;
    }
    case ChatEntry::Kind::Info: {
      ImGui::TextColored(rgb(200, 180, 110), "%s", icon(Icon::Info));
      ImGui::SameLine();
      chat_sel_.text("info", tr(e.text.c_str()), rgb(200, 180, 110));
      break;
    }
  }
  ImGui::PopID();
}

void Ui::chat_dialog(const Model* m) {
  bool busy = chat_->busy();
  float input_h = ImGui::GetTextLineHeight() * 4 + ImGui::GetStyle().FramePadding.y * 2;
  float footer = input_h + ImGui::GetFrameHeightWithSpacing() + ImGui::GetTextLineHeightWithSpacing() + 8;
  ImGui::BeginChild("conversation", ImVec2(0, -footer), ImGuiChildFlags_None);
  chat_sel_.begin();
  if (chat_cache_.empty()) {
    ImGui::PushTextWrapPos(0);
    ui::muted("%s", tr("Write what to do. The message goes to the orchestrator exactly like karkas task \"...\" "
                       "(or karkas debug for a bug): a ticket is created, the orchestrator and the navigators descend "
                       "the schema to the block, a worker changes its code and the tests decide. Schema changes wait "
                       "for your approval here."));
    ImGui::Spacing();
    ui::muted("%s", tr("Commands: /ticket run T-0001, /approve C-0001, /implement all, /test, /ask <block> \"question\"."));
    ImGui::PopTextWrapPos();
  }
  for (std::size_t i = 0; i < chat_cache_.size(); ++i) chat_entry(m, chat_cache_[i], i);
  if (m != nullptr && !m->active.empty()) {
    // What the agents do right now, one line each: every step is in the live tab.
    ImGui::Spacing();
    float a = static_cast<float>(0.6 + 0.4 * std::sin(pulse_ * 5.0));
    for (const auto& r : m->active) {
      ImGui::PushID(r.journal.filename().string().c_str());
      ImGui::AlignTextToFramePadding();
      ImGui::TextColored(rgb(240, 200, 30, a), "%s", with_icon(Icon::Agent, role_label(r.role) + " · " + r.block).c_str());
      ImGui::SameLine();
      ui::muted("%s", duration_text(core::unix_now() - r.started).c_str());
      ImGui::SameLine();
      if (ui::icon_button("watch", Icon::Eye, tr("Watch every step of this agent"), false, tr("Watch"))) open_live(r.journal.filename().string());
      ImGui::PopID();
    }
  }
  if (chat_scroll_) {
    ImGui::SetScrollHereY(1.0f);
    chat_scroll_ = false;
  }
  chat_sel_.end();
  ImGui::EndChild();
  ImGui::Separator();
  // Input: Enter sends, Ctrl+Enter is a new line.
  if (chat_focus_) {
    ImGui::SetKeyboardFocusHere();
    chat_focus_ = false;
  }
  float send_w = ImGui::CalcTextSize(with_icon(Icon::Send, tr("Send")).c_str()).x + 24;
  bool enter = ImGui::InputTextMultiline("##msg", chat_input_, sizeof chat_input_, ImVec2(-send_w - 6, input_h),
                                         ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_CtrlEnterForNewLine);
  ImGui::SameLine(0, 6);
  bool empty = core::trim(chat_input_).empty();
  ImGui::BeginDisabled(busy || empty);
  ImGui::PushStyleColor(ImGuiCol_Button, theme::accent(0.9f));
  ImGui::PushStyleColor(ImGuiCol_ButtonHovered, theme::accent(1.0f));
  bool send = ImGui::Button(with_icon(Icon::Send, tr("Send")).c_str(), ImVec2(send_w, input_h));
  ImGui::PopStyleColor(2);
  ImGui::EndDisabled();
  if ((send || enter) && !busy && !empty) {
    if (chat_->send(chat_mode_ == 1 ? ChatMode::Bug : ChatMode::Task, chat_input_)) chat_input_[0] = '\0';
    chat_focus_ = true;
  } else if (enter) {
    chat_focus_ = true;  // keep typing while the previous command runs
  }
  ui::muted("%s", busy ? tr("A command is running: wait for it or stop it.") : tr("Enter sends, Ctrl+Enter starts a new line."));
}

// --- live feed --------------------------------------------------------------------------------

void Ui::live_step(const FeedRun& run, const FeedStep& s, std::size_t i) {
  ImGui::PushID(static_cast<int>(i));
  std::string key = run.journal + "#" + std::to_string(i);
  bool open = live_open_.contains(key);
  auto toggle = [&](bool more) {
    if (!more) return;
    if (ImGui::SmallButton(open ? tr("less") : tr("more"))) {
      if (open) {
        live_open_.erase(key);
      } else {
        live_open_.insert(key);
      }
    }
    ui::hand_on_hover();
  };
  switch (s.kind) {
    case FeedStep::Kind::Start: {
      ImGui::TextColored(kAgent, "%s", with_icon(Icon::Play, fmt::format("{} {} · {}", tr("start"), when(s.ts), role_label(s.title))).c_str());
      auto [head, more] = head_lines(s.text, open ? 100000 : 4);
      ImGui::Indent();
      chat_sel_.text("task", head, kResult, theme::mono_font());
      toggle(more || open);
      ImGui::Unindent();
      break;
    }
    case FeedStep::Kind::Think: {
      std::string title = with_icon(Icon::Info, tr("thinks"));
      if (s.reasoning_tokens > 0) title += fmt::format("  ({} {})", s.reasoning_tokens, tr("tokens"));
      ImGui::TextColored(kThink, "%s", title.c_str());
      auto [head, more] = head_lines(s.text, open ? 100000 : 3);
      ImGui::Indent();
      chat_sel_.text("think", head, kThink);
      toggle(more || open);
      ImGui::Unindent();
      break;
    }
    case FeedStep::Kind::Text: {
      ImGui::Indent();
      chat_sel_.text("text", s.text, ImGui::GetStyleColorVec4(ImGuiCol_Text));
      ImGui::Unindent();
      break;
    }
    case FeedStep::Kind::Call: {
      ImGui::TextColored(kCall, "%s", (std::string("→ ") + s.title).c_str());
      auto [head, more] = head_lines(s.text, open ? 100000 : (s.title == "report" || s.title == "dispatch" ? 12 : 3));
      ImGui::Indent();
      chat_sel_.text("args", head, ImGui::GetStyleColorVec4(ImGuiCol_Text), theme::mono_font());
      toggle(more || open);
      ImGui::Unindent();
      break;
    }
    case FeedStep::Kind::Result: {
      ImVec4 c = s.error ? rgb(241, 110, 110) : kResult;
      ImGui::TextColored(c, "%s", (std::string("← ") + s.title + (s.error ? "  " + std::string(tr("error")) : "")).c_str());
      auto [head, more] = head_lines(s.text, open ? 100000 : (s.error ? 6 : 2));
      ImGui::Indent();
      chat_sel_.text("res", head, c, theme::mono_font());
      toggle(more || open);
      ImGui::Unindent();
      break;
    }
    case FeedStep::Kind::End:
      ImGui::TextColored(kAgent, "%s", with_icon(Icon::Check, fmt::format("{} {}", tr("end"), s.text.empty() ? s.title : s.text)).c_str());
      break;
    case FeedStep::Kind::Error:
      ImGui::TextColored(rgb(241, 96, 96), "%s", icon(Icon::Error));
      ImGui::SameLine();
      chat_sel_.text("err", s.text, rgb(241, 96, 96));
      break;
    case FeedStep::Kind::Cancelled:
      ImGui::TextColored(rgb(240, 200, 30), "%s", with_icon(Icon::Stop, tr("stopped by the user")).c_str());
      break;
  }
  ImGui::PopID();
}

void Ui::chat_live(const Model* m) {
  if (m == nullptr || !feed_) {
    ui::muted("%s", tr("indexing..."));
    return;
  }
  // The run to show: the chosen one, or (following) the newest active run.
  if (live_follow_ && !m->active.empty()) {
    const auto* newest = &m->active.front();
    for (const auto& a : m->active) {
      if (a.started > newest->started) newest = &a;
    }
    auto j = newest->journal.filename().string();
    if (j != live_journal_) {
      live_journal_ = j;
      live_seen_ = 0;
    }
  }
  if (live_journal_.empty() && !m->runs.empty()) live_journal_ = m->runs.back().journal;
  float avail_w = ImGui::GetContentRegionAvail().x;
  float avail_h = ImGui::GetContentRegionAvail().y;
  if (live_list_w_ <= 0) live_list_w_ = std::min(260.0f, avail_w * 0.32f);
  live_list_w_ = std::clamp(live_list_w_, 160.0f, std::max(160.0f, avail_w - 260.0f));
  // Left: active runs, then recent ones.
  ImGui::BeginChild("runs", ImVec2(live_list_w_, avail_h));
  ui::toggle("follow", Icon::Eye, tr("Follow"), &live_follow_, tr("Always show the agent that works now"));
  std::set<std::string> active;
  for (const auto& a : m->active) active.insert(a.journal.filename().string());
  auto row = [&](const std::string& journal, const std::string& title, const std::string& sub, bool is_active) {
    ImGui::PushID(journal.c_str());
    bool sel = journal == live_journal_;
    float line = ImGui::GetTextLineHeight();
    if (ImGui::Selectable("##r", sel, 0, ImVec2(0, line * 2.1f))) {
      live_journal_ = journal;
      live_follow_ = false;
      live_seen_ = 0;
    }
    ui::hand_on_hover();
    ImVec2 mn = ImGui::GetItemRectMin();
    ImVec2 mx = ImGui::GetItemRectMax();
    auto* dl = ImGui::GetWindowDrawList();
    float a = is_active ? static_cast<float>(0.6 + 0.4 * std::sin(pulse_ * 5.0)) : 1.0f;
    ImVec4 dot = is_active ? rgb(240, 200, 30, a) : rgb(120, 120, 120);
    dl->AddCircleFilled(ImVec2(mn.x + 6, mn.y + line * 0.55f), 3.5f, ImGui::GetColorU32(dot));
    dl->PushClipRect(mn, mx, true);
    dl->AddText(ImVec2(mn.x + 14, mn.y + 1), ImGui::GetColorU32(ImGuiCol_Text), title.c_str());
    dl->AddText(ImVec2(mn.x + 14, mn.y + line + 2), ImGui::GetColorU32(ImGuiCol_TextDisabled), sub.c_str());
    dl->PopClipRect();
    ImGui::PopID();
  };
  if (!m->active.empty()) {
    ui::panel_title(tr("WORKING NOW"));
    for (const auto& a : m->active) {
      row(a.journal.filename().string(), role_label(a.role) + " · " + a.block,
          (a.ticket.empty() ? std::string() : a.ticket + " · ") + duration_text(core::unix_now() - a.started), true);
    }
    ImGui::Spacing();
  }
  ui::panel_title(tr("RECENT RUNS"));
  int shown = 0;
  for (auto it = m->runs.rbegin(); it != m->runs.rend() && shown < 60; ++it) {
    if (it->journal.empty() || active.contains(it->journal)) continue;
    ++shown;
    row(it->journal, role_label(it->role) + " · " + it->block,
        fmt::format("{} {} · {} · ${:.3f}", it->ticket, when(it->ts), it->outcome, it->cost_usd), false);
  }
  ImGui::EndChild();
  ImGui::SameLine(0, 2);
  ui::vsplitter("live_split", &live_list_w_, 160.0f, 260.0f, avail_w, avail_h);
  ImGui::SameLine(0, 2);
  // Right: the steps of the run.
  ImGui::BeginChild("steps", ImVec2(0, avail_h), ImGuiChildFlags_Borders);
  if (live_journal_.empty()) {
    ImGui::PushTextWrapPos(0);
    ui::muted("%s", tr("No agent runs yet. They show here step by step while they work — also when started from a terminal."));
    ImGui::PopTextWrapPos();
    ImGui::EndChild();
    return;
  }
  const FeedRun& run = feed_->load(live_journal_);
  bool is_active = active.contains(live_journal_);
  ImGui::PushFont(theme::heading_font());
  ImGui::TextUnformatted((role_label(run.role) + " · " + run.block).c_str());
  ImGui::PopFont();
  ImGui::SameLine();
  ui::muted("%s", fmt::format("{}{} · {}", run.model, run.attempt > 1 ? fmt::format(" #{}", run.attempt) : "", run.ticket).c_str());
  std::string state = is_active ? fmt::format(fmt::runtime(tr("working {}")), duration_text(core::unix_now() - run.start))
                                : fmt::format("{} · {} · ${:.4f}", run.outcome, duration_text(run.end - run.start), run.cost);
  ImGui::TextColored(is_active ? rgb(240, 200, 30) : rgb(150, 150, 150), "%s", state.c_str());
  ImGui::SameLine();
  ui::muted("%s", fmt::format("{} {}", when(run.start), run.end > 0 ? "→ " + when(run.end) : "").c_str());
  ImGui::SameLine();
  std::string all;
  for (const auto& s : run.steps) all += s.title + (s.title.empty() ? "" : ": ") + s.text + "\n\n";
  ui::copy_button("copy_run", all);
  if (!run.ticket.empty()) {
    ImGui::SameLine(0, 4);
    if (ui::icon_button("run_ticket", Icon::Ticket, tr("Show the ticket in the panel below"), false, run.ticket.c_str())) {
      req_.ticket = run.ticket;
      bottom_select_ = "tickets";
      settings_.show_bottom = true;
    }
  }
  if (is_active) {
    for (const auto& a : m->active) {
      if (a.journal.filename().string() != live_journal_) continue;
      ImGui::SameLine(0, 4);
      if (a.stopping) {
        ui::muted("%s", tr("stopping after the current step..."));
      } else if (ui::icon_button("stop_run", Icon::Stop, tr("Stop this agent: it ends after its current step, its journal is finished, "
                                                            "the ticket stays open"),
                                 false, tr("Stop"))) {
        w_.stop_run(a.journal);
      }
    }
  }
  ImGui::Separator();
  ImGui::BeginChild("feed", ImVec2(0, 0));
  chat_sel_.begin();
  bool at_bottom = ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 4;
  for (std::size_t i = 0; i < run.steps.size(); ++i) {
    live_step(run, run.steps[i], i);
    ImGui::Spacing();
  }
  if (is_active) {
    float a = static_cast<float>(0.6 + 0.4 * std::sin(pulse_ * 5.0));
    ImGui::TextColored(rgb(240, 200, 30, a), "%s", with_icon(Icon::Agent, tr("waiting for the model...")).c_str());
  }
  // New steps scroll into view while the reader is at the bottom (or just opened the run).
  if (run.steps.size() != live_seen_ && (at_bottom || live_seen_ == 0)) ImGui::SetScrollHereY(1.0f);
  live_seen_ = run.steps.size();
  chat_sel_.end();
  ImGui::EndChild();
  ImGui::EndChild();
}

// --- window -----------------------------------------------------------------------------------

void Ui::draw_chat_window(const Model* m) {
  if (!show_chat_ || !chat_) return;
  const ImGuiViewport* vp = ImGui::GetMainViewport();
  ImVec2 size(std::min(vp->WorkSize.x * 0.55f, ImGui::GetFontSize() * 52), std::min(vp->WorkSize.y * 0.8f, ImGui::GetFontSize() * 48));
  ImGui::SetNextWindowSize(size, ImGuiCond_FirstUseEver);
  ImGui::SetNextWindowPos(ImVec2(vp->WorkPos.x + vp->WorkSize.x - size.x - 24, vp->WorkPos.y + vp->WorkSize.y - size.y - 48),
                          ImGuiCond_FirstUseEver);
  if (chat_window_focus_) {
    ImGui::SetNextWindowFocus();
    chat_window_focus_ = false;
  }
  ImGui::PushStyleColor(ImGuiCol_WindowBg, theme::col(theme::kSideBg));
  ImGui::PushStyleColor(ImGuiCol_Border, IM_COL32(80, 80, 86, 255));
  ImGui::PushStyleColor(ImGuiCol_TitleBg, IM_COL32(44, 44, 48, 255));
  ImGui::PushStyleColor(ImGuiCol_TitleBgActive, IM_COL32(52, 52, 58, 255));
  ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 1.0f);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 6.0f);
  // A floating window over everything else: it never docks into the layout (ADR 0021).
  bool open = ImGui::Begin((with_icon(Icon::Chat, tr("Orchestrator")) + "###chat_window").c_str(), &show_chat_,
                           ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoDocking);
  ImGui::PopStyleVar(2);
  ImGui::PopStyleColor(4);
  if (!open) {
    ImGui::End();
    return;
  }
  if (chat_->serial() != chat_serial_) {
    chat_serial_ = chat_->serial();
    chat_cache_ = chat_->entries();
    chat_scroll_ = true;
  }
  bool busy = chat_->busy();
  // Header: what a message becomes, Auto, and the state of the running command.
  ImGui::AlignTextToFramePadding();
  ui::muted("%s", tr("Message:"));
  ImGui::SameLine();
  bool task = chat_mode_ == 0;
  bool bug = chat_mode_ == 1;
  if (ui::toggle("m_task", Icon::Plus, tr("task"), &task, tr("A feature or change: karkas task \"...\""))) chat_mode_ = 0;
  ImGui::SameLine(0, 2);
  if (ui::toggle("m_bug", Icon::Warning, tr("bug"), &bug, tr("A symptom to find and fix: karkas debug \"...\""))) chat_mode_ = 1;
  ImGui::SameLine(0, 12);
  mode_switch("chat_mode");
  ImGui::SameLine(0, 12);
  bool autop = cfg_.auto_approve;
  if (ui::toggle("auto", Icon::Bolt, tr("Auto"), &autop,
                 tr("Auto: schema changes the agents propose are approved at once and the work goes on without you "
                    "([agents] auto_approve in karkas.toml)"))) {
    set_auto(autop);
  }
  ImGui::SameLine(ImGui::GetContentRegionMax().x - ImGui::GetFrameHeight() * 1.3f);
  ImGui::BeginDisabled(chat_cache_.empty());
  if (ui::icon_button("chat_clear", Icon::Trash, tr("Clear the conversation (tickets and logs stay)"))) chat_->clear();
  ImGui::EndDisabled();
  if (busy) {
    float a = static_cast<float>(0.6 + 0.4 * std::sin(pulse_ * 5.0));
    std::string t = chat_->current_ticket();
    ImGui::AlignTextToFramePadding();
    ImGui::TextColored(rgb(240, 200, 30, a), "%s", with_icon(Icon::Agent, t.empty() ? std::string(tr("working...")) : fmt::format(fmt::runtime(tr("working on {}...")), t)).c_str());
    ImGui::SameLine();
    auto stopping = chat_->stopping_for();
    if (!stopping) {
      if (ui::icon_button("chat_stop", Icon::Stop, tr("Stop like Ctrl+C: the agents finish their journals, the ticket stays open"), false,
                          tr("Stop"))) {
        chat_->stop();
      }
    } else if (*stopping > kKillAfterS) {
      if (ui::icon_button("chat_kill", Icon::Error, tr("The command does not stop: end it at once (SIGKILL)"), false, tr("Kill"))) chat_->kill();
    } else {
      ui::muted("%s", tr("stopping..."));
    }
  }
  // Mode mcp: the agents wait for their model — an MCP client. Say who waits and what to do.
  if (cfg_.mcp()) {
    const auto& waiting = mcp_waiting();
    if (!waiting.empty()) {
      std::int64_t oldest = core::unix_now();
      bool untaken = false;
      for (const auto& t : waiting) {
        if (!t.taken) {
          untaken = true;
          oldest = std::min(oldest, t.ts);
        }
      }
      float a = static_cast<float>(0.6 + 0.4 * std::sin(pulse_ * 5.0));
      ImGui::PushTextWrapPos(0);
      ImGui::TextColored(rgb(240, 200, 30, a), "%s",
                         with_icon(Icon::Agent, fmt::format(fmt::runtime(tr("{} turns of the agents wait for their model (MCP): {}")),
                                                            waiting.size(), waiting.front().role + " · " + waiting.front().block))
                             .c_str());
      if (untaken && core::unix_now() - oldest >= 3) {
        ui::muted("%s", tr("Nobody serves them yet. In Claude Code of this project say: \"serve the karkas agents\" — it takes the "
                           "turns with kk_agent_next and hands each run to the subagent of its level (karkas-<tier>)."));
      }
      ImGui::PopTextWrapPos();
    }
  }
  int working = m != nullptr ? static_cast<int>(m->active.size()) : 0;
  if (ImGui::BeginTabBar("##chat_tabs")) {
    ImGuiTabItemFlags f0 = chat_tab_select_ == 0 ? ImGuiTabItemFlags_SetSelected : 0;
    ImGuiTabItemFlags f1 = chat_tab_select_ == 1 ? ImGuiTabItemFlags_SetSelected : 0;
    chat_tab_select_ = -1;
    if (ImGui::BeginTabItem((with_icon(Icon::Chat, tr("Dialog")) + "###dialog").c_str(), nullptr, f0)) {
      chat_tab_ = 0;
      chat_dialog(m);
      ImGui::EndTabItem();
    }
    std::string live = with_icon(Icon::Agent, tr("Live")) + (working > 0 ? fmt::format(" ({})", working) : "") + "###live";
    if (ImGui::BeginTabItem(live.c_str(), nullptr, f1)) {
      chat_tab_ = 1;
      chat_live(m);
      ImGui::EndTabItem();
    }
    ImGui::EndTabBar();
  }
  ImGui::End();
}

}  // namespace kk::gui
