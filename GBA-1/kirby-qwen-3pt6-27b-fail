// kirby_gba_prototype.c
// Single-file GBA side-scroller prototype using tonc
// Compile with: pbp -o KirbyPrototype.gba kirby_gba_prototype.c -ltonc -ltgccore

#include <tonc.h>
#include <stdlib.h>

// ============================================================================
// CONSTANTS
// ============================================================================
#define SCREEN_W      240
#define SCREEN_H      160
#define WORLD_W       800
#define WORLD_H       320
#define GRAVITY       0.45f
#define JUMP_FORCE    -8.5f
#define MOVE_ACCEL    0.6f
#define MOVE_MAX      3.5f
#define FRICTION      0.85f
#define BOUNCE_FORCE  -5.0f

// ============================================================================
// STRUCTS & GLOBAL STATE
// ============================================================================
typedef struct {
    float x, y;
    float vx, vy;
    bool  grounded;
} Player;

typedef struct {
    int x, y, w, h;
    u16 color;
} Platform;

typedef struct {
    float x, y;
    float vx;
    bool  active;
} Enemy;

Player   player;
Platform platforms[] = {
    {0,    130, 800, 30,  COLOR_RGB(31,31,31)}, // Main ground
    {80,   90,  80,  10,  COLOR_RGB(20,10,0)},  // Platform 1
    {220,  60,  60,  10,  COLOR_RGB(20,10,0)},  // Platform 2
    {360,  100, 100, 10,  COLOR_RGB(20,10,0)},  // Platform 3
    {520,  50,  90,  10,  COLOR_RGB(20,10,0)},  // Platform 4
    {680,  80,  80,  10,  COLOR_RGB(20,10,0)}   // Platform 5
};
#define NUM_PLATFORMS (sizeof(platforms)/sizeof(Platform))

Enemy enemies[] = {
    {150, 110, 1.0, true},
    {300,  40, -0.8, true},
    {450,  80, 1.2, true},
    {600, 110, -1.0, true}
};
#define NUM_ENEMIES (sizeof(enemies)/sizeof(Enemy))

int cam_x = 0, cam_y = 0;

// ============================================================================
// GAME FUNCTIONS
// ============================================================================
void init_game() {
    gba_init(VRAM_INIT_DEFAULT);
    VRAM_SET(VRAM_A_MAIN); // Reserve VRAM for bitmap BG0
    bgInit(0, BgType_Bmp8, BgTileMap8(0,0,3,0), BgPal16(0), BgPri0);
    
    player = (Player){50.0f, 100.0f, 0.0f, 0.0f, false};
    bgClear(0);
}

void handle_input() {
    u16 keys = keysDown();
    u16 held = keysHeld();

    // Movement acceleration
    if (held & KEY_LEFT)  player.vx -= MOVE_ACCEL;
    if (held & KEY_RIGHT) player.vx += MOVE_ACCEL;

    // Jump
    if ((keys & KEY_A) && player.grounded) {
        player.vy = JUMP_FORCE;
        player.grounded = false;
    }
}

void update_physics() {
    // Horizontal movement
    player.vx *= FRICTION;
    if (player.vx > MOVE_MAX)  player.vx = MOVE_MAX;
    if (player.vx < -MOVE_MAX) player.vx = -MOVE_MAX;
    if (abs(player.vx) < 0.1f) player.vx = 0.0f;

    player.x += player.vx;

    // World bounds (X)
    if (player.x < 0)           { player.x = 0; player.vx = 0; }
    if (player.x > WORLD_W - 16){ player.x = WORLD_W - 16; player.vx = 0; }

    // Vertical movement & gravity
    player.vy += GRAVITY;
    player.y += player.vy;

    // World bounds (Y) - respawn if falls off
    if (player.y > WORLD_H) {
        player.x = 50.0f;
        player.y = 100.0f;
        player.vx = player.vy = 0.0f;
    }
}

void check_collisions() {
    player.grounded = false;

    // Platform collision (Separating Axis Theorem simplified)
    for (int i = 0; i < NUM_PLATFORMS; i++) {
        Platform *p = &platforms[i];
        if (player.x < p->x + p->w && player.x + 16 > p->x &&
            player.y < p->y + p->h && player.y + 16 > p->y) {
            
            float overlapX = 0, overlapY = 0;
            if (player.x + 8 < p->x + p->w/2) overlapX = p->x - (player.x + 16);
            else                              overlapX = (p->x + p->w) - player.x;
            
            if (player.y + 8 < p->y + p->h/2) overlapY = p->y - (player.y + 16);
            else                              overlapY = (p->y + p->h) - player.y;

            if (overlapX < overlapY) {
                // Horizontal collision
                player.x += overlapX < 0 ? -overlapX : overlapX;
                player.vx = 0;
            } else {
                // Vertical collision
                if (overlapY < 0) {
                    // Landing on top
                    player.y -= overlapY;
                    player.vy = 0;
                    player.grounded = true;
                } else {
                    // Hitting head
                    player.y += overlapY;
                    player.vy = 0;
                }
            }
        }
    }

    // Enemy interaction
    for (int i = 0; i < NUM_ENEMIES; i++) {
        Enemy *e = &enemies[i];
        if (!e->active) continue;

        // Simple patrol
        e->x += e->vx;
        if (e->x <= 0 || e->x >= WORLD_W - 16) e->vx = -e->vx;

        // Player-Enemy collision
        if (player.x < e->x + 16 && player.x + 16 > e->x &&
            player.y < e->y + 16 && player.y + 16 > e->y) {
            
            // Stomp check: falling onto enemy
            if (player.vy > 0 && (player.y + 16 - player.vy) <= e->y + 5) {
                e->active = false;
                player.vy = BOUNCE_FORCE;
            } else {
                // Hurt by enemy -> respawn
                player.x = 50.0f; player.y = 100.0f;
                player.vx = player.vy = 0.0f;
            }
        }
    }
}

void update_camera() {
    // Smooth follow
    int target_x = (int)player.x - SCREEN_W/2 + 8;
    int target_y = (int)player.y - SCREEN_H/2 + 8;
    
    cam_x += (target_x - cam_x) * 0.12f;
    cam_y += (target_y - cam_y) * 0.12f;

    // Clamp to world bounds
    if (cam_x < 0)                  cam_x = 0;
    if (cam_x > WORLD_W - SCREEN_W) cam_x = WORLD_W - SCREEN_W;
    if (cam_y < 0)                  cam_y = 0;
    if (cam_y > 80)                 cam_y = 80; // Keep ground mostly visible
}

void render_game() {
    bgClear(0);

    // Sky background
    drawRect(0, 0, SCREEN_W, SCREEN_H, COLOR_RGB(10, 18, 28));

    // Decorative background elements (parallax-ish)
    for (int i = 0; i < 5; i++) {
        int bx = (i * 180 - cam_x * 0.3f) % (WORLD_W + 200) - 100;
        drawRect(bx, 40 - cam_y * 0.2f, 40, 120, COLOR_RGB(15, 25, 40));
    }

    // Platforms
    for (int i = 0; i < NUM_PLATFORMS; i++) {
        int sx = platforms[i].x - cam_x;
        int sy = platforms[i].y - cam_y;
        if (sx + platforms[i].w > 0 && sx < SCREEN_W &&
            sy + platforms[i].h > 0 && sy < SCREEN_H) {
            drawRect(sx, sy, platforms[i].w, platforms[i].h, platforms[i].color);
        }
    }

    // Enemies
    for (int i = 0; i < NUM_ENEMIES; i++) {
        Enemy *e = &enemies[i];
        if (!e->active) continue;
        int sx = (int)e->x - cam_x;
        int sy = (int)e->y - cam_y;
        if (sx + 16 > 0 && sx < SCREEN_W && sy + 16 > 0 && sy < SCREEN_H) {
            drawRect(sx, sy, 16, 16, COLOR_RGB(31, 0, 0)); // Body
            drawCircle(sx + 5, sy + 6, 2, COLOR_RGB(31, 31, 31)); // Eyes
            drawCircle(sx + 11, sy + 6, 2, COLOR_RGB(31, 31, 31));
        }
    }

    // Player (Kirby-ish round character)
    int px = (int)player.x - cam_x;
    int py = (int)player.y - cam_y;
    drawCircle(px + 8, py + 8, 8, COLOR_RGB(31, 0, 31)); // Magenta body
    drawCircle(px + 5, py + 6, 2, COLOR_RGB(31, 31, 31)); // Left eye
    drawCircle(px + 11, py + 6, 2, COLOR_RGB(31, 31, 31)); // Right eye

    // Double buffer update
    bgUpdate(0);
}

// ============================================================================
// MAIN
// ============================================================================
int main() {
    init_game();

    while (1) {
        handle_input();
        update_physics();
        check_collisions();
        update_camera();
        render_game();
        frameDelay(); // Wait for VBlank
    }
    return 0;
}
