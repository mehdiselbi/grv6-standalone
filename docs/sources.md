# Sources (public only)

## DDJ-GRV6
- Official MIDI message list: https://downloads.support.alphatheta.com/software_info/dj-controllers/DDJ-GRV6/DDJ-GRV6_MIDI_Message_List_E1.pdf
  (linked from https://support.alphatheta.com/en-US/articles/37284057102489)
- Firmware 1.02: https://alphatheta.com/en/information/update-ddj-grv6-firmware-ver-1-02/

Facts read from the official list (still to be confirmed by Phase 0a capture):
- Decks 1-4: MIDI ch 1-4 (status 9n / Bn) for everything except performance pads.
- Pads: separate channels, SHIFT layer on the next channel (8/10/12/14 and 9/11/13/15 per the list).
- Browse/global and mixer master section: ch 7 (0x96 / 0xB6). Beat FX: ch 5 (0x94 / 0xB4).
- Faders and knobs: 14-bit, MSB CC + LSB CC (LSB = MSB + 0x20).
- Smart rotary selector: relative, CW 0x01-0x1E, CCW 0x7F-0x62.
- MIDI-OUT on ch 16: LOAD illumination notes 0x00-0x03, JOG indicator CC 0x00-0x03 value 0x00-0x48.
- Channel level meter: MIDI-OUT Bn CC 0x02 with documented thresholds.

## XDJ-AZ (UI reference only)
- Manual DRI1936C: https://downloads.support.alphatheta.com/manuals/all-in-one-dj-systems/XDJ-AZ/XDJ-AZ_DRI1936C_manual_EN.pdf
- Firmware 2.00: https://support.alphatheta.com/en-US/articles/37072403941145
- Firmware 1.30: https://alphatheta.com/en/information/update-xdj-az-firmware-ver-1-30/

## rekordbox exports
- Deep Symmetry export analysis: https://djl-analysis.deepsymmetry.org/rekordbox-export-analysis/exports.html
- crate-digger: https://github.com/Deep-Symmetry/crate-digger
- rekordcrate (MPL-2.0): https://github.com/Holzhaus/rekordcrate
- pyrekordbox Device Library Plus / OneLibrary: https://pyrekordbox.readthedocs.io/en/latest/formats/devicelib_plus.html
- rbox: https://docs.rs/rbox
- rekordbox USB export guide (Device Library vs OneLibrary): https://cdn.rekordbox.com/files/20251021171528/USB_export_guide_en_251007.pdf

## DSP
- Signalsmith Stretch (MIT): https://github.com/Signalsmith-Audio/signalsmith-stretch

## Values taken from public third-party implementations (2026-10-21)
Source: Mixxx mapping for DDJ-GRV6 by joergbeyer, https://github.com/joergbeyer/dj-controller
- Jog: relative, centre 0x40 (value - 64 = ticks, <64 reverse). Scratch uses 720 intervals per revolution at 33 1/3 rpm. Configurable in app.
- Jog CCs per deck (Bn): 0x21 pitch bend (wheel side), 0x22 platter turn vinyl on, 0x23 vinyl off, 0x29 SHIFT search. Touch notes (9n): 0x36, 0x67 with SHIFT. Matches official list.
- LEDs: on 0x7F, off 0x00 only. No colour values anywhere (official list also documents OFF=0x00 ON=0x7F only). Pads treated as single-colour.
- Hot cue pad LEDs deck 1: status 0x97 note 0x00+i. Sampler pads 0x97 0x30+i.
- VU meter (Bn CC 0x02): 0x26 one green, 0x41 two greens, 0x57 orange, 0x65 two oranges, 0x77 red. Matches official list.

## Audio on iPad
- Not documented publicly. Indirect evidence: djay iPad users cue on the GRV6 headphone jack with the channel CUE buttons (Algoriddim forum, 21 Aug 2025: https://community.algoriddim.com/t/ddj-grv6-two-questions/37725), so iOS sees separate master and headphone outputs. Confirm channel count once with the Audio tab Refresh.

## Official MIDI list, rows read 2026-10-21 (pdf.js text extraction)
- Browse ch 7: rotate CC 0x40 (+SHIFT 0x64); press 0x41 (+SHIFT 0x42); DISCOVER 0x35 (0x68); BACK 0x65 (0x66);
  VIEW 0x7A (0x67); PREVIEW 0x36 (0x37); tilt forward 0x38 (0x39), backward 0x3A (0x3B), left 0x3C (0x3D), right 0x2E (0x3F);
  LOAD 1-4 0x46-0x49 (+SHIFT 0x58, 0x59, 0x60, 0x61).
- Deck: list says MASTER TEMPO 0x1A (+SHIFT 0x60), BEAT SYNC 0x58 (0x5C), KEY SYNC 0x65 (0x64).
  Owner's unit: MASTER TEMPO = 0x65, KEY SYNC = 0x1A unshifted; SHIFT rows (0x60 range, 0x64 key reset) match the list.
