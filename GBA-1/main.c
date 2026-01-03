#include <tonc.h>

// === PHYSICS CONSTANTS ===
#define GRAVITY          0.15f
#define JUMP_FORCE       -3.0f
#define FLOAT_SPEED      -1.5f
#define MOVE_SPEED       2.0f
#define MAX_VEL          3.0f

// === GAME STRUCTURES ===
typedef struct {
    float x, y, vx, vy;
    int w, h;
    bool on_ground;
    bool is_floating;
    OBJ_ATTR *obj;
} Player;

typedef struct {
    float x, y, vx;
    int w, h;
    bool active;
    OBJ_ATTR *obj;
} Enemy;

// === GLOBAL STATE ===
Player g_player;
Enemy g_enemies[3];
int g_camera_x = 0;

// === LEVEL MAP (32x64) ===
const u8 g_level_map[32][64] = {
    // Sky rows
    {[0 ... 31] = 0}, {[0 ... 31] = 0}, {[0 ... 31] = 0}, {[0 ... 31] = 0},
    {[0 ... 31] = 0}, {[0 ... 31] = 0}, {[0 ... 31] = 0}, {[0 ... 31] = 0},
    {[0 ... 31] = 0}, {[0 ... 31] = 0}, {[0 ... 31] = 0}, {[0 ... 31] = 0},
    {[0 ... 31] = 0}, {[0 ... 31] = 0}, {[0 ... 31] = 0}, {[0 ... 31] = 0},
    {[0 ... 31] = 0}, {[0 ... 31] = 0}, {[0 ... 31] = 0}, {[0 ... 31] = 0},
    {[0 ... 31] = 0}, {[0 ... 31] = 0},
    // Platforms
    {0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1,1,1,1,1,0,0,0,0,0,0,0,0,0,0,0},
    {[0 ... 31] = 0}, {[0 ... 31] = 0}, {[0 ... 31] = 0}, {[0 ... 31] = 0},
    {[0 ... 31] = 0}, {[0 ... 31] = 0}, {[0 ... 31] = 0},
    // Ground
    {1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1}
};

// === GRAPHICS INITIALIZATION ===
void init_gfx() {
    // Palettes
    pal_bg_mem[0] = CLR_BLACK;
    pal_bg_mem[1] = CLR_GREEN;
    pal_obj_mem[4] = CLR_PINK;
    
    // Background tiles
    memset(&tile_mem[0][0], 0, sizeof(TILE)); // Empty tile
    TILE *ground = (TILE*)&tile_mem[0][1];
    for(int i = 0; i < 8; i++) ground->data[i] = 0x11111111;
    
    // Sprite tiles (16x16 = 4 tiles each)
    TILE *kirby = (TILE*)&tile_mem[4][0];
    TILE *enemy = (TILE*)&tile_mem[4][4];
    for(int i = 0; i < 32; i++) {
        kirby->data[i] = 0x44444444;  // Pink
        enemy->data[i] = 0x22222222;  // Red
    }
    
    // Background setup
    REG_BG0CNT = BG_CBB(0) | BG_SBB(30) | BG_4BPP | BG_REG_32x32;
    for(int y = 0; y < 32; y++) {
        for(int x = 0; x < 32; x++) {
            se_mem[30][y*32 + x] = g_level_map[y][x];
        }
    }
    
    // Sprite setup
    oam_init(obj_mem, 128);
    obj_set_attr(&obj_mem[0], ATTR0_SQUARE|ATTR0_16BPP, ATTR1_SIZE_16, ATTR2_ID(0));
    for(int i = 0; i < 3; i++) {
        obj_set_attr(&obj_mem[1+i], ATTR0_SQUARE|ATTR0_16BPP, ATTR1_SIZE_16, ATTR2_ID(4));
    }
}

// === GAME INITIALIZATION ===
void init_game() {
    g_player.x = 32; g_player.y = 64;
    g_player.vx = g_player.vy = 0;
    g_player.w = g_player.h = PLAYER_SIZE;
    g_player.on_ground = false;
    g_player.obj = &obj_mem[0];
    
    for(int i = 0; i < 3; i++) {
        g_enemies[i].x = 120 + i * 80;
        g_enemies[i].y = 120;
        g_enemies[i].vx = -0.5f;
        g_enemies[i].w = g_enemies[i].h = ENEMY_SIZE;
        g_enemies[i].active = true;
        g_enemies[i].obj = &obj_mem[1+i];
    }
}

// === INPUT HANDLING ===
void update_input() {
    key_poll();
    u16 held = key_is_down(KEY_ANY);
    
    if(held & KEY_LEFT) g_player.vx = -MOVE_SPEED;
    else if(held & KEY_RIGHT) g_player.vx = MOVE_SPEED;
    else g_player.vx *= 0.85f;
    
    if(key_hit(KEY_A) && g_player.on_ground) {
        g_player.vy = JUMP_FORCE;
    }
    
    if((held & KEY_A) && !g_player.on_ground && g_player.vy > FLOAT_SPEED) {
        g_player.is_floating = true;
        g_player.vy = FLOAT_SPEED;
    } else {
        g_player.is_floating = false;
    }
}

// === PHYSICS UPDATE ===
void update_physics() {
    if(!g_player.is_floating && !g_player.on_ground) {
        g_player.vy += GRAVITY;
        if(g_player.vy > MAX_VEL) g_player.vy = MAX_VEL;
    }
    
    g_player.x += g_player.vx;
    g_player.y += g_player.vy;
    
    for(int i = 0; i < 3; i++) {
        if(g_enemies[i].active) {
            g_enemies[i].x += g_enemies[i].vx;
            if(g_enemies[i].x < 50 || g_enemies[i].x > 400) {
                g_enemies[i].vx = -g_enemies[i].vx;
            }
        }
    }
    
    g_camera_x = (int)g_player.x - 120;
    if(g_camera_x < 0) g_camera_x = 0;
}

// === COLLISION DETECTION ===
void check_collisions() {
    if(g_player.y > 160) {
        g_player.x = 32; g_player.y = 64;
        g_player.vx = g_player.vy = 0;
        return;
    }
    
    int left = (int)g_player.x / TILE_SIZE;
    int right = (int)(g_player.x + g_player.w - 1) / TILE_SIZE;
    int top = (int)g_player.y / TILE_SIZE;
    int bottom = (int)(g_player.y + g_player.h - 1) / TILE_SIZE;
    
    g_player.on_ground = false;
    
    for(int y = top; y <= bottom; y++) {
        for(int x = left; x <= right; x++) {
            if(y >= 0 && y < MAP_HEIGHT && x >= 0 && x < MAP_WIDTH) {
                if(g_level_map[y][x] == 1) {
                    if(g_player.vy > 0 && g_player.y < y * TILE_SIZE) {
                        g_player.y = y * TILE_SIZE - g_player.h;
                        g_player.vy = 0;
                        g_player.on_ground = true;
                    }
                }
            }
        }
    }
    
    for(int i = 0; i < 3; i++) {
        if(g_enemies[i].active) {
            if(g_player.x < g_enemies[i].x + g_enemies[i].w &&
               g_player.x + g_player.w > g_enemies[i].x &&
               g_player.y < g_enemies[i].y + g_enemies[i].h &&
               g_player.y + g_player.h > g_enemies[i].y) {
                g_player.x = 32; g_player.y = 64;
                g_player.vx = g_player.vy = 0;
            }
        }
    }
}

// === RENDERING ===
void render() {
    REG_BG0HOFS = g_camera_x;
    obj_set_pos(g_player.obj, (int)g_player.x - g_camera_x, (int)g_player.y);
    
    for(int i = 0; i < 3; i++) {
        if(g_enemies[i].active) {
            obj_set_pos(g_enemies[i].obj, (int)g_enemies[i].x - g_camera_x, (int)g_enemies[i].y);
        }
    }
}

// === MAIN ===
int main() {
    irq_init(NULL);
    irq_add(II_VBLANK, NULL);
    REG_DISPCNT = DCNT_OBJ | DCNT_OBJ_1D | DCNT_BG0;
    
    init_gfx();
    init_game();
    
    while(1) {
        VBlankIntrWait();
        update_input();
        update_physics();
        check_collisions();
        render();
    }
    
    return 0;
}
