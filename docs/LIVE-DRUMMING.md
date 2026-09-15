# Live Drumming (1.5.8)

LIVE DRUMMING, beside the pattern numbers, makes MIDI notes select instruments across the whole kit. Selecting a channel still selects its sound editor; it does not redirect your pads. The shared roll has one row per sound channel, including the channels reached by scrolling.

## Connect an OKTO B

Connect the pad's USB Type B MIDI port to your Mac. Enable its MIDI input in your host and send it to BASAMAK. The supplied manual calls the device EDRUM; the supplied Mac troubleshooting notes identify it as OKTOB. In REAPER, use the device as the track's MIDI input, arm the track, and enable input monitoring. In BASAMAK Standalone, enable it in the audio/MIDI settings.

Enable LIVE DRUMMING. Open **Routing > Channel > MIDI In > Learn**, then strike the pad for that sound. The learning strike makes the assignment; strike again to hear it. Repeat for your other pads. You can also choose note numbers and MIDI channels manually. An overlapping assignment moves to its new sound channel, so one pad does not accidentally trigger two sounds. MIDI In assignments are independent of MIDI Out settings and are saved with the preset/project.

The OKTO B manual lists MIDI channel 10 and notes 36 (kick), 37 (rim), 38 (snare), 42 (closed hi-hat), 43 (low tom), 44 (pedal hi-hat), 45 (mid tom), 46 (open hi-hat), 48 (high tom), 49 (crash), and 51 (ride). Your selected kit/pad programming determines which eight you actually send: Learn avoids guessing. BASAMAK initially accepts any MIDI channel; Learn captures the actual channel. Note numbers select sounds at their configured tuning; strike velocity controls hit velocity. Note releases do not shorten a drum hit. MIDI Out uses its separately configured note/channel and a 10 ms drum gate.

## Record and edit

Open **KEYS/RECORD** and press **REC KIT**. Keyboard performance controls are dimmed, while recording controls remain active.

- **This pattern** records all channels into one take per pass through the displayed pattern or merged group. It temporarily loops the group in bar order.
- **Follow chain** uses each bar's playback settings and saves one kit take per visited bar.
- **Pad starts** arms recording until the first assigned pad starts BASAMAK's stopped transport. **3s count-in** gives a three-second countdown. With DAW Sync enabled, start playback in the DAW.
- Each new pass replaces the current drum notes; this is not overdubbing. Stopping REC keeps the latest captured take and lets playback continue. Closing the editor also finishes kit recording. STOP cancels a countdown too. Pause, mode switching, preset selection and note editing are unavailable during kit recording.

The **Takes** menu loads, renames and deletes kit takes, or saves your edits as a new take. Loading replaces the kit's notes in that take's bars after confirmation. The limits are 20 takes per starting pattern and 1000 per preset; the roll allows 2048 hits per bar across all channels. Recording stops at capacity and reports it.

Recording retains incoming timing. **Snap** only affects added/moved hits; **Quantize** applies the chosen grid to the displayed kit. Click to add, drag to move, Shift-drag vertically or use the wheel over a hit for velocity. Right-click offers velocity, pan and deletion. Delete removes the selected hit. **Clear** clears the whole displayed kit, leaving saved takes. **DRAG MIDI** exports the whole displayed kit using its input-note assignments.

Mute, Solo, Overlap, sound editing, effects, aux routing, MIDI Out, choke groups and ducking remain available. Step editing, keyboard Merge & Split and Generate are unavailable in this mode. Swing is read-only because drum hit timing is explicit. Pattern merging remains available while stopped/not recording; merged bars share sound settings as before. Existing sound voicing and effects remain part of each instrument.

## Overlap, Duck and Choke

These controls remain your choice in Live Drumming:

- **OV On:** successive hits on the same channel can ring together. **OV Off:** the next hit fades the previous tail, including tails still sounding from another bar. It applies to the current pattern or merged group. Short sounds still finish naturally.
- **Routing > Channel > Duck:** choose which other channel temporarily lowers this channel’s level, and how much. **Off** or zero amount disables it. The sound recovers after the triggering hit; tails from earlier bars are included.
- **Routing > Channel > Choke group:** channels in the same nonzero group cut each other’s tails, including across bars. **Off (no choke)** removes the channel from choke groups. Choke can cut another channel even when that other channel has OV On; OV governs repeats on the same channel. Factory kits group closed/open/pedal hat positions together.

Cutting a voice does not erase reverb or delay already sent to the master effects. Their Gate/Trail controls handle those effect tails separately. In the regular piano roll, note lengths and keyboard Poly govern overlap, so OV is dimmed there.

## Switching modes

If every pattern and saved-take list is empty, switching modes is immediate. Otherwise the switch offers **Keep and convert**, **Start fresh**, or **Cancel**. Hover each choice for details. Both conversion and clearing affect all patterns and saved takes; sounds, effects, mixer settings, chains and MIDI assignments remain.

To Live Drumming, conversion keeps hit rhythm, velocity and pan. Step swing/nudge/ratchets become explicit hit times. Melodies become fixed-pitch drum hits, simultaneous chord notes collapse to the loudest strike, and note lengths, per-note slot choices, glide, step modulation and loop conditions do not carry over. Separate channel takes become separate kit takes; they are not guessed into matching performances.

To regular mode, each channel receives one-shot piano-roll notes using its existing tuning. Timing is preserved with fractional note positions; velocity uses the regular roll's existing 8-bit resolution. Converted notes expose **Channel tuning** in their right-click menu. Choose **Piano-roll pitch** or move the pitch to use regular melodic notes. Kit takes become separate channel takes for the current pattern groups. A partial-bar/group take uses the regular recorder’s whole-group loading scope; takes spanning unmerged groups are split so they remain accessible. If the notes/takes cannot fit regular-mode limits, conversion leaves everything untouched and explains the limit.

Start fresh clears notes and takes. Cancel changes nothing. Undo restores the previous mode and its sequence data.

## Window size

Use **− / +** beside Presets to change the entire window in 5% increments. The corner resize handle remains available. Scaling preserves the layout's aspect ratio, fits the display's available area, and is saved in the project/preset. The keyboard and sound editor fit within the same scaled layout.

## Timing precision

Both piano rolls retain fractional note positions. The regular roll still uses 384 logical columns per bar to describe its grid, but starts and lengths are no longer restricted to whole columns. Snap Off allows continuous placement; recording does not snap. MIDI keyboard input is processed at the sample offsets supplied by the host, including note releases. Saved takes, undo, project files and mode conversion retain the fine timing. Existing integer-position projects load at exactly their previous positions.

DRAG MIDI uses **9600 ticks per quarter note** in both modes, about **0.052 ms per tick at 120 BPM**. This is an export-file resolution; it does not impose a grid on either internal roll. Exports include the displayed pattern/group's silent remainder. Drum exports include all rows with input-note assignments (Any channel becomes MIDI channel 10). Per-hit pan and sound/effect settings are not carried by the MIDI file.

## Ready-to-play factory kits

Find these under **Presets > Factory**. They open in Live Drumming mode with empty sequences, all 64 patterns using the same kit, and no saved takes. All sounds are synthesized in BASAMAK, so no external sample library is required. These are stylistic kits, not sampled replicas of acoustic instruments.

| Preset | Character |
|---|---|
| Live Drums - Room Session | Short kick, resonant snare, rounded toms |
| Live Drums - Dry Funk | Short kick and snare, tight hats, dry accents |
| Live Drums - Brush Lounge | Brushed snare, soft kick, sizzle and bell ride |
| Live Drums - 808 Circuit | Booming kick, electronic toms, metallic hats and clap |
| Live Drums - Warehouse | Punchy 909-style drums, bright hats and processed clap |
| Live Drums - Dusty Breaks | Boxy kick, roomy snare, compressed clap and rough cymbals |
| Live Drums - Cajon Circle | Wooden bass/slap, rattles and hand-percussion voices |
| Live Drums - Skin and Clay | Skin drum, tabla, udu, conga and bongo |
| Live Drums - Cinematic Ritual | Deep drums, gong, metal and longer resonances |
| Live Drums - Scrap Yard | Steel kick, spring knock, FM clang and trash stack |

Every kit preserves the OKTO B's normal physical pad positions, viewed from the player (L/R CH and X-STICK off):

| Physical position | BASAMAK channel | Normal role | MIDI note |
|---|---:|---|---:|
| Top left | 1 | Crash | 49 |
| Top middle-left | 2 | Tom 1 (high) | 48 |
| Top middle-right | 3 | Tom 2 (mid) | 45 |
| Top right | 4 | Ride | 51 |
| Bottom left | 5 | Kick | 36 |
| Bottom middle-left | 6 | Snare | 38 |
| Bottom middle-right | 7 | Tom 3 (low) | 43 |
| Bottom right | 8 | Closed hi-hat | 42 |

Sounds, MIDI In and MIDI Out notes follow that order together. Rows 9–11 provide the alternate open hi-hat (46), snare rim (37) and pedal hi-hat (44) triggers. Rows 12–16 add **39, 54, 56, 75, 82** for extra percussion. Every kit accepts any MIDI channel. New default input assignments also follow the physical order; saved project/preset assignments load unchanged. Closed/open/pedal positions (42/46/44) share a choke group. The same pad keeps its role when switching kits, although hand-percussion and experimental kits replace conventional drums with their own textures. If your OKTO kit has edited MIDI KEY assignments, use Routing > MIDI In > Learn.

Seven new sounds are available individually in Sound Bank: **Trash Stack, Cajon Slap, Udu Pot, Cabasa, Tambourine, Flexatone, Spring Knock**. All their sound settings remain visible and editable.
