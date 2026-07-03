BASAMAK  -  TouchOSC template  (Ch1 8x8 + M/S/OV + Play/Stop)
============================================================

WHAT IT IS
  A control surface for the latest TouchOSC (hexler.net/touchosc) that matches
  BASAMAK's factory MIDI map "Ch1 8x8 + M/S/OV + Play" out of the box. Everything
  is on MIDI CHANNEL 1:

    - 8x8 grid  : rows = channels 1-8 (colour-coded), cols = steps 1-8.
                  toggle pads -> CC (row*8 + col + 1) = CC 1..64.  Lit = step ON.
    - M / S / OV: three toggle buttons per channel, just LEFT of the step grid
                  (right after the "Channel N" label), each labelled on the button:
                  Mute    = CC 65..72   (channels 1-8)
                  Solo    = CC 73..80
                  Overlap = CC 81..88
    - PLAY      = CC 89   (top right)
    - STOP      = CC 90


SET UP MIDI  (TouchOSC sends nothing until you do this)
  1. On the COMPUTER running your DAW / BASAMAK:
       - Install and run "TouchOSC Bridge" (free, hexler.net). It makes a virtual
         MIDI port and lets your tablet's TouchOSC reach this computer. Leave it on.
  2. In TouchOSC ON THE TABLET, open the CONNECTIONS menu:
       - MIDI:   turn it ON.
       - BRIDGE: turn it ON and pick this computer (it shows up once TouchOSC Bridge
         is running on the same Wi-Fi).
       (Desktop TouchOSC: enable a MIDI connection to a virtual bus, e.g. the macOS
        IAC Driver, instead of the Bridge.)
  3. In your DAW: set the track's MIDI INPUT to "TouchOSC Bridge".
       In the BASAMAK standalone: Options > pick that MIDI input.
  4. In BASAMAK: click the MIDI dropdown (top bar) and choose
       "Ch1 8x8 + M/S/OV + Play"  to load the map.
  5. Tap a pad -> its step lights up. Top-left pad = channel 1, step 1.
     PLAY / STOP run the transport (when BASAMAK's "DAW Sync" is OFF).
