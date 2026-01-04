/*
 * TwinBee-style Vertical Shmup for GBA
 * Uses TONC library
 * Compile with: make (using a standard GBA Makefile with TONC)
 */

#include <tonc.h>
#include <string.h>

#define RGB15_C(r,g,b)  ((r) | ((g) << 5) | ((b) << 10))

// ============================================================================
// Constants
// ============================================================================
#define SCREEN_W        240
#define SCREEN_H        160
#define FIX_SHIFT       8
#define FIX(x)          ((x) << FIX_SHIFT)
#define UNFIX(x)        ((x) >> FIX_SHIFT)

#define MAX_P_BULLETS   16
#define MAX_ENEMIES     12
#define MAX_E_BULLETS   24
#define MAX_POWERUPS    4
#define MAX_EXPLOSIONS  8

#define PLAYER_SPEED    FIX(2)
#define BULLET_SPEED    FIX(4)
#define MAX_POWER       3

// ============================================================================
// Types
// ============================================================================
typedef struct {
    s32 x, y;
    s32 vx, vy;
    s16 timer;
    u8 active;
    u8 type;
    u8 hp;
    u8 frame;
} Entity;

// ============================================================================
// Sprite Tile Data (4bpp, 8x8)
// ============================================================================

// Player ship - cute bell-like design
static const u32 tile_player[8] = {
    0x00000000,
    0x00022000,
    0x00222200,
    0x02233220,
    0x22233222,
    0x22222222,
    0x02200220,
    0x00000000,
};

// Player bullet
static const u32 tile_pbullet[8] = {
    0x00000000,
    0x00011000,
    0x00111100,
    0x01111110,
    0x01111110,
    0x00111100,
    0x00011000,
    0x00000000,
};

// Enemy type 0 - basic (bell enemy)
static const u32 tile_enemy0[8] = {
    0x00044000,
    0x00444400,
    0x04455440,
    0x44455444,
    0x44444444,
    0x04444440,
    0x00400400,
    0x00000000,
};

// Enemy type 1 - medium
static const u32 tile_enemy1[8] = {
    0x00055000,
    0x00555500,
    0x05566550,
    0x55566555,
    0x55555555,
    0x05555550,
    0x00500500,
    0x00000000,
};

// Enemy type 2 - tough
static const u32 tile_enemy2[8] = {
    0x00077000,
    0x00777700,
    0x07788770,
    0x77788777,
    0x77777777,
    0x07777770,
    0x00700700,
    0x00000000,
};

// Enemy bullet
static const u32 tile_ebullet[8] = {
    0x00000000,
    0x00044000,
    0x00444400,
    0x00444400,
    0x00044000,
    0x00000000,
    0x00000000,
    0x00000000,
};

// Powerup (bell)
static const u32 tile_powerup[8] = {
    0x00099000,
    0x00999900,
    0x09999990,
    0x99999999,
    0x99999999,
    0x09999990,
    0x00099000,
    0x00900900,
};

// Explosion frame
static const u32 tile_explosion[8] = {
    0x00011000,
    0x01100110,
    0x10011001,
    0x10000001,
    0x10000001,
    0x10011001,
    0x01100110,
    0x00011000,
};

// Small star for background
static const u32 tile_star[8] = {
    0x00000000,
    0x00000000,
    0x00000000,
    0x00011000,
    0x00011000,
    0x00000000,
    0x00000000,
    0x00000000,
};

// Sprite palette
static const u16 obj_palette[16] = {
    RGB15_C( 0,  0,  0),    // 0: Transparent
    RGB15_C(31, 31, 31),    // 1: White
    RGB15_C( 0, 16, 31),    // 2: Light blue (player)
    RGB15_C(31, 31,  0),    // 3: Yellow (player accent)
    RGB15_C(31, 16, 16),    // 4: Pink (enemy 0)
    RGB15_C(31,  0,  0),    // 5: Red (enemy 1)
    RGB15_C(20, 16, 31),    // 6: Purple accent
    RGB15_C(20,  0, 31),    // 7: Purple (enemy 2)
    RGB15_C(31, 20,  0),    // 8: Orange accent
    RGB15_C( 0, 31,  0),    // 9: Green (powerup)
    RGB15_C(31, 16,  0),    // A: Orange (explosion)
    0, 0, 0, 0, 0
};

// Background palette
static const u16 bg_palette[16] = {
    RGB15_C( 0,  0,  4),    // 0: Dark blue (space)
    RGB15_C(20, 20, 25),    // 1: Dim star
    RGB15_C(31, 31, 31),    // 2: Bright star
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0
};

// ============================================================================
// Globals
// ============================================================================
static Entity player;
static Entity p_bullets[MAX_P_BULLETS];
static Entity enemies[MAX_ENEMIES];
static Entity e_bullets[MAX_E_BULLETS];
static Entity powerups[MAX_POWERUPS];
static Entity explosions[MAX_EXPLOSIONS];

static u32 score;
static u32 high_score;
static s32 lives;
static s32 power_level;
static u32 frame_count;
static u8 game_state;       // 0=title, 1=play, 2=gameover
static u8 shoot_cooldown;
static u8 invincible;
static s32 bg_scroll;

// ============================================================================
// Utility Functions
// ============================================================================

static inline s32 abs_val(s32 x) {
    return (x < 0) ? -x : x;
}

static inline int collides(s32 x1, s32 y1, s32 x2, s32 y2, s32 range) {
    return (abs_val(UNFIX(x1 - x2)) < range) && (abs_val(UNFIX(y1 - y2)) < range);
}

// ============================================================================
// Graphics Initialization
// ============================================================================

static void init_graphics(void) {
    // Copy sprite palette
    memcpy16(pal_obj_mem, obj_palette, 16);
    
    // Copy background palette  
    memcpy16(pal_bg_mem, bg_palette, 16);
    
    // Copy sprite tiles to tile memory (block 4 for sprites)
    memcpy32(&tile_mem[4][0], tile_player, 8);
    memcpy32(&tile_mem[4][1], tile_pbullet, 8);
    memcpy32(&tile_mem[4][2], tile_enemy0, 8);
    memcpy32(&tile_mem[4][3], tile_enemy1, 8);
    memcpy32(&tile_mem[4][4], tile_enemy2, 8);
    memcpy32(&tile_mem[4][5], tile_ebullet, 8);
    memcpy32(&tile_mem[4][6], tile_powerup, 8);
    memcpy32(&tile_mem[4][7], tile_explosion, 8);
    
    // Copy star tile for background
    memcpy32(&tile_mem[0][1], tile_star, 8);
    
    // Set up background 0 for starfield
    REG_BG0CNT = BG_CBB(0) | BG_SBB(30) | BG_4BPP | BG_REG_32x32 | BG_PRIO(3);
    
    // Generate random starfield in screenblock 30
    u16 *sb = (u16*)se_mem[30];
    for (int i = 0; i < 32*32; i++) {
        if ((qran() & 0x1F) == 0) {
            sb[i] = SE_PALBANK(0) | 1;  // Star tile
        } else {
            sb[i] = 0;  // Empty
        }
    }
    
    // Initialize OAM
    oam_init(obj_mem, 128);
}

// ============================================================================
// Entity Management
// ============================================================================

static void spawn_p_bullet(s32 x, s32 y, s32 vx) {
    for (int i = 0; i < MAX_P_BULLETS; i++) {
        if (!p_bullets[i].active) {
            p_bullets[i].x = x;
            p_bullets[i].y = y;
            p_bullets[i].vx = vx;
            p_bullets[i].vy = -BULLET_SPEED;
            p_bullets[i].active = 1;
            return;
        }
    }
}

static void spawn_enemy(void) {
    for (int i = 0; i < MAX_ENEMIES; i++) {
        if (!enemies[i].active) {
            enemies[i].x = FIX(16 + (qran() % (SCREEN_W - 32)));
            enemies[i].y = FIX(-12);
            enemies[i].vx = FIX((qran() % 3) - 1);
            
            // Enemy type based on score/time
            if (score > 5000) {
                enemies[i].type = qran() % 3;
            } else if (score > 2000) {
                enemies[i].type = qran() % 2;
            } else {
                enemies[i].type = 0;
            }
            
            enemies[i].vy = FIX(1) + (enemies[i].type << 6);
            enemies[i].hp = enemies[i].type + 1;
            enemies[i].timer = qran() % 60;
            enemies[i].active = 1;
            return;
        }
    }
}

static void spawn_e_bullet(s32 x, s32 y) {
    for (int i = 0; i < MAX_E_BULLETS; i++) {
        if (!e_bullets[i].active) {
            // Aim towards player
            s32 dx = player.x - x;
            s32 dy = player.y - y;
            
            e_bullets[i].x = x;
            e_bullets[i].y = y;
            e_bullets[i].vx = dx >> 6;  // Simple approximation
            e_bullets[i].vy = FIX(2);
            e_bullets[i].active = 1;
            return;
        }
    }
}

static void spawn_powerup(s32 x, s32 y) {
    for (int i = 0; i < MAX_POWERUPS; i++) {
        if (!powerups[i].active) {
            powerups[i].x = x;
            powerups[i].y = y;
            powerups[i].vy = FIX(1);
            powerups[i].timer = 0;
            powerups[i].active = 1;
            return;
        }
    }
}

static void spawn_explosion(s32 x, s32 y) {
    for (int i = 0; i < MAX_EXPLOSIONS; i++) {
        if (!explosions[i].active) {
            explosions[i].x = x;
            explosions[i].y = y;
            explosions[i].timer = 16;
            explosions[i].active = 1;
            return;
        }
    }
}

// ============================================================================
// Game Logic
// ============================================================================

static void init_game(void) {
    player.x = FIX(SCREEN_W / 2);
    player.y = FIX(SCREEN_H - 24);
    player.active = 1;
    
    score = 0;
    lives = 3;
    power_level = 0;
    frame_count = 0;
    shoot_cooldown = 0;
    invincible = 60;  // Brief invincibility at start
    bg_scroll = 0;
    
    memset(p_bullets, 0, sizeof(p_bullets));
    memset(enemies, 0, sizeof(enemies));
    memset(e_bullets, 0, sizeof(e_bullets));
    memset(powerups, 0, sizeof(powerups));
    memset(explosions, 0, sizeof(explosions));
}

static void player_hit(void) {
    if (invincible > 0) return;
    
    lives--;
    power_level = 0;
    invincible = 90;
    
    spawn_explosion(player.x, player.y);
    
    if (lives <= 0) {
        game_state = 2;
        if (score > high_score) high_score = score;
    }
}

static void update_player(void) {
    if (invincible > 0) invincible--;
    
    // Movement
    if (key_is_down(KEY_LEFT) && UNFIX(player.x) > 8)
        player.x -= PLAYER_SPEED;
    if (key_is_down(KEY_RIGHT) && UNFIX(player.x) < SCREEN_W - 8)
        player.x += PLAYER_SPEED;
    if (key_is_down(KEY_UP) && UNFIX(player.y) > 8)
        player.y -= PLAYER_SPEED;
    if (key_is_down(KEY_DOWN) && UNFIX(player.y) < SCREEN_H - 8)
        player.y += PLAYER_SPEED;
    
    // Shooting
    if (shoot_cooldown > 0) shoot_cooldown--;
    
    if (key_is_down(KEY_A) && shoot_cooldown == 0) {
        spawn_p_bullet(player.x, player.y - FIX(8), 0);
        
        if (power_level >= 1) {
            spawn_p_bullet(player.x - FIX(8), player.y - FIX(4), -FIX(1)/2);
            spawn_p_bullet(player.x + FIX(8), player.y - FIX(4), FIX(1)/2);
        }
        if (power_level >= 2) {
            spawn_p_bullet(player.x - FIX(12), player.y, -FIX(1));
            spawn_p_bullet(player.x + FIX(12), player.y, FIX(1));
        }
        if (power_level >= 3) {
            spawn_p_bullet(player.x, player.y - FIX(4), 0);
        }
        
        shoot_cooldown = (power_level >= 2) ? 6 : 10;
    }
    
    // Bomb (clears screen)
    if (key_hit(KEY_B) && lives > 0) {
        for (int i = 0; i < MAX_ENEMIES; i++) {
            if (enemies[i].active) {
                spawn_explosion(enemies[i].x, enemies[i].y);
                score += 50 * (enemies[i].type + 1);
                enemies[i].active = 0;
            }
        }
        for (int i = 0; i < MAX_E_BULLETS; i++) {
            e_bullets[i].active = 0;
        }
    }
}

static void update_p_bullets(void) {
    for (int i = 0; i < MAX_P_BULLETS; i++) {
        if (!p_bullets[i].active) continue;
        
        p_bullets[i].x += p_bullets[i].vx;
        p_bullets[i].y += p_bullets[i].vy;
        
        s32 bx = UNFIX(p_bullets[i].x);
        s32 by = UNFIX(p_bullets[i].y);
        
        if (by < -8 || bx < -8 || bx > SCREEN_W + 8)
            p_bullets[i].active = 0;
    }
}

static void update_enemies(void) {
    // Spawn rate increases with score
    int spawn_rate = 80 - (score / 200);
    if (spawn_rate < 25) spawn_rate = 25;
    
    if ((frame_count % spawn_rate) == 0) {
        spawn_enemy();
    }
    
    for (int i = 0; i < MAX_ENEMIES; i++) {
        if (!enemies[i].active) continue;
        
        enemies[i].x += enemies[i].vx;
        enemies[i].y += enemies[i].vy;
        enemies[i].timer++;
        
        // Bounce off walls
        s32 ex = UNFIX(enemies[i].x);
        if (ex < 8 || ex > SCREEN_W - 8) {
            enemies[i].vx = -enemies[i].vx;
        }
        
        // Shooting behavior for tougher enemies
        if (enemies[i].type >= 1 && (enemies[i].timer % 70) == 35) {
            spawn_e_bullet(enemies[i].x, enemies[i].y);
        }
        
        // Remove if off screen
        if (UNFIX(enemies[i].y) > SCREEN_H + 16)
            enemies[i].active = 0;
    }
}

static void update_e_bullets(void) {
    for (int i = 0; i < MAX_E_BULLETS; i++) {
        if (!e_bullets[i].active) continue;
        
        e_bullets[i].x += e_bullets[i].vx;
        e_bullets[i].y += e_bullets[i].vy;
        
        s32 by = UNFIX(e_bullets[i].y);
        s32 bx = UNFIX(e_bullets[i].x);
        
        if (by > SCREEN_H + 8 || bx < -8 || bx > SCREEN_W + 8)
            e_bullets[i].active = 0;
    }
}

static void update_powerups(void) {
    for (int i = 0; i < MAX_POWERUPS; i++) {
        if (!powerups[i].active) continue;
        
        powerups[i].y += powerups[i].vy;
        powerups[i].timer++;
        
        // Slight wobble
        powerups[i].x += (lu_sin(powerups[i].timer << 9) >> 11);
        
        if (UNFIX(powerups[i].y) > SCREEN_H + 8)
            powerups[i].active = 0;
    }
}

static void update_explosions(void) {
    for (int i = 0; i < MAX_EXPLOSIONS; i++) {
        if (!explosions[i].active) continue;
        
        explosions[i].timer--;
        if (explosions[i].timer <= 0)
            explosions[i].active = 0;
    }
}

static void check_collisions(void) {
    // Player bullets vs enemies
    for (int i = 0; i < MAX_P_BULLETS; i++) {
        if (!p_bullets[i].active) continue;
        
        for (int j = 0; j < MAX_ENEMIES; j++) {
            if (!enemies[j].active) continue;
            
            if (collides(p_bullets[i].x, p_bullets[i].y,
                        enemies[j].x, enemies[j].y, 10)) {
                p_bullets[i].active = 0;
                enemies[j].hp--;
                
                if (enemies[j].hp <= 0) {
                    spawn_explosion(enemies[j].x, enemies[j].y);
                    enemies[j].active = 0;
                    score += 100 * (enemies[j].type + 1);
                    
                    // Chance to drop powerup
                    if ((qran() & 0x07) == 0) {
                        spawn_powerup(enemies[j].x, enemies[j].y);
                    }
                }
                break;
            }
        }
    }
    
    // Enemy bullets vs player
    for (int i = 0; i < MAX_E_BULLETS; i++) {
        if (!e_bullets[i].active) continue;
        
        if (collides(e_bullets[i].x, e_bullets[i].y, player.x, player.y, 6)) {
            e_bullets[i].active = 0;
            player_hit();
        }
    }
    
    // Enemies vs player
    for (int i = 0; i < MAX_ENEMIES; i++) {
        if (!enemies[i].active) continue;
        
        if (collides(enemies[i].x, enemies[i].y, player.x, player.y, 8)) {
            spawn_explosion(enemies[i].x, enemies[i].y);
            enemies[i].active = 0;
            player_hit();
        }
    }
    
    // Powerups vs player
    for (int i = 0; i < MAX_POWERUPS; i++) {
        if (!powerups[i].active) continue;
        
        if (collides(powerups[i].x, powerups[i].y, player.x, player.y, 12)) {
            powerups[i].active = 0;
            if (power_level < MAX_POWER) power_level++;
            score += 100;
        }
    }
}

// ============================================================================
// Rendering
// ============================================================================

static void draw_sprites(void) {
    int oam_idx = 0;
    
    // Draw player (blink when invincible)
    if (invincible == 0 || (frame_count & 0x04)) {
        int px = UNFIX(player.x) - 4;
        int py = UNFIX(player.y) - 4;
        obj_set_attr(&obj_mem[oam_idx++],
            ATTR0_Y(py & 0xFF) | ATTR0_SQUARE | ATTR0_4BPP,
            ATTR1_X(px & 0x1FF) | ATTR1_SIZE_8,
            ATTR2_ID(0) | ATTR2_PRIO(1));
    }
    
    // Draw player bullets
    for (int i = 0; i < MAX_P_BULLETS && oam_idx < 120; i++) {
        if (!p_bullets[i].active) continue;
        int bx = UNFIX(p_bullets[i].x) - 4;
        int by = UNFIX(p_bullets[i].y) - 4;
        obj_set_attr(&obj_mem[oam_idx++],
            ATTR0_Y(by & 0xFF) | ATTR0_SQUARE | ATTR0_4BPP,
            ATTR1_X(bx & 0x1FF) | ATTR1_SIZE_8,
            ATTR2_ID(1) | ATTR2_PRIO(1));
    }
    
    // Draw enemies
    for (int i = 0; i < MAX_ENEMIES && oam_idx < 120; i++) {
        if (!enemies[i].active) continue;
        int ex = UNFIX(enemies[i].x) - 4;
        int ey = UNFIX(enemies[i].y) - 4;
        obj_set_attr(&obj_mem[oam_idx++],
            ATTR0_Y(ey & 0xFF) | ATTR0_SQUARE | ATTR0_4BPP,
            ATTR1_X(ex & 0x1FF) | ATTR1_SIZE_8,
            ATTR2_ID(2 + enemies[i].type) | ATTR2_PRIO(1));
    }
    
    // Draw enemy bullets
    for (int i = 0; i < MAX_E_BULLETS && oam_idx < 120; i++) {
        if (!e_bullets[i].active) continue;
        int bx = UNFIX(e_bullets[i].x) - 4;
        int by = UNFIX(e_bullets[i].y) - 4;
        obj_set_attr(&obj_mem[oam_idx++],
            ATTR0_Y(by & 0xFF) | ATTR0_SQUARE | ATTR0_4BPP,
            ATTR1_X(bx & 0x1FF) | ATTR1_SIZE_8,
            ATTR2_ID(5) | ATTR2_PRIO(1));
    }
    
    // Draw powerups
    for (int i = 0; i < MAX_POWERUPS && oam_idx < 120; i++) {
        if (!powerups[i].active) continue;
        int px = UNFIX(powerups[i].x) - 4;
        int py = UNFIX(powerups[i].y) - 4;
        obj_set_attr(&obj_mem[oam_idx++],
            ATTR0_Y(py & 0xFF) | ATTR0_SQUARE | ATTR0_4BPP,
            ATTR1_X(px & 0x1FF) | ATTR1_SIZE_8,
            ATTR2_ID(6) | ATTR2_PRIO(1));
    }
    
    // Draw explosions
    for (int i = 0; i < MAX_EXPLOSIONS && oam_idx < 120; i++) {
        if (!explosions[i].active) continue;
        int ex = UNFIX(explosions[i].x) - 4;
        int ey = UNFIX(explosions[i].y) - 4;
        // Scale based on timer for animation effect
        u16 size = (explosions[i].timer > 8) ? ATTR1_SIZE_8 : ATTR1_SIZE_8;
        obj_set_attr(&obj_mem[oam_idx++],
            ATTR0_Y(ey & 0xFF) | ATTR0_SQUARE | ATTR0_4BPP,
            ATTR1_X(ex & 0x1FF) | size,
            ATTR2_ID(7) | ATTR2_PRIO(0));
    }
    
    // Draw HUD: Lives as small sprites on top
    for (int i = 0; i < lives && oam_idx < 126; i++) {
        obj_set_attr(&obj_mem[oam_idx++],
            ATTR0_Y(4) | ATTR0_SQUARE | ATTR0_4BPP,
            ATTR1_X(SCREEN_W - 12 - i * 10) | ATTR1_SIZE_8,
            ATTR2_ID(0) | ATTR2_PRIO(0));
    }
    
    // Draw power level indicators
    for (int i = 0; i < power_level && oam_idx < 126; i++) {
        obj_set_attr(&obj_mem[oam_idx++],
            ATTR0_Y(SCREEN_H - 12) | ATTR0_SQUARE | ATTR0_4BPP,
            ATTR1_X(4 + i * 10) | ATTR1_SIZE_8,
            ATTR2_ID(6) | ATTR2_PRIO(0));
    }
    
    // Hide remaining sprites
    while (oam_idx < 128) {
        obj_hide(&obj_mem[oam_idx++]);
    }
}

static void update_background(void) {
    bg_scroll++;
    REG_BG0VOFS = -(bg_scroll >> 1);
}

// ============================================================================
// Main
// ============================================================================

int main(void) {
    // Initialize interrupts
    irq_init(NULL);
    irq_add(II_VBLANK, NULL);
    
    // Set display mode: Mode 0, OBJ enabled, 1D OBJ mapping, BG0 enabled
    REG_DISPCNT = DCNT_MODE0 | DCNT_OBJ | DCNT_OBJ_1D | DCNT_BG0;
    
    // Initialize graphics
    init_graphics();
    
    // Start at title
    game_state = 0;
    high_score = 0;
    
    // Main loop
    while (1) {
        VBlankIntrWait();
        key_poll();
        
        switch (game_state) {
            case 0: // Title screen
                // Simple "press start" behavior
                if (key_hit(KEY_START)) {
                    init_game();
                    game_state = 1;
                }
                
                // Animate title screen with moving stars
                update_background();
                draw_sprites();
                break;
                
            case 1: // Playing
                update_player();
                update_p_bullets();
                update_enemies();
                update_e_bullets();
                update_powerups();
                update_explosions();
                check_collisions();
                
                update_background();
                draw_sprites();
                
                frame_count++;
                break;
                
            case 2: // Game Over
                update_background();
                draw_sprites();
                
                if (key_hit(KEY_START)) {
                    init_game();
                    game_state = 1;
                }
                break;
        }
    }
    
    return 0;
}
