# Command used for broadcast_master_4.mp3 (times are in the ORIGINAL recording)
# cuts: retakes ("شمسي هوايي ډګر" false start; Congress report take 1 + "دا له سره وایم";
#       ombudsman report takes 1-2), 0.45 s pause inserted at the last joint
# needs the RNNoise model: curl -L -o audio_studio/models/std.rnnn \
#   https://raw.githubusercontent.com/richardpl/arnndn-models/master/std.rnnn
python3 audio_studio/broadcast_master.py INPUT audio_studio/broadcast_master_4 \
  --cut 103.5:107.6 --cut 120.55:132.6 --cut 202.95:245.45:0.45
