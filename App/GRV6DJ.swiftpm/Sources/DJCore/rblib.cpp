// rekordbox USB readers: Device Library (PIONEER/rekordbox/export.pdb) and ANLZ analysis files.
// Written from the public format documentation only:
//   Deep Symmetry, "rekordbox Export Analysis" https://djl-analysis.deepsymmetry.org/
//   crate-digger rekordbox_pdb.ksy / rekordbox_anlz.ksy (format descriptions)
// No code was copied from crate-digger, rekordcrate or pyrekordbox.
// Every read is bounds-checked: a damaged stick must never crash the player.
#include "djcore.h"

#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <vector>
#include <algorithm>

namespace {

std::vector<uint8_t> readFile(const char *path) {
    std::vector<uint8_t> v;
    FILE *f = std::fopen(path, "rb");
    if (!f) return v;
    std::fseek(f, 0, SEEK_END);
    long n = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    if (n > 0 && n < (1L << 30)) {
        v.resize(size_t(n));
        if (std::fread(v.data(), 1, size_t(n), f) != size_t(n)) v.clear();
    }
    std::fclose(f);
    return v;
}

struct Buf {
    const uint8_t *p;
    size_t n;
    bool ok(size_t o, size_t len) const { return o <= n && len <= n - o; }
    uint8_t u8(size_t o) const { return ok(o, 1) ? p[o] : 0; }
    uint16_t le16(size_t o) const { return ok(o, 2) ? uint16_t(p[o] | p[o + 1] << 8) : 0; }
    uint32_t le32(size_t o) const {
        return ok(o, 4) ? uint32_t(p[o]) | uint32_t(p[o + 1]) << 8 | uint32_t(p[o + 2]) << 16 | uint32_t(p[o + 3]) << 24 : 0;
    }
    uint16_t be16(size_t o) const { return ok(o, 2) ? uint16_t(p[o] << 8 | p[o + 1]) : 0; }
    uint32_t be32(size_t o) const {
        return ok(o, 4) ? uint32_t(p[o]) << 24 | uint32_t(p[o + 1]) << 16 | uint32_t(p[o + 2]) << 8 | uint32_t(p[o + 3]) : 0;
    }
};

void appendUtf8(std::string &s, uint32_t cp) {
    if (cp < 0x80) s += char(cp);
    else if (cp < 0x800) { s += char(0xC0 | cp >> 6); s += char(0x80 | (cp & 0x3F)); }
    else if (cp < 0x10000) { s += char(0xE0 | cp >> 12); s += char(0x80 | ((cp >> 6) & 0x3F)); s += char(0x80 | (cp & 0x3F)); }
    else { s += char(0xF0 | cp >> 18); s += char(0x80 | ((cp >> 12) & 0x3F)); s += char(0x80 | ((cp >> 6) & 0x3F)); s += char(0x80 | (cp & 0x3F)); }
}

std::string utf16(const Buf &b, size_t o, size_t bytes, bool bigEndian) {
    std::string s;
    for (size_t i = 0; i + 1 < bytes; i += 2) {
        uint32_t u = bigEndian ? b.be16(o + i) : b.le16(o + i);
        if (u >= 0xD800 && u < 0xDC00 && i + 3 < bytes) {
            uint32_t lo = bigEndian ? b.be16(o + i + 2) : b.le16(o + i + 2);
            u = 0x10000 + ((u - 0xD800) << 10) + (lo - 0xDC00);
            i += 2;
        }
        if (u == 0) break;
        appendUtf8(s, u);
    }
    return s;
}

// DeviceSQL string (Deep Symmetry "DeviceSQL Strings")
std::string dsqlString(const Buf &b, size_t o) {
    uint8_t k = b.u8(o);
    if (k == 0x40) { // long ASCII
        uint16_t len = b.le16(o + 1);
        if (len < 4 || !b.ok(o + 4, len - 4u)) return {};
        return std::string(reinterpret_cast<const char *>(b.p + o + 4), len - 4u);
    }
    if (k == 0x90) { // long UTF-16LE
        uint16_t len = b.le16(o + 1);
        if (len < 4 || !b.ok(o + 4, len - 4u)) return {};
        return utf16(b, o + 4, len - 4u, false);
    }
    size_t len = size_t(k >> 1);
    if (len < 1 || !b.ok(o + 1, len - 1)) return {};
    return std::string(reinterpret_cast<const char *>(b.p + o + 1), len - 1);
}

enum PageType { T_TRACKS = 0, T_GENRES = 1, T_ARTISTS = 2, T_ALBUMS = 3, T_LABELS = 4, T_KEYS = 5, T_COLORS = 6,
                T_PLAYLIST_TREE = 7, T_PLAYLIST_ENTRIES = 8, T_HISTORY_PLAYLISTS = 11, T_HISTORY_ENTRIES = 12,
                T_ARTWORK = 13 };

// Calls fn(rowBase) for every present row of a table.
template <typename F> void forEachRow(const Buf &b, uint32_t pageLen, uint32_t first, uint32_t last, uint32_t type, F fn) {
    uint32_t idx = first;
    std::vector<bool> seen(b.n / pageLen + 1, false);
    while (true) {
        size_t pg = size_t(idx) * pageLen;
        if (idx >= seen.size() || seen[idx] || !b.ok(pg, pageLen)) break;
        seen[idx] = true;
        if (b.le32(pg + 8) != type) break;
        uint32_t packed = uint32_t(b.u8(pg + 24)) | uint32_t(b.u8(pg + 25)) << 8 | uint32_t(b.u8(pg + 26)) << 16;
        uint32_t numOffsets = packed & 0x1FFF;
        uint8_t flags = b.u8(pg + 27);
        bool dataPage = (flags & 0x40) == 0;
        if (dataPage && numOffsets > 0) {
            uint32_t groups = (numOffsets - 1) / 16 + 1;
            for (uint32_t g = 0; g < groups; ++g) {
                size_t base = pg + pageLen - size_t(g) * 0x24;
                uint16_t present = b.le16(base - 4);
                for (uint32_t r = 0; r < 16 && g * 16 + r < numOffsets; ++r) {
                    if (!((present >> r) & 1)) continue;
                    uint16_t ofs = b.le16(base - (6 + 2 * r));
                    size_t row = pg + 0x28 + ofs;
                    if (row < pg + pageLen) fn(row);
                }
            }
        }
        if (idx == last) break;
        idx = b.le32(pg + 12); // next page
    }
}

std::string nameNearFar(const Buf &b, size_t row, uint16_t subtype, size_t nearOfs, size_t farOfs) {
    size_t o = (subtype & 0x04) ? b.le16(row + farOfs) : b.u8(row + nearOfs);
    return dsqlString(b, row + o);
}

} // namespace

struct djc_library {
    std::vector<djc_lib_track> tracks;
    std::vector<djc_lib_playlist> playlists;
    std::multimap<uint32_t, std::pair<uint32_t, uint32_t>> entries; // playlist -> (index, track)
    std::vector<std::string> strings; // owns every const char* handed out
    std::map<uint32_t, size_t> trackIndex;
    std::string error;
};

extern "C" {

djc_library *djc_lib_open_pdb(const char *path) {
    auto *lib = new djc_library();
    auto data = readFile(path);
    Buf b{data.data(), data.size()};
    if (data.size() < 0x40) { lib->error = "export.pdb missing or too small"; return lib; }
    uint32_t pageLen = b.le32(4), numTables = b.le32(8);
    if (pageLen < 512 || pageLen > 65536 || numTables > 64) { lib->error = "not a DeviceSQL export"; return lib; }

    std::map<uint32_t, std::string> artists, albums, genres, keys, labels, artwork, colors;
    struct RawTrack { djc_lib_track t; std::string s[9]; uint32_t artist, album, genre, key, label, remixer, art, color; };
    std::vector<RawTrack> raw;
    struct RawPl { djc_lib_playlist p; std::string name; };
    std::vector<RawPl> pls;

    for (uint32_t t = 0; t < numTables; ++t) {
        size_t th = 0x1C + size_t(t) * 16;
        uint32_t type = b.le32(th), first = b.le32(th + 8), last = b.le32(th + 12);
        auto simple = [&](std::map<uint32_t, std::string> &m) {
            forEachRow(b, pageLen, first, last, type, [&](size_t row) { m[b.le32(row)] = dsqlString(b, row + 4); });
        };
        switch (type) {
        case T_ARTISTS:
            forEachRow(b, pageLen, first, last, type, [&](size_t row) {
                artists[b.le32(row + 4)] = nameNearFar(b, row, b.le16(row), 9, 0x0A);
            });
            break;
        case T_ALBUMS:
            forEachRow(b, pageLen, first, last, type, [&](size_t row) {
                albums[b.le32(row + 12)] = nameNearFar(b, row, b.le16(row), 21, 0x16);
            });
            break;
        case T_GENRES: simple(genres); break;
        case T_LABELS: simple(labels); break;
        case T_ARTWORK: simple(artwork); break;
        case T_KEYS:
            forEachRow(b, pageLen, first, last, type, [&](size_t row) { keys[b.le32(row)] = dsqlString(b, row + 8); });
            break;
        case T_COLORS:
            forEachRow(b, pageLen, first, last, type, [&](size_t row) { colors[b.le16(row + 5)] = dsqlString(b, row + 8); });
            break;
        case T_TRACKS:
            forEachRow(b, pageLen, first, last, type, [&](size_t row) {
                RawTrack r{};
                r.t.sampleRate = b.le32(row + 8);
                r.t.fileSize = b.le32(row + 16);
                r.art = b.le32(row + 28);
                r.key = b.le32(row + 32);
                r.label = b.le32(row + 40);
                r.remixer = b.le32(row + 44);
                r.t.bitrate = b.le32(row + 48);
                r.t.trackNumber = b.le32(row + 52);
                r.t.bpm = b.le32(row + 56) / 100.0;
                r.genre = b.le32(row + 60);
                r.album = b.le32(row + 64);
                r.artist = b.le32(row + 68);
                r.t.id = b.le32(row + 72);
                r.t.year = b.le16(row + 80);
                r.t.duration = b.le16(row + 84);
                r.color = b.u8(row + 88);
                r.t.rating = b.u8(row + 89);
                auto str = [&](int i) { return dsqlString(b, row + b.le16(row + 94 + 2 * size_t(i))); };
                r.s[0] = str(17); // title
                r.s[1] = str(20); // file path
                r.s[2] = str(14); // analyze path
                r.s[3] = str(12); // mix name
                r.s[4] = str(16); // comment
                r.s[5] = str(10); // date added
                r.s[6] = str(19); // file name
                raw.push_back(std::move(r));
            });
            break;
        case T_PLAYLIST_TREE:
            forEachRow(b, pageLen, first, last, type, [&](size_t row) {
                RawPl p{};
                p.p.parentId = b.le32(row);
                p.p.sortOrder = b.le32(row + 8);
                p.p.id = b.le32(row + 12);
                p.p.isFolder = b.le32(row + 16) != 0;
                p.name = dsqlString(b, row + 20);
                pls.push_back(std::move(p));
            });
            break;
        case T_PLAYLIST_ENTRIES:
            forEachRow(b, pageLen, first, last, type, [&](size_t row) {
                lib->entries.insert({b.le32(row + 8), {b.le32(row), b.le32(row + 4)}});
            });
            break;
        default: break;
        }
    }
    auto get = [](const std::map<uint32_t, std::string> &m, uint32_t k) {
        auto it = m.find(k);
        return it == m.end() ? std::string() : it->second;
    };
    // store strings first, then point into them (reserve so pointers stay valid)
    lib->strings.reserve(raw.size() * 12 + pls.size() + 1);
    auto keep = [&](const std::string &s) { lib->strings.push_back(s); return lib->strings.back().c_str(); };
    for (auto &r : raw) {
        djc_lib_track t = r.t;
        t.title = keep(r.s[0]);
        t.filePath = keep(r.s[1]);
        t.analyzePath = keep(r.s[2]);
        t.mixName = keep(r.s[3]);
        t.comment = keep(r.s[4]);
        t.dateAdded = keep(r.s[5]);
        t.artist = keep(get(artists, r.artist));
        t.album = keep(get(albums, r.album));
        t.genre = keep(get(genres, r.genre));
        t.key = keep(get(keys, r.key));
        t.label = keep(get(labels, r.label));
        t.artworkPath = keep(get(artwork, r.art));
        t.colorId = r.color;
        lib->trackIndex[t.id] = lib->tracks.size();
        lib->tracks.push_back(t);
    }
    for (auto &p : pls) {
        djc_lib_playlist q = p.p;
        q.name = keep(p.name);
        lib->playlists.push_back(q);
    }
    std::sort(lib->playlists.begin(), lib->playlists.end(), [](const djc_lib_playlist &a, const djc_lib_playlist &b) {
        return a.parentId != b.parentId ? a.parentId < b.parentId : a.sortOrder < b.sortOrder;
    });
    if (lib->tracks.empty()) lib->error = "no tracks in export.pdb";
    return lib;
}

void djc_lib_close(djc_library *l) { delete l; }
const char *djc_lib_error(const djc_library *l) { return l->error.c_str(); }
int32_t djc_lib_track_count(const djc_library *l) { return int32_t(l->tracks.size()); }
const djc_lib_track *djc_lib_track_at(const djc_library *l, int32_t i) {
    return (i >= 0 && size_t(i) < l->tracks.size()) ? &l->tracks[size_t(i)] : nullptr;
}
int32_t djc_lib_track_index(const djc_library *l, uint32_t id) {
    auto it = l->trackIndex.find(id);
    return it == l->trackIndex.end() ? -1 : int32_t(it->second);
}
int32_t djc_lib_playlist_count(const djc_library *l) { return int32_t(l->playlists.size()); }
const djc_lib_playlist *djc_lib_playlist_at(const djc_library *l, int32_t i) {
    return (i >= 0 && size_t(i) < l->playlists.size()) ? &l->playlists[size_t(i)] : nullptr;
}
int32_t djc_lib_playlist_tracks(const djc_library *l, uint32_t playlistId, uint32_t *out, int32_t cap) {
    std::vector<std::pair<uint32_t, uint32_t>> v;
    auto r = l->entries.equal_range(playlistId);
    for (auto it = r.first; it != r.second; ++it) v.push_back(it->second);
    std::sort(v.begin(), v.end());
    int32_t n = 0;
    for (auto &e : v) { if (n < cap && out) out[n] = e.second; ++n; }
    return n;
}

} // extern "C"

// ---------------- ANLZ ----------------

struct djc_anlz {
    std::vector<djc_beat> beats;
    std::vector<djc_cue> cues;
    std::vector<std::string> comments;
    std::vector<float> wave[3]; // low, mid, high, 150 per second, 0..1
    int32_t waveSource = 0;     // 7 = PWV7 3-band, 5 = PWV5 colour, 3 = PWV3 mono
    std::vector<float> raw[3][4]; // [0] PWV7, [1] PWV5, [2] PWV3; ch 0 height, 1..3 colour
    struct BasicCue { int32_t hot; uint32_t timeMs; uint32_t status; };
    std::vector<BasicCue> basicStatus; // PCPT status, kept even when PCO2 replaces the basic list
    std::vector<djc_phrase> phrases;
    int32_t mood = 0, endBeat = 0;
    bool haveExtCues = false;
};

namespace {

void parseAnlz(djc_anlz *a, const std::vector<uint8_t> &data) {
    Buf b{data.data(), data.size()};
    if (data.size() < 12 || std::memcmp(data.data(), "PMAI", 4) != 0) return;
    size_t pos = b.be32(4);
    while (pos + 12 <= data.size()) {
        uint32_t tag = b.be32(pos), lenHeader = b.be32(pos + 4), lenTag = b.be32(pos + 8);
        if (lenTag < 12 || !b.ok(pos, lenTag)) break;
        size_t body = pos + 12, end = pos + lenTag;
        (void)lenHeader;
        switch (tag) {
        case 0x5051545a: { // PQTZ beat grid
            if (!a->beats.empty()) break;
            uint32_t n = b.be32(body + 8);
            for (uint32_t i = 0; i < n && body + 12 + i * 8 + 8 <= end; ++i) {
                size_t e = body + 12 + i * 8;
                a->beats.push_back({b.be16(e), b.be16(e + 2), b.be32(e + 4)});
            }
        } break;
        case 0x50434f32: { // PCO2 extended cues (hot cues A-H, colours, comments)
            uint32_t n = b.be16(body + 4);
            size_t e = body + 8;
            if (!a->haveExtCues) { // replace basic cues of the same file family
                a->cues.clear(); a->comments.clear(); a->haveExtCues = true;
            }
            for (uint32_t i = 0; i < n && e + 12 <= end; ++i) {
                if (std::memcmp(b.p + e, "PCP2", 4) != 0) break;
                uint32_t lenEntry = b.be32(e + 8);
                if (lenEntry < 16 || e + lenEntry > end) break;
                djc_cue c{};
                c.hotCue = int32_t(b.be32(e + 12));
                c.type = b.u8(e + 16);
                c.timeMs = b.be32(e + 20);
                c.loopMs = b.be32(e + 24);
                c.colorId = b.u8(e + 28);
                std::string comment;
                uint32_t lenComment = 0;
                if (lenEntry > 43) {
                    lenComment = b.be32(e + 40);
                    if (lenComment && b.ok(e + 44, lenComment)) comment = utf16(b, e + 44, lenComment, true);
                }
                size_t after = e + 44 + lenComment;
                if (lenEntry - lenComment > 47) {
                    c.hasRgb = 1;
                    c.r = b.u8(after + 1); c.g = b.u8(after + 2); c.b = b.u8(after + 3);
                }
                a->comments.push_back(comment);
                a->cues.push_back(c);
                e += lenEntry;
            }
        } break;
        case 0x50434f42: { // PCOB basic cues (list used only if no PCO2 seen; status always kept)
            uint32_t n = b.be16(body + 6);
            size_t e = body + 12;
            for (uint32_t i = 0, f = uint32_t(e); i < n && f + 40 <= end; ++i) {
                if (std::memcmp(b.p + f, "PCPT", 4) != 0) break;
                uint32_t lenEntry = b.be32(f + 8);
                if (lenEntry < 40 || f + lenEntry > end) break;
                a->basicStatus.push_back({int32_t(b.be32(f + 12)), b.be32(f + 32), b.be32(f + 16)});
                f += lenEntry;
            }
            if (a->haveExtCues) break;
            for (uint32_t i = 0; i < n && e + 12 <= end; ++i) {
                if (std::memcmp(b.p + e, "PCPT", 4) != 0) break;
                uint32_t lenEntry = b.be32(e + 8);
                if (lenEntry < 16 || e + lenEntry > end) break;
                djc_cue c{};
                c.hotCue = int32_t(b.be32(e + 12));
                c.type = b.u8(e + 28);
                c.timeMs = b.be32(e + 32);
                c.loopMs = b.be32(e + 36);
                a->comments.emplace_back();
                a->cues.push_back(c);
                e += lenEntry;
            }
        } break;
        case 0x50575637: { // PWV7 3-band scroll: bytes mid, high, low; 150/s
            uint32_t eb = b.be32(body), n = b.be32(body + 4);
            size_t d = body + 12;
            if (eb != 3 || !b.ok(d, size_t(n) * 3)) break;
            for (auto &w : a->wave) w.assign(n, 0.0f);
            for (auto &w : a->raw[0]) w.assign(n, 0.0f);
            for (uint32_t i = 0; i < n; ++i) {
                float mid = b.u8(d + i * 3) / 255.0f, high = b.u8(d + i * 3 + 1) / 255.0f, low = b.u8(d + i * 3 + 2) / 255.0f;
                a->wave[0][i] = low; a->wave[1][i] = mid; a->wave[2][i] = high;
                a->raw[0][0][i] = std::max(low, std::max(mid, high));
                a->raw[0][1][i] = low; a->raw[0][2][i] = mid; a->raw[0][3][i] = high;
            }
            a->waveSource = 7;
        } break;
        case 0x50575635: { // PWV5 colour scroll: 16-bit BE, rrr ggg bbb hhhhh xx; 150/s
            uint32_t eb = b.be32(body), n = b.be32(body + 4);
            size_t d = body + 12;
            if (eb != 2 || !b.ok(d, size_t(n) * 2)) break;
            for (auto &w : a->raw[1]) w.assign(n, 0.0f);
            for (uint32_t i = 0; i < n; ++i) {
                uint16_t v = b.be16(d + i * 2);
                a->raw[1][0][i] = ((v >> 2) & 0x1F) / 31.0f;
                a->raw[1][1][i] = ((v >> 13) & 7) / 7.0f;
                a->raw[1][2][i] = ((v >> 10) & 7) / 7.0f;
                a->raw[1][3][i] = ((v >> 7) & 7) / 7.0f;
            }
            if (a->waveSource >= 5) break;
            for (auto &w : a->wave) w.assign(n, 0.0f);
            for (uint32_t i = 0; i < n; ++i) {
                uint16_t v = b.be16(d + i * 2);
                float h = ((v >> 2) & 0x1F) / 31.0f;
                float red = ((v >> 13) & 7) / 7.0f, green = ((v >> 10) & 7) / 7.0f, blue = ((v >> 7) & 7) / 7.0f;
                float sum = red + green + blue + 1e-6f;
                // rekordbox colours: low = red-ish, mid = green, high = blue. Split height by colour weight.
                a->wave[0][i] = h * std::min(1.0f, red / sum * 1.6f);
                a->wave[1][i] = h * std::min(1.0f, green / sum * 1.6f);
                a->wave[2][i] = h * std::min(1.0f, blue / sum * 1.6f);
            }
            a->waveSource = 5;
        } break;
        case 0x50575633: { // PWV3 mono scroll: 1 byte, height = low 5 bits, whiteness = high 3 bits
            uint32_t eb = b.be32(body), n = b.be32(body + 4);
            size_t d = body + 12;
            if (eb != 1 || !b.ok(d, n)) break;
            for (auto &w : a->raw[2]) w.assign(n, 0.0f);
            for (uint32_t i = 0; i < n; ++i) {
                a->raw[2][0][i] = (b.u8(d + i) & 0x1F) / 31.0f;
                a->raw[2][1][i] = (b.u8(d + i) >> 5) / 7.0f;
            }
            if (a->waveSource >= 3) break;
            for (auto &w : a->wave) w.assign(n, 0.0f);
            for (uint32_t i = 0; i < n; ++i) {
                float h = (b.u8(d + i) & 0x1F) / 31.0f;
                a->wave[0][i] = h; a->wave[1][i] = h * 0.6f; a->wave[2][i] = h * 0.3f;
            }
            a->waveSource = 3;
        } break;
        case 0x50535349: { // PSSI phrases, possibly XOR masked
            uint32_t eb = b.be32(body);
            uint16_t n = b.be16(body + 4);
            size_t d = body + 6;
            size_t len = end - d;
            if (eb != 24 || len < 12) break;
            std::vector<uint8_t> x(b.p + d, b.p + end);
            uint16_t rawMood = uint16_t(x[0] << 8 | x[1]);
            if (rawMood > 20) {
                static const uint8_t base[19] = {0xCB, 0xE1, 0xEE, 0xFA, 0xE5, 0xEE, 0xAD, 0xEE, 0xE9, 0xD2,
                                                 0xE9, 0xEB, 0xE1, 0xE9, 0xF3, 0xE8, 0xE9, 0xF4, 0xE1};
                for (size_t i = 0; i < x.size(); ++i) x[i] ^= uint8_t(base[i % 19] + uint8_t(n));
            }
            Buf p{x.data(), x.size()};
            a->mood = p.be16(0);
            a->endBeat = p.be16(8);
            for (uint16_t i = 0; i < n; ++i) {
                size_t e = 14 + size_t(i) * 24; // mood, 6, end_beat, 2, bank, 1, then entries
                if (!p.ok(e, 24)) break;
                djc_phrase ph{};
                ph.index = p.be16(e);
                ph.beat = p.be16(e + 2);
                ph.kind = p.be16(e + 4);
                ph.fill = p.u8(e + 21);
                ph.beatFill = p.be16(e + 22);
                a->phrases.push_back(ph);
            }
        } break;
        default: break;
        }
        pos += lenTag;
    }
}

} // namespace

extern "C" {

djc_anlz *djc_anlz_open(const char *datPath) {
    auto *a = new djc_anlz();
    std::string base(datPath ? datPath : "");
    std::string stem = base.size() > 4 ? base.substr(0, base.size() - 4) : base;
    // .2EX first (3-band waveform), then .EXT (colour waveform, PCO2, phrases), then .DAT (grid, basic cues)
    for (const char *ext : {".2EX", ".EXT", ".DAT"}) {
        auto data = readFile((stem + ext).c_str());
        if (!data.empty()) parseAnlz(a, data);
    }
    // active loop flag lives only in PCPT (status 4, crate-digger cue_entry_status)
    for (auto &c : a->cues)
        for (auto &s : a->basicStatus)
            if (s.hot == c.hotCue && s.timeMs == c.timeMs && s.status == 4) c.active = 1;
    return a;
}

void djc_anlz_close(djc_anlz *a) { delete a; }
int32_t djc_anlz_beat_count(const djc_anlz *a) { return int32_t(a->beats.size()); }
const djc_beat *djc_anlz_beats(const djc_anlz *a) { return a->beats.empty() ? nullptr : a->beats.data(); }
int32_t djc_anlz_cue_count(const djc_anlz *a) { return int32_t(a->cues.size()); }
const djc_cue *djc_anlz_cue_at(const djc_anlz *a, int32_t i) {
    return (i >= 0 && size_t(i) < a->cues.size()) ? &a->cues[size_t(i)] : nullptr;
}
const char *djc_anlz_cue_comment(const djc_anlz *a, int32_t i) {
    return (i >= 0 && size_t(i) < a->comments.size()) ? a->comments[size_t(i)].c_str() : "";
}
int32_t djc_anlz_wave_count(const djc_anlz *a) { return int32_t(a->wave[0].size()); }
int32_t djc_anlz_wave_source(const djc_anlz *a) { return a->waveSource; }
const float *djc_anlz_wave_band(const djc_anlz *a, int32_t band) {
    return (band >= 0 && band < 3 && !a->wave[band].empty()) ? a->wave[band].data() : nullptr;
}
int32_t djc_anlz_wave_len(const djc_anlz *a, int32_t kind) {
    int k = kind == 7 ? 0 : kind == 5 ? 1 : kind == 3 ? 2 : -1;
    return k < 0 ? 0 : int32_t(a->raw[k][0].size());
}
const float *djc_anlz_wave_data(const djc_anlz *a, int32_t kind, int32_t ch) {
    int k = kind == 7 ? 0 : kind == 5 ? 1 : kind == 3 ? 2 : -1;
    if (k < 0 || ch < 0 || ch > 3 || a->raw[k][ch].empty()) return nullptr;
    return a->raw[k][ch].data();
}
int32_t djc_anlz_phrase_count(const djc_anlz *a) { return int32_t(a->phrases.size()); }
const djc_phrase *djc_anlz_phrases(const djc_anlz *a) { return a->phrases.empty() ? nullptr : a->phrases.data(); }
int32_t djc_anlz_mood(const djc_anlz *a) { return a->mood; }
int32_t djc_anlz_end_beat(const djc_anlz *a) { return a->endBeat; }

} // extern "C"
