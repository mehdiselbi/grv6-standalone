# GRV6Spikes (Phase 0)

## Setup
1. `brew install xcodegen`, then `cd Spikes && xcodegen` to generate GRV6Spikes.xcodeproj (or create an iOS App project by hand and drag in App/*.swift).
2. Run on the iPad mini 6. No special Info.plist keys needed for CoreMIDI or file import.
3. Power: the GRV6 must be powered from its adapter when used with a phone/tablet.
   Use a USB-C hub with PD pass-through so the iPad charges too.

## 0a MIDI logger
- Open the official list next to you: DDJ-GRV6_MIDI_Message_List_E1.pdf (AlphaTheta).
- For each control: type its Fig. id + name (e.g. `D1 PLAY L`, `M2 CH FADER 3`), Arm,
  move/press it through its full range (also with SHIFT, also deck 3/4 layer), Commit.
- Export CSV and send it to me. I diff it against the PDF; mismatches become fixtures.
- LED test: paste 3 bytes from the MIDI-OUT column only (e.g. a pad or JOG indicator row).

## 0b Audio outputs
- Tab Audio: note `outputChannels / max` and the port channel list.
- Step the tone channel 0,1,2,3… and write down which jack it comes out of
  (MASTER RCA, MASTER 1/4", BOOTH, PHONES 1/4", PHONES 3.5).
- Also turn the GRV6 HEADPHONES MIX knob while the tone plays on each channel.

## 0c Hub
- GRV6 + stick on the same hub. Start the tone, keep moving a jog, run Throughput test
  on a 20 MB+ file. Report MB/s, any dropouts heard, any "setup change" lines.

## 0d Stick formats
- On the Mac: `python3 -I ../tools/inspect_stick.py /Volumes/<STICK>` > report.txt
- Also tap "Inventory PIONEER/" on the iPad and screenshot it (confirms iPadOS sees the same tree).
- Export three fixture sticks from rekordbox: Device Library only, OneLibrary only, both.
