#!/usr/bin/env bash
# "Slowed" nasheed style, matched to a reference clip (WhatsApp video VID-20261126-WA0112):
#   measured vs. the original: speed AND pitch x0.865 (tape-style, ~-2.5 semitones),
#   highs softened above 5 kHz (-2..-5 dB), little extra reverb (a faint hall, -24 dB),
#   loudness ~ -11.5 LUFS like the reference.
# Usage: ./slowed_style.sh input output.mp3 [speed=0.865]
set -euo pipefail
IN="$1"; OUT="$2"; SP="${3:-0.865}"
TMP="$(mktemp -d)"; trap 'rm -rf "$TMP"' EXIT
ffmpeg -hide_banner -loglevel error -y -i "$IN" -map 0:a:0 -af "
  aformat=sample_fmts=fltp:channel_layouts=stereo,aresample=48000,
  asetrate=48000*$SP,aresample=48000:resampler=soxr,
  firequalizer=gain_entry='entry(0,0);entry(4000,0);entry(5000,-1.5);entry(6500,-2.5);entry(8000,-4);entry(10000,-5);entry(12000,-6);entry(24000,-8)'
" -c:a pcm_f32le "$TMP/slow.wav"
ffmpeg -hide_banner -loglevel error -y -i "$TMP/slow.wav" \
  -f lavfi -i "anoisesrc=d=2.8:c=pink:r=48000:a=0.5:seed=5" -f lavfi -i "anoisesrc=d=2.8:c=pink:r=48000:a=0.5:seed=77" \
  -filter_complex "[1][2]join=inputs=2:channel_layout=stereo,afade=t=out:st=0:d=2.8:curve=exp,highpass=f=200,lowpass=f=5000,adelay=20|26[ir];
    [0]asplit[d][s];[s][ir]afir=dry=1:wet=1:gtype=none,volume=0.004[w];[d][w]amix=inputs=2:normalize=0[m]" \
  -map "[m]" -c:a pcm_f32le "$TMP/mix.wav"
J=$(ffmpeg -hide_banner -i "$TMP/mix.wav" -af loudnorm=I=-11.5:TP=-1:LRA=7:print_format=json -f null - 2>&1 | sed -n '/^{/,/^}/p')
g(){ echo "$J" | grep "\"$1\"" | sed -E 's/.*: "([^"]+)".*/\1/'; }
ffmpeg -hide_banner -loglevel error -y -i "$TMP/mix.wav" -af "loudnorm=I=-11.5:TP=-1:LRA=7:measured_I=$(g input_i):measured_TP=$(g input_tp):measured_LRA=$(g input_lra):measured_thresh=$(g input_thresh):offset=$(g target_offset):linear=true,alimiter=limit=0.89:level=false,aresample=48000" -c:a libmp3lame -b:a 320k "$OUT"
echo "Done -> $OUT"
