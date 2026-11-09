# -*- coding: utf-8 -*-
"""P8 M-S0: generate the deterministic mono WAV used by audio-tools.

Parameters are frozen (48000 Hz / mono / 16-bit PCM / 0.5 s / 440 Hz sine with
exponential decay). Pure standard library, no randomness -> byte-identical output
on every run. Usage: python scripts/make-audio-wav.py [output.wav]
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


def build_frames():
    out = bytearray()
    for i in range(FRAMES):
        t = i / SAMPLE_RATE
        env = math.exp(-DECAY * t / DURATION_S)
        s = AMPLITUDE * env * math.sin(2.0 * math.pi * FREQ_HZ * t)
        v = int(round(max(-1.0, min(1.0, s)) * 32767.0))
        out += struct.pack("<h", v)
    return bytes(out)


def main():
    root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    path = sys.argv[1] if len(sys.argv) > 1 else os.path.join(root, "assets", "audio", "test-tone.wav")
    os.makedirs(os.path.dirname(path), exist_ok=True)
    data = build_frames()
    with wave.open(path, "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(SAMPLE_RATE)
        w.writeframes(data)
    print("wav: path=%s rate=%d ch=1 bits=16 frames=%d bytes=%d" %
          (path.replace("\\", "/"), SAMPLE_RATE, FRAMES, len(data)))


if __name__ == "__main__":
    main()
