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

int cursor_x = 0; // Current Palette selection (0 - 15)
int cursor_y = 0; // Current Color Index selection (0 - 14, mapping to hardware index 1 - 15)

int redraw_needed = 1;

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

// Generates 240 distinct test colors across 16 palettes in GBA/DS 15-bit format (BGR555)
void populate_test_palettes(void) {
    for (int p = 0; p < 16; p++) {
        // Color 0 in 4bpp palettes is ALWAYS transparent on hardware
        BG_PALETTE[p * 16 + 0] = RGB15(0, 0, 0); 

        for (int i = 1; i < 16; i++) {
            u16 color = 0;
            int step = i * 2; // Linear intensity step (2 to 30)

            switch (p) {
                case 0:  color = RGB15(step, 0, 0); break;                 // Pure Red Ramp
                case 1:  color = RGB15(0, step, 0); break;                 // Pure Green Ramp
                case 2:  color = RGB15(0, 0, step); break;                 // Pure Blue Ramp
                case 3:  color = RGB15(step, step, 0); break;              // Yellow Ramp
                case 4:  color = RGB15(0, step, step); break;              // Cyan Ramp
                case 5:  color = RGB15(step, 0, step); break;              // Magenta Ramp
                case 6:  color = RGB15(step, step / 2, 0); break;          // Orange Ramp
                case 7:  color = RGB15(0, step, step / 2); break;          // Teal Ramp
                case 8:  color = RGB15(step / 2, 0, step); break;          // Violet / Purple Ramp
                case 9:  color = RGB15(step, step / 2, step / 2); break;  // Soft Pink / Salmon
                case 10: color = RGB15(step / 2, step, 0); break;          // Lime / Chartreuse
                case 11: color = RGB15(step / 4, step / 2, step); break;  // Deep Sky Blue
                case 12: color = RGB15(step, step * 7 / 10, step * 4 / 10); break; // Earth Tones / Brown
                case 13: color = RGB15(12 + i, 12 + i, 16 + i); break;     // Cool Pastel Gamut
                case 14: color = RGB15(16 + i, 12 + i, 12 + i); break;     // Warm Pastel Gamut
                case 15: color = RGB15(step, step, step); break;           // Grayscale spectrum
            }

            BG_PALETTE[p * 16 + i] = color;
        }
    }

    // Set Palette 10 for coordinate and text labels on the top screen (Pure White)
    BG_PALETTE[10 * 16 + 1] = RGB15(31, 31, 31);
}

// Configures the Sub Screen palette with rich gradient steps for the terminal UI
void init_bottom_palette(void) {
    BG_PALETTE_SUB[0]  = RGB15(2, 2, 3);        // Near-black slate background
    BG_PALETTE_SUB[1]  = RGB15(31, 8, 8);       // High-contrast Red
    BG_PALETTE_SUB[2]  = RGB15(8, 31, 8);       // High-contrast Green
    BG_PALETTE_SUB[3]  = RGB15(31, 31, 8);      // High-contrast Yellow
    BG_PALETTE_SUB[4]  = RGB15(8, 8, 31);       // High-contrast Blue
    BG_PALETTE_SUB[5]  = RGB15(31, 8, 31);      // High-contrast Magenta
    BG_PALETTE_SUB[6]  = RGB15(8, 31, 31);      // High-contrast Cyan
    BG_PALETTE_SUB[7]  = RGB15(25, 25, 25);     // Off-white text
    BG_PALETTE_SUB[8]  = RGB15(12, 12, 12);     // Dark gray lines
    BG_PALETTE_SUB[9]  = RGB15(31, 16, 4);      // Vibrant Orange
    BG_PALETTE_SUB[10] = RGB15(20, 10, 31);     // Vibrant Indigo
    BG_PALETTE_SUB[11] = RGB15(15, 31, 15);     // Light Lime
    BG_PALETTE_SUB[12] = RGB15(30, 20, 25);     // Lavender Pastel
    BG_PALETTE_SUB[13] = RGB15(31, 28, 15);     // Cream Yellow
    BG_PALETTE_SUB[14] = RGB15(0, 20, 15);      // Forest Emerald
    BG_PALETTE_SUB[15] = RGB15(31, 31, 31);     // Pure white alert
}

// Render Top Screen containing the color matrix and text layers
void draw_top_test_matrix(void) {
    u16* board_map = bgGetMapPtr(bg_board_id);
    u16* pieces_map = bgGetMapPtr(bg_pieces_id);

    // Wipe background/foreground maps before writing
    memset(board_map, 0, 32 * 32 * sizeof(u16));
    memset(pieces_map, 0, 32 * 32 * sizeof(u16));

    // Top screen header
    draw_string(pieces_map, 3, 1, "=== DS HARDWARE PALETTE MATRIX ===", 10);
    
    // Column indicators (Palettes 0 to 15)
    draw_string(pieces_map, 7, 3, "0 1 2 3 4 5 6 7 8 9 A B C D E F", 10);

    for (int r = 0; r < 15; r++) {
        // Row indicators (Indices 1 to 15)
        char idx_lbl[4];
        sprintf(idx_lbl, "%2d", r + 1);
        draw_string(pieces_map, 3, 5 + r, idx_lbl, 10);

        for (int c = 0; c < 16; c++) {
            // Setup the background swatch tile block (solid character tile 1)
            // It references Palette Index 'c' and color index 'r + 1' internally
            set_tile(board_map, 7 + c * 1.5, 5 + r, 1, c); 

            // Handle the Layer overlay (demonstrating characters sitting physically on top of background)
            if (c == cursor_x && r == cursor_y) {
                // Flash indicator over selection
                set_tile(pieces_map, 7 + c * 1.5, 5 + r, 'X', 10);
            } else {
                // Standard structural layout dot
                set_tile(pieces_map, 7 + c * 1.5, 5 + r, '.', 10);
            }
        }
    }
}

// Display real-time channel measurements & specs on bottom screen
void draw_bottom_specs(void) {
    consoleSelect(&bottomConsole);
    printf("\x1b[1;1H"); // Cursor reset

    int palette_id = cursor_x;
    int color_index = cursor_y + 1; // Map 0-14 back to hardware index 1-15

    // Read exact 15-bit color value from DS palette memory
    u16 raw_color = BG_PALETTE[palette_id * 16 + color_index];

    // Decode channels (5 bits per channel)
    int r_val = (raw_color >> 0) & 0x1F;
    int g_val = (raw_color >> 5) & 0x1F;
    int b_val = (raw_color >> 10) & 0x1F;

    // Convert channels to 24-bit spectrum coordinates
    int r_8 = (r_val * 255) / 31;
    int g_8 = (g_val * 255) / 31;
    int b_8 = (b_val * 255) / 31;

    printf("\x1b[1;33m  DS MULTI-SCREEN PALETTE UTILITY  \x1b[0m\x1b[K\n");
    printf("\x1b[1;30m===================================\x1b[0m\x1b[K\n\n");

    printf("  \x1b[1;36mSELECTED COLOR PROPERTIES:\x1b[0m\x1b[K\n");
    printf("  -------------------------\x1b[K\n");
    printf("  Hardware Palette : \x1b[1;35m%2d (0x%X)\x1b[0m\x1b[K\n", palette_id, palette_id);
    printf("  Internal Index   : \x1b[1;35m%2d\x1b[0m\x1b[K\n", color_index);
    printf("  Raw BGR555 Val   : \x1b[1;32m0x%04X\x1b[0m\x1b[K\n\n", raw_color);

    printf("  \x1b[1;36mSIGNAL SPECTRUM ANALYSIS:\x1b[0m\x1b[K\n");
    printf("  -------------------------\x1b[K\n");
    printf("  Red Channel   : \x1b[1;31m%2d/31\x1b[0m  (8-bit: %3d)\x1b[K\n", r_val, r_8);
    printf("  Green Channel : \x1b[1;32m%2d/31\x1b[0m  (8-bit: %3d)\x1b[K\n", g_val, g_8);
    printf("  Blue Channel  : \x1b[1;34m%2d/31\x1b[0m  (8-bit: %3d)\x1b[K\n", b_val, b_8);
    printf("  Hex Output    : \x1b[1;33m#%02X%02X%02X\x1b[0m\x1b[K\n\n", r_8, g_8, b_8);

    printf("\x1b[1;30m-----------------------------------\x1b[0m\x1b[K\n");
    printf("  D-PAD : Move Selector Cursor\x1b[K\n");
    printf("  START : Exit Display Test Suite\x1b[K\n");

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

    // Allocate Standard Bottom Console
    consoleInit(&bottomConsole, 3, BgType_Text4bpp, BgSize_T_256x256, 31, 0, false, true);

    // Setup Top Screen layer arrays (sharing font bank tile zero)
    bg_board_id = bgInit(2, BgType_Text4bpp, BgSize_T_256x256, 29, 0);  // Low Priority: Swatches
    bg_pieces_id = bgInit(1, BgType_Text4bpp, BgSize_T_256x256, 30, 0); // High Priority: Cursor/Labels

    // Create a pure solid color glyph in memory at safe tile location [Index 1] (SOH slot)
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

    // Populate all 240 custom palette slots
    populate_test_palettes();
    init_bottom_palette();

    // Wipe console and graphics maps on startup
    u16* board_map = bgGetMapPtr(bg_board_id);
    u16* pieces_map = bgGetMapPtr(bg_pieces_id);
    memset(board_map, 0, 32 * 32 * sizeof(u16));
    memset(pieces_map, 0, 32 * 32 * sizeof(u16));

    consoleSelect(&bottomConsole);
    printf("\x1b[2J");
    fflush(stdout);

    redraw_needed = 1;

    while (pmMainLoop()) {
        scanKeys();
        u32 kDown = keysDown();

        if (kDown & KEY_START) break; 

        if (kDown & KEY_UP) {
            if (cursor_y > 0) {
                cursor_y--;
                redraw_needed = 1;
            }
        }
        if (kDown & KEY_DOWN) {
            if (cursor_y < 14) {
                cursor_y++;
                redraw_needed = 1;
            }
        }
        if (kDown & KEY_RIGHT) {
            if (cursor_x < 15) {
                cursor_x++;
                redraw_needed = 1;
            }
        }
        if (kDown & KEY_LEFT) {
            if (cursor_x > 0) {
                cursor_x--;
                redraw_needed = 1;
            }
        }

        if (redraw_needed) {
            draw_ui();
            redraw_needed = 0;
        }

        ds_yield();
        swiWaitForVBlank();
    }

    return 0;
}
