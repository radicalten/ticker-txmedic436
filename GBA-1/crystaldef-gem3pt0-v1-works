#include <tonc.h>
#include <stdlib.h>

// -------------------------------------------------------------------------
// CONSTANTS & CONFIG
// -------------------------------------------------------------------------
#define MAP_WIDTH  15
#define MAP_HEIGHT 10
#define TILE_SIZE  16

#define MAX_ENEMIES 20
#define MAX_TOWERS  50
#define MAX_PROJECTILES 20

// Colors (BGR 555)
#define CLR_GRASS   RGB15(5, 20, 5)
#define CLR_PATH    RGB15(20, 15, 10)
#define CLR_CURSOR  RGB15(31, 31, 31)
#define CLR_ENEMY   RGB15(31, 0, 0)
#define CLR_T_SOLDIER RGB15(0, 0, 31)   // Blue Tower
#define CLR_T_ARCHER  RGB15(31, 31, 0)  // Yellow Tower
#define CLR_PROJ    RGB15(31, 31, 31)
#define CLR_UI      RGB15(10, 10, 10)

// Tower Stats
#define TYPE_SOLDIER 0
#define TYPE_ARCHER  1

#define COST_SOLDIER 20
#define COST_ARCHER  50

#define RANGE_SOLDIER (32 * 32) // Squared distance
#define RANGE_ARCHER  (64 * 64)

#define DMG_SOLDIER   2
#define DMG_ARCHER    1

#define COOLDOWN_SOLDIER 30 // Frames
#define COOLDOWN_ARCHER  45

// -------------------------------------------------------------------------
// STRUCTS
// -------------------------------------------------------------------------
typedef struct {
    int x, y;     // Pixel coordinates (fixed point not needed for simple demo)
    int hp;
    int max_hp;
    int active;
    int path_idx; // Current node in path
    int dist_traveled; // For progress sorting
    int slow_timer;
} Enemy;

typedef struct {
    int grid_x, grid_y;
    int type;
    int cooldown_timer;
    int range_sq;
    int active;
} Tower;

typedef struct {
    int x, y;
    int target_id; // Index in enemy array
    int speed;
    int damage;
    int active;
} Projectile;

typedef struct {
    int x, y;
} Point;

// -------------------------------------------------------------------------
// GLOBALS
// -------------------------------------------------------------------------

// Simple S-shape path (Grid coordinates)
const Point path_nodes[] = {
    {0, 2}, {4, 2}, {4, 7}, {10, 7}, {10, 2}, {14, 2}, {14, 5}
};
#define PATH_NODE_COUNT 7

// Map Grid (0 = Build, 1 = Path)
u8 map_grid[MAP_WIDTH][MAP_HEIGHT];

Enemy enemies[MAX_ENEMIES];
Tower towers[MAX_TOWERS];
Projectile projectiles[MAX_PROJECTILES];

int money = 100;
int lives = 10;
int wave = 1;
int spawn_timer = 0;
int enemies_to_spawn = 5;
int frame_count = 0;

int cursor_x = 7;
int cursor_y = 5;

// -------------------------------------------------------------------------
// HELPER FUNCTIONS
// -------------------------------------------------------------------------

// Check if a point is inside the screen
INLINE int in_bounds(int x, int y) {
    return (x >= 0 && x < 240 && y >= 0 && y < 160);
}

// Draw a filled rectangle in Mode 3
void draw_rect(int x, int y, int w, int h, u16 color) {
    for (int iy = 0; iy < h; iy++) {
        for (int ix = 0; ix < w; ix++) {
            if (in_bounds(x + ix, y + iy))
                m3_plot(x + ix, y + iy, color);
        }
    }
}

// Simple circle approximation for units
void draw_circle(int x, int y, int r, u16 color) {
    draw_rect(x - r, y - r, r*2, r*2, color); // Keeping it boxy for speed in Mode 3
}

void init_game() {
    // Reset Map
    for(int x=0; x<MAP_WIDTH; x++) {
        for(int y=0; y<MAP_HEIGHT; y++) {
            map_grid[x][y] = 0; // Grass
        }
    }

    // Mark Path on Grid
    for (int i = 0; i < PATH_NODE_COUNT - 1; i++) {
        Point p1 = path_nodes[i];
        Point p2 = path_nodes[i+1];
        
        // Simple linear interpolation to mark grid
        int cx = p1.x;
        int cy = p1.y;
        while(cx != p2.x || cy != p2.y) {
            map_grid[cx][cy] = 1;
            if(cx < p2.x) cx++;
            else if(cx > p2.x) cx--;
            else if(cy < p2.y) cy++;
            else if(cy > p2.y) cy--;
        }
        map_grid[p2.x][p2.y] = 1;
    }

    // Clear Entities
    for(int i=0; i<MAX_ENEMIES; i++) enemies[i].active = 0;
    for(int i=0; i<MAX_TOWERS; i++) towers[i].active = 0;
    for(int i=0; i<MAX_PROJECTILES; i++) projectiles[i].active = 0;
}

void spawn_enemy() {
    for(int i=0; i<MAX_ENEMIES; i++) {
        if(!enemies[i].active) {
            enemies[i].active = 1;
            enemies[i].path_idx = 0;
            enemies[i].x = path_nodes[0].x * TILE_SIZE + 8;
            enemies[i].y = path_nodes[0].y * TILE_SIZE + 8;
            enemies[i].max_hp = 5 + (wave * 2);
            enemies[i].hp = enemies[i].max_hp;
            enemies[i].dist_traveled = 0;
            return;
        }
    }
}

void update_enemies() {
    for(int i=0; i<MAX_ENEMIES; i++) {
        if(!enemies[i].active) continue;

        Enemy *e = &enemies[i];
        
        // Target Node
        if (e->path_idx >= PATH_NODE_COUNT - 1) {
            // Reached end
            e->active = 0;
            lives--;
            continue;
        }

        Point target = path_nodes[e->path_idx + 1];
        int tx = target.x * TILE_SIZE + 8; // Center of tile
        int ty = target.y * TILE_SIZE + 8;

        int speed = 1; 

        // Move towards target
        if(e->x < tx) e->x += speed;
        else if(e->x > tx) e->x -= speed;
        
        if(e->y < ty) e->y += speed;
        else if(e->y > ty) e->y -= speed;

        e->dist_traveled += speed;

        // Check if reached node
        if(abs(e->x - tx) < 2 && abs(e->y - ty) < 2) {
            e->path_idx++;
        }
    }
}

void fire_projectile(int tx, int ty, int target_idx, int type) {
    for(int i=0; i<MAX_PROJECTILES; i++) {
        if(!projectiles[i].active) {
            projectiles[i].active = 1;
            projectiles[i].x = tx;
            projectiles[i].y = ty;
            projectiles[i].target_id = target_idx;
            projectiles[i].speed = 4;
            projectiles[i].damage = (type == TYPE_SOLDIER) ? DMG_SOLDIER : DMG_ARCHER;
            return;
        }
    }
}

void update_towers() {
    for(int i=0; i<MAX_TOWERS; i++) {
        if(!towers[i].active) continue;
        Tower *t = &towers[i];

        if(t->cooldown_timer > 0) {
            t->cooldown_timer--;
            continue;
        }

        // Find closest enemy
        int target_id = -1;
        int min_dist = 999999;

        for(int e=0; e<MAX_ENEMIES; e++) {
            if(!enemies[e].active) continue;

            int dx = enemies[e].x - (t->grid_x * TILE_SIZE + 8);
            int dy = enemies[e].y - (t->grid_y * TILE_SIZE + 8);
            int dist_sq = dx*dx + dy*dy;

            if(dist_sq <= t->range_sq) {
                // Simple targeting: Closest
                if(dist_sq < min_dist) {
                    min_dist = dist_sq;
                    target_id = e;
                }
            }
        }

        if(target_id != -1) {
            // Fire
            fire_projectile(t->grid_x * TILE_SIZE + 8, t->grid_y * TILE_SIZE + 8, target_id, t->type);
            t->cooldown_timer = (t->type == TYPE_SOLDIER) ? COOLDOWN_SOLDIER : COOLDOWN_ARCHER;
        }
    }
}

void update_projectiles() {
    for(int i=0; i<MAX_PROJECTILES; i++) {
        if(!projectiles[i].active) continue;
        Projectile *p = &projectiles[i];
        
        if(!enemies[p->target_id].active) {
            p->active = 0;
            continue;
        }

        Enemy *target = &enemies[p->target_id];
        
        // Homing logic
        int dx = target->x - p->x;
        int dy = target->y - p->y;
        
        // Normalize vector (roughly)
        if(dx > 0) p->x += p->speed;
        if(dx < 0) p->x -= p->speed;
        if(dy > 0) p->y += p->speed;
        if(dy < 0) p->y -= p->speed;

        // Collision
        if(abs(dx) < 4 && abs(dy) < 4) {
            target->hp -= p->damage;
            p->active = 0;
            if(target->hp <= 0) {
                target->active = 0;
                money += 5;
            }
        }
    }
}

void draw_game() {
    // 1. Draw Map (Optimization: In a real game, don't redraw static map every frame in Mode 3)
    // For this demo, we redraw everything to keep code simple and self-correcting.
    
    // Draw background
    for(int x=0; x<MAP_WIDTH; x++) {
        for(int y=0; y<MAP_HEIGHT; y++) {
            u16 color = (map_grid[x][y] == 1) ? CLR_PATH : CLR_GRASS;
            draw_rect(x*TILE_SIZE, y*TILE_SIZE, TILE_SIZE, TILE_SIZE, color);
            
            // Draw grid lines lightly
            m3_plot(x*TILE_SIZE, y*TILE_SIZE, RGB15(0,0,0));
        }
    }

    // 2. Draw Towers
    for(int i=0; i<MAX_TOWERS; i++) {
        if(towers[i].active) {
            u16 c = (towers[i].type == TYPE_SOLDIER) ? CLR_T_SOLDIER : CLR_T_ARCHER;
            draw_rect(towers[i].grid_x*TILE_SIZE + 2, towers[i].grid_y*TILE_SIZE + 2, 12, 12, c);
        }
    }

    // 3. Draw Enemies
    for(int i=0; i<MAX_ENEMIES; i++) {
        if(enemies[i].active) {
            draw_circle(enemies[i].x, enemies[i].y, 4, CLR_ENEMY);
            // HP Bar
            int hp_width = (enemies[i].hp * 10) / enemies[i].max_hp;
            if(hp_width < 0) hp_width = 0;
            draw_rect(enemies[i].x - 5, enemies[i].y - 6, 10, 2, RGB15(31,0,0));
            draw_rect(enemies[i].x - 5, enemies[i].y - 6, hp_width, 2, RGB15(0,31,0));
        }
    }

    // 4. Draw Projectiles
    for(int i=0; i<MAX_PROJECTILES; i++) {
        if(projectiles[i].active) {
            m3_plot(projectiles[i].x, projectiles[i].y, CLR_PROJ);
            m3_plot(projectiles[i].x+1, projectiles[i].y, CLR_PROJ);
            m3_plot(projectiles[i].x, projectiles[i].y+1, CLR_PROJ);
        }
    }

    // 5. Draw Cursor
    int cx = cursor_x * TILE_SIZE;
    int cy = cursor_y * TILE_SIZE;
    // Draw hollow box
    for(int i=0; i<16; i++) {
        m3_plot(cx + i, cy, CLR_CURSOR);
        m3_plot(cx + i, cy + 15, CLR_CURSOR);
        m3_plot(cx, cy + i, CLR_CURSOR);
        m3_plot(cx + 15, cy + i, CLR_CURSOR);
    }

    // 6. Draw UI (Text is hard in Mode 3 without fonts, using colored blocks for status)
    // Bottom Bar
    draw_rect(0, 150, 240, 10, CLR_UI);
    
    // Lives (Red Blocks)
    for(int i=0; i<lives; i++) {
        draw_rect(2 + (i*4), 152, 3, 6, RGB15(31,0,0));
    }
    
    // Money (Gold Blocks)
    int money_blocks = money / 10;
    if(money_blocks > 20) money_blocks = 20;
    for(int i=0; i<money_blocks; i++) {
        draw_rect(238 - (i*4), 152, 3, 6, RGB15(31,31,0));
    }
}

// -------------------------------------------------------------------------
// MAIN LOOP
// -------------------------------------------------------------------------
int main() {
    // Initialize graphics: Mode 3 (240x160 Bitmap), BG2 enabled
    REG_DISPCNT = DCNT_MODE3 | DCNT_BG2;

    init_game();

    while(1) {
        vid_vsync(); // Wait for VBlank to reduce tearing
        key_poll();

        // 1. Input Handling
        if(key_hit(KEY_RIGHT)) if(cursor_x < MAP_WIDTH-1) cursor_x++;
        if(key_hit(KEY_LEFT))  if(cursor_x > 0) cursor_x--;
        if(key_hit(KEY_UP))    if(cursor_y > 0) cursor_y--;
        if(key_hit(KEY_DOWN))  if(cursor_y < MAP_HEIGHT-1) cursor_y++;

        // Place Tower A (Soldier)
        if(key_hit(KEY_A)) {
            if(map_grid[cursor_x][cursor_y] == 0 && money >= COST_SOLDIER) {
                // Check if occupied
                int occupied = 0;
                for(int i=0; i<MAX_TOWERS; i++) {
                    if(towers[i].active && towers[i].grid_x == cursor_x && towers[i].grid_y == cursor_y) occupied = 1;
                }
                
                if(!occupied) {
                    for(int i=0; i<MAX_TOWERS; i++) {
                        if(!towers[i].active) {
                            towers[i].active = 1;
                            towers[i].grid_x = cursor_x;
                            towers[i].grid_y = cursor_y;
                            towers[i].type = TYPE_SOLDIER;
                            towers[i].range_sq = RANGE_SOLDIER;
                            towers[i].cooldown_timer = 0;
                            money -= COST_SOLDIER;
                            break;
                        }
                    }
                }
            }
        }

        // Place Tower B (Archer) - Use B Button
        if(key_hit(KEY_B)) {
            if(map_grid[cursor_x][cursor_y] == 0 && money >= COST_ARCHER) {
                int occupied = 0;
                for(int i=0; i<MAX_TOWERS; i++) {
                    if(towers[i].active && towers[i].grid_x == cursor_x && towers[i].grid_y == cursor_y) occupied = 1;
                }
                
                if(!occupied) {
                    for(int i=0; i<MAX_TOWERS; i++) {
                        if(!towers[i].active) {
                            towers[i].active = 1;
                            towers[i].grid_x = cursor_x;
                            towers[i].grid_y = cursor_y;
                            towers[i].type = TYPE_ARCHER;
                            towers[i].range_sq = RANGE_ARCHER;
                            towers[i].cooldown_timer = 0;
                            money -= COST_ARCHER;
                            break;
                        }
                    }
                }
            }
        }

        // Reset
        if(key_hit(KEY_START)) {
            money = 100;
            lives = 10;
            init_game();
        }

        // 2. Game Logic
        if(lives > 0) {
            spawn_timer++;
            if(spawn_timer > 60) {
                spawn_timer = 0;
                if(enemies_to_spawn > 0) {
                    spawn_enemy();
                    enemies_to_spawn--;
                } else {
                    // Check if wave clear
                    int active_enemies = 0;
                    for(int i=0; i<MAX_ENEMIES; i++) if(enemies[i].active) active_enemies++;
                    
                    if(active_enemies == 0) {
                        wave++;
                        enemies_to_spawn = 5 + wave;
                        money += 20; // Wave clear bonus
                    }
                }
            }

            update_enemies();
            update_towers();
            update_projectiles();
        }

        // 3. Drawing
        draw_game();
        
        // Game Over Text (Crude blocks)
        if(lives <= 0) {
            draw_rect(100, 70, 40, 20, RGB15(31, 0, 0));
        }

        frame_count++;
    }

    return 0;
}
