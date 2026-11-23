#!/usr/bin/env python3
"""Natural broadcast/YouTube voice-over cleanup for phone-mic recordings.

No reverb and no voice changing: the speaker's own voice is kept and only
cleaned up, thickened and levelled.

Pipeline
  1. decode to 48 kHz mono float
  2. remove retakes (--cut a:b[:pad]) with short fades; pad = seconds of silence inserted
  3. RNNoise denoise (arnndn) + click repair (adeclick)
  4. tone: rumble high-pass, warm/full low end (+body at 100-200 Hz), gentle presence,
     multiband de-esser, de-plosive, parallel compression for density
  5. breath / lip-smack suppressor (see breath_gain): unvoiced, quiet frames away from
     words and short isolated mouth clicks in pauses are turned down ~18 dB
  6. two-pass loudness normalisation to -14 LUFS / -1.5 dBTP, export

Usage
  broadcast_master.py IN OUT_BASENAME [--cut a:b[:pad]] ... [--rnn MODEL.rnnn]
Writes OUT_BASENAME.mp3 (320k, 48 kHz) and OUT_BASENAME.wav (24-bit, 48 kHz).
Requires ffmpeg and numpy; RNNoise models: https://github.com/richardpl/arnndn-models (std.rnnn)
"""
import argparse, json, os, re, subprocess, tempfile
import numpy as np

SR = 48000


def ff(*args):
    subprocess.run(["ffmpeg", "-hide_banner", "-loglevel", "error", "-y", *args], check=True)


def read(path):
    raw = subprocess.run(["ffmpeg", "-loglevel", "error", "-i", path, "-f", "f32le",
                          "-ac", "1", "-ar", str(SR), "-"], capture_output=True, check=True).stdout
    return np.frombuffer(raw, np.float32).astype(np.float64)


def write(path, x):
    x = np.asarray(x)
    subprocess.run(["ffmpeg", "-loglevel", "error", "-y", "-f", "f32le", "-ac", str(1 if x.ndim == 1 else x.shape[1]), "-ar", str(SR),
                    "-i", "-", "-c:a", "pcm_f32le", path], input=np.asarray(x, np.float32).tobytes(), check=True)


def process(x, chain, tmp, name, graph=False):
    a, b = os.path.join(tmp, name + "_in.wav"), os.path.join(tmp, name + "_out.wav")
    write(a, x)
    ff("-i", a, "-filter_complex" if graph else "-af", chain, "-ar", str(SR), "-c:a", "pcm_f32le", b)
    y = read(b)
    return np.pad(y, (0, max(0, len(x) - len(y))))[:len(x)]


def align(y, ref, max_lag=0.2):
    """Undo any processing latency (1 ms envelope cross-correlation)."""
    h = SR // 1000
    def env(v):
        n = len(v) // h * h
        e = np.log(np.sqrt((v[:n].reshape(-1, h) ** 2).mean(1)) + 1e-4)
        return e - e.mean()
    a, b = env(ref), env(y)
    n = min(len(a), len(b)); a, b = a[:n], b[:n]
    N = 1 << (2 * n).bit_length()
    c = np.fft.irfft(np.fft.rfft(b, N) * np.conj(np.fft.rfft(a, N)), N)
    L = int(max_lag * 1000)
    lags = np.r_[np.arange(0, L + 1), np.arange(-L, 0)]
    d = int(lags[np.argmax(np.r_[c[:L + 1], c[-L:]])] * h)
    return np.concatenate([y[d:], np.zeros(d)]) if d > 0 else np.concatenate([np.zeros(-d), y[:len(y) + d]])


def breath_gain(x, depth_db=18.0):
    """Per-sample gain that ducks breaths, lip smacks and low-level noise between words.

    Analysis on 5 ms frames (30 ms windows): level relative to the local speech level
    (95th percentile over 3 s) and voicing (normalised autocorrelation peak, 60-400 Hz).
    Frames above -32 dB rel are grouped into contiguous runs:
      * run containing voiced frames -> speech, untouched (incl. attached kh/sh/s/h sounds)
        plus 30 ms before / 60 ms after
      * unvoiced run, isolated from words -> breath, lip smack or noise burst: full duck
        (only a light -6 dB if it is longer than 40 ms and louder than -20 dB rel)
    Everything quieter than -32 dB rel outside speech (noise floor) is fully ducked.
    """
    hp = x - np.convolve(x, np.ones(240) / 240, "same")  # crude <200 Hz removal for analysis
    hop, W, fs = 240, 1440, 0.005
    n = max(0, (len(x) - W) // hop)
    fr = np.lib.stride_tricks.sliding_window_view(hp, W)[::hop][:n]
    lv = 10 * np.log10((fr ** 2).mean(1) + 1e-12)
    F = np.fft.rfft(fr * np.hanning(W), 2 * W, axis=1)
    ac = np.fft.irfft(np.abs(F) ** 2, axis=1)[:, :W]
    ac /= ac[:, :1] + 1e-12
    v = ac[:, SR // 400:SR // 60].max(1)
    k = 600
    ref = np.array([np.percentile(lv[max(0, i - k // 2):i + k // 2], 95) for i in range(0, n, 50)])
    rel = lv - np.repeat(ref, 50)[:n]
    voiced = (v > 0.5) & (rel > -30)
    # contiguous "active" runs (> -32 dB rel, gaps <= 10 ms bridged); a run that contains
    # voicing is speech (fricatives attached to words included) and is never touched
    active = rel > -32
    for gap in (1, 2):
        active[1:-gap] |= active[:-gap - 1] & active[gap + 1:]
    g_db = np.full(n, -depth_db)
    i = 0
    while i < n:
        if active[i]:
            j = i
            while j < n and active[j]: j += 1
            if voiced[i:j].any():
                g_db[max(0, i - 6):j + 12] = 1.0  # speech: protect, +30 ms before / +60 ms after
            elif (j - i) * fs > 0.04 and rel[i:j].max() >= -20:
                g_db[i:j] = np.minimum(g_db[i:j], -6.0)  # loud isolated unvoiced sound: light duck
            # else: breath / smack / noise burst -> stays at -depth
            i = j
        else:
            i += 1
    g_db = np.minimum(g_db, 0.0)
    g_db = np.convolve(g_db, np.ones(3) / 3, "same")  # 15 ms smoothing in dB
    t_frames = (np.arange(n) * hop + W / 2) / SR
    g = 10 ** (np.interp(np.arange(len(x)) / SR, t_frames, g_db) / 20)
    return g, (g_db < -10).mean()


def room(y, rt60=0.45, wet_db=-22.0, seed=3):
    """Light studio ambience: short decorrelated stereo room tail, returns (n, 2)."""
    n = int(rt60 * 1.2 * SR); t = np.arange(n) / SR
    rng = np.random.default_rng(seed); f = np.fft.rfftfreq(n, 1 / SR)
    shape = (f / 300) ** 2 / (1 + (f / 300) ** 2) / (1 + (f / 7000) ** 4)
    N = 1 << (len(y) + n).bit_length(); Y = np.fft.rfft(y, N); out = []
    for c in range(2):
        h = np.fft.irfft(np.fft.rfft(rng.standard_normal(n)) * shape, n) * 10 ** (-3 * t / rt60)
        h = np.concatenate([np.zeros(int((0.012 + 0.005 * c) * SR)), h]); h /= np.sqrt((h ** 2).sum())
        out.append(y + np.fft.irfft(Y * np.fft.rfft(h, N), N)[:len(y)] * 10 ** (wet_db / 20))
    return np.stack(out, 1)


TONE = ",".join([
    "highpass=f=70:p=2", "highpass=f=70:p=2",
    # measured source: +8 dB at 120-500 Hz vs 500-1k, -12..-20 dB missing above 2 kHz.
    # keep the body (thick/full), only trim boxiness at 300-650 Hz, restore presence moderately
    "firequalizer=gain_entry='entry(0,0);entry(70,0);entry(100,2.5);entry(150,3);entry(200,2);"
    "entry(300,-0.5);entry(450,-2);entry(650,-1.5);entry(900,0);entry(1300,1);entry(2000,3);"
    "entry(2800,5);entry(3800,6);entry(5000,7.5);entry(6500,9);entry(8000,9);entry(10000,6);"
    "entry(12000,1);entry(13000,0);entry(14000,-6);entry(24000,-12)'",
])

DEESS = ("acrossover=split=5500:order=4th[l][h];"
         "[h]acompressor=threshold=-36dB:ratio=5:attack=1:release=50:knee=3[h2];"
         "[l][h2]amix=inputs=2:normalize=0")

DEPLOSIVE = ("asplit=2[a][b];[a]lowpass=f=150:p=2,lowpass=f=150:p=2,"
             "acompressor=threshold=-28dB:ratio=8:attack=2:release=60[lo];"
             "[b]highpass=f=150:p=2,highpass=f=150:p=2[hi];[lo][hi]amix=inputs=2:normalize=0")

# gentle bus compression + parallel ("New York") compression for a dense, full voice
DENSITY = ("acompressor=threshold=-20dB:ratio=2:attack=12:release=180:knee=6:makeup=1.5,"
           "asplit=2[d][p];[p]acompressor=threshold=-36dB:ratio=8:attack=4:release=120:makeup=10,"
           "lowpass=f=6000[p2];[d][p2]amix=inputs=2:weights=1 0.35:normalize=0")


def main():
    p = argparse.ArgumentParser()
    p.add_argument("inp"); p.add_argument("out")
    p.add_argument("--cut", action="append", default=[], help="a:b[:pad_seconds] in original time")
    p.add_argument("--rnn", default=os.path.join(os.path.dirname(os.path.abspath(__file__)), "models", "std.rnnn"))
    p.add_argument("--breath-depth", type=float, default=18.0)
    p.add_argument("--lufs", type=float, default=-14.0)
    p.add_argument("--room", type=float, default=None, help="add light studio room, wet level in dB (e.g. -22)")
    a = p.parse_args()

    x = read(a.inp)
    # --- retake removal ---------------------------------------------------
    cuts = sorted(tuple(float(v) for v in c.split(":")) for c in a.cut)
    pieces, pos = [], 0.0
    f = int(0.01 * SR); fade = np.sin(np.linspace(0, np.pi / 2, f)) ** 2
    for c in cuts + [(len(x) / SR, None)]:
        seg = x[int(pos * SR):int(c[0] * SR)].copy()
        if pos > 0: seg[:f] *= fade
        if c[1] is not None: seg[-f:] *= fade[::-1]
        pieces.append(seg)
        if c[1] is not None:
            pieces.append(np.zeros(int((c[2] if len(c) > 2 else 0.0) * SR)))
            pos = c[1]
    x = np.concatenate(pieces) * 10 ** (-4 / 20)

    with tempfile.TemporaryDirectory() as tmp:
        # --- denoise + click repair ---------------------------------------
        rnn = os.path.join(tmp, "m.rnnn")  # copy: arnndn's option parser dislikes ':' etc. in paths
        with open(a.rnn, "rb") as s, open(rnn, "wb") as d:
            d.write(s.read())
        dn = align(process(x, f"arnndn=m={rnn}:mix=0.9,adeclick=w=20:o=75:t=3:b=2", tmp, "dn"), x)
        gain, frac = breath_gain(dn, depth_db=a.breath_depth)
        # --- tone & dynamics ----------------------------------------------
        y = process(dn, TONE, tmp, "tone")
        y = process(y, DEESS, tmp, "dess", graph=True)
        y = process(y, DEPLOSIVE, tmp, "dep", graph=True)
        y = process(y, DENSITY, tmp, "dens", graph=True)
        y = align(y, dn) * gain  # breath / smack suppression after all dynamics
        if a.room is not None:
            y = room(y, wet_db=a.room)
        # --- loudness / limiting / export ---------------------------------
        mixp = os.environ.get("BM_KEEP_MIX") or os.path.join(tmp, "mix.wav"); write(mixp, y)
        L = a.lufs
        meas = subprocess.run(["ffmpeg", "-hide_banner", "-i", mixp, "-af",
                               f"loudnorm=I={L}:TP=-1.5:LRA=11:print_format=json", "-f", "null", "-"],
                              capture_output=True, text=True).stderr
        j = json.loads(re.search(r"\{[^{}]*\}", meas[meas.rfind("[Parsed_loudnorm"):], re.S).group(0))
        ln = (f"loudnorm=I={L}:TP=-1.5:LRA=11:measured_I={j['input_i']}:measured_TP={j['input_tp']}:"
              f"measured_LRA={j['input_lra']}:measured_thresh={j['input_thresh']}:offset={j['target_offset']}:"
              f"linear=true,alimiter=limit=0.84:attack=3:release=50:level=false,aresample={SR},"
              "aformat=channel_layouts=stereo")
        ff("-i", mixp, "-af", ln, "-c:a", "pcm_s24le", a.out + ".wav")
        ff("-i", a.out + ".wav", "-c:a", "libmp3lame", "-b:a", "320k", a.out + ".mp3")
    print(f"breath/smack frames ducked: {frac * 100:.1f}%")
    print("done:", a.out + ".mp3", a.out + ".wav")


if __name__ == "__main__":
    main()
