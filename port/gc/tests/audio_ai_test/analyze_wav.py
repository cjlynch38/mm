#!/usr/bin/env python3
"""Checks an audio dump (Dolphin's Dump/Audio/dspdump*.wav) of the GameCube port's AI output.

Pure Python (no numpy). Reports:
  - the format, and per second: RMS and peak level of each channel and clipped samples
  - the active part (first to last sample above the noise floor), and gaps inside it: runs of exact
    digital silence of at least --gap-ms, which is what an AI underrun produces
  - discontinuities: samples whose second difference jumps far above that of their surroundings
    (clicks from dropped or repeated audio)
  - --tone L,R: the frequency of each channel's sine (zero crossings) on the longest gap-free stretch,
    against the expected tone at the --nominal-rate the producer generated it for
  - --spectrum: the strongest spectral peaks of the mix in a few windows (music), each with the nearest
    equal-tempered note (A4 = 440 Hz) and its offset in cents, after scaling the frequency to the
    --nominal-rate the game mixed for (a pitch error shows up as the same offset on every peak)

Exits 1 if --expect-gaps is given and the number of gaps differs, or if --tone frequencies are off
by more than --tolerance percent.

  python3 analyze_wav.py audio_ai_test.wav --tone 440,1000 --expect-gaps 1
  python3 analyze_wav.py mm.wav --spectrum --from 10 --fft 65536
"""
import argparse
import array
import cmath
import math
import struct
import sys


def read_wav(path):
    with open(path, "rb") as f:
        data = f.read()
    if data[0:4] != b"RIFF" or data[8:12] != b"WAVE":
        raise SystemExit(f"{path}: not a RIFF/WAVE file")
    pos = 12
    fmt = None
    pcm = None
    while pos + 8 <= len(data):
        cid, size = struct.unpack_from("<4sI", data, pos)
        body = data[pos + 8 : pos + 8 + size]
        if cid == b"fmt ":
            fmt = struct.unpack_from("<HHIIHH", body, 0)
        elif cid == b"data":
            pcm = body
        pos += 8 + size + (size & 1)
    if fmt is None or pcm is None:
        raise SystemExit(f"{path}: no fmt or data chunk")
    tag, channels, rate, _, _, bits = fmt
    if tag != 1 or bits != 16 or channels != 2:
        raise SystemExit(f"{path}: expected 16-bit stereo PCM, got tag {tag}, {channels} ch, {bits} bit")
    samples = array.array("h")
    samples.frombytes(pcm[: len(pcm) // 4 * 4])
    if sys.byteorder == "big":
        samples.byteswap()
    return rate, samples[0::2], samples[1::2]


def dbfs(value):
    return 20 * math.log10(value / 32768) if value > 0 else -math.inf


def rms(seq):
    return math.sqrt(sum(x * x for x in seq) / len(seq)) if len(seq) else 0.0


def find_gaps(left, right, start, end, min_len):
    """Runs of frames where both channels are exactly 0, at least min_len long, within [start, end)."""
    gaps = []
    run = 0
    for i in range(start, end):
        if left[i] == 0 and right[i] == 0:
            run += 1
        else:
            if run >= min_len:
                gaps.append((i - run, run))
            run = 0
    if run >= min_len:
        gaps.append((end - run, run))
    return gaps


def find_clicks(chan, start, end, factor, skip, margin=64):
    """Events (clicks within 10 ms merged) where |second difference| exceeds `factor` times the median of
    the 1024-sample block around it. Ignores `margin` samples at the ends and around each gap in `skip`."""
    events = []
    block = 1024
    excluded = [(start - margin, start + margin), (end - margin, end + margin)]
    excluded += [(s - margin, s + n + margin) for s, n in skip]
    for b in range(start, end, block):
        lo = max(b, 2)
        hi = min(b + block, end)
        if hi - lo < 16:
            continue
        d2 = [abs(chan[i] - 2 * chan[i - 1] + chan[i - 2]) for i in range(lo, hi)]
        ref = sorted(d2)[len(d2) // 2] + 8
        for k, v in enumerate(d2):
            i = lo + k
            if v > factor * ref and not any(a <= i <= z for a, z in excluded):
                if events and i - events[-1][0] < 320:
                    events[-1][1] = max(events[-1][1], v)
                else:
                    events.append([i, v, ref])
    return events


def zero_cross_freq(chan, start, end, rate):
    """Frequency from the first and last rising zero crossing (linear interpolation) in [start, end)."""
    first = last = None
    count = 0
    for i in range(start + 1, end):
        a, b = chan[i - 1], chan[i]
        if a < 0 <= b:
            t = (i - 1) + (-a) / (b - a)
            if first is None:
                first = t
            last = t
            count += 1
    if count < 3:
        return None
    return (count - 1) * rate / (last - first)


def fft(values):
    n = len(values)
    a = [complex(v) for v in values]
    j = 0
    for i in range(1, n):
        bit = n >> 1
        while j & bit:
            j ^= bit
            bit >>= 1
        j |= bit
        if i < j:
            a[i], a[j] = a[j], a[i]
    size = 2
    while size <= n:
        w = cmath.exp(-2j * math.pi / size)
        half = size // 2
        tw = [w**k for k in range(half)]
        for s in range(0, n, size):
            for k in range(half):
                u = a[s + k]
                v = a[s + k + half] * tw[k]
                a[s + k] = u + v
                a[s + k + half] = u - v
        size *= 2
    return a


def spectrum_peaks(left, right, start, rate, n=16384, count=6):
    window = [(left[start + i] + right[start + i]) * (0.5 - 0.5 * math.cos(2 * math.pi * i / (n - 1))) for i in range(n)]
    mags = [abs(c) for c in fft(window)[: n // 2]]
    peaks = []
    for k in range(2, n // 2 - 1):
        if mags[k] > mags[k - 1] and mags[k] >= mags[k + 1]:
            # Parabolic interpolation of the peak position
            a, b, c = math.log(mags[k - 1] + 1e-9), math.log(mags[k] + 1e-9), math.log(mags[k + 1] + 1e-9)
            p = 0.5 * (a - c) / (a - 2 * b + c) if (a - 2 * b + c) != 0 else 0.0
            peaks.append(((k + p) * rate / n, mags[k]))
    peaks.sort(key=lambda x: -x[1])
    top = peaks[0][1] if peaks else 1
    return [(f, 20 * math.log10(m / top)) for f, m in peaks[:count]]


NOTE_NAMES = ["C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"]


def nearest_note(freq):
    """Nearest equal-tempered note (A4 = 440 Hz) and the offset from it in cents."""
    semis = 12 * math.log2(freq / 440.0)
    k = round(semis)
    return f"{NOTE_NAMES[(k + 9) % 12]}{4 + (k + 9) // 12}", 100 * (semis - k)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("wav")
    ap.add_argument("--from", dest="t0", type=float, default=0.0, help="analyse from this time (s)")
    ap.add_argument("--to", dest="t1", type=float, default=None, help="analyse up to this time (s)")
    ap.add_argument("--gap-ms", type=float, default=1.0, help="shortest run of exact silence reported as a gap")
    ap.add_argument("--floor", type=int, default=64, help="noise floor for the active part (sample value)")
    ap.add_argument("--tone", help="expected sine frequencies L,R (Hz at --nominal-rate)")
    ap.add_argument("--nominal-rate", type=float, default=32000.0, help="rate the producer generated for")
    ap.add_argument("--tolerance", type=float, default=0.5, help="allowed tone error in percent")
    ap.add_argument("--expect-gaps", type=int, default=None)
    ap.add_argument("--click-factor", type=float, default=0.0,
                    help="report clicks: second difference above this many times the local median (0 = off)")
    ap.add_argument("--spectrum", action="store_true", help="spectral peaks in up to 4 windows")
    ap.add_argument("--fft", type=int, default=16384, help="spectrum window in frames (a power of 2)")
    args = ap.parse_args()
    if args.fft & (args.fft - 1):
        raise SystemExit("--fft must be a power of 2")

    rate, left, right = read_wav(args.wav)
    total = len(left)
    print(f"{args.wav}: {rate} Hz, 16-bit stereo, {total} frames = {total / rate:.2f} s")
    lo = int(args.t0 * rate)
    hi = total if args.t1 is None else min(total, int(args.t1 * rate))
    failed = False

    print("\n   s   RMS L    RMS R   peak L  peak R  clipped")
    for s in range(lo // rate, (hi + rate - 1) // rate):
        a, b = max(lo, s * rate), min(hi, (s + 1) * rate)
        if b <= a:
            continue
        l, r = left[a:b], right[a:b]
        clipped = sum(1 for x in l if x >= 32767 or x <= -32768) + sum(1 for x in r if x >= 32767 or x <= -32768)
        print(f"{s:4d} {dbfs(rms(l)):7.1f}  {dbfs(rms(r)):7.1f}  {max(map(abs, l)):6d}  {max(map(abs, r)):6d}  {clipped:7d}")

    active = [i for i in (range(lo, hi)) if abs(left[i]) > args.floor or abs(right[i]) > args.floor]
    if not active:
        print("\nno signal above the floor")
        return 1
    a0, a1 = active[0], active[-1] + 1
    del active
    print(f"\nactive part: {a0 / rate:.3f} s to {a1 / rate:.3f} s ({(a1 - a0) / rate:.2f} s)")

    gaps = find_gaps(left, right, a0, a1, max(1, int(args.gap_ms * rate / 1000)))
    print(f"gaps of exact silence >= {args.gap_ms} ms inside it: {len(gaps)}")
    for start, n in gaps[:40]:
        print(f"  at {start / rate:8.3f} s: {n} frames ({n * 1000 / rate:.1f} ms)")
    if args.expect_gaps is not None and len(gaps) != args.expect_gaps:
        print(f"FAIL: expected {args.expect_gaps} gaps")
        failed = True

    if args.click_factor > 0:
        for name, chan in (("left", left), ("right", right)):
            clicks = find_clicks(chan, a0, a1, args.click_factor, gaps)
            print(f"clicks ({name}, second difference > {args.click_factor}x local median, away from gaps): "
                  f"{len(clicks)}")
            for i, v, ref in clicks[:10]:
                print(f"  at {i / rate:8.3f} s: {v} (local median {ref})")

    if args.tone:
        want = [float(x) for x in args.tone.split(",")]
        # Longest stretch without a gap
        bounds = [a0] + [x for s, n in gaps for x in (s, s + n)] + [a1]
        stretches = [(bounds[i], bounds[i + 1]) for i in range(0, len(bounds), 2)]
        s0, s1 = max(stretches, key=lambda st: st[1] - st[0])
        print(f"\ntone check on {s0 / rate:.3f}-{s1 / rate:.3f} s:")
        for name, chan, f_want in (("left", left, want[0]), ("right", right, want[1 if len(want) > 1 else 0])):
            f = zero_cross_freq(chan, s0, s1, rate)
            if f is None:
                print(f"  {name}: no tone")
                failed = True
                continue
            # The producer generated f_want cycles per nominal-rate sample; at the WAV's rate that is:
            f_exp = f_want * rate / args.nominal_rate
            err = 100 * (f - f_exp) / f_exp
            ok = abs(err) <= args.tolerance
            failed |= not ok
            print(f"  {name}: {f:.2f} Hz at {rate} Hz, expected {f_exp:.2f} ({f_want:g} Hz at {args.nominal_rate:g}): "
                  f"{err:+.3f}% {'ok' if ok else 'FAIL'}")

    if args.spectrum:
        n = args.fft
        span = a1 - a0 - n
        if span > 0:
            print(f"\nspectral peaks (mix, Hann window, {n} frames; note and cents at the {args.nominal_rate:g} Hz the "
                  f"game mixed for):")
            for w in range(4):
                start = a0 + span * (w + 1) // 5
                parts = []
                for f, db in spectrum_peaks(left, right, start, rate, n):
                    note, cents = nearest_note(f * args.nominal_rate / rate)
                    parts.append(f"{f:.1f} Hz {note}{cents:+.0f}c ({db:+.0f} dB)")
                print(f"  at {start / rate:7.2f} s: {', '.join(parts)}")

    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
