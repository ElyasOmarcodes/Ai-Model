#!/usr/bin/env bash
# Studio / broadcast-style mastering chain for voice recordings.
# Usage: ./studio_master.sh input.(mp3|wav) output.mp3 [normal|strong]
#   normal : clean broadcast voice, subtle room
#   strong : bigger "show" sound - longer/louder plate, more air & warmth,
#            harmonic exciter, wider stereo image, firmer compression
# Requires: ffmpeg (sox optional for analysis)
set -euo pipefail
IN="$1"; OUT="$2"; PRESET="${3:-normal}"
TMP="$(mktemp -d)"; trap 'rm -rf "$TMP"' EXIT

if [ "$PRESET" = "strong" ]; then
  IR_LEN=2.4; PREDELAY="30|41"; IR_LP=8500; WET=0.075
  LOW_G=3.0; MUD_G=-3.5; PRES_G=3.5; AIR_G=5.0
  EXCITER="aexciter=amount=1.2:drive=6:freq=3500:blend=0,"
  COMP1="threshold=-24dB:ratio=4:attack=6:release=110:knee=4:makeup=4"
  WIDTH="stereowiden=delay=18:feedback=0.15:crossfeed=0.25:drymix=0.9"
else
  IR_LEN=1.6; PREDELAY="18|23"; IR_LP=7000; WET=0.035
  LOW_G=2.0; MUD_G=-3.0; PRES_G=2.5; AIR_G=3.5
  EXCITER=""
  COMP1="threshold=-22dB:ratio=3.5:attack=8:release=120:knee=4:makeup=3"
  WIDTH="extrastereo=m=1.15"
fi

# 1) Synthetic stereo "plate" impulse response (decorrelated L/R noise with exponential decay)
ffmpeg -hide_banner -loglevel error -y \
  -f lavfi -i "anoisesrc=d=$IR_LEN:c=pink:r=44100:a=0.5:seed=11" \
  -f lavfi -i "anoisesrc=d=$IR_LEN:c=pink:r=44100:a=0.5:seed=97" \
  -filter_complex "[0][1]join=inputs=2:channel_layout=stereo,
     afade=t=out:st=0:d=$IR_LEN:curve=exp,
     lowpass=f=$IR_LP,highpass=f=250,
     adelay=$PREDELAY[ir]" -map "[ir]" "$TMP/ir.wav"

# 2) Clean-up + vocal EQ + de-ess + compression (dry voice chain)
ffmpeg -hide_banner -loglevel error -y -i "$IN" -af "
  aformat=sample_fmts=fltp:sample_rates=44100:channel_layouts=stereo,
  volume=-3dB,
  highpass=f=75:poles=2,
  afftdn=nf=-30:nr=10:tn=1,
  equalizer=f=120:t=q:w=1.0:g=$LOW_G,
  equalizer=f=320:t=q:w=1.2:g=$MUD_G,
  equalizer=f=3200:t=q:w=1.0:g=$PRES_G,
  highshelf=f=10000:g=$AIR_G,
  $EXCITER
  deesser=i=0.4:m=0.5:f=0.5,
  acompressor=$COMP1,
  acompressor=threshold=-10dB:ratio=6:attack=2:release=60:knee=2
" -c:a pcm_f32le "$TMP/dry.wav"

# 3) Reverb send + stereo width, then mix with dry
ffmpeg -hide_banner -loglevel error -y -i "$TMP/dry.wav" -i "$TMP/ir.wav" -filter_complex "
  [0]asplit=2[d][s];
  [s][1]afir=dry=1:wet=1:gtype=none[w0];
  [w0]highpass=f=300,volume=$WET[w];
  [d]$WIDTH[d2];
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
