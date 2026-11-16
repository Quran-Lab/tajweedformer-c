"""Makes test/data/eval/*.flac for tf40_tests eval_units: synthetic integer signals (the same formula as the test's
ev_t_signal) written as WAV, then encoded by ffmpeg's FLAC encoder (LPC, stereo decorrelation, 24-bit, wasted bits).
Run once; the FLAC files are committed, the WAVs are not."""
import os
import struct
import subprocess
import sys

OUT = sys.argv[1]
FFMPEG = sys.argv[2]
TMP = sys.argv[3]


def sig(n, amp, seed, stereo, mul):
    st = seed
    a, b = [], []
    for i in range(n):
        tri = (i * 7) % 400 - 200
        st = (st * 1103515245 + 12345) & 0x7FFFFFFF
        nz = (st >> 16) % 33 - 16
        x = (tri * amp + nz) * mul
        a.append(x)
        if stereo:
            b.append(x - (x >> 2) + (i * 5) % 50 - 25)
    return a, b


def wav(path, rate, bits, chans):
    n = len(chans[0])
    bps = bits // 8
    data = bytearray()
    for i in range(n):
        for c in chans:
            data += int(c[i]).to_bytes(bps, "little", signed=True)
    fmt = struct.pack("<HHIIHH", 1, len(chans), rate, rate * bps * len(chans), bps * len(chans), bits)
    with open(path, "wb") as f:
        f.write(b"RIFF" + struct.pack("<I", 4 + 8 + len(fmt) + 8 + len(data)) + b"WAVE")
        f.write(b"fmt " + struct.pack("<I", len(fmt)) + fmt + b"data" + struct.pack("<I", len(data)) + data)


CASES = [  # name, rate, bits, stereo, n, amp, seed, mul
    ("s16_44k_stereo", 44100, 16, True, 2205, 40, 1, 1),
    ("s24_48k_mono", 48000, 24, False, 2400, 10000, 2, 1),
    ("s16_8k_wasted", 8000, 16, False, 1000, 40, 3, 4),
    ("s24_16k_stereo", 16000, 24, True, 800, 10000, 4, 1),
]
os.makedirs(OUT, exist_ok=True)
for name, rate, bits, stereo, n, amp, seed, mul in CASES:
    a, b = sig(n, amp, seed, stereo, mul)
    w = os.path.join(TMP, name + ".wav")
    wav(w, rate, bits, [a, b] if stereo else [a])
    f = os.path.join(OUT, name + ".flac")
    cmd = [FFMPEG, "-hide_banner", "-loglevel", "error", "-y", "-i", w, "-map_metadata", "-1", "-fflags", "+bitexact", "-flags:a", "+bitexact",
           "-c:a", "flac", "-compression_level", "8"]
    if bits == 24:
        cmd += ["-sample_fmt", "s32", "-bits_per_raw_sample", "24"]
    subprocess.run(cmd + [f], check=True)
    os.remove(w)
    print(name, os.path.getsize(f))
