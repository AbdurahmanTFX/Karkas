#include "feed.hpp"

#include <fstream>
#include <nlohmann/json.hpp>

namespace kk::gui {

namespace {

using json = nlohmann::json;

std::string str(const json& j, const char* k) {
  if (!j.is_object() || !j.contains(k) || j[k].is_null()) return {};
  return j[k].is_string() ? j[k].get<std::string>() : j[k].dump();
}

std::int64_t num(const json& j, const char* k) {
  if (!j.is_object() || !j.contains(k) || !j[k].is_number()) return 0;
  return j[k].get<std::int64_t>();
}

// The arguments of a tool call as a person reads them: the path of a file tool, the block of a
// dispatch with its reason, otherwise compact JSON.
std::string args_text(const std::string& name, const json& in) {
  if (!in.is_object()) return in.dump();
  if (name == "read_file" || name == "list_dir") {
    std::string s = str(in, "path");
    if (in.contains("offset") || in.contains("limit")) s += " [" + str(in, "offset") + "+" + str(in, "limit") + "]";
    return s.empty() ? "." : s;
  }
  if (name == "write_file") return str(in, "path") + " (" + std::to_string(str(in, "content").size()) + " bytes)\n" + str(in, "content");
  if (name == "edit_file") return str(in, "path") + "\n- " + str(in, "old") + "\n+ " + str(in, "new");
  if (name == "grep") return "/" + str(in, "pattern") + "/ in " + (str(in, "path").empty() ? "." : str(in, "path"));
  if (name == "dispatch") return "→ " + str(in, "block") + " (" + str(in, "difficulty") + "): " + str(in, "why");
  if (name == "report") {
    std::string s = str(in, "status");
    for (const char* k : {"cause", "fix", "files", "tests", "api"}) {
      auto v = str(in, k);
      if (!v.empty()) s += std::string("\n") + k + ": " + v;
    }
    if (auto p = str(in, "proposal"); !p.empty()) s += "\nproposal:\n" + p;
    return s;
  }
  if (name == "propose_change") return str(in, "reason") + "\n" + str(in, "diff");
  if (name == "ask") return str(in, "block") + ": " + str(in, "question");
  if (name == "answer") return str(in, "text");
  if (name == "close" || name == "escalate") return str(in, "verdict") + (str(in, "note").empty() ? "" : " " + str(in, "note"));
  return in.dump();
}

}  // namespace

void feed_line(FeedRun& run, const std::string& line) {
  auto j = json::parse(line, nullptr, false);
  if (!j.is_object()) return;
  auto type = str(j, "type");
  if (type == "run_start") {
    run.ticket = str(j, "ticket");
    run.role = str(j, "role");
    run.block = str(j, "block");
    run.model = str(j, "model");
    run.tier = str(j, "tier");
    run.attempt = static_cast<int>(num(j, "attempt"));
    run.start = num(j, "ts");
    return;
  }
  if (type == "run_end") {
    run.end = num(j, "ts");
    run.outcome = str(j, "outcome");
    run.cost = j.value("cost_usd", 0.0);
    run.in_tokens = num(j, "input_tokens");
    run.out_tokens = num(j, "output_tokens");
    run.cache_read = num(j, "cache_read_tokens");
    return;
  }
  if (type != "step") return;
  auto kind = str(j, "kind");
  const json& d = j.contains("data") ? j["data"] : json::object();
  FeedStep base;
  base.n = static_cast<int>(num(j, "n"));
  base.ts = num(j, "ts");
  if (kind == "start") {
    run.task = str(d, "task");
    FeedStep s = base;
    s.kind = FeedStep::Kind::Start;
    s.title = str(d, "role");
    s.text = run.task;
    run.steps.push_back(std::move(s));
  } else if (kind == "response") {
    std::int64_t out = d.contains("usage") ? num(d["usage"], "output") : 0;
    std::int64_t reasoning = d.contains("usage") ? num(d["usage"], "reasoning") : 0;
    bool first = true;
    for (const auto& c : d.value("content", json::array())) {
      FeedStep s = base;
      if (first) {
        s.out_tokens = out;
        s.reasoning_tokens = reasoning;
        first = false;
      }
      auto t = str(c, "type");
      if (t == "reasoning" || t == "thinking") {
        s.kind = FeedStep::Kind::Think;
        s.text = str(c, t == "thinking" ? "thinking" : "text");
      } else if (t == "text") {
        s.kind = FeedStep::Kind::Text;
        s.text = str(c, "text");
      } else if (t == "tool_use") {
        s.kind = FeedStep::Kind::Call;
        s.title = str(c, "name");
        s.text = args_text(s.title, c.contains("input") ? c["input"] : json::object());
      } else {
        continue;
      }
      if (s.text.empty() && s.kind != FeedStep::Kind::Call) continue;
      run.steps.push_back(std::move(s));
    }
  } else if (kind == "tool") {
    FeedStep s = base;
    s.kind = FeedStep::Kind::Result;
    s.title = str(d, "name");
    s.text = str(d, "result");
    s.error = d.value("is_error", false);
    run.steps.push_back(std::move(s));
  } else if (kind == "end") {
    FeedStep s = base;
    s.kind = FeedStep::Kind::End;
    s.title = str(d, "outcome");
    s.text = str(d, "terminal");
    run.steps.push_back(std::move(s));
  } else if (kind == "error") {
    FeedStep s = base;
    s.kind = FeedStep::Kind::Error;
    s.text = str(d, "message");
    run.steps.push_back(std::move(s));
  } else if (kind == "cancelled") {
    FeedStep s = base;
    s.kind = FeedStep::Kind::Cancelled;
    s.text = str(d, "message");
    run.steps.push_back(std::move(s));
  }
}

const FeedRun& JournalFeed::load(const std::string& journal) {
  auto& e = cache_[journal];
  e.run.journal = journal;
  auto path = dir_ / journal;
  std::error_code ec;
  auto size = std::filesystem::file_size(path, ec);
  if (ec || size <= e.offset) return e.run;  // nothing new (or gone)
  std::ifstream in(path, std::ios::binary);
  if (!in) return e.run;
  in.seekg(static_cast<std::streamoff>(e.offset));
  std::string chunk(static_cast<std::size_t>(size - e.offset), '\0');
  in.read(chunk.data(), static_cast<std::streamsize>(chunk.size()));
  chunk.resize(static_cast<std::size_t>(in.gcount()));
  e.offset += chunk.size();
  std::string buf = std::move(e.partial) + chunk;
  e.partial.clear();
  std::size_t pos = 0;
  while (true) {
    auto nl = buf.find('\n', pos);
    if (nl == std::string::npos) {
      e.partial = buf.substr(pos);  // completed by a later append
      break;
    }
    if (nl > pos) feed_line(e.run, buf.substr(pos, nl - pos));
    pos = nl + 1;
  }
  return e.run;
}

}  // namespace kk::gui
