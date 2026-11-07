// Composition root (5.7): creates every leaf and wires them along the links of the schema:
//   io.reader -> stream.pump -> stream.ring -> dsp.chain(gain, lowpass, echo) -> mix -> io.writer
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>
#include <vector>

#include "chain.hpp"
#include "echo.hpp"
#include "gain.hpp"
#include "log.hpp"
#include "lowpass.hpp"
#include "mix.hpp"
#include "pump.hpp"
#include "reader.hpp"
#include "ring.hpp"
#include "types.hpp"
#include "writer.hpp"

namespace {

struct Options {
  std::vector<std::string> inputs;
  std::string output;
  float gain = 1.0f;
  float cutoff = 0.0f;
  std::size_t echo_ms = 0;
  float feedback = 0.3f;
  float echo_mix = 0.3f;
  std::vector<float> track_gains;
  bool float_out = false;
};

void usage() {
  std::fprintf(stderr,
               "usage: wavkit -o OUT.wav [--gain G] [--lowpass HZ] [--echo MS] [--feedback F] [--echo-mix M]\n"
               "              [--track-gain G]... [--float] IN.wav [IN2.wav ...]\n");
}

bool parse(int argc, char** argv, Options& o) {
  for (int i = 1; i < argc; ++i) {
    std::string a = argv[i];
    auto next = [&]() -> const char* { return i + 1 < argc ? argv[++i] : nullptr; };
    const char* v = nullptr;
    if (a == "-o" && (v = next())) o.output = v;
    else if (a == "--gain" && (v = next())) o.gain = std::strtof(v, nullptr);
    else if (a == "--lowpass" && (v = next())) o.cutoff = std::strtof(v, nullptr);
    else if (a == "--echo" && (v = next())) o.echo_ms = std::strtoul(v, nullptr, 10);
    else if (a == "--feedback" && (v = next())) o.feedback = std::strtof(v, nullptr);
    else if (a == "--echo-mix" && (v = next())) o.echo_mix = std::strtof(v, nullptr);
    else if (a == "--track-gain" && (v = next())) o.track_gains.push_back(std::strtof(v, nullptr));
    else if (a == "--float") o.float_out = true;
    else if (!a.empty() && a[0] == '-') return false;
    else o.inputs.push_back(a);
  }
  return !o.output.empty() && !o.inputs.empty();
}

// Adapts the ring buffer to the pump's sink interface (the pump knows no ring).
struct RingSink final : wk::IFrameSink {
  explicit RingSink(wk::IRing& r) : ring(r) {}
  bool push(std::span<const wk::Frame> f) override { return ring.push(f); }
  wk::IRing& ring;
};

bool process_track(const std::string& path, const Options& o, wk::AudioFormat& fmt, std::vector<wk::Frame>& out) {
  auto reader = wk::make_reader();
  auto f = reader->open(path);
  if (!f.ok()) {
    wk::log(wk::Level::Error, path + ": " + f.error());
    return false;
  }
  fmt = f.value();
  auto ring = wk::make_ring(8192);
  RingSink sink(*ring);
  auto pump = wk::make_pump(*reader, sink, 256);
  auto gain = wk::make_gain();
  gain->set_gain(o.gain);
  auto lowpass = wk::make_lowpass();
  if (o.cutoff > 0.0f) lowpass->set_cutoff(o.cutoff, static_cast<float>(fmt.sample_rate));
  auto echo = wk::make_echo();
  if (o.echo_ms > 0) echo->set_params(o.echo_ms * static_cast<std::size_t>(fmt.sample_rate) / 1000, o.feedback, o.echo_mix);
  auto chain = wk::make_chain(gain.get(), o.cutoff > 0.0f ? lowpass.get() : nullptr, o.echo_ms > 0 ? echo.get() : nullptr);
  pump->start();
  std::vector<wk::Frame> buf(512);
  while (true) {
    std::size_t n = ring->pop(buf);
    if (n > 0) {
      std::span<wk::Frame> part(buf.data(), n);
      chain->process(part);
      out.insert(out.end(), part.begin(), part.end());
      continue;
    }
    if (pump->finished() && ring->fill() == 0) break;
    std::this_thread::sleep_for(std::chrono::microseconds(100));
  }
  pump->stop();
  return true;
}

}  // namespace

int run_app(int argc, char** argv) {
  Options o;
  if (!parse(argc, argv, o)) {
    usage();
    return 2;
  }
  std::vector<std::vector<wk::Frame>> tracks;
  wk::AudioFormat fmt;
  for (const auto& in : o.inputs) {
    std::vector<wk::Frame> t;
    if (!process_track(in, o, fmt, t)) return 1;
    tracks.push_back(std::move(t));
  }
  auto mix = wk::make_mix();
  for (std::size_t i = 0; i < o.track_gains.size(); ++i) mix->set_track_gain(i, o.track_gains[i]);
  std::vector<wk::Frame> mixed;
  mix->mix(tracks, mixed);
  auto writer = wk::make_writer();
  wk::AudioFormat out_fmt{fmt.sample_rate, 2, o.float_out ? 32 : 16, o.float_out};
  if (auto r = writer->open(o.output, out_fmt); !r.ok()) {
    wk::log(wk::Level::Error, r.error());
    return 1;
  }
  (void)writer->write(mixed);
  (void)writer->close();
  wk::log(wk::Level::Info, "wrote " + std::to_string(mixed.size()) + " frames to " + o.output);
  return 0;
}
