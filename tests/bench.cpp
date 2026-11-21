// Psychic's cost: renders a pattern through the CLAP entry for SECONDS at 48 kHz in 128-frame blocks, as Melodee
// plays it, and prints the time a block takes here. tools/bench.sh runs it under callgrind for instruction counts
// (exact and repeatable, unlike timings on a computer that is not the MPC).
//   bench [worst|typical|turning|stopped] [SECONDS]
#include "../src/psychic.cpp"

#include <chrono>
#include <string>
#include <vector>

int main (int argc, char** argv)
{
    const std::string mode = argc > 1 ? argv[1] : "worst";
    const double seconds = argc > 2 ? std::atof (argv[2]) : 10.0;
    constexpr double rate = 48000.0;
    constexpr uint32_t block = 128;

    static const clap_host host { CLAP_VERSION, nullptr, "bench", "", "", "1", [] (const clap_host*, const char*) -> const void* { return nullptr; },
                                  [] (const clap_host*) {}, [] (const clap_host*) {}, [] (const clap_host*) {} };
    clap_entry.init ("");
    const auto* factory = static_cast<const clap_plugin_factory*> (clap_entry.get_factory (CLAP_PLUGIN_FACTORY_ID));
    const clap_plugin* plugin = factory->create_plugin (factory, &host, "com.hdslave.psychic");
    plugin->init (plugin);
    plugin->activate (plugin, rate, 1, block);
    plugin->start_processing (plugin);

    // The patch: worst keeps every part busy all the time (Drone holds the VCAs open, FM, ring, noise, the folder
    // moved by EG1 and the Mod VCO, resonance, both sequencers corrupting on their own grids); typical is the
    // default patch at 1/16 with a little Corrupt; stopped renders with the transport stopped and nothing sounding.
    std::vector<std::pair<uint32_t, double>> values;
    using namespace psychic::ParamId;
    if (mode == "worst")
        values = { { drone, 1 }, { fmAmt, 0.6 }, { ringLevel, 0.5 }, { modLevel, 0.4 }, { noiseLevel, 0.3 }, { fold, 0.5 }, { vcwEg1Amt, 0.4 },
                   { modFold, 0.3 }, { modCutoff, 0.3 }, { resonance, 0.8 }, { corrupt1, 0.7 }, { corrupt2, 0.6 }, { clockDiv, 5 }, { clock2, 5 },
                   { modEg1Amt, 0.3 } };
    else if (mode == "typical" || mode == "turning")
        values = { { clockDiv, 5 }, { corrupt1, 0.2 } };
    // turning: typical, with a hand on two knobs the whole time (Cutoff and Fold moving every 16 frames, as fine-grained
    // automation sends them): what the knobs' easing and the engine's retargeting cost.

    struct Events
    {
        std::vector<clap_event_param_value> list;
        clap_input_events in { this, [] (const clap_input_events* e) { return (uint32_t) static_cast<const Events*> (e->ctx)->list.size(); },
                               [] (const clap_input_events* e, uint32_t i) { return &static_cast<const Events*> (e->ctx)->list[i].header; } };
    } first, none, turning;
    for (auto& [id, v] : values)
    {
        clap_event_param_value e {};
        e.header = { sizeof (e), 0, CLAP_CORE_EVENT_SPACE_ID, CLAP_EVENT_PARAM_VALUE, 0 };
        e.param_id = id;
        e.note_id = e.port_index = e.channel = e.key = -1;
        e.value = v;
        first.list.push_back (e);
    }
    clap_output_events out { nullptr, [] (const clap_output_events*, const clap_event_header*) { return true; } };

    std::vector<float> left (block), right (block);
    float* outs[2] = { left.data(), right.data() };
    clap_audio_buffer buffer {};
    buffer.data32 = outs;
    buffer.channel_count = 2;
    const uint64_t blocks = (uint64_t) (seconds * rate / block);
    double sum = 0.0;
    const auto t0 = std::chrono::steady_clock::now();
    for (uint64_t b = 0; b < blocks; ++b)
    {
        clap_event_transport t {};
        t.header = { sizeof (t), 0, CLAP_CORE_EVENT_SPACE_ID, CLAP_EVENT_TRANSPORT, 0 };
        t.flags = CLAP_TRANSPORT_HAS_TEMPO | CLAP_TRANSPORT_HAS_BEATS_TIMELINE | (mode != "stopped" ? CLAP_TRANSPORT_IS_PLAYING : 0);
        t.tempo = 120.0;
        t.song_pos_beats = (clap_beattime) std::llround ((double) (b * block) * 2.0 / rate * (double) CLAP_BEATTIME_FACTOR);
        clap_process p {};
        p.frames_count = block;
        p.transport = &t;
        p.audio_outputs = &buffer;
        p.audio_outputs_count = 1;
        p.in_events = b == 0 ? &first.in : &none.in;
        if (mode == "turning" && b > 0)
        {
            turning.list.clear();
            for (uint32_t at = 0; at < block; at += 16)
                for (uint32_t id : { cutoff, fold })
                {
                    clap_event_param_value e {};
                    e.header = { sizeof (e), at, CLAP_CORE_EVENT_SPACE_ID, CLAP_EVENT_PARAM_VALUE, 0 };
                    e.param_id = id;
                    e.note_id = e.port_index = e.channel = e.key = -1;
                    e.value = 0.5 + 0.3 * std::sin ((double) (b * block + at) * (id == cutoff ? 1.0e-4 : 1.7e-4));
                    turning.list.push_back (e);
                }
            p.in_events = &turning.in;
        }
        p.out_events = &out;
        plugin->process (plugin, &p);
        sum += left[0];
    }
    const double us = std::chrono::duration<double, std::micro> (std::chrono::steady_clock::now() - t0).count();
    std::printf ("%s: %llu blocks, %.2f us a block here (checksum %.6f)\n", mode.c_str(), (unsigned long long) blocks, us / blocks, sum);
    plugin->stop_processing (plugin);
    plugin->deactivate (plugin);
    plugin->destroy (plugin);
    clap_entry.deinit();
    return 0;
}
