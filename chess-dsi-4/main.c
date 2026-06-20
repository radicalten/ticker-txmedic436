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

int selected_color = 0; // Current selection index (0 - 15)
int redraw_needed = 1;

// 16 Standard Console/ANSI Color Names
const char* color_names[16] = {
    "Black",       "Dark Red",    "Dark Green",  "Dark Yellow",
    "Dark Blue",    "Dark Magenta","Dark Cyan",   "Light Gray",
    "Dark Gray",    "Bright Red",  "Bright Green", "Bright Yellow",
    "Bright Blue",  "Bright Magenta","Bright Cyan", "White"
};

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

// Map the 16 Standard Colors to Color Index 1 across 16 Hardware Palettes
void populate_standard_palettes(void) {
    u16 standard_colors[16] = {
        RGB15(0, 0, 0),       // 0: Black
        RGB15(15, 0, 0),      // 1: Dark Red
        RGB15(0, 15, 0),      // 2: Dark Green
        RGB15(15, 15, 0),     // 3: Dark Yellow (Brown)
        RGB15(0, 0, 15),      // 4: Dark Blue
        RGB15(15, 0, 15),     // 5: Dark Magenta
        RGB15(0, 15, 15),     // 6: Dark Cyan
        RGB15(22, 22, 22),    // 7: Light Gray
        RGB15(11, 11, 11),    // 8: Dark Gray
        RGB15(31, 0, 0),      // 9: Bright Red
        RGB15(0, 31, 0),      // 10: Bright Green
        RGB15(31, 31, 0),     // 11: Bright Yellow
        RGB15(0, 0, 31),      // 12: Bright Blue
        RGB15(31, 0, 31),     // 13: Bright Magenta
        RGB15(0, 31, 31),     // 14: Bright Cyan
        RGB15(31, 31, 31)     // 15: White
    };

    for (int p = 0; p < 16; p++) {
        // Color 0 is kept transparent for layer overlap
        BG_PALETTE[p * 16 + 0] = RGB15(0, 0, 0); 
        // Color 1 is assigned the designated hardware color
        BG_PALETTE[p * 16 + 1] = standard_colors[p];
    }
}

// Configure bottom screen terminal palette
void init_bottom_palette(void) {
    BG_PALETTE_SUB[0]  = RGB15(2, 2, 3);        // Near-black background
    BG_PALETTE_SUB[1]  = RGB15(31, 8, 8);       // High-contrast Red
    BG_PALETTE_SUB[2]  = RGB15(8, 31, 8);       // High-contrast Green
    BG_PALETTE_SUB[3]  = RGB15(31, 31, 8);      // High-contrast Yellow
    BG_PALETTE_SUB[4]  = RGB15(8, 8, 31);       // High-contrast Blue
    BG_PALETTE_SUB[5]  = RGB15(31, 8, 31);      // High-contrast Magenta
    BG_PALETTE_SUB[6]  = RGB15(8, 31, 31);      // High-contrast Cyan
    BG_PALETTE_SUB[7]  = RGB15(25, 25, 25);     // Off-white text
    BG_PALETTE_SUB[8]  = RGB15(12, 12, 12);     // Dark gray lines
    BG_PALETTE_SUB[9]  = RGB15(31, 16, 4);      // Vibrant Orange
    BG_PALETTE_SUB[15] = RGB15(31, 31, 31);     // Pure White
}

// Render Top Screen containing the 16 standard swatches
void draw_top_test_matrix(void) {
    u16* board_map = bgGetMapPtr(bg_board_id);
    u16* pieces_map = bgGetMapPtr(bg_pieces_id);

    // Clear main maps
    memset(board_map, 0, 32 * 32 * sizeof(u16));
    memset(pieces_map, 0, 32 * 32 * sizeof(u16));

    // Header title - using Palette 15 (White)
    draw_string(pieces_map, 2, 1, "=== STANDARD 16-COLOR TEST ===", 15);

    for (int i = 0; i < 16; i++) {
        int col = i / 8; // Left (0) or Right (1) Column
        int row = i % 8; // Row Index (0 to 7)

        int start_x = (col == 0) ? 2 : 17;
        int start_y = 4 + row * 2;

        // Draw Swatch Block on BG2 (4 columns wide, 1 row high)
        // Set tile ID to 1 (solid character block) referencing Palette i
        for (int dx = 0; dx < 4; dx++) {
            set_tile(board_map, start_x + dx, start_y, 1, i);
        }

        // Draw alphanumeric labels on BG1 (above the background)
        char label[16];
        sprintf(label, "%2d:%s", i, color_names[i]);
        label[11] = '\0'; // Clamp text length to avoid column overlaps
        draw_string(pieces_map, start_x + 5, start_y, label, 15);

        // Render Selector Bracket indicators
        if (i == selected_color) {
            set_tile(pieces_map, start_x - 1, start_y, '[', 15);
            set_tile(pieces_map, start_x + 4, start_y, ']', 15);
        }
    }
}

// Display selected color specs on the bottom terminal screen
void draw_bottom_specs(void) {
    consoleSelect(&bottomConsole);
    printf("\x1b[1;1H");

    int i = selected_color;
    // Extract color value from the first color register of the palette index
    u16 raw_color = BG_PALETTE[i * 16 + 1];

    // Decode 15-bit (BGR555) Channels
    int r_val = (raw_color >> 0) & 0x1F;
    int g_val = (raw_color >> 5) & 0x1F;
    int b_val = (raw_color >> 10) & 0x1F;

    // Convert channels to 8-bit scale
    int r_8 = (r_val * 255) / 31;
    int g_8 = (g_val * 255) / 31;
    int b_8 = (b_val * 255) / 31;

    printf("\x1b[1;33m  DS STANDARD 16-COLOR SPEC SHEET  \x1b[0m\x1b[K\n");
    printf("\x1b[1;30m===================================\x1b[0m\x1b[K\n\n");

    printf("  \x1b[1;36mCOLOR DETAILS:\x1b[0m\x1b[K\n");
    printf("  -------------------------\x1b[K\n");
    printf("  Color Index      : \x1b[1;35m%2d (0x%X)\x1b[0m\x1b[K\n", i, i);
    printf("  Display Name     : \x1b[1;35m%s\x1b[0m\x1b[K\n", color_names[i]);
    printf("  Internal Palette : \x1b[1;32m%2d\x1b[0m\x1b[K\n", i);
    printf("  Raw BGR555 Val   : \x1b[1;32m0x%04X\x1b[0m\x1b[K\n\n", raw_color);

    printf("  \x1b[1;36mSIGNAL SPECTRUM ANALYSIS:\x1b[0m\x1b[K\n");
    printf("  -------------------------\x1b[K\n");
    printf("  Red Channel   : \x1b[1;31m%2d/31\x1b[0m  (8-bit: %3d)\x1b[K\n", r_val, r_8);
    printf("  Green Channel : \x1b[1;32m%2d/31\x1b[0m  (8-bit: %3d)\x1b[K\n", g_val, g_8);
    printf("  Blue Channel  : \x1b[1;34m%2d/31\x1b[0m  (8-bit: %3d)\x1b[K\n", b_val, b_8);
    printf("  Hex Value     : \x1b[1;33m#%02X%02X%02X\x1b[0m\x1b[K\n\n", r_8, g_8, b_8);

    printf("\x1b[1;30m-----------------------------------\x1b[0m\x1b[K\n");
    printf("  D-PAD : Navigate Color Set\x1b[K\n");
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

    // Populate our custom palette profiles
    populate_standard_palettes();
    init_bottom_palette();

    // Reset top screen map memory
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
            if ((selected_color % 8) > 0) {
                selected_color--;
                redraw_needed = 1;
            }
        }
        if (kDown & KEY_DOWN) {
            if ((selected_color % 8) < 7) {
                selected_color++;
                redraw_needed = 1;
            }
        }
        if (kDown & (KEY_LEFT | KEY_RIGHT)) {
            selected_color ^= 8; // Swap between left and right column
            redraw_needed = 1;
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
