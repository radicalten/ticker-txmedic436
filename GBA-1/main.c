// Mario Bros 3 Style Platformer for GBA
// Compile with: make (using tonc's template makefile)
// Or: arm-none-eabi-gcc -mthumb -mthumb-interwork -specs=gba.specs mario.c -ltonc -o mario.gba

#include <tonc.h>
#include <string.h>

// ============================================================================
// CONSTANTS
// ============================================================================

#define FIXED_SHIFT     8
#define FIXED_ONE       (1 << FIXED_SHIFT)

#define GRAVITY         12
#define MAX_FALL_SPEED  (6 * FIXED_ONE)
#define JUMP_POWER      (48 * FIXED_ONE / 10)
#define MOVE_SPEED      (18 * FIXED_ONE / 10)
#define MAX_SPEED       (2 * FIXED_ONE)
#define FRICTION        8

#define PLAYER_WIDTH    14
#define PLAYER_HEIGHT   16

#define LEVEL_WIDTH     64
#define LEVEL_HEIGHT    20

#define SCREEN_WIDTH    240
#define SCREEN_HEIGHT   160

#define TILE_SIZE       8

// Tile indices
#define TILE_SKY        0
#define TILE_GROUND     1
#define TILE_BRICK      2
#define TILE_QUESTION   3
#define TILE_BLOCK      4
#define TILE_PIPE_TL    5
#define TILE_PIPE_TR    6
#define TILE_PIPE_BL    7
#define TILE_PIPE_BR    8
#define TILE_COIN       9
#define TILE_CLOUD      10
#define TILE_BUSH       11
#define TILE_HILL       12
#define TILE_FLAG_TOP   13
#define TILE_FLAG_POLE  14
#define TILE_CASTLE     15

// Sprite indices
#define SPR_MARIO_STAND     0
#define SPR_MARIO_RUN1      1
#define SPR_MARIO_RUN2      2
#define SPR_MARIO_RUN3      3
#define SPR_MARIO_JUMP      4
#define SPR_GOOMBA1         5
#define SPR_GOOMBA2         6
#define SPR_GOOMBA_FLAT     7
#define SPR_KOOPA1          8
#define SPR_KOOPA2          9
#define SPR_SHELL           10
#define SPR_COIN_1          11
#define SPR_COIN_2          12
#define SPR_MUSHROOM        13

// Game states
#define STATE_TITLE     0
#define STATE_PLAYING   1
#define STATE_DEAD      2
#define STATE_WIN       3

// ============================================================================
// TILE DATA (4bpp, 8x8 tiles)
// ============================================================================

// Palette: 0=trans, 1=black, 2=red, 3=brown, 4=tan, 5=blue, 6=white, 7=green, 8=yellow, 9=orange, 10=pink

const u16 bg_palette[16] = {
    RGB15(12,20,31), // 0 - Sky blue (transparent for sprites)
    RGB15(0,0,0),    // 1 - Black
    RGB15(31,0,0),   // 2 - Red
    RGB15(20,10,5),  // 3 - Brown
    RGB15(28,22,12), // 4 - Tan/Beige
    RGB15(0,0,31),   // 5 - Blue
    RGB15(31,31,31), // 6 - White
    RGB15(0,20,0),   // 7 - Green
    RGB15(31,31,0),  // 8 - Yellow
    RGB15(31,16,0),  // 9 - Orange
    RGB15(31,20,20), // 10 - Pink
    RGB15(16,8,0),   // 11 - Dark brown
    RGB15(0,31,0),   // 12 - Bright green
    RGB15(31,28,20), // 13 - Light tan
    RGB15(8,24,8),   // 14 - Medium green
    RGB15(24,16,8),  // 15 - Medium brown
};

const u16 sprite_palette[16] = {
    RGB15(31,0,31),  // 0 - Magenta (transparent)
    RGB15(0,0,0),    // 1 - Black
    RGB15(31,0,0),   // 2 - Red
    RGB15(20,10,5),  // 3 - Brown
    RGB15(28,22,12), // 4 - Tan/Skin
    RGB15(0,0,31),   // 5 - Blue
    RGB15(31,31,31), // 6 - White
    RGB15(0,20,0),   // 7 - Green
    RGB15(31,31,0),  // 8 - Yellow
    RGB15(31,16,0),  // 9 - Orange
    RGB15(31,20,20), // 10 - Pink
    RGB15(16,8,0),   // 11 - Dark brown
    RGB15(0,31,0),   // 12 - Bright green
    RGB15(24,20,16), // 13 - Light skin
    RGB15(8,24,8),   // 14 - Medium green
    RGB15(12,12,12), // 15 - Gray
};

// 8x8 tile data (4bpp = 32 bytes per tile)
const u32 bg_tiles[] = {
    // Tile 0: Sky (empty)
    0x00000000, 0x00000000, 0x00000000, 0x00000000,
    0x00000000, 0x00000000, 0x00000000, 0x00000000,
    
    // Tile 1: Ground block (brick pattern)
    0x33333333, 0x33111113, 0x33111113, 0x33111113,
    0x33333333, 0x11131113, 0x11131113, 0x11131113,
    
    // Tile 2: Brick
    0x33333333, 0x34444443, 0x34444443, 0x33333333,
    0x33333333, 0x44434443, 0x44434443, 0x33333333,
    
    // Tile 3: Question block
    0x88888888, 0x89999998, 0x89188198, 0x89111198,
    0x89911198, 0x89988198, 0x89911198, 0x88888888,
    
    // Tile 4: Solid block
    0x11111111, 0x13333331, 0x13333331, 0x13333331,
    0x13333331, 0x13333331, 0x13333331, 0x11111111,
    
    // Tile 5: Pipe top-left
    0x77777777, 0x7CCCCCCC, 0x7C777777, 0x7C777777,
    0x7C777777, 0x7C777777, 0x17777771, 0x17777771,
    
    // Tile 6: Pipe top-right
    0x77777771, 0xCCCCCCC1, 0x77777771, 0x77777771,
    0x77777771, 0x77777771, 0x17777771, 0x17777771,
    
    // Tile 7: Pipe body-left
    0x17777771, 0x17777771, 0x17777771, 0x17777771,
    0x17777771, 0x17777771, 0x17777771, 0x17777771,
    
    // Tile 8: Pipe body-right
    0x17777771, 0x17777771, 0x17777771, 0x17777771,
    0x17777771, 0x17777771, 0x17777771, 0x17777771,
    
    // Tile 9: Coin
    0x00088000, 0x00899800, 0x08981980, 0x08988980,
    0x08988980, 0x08981980, 0x00899800, 0x00088000,
    
    // Tile 10: Cloud
    0x00066000, 0x06666660, 0x66666666, 0x66666666,
    0x06666660, 0x00666600, 0x00000000, 0x00000000,
    
    // Tile 11: Bush
    0x00077000, 0x07777770, 0x77777777, 0x77777777,
    0x07777770, 0x00777700, 0x00000000, 0x00000000,
    
    // Tile 12: Hill
    0x00007000, 0x00077700, 0x00777770, 0x07777777,
    0x77777777, 0x77777777, 0x77777777, 0x77777777,
    
    // Tile 13: Flag top
    0x00000011, 0x00022211, 0x00222211, 0x02222211,
    0x00222211, 0x00022211, 0x00000011, 0x00000011,
    
    // Tile 14: Flag pole
    0x00000011, 0x00000011, 0x00000011, 0x00000011,
    0x00000011, 0x00000011, 0x00000011, 0x00000011,
    
    // Tile 15: Castle block
    0x33333333, 0x34343434, 0x33333333, 0x43434343,
    0x33333333, 0x34343434, 0x33333333, 0x43434343,
};

// Sprite tiles (16x16 = 4 8x8 tiles each, 4bpp)
const u32 sprite_tiles[] = {
    // Mario standing (tiles 0-3 for 16x16)
    // Top-left
    0x00000000, 0x00022200, 0x00222220, 0x00333140,
    0x03314140, 0x03314440, 0x00344400, 0x00222220,
    // Top-right
    0x00000000, 0x00222000, 0x02222220, 0x04133300,
    0x04143130, 0x04443130, 0x00044300, 0x02222200,
    // Bottom-left
    0x02242200, 0x22224220, 0x44422240, 0x44042040,
    0x44002000, 0x00330300, 0x03333000, 0x03330000,
    // Bottom-right
    0x00242200, 0x02422220, 0x04222044, 0x04024044,
    0x00020044, 0x00303300, 0x00033330, 0x00003330,
    
    // Mario running frame 1 (tiles 4-7)
    0x00000000, 0x00022200, 0x00222220, 0x00333140,
    0x03314140, 0x03314440, 0x00344400, 0x00222220,
    0x00000000, 0x00222000, 0x02222220, 0x04133300,
    0x04143130, 0x04443130, 0x00044300, 0x02222200,
    0x02242200, 0x22224220, 0x44022040, 0x44002000,
    0x00033040, 0x00333440, 0x03330440, 0x00000400,
    0x00242200, 0x02422220, 0x04022044, 0x00020044,
    0x04033000, 0x04433300, 0x04400330, 0x00400000,
    
    // Mario running frame 2 (tiles 8-11)
    0x00000000, 0x00022200, 0x00222220, 0x00333140,
    0x03314140, 0x03314440, 0x00344400, 0x00222220,
    0x00000000, 0x00222000, 0x02222220, 0x04133300,
    0x04143130, 0x04443130, 0x00044300, 0x02222200,
    0x02242200, 0x22224220, 0x44022040, 0x00300000,
    0x03304000, 0x33340000, 0x03344000, 0x00044000,
    0x00242200, 0x02422220, 0x04022044, 0x00000300,
    0x00004030, 0x00004330, 0x00004433, 0x00004400,
    
    // Mario running frame 3 (tiles 12-15)
    0x00000000, 0x00022200, 0x00222220, 0x00333140,
    0x03314140, 0x03314440, 0x00344400, 0x00222220,
    0x00000000, 0x00222000, 0x02222220, 0x04133300,
    0x04143130, 0x04443130, 0x00044300, 0x02222200,
    0x02242200, 0x22224220, 0x44022040, 0x40302000,
    0x43330000, 0x44330000, 0x00333000, 0x00033300,
    0x00242200, 0x02422220, 0x04022044, 0x00020304,
    0x00003344, 0x00003344, 0x00033300, 0x00330000,
    
    // Mario jumping (tiles 16-19)
    0x00003000, 0x00033300, 0x00222200, 0x00222220,
    0x00333140, 0x03314140, 0x03314440, 0x00344400,
    0x00030000, 0x00333000, 0x00222200, 0x02222220,
    0x04133300, 0x04143130, 0x04443130, 0x00044300,
    0x00222220, 0x02242200, 0x22224220, 0x44422440,
    0x40042000, 0x03042040, 0x33300440, 0x03300440,
    0x02222200, 0x00242200, 0x02422220, 0x04422244,
    0x00024004, 0x04024030, 0x04400330, 0x04400330,
    
    // Goomba frame 1 (tiles 20-23)
    0x00000000, 0x00033000, 0x00333300, 0x03333330,
    0x03316130, 0x03111130, 0x33366333, 0x33333333,
    0x00000000, 0x00033000, 0x00333300, 0x03333330,
    0x03163130, 0x03111130, 0x33366333, 0x33333333,
    0x33333333, 0x33333333, 0x03333330, 0x00313300,
    0x03131000, 0x33110000, 0x33100000, 0x00000000,
    0x33333333, 0x33333333, 0x03333330, 0x00331300,
    0x00013130, 0x00001133, 0x00000133, 0x00000000,
    
    // Goomba frame 2 (tiles 24-27)
    0x00000000, 0x00033000, 0x00333300, 0x03333330,
    0x03316130, 0x03111130, 0x33366333, 0x33333333,
    0x00000000, 0x00033000, 0x00333300, 0x03333330,
    0x03163130, 0x03111130, 0x33366333, 0x33333333,
    0x33333333, 0x33333333, 0x03333330, 0x00313300,
    0x00013100, 0x00001130, 0x00000330, 0x00000000,
    0x33333333, 0x33333333, 0x03333330, 0x00331300,
    0x00131000, 0x03110000, 0x03300000, 0x00000000,
    
    // Goomba flat (tiles 28-31)
    0x00000000, 0x00000000, 0x00000000, 0x00000000,
    0x00000000, 0x00000000, 0x33333333, 0x11111111,
    0x00000000, 0x00000000, 0x00000000, 0x00000000,
    0x00000000, 0x00000000, 0x33333333, 0x11111111,
    0x00000000, 0x00000000, 0x00000000, 0x00000000,
    0x00000000, 0x00000000, 0x00000000, 0x00000000,
    0x00000000, 0x00000000, 0x00000000, 0x00000000,
    0x00000000, 0x00000000, 0x00000000, 0x00000000,
    
    // Koopa frame 1 (tiles 32-35)
    0x00000000, 0x00007000, 0x00077700, 0x00766670,
    0x07661670, 0x07666670, 0x00777770, 0x00777700,
    0x00000000, 0x00070000, 0x00777000, 0x07666700,
    0x07616670, 0x07666670, 0x07777700, 0x00777700,
    0x00777700, 0x07727770, 0x07772770, 0x07777700,
    0x00707000, 0x00888000, 0x08888000, 0x08800000,
    0x00777700, 0x07772770, 0x07727770, 0x00777700,
    0x00070700, 0x00088800, 0x00088880, 0x00000880,
    
    // Koopa frame 2 (tiles 36-39)
    0x00000000, 0x00007000, 0x00077700, 0x00766670,
    0x07661670, 0x07666670, 0x00777770, 0x00777700,
    0x00000000, 0x00070000, 0x00777000, 0x07666700,
    0x07616670, 0x07666670, 0x07777700, 0x00777700,
    0x00777700, 0x07727770, 0x07772770, 0x07777700,
    0x00070700, 0x00088800, 0x00088880, 0x00000880,
    0x00777700, 0x07772770, 0x07727770, 0x00777700,
    0x00707000, 0x00888000, 0x08888000, 0x08800000,
    
    // Shell (tiles 40-43)
    0x00000000, 0x00000000, 0x00077700, 0x00777770,
    0x07777770, 0x07727770, 0x07772770, 0x07772770,
    0x00000000, 0x00000000, 0x00777000, 0x07777700,
    0x07777770, 0x07772770, 0x07727770, 0x07727770,
    0x07777770, 0x07777770, 0x00777770, 0x00077700,
    0x00000000, 0x00000000, 0x00000000, 0x00000000,
    0x07777770, 0x07777770, 0x07777700, 0x00777000,
    0x00000000, 0x00000000, 0x00000000, 0x00000000,
    
    // Coin spinning 1 (tiles 44-47)
    0x00000000, 0x00088000, 0x00899800, 0x08988980,
    0x08988980, 0x08988980, 0x08988980, 0x08988980,
    0x00000000, 0x00088000, 0x00899800, 0x08988980,
    0x08988980, 0x08988980, 0x08988980, 0x08988980,
    0x08988980, 0x08988980, 0x08988980, 0x08988980,
    0x00899800, 0x00088000, 0x00000000, 0x00000000,
    0x08988980, 0x08988980, 0x08988980, 0x08988980,
    0x00899800, 0x00088000, 0x00000000, 0x00000000,
    
    // Coin spinning 2 (tiles 48-51)
    0x00000000, 0x00008000, 0x00008800, 0x00088800,
    0x00088800, 0x00088800, 0x00088800, 0x00088800,
    0x00000000, 0x00008000, 0x00008800, 0x00088800,
    0x00088800, 0x00088800, 0x00088800, 0x00088800,
    0x00088800, 0x00088800, 0x00088800, 0x00088800,
    0x00008800, 0x00008000, 0x00000000, 0x00000000,
    0x00088800, 0x00088800, 0x00088800, 0x00088800,
    0x00008800, 0x00008000, 0x00000000, 0x00000000,
    
    // Mushroom (tiles 52-55)
    0x00000000, 0x00022200, 0x02222220, 0x22266222,
    0x22666622, 0x22666622, 0x22266222, 0x02222220,
    0x00000000, 0x00222000, 0x02222220, 0x22266222,
    0x22666622, 0x22666622, 0x22266222, 0x02222220,
    0x00444400, 0x04444440, 0x04414140, 0x04414140,
    0x04444440, 0x00444400, 0x00000000, 0x00000000,
    0x00444400, 0x04444440, 0x04141440, 0x04141440,
    0x04444440, 0x00444400, 0x00000000, 0x00000000,
};

// ============================================================================
// LEVEL DATA
// ============================================================================

// Level map (20 rows x 64 columns)
const u8 level_data[LEVEL_HEIGHT * LEVEL_WIDTH] = {
    // Row 0 (top - mostly sky)
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    // Row 1
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    // Row 2 - clouds
    0,0,0,0,10,0,0,0,0,0,0,0,0,0,0,10,10,0,0,0,0,0,0,0,10,0,0,0,0,0,0,0,10,10,0,0,0,0,0,0,0,0,10,0,0,0,0,0,0,0,0,0,0,0,0,10,0,0,0,0,0,0,0,0,
    // Row 3
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    // Row 4
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,13,0,0,0,0,0,0,
    // Row 5
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,14,0,0,0,0,0,0,
    // Row 6
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,14,0,0,0,0,0,0,
    // Row 7
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,14,0,0,0,0,0,0,
    // Row 8
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,14,0,0,0,0,0,0,
    // Row 9 (question/brick blocks)
    0,0,0,0,0,0,0,0,0,0,0,0,2,3,2,3,2,0,0,0,0,0,0,0,0,0,0,0,0,2,2,2,0,0,0,0,0,0,0,0,3,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,14,0,0,0,0,0,0,
    // Row 10
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,14,15,15,15,15,0,0,
    // Row 11
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,14,15,15,15,15,0,0,
    // Row 12 - pipes top
    0,0,0,0,0,0,0,0,0,0,5,6,0,0,0,0,0,0,0,0,0,0,0,0,5,6,0,0,0,0,0,0,0,0,0,0,0,0,0,4,4,4,0,0,0,0,0,0,0,0,0,5,6,0,0,0,0,14,15,15,15,15,0,0,
    // Row 13 - pipes body
    0,0,0,0,0,0,0,0,0,0,7,8,0,0,0,0,0,0,0,0,0,0,0,0,7,8,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,7,8,0,0,0,0,14,15,15,15,15,0,0,
    // Row 14 - pipes body
    0,0,0,0,0,0,0,0,0,0,7,8,0,0,0,0,0,0,0,0,0,0,0,0,7,8,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,7,8,0,0,0,0,14,15,15,15,15,0,0,
    // Row 15 - ground with gaps
    1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,0,0,0,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,
    // Row 16 - underground
    1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,0,0,0,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,
    // Row 17
    1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,
    // Row 18
    1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,
    // Row 19 (bottom)
    1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,
};

// ============================================================================
// GAME STRUCTURES
// ============================================================================

typedef struct {
    int x, y;           // Fixed-point position
    int vx, vy;         // Fixed-point velocity
    int width, height;
    int on_ground;
    int facing_right;
    int frame;
    int frame_timer;
    int state;          // 0=small, 1=big, 2=fire
    int invincible;
    int inv_timer;
    int dead;
} Player;

typedef struct {
    int x, y;
    int vx;
    int type;           // 0=goomba, 1=koopa, 2=shell
    int active;
    int frame;
    int frame_timer;
    int stomped;
    int stomp_timer;
} Enemy;

typedef struct {
    int x, y;
    int active;
    int collected;
    int frame;
} Coin;

typedef struct {
    int x, y;
    int active;
    int vx, vy;
} Item;

#define MAX_ENEMIES 16
#define MAX_COINS   32
#define MAX_ITEMS   8

// ============================================================================
// GLOBAL VARIABLES
// ============================================================================

Player player;
Enemy enemies[MAX_ENEMIES];
Coin coins[MAX_COINS];
Item items[MAX_ITEMS];

int camera_x = 0;
int score = 0;
int lives = 3;
int game_coins = 0;
int game_state = STATE_TITLE;
int level_complete = 0;
int death_timer = 0;

// ============================================================================
// UTILITY FUNCTIONS
// ============================================================================

// Check if a tile is solid
int is_solid_tile(int tile_x, int tile_y) {
    if (tile_x < 0 || tile_x >= LEVEL_WIDTH || tile_y < 0 || tile_y >= LEVEL_HEIGHT)
        return (tile_y >= LEVEL_HEIGHT - 5); // Below level is solid
    
    int tile = level_data[tile_y * LEVEL_WIDTH + tile_x];
    return (tile == TILE_GROUND || tile == TILE_BRICK || tile == TILE_QUESTION ||
            tile == TILE_BLOCK || tile == TILE_PIPE_TL || tile == TILE_PIPE_TR ||
            tile == TILE_PIPE_BL || tile == TILE_PIPE_BR || tile == TILE_CASTLE);
}

// Check collision between player and tilemap
int check_tile_collision(int px, int py, int pw, int ph) {
    int left_tile = px / TILE_SIZE;
    int right_tile = (px + pw - 1) / TILE_SIZE;
    int top_tile = py / TILE_SIZE;
    int bottom_tile = (py + ph - 1) / TILE_SIZE;
    
    for (int ty = top_tile; ty <= bottom_tile; ty++) {
        for (int tx = left_tile; tx <= right_tile; tx++) {
            if (is_solid_tile(tx, ty))
                return 1;
        }
    }
    return 0;
}

// ============================================================================
// INITIALIZATION
// ============================================================================

void init_graphics(void) {
    // Set video mode 0 with BG0 and sprites
    REG_DISPCNT = DCNT_MODE0 | DCNT_BG0 | DCNT_OBJ | DCNT_OBJ_1D;
    
    // Setup BG0 - 64x32 tiles, 4bpp
    REG_BG0CNT = BG_CBB(0) | BG_SBB(28) | BG_4BPP | BG_REG_64x32 | BG_PRIO(1);
    
    // Copy palettes
    memcpy(pal_bg_mem, bg_palette, sizeof(bg_palette));
    memcpy(pal_obj_mem, sprite_palette, sizeof(sprite_palette));
    
    // Copy tile data
    memcpy(&tile_mem[0][0], bg_tiles, sizeof(bg_tiles));
    memcpy(&tile_mem[4][0], sprite_tiles, sizeof(sprite_tiles));
    
    // Clear all sprites
    oam_init(obj_mem, 128);
}

void init_level(void) {
    // Copy level to screen entry buffer
    u16 *map = (u16*)se_mem[28];
    
    for (int y = 0; y < 32; y++) {
        for (int x = 0; x < 64; x++) {
            int tile = 0;
            if (y < LEVEL_HEIGHT && x < LEVEL_WIDTH) {
                tile = level_data[y * LEVEL_WIDTH + x];
            }
            map[y * 64 + x] = tile;
        }
    }
}

void init_player(void) {
    player.x = 24 * FIXED_ONE;
    player.y = 112 * FIXED_ONE;
    player.vx = 0;
    player.vy = 0;
    player.width = PLAYER_WIDTH;
    player.height = PLAYER_HEIGHT;
    player.on_ground = 0;
    player.facing_right = 1;
    player.frame = 0;
    player.frame_timer = 0;
    player.state = 0;
    player.invincible = 0;
    player.inv_timer = 0;
    player.dead = 0;
}

void init_enemies(void) {
    // Clear all enemies
    for (int i = 0; i < MAX_ENEMIES; i++) {
        enemies[i].active = 0;
    }
    
    // Spawn some goombas
    enemies[0] = (Enemy){.x = 180*FIXED_ONE, .y = 112*FIXED_ONE, .vx = -FIXED_ONE/2, .type = 0, .active = 1};
    enemies[1] = (Enemy){.x = 280*FIXED_ONE, .y = 112*FIXED_ONE, .vx = -FIXED_ONE/2, .type = 0, .active = 1};
    enemies[2] = (Enemy){.x = 320*FIXED_ONE, .y = 112*FIXED_ONE, .vx = -FIXED_ONE/2, .type = 1, .active = 1};
    enemies[3] = (Enemy){.x = 400*FIXED_ONE, .y = 112*FIXED_ONE, .vx = -FIXED_ONE/2, .type = 0, .active = 1};
    enemies[4] = (Enemy){.x = 430*FIXED_ONE, .y = 112*FIXED_ONE, .vx = -FIXED_ONE/2, .type = 0, .active = 1};
}

void init_coins(void) {
    for (int i = 0; i < MAX_COINS; i++) {
        coins[i].active = 0;
    }
    
    // Spawn some floating coins
    int coin_idx = 0;
    coins[coin_idx++] = (Coin){.x = 100, .y = 96, .active = 1};
    coins[coin_idx++] = (Coin){.x = 112, .y = 96, .active = 1};
    coins[coin_idx++] = (Coin){.x = 124, .y = 96, .active = 1};
    coins[coin_idx++] = (Coin){.x = 200, .y = 80, .active = 1};
    coins[coin_idx++] = (Coin){.x = 240, .y = 64, .active = 1};
    coins[coin_idx++] = (Coin){.x = 350, .y = 88, .active = 1};
    coins[coin_idx++] = (Coin){.x = 362, .y = 88, .active = 1};
}

void init_items(void) {
    for (int i = 0; i < MAX_ITEMS; i++) {
        items[i].active = 0;
    }
}

void init_game(void) {
    init_graphics();
    init_level();
    init_player();
    init_enemies();
    init_coins();
    init_items();
    camera_x = 0;
    score = 0;
    game_coins = 0;
    level_complete = 0;
}

// ============================================================================
// PLAYER UPDATE
// ============================================================================

void update_player(void) {
    if (player.dead) {
        player.vy += GRAVITY;
        player.y += player.vy;
        
        if (player.y > 200 * FIXED_ONE) {
            death_timer++;
            if (death_timer > 120) {
                lives--;
                if (lives > 0) {
                    init_player();
                    camera_x = 0;
                    init_enemies();
                    death_timer = 0;
                } else {
                    game_state = STATE_TITLE;
                    lives = 3;
                }
            }
        }
        return;
    }
    
    // Input handling
    int moving = 0;
    
    if (key_is_down(KEY_LEFT)) {
        player.vx -= MOVE_SPEED;
        if (player.vx < -MAX_SPEED) player.vx = -MAX_SPEED;
        player.facing_right = 0;
        moving = 1;
    }
    
    if (key_is_down(KEY_RIGHT)) {
        player.vx += MOVE_SPEED;
        if (player.vx > MAX_SPEED) player.vx = MAX_SPEED;
        player.facing_right = 1;
        moving = 1;
    }
    
    // Friction when not moving
    if (!moving) {
        if (player.vx > 0) {
            player.vx -= FRICTION;
            if (player.vx < 0) player.vx = 0;
        } else if (player.vx < 0) {
            player.vx += FRICTION;
            if (player.vx > 0) player.vx = 0;
        }
    }
    
    // Jumping
    if (key_hit(KEY_A) && player.on_ground) {
        player.vy = -JUMP_POWER;
        player.on_ground = 0;
    }
    
    // Variable jump height
    if (!key_is_down(KEY_A) && player.vy < 0) {
        player.vy += GRAVITY;
    }
    
    // Apply gravity
    player.vy += GRAVITY;
    if (player.vy > MAX_FALL_SPEED) player.vy = MAX_FALL_SPEED;
    
    // Horizontal collision
    int new_x = player.x + player.vx;
    int px = new_x >> FIXED_SHIFT;
    int py = player.y >> FIXED_SHIFT;
    
    if (new_x < 0) {
        new_x = 0;
        player.vx = 0;
    }
    
    if (!check_tile_collision(px, py, player.width, player.height)) {
        player.x = new_x;
    } else {
        player.vx = 0;
        // Align to tile
        if (player.vx > 0) {
            player.x = ((px / TILE_SIZE) * TILE_SIZE - player.width) * FIXED_ONE;
        }
    }
    
    // Vertical collision
    int new_y = player.y + player.vy;
    px = player.x >> FIXED_SHIFT;
    py = new_y >> FIXED_SHIFT;
    
    player.on_ground = 0;
    
    if (!check_tile_collision(px, py, player.width, player.height)) {
        player.y = new_y;
    } else {
        if (player.vy > 0) {
            // Landing
            player.on_ground = 1;
            player.y = ((py / TILE_SIZE) * TILE_SIZE - player.height) * FIXED_ONE;
        } else {
            // Hit ceiling
            player.y = (((py + player.height) / TILE_SIZE + 1) * TILE_SIZE) * FIXED_ONE;
        }
        player.vy = 0;
    }
    
    // Animation
    if (!player.on_ground) {
        player.frame = 4; // Jump frame
    } else if (player.vx != 0) {
        player.frame_timer++;
        if (player.frame_timer >= 6) {
            player.frame_timer = 0;
            player.frame++;
            if (player.frame > 3) player.frame = 1;
        }
    } else {
        player.frame = 0;
        player.frame_timer = 0;
    }
    
    // Invincibility timer
    if (player.invincible) {
        player.inv_timer--;
        if (player.inv_timer <= 0) {
            player.invincible = 0;
        }
    }
    
    // Death by falling
    if ((player.y >> FIXED_SHIFT) > 180) {
        player.dead = 1;
        player.vy = -JUMP_POWER;
        death_timer = 0;
    }
    
    // Win condition - reach flag pole
    if ((player.x >> FIXED_SHIFT) >= 57 * 8) {
        game_state = STATE_WIN;
    }
}

// ============================================================================
// ENEMY UPDATE
// ============================================================================

void update_enemies(void) {
    for (int i = 0; i < MAX_ENEMIES; i++) {
        if (!enemies[i].active) continue;
        
        Enemy *e = &enemies[i];
        
        // Off-screen check
        int ex = e->x >> FIXED_SHIFT;
        if (ex < camera_x - 32 || ex > camera_x + SCREEN_WIDTH + 32) continue;
        
        if (e->stomped) {
            e->stomp_timer--;
            if (e->stomp_timer <= 0) {
                e->active = 0;
            }
            continue;
        }
        
        // Movement
        e->x += e->vx;
        
        // Gravity
        e->y += GRAVITY * 2;
        
        // Ground check
        int tile_x = ex / TILE_SIZE;
        int tile_y = ((e->y >> FIXED_SHIFT) + 16) / TILE_SIZE;
        if (is_solid_tile(tile_x, tile_y)) {
            e->y = ((tile_y * TILE_SIZE) - 16) * FIXED_ONE;
        }
        
        // Wall collision - reverse direction
        int left_tile = ex / TILE_SIZE;
        int right_tile = (ex + 14) / TILE_SIZE;
        int mid_tile = ((e->y >> FIXED_SHIFT) + 8) / TILE_SIZE;
        
        if (is_solid_tile(left_tile - 1, mid_tile) || is_solid_tile(right_tile + 1, mid_tile)) {
            e->vx = -e->vx;
        }
        
        // Edge detection for koopas
        if (e->type == 1) {
            int below_left = ((e->y >> FIXED_SHIFT) + 20) / TILE_SIZE;
            if (!is_solid_tile(ex / TILE_SIZE, below_left)) {
                e->vx = -e->vx;
            }
        }
        
        // Animation
        e->frame_timer++;
        if (e->frame_timer >= 10) {
            e->frame_timer = 0;
            e->frame = !e->frame;
        }
        
        // Player collision
        if (!player.dead && !player.invincible) {
            int px = player.x >> FIXED_SHIFT;
            int py = player.y >> FIXED_SHIFT;
            
            // Check collision
            if (px < ex + 14 && px + player.width > ex &&
                py < (e->y >> FIXED_SHIFT) + 16 && py + player.height > (e->y >> FIXED_SHIFT)) {
                
                // Check if stomping (player falling onto enemy)
                if (player.vy > 0 && py + player.height < (e->y >> FIXED_SHIFT) + 10) {
                    // Stomp!
                    if (e->type == 1) {
                        // Koopa turns into shell
                        e->type = 2;
                        e->vx = 0;
                    } else if (e->type == 2) {
                        // Kick shell
                        e->vx = (px < ex) ? 4 * FIXED_ONE : -4 * FIXED_ONE;
                    } else {
                        // Goomba flattened
                        e->stomped = 1;
                        e->stomp_timer = 30;
                    }
                    player.vy = -JUMP_POWER / 2;
                    score += 100;
                } else {
                    // Hit by enemy
                    if (e->type != 2 || e->vx != 0) {
                        player.dead = 1;
                        player.vy = -JUMP_POWER;
                        death_timer = 0;
                    } else {
                        // Kick stationary shell
                        e->vx = (px < ex) ? 4 * FIXED_ONE : -4 * FIXED_ONE;
                    }
                }
            }
        }
    }
}

// ============================================================================
// COIN UPDATE
// ============================================================================

void update_coins(void) {
    for (int i = 0; i < MAX_COINS; i++) {
        if (!coins[i].active) continue;
        
        Coin *c = &coins[i];
        
        // Animation
        c->frame = (c->frame + 1) % 16;
        
        // Player collection
        int px = player.x >> FIXED_SHIFT;
        int py = player.y >> FIXED_SHIFT;
        
        if (px < c->x + 8 && px + player.width > c->x &&
            py < c->y + 8 && py + player.height > c->y) {
            c->active = 0;
            game_coins++;
            score += 200;
            
            // Extra life at 100 coins
            if (game_coins >= 100) {
                game_coins -= 100;
                lives++;
            }
        }
    }
}

// ============================================================================
// CAMERA UPDATE
// ============================================================================

void update_camera(void) {
    int target_x = (player.x >> FIXED_SHIFT) - SCREEN_WIDTH / 3;
    
    // Smooth camera follow
    camera_x += (target_x - camera_x) / 8;
    
    // Clamp camera
    if (camera_x < 0) camera_x = 0;
    if (camera_x > (LEVEL_WIDTH * TILE_SIZE) - SCREEN_WIDTH)
        camera_x = (LEVEL_WIDTH * TILE_SIZE) - SCREEN_WIDTH;
    
    // Update background scroll
    REG_BG0HOFS = camera_x;
    REG_BG0VOFS = 0;
}

// ============================================================================
// RENDERING
// ============================================================================

void draw_player(void) {
    int screen_x = (player.x >> FIXED_SHIFT) - camera_x;
    int screen_y = (player.y >> FIXED_SHIFT);
    
    // Blink when invincible
    if (player.invincible && (player.inv_timer & 4)) {
        obj_hide(&obj_mem[0]);
        return;
    }
    
    // 16x16 sprite (4 tiles)
    int tile = player.frame * 4;
    int attr1_flip = player.facing_right ? 0 : ATTR1_HFLIP;
    
    obj_set_attr(&obj_mem[0],
        ATTR0_SQUARE | ATTR0_4BPP,
        ATTR1_SIZE_16 | attr1_flip,
        ATTR2_ID(tile) | ATTR2_PRIO(0));
    
    obj_set_pos(&obj_mem[0], screen_x, screen_y);
}

void draw_enemies(void) {
    int sprite_id = 1;
    
    for (int i = 0; i < MAX_ENEMIES && sprite_id < 32; i++) {
        if (!enemies[i].active) continue;
        
        Enemy *e = &enemies[i];
        int screen_x = (e->x >> FIXED_SHIFT) - camera_x;
        int screen_y = (e->y >> FIXED_SHIFT);
        
        // Skip off-screen
        if (screen_x < -16 || screen_x > SCREEN_WIDTH + 16) {
            obj_hide(&obj_mem[sprite_id]);
            sprite_id++;
            continue;
        }
        
        int tile;
        int attr1 = ATTR1_SIZE_16;
        
        if (e->stomped) {
            tile = 28; // Flat goomba
        } else if (e->type == 0) {
            // Goomba
            tile = 20 + (e->frame ? 4 : 0);
        } else if (e->type == 1) {
            // Koopa
            tile = 32 + (e->frame ? 4 : 0);
            if (e->vx > 0) attr1 |= ATTR1_HFLIP;
        } else {
            // Shell
            tile = 40;
        }
        
        obj_set_attr(&obj_mem[sprite_id],
            ATTR0_SQUARE | ATTR0_4BPP,
            attr1,
            ATTR2_ID(tile) | ATTR2_PRIO(0));
        
        obj_set_pos(&obj_mem[sprite_id], screen_x, screen_y);
        sprite_id++;
    }
    
    // Hide unused enemy sprites
    while (sprite_id < 32) {
        obj_hide(&obj_mem[sprite_id]);
        sprite_id++;
    }
}

void draw_coins_sprites(void) {
    int sprite_id = 32;
    
    for (int i = 0; i < MAX_COINS && sprite_id < 64; i++) {
        if (!coins[i].active) {
            continue;
        }
        
        Coin *c = &coins[i];
        int screen_x = c->x - camera_x;
        int screen_y = c->y;
        
        if (screen_x < -16 || screen_x > SCREEN_WIDTH + 16) {
            obj_hide(&obj_mem[sprite_id]);
            sprite_id++;
            continue;
        }
        
        int tile = (c->frame < 8) ? 44 : 48;
        
        obj_set_attr(&obj_mem[sprite_id],
            ATTR0_SQUARE | ATTR0_4BPP,
            ATTR1_SIZE_16,
            ATTR2_ID(tile) | ATTR2_PRIO(0));
        
        obj_set_pos(&obj_mem[sprite_id], screen_x, screen_y);
        sprite_id++;
    }
    
    // Hide unused coin sprites  
    while (sprite_id < 64) {
        obj_hide(&obj_mem[sprite_id]);
        sprite_id++;
    }
}

// Simple number drawing using sprites (reusing coin tiles as digits placeholder)
void draw_hud(void) {
    // For a proper implementation, you'd have digit tiles
    // This is simplified - just showing the concept
    
    // Lives indicator in top-left
    obj_set_attr(&obj_mem[64],
        ATTR0_SQUARE | ATTR0_4BPP,
        ATTR1_SIZE_16,
        ATTR2_ID(0) | ATTR2_PRIO(0));
    obj_set_pos(&obj_mem[64], 8, 8);
    
    // Coin counter icon
    obj_set_attr(&obj_mem[65],
        ATTR0_SQUARE | ATTR0_4BPP,
        ATTR1_SIZE_16,
        ATTR2_ID(44) | ATTR2_PRIO(0));
    obj_set_pos(&obj_mem[65], SCREEN_WIDTH - 40, 8);
}

void draw_title_screen(void) {
    // Hide all sprites except title elements
    for (int i = 0; i < 128; i++) {
        obj_hide(&obj_mem[i]);
    }
    
    // You could draw "PRESS START" using sprites
    // For simplicity, we'll just show Mario sprite in center
    obj_set_attr(&obj_mem[0],
        ATTR0_SQUARE | ATTR0_4BPP,
        ATTR1_SIZE_16,
        ATTR2_ID(0) | ATTR2_PRIO(0));
    obj_set_pos(&obj_mem[0], SCREEN_WIDTH/2 - 8, SCREEN_HEIGHT/2 - 8);
}

void draw_win_screen(void) {
    // Simple win indication
    for (int i = 0; i < 128; i++) {
        obj_hide(&obj_mem[i]);
    }
    
    // Show Mario at flag
    obj_set_attr(&obj_mem[0],
        ATTR0_SQUARE | ATTR0_4BPP,
        ATTR1_SIZE_16,
        ATTR2_ID(0) | ATTR2_PRIO(0));
    obj_set_pos(&obj_mem[0], SCREEN_WIDTH/2 - 8, SCREEN_HEIGHT/2 - 16);
}

// ============================================================================
// MAIN GAME LOOP
// ============================================================================

int main(void) {
    // Initialize
    irq_init(NULL);
    irq_enable(II_VBLANK);
    
    init_game();
    game_state = STATE_TITLE;
    
    while (1) {
        vid_vsync();
        key_poll();
        
        switch (game_state) {
            case STATE_TITLE:
                draw_title_screen();
                if (key_hit(KEY_START)) {
                    init_game();
                    game_state = STATE_PLAYING;
                }
                break;
                
            case STATE_PLAYING:
                update_player();
                update_enemies();
                update_coins();
                update_camera();
                
                draw_player();
                draw_enemies();
                draw_coins_sprites();
                draw_hud();
                break;
                
            case STATE_DEAD:
                // Handled in update_player death sequence
                break;
                
            case STATE_WIN:
                draw_win_screen();
                if (key_hit(KEY_START)) {
                    game_state = STATE_TITLE;
                }
                break;
        }
    }
    
    return 0;
}
