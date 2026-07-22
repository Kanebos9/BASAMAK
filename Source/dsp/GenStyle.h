#pragma once
// ============================================================================
// GenStyle [2026-07-22 r22] - STYLES AS DATA (the MMA lesson, GENERATE-THEORY v3/v5).
//
// FOR NEW READERS: every GENERATE style - the drum-kit DNA (D1-D10), the mined
// calibration constants, AND the melodic/bass/comp genre vocabulary - is ONE
// text format now, parsed at startup. The 8 factory styles are EMBEDDED here as
// strings in that SAME format (single source of truth: what a user can write in
// ~/Documents/BASAMAK/Styles/*.basamakstyle is exactly what the factory ships).
// DrumGen + PartGen consume the parsed tables; a style file only changes WHAT
// the seeds pick from - generation stays fully deterministic.
//
// FORMAT (docs/STYLES.md is the user-facing spec; cells are 1-based 16ths):
//   style "Name"                      required first statement
//   swing 54                          50 = straight .. 75 = max
//   kick.canon 1 5 9 13               immutable kick cells (D1)
//   kick.opt 7 8 11                   optional cells the seed picks from
//   kick.optn 2                       how many optional cells land (cap 2 - D1)
//   snare.cells 5 13                  the backbeat scheme (D2)
//   hat.tier offbeat|8ths|16ths       hat language (D3)
//   hat.rolls 8 16?                   ratchet cells (D5); '?' = 50% chance
//   openhat all|sparse                D4: every 8th offbeat vs 1-2 picked
//   ghosts 2                          ghost snares per bar at Medium (D6)
//   ghost.ratio 0.22                  mined ghost-vs-backbeat velocity ratio
//   fill auto|crescendo|double|hatroll  D8 fill preference
//   vel.kick 112 30                   mean MIDI velocity + sd (jitter) per role
//   vel.snare 107 8
//   vel.hat 64 25
//   accent v0 .. v15                  mined per-16th mean velocity (MIDI)
//   micro.kick 0                      push/drag ms per role (negative = early);
//   micro.snare 2.7                   the kit writer anchors the KICK to the
//   micro.hat -0.9                    grid (G5: the bass may never lead it)
//   micro.ohat -2.4 / micro.perc 0 / micro.bass 4
//   bass W : pos dur deg vel | ...    a weighted bass cell; deg = R|3|5|7|O|L|D|A
//                                     (Root, 3rd, 5th, 7th, Octave up, octave
//                                     beLow, 5th Down, Approach); dur in 16ths;
//                                     vel in MIDI
//   mel W : 1C 4L 7C 11A ; slope 0 3  a weighted melody cell: pos+category
//                                     (C chord tone, L color/scale, A approach,
//                                     R rest) + the cell's pitch slope range
//   comp W : 3 7 11 15                a weighted chord-stab rhythm template (H3)
//   prog W : 1 5 6 4                  a weighted progression (1-based scale degrees);
//                                     the POOL replaces the stock fallback - New idea
//                                     rerolls the pick (originality mandate r22)
//   hook 2                            phrasing: the hook repeats every N bars
//   mel.density 1.0                   topline onset-budget multiplier
// Unknown keys are IGNORED (forward compatibility); malformed values on known
// keys FAIL the parse - the caller skips the file with a readout note, never a
// crash. Parsing is message-thread only; std-only (headless-testable, no JUCE).
// ============================================================================

#include <cstdint>
#include <cstdlib>
#include <cmath>
#include <cstring>
#include <string>
#include <vector>
#include <algorithm>

namespace GenStyle
{
// bass degree codes (BassEv::code): scale-degree indices for R/3/5/7, specials negative-ish
enum { DegRoot = 0, DegThird = 2, DegFifth = 4, DegSeventh = 6,
       DegOct = 7, DegOctDown = -3, DegFifthDown = -2, DegApproach = -1 };

struct BassEv   { int pos16 = 0; int dur16 = 1; int code = DegRoot; int vel = 100; };
struct BassCell { float w = 1.0f; std::vector<BassEv> ev; };
struct MelEv    { int pos16 = 0; char cat = 'L'; };            // C | L | A (R dropped at parse)
struct MelCell  { float w = 1.0f; std::vector<MelEv> ev; int slopeLo = 0, slopeHi = 0; };
struct CompCell { float w = 1.0f; std::vector<int> pos16; };
struct ProgCell { float w = 1.0f; std::vector<int> degs; };    // 0-based scale degrees per bar

struct Style
{
    std::string name;
    bool  user = false;              // came from ~/Documents/BASAMAK/Styles (tagged in the picker)
    float swingPct = 50.0f;
    // ---- drum DNA (D1-D10 + the calibration constants) ----
    uint16_t kickCanon = 1, kickOpt = 0;  int kickOptN = 0;
    uint16_t snareCells = 0;
    int      hatTier = 1;                              // 0 offbeat | 1 8ths | 2 16ths
    uint16_t hatRollAlways = 0, hatRollMaybe = 0;      // D5 ratchet cells ('?' = 50%)
    bool     openHatAll = false;                       // D4: all 8th offbeats vs sparse
    int      ghostBudget = 0;  float ghostRatio = 0.25f;
    int      fillVariant = -1;                         // -1 auto | 0 crescendo | 1 double | 2 hatroll
    float    kickVel = 0.80f, snareVel = 0.84f, hatVel = 0.45f;   // 0..1 (MIDI/127 in the file)
    float    sdKick = 30.0f, sdSnare = 8.0f, sdHat = 25.0f;       // MIDI-sd jitter widths
    uint8_t  accent[16] = { 63, 40, 41, 64, 89, 49, 40, 50, 82, 40, 41, 57, 90, 68, 53, 56 };
    float    microKick = 0.0f, microSnare = 0.0f, microHat = 0.0f,
             microOHat = 0.0f, microPerc = 0.0f, microBass = 0.0f;   // ms; negative = early
    // ---- melodic DNA (r22 STAGE 2: the style speaks for EVERY role) ----
    std::vector<BassCell> bass;
    std::vector<MelCell>  mel;
    std::vector<CompCell> comp;
    std::vector<ProgCell> progs;   // [r22 originality] the progression POOL New idea rerolls from
    int   hookBars = 2;
    float melDensity = 1.0f;
};

// ---------------------------------------------------------------------------------- the parser
namespace parsedetail
{
inline void trim(std::string& s)
{
    size_t a = 0, b = s.size();
    while (a < b && (s[a] == ' ' || s[a] == '\t' || s[a] == '\r')) ++a;
    while (b > a && (s[b - 1] == ' ' || s[b - 1] == '\t' || s[b - 1] == '\r')) --b;
    s = s.substr(a, b - a);
}
inline std::string lower(std::string s)
{ for (auto& ch : s) if (ch >= 'A' && ch <= 'Z') ch = (char) (ch - 'A' + 'a'); return s; }
inline bool num(const std::string& t, double& out)
{
    if (t.empty()) return false;
    char* end = nullptr;
    out = std::strtod(t.c_str(), &end);
    return end != nullptr && *end == '\0';
}
inline void split(const std::string& s, std::vector<std::string>& out)
{
    out.clear();
    std::string cur;
    for (char ch : s)
    {
        if (ch == ' ' || ch == '\t') { if (! cur.empty()) { out.push_back(cur); cur.clear(); } }
        else cur.push_back(ch);
    }
    if (! cur.empty()) out.push_back(cur);
}
inline bool cellMask(const std::vector<std::string>& toks, size_t from, uint16_t& mask, std::string& err)
{
    mask = 0;
    for (size_t i = from; i < toks.size(); ++i)
    {
        double v;
        if (! num(toks[i], v) || v < 1.0 || v > 16.0 || v != std::floor(v))
        { err = "bad cell '" + toks[i] + "' (want 1..16)"; return false; }
        mask = (uint16_t) (mask | (1u << ((int) v - 1)));
    }
    return true;
}
} // namespace parsedetail

// Parse one style text. Returns false + err ("line N: what") on any malformed KNOWN key;
// unknown keys are ignored (forward compatibility). Never throws, never crashes.
inline bool parse(const char* text, Style& out, std::string& err)
{
    using namespace parsedetail;
    out = Style();
    err.clear();
    if (text == nullptr) { err = "empty text"; return false; }
    std::string all(text);
    size_t pos = 0;
    int lineNo = 0;
    bool named = false;
    auto fail = [&](const std::string& what)
    { err = "line " + std::to_string(lineNo) + ": " + what; return false; };
    while (pos <= all.size())
    {
        size_t nl = all.find('\n', pos);
        std::string line = all.substr(pos, nl == std::string::npos ? std::string::npos : nl - pos);
        pos = nl == std::string::npos ? all.size() + 1 : nl + 1;
        ++lineNo;
        const size_t hash = line.find('#');
        if (hash != std::string::npos) line = line.substr(0, hash);
        trim(line);
        if (line.empty()) continue;
        std::vector<std::string> toks; split(line, toks);
        const std::string key = lower(toks[0]);
        auto oneNum = [&](double lo, double hi, double& v) -> bool
        {
            if (toks.size() < 2 || ! num(toks[1], v) || v < lo || v > hi) return false;
            return true;
        };
        if (key == "style")
        {   // style "Name with spaces" (quotes optional for single words)
            const size_t q1 = line.find('"');
            std::string nm;
            if (q1 != std::string::npos)
            {
                const size_t q2 = line.find('"', q1 + 1);
                if (q2 == std::string::npos) return fail("unterminated style name quote");
                nm = line.substr(q1 + 1, q2 - q1 - 1);
            }
            else if (toks.size() >= 2) nm = toks[1];
            trim(nm);
            if (nm.empty()) return fail("style needs a name");
            out.name = nm; named = true;
        }
        else if (key == "swing")
        { double v; if (! oneNum(40.0, 80.0, v)) return fail("swing wants 40..80"); out.swingPct = (float) v; }
        else if (key == "kick.canon")
        { if (! cellMask(toks, 1, out.kickCanon, err)) return fail(err); }
        else if (key == "kick.opt")
        { if (! cellMask(toks, 1, out.kickOpt, err)) return fail(err); }
        else if (key == "kick.optn")
        { double v; if (! oneNum(0.0, 2.0, v)) return fail("kick.optn wants 0..2"); out.kickOptN = (int) v; }
        else if (key == "snare.cells")
        { if (! cellMask(toks, 1, out.snareCells, err)) return fail(err); }
        else if (key == "hat.tier")
        {
            if (toks.size() < 2) return fail("hat.tier wants offbeat|8ths|16ths");
            const std::string t = lower(toks[1]);
            if      (t == "offbeat") out.hatTier = 0;
            else if (t == "8ths")    out.hatTier = 1;
            else if (t == "16ths")   out.hatTier = 2;
            else return fail("hat.tier wants offbeat|8ths|16ths");
        }
        else if (key == "hat.rolls")
        {
            out.hatRollAlways = out.hatRollMaybe = 0;
            for (size_t i = 1; i < toks.size(); ++i)
            {
                std::string t = toks[i];
                const bool maybe = ! t.empty() && t.back() == '?';
                if (maybe) t.pop_back();
                double v;
                if (! num(t, v) || v < 1.0 || v > 16.0 || v != std::floor(v))
                    return fail("bad hat.rolls cell '" + toks[i] + "'");
                const uint16_t bit = (uint16_t) (1u << ((int) v - 1));
                if (maybe) out.hatRollMaybe = (uint16_t) (out.hatRollMaybe | bit);
                else       out.hatRollAlways = (uint16_t) (out.hatRollAlways | bit);
            }
        }
        else if (key == "openhat")
        {
            if (toks.size() < 2) return fail("openhat wants all|sparse");
            const std::string t = lower(toks[1]);
            if (t == "all") out.openHatAll = true;
            else if (t == "sparse") out.openHatAll = false;
            else return fail("openhat wants all|sparse");
        }
        else if (key == "ghosts")
        { double v; if (! oneNum(0.0, 3.0, v)) return fail("ghosts wants 0..3"); out.ghostBudget = (int) v; }
        else if (key == "ghost.ratio")
        { double v; if (! oneNum(0.0, 1.0, v)) return fail("ghost.ratio wants 0..1"); out.ghostRatio = (float) v; }
        else if (key == "fill")
        {
            if (toks.size() < 2) return fail("fill wants auto|crescendo|double|hatroll");
            const std::string t = lower(toks[1]);
            if      (t == "auto")      out.fillVariant = -1;
            else if (t == "crescendo") out.fillVariant = 0;
            else if (t == "double")    out.fillVariant = 1;
            else if (t == "hatroll")   out.fillVariant = 2;
            else return fail("fill wants auto|crescendo|double|hatroll");
        }
        else if (key == "vel.kick" || key == "vel.snare" || key == "vel.hat")
        {
            double m, sd = 0.0;
            if (toks.size() < 2 || ! num(toks[1], m) || m < 1.0 || m > 127.0)
                return fail(key + " wants <midi 1..127> [sd]");
            if (toks.size() >= 3 && (! num(toks[2], sd) || sd < 0.0 || sd > 64.0))
                return fail(key + " sd wants 0..64");
            const float mean = (float) (m / 127.0);
            if (key == "vel.kick")  { out.kickVel = mean;  if (toks.size() >= 3) out.sdKick  = (float) sd; }
            if (key == "vel.snare") { out.snareVel = mean; if (toks.size() >= 3) out.sdSnare = (float) sd; }
            if (key == "vel.hat")   { out.hatVel = mean;   if (toks.size() >= 3) out.sdHat   = (float) sd; }
        }
        else if (key == "accent")
        {
            if (toks.size() != 17) return fail("accent wants exactly 16 values");
            for (int i = 0; i < 16; ++i)
            {
                double v;
                if (! num(toks[(size_t) i + 1], v) || v < 0.0 || v > 127.0)
                    return fail("bad accent value '" + toks[(size_t) i + 1] + "'");
                out.accent[i] = (uint8_t) v;
            }
        }
        else if (key.rfind("micro.", 0) == 0)
        {
            double v;
            if (! oneNum(-30.0, 30.0, v)) return fail(key + " wants -30..30 ms");
            const std::string role = key.substr(6);
            if      (role == "kick")  out.microKick  = (float) v;
            else if (role == "snare") out.microSnare = (float) v;
            else if (role == "hat")   out.microHat   = (float) v;
            else if (role == "ohat")  out.microOHat  = (float) v;
            else if (role == "perc")  out.microPerc  = (float) v;
            else if (role == "bass")  out.microBass  = (float) v;
            // unknown micro role: ignored (forward compat)
        }
        else if (key == "bass")
        {   // bass W : pos dur deg vel | pos dur deg vel | ...
            const size_t colon = line.find(':');
            if (colon == std::string::npos) return fail("bass wants 'bass W : pos dur deg vel | ...'");
            BassCell cell;
            { std::vector<std::string> head; split(line.substr(0, colon), head);
              double w;
              if (head.size() < 2 || ! num(head[1], w) || w <= 0.0 || w > 99.0)
                  return fail("bass weight wants 0..99");
              cell.w = (float) w; }
            std::string body = line.substr(colon + 1);
            size_t bp = 0;
            while (bp <= body.size())
            {
                size_t bar = body.find('|', bp);
                std::string seg = body.substr(bp, bar == std::string::npos ? std::string::npos : bar - bp);
                bp = bar == std::string::npos ? body.size() + 1 : bar + 1;
                trim(seg);
                if (seg.empty()) continue;
                std::vector<std::string> et; split(seg, et);
                double p, d, v;
                if (et.size() != 4 || ! num(et[0], p) || p < 1.0 || p > 16.0
                    || ! num(et[1], d) || d < 1.0 || d > 16.0
                    || ! num(et[3], v) || v < 1.0 || v > 127.0)
                    return fail("bass event wants 'pos dur deg vel' (got '" + seg + "')");
                BassEv e; e.pos16 = (int) p - 1; e.dur16 = (int) d; e.vel = (int) v;
                const std::string dg = lower(et[2]);
                if      (dg == "r") e.code = DegRoot;
                else if (dg == "3") e.code = DegThird;
                else if (dg == "5") e.code = DegFifth;
                else if (dg == "7") e.code = DegSeventh;
                else if (dg == "o") e.code = DegOct;
                else if (dg == "l") e.code = DegOctDown;
                else if (dg == "d") e.code = DegFifthDown;
                else if (dg == "a") e.code = DegApproach;
                else return fail("bass degree wants R|3|5|7|O|L|D|A (got '" + et[2] + "')");
                cell.ev.push_back(e);
            }
            if (cell.ev.empty()) return fail("bass cell has no events");
            out.bass.push_back(cell);
        }
        else if (key == "mel")
        {   // mel W : 1C 4L 7C 11A ; slope lo hi
            const size_t colon = line.find(':');
            if (colon == std::string::npos) return fail("mel wants 'mel W : 1C 4L ... ; slope lo hi'");
            MelCell cell;
            { std::vector<std::string> head; split(line.substr(0, colon), head);
              double w;
              if (head.size() < 2 || ! num(head[1], w) || w <= 0.0 || w > 99.0)
                  return fail("mel weight wants 0..99");
              cell.w = (float) w; }
            std::string body = line.substr(colon + 1);
            const size_t semi = body.find(';');
            std::string evPart = body.substr(0, semi == std::string::npos ? std::string::npos : semi);
            std::vector<std::string> et; split(evPart, et);
            for (auto& t : et)
            {
                if (t.size() < 2) return fail("mel event wants '<pos><C|L|A|R>' (got '" + t + "')");
                const char cat = (char) std::toupper((unsigned char) t.back());
                if (cat != 'C' && cat != 'L' && cat != 'A' && cat != 'R')
                    return fail("mel category wants C|L|A|R (got '" + t + "')");
                double p;
                if (! num(t.substr(0, t.size() - 1), p) || p < 1.0 || p > 16.0)
                    return fail("mel position wants 1..16 (got '" + t + "')");
                if (cat == 'R') continue;                        // a rest = no onset (authoring aid)
                MelEv e; e.pos16 = (int) p - 1; e.cat = cat;
                cell.ev.push_back(e);
            }
            if (semi != std::string::npos)
            {
                std::vector<std::string> st; split(body.substr(semi + 1), st);
                double lo, hi;
                if (st.size() != 3 || lower(st[0]) != "slope"
                    || ! num(st[1], lo) || ! num(st[2], hi) || lo > hi
                    || lo < -24.0 || hi > 24.0)
                    return fail("mel slope wants 'slope lo hi' within -24..24");
                cell.slopeLo = (int) lo; cell.slopeHi = (int) hi;
            }
            if (cell.ev.empty()) return fail("mel cell has no onsets");
            out.mel.push_back(cell);
        }
        else if (key == "comp")
        {   // comp W : pos pos ...
            const size_t colon = line.find(':');
            if (colon == std::string::npos) return fail("comp wants 'comp W : pos pos ...'");
            CompCell cell;
            { std::vector<std::string> head; split(line.substr(0, colon), head);
              double w;
              if (head.size() < 2 || ! num(head[1], w) || w <= 0.0 || w > 99.0)
                  return fail("comp weight wants 0..99");
              cell.w = (float) w; }
            std::vector<std::string> et; split(line.substr(colon + 1), et);
            for (auto& t : et)
            {
                double p;
                if (! num(t, p) || p < 1.0 || p > 16.0)
                    return fail("comp position wants 1..16 (got '" + t + "')");
                cell.pos16.push_back((int) p - 1);
            }
            if (cell.pos16.empty()) return fail("comp cell has no positions");
            out.comp.push_back(cell);
        }
        else if (key == "prog")
        {   // prog W : 1 5 6 4  (1-based scale degrees, one per bar of the loop)
            const size_t colon = line.find(':');
            if (colon == std::string::npos) return fail("prog wants 'prog W : deg deg ...'");
            ProgCell cell;
            { std::vector<std::string> head; split(line.substr(0, colon), head);
              double w;
              if (head.size() < 2 || ! num(head[1], w) || w <= 0.0 || w > 99.0)
                  return fail("prog weight wants 0..99");
              cell.w = (float) w; }
            std::vector<std::string> et; split(line.substr(colon + 1), et);
            for (auto& t : et)
            {
                double v;
                if (! num(t, v) || v < 1.0 || v > 7.0 || v != std::floor(v))
                    return fail("prog degree wants 1..7 (got '" + t + "')");
                cell.degs.push_back((int) v - 1);
            }
            if (cell.degs.empty()) return fail("prog has no degrees");
            out.progs.push_back(cell);
        }
        else if (key == "hook")
        { double v; if (! oneNum(1.0, 8.0, v)) return fail("hook wants 1..8 bars"); out.hookBars = (int) v; }
        else if (key == "mel.density")
        { double v; if (! oneNum(0.25, 3.0, v)) return fail("mel.density wants 0.25..3"); out.melDensity = (float) v; }
        // anything else: an unknown key - IGNORED (forward compatibility)
    }
    if (! named) { err = "no 'style \"Name\"' line"; return false; }
    return true;
}

// ------------------------------------------------------------------- the 8 FACTORY styles
// The SAME text format users write - the single source of truth (GENERATE-THEORY v5).
// Drum rows = the r20 DNA + docs/generate-calibration.md; melodic rows = the v5 addendum's
// researched genre vocabulary (bass cells / topline cells / comp templates per genre).
inline const char* const kFactoryText[8] = {
// -------- House (offbeat octave bass, offbeat piano stabs, 2-bar hooks) --------
R"(style "House"
swing 54
kick.canon 1 5 9 13
kick.optn 0
snare.cells 5 13
hat.tier offbeat
openhat all
ghosts 0
ghost.ratio 0.25
vel.kick 112 30
vel.snare 107 8
vel.hat 64 25
accent 63 40 41 64 89 49 40 50 82 40 41 57 90 68 53 56
micro.kick 0
micro.snare 2.7
micro.hat -0.9
micro.ohat -2.4
micro.bass 4
bass 3 : 3 1 O 98 | 7 1 O 94 | 11 1 O 98 | 15 1 A 92
bass 2 : 3 2 R 98 | 7 2 R 94 | 11 2 R 98 | 15 1 O 92
bass 1 : 1 1 R 96 | 3 1 O 92 | 5 1 R 96 | 7 1 O 92 | 9 1 R 96 | 11 1 O 92 | 13 1 R 96 | 15 1 O 92
bass 1 : 3 3 R 98 | 9 2 5 92 | 11 3 R 98 | 15 1 A 90
mel 2 : 1C 4L 7C 11L ; slope 0 3
mel 1 : 3C 7L 9C 15A ; slope -3 0
mel 2 : 3C 5L 7C 11L 15A ; slope 0 4
mel 1 : 1C 3L 4C 9L 11C ; slope -2 2
mel 1 : 2C 7L 10C 15L ; slope 2 5
mel 1 : 1C 8L 11C ; slope -4 -1
comp 3 : 3 7 11 15
comp 1 : 4 7 11 14
comp 1 : 3
comp 1 : 3 7 12 15
prog 3 : 1 6 4 5
prog 2 : 6 4 1 5
prog 1 : 1 4 6 5
prog 1 : 2 5 1 4
hook 2
mel.density 1.0
)",
// -------- Techno (rolling 16th one-note bass off the kick, one-stab comp, hypnotic loop) --------
R"(style "Techno"
swing 50
kick.canon 1 5 9 13
kick.optn 0
snare.cells 5 13
hat.tier offbeat
openhat all
ghosts 0
ghost.ratio 0.25
vel.kick 114 30
vel.snare 102 8
vel.hat 66 25
accent 63 40 41 64 89 49 40 50 82 40 41 57 90 68 53 56
micro.kick 0
micro.snare 0
micro.hat -1
micro.ohat -2
micro.bass 2
bass 3 : 2 1 R 74 | 3 1 R 100 | 4 1 R 70 | 6 1 R 74 | 7 1 R 100 | 8 1 R 72 | 10 1 R 76 | 11 1 R 100 | 12 1 R 70 | 14 1 R 74 | 15 1 R 100 | 16 1 R 72
bass 2 : 3 1 R 100 | 7 1 R 96 | 11 1 R 100 | 15 1 R 96
bass 1 : 3 1 R 100 | 4 1 R 74 | 7 1 R 100 | 8 1 R 74 | 11 1 R 100 | 12 1 R 74 | 15 1 R 100 | 16 1 R 74
bass 1 : 3 2 R 100 | 7 1 7 84 | 11 2 R 100 | 15 1 5 84
mel 2 : 3C 7L 11C ; slope 0 0
mel 1 : 4C 11L ; slope 0 2
mel 2 : 3C 7C 11C 15L ; slope 0 0
mel 1 : 2C 8L 14C ; slope -2 0
mel 1 : 4C 12L ; slope 0 0
mel 1 : 3C 6L 10C 14L ; slope 0 2
comp 2 : 3
comp 1 : 4 7 10 15
comp 1 : 7
comp 1 : 11
prog 3 : 1 1 1 1
prog 2 : 1 6 1 6
prog 1 : 1 7 1 7
prog 1 : 1 4 1 4
hook 1
mel.density 0.7
)",
// -------- Boom-bap (two-beat root bass answering the kick, loop-anchor comp) --------
R"(style "Boom-bap"
swing 60
kick.canon 1
kick.opt 7 8 11 15
kick.optn 2
snare.cells 5 13
hat.tier 8ths
openhat sparse
ghosts 2
ghost.ratio 0.22
vel.kick 97 27
vel.snare 109 12
vel.hat 51 29
accent 76 44 61 47 104 48 53 44 72 47 66 52 97 58 70 49
micro.kick -1.2
micro.snare 4.2
micro.hat -1.7
micro.ohat 0
micro.bass 6
bass 3 : 1 5 R 105 | 8 4 5 96 | 15 1 A 86
bass 2 : 1 4 R 104 | 11 3 R 98 | 15 2 5 90
bass 2 : 1 4 R 106 | 8 2 5 94 | 11 3 R 100 | 15 1 A 88
bass 1 : 1 6 R 104 | 9 2 R 98 | 11 4 7 94
mel 2 : 1C 6L 8A 11C ; slope -4 0
mel 1 : 4C 7L 12C ; slope -2 2
mel 2 : 1C 4L 8C 11L ; slope -3 0
mel 1 : 3C 6L 11C 14A ; slope -2 1
mel 1 : 1C 8L 12C ; slope 0 3
mel 1 : 2C 6C 9L 13C ; slope -4 -1
comp 2 : 1
comp 1 : 1 13 15
comp 1 : 1 5 13
comp 1 : 1 8
prog 3 : 1 4 1 4
prog 2 : 2 5 1 1
prog 1 : 1 6 2 5
prog 1 : 1 7 6 7
hook 2
mel.density 0.9
)",
// -------- Trap (808 copies the kick 1/8/11, sustains + glide, held-pad comp, sparse topline) --------
R"(style "Trap"
swing 50
kick.canon 1
kick.opt 4 7 8 11 12 15
kick.optn 2
snare.cells 9
hat.tier 16ths
hat.rolls 8 16?
openhat sparse
ghosts 1
ghost.ratio 0.22
fill hatroll
vel.kick 108 27
vel.snare 114 12
vel.hat 53 29
accent 76 44 61 47 104 48 53 44 72 47 66 52 97 58 70 49
micro.kick -1.2
micro.snare 4.2
micro.hat -1.7
micro.ohat 0
micro.bass 0
bass 3 : 1 7 R 110 | 8 3 R 104 | 11 6 L 106
bass 2 : 1 6 R 110 | 7 2 R 104 | 15 2 A 96
bass 2 : 1 3 R 110 | 4 3 R 104 | 11 4 L 106 | 15 2 A 94
bass 1 : 1 10 R 110 | 13 4 3 100
mel 1 : 1C 5L 11C ; slope -5 0
mel 1 : 3C 9L ; slope -3 0
mel 1 : 1C 7L 13C ; slope -4 0
mel 1 : 5C 11L 15C ; slope -3 1
mel 1 : 1C 9C ; slope -6 -2
mel 1 : 3C 7L 12C ; slope 0 2
comp 3 : 1
comp 1 : 1 9
comp 1 : 1 11
comp 1 : 1 15
prog 3 : 1 6 1 6
prog 2 : 1 6 7 7
prog 1 : 1 4 6 5
prog 1 : 1 3 6 7
hook 2
mel.density 0.55
)",
// -------- DnB (whole-bar reese sustains vs 1+11 two-hit, pad-per-bar comp) --------
R"(style "DnB"
swing 50
kick.canon 1 11
kick.opt 7
kick.optn 1
snare.cells 5 13
hat.tier 8ths
openhat sparse
ghosts 1
ghost.ratio 0.25
vel.kick 108 30
vel.snare 109 12
vel.hat 64 23
accent 62 41 56 46 93 51 48 51 58 50 62 54 87 54 66 46
micro.kick -1.2
micro.snare 3.3
micro.hat -2.6
micro.ohat -3.4
micro.bass 0
bass 3 : 1 16 R 108
bass 2 : 1 10 R 108 | 11 6 R 102
bass 1 : 1 2 R 106 | 6 2 R 100 | 11 2 5 102
bass 1 : 1 8 R 108 | 9 2 7 96 | 11 4 R 104
mel 2 : 1C 4L 7C 12L ; slope 0 4
mel 1 : 2C 7A 9C ; slope -4 0
mel 2 : 1C 5L 9C 13L ; slope 0 3
mel 1 : 3C 7L 11C 15A ; slope -3 0
mel 1 : 1C 6L 11C ; slope 2 5
mel 1 : 4C 9L 12C 15L ; slope -2 2
comp 2 : 1
comp 1 : 4 7 11 15
comp 1 : 1 9
comp 1 : 7 15
prog 3 : 1 6 4 5
prog 2 : 1 4 1 4
prog 1 : 6 7 1 1
prog 1 : 1 5 6 4
hook 2
mel.density 1.0
)",
// -------- Reggaeton (tresillo 3-3-2 bass mirrored per half-bar, dembow-cell comp, dense topline) --------
R"(style "Reggaeton"
swing 50
kick.canon 1 5 9 13
kick.optn 0
snare.cells 4 7 12 15
hat.tier 8ths
openhat sparse
ghosts 0
ghost.ratio 0.25
vel.kick 102 26
vel.snare 99 10
vel.hat 57 24
accent 57 45 62 55 58 48 59 54 53 59 62 55 65 55 61 57
micro.kick 0
micro.snare 0
micro.hat -4.3
micro.ohat 4.2
micro.bass 5
bass 3 : 1 3 R 106 | 4 3 R 100 | 7 2 5 98 | 9 3 R 104 | 12 3 R 100 | 15 2 5 98
bass 2 : 1 8 R 106 | 9 8 R 102
bass 2 : 1 3 R 106 | 4 4 3 98 | 9 3 R 104 | 12 4 5 98
bass 1 : 1 2 R 106 | 4 2 R 100 | 7 2 5 98 | 9 2 R 104 | 12 2 R 100 | 15 2 5 98
mel 2 : 1C 4C 7L 9C 12L 15A ; slope -2 2
mel 1 : 4C 7L 12C 15L ; slope 0 3
mel 2 : 1C 4L 7C 9C 12L 15C ; slope 0 2
mel 1 : 1C 3L 4C 9L 12C 15A ; slope -3 0
mel 1 : 4C 7C 12L 15C ; slope -2 2
mel 1 : 1C 7L 9C 15L ; slope 2 4
comp 3 : 4 7 12 15
comp 1 : 3 7 11 15
comp 1 : 4 7 12
comp 1 : 1 4 7 12 15
prog 3 : 1 6 3 7
prog 2 : 6 4 1 5
prog 1 : 1 7 6 7
prog 1 : 1 4 5 4
hook 1
mel.density 1.2
)",
// -------- Funk (the One + ghosted 16ths, octave pops, chank-accent comp, 1-bar riffs) --------
R"(style "Funk"
swing 54
kick.canon 1
kick.opt 3 8 10 11 14
kick.optn 2
snare.cells 5 13
hat.tier 16ths
openhat sparse
ghosts 3
ghost.ratio 0.27
vel.kick 92 30
vel.snare 110 12
vel.hat 48 23
accent 62 41 56 46 93 51 48 51 58 50 62 54 87 54 66 46
micro.kick -1.2
micro.snare 3.3
micro.hat -2.6
micro.ohat -3.4
micro.bass 5
bass 3 : 1 2 R 112 | 4 1 R 30 | 6 1 R 32 | 7 1 R 104 | 8 1 O 96 | 11 1 R 30 | 13 2 R 106 | 14 1 R 34 | 15 1 7 98
bass 2 : 1 2 R 112 | 6 1 R 30 | 8 1 O 94 | 11 1 5 98 | 14 1 R 32 | 16 1 A 88
bass 2 : 1 2 R 112 | 3 1 O 92 | 7 1 R 100 | 10 1 R 30 | 11 1 5 96 | 13 1 R 104 | 15 1 7 94
bass 1 : 1 1 R 112 | 2 1 R 28 | 4 1 O 90 | 7 1 5 96 | 9 1 R 30 | 11 2 R 102 | 14 1 A 86 | 16 1 R 30
mel 2 : 1C 3L 7C 8A 11C 14L ; slope -2 2
mel 1 : 2C 4L 7C 11A ; slope 0 4
mel 2 : 1C 4L 6C 8L 11C 15A ; slope -2 2
mel 1 : 1C 3C 7L 10C 14L ; slope 0 3
mel 1 : 2C 7L 8C 12L 15C ; slope -3 0
mel 1 : 1C 6L 8A 11C 16L ; slope 0 2
comp 3 : 2 4 7 10 12 15
comp 1 : 5 13 16
comp 1 : 2 4 7 12 14
comp 1 : 1 4 7 11 13
prog 3 : 1 1 4 1
prog 2 : 1 4 1 4
prog 1 : 2 5 2 5
prog 1 : 1 1 1 1
hook 1
mel.density 1.2
)",
// -------- Pop (root quarters vs driving 8ths, pad/ballad comp, hummable 2-bar hooks) --------
R"(style "Pop"
swing 50
kick.canon 1 9
kick.opt 15
kick.optn 1
snare.cells 5 13
hat.tier 8ths
openhat sparse
ghosts 1
ghost.ratio 0.25
vel.kick 94 21
vel.snare 107 12
vel.hat 55 32
accent 63 40 41 64 89 49 40 50 82 40 41 57 90 68 53 56
micro.kick 0
micro.snare 2.7
micro.hat -0.9
micro.ohat -2.4
micro.bass 2
bass 3 : 1 4 R 100 | 5 4 R 96 | 9 4 R 100 | 13 3 R 96 | 16 1 A 88
bass 2 : 1 2 R 100 | 3 2 R 92 | 5 2 R 98 | 7 2 R 92 | 9 2 R 100 | 11 2 R 92 | 13 2 O 98 | 15 2 R 92
bass 2 : 1 4 R 100 | 5 4 5 94 | 9 4 R 100 | 13 4 O 96
bass 1 : 1 6 R 102 | 9 4 R 98 | 13 2 5 92 | 16 1 A 88
mel 2 : 1C 5L 9C 13L ; slope 0 3
mel 1 : 2C 5L 8C 12A ; slope -3 0
mel 2 : 1C 3L 5C 9C 13L ; slope 0 4
mel 1 : 1C 5C 8L 11C 15A ; slope -2 2
mel 1 : 3C 7L 9C 13L ; slope 2 5
mel 1 : 1C 4L 7C 12C ; slope -4 -1
comp 2 : 1 9
comp 1 : 1 3 5 7 9 11 13 15
comp 1 : 1 5 9 13
comp 1 : 1
prog 3 : 1 5 6 4
prog 2 : 6 4 1 5
prog 1 : 1 4 6 5
prog 1 : 1 6 4 5
hook 2
mel.density 1.0
)"
};
static constexpr int NUM_FACTORY = 8;

// ------------------------------------------------------------------------------- the registry
// Message-thread only mutation (editor init / the Style picker's Refresh); generation reads a
// const Style* it holds for exactly one genAction call. Factory entries parse on first use.
inline std::vector<Style>& registry()
{
    static std::vector<Style> r;
    return r;
}
inline void ensureFactory()
{
    auto& r = registry();
    if (! r.empty()) return;
    for (int i = 0; i < NUM_FACTORY; ++i)
    {
        Style s; std::string err;
        if (! parse(kFactoryText[i], s, err)) { s = Style(); s.name = "Style " + std::to_string(i); }
        r.push_back(s);
    }
}
inline int count() { ensureFactory(); return (int) registry().size(); }
inline const Style& at(int idx)
{
    ensureFactory();
    auto& r = registry();
    if (idx < 0 || idx >= (int) r.size()) idx = 0;
    return r[(size_t) idx];
}
inline const Style& factory(int idx)   // DrumGen's index fallback (tests, no-registry callers)
{ return at(idx < 0 || idx >= NUM_FACTORY ? 0 : idx); }

// Add or replace a USER style (name collision = the user's version wins, tagged).
// Returns "" on success, else the parse error - the caller reports + skips (never crashes).
inline std::string addUser(const std::string& text)
{
    ensureFactory();
    Style s; std::string err;
    if (! parse(text.c_str(), s, err)) return err;
    s.user = true;
    auto& r = registry();
    for (auto& e : r)
        if (parsedetail::lower(e.name) == parsedetail::lower(s.name)) { e = s; return {}; }
    r.push_back(s);
    return {};
}
inline void resetToFactory()
{
    registry().clear();
    ensureFactory();
}
} // namespace GenStyle
