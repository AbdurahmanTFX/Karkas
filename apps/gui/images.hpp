#pragma once
// Images of Markdown notes in karkas-gui (docs/adr/0041): the file an image shows is found by
// core::MediaFinder and decoded by stb_image on a thread of its own (PNG, JPEG, the first frame of a GIF,
// BMP; shrunk to at most 2048 px on the long side), then uploaded to an OpenGL texture on the UI thread.
// A file changed on disk is read again and a missing one looked for again (every 2 s). URLs are never
// fetched.
#include <imgui.h>

#include <condition_variable>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "image_decode.hpp"
#include "kk/core/notes.hpp"

namespace kk::gui {

struct NoteImage {
  enum class State { Loading, Ready, Missing, Failed };
  State state = State::Loading;
  ImTextureID tex = 0;
  int w = 0, h = 0;  // the file's own size (the texture may be smaller)
  std::string file;  // from the project root ("" while not found)
  std::string error;  // Failed: why
};

class ImageCache {
 public:
  ImageCache();
  ~ImageCache();  // deletes the textures: the GL context must be current
  ImageCache(const ImageCache&) = delete;
  ImageCache& operator=(const ImageCache&) = delete;

  void set_root(const std::filesystem::path& root);  // a project: images of another one are forgotten
  void set_wake(std::function<void()> wake);         // called when an image is decoded (the UI redraws)
  // The image `src` of note file `note_file` (an ![[embed]] when `wiki`); looked for the first time asked.
  NoteImage get(const std::string& note_file, const std::string& src, bool wiki);
  // Once per frame on the UI thread: decoded images become textures.
  void upload();

 private:
  struct Entry {
    std::string note_file, src;
    bool wiki = false;
    NoteImage shown;                       // what the UI draws
    std::filesystem::file_time_type mtime{};
    Pixels pixels;                         // decoded, not uploaded yet
    bool decoded = false;
    bool queued = false;
  };
  void loop(std::stop_token st);
  void load(const std::string& key);  // on the thread: find, read, decode

  std::filesystem::path root_;
  std::function<void()> wake_;
  std::mutex mu_;
  std::condition_variable_any cv_;
  std::map<std::string, Entry> entries_;  // "<note file>\n<src>\n<w|m>" → its image
  std::deque<std::string> queue_;
  std::uint64_t generation_ = 0;  // a project change makes running loads stale
  std::vector<ImTextureID> dead_;  // textures to delete on the UI thread
  // The thread's own: one finder a pass (its index of names is made at most once a pass, when needed).
  std::unique_ptr<core::MediaFinder> finder_;
  std::filesystem::path finder_root_;
  std::jthread thread_;
};

}  // namespace kk::gui
