#include <doctest/doctest.h>

#include <algorithm>

#include "kk/code/analyzer.hpp"
#include "kk/code/lexical.hpp"
#include "kk/core/lang.hpp"

using namespace kk;

namespace {

index::FileFacts facts(const char* lang, std::string_view text, bool iface = false) {
  const auto* l = core::language_by_id(lang);
  REQUIRE(l != nullptr);
  return code::analyze_lexical(*l, text, iface);
}

const index::SymbolFact* sym(const index::FileFacts& f, std::string_view name, std::string_view scope = "~") {
  for (const auto& s : f.symbols) {
    if (s.name == name && (scope == "~" || s.scope == scope)) return &s;
  }
  return nullptr;
}

bool has_ref(const index::FileFacts& f, std::string_view kind, std::string_view target) {
  return std::any_of(f.refs.begin(), f.refs.end(), [&](const index::RefFact& r) { return r.kind == kind && r.target == target; });
}

std::vector<std::string> resolve(const char* lang, const std::string& from, const std::string& target, const std::vector<std::string>& files) {
  code::PathIndex idx(files);
  return code::resolve_import(*core::language_by_id(lang), from, target, idx);
}

}  // namespace

TEST_CASE("lexical: masking keeps lines, drops comments and string contents (ADR 0051)") {
  const auto* py = core::language_by_id("python");
  auto m = code::mask_code(*py, "x = 'a # b'  # c\ns = \"\"\"doc\ndef f():\"\"\"\n", true);
  CHECK(m.find("# c") == std::string::npos);
  CHECK(m.find("def f") == std::string::npos);
  CHECK(std::count(m.begin(), m.end(), '\n') == 3);
  const auto* sh = core::language_by_id("bash");
  auto s = code::mask_code(*sh, "echo $# x # note\ncat <<EOF\nfoo() {\nEOF\nbar\n", true);
  CHECK(s.find("$#") != std::string::npos);  // '#' inside a word is no comment
  CHECK(s.find("note") == std::string::npos);
  CHECK(s.find("foo()") == std::string::npos);  // heredoc body
  CHECK(s.find("bar") != std::string::npos);
  const auto* lua = core::language_by_id("lua");
  auto l = code::mask_code(*lua, "--[[ function a() end ]]\nlocal s = [[function b()]]\nfunction c() end\n", true);
  CHECK(l.find("function a") == std::string::npos);
  CHECK(l.find("function b") == std::string::npos);
  CHECK(l.find("function c") != std::string::npos);
}

TEST_CASE("lexical: Python classes, methods, privacy, imports, entry") {
  auto f = facts("python", R"(import os, json
from .ring import Ring
from ..util.paths import norm

class Mixer:
    def __init__(self, n):
        self.n = n

    def set_gain(self, track, gain=1.0):
        def clamp(x):
            return x
        return clamp(gain)

    def _private(self):
        pass

def mix(frames, gains):
    return Mixer(len(frames)).set_gain(0, gains[0])

if __name__ == "__main__":
    mix([], [1])
)");
  const auto* m = sym(f, "set_gain");
  REQUIRE(m != nullptr);
  CHECK(m->kind == "method");
  CHECK(m->scope == "Mixer");
  CHECK(m->params == 2);  // self is the object
  CHECK(m->exported);
  CHECK(sym(f, "clamp") == nullptr);  // local to a method
  REQUIRE(sym(f, "_private") != nullptr);
  CHECK_FALSE(sym(f, "_private")->exported);
  REQUIRE(sym(f, "mix") != nullptr);
  CHECK(sym(f, "mix")->kind == "function");
  CHECK(sym(f, "mix")->params == 2);
  CHECK(sym(f, "Mixer")->kind == "class");
  CHECK(sym(f, "main", "") != nullptr);  // the program's entry
  CHECK(has_ref(f, "import", "os"));
  CHECK(has_ref(f, "import", "json"));
  CHECK(has_ref(f, "import", ".ring"));
  CHECK(has_ref(f, "import", "..util.paths"));
  CHECK(has_ref(f, "call", "set_gain"));
  CHECK(has_ref(f, "call", "Mixer"));
  CHECK_FALSE(has_ref(f, "call", "if"));
}

TEST_CASE("lexical: JavaScript and TypeScript exports") {
  auto js = facts("javascript", R"(const fs = require('fs');
import { ring } from './ring.js';
export function mix(a, b) { return ring(a); }
function helper() {}
export const scale = (x, k) => x * k;
export class Track {
  constructor(n) { this.n = n; }
  gain(v) { return v; }
  #secret() {}
}
module.exports = { helper };
)");
  CHECK(sym(js, "mix")->exported);
  CHECK(sym(js, "mix")->params == 2);
  CHECK(sym(js, "helper")->exported);  // module.exports
  CHECK(sym(js, "scale")->exported);
  CHECK(sym(js, "scale")->params == 2);
  REQUIRE(sym(js, "gain") != nullptr);
  CHECK(sym(js, "gain")->scope == "Track");
  CHECK(sym(js, "gain")->exported);
  CHECK_FALSE(sym(js, "#secret")->exported);
  CHECK(has_ref(js, "import", "fs"));
  CHECK(has_ref(js, "import", "./ring.js"));
  auto ts = facts("typescript", R"(export interface Frame { len: number }
export type Gain = number;
enum Mode { A, B }
export class Bus {
  private level: number = 0;
  public push(f: Frame, g: Map<string, number>): void {}
  private reset(): void {}
}
)");
  CHECK(sym(ts, "Frame")->kind == "class");
  CHECK(sym(ts, "Gain")->kind == "alias");
  CHECK(sym(ts, "Mode")->kind == "enum");
  CHECK_FALSE(sym(ts, "Mode")->exported);
  CHECK(sym(ts, "push")->params == 2);  // the comma inside Map<string, number> is no parameter
  CHECK(sym(ts, "push")->exported);
  CHECK_FALSE(sym(ts, "reset")->exported);
}

TEST_CASE("lexical: Go functions, methods, exported names, import blocks") {
  auto f = facts("go", R"(package mix

import (
	"fmt"
	ring "example.com/audio/ring"
)
import "example.com/audio/types"

type (
	Gain float32
	Frame struct{ L, R float32 }
)

type Mixer struct {
	tracks int
}

type Source interface {
	Read(buf []Frame) (int, error)
}

func New(n int) *Mixer { return &Mixer{tracks: n} }

func (m *Mixer) SetGain(track int, g Gain) { fmt.Println(track) }

func (m Mixer) count() int { return m.tracks }
)");
  CHECK(sym(f, "New")->exported);
  CHECK(sym(f, "SetGain")->kind == "method");
  CHECK(sym(f, "SetGain")->scope == "Mixer");
  CHECK(sym(f, "SetGain")->params == 2);
  CHECK_FALSE(sym(f, "count")->exported);
  CHECK(sym(f, "Frame")->kind == "struct");
  CHECK(sym(f, "Gain")->kind == "alias");
  REQUIRE(sym(f, "Read") != nullptr);
  CHECK(sym(f, "Read")->scope == "Source");
  CHECK(has_ref(f, "import", "fmt"));
  CHECK(has_ref(f, "import", "example.com/audio/ring"));
  CHECK(has_ref(f, "import", "example.com/audio/types"));
  CHECK(has_ref(f, "call", "Println"));
}

TEST_CASE("lexical: Rust pub items, impl blocks, trait impls, use and mod") {
  auto f = facts("rust", R"(use crate::dsp::chain::Chain;
use std::fmt;
mod ring;

pub struct Mixer { tracks: usize }

pub trait Source {
    fn read(&mut self, buf: &mut [f32]) -> usize;
}

impl Mixer {
    pub fn new(tracks: usize) -> Self { Mixer { tracks } }
    pub fn set_gain(&mut self, track: usize, gain: f32) {}
    fn clamp(&self, x: f32) -> f32 { x }
}

impl fmt::Display for Mixer {
    fn fmt(&self, f: &mut fmt::Formatter) -> fmt::Result { Ok(()) }
}

fn main() { let m = Mixer::new(2); }
)");
  CHECK(sym(f, "new")->scope == "Mixer");
  CHECK(sym(f, "new")->exported);
  CHECK(sym(f, "set_gain")->params == 2);
  CHECK_FALSE(sym(f, "clamp")->exported);
  CHECK(sym(f, "fmt")->exported);  // a trait's method
  CHECK(sym(f, "read")->exported);
  CHECK(sym(f, "read")->params == 1);
  CHECK(sym(f, "Source")->kind == "class");
  CHECK(sym(f, "main", "") != nullptr);
  CHECK(has_ref(f, "import", "crate::dsp::chain::Chain"));
  CHECK(has_ref(f, "import", "mod ring"));
}

TEST_CASE("lexical: Java, C# (braces on the next line) and Kotlin") {
  auto j = facts("java", R"(package audio.mix;

import audio.ring.Ring;
import java.util.*;

public class Mixer {
    private int tracks;
    public Mixer(int n) { tracks = n; }
    public void setGain(int track, float gain) {
        return;
    }
    private static int clamp(int x) { return Math.max(0, x); }
    public static void main(String[] args) { new Mixer(2).setGain(0, 1f); }
}
)");
  CHECK(sym(j, "setGain")->scope == "Mixer");
  CHECK(sym(j, "setGain")->exported);
  CHECK(sym(j, "setGain")->params == 2);
  CHECK_FALSE(sym(j, "clamp")->exported);
  CHECK(sym(j, "main", "") != nullptr);
  CHECK(has_ref(j, "import", "audio.ring.Ring"));
  auto cs = facts("csharp", R"(namespace Audio.Mix
{
    public class Mixer
    {
        public void SetGain(int track, float gain)
        {
        }

        private int Clamp(int x)
        {
            return x;
        }
    }
}
)");
  REQUIRE(sym(cs, "SetGain") != nullptr);
  CHECK(sym(cs, "SetGain")->scope == "Mixer");
  CHECK(sym(cs, "SetGain")->exported);
  CHECK_FALSE(sym(cs, "Clamp")->exported);
  auto kt = facts("kotlin", R"(class Mixer(val n: Int) {
    fun count() = n
    fun setGain(track: Int, gain: Float) { }
    private fun clamp(x: Int): Int = x
}
fun main() { Mixer(2).setGain(0, 1f) }
)");
  REQUIRE(sym(kt, "setGain") != nullptr);
  CHECK(sym(kt, "setGain")->scope == "Mixer");  // an expression body before it opens no scope
  CHECK_FALSE(sym(kt, "clamp")->exported);
  CHECK(sym(kt, "main", "") != nullptr);
}

TEST_CASE("lexical: shell functions, subcommands, sourced files, called commands") {
  auto f = facts("bash", R"(#!/bin/bash
# profile-switch.sh goto|altab|move-win
source ~/.config/hypr/MyScripts/config.sh
. "$DIR/lib.sh"

_dispatch() {
    hyprctl dispatch "$1"
}

notify_profile() {
    notify-send "P$1"
}

ACTION=$1
case "$ACTION" in
    goto)
        save_profile "$BASE" 3 && _dispatch "x"
        ;;
    altab|tab)
        case "$2" in
            inner) echo no ;;
        esac
        ;;
    move-win) ;;
    *) echo "usage" >&2 ;;
esac
)");
  REQUIRE(sym(f, "notify_profile") != nullptr);
  CHECK(sym(f, "notify_profile")->exported);
  CHECK(sym(f, "notify_profile")->params == -1);
  CHECK_FALSE(sym(f, "_dispatch")->exported);
  for (const char* c : {"goto", "altab", "tab", "move-win"}) {
    INFO(c);
    REQUIRE(sym(f, c) != nullptr);
    CHECK(sym(f, c)->kind == "command");
    CHECK(sym(f, c)->exported);
  }
  CHECK(sym(f, "inner") == nullptr);  // a nested case is no subcommand
  CHECK(has_ref(f, "import", "~/.config/hypr/MyScripts/config.sh"));
  CHECK(has_ref(f, "import", "$DIR/lib.sh"));
  CHECK(has_ref(f, "call", "save_profile"));
  CHECK(has_ref(f, "call", "_dispatch"));
  CHECK(has_ref(f, "call", "hyprctl"));
  CHECK_FALSE(has_ref(f, "call", "echo"));
  auto arith = facts("bash", "if (( before > 3 )); then\n  brightnessctl set 40%\nfi\n");
  CHECK_FALSE(has_ref(arith, "call", "before"));  // a variable of (( )), not a command
  CHECK(has_ref(arith, "call", "brightnessctl"));
}

TEST_CASE("lexical: an executable script without subcommands is one command named after its file") {
  const auto* sh = core::language_by_id("bash");
  auto f = code::analyze_lexical(*sh, "#!/bin/bash\nhide_all() { :; }\nhide_all\n", false, "MyScripts/show-desktop.sh");
  REQUIRE(sym(f, "show-desktop") != nullptr);
  CHECK(sym(f, "show-desktop")->kind == "command");
  auto lib = code::analyze_lexical(*sh, "# sourced by the others\nlog() { :; }\n", false, "lib/common.sh");
  CHECK(sym(lib, "common") == nullptr);  // no #!: a library, not a command
}

TEST_CASE("lexical: Lua, Ruby, PHP") {
  auto lua = facts("lua", R"(local util = require("hypr.util")
local M = {}
function M.bind(key, action) return util.wrap(action) end
local function helper(x) return x end
function M:reload() end
hl.bind("SUPER + T", hl.dsp.exec_cmd("kitty"))
return M
)");
  CHECK(sym(lua, "bind")->scope == "M");
  CHECK(sym(lua, "bind")->params == 2);
  CHECK(sym(lua, "bind")->exported);
  CHECK_FALSE(sym(lua, "helper")->exported);
  CHECK(sym(lua, "reload")->scope == "M");
  CHECK(has_ref(lua, "import", "hypr.util"));
  CHECK(has_ref(lua, "call", "exec_cmd"));
  auto rb = facts("ruby", R"(require_relative 'ring'
class Mixer
  def set_gain(track, gain)
  end

  private

  def clamp(x)
  end
end
)");
  CHECK(sym(rb, "set_gain")->scope == "Mixer");
  CHECK(sym(rb, "set_gain")->exported);
  CHECK_FALSE(sym(rb, "clamp")->exported);
  auto php = facts("php", R"(<?php
require_once __DIR__ . '/ring.php';
class Mixer {
    public function setGain($track, $gain) {}
    private function clamp($x) { return $x; }
}
)");
  CHECK(sym(php, "setGain")->exported);
  CHECK(sym(php, "setGain")->params == 2);
  CHECK_FALSE(sym(php, "clamp")->exported);
  CHECK(has_ref(php, "import", "/ring.php"));
}

TEST_CASE("lexical: imports resolve to project files by the rules of each language") {
  const std::vector<std::string> files = {"app/audio/mix.py",         "app/audio/ring.py",     "app/util/__init__.py",
                                          "web/src/ring.ts",          "web/src/ui/index.tsx",  "pkg/ring/ring.go",
                                          "pkg/ring/buffer.go",       "src/dsp/chain.rs",      "src/ring/mod.rs",
                                          "src/main.rs",              "MyScripts/config.sh",   "lua/hypr/util.lua",
                                          "src/main/java/audio/ring/Ring.java"};
  CHECK(resolve("python", "app/audio/mix.py", ".ring", files) == std::vector<std::string>{"app/audio/ring.py"});
  CHECK(resolve("python", "app/audio/mix.py", "..util", files) == std::vector<std::string>{"app/util/__init__.py"});
  CHECK(resolve("python", "tools/x.py", "audio.ring", files) == std::vector<std::string>{"app/audio/ring.py"});
  CHECK(resolve("python", "tools/x.py", "os", files).empty());
  CHECK(resolve("typescript", "web/src/app.ts", "./ring", files) == std::vector<std::string>{"web/src/ring.ts"});
  CHECK(resolve("typescript", "web/src/app.ts", "./ui", files) == std::vector<std::string>{"web/src/ui/index.tsx"});
  CHECK(resolve("typescript", "web/src/app.ts", "react", files).empty());
  CHECK(resolve("go", "cmd/main.go", "example.com/audio/pkg/ring", files).size() == 2);
  CHECK(resolve("go", "cmd/main.go", "fmt", files).empty());
  CHECK(resolve("rust", "src/main.rs", "crate::dsp::chain::Chain", files) == std::vector<std::string>{"src/dsp/chain.rs"});
  CHECK(resolve("rust", "src/main.rs", "mod ring", files) == std::vector<std::string>{"src/ring/mod.rs"});
  CHECK(resolve("rust", "src/main.rs", "serde::Serialize", files).empty());
  CHECK(resolve("bash", "MyScripts/ws.sh", "~/.config/hypr/MyScripts/config.sh", files) == std::vector<std::string>{"MyScripts/config.sh"});
  CHECK(resolve("bash", "MyScripts/ws.sh", "config.sh", files) == std::vector<std::string>{"MyScripts/config.sh"});
  CHECK(resolve("lua", "init.lua", "hypr.util", files) == std::vector<std::string>{"lua/hypr/util.lua"});
  CHECK(resolve("java", "src/main/java/audio/mix/Mixer.java", "audio.ring.Ring", files) ==
        std::vector<std::string>{"src/main/java/audio/ring/Ring.java"});
}

TEST_CASE("lexical: the analyzer takes other languages by the file name") {
  code::CppAnalyzer a;
  index::CodeFile f;
  f.path = "app/mix.py";
  f.role = "impl";
  auto py = a.analyze(f, "def mix(a, b):\n    return a\n");
  REQUIRE(sym(py, "mix") != nullptr);
  CHECK(sym(py, "mix")->exported);
  f.path = "src/mix.cpp";
  auto cpp = a.analyze(f, "int mix(int a, int b) { return a; }\n");
  REQUIRE(sym(cpp, "mix") != nullptr);
  CHECK_FALSE(sym(cpp, "mix")->exported);  // a C++ source exports nothing; its header does
}
