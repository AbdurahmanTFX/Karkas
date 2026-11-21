#include "images.hpp"

#ifdef _WIN32
#include <windows.h>  // before GL/gl.h, which needs its macros
#endif
#include <GL/gl.h>
#ifndef GL_CLAMP_TO_EDGE
#define GL_CLAMP_TO_EDGE 0x812F  // OpenGL 1.2; the gl.h of Windows is 1.1
#endif

#include <algorithm>
#include <chrono>
#include <cstring>
#include <fstream>

#include "i18n.hpp"
#include "kk/core/text.hpp"

namespace kk::gui {

namespace fs = std::filesystem;

namespace {

constexpr std::uintmax_t kMaxFileBytes = 64ull * 1024 * 1024;
constexpr auto kRecheck = std::chrono::seconds(2);

ImTextureID to_id(GLuint t) { return static_cast<ImTextureID>(t); }
GLuint to_gl(ImTextureID t) { return static_cast<GLuint>(t); }

std::string key_of(const std::string& note_file, const std::string& src, bool wiki) {
  return note_file + "\n" + src + (wiki ? "\nw" : "\nm");
}

}  // namespace

ImageCache::ImageCache() : thread_([this](std::stop_token st) { loop(st); }) {}

ImageCache::~ImageCache() {
  thread_.request_stop();
  cv_.notify_all();
  thread_.join();
  for (auto& [key, e] : entries_) {
    if (e.shown.tex != 0) dead_.push_back(e.shown.tex);
  }
  for (auto t : dead_) {
    GLuint id = to_gl(t);
    glDeleteTextures(1, &id);
  }
}

void ImageCache::set_root(const fs::path& root) {
  std::lock_guard lock{mu_};
  root_ = root;
  for (auto& [key, e] : entries_) {
    if (e.shown.tex != 0) dead_.push_back(e.shown.tex);
  }
  entries_.clear();
  queue_.clear();
  ++generation_;
}

void ImageCache::set_wake(std::function<void()> wake) {
  std::lock_guard lock{mu_};
  wake_ = std::move(wake);
}

NoteImage ImageCache::get(const std::string& note_file, const std::string& src, bool wiki) {
  std::lock_guard lock{mu_};
  auto key = key_of(note_file, src, wiki);
  auto [it, made] = entries_.try_emplace(key);
  if (made) {
    it->second.note_file = note_file;
    it->second.src = src;
    it->second.wiki = wiki;
    it->second.queued = true;
    queue_.push_back(key);
    cv_.notify_one();
  }
  return it->second.shown;
}

void ImageCache::upload() {
  std::lock_guard lock{mu_};
  for (auto t : dead_) {
    GLuint id = to_gl(t);
    glDeleteTextures(1, &id);
  }
  dead_.clear();
  for (auto& [key, e] : entries_) {
    if (!e.decoded) continue;
    e.decoded = false;
    if (e.shown.tex != 0) {
      GLuint old = to_gl(e.shown.tex);
      glDeleteTextures(1, &old);
      e.shown.tex = 0;
    }
    if (!e.pixels.error.empty() || e.pixels.rgba.empty()) {
      e.shown.state = NoteImage::State::Failed;
      e.shown.error = e.pixels.error;
      e.pixels = {};
      continue;
    }
    GLuint tex = 0;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, e.pixels.w, e.pixels.h, 0, GL_RGBA, GL_UNSIGNED_BYTE, e.pixels.rgba.data());
    glBindTexture(GL_TEXTURE_2D, 0);
    e.shown.tex = to_id(tex);
    e.shown.w = e.pixels.full_w;
    e.shown.h = e.pixels.full_h;
    e.shown.state = NoteImage::State::Ready;
    e.shown.error.clear();
    e.pixels = {};
  }
}

void ImageCache::load(const std::string& key) {
  std::string note_file;
  std::string src;
  bool wiki = false;
  fs::path root;
  std::uint64_t gen = 0;
  fs::file_time_type known{};
  bool had_file = false;
  {
    std::lock_guard lock{mu_};
    auto it = entries_.find(key);
    if (it == entries_.end()) return;
    it->second.queued = false;
    note_file = it->second.note_file;
    src = it->second.src;
    wiki = it->second.wiki;
    root = root_;
    gen = generation_;
    known = it->second.mtime;
    had_file = !it->second.shown.file.empty() && it->second.shown.state != NoteImage::State::Loading;
  }
  // Found anew each time: the file may have appeared, moved or (for an embed) a nearer one been made.
  if (!finder_ || finder_root_ != root) {
    finder_ = std::make_unique<core::MediaFinder>(root);
    finder_root_ = root;
  }
  auto found = finder_->find(note_file, src, wiki);
  std::error_code ec;
  fs::file_time_type mtime{};
  if (found) mtime = fs::last_write_time(root / *found, ec);
  auto set = [&](auto&& change) {
    std::lock_guard lock{mu_};
    if (gen != generation_) return;
    auto it = entries_.find(key);
    if (it != entries_.end()) change(it->second);
  };
  if (!found) {
    bool changed = false;
    std::function<void()> wake;
    set([&](Entry& e) {
      changed = e.shown.state != NoteImage::State::Missing;
      if (e.shown.tex != 0) dead_.push_back(e.shown.tex);
      e.shown = NoteImage{};
      e.shown.state = NoteImage::State::Missing;
      e.mtime = {};
      wake = wake_;
    });
    if (changed && wake) wake();
    return;
  }
  if (had_file && mtime == known) {
    bool same = false;
    set([&](Entry& e) { same = e.shown.file == *found; });
    if (same) return;  // unchanged
  }
  Pixels px;
  auto ext = core::to_lower(fs::path(*found).extension().string());
  auto size = fs::file_size(root / *found, ec);
  if (ext == ".webp" || ext == ".svg" || ext == ".avif") {
    px.error = tr("this format is not shown: PNG, JPEG, GIF and BMP are");
  } else if (ec || size > kMaxFileBytes) {
    px.error = tr("the file is larger than 64 MB");
  } else {
    std::ifstream in(root / *found, std::ios::binary);
    std::vector<std::uint8_t> bytes(size);
    in.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(size));
    px = in ? decode_image(bytes) : Pixels{.w = 0, .h = 0, .full_w = 0, .full_h = 0, .rgba = {}, .error = tr("cannot read the file")};
  }
  std::function<void()> wake;
  set([&](Entry& e) {
    e.shown.file = *found;
    e.mtime = mtime;
    e.pixels = std::move(px);
    e.decoded = true;
    wake = wake_;
  });
  if (wake) wake();
}

void ImageCache::loop(std::stop_token st) {
  auto next_check = std::chrono::steady_clock::now() + kRecheck;
  while (!st.stop_requested()) {
    std::string key;
    {
      std::unique_lock lock{mu_};
      cv_.wait_until(lock, st, next_check, [&] { return !queue_.empty(); });
      if (st.stop_requested()) return;
      if (queue_.empty()) {
        // Every few seconds: files changed or made since are looked at again.
        for (auto& [k, e] : entries_) {
          if (!e.queued && e.shown.state != NoteImage::State::Loading) {
            e.queued = true;
            queue_.push_back(k);
          }
        }
        next_check = std::chrono::steady_clock::now() + kRecheck;
        finder_.reset();  // a new pass sees files made since
        continue;
      }
      key = std::move(queue_.front());
      queue_.pop_front();
    }
    load(key);
  }
}

}  // namespace kk::gui
