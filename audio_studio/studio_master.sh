#!/usr/bin/env bash
# Studio / broadcast-style mastering chain for voice recordings.
# Usage: ./studio_master.sh input.(mp3|wav) output.mp3
# Requires: ffmpeg (sox optional for analysis)
set -euo pipefail
IN="$1"; OUT="$2"
TMP="$(mktemp -d)"; trap 'rm -rf "$TMP"' EXIT

# 1) Synthetic stereo "plate" impulse response (decorrelated L/R, ~1.6 s tail, 18 ms pre-delay)
ffmpeg -hide_banner -loglevel error -y \
  -f lavfi -i "anoisesrc=d=1.6:c=pink:r=44100:a=0.5:seed=11" \
  -f lavfi -i "anoisesrc=d=1.6:c=pink:r=44100:a=0.5:seed=97" \
  -filter_complex "[0][1]join=inputs=2:channel_layout=stereo,
     afade=t=out:st=0:d=1.6:curve=exp,
     lowpass=f=7000,highpass=f=250,
     adelay=18|23[ir]" -map "[ir]" "$TMP/ir.wav"

# 2) Clean-up + vocal EQ + de-ess + compression (dry voice chain)
ffmpeg -hide_banner -loglevel error -y -i "$IN" -af "
  aformat=sample_fmts=fltp:sample_rates=44100:channel_layouts=stereo,
  volume=-3dB,
  highpass=f=75:poles=2,
  afftdn=nf=-30:nr=10:tn=1,
  equalizer=f=120:t=q:w=1.0:g=2.0,
  equalizer=f=320:t=q:w=1.2:g=-3.0,
  equalizer=f=3200:t=q:w=1.0:g=2.5,
  highshelf=f=10000:g=3.5,
  deesser=i=0.4:m=0.5:f=0.5,
  acompressor=threshold=-22dB:ratio=3.5:attack=8:release=120:knee=4:makeup=3,
  acompressor=threshold=-10dB:ratio=6:attack=2:release=60:knee=2
" -c:a pcm_f32le "$TMP/dry.wav"

# 3) Reverb send (wet ~15 dB below dry) + subtle stereo width, then mix with dry
ffmpeg -hide_banner -loglevel error -y -i "$TMP/dry.wav" -i "$TMP/ir.wav" -filter_complex "
  [0]asplit=2[d][s];
  [s][1]afir=dry=1:wet=1:gtype=none[w0];
  [w0]highpass=f=300,volume=0.035[w];
  [d]extrastereo=m=1.15[d2];
  [d2][w]amix=inputs=2:weights=1 1:normalize=0[m]
" -map "[m]" -c:a pcm_f32le "$TMP/mix.wav"

# 4) Two-pass loudness normalisation to -14 LUFS / -1.5 dBTP (streaming/show standard)
J=$(ffmpeg -hide_banner -i "$TMP/mix.wav" -af loudnorm=I=-14:TP=-1.5:LRA=9:print_format=json -f null - 2>&1 | sed -n '/^{/,/^}/p')
g(){ echo "$J" | grep "\"$1\"" | sed -E 's/.*: "([^"]+)".*/\1/'; }
ffmpeg -hide_banner -loglevel error -y -i "$TMP/mix.wav" -af "
  loudnorm=I=-14:TP=-1.5:LRA=9:measured_I=$(g input_i):measured_TP=$(g input_tp):measured_LRA=$(g input_lra):measured_thresh=$(g input_thresh):offset=$(g target_offset):linear=true,
  alimiter=limit=0.84:attack=3:release=50:level=false,
  aresample=44100
" -c:a libmp3lame -b:a 320k "$OUT"
echo "Done -> $OUT"
