#!/usr/bin/env python3
"""Broadcast/YouTube voice-over mastering with retake removal and quoted-voice styling.

Pipeline
  1. decode to 48 kHz mono float
  2. remove retakes (--cut a:b[:pad]) with short fades; pad = seconds of silence inserted
  3. corrective "bad mic" chain: high-pass, de-plosive (low-band compressor),
     corrective EQ (de-mud + restore presence/air), harmonic exciter, de-esser,
     light denoise, gentle compression
  4. quoted passages (--quote a:b:style, times in ORIGINAL file) are routed to a
     separate voice chain so they read as another speaker:
        speech : deeper pitch, "archival speech" EQ + warmth, large hall
        phone  : slightly higher pitch, telephone/radio band, grit, small room
     the narrator gets a subtle stereo plate
  5. sum, two-pass loudness normalisation to -14 LUFS / -1.5 dBTP, export

Usage
  broadcast_master.py IN OUT_BASENAME --cut 103.5:107.6 --quote 23.45:27.7:speech ...
Writes OUT_BASENAME.mp3 (320k, 48 kHz) and OUT_BASENAME.wav (24-bit, 48 kHz).
Requires ffmpeg (with rubberband) and numpy.
"""
import argparse, json, os, re, subprocess, tempfile
import numpy as np

SR = 48000


def ff(*args):
    subprocess.run(["ffmpeg", "-hide_banner", "-loglevel", "error", "-y", *args], check=True)


def read(path, ch=1):
    raw = subprocess.run(["ffmpeg", "-loglevel", "error", "-i", path, "-f", "f32le",
                          "-ac", str(ch), "-ar", str(SR), "-"], capture_output=True, check=True).stdout
    x = np.frombuffer(raw, np.float32).astype(np.float64)
    return x.reshape(-1, ch) if ch > 1 else x


def write(path, x):
    x = np.asarray(x, np.float32)
    ch = 1 if x.ndim == 1 else x.shape[1]
    subprocess.run(["ffmpeg", "-loglevel", "error", "-y", "-f", "f32le", "-ac", str(ch), "-ar", str(SR),
                    "-i", "-", "-c:a", "pcm_f32le", path], input=x.tobytes(), check=True)


def process(x, chain, tmp, name, graph=False):
    a, b = os.path.join(tmp, name + "_in.wav"), os.path.join(tmp, name + "_out.wav")
    write(a, x)
    ff("-i", a, "-filter_complex" if graph else "-af", chain, "-ar", str(SR), "-c:a", "pcm_f32le", b)
    y = read(b)
    n = len(x)
    return np.pad(y, (0, max(0, n - len(y))))[:n]


def lag_of(y, ref, max_lag=0.2):
    """Delay (samples) of y relative to ref, by cross-correlation of 1 ms envelopes, then refined."""
    h = SR // 1000
    def env(v):
        n = len(v) // h * h
        return np.log(np.sqrt((v[:n].reshape(-1, h) ** 2).mean(1)) + 1e-4)
    a, b = env(ref), env(y)
    n = min(len(a), len(b)); a, b = a[:n] - a[:n].mean(), b[:n] - b[:n].mean()
    N = 1 << (2 * n).bit_length()
    c = np.fft.irfft(np.fft.rfft(b, N) * np.conj(np.fft.rfft(a, N)), N)
    L = int(max_lag * 1000)
    lags = np.r_[np.arange(0, L + 1), np.arange(-L, 0)]
    return int(lags[np.argmax(np.r_[c[:L + 1], c[-L:]])] * h)


def align(y, ref):
    d = lag_of(y, ref)
    y = np.concatenate([y[d:], np.zeros(d)]) if d > 0 else np.concatenate([np.zeros(-d), y[:len(y) + d]])
    return y


def ramp_mask(n, regions, fade=0.02):
    """1 inside regions, raised-cosine ramps of `fade` s centred on the edges."""
    t = np.arange(n) / SR
    m = np.zeros(n)
    for a, b in regions:
        up = np.clip((t - (a - fade / 2)) / fade, 0, 1)
        dn = np.clip(((b + fade / 2) - t) / fade, 0, 1)
        m = np.maximum(m, np.minimum(up, dn))
    return 0.5 - 0.5 * np.cos(np.pi * m)


def reverb_ir(rt60, predelay, lo, hi, seed, length=None):
    """Decorrelated stereo noise-tail IR, unit energy per channel."""
    length = length or rt60 * 1.1
    n = int(length * SR)
    rng = np.random.default_rng(seed)
    t = np.arange(n) / SR
    env = 10 ** (-3 * t / rt60)
    f = np.fft.rfftfreq(n, 1 / SR)
    shape = np.where(f > 0, 1 / np.sqrt(np.maximum(f, 1)), 0)  # pink
    shape *= 1 / (1 + (f / hi) ** 4) * (f / lo) ** 2 / (1 + (f / lo) ** 2)  # band-limit
    out = []
    for c in range(2):
        w = np.fft.irfft(np.fft.rfft(rng.standard_normal(n)) * shape, n) * env
        w = np.concatenate([np.zeros(int((predelay + 0.004 * c) * SR)), w])
        out.append(w / np.sqrt((w ** 2).sum()))
    L = max(len(o) for o in out)
    return np.stack([np.pad(o, (0, L - len(o))) for o in out], 1)


def convolve(x, ir):
    n = len(x) + len(ir) - 1
    N = 1 << (n - 1).bit_length()
    X = np.fft.rfft(x, N)
    return np.stack([np.fft.irfft(X * np.fft.rfft(ir[:, c], N), N)[:len(x)] for c in range(2)], 1)


def rms(x, m):
    w = m > 0.99
    return np.sqrt((x[w] ** 2).mean()) if w.any() else 1.0


CLEAN_STEPS = [
    "volume=-4dB",
    "highpass=f=75:p=2", "highpass=f=75:p=2",
    # corrective EQ (measured: +8 dB boom at 120-500 Hz, -12..-20 dB missing above 2 kHz):
    # de-mud / de-box, restore presence and air lost by the mic + low-bitrate codec
    "firequalizer=gain_entry='entry(0,0);entry(100,0);entry(150,-3);entry(200,-5);entry(300,-6);"
    "entry(450,-4);entry(650,-1.5);entry(900,0);entry(1300,1.5);entry(2000,4);entry(2800,6.5);"
    "entry(3800,8);entry(5000,10);entry(6500,12);entry(8000,13);entry(9500,12);entry(11000,9);"
    "entry(12000,5);entry(13000,0);entry(14000,-6);entry(24000,-12)'",
    "afftdn=nf=-55:nr=8:tn=1",  # (CLEAN_STEPS index split point: DEESS graph runs before this)
    "acompressor=threshold=-20dB:ratio=2.5:attack=10:release=150:knee=4:makeup=2",
]
CLEAN = ",".join(CLEAN_STEPS)

# parallel exciter: saturate the 3-7 kHz band and add back only the new harmonics above 7 kHz
EXCITER = ("asplit=2[d][e];[e]highpass=f=3000:p=2,highpass=f=3000:p=2,volume=12dB,"
           "asoftclip=type=tanh:threshold=0.3,highpass=f=7000:p=2,highpass=f=7000:p=2,"
           "lowpass=f=15000:p=2,lowpass=f=15000:p=2,volume=-12dB[h];[d][h]amix=inputs=2:weights=1 0.6:normalize=0")

# de-esser: compress only the 5.5 kHz+ band (ffmpeg's deesser filter adds clicks at word onsets)
DEESS = ("acrossover=split=5500:order=4th[l][h];"
         "[h]acompressor=threshold=-36dB:ratio=5:attack=1:release=50:knee=3[h2];"
         "[l][h2]amix=inputs=2:normalize=0")

# de-plosive: compress only the band below 150 Hz, fast
DEPLOSIVE = ("asplit=2[a][b];[a]lowpass=f=150:p=2,lowpass=f=150:p=2,"
             "acompressor=threshold=-32dB:ratio=10:attack=2:release=60[lo];"
             "[b]highpass=f=150:p=2,highpass=f=150:p=2[hi];[lo][hi]amix=inputs=2:normalize=0")

STYLES = {
    # pitch ratio, chain, reverb (rt60, predelay, lo, hi, wet dB), level offset dB vs narrator
    "speech": dict(
        chain="rubberband=pitch=0.89:formant=shifted:pitchq=quality,"
              "highpass=f=140:p=2,lowpass=f=7000:p=2,"
              "equalizer=f=220:t=q:w=1:g=3,equalizer=f=1600:t=q:w=1:g=3,"
              "asoftclip=type=atan:threshold=0.6:output=1.2,lowpass=f=7500:p=2,"
              "acompressor=threshold=-24dB:ratio=4:attack=5:release=100:makeup=3",
        rev=(1.9, 0.035, 300, 6000, -10), level=-1.0),
    "phone": dict(
        chain="rubberband=pitch=1.06:formant=shifted:pitchq=quality,"
              "highpass=f=380:p=2,highpass=f=380:p=2,lowpass=f=3300:p=2,lowpass=f=3300:p=2,"
              "equalizer=f=1800:t=q:w=1.2:g=5,"
              "asoftclip=type=tanh:threshold=0.4:output=1.4,lowpass=f=3600:p=2,lowpass=f=3600:p=2,"
              "acompressor=threshold=-26dB:ratio=6:attack=3:release=80:makeup=4",
        rev=(0.35, 0.008, 400, 3500, -17), level=-1.5),
}
NARRATOR_REV = (1.5, 0.02, 250, 8000, -19)


def main():
    p = argparse.ArgumentParser()
    p.add_argument("inp"); p.add_argument("out")
    p.add_argument("--cut", action="append", default=[], help="a:b[:pad_seconds] in original time")
    p.add_argument("--quote", action="append", default=[], help="a:b:style in original time")
    a = p.parse_args()

    x = read(a.inp)
    # --- 1. retake removal -------------------------------------------------
    cuts = sorted(tuple(float(v) for v in c.split(":")) for c in a.cut)
    pieces, tmap, pos, out_t = [], [], 0.0, 0.0
    f = int(0.01 * SR); fade = np.sin(np.linspace(0, np.pi / 2, f)) ** 2
    for c in cuts + [(len(x) / SR, None)]:
        seg = x[int(pos * SR):int(c[0] * SR)].copy()
        if pos > 0: seg[:f] *= fade
        if c[1] is not None: seg[-f:] *= fade[::-1]
        tmap.append((pos, c[0], out_t - pos)); pieces.append(seg); out_t += len(seg) / SR
        if c[1] is not None:
            pad = c[2] if len(c) > 2 else 0.0
            pieces.append(np.zeros(int(pad * SR))); out_t += int(pad * SR) / SR
            pos = c[1]
    x = np.concatenate(pieces)

    def mapt(t):
        for s, e, off in tmap:
            if s <= t <= e: return t + off
        raise ValueError(f"time {t} falls inside a cut")

    quotes = []
    for q in a.quote:
        s, e, st = q.split(":"); quotes.append((mapt(float(s)), mapt(float(e)), st))

    with tempfile.TemporaryDirectory() as tmp:
        # --- 2. corrective chain ------------------------------------------
        k = next(i for i, st in enumerate(CLEAN_STEPS) if st.startswith("afftdn"))
        clean = process(x, ",".join(CLEAN_STEPS[:k]), tmp, "pre")
        clean = process(clean, EXCITER, tmp, "exc", graph=True)
        clean = process(clean, DEESS, tmp, "dess", graph=True)
        clean = process(clean, ",".join(CLEAN_STEPS[k:]), tmp, "post")
        clean = process(clean, DEPLOSIVE, tmp, "dep", graph=True)
        clean = align(clean, x)

        n = len(clean)
        masks = {st: ramp_mask(n, [(s, e) for s, e, q in quotes if q == st]) for st in {q for *_, q in quotes}}
        narr_m = 1 - sum(masks.values()) if masks else np.ones(n)

        def with_reverb(dry, rev, seed):
            rt, pre, lo, hi, wet_db = rev
            wet = convolve(dry, reverb_ir(rt, pre, lo, hi, seed))
            return dry[:, None] + wet * 10 ** (wet_db / 20)

        narr_dry = clean * narr_m
        mix = with_reverb(narr_dry, NARRATOR_REV, 1)
        ref = rms(clean, narr_m)
        for i, (st, m) in enumerate(masks.items()):
            cfg = STYLES[st]
            y = process(clean * m, cfg["chain"], tmp, st)
            y = align(y, clean * m) * m  # re-align and re-mask after pitch shifting
            y *= ref / rms(y, m) * 10 ** (cfg["level"] / 20)
            mix += with_reverb(y, cfg["rev"], 10 + i)

        # --- 3. loudness / limiting / export -------------------------------
        mixp = os.environ.get("BM_KEEP_MIX") or os.path.join(tmp, "mix.wav"); write(mixp, mix)
        meas = subprocess.run(["ffmpeg", "-hide_banner", "-i", mixp, "-af",
                               "loudnorm=I=-14:TP=-1.5:LRA=11:print_format=json", "-f", "null", "-"],
                              capture_output=True, text=True).stderr
        j = json.loads(re.search(r"\{[^{}]*\}", meas[meas.rfind("[Parsed_loudnorm"):], re.S).group(0))
        ln = (f"loudnorm=I=-14:TP=-1.5:LRA=11:measured_I={j['input_i']}:measured_TP={j['input_tp']}:"
              f"measured_LRA={j['input_lra']}:measured_thresh={j['input_thresh']}:offset={j['target_offset']}:"
              f"linear=true,alimiter=limit=0.84:attack=3:release=50:level=false,aresample={SR}")
        ff("-i", mixp, "-af", ln, "-c:a", "pcm_s24le", a.out + ".wav")
        ff("-i", a.out + ".wav", "-c:a", "libmp3lame", "-b:a", "320k", a.out + ".mp3")
    print("quotes (output time):", [(round(s, 2), round(e, 2), q) for s, e, q in quotes])
    print("done:", a.out + ".mp3", a.out + ".wav")


if __name__ == "__main__":
    main()
