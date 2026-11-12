// DJCore: real-time 4 deck engine + 4 channel mixer.
// C API so Swift can call it without C++ interop.
// Threading:
//   - djc_render / djc_ioproc: audio thread only. Never allocates, locks or blocks.
//   - djc_send: one producer per queue index (0 = UI main thread, 1 = MIDI thread).
//   - everything else: UI thread.
#ifndef DJCORE_H
#define DJCORE_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct djc_engine djc_engine;
typedef struct djc_track djc_track;

enum { DJC_DECKS = 4, DJC_HOTCUES = 8 };

typedef enum {
    // deck transport
    DJC_LOAD = 1,          // ptr = djc_track* (ownership moves to engine)
    DJC_PLAY,              // toggle play/pause (CDJ PLAY/PAUSE)
    DJC_CUE,               // i = 1 down, 0 up (CDJ CUE)
    DJC_SEEK,              // v = position in seconds
    DJC_TEMPO,             // v = fader -1..+1 (up = slower, CDJ style handled by caller)
    DJC_TEMPO_RANGE,       // v = range fraction (0.06, 0.10, 0.16, 1.0); v < 0 cycles
    DJC_KEYLOCK,           // i = 0/1, 2 = toggle
    DJC_SYNC,              // i = 0 off, 1 on, 2 toggle
    DJC_MAKE_MASTER,
    DJC_QUANTIZE,          // i = 0/1, 2 = toggle
    DJC_SET_GRID,          // v = bpm, i ignored, ptr ignored; v2 = first beat seconds
    // jog
    DJC_JOG_TOUCH,         // i = 0/1 (platter touch in vinyl mode)
    DJC_JOG_TURN,          // i = signed ticks
    DJC_JOG_SEARCH,        // i = signed ticks (SHIFT + jog)
    DJC_VINYL_MODE,        // i = 0/1, 2 = toggle
    DJC_JOG_TICKS_PER_REV, // v = ticks per revolution (default 720)
    DJC_JOG_NUDGE_GAIN,    // v = rate offset per jog revolution per second (default 0.8)
    DJC_KEY_SHIFT,         // v = semitones (-12..12); i = 101: v is relative; i = 100: reset
    DJC_KEY_SYNC,          // i = 0/1, 2 = toggle (matching needs track keys, Phase 3)
    // cues and loops
    DJC_HOTCUE,            // i = index 0..7, v = 1 down / 0 up
    DJC_HOTCUE_DELETE,     // i = index
    DJC_LOOP_IN,
    DJC_LOOP_OUT,
    DJC_LOOP_EXIT,         // exit or reloop toggle
    DJC_BEAT_LOOP,         // v = beats (0.25 .. 32)
    DJC_LOOP_HALVE,
    DJC_LOOP_DOUBLE,
    DJC_BEAT_JUMP,         // v = beats (signed)
    DJC_CUE_CALL,          // i = -1 previous / +1 next cue point (main cue + hot cues)
    DJC_SET_CUE,           // v = seconds (rekordbox memory cue on load)
    DJC_SET_HOTCUE,        // i = 0..7, v = seconds, v2 = loop end seconds (0 = plain hot cue)
    DJC_SET_MEMCUE,        // i = slot 0..31 (-1 clears all), v = seconds (used by CUE/LOOP CALL)
    // mixer channel (deck index = channel index)
    DJC_TRIM = 100,        // v 0..1 (0.5 = 0 dB)
    DJC_EQ_HI,             // v 0..1 (0 = kill, 0.5 = 0 dB, 1 = +6 dB)
    DJC_EQ_MID,
    DJC_EQ_LOW,
    DJC_COLOR,             // v 0..1 (0.5 = off, <0.5 LPF, >0.5 HPF)
    DJC_FADER,             // v 0..1
    DJC_XF_ASSIGN,         // i = 0 A, 1 THRU, 2 B
    DJC_CH_CUE,            // i = 0/1, 2 = toggle
    // master section (deck ignored)
    DJC_CROSSFADER = 200,  // v 0..1
    DJC_MASTER_LEVEL,      // v 0..1
    DJC_MASTER_EQ,         // i = band 0 low (100 Hz shelf) / 1 mid (1 kHz) / 2 high (10 kHz shelf), v = dB (-12 .. +6)
    DJC_PHONES_MIX,        // v 0 = cue only, 1 = master only
    DJC_PHONES_LEVEL,      // v 0..1
    DJC_MASTER_CUE,        // i = 0/1, 2 = toggle
    DJC_ROUTING,           // i = masterL | masterR<<8 | phonesL<<16 | phonesR<<24 (0xFF = none)
    DJC_SPLIT_CUE,         // i = 0/1: master mono on masterL, cue mono on masterR
    DJC_COLOR_FX_ON,       // i = 0/1, 2 = toggle (colour filter bypass)
    // Beat FX
    DJC_FX_TYPE,           // i = 0..13 (see djc_fx_name)
    DJC_FX_TARGET,         // i = 0-3 channel, 4 master, 5 sampler
    DJC_FX_ON,             // i = 0/1, 2 = toggle
    DJC_FX_LEVEL,          // v = 0..1 LEVEL/DEPTH
    DJC_FX_BEAT,           // i = -1 / +1 step through 1/4 .. 32 beats
    DJC_COLOR_FX_TYPE,     // i = 0 FILTER, 1 DUB ECHO, 2 SWEEP, 3 SPACE; -1 = next
    DJC_COLOR_FX_PARAM,    // v = 0..1 colour FX PARAMETER (intensity)
    DJC_FX_BAND,           // i = 0 LOW, 1 MID, 2 HI: toggle that band in the Beat FX frequency select
    // GROOVE CIRCUIT drum roller (deck = side: 0 left, 1 right)
    DJC_DRUM_PAD,          // i = 0 KICK 1 SNARE 2 CLAP 3 HAT/CRASH, v = 1 press: select + play / deselect
    DJC_DRUM_ROLL,         // i = 0..3 roll rate (1, 1/2, 1/4, 1/8 beat), v = 1 press / 0 release
    DJC_DRUM_HOLD,         // i = 1 lever down (hold the roll), 0 lever up: stops both sides' drums and turns Beat FX off
    DJC_DRUM_RELEASE,      // hold off
    DJC_DRUM_GAIN,         // v = 0..1 GAIN knob: deck 0 (left) = gain of both kits, deck 1 (right) = pitch of both
    DJC_DRUM_CUE,          // CAPTURE/LENGTH: toggle headphone CUE of that side's drums
    DJC_DUB_BEATS,         // v = DUB ECHO delay time in beats (0.25 .. 2), default 0.75
    DJC_NUDGE_SNAP         // i = 1: after a jog nudge, glide onto the nearest beat of the deck it is mixed with
} djc_cmd_type;

typedef struct {
    int32_t type;
    int32_t deck;
    int32_t i;
    double v;
    double v2;
    void *ptr;
} djc_cmd;

typedef struct {
    int32_t loaded, playing, keylock, sync, master, quantize, loopActive, vinyl;
    double position;      // seconds (what is heard now)
    double duration;      // seconds
    double trackBpm;      // analysed bpm
    double bpm;           // effective bpm
    double rate;          // 1.0 = original speed
    double tempoRange;
    double cuePoint;      // seconds
    double loopIn, loopOut; // seconds, -1 none
    double hotcues[DJC_HOTCUES]; // seconds, -1 empty
    double tempo;         // tempo fader -1..1
    int32_t keyShift, keySync;
    double stamp;         // djc_now() when this state was published
    double hotLoopEnd[DJC_HOTCUES]; // seconds, 0 if the hot cue is not a loop
    float peakL, peakR;   // channel meter, pre-fader, 0..1+
    float onAir;          // channel fader x crossfader gain while playing (0 = not heard on the master)
} djc_deck_state;

typedef struct {
    float trim[DJC_DECKS], hi[DJC_DECKS], mid[DJC_DECKS], low[DJC_DECKS], color[DJC_DECKS], fader[DJC_DECKS];
    int32_t xf[DJC_DECKS], cue[DJC_DECKS];
    float crossfader, masterLevel, phonesMix, phonesLevel;
    int32_t masterCue, colorFxOn;
    int32_t colorFxType;  // 0 FILTER, 1 DUB ECHO, 2 SWEEP, 3 SPACE
    float colorFxParam;   // 0..1
} djc_mixer_state;

typedef struct {
    float masterPeakL, masterPeakR;
    int32_t xruns;       // render calls longer than the buffer budget (approx)
    double cpuLoad;      // last render time / buffer time
} djc_master_state;

typedef struct {
    int32_t type, target, on, beatIndex, beatCode;
    float level;
    double bpm, beats, timeMs;
    int32_t bands;        // Beat FX frequency select: bit0 LOW, bit1 MID, bit2 HI
} djc_fx_state;

// lifecycle (UI thread)
djc_engine *djc_create(double sampleRate, int32_t maxFramesPerRender);
void djc_destroy(djc_engine *e);
double djc_sample_rate(const djc_engine *e);

// tracks (UI thread). Copies the samples. Samples must already be at the engine sample rate.
djc_track *djc_track_create(const float *left, const float *right, int64_t frames, double sampleRate,
                            double bpm, double firstBeatSeconds);
void djc_track_destroy(djc_track *t);
// rekordbox beat list (seconds, one entry per beat). Call before DJC_LOAD. Drum rolls then follow every beat
// of the real grid instead of a constant-BPM grid.
void djc_track_set_beats(djc_track *t, const double *seconds, int32_t count);

// commands. queue 0 = UI thread, 1 = MIDI thread. Returns 0 if the queue was full.
int32_t djc_send(djc_engine *e, int32_t queue, const djc_cmd *cmd);
// convenience: build and send
int32_t djc_post(djc_engine *e, int32_t queue, int32_t type, int32_t deck, int32_t i, double v);

// frees tracks the engine has released (UI thread, call ~10x per second)
void djc_collect(djc_engine *e);

// state snapshots (any thread, lock-free, values may be one block stale)
void djc_get_deck_state(const djc_engine *e, int32_t deck, djc_deck_state *out);
void djc_get_master_state(const djc_engine *e, djc_master_state *out);
void djc_get_mixer_state(const djc_engine *e, djc_mixer_state *out);
void djc_get_fx_state(const djc_engine *e, djc_fx_state *out);
const char *djc_fx_name(int32_t type);
// drum roller state of a side: bit0 headphone CUE on, bits1-4 drums selected, bits 8-11 roll rate running,
// bit12 HOLD, bits 16-20 pitch + 12 (semitones), bits 21-27 gain %
int32_t djc_drum_state(const djc_engine *e, int32_t side);
// Replace a drum sound (call before audio starts). slot 0 KICK 1 SNARE 2 CLAP 3 HAT/CRASH; R may be null (mono).
void djc_drum_set_sample(djc_engine *e, int32_t side, int32_t slot, const float *L, const float *R, int32_t frames);

// ---- DDJ-GRV6 MIDI mapping (midi.cpp). Bytes per the official AlphaTheta list, see docs/sources.md ----
typedef struct djc_midi djc_midi;
enum {
    DJC_EV_BROWSE_TURN = 1, // value = signed steps
    DJC_EV_BROWSE_PRESS,
    DJC_EV_LOAD,            // deck = 0..3
    DJC_EV_PREVIEW,
    DJC_EV_UNMAPPED,        // raw = the message, for verification
    DJC_EV_DECK_LAYER,      // deck = 0..3, value = 1 when the GRV6 switched that deck on
    DJC_EV_BROWSE_BACK,     // BACK or SHIFT + BROWSE press: value 1 = up one level, 2 = SHIFT+BACK = top level
    DJC_EV_VIEW,            // VIEW button: toggle browser / waveform screen
    DJC_EV_TILT,            // BROWSE knob tilt: value 1 forward, 2 backward, 3 left, 4 right
    DJC_EV_KEY_SYNC,        // KEY SYNC pressed on deck: the app decides which deck to retune (the queued one)
    DJC_EV_SOURCE           // DISCOVER: open / close the SOURCE screen
};
typedef struct {
    int32_t kind, deck, value;
    uint8_t raw[3];
} djc_ui_event;
djc_midi *djc_midi_create(djc_engine *e);
void djc_midi_destroy(djc_midi *m);
// MIDI thread: one complete 2 or 3 byte channel message
void djc_midi_in(djc_midi *m, const uint8_t *msg, int32_t len);
// UI thread: fills buf with 3-byte LED/meter messages that changed since the last call. Returns byte count.
int32_t djc_midi_out(djc_midi *m, uint8_t *buf, int32_t cap);
// UI thread: forget LED cache so everything is resent (call after (re)connecting)
void djc_midi_reset_leds(djc_midi *m);
// current pad mode of a deck: 0 HOT CUE, 1 STEMS, 2 BEAT JUMP, 3 SAMPLER, 4 KEYBOARD, 5 PAD FX, 6 BEAT LOOP, 7 KEY SHIFT
int32_t djc_midi_pad_mode(const djc_midi *m, int32_t deck);
// UI thread: controller events for the UI (browser, load). Returns 0 when empty.
int32_t djc_midi_poll_event(djc_midi *m, djc_ui_event *ev);
// test/diagnostic: number of entries in the mapping table and whether each has a source
int32_t djc_midi_table_size(void);
const char *djc_midi_table_source(int32_t index);

// audio thread: render `frames` frames into `channels` non-interleaved buffers
void djc_render(djc_engine *e, float *const *outputs, int32_t channels, int32_t frames);

// CoreAudio AURenderCallback-compatible entry (refCon = djc_engine*). Declared with void* so the
// header stays free of Apple headers; cast is done in the .cpp.
int32_t djc_ioproc(void *refCon, void *ioActionFlags, const void *inTimeStamp, uint32_t inBusNumber,
                   uint32_t inNumberFrames, void *ioData);

// iOS output: creates and starts a RemoteIO unit rendering `channels` channels from the engine.
// Configure and activate AVAudioSession first. Returns an opaque handle, or NULL with *status set.
void *djc_io_start(djc_engine *e, int32_t channels, int32_t *status);
void djc_io_stop(void *unit);

// ---- set recorder: the master output (after master level + limiter) goes into a lock-free ring buffer.
// The app drains it from a background thread and writes the file. Ring holds 10 s.
void djc_rec_start(djc_engine *e);   // UI thread: clears the ring, then starts capturing
void djc_rec_stop(djc_engine *e);    // UI thread: stops capturing (drain the rest afterwards)
// writer thread: copies up to maxFrames interleaved stereo frames into dst, returns frames copied
int32_t djc_rec_read(djc_engine *e, float *dst, int32_t maxFrames);
int64_t djc_rec_dropped(const djc_engine *e); // frames lost because the writer fell 10 s behind

// clock used for djc_deck_state.stamp (seconds, monotonic); use it to extrapolate positions smoothly
double djc_now(void);

// offline helpers (UI / background thread)
// Estimates bpm (70..180) and first beat (seconds) from a mono signal.
// lag (s, + = audio later than grid) between a rekordbox beat list and the kick onsets in the decoded audio,
// and a confidence (peak / median score). Offline, any thread.
void djc_grid_offset(const float *mono, int64_t frames, double sr, const double *beats, int32_t count,
                     double *lagOut, double *confidenceOut);
void djc_estimate_bpm(const float *mono, int64_t frames, double sampleRate, double *bpmOut, double *firstBeatOut);
// Builds an overview: `bins` peak values (0..1) per band (low, mid, high) for waveform drawing.
void djc_overview(const float *mono, int64_t frames, double sampleRate, int32_t bins, float *low, float *mid,
                  float *high);

// ---- rekordbox Device Library (export.pdb) + ANLZ analysis (rblib.cpp). UI/background thread only. ----
typedef struct djc_library djc_library;
typedef struct {
    uint32_t id, sampleRate, fileSize, bitrate, trackNumber, colorId, rating, year, duration; // duration in s
    double bpm;
    const char *title, *artist, *album, *genre, *key, *label, *mixName, *comment, *dateAdded;
    const char *filePath;    // relative to the stick root, e.g. "/Contents/Artist/Album/x.mp3"
    const char *analyzePath; // "/PIONEER/USBANLZ/P016/0000875E/ANLZ0000.DAT"
    const char *artworkPath;
} djc_lib_track;
typedef struct {
    uint32_t id, parentId, sortOrder, isFolder;
    const char *name;
} djc_lib_playlist;

djc_library *djc_lib_open_pdb(const char *path);
void djc_lib_close(djc_library *lib);
const char *djc_lib_error(const djc_library *lib); // "" when fine
int32_t djc_lib_track_count(const djc_library *lib);
const djc_lib_track *djc_lib_track_at(const djc_library *lib, int32_t index);
int32_t djc_lib_track_index(const djc_library *lib, uint32_t trackId); // -1 if missing
int32_t djc_lib_playlist_count(const djc_library *lib);
const djc_lib_playlist *djc_lib_playlist_at(const djc_library *lib, int32_t index);
// track ids in playlist order. Returns the total count (may exceed cap).
int32_t djc_lib_playlist_tracks(const djc_library *lib, uint32_t playlistId, uint32_t *out, int32_t cap);

typedef struct djc_anlz djc_anlz;
typedef struct { uint16_t beatNumber, tempo100; uint32_t timeMs; } djc_beat;
typedef struct {
    int32_t hotCue;   // 0 = memory cue, 1..8 = hot cue A..H
    int32_t type;     // 1 = cue, 2 = loop
    uint32_t timeMs, loopMs;
    int32_t colorId, hasRgb;
    uint8_t r, g, b;
    int32_t active;   // 1 = rekordbox "active loop" (PCPT status 4): engages when playback reaches it
} djc_cue;
typedef struct { uint16_t index, beat, kind, beatFill; uint8_t fill; } djc_phrase;

// Opens the analysis for a track: pass the full path of the .DAT; .EXT and .2EX next to it are read too.
djc_anlz *djc_anlz_open(const char *datPath);
void djc_anlz_close(djc_anlz *a);
int32_t djc_anlz_beat_count(const djc_anlz *a);
const djc_beat *djc_anlz_beats(const djc_anlz *a);
int32_t djc_anlz_cue_count(const djc_anlz *a);
const djc_cue *djc_anlz_cue_at(const djc_anlz *a, int32_t i);
const char *djc_anlz_cue_comment(const djc_anlz *a, int32_t i);
int32_t djc_anlz_wave_count(const djc_anlz *a);   // entries, 150 per second
int32_t djc_anlz_wave_source(const djc_anlz *a);  // 7 PWV7, 5 PWV5, 3 PWV3, 0 none
const float *djc_anlz_wave_band(const djc_anlz *a, int32_t band); // 0 low, 1 mid, 2 high; 0..1
// Raw waveform per rekordbox kind (7 = PWV7 3-band, 5 = PWV5 RGB, 3 = PWV3 blue), 150 entries/s.
// ch 0 = height 0..1; ch 1..3 = 3-band low/mid/high, RGB red/green/blue (0..1, 3-bit), blue whiteness (ch 1).
int32_t djc_anlz_wave_len(const djc_anlz *a, int32_t kind);
const float *djc_anlz_wave_data(const djc_anlz *a, int32_t kind, int32_t ch);
int32_t djc_anlz_phrase_count(const djc_anlz *a);
const djc_phrase *djc_anlz_phrases(const djc_anlz *a);
int32_t djc_anlz_mood(const djc_anlz *a);         // 1 high, 2 mid, 3 low
int32_t djc_anlz_end_beat(const djc_anlz *a);

#ifdef __cplusplus
}
#endif
#endif
