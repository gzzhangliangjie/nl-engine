# -*- coding: utf-8 -*-
"""P8 M-S0/M-S1: generate the deterministic mono WAVs used by audio-tools.

Defaults are frozen (48000 Hz / mono / 16-bit PCM / 0.5 s / 440 Hz sine with
exponential decay, amplitude 0.6) -> test-tone.wav stays byte-identical forever.
Pure standard library, no randomness -> byte-identical output on every run.

Usage: python scripts/make-audio-wav.py [output.wav] [freq_hz] [amplitude]
M-S1 usage (second, distinct clip for concurrent-source mixing evidence):
    python scripts/make-audio-wav.py assets/audio/test-tone-b.wav 660 0.4
"""
import math
import os
import struct
import sys
import wave

SAMPLE_RATE = 48000
DURATION_S = 0.5
FRAMES = int(SAMPLE_RATE * DURATION_S)
FREQ_HZ = 440.0
AMPLITUDE = 0.6
DECAY = 6.0  # exp(-DECAY * t / DURATION_S)


def build_frames(freq=FREQ_HZ, amplitude=AMPLITUDE):
    out = bytearray()
    for i in range(FRAMES):
        t = i / SAMPLE_RATE
        env = math.exp(-DECAY * t / DURATION_S)
        s = amplitude * env * math.sin(2.0 * math.pi * freq * t)
        v = int(round(max(-1.0, min(1.0, s)) * 32767.0))
        out += struct.pack("<h", v)
    return bytes(out)


def main():
    root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    path = sys.argv[1] if len(sys.argv) > 1 else os.path.join(root, "assets", "audio", "test-tone.wav")
    freq = float(sys.argv[2]) if len(sys.argv) > 2 else FREQ_HZ
    amplitude = float(sys.argv[3]) if len(sys.argv) > 3 else AMPLITUDE
    os.makedirs(os.path.dirname(path), exist_ok=True)
    data = build_frames(freq, amplitude)
    with wave.open(path, "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(SAMPLE_RATE)
        w.writeframes(data)
    print("wav: path=%s rate=%d ch=1 bits=16 frames=%d bytes=%d freq=%.1f amp=%.3f" %
          (path.replace("\\", "/"), SAMPLE_RATE, FRAMES, len(data), freq, amplitude))


if __name__ == "__main__":
    main()
