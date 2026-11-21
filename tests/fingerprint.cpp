// The sound's fingerprint: renders fixed scenarios through the CLAP entry, as a host plays them, and prints a 64-bit
// hash of every output sample's bits for each. tests/fingerprint.txt holds the hashes the current sound gives, and
// `make test` fails if one changes: work meant to keep the sound (a speed-up, a clean-up) must leave them all as they
// are. A change meant to change the sound updates the file with `make fingerprint-update`.
//   fingerprint PLUGIN.clap [EXPECTED.txt]
// The scenarios: each factory preset, the bench's patches, random settings with the buttons and MIDI, other rates,
// stop and jumps, other block sizes, and the sequencers' own moves (Corrupt, flips, shifts, Chain Seq, Clock 2, the
// buffer). The values the plug-in tells the host it changed are hashed too. Hashes hold for this toolchain and these flags (Melodee's build image, -O3
// -mcpu=cortex-a55 -ffp-contract=fast): another compiler may round differently.
#include <clap/clap.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <dlfcn.h>
#include <functional>
#include <map>
#include <random>
#include <string>
#include <vector>

namespace
{
const clap_plugin_factory* factory = nullptr;
const clap_host host { CLAP_VERSION, nullptr, "fingerprint", "", "", "1", [] (const clap_host*, const char*) -> const void* { return nullptr; },
                       [] (const clap_host*) {}, [] (const clap_host*) {}, [] (const clap_host*) {} };

struct Event
{
    uint64_t frame;
    enum { param, noteOn, noteOff } kind;
    std::string name; // the parameter's
    double value;     // its value, or the note's key
};

struct Scenario
{
    std::string name;
    std::string preset;                                  // a factory preset loaded first ("" none)
    std::vector<std::pair<std::string, double>> values;  // then these
    std::vector<Event> events;
    double rate = 48000.0, seconds = 8.0, bpm = 120.0;
    uint32_t block = 128;
    // The transport for the block starting at frame `at`: {playing, beats}. Default: playing from the top.
    std::function<std::pair<bool, double> (uint64_t, double, double)> transport;
};

uint64_t fnv (uint64_t h, const void* data, size_t n)
{
    const auto* p = static_cast<const uint8_t*> (data);
    for (size_t i = 0; i < n; ++i)
        h = (h ^ p[i]) * 0x100000001b3ull;
    return h;
}

uint64_t render (const Scenario& s)
{
    const auto* d = factory->get_plugin_descriptor (factory, 0);
    const clap_plugin* plugin = factory->create_plugin (factory, &host, d->id);
    plugin->init (plugin);
    const auto* params = static_cast<const clap_plugin_params*> (plugin->get_extension (plugin, CLAP_EXT_PARAMS));
    std::map<std::string, clap_id> ids;
    for (uint32_t i = 0; i < params->count (plugin); ++i)
    {
        clap_param_info info {};
        params->get_info (plugin, i, &info);
        ids[info.name] = info.id;
    }
    if (! s.preset.empty())
    {
        const auto* presets = static_cast<const clap_plugin_preset_load*> (plugin->get_extension (plugin, CLAP_EXT_PRESET_LOAD));
        if (presets == nullptr || ! presets->from_location (plugin, CLAP_PRESET_DISCOVERY_LOCATION_PLUGIN, nullptr, s.preset.c_str()))
            std::printf ("no preset %s\n", s.preset.c_str());
    }
    plugin->activate (plugin, s.rate, 1, 4096);
    plugin->start_processing (plugin);

    struct In
    {
        std::vector<clap_event_param_value> params;
        std::vector<clap_event_note> notes;
        std::vector<const clap_event_header*> order;
        clap_input_events events { this, [] (const clap_input_events* e) { return (uint32_t) static_cast<const In*> (e->ctx)->order.size(); },
                                   [] (const clap_input_events* e, uint32_t i) { return static_cast<const In*> (e->ctx)->order[i]; } };
    };
    // What the plug-in tells the host (the sequences' bits and CVs it changed itself, the buttons' releases) is part
    // of the fingerprint too, hashed apart from the samples.
    uint64_t told = 0xcbf29ce484222325ull;
    clap_output_events out { &told, [] (const clap_output_events* o, const clap_event_header* h) {
        if (h->space_id == CLAP_CORE_EVENT_SPACE_ID && h->type == CLAP_EVENT_PARAM_VALUE)
        {
            const auto* v = reinterpret_cast<const clap_event_param_value*> (h);
            auto* hash = static_cast<uint64_t*> (o->ctx);
            *hash = fnv (*hash, &v->param_id, sizeof (v->param_id));
            *hash = fnv (*hash, &v->value, sizeof (v->value));
        }
        return true;
    } };
    auto value = [&] (uint32_t time, const std::string& name, double v) {
        clap_event_param_value e {};
        e.header = { sizeof (e), time, CLAP_CORE_EVENT_SPACE_ID, CLAP_EVENT_PARAM_VALUE, 0 };
        e.param_id = ids.count (name) ? ids[name] : CLAP_INVALID_ID;
        if (! ids.count (name))
            std::printf ("no parameter %s\n", name.c_str());
        e.note_id = e.port_index = e.channel = e.key = -1;
        e.value = v;
        return e;
    };

    const uint64_t frames = (uint64_t) (s.seconds * s.rate);
    std::vector<float> left (s.block), right (s.block);
    uint64_t hash = 0xcbf29ce484222325ull;
    for (uint64_t at = 0; at < frames; at += s.block)
    {
        const uint32_t n = (uint32_t) std::min<uint64_t> (s.block, frames - at);
        In in;
        in.params.reserve (s.values.size() + s.events.size());
        in.notes.reserve (s.events.size());
        if (at == 0)
            for (auto& [name, v] : s.values)
                in.params.push_back (value (0, name, v));
        for (const auto& e : s.events)
            if (e.frame >= at && e.frame < at + n)
            {
                const uint32_t t = (uint32_t) (e.frame - at);
                if (e.kind == Event::param)
                    in.params.push_back (value (t, e.name, e.value));
                else
                {
                    clap_event_note note {};
                    note.header = { sizeof (note), t, CLAP_CORE_EVENT_SPACE_ID, (uint16_t) (e.kind == Event::noteOn ? CLAP_EVENT_NOTE_ON : CLAP_EVENT_NOTE_OFF), 0 };
                    note.note_id = -1, note.port_index = 0, note.channel = 0, note.key = (int16_t) e.value, note.velocity = 0.9;
                    in.notes.push_back (note);
                }
            }
        for (auto& p : in.params)
            in.order.push_back (&p.header);
        for (auto& p : in.notes)
            in.order.push_back (&p.header);
        std::stable_sort (in.order.begin(), in.order.end(), [] (const clap_event_header* a, const clap_event_header* b) { return a->time < b->time; });

        const auto [playing, beats] = s.transport ? s.transport (at, s.bpm, s.rate) : std::make_pair (true, (double) at * s.bpm / 60.0 / s.rate);
        clap_event_transport t {};
        t.header = { sizeof (t), 0, CLAP_CORE_EVENT_SPACE_ID, CLAP_EVENT_TRANSPORT, 0 };
        t.flags = CLAP_TRANSPORT_HAS_TEMPO | CLAP_TRANSPORT_HAS_BEATS_TIMELINE | CLAP_TRANSPORT_HAS_TIME_SIGNATURE | (playing ? CLAP_TRANSPORT_IS_PLAYING : 0);
        t.tempo = s.bpm;
        t.song_pos_beats = (clap_beattime) std::llround (beats * (double) CLAP_BEATTIME_FACTOR);
        t.tsig_num = 4, t.tsig_denom = 4;
        float* outs[2] = { left.data(), right.data() };
        clap_audio_buffer buffer {};
        buffer.data32 = outs;
        buffer.channel_count = 2;
        clap_process p {};
        p.steady_time = (int64_t) at;
        p.frames_count = n;
        p.transport = &t;
        p.audio_outputs = &buffer;
        p.audio_outputs_count = 1;
        p.in_events = &in.events;
        p.out_events = &out;
        plugin->process (plugin, &p);
        for (uint32_t i = 0; i < n; ++i) // left and right a frame at a time: the hash is the same whatever the block size
        {
            hash = fnv (hash, &left[i], sizeof (float));
            hash = fnv (hash, &right[i], sizeof (float));
        }
    }
    plugin->stop_processing (plugin);
    plugin->deactivate (plugin);
    plugin->destroy (plugin);
    // Then the values the host heard, in order, without their times (they come at the end of the block they fell in).
    return fnv (hash, &told, sizeof (told));
}

// Random settings, every one in its range (the buttons left out), from a fixed seed. They are drawn for the
// parameters `order` names, in its order: the parameters the plug-in had when the scenario was made, so a parameter
// added since (or moved in the table) stays at its default there and the old hashes still hold.
std::vector<std::pair<std::string, double>> randomValues (uint32_t seed, const std::vector<std::string>& skip,
                                                          const std::vector<std::string>& order)
{
    std::vector<std::pair<std::string, double>> out;
    std::mt19937 random (seed);
    const auto* d = factory->get_plugin_descriptor (factory, 0);
    const clap_plugin* plugin = factory->create_plugin (factory, &host, d->id);
    plugin->init (plugin);
    const auto* params = static_cast<const clap_plugin_params*> (plugin->get_extension (plugin, CLAP_EXT_PARAMS));
    std::map<std::string, clap_param_info> byName;
    for (uint32_t i = 0; i < params->count (plugin); ++i)
    {
        clap_param_info info {};
        params->get_info (plugin, i, &info);
        byName[info.name] = info;
    }
    for (const auto& name : order)
    {
        if (byName.count (name) == 0)
        {
            std::printf ("no parameter %s\n", name.c_str());
            continue;
        }
        const clap_param_info info = byName[name];
        std::uniform_real_distribution<double> u (info.min_value, info.max_value);
        double v = u (random);
        if (info.flags & CLAP_PARAM_IS_STEPPED)
            v = std::round (v);
        if (std::find (skip.begin(), skip.end(), std::string (info.name)) == skip.end())
            out.push_back ({ info.name, v });
    }
    plugin->destroy (plugin);
    return out;
}

std::vector<Scenario> scenarios();
} // namespace

#include "fingerprint_scenarios.h"

int main (int argc, char** argv)
{
    if (argc < 2)
    {
        std::fprintf (stderr, "usage: fingerprint PLUGIN.clap [EXPECTED.txt]\n");
        return 2;
    }
    void* library = dlopen (argv[1], RTLD_NOW | RTLD_LOCAL);
    const auto* entry = library ? static_cast<const clap_plugin_entry*> (dlsym (library, "clap_entry")) : nullptr;
    if (entry == nullptr || ! entry->init (argv[1]))
    {
        std::printf ("FAIL cannot load %s\n", argv[1]);
        return 1;
    }
    factory = static_cast<const clap_plugin_factory*> (entry->get_factory (CLAP_PLUGIN_FACTORY_ID));
    std::map<std::string, std::string> expected;
    if (argc > 2)
        if (FILE* f = std::fopen (argv[2], "r"))
        {
            char name[128], hash[32];
            while (std::fscanf (f, "%127s %31s", name, hash) == 2)
                expected[name] = hash;
            std::fclose (f);
        }
    int changed = 0, missing = 0;
    for (const auto& s : scenarios())
    {
        char hash[32];
        std::snprintf (hash, sizeof (hash), "%016llx", (unsigned long long) render (s));
        std::printf ("%s %s\n", s.name.c_str(), hash);
        if (argc > 2)
        {
            const auto it = expected.find (s.name);
            if (it == expected.end())
                ++missing, std::fprintf (stderr, "fingerprint: %s is new (%s)\n", s.name.c_str(), hash);
            else if (it->second != hash)
                ++changed, std::fprintf (stderr, "FAIL fingerprint: %s was %s, is %s: the sound changed\n", s.name.c_str(), it->second.c_str(), hash);
        }
    }
    entry->deinit();
    if (argc > 2)
        std::fprintf (stderr, "fingerprint: %d changed, %d new\n", changed, missing);
    return changed == 0 && missing == 0 ? 0 : 1;
}
