// kirby_scroller.c - A Kirby Superstar-inspired 2D side scroller for GBA using TONC
// Compile with: gcc -o kirby_scroller.elf kirby_scroller.c -I/path/to/tonc/include -lm
// Then convert to .gba using objcopy

#include <tonc.h>
#include <stdlib.h>
#include <string.h>

// ============================================================================
// Constants and Macros
// ============================================================================

#define SCREEN_WIDTH    240
#define SCREEN_HEIGHT   160
#define MAP_WIDTH       1024  // Scrolling level width
#define MAP_HEIGHT      256
#define TILE_SIZE       16
#define GRAVITY         0.3f
#define JUMP_FORCE      -4.5f
#define MOVE_SPEED      1.5f
#define MAX_FALL_SPEED  6.0f
#define FLOAT_FORCE     -0.2f  // Kirby's float ability

// Object types
#define OBJ_PLAYER      0
#define OBJ_ENEMY       1
#define OBJ_STAR        2  // Star projectile
#define OBJ_COLLECTIBLE 3

// Enemy types
#define ENEMY_WADDLE_DEE  0
#define ENEMY_BLADE_KNIGHT 1

// ============================================================================
// Data Structures
// ============================================================================

typedef struct {
    float x, y;          // Position (sub-pixel precision)
    float vx, vy;        // Velocity
    int width, height;   // Bounding box
    int type;            // Object type
    int subtype;         // Enemy type, etc.
    bool active;         // Is object active?
    bool on_ground;      // Is on ground?
    bool facing_right;   // Facing direction
    int health;          // Health/hit points
    int anim_frame;      // Animation frame
    int anim_timer;      // Animation timer
    int invincible_timer; // Invincibility frames after hit
    
    // Kirby-specific
    bool floating;       // Is Kirby floating?
    bool has_star;       // Does Kirby have a star?
    float float_vy;      // Floating vertical velocity
    
    // Enemy-specific
    int patrol_dir;      // Patrol direction for enemies
    int patrol_timer;    // Patrol timer
} GameObject;

typedef struct {
    int tile_id;         // Tile ID in tileset
    bool solid;          // Is tile solid?
} MapTile;

// ============================================================================
// Global Variables
// ============================================================================

// Screen block entries for background
OBJ_ATTR obj_buffer[128];
OBJ_AFFINE *obj_aff_buffer = (OBJ_AFFINE*)obj_buffer;

// Sprites
TILE *tile_mem = (TILE*)MEM_VRAM;
TILE *tile8_mem = (TILE*)&MEM_VRAM[0x10000];

// Objects
GameObject player;
GameObject enemies[10];
GameObject stars[5];
GameObject collectibles[10];
int score = 0;
int lives = 3;

// Level map
MapTile level_map[MAP_WIDTH / TILE_SIZE][MAP_HEIGHT / TILE_SIZE];

// Camera
int camera_x = 0;
int camera_y = 0;

// Input
int prev_keys = 0;

// ============================================================================
// Simple Sprite Graphics (Generated programmatically)
// ============================================================================

// Create a simple Kirby-like sprite (16x16 pixels)
void draw_kirby_sprite(int tile_start, int frame) {
    u32 *tile_data = (u32*)&tile8_mem[tile_start];
    int x, y;
    
    // Simple circular character
    for(y = 0; y < 16; y++) {
        u32 row = 0;
        for(x = 0; x < 16; x++) {
            int dx = x - 8;
            int dy = y - 8;
            int dist = dx*dx + dy*dy;
            
            u8 color;
            if(dist < 36) { // Body
                color = 0xFF; // Pink (index 255)
            } else if(dist < 49) { // Outline
                color = 0x01; // Black
            } else {
                color = 0x00; // Transparent
            }
            
            // Pack 2 pixels per byte for 4bpp
            int shift = (x % 2) * 4;
            row |= (color & 0xF) << shift;
        }
        tile_data[y] = row;
    }
    
    // Add eyes based on frame
    if(frame == 0) {
        // Normal eyes
        tile_data[5] |= 0x00FF0000; // Left eye
        tile_data[5] |= 0x0000FF00; // Right eye
    } else {
        // Blinking/action eyes
        tile_data[5] |= 0x00AA0000;
        tile_data[5] |= 0x0000AA00;
    }
}

// Create a simple Waddle Dee sprite
void draw_waddle_dee_sprite(int tile_start, int frame) {
    u32 *tile_data = (u32*)&tile8_mem[tile_start];
    int x, y;
    
    for(y = 0; y < 16; y++) {
        u32 row = 0;
        for(x = 0; x < 16; x++) {
            int dx = x - 8;
            int dy = y - 10;
            int dist = dx*dx + dy*dy;
            
            u8 color;
            if(dist < 25) { // Body
                color = 0xDD; // Orange
            } else if(dist < 36 && y > 8) { // Feet
                color = 0xBB; // Darker orange
            } else if(dist < 36) { // Outline
                color = 0x01;
            } else {
                color = 0x00;
            }
            
            int shift = (x % 2) * 4;
            row |= (color & 0xF) << shift;
        }
        tile_data[y] = row;
    }
}

// Create a simple star sprite
void draw_star_sprite(int tile_start) {
    u32 *tile_data = (u32*)&tile8_mem[tile_start];
    int x, y;
    
    for(y = 0; y < 8; y++) {
        u32 row = 0;
        for(x = 0; x < 8; x++) {
            int dx = abs(x - 4);
            int dy = abs(y - 4);
            int dist = dx + dy;
            
            u8 color;
            if(dist < 4) {
                color = 0xEE; // Yellow
            } else if(dist < 5) {
                color = 0x01; // Outline
            } else {
                color = 0x00;
            }
            
            int shift = (x % 2) * 4;
            row |= (color & 0xF) << shift;
        }
        tile_data[y] = row;
    }
}

// ============================================================================
// Game Functions
// ============================================================================

void init_level() {
    // Generate a simple level with platforms
    int tile_x, tile_y;
    
    for(tile_y = 0; tile_y < MAP_HEIGHT / TILE_SIZE; tile_y++) {
        for(tile_x = 0; tile_x < MAP_WIDTH / TILE_SIZE; tile_x++) {
            level_map[tile_x][tile_y].tile_id = 0;
            level_map[tile_x][tile_y].solid = false;
            
            // Ground
            if(tile_y >= 14) {
                level_map[tile_x][tile_y].tile_id = 1;
                level_map[tile_x][tile_y].solid = true;
            }
            
            // Platforms
            if(tile_y == 10 && (tile_x % 8) < 4) {
                level_map[tile_x][tile_y].tile_id = 2;
                level_map[tile_x][tile_y].solid = true;
            }
            
            if(tile_y == 7 && (tile_x % 12) < 6 && tile_x > 10) {
                level_map[tile_x][tile_y].tile_id = 2;
                level_map[tile_x][tile_y].solid = true;
            }
        }
    }
}

void init_objects() {
    // Initialize player (Kirby)
    memset(&player, 0, sizeof(GameObject));
    player.x = 80;
    player.y = 100;
    player.width = 16;
    player.height = 16;
    player.type = OBJ_PLAYER;
    player.active = true;
    player.facing_right = true;
    player.health = 3;
    
    // Initialize enemies
    memset(enemies, 0, sizeof(enemies));
    
    // Waddle Dee enemies
    enemies[0].x = 200;
    enemies[0].y = 208;
    enemies[0].vx = -0.5f;
    enemies[0].width = 16;
    enemies[0].height = 16;
    enemies[0].type = OBJ_ENEMY;
    enemies[0].subtype = ENEMY_WADDLE_DEE;
    enemies[0].active = true;
    enemies[0].health = 1;
    enemies[0].patrol_dir = -1;
    
    enemies[1].x = 400;
    enemies[1].y = 144;
    enemies[1].vx = -0.5f;
    enemies[1].width = 16;
    enemies[1].height = 16;
    enemies[1].type = OBJ_ENEMY;
    enemies[1].subtype = ENEMY_WADDLE_DEE;
    enemies[1].active = true;
    enemies[1].health = 1;
    enemies[1].patrol_dir = 1;
    
    // Blade Knight
    enemies[2].x = 600;
    enemies[2].y = 208;
    enemies[2].vx = -0.3f;
    enemies[2].width = 16;
    enemies[2].height = 20;
    enemies[2].type = OBJ_ENEMY;
    enemies[2].subtype = ENEMY_BLADE_KNIGHT;
    enemies[2].active = true;
    enemies[2].health = 2;
    enemies[2].patrol_dir = -1;
    
    // Initialize stars
    memset(stars, 0, sizeof(stars));
    
    // Initialize collectibles
    memset(collectibles, 0, sizeof(collectibles));
    for(int i = 0; i < 5; i++) {
        collectibles[i].x = 150 + i * 120;
        collectibles[i].y = 150;
        collectibles[i].width = 8;
        collectibles[i].height = 8;
        collectibles[i].type = OBJ_COLLECTIBLE;
        collectibles[i].active = true;
    }
}

bool check_collision(float x1, float y1, int w1, int h1,
                    float x2, float y2, int w2, int h2) {
    return (x1 < x2 + w2 && x1 + w1 > x2 &&
            y1 < y2 + h2 && y1 + h1 > y2);
}

void handle_player_input() {
    int keys = key_curr_state();
    int keys_pressed = keys & ~prev_keys;
    
    // Left/Right movement
    if(key_is_down(KEY_LEFT)) {
        player.vx = -MOVE_SPEED;
        player.facing_right = false;
    } else if(key_is_down(KEY_RIGHT)) {
        player.vx = MOVE_SPEED;
        player.facing_right = true;
    } else {
        player.vx *= 0.8f; // Friction
        if(fabsf(player.vx) < 0.1f) player.vx = 0;
    }
    
    // Jump
    if(keys_pressed & KEY_A && player.on_ground) {
        player.vy = JUMP_FORCE;
        player.on_ground = false;
    }
    
    // Float (hold A in air)
    if(key_is_down(KEY_A) && !player.on_ground && player.vy > 0) {
        player.floating = true;
        player.vy += FLOAT_FORCE;
        if(player.vy < -1.0f) player.vy = -1.0f;
    } else {
        player.floating = false;
    }
    
    // Attack (B button)
    if(keys_pressed & KEY_B && player.has_star) {
        // Find inactive star slot
        for(int i = 0; i < 5; i++) {
            if(!stars[i].active) {
                stars[i].x = player.x + (player.facing_right ? 16 : -8);
                stars[i].y = player.y + 4;
                stars[i].vx = player.facing_right ? 4.0f : -4.0f;
                stars[i].vy = 0;
                stars[i].width = 8;
                stars[i].height = 8;
                stars[i].type = OBJ_STAR;
                stars[i].active = true;
                player.has_star = false;
                break;
            }
        }
    }
    
    // Inhale (R button) - Kirby's signature move
    if(keys_pressed & KEY_R) {
        // Check if any enemy is close enough to inhale
        for(int i = 0; i < 10; i++) {
            if(enemies[i].active) {
                float dx = player.x - enemies[i].x;
                float dy = player.y - enemies[i].y;
                float dist = sqrtf(dx*dx + dy*dy);
                
                if(dist < 30 && player.facing_right == (dx < 0)) {
                    // Inhale enemy and get star
                    enemies[i].active = false;
                    player.has_star = true;
                    break;
                }
            }
        }
    }
    
    prev_keys = keys;
}

void update_player() {
    // Apply gravity
    if(!player.floating) {
        player.vy += GRAVITY;
        if(player.vy > MAX_FALL_SPEED) player.vy = MAX_FALL_SPEED;
    }
    
    // Update position
    player.x += player.vx;
    player.y += player.vy;
    
    // Check map collisions
    int left_tile = (int)(player.x / TILE_SIZE);
    int right_tile = (int)((player.x + player.width - 1) / TILE_SIZE);
    int top_tile = (int)(player.y / TILE_SIZE);
    int bottom_tile = (int)((player.y + player.height - 1) / TILE_SIZE);
    
    // Boundary check
    if(left_tile < 0) { player.x = 0; player.vx = 0; }
    if(right_tile >= MAP_WIDTH / TILE_SIZE) { 
        player.x = MAP_WIDTH - player.width; 
        player.vx = 0; 
    }
    if(top_tile < 0) { player.y = 0; player.vy = 0; }
    
    // Ground collision
    player.on_ground = false;
    for(int tx = left_tile; tx <= right_tile; tx++) {
        if(tx < 0 || tx >= MAP_WIDTH / TILE_SIZE) continue;
        for(int ty = top_tile; ty <= bottom_tile; ty++) {
            if(ty < 0 || ty >= MAP_HEIGHT / TILE_SIZE) continue;
            if(level_map[tx][ty].solid) {
                // Check collision
                if(check_collision(player.x, player.y, player.width, player.height,
                                  tx * TILE_SIZE, ty * TILE_SIZE, TILE_SIZE, TILE_SIZE)) {
                    // Determine collision side
                    float overlap_left = (player.x + player.width) - tx * TILE_SIZE;
                    float overlap_right = (tx + 1) * TILE_SIZE - player.x;
                    float overlap_top = (player.y + player.height) - ty * TILE_SIZE;
                    float overlap_bottom = (ty + 1) * TILE_SIZE - player.y;
                    
                    // Find minimum overlap
                    if(overlap_left < overlap_right && overlap_left < overlap_top && 
                       overlap_left < overlap_bottom) {
                        player.x = tx * TILE_SIZE - player.width;
                        player.vx = 0;
                    } else if(overlap_right < overlap_left && overlap_right < overlap_top && 
                             overlap_right < overlap_bottom) {
                        player.x = (tx + 1) * TILE_SIZE;
                        player.vx = 0;
                    } else if(overlap_top < overlap_bottom && overlap_top < overlap_left && 
                             overlap_top < overlap_right) {
                        player.y = ty * TILE_SIZE - player.height;
                        player.vy = 0;
                        player.on_ground = true;
                    } else {
                        player.y = (ty + 1) * TILE_SIZE;
                        player.vy = 0;
                    }
                }
            }
        }
    }
    
    // Update animation
    player.anim_timer++;
    if(player.anim_timer > 10) {
        player.anim_timer = 0;
        player.anim_frame = !player.anim_frame;
    }
    
    // Invincibility timer
    if(player.invincible_timer > 0) {
        player.invincible_timer--;
    }
    
    // Update camera
    camera_x = player.x - SCREEN_WIDTH / 2;
    camera_y = player.y - SCREEN_HEIGHT / 2;
    
    // Clamp camera
    if(camera_x < 0) camera_x = 0;
    if(camera_x > MAP_WIDTH - SCREEN_WIDTH) camera_x = MAP_WIDTH - SCREEN_WIDTH;
    if(camera_y < 0) camera_y = 0;
    if(camera_y > MAP_HEIGHT - SCREEN_HEIGHT) camera_y = MAP_HEIGHT - SCREEN_HEIGHT;
}

void update_enemies() {
    for(int i = 0; i < 10; i++) {
        if(!enemies[i].active) continue;
        
        // Apply gravity
        enemies[i].vy += GRAVITY;
        if(enemies[i].vy > MAX_FALL_SPEED) enemies[i].vy = MAX_FALL_SPEED;
        
        // Patrol behavior
        if(enemies[i].subtype == ENEMY_WADDLE_DEE) {
            enemies[i].vx = 0.5f * enemies[i].patrol_dir;
            enemies[i].patrol_timer++;
            if(enemies[i].patrol_timer > 120) {
                enemies[i].patrol_timer = 0;
                enemies[i].patrol_dir *= -1;
            }
        } else if(enemies[i].subtype == ENEMY_BLADE_KNIGHT) {
            // Blade Knight - more aggressive, follows player slightly
            float dx = player.x - enemies[i].x;
            if(fabsf(dx) < 100) {
                enemies[i].vx = (dx > 0 ? 1.0f : -1.0f);
            } else {
                enemies[i].vx = 0.3f * enemies[i].patrol_dir;
            }
            enemies[i].patrol_timer++;
            if(enemies[i].patrol_timer > 180) {
                enemies[i].patrol_timer = 0;
                enemies[i].patrol_dir *= -1;
            }
        }
        
        // Update position
        enemies[i].x += enemies[i].vx;
        enemies[i].y += enemies[i].vy;
        
        // Simple ground collision
        int tx = (int)(enemies[i].x / TILE_SIZE);
        int ty = (int)((enemies[i].y + enemies[i].height) / TILE_SIZE);
        
        if(ty >= 0 && ty < MAP_HEIGHT / TILE_SIZE && 
           tx >= 0 && tx < MAP_WIDTH / TILE_SIZE) {
            if(level_map[tx][ty].solid) {
                enemies[i].y = ty * TILE_SIZE - enemies[i].height;
                enemies[i].vy = 0;
            }
        }
        
        // Check collision with player
        if(check_collision(player.x, player.y, player.width, player.height,
                          enemies[i].x, enemies[i].y, enemies[i].width, enemies[i].height)) {
            if(player.invincible_timer == 0) {
                // Player takes damage if not inhaling
                if(!key_is_down(KEY_R)) {
                    player.health--;
                    player.invincible_timer = 60;
                    
                    // Knockback
                    player.vy = -2.0f;
                    player.vx = (player.x > enemies[i].x ? 2.0f : -2.0f);
                    
                    if(player.health <= 0) {
                        lives--;
                        player.health = 3;
                        player.x = 80;
                        player.y = 100;
                    }
                }
            }
        }
        
        // Check collision with stars
        for(int j = 0; j < 5; j++) {
            if(stars[j].active && check_collision(
                stars[j].x, stars[j].y, stars[j].width, stars[j].height,
                enemies[i].x, enemies[i].y, enemies[i].width, enemies[i].height)) {
                enemies[i].health--;
                stars[j].active = false;
                
                if(enemies[i].health <= 0) {
                    enemies[i].active = false;
                    score += 100;
                }
            }
        }
    }
}

void update_stars() {
    for(int i = 0; i < 5; i++) {
        if(!stars[i].active) continue;
        
        stars[i].x += stars[i].vx;
        stars[i].y += stars[i].vy;
        
        // Deactivate if off screen
        if(stars[i].x < camera_x - 16 || stars[i].x > camera_x + SCREEN_WIDTH + 16 ||
           stars[i].y < camera_y - 16 || stars[i].y > camera_y + SCREEN_HEIGHT + 16) {
            stars[i].active = false;
        }
        
        // Check wall collision
        int tx = (int)(stars[i].x / TILE_SIZE);
        int ty = (int)(stars[i].y / TILE_SIZE);
        if(tx >= 0 && tx < MAP_WIDTH / TILE_SIZE && 
           ty >= 0 && ty < MAP_HEIGHT / TILE_SIZE) {
            if(level_map[tx][ty].solid) {
                stars[i].active = false;
            }
        }
    }
}

void update_collectibles() {
    for(int i = 0; i < 10; i++) {
        if(!collectibles[i].active) continue;
        
        // Check collision with player
        if(check_collision(player.x, player.y, player.width, player.height,
                          collectibles[i].x, collectibles[i].y, 
                          collectibles[i].width, collectibles[i].height)) {
            collectibles[i].active = false;
            score += 50;
            player.has_star = true; // Gain star ability
        }
    }
}

void draw_background() {
    // Clear background to sky blue
    memset(MEM_VRAM, 0x78, 0x9600); // Mode 3 background
    
    // Draw ground tiles
    for(int ty = 0; ty < MAP_HEIGHT / TILE_SIZE; ty++) {
        for(int tx = camera_x / TILE_SIZE; tx < (camera_x + SCREEN_WIDTH) / TILE_SIZE + 1; tx++) {
            if(tx < 0 || tx >= MAP_WIDTH / TILE_SIZE) continue;
            
            int screen_x = tx * TILE_SIZE - camera_x;
            int screen_y = ty * TILE_SIZE - camera_y;
            
            if(level_map[tx][ty].solid) {
                // Draw tile as colored rectangle
                u16 color = level_map[tx][ty].tile_id == 1 ? RGB15(10, 20, 5) : 
                                                             RGB15(15, 12, 8);
                for(int y = 0; y < TILE_SIZE && screen_y + y < SCREEN_HEIGHT; y++) {
                    if(screen_y + y < 0) continue;
                    u16 *dst = &((u16*)MEM_VRAM)[(screen_y + y) * SCREEN_WIDTH + screen_x];
                    for(int x = 0; x < TILE_SIZE && screen_x + x < SCREEN_WIDTH; x++) {
                        if(screen_x + x >= 0) {
                            dst[x] = color;
                        }
                    }
                }
            }
        }
    }
}

void draw_sprites() {
    oam_init(obj_buffer, 128);
    int obj_id = 0;
    
    // Draw player (Kirby)
    if(player.invincible_timer % 4 < 2) { // Flash when invincible
        obj_set_attr(&obj_buffer[obj_id],
            ATTR0_SQUARE | ATTR0_Y((int)(player.y - camera_y)),
            ATTR1_SIZE_16 | ATTR1_X((int)(player.x - camera_x)),
            ATTR2_PALBANK(0) | (player.floating ? 2 : player.anim_frame));
        if(!player.facing_right) {
            obj_buffer[obj_id].attr1 |= ATTR1_HFLIP;
        }
        obj_id++;
    }
    
    // Draw enemies
    for(int i = 0; i < 10; i++) {
        if(!enemies[i].active) continue;
        if(obj_id >= 128) break;
        
        int tile_offset = 4 + enemies[i].subtype * 4;
        obj_set_attr(&obj_buffer[obj_id],
            ATTR0_SQUARE | ATTR0_Y((int)(enemies[i].y - camera_y)),
            ATTR1_SIZE_16 | ATTR1_X((int)(enemies[i].x - camera_x)),
            ATTR2_PALBANK(0) | tile_offset);
        obj_id++;
    }
    
    // Draw stars
    for(int i = 0; i < 5; i++) {
        if(!stars[i].active) continue;
        if(obj_id >= 128) break;
        
        obj_set_attr(&obj_buffer[obj_id],
            ATTR0_SQUARE | ATTR0_Y((int)(stars[i].y - camera_y)),
            ATTR1_SIZE_8 | ATTR1_X((int)(stars[i].x - camera_x)),
            ATTR2_PALBANK(0) | 12);
        obj_id++;
    }
    
    // Draw collectibles
    for(int i = 0; i < 10; i++) {
        if(!collectibles[i].active) continue;
        if(obj_id >= 128) break;
        
        obj_set_attr(&obj_buffer[obj_id],
            ATTR0_SQUARE | ATTR0_Y((int)(collectibles[i].y - camera_y)),
            ATTR1_SIZE_8 | ATTR1_X((int)(collectibles[i].x - camera_x)),
            ATTR2_PALBANK(0) | 13);
        obj_id++;
    }
    
    // Hide remaining sprites
    for(int i = obj_id; i < 128; i++) {
        obj_buffer[i].attr0 = ATTR0_HIDE;
    }
    
    oam_copy(MEM_OAM, obj_buffer, 128);
}

void init_palette() {
    // Set up a simple palette
    u16 *pal = (u16*)MEM_PALETTE;
    
    // Transparent (color 0)
    pal[0] = 0;
    
    // Basic colors
    pal[1] = RGB15(0, 0, 0);     // Black
    pal[2] = RGB15(31, 31, 31);  // White
    pal[3] = RGB15(31, 0, 0);    // Red
    pal[4] = RGB15(0, 31, 0);    // Green
    pal[5] = RGB15(0, 0, 31);    // Blue
    pal[6] = RGB15(31, 31, 0);   // Yellow
    pal[7] = RGB15(31, 0, 31);   // Magenta
    pal[8] = RGB15(0, 31, 31);   // Cyan
    
    // Kirby colors
    pal[0xFF] = RGB15(31, 20, 25); // Pink
    pal[0xDD] = RGB15(31, 15, 5);  // Orange
    pal[0xEE] = RGB15(31, 31, 0);  // Yellow
}

void draw_hud() {
    // Simple text-based HUD using background tiles
    tte_init_chr4c(0, BG_CBB(0) | BG_SBB(31), 0xF000, CLR_WHITE, 0, NULL, NULL);
    
    // Position text
    tte_set_pos(8, 8);
    tte_write("HP:");
    tte_set_pos(32, 8);
    for(int i = 0; i < player.health; i++) {
        tte_write("#");
    }
    
    tte_set_pos(120, 8);
    tte_write("Score:");
    char score_str[10];
    siprintf(score_str, "%d", score);
    tte_set_pos(160, 8);
    tte_write(score_str);
    
    tte_set_pos(200, 8);
    tte_write("Lives:");
    char lives_str[5];
    siprintf(lives_str, "%d", lives);
    tte_set_pos(232, 8);
    tte_write(lives_str);
}

// ============================================================================
// Main Program
// ============================================================================

int main() {
    // Initialize GBA hardware
    irq_init(NULL);
    irq_enable(II_VBLANK);
    
    // Set video mode 0 (tiled with objects)
    REG_DISPCNT = DCNT_MODE0 | DCNT_BG0 | DCNT_BG1 | DCNT_OBJ | DCNT_OBJ_1D;
    
    // Initialize background control
    REG_BG0CNT = BG_CBB(0) | BG_SBB(30) | BG_4BPP | BG_REG_32x32;
    REG_BG1CNT = BG_CBB(1) | BG_SBB(29) | BG_4BPP | BG_REG_32x32;
    
    // Initialize palette
    init_palette();
    
    // Draw sprites to tile memory
    draw_kirby_sprite(0, 0);    // Kirby frame 0
    draw_kirby_sprite(4, 1);    // Kirby frame 1
    draw_waddle_dee_sprite(8, 0);  // Waddle Dee
    draw_waddle_dee_sprite(12, 1); // Waddle Dee frame 2
    draw_star_sprite(16);       // Star
    draw_star_sprite(18);       // Collectible star
    
    // Initialize game
    init_level();
    init_objects();
    
    // Initialize text engine for HUD
    tte_init_chr4c(1, BG_CBB(1) | BG_SBB(29), 0, CLR_WHITE, 0, NULL, NULL);
    
    // Game loop
    while(1) {
        VBlankIntrWait();
        key_poll();
        
        // Update game state
        handle_player_input();
        update_player();
        update_enemies();
        update_stars();
        update_collectibles();
        
        // Draw everything
        draw_background();
        draw_sprites();
        draw_hud();
        
        // Scroll background
        REG_BG0HOFS = camera_x;
        REG_BG0VOFS = camera_y;
        REG_BG1HOFS = camera_x / 2; // Parallax scrolling for background
        REG_BG1VOFS = camera_y / 2;
        
        // Game over check
        if(lives <= 0) {
            // Simple game over - reset
            lives = 3;
            score = 0;
            player.health = 3;
            player.x = 80;
            player.y = 100;
            init_objects();
        }
    }
    
    return 0;
}
