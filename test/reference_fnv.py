"""The reference data's fingerprint from the plaintext table, for the conformance run's check of the SDK's reader.

  python test/reference_fnv.py <expected_tj3.zfx>

Reads a ZFTJEXP1 file (quranlab-core tajweed/src/reference.c: the 160-byte header, the ayah counts, the ayah
index, the units and the word of each unit) and prints `ref_fnv <16 hex>` and the ayah count: FNV-1a 64 over every
ayah in order of (unit count, word count) as two 32-bit ints, the units as 32-bit ints, and each word's first unit as
32-bit ints. That is quranlab-core's tests/qlc_bench.c --reference fingerprint, and what `tf40_tests conformance`
prints from tf_reference_units through the Core: equal values mean the SDK gives every ayah's units and words as
the plaintext table holds them. Reads the plaintext file only; writes nothing.
"""
import struct
import sys

FNV0 = 1469598103934665603
PRIME = 1099511628211
MASK = (1 << 64) - 1


def fnv1a(h, data):
    for b in data:
        h = ((h ^ b) * PRIME) & MASK
    return h


def ints(values):
    return struct.pack("<%di" % len(values), *values)


def main(argv):
    data = open(argv[1], "rb").read()
    if len(data) < 160 or data[:8] != b"ZFTJEXP1":
        sys.exit("not a plaintext ZFTJEXP1 file")
    n_surah, n_ayah, n_tok = struct.unpack_from("<3I", data, 20)
    off = struct.unpack_from("<8I", data, 104)
    counts = struct.unpack_from("<%dH" % n_surah, data, off[0])
    units = data[off[2]:off[2] + n_tok]
    words = data[off[3]:off[3] + n_tok]
    h, a = FNV0, 0
    for s in range(n_surah):
        for _ in range(counts[s]):
            first, = struct.unpack_from("<I", data, off[1] + 20 * a)
            count, = struct.unpack_from("<H", data, off[1] + 20 * a + 12)
            nw = data[off[1] + 20 * a + 16]
            u = list(units[first:first + count])
            w = list(words[first:first + count])
            starts = [0] * nw
            prev = -1
            for k, word in enumerate(w):
                if word != prev:
                    prev = word
                    if word < nw:
                        starts[word] = k
            h = fnv1a(h, ints([count, nw]))
            h = fnv1a(h, ints(u))
            h = fnv1a(h, ints(starts))
            a += 1
    print("reference: %d ayahs, ref_fnv %016x" % (a, h))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
