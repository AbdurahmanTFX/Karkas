#!/usr/bin/env python3
"""Hidden acceptance tests for wavkit (benchmark, section 17). Standard library only."""
import argparse, math, os, struct, subprocess, sys, tempfile, wave

RATE = 48000


def write_wav(path, frames, channels=2, fmt="pcm16"):
    """frames: list of tuples of floats in [-1, 1]."""
    if fmt == "pcm16":
        with wave.open(path, "wb") as w:
            w.setnchannels(channels)
            w.setsampwidth(2)
            w.setframerate(RATE)
            data = b"".join(struct.pack("<" + "h" * channels, *[max(-32768, min(32767, int(round(s * 32767)))) for s in f]) for f in frames)
            w.writeframes(data)
    else:
        data = b"".join(struct.pack("<" + "f" * channels, *f) for f in frames)
        hdr = b"RIFF" + struct.pack("<I", 36 + len(data)) + b"WAVEfmt " + struct.pack("<IHHIIHH", 16, 3, channels, RATE, RATE * 4 * channels, 4 * channels, 32)
        with open(path, "wb") as fh:
            fh.write(hdr + b"data" + struct.pack("<I", len(data)) + data)


def read_wav(path):
    with open(path, "rb") as fh:
        b = fh.read()
    assert b[:4] == b"RIFF" and b[8:12] == b"WAVE", "not RIFF/WAVE"
    pos, fmt = 12, None
    while pos < len(b):
        cid, size = b[pos:pos + 4], struct.unpack("<I", b[pos + 4:pos + 8])[0]
        body = b[pos + 8:pos + 8 + size]
        if cid == b"fmt ":
            fmt = struct.unpack("<HHIIHH", body[:16])
        elif cid == b"data":
            tag, ch, rate, _, _, bits = fmt
            n = size // (ch * bits // 8)
            assert struct.unpack("<I", b[4:8])[0] == 36 + size, "RIFF size does not match"
            code = "h" if bits == 16 else "f"
            vals = struct.unpack("<" + code * (n * ch), body[:n * ch * bits // 8])
            scale = 32767.0 if bits == 16 else 1.0
            return rate, ch, bits, [tuple(vals[i * ch + c] / scale for c in range(ch)) for i in range(n)]
        pos += 8 + size + (size & 1)
    raise AssertionError("no data chunk")


def sine(n, freq, amp=0.5):
    return [(amp * math.sin(2 * math.pi * freq * i / RATE),) * 2 for i in range(n)]


class Suite:
    def __init__(self, exe, tmp):
        self.exe, self.tmp, self.passed, self.total = exe, tmp, 0, 0

    def run(self, *args):
        return subprocess.run([self.exe, *args], capture_output=True, text=True, timeout=120)

    def p(self, name):
        return os.path.join(self.tmp, name)

    def check(self, name, fn):
        self.total += 1
        try:
            fn()
            self.passed += 1
            print(f"PASS {name}")
        except Exception as e:  # noqa: BLE001
            print(f"FAIL {name}: {e}")

    # --- S1 -------------------------------------------------------------------------------------
    def s1(self):
        def passthrough():
            src = sine(4800, 440)
            write_wav(self.p("a.wav"), src)
            r = self.run("-o", self.p("o.wav"), self.p("a.wav"))
            assert r.returncode == 0, r.stderr
            rate, ch, bits, out = read_wav(self.p("o.wav"))
            assert (rate, ch, bits) == (RATE, 2, 16)
            assert len(out) == len(src)
            assert max(abs(o[0] - s[0]) for o, s in zip(out, src)) < 2e-4
        self.check("pcm16 passthrough is lossless", passthrough)

        def long_file():
            src = [((i % 1000) / 1000.0 - 0.5, 0.0) for i in range(48000 * 6)]
            write_wav(self.p("long.wav"), src)
            assert self.run("-o", self.p("o.wav"), self.p("long.wav")).returncode == 0
            _, _, _, out = read_wav(self.p("o.wav"))
            assert len(out) == len(src), f"{len(out)} frames, expected {len(src)}"
            bad = sum(1 for o, s in zip(out, src) if abs(o[0] - s[0]) > 1e-3)
            assert bad == 0, f"{bad} frames differ (lost or duplicated)"
        self.check("6 s of audio keeps every frame (ring buffer)", long_file)

        def gain():
            write_wav(self.p("g.wav"), [(0.8, -0.4)] * 1000)
            assert self.run("--gain", "0.5", "-o", self.p("o.wav"), self.p("g.wav")).returncode == 0
            _, _, _, out = read_wav(self.p("o.wav"))
            assert abs(out[500][0] - 0.4) < 1e-3 and abs(out[500][1] + 0.2) < 1e-3, out[500]
        self.check("gain is applied exactly once", gain)

        def mix_clip():
            write_wav(self.p("m1.wav"), [(0.9, 0.9)] * 500)
            write_wav(self.p("m2.wav"), [(0.9, -0.9)] * 800)
            assert self.run("-o", self.p("o.wav"), self.p("m1.wav"), self.p("m2.wav")).returncode == 0
            _, _, _, out = read_wav(self.p("o.wav"))
            assert len(out) == 800
            assert all(-1.0 <= s <= 1.0 for f in out for s in f)
            assert abs(out[100][0] - 1.0) < 1e-3 and abs(out[100][1]) < 1e-3
        self.check("mix is clip-safe and as long as the longest track", mix_clip)

        def track_gain():
            write_wav(self.p("t1.wav"), [(0.5, 0.5)] * 100)
            write_wav(self.p("t2.wav"), [(0.5, 0.5)] * 100)
            assert self.run("--track-gain", "1", "--track-gain", "0", "-o", self.p("o.wav"), self.p("t1.wav"), self.p("t2.wav")).returncode == 0
            _, _, _, out = read_wav(self.p("o.wav"))
            assert abs(out[50][0] - 0.5) < 1e-3
        self.check("per-track gain", track_gain)

        def echo_bounded():
            write_wav(self.p("e.wav"), [(0.5, 0.5)] * (RATE * 3))
            assert self.run("--echo", "20", "--feedback", "0.95", "--echo-mix", "0.5", "-o", self.p("o.wav"), "--float", self.p("e.wav")).returncode == 0
            _, _, bits, out = read_wav(self.p("o.wav"))
            assert bits == 32
            assert max(abs(f[0]) for f in out) < 20.0, "echo output grows without bound"
        self.check("echo with feedback stays bounded", echo_bounded)

        def lowpass():
            write_wav(self.p("hi.wav"), sine(9600, 12000))
            assert self.run("--lowpass", "500", "-o", self.p("o.wav"), self.p("hi.wav")).returncode == 0
            _, _, _, out = read_wav(self.p("o.wav"))
            assert max(abs(f[0]) for f in out[1000:]) < 0.1
        self.check("low-pass removes high frequencies", lowpass)

        def float_in():
            write_wav(self.p("f.wav"), [(0.25, -0.75)] * 300, fmt="float32")
            assert self.run("-o", self.p("o.wav"), self.p("f.wav")).returncode == 0
            _, _, _, out = read_wav(self.p("o.wav"))
            assert abs(out[10][1] + 0.75) < 1e-3
        self.check("float32 input", float_in)

        def mono():
            with wave.open(self.p("mono.wav"), "wb") as w:
                w.setnchannels(1); w.setsampwidth(2); w.setframerate(RATE)
                w.writeframes(struct.pack("<h", 16384) * 200)
            assert self.run("-o", self.p("o.wav"), self.p("mono.wav")).returncode == 0
            _, ch, _, out = read_wav(self.p("o.wav"))
            assert ch == 2 and abs(out[10][0] - 0.5) < 1e-3 and abs(out[10][1] - 0.5) < 1e-3
        self.check("mono input goes to both channels", mono)

        def little_endian():
            write_wav(self.p("le.wav"), [(0.5, 0.0)] * 10)
            assert self.run("-o", self.p("o.wav"), self.p("le.wav")).returncode == 0
            with open(self.p("o.wav"), "rb") as fh:
                b = fh.read()
            data = b.index(b"data") + 8
            assert struct.unpack("<h", b[data:data + 2])[0] in (16383, 16384), b[data:data + 2].hex()
        self.check("output samples are little-endian", little_endian)

        def bad_input():
            with open(self.p("bad.wav"), "wb") as fh:
                fh.write(b"definitely not a wav file")
            r = self.run("-o", self.p("o.wav"), self.p("bad.wav"))
            assert r.returncode == 1, f"exit {r.returncode}"
            r2 = self.run("--no-such-flag")
            assert r2.returncode == 2, f"exit {r2.returncode}"
        self.check("bad input: exit 1, bad arguments: exit 2", bad_input)

    # --- S2 -------------------------------------------------------------------------------------
    def s2(self):
        def reverb():
            write_wav(self.p("r.wav"), [(1.0, 1.0)] + [(0.0, 0.0)] * 9600)
            assert self.run("--reverb", "0", "-o", self.p("o0.wav"), "--float", self.p("r.wav")).returncode == 0
            _, _, _, dry = read_wav(self.p("o0.wav"))
            assert max(abs(f[0]) for f in dry[100:]) < 1e-6, "ROOM 0 must leave samples unchanged"
            assert self.run("--reverb", "0.7", "-o", self.p("o1.wav"), "--float", self.p("r.wav")).returncode == 0
            _, _, _, wet = read_wav(self.p("o1.wav"))
            assert sum(abs(f[0]) for f in wet[1000:]) > 0.01, "no reverb tail"
        self.check("s2 reverb", reverb)

        def pan():
            write_wav(self.p("p.wav"), [(0.5, 0.5)] * 100)
            assert self.run("--pan", "1", "-o", self.p("o.wav"), self.p("p.wav")).returncode == 0
            _, _, _, out = read_wav(self.p("o.wav"))
            assert abs(out[50][0]) < 1e-3 and abs(out[50][1] - 0.5) < 1e-3, out[50]
        self.check("s2 pan", pan)

        def normalize():
            write_wav(self.p("n.wav"), [(0.25, -0.125)] * 100)
            assert self.run("--normalize", "-o", self.p("o.wav"), self.p("n.wav")).returncode == 0
            _, _, _, out = read_wav(self.p("o.wav"))
            assert abs(out[50][0] - 1.0) < 1e-3 and abs(out[50][1] + 0.5) < 1e-3, out[50]
        self.check("s2 normalize", normalize)

        def batch():
            ind, outd = self.p("in"), self.p("out")
            os.makedirs(ind, exist_ok=True)
            os.makedirs(outd, exist_ok=True)
            write_wav(os.path.join(ind, "x.wav"), [(0.2, 0.2)] * 50)
            write_wav(os.path.join(ind, "y.wav"), [(0.4, 0.4)] * 60)
            with open(os.path.join(ind, "z.wav"), "wb") as fh:
                fh.write(b"broken")
            r = self.run("--batch", ind, "-o", outd, "--gain", "2")
            assert r.returncode == 1, f"exit {r.returncode} (one bad file)"
            _, _, _, x = read_wav(os.path.join(outd, "x.wav"))
            _, _, _, y = read_wav(os.path.join(outd, "y.wav"))
            assert abs(x[10][0] - 0.4) < 1e-3 and len(y) == 60
        self.check("s2 batch", batch)

        def progress():
            write_wav(self.p("pr.wav"), [(0.1, 0.1)] * RATE)
            r = self.run("--progress", "-o", self.p("o.wav"), self.p("pr.wav"))
            vals = [int(l.split()[1]) for l in r.stderr.splitlines() if l.startswith("progress ")]
            assert vals and vals[-1] == 100 and vals == sorted(vals), vals[:10]
        self.check("s2 progress", progress)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--wavkit", required=True)
    ap.add_argument("--scenario", default="s1")
    a = ap.parse_args()
    with tempfile.TemporaryDirectory() as tmp:
        s = Suite(os.path.abspath(a.wavkit), tmp)
        if not os.access(s.exe, os.X_OK):
            print(f"FAIL build: {s.exe} is missing")
            print("hidden 0/1")
            return 1
        s.s1()
        if a.scenario == "s2":
            s.s2()
        print(f"hidden {s.passed}/{s.total}")
        return 0 if s.passed == s.total else 1


if __name__ == "__main__":
    sys.exit(main())
