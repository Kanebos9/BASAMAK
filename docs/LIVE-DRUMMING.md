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

## Switching modes

The switch offers **Keep and convert**, **Start fresh**, or **Cancel**. Hover each choice for details. Both conversion and clearing affect all patterns and saved takes; sounds, effects, mixer settings, chains and MIDI assignments remain.

To Live Drumming, conversion keeps hit rhythm, velocity and pan. Step swing/nudge/ratchets become explicit hit times. Melodies become fixed-pitch drum hits, simultaneous chord notes collapse to the loudest strike, and note lengths, per-note slot choices, glide, step modulation and loop conditions do not carry over. Separate channel takes become separate kit takes; they are not guessed into matching performances.

To regular mode, each channel receives one-shot piano-roll notes using its existing tuning. Timing rounds to the regular roll's 384 positions per bar; velocity uses its existing 8-bit resolution. Converted notes expose **Channel tuning** in their right-click menu. Choose **Piano-roll pitch** or move the pitch to use regular melodic notes. Kit takes become separate channel takes for the current pattern groups. A partial-bar/group take uses the regular recorder’s whole-group loading scope; takes spanning unmerged groups are split so they remain accessible. If the notes/takes cannot fit regular-mode limits, conversion leaves everything untouched and explains the limit.

Start fresh clears notes and takes. Cancel changes nothing. Undo restores the previous mode and its sequence data.

## Window size

Use **− / +** beside Presets to change the entire window in 5% increments. The corner resize handle remains available. Scaling preserves the layout's aspect ratio, fits the display's available area, and is saved in the project/preset. The keyboard and sound editor fit within the same scaled layout.
