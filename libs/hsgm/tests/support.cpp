#include "support.hpp"

#include <algorithm>

namespace kk::hsgm::test {

namespace {

std::string dir_of(std::string id) {
  std::replace(id.begin(), id.end(), '.', '/');
  return id;
}

std::string last_segment(const std::string& id) {
  auto dot = id.rfind('.');
  return dot == std::string::npos ? id : id.substr(dot + 1);
}

}  // namespace

Make& Make::block(const std::string& id, const std::string& kind) {
  index::BlockRow b;
  b.id = id;
  b.kind = kind;
  auto dot = id.rfind('.');
  b.parent = dot == std::string::npos ? "" : id.substr(0, dot);
  b.depth = static_cast<int>(std::count(id.begin(), id.end(), '.')) + 1;
  b.dir = dir_of(id);
  auto& blocks = in.snap.blocks;
  blocks.push_back(b);
  std::sort(blocks.begin(), blocks.end(), [](const auto& x, const auto& y) { return x.id < y.id; });
  if (b.parent.empty()) {
    in.snap.top_level.push_back(id);
  } else {
    for (auto& p : blocks) {
      if (p.id == b.parent) p.children.push_back(id);
    }
  }
  return *this;
}

Make& Make::link(const std::string& kind, const std::string& src, const std::string& dst, const std::string& src_port,
                 const std::string& dst_port) {
  index::LinkRow l;
  l.id = static_cast<std::int64_t>(in.snap.links.size()) + 1;
  l.kind = kind;
  l.src_block = src;
  l.src_port = src_port;
  l.dst_block = dst;
  l.dst_port = dst_port;
  l.declared_in = (kind == "data" || kind == "event") ? dst : src;
  in.snap.links.push_back(l);
  return *this;
}

Make& Make::suite(const std::string& block, const std::string& kind) {
  index::CodeFile f;
  f.block = block;
  f.role = "test";
  if (kind == "integration") {
    f.path = "tests/integration/" + block + "/" + last_segment(block) + "_integration.cpp";
  } else if (kind == "contract") {
    f.path = dir_of(block) + "/tests/contract/" + last_segment(block) + "_contract.cpp";
  } else {
    f.path = dir_of(block) + "/tests/unit/" + last_segment(block) + "_test.cpp";
  }
  in.files.push_back(f);
  std::sort(in.files.begin(), in.files.end(), [](const auto& x, const auto& y) { return x.path < y.path; });
  return *this;
}

Make& Make::result(const std::string& block, index::TestCounts contract, index::TestCounts unit,
                   index::TestCounts integration, bool fresh, bool build_failed) {
  index::TestRow r;
  r.block = block;
  r.contract = contract;
  r.unit = unit;
  r.integration = integration;
  r.fresh = fresh;
  r.build_failed = build_failed;
  auto& tests = in.snap.tests;
  std::erase_if(tests, [&](const auto& t) { return t.block == block; });
  tests.push_back(r);
  std::sort(tests.begin(), tests.end(), [](const auto& x, const auto& y) { return x.block < y.block; });
  return *this;
}

Make& Make::drift(const std::string& block, const std::string& code) {
  index::DriftRow d;
  d.block = block;
  d.code = code;
  d.severity = "E";
  in.snap.drift.push_back(d);
  return *this;
}

Make& Make::inv(const std::string& block, const std::string& text) {
  in.snap.invariants[block].push_back(text);
  return *this;
}

Make& Make::test_case(const std::string& block, const std::string& name) {
  in.test_cases[block].push_back(name);
  return *this;
}

Make& Make::run(const std::string& role, const std::string& block, double cost_usd, std::int64_t input_tokens,
                bool session_cost) {
  index::RunRow r;
  r.id = static_cast<std::int64_t>(in.runs.size()) + 1;
  r.role = role;
  r.block = block;
  r.cost_usd = cost_usd;
  r.input_tokens = input_tokens;
  r.session_cost = session_cost;
  in.runs.push_back(r);
  return *this;
}

}  // namespace kk::hsgm::test
