# GRV6 DJ engine notes (Phase 1)

Open `GRV6DJ.swiftpm` in Xcode, pick the iPad, press Run. Set your own `teamIdentifier` and `bundleIdentifier` in `Package.swift` first (see the top-level README).

This file is the Phase 1 engine notes. The top-level `README.md` describes the current app.

## Layout
- `GRV6DJ.swiftpm/Sources/DJCore`: C++17 real-time core, C API in `include/djcore.h`
  - `engine.cpp`: 4 decks (varispeed, keylock via Signalsmith Stretch 1.3.2, cues, hot cues, loops, beat jump,
    sync with phase lock, jog nudge/scratch/search), 4 channel mixer (trim, 3-band isolator 250 Hz / 3 kHz with
    kill, colour filter LPF/HPF, fader, crossfader A/THRU/B, cue bus, master, phones cue/master mix), routing
  - `ioproc.cpp`: RemoteIO output, render callback in C++ (no Swift on the audio thread)
  - `analysis.cpp`: stand-in BPM/first-beat estimate and 3-band overview (Phase 3 uses rekordbox grids)
  - `vendor/`: Signalsmith Stretch + Linear (MIT), unmodified apart from removed unused backends
- `GRV6DJ.swiftpm/Sources/App`: SwiftUI test UI
- `tests/`: host tests, `./tests/run.sh` (macOS or Linux)

## Real-time rules (enforced by tests)
- UI and MIDI threads talk to the audio thread through two lock-free SPSC queues.
- Tracks are allocated on a background thread; replaced tracks go back through a garbage queue and are freed
  on the UI thread.
- `core_tests.cpp` overrides `operator new/delete` and fails if anything allocates inside `djc_render`.
- Engine runs at 48 kHz fixed; RemoteIO converts to the hardware rate.

## Host test results (2026-10-21, x86 Linux, portable FFT)
| Check | Result |
| --- | --- |
| varispeed +10% on 440 Hz | 484.0 Hz |
| keylock +10% / -10% | 440.0 Hz / 440.0 Hz |
| position at rate | 4.400 s after 4 s at +10% |
| allocations in render | 0 |
| 4-beat loop quantised | 10.000-12.000 s, holds |
| CUE (CDJ behaviour) | return+pause, set+preview, release returns |
| sync 126 -> 120 BPM | 120.00 BPM, phase error 0.0000 beats |
| low kill at 60 Hz | -49.6 dB |
| crossfader / cue bus | cut OK / cue audible with fader down |
| BPM estimate (synthetic 124) | 124.00, first beat exact |
| 4 decks keylocked | 43% of one x86 core (iOS build uses Accelerate FFT) |

## Phase 1 known limits
- BPM/grid is estimated. Tap the BPM on a deck to correct it. Real grids come in Phase 3.
- Hot cue jumps with keylock re-prime the stretcher (tiny gap possible).
- Keylock latency compensation assumes the tempo at the last jump; drift is under ~3 ms for a ±6% move.
- Tracks are fully decoded to RAM (about 23 MB per minute, stereo float).
