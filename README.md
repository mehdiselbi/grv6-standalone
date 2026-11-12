# GRV6 Standalone

An iPad app that turns a **Pioneer DJ / AlphaTheta DDJ-GRV6** controller and an **iPad mini 6** into a standalone, laptop-free DJ setup with a screen laid out like an all-in-one player. Plug in a rekordbox USB stick, plug in the controller, and play.

It is a personal project, built for one setup and shared as source code. There is no App Store version: you build it yourself with Xcode (instructions below).

> **Not affiliated with AlphaTheta or Pioneer DJ.** "DDJ-GRV6", "XDJ-AZ", "rekordbox", "DJM-A9" and "DJM-V10" are trademarks of their owners and are named here only to describe compatibility and the design references. This project contains no AlphaTheta firmware, code, fonts, icons, logos or other assets. Every graphic is drawn by the app itself, and the effects are our own implementations written from public descriptions of how they sound and behave.

---

## Contents

- [Main features](#main-features)
- [What you need](#what-you-need)
- [Install without AI](#install-without-ai)
- [Install with AI](#install-with-ai)
- [Preparing your music](#preparing-your-music)
- [Controller mapping highlights](#controller-mapping-highlights)
- [Fork it and make it yours](#fork-it-and-make-it-yours)
- [Project layout](#project-layout)
- [Running the tests](#running-the-tests)
- [Known limits](#known-limits)
- [Licences and credits](#licences-and-credits)

---

## Main features

**Decks and playback**
- 4 decks with the CDJ-style CUE button, PLAY/PAUSE, 8 hot cues, memory cues and CUE/LOOP CALL.
- Quantize that follows the rekordbox beat grid. Hot cues are quantized CDJ-style: they jump on the next beat and keep the phase.
- Beat sync with phase lock, tempo ranges ±6 / 10 / 16 % / WIDE, and BPM shown and rounded to 0.1.
- MASTER TEMPO (key lock) using Signalsmith Stretch. KEY SHIFT and a **KEY SYNC** that retunes the deck you are cueing to a harmonically compatible key (Camelot ±1 or the relative key, at most ±3 semitones). It never retunes the track the crowd is hearing alone.
- Loops: IN/OUT, reloop/exit, beat loops 1/4 to 32 beats, halve/double, and beat jump.
- Jog wheels: vinyl mode scratch, nudge, and SHIFT + jog search.
- Auto cue to the first sound or first beat. Optional "load at memory cue".
- Automatic grid offset check: if the rekordbox grid sits a few milliseconds off the kicks, the app shifts it at load time (it never changes your stick).

**Mixer and effects**
- 4 channels with trim, 3-band isolator EQ with kill, and the SOUND COLOR FX knob. Crossfader with A / THRU / B assign.
- SOUND COLOR FX: FILTER, **DUB ECHO**, SWEEP and SPACE. The DUB ECHO is modelled on the behaviour of the DJM-A9 / DJM-V10 effect: the COLOR knob sets the send, the band and the echo time, PARAMETER sets the feedback, and the tail keeps ringing after you centre the knob.
- BEAT FX: DELAY, ECHO, LOW CUT ECHO, SPIRAL, HELIX, REVERB, FLANGER, PHASER, FILTER, TRANS, PITCH, ROLL, MOBIUS SAW and MOBIUS TRI. Beat-synced, with LOW/MID/HI band select and a channel or master target.
- Post-fader effects, so echo and reverb tails keep ringing when you pull the fader down. The ON/OFF button or the release lever cuts them.
- Master EQ (LOW / MID / HI) and a level meter in the top bar.
- Master safety chain: headroom, a lookahead limiter at -0.3 dBFS and a soft clip, so two kicks on top of each other do not distort.
- Headphones: channel CUE and MASTER CUE. You only hear the master in the phones when MASTER CUE is on, and a channel already heard through MASTER CUE is never added twice.

**Groove Circuit (drums)**
- Built-in 909 and 707 style drum kits on the GRV6 drum pads, locked to the grid of the deck you are hearing.
- Rolls at 1, 1/2, 1/4 and 1/8 beat, hold with the lever, and a release lever that also kills Beat FX and colour FX tails.
- CAPTURE/LENGTH sends that side's drums to the headphone cue so you can pre-listen.

**Library and screens**
- Reads rekordbox **Device Library** USB exports (export.pdb + ANLZ files): beat grids, cues, hot cues, loops, phrases, keys and waveforms. It only reads. It never writes to your stick.
- SOURCE screen like an all-in-one player: the iPad's internal library plus up to 8 remembered USB sticks, detected automatically when plugged in, with song and playlist counts and free space.
- Copy a stick to the iPad (only changed files are copied), so you can play with no stick at all.
- Browser by artist, album, track, key, playlist and folder, with sorting. Tracks you played this session turn green.
- Metal waveforms with beat ticks, red downbeats, the loop highlight and a phrase bar.
- Warnings like the hardware: Load Lock (refuses to load onto a deck that is playing on air), cannot play track, unsupported format, not analysed by rekordbox, clip.

**Recording**
- REC button records the master output to WAV 24-bit or AAC 320 kbps, with a timer, a confirm-to-stop and a storage check. Recordings are listed in Settings, where you can share or delete them.

**Engineering**
- C++17 real-time core. The audio thread never allocates or locks, and the tests fail if it does.
- Lock-free queues between the UI, MIDI and audio threads.
- MIDI values come from the official DDJ-GRV6 MIDI message list. The LEDs follow the app state (PLAY/CUE blink when paused, loop buttons blink in a loop, the colour FX button blinks while on).

---

## What you need

| Item | Notes |
| --- | --- |
| iPad | Built and tested on an **iPad mini 6** (iPadOS 17 or later, landscape). Other iPads with USB-C should work but the layout is tuned for the mini. |
| Controller | **DDJ-GRV6** (tested on firmware 1.02). Other controllers are not mapped. |
| USB-C hub | One with **USB-C Power Delivery pass-through** and at least two USB-A/USB-C data ports: one for the GRV6, one for a USB stick, and power in for the iPad. |
| Power | The GRV6 must be powered from its own adapter when used with a tablet. |
| Mac | A Mac with **Xcode 16 or later** to build and install the app. Swift Playgrounds cannot build it because the audio core is C++. |
| Apple ID | A free Apple ID works (the app then needs re-installing every 7 days). A paid Apple Developer account gives you a one-year install. |
| Music | A USB stick exported from rekordbox in the **Device Library** format, or files copied to the iPad with the app. |

---

## Install without AI

### 1. Get the code

```bash
git clone https://github.com/mehdiselbi/grv6-standalone.git
cd grv6-standalone
```

Or press **Code → Download ZIP** on GitHub and unzip it.

### 2. Set your own signing team

The project is a Swift package app (`App/GRV6DJ.swiftpm`). Its signing settings live in `App/GRV6DJ.swiftpm/Package.swift`:

```swift
bundleIdentifier: "local.mehdi.GRV6DJ",
teamIdentifier: "B5JLGJYA59",
```

Change both:
- `teamIdentifier`: your own Team ID. In Xcode open **Settings → Accounts**, add your Apple ID, and select your team. The 10-character Team ID is also shown at [developer.apple.com/account](https://developer.apple.com/account) under Membership.
- `bundleIdentifier`: anything unique to you, for example `com.yourname.GRV6DJ`.

You can also set the team in Xcode under the app target's **Signing & Capabilities** tab.

### 3. Prepare the iPad

1. Connect the iPad to the Mac with a cable and tap **Trust** on the iPad.
2. Turn on **Developer Mode**: **Settings → Privacy & Security → Developer Mode**. The iPad restarts and asks you to confirm.

### 4. Build and run

1. Double-click `App/GRV6DJ.swiftpm` (or **File → Open** in Xcode).
2. Wait for Xcode to finish indexing, pick your iPad as the run destination in the toolbar, and press **Run** (⌘R).
3. The first time, iPadOS blocks the app from an untrusted developer. On the iPad open **Settings → General → VPN & Device Management**, tap your Apple ID under Developer App and tap **Trust**. Then press Run again.

### 5. Connect the hardware

1. Plug the GRV6 power adapter in and switch it on.
2. Plug the hub into the iPad, then the GRV6 and your USB stick into the hub, and the iPad charger into the hub's PD port.
3. Open GRV6 DJ. The controller is found automatically, and its LEDs light up.
4. Press **DISCOVER** on the GRV6 to open the SOURCE screen. The first time you use a new stick, tap it in the list and pick its top folder in the Files picker once (an iPadOS rule). After that it is detected automatically each time you plug it in.

---

## Install with AI

The repository is set up to be worked on with an AI coding assistant. `CLAUDE.md` at the top level tells the assistant what the project is, how it is built and the rules it must follow (no guessed MIDI bytes or file formats, no allocations on the audio thread, no AlphaTheta assets, never write to the USB stick).

### With Claude Code

1. Install Claude Code by following [the official docs](https://docs.claude.com/en/docs/claude-code/overview).
2. Clone the repo and start it in the repo folder:

   ```bash
   git clone https://github.com/mehdiselbi/grv6-standalone.git
   cd grv6-standalone
   claude
   ```

3. Ask it to set you up, for example:

   > Read CLAUDE.md and README.md. Set my Team ID to XXXXXXXXXX and my bundle ID to com.myname.GRV6DJ in Package.swift, run the host tests, then tell me the steps to install on my iPad.

4. Building to the iPad still happens in Xcode on your Mac (step 4 above). The assistant can run the C++ tests, explain the code, and make the changes you ask for.

### With Claude on the web or the desktop app

Connect your GitHub account, add your fork of this repository to the conversation, and ask the same kind of question. It can read the code, explain it and propose changes. If it is linked to your Mac, it can also edit the files you open in Xcode.

### Good first prompts

- "Explain how a MIDI message from the jog wheel reaches the audio engine."
- "Add a new Beat FX called X. Write a test for it in App/tests/core_tests.cpp first."
- "Map my controller: here is its official MIDI message list (attach the PDF)."

---

## Preparing your music

1. In rekordbox, analyse your tracks (beat grid, key, waveforms, phrases).
2. Export a playlist or your collection to a USB stick in the **Device Library** format. If your rekordbox version offers both Device Library and OneLibrary, keep Device Library on (this app does not read OneLibrary yet).
3. Plug the stick into the hub and open the SOURCE screen (DISCOVER).
4. Optional: on the SOURCE screen, select the stick and press **COPY TO IPAD**. The copy appears as the iPad's internal library, so you can play without the stick. **UPDATE IPAD COPY** copies only what changed.

Supported audio files are the ones iPadOS can decode, such as MP3, AAC/M4A, WAV and AIFF. Anything else shows UNSUPPORTED FILE FORMAT.

---

## Controller mapping highlights

The full table is in `App/GRV6DJ.swiftpm/Sources/DJCore/midi.cpp`. Every row cites the official DDJ-GRV6 MIDI message list. Notable choices:

| Control | Action in the app |
| --- | --- |
| DISCOVER | Opens or closes the SOURCE screen |
| SHIFT + DISCOVER | Browser |
| VIEW | Switches between the browser and the waveform screen |
| BROWSE knob turn / press / BACK | Moves the highlight / opens or loads / goes back |
| KEY SYNC | Retunes the deck you are cueing to a compatible key (max ±3 semitones) |
| MASTER TEMPO | Key lock on/off |
| Pad modes | HOT CUE, BEAT JUMP, BEAT LOOP and KEY SHIFT are implemented |
| Groove Circuit pads | Kick, snare, clap, hat/crash. GAIN knob left = drum level, right = drum pitch |
| Groove Circuit lever | Down holds the roll. Release stops the drums and kills Beat FX and colour FX tails |
| CAPTURE / LENGTH | Headphone cue for that side's drums |
| Beat FX ON/OFF | Turning it off cuts the tail |
| SOUND COLOR FX ON/OFF | Turning it off cuts the DUB ECHO / SPACE tail |

---

## Fork it and make it yours

1. On GitHub, open [mehdiselbi/grv6-standalone](https://github.com/mehdiselbi/grv6-standalone) and press **Fork** (top right). This makes your own copy under your account.
2. Clone **your** fork:

   ```bash
   git clone https://github.com/YOUR-USERNAME/grv6-standalone.git
   cd grv6-standalone
   git remote add upstream https://github.com/mehdiselbi/grv6-standalone.git
   ```

3. Set your own `teamIdentifier` and `bundleIdentifier` (see [step 2](#2-set-your-own-signing-team)) and commit that change in your fork.
4. Work on a branch:

   ```bash
   git checkout -b my-feature
   # edit, then run the tests
   bash App/tests/run.sh
   git commit -am "Describe your change"
   git push -u origin my-feature
   ```

5. To get later changes from the original project:

   ```bash
   git fetch upstream
   git merge upstream/main
   ```

6. To suggest a change back, open a **Pull Request** from your branch on GitHub. Please keep the project rules from `CLAUDE.md`: cite a public source for every MIDI value and file-format field, keep the audio thread free of allocations and locks, add a test, and do not add any AlphaTheta asset.

**Ideas for forks:** a mapping for another controller, OneLibrary support, stems, a different screen theme, your own drum kits (replace the WAV files in `App/GRV6DJ.swiftpm/Sources/App/Samples/`).

---

## Project layout

```
grv6-standalone/
├── CLAUDE.md                     Project brief and rules for AI assistants (and humans)
├── App/
│   ├── GRV6DJ.swiftpm/           The app: open this in Xcode
│   │   ├── Package.swift         App settings: name, bundle ID, team, iOS version
│   │   └── Sources/
│   │       ├── DJCore/           C++17 real-time core with a C API (include/djcore.h)
│   │       │   ├── engine.cpp    Decks, mixer, sync, cues, loops, limiter, recorder ring
│   │       │   ├── fx.hpp        Beat FX
│   │       │   ├── cfx.hpp       Sound Color FX (FILTER, DUB ECHO, SWEEP, SPACE)
│   │       │   ├── drums.hpp     Groove Circuit drum engine
│   │       │   ├── dsp.hpp       Filters and biquads
│   │       │   ├── midi.cpp      DDJ-GRV6 mapping and LED output
│   │       │   ├── rblib.cpp     rekordbox export.pdb / ANLZ reader
│   │       │   ├── analysis.cpp  Waveform overview, grid offset check
│   │       │   ├── ioproc.cpp    CoreAudio output
│   │       │   └── vendor/       Signalsmith Stretch (MIT)
│   │       └── App/              SwiftUI screens, Metal waveforms, USB sources, recorder, drum samples
│   └── tests/                    Host tests for the C++ core (Linux or macOS)
├── Spikes/                       Phase 0 test app: MIDI logger, audio route probe, stick probe
├── tools/inspect_stick.py        Prints a report of a rekordbox USB stick's structure
└── docs/                         Public sources used for MIDI values and file formats
```

---

## Running the tests

The C++ core has host tests that run on a Mac or Linux with a C++17 compiler:

```bash
bash App/tests/run.sh
```

They check the audio engine (sync, key lock, cues, loops, quantize, effects, limiter, headphone routing, recorder), the MIDI mapping, the rekordbox reader against public test exports, and that nothing allocates on the audio thread. The run ends with `ALL PASSED (0 failures)`.

---

## Known limits

- Only the DDJ-GRV6 is mapped.
- rekordbox **OneLibrary** exports are detected but not read yet. Use Device Library.
- Pad modes STEMS, SAMPLER, KEYBOARD and PAD FX are not implemented.
- A new USB stick needs one pick in the Files picker before it is detected automatically (iPadOS does not let apps read sticks without it).
- With a free Apple ID the app stops opening after 7 days. Run it again from Xcode to renew.

---

## Licences and credits

- **This project's code:** MIT licence, see [LICENSE](LICENSE).
- **Signalsmith Stretch and Signalsmith Linear** (time-stretch and FFT) by Geraint Luff / Signalsmith Audio, MIT licence. See `App/GRV6DJ.swiftpm/Sources/DJCore/vendor/`.
- **Test exports** in `App/tests/fixtures/rekordcrate/` are copied unmodified from [rekordcrate](https://github.com/Holzhaus/rekordcrate) under the Mozilla Public License 2.0 (licence file in that folder).
- **rekordbox file formats** were implemented from public documentation: [Deep Symmetry's export analysis](https://djl-analysis.deepsymmetry.org/rekordbox-export-analysis/exports.html), rekordcrate and pyrekordbox. See `docs/sources.md`.
- **MIDI values** come from AlphaTheta's public DDJ-GRV6 MIDI message list, cross-checked with [joergbeyer's Mixxx mapping](https://github.com/joergbeyer/dj-controller). See `docs/sources.md`.
- **Drum samples** in `App/GRV6DJ.swiftpm/Sources/App/Samples/` are from the owner's own sample packs. Check that their licence allows your use before you redistribute them in a fork, or replace them with your own.

See [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md) for the full list.
