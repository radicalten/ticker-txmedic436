#define IS_GUI // Tells 3ds_bridge.h to let this file write directly to the screens
#include <nds.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "3ds_bridge.h"

// Hardware Background Layer IDs
int bg_board_id;
int bg_pieces_id;

PrintConsole topConsole, bottomConsole;

// Navigation and Cycling Control
int cursor_x = 0;        // Current Palette selection (0 - 15)
int cursor_y = 0;        // Current Color Index selection (0 - 14, mapping to index 1 - 15)
int active_mode = 0;     // 0: HSV Spectrum, 1: RGB Ramps, 2: Classic/ANSI, 3: Grayscale/Pastel
int auto_cycle = 1;      // Automated color phase cycling toggle
int cycle_phase = 0;     // Hue/Color shifting offset
int cycle_speed = 2;     // Increment steps per frame (0 - 8)
int redraw_needed = 1;

const char* mode_names[] = { "HSV SPECTRUM", "RGB RAMPS", "CLASSIC SYSTEM", "GRAY/PASTEL" };

// Map coordinates helper
void set_tile(u16* map, int x, int y, u16 tile, u16 palette) {
    if (x >= 0 && x < 32 && y >= 0 && y < 32) {
        map[y * 32 + x] = tile | (palette << 12);
    }
}

// String printing on graphics map helper
void draw_string(u16* map, int x, int y, const char* str, u16 palette) {
    while (*str) {
        set_tile(map, x, y, (u16)(*str), palette);
        x++;
        str++;
    }
}

// Fixed integer-based HSV to RGB15 converter
u16 hsv_to_rgb15(int h, int s, int v) {
    // h: 0..359, s: 0..31, v: 0..31
    if (s == 0) return RGB15(v, v, v);

    int base = ((31 - s) * v) / 31;
    int sex = h / 60;
    int ext = h % 60;
    
    int r = 0, g = 0, b = 0;
    int down = (v * (60 - ext)) / 60;
    int up = (v * ext) / 60;

    switch (sex) {
        case 0: r = v; g = up; b = base; break;
        case 1: r = down; g = v; b = base; break;
        case 2: r = base; g = v; b = up; break;
        case 3: r = base; g = down; b = v; break;
        case 4: r = up; g = base; b = v; break;
        default: r = v; g = base; b = down; break;
    }
    return RGB15(r, g, b);
}

// Generates dynamic and static color palettes based on mode and phase offset
void update_palettes(void) {
    for (int p = 0; p < 16; p++) {
        // Color Index 0 is transparent on GBA/DS BG hardware
        BG_PALETTE[p * 16 + 0] = RGB15(0, 0, 0); 

        for (int i = 1; i < 16; i++) {
            u16 color = 0;
            int idx = p * 15 + (i - 1); // 0 to 239 flat index

            switch (active_mode) {
                case 0: { // Dynamic HSV Hue sweep
                    int hue = (idx * 360 / 240 + cycle_phase) % 360;
                    color = hsv_to_rgb15(hue, 28, 31);
                    break;
                }
                case 1: { // RGB Step Matrix with shifting phase intensity
                    int r = (idx % 8) * 4 + (cycle_phase / 4) % 4;
                    int g = ((idx / 8) % 8) * 4 + (cycle_phase / 8) % 4;
                    int b = (idx / 64) * 8;
                    color = RGB15(r & 31, g & 31, b & 31);
                    break;
                }
                case 2: { // Classic / ANSI color tables with phase shifts
                    int base_idx = (idx + (cycle_phase / 10)) % 16;
                    u16 base_colors[16] = {
                        RGB15(0,0,0),    RGB15(16,0,0),  RGB15(0,16,0),  RGB15(16,16,0),
                        RGB15(0,0,16),   RGB15(16,0,16), RGB15(0,16,16), RGB15(24,24,24),
                        RGB15(10,10,10), RGB15(31,0,0),  RGB15(0,31,0),  RGB15(31,31,0),
                        RGB15(0,0,31),   RGB15(31,0,31), RGB15(0,31,31), RGB15(31,31,31)
                    };
                    color = base_colors[base_idx];
                    break;
                }
                case 3: { // Grayscale & Pastel Ramp combinations
                    int val = (idx % 16) * 2;
                    int pastel_hue = (idx * 360 / 240 + cycle_phase) % 360;
                    if (p < 4) {
                        color = RGB15(val, val, val); // Pure Grayscale
                    } else {
                        color = hsv_to_rgb15(pastel_hue, 12, 28); // Low-saturation pastel sweep
                    }
                    break;
                }
            }
            BG_PALETTE[p * 16 + i] = color;
        }
    }

    // Set Palette 10/11 for UI Labels (Pure White and Dull Gray)
    BG_PALETTE[10 * 16 + 1] = RGB15(31, 31, 31);
    BG_PALETTE[11 * 16 + 1] = RGB15(15, 15, 15);
}

// Configure bottom screen text color profiles
void init_bottom_palette(void) {
    BG_PALETTE_SUB[0]  = RGB15(2, 2, 3);        // Near-black background
    BG_PALETTE_SUB[1]  = RGB15(31, 8, 8);       // Crimson
    BG_PALETTE_SUB[2]  = RGB15(8, 31, 8);       // Emerald
    BG_PALETTE_SUB[3]  = RGB15(31, 22, 4);      // Amber
    BG_PALETTE_SUB[4]  = RGB15(8, 16, 31);      // Cobalt Blue
    BG_PALETTE_SUB[7]  = RGB15(25, 25, 25);     // Off-white
    BG_PALETTE_SUB[8]  = RGB15(12, 12, 12);     // Slate outline
    BG_PALETTE_SUB[15] = RGB15(31, 31, 31);     // Crisp White
}

// Render Top Screen containing the color sweeps matrix
void draw_top_test_matrix(void) {
    u16* board_map = bgGetMapPtr(bg_board_id);
    u16* pieces_map = bgGetMapPtr(bg_pieces_id);

    // Wipe layout maps
    memset(board_map, 0, 32 * 32 * sizeof(u16));
    memset(pieces_map, 0, 32 * 32 * sizeof(u16));

    draw_string(pieces_map, 2, 1, "=== NDS COMPREHENSIVE SWEEPER ===", 10);
    draw_string(pieces_map, 2, 3, "P0 P1 P2 P3 P4 P5 P6 P7 P8 P9 PA PB PC PD PE PF", 11);

    for (int r = 0; r < 15; r++) {
        for (int c = 0; c < 16; c++) {
            // Write solid background swatches on BG2
            set_tile(board_map, 2 + c * 1.75, 5 + r, 1, c); 

            // Handle HUD Selection cursor overlays on BG1
            if (c == cursor_x && r == cursor_y) {
                set_tile(pieces_map, 2 + c * 1.75, 5 + r, 'X', 10);
            } else {
                set_tile(pieces_map, 2 + c * 1.75, 5 + r, '.', 11);
            }
        }
    }
}

// Display real-time specs using high-density, condensed labels
void draw_bottom_specs(void) {
    consoleSelect(&bottomConsole);
    printf("\x1b[1;1H");

    int palette_id = cursor_x;
    int color_index = cursor_y + 1;

    u16 raw_color = BG_PALETTE[palette_id * 16 + color_index];

    // Decode 5-bit channels
    int r_5 = (raw_color >> 0) & 0x1F;
    int g_5 = (raw_color >> 5) & 0x1F;
    int b_5 = (raw_color >> 10) & 0x1F;

    // Convert channels to 8-bit scale
    int r_8 = (r_5 * 255) / 31;
    int g_8 = (g_5 * 255) / 31;
    int b_8 = (b_5 * 255) / 31;

    printf("\x1b[1;33m--- COMPREHENSIVE DISPLAY TEST DIAGNOSTICS ---\x1b[0m\x1b[K\n");
    printf("\x1b[1;30m==============================================\x1b[0m\x1b[K\n\n");

    printf("  \x1b[1;36m[SYS STATUS]\x1b[0m\x1b[K\n");
    printf("  MD  : %s\x1b[K\n", mode_names[active_mode]);
    printf("  SEL : P%d / IDX %d\x1b[K\n", palette_id, color_index);
    printf("  AUT : %s (SPD: %d)\x1b[K\n", auto_cycle ? "ACTIVE" : "PAUSED", cycle_speed);
    printf("  PHS : %d\x1b[K\n\n", cycle_phase);

    printf("  \x1b[1;36m[COLOR ANALYSIS]\x1b[0m\x1b[K\n");
    printf("  BGR : %02d, %02d, %02d (5-bit channels)\x1b[K\n", r_5, g_5, b_5);
    printf("  8b  : R%03d G%03d B%03d\x1b[K\n", r_8, g_8, b_8);
    printf("  HEX : #%02X%02X%02X\x1b[K\n", r_8, g_8, b_8);
    printf("  RAW : 0x%04X\x1b[K\n\n", raw_color);

    printf("\x1b[1;30m----------------------------------------------\x1b[0m\x1b[K\n");
    printf("  DPAD : Move Cursor  | [A] : Toggle Auto-Cycle\x1b[K\n");
    printf("  L/R  : Switch Mode  | [Y] : Adjust Cycle Speed\x1b[K\n");
    printf("  [B]  : Force Reset  | [START] : Exit App\x1b[K");

    fflush(stdout);
}

void draw_ui(void) {
    draw_top_test_matrix();
    draw_bottom_specs();
}

int main(int argc, char **argv) {
    (void)argc; (void)argv;
    
    irqEnable(IRQ_VBLANK);

    // Setup 2D Graphics modes for both screens
    videoSetMode(MODE_0_2D);
    videoSetModeSub(MODE_0_2D);
    vramSetBankA(VRAM_A_MAIN_BG);
    vramSetBankC(VRAM_C_SUB_BG);

    // Initialize both standard consoles. 
    // Initializing topConsole on BG3 forces VRAM Bank A to load the default font glyphs into Tile Base 0.
    consoleInit(&topConsole, 3, BgType_Text4bpp, BgSize_T_256x256, 31, 0, true, true);
    consoleInit(&bottomConsole, 3, BgType_Text4bpp, BgSize_T_256x256, 31, 0, false, true);

    // Initialize top custom backgrounds (Sharing Tile Base 0 with loaded console font)
    bg_board_id = bgInit(2, BgType_Text4bpp, BgSize_T_256x256, 29, 0);  // Low Priority: Swatches
    bg_pieces_id = bgInit(1, BgType_Text4bpp, BgSize_T_256x256, 30, 0); // High Priority: Cursor/Labels

    // Create a pure solid color glyph in tile index 1
    u32 solid_tile[8] = {
        0x11111111,
        0x11111111,
        0x11111111,
        0x11111111,
        0x11111111,
        0x11111111,
        0x11111111,
        0x11111111
    };
    u8* tile_memory = (u8*)bgGetGfxPtr(bg_board_id);
    dmaCopy(solid_tile, tile_memory + (1 * 32), sizeof(solid_tile));

    init_bottom_palette();

    // Clear graphics and text screen banks before starting
    u16* board_map = bgGetMapPtr(bg_board_id);
    u16* pieces_map = bgGetMapPtr(bg_pieces_id);
    memset(board_map, 0, 32 * 32 * sizeof(u16));
    memset(pieces_map, 0, 32 * 32 * sizeof(u16));

    consoleSelect(&topConsole);
    printf("\x1b[2J");
    fflush(stdout);

    consoleSelect(&bottomConsole);
    printf("\x1b[2J");
    fflush(stdout);

    int frame_count = 0;

    while (pmMainLoop()) {
        scanKeys();
        u32 kDown = keysDown();

        if (kDown & KEY_START) break; 

        // Handle diagnostic grid navigation
        if (kDown & KEY_UP)    { if (cursor_y > 0)  { cursor_y--; redraw_needed = 1; } }
        if (kDown & KEY_DOWN)  { if (cursor_y < 14) { cursor_y++; redraw_needed = 1; } }
        if (kDown & KEY_RIGHT) { if (cursor_x < 15) { cursor_x++; redraw_needed = 1; } }
        if (kDown & KEY_LEFT)  { if (cursor_x > 0)  { cursor_x--; redraw_needed = 1; } }

        // Change dynamic mode profiles
        if (kDown & KEY_L) {
            active_mode = (active_mode - 1 + 4) % 4;
            redraw_needed = 1;
        }
        if (kDown & KEY_R) {
            active_mode = (active_mode + 1) % 4;
            redraw_needed = 1;
        }

        // Configuration toggles
        if (kDown & KEY_A) {
            auto_cycle = !auto_cycle;
            redraw_needed = 1;
        }
        if (kDown & KEY_Y) {
            cycle_speed = (cycle_speed + 1) % 9; // Speeds 0 to 8
            redraw_needed = 1;
        }
        if (kDown & KEY_B) { // Reset parameters
            cycle_phase = 0;
            cursor_x = 0;
            cursor_y = 0;
            redraw_needed = 1;
        }

        // If automated cycling is on, increment phase step every frame
        if (auto_cycle && cycle_speed > 0) {
            cycle_phase = (cycle_phase + cycle_speed) % 360;
            redraw_needed = 1;
        }

        // Render graphics matrix frames
        if (redraw_needed) {
            update_palettes();
            draw_ui();
            redraw_needed = 0;
        }

        frame_count++;
        ds_yield();
        swiWaitForVBlank();
    }

    return 0;
}
