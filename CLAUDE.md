# GRV6Standalone

Personal iPad app that turns a DDJ-GRV6 + iPad mini 6 into a standalone 4-deck player with an XDJ-AZ style screen. Owner: Mehdi. Shared as source on GitHub (MIT), never shipped as a binary. See README.md for features and install steps.

## Status (2026-11-11)
Phases 0-4 are done and running on the owner's iPad. Since then the app also has the Groove Circuit drums, Beat FX and Sound Color FX (incl. DUB ECHO), master EQ, limiter, set recorder, SOURCE screen with auto-detected sticks and an iPad library copy, KEY SYNC to the queued deck, and XDJ-AZ style warnings. The app lives in `App/GRV6DJ.swiftpm`; host tests: `bash App/tests/run.sh` (must end with ALL PASSED).

## For forks
Set your own `teamIdentifier` and `bundleIdentifier` in `App/GRV6DJ.swiftpm/Package.swift` before building.

## Hardware and firmware (checked 2026-10-20)
- DDJ-GRV6 firmware 1.02 (13 May 2025), the only published update.
- XDJ-AZ reference firmware 2.00 (6 Oct 2026). UI baseline = manual DRI1936C + 1.30 and 2.00 release notes.
- iPad mini 6, landscape 1133x744 pt. GRV6 must be powered from its adapter with mobile devices. Use a USB-C hub with PD pass-through.

## Hard rules
1. Swift + SwiftUI for UI, Metal for waveforms, C++ for the DSP core, CoreAudio + CoreMIDI. Time-stretch: Signalsmith Stretch (MIT).
2. The real-time audio thread never allocates, locks, logs, or calls Swift/ObjC runtime. Lock-free SPSC queues between UI and audio.
3. Never guess MIDI bytes. Every MIDI constant cites the official list (docs/sources.md) AND a row from our Phase 0a capture CSV. A constant without both fails a unit test.
4. Never guess file formats. Parsers come from public docs only: Deep Symmetry export analysis, rekordcrate (MPL-2.0, read for reference, do not copy code), pyrekordbox, rbox. Test against the owner's fixture sticks.
5. Never use, extract or request AlphaTheta firmware, fonts, icons, logos or binaries. All assets drawn by us. Neutral placeholder wordmark. Fonts: SF Pro, SF Mono.
6. Never write to a USB stick. Tag List, History, Played state live in app storage.
7. Out of scope: streaming, cloud, Playlist Bank, 4 DECK enlarged view (deferred), Bluetooth/Wi-Fi.
8. Work in phases. Stop at the end of each phase and wait for the owner's go-ahead.

## Phases
- 0 Spikes (done): `Spikes/` app + `tools/inspect_stick.py`. See `Spikes/README.md`.
  - 0a MIDI logger -> `fixtures/midi/grv6_mapping.csv`
  - 0b USB audio channel -> jack table -> `docs/phase0-results.md`
  - 0c GRV6 + stick on one hub, throughput and dropouts -> same file
  - 0d stick format reports -> `fixtures/sticks/*.txt`
- 1 Audio engine, 4 decks, sync, keylock, hot cues, loops, EQ, filter, crossfader, master + cue bus. Minimal test UI.
- 2 GRV6 mapping + LED output, driven by the Phase 0a CSV.
- 3 Library readers (Device Library export.pdb + ANLZ, OneLibrary exportLibrary.db) with unit tests on fixture sticks.
- 4 XDJ-AZ style UI from the approved spec.
- 5 Instruments pass: zero dropouts with 4 decks + keylock.

## Spec and decisions
- UI spec (approved items + open gaps): https://claude.ai/artifact/76sTz8eMwigyEXeivKe6xq
- Phrase bar: own palette keyed to rekordbox phrase types, fallback to 30 s ticks.
- Deck strip order on browse screens: 3 1 2 4. Deck card grid on waveform screen: 1 2 / 3 4.

## Build
- Main app: open `App/GRV6DJ.swiftpm` in Xcode 16+, run on an iPad in Developer Mode. The Phase 0 spike app is in `Spikes/`.
- Device: the owner's iPad mini 6 in Developer Mode.
