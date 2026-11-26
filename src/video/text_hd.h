// High-resolution text: the game's pixel-art letters and digits (menus,
// captions, score) are drawn from vector fonts instead of being upscaled.
//
// Files in textures/text/ (made by tools/make_text_atlas.py):
//   atlas.png   one slot per tile pattern: R = letter coverage, G = outline or
//               shadow coverage, B = position in the top-to-bottom gradient
//   glyphs.txt  pattern, hash of its pixels, atlas slot and the palette
//               nibbles of the gradient's top and bottom and of the edge
// A tile is replaced only while its pattern still holds the pixels the atlas
// was made from. Colours come from the game's palette every frame.
#pragma once
#include <cstdint>
#include <cstdio>

#include "../common/fnv1a.h"
#include <string>
#include <unordered_map>
#include <vector>

class TextHD {
public:
    static constexpr int CELLS_X = 62, CELLS_Y = 48;   // 496x384 in 8x8 cells
    // Per cell, 4 BGRA texels: [0] atlas slot (low byte in R, high byte in G,
    // A = 255 when the cell is text), [1] gradient top, [2] gradient bottom,
    // [3] edge colour (A = 0 when that colour is transparent)
    static constexpr int CELL_TEXELS = 4;
    // One more row after the cells lists the 2D replacement pictures on screen
    // (Pack2D::apply): texel 0 R = how many, then per picture a texel with its
    // rectangle in cells (R x0, G y0, B x1, A y1) and one with its slot in R
    static constexpr int ROWS = CELLS_Y + 1, ROW_TEXELS = CELLS_X * CELL_TEXELS;
    static constexpr int ATLAS_COLS = 16;   // tools/make_text_atlas.py COLS

    struct Glyph {
        uint64_t hash = 0;
        uint16_t slot = 0;
        uint8_t top = 0, bottom = 0, edge = 0;   // palette nibbles
        bool used = false;
    };

    // A glyph drawn with tiles shared by other glyphs (Star Wars Trilogy's
    // digits: 3x3 pieces): found as a whole block, each cell with its own slot.
    // glyphs.txt: G width height top bottom edge, then pattern:hash:slot per cell
    struct Group {
        int w = 0, h = 0;
        uint8_t top = 0, bottom = 0, edge = 0;
        std::vector<uint32_t> pattern;
        std::vector<uint64_t> hash;
        std::vector<uint16_t> slot;
        std::vector<bool> blank;   // the piece shows nothing (any empty cell matches it)
        int anchor = -1;           // 8 bpp: the first cell that is not empty
    };
    std::vector<Group> groups;
    std::vector<Group> groups8;   // 8 bpp blocks, by the hashes of their cells (glyphs.txt "G8")

    bool init(const std::string& dir);
    // Adds another folder's glyphs (its atlas below this one's, its slots moved by as many): Wheelkit's letters of
    // the arcade's screens (textures/text_wheelkit, the Redrawn letters option). Modified version, not upstream's
    bool add(const std::string& dir);
    bool empty() const { return count_ == 0 && groups.empty() && groups8.empty(); }
    // Glyph for a 4 bpp pattern, if its pixels in `vram` are the expected ones
    const Glyph* find(uint32_t pattern, const uint8_t* vram) const;
    // ... for an 8 bpp pattern (64 bytes; top/bottom/edge are then palette
    // entries 1-255 of the tile's bank of 256)
    const Glyph* find8(uint32_t pattern, const uint8_t* vram) const;
    static bool pattern_is(uint32_t pattern, uint64_t hash, const uint8_t* vram) {
        return fnv1a_bytes(vram + pattern * 32, 32) == hash;
    }

    int atlas_w = 0, atlas_h = 0;
    std::vector<uint8_t> atlas_rgba;

private:
    bool read_glyphs(FILE* f, unsigned offset);   // glyphs.txt, its slots moved by offset; closes f
    std::vector<Glyph> glyphs_;   // indexed by pattern number
    std::unordered_map<uint64_t, Glyph> glyphs8_;  // 8 bpp ones (glyphs.txt "B" lines), by the hash of their pixels
    int count_ = 0;
};
