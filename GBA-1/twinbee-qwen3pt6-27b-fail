/**
 * Twinbee-like Vertical Shmup for GBA
 * Uses tonc library (https://www.coranac.com/projects/tonc/)
 * Compile with devkitARM + tonc toolchain
 * 
 * Controls:
 *   D-Pad: Move player
 *   A: Shoot (also auto-fires)
 *   Start: Pause/Resume
 *   Select: Reset
 */

#include <tonc.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

// ==================== CONSTANTS ====================
#define SCREEN_W    240
#define SCREEN_H    160
#define MAX_ENEMIES 12
#define MAX_BULLETS 30
#define MAX_EXPLOS  8
#define SHOOT_COOLDOWN 8
#define ENEMY_SPAWN_RATE 60

// Sprite sizes
#define SPRITE_W    8
#define SPRITE_H    8
#define PLAYER_W    16
#define PLAYER_H    16

// ==================== SPRITE & PALETTE DATA ====================
// 4bpp format: each u16 holds 2 pixels. Lower 4 bits = pixel 0, upper 4 = pixel 1.
// Palette indices: 0=transparent, 1=dark, 2=mid, 3=bright

const u16 player_gfx[64] = {
    0x0000, 0x0000, 0x0110, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000,
    0x0000, 0x0000, 0x1221, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000,
    0x0000, 0x0000, 0x2332, 0x2222, 0x0000, 0x0000, 0x0000, 0x0000,
    0x0000, 0x0000, 0x1221, 0x2112, 0x0000, 0x0000, 0x0000, 0x0000,
    0x0000, 0x0000, 0x0110, 0x0110, 0x0000, 0x0000, 0x0000, 0x0000,
    0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000,
    0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000,
    0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000
};

const u16 enemy_gfx[64] = {
    0x0000, 0x0000, 0x0110, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000,
    0x0000, 0x0000, 0x1221, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000,
    0x0000, 0x0000, 0x2332, 0x3332, 0x0000, 0x0000, 0x0000, 0x0000,
    0x0000, 0x0000, 0x2112, 0x2112, 0x0000, 0x0000, 0x0000, 0x0000,
    0x0000, 0x0000, 0x1001, 0x1001, 0x0000, 0x0000, 0x0000, 0x0000,
    0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000,
    0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000,
    0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000
};

const u16 bullet_gfx[64] = {
    0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000,
    0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000,
    0x0000, 0x0000, 0x0110, 0x1111, 0x0000, 0x0000, 0x0000, 0x0000,
    0x0000, 0x0000, 0x1221, 0x2222, 0x0000, 0x0000, 0x0000, 0x0000,
    0x0000, 0x0000, 0x1221, 0x2222, 0x0000, 0x0000, 0x0000, 0x0000,
    0x0000, 0x0000, 0x0110, 0x1111, 0x0000, 0x0000, 0x0000, 0x0000,
    0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000,
    0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000
};

const u16 explosion_gfx[64] = {
    0x0000, 0x0000, 0x0110, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000,
    0x0000, 0x0000, 0x1221, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000,
    0x0000, 0x0000, 0x2332, 0x3332, 0x0000, 0x0000, 0x0000, 0x0000,
    0x0000, 0x0000, 0x2332, 0x3332, 0x0000, 0x0000, 0x0000, 0x0000,
    0x0000, 0x0000, 0x1221, 0x2221, 0x0000, 0x0000, 0x0000, 0x0000,
    0x0000, 0x0000, 0x0110, 0x1110, 0x0000, 0x0000, 0x0000, 0x0000,
    0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000,
    0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000
};

// Palettes (RGBA16)
const u16 player_pal[16] = {
    CLR_TRANSP, RGB15(0,0,128), RGB15(0,64,224), RGB15(0,192,255),
    RGB15(128,128,255), RGB15(192,192,255), RGB15(255,255,255), CLR_NONE,
    CLR_NONE, CLR_NONE, CLR_NONE, CLR_NONE, CLR_NONE, CLR_NONE, CLR_NONE, CLR_NONE
};
const u16 enemy_pal[16] = {
    CLR_TRANSP, RGB15(128,0,0), RGB15(224,0,0), RGB15(255,64,64),
    RGB15(255,128,128), RGB15(255,192,192), RGB15(255,255,255), CLR_NONE,
    CLR_NONE, CLR_NONE, CLR_NONE, CLR_NONE, CLR_NONE, CLR_NONE, CLR_NONE, CLR_NONE
};
const u16 bullet_pal[16] = {
    CLR_TRANSP, RGB15(128,128,0), RGB15(224,224,0), RGB15(255,255,64),
    RGB15(255,255,128), RGB15(255,255,192), RGB15(255,255,255), CLR_NONE,
    CLR_NONE, CLR_NONE, CLR_NONE, CLR_NONE, CLR_NONE, CLR_NONE, CLR_NONE, CLR_NONE
};
const u16 explosion_pal[16] = {
    CLR_TRANSP, RGB15(128,64,0), RGB15(224,128,0), RGB15(255,192,64),
    RGB15(255,224,128), RGB15(255,255,192), RGB15(255,255,255), CLR_NONE,
    CLR_NONE, CLR_NONE, CLR_NONE, CLR_NONE, CLR_NONE, CLR_NONE, CLR_NONE, CLR_NONE
};

// ==================== GAME STATE ====================
typedef struct {
    int x, y;
    int w, h;
    int alive;
} Entity;

typedef struct {
    Entity base;
    int timer;
    int pattern; // 0=straight, 1=sine, 2=zigzag
    int phase;
} Enemy;

typedef struct {
    Entity base;
    int dx, dy;
} Bullet;

typedef struct {
    Entity base;
    int frame;
} Explosion;

// Global variables
Entity player;
Enemy enemies[MAX_ENEMIES];
Bullet bullets[MAX_BULLETS];
Explosion explosions[MAX_EXPLOS];
int score = 0;
int lives = 3;
int shoot_timer = 0;
int spawn_timer = 0;
int scroll_y = 0;
int paused = 0;
int game_over = 0;

// OAM pointer
OAM* oam = 0;

// ==================== HELPERS ====================
void rect_overlap(int ax, int ay, int aw, int ah, int bx, int by, int bw, int bh, int* res) {
    *res = (ax < bx+bw && ax+aw > bx && ay < by+bh && ay+ah > by);
}

void spawn_explosion(int x, int y) {
    for(int i=0; i<MAX_EXPLOS; i++) {
        if(!explosions[i].base.alive) {
            explosions[i].base.x = x - SPRITE_W/2;
            explosions[i].base.y = y - SPRITE_H/2;
            explosions[i].base.w = SPRITE_W;
            explosions[i].base.h = SPRITE_H;
            explosions[i].base.alive = 1;
            explosions[i].frame = 0;
            return;
        }
    }
}

// ==================== INITIALIZATION ====================
void init_game() {
    // Clear entities
    memset(&player, 0, sizeof(player));
    memset(enemies, 0, sizeof(enemies));
    memset(bullets, 0, sizeof(bullets));
    memset(explosions, 0, sizeof(explosions));

    // Reset state
    score = 0; lives = 3; shoot_timer = 0; spawn_timer = 0;
    scroll_y = 0; paused = 0; game_over = 0;

    // Player setup
    player.x = SCREEN_W/2 - PLAYER_W/2;
    player.y = SCREEN_H - 40;
    player.w = PLAYER_W; player.h = PLAYER_H;
    player.alive = 1;

    // Load sprites to VRAM
    gfx_upload(0, player_gfx, 64, GFX_4BPP);
    gfx_upload(1, enemy_gfx, 64, GFX_4BPP);
    gfx_upload(2, bullet_gfx, 64, GFX_4BPP);
    gfx_upload(3, explosion_gfx, 64, GFX_4BPP);

    // Upload palettes
    pal_set_rgba16(0, player_pal, 4);
    pal_set_rgba16(4, enemy_pal, 4);
    pal_set_rgba16(8, bullet_pal, 4);
    pal_set_rgba16(12, explosion_pal, 4);

    // Setup OAM
    oam = oam_init();
    oam_attach_bmp(oam, 0);

    // Setup text
    text_setup_4bpp();
}

// ==================== UPDATE ====================
void update_game() {
    if(paused || !player.alive) return;

    // Input
    u32 keys = keys_poll();
    int dx = 0, dy = 0;
    if(KEY_DOWN(keys, KEY_LEFT)) dx = -2;
    if(KEY_DOWN(keys, KEY_RIGHT)) dx = 2;
    if(KEY_DOWN(keys, KEY_UP)) dy = -2;
    if(KEY_DOWN(keys, KEY_DOWN)) dy = 2;

    // Move player
    player.x += dx;
    player.y += dy;
    if(player.x < 0) player.x = 0;
    if(player.y < 0) player.y = 0;
    if(player.x + player.w > SCREEN_W) player.x = SCREEN_W - player.w;
    if(player.y + player.h > SCREEN_H) player.y = SCREEN_H - player.h;

    // Shooting
    if(shoot_timer > 0) shoot_timer--;
    if(KEY_DOWN(keys, KEY_A) || shoot_timer == 0) {
        shoot_timer = SHOOT_COOLDOWN;
        // Spawn player bullet
        for(int i=0; i<MAX_BULLETS; i++) {
            if(!bullets[i].base.alive) {
                bullets[i].base.x = player.x + PLAYER_W/2 - SPRITE_W/2;
                bullets[i].base.y = player.y;
                bullets[i].base.w = SPRITE_W; bullets[i].base.h = SPRITE_H;
                bullets[i].base.alive = 1;
                bullets[i].dx = 0; bullets[i].dy = -4;
                break;
            }
        }
    }

    // Enemy spawning
    if(spawn_timer > 0) spawn_timer--;
    else {
        spawn_timer = ENEMY_SPAWN_RATE;
        for(int i=0; i<MAX_ENEMIES; i++) {
            if(!enemies[i].base.alive) {
                enemies[i].base.x = rand() % (SCREEN_W - SPRITE_W);
                enemies[i].base.y = -SPRITE_H;
                enemies[i].base.w = SPRITE_W; enemies[i].base.h = SPRITE_H;
                enemies[i].base.alive = 1;
                enemies[i].timer = 0;
                enemies[i].pattern = rand() % 3;
                enemies[i].phase = rand() % 256;
                break;
            }
        }
    }

    // Update enemies
    for(int i=0; i<MAX_ENEMIES; i++) {
        if(!enemies[i].base.alive) continue;
        Enemy* e = &enemies[i];
        e->timer++;
        e->base.y += 1;

        if(e->pattern == 1) e->base.x += (sinf((e->timer + e->phase)*0.05f) * 2);
        else if(e->pattern == 2) e->base.x += ((e->timer % 60 < 30) ? 1 : -1);

        if(e->base.y > SCREEN_H) {
            e->base.alive = 0;
        }
    }

    // Update bullets
    for(int i=0; i<MAX_BULLETS; i++) {
        if(!bullets[i].base.alive) continue;
        Bullet* b = &bullets[i];
        b->base.x += b->dx;
        b->base.y += b->dy;
        if(b->base.y < -b->base.h || b->base.y > SCREEN_H) {
            b->base.alive = 0;
        }
    }

    // Update explosions
    for(int i=0; i<MAX_EXPLOS; i++) {
        if(!explosions[i].base.alive) continue;
        explosions[i].frame++;
        if(explosions[i].frame > 15) {
            explosions[i].base.alive = 0;
        }
    }

    // Collision: Bullet vs Enemy
    for(int i=0; i<MAX_BULLETS; i++) {
        if(!bullets[i].base.alive) continue;
        for(int j=0; j<MAX_ENEMIES; j++) {
            if(!enemies[j].base.alive) continue;
            int hit = 0;
            rect_overlap(bullets[i].base.x, bullets[i].base.y, bullets[i].base.w, bullets[i].base.h,
                         enemies[j].base.x, enemies[j].base.y, enemies[j].base.w, enemies[j].base.h, &hit);
            if(hit) {
                bullets[i].base.alive = 0;
                enemies[j].base.alive = 0;
                score += 100;
                spawn_explosion(enemies[j].base.x + SPRITE_W/2, enemies[j].base.y + SPRITE_H/2);
            }
        }
    }

    // Collision: Player vs Enemy
    for(int i=0; i<MAX_ENEMIES; i++) {
        if(!enemies[i].base.alive) continue;
        int hit = 0;
        rect_overlap(player.x, player.y, player.w, player.h,
                     enemies[i].base.x, enemies[i].base.y, enemies[i].base.w, enemies[i].base.h, &hit);
        if(hit) {
            enemies[i].base.alive = 0;
            spawn_explosion(player.x + PLAYER_W/2, player.y + PLAYER_H/2);
            lives--;
            if(lives <= 0) {
                player.alive = 0;
                game_over = 1;
            } else {
                // Respawn briefly invincible (not implemented fully, just recenter)
                player.x = SCREEN_W/2 - PLAYER_W/2;
                player.y = SCREEN_H - 40;
            }
        }
    }

    // Scroll background
    scroll_y += 1;
    if(scroll_y > 256) scroll_y -= 256;
}

// ==================== RENDER ====================
void render_game() {
    oam_clear();

    // Draw player
    if(player.alive) {
        obj_set(&oam[0], player.x, player.y, OBJ_SIZE(32,32)|OBJ_4BPP, 0);
        oam_set_attr3(&oam[0], OBJ_PAL(0));
    }

    // Draw enemies
    int oam_idx = 1;
    for(int i=0; i<MAX_ENEMIES; i++) {
        if(!enemies[i].base.alive || oam_idx >= 128) continue;
        obj_set(&oam[oam_idx], enemies[i].base.x, enemies[i].base.y, OBJ_SIZE(16,16)|OBJ_4BPP, 1);
        oam_set_attr3(&oam[oam_idx], OBJ_PAL(1));
        oam_idx++;
    }

    // Draw bullets
    for(int i=0; i<MAX_BULLETS; i++) {
        if(!bullets[i].base.alive || oam_idx >= 128) continue;
        obj_set(&oam[oam_idx], bullets[i].base.x, bullets[i].base.y, OBJ_SIZE(16,16)|OBJ_4BPP, 2);
        oam_set_attr3(&oam[oam_idx], OBJ_PAL(2));
        oam_idx++;
    }

    // Draw explosions
    for(int i=0; i<MAX_EXPLOS; i++) {
        if(!explosions[i].base.alive || oam_idx >= 128) continue;
        obj_set(&oam[oam_idx], explosions[i].base.x, explosions[i].base.y, OBJ_SIZE(16,16)|OBJ_4BPP, 3);
        oam_set_attr3(&oam[oam_idx], OBJ_PAL(3));
        oam_idx++;
    }

    // Update OAM
    oam_update();

    // Scroll BG
    bg_set_scroll(BG0, 0, scroll_y);

    // UI Text
    text_cls();
    text_set_fgcolor(CLR_WHITE);
    text_set_bgcolor(CLR_TRANSP);
    text_putsf(2, 2, "SCORE: %06d", score);
    text_putsf(2, 14, "LIVES: %d", lives);
    if(paused) text_puts(SCREEN_W/2-32, SCREEN_H/2-8, "PAUSED");
    if(game_over) {
        text_puts(SCREEN_W/2-48, SCREEN_H/2-16, "GAME OVER");
        text_puts(SCREEN_W/2-40, SCREEN_H/2, "SELECT: RESTART");
    }
}

// ==================== VBLANK ====================
void vblank_handler() {
    u32 keys = keys_poll();
    if(KEY_DOWN(keys, KEY_START)) paused = !paused;
    if(KEY_DOWN(keys, KEY_SELECT)) {
        if(game_over || lives <= 0) {
            init_game();
        }
    }

    if(!paused) {
        update_game();
    }
    render_game();
}

// ==================== MAIN ====================
int main() {
    // GBA & Video init
    gba_init();
    vid_init(V_MODE0);

    // Background setup (simple scrolling tilemap)
    bg_init(BG0, 0, BG_MAP_SIZE(256,256), BG_4BPP, FALSE);
    bg_set_scroll(BG0, 0, 0);
    // Fill with simple star pattern tile
    u16* bg_map = bg_get_map(BG0);
    memset(bg_map, 0, BG_MAP_SIZE(256,256) * 2); // tile 0 (empty/transparent)
    // We'll just use BG0 offset for scrolling effect. Sprites handle gameplay.

    // Initialize game
    init_game();

    // VBlank interrupt
    VBlank_in(vblank_handler);

    // Main loop
    while(1) {
        wait_vbl_done();
    }
    return 0;
}
