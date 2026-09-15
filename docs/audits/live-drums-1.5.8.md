# Live Drumming sound audit — 1.5.8

The final bank contains 274 sounds, including seven new synthesized percussion voices. The ten Live Drums kits combine those voices with existing drums; they require no external samples.

`BankAudit` rendered the final bank at C4 for pitch-normalized, same-category comparison ([report](bank-similarity.txt)). `BankAudit --native-drums` additionally compared all percussion families at each sound’s configured tuning ([report](live-drums-native-1.5.8.txt)). Both use early/late spectral fingerprints and decay measurements, not a listening model.

| New sound | Closest C4 neighbour | Score | Closest native-tuning neighbour | Score |
|---|---|---:|---|---:|
| Trash Stack | Cluster Cymbal | 0.638 | Room Snare | 0.757 |
| Cajon Slap | Woodblock | 0.924 | Mod Snare | 0.785 |
| Udu Pot | 808 Conga | 0.776 | Comet Tom | 0.782 |
| Cabasa | Shaker | 0.623 | Snap Snare | 0.877 |
| Tambourine | Shaker | 0.559 | Bell Ride | 0.815 |
| Flexatone | Mod Wood Block | 0.637 | Bell Cymbal | 0.676 |
| Spring Knock | Static Hit | 0.429 | Mod Metal Plate | 0.778 |

Three proposed additions were removed after the C4 comparison found very close existing sounds. Cajon Slap remains the closest of the retained additions at C4; its wood resonance and noisy slap should be judged by listening in the intended drum register. These scores screen for near-duplicates; they do not establish subjective quality or acoustic realism. The user’s audition remains the final judgement.

`PresetTest` checks every kit row for finite, audible output, an authored sound, and unique MIDI routing. The first eight assignments follow the physical OKTO B pad layout. The revised kits use crash/accent/transition/ride, then kick/snare/one tom/hi-hat. Both 42 and 46 route to channel 8. `LiveDrumTest` sends the physical pad messages through the actual processor while another channel is selected, checking channel order, alternate-note export and fixed full-velocity low sounds. The revision changes kit combinations and performance settings; the 274 bank sounds and similarity reports above remain unchanged. See [the current kit guide](../LIVE-DRUMMING.md).
