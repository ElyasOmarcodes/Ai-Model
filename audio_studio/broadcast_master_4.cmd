# Command used for broadcast_master_4.mp3 (times are in the ORIGINAL recording)
# cuts: retakes ("شمسي هوايي ډګر" false start; Congress report take 1 + "دا له سره وایم";
#       ombudsman report takes 1-2), 0.45 s pause inserted at the last joint
# quotes: Bush's demand -> speech style; Taliban reply -> phone style
python3 audio_studio/broadcast_master.py INPUT audio_studio/broadcast_master_4 \
  --cut 103.5:107.6 --cut 120.55:132.6 --cut 202.95:245.45:0.45 \
  --quote 23.45:27.7:speech --quote 29.38:35.3:phone
