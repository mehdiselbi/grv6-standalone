// DDJ-GRV6 <-> DJCore mapping.
//
// Every byte below comes from AlphaTheta's "DDJ-GRV6 List of MIDI messages" (DDJ-GRV6_MIDI_Message_List_E1.pdf).
// Source tags in kMap:
//   "AT"      official list
//   "AT+MX"   official list and the public Mixxx mapping (github.com/joergbeyer/dj-controller) agree
//   "AT+VDJ"  official list; VirtualDJ GRV6 manual says the knob is hardware-only, so we ignore it
//   "AT+HW"   official list, button identity confirmed by pressing it on the owner's unit (2026-10-21)
//   "AT~"     official list, row order of the PDF text; the on-screen FX name lets the owner confirm it
// Messages not in kMap (e.g. browse tilt / BACK / VIEW, whose rows we could not attribute with certainty)
// are reported to the UI as DJC_EV_UNMAPPED so they can be verified on the hardware before being mapped.
//
// Threading: djc_midi_in runs on the CoreMIDI thread and posts to engine queue 1.
//            djc_midi_out / djc_midi_poll_event / djc_midi_reset_leds run on the UI thread.
#include "djcore.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>

namespace {

enum Scope : uint8_t { DECK, GLOBAL, FX }; // DECK = MIDI ch 1-4, FX = ch 5 (0x94/0xB4), GLOBAL = ch 7 (0x96/0xB6)
enum Kind : uint8_t { NOTE, CC };

enum Action : uint8_t {
    IGNORE = 0,
    // deck notes
    PLAY, CUE, CUE_TO_START, JOG_TOUCH, QUANTIZE, LOOP_IN, LOOP_4BEAT, LOOP_OUT, LOOP_HALVE, LOOP_DOUBLE, RELOOP,
    CALL_PREV, CALL_NEXT, JUMP_BACK, JUMP_FWD, KEYLOCK, RANGE_CYCLE, SYNC, MAKE_MASTER, CH_CUE,
    MODE_HOTCUE, MODE_STEMS, MODE_BEATJUMP, MODE_SAMPLER, MODE_KEYBOARD, MODE_PADFX, MODE_BEATLOOP, MODE_KEYSHIFT,
    // deck CCs (14-bit: MSB at data1, LSB at data1 + 0x20)
    TEMPO, TRIM, EQ_HI, EQ_MID, EQ_LOW, FADER,
    // deck relative CCs
    JOG_SIDE, JOG_PLATTER_VINYL, JOG_PLATTER_NOVINYL, JOG_SEARCH,
    // global
    XFADER, PHONES_MIX, COLOR1, COLOR2, COLOR3, COLOR4, COLOR_FX_ON, COLOR_FX_NEXT, MASTER_CUE,
    BROWSE_TURN, BROWSE_TURN_FAST, BROWSE_PRESS, BROWSE_BACK, BROWSE_TOP, VIEW_TOGGLE, TILT_FWD, TILT_BACK,
    TILT_LEFT, TILT_RIGHT, SOURCE_OPEN, LOAD1, LOAD2, LOAD3, LOAD4, PREVIEW,
    KEY_SYNC, KEY_RESET, DECK_LAYER, SHIFT_KEY, VINYL_TOGGLE,
    // Beat FX
    FX_TYPE0, FX_TYPE13 = FX_TYPE0 + 13, FX_CH1, FX_CH2, FX_CH3, FX_CH4, FX_MST, FX_SP, FX_ONOFF, FX_BEAT_DOWN,
    FX_BEAT_UP, FX_LEVEL,
    // GROOVE CIRCUIT as a drum roller
    DRUM_PAD0, DRUM_PAD1, DRUM_PAD2, DRUM_PAD3, DRUM_ROLL0, DRUM_ROLL1, DRUM_ROLL2, DRUM_ROLL3,
    DRUM_HOLD, DRUM_RELEASE, DRUM_GAIN, DRUM_CUE_BTN, DRUM_LEVER,
};

struct Entry {
    Scope scope;
    Kind kind;
    uint8_t data1;
    Action action;
    const char *control; // name in the official list
    const char *source;
};

// clang-format off
const Entry kMap[] = {
    // ---- deck section, MIDI ch 1-4 = deck 1-4 (deck 3/4 when the DECK 3/4 layer is selected on the unit)
    {DECK, NOTE, 0x0B, PLAY,           "D1 PLAY/PAUSE",             "AT+MX"},
    {DECK, NOTE, 0x47, IGNORE,         "D1 PLAY/PAUSE +SHIFT",      "AT+MX"},
    {DECK, NOTE, 0x0C, CUE,            "D2 CUE",                    "AT+MX"},
    {DECK, NOTE, 0x48, CUE_TO_START,   "D2 CUE +SHIFT",             "AT+MX"},
    {DECK, NOTE, 0x3F, SHIFT_KEY,      "D3 SHIFT",                  "AT+MX"},
    {DECK, NOTE, 0x36, JOG_TOUCH,      "D4 JOG touch",              "AT+MX"},
    {DECK, NOTE, 0x67, JOG_TOUCH,      "D4 JOG touch +SHIFT",       "AT+MX"},
    {DECK, NOTE, 0x40, IGNORE,         "D5 SLIP (not implemented)", "AT"},
    {DECK, NOTE, 0x17, VINYL_TOGGLE,   "D5 SLIP +SHIFT = vinyl/CD jog mode (VDJ manual)", "AT+VDJ"},
    {DECK, NOTE, 0x35, QUANTIZE,       "D6 QUANTIZE",               "AT"},
    {DECK, NOTE, 0x68, IGNORE,         "D6 QUANTIZE +SHIFT",        "AT"},
    {DECK, NOTE, 0x10, LOOP_IN,        "D7 IN/4 BEAT",              "AT+MX"},
    {DECK, NOTE, 0x14, LOOP_4BEAT,     "D7 IN/4 BEAT long press",   "AT"},
    {DECK, NOTE, 0x4C, LOOP_HALVE,     "D7 IN/4 BEAT +SHIFT",       "AT+MX"},
    {DECK, NOTE, 0x11, LOOP_OUT,       "D8 OUT",                    "AT+MX"},
    {DECK, NOTE, 0x77, LOOP_DOUBLE,    "D8 OUT +SHIFT",             "AT"},
    {DECK, NOTE, 0x4D, RELOOP,         "D9 RELOOP/EXIT",            "AT+MX"},
    {DECK, NOTE, 0x50, IGNORE,         "D9 RELOOP/EXIT +SHIFT",     "AT+MX"},
    {DECK, NOTE, 0x51, CALL_PREV,      "D10 CUE/LOOP CALL <",       "AT"},
    {DECK, NOTE, 0x61, JUMP_BACK,      "D10 CUE/LOOP CALL < +SHIFT","AT"},
    {DECK, NOTE, 0x53, CALL_NEXT,      "D11 CUE/LOOP CALL >",       "AT"},
    {DECK, NOTE, 0x62, JUMP_FWD,       "D11 CUE/LOOP CALL > +SHIFT","AT"},
    {DECK, NOTE, 0x3D, IGNORE,         "D12 MEMORY (Phase 3)",      "AT"},
    {DECK, NOTE, 0x3E, IGNORE,         "D12 MEMORY +SHIFT",         "AT"},
    {DECK, NOTE, 0x72, IGNORE,         "D13-16 DECK select",        "AT"},
    {DECK, NOTE, 0x73, IGNORE,         "D13-16 DECK select +SHIFT", "AT"},
    {DECK, NOTE, 0x3C, DECK_LAYER,     "deck control on/off",       "AT"},
    // MASTER TEMPO / KEY SYNC, official list p.2 read by row position (2026-11-05):
    // D17 MASTER TEMPO 9n 1A (+SHIFT 9n 60), D20 KEY SYNC 9n 65 (+SHIFT 9n 64), LEDs on the same notes.
    // The 2026-10-21 owner test had the two unshifted notes swapped: pressing MASTER TEMPO ran KEY SYNC
    // (pitch moved, no MT shown), so we follow the official list.
    {DECK, NOTE, 0x1A, KEYLOCK,        "D17 MASTER TEMPO",          "AT"},
    {DECK, NOTE, 0x60, RANGE_CYCLE,    "D17 MASTER TEMPO +SHIFT",   "AT+HW"},
    {DECK, NOTE, 0x58, SYNC,           "D19 BEAT SYNC",             "AT+MX"},
    {DECK, NOTE, 0x5C, MAKE_MASTER,    "D19 BEAT SYNC +SHIFT",      "AT+MX"},
    {DECK, NOTE, 0x65, KEY_SYNC,       "D20 KEY SYNC",              "AT"},
    {DECK, NOTE, 0x64, KEY_RESET,      "D20 KEY SYNC +SHIFT",       "AT+HW"},
    {DECK, NOTE, 0x54, CH_CUE,         "M7 CH CUE",                 "AT+MX"},
    {DECK, NOTE, 0x39, IGNORE,         "M7 CH CUE +SHIFT",          "AT"},
    {DECK, NOTE, 0x1B, MODE_HOTCUE,    "P9 HOT CUE MODE",           "AT"},
    {DECK, NOTE, 0x69, MODE_KEYBOARD,  "P9 HOT CUE MODE +SHIFT",    "AT"},
    {DECK, NOTE, 0x1E, MODE_STEMS,     "P10 STEMS MODE",            "AT"},
    {DECK, NOTE, 0x6B, MODE_PADFX,     "P10 STEMS MODE +SHIFT",     "AT"},
    {DECK, NOTE, 0x20, MODE_BEATJUMP,  "P11 BEAT JUMP MODE",        "AT"},
    {DECK, NOTE, 0x6D, MODE_BEATLOOP,  "P11 BEAT JUMP MODE +SHIFT", "AT"},
    {DECK, NOTE, 0x22, MODE_SAMPLER,   "P12 SAMPLER MODE",          "AT"},
    {DECK, NOTE, 0x6F, MODE_KEYSHIFT,  "P12 SAMPLER MODE +SHIFT",   "AT"},
    {DECK, NOTE, 0x79, IGNORE,         "pad mode long press",       "AT"},
    {DECK, NOTE, 0x66, IGNORE,         "CH fader start PLAY",       "AT"},
    {DECK, NOTE, 0x5D, IGNORE,         "CH fader start SYNC",       "AT"},
    {DECK, NOTE, 0x52, IGNORE,         "CH fader start CUE",        "AT"},
    // Groove Circuit is out of scope
    // GROOVE CIRCUIT, used as an RMX-1000-style drum roller (owner's design): left = decks 1/3, right = decks 2/4
    {DECK, NOTE, 0x00, DRUM_PAD0, "G2 DRUM SWAP 1 -> KICK", "AT"},  {DECK, NOTE, 0x01, DRUM_PAD1, "G3 DRUM SWAP 2 -> SNARE", "AT"},
    {DECK, NOTE, 0x02, DRUM_PAD2, "G4 DRUM SWAP 3 -> CLAP", "AT"},  {DECK, NOTE, 0x03, DRUM_PAD3, "G5 DRUM SWAP 4 -> HAT", "AT"},
    {DECK, NOTE, 0x08, DRUM_CUE_BTN, "G6 CAPTURE/LENGTH -> drum headphone CUE", "AT"},       {DECK, NOTE, 0x2B, IGNORE, "G6 CAPTURE +SHIFT", "AT"},
    {DECK, NOTE, 0x04, DRUM_ROLL0, "G7 DRUM ROLL 1 (1 beat)", "AT"}, {DECK, NOTE, 0x05, DRUM_ROLL1, "G8 DRUM ROLL 2 (1/2)", "AT"},
    {DECK, NOTE, 0x06, DRUM_ROLL2, "G9 DRUM ROLL 3 (1/4)", "AT"},   {DECK, NOTE, 0x07, DRUM_ROLL3, "G10 DRUM ROLL 4 (1/8)", "AT"},
    {DECK, NOTE, 0x2C, IGNORE, "G2 +SHIFT", "AT"}, {DECK, NOTE, 0x2D, IGNORE, "G3 +SHIFT", "AT"},
    {DECK, NOTE, 0x2E, IGNORE, "G4 +SHIFT", "AT"}, {DECK, NOTE, 0x2F, IGNORE, "G5 +SHIFT", "AT"},
    {DECK, NOTE, 0x30, IGNORE, "G7 +SHIFT", "AT"}, {DECK, NOTE, 0x31, IGNORE, "G8 +SHIFT", "AT"},
    {DECK, NOTE, 0x32, IGNORE, "G9 +SHIFT", "AT"}, {DECK, NOTE, 0x33, IGNORE, "G10 +SHIFT", "AT"},
    {DECK, NOTE, 0x24, DRUM_LEVER, "G11 lever (locks) -> HOLD", "AT"}, {DECK, NOTE, 0x26, IGNORE, "G11 +SHIFT fwd", "AT"},
    {DECK, NOTE, 0x27, IGNORE, "G11 +SHIFT back", "AT"},
    {DECK, CC,   0x12, DRUM_GAIN,      "G1 GAIN: left = drum GAIN, right = drum PITCH", "AT"},
    // 14-bit deck CCs
    {DECK, CC,   0x00, TEMPO,          "D18 TEMPO",                 "AT+MX"},
    {DECK, CC,   0x05, TEMPO,          "D18 TEMPO +SHIFT",          "AT"},
    {DECK, CC,   0x04, TRIM,           "M3 TRIM",                   "AT+MX"},
    {DECK, CC,   0x07, EQ_HI,          "M4 EQ HI",                  "AT+MX"},
    {DECK, CC,   0x0B, EQ_MID,         "M5 EQ MID",                 "AT+MX"},
    {DECK, CC,   0x0F, EQ_LOW,         "M6 EQ LOW",                 "AT+MX"},
    {DECK, CC,   0x13, FADER,          "M2 CH FADER",               "AT+MX"},
    // relative jog CCs, centre 0x40
    {DECK, CC,   0x21, JOG_SIDE,       "D4 JOG wheel side",         "AT+MX"},
    {DECK, CC,   0x22, JOG_PLATTER_VINYL,   "D4 JOG platter, vinyl on",  "AT+MX"},
    {DECK, CC,   0x23, JOG_PLATTER_NOVINYL, "D4 JOG platter, vinyl off", "AT+MX"},
    {DECK, CC,   0x26, JOG_SEARCH,     "D4 JOG wheel side +SHIFT",  "AT"},
    {DECK, CC,   0x29, JOG_SEARCH,     "D4 JOG platter +SHIFT",     "AT+MX"},

    // ---- global section, MIDI ch 7 (0x96 / 0xB6)
    {GLOBAL, CC,   0x1F, XFADER,       "M1 CROSSFADER",             "AT+MX"},
    {GLOBAL, CC,   0x0C, PHONES_MIX,   "M13 HEADPHONES MIX",        "AT+MX"},
    {GLOBAL, CC,   0x0D, IGNORE,       "M12 HEADPHONES LEVEL (analogue)", "AT+MX"},
    {GLOBAL, CC,   0x08, IGNORE,       "M8 MASTER LEVEL (hardware)", "AT+VDJ"},
    {GLOBAL, CC,   0x09, IGNORE,       "M10 BOOTH LEVEL (hardware)", "AT+VDJ"},
    {GLOBAL, CC,   0x05, IGNORE,       "M14 MIC LEVEL",             "AT"},
    {GLOBAL, CC,   0x17, COLOR1,       "F1 SOUND COLOR FX CH1",     "AT"},
    {GLOBAL, CC,   0x18, COLOR2,       "F1 SOUND COLOR FX CH2",     "AT"},
    {GLOBAL, CC,   0x19, COLOR3,       "F1 SOUND COLOR FX CH3",     "AT"},
    {GLOBAL, CC,   0x1A, COLOR4,       "F1 SOUND COLOR FX CH4",     "AT"},
    // owner's choice: the ON button picks the next colour FX (FILTER > DUB ECHO > SWEEP > SPACE), SHIFT+ON = on/off
    {GLOBAL, NOTE, 0x00, COLOR_FX_NEXT, "F2 SOUND COLOR FX ON/OFF",  "AT"},
    {GLOBAL, NOTE, 0x08, COLOR_FX_ON,  "F2 SOUND COLOR FX +SHIFT",  "AT"},
    {GLOBAL, NOTE, 0x63, MASTER_CUE,   "M9 MASTER CUE",             "AT"},
    {GLOBAL, NOTE, 0x62, IGNORE,       "M9 MASTER CUE +SHIFT",      "AT"},
    {GLOBAL, NOTE, 0x6D, IGNORE,       "M11 Android MONO/STEREO",   "AT"},
    {GLOBAL, CC,   0x40, BROWSE_TURN,  "B1 BROWSE rotate",          "AT+MX"},
    {GLOBAL, CC,   0x64, BROWSE_TURN_FAST, "B1 BROWSE rotate +SHIFT", "AT"},
    {GLOBAL, NOTE, 0x41, BROWSE_PRESS, "B1 BROWSE press",           "AT+MX"},
    {GLOBAL, NOTE, 0x42, BROWSE_BACK,  "B1 BROWSE press +SHIFT",    "AT+MX"},
    {GLOBAL, NOTE, 0x46, LOAD1,        "B6 LOAD 1",                 "AT+MX"},
    {GLOBAL, NOTE, 0x47, LOAD2,        "B6 LOAD 2",                 "AT+MX"},
    {GLOBAL, NOTE, 0x48, LOAD3,        "B6 LOAD 3",                 "AT+MX"},
    {GLOBAL, NOTE, 0x49, LOAD4,        "B6 LOAD 4",                 "AT+MX"},
    {GLOBAL, NOTE, 0x58, IGNORE,       "B6 LOAD 1 +SHIFT",          "AT"},
    {GLOBAL, NOTE, 0x59, IGNORE,       "B6 LOAD 2 +SHIFT",          "AT"},
    {GLOBAL, NOTE, 0x60, IGNORE,       "B6 LOAD 3 +SHIFT",          "AT"},
    {GLOBAL, NOTE, 0x61, IGNORE,       "B6 LOAD 4 +SHIFT",          "AT"},
    {GLOBAL, NOTE, 0x36, PREVIEW,      "B5 PREVIEW",                "AT"},
    {GLOBAL, NOTE, 0x65, BROWSE_BACK,  "B3 BACK",                   "AT"},
    {GLOBAL, NOTE, 0x66, BROWSE_TOP,   "B3 BACK +SHIFT",            "AT"},
    {GLOBAL, NOTE, 0x7A, VIEW_TOGGLE,  "B4 VIEW",                   "AT"},
    {GLOBAL, NOTE, 0x67, IGNORE,       "B4 VIEW +SHIFT",            "AT"},
    // B2 DISCOVER, official list p.1 (96 35, +SHIFT 96 68), read by row position 2026-11-10 between B1 press
    // (96 41/42) and B3 BACK (96 65/66). Owner (2026-11-10): DISCOVER = the XDJ-AZ SOURCE screen, SHIFT = BROWSE.
    {GLOBAL, NOTE, 0x35, SOURCE_OPEN,  "B2 DISCOVER -> SOURCE",     "AT"},
    {GLOBAL, NOTE, 0x68, VIEW_TOGGLE,  "B2 DISCOVER +SHIFT -> BROWSE", "AT"},
    {GLOBAL, NOTE, 0x38, TILT_FWD,     "B1 tilt forward",           "AT"},
    {GLOBAL, NOTE, 0x39, IGNORE,       "B1 tilt forward +SHIFT",    "AT"},
    {GLOBAL, NOTE, 0x3A, TILT_BACK,    "B1 tilt backward",          "AT"},
    {GLOBAL, NOTE, 0x3B, IGNORE,       "B1 tilt backward +SHIFT",   "AT"},
    {GLOBAL, NOTE, 0x3C, TILT_LEFT,    "B1 tilt to the left",       "AT"},
    {GLOBAL, NOTE, 0x3D, IGNORE,       "B1 tilt to the left +SHIFT","AT"},
    {GLOBAL, NOTE, 0x2E, TILT_RIGHT,   "B1 tilt to the right",      "AT"},
    {GLOBAL, NOTE, 0x3F, IGNORE,       "B1 tilt to the right +SHIFT","AT"},
    {GLOBAL, NOTE, 0x37, IGNORE,       "B5 PREVIEW +SHIFT",         "AT"},
    // ---- Beat FX section, MIDI ch 5 (0x94 / 0xB4)
    {FX, NOTE, 0x20, FX_TYPE0,         "F5 BEAT FX SELECT DELAY",   "AT"},
    {FX, NOTE, 0x21, Action(FX_TYPE0 + 1),  "F5 ECHO",              "AT"},
    {FX, NOTE, 0x22, Action(FX_TYPE0 + 2),  "F5 LOW CUT ECHO",      "AT"},
    {FX, NOTE, 0x23, Action(FX_TYPE0 + 3),  "F5 SPIRAL",            "AT"},
    {FX, NOTE, 0x24, Action(FX_TYPE0 + 4),  "F5 HELIX",             "AT"},
    {FX, NOTE, 0x25, Action(FX_TYPE0 + 5),  "F5 REVERB",            "AT"},
    {FX, NOTE, 0x26, Action(FX_TYPE0 + 6),  "F5 FLANGER",           "AT"},
    {FX, NOTE, 0x27, Action(FX_TYPE0 + 7),  "F5 PHASER",            "AT"},
    {FX, NOTE, 0x28, Action(FX_TYPE0 + 8),  "F5 FILTER",            "AT"},
    {FX, NOTE, 0x29, Action(FX_TYPE0 + 9),  "F5 TRANS",             "AT"},
    {FX, NOTE, 0x2A, Action(FX_TYPE0 + 10), "F5 PITCH",             "AT"},
    {FX, NOTE, 0x2B, Action(FX_TYPE0 + 11), "F5 ROLL",              "AT"},
    {FX, NOTE, 0x2C, Action(FX_TYPE0 + 12), "F5 MOBIUS SAW",        "AT"},
    {FX, NOTE, 0x2D, Action(FX_TYPE0 + 13), "F5 MOBIUS TRI",        "AT"},
    {FX, NOTE, 0x10, FX_CH1,           "F6 CH SELECT 1",            "AT+MX"},
    {FX, NOTE, 0x11, FX_CH2,           "F6 CH SELECT 2",            "AT+MX"},
    {FX, NOTE, 0x12, FX_CH3,           "F6 CH SELECT 3",            "AT+MX"},
    {FX, NOTE, 0x13, FX_CH4,           "F6 CH SELECT 4",            "AT+MX"},
    {FX, NOTE, 0x14, FX_MST,           "F6 CH SELECT MST",          "AT+MX"},
    {FX, NOTE, 0x16, FX_SP,            "F6 CH SELECT SP",           "AT+MX"},
    {FX, NOTE, 0x18, IGNORE, "F6 +SHIFT", "AT"}, {FX, NOTE, 0x19, IGNORE, "F6 +SHIFT", "AT"},
    {FX, NOTE, 0x1A, IGNORE, "F6 +SHIFT", "AT"}, {FX, NOTE, 0x1B, IGNORE, "F6 +SHIFT", "AT"},
    {FX, NOTE, 0x1C, IGNORE, "F6 +SHIFT", "AT"}, {FX, NOTE, 0x1E, IGNORE, "F6 +SHIFT", "AT"},
    {FX, NOTE, 0x47, FX_ONOFF,         "F8 BEAT FX ON/OFF",         "AT+MX"},
    {FX, NOTE, 0x43, FX_ONOFF,         "F8 BEAT FX ON/OFF +SHIFT",  "AT+MX"},
    {FX, NOTE, 0x4A, FX_BEAT_DOWN,     "F3 BEAT <",                 "AT"},
    {FX, NOTE, 0x66, IGNORE,           "F3 BEAT < +SHIFT",          "AT"},
    {FX, NOTE, 0x4B, FX_BEAT_UP,       "F4 BEAT >",                 "AT"},
    {FX, NOTE, 0x6B, IGNORE,           "F4 BEAT > +SHIFT",          "AT"},
    {FX, CC,   0x02, FX_LEVEL,         "F7 LEVEL/DEPTH",            "AT+MX"},
    {GLOBAL, NOTE, 0x50, IGNORE, "crossfader start PLAY", "AT"}, {GLOBAL, NOTE, 0x51, IGNORE, "crossfader start PLAY", "AT"},
    {GLOBAL, NOTE, 0x52, IGNORE, "crossfader start CUE", "AT"},  {GLOBAL, NOTE, 0x53, IGNORE, "crossfader start CUE", "AT"},
    {GLOBAL, NOTE, 0x54, IGNORE, "crossfader start SYNC", "AT"}, {GLOBAL, NOTE, 0x55, IGNORE, "crossfader start SYNC", "AT"},
};
// clang-format on
constexpr int kMapSize = int(sizeof(kMap) / sizeof(kMap[0]));

// Pads: normal on MIDI ch 8/10/12/14 (status 0x97/0x99/0x9B/0x9D), SHIFT on ch 9/11/13/15.
// note = mode * 0x10 + (pad - 1). Modes in list order. Source: AT+MX.
enum PadMode { PM_HOTCUE = 0, PM_STEMS, PM_BEATJUMP, PM_SAMPLER, PM_KEYBOARD, PM_PADFX, PM_BEATLOOP, PM_KEYSHIFT };
const uint8_t kModeButtonNote[8] = {0x1B, 0x1E, 0x20, 0x22, 0x69, 0x6B, 0x6D, 0x6F};

// Our choices for pad layouts (rekordbox's exact layout is not in the list):
const double kBeatJump[8] = {-1, 1, -2, 2, -4, 4, -8, 8};
const double kBeatLoop[8] = {0.25, 0.5, 1, 2, 4, 8, 16, 32};

const Entry *find(Scope s, Kind k, uint8_t d1) {
    for (const auto &e : kMap)
        if (e.scope == s && e.kind == k && e.data1 == d1) return &e;
    return nullptr;
}

template <typename T, size_t N> struct Spsc {
    T buf[N];
    std::atomic<size_t> head{0}, tail{0};
    bool push(const T &v) {
        size_t h = head.load(std::memory_order_relaxed), n = (h + 1) % N;
        if (n == tail.load(std::memory_order_acquire)) return false;
        buf[h] = v;
        head.store(n, std::memory_order_release);
        return true;
    }
    bool pop(T &v) {
        size_t t = tail.load(std::memory_order_relaxed);
        if (t == head.load(std::memory_order_acquire)) return false;
        v = buf[t];
        tail.store((t + 1) % N, std::memory_order_release);
        return true;
    }
};

} // namespace

struct djc_midi {
    djc_engine *e;
    // MIDI thread state
    uint8_t msb[16][128];
    int lastVinyl[DJC_DECKS] = {-1, -1, -1, -1};
    bool shift[DJC_DECKS] = {false, false, false, false}; // software SHIFT state per deck section
    std::atomic<int> vinyl[DJC_DECKS];
    double lastBeatLoop[DJC_DECKS] = {0, 0, 0, 0};
    // shared
    std::atomic<int> padMode[DJC_DECKS];
    Spsc<djc_ui_event, 256> events;
    // UI thread LED cache: 0..127 sent value, 0xFF unknown
    uint8_t noteCache[16][128];
    uint8_t ccCache[16][128];
};

static void post(djc_midi *m, int type, int deck, int i = 0, double v = 0) { djc_post(m->e, 1, type, deck, i, v); }

static void uiEvent(djc_midi *m, int kind, int deck, int value, const uint8_t *raw = nullptr) {
    djc_ui_event ev{kind, deck, value, {0, 0, 0}};
    if (raw) std::memcpy(ev.raw, raw, 3);
    m->events.push(ev);
}

static inline int rel(uint8_t v) { return int(v) - 0x40; }          // jog: centre 0x40
static inline int relBrowse(uint8_t v) { return v < 0x40 ? v : int(v) - 128; } // browse: 1..30 / 127..98

extern "C" {

djc_midi *djc_midi_create(djc_engine *e) {
    auto *m = new djc_midi();
    m->e = e;
    std::memset(m->msb, 0, sizeof m->msb);
    for (auto &p : m->padMode) p.store(PM_HOTCUE);
    for (auto &v : m->vinyl) v.store(1);
    djc_midi_reset_leds(m);
    return m;
}

void djc_midi_destroy(djc_midi *m) { delete m; }

int32_t djc_midi_table_size(void) { return kMapSize; }
const char *djc_midi_table_source(int32_t i) { return (i >= 0 && i < kMapSize) ? kMap[i].source : nullptr; }

void djc_midi_in(djc_midi *m, const uint8_t *msg, int32_t len) {
    if (len < 3) return;
    uint8_t st = msg[0], type = st & 0xF0, ch = st & 0x0F, d1 = msg[1] & 0x7F, d2 = msg[2] & 0x7F;
    bool isNote = type == 0x90 || type == 0x80;
    if (type == 0x80) d2 = 0;
    bool press = isNote && d2 > 0;

    // pads
    if (isNote && ch >= 7 && ch <= 14) {
        int deck = (ch - 7) / 2;
        bool shift = ((ch - 7) % 2) == 1;
        int mode = d1 >> 4, pad = d1 & 0x0F;
        if (pad > 7) { uiEvent(m, DJC_EV_UNMAPPED, deck, 0, msg); return; }
        m->padMode[deck].store(mode, std::memory_order_relaxed);
        switch (mode) {
        case PM_HOTCUE:
            if (shift) { if (press) post(m, DJC_HOTCUE_DELETE, deck, pad); }
            else post(m, DJC_HOTCUE, deck, pad, press ? 1 : 0);
            break;
        case PM_BEATJUMP:
            if (press) post(m, DJC_BEAT_JUMP, deck, 0, kBeatJump[pad]);
            break;
        case PM_BEATLOOP:
            if (press) {
                djc_deck_state s;
                djc_get_deck_state(m->e, deck, &s);
                if (s.loopActive && m->lastBeatLoop[deck] == kBeatLoop[pad]) {
                    post(m, DJC_LOOP_EXIT, deck);
                    m->lastBeatLoop[deck] = 0;
                } else {
                    post(m, DJC_BEAT_LOOP, deck, 0, kBeatLoop[pad]);
                    m->lastBeatLoop[deck] = kBeatLoop[pad];
                }
            }
            break;
        case PM_KEYSHIFT:
            if (press) {
                // our layout: top row +1..+4, bottom row -1..-4 semitones; same pad again resets
                int semis = pad < 4 ? pad + 1 : -(pad - 3);
                djc_deck_state s;
                djc_get_deck_state(m->e, deck, &s);
                post(m, DJC_KEY_SHIFT, deck, 0, s.keyShift == semis ? 0 : semis);
            }
            break;
        default: break; // stems, sampler, keyboard, pad fx: not implemented yet
        }
        return;
    }

    Scope scope;
    int deck = 0;
    if (ch <= 3) { scope = DECK; deck = ch; }
    else if (ch == 6) scope = GLOBAL;
    else if (ch == 4) scope = FX;
    else { uiEvent(m, DJC_EV_UNMAPPED, 0, 0, msg); return; }

    Kind kind = isNote ? NOTE : CC;
    uint8_t key = d1;
    bool lsb = false;
    const Entry *en = (type == 0xB0 || isNote) ? find(scope, kind, d1) : nullptr;
    if (!en && kind == CC && d1 >= 0x20) { // 14-bit LSB?
        const Entry *msbEntry = find(scope, CC, uint8_t(d1 - 0x20));
        if (msbEntry && msbEntry->action != JOG_SIDE && msbEntry->action != JOG_PLATTER_VINYL &&
            msbEntry->action != JOG_PLATTER_NOVINYL && msbEntry->action != JOG_SEARCH &&
            msbEntry->action != BROWSE_TURN && msbEntry->action != BROWSE_TURN_FAST) {
            en = msbEntry; lsb = true; key = uint8_t(d1 - 0x20);
        }
    }
    if (!en) { uiEvent(m, DJC_EV_UNMAPPED, deck, 0, msg); return; }

    // 14-bit value: MSB arrives first (applied coarse), LSB refines
    double v14 = 0;
    if (kind == CC) {
        if (lsb) v14 = double((m->msb[ch][key] << 7) | d2) / 16383.0;
        else { m->msb[ch][key] = d2; v14 = double(d2 << 7) / 16383.0; }
    }

    switch (en->action) {
    case IGNORE: break;
    case PLAY: if (press) post(m, DJC_PLAY, deck); break;
    case CUE: post(m, DJC_CUE, deck, press ? 1 : 0); break;
    case CUE_TO_START: if (press) post(m, DJC_SEEK, deck, 0, 0); break;
    case JOG_TOUCH: post(m, DJC_JOG_TOUCH, deck, press ? 1 : 0); break;
    case QUANTIZE: if (press) post(m, DJC_QUANTIZE, deck, 2); break;
    case LOOP_IN: if (press) post(m, DJC_LOOP_IN, deck); break;
    case LOOP_4BEAT: if (press) post(m, DJC_BEAT_LOOP, deck, 0, 4); break;
    case LOOP_OUT: if (press) post(m, DJC_LOOP_OUT, deck); break;
    case LOOP_HALVE: if (press) post(m, DJC_LOOP_HALVE, deck); break;
    case LOOP_DOUBLE: if (press) post(m, DJC_LOOP_DOUBLE, deck); break;
    case RELOOP: if (press) post(m, DJC_LOOP_EXIT, deck); break;
    // SHIFT may arrive as its own note (0x3F) while the button keeps its normal note, so track it here.
    case CALL_PREV: if (press) { if (m->shift[deck]) post(m, DJC_LOOP_HALVE, deck); else post(m, DJC_CUE_CALL, deck, -1); } break;
    case CALL_NEXT: if (press) { if (m->shift[deck]) post(m, DJC_LOOP_DOUBLE, deck); else post(m, DJC_CUE_CALL, deck, 1); } break;
    case JUMP_BACK: if (press) post(m, DJC_BEAT_JUMP, deck, 0, -4); break;
    case JUMP_FWD: if (press) post(m, DJC_BEAT_JUMP, deck, 0, 4); break;
    case KEYLOCK: if (press) { if (m->shift[deck]) post(m, DJC_TEMPO_RANGE, deck, 0, -1); else post(m, DJC_KEYLOCK, deck, 2); } break;
    case RANGE_CYCLE: if (press) post(m, DJC_TEMPO_RANGE, deck, 0, -1); break;
    case SYNC: if (press) post(m, DJC_SYNC, deck, 2); break;
    case MAKE_MASTER: if (press) post(m, DJC_MAKE_MASTER, deck); break;
    case CH_CUE: if (press) post(m, DJC_CH_CUE, deck, 2); break;
    case MODE_HOTCUE: case MODE_STEMS: case MODE_BEATJUMP: case MODE_SAMPLER:
    case MODE_KEYBOARD: case MODE_PADFX: case MODE_BEATLOOP: case MODE_KEYSHIFT:
        if (press) m->padMode[deck].store(int(en->action - MODE_HOTCUE), std::memory_order_relaxed);
        break;
    // official list: TEMPO "-" side = min, "+" side = max
    case TEMPO: { // centre zone = exactly 0 % (like the hardware's centre click), so two decks at 0 % never drift
        double t = v14 * 2.0 - 1.0;
        if (std::fabs(t) < 0.015) t = 0;
        post(m, DJC_TEMPO, deck, 0, t);
    } break;
    case TRIM: post(m, DJC_TRIM, deck, 0, v14); break;
    case EQ_HI: post(m, DJC_EQ_HI, deck, 0, v14); break;
    case EQ_MID: post(m, DJC_EQ_MID, deck, 0, v14); break;
    case EQ_LOW: post(m, DJC_EQ_LOW, deck, 0, v14); break;
    case FADER: post(m, DJC_FADER, deck, 0, v14); break;
    case DRUM_PAD0: case DRUM_PAD1: case DRUM_PAD2: case DRUM_PAD3:
        post(m, DJC_DRUM_PAD, deck & 1, int(en->action - DRUM_PAD0), press ? 1 : 0); break;
    case DRUM_ROLL0: case DRUM_ROLL1: case DRUM_ROLL2: case DRUM_ROLL3:
        post(m, DJC_DRUM_ROLL, deck & 1, int(en->action - DRUM_ROLL0), press ? 1 : 0); break;
    case DRUM_HOLD: break;
    case DRUM_RELEASE: break;
    case DRUM_CUE_BTN: if (press) post(m, DJC_DRUM_CUE, deck & 1); break;
    case DRUM_LEVER: post(m, DJC_DRUM_HOLD, deck & 1, press ? 1 : 0); break;
    case DRUM_GAIN: post(m, DJC_DRUM_GAIN, deck & 1, 0, v14); break;
    case JOG_SIDE: post(m, DJC_JOG_TURN, deck, rel(d2)); break;
    case JOG_PLATTER_VINYL:
    case JOG_PLATTER_NOVINYL: {
        int vinyl = en->action == JOG_PLATTER_VINYL;
        if (m->lastVinyl[deck] != vinyl) { // the unit switched mode itself
            post(m, DJC_VINYL_MODE, deck, vinyl); m->lastVinyl[deck] = vinyl; m->vinyl[deck].store(vinyl);
        }
        post(m, DJC_JOG_TURN, deck, rel(d2));
    } break;
    case JOG_SEARCH: post(m, DJC_JOG_SEARCH, deck, rel(d2)); break;
    case XFADER: post(m, DJC_CROSSFADER, 0, 0, v14); break;
    case PHONES_MIX: post(m, DJC_PHONES_MIX, 0, 0, v14); break;
    case COLOR1: case COLOR2: case COLOR3: case COLOR4:
        post(m, DJC_COLOR, int(en->action - COLOR1), 0, v14);
        break;
    case COLOR_FX_ON: if (press) post(m, DJC_COLOR_FX_ON, 0, 2); break;
    case COLOR_FX_NEXT: if (press) post(m, DJC_COLOR_FX_TYPE, 0, -1); break;
    case MASTER_CUE: if (press) post(m, DJC_MASTER_CUE, 0, 2); break;
    case BROWSE_TURN: uiEvent(m, DJC_EV_BROWSE_TURN, 0, relBrowse(d2)); break;
    case BROWSE_TURN_FAST: uiEvent(m, DJC_EV_BROWSE_TURN, 0, relBrowse(d2) * 8); break;
    case BROWSE_PRESS: if (press) uiEvent(m, DJC_EV_BROWSE_PRESS, 0, 1); break;
    case BROWSE_BACK: if (press) uiEvent(m, DJC_EV_BROWSE_BACK, 0, 1); break;
    case SOURCE_OPEN: if (press) uiEvent(m, DJC_EV_SOURCE, 0, 1); break;
    case BROWSE_TOP: if (press) uiEvent(m, DJC_EV_BROWSE_BACK, 0, 2); break;
    case VIEW_TOGGLE: if (press) uiEvent(m, DJC_EV_VIEW, 0, 1); break;
    case TILT_FWD: if (press) uiEvent(m, DJC_EV_TILT, 0, 1); break;
    case TILT_BACK: if (press) uiEvent(m, DJC_EV_TILT, 0, 2); break;
    case TILT_LEFT: if (press) uiEvent(m, DJC_EV_TILT, 0, 3); break;
    case TILT_RIGHT: if (press) uiEvent(m, DJC_EV_TILT, 0, 4); break;
    case LOAD1: case LOAD2: case LOAD3: case LOAD4:
        if (press) uiEvent(m, DJC_EV_LOAD, int(en->action - LOAD1), 1);
        break;
    case PREVIEW: if (press) uiEvent(m, DJC_EV_PREVIEW, 0, 1); break;
    // KEY SYNC: the app picks the deck to retune (the queued one, never a deck that is live on its own)
    case KEY_SYNC: if (press) { if (m->shift[deck]) post(m, DJC_KEY_SHIFT, deck, 100); else uiEvent(m, DJC_EV_KEY_SYNC, deck, 1); } break;
    case SHIFT_KEY: m->shift[deck] = press; break;
    case VINYL_TOGGLE:
        if (press) {
            int v = !m->vinyl[deck].load();
            m->vinyl[deck].store(v);
            post(m, DJC_VINYL_MODE, deck, v);
            m->lastVinyl[deck] = v;
        }
        break;
    case KEY_RESET: if (press) post(m, DJC_KEY_SHIFT, deck, 100); break;
    case DECK_LAYER: uiEvent(m, DJC_EV_DECK_LAYER, deck, d2 > 0 ? 1 : 0); break;
    case FX_CH1: case FX_CH2: case FX_CH3: case FX_CH4: case FX_MST: case FX_SP:
        if (press) post(m, DJC_FX_TARGET, 0, int(en->action - FX_CH1));
        break;
    case FX_ONOFF: if (press) post(m, DJC_FX_ON, 0, 2); break;
    case FX_BEAT_DOWN: if (press) post(m, DJC_FX_BEAT, 0, -1); break;
    case FX_BEAT_UP: if (press) post(m, DJC_FX_BEAT, 0, 1); break;
    case FX_LEVEL: post(m, DJC_FX_LEVEL, 0, 0, v14); break;
    default:
        if (en->action >= FX_TYPE0 && en->action <= FX_TYPE13) {
            if (press) post(m, DJC_FX_TYPE, 0, int(en->action - FX_TYPE0));
        }
        break;
    }
}

int32_t djc_midi_poll_event(djc_midi *m, djc_ui_event *ev) { return m->events.pop(*ev) ? 1 : 0; }

int32_t djc_midi_pad_mode(const djc_midi *m, int32_t deck) {
    return (deck >= 0 && deck < DJC_DECKS) ? m->padMode[deck].load(std::memory_order_relaxed) : 0;
}

void djc_midi_reset_leds(djc_midi *m) {
    std::memset(m->noteCache, 0xFF, sizeof m->noteCache);
    std::memset(m->ccCache, 0xFF, sizeof m->ccCache);
}

namespace {
struct Out {
    uint8_t *buf;
    int cap, n = 0;
    void note(djc_midi *m, int ch, int note, bool on) {
        uint8_t v = on ? 0x7F : 0x00;
        if (m->noteCache[ch][note] == v || n + 3 > cap) return;
        m->noteCache[ch][note] = v;
        buf[n++] = uint8_t(0x90 | ch); buf[n++] = uint8_t(note); buf[n++] = v;
    }
    void cc(djc_midi *m, int ch, int cc, int value) {
        uint8_t v = uint8_t(value < 0 ? 0 : (value > 127 ? 127 : value));
        if (m->ccCache[ch][cc] == v || n + 3 > cap) return;
        m->ccCache[ch][cc] = v;
        buf[n++] = uint8_t(0xB0 | ch); buf[n++] = uint8_t(cc); buf[n++] = v;
    }
};

// official list: level meter thresholds 0x26 one green, 0x41 two greens, 0x57 orange, 0x65 two oranges, 0x77 red
int meterValue(float peak) {
    if (peak <= 1e-5f) return 0;
    double db = 20.0 * std::log10(double(peak));
    if (db >= -0.5) return 0x7F;   // red
    if (db >= -3) return 0x6E;     // two oranges
    if (db >= -6) return 0x5C;     // one orange
    if (db >= -12) return 0x48;    // two greens
    if (db >= -24) return 0x30;    // one green
    return 0;
}
} // namespace

int32_t djc_midi_out(djc_midi *m, uint8_t *buf, int32_t cap) {
    Out o{buf, cap};
    djc_mixer_state mx;
    djc_get_mixer_state(m->e, &mx);
    // blink phase, ~1.7 Hz (LEDs only know on/off: official list OFF=0x00, ON=0x7F, so blinking is done here)
    bool blink = std::fmod(djc_now(), 0.6) < 0.3;
    o.note(m, 6, 0x00, mx.colorFxOn != 0 && blink);   // F2 SOUND COLOR FX ON/OFF (MIDI-OUT 96 00): blinks while on
    for (int d = 0; d < DJC_DECKS; ++d) {
        djc_deck_state s;
        djc_get_deck_state(m->e, d, &s);
        bool paused = s.loaded && !s.playing;
        o.note(m, d, 0x0B, paused ? blink : true);     // D1 PLAY (9n 0B): blinks when a loaded deck is stopped
        o.note(m, d, 0x0C, paused ? blink : true);     // D2 CUE (9n 0C): same
        o.note(m, d, 0x35, s.quantize);                // D6 QUANTIZE
        o.note(m, d, 0x40, false);                     // D5 SLIP (not implemented)
        o.note(m, d, 0x17, m->vinyl[d].load() != 0);   // SHIFT layer of SLIP: vinyl mode indicator
        o.note(m, d, 0x10, s.loopActive && blink);     // D7 IN (9n 10): blinks during loop playback
        o.note(m, d, 0x11, s.loopActive && blink);     // D8 OUT (9n 11); RELOOP/EXIT has no LED in the list
        o.note(m, d, 0x1A, s.keylock);                 // D17 MASTER TEMPO (9n 1A)
        o.note(m, d, 0x65, s.keySync);                 // D20 KEY SYNC (9n 65)
        o.note(m, d, 0x58, s.sync);                    // D19 BEAT SYNC
        o.note(m, d, 0x54, mx.cue[d] != 0);            // M7 CH CUE
        int mode = m->padMode[d].load(std::memory_order_relaxed);
        for (int k = 0; k < 8; ++k) o.note(m, d, kModeButtonNote[k], k == mode); // P9-P12 (+SHIFT)
        // pads on ch 8/10/12/14
        int pch = 7 + 2 * d;
        for (int p = 0; p < 8; ++p) {
            bool on = false;
            if (mode == PM_HOTCUE) on = s.hotcues[p] >= 0;
            else if (mode == PM_BEATLOOP) on = s.loopActive && s.loopIn >= 0 && s.trackBpm > 0 &&
                std::fabs((s.loopOut - s.loopIn) * s.trackBpm / 60.0 - kBeatLoop[p]) < 0.01;
            else if (mode == PM_BEATJUMP) on = s.loaded;
            else if (mode == PM_KEYSHIFT) on = s.keyShift == (p < 4 ? p + 1 : -(p - 3));
            o.note(m, pch, mode * 0x10 + p, on);
        }
        o.note(m, 15, d, s.loaded);                    // LOAD illumination, ch 16 note 0-3
        int ds = djc_drum_state(m->e, d & 1);          // GROOVE CIRCUIT drum roller LEDs
        o.note(m, d, 0x08, (ds & 1) != 0);             // CAPTURE lit = drums in the headphone cue
        for (int k = 0; k < 4; ++k) {
            o.note(m, d, k, (ds & (2 << k)) != 0);     // selected drums
            o.note(m, d, 0x04 + k, (ds & (256 << k)) != 0); // roll rate
        }
        int jog = 0;
        if (s.loaded) {                                 // JOG indicator, ch 16 CC 0-3, 0x00-0x48
            double revs = s.position * (100.0 / 3.0) / 60.0;
            jog = int((revs - std::floor(revs)) * 0x48);
        }
        o.cc(m, 15, d, jog);
        o.cc(m, d, 0x02, meterValue(std::max(s.peakL, s.peakR))); // M15 CH LEVEL METER
    }
    djc_fx_state fx;
    djc_get_fx_state(m->e, &fx);
    o.note(m, 4, 0x47, fx.on != 0);                    // F8 BEAT FX ON/OFF
    o.note(m, 4, 0x43, fx.on != 0);
    o.cc(m, 4, 0x64, fx.beatCode);                     // F9 beat indicator
    return o.n;
}

} // extern "C"
