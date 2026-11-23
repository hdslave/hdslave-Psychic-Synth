// Psychic's fingerprint scenarios (tests/fingerprint.cpp).
#pragma once

namespace
{
// The parameters of Psychic 0.1, in its table's order: the random scenarios draw for these. A parameter added later
// stays at its default there, so these hashes still hold.
const std::vector<std::string> order01 { "VCO Freq", "VCO EG1 Amt", "VCO Seq 1 Amt", "Mod VCO Freq", "Mod VCO EG1 Amt", "Mod VCO Seq 2 Amt",
    "FM Amt", "VCO Level", "Mod VCO Level", "Ring Mod Level", "Noise Level", "Noise Tone", "Fold", "Bias", "VCW EG1 Amt", "VCW Seq 1 Amt",
    "Cutoff", "Resonance", "Filter Mode", "VCF EG1 Amt", "VCF Seq 2 Amt", "Order", "Blend", "Volume", "EG1 Decay", "EG2 Decay",
    "EG Trig Mix", "Clock Div", "Run", "Quantize", "Trigger", "Advance", "Reset", "Corrupt 1", "CV Range 1", "Length 1", "Bit Flip 1",
    "Bit Shift 1", "Bits 1", "Corrupt 2", "CV Range 2", "Length 2", "Bit Flip 2", "Bit Shift 2", "Bits 2", "Chain Seq", "Write Head 1",
    "Write Head 2", "Swing", "Save", "Recall", "Clock 2", "Mod VCO > Fold", "Mod VCO > Cutoff", "Drone", "Seq 1 CV 1", "Seq 1 CV 2",
    "Seq 1 CV 3", "Seq 1 CV 4", "Seq 1 CV 5", "Seq 1 CV 6", "Seq 1 CV 7", "Seq 1 CV 8", "Seq 2 CV 1", "Seq 2 CV 2", "Seq 2 CV 3",
    "Seq 2 CV 4", "Seq 2 CV 5", "Seq 2 CV 6", "Seq 2 CV 7", "Seq 2 CV 8" };
const std::vector<std::string> buttons { "Trigger", "Advance", "Reset", "Bit Flip 1", "Bit Shift 1", "Bit Flip 2", "Bit Shift 2", "Save", "Recall" };

std::vector<Scenario> scenarios()
{
    std::vector<Scenario> list;
    for (const char* key : { "default", "psychic-bells", "fold-bass", "chain-melody", "polyrhythm", "corrupt-drone", "noise-perc" })
    {
        Scenario s;
        s.name = std::string ("preset-") + key;
        s.preset = key;
        list.push_back (s);
    }
    // The bench's patches.
    Scenario worst;
    worst.name = "bench-worst";
    worst.values = { { "Drone", 1 }, { "FM Amt", 0.6 }, { "Ring Mod Level", 0.5 }, { "Mod VCO Level", 0.4 }, { "Noise Level", 0.3 },
                     { "Fold", 0.5 }, { "VCW EG1 Amt", 0.4 }, { "Mod VCO > Fold", 0.3 }, { "Mod VCO > Cutoff", 0.3 }, { "Resonance", 0.8 },
                     { "Corrupt 1", 0.7 }, { "Corrupt 2", 0.6 }, { "Clock Div", 5 }, { "Clock 2", 5 }, { "Mod VCO EG1 Amt", 0.3 } };
    list.push_back (worst);
    Scenario typical;
    typical.name = "bench-typical";
    typical.values = { { "Clock Div", 5 }, { "Corrupt 1", 0.2 } };
    list.push_back (typical);
    // Random settings with the buttons, MIDI and knobs moving while it plays.
    const size_t firstRandom = list.size();
    for (uint32_t r = 0; r < 24; ++r)
    {
        Scenario s;
        s.name = "random-" + std::to_string (r);
        s.values = randomValues (1000 + r, buttons, order01);
        s.seconds = 3.0, s.bpm = 150.0;
        s.events = { { 24000, Event::param, "Trigger", 1.0 }, { 30011, Event::param, "Bit Flip 1", 1.0 }, { 40001, Event::param, "Bit Shift 2", 1.0 },
                     { 48011, Event::param, "Advance", 1.0 }, { 55555, Event::param, "Save", 1.0 }, { 62403, Event::noteOn, "", 64 },
                     { 72000, Event::noteOff, "", 64 }, { 80000, Event::param, "Reset", 1.0 }, { 96005, Event::param, "Run", 1.0 },
                     { 100007, Event::param, "Cutoff", 0.25 }, { 110000, Event::param, "Recall", 1.0 }, { 115201, Event::param, "Clock Div", 7.0 } };
        list.push_back (s);
    }
    // Other block sizes (random-0's scenario) and other rates.
    for (uint32_t block : { 1u, 37u, 4096u })
    {
        Scenario s = list[firstRandom];
        s.name = "random-0-block-" + std::to_string (block);
        s.block = block;
        list.push_back (s);
    }
    for (double rate : { 44100.0, 96000.0 })
    {
        Scenario s;
        s.name = "rate-" + std::to_string ((int) rate);
        s.preset = "psychic-bells";
        s.rate = rate;
        s.seconds = 4.0;
        list.push_back (s);
    }
    // Stopped, started again, jumped, a loop's wrap.
    Scenario moves;
    moves.name = "stop-start-jump";
    moves.preset = "polyrhythm";
    moves.seconds = 6.0;
    moves.transport = [] (uint64_t at, double bpm, double rate) -> std::pair<bool, double> {
        const double f = (double) at;
        if (at < 48000) return { true, f * bpm / 60.0 / rate };
        if (at < 72000) return { false, 2.0 };
        if (at < 144000) return { true, (f - 72000) * bpm / 60.0 / rate };
        if (at < 200000) return { true, 3.3 + (f - 144000) * bpm / 60.0 / rate };
        return { true, 100.0 / 24000.0 + (f - 200000) * bpm / 60.0 / rate };
    };
    list.push_back (moves);
    // Corrupt across its turn, each sequencer, with the write heads ahead.
    for (double corrupt : { 0.25, 0.5, 0.75, 1.0 })
    {
        Scenario s;
        s.name = "corrupt-" + std::to_string ((int) std::lround (corrupt * 100));
        s.preset = "psychic-bells";
        s.values = { { "Corrupt 1", corrupt }, { "Corrupt 2", 1.0 - corrupt * 0.5 }, { "Write Head 1", 2 }, { "Write Head 2", 5 } };
        s.seconds = 10.0;
        list.push_back (s);
    }
    // The sequencers' moves: Chain Seq on and off while it plays, Bit Shift chained (the ring; SEQ 2's head), Reset,
    // Length changes, Clock 2's own grid, Swing, the buffer.
    Scenario chain;
    chain.name = "chain-moves";
    chain.preset = "chain-melody";
    chain.seconds = 8.0;
    chain.events = { { 30000, Event::param, "Bit Shift 1", 1.0 }, { 50000, Event::param, "Bit Shift 2", 1.0 }, { 70000, Event::param, "Save", 1.0 },
                     { 90000, Event::param, "Length 1", 3.0 }, { 110000, Event::param, "Chain Seq", 0.0 }, { 130000, Event::param, "Bit Flip 2", 1.0 },
                     { 150000, Event::param, "Chain Seq", 1.0 }, { 170000, Event::param, "Reset", 1.0 }, { 200000, Event::param, "Recall", 1.0 },
                     { 250000, Event::param, "Corrupt 1", 0.9 } };
    list.push_back (chain);
    for (double swing : { 0.5, 0.66 })
    {
        Scenario s;
        s.name = "clock2-swing-" + std::to_string ((int) std::lround (swing * 100));
        s.preset = "polyrhythm";
        s.values = { { "Swing", swing } };
        s.seconds = 6.0;
        s.events = { { 100000, Event::param, "Clock 2", 7.0 }, { 180000, Event::param, "Clock 2", 0.0 } };
        list.push_back (s);
    }
    // Notes: transposing while it plays, and playing it (stopped), in two modes.
    for (bool playing : { true, false })
    {
        Scenario s;
        s.name = std::string ("notes-") + (playing ? "playing" : "stopped");
        s.preset = "fold-bass";
        s.seconds = 4.0;
        for (int i = 0; i < 12; ++i)
            s.events.push_back ({ (uint64_t) (4000 + i * 15000), Event::noteOn, "", (double) (55 + (i * 5) % 13) });
        s.events.push_back ({ 100000, Event::param, "Quantize", 0.0 });
        if (! playing)
            s.transport = [] (uint64_t, double, double) -> std::pair<bool, double> { return { false, 0.0 }; };
        list.push_back (s);
    }
    // The routing: each Order across Blend, Filter Mode and the folder's Bias.
    for (int order = 0; order < 3; ++order)
    {
        Scenario s;
        s.name = "order-" + std::to_string (order);
        s.preset = "default";
        s.values = { { "Order", (double) order }, { "Fold", 0.45 }, { "Bias", -0.3 }, { "Filter Mode", 0.4 }, { "Resonance", 0.75 } };
        s.seconds = 4.0;
        s.events = { { 60000, Event::param, "Blend", 0.0 }, { 120000, Event::param, "Blend", 1.0 }, { 150000, Event::param, "Filter Mode", 1.0 } };
        list.push_back (s);
    }
    // 0.1.1: a two-bar loop as Melodee plays it, its end falling inside a block (told at the next block's start),
    // through two clocks on their own grids, and chained with Bit Shift 2's gap and the dice.
    for (const char* key : { "polyrhythm", "chain-melody" })
    {
        Scenario s;
        s.name = std::string ("melodee-loop-") + key;
        s.preset = key;
        s.bpm = 8.0 * 60.0 * 48000.0 / (128.0 * 1384 + 8);
        s.seconds = 9.0;
        if (std::string (key) == "chain-melody")
            s.values = { { "Corrupt 1", 0.8 }, { "Corrupt 2", 0.3 } }, s.events = { { 30000, Event::param, "Bit Shift 2", 1.0 } };
        s.transport = [] (uint64_t at, double bpm, double rate) -> std::pair<bool, double> {
            return { true, std::fmod ((double) at * bpm / 60.0 / rate, 8.0) };
        };
        list.push_back (s);
    }
    // 0.2.0: with Run on, notes 36-51 are step pads (SEQ 1's bits 1-8, then SEQ 2's): pads while it plays (on steps'
    // frames and between them, two on one frame, one pad twice on a frame, a re-press), chained, and stopped, with
    // notes outside the pads among them; and the same notes with Run off, the keyboard as before (as 0.1.3 hashes it).
    const std::vector<std::pair<uint64_t, int>> pads { { 3000, 38 }, { 12000, 44 }, { 12000, 41 }, { 30001, 60 }, { 36000, 51 },
                                                       { 47000, 36 }, { 47000, 36 }, { 60000, 35 }, { 72000, 49 }, { 84000, 43 },
                                                       { 99999, 52 }, { 120000, 46 }, { 120000, 39 }, { 150000, 40 }, { 170000, 40 } };
    for (const char* key : { "pads-playing", "pads-chained", "pads-stopped", "pads-run-stop" })
    {
        const std::string name = key;
        Scenario s;
        s.name = name;
        s.preset = name == "pads-chained" ? "chain-melody" : "fold-bass";
        s.seconds = 4.0;
        for (const auto& [frame, key] : pads)
            s.events.push_back ({ frame, Event::noteOn, "", (double) key });
        s.events.push_back ({ 172000, Event::noteOff, "", 40.0 });
        if (name == "pads-stopped")
            s.transport = [] (uint64_t, double, double) -> std::pair<bool, double> { return { false, 0.0 }; };
        if (name == "pads-run-stop")
            s.values = { { "Run", 0.0 } };
        list.push_back (s);
    }
    // 0.2.1: with Run off, notes transpose Unquantized too (the pads play notes): pads and notes outside them, two on one
    // frame, then Run switched on (pads are steps, and Unquantized ignores the held transposition) and off again (it is
    // back), an Advance; stopped and playing.
    for (bool playing : { false, true })
    {
        Scenario s;
        s.name = std::string ("keys-unquantized-") + (playing ? "playing" : "stopped");
        s.preset = "default";
        s.values = { { "Run", 0.0 }, { "Quantize", 0.0 } };
        s.seconds = 4.0;
        for (const auto& [frame, key] : std::vector<std::pair<uint64_t, int>> { { 2000, 36 }, { 14000, 37 }, { 26000, 48 }, { 38000, 43 }, { 50000, 51 },
                                                                               { 50000, 44 }, { 62000, 60 }, { 74000, 30 }, { 86000, 72 }, { 104000, 38 },
                                                                               { 116000, 52 }, { 128000, 47 }, { 150000, 40 }, { 160000, 65 }, { 170000, 49 } })
            s.events.push_back ({ frame, Event::noteOn, "", (double) key });
        s.events.push_back ({ 96001, Event::param, "Run", 1.0 });
        s.events.push_back ({ 140003, Event::param, "Run", 0.0 });
        s.events.push_back ({ 165000, Event::param, "Advance", 1.0 });
        s.events.push_back ({ 175000, Event::noteOff, "", 40.0 });
        if (! playing)
            s.transport = [] (uint64_t, double, double) -> std::pair<bool, double> { return { false, 0.0 }; };
        list.push_back (s);
    }
    return list;
}
} // namespace
