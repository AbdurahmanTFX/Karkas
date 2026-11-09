// The orchestrator chat window of karkas-gui (docs/adr/0019): a message becomes
// `karkas task` / `karkas debug` / a "/command" run by the Chat (chat.hpp); its progress,
// the ticket it ends with and the next steps (continue, approve a change) show here.
#include <fmt/format.h>
#include <imgui.h>

#include <algorithm>
#include <cstdio>

#include "i18n.hpp"
#include "icons.hpp"
#include "kk/core/text.hpp"
#include "theme.hpp"
#include "ui.hpp"
#include "widgets.hpp"

namespace kk::gui {

namespace {

constexpr double kKillAfterS = 5.0;

const ImVec4 kUser = rgb(55, 148, 255);
const ImVec4 kAgent = rgb(78, 201, 176);

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

}  // namespace

void Ui::chat_entry(const Model* m, const ChatEntry& e, std::size_t i) {
  ImGui::PushID(static_cast<int>(i));
  float wrap = ImGui::GetContentRegionAvail().x - 24;
  switch (e.kind) {
    case ChatEntry::Kind::User: {
      ImGui::Spacing();
      bubble(with_alpha(kUser, 0.16f), wrap, [&] {
        const char* mode = e.mode == "bug" ? tr("bug") : e.mode == "command" ? tr("command") : tr("task");
        ImGui::TextColored(kUser, "%s", with_icon(Icon::Chat, fmt::format("{} · {}", tr("you"), mode)).c_str());
        ImGui::TextUnformatted(e.text.c_str());
      });
      break;
    }
    case ChatEntry::Kind::Progress: {
      ImGui::PushFont(theme::mono_font());
      ImGui::TextColored(rgb(150, 150, 150), "· %s", e.text.c_str());
      ImGui::PopFont();
      break;
    }
    case ChatEntry::Kind::Log: {
      bool warn = core::contains(e.text, " warn") || core::contains(e.text, " error");
      ImGui::PushFont(theme::mono_font());
      ImGui::PushTextWrapPos(0);
      ImGui::TextColored(warn ? rgb(230, 160, 60) : rgb(120, 120, 120), "%s", e.text.c_str());
      ImGui::PopTextWrapPos();
      ImGui::PopFont();
      break;
    }
    case ChatEntry::Kind::Result: {
      ImGui::Spacing();
      ImGui::TextColored(kAgent, "%s", with_icon(Icon::Agent, tr("orchestrator")).c_str());
      ImGui::PushStyleColor(ImGuiCol_ChildBg, theme::col(theme::kEditorBg));
      ImGui::BeginChild("result", ImVec2(0, 0), ImGuiChildFlags_AutoResizeY | ImGuiChildFlags_Borders, ImGuiWindowFlags_HorizontalScrollbar);
      ui::ksl_text(e.text);
      ImGui::EndChild();
      ImGui::PopStyleColor();
      // Next steps of the protocol: the ticket, its continuation, changes waiting for approval.
      if (!e.ticket.empty()) {
        if (ui::icon_button("open_t", Icon::Ticket, tr("Show the ticket in the panel below"), false, e.ticket.c_str())) {
          req_.ticket = e.ticket;
          bottom_select_ = "tickets";
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
        bool pending = false;
        if (m != nullptr) {
          for (const auto& ch : m->changes) pending |= ch.id == c && ch.status.value == "pending";
        }
        ImGui::SameLine(0, 4);
        ImGui::PushID(c.c_str());
        if (ui::icon_button("show_c", Icon::Changes, tr("Show the proposed schema change"), false, c.c_str())) {
          side_ = SideView::Changes;
          side_visible_ = true;
          req_.change = c;
        }
        if (pending) {
          ImGui::SameLine(0, 4);
          if (ui::icon_button("approve_c", Icon::Check, tr("Approve the change: the cards are written"), false, tr("Approve"))) {
            w_.approve_change(c);
          }
        }
        ImGui::PopID();
      }
      break;
    }
    case ChatEntry::Kind::Error: {
      ImGui::PushTextWrapPos(0);
      ImGui::TextColored(rgb(241, 96, 96), "%s %s", icon(Icon::Error), e.text.c_str());
      ImGui::PopTextWrapPos();
      break;
    }
    case ChatEntry::Kind::Info: {
      ImGui::PushTextWrapPos(0);
      ImGui::TextColored(rgb(200, 180, 110), "%s %s", icon(Icon::Info), tr(e.text.c_str()));
      ImGui::PopTextWrapPos();
      break;
    }
  }
  ImGui::PopID();
}

void Ui::draw_chat_window(const Model* m) {
  if (!show_chat_ || !chat_) return;
  const ImGuiViewport* vp = ImGui::GetMainViewport();
  ImVec2 size(std::min(vp->WorkSize.x * 0.45f, ImGui::GetFontSize() * 40), std::min(vp->WorkSize.y * 0.75f, ImGui::GetFontSize() * 44));
  ImGui::SetNextWindowSize(size, ImGuiCond_FirstUseEver);
  ImGui::SetNextWindowPos(ImVec2(vp->WorkPos.x + vp->WorkSize.x - size.x - 24, vp->WorkPos.y + vp->WorkSize.y - size.y - 48),
                          ImGuiCond_FirstUseEver);
  ImGui::PushStyleColor(ImGuiCol_WindowBg, theme::col(theme::kSideBg));
  ImGui::PushStyleColor(ImGuiCol_Border, IM_COL32(80, 80, 86, 255));
  ImGui::PushStyleColor(ImGuiCol_TitleBg, IM_COL32(44, 44, 48, 255));
  ImGui::PushStyleColor(ImGuiCol_TitleBgActive, IM_COL32(52, 52, 58, 255));
  ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 1.0f);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 6.0f);
  bool open = ImGui::Begin((with_icon(Icon::Chat, tr("Orchestrator")) + "###chat_window").c_str(), &show_chat_,
                           ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoCollapse);
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
  // Header: what a message becomes, and the state of the running command.
  ImGui::AlignTextToFramePadding();
  ui::muted("%s", tr("Message:"));
  ImGui::SameLine();
  bool task = chat_mode_ == 0;
  bool bug = chat_mode_ == 1;
  if (ui::toggle("m_task", Icon::Plus, tr("task"), &task, tr("A feature or change: karkas task \"...\""))) chat_mode_ = 0;
  ImGui::SameLine(0, 2);
  if (ui::toggle("m_bug", Icon::Warning, tr("bug"), &bug, tr("A symptom to find and fix: karkas debug \"...\""))) chat_mode_ = 1;
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
  ImGui::Separator();
  // Conversation.
  float input_h = ImGui::GetTextLineHeight() * 4 + ImGui::GetStyle().FramePadding.y * 2;
  float footer = input_h + ImGui::GetFrameHeightWithSpacing() + ImGui::GetTextLineHeightWithSpacing() + 8;
  ImGui::BeginChild("conversation", ImVec2(0, -footer), ImGuiChildFlags_None);
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
  if (chat_scroll_) {
    ImGui::SetScrollHereY(1.0f);
    chat_scroll_ = false;
  }
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
  ImGui::End();
}

}  // namespace kk::gui
