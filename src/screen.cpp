#include "screen.h"

namespace psychic
{
namespace
{
using hdui::Ink;
using hdui::Raster;
using namespace ParamId;
constexpr Ink ground = 0xff24191e, film = 0xff140f17, panel = 0xff39272c, rule = 0xff765853;
constexpr Ink paper = 0xffe8d9b6, light = 0xffffe7c8, muted = 0xffcdaea1, amber = 0xffffb373;
constexpr Ink carbon = 0xff352c30, brick = 0xff943e31, pencil = 0xffc0ac89, teal = 0xff37706a;
constexpr Ink hitInk[] = {carbon, 0xff864337, brick, 0xffbb4631, 0xffdc472b};
constexpr uint32_t actions[] = {bitShift1, bitShift2, save, recall};
constexpr const char* actionLabels[] = {"SHIFT A", "SHIFT B", "SAVE", "RECALL"};
constexpr int cellCount = 16, choiceCount = cellCount + int (std::size (actions));
constexpr int headerH = 27, topH = 68, bodyH = 90, footH = 33;
static_assert (headerH + topH + bodyH + footH == hdui::height);

uint32_t cvId (int cell)
{
    return (cell < 8 ? cv1First : cv2First) + cell % 8;
}

uint32_t bitsId (int lane)
{
    return lane ? bits2 : bits1;
}

const Param& parameter (uint32_t id)
{
    return paramTable[paramIndex (id)];
}

bool bit (int mask, int column)
{
    return (mask & (1 << column)) != 0;
}

int wrap (int value, int32_t delta, int n)
{
    return int (((int64_t (value) + delta) % n + n) % n);
}

void large (Raster& r, int x, int y, const char* text, Ink color, bool italic = false)
{
    for (; *text; ++text, x += 14)
    {
        const auto& glyph = hdui::font::glyphs[static_cast<unsigned char> (*text) - 32];
        for (int j = 0; j < hdui::font::glyphHeight; ++j)
            for (int k = 0; k < hdui::font::glyphWidth; ++k)
                if (glyph[j] & (1 << k))
                    r.box (x + 2 * k + (italic ? (10 - j) / 3 : 0), y + 2 * j, italic ? 3 : 2, 2, color);
    }
}

void diamond (Raster& r, int x, int y, Ink color)
{
    for (int j = -2; j <= 2; ++j)
        r.box (x - 2 + std::abs (j), y + j, 5 - 2 * std::abs (j), 1, color);
}

void cellLabel (char (&text)[16], int cell)
{
    std::snprintf (text, sizeof (text), "%c%02d", cell < 8 ? 'A' : 'B', cell % 8 + 1);
}

void voltage (char (&text)[32], float value)
{
    std::snprintf (text, sizeof (text), "%+.2f V", 5 * value);
}

void shortValue (uint32_t id, float value, char (&text)[48])
{
    const Param& p = parameter (id);
    if (p.unit == Unit::percent)
        std::snprintf (text, sizeof (text), "%.0f%%", value * 100);
    else
        formatParam (p, value, text, sizeof (text));
}

// Sixteen exposure densities. Voltage moves the optical trace up and down; a punched gate exposes more of the film.
constexpr Ink negative[] = {0xff251d25, 0xff322930, 0xff41363b, 0xff4d4246, 0xff5c5052, 0xff6a5d5e, 0xff786b6a, 0xff857976,
                            0xff958783, 0xffa2958d, 0xffb0a399, 0xffbeb1a4, 0xffcbbdae, 0xffd7cbb8, 0xffe3d7c4, 0xffefe2ce};
constexpr Ink positive[] = {0xff301b22, 0xff402028, 0xff512b30, 0xff623638, 0xff74413f, 0xff884c44, 0xff985a4e, 0xffac6858,
                            0xffbb7862, 0xffcb876c, 0xffd89677, 0xffe1a581, 0xffedb38d, 0xfff5c69b, 0xffffd6af, 0xffffe6c4};

void exposure (Raster& r, int x, int y, float cv, bool on, int cell, const Ink* inks)
{
    constexpr int wave[] = {0, 1, 1, 2, 2, 2, 1, 1, 0, -1, -1, -2, -2, -2, -1, -1};
    const int baseline = 8 - int (std::lround (cv * 5));
    for (int dx = 0; dx < 16; ++dx)
    {
        const int center = baseline + wave[(dx + cell * 3) % 16];
        for (int dy = 0; dy < 17; ++dy)
        {
            const int distance = std::abs (dy - center);
            const int grain = ((dx * 5 + dy * 3 + cell) % 7 == 0) - ((dx + dy + cell) % 5 == 0);
            const int density = (on ? 15 - 2 * distance : 5 - 2 * distance) + grain;
            r.dot (x + dx, y + dy, inks[std::clamp (density, 0, 15)]);
        }
    }
}

void punch (Raster& r, int x, int y, bool on, bool enabled)
{
    const Ink outline = enabled ? pencil : 0xffcebea0;
    if (on)
    {
        r.box (x, y + 1, 8, 9, enabled ? carbon : pencil);
        r.box (x + 1, y, 6, 1, enabled ? carbon : pencil);
        r.box (x + 1, y + 10, 6, 1, light);
    }
    else
        r.frame (x, y, 8, 11, outline);
}
} // namespace

void Screen::setMemory (const std::array<float, seqStateCount>& savedValues, bool full)
{
    memoryChanged |= haveMemory != full || memory != savedValues;
    memory = savedValues;
    haveMemory = full;
}

bool Screen::cellChanged (int cell) const
{
    const uint32_t bits = bitsId (cell / 8), cv = cvId (cell);
    return haveMemory && (bit (int (live[bits]) ^ int (saved (bits)), cell % 8) || live[cv] != saved (cv));
}

void Screen::selectGroup (int n)
{
    group = n;
    rows = 0;
    if (group)
        for (const auto& p : paramTable)
            if (std::strcmp (p.module, groups[group]) == 0) fields[rows++] = p.id;
}

void Screen::knobs (clap_id ids[3]) const
{
    if (!group)
    {
        ids[0] = inspector ? cvId (selection) : ParamId::corrupt1;
        ids[1] = inspector ? (cellPage () ? ParamId::length2 : ParamId::length1) : ParamId::corrupt2;
        ids[2] = inspector ? (cellPage () ? ParamId::corrupt2 : ParamId::corrupt1) : ParamId::clockDiv;
        return;
    }
    for (int k = 0; k < 3; ++k)
    {
        const int n = row () + k;
        ids[k] = n < rows && parameter (fields[n]).kind != Kind::button ? fields[n] : CLAP_INVALID_ID;
    }
}

bool Screen::input (const melodee_screen_input_t& e, Edit& out)
{
    if (e.kind == MELODEE_SCREEN_NUDGE && !editing)
    {
        if (inspector && e.value < 0)
            inspector = false;
        else
        {
            const int next = std::clamp (group + (e.value > 0) - (e.value < 0), 0, int (std::size (groups)) - 1);
            if (next == group) return false;
            inspector = false;
            selectGroup (next);
        }
    }
    else if (!group)
    {
        if (e.kind == MELODEE_SCREEN_WHEEL)
            selection = wrap (selection, e.value, inspector ? cellCount : choiceCount);
        else if (e.kind == MELODEE_SCREEN_PUSH)
        {
            if (inspector)
            {
                const uint32_t id = bitsId (cellPage ());
                const int mask = int (feed.value (values, paramIndex (id)));
                out = {id, float (mask ^ (1 << (selection % 8))), false};
            }
            else if (selection < cellCount)
                inspector = true;
            else
                out = {actions[selection - cellCount], 0, true};
        }
        else
            return false;
    }
    else
    {
        const Param& p = parameter (fields[row ()]);
        if (e.kind == MELODEE_SCREEN_PUSH)
        {
            if (p.kind == Kind::button)
                out = {p.id, 0, true};
            else
                editing = !editing;
        }
        else if (!editing && e.kind == MELODEE_SCREEN_WHEEL)
            cursors[group] = int (std::clamp<int64_t> (int64_t (row ()) + e.value, 0, rows - 1));
        else if (editing && (e.kind == MELODEE_SCREEN_WHEEL || e.kind == MELODEE_SCREEN_NUDGE))
        {
            const float old = feed.value (values, paramIndex (p.id));
            const float step = p.kind == Kind::continuous ? (p.max - p.min) / (e.kind == MELODEE_SCREEN_NUDGE ? 16
                                                                               : e.shift                      ? 1024
                                                                                                              : 128)
                                                          : 1;
            const float next = clampParam (p, old + e.value * step);
            if (next != old) out = {p.id, next, false};
        }
        else
            return false;
    }
    changed = true;
    return true;
}

void Screen::header (Raster& r) const
{
    large (r, 5, 5, "PSYCHIC", light, true);
    r.box (111, 5, 1, 16, rule);
    char name[16];
    if (inspector)
    {
        cellLabel (name, selection);
        r.text (122, 8, "CELL", muted);
        r.text (159, 8, name, amber);
    }
    else
        r.text (122, 8, group ? groups[group] : "CONTACT SHEET", muted);
    if (!group && !inspector)
    {
        char focus[24];
        if (selection < cellCount)
        {
            cellLabel (name, selection);
            std::snprintf (focus, sizeof (focus), "PUSH %s", name);
        }
        else
            std::snprintf (focus, sizeof (focus), "PUSH %s", actionLabels[selection - cellCount]);
        r.right (312, 8, focus, amber);
    }
    else
        r.right (312, 8, live[ParamId::chain] > .5f ? "CHAIN" : clockDivChoices[int (live[ParamId::clockDiv])], amber);
    r.box (5, 25, 310, 1, rule);
}

void Screen::films (Raster& r) const
{
    int diff = 0;
    for (int c = 0; c < cellCount; ++c)
        diff += cellChanged (c);
    for (int side = 0; side < 2; ++side)
    {
        const int left = 5 + side * 156;
        r.text (left + 2, 2, side ? "LIVE" : "SAVED", side ? amber : muted);
        char status[24];
        if (side && haveMemory)
            std::snprintf (status, sizeof (status), "%d CHANGED", diff);
        else if (side)
            std::snprintf (status, sizeof (status), "%d + %d", int (live[ParamId::length1]), int (live[ParamId::length2]));
        else
            std::snprintf (status, sizeof (status), "%s", haveMemory ? "NEGATIVE" : "EMPTY");
        r.right (left + 150, 2, status, muted);
        r.box (left, 14, 153, 52, film);
        for (int x = left + 4; x < left + 148; x += 13)
        {
            r.box (x, 16, 5, 2, rule);
            r.box (x, 62, 5, 2, rule);
        }
        if (!side && !haveMemory)
        {
            r.text (left + 12, 29, "No saved pattern", muted);
            r.text (left + 12, 44, "Select SAVE below", muted);
            continue;
        }
        for (int cell = 0; cell < cellCount; ++cell)
        {
            const int lane = cell / 8, k = cell % 8, x = left + 8 + 17 * k, y = 21 + 19 * lane;
            const auto bits = bitsId (lane), cv = cvId (cell);
            exposure (r, x, y, side ? live[cv] : saved (cv), bit (int (side ? live[bits] : saved (bits)), k), cell,
                      side ? positive : negative);
            if (side && cellChanged (cell)) r.box (x, y + 16, 16, 2, amber);
        }
    }
}

void Screen::card (Raster& r) const
{
    r.box (5, 1, 310, 65, paper);
    r.box (5, 1, 2, 65, pencil);
    for (int y : {8, 23, 38, 53})
    {
        r.box (9, y, 3, 5, ground);
        r.box (308, y, 3, 5, ground);
    }
    r.text (17, 6, "BIT", brick);
    for (int k = 0; k < 8; ++k)
    {
        const int x = 48 + k * 32;
        const char label[] = {char ('1' + k), 0};
        r.text (x + 9, 6, label, carbon);
        r.box (x - 2, 17, 1, 44, pencil);
        for (int lane = 0; lane < 2; ++lane)
        {
            const int cell = lane * 8 + k, y = 19 + 21 * lane;
            const bool enabled = k < int (live[lane ? ParamId::length2 : ParamId::length1]);
            punch (r, x + 4, y + 3, bit (int (live[bitsId (lane)]), k), enabled);
            // A bipolar voltage ruler next to the punched hole.
            r.box (x + 15, y + 9, 10, 1, pencil);
            const int bar = int (std::lround (live[cvId (cell)] * 6));
            r.box (x + 18, y + 9 - std::max (0, bar), 4, std::abs (bar) + 1, enabled ? teal : pencil);
            if (selection == cell) r.frame (x, y, 28, 19, brick);
            if (cellChanged (cell)) r.box (x + 23, y + 2, 3, 3, brick);
            for (int s = 0; s < 2; ++s)
            {
                if (head (s, false) == cell) r.box (x + 2 + s * 12, y + 16, 10, 2, hitInk[flash[s]]);
                if (head (s, true) == cell) diamond (r, x + 6 + s * 12, y + 1, teal);
            }
        }
    }
    r.text (19, 24, "A", carbon);
    r.text (19, 46, "B", carbon);
    for (int i = 0; i < int (std::size (actions)); ++i)
    {
        const int x = 5 + 78 * i;
        const bool selected = selection == cellCount + i;
        r.box (x, 71, 75, 16, selected ? amber : panel);
        r.text (x + 5, 75, actionLabels[i],
                selected ? carbon : (actions[i] == ParamId::recall && !haveMemory ? muted : light));
    }
}

void Screen::cellValues (Raster& r) const
{
    const uint32_t bits = bitsId (cellPage ()), cv = cvId (selection);
    for (int side = 0; side < 2; ++side)
    {
        const int x = 5 + 156 * side;
        r.box (x, 3, 153, 62, side ? paper : panel);
        const Ink fg = side ? carbon : light, sub = side ? brick : muted;
        r.text (x + 7, 9, side ? "LIVE" : "SAVED", sub);
        if (!side && !haveMemory)
        {
            r.text (x + 7, 32, "No saved print", muted);
            continue;
        }
        const bool on = bit (int (side ? live[bits] : saved (bits)), selection % 8);
        r.right (x + 146, 9, on ? "GATE ON" : "GATE OFF", sub);
        char text[32];
        voltage (text, side ? live[cv] : saved (cv));
        large (r, x + 7, 31, text, fg);
        r.box (x + 7, 54, 139, 1, side ? pencil : rule);
    }
}

void Screen::cellDetail (Raster& r) const
{
    const int lane = cellPage ();
    const uint32_t cv = cvId (selection);
    const int k = selection % 8;
    char text[48];
    r.text (9, 3, "STEP VOLTAGE", muted);
    if (haveMemory)
        std::snprintf (text, sizeof (text), "CHANGE  %+.2f V", 5 * (live[cv] - saved (cv)));
    else
        std::snprintf (text, sizeof (text), "NO BASELINE");
    r.right (311, 3, text, amber);
    constexpr int left = 18, width = 284;
    for (int j = 0; j <= 20; ++j)
        r.box (left + j * width / 20, j % 2 ? 23 : 20, 1, j % 2 ? 4 : 10, rule);
    r.box (left, 25, width + 1, 1, muted);
    auto at = [&] (float v) { return left + int (std::lround ((v + 1) * .5f * width)); };
    if (haveMemory) r.frame (at (saved (cv)) - 3, 17, 7, 7, light);
    const int x = at (live[cv]);
    for (int dy = 0; dy < 5; ++dy)
        r.box (x - dy, 27 + dy, 2 * dy + 1, 1, amber);
    r.text (8, 36, "-5 V", muted);
    r.text (150, 36, "0 V", muted);
    r.right (312, 36, "+5 V", muted);
    r.box (5, 49, 310, 1, rule);
    const int length = int (live[lane ? ParamId::length2 : ParamId::length1]);
    std::snprintf (text, sizeof (text), "LANE %c", 'A' + lane);
    r.text (9, 56, text, light);
    for (int j = 0; j < 8; ++j)
    {
        const int pos = 67 + 19 * j;
        if (bit (int (live[bitsId (lane)]), j))
            r.box (pos, 56, 14, 8, j < length ? amber : rule);
        else
            r.frame (pos, 56, 14, 8, j < length ? muted : rule);
        if (j == k) r.box (pos, 67, 14, 2, amber);
    }
    std::snprintf (text, sizeof (text), "%d/8 STEPS", length);
    r.right (311, 56, text, muted);
    r.text (9, 76, "PUSH GATE", light);
    r.text (111, 76, "WHEEL CELL", muted);
    r.right (311, 76, "- BACK", light);
}

void Screen::list (Raster& r) const
{
    const int start = std::max (0, row () - 6);
    r.text (9, 4, editing ? "TURN TO EDIT" : "PARAMETERS", amber);
    r.right (311, 4, editing ? "PUSH TO FINISH" : "- / +  PAGE", muted);
    for (int i = start; i < std::min (rows, start + 8); ++i)
    {
        const auto& p = parameter (fields[i]);
        const bool active = i == row ();
        const int y = 19 + (i - start) * 17;
        if (active) r.box (5, y - 2, 310, 16, paper);
        r.text (10, y + 1, active ? (editing ? "*" : ">") : " ", brick);
        r.text (24, y + 1, p.name, active ? carbon : light);
        char text[48];
        shortValue (p.id, live[p.id], text);
        r.right (310, y + 1, p.kind == Kind::button ? "PUSH" : text, active ? brick : muted);
    }
}

void Screen::footer (Raster& r) const
{
    r.box (0, 0, 320, footH, film);
    if (shift)
    {
        r.text (6, 3,
                group       ? "Wheel selects   Push edits or triggers"
                : inspector ? "Wheel selects cell   Push toggles gate"
                            : "Wheel selects   Push opens cell or acts",
                light);
        r.text (6, 17,
                group       ? "While editing: Shift + wheel fine steps"
                : inspector ? "K1 voltage   K2 length   K3 corruption"
                            : "+ parameters   K1/K2 corruption   K3 clock",
                muted);
        return;
    }
    clap_id bindings[3];
    knobs (bindings);
    for (int k = 0; k < 3; ++k)
    {
        const int x = 5 + 105 * k;
        if (k) r.box (x - 3, 3, 1, 25, panel);
        char label[20];
        std::snprintf (label, sizeof (label), "K%d", k + 1);
        r.text (x, 3, label, amber);
        if (bindings[k] == CLAP_INVALID_ID) continue;
        const Param& p = parameter (bindings[k]);
        const char* const home[] = {"CORRUPT A", "CORRUPT B", "CLOCK DIV"};
        const char* const cell[] = {"STEP CV", "LENGTH", "CORRUPT"};
        std::snprintf (label, sizeof (label), "%.11s", group ? p.name : inspector ? cell[k] : home[k]);
        r.text (x + 19, 3, label, muted);
        char text[48];
        shortValue (p.id, live[p.id], text);
        text[14] = 0;
        r.text (x, 16, text, light);
        constexpr int width = 95;
        const float norm = (live[p.id] - p.min) / (p.max - p.min);
        r.box (x, 28, width, 1, rule);
        r.box (x, 28, int (std::lround (norm * width)), 1, amber);
        if (pots[k].position >= 0 && !pots[k].engaged)
            r.frame (x + pots[k].position * (width - 1) / 127 - 1, 26, 3, 4, light);
    }
}

bool Screen::paint (const melodee_screen_canvas_t& c)
{
    const bool whole = first || c.whole;
    bool params = false, knobsMoved = false;
    for (uint32_t i = 0; i < paramCount; ++i)
    {
        float& old = live[paramTable[i].id];
        const float value = feed.value (values, i);
        params |= old != value;
        old = value;
    }
    for (int k = 0; k < 3; ++k)
    {
        knobsMoved |= pots[k].engaged != c.knobs[k].engaged || pots[k].position != c.knobs[k].position;
        pots[k] = c.knobs[k];
    }
    const auto next = feed.heads.load (std::memory_order_acquire);
    bool moved = next != heads;
    heads = next;
    for (int s = 0; s < 2; ++s)
    {
        const auto serial = feed.hits[s].load (std::memory_order_acquire);
        if (serial != hits[s])
        {
            hits[s] = serial;
            hitAt[s] = c.now_ms;
        }
        const auto age = c.now_ms - hitAt[s];
        const int level = serial && age < 160 ? 4 - int (age / 40) : 0;
        moved |= flash[s] != level;
        flash[s] = level;
    }
    const bool content = whole || changed || params;
    const bool helpChanged = shift != c.shift;
    shift = c.shift;
    bool drew = false;
    if (content) drew |= picture.region<320, headerH> (c.pixels, c.stride, whole, 0, 0, [&] (Raster& r) { header (r); });
    if (!group)
    {
        if (content || memoryChanged)
            drew |= picture.region<320, topH> (c.pixels, c.stride, whole, 0, headerH,
                                               [&] (Raster& r)
                                               {
                                                   if (inspector)
                                                       cellValues (r);
                                                   else
                                                       films (r);
                                               });
        if (content || memoryChanged || (!inspector && moved))
            drew |= picture.region<320, bodyH> (c.pixels, c.stride, whole, 0, headerH + topH,
                                                [&] (Raster& r)
                                                {
                                                    if (inspector)
                                                        cellDetail (r);
                                                    else
                                                        card (r);
                                                });
    }
    else if (content)
        drew |= picture.region<320, topH + bodyH> (c.pixels, c.stride, whole, 0, headerH, [&] (Raster& r) { list (r); });
    if (content || knobsMoved || helpChanged)
        drew |= picture.region<320, footH> (c.pixels, c.stride, whole, 0, headerH + topH + bodyH,
                                            [&] (Raster& r) { footer (r); });
    first = changed = memoryChanged = false;
    return drew;
}
} // namespace psychic
