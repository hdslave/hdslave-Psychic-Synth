// Loads Psychic.clap as a host does and checks it: description and ports, parameters, state (with the buffer), presets,
// the sequencers against the host's transport (timing to the frame, stop, restart, jumps, a loop that wraps inside a
// block), the same output whatever the block size, the buttons, the bits and their CVs (Bit Flip, Bit Shift, Length,
// Chain Seq, Reset, Advance, Corrupt, Save and Recall, all heard back by the host), EG Trig Mix, a softer trigger in a
// louder one's attack, the quantizer and MIDI transposing (Unquantized too with Run off), the step pads (notes 36-51
// with Run on), Clock 2, the routing (Order, Blend, Fold, Drone, Noise Tone) and bounded output at extreme settings.
// Writes demo renders as WAV files into the output folder.
//   host Psychic.clap OUT_FOLDER
#include "wav.h"
#include "../src/engine.h" // the Decimator alone: its impulse delay is measured below (testLatencyTail)
#include "../src/melodee_screen.h"

#include <clap/clap.h>

#include <algorithm>
#include <cmath>
#include <chrono>
#include <thread>
#include <cstdio>
#include <cstring>
#include <dlfcn.h>
#include <fstream>
#include <iterator>
#include <cstdlib>
#include <functional>
#include <map>
#include <random>
#include <string>
#include <vector>

namespace
{
int failures = 0, checks = 0;
#define CHECK(c) do { ++checks; if (! (c)) { ++failures; std::printf ("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); } } while (0)
bool debug() { return std::getenv ("PSYCHIC_DEBUG") != nullptr; }

const clap_plugin_entry* entry = nullptr;
const clap_plugin_factory* factory = nullptr;

// The host side: log, and the two a preset load tells (counted); the plug-in must cope without the rest.
int paints = 0, wakes = 0, flushes = 0;
int rescans = 0, presetsLoaded = 0, presetErrors = 0;
int tailNotices = 0;
bool offerTail = true; // false: a host without clap.tail (testTailNotices)
const void* hostExtension (const clap_host*, const char* id)
{
    static const melodee_screen_host_t screen { [] (const void*, uint32_t) { ++paints; } };
    if (std::strcmp (id, MELODEE_SCREEN_CLAP_HOST) == 0) return &screen;
    static const clap_host_log log { [] (const clap_host*, clap_log_severity s, const char* m) { std::printf ("  [plug-in log %d] %s\n", (int) s, m); } };
    static const clap_host_params params { [] (const clap_host*, clap_param_rescan_flags f) { rescans += (f & CLAP_PARAM_RESCAN_VALUES) != 0; },
                                           [] (const clap_host*, clap_id, clap_param_clear_flags) {}, [] (const clap_host*) { ++flushes; } };
    static const clap_host_preset_load presetLoad {
        [] (const clap_host*, uint32_t, const char*, const char*, int32_t, const char*) { ++presetErrors; },
        [] (const clap_host*, uint32_t, const char*, const char*) { ++presetsLoaded; },
    };
    if (std::strcmp (id, CLAP_EXT_LOG) == 0) return &log;
    if (std::strcmp (id, CLAP_EXT_PARAMS) == 0) return &params;
    if (std::strcmp (id, CLAP_EXT_PRESET_LOAD) == 0) return &presetLoad;
    static const clap_host_tail tailHost { [] (const clap_host*) { ++tailNotices; } };
    if (offerTail && std::strcmp (id, CLAP_EXT_TAIL) == 0) return &tailHost;
    return nullptr;
}

clap_host host {
    CLAP_VERSION, nullptr, "Psychic test host", "tests", "", "1",
    hostExtension, [] (const clap_host*) {}, [] (const clap_host*) { ++wakes; }, [] (const clap_host*) {},
};

// Input events for one process call, in time order.
struct Events
{
    struct Any { union { clap_event_header header; clap_event_param_value param; clap_event_note note; clap_event_midi midi; }; };
    std::vector<Any> list;
    // Run once when the plug-in first reads event `hookAt`: something the main thread does in the middle of a block.
    std::function<void()> hook;
    uint32_t hookAt = ~0u;
    clap_input_events in { this, [] (const clap_input_events* e) { return (uint32_t) static_cast<const Events*> (e->ctx)->list.size(); },
                           [] (const clap_input_events* e, uint32_t i) -> const clap_event_header* {
                               auto* self = static_cast<Events*> (e->ctx);
                               if (i == self->hookAt && self->hook)
                               {
                                   auto run = std::move (self->hook);
                                   self->hook = nullptr;
                                   run();
                               }
                               return &self->list[i].header;
                           } };
    void param (uint32_t time, clap_id id, double value)
    {
        Any a {};
        a.param = { { sizeof (clap_event_param_value), time, CLAP_CORE_EVENT_SPACE_ID, CLAP_EVENT_PARAM_VALUE, 0 }, id, nullptr, -1, -1, -1, -1, value };
        list.push_back (a);
    }
    void note (uint32_t time, uint16_t type, int16_t key, double velocity)
    {
        Any a {};
        a.note = { { sizeof (clap_event_note), time, CLAP_CORE_EVENT_SPACE_ID, type, 0 }, -1, 0, 0, key, velocity };
        list.push_back (a);
    }
    void midi (uint32_t time, uint8_t status, uint8_t a1, uint8_t a2)
    {
        Any a {};
        a.midi = { { sizeof (clap_event_midi), time, CLAP_CORE_EVENT_SPACE_ID, CLAP_EVENT_MIDI, 0 }, 0, { status, a1, a2 } };
        list.push_back (a);
    }
    void sort() { std::stable_sort (list.begin(), list.end(), [] (const Any& a, const Any& b) { return a.header.time < b.header.time; }); }
};

// What the plug-in sends back: parameter values it changed itself (buttons returning to 0, the sequences' bits and
// CVs), with the frame (from the render's start) of the block they came in.
struct OutEvents
{
    struct Value { clap_id id; double value; uint64_t frame; };
    std::vector<Value> values;
    uint64_t blockStart = 0;
    clap_output_events out { this, [] (const clap_output_events* o, const clap_event_header* h) {
        if (h->space_id == CLAP_CORE_EVENT_SPACE_ID && h->type == CLAP_EVENT_PARAM_VALUE)
        {
            auto* self = static_cast<OutEvents*> (o->ctx);
            const auto* v = reinterpret_cast<const clap_event_param_value*> (h);
            self->values.push_back ({ v->param_id, v->value, self->blockStart + h->time });
        }
        return true;
    } };
    std::vector<double> of (clap_id id) const
    {
        std::vector<double> out;
        for (auto& v : values)
            if (v.id == id)
                out.push_back (v.value);
        return out;
    }
};

// The transport a block hears: Melodee's way, the place at the block's start counted on by its frames.
struct Transport
{
    bool playing = true;
    double bpm = 120.0, beats = 0.0;
    bool placed = true; // false: a host that sends no song position
};

struct Instance
{
    const clap_plugin* plugin = nullptr;
    const clap_plugin_params* params = nullptr;
    const clap_plugin_state* state = nullptr;
    const clap_plugin_preset_load* presets = nullptr;
    std::map<std::string, clap_id> ids;
    double rate = 48000.0;

    explicit Instance (double sampleRate = 48000.0, uint32_t maxFrames = 4096, const clap_host* with = &host) : rate (sampleRate)
    {
        const auto* d = factory->get_plugin_descriptor (factory, 0);
        plugin = factory->create_plugin (factory, with, d->id);
        CHECK (plugin != nullptr && plugin->init (plugin));
        params = static_cast<const clap_plugin_params*> (plugin->get_extension (plugin, CLAP_EXT_PARAMS));
        state = static_cast<const clap_plugin_state*> (plugin->get_extension (plugin, CLAP_EXT_STATE));
        presets = static_cast<const clap_plugin_preset_load*> (plugin->get_extension (plugin, CLAP_EXT_PRESET_LOAD));
        for (uint32_t i = 0; params && i < params->count (plugin); ++i)
        {
            clap_param_info info {};
            params->get_info (plugin, i, &info);
            ids[info.name] = info.id;
        }
        CHECK (plugin->activate (plugin, rate, 1, maxFrames));
        CHECK (plugin->start_processing (plugin));
    }
    ~Instance()
    {
        plugin->stop_processing (plugin);
        plugin->deactivate (plugin);
        plugin->destroy (plugin);
    }
    clap_id id (const std::string& name)
    {
        const auto it = ids.find (name);
        if (it == ids.end())
        {
            std::printf ("FAIL no parameter named \"%s\"\n", name.c_str());
            ++failures;
            return CLAP_INVALID_ID;
        }
        return it->second;
    }
    bool loadPreset (const char* key)
    {
        return presets != nullptr && presets->from_location (plugin, CLAP_PRESET_DISCOVERY_LOCATION_PLUGIN, nullptr, key);
    }
    double value (const std::string& name)
    {
        double v = 0.0;
        params->get_value (plugin, id (name), &v);
        return v;
    }
    // The value whose text is `text` (a choice by its name).
    double valueOf (const std::string& name, const std::string& text)
    {
        double v = 0.0;
        CHECK (params->text_to_value (plugin, id (name), text.c_str(), &v));
        return v;
    }
    std::string text (const std::string& name, double v)
    {
        char t[64] = {};
        params->value_to_text (plugin, id (name), v, t, sizeof (t));
        return t;
    }

    // Renders `frames` in blocks of `block`, with `events(frame, n, list)` adding events for the block starting at
    // `frame`; the transport is asked for each block.
    void render (uint32_t frames, uint32_t block, std::vector<float>& left, std::vector<float>& right,
                 const std::function<Transport (uint64_t)>& transport, const std::function<void (uint64_t, uint32_t, Events&)>& events = {},
                 OutEvents* out = nullptr)
    {
        left.assign (frames, 0.0f);
        right.assign (frames, 0.0f);
        OutEvents ignored;
        OutEvents* to = out ? out : &ignored;
        for (uint64_t at = 0; at < frames; at += block)
        {
            const uint32_t n = (uint32_t) std::min<uint64_t> (block, frames - at);
            const auto t = transport (at);
            clap_event_transport tr {};
            tr.header = { sizeof (clap_event_transport), 0, CLAP_CORE_EVENT_SPACE_ID, CLAP_EVENT_TRANSPORT, 0 };
            tr.flags = CLAP_TRANSPORT_HAS_TEMPO | (t.placed ? CLAP_TRANSPORT_HAS_BEATS_TIMELINE : 0) | (t.playing ? CLAP_TRANSPORT_IS_PLAYING : 0);
            tr.tempo = t.bpm;
            tr.song_pos_beats = (clap_beattime) std::llround (t.beats * (double) CLAP_BEATTIME_FACTOR);
            Events e;
            if (events)
                events (at, n, e);
            e.sort();
            float* outs[2] = { left.data() + at, right.data() + at };
            clap_audio_buffer buffer {};
            buffer.data32 = outs;
            buffer.channel_count = 2;
            clap_process p {};
            p.steady_time = (int64_t) at;
            p.frames_count = n;
            p.transport = &tr;
            p.audio_outputs = &buffer;
            p.audio_outputs_count = 1;
            p.in_events = &e.in;
            to->blockStart = at;
            p.out_events = &to->out;
            CHECK (plugin->process (plugin, &p) != CLAP_PROCESS_ERROR);
        }
    }
    void set (Events& e, const std::string& name, double value) { e.param (0, id (name), value); }
    // Parameter changes before anything plays: one silent, stopped block.
    void setup (const std::vector<std::pair<std::string, double>>& values)
    {
        std::vector<float> l, r;
        render (1, 1, l, r, [] (uint64_t) { return Transport { false, 120.0, 0.0 }; }, [&] (uint64_t at, uint32_t, Events& e) {
            if (at == 0)
                for (auto& [name, v] : values)
                    set (e, name, v);
        });
    }
    // A button pressed at frame `at` of a render: for an events callback.
    std::function<void (uint64_t, uint32_t, Events&)> pressAt (std::vector<std::pair<uint64_t, std::string>> presses)
    {
        return [this, presses] (uint64_t at, uint32_t n, Events& e) {
            for (auto& [when, name] : presses)
                if (when >= at && when < at + n)
                    e.param ((uint32_t) (when - at), id (name), 1.0);
        };
    }
};

using Values = std::vector<std::pair<std::string, double>>;

// A steady wave's frequency in [from, to): from its first and last rising zero crossings, each placed between its
// two frames.
double frequency (const std::vector<float>& x, size_t from, size_t to, double rate = 48000.0)
{
    double first = -1.0, last = -1.0;
    long periods = -1;
    for (size_t i = from + 1; i < to; ++i)
        if (x[i - 1] <= 0.0f && x[i] > 0.0f)
        {
            const double at = (double) (i - 1) + (double) x[i - 1] / ((double) x[i - 1] - (double) x[i]);
            if (first < 0.0)
                first = at;
            last = at;
            ++periods;
        }
    return periods > 0 ? periods * rate / (last - first) : 0.0;
}

// Frames where a hit starts: the first nonzero frame after at least `gap` frames of exact silence (Psychic's output is
// exactly zero while its VCAs are closed), so these are to the frame.
std::vector<long> onsets (const std::vector<float>& x, long gap = 96)
{
    std::vector<long> found;
    long silent = gap;
    for (long i = 0; i < (long) x.size(); ++i)
    {
        if (x[(size_t) i] == 0.0f)
        {
            ++silent;
            continue;
        }
        if (silent >= gap)
            found.push_back (i);
        silent = 0;
    }
    return found;
}

Transport playingFrom (double startBeats, double bpm, double rate, uint64_t at)
{
    return { true, bpm, startBeats + (double) at * bpm / 60.0 / rate };
}
auto from0 = [] (uint64_t at) { return playingFrom (0.0, 120.0, 48000.0, at); };
auto stopped = [] (uint64_t) { return Transport { false, 120.0, 0.0 }; };

// A short, clean hit on every on bit of SEQ 1: the VCO alone at a fixed pitch, the shortest EG2, no modulation;
// SEQ 2 silent (EG Trig Mix at Seq 1). `bits1` the pattern.
Values clickPatch (Instance& a, int bits1 = 255)
{
    return { { "VCO Freq", 1.0 }, { "VCO Level", 0.5 }, { "Mod VCO Level", 0.0 }, { "Ring Mod Level", 0.0 }, { "Noise Level", 0.0 },
             { "FM Amt", 0.0 }, { "Fold", 0.0 }, { "VCW EG1 Amt", 0.0 }, { "VCF EG1 Amt", 0.0 }, { "VCO EG1 Amt", 0.0 },
             { "Cutoff", 1.0 }, { "EG2 Decay", 0.0 }, { "EG1 Decay", 0.0 }, { "EG Trig Mix", 0.0 }, { "CV Range 1", 0.0 },
             { "CV Range 2", 0.0 }, { "Bits 1", (double) bits1 }, { "Clock Div", a.valueOf ("Clock Div", "1/8") }, { "Volume", 0.6 } };
}

// Hits' starts, as steps of `stepFrames` from the first frame (-1 where a hit is off the grid).
std::vector<long> stepsOf (const std::vector<long>& hits, long stepFrames)
{
    std::vector<long> out;
    for (long h : hits)
        out.push_back (h % stepFrames == 0 ? h / stepFrames : -1);
    return out;
}

void printHits (const char* what, const std::vector<long>& hits)
{
    if (! debug())
        return;
    std::printf ("debug %s:", what);
    for (long h : hits)
        std::printf (" %ld", h);
    std::printf ("\n");
}

void testDescription()
{
    CHECK (factory->get_plugin_count (factory) == 1);
    const auto* d = factory->get_plugin_descriptor (factory, 0);
    CHECK (d != nullptr && std::string (d->name) == "hdslave Psychic" && std::string (d->id) == "com.hdslave.psychic");
    bool instrument = false, stereo = false, mono = false;
    for (const char* const* f = d->features; f && *f; ++f)
    {
        instrument |= std::strcmp (*f, CLAP_PLUGIN_FEATURE_INSTRUMENT) == 0;
        stereo |= std::strcmp (*f, CLAP_PLUGIN_FEATURE_STEREO) == 0;
        mono |= std::strcmp (*f, CLAP_PLUGIN_FEATURE_MONO) == 0;
    }
    CHECK (instrument && stereo && ! mono); // its one output is a stereo port

    Instance a;
    const auto* audio = static_cast<const clap_plugin_audio_ports*> (a.plugin->get_extension (a.plugin, CLAP_EXT_AUDIO_PORTS));
    const auto* notes = static_cast<const clap_plugin_note_ports*> (a.plugin->get_extension (a.plugin, CLAP_EXT_NOTE_PORTS));
    CHECK (audio && audio->count (a.plugin, true) == 0 && audio->count (a.plugin, false) == 1);
    clap_audio_port_info port {};
    CHECK (audio && audio->get (a.plugin, 0, false, &port) && port.channel_count == 2);
    CHECK (notes && notes->count (a.plugin, true) == 1 && notes->count (a.plugin, false) == 0);
}

void testParameters()
{
    Instance a;
    const uint32_t count = a.params->count (a.plugin);
    CHECK (count == 71);
    std::map<clap_id, int> seen;
    for (uint32_t i = 0; i < count; ++i)
    {
        clap_param_info info {};
        CHECK (a.params->get_info (a.plugin, i, &info));
        CHECK (++seen[info.id] == 1);
        CHECK (info.id < 71);
        CHECK (info.min_value <= info.default_value && info.default_value <= info.max_value);
        double v = 0.0;
        CHECK (a.params->get_value (a.plugin, info.id, &v) && v == info.default_value);
        // Text round trips to the same value, within the display's precision, at the default and across the range.
        for (double at : { v, info.min_value, info.max_value, 0.37 * info.min_value + 0.63 * info.max_value })
        {
            if (info.flags & CLAP_PARAM_IS_STEPPED)
                at = std::round (at);
            char text[64] = {};
            CHECK (a.params->value_to_text (a.plugin, info.id, at, text, sizeof (text)) && text[0] != 0);
            double back = -1e9;
            CHECK (a.params->text_to_value (a.plugin, info.id, text, &back));
            const bool near = std::fabs (back - at) <= 0.02 * (info.max_value - info.min_value) + 1e-9;
            if (! near)
                std::printf ("  %s: %g -> \"%s\" -> %g\n", info.name, at, text, back);
            CHECK (near);
        }
    }
    for (const char* name : { "VCO Freq", "VCO EG1 Amt", "VCO Seq 1 Amt", "Mod VCO Freq", "Mod VCO EG1 Amt", "Mod VCO Seq 2 Amt", "FM Amt",
                              "VCO Level", "Ring Mod Level", "Mod VCO Level", "Noise Level", "Noise Tone", "Fold", "Bias", "VCW EG1 Amt",
                              "VCW Seq 1 Amt", "Cutoff", "Resonance", "Filter Mode", "VCF EG1 Amt", "VCF Seq 2 Amt", "Order", "Blend",
                              "Volume", "EG1 Decay", "EG2 Decay", "EG Trig Mix", "Clock Div", "Swing", "Run", "Trigger", "Advance",
                              "Reset", "Chain Seq", "Quantize", "Corrupt 1", "CV Range 1", "Length 1", "Bit Flip 1", "Bit Shift 1",
                              "Bits 1", "Write Head 1", "Corrupt 2", "CV Range 2", "Length 2", "Bit Flip 2", "Bit Shift 2", "Bits 2",
                              "Write Head 2", "Save", "Recall", "Seq 1 CV 1", "Seq 1 CV 8", "Seq 2 CV 1", "Seq 2 CV 8", "Clock 2",
                              "Mod VCO > Fold", "Mod VCO > Cutoff", "Drone" })
        CHECK (a.ids.count (name) == 1);
    // The texts the panel's language needs.
    CHECK (a.text ("Bits 1", 1 + 4 + 8) == "X.XX....");
    CHECK (a.valueOf ("Bits 1", "..X....X") == 4 + 128);
    CHECK (a.text ("VCO Seq 1 Amt", 1.0) == "QTZ" && a.valueOf ("VCO Seq 1 Amt", "QTZ") == 1.0);
    CHECK (a.text ("Filter Mode", 0.0) == "LP" && a.text ("Filter Mode", 1.0) == "BP");
    CHECK (a.text ("EG Trig Mix", 0.0) == "Seq 1" && a.text ("EG Trig Mix", 1.0) == "Seq 2" && a.text ("EG Trig Mix", 0.5) == "50 / 50");
    CHECK (a.text ("Quantize", 0.0) == "Unquantized" && a.text ("Quantize", 15.0) == "Quads (Minor 3rds)");
    CHECK (a.text ("Quantize", 14.0) == "Minor 6-Note" && a.valueOf ("Quantize", "Minor 6-Note") == 14.0);
    CHECK (a.text ("VCO Freq", 0.0) == "20.0 Hz" && a.text ("VCO Freq", 1.0) == "5.12 kHz");
    CHECK (a.text ("Mod VCO Freq", 0.0) == "0.16 Hz" && a.text ("Mod VCO Freq", 1.0) == "1.31 kHz");
    CHECK (a.text ("Seq 1 CV 1", 1.0) == "+5.00 V" && a.text ("CV Range 1", 0.2) == "+-1.0 oct");
}

std::vector<uint8_t> saveState (Instance& a)
{
    std::vector<uint8_t> saved;
    clap_ostream os { &saved, [] (const clap_ostream* s, const void* d, uint64_t n) -> int64_t {
        auto* v = static_cast<std::vector<uint8_t>*> (s->ctx);
        v->insert (v->end(), (const uint8_t*) d, (const uint8_t*) d + n);
        return (int64_t) n;
    } };
    CHECK (a.state->save (a.plugin, &os));
    return saved;
}
bool loadState (Instance& b, const std::vector<uint8_t>& saved)
{
    struct Reader { const std::vector<uint8_t>* v; size_t at; } reader { &saved, 0 };
    clap_istream is { &reader, [] (const clap_istream* s, void* to, uint64_t n) -> int64_t {
        auto* r = static_cast<Reader*> (s->ctx);
        const size_t k = std::min<size_t> ((size_t) n, r->v->size() - r->at);
        std::memcpy (to, r->v->data() + r->at, k);
        r->at += k;
        return (int64_t) k;
    } };
    return b.state->load (b.plugin, &is);
}

void testState()
{
    std::vector<float> l1, r1, l2, r2;
    Instance a;
    a.setup ({ { "VCO Freq", 0.31 }, { "FM Amt", 0.4 }, { "Bias", -0.6 }, { "Seq 1 CV 3", 0.7 }, { "Bits 2", 77 }, { "Order", 2.0 },
               { "Chain Seq", 1.0 }, { "Quantize", 9.0 } });
    const auto saved = saveState (a);
    Instance b;
    CHECK (loadState (b, saved));
    for (auto& [name, id] : a.ids)
        CHECK (a.value (name) == b.value (name));
    b.setup ({}); // the same one-frame start as a's
    a.render (48000, 128, l1, r1, from0);
    b.render (48000, 128, l2, r2, from0);
    CHECK (l1 == l2 && r1 == r2);
    // A truncated or foreign state is refused.
    std::vector<uint8_t> junk { 'J', 'U', 'N', 'K' };
    CHECK (! loadState (b, junk));
    std::vector<uint8_t> cut (saved.begin(), saved.begin() + 40);
    CHECK (! loadState (b, cut));
    // A state from a newer version is refused too (its layout is not this one's to guess at).
    std::vector<uint8_t> future = saved;
    future[4] = 2; // the version word after the tag
    CHECK (! loadState (b, future));
    // A state without the buffer (the values alone) loads, its buffer empty.
    std::vector<uint8_t> valuesOnly (saved.begin(), saved.end() - (4 + 8 + 24 * 8));
    Instance c;
    CHECK (loadState (c, valuesOnly) && c.value ("Bits 2") == 77);

    // The buffer travels with the state: Save in one, change the bits, save the state; loaded in another, Recall
    // brings back the saved bits.
    Instance d;
    d.setup ({ { "Bits 1", 0x5a } });
    d.render (256, 128, l1, r1, stopped, d.pressAt ({ { 10, "Save" } }));
    d.setup ({ { "Bits 1", 3 } });
    const auto withBuffer = saveState (d);
    Instance e;
    CHECK (loadState (e, withBuffer) && e.value ("Bits 1") == 3);
    OutEvents out;
    e.render (256, 128, l1, r1, stopped, e.pressAt ({ { 5, "Recall" } }), &out);
    CHECK (e.value ("Bits 1") == 0x5a);
    const auto heard = out.of (e.id ("Bits 1"));
    CHECK (! heard.empty() && heard.back() == 0x5a);
    // With nothing saved, Recall does nothing.
    Instance f;
    f.setup ({ { "Bits 1", 9 } });
    f.render (256, 128, l1, r1, stopped, f.pressAt ({ { 5, "Recall" } }));
    CHECK (f.value ("Bits 1") == 9);

    // A state loaded on the main thread while a block plays, in the block where the engine changes the same value
    // itself (Bit Flip): the loaded value stands, and the engine follows it from the next block.
    Instance g;
    g.setup ({ { "Bits 1", 1 } });
    Instance h;
    h.setup ({ { "Bits 1", 0x81 } });
    const auto loaded = saveState (h);
    g.render (512, 128, l1, r1, from0, [&] (uint64_t at, uint32_t, Events& e) {
        if (at != 256)
            return;
        e.param (10, g.id ("Bit Flip 1"), 1.0); // the engine flips a bit at frame 10
        e.param (100, g.id ("Volume"), 0.7);     // reading this event, the "main thread" loads the state
        e.hookAt = 1;
        e.hook = [&] { CHECK (loadState (g, loaded)); };
    });
    CHECK (g.value ("Bits 1") == 0x81);
    g.render (256, 128, l1, r1, from0);
    CHECK (g.value ("Bits 1") == 0x81);
}

// The host's silence (hdslaveOS sends CC 120 and CC 123 at 0 on every channel, which arrive as NOTE_CHOKEs in the
// CLAP dialect and as the CCs themselves in the MIDI one) stops what sounds, Drone included, within a block; the
// next step, Trigger or note strikes again. A note off still lets a hit ring out, and CC 121 has nothing to reset.
void testPanic()
{
    std::vector<float> l, r, ref;
    auto silentFrom = [] (const std::vector<float>& x, size_t from) {
        for (size_t i = from; i < x.size(); ++i)
            if (x[i] != 0.0f)
                return false;
        return true;
    };
    auto soundsIn = [] (const std::vector<float>& x, size_t from, size_t to) {
        for (size_t i = from; i < to && i < x.size(); ++i)
            if (std::fabs (x[i]) > 0.005f)
                return true;
        return false;
    };
    // Drone held open, stopped: each silencer at frame 2400 ends it; a note off and CC 121/40 change nothing.
    constexpr uint64_t at2400 = 2400;
    auto droneRun = [&] (int silencer, std::vector<float>& out) {
        Instance a;
        a.setup ({ { "Drone", 1.0 } });
        a.render (4800, 128, out, r, stopped, [&] (uint64_t at, uint32_t n, Events& e) {
            if (at2400 < at || at2400 >= at + n)
                return;
            const uint32_t t = (uint32_t) (at2400 - at);
            if (silencer == 1) e.note (t, CLAP_EVENT_NOTE_CHOKE, 60, 0.0);
            else if (silencer == 2) e.note (t, CLAP_EVENT_NOTE_CHOKE, 99, 0.0); // a key never played: one voice
            else if (silencer == 3) e.midi (t, 0xb0, 120, 0);
            else if (silencer == 4) e.midi (t, 0xb3, 123, 0);
            else if (silencer == 5) e.note (t, CLAP_EVENT_NOTE_OFF, 60, 0.0);
            else if (silencer == 6) { e.midi (t, 0xb0, 121, 0); e.midi (t, 0xb0, 40, 100); }
            else if (silencer == 7) e.midi (t, 0xb0, 120, 64); // channel mode is value 0 only
        });
    };
    droneRun (0, ref);
    CHECK (soundsIn (ref, 3000, 4800));
    for (int s : { 1, 2, 3, 4 })
    {
        droneRun (s, l);
        CHECK (silentFrom (l, 2400 + 128));
    }
    for (int s : { 5, 6, 7 })
    {
        droneRun (s, l);
        CHECK (l == ref);
    }
    // The longest EG2 tail (8 s to -60 dB), struck stopped: a choke a second in silences it.
    {
        Instance a;
        a.setup ({ { "EG1 Decay", 1.0 }, { "EG2 Decay", 1.0 } });
        a.render (96000, 128, l, r, stopped, [&] (uint64_t at, uint32_t n, Events& e) {
            if (at == 0) e.note (0, CLAP_EVENT_NOTE_ON, 60, 1.0);
            if (48000 >= at && 48000 < at + n) e.note ((uint32_t) (48000 - at), CLAP_EVENT_NOTE_CHOKE, 60, 0.0);
        });
        CHECK (soundsIn (l, 47000, 48000) && silentFrom (l, 48000 + 128));
    }
    // Playing: a choke between steps silences the tail, and the next step strikes again.
    {
        Instance a;
        auto p = clickPatch (a);
        p.push_back ({ "EG2 Decay", 1.0 });
        a.setup (p);
        a.render (48000, 128, l, r, from0, [&] (uint64_t at, uint32_t n, Events& e) {
            if (6000 >= at && 6000 < at + n) e.midi ((uint32_t) (6000 - at), 0xb0, 120, 0);
        });
        CHECK (soundsIn (l, 0, 6000) && silentFrom (std::vector<float> (l.begin(), l.begin() + 12000), 6000 + 128)
               && soundsIn (l, 12000, 18000));
    }
    // Same frame, in the order sent: note then choke is silent; choke then note sounds.
    {
        auto both = [&] (bool chokeFirst, std::vector<float>& out) {
            Instance a;
            a.setup ({ { "EG2 Decay", 1.0 } });
            a.render (9600, 128, out, r, stopped, [&] (uint64_t at, uint32_t, Events& e) {
                if (at != 1280)
                    return;
                if (chokeFirst) e.note (0, CLAP_EVENT_NOTE_CHOKE, 60, 0.0), e.note (0, CLAP_EVENT_NOTE_ON, 60, 1.0);
                else e.note (0, CLAP_EVENT_NOTE_ON, 60, 1.0), e.note (0, CLAP_EVENT_NOTE_CHOKE, 60, 0.0);
            });
        };
        both (false, l);
        CHECK (silentFrom (l, 1280 + 128));
        both (true, l);
        CHECK (soundsIn (l, 1280, 9600));
    }
}

// A silence while Drone is off (hdslaveOS sends CC 123 at 0 on every Stop; CC 120 and the chokes come with its
// silence) keeps nothing shut that a Drone needs later: Drone turned on afterwards, or a drone preset or state loaded,
// opens the VCAs at once, to the bit as in an instance never silenced. A silence while Drone is on still holds until a
// strike (testPanic), but Drone turned off and on again, or a drone state loaded, opens them again.
void testDroneAfterSilence()
{
    std::vector<float> l, r, ref;
    auto soundsIn = [] (const std::vector<float>& x, size_t from, size_t to) {
        for (size_t i = from; i < to && i < x.size(); ++i)
            if (std::fabs (x[i]) > 0.005f)
                return true;
        return false;
    };
    auto silencer = [] (int s, Events& e) {
        if (s == 1) e.midi (0, 0xb0, 123, 0);                       // Stop's All Notes Off
        else if (s == 2) e.midi (0, 0xb5, 120, 0);                  // All Sound Off
        else if (s == 3) e.note (0, CLAP_EVENT_NOTE_CHOKE, 60, 0.0); // the CLAP dialect's choke
    };
    // Drone off, stopped: a silence at frame 1280 (0: none), Drone turned on at 4864.
    auto turnedOn = [&] (int s, std::vector<float>& out) {
        Instance a;
        a.render (14400, 128, out, r, stopped, [&] (uint64_t at, uint32_t, Events& e) {
            if (at == 1280) silencer (s, e);
            if (at == 4864) e.param (0, a.id ("Drone"), 1.0);
        });
    };
    turnedOn (0, ref);
    CHECK (soundsIn (ref, 6000, 14400));
    for (int s : { 1, 2, 3 })
    {
        turnedOn (s, l);
        CHECK (l == ref);
    }
    // The same silence, then a drone preset, or a drone state, loaded between blocks (the main thread's load).
    Instance droning;
    droning.setup ({ { "Drone", 1.0 }, { "Cutoff", 0.8 } });
    const auto droneState = saveState (droning);
    auto loaded = [&] (int s, bool preset, std::vector<float>& out) {
        Instance a;
        a.render (2560, 128, l, r, stopped, [&] (uint64_t at, uint32_t, Events& e) {
            if (at == 1280) silencer (s, e);
        });
        CHECK (preset ? a.loadPreset ("corrupt-drone") : loadState (a, droneState));
        a.render (9600, 128, out, r, stopped);
    };
    for (bool preset : { true, false })
    {
        loaded (0, preset, ref);
        CHECK (soundsIn (ref, 2000, 9600));
        for (int s : { 1, 2, 3 })
        {
            loaded (s, preset, l);
            CHECK (l == ref);
        }
    }
    // Drone on and silenced (it holds), then Drone off and on again: it sounds from there.
    {
        Instance a;
        a.setup ({ { "Drone", 1.0 } });
        a.render (14400, 128, l, r, stopped, [&] (uint64_t at, uint32_t, Events& e) {
            if (at == 1280) silencer (2, e);
            if (at == 4864) e.param (0, a.id ("Drone"), 0.0);
            if (at == 6400) e.param (0, a.id ("Drone"), 1.0);
        });
        CHECK (! soundsIn (l, 1280 + 128, 6400) && soundsIn (l, 8000, 14400));
    }
    // Drone on and silenced, then a drone state loaded: it sounds as loaded.
    {
        Instance a;
        a.setup ({ { "Drone", 1.0 } });
        a.render (4800, 128, l, r, stopped, [&] (uint64_t at, uint32_t, Events& e) {
            if (at == 1280) silencer (1, e);
        });
        CHECK (! soundsIn (l, 1280 + 128, 4800));
        CHECK (loadState (a, droneState));
        a.render (9600, 128, l, r, stopped);
        CHECK (soundsIn (l, 2000, 9600));
    }
}

// hdslaveOS's idle gate for an instrument (src/melodee/plugin/idle_gate.h, as plugin_rack.cpp and clap_instance.cpp run
// it), with the song stopped and no key held: before each 128-frame block the plug-in is skipped once its output has
// been under -100 dB for half a second, unless something waits for it: MIDI or a value for that block, or its own
// request_process (the host clears the wish as a block starts, so one asked for in between runs the next block).
// Skipped, it is not called at all. A preset or state load on the main thread is none of these.
struct GatedHost
{
    clap_host host;
    bool asked = false;
    GatedHost()
        : host { CLAP_VERSION, this, "Psychic gated test host", "tests", "", "1", hostExtension, [] (const clap_host*) {},
                 [] (const clap_host* h) { static_cast<GatedHost*> (h->host_data)->asked = true; }, [] (const clap_host*) {} }
    {
    }
};
// `frames` stopped through `a` in 128-frame blocks, `events` adding a block's events; with `gate` (the host `a` was made
// with, and its quiet count, kept across calls), through the idle gate, `skipped` counting the blocks it slept.
void gatedRender (Instance& a, uint32_t frames, std::vector<float>& out, GatedHost* gate, uint32_t& quiet, int& skipped,
                  const std::function<void (uint64_t, Events&)>& events = {})
{
    out.assign (frames, 0.0f);
    std::vector<float> right (frames, 0.0f);
    const uint32_t needed = (uint32_t) std::lround (0.5 * a.rate);
    for (uint32_t at = 0; at < frames; at += 128)
    {
        const uint32_t n = std::min<uint32_t> (128, frames - at);
        Events e;
        if (events)
            events (at, e);
        e.sort();
        if (gate != nullptr)
        {
            const bool waiting = gate->asked || ! e.list.empty();
            if (waiting)
                quiet = 0;
            else if (quiet >= needed)
            {
                ++skipped;
                continue;
            }
            gate->asked = false;
        }
        clap_event_transport tr {};
        tr.header = { sizeof (clap_event_transport), 0, CLAP_CORE_EVENT_SPACE_ID, CLAP_EVENT_TRANSPORT, 0 };
        tr.flags = CLAP_TRANSPORT_HAS_TEMPO | CLAP_TRANSPORT_HAS_BEATS_TIMELINE;
        tr.tempo = 120.0;
        float* outs[2] = { out.data() + at, right.data() + at };
        clap_audio_buffer buffer {};
        buffer.data32 = outs;
        buffer.channel_count = 2;
        OutEvents ignored;
        clap_process p {};
        p.steady_time = -1;
        p.frames_count = n;
        p.transport = &tr;
        p.audio_outputs = &buffer;
        p.audio_outputs_count = 1;
        p.in_events = &e.in;
        p.out_events = &ignored.out;
        CHECK (a.plugin->process (a.plugin, &p) != CLAP_PROCESS_ERROR);
        bool loud = false;
        for (uint32_t i = 0; i < n; ++i)
            loud |= std::fabs (outs[0][i]) > 1e-5f || std::fabs (outs[1][i]) > 1e-5f;
        quiet = loud ? 0 : std::min (quiet + n, needed);
    }
}

// A drone preset or state loaded while hdslaveOS lets the plug-in sleep (after a Stop's CC 123 with Drone off: silent,
// no key held) drones from the next block, as in an instance run every block: the load asks the host to run it.
void testLoadWakesSleeping()
{
    std::vector<float> l, ref;
    auto soundsIn = [] (const std::vector<float>& x) {
        for (float v : x)
            if (std::fabs (v) > 0.005f)
                return true;
        return false;
    };
    Instance droning;
    droning.setup ({ { "Drone", 1.0 }, { "Cutoff", 0.8 } });
    const auto droneState = saveState (droning);
    for (bool preset : { true, false })
    {
        auto run = [&] (bool gated, std::vector<float>& after, int& skipped) {
            GatedHost gate;
            Instance a (48000.0, 4096, gated ? &gate.host : &host);
            uint32_t quiet = 0;
            gatedRender (a, 96000, l, gated ? &gate : nullptr, quiet, skipped, [] (uint64_t at, Events& e) {
                if (at == 1280) e.midi (0, 0xb0, 123, 0); // Stop's All Notes Off, Drone off
            });
            CHECK (preset ? a.loadPreset ("corrupt-drone") : loadState (a, droneState));
            gatedRender (a, 9600, after, gated ? &gate : nullptr, quiet, skipped);
        };
        int skipped = 0, none = 0;
        run (false, ref, none);
        run (true, l, skipped);
        CHECK (skipped > 0); // it slept before the load
        CHECK (soundsIn (ref) && soundsIn (l));
        CHECK (l == ref);
    }
}

// Play landing part-way through a step: within the Play window (128 frames, the device's block) that step plays at
// once; further in, the next step is the first. The same at every block size, to the bit.
void testMidGridPlay()
{
    std::vector<float> l, r, at128;
    auto run = [&] (double startFrames, uint32_t block, std::vector<float>& out) {
        Instance a;
        a.setup (clickPatch (a));
        const double startBeats = startFrames / 24000.0; // 120 BPM at 48 kHz: 24,000 frames a beat, 12,000 a 1/8 step
        a.render (48000, block, out, r, [startBeats] (uint64_t at) { return playingFrom (startBeats, 120.0, 48000.0, at); });
    };
    // 100 frames past the second step's start: it plays at once, then on the grid.
    run (12100.0, 128, at128);
    CHECK ((onsets (at128) == std::vector<long> { 0, 11900, 23900, 35900, 47900 }));
    for (uint32_t block : { 1u, 37u, 4096u })
    {
        run (12100.0, block, l);
        CHECK (l == at128);
    }
    // 2,000 frames in: that step is gone; the next is the first, whatever the block.
    run (14000.0, 128, at128);
    CHECK ((onsets (at128) == std::vector<long> { 10000, 22000, 34000, 46000 }));
    for (uint32_t block : { 1u, 37u, 4096u })
    {
        run (14000.0, block, l);
        CHECK (l == at128);
    }
}

// A state loaded while the plug-in rings: its values are in place at the next block (the glides settle), not eased
// to over the next millisecond. Here the loaded Volume is 0, so past the decimator's flush the output is exact 0.
void testSettleOnLoad()
{
    std::vector<float> l, r;
    Instance quiet;
    quiet.setup ({ { "Drone", 1.0 }, { "Volume", 0.0 } });
    const auto state = saveState (quiet);
    Instance a;
    a.setup ({ { "Drone", 1.0 } });
    a.render (4800, 128, l, r, stopped);
    bool ringing = false;
    for (size_t i = 3800; i < l.size(); ++i)
        ringing |= std::fabs (l[i]) > 0.005f;
    CHECK (ringing);
    CHECK (loadState (a, state));
    a.render (2048, 128, l, r, stopped);
    bool settled = true;
    for (size_t i = 64; i < l.size(); ++i)
        settled &= l[i] == 0.0f;
    CHECK (settled);
}

// Activation refuses a rate that is no rate (the engine divides by it).
void testHardening()
{
    const auto* d = factory->get_plugin_descriptor (factory, 0);
    for (double bad : { 0.0, -48000.0, HUGE_VAL, (double) NAN })
    {
        const clap_plugin* plugin = factory->create_plugin (factory, &host, d->id);
        CHECK (plugin != nullptr && plugin->init (plugin));
        CHECK (! plugin->activate (plugin, bad, 1, 128));
        plugin->destroy (plugin);
    }
}

// One inner phase's delay through the decimator, in host frames: the energy centroid of its impulse response,
// measured on the Decimator compiled from src/engine.h. (The host binary builds it without the plug-in's
// compiler flags; the delay in whole frames is set by the FIR's tap count, not by the last bit of a tap, and a
// plug-in-level impulse is impossible: Psychic has no audio input.)
double decimatorDelay (bool oddPhase)
{
    psychic::Decimator d;
    d.design();
    double num = 0.0, den = 0.0;
    for (int i = 0; i < 64; ++i)
    {
        const float y = d.push (i == 0 && ! oddPhase ? 1.0f : 0.0f, i == 0 && oddPhase ? 1.0f : 0.0f);
        num += (double) i * (double) y * (double) y, den += (double) y * (double) y;
    }
    return den > 0.0 ? num / den : -1.0;
}

// Reported latency and tail, against renders of the same instances. Latency is the decimator's 47-tap FIR group
// delay, measured by impulse above (the even inner phase centres exactly on the reported frame, the odd phase
// half a frame later), the same at every rate. The tail is EG2 ringing to exact zero; with Drone it never ends.
void testLatencyTail()
{
    std::vector<float> l, r;
    const double evenDelay = decimatorDelay (false), oddDelay = decimatorDelay (true);
    for (double rate : { 44100.0, 48000.0, 96000.0 })
    {
        Instance a (rate);
        const auto* latency = static_cast<const clap_plugin_latency*> (a.plugin->get_extension (a.plugin, CLAP_EXT_LATENCY));
        CHECK (latency != nullptr);
        if (latency == nullptr)
            continue;
        const double reported = (double) latency->get (a.plugin);
        CHECK (std::fabs (evenDelay - reported) < 0.01 && std::fabs (oddDelay - (reported + 0.5)) < 0.01);
    }
    for (double eg2 : { 0.0, 0.45, 1.0 })
    {
        Instance a;
        a.setup ({ { "EG2 Decay", eg2 }, { "VCO Level", 0.5 }, { "Mod VCO Level", 0.0 } });
        const auto* tail = static_cast<const clap_plugin_tail*> (a.plugin->get_extension (a.plugin, CLAP_EXT_TAIL));
        CHECK (tail != nullptr);
        if (tail == nullptr)
            continue;
        const uint32_t reported = tail->get (a.plugin);
        a.render (720000, 1024, l, r, stopped, [&] (uint64_t at, uint32_t, Events& e) {
            if (at == 0) e.note (0, CLAP_EVENT_NOTE_ON, 60, 1.0);
        });
        long last = -1;
        for (long i = 0; i < (long) l.size(); ++i)
            if (l[(size_t) i] != 0.0f)
                last = i;
        CHECK (last >= 0 && reported >= (uint32_t) (last + 1) && reported <= (uint32_t) (last + 1) + 2400);
    }
    {
        Instance a;
        a.setup ({ { "Drone", 1.0 } });
        const auto* tail = static_cast<const clap_plugin_tail*> (a.plugin->get_extension (a.plugin, CLAP_EXT_TAIL));
        CHECK (tail != nullptr && tail->get (a.plugin) >= 0x7fffffffu); // at least INT32_MAX: infinite
    }
}

// A caching host re-reads the tail only when told it changed: EG2 Decay and Drone (the two values tailGet
// reads) tell it, by process event, by flush and by a preset load (whose notice waits for the next block:
// clap_host_tail::changed is an audio-thread call). An unrelated knob, or the same value again, does not. A
// host that does not offer clap.tail still works.
void testTailNotices()
{
    std::vector<float> l, r;
    Instance a;
    const int base = tailNotices;
    a.setup ({ { "Volume", 0.4 } });
    CHECK (tailNotices == base);
    a.setup ({ { "EG2 Decay", 0.8 } });
    CHECK (tailNotices == base + 1);
    a.setup ({ { "EG2 Decay", 0.8 } }); // no change: no notice
    CHECK (tailNotices == base + 1);
    {
        Events e;
        e.param (0, a.id ("EG2 Decay"), 0.6);
        OutEvents out;
        a.params->flush (a.plugin, &e.in, &out.out);
        a.render (128, 128, l, r, stopped); // a flush's notice lands with the next block
        CHECK (tailNotices == base + 2);
    }
    a.setup ({ { "Drone", 1.0 } });
    CHECK (tailNotices == base + 3);
    CHECK (a.loadPreset ("default")); // EG2 Decay back to 0.45, Drone off: one notice, on the next block
    CHECK (tailNotices == base + 3);
    a.render (128, 128, l, r, stopped);
    CHECK (tailNotices == base + 4);

    offerTail = false;
    {
        Instance b;
        b.setup ({ { "EG2 Decay", 0.9 }, { "Drone", 1.0 } });
        b.render (128, 128, l, r, stopped);
        const auto* tail = static_cast<const clap_plugin_tail*> (b.plugin->get_extension (b.plugin, CLAP_EXT_TAIL));
        CHECK (tail != nullptr && tail->get (b.plugin) >= 0x7fffffffu);
    }
    offerTail = true;
    CHECK (tailNotices == base + 4);
}

// The factory presets: listed by preset discovery as a host's browser reads them, loaded by key through
// preset-load, each setting every value.
struct Listed { std::string name, key; bool ours = false; uint32_t flags = 0; };
std::vector<Listed> listPresets()
{
    std::vector<Listed> out;
    const auto* f = static_cast<const clap_preset_discovery_factory*> (entry->get_factory (CLAP_PRESET_DISCOVERY_FACTORY_ID));
    CHECK (f != nullptr && entry->get_factory (CLAP_PRESET_DISCOVERY_FACTORY_ID_COMPAT) == (const void*) f);
    if (f == nullptr || f->count (f) != 1)
        return out;
    const auto* d = f->get_descriptor (f, 0);
    CHECK (d != nullptr && f->get_descriptor (f, 1) == nullptr);
    struct Index
    {
        std::vector<clap_preset_discovery_location> found;
        clap_preset_discovery_indexer indexer {};
    } index;
    index.indexer = { CLAP_VERSION, "tests", "", "", "1", &index,
                      [] (const clap_preset_discovery_indexer*, const clap_preset_discovery_filetype*) { return true; },
                      [] (const clap_preset_discovery_indexer* i, const clap_preset_discovery_location* l) {
                          static_cast<Index*> (i->indexer_data)->found.push_back (*l);
                          return true;
                      },
                      [] (const clap_preset_discovery_indexer*, const clap_preset_discovery_soundpack*) { return true; },
                      [] (const clap_preset_discovery_indexer*, const char*) -> const void* { return nullptr; } };
    CHECK (f->create (f, &index.indexer, "com.example.other") == nullptr);
    const auto* provider = f->create (f, &index.indexer, d->id);
    CHECK (provider != nullptr && provider->init (provider));
    if (provider == nullptr)
        return out;
    CHECK (index.found.size() == 1 && index.found[0].kind == CLAP_PRESET_DISCOVERY_LOCATION_PLUGIN && index.found[0].location == nullptr);
    struct Receive
    {
        clap_preset_discovery_metadata_receiver r {};
        static std::vector<Listed>& of (const clap_preset_discovery_metadata_receiver* r) { return *static_cast<std::vector<Listed>*> (r->receiver_data); }
    } receive;
    using R = Receive;
    receive.r.receiver_data = &out;
    receive.r.on_error = [] (const clap_preset_discovery_metadata_receiver*, int32_t, const char*) { CHECK (false); };
    receive.r.begin_preset = [] (const clap_preset_discovery_metadata_receiver* r, const char* name, const char* key) {
        R::of (r).push_back ({ name ? name : "", key ? key : "" });
        return true;
    };
    receive.r.add_plugin_id = [] (const clap_preset_discovery_metadata_receiver* r, const clap_universal_plugin_id* id) {
        R::of (r).back().ours = std::string (id->abi) == "clap" && std::string (id->id) == "com.hdslave.psychic";
    };
    receive.r.set_soundpack_id = [] (const clap_preset_discovery_metadata_receiver*, const char*) {};
    receive.r.set_flags = [] (const clap_preset_discovery_metadata_receiver* r, uint32_t flags) { R::of (r).back().flags = flags; };
    receive.r.add_creator = [] (const clap_preset_discovery_metadata_receiver*, const char*) {};
    receive.r.set_description = [] (const clap_preset_discovery_metadata_receiver*, const char*) {};
    receive.r.set_timestamps = [] (const clap_preset_discovery_metadata_receiver*, clap_timestamp, clap_timestamp) {};
    receive.r.add_feature = [] (const clap_preset_discovery_metadata_receiver*, const char*) {};
    receive.r.add_extra_info = [] (const clap_preset_discovery_metadata_receiver*, const char*, const char*) {};
    CHECK (! provider->get_metadata (provider, CLAP_PRESET_DISCOVERY_LOCATION_FILE, "/nowhere.preset", &receive.r));
    CHECK (out.empty());
    CHECK (provider->get_metadata (provider, CLAP_PRESET_DISCOVERY_LOCATION_PLUGIN, nullptr, &receive.r));
    provider->destroy (provider);
    return out;
}

const std::vector<std::string> presetNames { "Default", "Psychic Bells", "Fold Bass", "Chain Melody", "Polyrhythm", "Corrupt Drone", "Noise Perc" };
const std::vector<std::string> presetKeys { "default", "psychic-bells", "fold-bass", "chain-melody", "polyrhythm", "corrupt-drone", "noise-perc" };

void testPresets()
{
    const auto listed = listPresets();
    std::vector<std::string> names, keys;
    for (const auto& p : listed)
    {
        names.push_back (p.name), keys.push_back (p.key);
        CHECK (p.ours && (p.flags & CLAP_PRESET_DISCOVERY_IS_FACTORY_CONTENT) != 0);
    }
    CHECK (names == presetNames);
    CHECK (keys == presetKeys);

    Instance a;
    CHECK (a.presets != nullptr);
    const int errors = presetErrors;
    CHECK (! a.loadPreset ("no-such-preset") && ! a.loadPreset (nullptr));
    CHECK (! a.presets->from_location (a.plugin, CLAP_PRESET_DISCOVERY_LOCATION_FILE, "/nowhere", "fold-bass"));
    CHECK (presetErrors == errors + 3);
    // Default sets everything back, the sequences too; the host is told the values moved.
    a.setup ({ { "Volume", 0.1 }, { "Cutoff", 0.2 }, { "Chain Seq", 1.0 }, { "Seq 2 CV 4", -0.5 }, { "Bits 1", 3 }, { "Clock Div", 7.0 } });
    const int rescanned = rescans, loaded = presetsLoaded;
    CHECK (a.loadPreset ("default"));
    CHECK (rescans == rescanned + 1 && presetsLoaded == loaded + 1);
    for (uint32_t i = 0; i < a.params->count (a.plugin); ++i)
    {
        clap_param_info info {};
        a.params->get_info (a.plugin, i, &info);
        double v = -1.0;
        a.params->get_value (a.plugin, info.id, &v);
        CHECK (v == info.default_value);
    }
    // Loaded while playing: the next block plays it (Fold Bass's 1/16 steps instead of the 1/32 just set).
    std::vector<float> l, r;
    Instance b;
    b.setup ({ { "Clock Div", b.valueOf ("Clock Div", "1/32") } });
    b.render (128, 128, l, r, from0);
    CHECK (b.loadPreset ("fold-bass") && b.value ("Clock Div") == b.valueOf ("Clock Div", "1/16"));
    b.render (48000, 128, l, r, from0);
    bool finite = true;
    for (float s : l)
        finite &= std::isfinite (s);
    CHECK (finite);
}

void testTiming()
{
    // 120 BPM, eighth-note steps: a step every 12,000 frames at 48 kHz; the first on frame 0 of Play. Every bit on.
    std::vector<float> l, r;
    Instance a;
    a.setup (clickPatch (a));
    a.render (48000 * 4, 128, l, r, from0);
    auto hits = onsets (l);
    printHits ("timing", hits);
    CHECK (hits.size() == 16);
    for (size_t i = 0; i < hits.size(); ++i)
        CHECK (hits[i] == (long) i * 12000);
    CHECK (l == r);

    // Sixteenth notes at 90 BPM: 8,000 frames a step.
    Instance b;
    auto patch = clickPatch (b);
    patch.push_back ({ "Clock Div", b.valueOf ("Clock Div", "1/16") });
    b.setup (patch);
    b.render (48000 * 2, 128, l, r, [] (uint64_t at) { return playingFrom (0.0, 90.0, 48000.0, at); });
    hits = onsets (l);
    CHECK (hits.size() == 12);
    for (size_t i = 0; i < hits.size(); ++i)
        CHECK (hits[i] == (long) i * 8000);

    // Other rates keep the musical timing.
    for (double rate : { 44100.0, 96000.0 })
    {
        Instance c (rate);
        c.setup (clickPatch (c));
        c.render ((uint32_t) rate * 2, 128, l, r, [rate] (uint64_t at) { return playingFrom (0.0, 120.0, rate, at); });
        hits = onsets (l, (long) (rate / 500));
        CHECK (hits.size() == 8);
        for (size_t i = 0; i < hits.size(); ++i)
            CHECK (hits[i] == std::lround (i * rate / 4));
    }

    // Swing 66 %: the second step of each pair comes at 66 % of the pair (24,000 frames).
    Instance d;
    patch = clickPatch (d);
    patch.push_back ({ "Swing", 0.66 });
    d.setup (patch);
    d.render (48000 * 2, 128, l, r, from0);
    hits = onsets (l);
    CHECK (hits.size() == 8);
    for (size_t i = 0; i < hits.size(); ++i)
        CHECK (hits[i] == (long) (i / 2) * 24000 + ((i & 1) ? 15840 : 0));
}

void testStopRestartJump()
{
    std::vector<float> l, r;
    // Plays 1 s, stops for 1 s, plays again from the top: hits stop with the transport and start again on frame 0 of
    // the second Play.
    Instance a;
    a.setup (clickPatch (a));
    a.render (48000 * 3, 128, l, r, [] (uint64_t at) {
        if (at < 48000) return playingFrom (0.0, 120.0, 48000.0, at);
        if (at < 96000) return Transport { false, 120.0, 2.0 };
        return playingFrom (0.0, 120.0, 48000.0, at - 96000);
    });
    auto hits = onsets (l);
    CHECK ((hits == std::vector<long> { 0, 12000, 24000, 36000, 96000, 108000, 120000, 132000 }));

    // A jump into the middle of a step waits for the next step; one just after a step's start (a loop's wrap) plays
    // that step at once.
    Instance b;
    b.setup (clickPatch (b));
    constexpr uint64_t jump = 128 * 100;
    b.render (48000 * 2, 128, l, r, [] (uint64_t at) {
        if (at < jump) return playingFrom (0.0, 120.0, 48000.0, at);
        return playingFrom (3.3, 120.0, 48000.0, at - jump); // the next step at beat 3.5: 0.2 beat on
    });
    hits = onsets (l);
    CHECK (std::find (hits.begin(), hits.end(), (long) jump + 4800) != hits.end());
    CHECK (std::find_if (hits.begin(), hits.end(), [&] (long h) { return h > (long) jump && h < (long) jump + 4800; }) == hits.end());

    Instance c;
    c.setup (clickPatch (c));
    constexpr uint64_t wrap = 128 * 40;
    c.render (48000, 128, l, r, [] (uint64_t at) {
        if (at < wrap) return playingFrom (1.0, 120.0, 48000.0, at);
        return playingFrom (100.0 / 24000.0, 120.0, 48000.0, at - wrap); // 100 frames past the top
    });
    hits = onsets (l);
    CHECK (std::find (hits.begin(), hits.end(), (long) wrap) != hits.end());

    // A host with no song position: the sequences count from Play themselves.
    Instance d;
    d.setup (clickPatch (d));
    d.render (48000 * 2, 128, l, r, [] (uint64_t) { return Transport { true, 120.0, 0.0, false }; });
    hits = onsets (l);
    CHECK (hits.size() == 8);
    for (size_t i = 0; i < hits.size(); ++i)
        CHECK (hits[i] == (long) i * 12000);
}

// A busy patch with the dice rolling: the same transport and events heard in blocks of 128, 37, 1 and 4096 frames
// give the same output to the bit, and the same values heard back.
void testBlockSizes()
{
    std::vector<float> l128, r128, l, r;
    auto patch = [] (Instance& a) {
        return Values { { "FM Amt", 0.5 }, { "Fold", 0.4 }, { "Ring Mod Level", 0.4 }, { "Noise Level", 0.3 }, { "Resonance", 0.7 },
                        { "Corrupt 1", 0.8 }, { "Corrupt 2", 0.45 }, { "Clock Div", a.valueOf ("Clock Div", "1/16") }, { "Clock 2", 5.0 },
                        { "Chain Seq", 1.0 }, { "Mod VCO > Fold", 0.3 } };
    };
    auto events = [] (Instance* a) {
        return [a] (uint64_t at, uint32_t n, Events& e) {
            const uint64_t moves[] = { 30000, 41111, 52222 };
            if (moves[0] >= at && moves[0] < at + n) e.param ((uint32_t) (moves[0] - at), a->id ("Cutoff"), 0.3);
            if (moves[1] >= at && moves[1] < at + n) e.param ((uint32_t) (moves[1] - at), a->id ("Bit Flip 2"), 1.0);
            if (moves[2] >= at && moves[2] < at + n) e.note ((uint32_t) (moves[2] - at), CLAP_EVENT_NOTE_ON, 65, 0.8);
        };
    };
    auto t = [] (uint64_t at) { return playingFrom (0.0, 133.0, 48000.0, at); };
    OutEvents first;
    {
        Instance a;
        a.setup (patch (a));
        a.render (48000 * 3, 128, l128, r128, t, events (&a), &first);
    }
    CHECK (first.values.size() > 4); // the dice changed things, and the host heard
    for (uint32_t block : { 37u, 1u, 4096u })
    {
        Instance a;
        a.setup (patch (a));
        OutEvents out;
        a.render (48000 * 3, block, l, r, t, events (&a), &out);
        CHECK (l == l128 && r == r128);
        // The same values (heard at the end of whichever block they fell in).
        std::vector<std::pair<clap_id, double>> x, y;
        for (auto& v : first.values) x.push_back ({ v.id, v.value });
        for (auto& v : out.values) y.push_back ({ v.id, v.value });
        std::map<clap_id, double> lastX, lastY;
        for (auto& [id, v] : x) lastX[id] = v;
        for (auto& [id, v] : y) lastY[id] = v;
        CHECK (lastX == lastY);
    }
}

// Each hit's level: the square root of its energy, from its start to the next's (unlike a peak, the same wherever
// the wave's cycle falls under the envelope).
std::vector<float> peaksOf (const std::vector<float>& x, const std::vector<long>& hits)
{
    std::vector<float> out;
    for (size_t i = 0; i < hits.size(); ++i)
    {
        const long to = i + 1 < hits.size() ? hits[i + 1] : (long) x.size();
        double e = 0.0;
        for (long k = hits[i]; k < to; ++k)
            e += (double) x[(size_t) k] * x[(size_t) k];
        out.push_back ((float) std::sqrt (e));
    }
    return out;
}

void testBitsAndTrigMix()
{
    std::vector<float> l, r;
    // The on bits are the hits: X.XX.X.. at 1/8 plays steps 1, 3, 4, 6, then again.
    {
        Instance a;
        a.setup (clickPatch (a, 1 + 4 + 8 + 32));
        a.render (48000 * 4, 128, l, r, from0);
        const auto steps = stepsOf (onsets (l), 12000);
        CHECK ((steps == std::vector<long> { 0, 2, 3, 5, 8, 10, 11, 13 }));
    }
    // EG Trig Mix: at Seq 1 only SEQ 1's bits play; at Seq 2 only SEQ 2's; in the middle each alone at half, both
    // together at full.
    auto run = [&] (double mix, std::vector<long>& steps, std::vector<float>& peaks) {
        Instance a;
        auto p = clickPatch (a, 1 + 2);   // SEQ 1: steps 1 and 2
        p.push_back ({ "Bits 2", 2 + 4 }); // SEQ 2: steps 2 and 3
        p.push_back ({ "EG Trig Mix", mix });
        p.push_back ({ "VCO Freq", 0.75 }); // 1.28 kHz
        p.push_back ({ "Volume", 0.35 });
        p.push_back ({ "EG2 Decay", 0.3 }); // 74 ms: many cycles a hit, silent again before the next
        p.push_back ({ "Length 1", 4 });
        p.push_back ({ "Length 2", 4 });
        a.setup (p);
        a.render (48000 * 2, 128, l, r, from0);
        const auto hits = onsets (l);
        steps = stepsOf (hits, 12000);
        peaks = peaksOf (l, hits);
    };
    std::vector<long> s;
    std::vector<float> pk;
    run (0.0, s, pk);
    CHECK ((s == std::vector<long> { 0, 1, 4, 5 }));
    run (1.0, s, pk);
    CHECK ((s == std::vector<long> { 1, 2, 5, 6 }));
    run (0.5, s, pk);
    CHECK ((s == std::vector<long> { 0, 1, 2, 4, 5, 6 }));
    if (debug()) std::printf ("debug mix levels: %.4f %.4f %.4f %.4f %.4f %.4f\n", pk[0], pk[1], pk[2], pk[3], pk[4], pk[5]);
    CHECK (pk.size() == 6 && std::fabs (pk[4] / pk[3] - 2.0f) < 0.02f && std::fabs (pk[5] / pk[3] - 1.0f) < 0.01f);
}

// A softer trigger while a louder one is still rising leaves the louder peak: the hit sounds to the bit as the louder
// trigger alone.
void testSofterTriggerInAttack()
{
    std::vector<float> l, r, alone;
    auto patch = [] (Instance& a, int bits1, int bits2) {
        auto p = clickPatch (a, bits1);
        p.push_back ({ "Bits 2", bits2 });
        p.push_back ({ "EG Trig Mix", 0.5 });
        p.push_back ({ "VCO Freq", 0.75 });
        p.push_back ({ "Volume", 0.35 });
        p.push_back ({ "EG2 Decay", 0.3 });
        p.push_back ({ "Length 1", 4 });
        p.push_back ({ "Length 2", 4 });
        return p;
    };
    // Stopped, notes at velocity 1 then 0.4: on one frame (a host that gives a block's notes all on its first frame)
    // and half a millisecond apart (24 frames: the first one's attack half done).
    auto notes = [&] (std::vector<std::pair<uint64_t, double>> list, std::vector<float>& out) {
        Instance a;
        a.setup (patch (a, 0, 0));
        a.render (24000, 128, out, r, stopped, [&] (uint64_t at, uint32_t n, Events& e) {
            for (auto& [when, velocity] : list)
                if (when >= at && when < at + n)
                    e.note ((uint32_t) (when - at), CLAP_EVENT_NOTE_ON, 60, velocity);
        });
    };
    notes ({ { 1280, 1.0 } }, alone);
    CHECK ((onsets (alone) == std::vector<long> { 1280 }));
    notes ({ { 1280, 1.0 }, { 1280, 0.4 } }, l);
    CHECK (l == alone);
    notes ({ { 1280, 1.0 }, { 1304, 0.4 } }, l);
    CHECK (l == alone);
    // A Trigger pressed on a step's frame (the frame's events come before its clock): the step, SEQ 1 alone at half,
    // strikes while the Trigger's full charge is rising. The hit is full, as where both sequences meet.
    auto steps = [&] (int bits2, bool trigger, std::vector<float>& out) {
        Instance a;
        a.setup (patch (a, 1 + 2, bits2));
        a.render (36000, 128, out, r, from0, trigger ? a.pressAt ({ { 12000, "Trigger" } }) : nullptr);
    };
    steps (2, false, alone); // step 2: both sequences, full
    CHECK ((onsets (alone) == std::vector<long> { 0, 12000 }));
    steps (0, true, l);      // step 2: SEQ 1 alone and the Trigger
    CHECK (l == alone);
}

void testLengthShiftFlip()
{
    std::vector<float> l, r;
    auto t = from0;
    // Length 3 on SEQ 1 and 5 on SEQ 2 (both into the EGs): a polymeter, each its own cycle.
    {
        Instance a;
        auto p = clickPatch (a, 1);
        p.push_back ({ "Bits 2", 1 });
        p.push_back ({ "Length 1", 3 });
        p.push_back ({ "Length 2", 5 });
        p.push_back ({ "EG Trig Mix", 0.5 });
        a.setup (p);
        a.render (48000 * 8, 128, l, r, t);
        const auto steps = stepsOf (onsets (l), 12000);
        CHECK ((steps == std::vector<long> { 0, 3, 5, 6, 9, 10, 12, 15, 18, 20, 21, 24, 25, 27, 30 }));
    }
    // Bit Shift: X....... becomes .X......: the host hears Bits 1 change, and the hit moves a step later.
    {
        Instance a;
        a.setup (clickPatch (a, 1));
        OutEvents out;
        a.render (48000 * 4, 128, l, r, t, a.pressAt ({ { 6000, "Bit Shift 1" } }), &out);
        CHECK (a.value ("Bits 1") == 2);
        const auto heard = out.of (a.id ("Bits 1"));
        CHECK (heard.size() == 1 && heard[0] == 2);
        const auto steps = stepsOf (onsets (l), 12000);
        CHECK ((steps == std::vector<long> { 0, 1, 9 })); // the next step is step 2, now on
        // Eight shifts go round to where it began, Length 4 wraps at 4.
        Instance b;
        auto p = clickPatch (b, 8);
        p.push_back ({ "Length 1", 4 });
        b.setup (p);
        b.render (2048, 128, l, r, stopped, b.pressAt ({ { 10, "Bit Shift 1" } }));
        CHECK (b.value ("Bits 1") == 1);
    }
    // Bit Flip: the bit under the write head turns over. Pressed during step 2 with the write head at +0 it is step
    // 2's; at +3, step 5's. Turned on, it takes a new CV, which the host hears too.
    for (int ahead : { 0, 3 })
    {
        Instance a;
        auto p = clickPatch (a, 1);
        p.push_back ({ "Write Head 1", ahead });
        a.setup (p);
        OutEvents out;
        a.render (48000 * 5, 128, l, r, t, a.pressAt ({ { 18000, "Bit Flip 1" } }), &out);
        const int bit = 1 + ahead;
        CHECK (a.value ("Bits 1") == 1 + (1 << bit));
        CHECK (out.of (a.id ("Seq 1 CV " + std::to_string (bit + 1))).size() == 1);
        const auto steps = stepsOf (onsets (l), 12000);
        if (ahead == 0) CHECK ((steps == std::vector<long> { 0, 8, 9, 16, 17 }));
        else CHECK ((steps == std::vector<long> { 0, 4, 8, 12, 16 }));
    }
    // Flipped twice in one step it is off again; only turning on gave it a new CV.
    {
        Instance a;
        a.setup (clickPatch (a, 1));
        OutEvents out;
        a.render (48000, 128, l, r, from0, a.pressAt ({ { 18000, "Bit Flip 1" }, { 20000, "Bit Flip 1" } }), &out);
        CHECK (a.value ("Bits 1") == 1 && out.of (a.id ("Bits 1")).size() == 2 && out.of (a.id ("Seq 1 CV 2")).size() == 1);
    }
}

void testChainResetAdvance()
{
    std::vector<float> l, r;
    // Chain Seq: one ring of SEQ 1's 4 bits then SEQ 2's 4: X... then .X.. plays steps 1 and 6 of each 8.
    {
        Instance a;
        auto p = clickPatch (a, 1);
        p.push_back ({ "Bits 2", 2 });
        p.push_back ({ "Length 1", 4 });
        p.push_back ({ "Length 2", 4 });
        p.push_back ({ "Chain Seq", 1.0 });
        a.setup (p);
        a.render (48000 * 8, 128, l, r, from0);
        CHECK ((stepsOf (onsets (l), 12000) == std::vector<long> { 0, 5, 8, 13, 16, 21, 24, 29 }));
        // Chained, Bit Shift 1 turns the whole ring: SEQ 1's step 4 goes to SEQ 2's step 1 and so on.
        Instance b;
        auto q = clickPatch (b, 8);  // SEQ 1 step 4
        q.push_back ({ "Bits 2", 8 }); // SEQ 2 step 4: the ring's last
        q.push_back ({ "Length 1", 4 });
        q.push_back ({ "Length 2", 4 });
        q.push_back ({ "Chain Seq", 1.0 });
        b.setup (q);
        b.render (2048, 128, l, r, stopped, b.pressAt ({ { 10, "Bit Shift 1" } }));
        CHECK (b.value ("Bits 1") == 1 && b.value ("Bits 2") == 1);
    }
    // Reset: pressed in step 3 (frame 30,000), the next clock plays bit 1 again.
    {
        Instance a;
        a.setup (clickPatch (a, 1));
        a.render (48000 * 3, 128, l, r, from0, a.pressAt ({ { 30000, "Reset" } }));
        CHECK ((onsets (l) == std::vector<long> { 0, 36000, 132000 }));
    }
    // Advance (the sequences stopped: Run off) moves the play heads without playing; Trigger plays both envelopes;
    // the pitch is the CV of the bit the head reached. Bits 1 and 2 on, bit 2 an octave up (CV Range +-5 octaves:
    // 0.2 is +1 octave), the Major scale.
    {
        Instance a;
        a.setup ({ { "Run", 0.0 }, { "Bits 1", 3 }, { "Seq 1 CV 1", 0.0 }, { "Seq 1 CV 2", 0.2 }, { "CV Range 1", 1.0 }, { "VCO Freq", 0.5 },
                   { "Mod VCO Level", 0.0 }, { "FM Amt", 0.0 }, { "Fold", 0.0 }, { "VCW EG1 Amt", 0.0 }, { "VCF EG1 Amt", 0.0 }, { "Cutoff", 1.0 },
                   { "EG2 Decay", 0.8 }, { "Quantize", a.valueOf ("Quantize", "Major") } });
        a.render (48000 * 2, 128, l, r, from0, a.pressAt ({ { 1000, "Advance" }, { 2000, "Trigger" }, { 48000, "Advance" }, { 49000, "Trigger" } }));
        const double first = frequency (l, 4000, 28000), second = frequency (l, 52000, 76000);
        if (debug()) std::printf ("debug advance: %.1f %.1f\n", first, second);
        CHECK (std::fabs (first - 320.0) < 0.5 && std::fabs (second - 640.0) < 1.0);
        // Running, Advance does nothing.
        Instance b;
        b.setup (clickPatch (b, 1));
        b.render (48000 * 3, 128, l, r, from0, b.pressAt ({ { 30000, "Advance" } }));
        CHECK ((onsets (l) == std::vector<long> { 0, 96000 }));
    }
}

void testCorrupt()
{
    std::vector<float> l, r, l2, r2;
    auto patch = [] (Instance& a, double corrupt) {
        auto p = clickPatch (a, 1 + 4 + 16 + 32);
        p.push_back ({ "Corrupt 1", corrupt });
        p.push_back ({ "CV Range 1", 0.5 });
        p.push_back ({ "VCO Freq", 0.5 });
        p.push_back ({ "EG2 Decay", 0.3 });
        return p;
    };
    // Corrupt at 0: the sequence loops as it is (each bar's hits where the last bar's were, at the same pitch) and
    // nothing changes. (Not to the bit: the oscillators run on from where they were.)
    {
        Instance a;
        a.setup (patch (a, 0.0));
        OutEvents out;
        a.render (48000 * 8, 128, l, r, from0, {}, &out);
        const auto hits = onsets (l, 24);
        std::vector<double> pitch;
        for (long h : hits)
            pitch.push_back (frequency (l, (size_t) h + 200, (size_t) h + 3200));
        if (debug()) { std::printf ("debug corrupt 0:"); for (size_t i = 0; i < hits.size(); ++i) std::printf (" %ld/%.1f", hits[i], pitch[i]); std::printf ("\n"); }
        bool loops = hits.size() == 16;
        for (size_t i = 4; loops && i < hits.size(); ++i)
            loops = hits[i] == hits[i - 4] + 96000 && std::fabs (pitch[i] / pitch[i - 4] - 1.0) < 0.003;
        CHECK (loops);
        CHECK (out.values.empty());
    }
    // Up to the middle: new CVs at the write head, the rhythm kept.
    {
        Instance a;
        a.setup (patch (a, 0.45));
        OutEvents out;
        a.render (48000 * 16, 128, l, r, from0, {}, &out);
        const auto steps = stepsOf (onsets (l, 24), 12000);
        bool rhythm = ! steps.empty();
        for (long s : steps)
            rhythm &= s >= 0 && ((1 + 4 + 16 + 32) >> (s % 8) & 1);
        CHECK (rhythm && steps.size() == 32);
        CHECK (out.of (a.id ("Bits 1")).empty() && ! out.values.empty());
        int cvs = 0;
        for (int i = 1; i <= 8; ++i)
            cvs += (int) out.of (a.id ("Seq 1 CV " + std::to_string (i))).size();
        if (debug()) std::printf ("debug corrupt 0.45: %d new CVs in 32 hits\n", cvs);
        CHECK (cvs >= 2 && cvs <= 15); // about 22 % of 32
    }
    // Full: the bits flip too, about half of the steps; the same each time from the same start.
    std::vector<double> once, again;
    for (auto* heard : { &once, &again })
    {
        Instance a;
        a.setup (patch (a, 1.0));
        OutEvents out;
        a.render (48000 * 8, 128, heard == &once ? l : l2, heard == &once ? r : r2, from0, {}, &out);
        *heard = out.of (a.id ("Bits 1"));
    }
    if (debug()) std::printf ("debug corrupt 1: %zu flips in 64 steps\n", once.size());
    CHECK (once.size() >= 16 && once.size() <= 48 && once == again && l == l2);
}

void testPitchAndQuantizer()
{
    // A held note: Drone opens the VCAs; SEQ 1 plays its bit 1 on every step (Length 1), the VCO alone through an
    // open filter.
    auto held = [] (Values extra, std::function<void (uint64_t, uint32_t, Events&)> events = {}, Instance* use = nullptr) {
        Instance own;
        Instance& a = use ? *use : own;
        Values v { { "Drone", 1.0 }, { "Bits 1", 1 }, { "Length 1", 1 }, { "VCO Freq", 0.5 }, { "VCO Level", 0.5 }, { "Mod VCO Level", 0.0 },
                   { "FM Amt", 0.0 }, { "Fold", 0.0 }, { "VCW EG1 Amt", 0.0 }, { "VCO EG1 Amt", 0.0 }, { "Blend", 0.0 }, { "CV Range 1", 1.0 } };
        v.insert (v.end(), extra.begin(), extra.end());
        a.setup (v);
        std::vector<float> l, r;
        a.render (48000, 128, l, r, from0, events);
        return frequency (l, 12000, 48000);
    };
    auto near = [] (double hz, double want) { return std::fabs (hz / want - 1.0) < 0.002; }; // a tenth of a semitone is 0.6 %
    const double root = 320.0; // VCO Freq at the middle: 20 Hz * 2^4
    CHECK (near (held ({ { "Seq 1 CV 1", 0.0 } }), root));
    // CV 0.1 at full CV Range: half an octave, 6 semitones. Unquantized it is that; Major has F or G (6 is between:
    // ties go down, to F, 5); Chromatic has 6; Quads 6; Whole Tone 6; Pentatonic 7 (G).
    struct Case { const char* mode; double semitones; };
    for (const Case& c : { Case { "Unquantized", 6.0 }, Case { "Chromatic", 6.0 }, Case { "Major", 5.0 }, Case { "Pentatonic", 7.0 },
                           Case { "Quads (Minor 3rds)", 6.0 }, Case { "7 Sus 4", 5.0 }, Case { "Major 7th", 7.0 } })
    {
        Instance probe;
        const double hz = held ({ { "Seq 1 CV 1", 0.1 }, { "Quantize", probe.valueOf ("Quantize", c.mode) } });
        if (debug()) std::printf ("debug quantize %s: %.1f Hz (want %.1f)\n", c.mode, hz, root * std::exp2 (c.semitones / 12.0));
        CHECK (near (hz, root * std::exp2 (c.semitones / 12.0)));
    }
    // Below QTZ the CV comes in scaled: half the turn (to QTZ) is half the interval, Major's 5 semitones 2.5.
    CHECK (near (held ({ { "Seq 1 CV 1", 0.1 }, { "VCO Seq 1 Amt", 0.49 } }), root * std::exp2 (2.5 / 12.0)));
    // CV Range: the CV's reach (a quarter of it: +-1.25 octaves; CV 0.4 is half an octave again).
    CHECK (near (held ({ { "Seq 1 CV 1", 0.4 }, { "CV Range 1", 0.25 }, { "Quantize", 0.0 } }), root * std::exp2 (0.5)));
    // With Run off, a MIDI note transposes the quantized sequence by its distance from middle C: G above it 7 semitones
    // up, 48 an octave down, 52 and 35 (off the pads) 8 and 25 semitones down. With Run on every key is a step pad
    // (0.3.0, testDrumBanks): 67 (bank B's pad 16, SEQ 2's bit 8) leaves SEQ 1 where its step put it; Unquantized too.
    auto note = [] (int key) {
        return [key] (uint64_t at, uint32_t, Events& e) {
            if (at == 0) e.note (0, CLAP_EVENT_NOTE_ON, (int16_t) key, 1.0);
        };
    };
    CHECK (near (held ({ { "Seq 1 CV 1", 0.0 }, { "Run", 0.0 } }, note (67)), root * std::exp2 (7.0 / 12.0)));
    CHECK (near (held ({ { "Seq 1 CV 1", 0.0 }, { "Run", 0.0 } }, note (48)), root / 2.0));
    CHECK (near (held ({ { "Seq 1 CV 1", 0.0 }, { "Run", 0.0 } }, note (52)), root * std::exp2 (-8.0 / 12.0)));
    CHECK (near (held ({ { "Seq 1 CV 1", 0.0 }, { "Run", 0.0 } }, note (35)), root * std::exp2 (-25.0 / 12.0)));
    CHECK (near (held ({ { "Seq 1 CV 1", 0.0 } }, note (67)), root));
    CHECK (near (held ({ { "Seq 1 CV 1", 0.0 }, { "Quantize", 0.0 } }, note (67)), root));
    // The Mod VCO at three quarters of its knob: 0.16 Hz * 2^9.75 = 138 Hz, alone, SEQ 2 at QTZ (its CV at 0).
    {
        Instance a;
        a.setup ({ { "Drone", 1.0 }, { "VCO Level", 0.0 }, { "Mod VCO Level", 0.5 }, { "Mod VCO Freq", 0.75 }, { "Fold", 0.0 },
                   { "VCW EG1 Amt", 0.0 }, { "Blend", 0.0 }, { "Bits 2", 1 }, { "Length 2", 1 }, { "Seq 2 CV 1", 0.0 } });
        std::vector<float> l, r;
        a.render (48000, 128, l, r, from0);
        CHECK (near (frequency (l, 12000, 48000), 0.16 * std::exp2 (9.75)));
    }
    // Stopped, a note plays: it triggers both envelopes (at its velocity).
    Instance s;
    s.setup ({ { "Run", 1.0 } });
    std::vector<float> l, r;
    s.render (48000, 128, l, r, stopped, [&] (uint64_t at, uint32_t, Events& e) {
        if (at == 128 * 10) e.note (3, CLAP_EVENT_NOTE_ON, 60, 0.8);
    });
    CHECK ((onsets (l) == std::vector<long> { 1283 }));
}

void testClock2()
{
    // Clock 2 on its own grid: 1/8T (8,000 frames at 120 BPM) for SEQ 2 against SEQ 1's 1/8 (12,000). EG Trig Mix at
    // Seq 2: only SEQ 2's hits, on the triplets.
    std::vector<float> l, r;
    Instance a;
    auto p = clickPatch (a, 0);
    p.push_back ({ "Bits 2", 255 });
    p.push_back ({ "EG Trig Mix", 1.0 });
    p.push_back ({ "Clock 2", a.valueOf ("Clock 2", "1/8T") });
    a.setup (p);
    a.render (48000 * 2, 128, l, r, from0);
    CHECK ((stepsOf (onsets (l), 8000) == std::vector<long> { 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11 }));
    // Both, in the middle: where they meet (every 24,000 frames) the hit is twice as loud as either alone.
    Instance b;
    auto q = clickPatch (b, 255);
    q.push_back ({ "Bits 2", 255 });
    q.push_back ({ "EG Trig Mix", 0.5 });
    q.push_back ({ "Clock 2", b.valueOf ("Clock 2", "1/8T") });
    q.push_back ({ "VCO Freq", 0.75 });
    q.push_back ({ "Volume", 0.35 });
    q.push_back ({ "EG2 Decay", 0.15 }); // 27 ms: silent again within the 4,000 frames between the closest hits
    b.setup (q);
    b.render (48000 * 2, 128, l, r, from0);
    const auto hits = onsets (l);
    CHECK ((hits == std::vector<long> { 0, 8000, 12000, 16000, 24000, 32000, 36000, 40000, 48000, 56000, 60000, 64000, 72000, 80000, 84000, 88000 }));
    const auto peaks = peaksOf (l, hits);
    if (debug()) std::printf ("debug clock 2 levels: %.4f %.4f %.4f\n", peaks[1], peaks[2], peaks[4]);
    CHECK (peaks.size() == 16 && std::fabs (peaks[4] / peaks[1] - 2.0f) < 0.02f && std::fabs (peaks[2] / peaks[1] - 1.0f) < 0.01f);
}

void testRouting()
{
    std::vector<float> a1, b1, l, r;
    auto render = [] (Values v, std::vector<float>& out) {
        Instance a;
        Values base { { "Fold", 0.5 }, { "VCW EG1 Amt", 0.3 }, { "Resonance", 0.6 }, { "Cutoff", 0.4 }, { "FM Amt", 0.3 }, { "Clock Div", 5.0 } };
        base.insert (base.end(), v.begin(), v.end());
        a.setup (base);
        std::vector<float> r;
        a.render (48000 * 2, 128, out, r, from0);
    };
    // The VCW's path is the same in Parallel and VCW > VCF: at Blend VCW the two are the same to the bit; so are
    // Parallel and VCF > VCW at Blend VCF. At the other ends they differ.
    render ({ { "Order", 0.0 }, { "Blend", 0.0 } }, a1);
    render ({ { "Order", 1.0 }, { "Blend", 0.0 } }, b1);
    CHECK (a1 == b1);
    render ({ { "Order", 0.0 }, { "Blend", 1.0 } }, a1);
    render ({ { "Order", 2.0 }, { "Blend", 1.0 } }, b1);
    CHECK (a1 == b1);
    render ({ { "Order", 1.0 }, { "Blend", 1.0 } }, b1);
    CHECK (a1 != b1);
    render ({ { "Order", 2.0 }, { "Blend", 0.0 } }, a1);
    render ({ { "Order", 0.0 }, { "Blend", 0.0 } }, b1);
    CHECK (a1 != b1);

    // Fold: at 0 the VCW passes a sine through (one crossing a cycle); turned up, it folds it into many.
    auto crossingsAt = [] (Values v) {
        Instance a;
        Values base { { "Drone", 1.0 }, { "VCO Freq", 0.5 }, { "VCO Level", 0.5 }, { "Mod VCO Level", 0.0 }, { "FM Amt", 0.0 }, { "Blend", 0.0 },
                      { "VCW EG1 Amt", 0.0 }, { "CV Range 1", 0.0 } };
        base.insert (base.end(), v.begin(), v.end());
        a.setup (base);
        std::vector<float> l, r;
        a.render (48000, 128, l, r, stopped);
        long n = 0;
        for (size_t i = 24001; i < 48000; ++i)
            n += l[i - 1] <= 0.0f && l[i] > 0.0f;
        return n * 2.0; // rising crossings a second
    };
    const double clean = crossingsAt ({ { "Fold", 0.0 } }), folded = crossingsAt ({ { "Fold", 0.8 } });
    if (debug()) std::printf ("debug fold: %.0f %.0f\n", clean, folded);
    CHECK (std::fabs (clean - 320.0) < 4.0 && folded > clean * 2.5);
    // Noise Tone: low to the left (few crossings), high to the right (many).
    const double low = crossingsAt ({ { "VCO Level", 0.0 }, { "Noise Level", 0.5 }, { "Noise Tone", 0.0 }, { "Fold", 0.0 } });
    const double high = crossingsAt ({ { "VCO Level", 0.0 }, { "Noise Level", 0.5 }, { "Noise Tone", 1.0 }, { "Fold", 0.0 } });
    if (debug()) std::printf ("debug noise tone: %.0f %.0f\n", low, high);
    CHECK (high > low * 4.0);
    // Drone: stopped and never triggered, it sounds; without it, silence.
    Instance d;
    d.setup ({ { "Drone", 1.0 } });
    d.render (24000, 128, l, r, stopped);
    float loud = 0.0f;
    for (float s : l)
        loud = std::max (loud, std::fabs (s));
    CHECK (loud > 0.05f);
    Instance q;
    q.render (24000, 128, l, r, stopped);
    CHECK (onsets (l).empty());
}

void testExtremes()
{
    // Random settings, every one in its range: the output stays a number and within full scale.
    std::mt19937 random (7);
    std::vector<float> l, r;
    for (int round = 0; round < 60; ++round)
    {
        Instance a;
        Values values;
        for (uint32_t i = 0; i < a.params->count (a.plugin); ++i)
        {
            clap_param_info info {};
            a.params->get_info (a.plugin, i, &info);
            if (info.flags & CLAP_PARAM_IS_ENUM && info.max_value == 1.0 && info.min_value == 0.0 && std::string (info.name) != "Drone"
                && std::string (info.name) != "Run" && std::string (info.name) != "Chain Seq")
                continue; // the buttons
            std::uniform_real_distribution<double> d (info.min_value, info.max_value);
            double v = round < 2 ? (round == 0 ? info.min_value : info.max_value) : d (random);
            if (info.flags & CLAP_PARAM_IS_STEPPED)
                v = std::round (v);
            values.push_back ({ info.name, v });
        }
        a.setup (values);
        a.render (24000, 128, l, r, [] (uint64_t at) { return playingFrom (0.0, 170.0, 48000.0, at); },
                  a.pressAt ({ { 3000, "Bit Flip 1" }, { 7000, "Bit Shift 2" }, { 9000, "Trigger" }, { 15000, "Save" }, { 20000, "Recall" } }));
        bool finite = true;
        float peak = 0.0f;
        for (float s : l)
            finite &= std::isfinite (s), peak = std::max (peak, std::fabs (s));
        CHECK (finite && peak <= 1.0f);
    }
}

// Each hit's energy (as peaksOf), for telling a hit struck twice from one struck once.
std::vector<float> energies (const std::vector<float>& x, const std::vector<long>& hits) { return peaksOf (x, hits); }

void testClockChanges()
{
    std::vector<float> l, r;
    // Clock Div changed on a step's frame (beat 2, frame 48,000, from 1/8 to 1/16): that step still plays, and the
    // new grid goes on from it.
    for (const char* to : { "1/16", "1/4", "1/8T" })
    {
        Instance a;
        a.setup (clickPatch (a));
        const double div = a.valueOf ("Clock Div", to);
        a.render (96000, 128, l, r, from0, [&] (uint64_t at, uint32_t n, Events& e) {
            if (48000 >= at && 48000 < at + n) e.param ((uint32_t) (48000 - at), a.id ("Clock Div"), div);
        });
        const auto hits = onsets (l);
        printHits (to, hits);
        const long next = std::string (to) == "1/16" ? 54000 : std::string (to) == "1/4" ? 72000 : 56000;
        CHECK (std::find (hits.begin(), hits.end(), 48000L) != hits.end() && std::find (hits.begin(), hits.end(), next) != hits.end());
    }
    // Clock Div changed on a step of Clock 2's own grid (1/8T: frame 40,000) leaves Clock 2 alone: its step plays.
    {
        Instance a;
        auto p = clickPatch (a, 0);
        p.push_back ({ "Bits 2", 255 });
        p.push_back ({ "EG Trig Mix", 1.0 });
        p.push_back ({ "Clock 2", a.valueOf ("Clock 2", "1/8T") });
        a.setup (p);
        a.render (96000, 128, l, r, from0, [&] (uint64_t at, uint32_t n, Events& e) {
            if (40000 >= at && 40000 < at + n) e.param ((uint32_t) (40000 - at), a.id ("Clock Div"), a.valueOf ("Clock Div", "1/16"));
        });
        CHECK ((stepsOf (onsets (l), 8000) == std::vector<long> { 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11 }));
    }
    // A preset (here a state) loaded between two blocks, the next block's first frame a step: that step plays at the
    // new Clock Div.
    {
        Instance a;
        a.setup (clickPatch (a));
        Instance b;
        auto q = clickPatch (b);
        q.push_back ({ "Clock Div", b.valueOf ("Clock Div", "1/16") });
        b.setup (q);
        const auto state = saveState (b);
        a.render (48000, 128, l, r, from0);
        CHECK (loadState (a, state));
        a.render (24000, 128, l, r, [] (uint64_t at) { return playingFrom (2.0, 120.0, 48000.0, at); });
        CHECK ((onsets (l) == std::vector<long> { 0, 6000, 12000, 18000 }));
    }
    // A host whose tempo ramps from 120 to 240 BPM over a second, its count kept per frame while each block hears the
    // tempo at its start: our count drifts from the host's by a little each block. Every clock plays once, near its
    // time: none twice (a hit struck twice would be louder), none lost.
    for (uint32_t block : { 128u, 256u, 512u })
    {
        Instance a;
        a.setup (clickPatch (a));
        auto beatsAt = [] (double t) { return t <= 1.0 ? 2.0 * t + t * t : 3.0 + 4.0 * (t - 1.0); };
        a.render (96000, block, l, r, [&] (uint64_t at) {
            const double t = at / 48000.0;
            return Transport { true, 120.0 + 120.0 * std::min (t, 1.0), beatsAt (t) };
        });
        const auto hits = onsets (l);
        const auto e = energies (l, hits);
        bool placed = hits.size() == 14; // clocks at 0, 0.5 ... 6.5 beats
        for (size_t k = 0; placed && k < hits.size(); ++k)
        {
            const double b = 0.5 * k, t = b <= 3.0 ? -1.0 + std::sqrt (1.0 + b) : 1.0 + (b - 3.0) / 4.0;
            placed = std::labs (hits[k] - std::lround (t * 48000.0)) <= (long) block;
        }
        const float most = *std::max_element (e.begin(), e.end()), least = *std::min_element (e.begin(), e.end());
        if (debug()) std::printf ("debug ramp %u: %zu hits, energy %.4f..%.4f\n", block, hits.size(), least, most);
        CHECK (placed && most < least * 1.1f);
    }
    // A tempo no host should send (infinite, 1e13 BPM): taken as 1000 BPM; the block returns, its output finite.
    for (double bpm : { 1.0e13, (double) INFINITY })
    {
        Instance a;
        a.setup (clickPatch (a));
        a.render (4096, 128, l, r, [bpm] (uint64_t at) { return Transport { true, bpm, (double) at * 1000.0 / 60.0 / 48000.0 }; });
        bool finite = true;
        for (float s : l)
            finite &= std::isfinite (s);
        CHECK (finite && ! onsets (l).empty());
    }
    // A song position before the top (a pre-roll from beat -1): steps there are the ring's last ones; bit 1 lands on
    // the top.
    {
        Instance a;
        a.setup (clickPatch (a, 1));
        a.render (144000, 128, l, r, [] (uint64_t at) { return playingFrom (-1.0, 120.0, 48000.0, at); });
        printHits ("pre-roll", onsets (l));
        CHECK ((onsets (l) == std::vector<long> { 24000, 120000 }));
    }
}

void testOffsetsThroughLoops()
{
    std::vector<float> l, r;
    // A two-bar loop (16 eighths) going round: what Reset did stays. Reset in step 3 makes bit 1 play on step 4; after
    // the wrap it still does.
    auto loop = [] (uint64_t at) { return playingFrom (0.0, 120.0, 48000.0, at % 192000); };
    {
        Instance a;
        a.setup (clickPatch (a, 1));
        a.render (384000, 128, l, r, loop, a.pressAt ({ { 30000, "Reset" } }));
        CHECK ((stepsOf (onsets (l), 12000) == std::vector<long> { 0, 3, 11, 19, 27 }));
    }
    // Chained, Bit Shift 2 opens a gap of a bit between the heads (SEQ 2's ahead); the loop keeps it. Only SEQ 1's
    // ring bit 1 is on: head 1 plays it on steps 0, 8; head 2 on steps 7, 15.
    {
        Instance a;
        auto p = clickPatch (a, 1);
        p.push_back ({ "Bits 2", 0 });
        p.push_back ({ "Length 1", 4 });
        p.push_back ({ "Length 2", 4 });
        p.push_back ({ "Chain Seq", 1.0 });
        p.push_back ({ "EG Trig Mix", 0.5 });
        a.setup (p);
        a.render (384000, 128, l, r, loop, a.pressAt ({ { 6000, "Bit Shift 2" } }));
        CHECK ((stepsOf (onsets (l), 12000) == std::vector<long> { 0, 7, 8, 15, 16, 23, 24, 31 }));
        // Played again from a stop, the heads start together.
        a.render (48000, 128, l, r, stopped);
        a.render (24000, 128, l, r, from0);
        CHECK ((onsets (l) == std::vector<long> { 0 }));
    }
    // Run off holds the sequence (no steps), Run on goes on from where it held at the next clock. Bits 1 and 3 on.
    {
        Instance a;
        a.setup (clickPatch (a, 1 + 4));
        a.render (144000, 128, l, r, from0, [&] (uint64_t at, uint32_t n, Events& e) {
            if (30000 >= at && 30000 < at + n) e.param ((uint32_t) (30000 - at), a.id ("Run"), 0.0);
            if (60000 >= at && 60000 < at + n) e.param ((uint32_t) (60000 - at), a.id ("Run"), 1.0);
        });
        CHECK ((onsets (l) == std::vector<long> { 0, 24000, 120000 }));
    }
    // Lengths and Chain Seq changed while it plays, the heads past the ring's new end: they go round, the steps stay
    // on the grid, nothing breaks.
    {
        Instance a;
        auto p = clickPatch (a, 0x55);
        p.push_back ({ "Bits 2", 0xaa });
        p.push_back ({ "EG Trig Mix", 0.5 });
        p.push_back ({ "Clock Div", a.valueOf ("Clock Div", "1/16") });
        a.setup (p);
        a.render (240000, 128, l, r, from0, [&] (uint64_t at, uint32_t n, Events& e) {
            const struct { uint64_t when; const char* name; double v; } moves[] = {
                { 20000, "Chain Seq", 1.0 }, { 50000, "Length 1", 2.0 }, { 70000, "Bit Shift 2", 1.0 }, { 90000, "Length 2", 1.0 },
                { 110000, "Chain Seq", 0.0 }, { 130000, "Length 1", 7.0 }, { 150000, "Chain Seq", 1.0 }, { 170000, "Length 2", 8.0 } };
            for (auto& m : moves)
                if (m.when >= at && m.when < at + n)
                    e.param ((uint32_t) (m.when - at), a.id (m.name), m.v);
        });
        const auto hits = onsets (l);
        bool grid = hits.size() > 20;
        for (long h : hits)
            grid &= h % 6000 == 0;
        CHECK (grid);
    }
}

// Melodee tells a plug-in the song's place only at its own blocks, counting on through a block as if it did not loop:
// a loop whose end falls inside a block is told at the next block's start, its first step already played on the
// right frame (the loop's end is its start). That step must not play again.
void testMidBlockLoop()
{
    std::vector<float> l, r;
    // A 2-bar loop ending 8 frames into a block (177,160 frames: about 130.05 BPM), told 120 frames late.
    constexpr double loopFrames = 128.0 * 1384 + 8, bpm = 8.0 * 60.0 * 48000.0 / loopFrames;
    auto melodee = [] (uint64_t at) { return Transport { true, bpm, std::fmod ((double) at * bpm / 60.0 / 48000.0, 8.0) }; };
    {
        Instance a;
        a.setup (clickPatch (a));
        a.render ((uint32_t) (3 * loopFrames), 128, l, r, melodee);
        const auto hits = onsets (l);
        const auto e = energies (l, hits);
        bool placed = hits.size() == 48;
        for (size_t k = 0; placed && k < hits.size(); ++k)
            placed = std::labs (hits[k] - (long) std::ceil (k * loopFrames / 16.0 - 1.0e-6)) <= 1;
        const float most = *std::max_element (e.begin(), e.end()), least = *std::min_element (e.begin(), e.end());
        if (debug()) std::printf ("debug mid-block loop: %zu hits, energy %.4f..%.4f\n", hits.size(), least, most);
        CHECK (placed && most < least * 1.1f); // a step struck twice would be louder
    }
    // A ring of 5 (bit 1 only) in the 16-step loop: the step the count ran on to at the loop's end is silent, the
    // loop's first is not: it sounds, at the start of the block that tells of the wrap.
    {
        Instance a;
        auto p = clickPatch (a, 1);
        p.push_back ({ "Length 1", 5 });
        a.setup (p);
        a.render ((uint32_t) (1.5 * loopFrames), 128, l, r, melodee);
        const auto hits = onsets (l);
        printHits ("mid-block loop, ring 5", hits);
        CHECK (std::find (hits.begin(), hits.end(), (long) loopFrames + 120) != hits.end());
        CHECK (std::find (hits.begin(), hits.end(), (long) loopFrames) == hits.end());
    }
    // Both sequences into the EGs (EG Trig Mix in the middle), at 1/4. The loop's first step is both (full); the step
    // the count ran on to is SEQ 2's alone (half): it struck on the loop's last frame, and SEQ 1's bit sounds a block
    // late. The downbeat is still full, as on the first pass and as a host that tells of the wrap on its own frame
    // (8-frame blocks) plays it. Apart: SEQ 1 a ring of 5, SEQ 2 of 4, bit 1 on in each. Chained: one ring of 5, its
    // bits 1, 2 and 5 on, SEQ 2's head a bit ahead (Bit Shift 2). At EG Trig Mix 1 only SEQ 2 counts: it struck at
    // full on the loop's last frame, so SEQ 1's late bit adds nothing and the downbeat strikes once, not again.
    for (float mix : { 0.5f, 0.0f, 1.0f })
    for (bool chained : { false, true })
    {
        auto downbeats = [&] (uint32_t block, const std::function<Transport (uint64_t)>& transport) {
            Instance a;
            auto p = clickPatch (a, chained ? 1 + 2 : 1);
            p.push_back ({ "Bits 2", chained ? 2 : 1 });
            p.push_back ({ "Length 1", chained ? 3 : 5 });
            p.push_back ({ "Length 2", chained ? 2 : 4 });
            p.push_back ({ "Chain Seq", chained ? 1.0 : 0.0 });
            p.push_back ({ "EG Trig Mix", mix });
            p.push_back ({ "Clock Div", a.valueOf ("Clock Div", "1/4") });
            p.push_back ({ "VCO Freq", 0.75 });
            p.push_back ({ "Volume", 0.35 });
            p.push_back ({ "EG2 Decay", 0.45 }); // 203 ms: long against the block, silent before the next step
            a.setup (p);
            a.render ((uint32_t) (3 * loopFrames), block, l, r, transport, chained ? a.pressAt ({ { 6000, "Bit Shift 2" } }) : nullptr);
            const auto hits = onsets (l);
            const auto e = energies (l, hits);
            std::vector<float> out;
            for (size_t k = 0; k < hits.size(); ++k)
                if (hits[k] % (long) loopFrames <= 120)
                    out.push_back (e[k]);
            printHits ("loop downbeats, onsets", hits);
            if (debug()) for (float x : out) std::printf ("debug loop downbeat energy %.4f\n", x);
            return out;
        };
        const auto late = downbeats (128, melodee);
        const auto onTime = downbeats (8, [] (uint64_t at) { return playingFrom (0.0, bpm, 48000.0, at % (uint64_t) loopFrames); });
        CHECK (late.size() == 3 && onTime.size() == 3);
        for (size_t k = 0; k < std::min (late.size(), onTime.size()); ++k)
        {
            CHECK (std::fabs (onTime[k] / onTime[0] - 1.0f) < 0.005f);
            CHECK (std::fabs (late[k] / onTime[k] - 1.0f) < 0.03f);
        }
    }
}

void testHostEdges()
{
    std::vector<float> l, r, l2, r2;
    // Drone switched on while nothing sounds: it sounds from that frame, whatever the block size.
    std::vector<float> first;
    for (uint32_t block : { 128u, 1u, 37u, 4096u })
    {
        Instance a;
        a.render (24000, block, l, r, stopped, [&] (uint64_t at, uint32_t n, Events& e) {
            if (1000 >= at && 1000 < at + n) e.param ((uint32_t) (1000 - at), a.id ("Drone"), 1.0);
        });
        if (block == 128)
        {
            first = l;
            CHECK ((onsets (l) == std::vector<long> { 1000 }));
        }
        else
            CHECK (l == first);
    }
    // With Run off (0.3.0: with Run on every key is a step pad, which plays), a note on Play's first frame and on
    // Stop's first frame both play: the sequences do not run.
    {
        Instance a;
        auto v = clickPatch (a, 0);
        v.push_back ({ "Run", 0.0 });
        a.setup (v);
        a.render (48000, 128, l, r, [] (uint64_t at) {
            if (at < 12800) return Transport { false, 120.0, 0.0 };
            if (at < 25600) return playingFrom (0.0, 120.0, 48000.0, at - 12800);
            return Transport { false, 120.0, 0.0 };
        }, [&] (uint64_t at, uint32_t, Events& e) {
            if (at == 12800 || at == 25600) e.note (0, CLAP_EVENT_NOTE_ON, 60, 1.0);
        });
        CHECK ((onsets (l) == std::vector<long> { 12800, 25600 }));
    }
    // A Reset sent with Play sticks (Play places the heads, then the Reset moves them): bit 1 on the second step.
    {
        Instance a;
        a.setup (clickPatch (a, 1));
        a.render (48000, 128, l, r, [] (uint64_t at) { return at < 12800 ? Transport { false, 120.0, 0.0 } : playingFrom (1.0, 120.0, 48000.0, at - 12800); },
                  a.pressAt ({ { 12800, "Reset" } }));
        CHECK ((onsets (l) == std::vector<long> { 12800 }));
    }
    // MIDI 1.0 notes (the MIDI dialect) do what CLAP notes do: stopped, a note on plays; a note on at velocity 0 is a
    // note off and plays nothing.
    {
        Instance a;
        a.setup (clickPatch (a, 0));
        a.render (48000, 128, l, r, stopped, [&] (uint64_t at, uint32_t, Events& e) {
            if (at == 1280) e.midi (3, 0x90, 64, 100);
            if (at == 25600) e.midi (0, 0x90, 64, 0);
        });
        CHECK ((onsets (l) == std::vector<long> { 1283 }));
    }
    // Flush (no process call), active: values and buttons act as in process; the host hears what the engine changed.
    {
        Instance a;
        a.setup ({ { "Bits 1", 1 } });
        Events e;
        e.param (0, a.id ("Bits 1"), 3);
        e.param (0, a.id ("Bit Flip 1"), 1.0); // the write head is on bit 1: 3 becomes 2
        OutEvents out;
        a.params->flush (a.plugin, &e.in, &out.out);
        CHECK (a.value ("Bits 1") == 2 && out.of (a.id ("Bits 1")).size() == 1 && out.of (a.id ("Bit Flip 1")).size() == 1);
    }
    // Flush before activation (the main thread, nothing running): the same.
    {
        const auto* d = factory->get_plugin_descriptor (factory, 0);
        const clap_plugin* plugin = factory->create_plugin (factory, &host, d->id);
        CHECK (plugin->init (plugin));
        const auto* params = static_cast<const clap_plugin_params*> (plugin->get_extension (plugin, CLAP_EXT_PARAMS));
        Instance names;
        Events e;
        e.param (0, names.id ("Bits 2"), 6);
        e.param (0, names.id ("Bit Shift 2"), 1.0);
        OutEvents out;
        params->flush (plugin, &e.in, &out.out);
        double v = 0.0;
        CHECK (params->get_value (plugin, names.id ("Bits 2"), &v) && v == 12);
        plugin->destroy (plugin);
    }
    // A state loaded while a block plays, in the block where Save is pressed: the loaded buffer stands.
    {
        Instance g;
        g.setup ({ { "Bits 1", 128 } });
        Instance h;
        h.setup ({ { "Bits 1", 15 } });
        h.render (256, 128, l, r, stopped, h.pressAt ({ { 0, "Save" } }));
        h.setup ({ { "Bits 1", 7 } });
        const auto loaded = saveState (h); // values: Bits 7; its buffer: Bits 15
        g.render (256, 128, l, r, stopped, [&] (uint64_t at, uint32_t, Events& e) {
            if (at != 128)
                return;
            e.param (10, g.id ("Save"), 1.0);
            e.param (100, g.id ("Volume"), 0.7);
            e.hookAt = 1;
            e.hook = [&] { CHECK (loadState (g, loaded)); };
        });
        CHECK (g.value ("Bits 1") == 7);
        g.render (256, 128, l, r, stopped, g.pressAt ({ { 10, "Recall" } }));
        CHECK (g.value ("Bits 1") == 15);
    }
    // ...and where the engine changes a value to something else and back is the loaded one (a state with the same
    // Bits as before the Bit Shift): the loaded value stands there too, and the engine has it from the next block.
    {
        Instance g;
        g.setup (clickPatch (g, 1));
        const auto same = saveState (g);
        g.render (256, 128, l, r, from0, [&] (uint64_t at, uint32_t, Events& e) {
            if (at != 128)
                return;
            e.param (10, g.id ("Bit Shift 1"), 1.0); // the engine: Bits 1 -> 2
            e.param (100, g.id ("Volume"), 0.6);
            e.hookAt = 1;
            e.hook = [&] { CHECK (loadState (g, same)); }; // the main thread: Bits 1 (as it was)
        });
        CHECK (g.value ("Bits 1") == 1);
        g.render (96000, 128, l, r, [] (uint64_t at) { return playingFrom (0.0, 120.0, 48000.0, at + 256); });
        // Bit 1 again at the next bar (this render starts at song frame 256, the last hit still ringing into it), not
        // bit 2 at the next step.
        auto hits = onsets (l);
        hits.erase (std::remove_if (hits.begin(), hits.end(), [] (long h) { return h < 2000; }), hits.end());
        CHECK ((hits == std::vector<long> { 96000 - 256 }));
    }
    // Odd text: empty, not a number, padded, a choice by number only when nothing follows it.
    {
        Instance a;
        double v = -1.0;
        CHECK (! a.params->text_to_value (a.plugin, a.id ("Cutoff"), "", &v));
        CHECK (! a.params->text_to_value (a.plugin, a.id ("Cutoff"), "nan", &v));
        CHECK (! a.params->text_to_value (a.plugin, a.id ("Volume"), "inf %", &v));
        CHECK (! a.params->text_to_value (a.plugin, a.id ("Clock Div"), "1/16x", &v));
        CHECK (! a.params->text_to_value (a.plugin, a.id ("Clock Div"), "1/3", &v));
        CHECK (! a.params->text_to_value (a.plugin, a.id ("Clock Div"), "12", &v));
        CHECK (a.valueOf ("Clock Div", " 1/16 ") == 5.0 && a.valueOf ("Clock Div", "5") == 5.0 && a.valueOf ("Clock 2", " 1/16") == 6.0);
        CHECK (a.valueOf ("Cutoff", " 1 kHz ") == a.valueOf ("Cutoff", "1000 Hz"));
        // Display at a unit's edge, and two-way shares adding up to 100.
        CHECK (a.text ("Cutoff", a.valueOf ("Cutoff", "999.97 Hz")) == "1.00 kHz");
        CHECK (a.text ("EG1 Decay", a.valueOf ("EG1 Decay", "999.8 ms")) == "1.00 s");
        CHECK (a.text ("Filter Mode", 0.59) == "LP 41 / BP 59" && a.text ("Blend", 0.004) == "VCW");
        CHECK (a.text ("VCO Seq 1 Amt", 0.979) == "99 %");
    }
}

// 0.2.0: with Run on (the host playing or stopped), notes 36-51 are step pads: 36-43 play SEQ 1's bits 1-8, 44-51
// SEQ 2's (chained, the ring's 16 bits in that order). A pad plays its bit as the sequencer plays it (its CV through CV
// Range, the quantizer and the transposition; both envelopes at the pad's velocity), whether it is on or off, without
// moving a head or changing anything the host must hear. Other notes transpose as before; with Run off every note is
// the keyboard again (it transposes, and stopped it plays).
void testStepPads()
{
    std::vector<float> l, r, want, wantR;
    struct Hit { uint64_t when; int key; double velocity = 1.0; uint16_t type = CLAP_EVENT_NOTE_ON; };
    auto hits = [] (std::vector<Hit> list) {
        return [list] (uint64_t at, uint32_t n, Events& e) {
            for (const auto& h : list)
                if (h.when >= at && h.when < at + n)
                    e.note ((uint32_t) (h.when - at), h.type, (int16_t) h.key, h.velocity);
        };
    };
    auto energy = [] (const std::vector<float>& x, size_t from, size_t to) {
        double e = 0.0;
        for (size_t i = from; i < to && i < x.size(); ++i)
            e += (double) x[i] * x[i];
        return e;
    };
    // Both sequencers heard: SEQ 1 on the VCO's pitch and Fold, SEQ 2 on the Mod VCO's pitch and the cutoff; every bit
    // its own CV; Chromatic; Clock Div 1/16 (6,000 frames).
    auto patch = [] (Instance& a, int bits1, int bits2, double mix, bool chained, double run = 1.0) {
        Values v { { "Clock Div", a.valueOf ("Clock Div", "1/16") }, { "Bits 1", (double) bits1 }, { "Bits 2", (double) bits2 },
                   { "EG Trig Mix", mix }, { "Chain Seq", chained ? 1.0 : 0.0 }, { "Run", run },
                   { "Quantize", a.valueOf ("Quantize", "Chromatic") }, { "CV Range 1", 0.5 }, { "CV Range 2", 0.5 },
                   { "VCW Seq 1 Amt", 0.4 }, { "VCF Seq 2 Amt", 0.6 }, { "Mod VCO Level", 0.4 }, { "Mod VCO Freq", 0.6 },
                   { "EG2 Decay", 0.3 }, { "Fold", 0.2 } };
        for (int i = 0; i < 8; ++i)
        {
            v.push_back ({ "Seq 1 CV " + std::to_string (i + 1), -0.7 + 0.19 * i });
            v.push_back ({ "Seq 2 CV " + std::to_string (i + 1), 0.65 - 0.17 * i });
        }
        return v;
    };

    // Each pad against the sequencer playing its bit (that bit alone on, playing from the top, at its step's frame):
    // the pad pressed on that frame sounds the same, to the bit, and the host hears nothing. Stopped, with the bit on
    // and with it off; playing with every bit off (only the pad sounds; for bit 1 that is Play's own frame); and chained
    // (the ring's bit k, on both outputs, as both heads together play it). (Before 0.3.0 the playing case was also
    // transposed by a note outside the pads; with Run on every key is a pad now: testDrumBanks.)
    int matched = 0, cases = 0;
    for (int chained = 0; chained < 2; ++chained)
        for (int k = 0; k < 16; ++k)
            for (int variant = 0; variant < 3; ++variant) // stopped and on; stopped and off; playing, all off
            {
                const int lane = k >> 3, one = 1 << (k & 7);
                const uint64_t at = 6000u * (uint64_t) (chained ? k : k & 7);
                const uint32_t frames = (uint32_t) at + 6000;
                const double mix = chained ? 0.5 : (lane == 0 ? 0.0 : 1.0);
                std::vector<Hit> top;
                {
                    Instance a;
                    a.setup (patch (a, lane == 0 ? one : 0, lane == 1 ? one : 0, mix, chained));
                    a.render (frames, 128, want, wantR, from0, hits (top));
                }
                Instance p;
                const bool on = variant == 0;
                p.setup (patch (p, on && lane == 0 ? one : 0, on && lane == 1 ? one : 0, mix, chained));
                auto pressed = top;
                pressed.push_back ({ at, 36 + k });
                OutEvents out;
                p.render (frames, 128, l, r, variant == 2 ? from0 : stopped, hits (pressed), &out);
                const bool same = onsets (want) == std::vector<long> { (long) at } && l == want && r == wantR && out.values.empty();
                ++cases;
                matched += same ? 1 : 0;
                if (! same && debug())
                    std::printf ("debug pad %d chained %d variant %d differs from its step\n", 36 + k, chained, variant);
            }
    CHECK (cases == 96 && matched == cases);

    // Measured: the VCO alone, held open by Drone, SEQ 1 at full CV Range in Major (CV 0.1 is 6 semitones, which Major
    // makes 5, a tie going down; 0.2 an octave).
    const double root = 320.0;
    auto near = [] (double hz, double target) { return std::fabs (hz / target - 1.0) < 0.002; };
    auto pitches = [&] (Values extra, std::vector<Hit> list, std::vector<std::pair<size_t, size_t>> windows,
                        const std::function<Transport (uint64_t)>& transport, std::vector<std::pair<uint64_t, std::string>> presses = {}) {
        Instance a;
        Values v { { "Drone", 1.0 }, { "VCO Freq", 0.5 }, { "VCO Level", 0.5 }, { "Mod VCO Level", 0.0 }, { "FM Amt", 0.0 }, { "Fold", 0.0 },
                   { "VCW EG1 Amt", 0.0 }, { "VCO EG1 Amt", 0.0 }, { "Blend", 0.0 }, { "CV Range 1", 1.0 },
                   { "Quantize", a.valueOf ("Quantize", "Major") }, { "Seq 1 CV 1", 0.0 }, { "Seq 1 CV 3", 0.1 }, { "Seq 1 CV 4", 0.2 },
                   { "Seq 1 CV 5", -0.2 }, { "Seq 1 CV 6", 0.2 }, { "Seq 2 CV 1", 0.2 } };
        v.insert (v.end(), extra.begin(), extra.end());
        a.setup (v);
        auto notes = hits (list);
        auto press = a.pressAt (presses);
        a.render (96000, 128, l, r, transport, [&] (uint64_t at, uint32_t n, Events& e) { notes (at, n, e); press (at, n, e); });
        std::vector<double> hz;
        for (const auto& [from, to] : windows)
            hz.push_back (frequency (l, from, to));
        if (debug())
        {
            std::printf ("debug pad pitches:");
            for (double h : hz) std::printf (" %.1f", h);
            std::printf ("\n");
        }
        return hz;
    };
    const auto all = std::vector<std::pair<size_t, size_t>> { { 4000, 46000 }, { 52000, 94000 } };
    // Stopped: pad 38 (SEQ 1's bit 3) plays its CV, quantized; transposed a fifth by a note played with Run off (67;
    // 0.3.0: with Run on every key is a pad), Run then switched on.
    auto hz = pitches ({}, { { 0, 38 } }, all, stopped);
    CHECK (near (hz[0], root * std::exp2 (5.0 / 12.0)) && near (hz[1], root * std::exp2 (5.0 / 12.0)));
    hz = pitches ({ { "Run", 0.0 } }, { { 0, 67 }, { 2000, 38 } }, all, stopped, { { 1000, "Run" } });
    CHECK (near (hz[0], root * 2.0));
    // A pad never transposes: after the fifth, pad 50 (SEQ 2's bit 7) leaves SEQ 1 and the transposition as they were.
    hz = pitches ({ { "Run", 0.0 } }, { { 0, 67 }, { 48000, 50 } }, all, stopped, { { 1000, "Run" } });
    CHECK (near (hz[0], root * std::exp2 (7.0 / 12.0)) && near (hz[1], root * std::exp2 (7.0 / 12.0)));
    // Unquantized: the CV as it is (6 semitones), and with Run on the transposition is not heard (with Run off it is
    // since 0.2.1: testUnquantizedKeys).
    hz = pitches ({ { "Quantize", 0.0 }, { "Run", 0.0 } }, { { 0, 67 }, { 2000, 38 } }, all, stopped, { { 1000, "Run" } });
    CHECK (near (hz[0], root * std::exp2 (6.0 / 12.0)));
    // SEQ 2's pads move only SEQ 2 apart; chained, the ring's bit goes to both outputs (SEQ 2's bit 1 at CV 0.2: up an
    // octave on the VCO too).
    hz = pitches ({}, { { 0, 44 } }, all, stopped);
    CHECK (near (hz[0], root));
    hz = pitches ({ { "Chain Seq", 1.0 } }, { { 0, 44 } }, all, stopped);
    CHECK (near (hz[0], root * 2.0));
    // Run off: the pads are the keyboard again: each note transposes by its distance from middle C.
    hz = pitches ({ { "Run", 0.0 } }, { { 0, 38 }, { 48000, 50 } }, all, stopped);
    CHECK (near (hz[0], root * std::exp2 (-22.0 / 12.0)) && near (hz[1], root * std::exp2 (-10.0 / 12.0)));
    // Playing (bits 1 and 5 on, at 1/8): a pad's CV holds until that sequencer's next on bit. Pad 39 (bit 4, an octave
    // up) in step 2 holds through the off steps 3 and 4 (the play head did not move: from bit 4 the next clock would
    // have played bit 5), SEQ 2's pad 44 leaves it, and step 5 replaces it (CV -0.2: an octave down).
    hz = pitches ({ { "Bits 1", 1 + 16 } }, { { 18000, 39 }, { 30000, 44 } }, { { 2000, 17000 }, { 20000, 29000 }, { 32000, 46000 }, { 50000, 70000 } }, from0);
    CHECK (near (hz[0], root) && near (hz[1], root * 2.0) && near (hz[2], root * 2.0) && near (hz[3], root / 2.0));
    // Stopped, the play head stays too: Advance after pad 41 (bit 6) goes on from where the head was, to bit 1.
    hz = pitches ({ { "Bits 1", 255 } }, { { 1000, 41 } }, { { 3000, 29000 }, { 32000, 90000 } }, stopped, { { 30000, "Advance" } });
    CHECK (near (hz[0], root * 2.0) && near (hz[1], root));

    // Playing: pads between the steps sound where they are pressed and leave the steps as they were (the same frames,
    // the same level), Bits and CVs as they were, nothing heard back.
    {
        std::vector<float> plain;
        Instance a;
        a.setup (clickPatch (a, 1 + 4));
        a.render (144000, 128, plain, r, from0);
        Instance b;
        b.setup (clickPatch (b, 1 + 4));
        OutEvents out;
        b.render (144000, 128, l, r, from0, hits ({ { 7000, 37 }, { 31000, 42, 0.6 }, { 55000, 47 }, { 80000, 51 } }), &out);
        CHECK ((onsets (plain) == std::vector<long> { 0, 24000, 96000, 120000 }));
        CHECK ((onsets (l) == std::vector<long> { 0, 7000, 24000, 31000, 55000, 80000, 96000, 120000 }));
        bool level = true;
        for (size_t step : { 24000u, 96000u, 120000u })
            level &= std::fabs (energy (l, step, step + 5000) / energy (plain, step, step + 5000) - 1.0) < 0.02;
        CHECK (level);
        CHECK (out.values.empty() && b.value ("Bits 1") == 5 && b.value ("Bits 2") == a.value ("Bits 2"));
        bool cvs = true;
        for (int i = 1; i <= 8; ++i)
            cvs &= b.value ("Seq 1 CV " + std::to_string (i)) == a.value ("Seq 1 CV " + std::to_string (i))
                   && b.value ("Seq 2 CV " + std::to_string (i)) == a.value ("Seq 2 CV " + std::to_string (i));
        CHECK (cvs);
    }

    // Two pads on one frame: one sequencer's output takes the louder pad's CV (on a tie, the later's); the envelopes
    // the louder peak. One pad twice on a frame plays once.
    auto sound = [&] (std::vector<Hit> list, std::vector<float>& out) {
        Instance a;
        a.setup (patch (a, 0, 0, 0.5, false));
        a.render (16000, 128, out, r, stopped, hits (list));
    };
    std::vector<float> loud39, loud37, tie39, tie37, both;
    sound ({ { 1280, 39, 1.0 } }, loud39);
    sound ({ { 1280, 37, 1.0 } }, loud37);
    sound ({ { 1280, 39, 0.8 } }, tie39);
    sound ({ { 1280, 37, 0.8 } }, tie37);
    CHECK (loud39 != loud37 && tie39 != tie37 && loud39 != tie39 && (onsets (loud39) == std::vector<long> { 1280 }));
    sound ({ { 1280, 37, 0.5 }, { 1280, 39, 1.0 } }, both);
    CHECK (both == loud39);
    sound ({ { 1280, 39, 1.0 }, { 1280, 37, 0.5 } }, both);
    CHECK (both == loud39);
    sound ({ { 1280, 37, 0.8 }, { 1280, 39, 0.8 } }, both);
    CHECK (both == tie39);
    sound ({ { 1280, 39, 0.8 }, { 1280, 37, 0.8 } }, both);
    CHECK (both == tie37);
    sound ({ { 1280, 39, 1.0 }, { 1280, 39, 1.0 } }, both);
    CHECK (both == loud39);
    // Pads of both sequences on one frame (each one's bit 2) sound as the sequencer playing that step with both bits on
    // (EG Trig Mix in the middle: full), to the bit.
    {
        Instance a;
        a.setup (patch (a, 2, 2, 0.5, false));
        a.render (12000, 128, want, wantR, from0);
        Instance b;
        b.setup (patch (b, 0, 0, 0.5, false));
        b.render (12000, 128, l, r, stopped, hits ({ { 6000, 37 }, { 6000, 45 } }));
        CHECK ((onsets (want) == std::vector<long> { 6000 }) && l == want);
    }
    // On, on, off of one pad (Melodee's re-press): each on plays the step again, the off changes nothing. Pressed again
    // while it rings, it strikes again.
    {
        std::vector<float> noOff, first;
        sound ({ { 1280, 40 }, { 8960, 40 } }, noOff);
        sound ({ { 1280, 40 }, { 8960, 40 }, { 10240, 40, 0.0, CLAP_EVENT_NOTE_OFF } }, both);
        CHECK (both == noOff && (onsets (noOff) == std::vector<long> { 1280, 8960 }));
        sound ({ { 1280, 40 } }, first);
        sound ({ { 1280, 40 }, { 3840, 40 }, { 10240, 40, 0.0, CLAP_EVENT_NOTE_OFF } }, both);
        CHECK (std::equal (first.begin(), first.begin() + 3840, both.begin()) && energy (both, 3840, 6000) > 4.0 * energy (first, 3840, 6000));
        // A note off of the pad, CC 121 and a CC numbered like a pad: none is a pad press, none changes what rings.
        // (A choke and CC 120/123 do silence it: testPanic.)
        Instance a;
        a.setup (patch (a, 0, 0, 0.5, false));
        a.render (16000, 128, both, r, stopped, [&] (uint64_t at, uint32_t n, Events& e) {
            hits ({ { 1280, 40 } }) (at, n, e);
            if (at != 2560)
                return;
            e.note (0, CLAP_EVENT_NOTE_OFF, 40, 0.0);
            for (uint8_t ch = 0; ch < 16; ++ch)
                e.midi (0, (uint8_t) (0xb0 | ch), 121, 0);
            e.midi (0, 0xb0, 40, 100);
        });
        CHECK (both == first);
    }

    // A pad on Play's frame: the transport comes first, then the pad, which sounds (a note outside the pads there only
    // transposes). When the step Play lands on is on, that step plays on the same frame and replaces the pad's CV: the
    // same, to the bit, as Play alone.
    {
        auto playAt = [] (uint64_t at) { return at < 12800 ? Transport { false, 120.0, 0.0 } : playingFrom (0.0, 120.0, 48000.0, at - 12800); };
        Instance a;
        a.setup (patch (a, 0, 0, 0.0, false));
        a.render (18000, 128, l, r, playAt, hits ({ { 12800, 38, 0.7 } }));
        CHECK ((onsets (l) == std::vector<long> { 12800 }));
        Instance b;
        b.setup (patch (b, 1, 0, 0.0, false));
        b.render (18000, 128, want, wantR, playAt);
        Instance c;
        c.setup (patch (c, 1, 0, 0.0, false));
        c.render (18000, 128, l, r, playAt, hits ({ { 12800, 38, 0.7 } }));
        CHECK ((onsets (want) == std::vector<long> { 12800 }) && l == want);
    }
    // Settings on a frame come before its notes: Run switched on with a pad on one frame (the pad sent first) makes it
    // a step pad; switched off with it, the note is the keyboard's (it transposes and plays).
    {
        auto runAt = [&] (double before, double after, std::vector<float>& out) {
            Instance a;
            a.setup (patch (a, 0, 0, 0.0, false, before));
            a.render (12000, 128, out, r, stopped, [&] (uint64_t at, uint32_t, Events& e) {
                if (at != 1280)
                    return;
                e.note (0, CLAP_EVENT_NOTE_ON, 38, 1.0);
                if (after != before)
                    e.param (0, a.id ("Run"), after);
            });
        };
        std::vector<float> stepPad, key, switchedOn, switchedOff;
        runAt (1.0, 1.0, stepPad);
        runAt (0.0, 0.0, key);
        runAt (0.0, 1.0, switchedOn);
        runAt (1.0, 0.0, switchedOff);
        CHECK (stepPad != key && switchedOn == stepPad && switchedOff == key);
        CHECK ((onsets (key) == std::vector<long> { 1280 }) && (onsets (stepPad) == std::vector<long> { 1280 }));
    }

    // The same output, and the same values heard back, whatever the block size (1, 37, 128, 4096 frames): playing (the
    // dice rolling, Chain Seq switched on midway, pads on steps' frames, two on one frame, a re-press, notes outside
    // the pads) and stopped.
    for (bool playing : { true, false })
    {
        auto events = [&hits] (Instance* a) {
            auto notes = hits ({ { 5000, 37, 0.7 }, { 5000, 45, 0.9 }, { 12000, 40 }, { 20000, 40, 0.8 }, { 20000, 40, 0.8 },
                                 { 26000, 40, 0.0, CLAP_EVENT_NOTE_OFF }, { 33333, 51, 0.5 }, { 45000, 44 }, { 52000, 60, 0.6 },
                                 { 61000, 36, 0.4 }, { 61000, 43, 0.4 }, { 70000, 35 }, { 75000, 52, 0.9 }, { 80000, 47, 0.3 } });
            return [a, notes] (uint64_t at, uint32_t n, Events& e) {
                notes (at, n, e);
                if (41111 >= at && 41111 < at + n)
                    e.param ((uint32_t) (41111 - at), a->id ("Chain Seq"), 1.0);
            };
        };
        auto setup = [&] (Instance& a) {
            auto v = patch (a, 0x5a, 0xa5, 0.5, false);
            v.push_back ({ "Corrupt 1", 0.6 });
            v.push_back ({ "Corrupt 2", 0.3 });
            a.setup (v);
        };
        std::vector<float> l128, r128;
        OutEvents first;
        {
            Instance a;
            setup (a);
            a.render (96000, 128, l128, r128, playing ? from0 : stopped, events (&a), &first);
        }
        if (playing)
            CHECK (first.values.size() > 4);
        for (uint32_t block : { 37u, 1u, 4096u })
        {
            Instance a;
            setup (a);
            OutEvents out;
            a.render (96000, block, l, r, playing ? from0 : stopped, events (&a), &out);
            CHECK (l == l128 && r == r128);
            std::map<clap_id, double> lastX, lastY;
            for (auto& v : first.values) lastX[v.id] = v.value;
            for (auto& v : out.values) lastY[v.id] = v.value;
            CHECK (lastX == lastY);
        }
    }
}

// 0.2.1: with Run off (the sequencer off), every note transposes Unquantized too, so the pads play notes chromatically:
// the note's distance from middle C goes onto both CV outputs after CV Range, where the quantized modes add it, and
// through the same Seq Amt. With Run on, Unquantized still ignores the transposition, as the panel does (the step pads
// and the notes outside them as in 0.2.0).
void testUnquantizedKeys()
{
    std::vector<float> l, r;
    // Something on a frame, in the order listed: a note (key >= 0) or a parameter's value (a button pressed at 1).
    struct Ev { uint64_t when; int key; std::string name = ""; double value = 1.0; };
    auto play = [&] (Values v, std::vector<Ev> list, const std::function<Transport (uint64_t)>& transport, uint32_t frames,
                     std::vector<float>& out, uint32_t block = 128) {
        Instance a;
        a.setup (v);
        a.render (frames, block, out, r, transport, [&] (uint64_t at, uint32_t n, Events& e) {
            for (const auto& h : list)
                if (h.when >= at && h.when < at + n)
                {
                    if (h.key >= 0)
                        e.note ((uint32_t) (h.when - at), CLAP_EVENT_NOTE_ON, (int16_t) h.key, h.value);
                    else
                        e.param ((uint32_t) (h.when - at), a.id (h.name), h.value);
                }
        });
    };
    const double root = 320.0, mod = 0.16 * std::exp2 (9.75);
    auto near = [] (double hz, double want) { return std::fabs (hz / want - 1.0) < 0.002; };
    auto semis = [] (double s) { return std::exp2 (s / 12.0); };
    auto pitches = [&] (const std::vector<float>& x, std::vector<std::pair<size_t, size_t>> windows) {
        std::vector<double> hz;
        for (const auto& [from, to] : windows)
            hz.push_back (frequency (x, from, to));
        if (debug())
        {
            std::printf ("debug unquantized keys:");
            for (double h : hz) std::printf (" %.2f", h);
            std::printf ("\n");
        }
        return hz;
    };
    // The VCO alone, held open by Drone, SEQ 1 at full CV Range and at QTZ (1 V/octave), Length 1, Unquantized, Run off.
    Instance names;
    const double unquantized = names.valueOf ("Quantize", "Unquantized"), chromatic = names.valueOf ("Quantize", "Chromatic");
    auto vco = [&] (Values extra = {}) {
        Values v { { "Drone", 1.0 }, { "Bits 1", 1 }, { "Length 1", 1 }, { "VCO Freq", 0.5 }, { "VCO Level", 0.5 }, { "Mod VCO Level", 0.0 },
                   { "FM Amt", 0.0 }, { "Fold", 0.0 }, { "VCW EG1 Amt", 0.0 }, { "VCO EG1 Amt", 0.0 }, { "Blend", 0.0 }, { "CV Range 1", 1.0 },
                   { "VCO Seq 1 Amt", 1.0 }, { "Seq 1 CV 1", 0.0 }, { "Quantize", unquantized }, { "Run", 0.0 } };
        v.insert (v.end(), extra.begin(), extra.end());
        return v;
    };
    const auto quarters = std::vector<std::pair<size_t, size_t>> { { 2000, 22000 }, { 26000, 46000 }, { 50000, 70000 }, { 74000, 94000 } };

    // Run off: notes 36, 37 and 48 (pads A01, A02 and A13) a semitone and an octave apart, each its distance from middle
    // C, then 60 back where the CV puts it: stopped, and with the host playing (the sequences hold, silent).
    for (bool playing : { false, true })
    {
        play (vco(), { { 0, 36 }, { 24000, 37 }, { 48000, 48 }, { 72000, 60 } }, playing ? from0 : stopped, 96000, l);
        const auto hz = pitches (l, quarters);
        CHECK (near (hz[0], root * semis (-24.0)) && near (hz[1], root * semis (-23.0)) && near (hz[2], root * semis (-12.0)) && near (hz[3], root));
        CHECK (near (hz[1] / hz[0], semis (1.0)) && near (hz[2] / hz[0], 2.0));
    }
    // The same route as the quantized modes: with the CV at 0, Unquantized and Chromatic sound the same notes to the bit.
    {
        std::vector<float> chroma;
        const std::vector<Ev> keys { { 0, 36 }, { 24000, 37, "", 0.7 }, { 48000, 48 }, { 72000, 72, "", 0.4 } };
        play (vco(), keys, stopped, 96000, l);
        play (vco ({ { "Quantize", chromatic } }), keys, stopped, 96000, chroma);
        CHECK (l == chroma && near (frequency (l, 74000, 94000), root * 2.0));
    }
    // A CV between notes keeps its place, transposed: CV 0.105 (6.3 semitones, put on the output by an Advance) under
    // note 37 is 16.7 semitones down (Chromatic would make it 17).
    play (vco ({ { "Seq 1 CV 1", 0.105 } }), { { 0, -1, "Advance" }, { 0, 37 } }, stopped, 48000, l);
    CHECK (near (pitches (l, { { 4000, 46000 } })[0], root * semis (6.3 - 23.0)));
    // Below QTZ the transposition comes in scaled with the CV, as quantized: half the turn, half of note 48's octave.
    play (vco ({ { "VCO Seq 1 Amt", 0.49 } }), { { 0, 48 } }, stopped, 48000, l);
    CHECK (near (pitches (l, { { 4000, 46000 } })[0], root * semis (-6.0)));
    // SEQ 2's output too: the Mod VCO alone at QTZ, an octave down under note 48.
    {
        Values v { { "Drone", 1.0 }, { "VCO Level", 0.0 }, { "Ring Mod Level", 0.0 }, { "Noise Level", 0.0 }, { "Mod VCO Level", 0.5 },
                   { "Mod VCO Freq", 0.75 }, { "Fold", 0.0 }, { "VCW EG1 Amt", 0.0 }, { "Mod VCO EG1 Amt", 0.0 }, { "Blend", 0.0 },
                   { "Bits 2", 1 }, { "Length 2", 1 }, { "Seq 2 CV 1", 0.0 }, { "Mod VCO Seq 2 Amt", 1.0 }, { "Quantize", unquantized }, { "Run", 0.0 } };
        play (v, { { 0, 48 } }, stopped, 48000, l);
        CHECK (near (pitches (l, { { 4000, 46000 } })[0], mod / 2.0));
    }

    // Run on, Unquantized: no key transposes (0.3.0: every key is a step pad). SEQ 2's pad 16 in four banks (35, 67,
    // 83, 99) leaves SEQ 1's pitch alone, stopped (they play) and playing (bit 1, CV 0, on every step); and a step pad
    // plays its bit's CV as it is, untransposed: pad 36 (CV 0.1, 6 semitones) after bank B's pad 1 (52) on its frame
    // (the same velocity: the later pad's CV and bank).
    for (bool playing : { false, true })
    {
        play (vco ({ { "Run", 1.0 } }), { { 0, 35 }, { 24000, 67 }, { 48000, 83 }, { 72000, 99 } }, playing ? from0 : stopped, 96000, l);
        const auto hz = pitches (l, quarters);
        CHECK (near (hz[0], root) && near (hz[1], root) && near (hz[2], root) && near (hz[3], root));
    }
    play (vco ({ { "Run", 1.0 }, { "Seq 1 CV 1", 0.1 } }), { { 0, 52 }, { 0, 36 } }, stopped, 48000, l);
    CHECK (near (pitches (l, { { 4000, 46000 } })[0], root * semis (6.0)));

    // Run switched over a held transposition: note 48 with Run off (an octave down); Run on: Unquantized ignores it (the
    // CV as it is); Run off again: the octave down is back (the last note's transposition holds, as quantized). Stopped,
    // and playing (with Run on, SEQ 1 steps on its bit 1, CV 0).
    for (bool playing : { false, true })
    {
        play (vco(), { { 0, 48 }, { 30000, -1, "Run", 1.0 }, { 60000, -1, "Run", 0.0 } }, playing ? from0 : stopped, 96000, l);
        const auto hz = pitches (l, { { 4000, 28000 }, { 34000, 58000 }, { 64000, 94000 } });
        CHECK (near (hz[0], root / 2.0) && near (hz[1], root) && near (hz[2], root / 2.0));
    }
    // Settings on a frame come before its notes: Run switched off with a note (sent first) on one frame makes it the
    // keyboard's, transposing, as with Run off all along; switched on with note 52 (bank B's pad 1: SEQ 1's bit 1, CV 0,
    // an octave up, 0.3.0), it is a step pad, as with Run on all along. To the bit.
    {
        std::vector<float> switched, along;
        play (vco ({ { "Run", 1.0 } }), { { 12800, 37 }, { 12800, -1, "Run", 0.0 } }, stopped, 36000, switched);
        play (vco(), { { 12800, 37 } }, stopped, 36000, along);
        CHECK (switched == along && near (pitches (switched, { { 16000, 34000 } })[0], root * semis (-23.0)));
        play (vco(), { { 12800, 52 }, { 12800, -1, "Run", 1.0 } }, stopped, 36000, switched);
        play (vco ({ { "Run", 1.0 } }), { { 12800, 52 } }, stopped, 36000, along);
        CHECK (switched == along && near (pitches (switched, { { 16000, 34000 } })[0], root * 2.0));
    }

    // The same output whatever the block size (1, 37, 128, 4096 frames): both sequences heard, Unquantized, Run off;
    // pads and notes outside them, two on one frame, Run switched on (pads become steps) and off again mid-block, an
    // Advance, Quantize to Chromatic and back; stopped and playing.
    for (bool playing : { false, true })
    {
        Instance a;
        Values v { { "Clock Div", a.valueOf ("Clock Div", "1/16") }, { "Bits 1", 0x5a }, { "Bits 2", 0xa5 }, { "EG Trig Mix", 0.5 }, { "Run", 0.0 },
                   { "Quantize", unquantized }, { "CV Range 1", 0.5 }, { "CV Range 2", 0.5 }, { "VCW Seq 1 Amt", 0.4 }, { "VCF Seq 2 Amt", 0.6 },
                   { "Mod VCO Level", 0.4 }, { "Mod VCO Freq", 0.6 }, { "EG2 Decay", 0.3 }, { "Fold", 0.2 } };
        for (int i = 0; i < 8; ++i)
        {
            v.push_back ({ "Seq 1 CV " + std::to_string (i + 1), -0.7 + 0.19 * i });
            v.push_back ({ "Seq 2 CV " + std::to_string (i + 1), 0.65 - 0.17 * i });
        }
        const std::vector<Ev> list { { 3000, 36 }, { 9001, 37, "", 0.6 }, { 15000, 48 }, { 21003, 60, "", 0.5 }, { 27000, 40 }, { 27000, 45, "", 0.8 },
                                     { 33333, -1, "Run", 1.0 }, { 39000, 52 }, { 45000, 38 }, { 50001, -1, "Run", 0.0 }, { 56000, 30 },
                                     { 61111, -1, "Advance" }, { 66000, -1, "Quantize", chromatic }, { 70000, 50, "", 0.9 },
                                     { 76543, -1, "Quantize", unquantized }, { 82000, 47 }, { 88888, 71, "", 0.3 } };
        std::vector<float> at128;
        play (v, list, playing ? from0 : stopped, 96000, at128);
        bool same = true;
        for (uint32_t block : { 1u, 37u, 4096u })
        {
            play (v, list, playing ? from0 : stopped, 96000, l, block);
            same &= l == at128;
        }
        CHECK (same && std::count_if (at128.begin(), at128.end(), [] (float x) { return x != 0.0f; }) > 20000);
    }
}

// 0.3.0: the drum map. The .clap carries Melodee's drum-map JSON (as a plug-in library has it), so hdslaveOS plays a
// Psychic track's pads as drums, notes 36-51 whatever the octave, key or scale, and names them: 16 levels, each a name
// and a note, 36-51 in order.
void testPadMap (const char* path)
{
    std::ifstream f (path, std::ios::binary);
    const std::string bytes ((std::istreambuf_iterator<char> (f)), std::istreambuf_iterator<char>());
    const size_t at = bytes.find ("{\"pad_layout\":\"drums\"");
    CHECK (at != std::string::npos);
    if (at == std::string::npos)
        return;
    const std::string json = bytes.substr (at, bytes.find ('\0', at) - at);
    CHECK (json.find ("\"name\":\"hdslave Psychic\"") != std::string::npos && json.find ("\"levels\":{") != std::string::npos);
    // Each level as Melodee reads one: {"name":"...","note":N}; balanced braces, nothing after the map.
    int levels = 0, depth = 0, deepest = 0;
    bool inOrder = true;
    for (size_t from = json.find ("{\"name\":\""); from != std::string::npos; from = json.find ("{\"name\":\"", from + 1))
    {
        const size_t note = json.find ("\",\"note\":", from);
        const size_t close = json.find ('}', from);
        if (note == std::string::npos || close == std::string::npos || note > close)
            break;
        const std::string name = json.substr (from + 9, note - from - 9);
        const int key = std::atoi (json.c_str() + note + 9);
        inOrder &= key == 36 + levels && ! name.empty() && name.size() <= 12;
        ++levels;
    }
    for (char c : json)
    {
        depth += c == '{' ? 1 : c == '}' ? -1 : 0;
        deepest = std::max (deepest, depth);
    }
    CHECK (levels == 16 && inOrder && depth == 0 && deepest == 3 && json.back() == '}');
    CHECK (json.find ("S1 Bit 1") != std::string::npos && json.find ("S2 Bit 8") != std::string::npos);
}

// 0.3.0: the banks. hdslaveOS's - / + move a drum track's bank 16 keys (B 52-67, C 68-83, D 84-99). With Run on, every
// bank is the same 16 step pads an octave a bank apart (A-D 0-3 octaves up; at most two down and three up): a pad of
// any bank plays its step (52 SEQ 1's bit 1, as 36 does) with the bank's octaves on that CV output, held with the CV
// until the sequencer's next on bit. With Run off there are no banks: every key is its own note, as before 0.3.0.
void testDrumBanks()
{
    using psychic::padOf;
    CHECK (padOf (36).slot == 0 && padOf (36).semitones == 0 && padOf (51).slot == 15 && padOf (51).semitones == 0);
    CHECK (padOf (52).slot == 0 && padOf (52).semitones == 12 && padOf (68).slot == 0 && padOf (68).semitones == 24);
    CHECK (padOf (84).slot == 0 && padOf (84).semitones == 36 && padOf (99).slot == 15 && padOf (99).semitones == 36);
    CHECK (padOf (100).slot == 0 && padOf (100).semitones == 36 && padOf (127).semitones == 36);
    CHECK (padOf (35).slot == 15 && padOf (35).semitones == -12 && padOf (20).slot == 0 && padOf (0).semitones == -24);
    bool slots = true;
    for (int key = 0; key < 128; ++key)
        slots &= padOf (key).slot >= 0 && padOf (key).slot < 16 && (key - 36 - padOf (key).slot) % 16 == 0;
    CHECK (slots);

    std::vector<float> l, r, want, wantR;
    struct Hit { uint64_t when; int key; double velocity = 1.0; };
    auto pitches = [&] (Values extra, std::vector<Hit> list, std::vector<std::pair<size_t, size_t>> windows,
                        const std::function<Transport (uint64_t)>& transport, OutEvents* out = nullptr) {
        Instance a;
        // The VCO alone, held open by Drone, SEQ 1 at full CV Range in Major (CV 0.1: 5 semitones; 0.2 an octave).
        Values v { { "Drone", 1.0 }, { "VCO Freq", 0.5 }, { "VCO Level", 0.5 }, { "Mod VCO Level", 0.0 }, { "FM Amt", 0.0 }, { "Fold", 0.0 },
                   { "VCW EG1 Amt", 0.0 }, { "VCO EG1 Amt", 0.0 }, { "Blend", 0.0 }, { "CV Range 1", 1.0 },
                   { "Quantize", a.valueOf ("Quantize", "Major") }, { "Seq 1 CV 1", 0.1 }, { "Seq 1 CV 4", 0.2 }, { "Seq 1 CV 5", -0.2 },
                   { "Seq 2 CV 1", 0.2 } };
        v.insert (v.end(), extra.begin(), extra.end());
        a.setup (v);
        a.render (96000, 128, l, r, transport, [&] (uint64_t at, uint32_t n, Events& e) {
            for (const auto& h : list)
                if (h.when >= at && h.when < at + n)
                    e.note ((uint32_t) (h.when - at), CLAP_EVENT_NOTE_ON, (int16_t) h.key, h.velocity);
        }, out);
        std::vector<double> hz;
        for (const auto& [from, to] : windows)
            hz.push_back (frequency (l, from, to));
        if (debug())
        {
            std::printf ("debug drum banks:");
            for (double h : hz) std::printf (" %.1f", h);
            std::printf ("\n");
        }
        return hz;
    };
    const double root = 320.0;
    auto near = [] (double hz, double target) { return std::fabs (hz / target - 1.0) < 0.002; };
    auto semis = [] (double s) { return std::exp2 (s / 12.0); };
    const auto whole = std::vector<std::pair<size_t, size_t>> { { 4000, 90000 } };

    // Run on, stopped: pad 1 of banks A, B, C, D, the one past D (clamped to D's) and two below A: SEQ 1's bit 1 (5
    // semitones), 0, 1, 2, 3, 3 and -1, -2 octaves; never another step, never silence. Each reports nothing to the host.
    const std::vector<std::pair<int, double>> pad1 { { 36, 5.0 }, { 52, 17.0 }, { 68, 29.0 }, { 84, 41.0 }, { 100, 41.0 }, { 20, -7.0 },
                                                     { 4, -19.0 } };
    for (const auto& [key, semitones] : pad1)
    {
        OutEvents out;
        const auto hz = pitches ({}, { { 0, key } }, whole, stopped, &out);
        CHECK (near (hz[0], root * semis (semitones)) && out.values.empty());
    }
    // Every bank's pad is bank A's pad an octave a bank up: SEQ 1's bit 4 (CV 0.2, an octave) on pads 39, 55, 71, 87.
    {
        const auto a = pitches ({}, { { 0, 39 } }, whole, stopped)[0];
        const auto b = pitches ({}, { { 0, 55 } }, whole, stopped)[0];
        const auto c = pitches ({}, { { 0, 71 } }, whole, stopped)[0];
        const auto d = pitches ({}, { { 0, 87 } }, whole, stopped)[0];
        CHECK (near (a, root * 2.0) && near (b, a * 2.0) && near (c, a * 4.0) && near (d, a * 8.0));
    }
    // SEQ 2's pads in bank B (60: SEQ 2's bit 1) move only SEQ 2; chained, both outputs take the ring's bit and its
    // octave (SEQ 2's bit 1 at CV 0.2, an octave, plus bank B's: two octaves on the VCO).
    CHECK (near (pitches ({}, { { 0, 60 } }, whole, stopped)[0], root));
    CHECK (near (pitches ({ { "Chain Seq", 1.0 } }, { { 0, 60 } }, whole, stopped)[0], root * 4.0));
    // In every Quantize mode: Unquantized, the CV as it is (6 semitones) plus the bank's octave.
    CHECK (near (pitches ({ { "Quantize", 0.0 } }, { { 0, 52 } }, whole, stopped)[0], root * semis (18.0)));
    // Two pads on one frame, one output: the louder pad's CV and bank (the softer bank-C pad first or last).
    CHECK (near (pitches ({}, { { 0, 68, 0.4 }, { 0, 39, 1.0 } }, whole, stopped)[0], root * 2.0));
    CHECK (near (pitches ({}, { { 0, 39, 1.0 }, { 0, 68, 0.4 } }, whole, stopped)[0], root * 2.0));
    // Playing (bits 1 and 5 on, at 1/8): bank B's pad 4 (55: bit 4, an octave, up one more) in step 2 holds through the
    // off steps 3 and 4, and step 5 replaces it, its octave too (CV -0.2: an octave down, not up). The Bits and CVs
    // the host knows are as they were.
    {
        OutEvents out;
        const auto hz = pitches ({ { "Bits 1", 1 + 16 } }, { { 18000, 55 } }, { { 2000, 17000 }, { 20000, 29000 }, { 32000, 46000 }, { 50000, 70000 } },
                                 from0, &out);
        CHECK (near (hz[0], root * semis (5.0)) && near (hz[1], root * 4.0) && near (hz[2], root * 4.0) && near (hz[3], root / 2.0));
        CHECK (out.values.empty());
    }
    // The same to the bit, whatever the bank, as bank A for the envelopes: with CV Range 0 and Seq Amts 0 no CV is heard,
    // so pads 36, 52, 68, 84 and 20 strike the same sound.
    {
        auto strike = [&] (int key, std::vector<float>& out) {
            Instance a;
            a.setup ({ { "VCO Seq 1 Amt", 0.0 }, { "Mod VCO Seq 2 Amt", 0.0 }, { "VCW Seq 1 Amt", 0.0 }, { "VCF Seq 2 Amt", 0.0 }, { "EG2 Decay", 0.3 } });
            a.render (24000, 128, out, r, stopped, [&] (uint64_t at, uint32_t, Events& e) {
                if (at == 1280) e.note (0, CLAP_EVENT_NOTE_ON, (int16_t) key, 0.8);
            });
        };
        strike (36, want);
        bool same = onsets (want) == std::vector<long> { 1280 };
        for (int key : { 52, 68, 84, 20 })
        {
            strike (key, l);
            same &= l == want;
        }
        CHECK (same);
    }

    // Run off (the sequencer off): no banks. Every key is its own note, exactly as before 0.3.0: it transposes by key -
    // 60, so bank B's pad 1 (52) is 8 semitones down, 16 above pad A01 (36); 68, 84 and 99 (banks C and D) 8, 24 and 39
    // up; 35 and 30 below the pads 25 and 30 down. (The fingerprint's banks-run-off scenario holds it to the bit.)
    for (const auto& [key, semitones] : std::vector<std::pair<int, double>> { { 36, -24.0 }, { 52, -8.0 }, { 68, 8.0 }, { 84, 24.0 }, { 99, 39.0 },
                                                                            { 35, -25.0 }, { 30, -30.0 } })
        CHECK (near (pitches ({ { "Run", 0.0 }, { "Seq 1 CV 1", 0.0 } }, { { 0, key } }, whole, stopped)[0], root * semis (semitones)));
    {
        const auto a = pitches ({ { "Run", 0.0 }, { "Seq 1 CV 1", 0.0 } }, { { 0, 36 } }, whole, stopped)[0];
        const auto b = pitches ({ { "Run", 0.0 }, { "Seq 1 CV 1", 0.0 } }, { { 0, 52 } }, whole, stopped)[0];
        CHECK (near (b, a * semis (16.0)));
    }

    // The same output whatever the block size, banks on steps' frames and between them, two on one frame, playing.
    {
        auto run = [&] (uint32_t block, std::vector<float>& out) {
            Instance a;
            a.setup ({ { "Clock Div", a.valueOf ("Clock Div", "1/16") }, { "Bits 1", 0x5a }, { "Bits 2", 0xa5 }, { "EG Trig Mix", 0.5 },
                       { "Corrupt 1", 0.5 }, { "CV Range 1", 0.5 }, { "CV Range 2", 0.5 } });
            const std::vector<Hit> list { { 6000, 52 }, { 9000, 71, 0.6 }, { 12000, 84 }, { 12000, 20, 0.9 }, { 30001, 99 }, { 45000, 3 }, { 60000, 127 } };
            a.render (72000, block, out, r, from0, [&] (uint64_t at, uint32_t n, Events& e) {
                for (const auto& h : list)
                    if (h.when >= at && h.when < at + n)
                        e.note ((uint32_t) (h.when - at), CLAP_EVENT_NOTE_ON, (int16_t) h.key, h.velocity);
            });
        };
        run (128, want);
        bool same = true;
        for (uint32_t block : { 1u, 37u, 4096u })
        {
            run (block, l);
            same &= l == want;
        }
        CHECK (same);
    }
}

void testScreen()
{
    Instance a;
    const auto* screen = static_cast<const melodee_screen_t*> (a.plugin->get_extension (a.plugin, MELODEE_SCREEN_CLAP));
    CHECK (screen);
    if (!screen) return;
    constexpr int stride = 327;
    constexpr uint32_t guard = 0x13579bdf;
    std::vector<uint32_t> pixels (stride * 218, guard);
    melodee_screen_frame_t canvas {};
    canvas.width = 320; canvas.height = 218; canvas.stride = stride; canvas.area = MELODEE_SCREEN_PAGE; canvas.pixels = pixels.data(); canvas.time_ms = 1000;
    for (int k = 0; k < 3; ++k) canvas.pots[k] = -1, canvas.pots_live[k] = 1;
    auto paint = [&] (bool whole = false) { canvas.redraw_all = whole ? 1u : 0u; return screen->draw (a.plugin, &canvas); };
    auto key = [&] (uint32_t kind, int v = 1) {
        const melodee_screen_input_t input {kind, v, 0};
        return screen->input (a.plugin, &input);
    };
    auto knobs = [&] { std::vector<uint32_t> ids (3); screen->controls (a.plugin, ids.data()); return ids; };
    auto pixel = [&] (int x, int y) { return pixels[y*stride+x]; };
    CHECK (paint (true) && !paint());
    for (int y = 0; y < 218; ++y)
    {
        CHECK (pixel (0,y) != guard && pixel (319,y) != guard);
        CHECK (pixel (320,y) == guard && pixel (326,y) == guard);
    }
    canvas.stride = 319;
    CHECK (!paint (true));
    canvas.stride = stride;
    CHECK ((knobs() == std::vector<clap_id> {a.id ("Corrupt 1"),a.id ("Corrupt 2"),a.id ("Clock Div")}));
    // The four-beat spin follows song time. Its repaint clock keeps ticking even when
    // a silent instrument gets no audio callbacks. Use the cell editor to isolate the eye.
    CHECK (key (MELODEE_SCREEN_PUSH));
    a.setup ({{"Run", 0}});
    auto position = [&](double bpm, double beats) {
        std::vector<float> left, right;
        a.render (128,128,left,right,[&](uint64_t) { return Transport {true,bpm,beats}; });
    };
    position (120,0);
    canvas.playing = true;
    CHECK (paint (true));
    const auto rest = pixels;
    const int asked = paints;
    CHECK (!paint() && paints > asked);
    canvas.time_ms += 500;
    CHECK (paint());
    const auto quarter = pixels;
    int changedPixels = 0;
    bool onlyEye = true;
    for (int y=0; y<218; ++y)
        for (int x=0; x<stride; ++x)
            if (pixels[y*stride+x] != rest[y*stride+x])
            {
                ++changedPixels;
                onlyEye &= x>=154 && x<218 && y>=2 && y<37;
            }
    CHECK (changedPixels > 60 && onlyEye);
    CHECK (paint (true) && pixels == quarter);
    position (60,0);
    CHECK (paint() && pixels == rest);
    canvas.time_ms += 500;
    CHECK (paint() && pixels != rest && pixels != quarter);
    const auto halfBeat = pixels;
    canvas.time_ms += 500;
    CHECK (paint() && pixels == quarter);
    position (120,-3.5);
    CHECK (paint() && pixels == halfBeat);
    position (120,4);
    CHECK (paint() && pixels == rest);
    position (120,2);
    CHECK (paint() && pixels != rest);
    canvas.playing = false;
    CHECK (paint() && pixels == rest);
    const int stoppedPaints = paints;
    canvas.time_ms += 1000;
    CHECK (!paint() && paints == stoppedPaints);
    CHECK (key (MELODEE_SCREEN_STEP,-1) && a.loadPreset ("default"));
    a.setup ({});
    const auto emptyState = saveState (a);
    CHECK (key (MELODEE_SCREEN_PUSH));
    CHECK ((knobs() == std::vector<clap_id> {a.id ("Seq 1 CV 1"),a.id ("Length 1"),a.id ("Corrupt 1")}));
    int rescan = rescans, wake = wakes, flush = flushes;
    CHECK (key (MELODEE_SCREEN_PUSH));
    CHECK (a.value ("Bits 1") == 108 && rescans > rescan && flushes > flush && wakes > wake);
    OutEvents told;
    a.params->flush (a.plugin,nullptr,&told.out);
    CHECK (told.of (a.id ("Bits 1")) == std::vector<double> {108});
    CHECK (key (MELODEE_SCREEN_WHEEL,8));
    CHECK ((knobs() == std::vector<clap_id> {a.id ("Seq 2 CV 1"),a.id ("Length 2"),a.id ("Corrupt 2")}));
    CHECK (key (MELODEE_SCREEN_WHEEL,-8) && key (MELODEE_SCREEN_STEP,-1));

    // Save, then edit before the next audio block. Even a flush in between must preserve that order.
    CHECK (a.loadPreset ("default"));
    CHECK (key (MELODEE_SCREEN_WHEEL,-2) && key (MELODEE_SCREEN_PUSH)); // SAVE
    CHECK (key (MELODEE_SCREEN_WHEEL,2) && key (MELODEE_SCREEN_PUSH) && key (MELODEE_SCREEN_PUSH));
    a.params->flush (a.plugin,nullptr,&told.out);
    a.setup ({});
    CHECK (a.value ("Bits 1") == 108 && key (MELODEE_SCREEN_STEP,-1));
    CHECK (paint());
    constexpr auto marked = 0xffa20d65u;
    constexpr int markX = 48+23, markY = 102+19+2;
    CHECK (pixel (markX,markY) == marked);
    const auto withChanges = saveState (a);
    CHECK (key (MELODEE_SCREEN_WHEEL,-1) && key (MELODEE_SCREEN_PUSH)); // RECALL
    a.setup ({});
    CHECK (a.value ("Bits 1") == 109 && paint() && pixel (markX,markY) != marked);
    CHECK (loadState (a,withChanges) && paint() && pixel (markX,markY) == marked);
    CHECK (loadState (a,emptyState) && paint() && pixel (markX,markY) != marked);

    // A voltage-only edit on a silent gate also marks the contact print.
    CHECK (key (MELODEE_SCREEN_WHEEL,-1) && key (MELODEE_SCREEN_PUSH)); // SAVE
    a.setup ({});
    a.setup ({{"Seq 1 CV 2",0.75}});
    CHECK (paint() && pixel (markX+32, markY) == marked && pixel (markX,markY) != marked);
    CHECK (key (MELODEE_SCREEN_WHEEL,1) && key (MELODEE_SCREEN_PUSH));
    a.setup ({});
    CHECK (paint() && pixel (markX+32,markY) != marked);

    int before = paints;
    a.setup ({{"Clock 2",2},{"Write Head 1",1}});
    CHECK (paints > before && paint());
    // Separate clocks put A on its second cell while B still occupies its first.
    std::vector<float> l,r;
    a.render (12001,128,l,r,from0);
    before = paints;
    CHECK (paint() && paints > before);
    canvas.time_ms += 250;
    paint();
    constexpr auto restingHead = 0xff211a35u;
    CHECK (pixel (82,137) == restingHead && pixel (62,158) == restingHead);
    constexpr auto writeHead = 0xff285f83u;
    CHECK (pixel (118,122) == writeHead && pixel (66,143) == writeHead);
    before = paints;
    CHECK (!paint() && paints == before);
    // In a chain both playheads can land on B02. Their indicators have separate slots.
    a.setup ({{"Length 1",1},{"Length 2",3},{"Chain Seq",1},{"Clock 2",0}});
    a.render (1,1,l,r,[] (uint64_t) { return Transport {true,120,1}; });
    CHECK (paint());
    canvas.time_ms += 250;
    paint();
    CHECK (pixel (82,158) == restingHead && pixel (94,158) == restingHead);

    CHECK (a.loadPreset ("default"));
    a.setup ({});
    CHECK (key (MELODEE_SCREEN_STEP) && key (MELODEE_SCREEN_WHEEL,3));
    CHECK (knobs()[0] == CLAP_INVALID_ID);
    // Trigger asks the host to wake up. A press queued while the host slept (no audio ran) still fires on waking:
    // its age is counted in frames rendered, not wall time, so the sleep below does not expire it.
    wake = wakes;
    CHECK (key (MELODEE_SCREEN_PUSH) && wakes > wake);
    std::this_thread::sleep_for (std::chrono::milliseconds (520));
    CHECK (key (MELODEE_SCREEN_WHEEL,1) && key (MELODEE_SCREEN_PUSH)); // Advance, queued behind the Trigger
    a.plugin->reset (a.plugin);
    auto stopped = [] (uint64_t) { return Transport {false,120,0}; };
    a.render (1024,128,l,r,stopped);
    CHECK (std::any_of (l.begin(),l.end(),[] (float v){return std::fabs(v)>.005f;}));
    CHECK (key (MELODEE_SCREEN_WHEEL,-1) && key (MELODEE_SCREEN_PUSH));
    a.render (1024,128,l,r,stopped);
    CHECK (std::any_of (l.begin(),l.end(),[] (float v){return std::fabs(v)>.005f;}));

    // Every public non-button parameter remains reachable with the hardware encoders.
    std::vector<clap_id> reached;
    for (int g=0; g<13; ++g)
    {
        key (MELODEE_SCREEN_WHEEL,-100);
        for (int row=0; row<16; ++row)
        {
            const auto ids = knobs();
            reached.insert (reached.end(),ids.begin(),ids.end());
            key (MELODEE_SCREEN_WHEEL,1);
        }
        if (g<12) CHECK (key (MELODEE_SCREEN_STEP));
    }
    for (uint32_t n=0; n<a.params->count (a.plugin); ++n)
    {
        clap_param_info info {};
        a.params->get_info (a.plugin,n,&info);
        if (a.text (info.name,1)!="Pressed")
            CHECK (std::find (reached.begin(),reached.end(),info.id)!=reached.end());
    }
    CHECK (!key (MELODEE_SCREEN_STEP));
}

// A page press queued while the host slept fires once on waking, at either device rate (its 500 ms of grace is
// counted in frames rendered). On a fresh instance: Sequencer group, its Trigger row, push.
void testScreenPressSleep()
{
    for (double rate : { 44100.0, 48000.0 })
    {
        Instance a (rate);
        const auto* screen = static_cast<const melodee_screen_t*> (a.plugin->get_extension (a.plugin, MELODEE_SCREEN_CLAP));
        CHECK (screen != nullptr);
        if (screen == nullptr)
            continue;
        auto key = [&] (uint32_t kind, int v = 1) {
            const melodee_screen_input_t input { kind, v, 0 };
            return screen->input (a.plugin, &input);
        };
        a.setup ({});
        CHECK (key (MELODEE_SCREEN_STEP) && key (MELODEE_SCREEN_WHEEL, 3));
        std::vector<uint32_t> ids (3);
        screen->controls (a.plugin, ids.data());
        CHECK (ids[0] == MELODEE_SCREEN_NO_CONTROL); // a button's row: Trigger
        CHECK (key (MELODEE_SCREEN_PUSH));
        std::this_thread::sleep_for (std::chrono::milliseconds (520)); // the sleeping host: no process calls
        std::vector<float> l, r;
        a.render (2048, 128, l, r, stopped);
        CHECK (std::any_of (l.begin(), l.end(), [] (float v) { return std::fabs (v) > .005f; }));
    }
}

void demos (const std::string& folder)
{
    // The factory presets, 8 s each at 120 BPM: each loaded by its key, written out to listen to.
    for (const auto& key : presetKeys)
    {
        Instance a;
        CHECK (a.loadPreset (key.c_str()));
        a.setup ({});
        std::vector<float> l, r;
        a.render (48000 * 8, 128, l, r, from0);
        float peak = 0.0f;
        double sum = 0.0;
        bool finite = true;
        for (float s : l)
            finite &= std::isfinite (s), peak = std::max (peak, std::fabs (s)), sum += (double) s * s;
        const double rms = std::sqrt (sum / l.size());
        std::printf ("demo %-14s peak %.3f  rms %.3f\n", key.c_str(), peak, rms);
        CHECK (finite && peak > 0.1f && peak <= 1.0f);
        const auto path = folder + "/demo-" + key + ".wav";
        CHECK (writeWav (path.c_str(), l, r, 48000));
    }
}
} // namespace

int main (int argc, char** argv)
{
    if (argc < 3)
    {
        std::fprintf (stderr, "usage: host Psychic.clap OUT_FOLDER\n");
        return 2;
    }
    void* library = dlopen (argv[1], RTLD_NOW | RTLD_LOCAL);
    if (library == nullptr)
    {
        std::printf ("FAIL dlopen: %s\n", dlerror());
        return 1;
    }
    entry = static_cast<const clap_plugin_entry*> (dlsym (library, "clap_entry"));
    if (entry == nullptr || ! entry->init (argv[1]))
    {
        std::printf ("FAIL no clap_entry\n");
        return 1;
    }
    factory = static_cast<const clap_plugin_factory*> (entry->get_factory (CLAP_PLUGIN_FACTORY_ID));
    testDescription();
    testParameters();
    testState();
    testPanic();
    testDroneAfterSilence();
    testLoadWakesSleeping();
    testMidGridPlay();
    testSettleOnLoad();
    testHardening();
    testLatencyTail();
    testTailNotices();
    testPresets();
    testTiming();
    testStopRestartJump();
    testBlockSizes();
    testBitsAndTrigMix();
    testSofterTriggerInAttack();
    testLengthShiftFlip();
    testChainResetAdvance();
    testCorrupt();
    testPitchAndQuantizer();
    testClock2();
    testRouting();
    testClockChanges();
    testOffsetsThroughLoops();
    testMidBlockLoop();
    testHostEdges();
    testStepPads();
    testUnquantizedKeys();
    testPadMap (argv[1]);
    testDrumBanks();
    testScreen();
    testScreenPressSleep();
    testExtremes();
    demos (argv[2]);
    entry->deinit();
    std::printf ("%d checks, %d failed\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
