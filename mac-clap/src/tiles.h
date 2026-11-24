#pragma once

#include "hdslave_tiles.h"
#include "screen.h"

namespace psychic
{

struct TilesAccess
{
    // 12 tiles (4x3 grid): Contact sheet (with all 16 CV steps & 2x8 bits) + all 11 synth/seq modules
    static constexpr int kTileCount = 12;
    static constexpr int kTileGroup[kTileCount] = { 0, 1, 2, 4, 6, 7, 8, 9, 10, 11, 12, 13 };

    static uint32_t CLAP_ABI count (const clap_plugin_t*, int* out_cols, int* out_rows)
    {
        if (out_cols) *out_cols = 4;
        if (out_rows) *out_rows = 3;
        return kTileCount;
    }

    static const char* CLAP_ABI title (const clap_plugin_t*, uint32_t tile)
    {
        static constexpr const char* kTitles[kTileCount] = {
            "01 // CONTACT SHEET (16 STEPS)",
            "02 // CORRUPT & CLOCK",
            "03 // SEQUENCER A",
            "04 // SEQUENCER B",
            "05 // SINE VCO",
            "06 // MOD VCO (TZFM)",
            "07 // RING & MIXER",
            "08 // WAVEFOLDER (VCW)",
            "09 // STATE-VAR VCF",
            "10 // 5x DECAY ENVELOPES",
            "11 // BLEND & OUTPUT",
            "12 // CV PATCH ROUTING"
        };
        return tile < kTileCount ? kTitles[tile] : "";
    }

    static bool CLAP_ABI paint (const clap_plugin_t* plugin, uint32_t tile, const melodee_screen_frame_t* frame, clap_id knobs_out[3])
    {
        if (tile >= kTileCount || !frame || !frame->pixels) return false;
        auto& p = of (plugin);
        const int g = kTileGroup[tile];
        const int savedGroup = p.screen.group;
        const bool savedInspector = p.screen.inspector;

        p.screen.inspector = false;
        p.screen.selectGroup (g);
        p.screen.changed = true;

        melodee_screen_frame_t c = *frame;
        c.redraw_all = 1;
        const bool drew = screenDraw (plugin, &c);
        if (knobs_out)
            p.screen.knobs (knobs_out);

        p.screen.inspector = savedInspector;
        p.screen.selectGroup (savedGroup);
        return drew;
    }

    static bool CLAP_ABI hit_test (const clap_plugin_t* plugin, uint32_t tile, int x, int y, hdslave_tile_hotspot_t* out_spot)
    {
        if (!out_spot || tile >= kTileCount) return false;
        *out_spot = {};
        out_spot->param_id = CLAP_INVALID_ID;
        out_spot->param_id_y = CLAP_INVALID_ID;

        auto& p = of (plugin);
        const int g = kTileGroup[tile];

        if (g == 0)
        {
            // CONTACT SHEET (y: 42..187)
            // 1. Live film strip cells (x: 169..305, y: 63..95) -> drag step CV
            if (x >= 169 && x < 305 && y >= 63 && y < 95)
            {
                const int k = std::clamp ((x - 169) / 17, 0, 7);
                const int lane = (y - 63) >= 16 ? 1 : 0;
                out_spot->kind = HDSLAVE_HOTSPOT_PARAM;
                out_spot->param_id = (lane ? ParamId::cv2First : ParamId::cv1First) + (uint32_t) k;
                out_spot->x = (int16_t) (169 + k * 17);
                out_spot->y = (int16_t) (63 + lane * 16);
                out_spot->w = 16;
                out_spot->h = 15;
                return true;
            }
            // 2. Punch-card 8-bit rows A & B (x: 46..304, y: 120..163)
            if (x >= 46 && x < 304 && y >= 120 && y < 163)
            {
                const int k = std::clamp ((x - 48) / 32, 0, 7);
                const int lane = (y - 120) >= 21 ? 1 : 0;
                const int cx = 48 + k * 32;
                const int cy = 121 + lane * 21;
                if (x - cx < 15)
                {
                    // Left side: punched hole bit toggle!
                    out_spot->kind = HDSLAVE_HOTSPOT_BIT_TOGGLE;
                    out_spot->param_id = lane ? ParamId::bits2 : ParamId::bits1;
                    out_spot->custom_id = k;
                    out_spot->x = (int16_t) cx;
                    out_spot->y = (int16_t) cy;
                    out_spot->w = 15;
                    out_spot->h = 19;
                    return true;
                }
                else
                {
                    // Right side: bipolar voltage ruler drag!
                    out_spot->kind = HDSLAVE_HOTSPOT_PARAM;
                    out_spot->param_id = (lane ? ParamId::cv2First : ParamId::cv1First) + (uint32_t) k;
                    out_spot->x = (int16_t) (cx + 15);
                    out_spot->y = (int16_t) cy;
                    out_spot->w = 14;
                    out_spot->h = 19;
                    return true;
                }
            }
            // 3. Action buttons: > SHIFT A, > SHIFT B, + SAVE, < RECALL (y: 169..187)
            if (x >= 5 && x < 317 && y >= 169 && y < 187)
            {
                static constexpr uint32_t kActions[4] = { ParamId::bitShift1, ParamId::bitShift2, ParamId::save, ParamId::recall };
                const int idx = std::clamp ((x - 5) / 78, 0, 3);
                out_spot->kind = HDSLAVE_HOTSPOT_BUTTON;
                out_spot->param_id = kActions[idx];
                out_spot->x = (int16_t) (5 + idx * 78);
                out_spot->y = 170;
                out_spot->w = 75;
                out_spot->h = 16;
                return true;
            }
        }
        else
        {
            // Parameter list (y: 42 + 19 + i * 16 = 61 + i * 16, h = 16)
            const int savedGroup = p.screen.group;
            p.screen.selectGroup (g);
            const int numRows = p.screen.rows;
            uint32_t rowIds[16] {};
            for (int i = 0; i < numRows && i < 16; ++i)
                rowIds[i] = p.screen.fields[i];
            p.screen.selectGroup (savedGroup);

            if (y >= 59 && y < 59 + numRows * 16)
            {
                const int rowIdx = std::clamp ((y - 59) / 16, 0, numRows - 1);
                const uint32_t id = rowIds[rowIdx];
                const auto& param = paramTable[paramIndex (id)];
                out_spot->kind = param.kind == Kind::button ? HDSLAVE_HOTSPOT_BUTTON : HDSLAVE_HOTSPOT_PARAM;
                out_spot->param_id = id;
                out_spot->x = 5;
                out_spot->y = (int16_t) (59 + rowIdx * 16);
                out_spot->w = 310;
                out_spot->h = 16;
                return true;
            }
        }
        return false;
    }

    static void CLAP_ABI focus (const clap_plugin_t* plugin, uint32_t tile, int x, int y, bool)
    {
        if (tile >= kTileCount) return;
        auto& p = of (plugin);
        const int g = kTileGroup[tile];
        p.screen.inspector = false;
        p.screen.selectGroup (g);
        if (g == 0)
        {
            if (x >= 169 && x < 305 && y >= 63 && y < 95)
            {
                const int k = std::clamp ((x - 169) / 17, 0, 7);
                const int lane = (y - 63) >= 16 ? 1 : 0;
                p.screen.selection = lane * 8 + k;
            }
            else if (x >= 46 && x < 304 && y >= 120 && y < 163)
            {
                const int k = std::clamp ((x - 48) / 32, 0, 7);
                const int lane = (y - 120) >= 21 ? 1 : 0;
                p.screen.selection = lane * 8 + k;
            }
            else if (x >= 5 && x < 317 && y >= 169 && y < 187)
            {
                p.screen.selection = 16 + std::clamp ((x - 5) / 78, 0, 3);
            }
        }
        else if (y >= 59 && y < 59 + p.screen.rows * 16)
        {
            p.screen.cursors[g] = std::clamp ((y - 59) / 16, 0, std::max (0, p.screen.rows - 1));
        }
        p.screen.changed = true;
    }

    static void CLAP_ABI action (const clap_plugin_t*, uint32_t, int32_t, int32_t, bool) {}
};

const clap_plugin_hdslave_tiles_t tilesExtension {
    TilesAccess::count,
    TilesAccess::title,
    TilesAccess::paint,
    TilesAccess::hit_test,
    TilesAccess::focus,
    TilesAccess::action,
    188,
};

} // namespace psychic
