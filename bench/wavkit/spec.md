# wavkit — task description (benchmark S1)

Build `wavkit`, a command line audio pipeline in C++20 with CMake + Ninja.
`cmake -S . -B build -G Ninja && cmake --build build` must produce the executable `build/wavkit`.
No audio device, no third-party audio libraries.

## Command line

    wavkit -o OUT.wav [--gain G] [--lowpass HZ] [--echo MS] [--feedback F] [--echo-mix M]
           [--track-gain G]... [--float] IN.wav [IN2.wav ...]

- Every input file is one track. Effects run on every track in this order: gain, low-pass, echo.
- `--gain G` (default 1.0): multiply every sample by G, exactly once.
- `--lowpass HZ` (default off): one-pole low-pass, `y += a * (x - y)`, `a = 1 - exp(-2*pi*HZ/rate)`.
- `--echo MS` (default off): feedback delay line of MS milliseconds; `--feedback F` in [0, 0.99]
  (default 0.3, values above are clamped), `--echo-mix M` in [0, 1] (default 0.3):
  `out = in*(1-M) + delayed*M`, the line stores `in + delayed*F`.
- `--track-gain G` repeated: gain of track 1, 2, ... in the mix (default 1.0).
- Mix: the sum of the tracks with their gains, length of the longest track, every sample
  clamped to [-1, 1] (clip-safe).
- `--float`: write float32 instead of PCM16.

## Formats

- Input: RIFF/WAVE, PCM16 or float32, mono or stereo, any sample rate (all inputs of one call
  share it). Mono is duplicated to both channels. PCM16 maps to [-1, 1) by dividing by 32768.
- Output: RIFF/WAVE little-endian, stereo, the input sample rate, PCM16 (default, value =
  round(clamp(x) * 32767)) or float32; header sizes match the number of frames.
- Unsupported or broken input: message on stderr, exit code 1, no crash. Bad arguments: exit 2.

## Internals

A reader thread moves frames from the reader to processing through a lock-free single-producer
single-consumer ring buffer; processing runs on the main thread.
