#include "tilegen.h"

#include "../common/fnv1a.h"

#include <algorithm>
#include <cstring>

#include "sky_pack.h"
#include "text_hd.h"

namespace {
constexpr uint32_t LINE_SCROLL = 0xF6000, MASK = 0xF7000, NAME_TABLES = 0xF8000, PALETTE = 0x100000,
                   REGS = 0x180000;

inline uint32_t le32(const uint8_t* p) {
    uint32_t v;
    memcpy(&v, p, 4);
    return v;   // x86 is little-endian, like the device
}

// Palette entry -> BGRA with the layer pair's colour offset; 0 if transparent
inline uint32_t to_bgra(uint32_t c, const int* offset) {
    if (c & 0x8000) return 0;
    int r = int(c & 31) * 255 / 31 + offset[0];
    int g = int((c >> 5) & 31) * 255 / 31 + offset[1];
    int b = int((c >> 10) & 31) * 255 / 31 + offset[2];
    r = std::clamp(r, 0, 255);
    g = std::clamp(g, 0, 255);
    b = std::clamp(b, 0, 255);
    return 0xFF000000u | uint32_t(r) << 16 | uint32_t(g) << 8 | uint32_t(b);
}

// Position of screen pixel (x, y) in a layer's 512x512 picture
inline void layer_position(const uint8_t* vram, int layer, int x, int y, uint32_t scroll, int& px, int& py) {
    int xs = int(scroll & 0x3FF);
    int ys = int((scroll >> 16) & 0x1FF);
    if (scroll & 0x8000) {   // line scroll: X comes from the table
        uint32_t w = le32(vram + LINE_SCROLL + 0x400 * layer + (y / 2) * 4);
        xs = int((y & 1) ? (w & 0xFFFF) : (w >> 16)) & 0x3FF;
    }
    px = (x + xs) & 511;
    py = (y + ys) & 511;
}

// Pixel (px, py) of a layer's picture, 0 if transparent/empty
uint32_t plane_pixel(const uint8_t* vram, int layer, int px, int py, bool four_bpp, const int* offset) {
    uint32_t pair = le32(vram + NAME_TABLES + 0x2000 * layer + ((py / 8) * 32 + px / 16) * 4);
    uint32_t e = (px & 8) ? (pair & 0xFFFF) : (pair >> 16);
    int fx = px & 7, fy = py & 7;
    uint32_t index;
    if (four_bpp) {
        uint32_t pattern = ((e & 0x3FFF) << 1) | (e >> 15);
        uint32_t row = le32(vram + ((pattern * 32 + fy * 4) % LINE_SCROLL));
        index = (e & 0x7FF0) | ((row >> ((7 - fx) * 4)) & 15);
    } else {
        uint32_t pattern = e & 0x3FFF;
        uint32_t row = le32(vram + ((pattern * 64 + fy * 8 + (fx & 4)) % LINE_SCROLL));
        index = (e & 0x7F00) | ((row >> ((3 - (fx & 3)) * 8)) & 0xFF);
    }
    return to_bgra(le32(vram + PALETTE + index * 4), offset);
}

// One row (py) of a layer's 512-pixel picture as palette indices: the same
// lookups as plane_pixel, with the name table entry and the pattern row read
// once per tile instead of once per pixel
void decode_row(const uint8_t* vram, int layer, int py, bool four_bpp, uint16_t* row) {
    const uint8_t* names = vram + NAME_TABLES + 0x2000 * layer + (py / 8) * 32 * 4;
    int fy = py & 7;
    for (int tx = 0; tx < 64; tx++) {
        uint32_t pair = le32(names + (tx / 2) * 4);
        uint32_t e = (tx & 1) ? (pair & 0xFFFF) : (pair >> 16);
        uint16_t* out = row + tx * 8;
        if (four_bpp) {
            uint32_t pattern = ((e & 0x3FFF) << 1) | (e >> 15);
            uint32_t bits = le32(vram + ((pattern * 32 + fy * 4) % LINE_SCROLL));
            for (int fx = 0; fx < 8; fx++) out[fx] = uint16_t((e & 0x7FF0) | ((bits >> ((7 - fx) * 4)) & 15));
        } else {
            uint32_t pattern = e & 0x3FFF;
            uint32_t lo = le32(vram + ((pattern * 64 + fy * 8) % LINE_SCROLL));
            uint32_t hi = le32(vram + ((pattern * 64 + fy * 8 + 4) % LINE_SCROLL));
            for (int fx = 0; fx < 8; fx++)
                out[fx] = uint16_t((e & 0x7F00) | (((fx & 4) ? hi : lo) >> ((3 - (fx & 3)) * 8) & 0xFF));
        }
    }
}

// Palette entry -> colour for one set of offsets, remembered within a
// compose(): the game uses a few hundred entries per frame, each on many
// pixels. stamp marks the entries converted in the current compose.
struct PaletteCache {
    uint32_t colour[32768];
    uint32_t stamp[32768];
};
thread_local PaletteCache g_palette_cache[3];   // offsets of pair A/A', of pair B/B', none
thread_local uint32_t g_palette_generation = 0;

// Pixel of one layer at screen position (x, y), 0 if transparent/empty
uint32_t layer_pixel(const uint8_t* vram, int layer, int x, int y, bool four_bpp, uint32_t scroll,
                     const int* offset) {
    int px, py;
    layer_position(vram, layer, x, y, scroll, px, py);
    return plane_pixel(vram, layer, px, py, four_bpp, offset);
}

// A BGRA colour with a layer pair's colour offset
inline uint32_t with_offset(uint32_t c, const int* offset) {
    int r = std::clamp(int((c >> 16) & 0xFF) + offset[0], 0, 255);
    int g = std::clamp(int((c >> 8) & 0xFF) + offset[1], 0, 255);
    int b = std::clamp(int(c & 0xFF) + offset[2], 0, 255);
    return 0xFF000000u | uint32_t(r) << 16 | uint32_t(g) << 8 | uint32_t(b);
}

// Cells of the foreground drawn as high-resolution text (text_hd.h): the
// topmost non-empty tile of each cell, when the game shows it on the 8x8 grid
// and it is a known glyph. Returns the layer that holds it per cell (-1: none).
void find_text(const uint8_t* vram, const TextHD& text, const uint32_t* scroll, uint32_t format,
               const int offsets[2][3], int8_t* text_layer, uint32_t* cells) {
    constexpr int CX = TextHD::CELLS_X, CY = TextHD::CELLS_Y;
    memset(cells, 0, CX * CY * TextHD::CELL_TEXELS * 4);
    // For the groups: each cell's topmost non-empty tile that is no glyph
    // (pattern, name table entry, layer, layer pair; pattern ~0u: none)
    struct Seen { uint32_t pattern = ~0u, e = 0; int8_t layer = -1, pair = 0; };
    static Seen seen[CX * CY];
    for (Seen& s : seen) s = Seen{};
    // ... and for the 8 bpp blocks, the hash of that tile's pixels (0: none)
    struct Seen8 { uint64_t hash = 0; uint32_t e = 0; int8_t layer = -1, pair = 0; };
    static Seen8 seen8[CX * CY];
    for (Seen8& s : seen8) s = Seen8{};
    auto colour_of = [&](uint32_t e, int pair, uint32_t nibble) {
        return nibble ? to_bgra(le32(vram + PALETTE + ((e & 0x7FF0) | nibble) * 4), offsets[pair]) : 0u;
    };
    for (int cy = 0; cy < CY; cy++)
        for (int cx = 0; cx < CX; cx++) {
            int8_t& found = text_layer[cy * CX + cx];
            found = -1;
            int column = cx / 4;   // A/A' and B/B' are chosen per 32-pixel column
            for (int pair = 0; pair < 2; pair++) {
                // The selection must be the same on the 8 rows of the cell
                uint32_t bit = 0;
                bool mixed = false;
                for (int y = 0; y < 8; y++) {
                    uint32_t mask = le32(vram + MASK + 4 * (cy * 8 + y));
                    uint32_t bits = pair == 0 ? (mask >> 16) : (mask & 0xFFFF);
                    uint32_t b = (bits >> (15 - column)) & 1;
                    if (y == 0) bit = b;
                    else mixed |= b != bit;
                }
                if (mixed) break;
                int layer = pair * 2 + (bit ? 0 : 1);
                if (!(scroll[layer] & 0x80000000u) || !(format & (1u << (8 + layer)))) continue;
                int xs = int(scroll[layer] & 0x3FF), ys = int((scroll[layer] >> 16) & 0x1FF);
                bool four_bpp = format & (1u << (12 + layer));
                if (scroll[layer] & 0x8000) {
                    // Line scroll (Sega Rally 2's RANKING slides its rows in): a cell whose 8 lines share one X is
                    // on the grid like a plain scroll. Wheelkit (modified version, not upstream's)
                    int line_x = -1;
                    bool same = true;
                    for (int y = cy * 8; y < cy * 8 + 8 && same; y++) {
                        uint32_t w = le32(vram + LINE_SCROLL + 0x400 * layer + (y / 2) * 4);
                        int x = int((y & 1) ? (w & 0xFFFF) : (w >> 16)) & 0x3FF;
                        same = line_x < 0 || x == line_x;
                        line_x = x;
                    }
                    if (!same) break;
                    xs = line_x;
                }
                if ((xs & 7) || (ys & 7)) break;
                int px = (cx * 8 + xs) & 511, py = (cy * 8 + ys) & 511;
                uint32_t pair_word = le32(vram + NAME_TABLES + 0x2000 * layer + ((py / 8) * 32 + px / 16) * 4);
                uint32_t e = (px & 8) ? (pair_word & 0xFFFF) : (pair_word >> 16);
                // 4 bpp (32 bytes a pattern) or 8 bpp (64: Sega Rally 2's HUD)
                uint32_t pattern = four_bpp ? ((e & 0x3FFF) << 1) | (e >> 15) : e & 0x3FFF;
                int size = four_bpp ? 32 : 64;
                const uint8_t* bytes = vram + (pattern * size) % LINE_SCROLL;
                bool blank = true;
                for (int i = 0; i < size && blank; i++) blank = bytes[i] == 0;
                if (blank) continue;   // transparent: look at the pair below
                const TextHD::Glyph* g = four_bpp ? text.find(pattern, vram) : text.find8(pattern, vram);
                if (!g) {
                    if (four_bpp) seen[cy * CX + cx] = Seen{pattern, e, int8_t(layer), int8_t(pair)};
                    else if (!text.groups8.empty())
                        seen8[cy * CX + cx] = Seen8{fnv1a_bytes(bytes, 64), e, int8_t(layer), int8_t(pair)};
                    break;
                }
                found = int8_t(layer);
                uint32_t* c = cells + (cy * CX + cx) * TextHD::CELL_TEXELS;
                const uint32_t bank = e & (four_bpp ? 0x7FF0u : 0x7F00u);
                auto colour = [&](uint32_t nibble) {
                    return nibble ? to_bgra(le32(vram + PALETTE + (bank | nibble) * 4), offsets[pair]) : 0u;
                };
                c[0] = 0xFF000000u | uint32_t(g->slot & 0xFF) << 16 | uint32_t(g->slot >> 8) << 8;
                c[1] = colour(g->top);
                c[2] = colour(g->bottom);
                c[3] = colour(g->edge);
                break;
            }
        }
    // Groups: the whole block on screen, from its top left piece
    for (const TextHD::Group& gr : text.groups)
        for (int cy = 0; cy + gr.h <= CY; cy++)
            for (int cx = 0; cx + gr.w <= CX; cx++) {
                const Seen& anchor = seen[cy * CX + cx];
                if (anchor.pattern != gr.pattern[0] || text_layer[cy * CX + cx] >= 0) continue;
                bool match = true;
                for (int k = 0; k < gr.w * gr.h && match; k++) {
                    int i = (cy + k / gr.w) * CX + cx + k % gr.w;
                    const Seen& s = seen[i];
                    if (text_layer[i] >= 0) match = false;
                    else if (gr.blank[k]) match = s.pattern == ~0u || s.pattern == gr.pattern[k];
                    else match = s.pattern == gr.pattern[k] && TextHD::pattern_is(s.pattern, gr.hash[k], vram);
                }
                if (!match) continue;
                for (int k = 0; k < gr.w * gr.h; k++) {
                    int i = (cy + k / gr.w) * CX + cx + k % gr.w;
                    text_layer[i] = anchor.layer;
                    uint32_t* c = cells + i * TextHD::CELL_TEXELS;
                    c[0] = 0xFF000000u | uint32_t(gr.slot[k] & 0xFF) << 16 | uint32_t(gr.slot[k] >> 8) << 8;
                    c[1] = colour_of(anchor.e, anchor.pair, gr.top);
                    c[2] = colour_of(anchor.e, anchor.pair, gr.bottom);
                    c[3] = colour_of(anchor.e, anchor.pair, gr.edge);
                }
            }
    // 8 bpp blocks (Sega Rally 2's position and time left): found from their
    // first tile that is not empty, every cell's pixels as listed, the empty
    // ones empty; all of the block is drawn (the new letters may reach into
    // the empty cells), coloured from the first tile's palette bank
    for (const TextHD::Group& gr : text.groups8) {
        int ay = gr.anchor / gr.w, ax = gr.anchor % gr.w;
        for (int i = 0; i < CX * CY; i++) {
            const Seen8& anchor = seen8[i];
            if (anchor.hash != gr.hash[gr.anchor]) continue;
            int cy = i / CX - ay, cx = i % CX - ax;
            if (cy < 0 || cx < 0 || cy + gr.h > CY || cx + gr.w > CX) continue;
            bool match = true;
            for (int k = 0; k < gr.w * gr.h && match; k++) {
                int j = (cy + k / gr.w) * CX + cx + k % gr.w;
                match = text_layer[j] < 0 && seen8[j].hash == gr.hash[k];
            }
            if (!match) continue;
            const uint32_t bank = anchor.e & 0x7F00u;
            auto colour = [&](uint32_t n) {
                return n ? to_bgra(le32(vram + PALETTE + (bank | n) * 4), offsets[anchor.pair]) : 0u;
            };
            for (int k = 0; k < gr.w * gr.h; k++) {
                int j = (cy + k / gr.w) * CX + cx + k % gr.w;
                text_layer[j] = anchor.layer;
                uint32_t* c = cells + j * TextHD::CELL_TEXELS;
                c[0] = 0xFF000000u | uint32_t(gr.slot[k] & 0xFF) << 16 | uint32_t(gr.slot[k] >> 8) << 8;
                c[1] = colour(gr.top);
                c[2] = colour(gr.bottom);
                c[3] = colour(gr.edge);
            }
        }
    }
}
}  // namespace

void TileGen::front_offset(const uint8_t* vram, int out[3]) {
    uint32_t o = le32(vram + REGS + 0x40);
    for (int c = 0; c < 3; c++) out[c] = 2 * int(int8_t((o >> (8 * c)) & 0xFF));
}

void TileGen::layer_plane(const uint8_t* vram, int layer, bool four_bpp, std::vector<uint32_t>& out) {
    static const int no_offset[3] = {0, 0, 0};
    out.resize(512 * 512);
    for (int py = 0; py < 512; py++)
        for (int px = 0; px < 512; px++) out[py * 512 + px] = plane_pixel(vram, layer, px, py, four_bpp, no_offset);
}

uint64_t TileGen::layer_key(const uint8_t* vram, int layer, bool four_bpp) {
    uint64_t h = FNV1A_BASIS ^ (four_bpp ? 1 : 2);
    auto mix = [&](uint64_t v) { h = fnv1a_mix(h, v); };
    // Palette banks: 16 colours each in 4 bpp, 256 in 8 bpp
    bool used[2048] = {};
    for (int i = 0; i < 64 * 64; i++) {
        uint32_t pair = le32(vram + NAME_TABLES + 0x2000 * layer + (i / 2) * 4);
        uint32_t e = (i & 1) ? (pair & 0xFFFF) : (pair >> 16);
        mix(e);
        uint32_t start, bytes;
        if (four_bpp) {
            start = ((((e & 0x3FFF) << 1) | (e >> 15)) * 32) % LINE_SCROLL;
            bytes = 32;
            used[(e & 0x7FF0) >> 4] = true;
        } else {
            start = ((e & 0x3FFF) * 64) % LINE_SCROLL;
            bytes = 64;
            used[(e & 0x7F00) >> 4] = true;
        }
        for (uint32_t b = 0; b < bytes; b += 8) {
            uint64_t v;
            memcpy(&v, vram + start + b, 8);
            mix(v);
        }
    }
    int bank_colours = four_bpp ? 16 : 256;
    for (int bank = 0; bank < 2048; bank++)
        if (used[bank])
            for (int c = 0; c < bank_colours; c += 2) {
                uint64_t v;
                memcpy(&v, vram + PALETTE + (bank * 16 + c) * 4, 8);
                mix(v);
            }
    return h;
}

void TileGen::compose(const uint8_t* vram, const Output& out, const TextHD* text, SkyPack* sky) {
    uint32_t* back = out.back;
    uint32_t* front = out.front;
    uint32_t* front_raw = out.front_raw;
    uint32_t* front_raw_top = out.front_raw_top;
    uint32_t* front_under = out.front_under;
    uint32_t* back_raw = out.back_raw;
    uint32_t* back_raw_top = out.back_raw_top;
    uint32_t* back_under = out.back_under;
    uint32_t* text_cells = out.text_cells;
    uint32_t* sky_coords = out.sky_coords;
    int32_t* back_rows = out.back_rows;
    static const int no_offset[3] = {0, 0, 0};
    const uint8_t* regs = vram + REGS;
    uint32_t format = le32(regs + 0x20);
    int offsets[2][3];
    for (int pair = 0; pair < 2; pair++) {
        uint32_t o = le32(regs + 0x40 + 4 * pair);
        for (int c = 0; c < 3; c++) offsets[pair][c] = 2 * int(int8_t((o >> (8 * c)) & 0xFF));
    }
    uint32_t scroll[4];
    for (int l = 0; l < 4; l++) scroll[l] = le32(regs + 0x60 + 4 * l);

    // A background is only extended beyond the screen if it is a wrapping
    // panorama. Framed pictures (the intro's cloud backdrop) leave the 16
    // off-screen columns of the 512-pixel map black or empty, and repeating
    // them would tile the picture at the sides: those keep black sides.
    bool extend[4] = {false, false, false, false};
    for (int layer = 0; layer < 4; layer++) {
        if (!(scroll[layer] & 0x80000000u) || (format & (1u << (8 + layer)))) continue;
        int dark = 0, total = 0;
        for (int y = 0; y < SCREEN_H; y += 2)
            for (int x = SCREEN_W; x < 512; x++) {
                uint32_t c = layer_pixel(vram, layer, x, y, format & (1u << (12 + layer)), scroll[layer],
                                         offsets[layer / 2]);
                total++;
                if (((c >> 16) & 0xFF) < 24 && ((c >> 8) & 0xFF) < 24 && (c & 0xFF) < 24) dark++;
            }
        extend[layer] = dark * 10 < total * 9;
    }

    // Text drawn in high resolution is left out of the foreground image
    int8_t text_layer[TextHD::CELLS_X * TextHD::CELLS_Y];
    bool has_text = text && text_cells && !text->empty();
    if (has_text) find_text(vram, *text, scroll, format, offsets, text_layer, text_cells);
    else if (text_cells) memset(text_cells, 0, TextHD::CELLS_X * TextHD::CELLS_Y * TextHD::CELL_TEXELS * 4);
    if (text_cells) text_cells[TextHD::CELLS_Y * TextHD::ROW_TEXELS] = 0;   // no replacement pictures yet

    // Replaced backdrops (sky_pack.h)
    const SkyPack::Picture* sky_picture[4] = {nullptr, nullptr, nullptr, nullptr};
    if (sky) {
        for (int layer = 0; layer < 4; layer++)
            if ((scroll[layer] & 0x80000000u) && !(format & (1u << (8 + layer))))
                sky_picture[layer] = sky->find(vram, layer, format & (1u << (12 + layer)));
        sky->frame_done();
    }
    if (sky_coords) {
        memset(sky_coords, 0, SKY_COORDS_W * SKY_COORDS_H * 4);
        for (int pair = 0; pair < 2; pair++) {
            uint32_t& o = sky_coords[SCREEN_H * SKY_COORDS_W + pair];
            o = 0xFF000000u;
            for (int c = 0; c < 3; c++) o |= uint32_t(offsets[pair][c] / 2 + 128) << (16 - 8 * c);
        }
    }

    memset(back, 0, BACK_W * SCREEN_H * 4);
    memset(front, 0, SCREEN_W * SCREEN_H * 4);
    if (front_raw) memset(front_raw, 0, SCREEN_W * SCREEN_H * 4);
    if (front_raw_top) memset(front_raw_top, 0, SCREEN_W * SCREEN_H * 4);
    if (back_raw) memset(back_raw, 0, SCREEN_W * SCREEN_H * 4);
    if (back_raw_top) memset(back_raw_top, 0, SCREEN_W * SCREEN_H * 4);
    if (back_under) memset(back_under, 0, SCREEN_W * SCREEN_H * 4);
    if (front_under) memset(front_under, 0, SCREEN_W * SCREEN_H * 4);
    if (++g_palette_generation == 0) {   // wrapped: forget every entry
        for (PaletteCache& pc : g_palette_cache) memset(pc.stamp, 0, sizeof(pc.stamp));
        g_palette_generation = 1;
    }
    const int* offset_sets[3] = {offsets[0], offsets[1], no_offset};
    // A layer is decoded at most once per screen row (its picture row and
    // horizontal scroll are fixed along the row), the first time a pixel of
    // the row needs it; the pixels are then looked up
    uint16_t rows[4][512];
    int row_xs[4] = {0, 0, 0, 0}, row_py[4] = {0, 0, 0, 0};
    bool decoded[4];
    for (int y = 0; y < SCREEN_H; y++) {
        uint32_t mask = le32(vram + MASK + 4 * y);
        decoded[0] = decoded[1] = decoded[2] = decoded[3] = false;
        auto row_of = [&](int layer) {
            if (!decoded[layer]) {
                decoded[layer] = true;
                layer_position(vram, layer, 0, y, scroll[layer], row_xs[layer], row_py[layer]);
                decode_row(vram, layer, row_py[layer], format & (1u << (12 + layer)), rows[layer]);
            }
        };
        // Colour of a layer at screen column x (0 if transparent) with offset
        // set `which` (0, 1: the layer pairs' offsets; 2: none)
        auto pixel_at = [&](int layer, int x, int which) {
            row_of(layer);
            uint16_t index = rows[layer][(x + row_xs[layer]) & 511];
            PaletteCache& pc = g_palette_cache[which];
            if (pc.stamp[index] != g_palette_generation) {
                pc.stamp[index] = g_palette_generation;
                pc.colour[index] = to_bgra(le32(vram + PALETTE + index * 4), offset_sets[which]);
            }
            return pc.colour[index];
        };
        int shown_by[4] = {0, 0, 0, 0};   // back pixels on screen each layer shows on this row
        for (int x = -BACK_MARGIN; x < SCREEN_W + BACK_MARGIN; x++) {
            bool on_screen = x >= 0 && x < SCREEN_W;
            int column = std::clamp(x, 0, SCREEN_W - 1) / 32;
            int back_layer = -1;
            // Pair B/B' first, then A/A' on top; the mask picks primary or alternate
            for (int pair = 1; pair >= 0; pair--) {
                uint32_t bits = pair == 0 ? (mask >> 16) : (mask & 0xFFFF);
                int layer = pair * 2 + (((bits >> (15 - column)) & 1) ? 0 : 1);
                if (!(scroll[layer] & 0x80000000u)) continue;
                bool above_3d = format & (1u << (8 + layer));
                if (!on_screen && (above_3d || !extend[layer])) continue;
                if (!above_3d && on_screen && (back_raw || back_raw_top)) {
                    uint32_t raw = pixel_at(layer, x, 2);
                    if (raw && back_raw) back_raw[y * SCREEN_W + x] = raw;
                    if (raw && back_raw_top && pair == 0) back_raw_top[y * SCREEN_W + x] = raw;
                }
                if (sky_picture[layer]) {
                    row_of(layer);
                    int px = (x + row_xs[layer]) & 511, py = row_py[layer];
                    uint32_t c = sky_picture[layer]->low[py * SkyPack::SIZE + px];
                    int i = y * BACK_W + x + BACK_MARGIN;
                    if (c) back[i] = with_offset(c, offsets[pair]);
                    if (c) back_layer = layer;
                    if (c && back_under && on_screen && pair == 1) back_under[y * SCREEN_W + x] = back[i];
                    // Where this picture is transparent a replaced layer below keeps showing
                    if (sky_coords && (c || !sky_coords[i]))
                        sky_coords[i] = 0xFF000000u | uint32_t(px & 255) << 16 | uint32_t(py & 255) << 8 |
                                        (sky_picture[layer]->slot - SkyPack::FIRST_SLOT) << 2 | uint32_t(pair) << 5 |
                                        uint32_t(py >> 8) << 1 | uint32_t(px >> 8);
                    continue;
                }
                uint32_t c = pixel_at(layer, x, pair);
                if (!c) continue;
                if (above_3d) {
                    if (!has_text || text_layer[(y / 8) * TextHD::CELLS_X + x / 8] != layer)
                        front[y * SCREEN_W + x] = c;
                    if (front_under && pair == 1) front_under[y * SCREEN_W + x] = c;
                    if (front_raw || front_raw_top) {
                        uint32_t raw = pixel_at(layer, x, 2);
                        if (front_raw) front_raw[y * SCREEN_W + x] = raw;
                        if (front_raw_top && pair == 0) front_raw_top[y * SCREEN_W + x] = raw;
                    }
                }
                else {
                    back[y * BACK_W + x + BACK_MARGIN] = c;
                    back_layer = layer;
                    if (back_under && on_screen && pair == 1) back_under[y * SCREEN_W + x] = c;
                    if (sky_coords) sky_coords[y * BACK_W + x + BACK_MARGIN] = 0;
                }
            }
            if (on_screen && back_layer >= 0) shown_by[back_layer]++;
        }
        if (back_rows) {
            int best = 0;
            for (int l = 1; l < 4; l++)
                if (shown_by[l] > shown_by[best]) best = l;
            int32_t* r = back_rows + 3 * y;
            r[0] = shown_by[best] ? best : -1;
            r[1] = row_xs[best];
            r[2] = row_py[best];
        }
    }
}
