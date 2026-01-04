// crystal_defense.c
// Minimal tower-defense style demo for GBA using tonc (Mode 3).
// No external assets; uses colored rectangles only.

#include <tonc.h>

// -----------------------------------------------------------------------------
// Basic constants
// -----------------------------------------------------------------------------

#define SCREEN_WIDTH    240
#define SCREEN_HEIGHT   160

#define TILE_SIZE       16
#define GRID_W          (SCREEN_WIDTH / TILE_SIZE)  // 15
#define GRID_H          9                           // 9 rows for map (0..8)
#define MAP_HEIGHT      (GRID_H * TILE_SIZE)       // 144, bottom 16px for UI

#define MAX_WAYPOINTS   16
#define MAX_TOWERS      32
#define MAX_ENEMIES     32
#define MAX_WAVES       5

#define TOWER_COST              10
#define TOWER_REFUND            (TOWER_COST/2)
#define TOWER_RANGE             (TILE_SIZE*3/2)    // 24px
#define TOWER_RANGE2            (TOWER_RANGE*TOWER_RANGE)
#define TOWER_COOLDOWN_FRAMES   20
#define TOWER_DAMAGE            1

#define ENEMY_REWARD            3

// -----------------------------------------------------------------------------
// Utility macros
// -----------------------------------------------------------------------------

#define ABS(x) ((x)<0 ? -(x) : (x))

// -----------------------------------------------------------------------------
// Global key state
// -----------------------------------------------------------------------------

static u16 curKeys = 0;
static u16 prevKeys = 0;

#define KEY_DOWN(k)    (curKeys & (k))
#define KEY_PRESSED(k) ((curKeys & (k)) && !(prevKeys & (k)))

// -----------------------------------------------------------------------------
// Simple drawing helpers for Mode 3
// -----------------------------------------------------------------------------

static inline void vsync(void)
{
    // Wait for VBlank
    while (REG_VCOUNT >= 160);
    while (REG_VCOUNT < 160);
}

static inline void draw_pixel(int x, int y, u16 color)
{
    if (x < 0 || x >= SCREEN_WIDTH || y < 0 || y >= SCREEN_HEIGHT)
        return;
    vid_mem[y*SCREEN_WIDTH + x] = color;
}

static void draw_rect(int x, int y, int w, int h, u16 color)
{
    // Clip to screen
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > SCREEN_WIDTH)  w = SCREEN_WIDTH - x;
    if (y + h > SCREEN_HEIGHT) h = SCREEN_HEIGHT - y;
    if (w <= 0 || h <= 0) return;

    for (int j = 0; j < h; j++)
    {
        u16 *dst = &vid_mem[(y + j) * SCREEN_WIDTH + x];
        for (int i = 0; i < w; i++)
            dst[i] = color;
    }
}

static void draw_line(int x0, int y0, int x1, int y1, u16 color)
{
    int dx = ABS(x1 - x0), sx = x0 < x1 ? 1 : -1;
    int dy = -ABS(y1 - y0), sy = y0 < y1 ? 1 : -1;
    int err = dx + dy, e2;

    for (;;)
    {
        draw_pixel(x0, y0, color);
        if (x0 == x1 && y0 == y1) break;
        e2 = 2 * err;
        if (e2 >= dy) { err += dy; x0 += sx; }
        if (e2 <= dx) { err += dx; y0 += sy; }
    }
}

// -----------------------------------------------------------------------------
// Game data structures
// -----------------------------------------------------------------------------

typedef struct
{
    int gx, gy;   // grid coordinates
} Waypoint;

typedef struct
{
    int active;
    int gx, gy;
    int x, y;        // pixel center
    int cooldown;
} Tower;

typedef struct
{
    int active;
    int x, y;        // pixel center
    int hp, maxHp;
    int speed;       // pixels per frame
    int pathIndex;   // index of next waypoint
} Enemy;

// Path and map data
static Waypoint path[MAX_WAYPOINTS];
static int      pathLength = 0;

static int pathGrid[GRID_H][GRID_W];        // 1 if on path, else 0
static int towerIndexGrid[GRID_H][GRID_W];  // index of tower, or -1

// Towers and enemies
static Tower towers[MAX_TOWERS];
static Enemy enemies[MAX_ENEMIES];

// Game state
static int cursorGX = 2;
static int cursorGY = 4;

static int gold      = 0;
static int crystals  = 0;
static int wave      = 0;

static int enemiesSpawnedInWave   = 0;
static int enemiesToSpawnInWave   = 0;
static int spawnTimer             = 0;
static int spawnInterval          = 0;
static int baseEnemyHp            = 0;
static int baseEnemySpeed         = 0;

static int gameOver = 0;   // 0 = playing, 1 = lost, 2 = won

// -----------------------------------------------------------------------------
// Input
// -----------------------------------------------------------------------------

static void poll_keys(void)
{
    prevKeys = curKeys;
    curKeys  = ~REG_KEYINPUT & KEY_MASK;
}

// -----------------------------------------------------------------------------
// Path initialization
// -----------------------------------------------------------------------------

static void init_path(void)
{
    // Define a fixed path (axis-aligned segments) across the grid.
    // All coordinates must be within 0..GRID_W-1, 0..GRID_H-1.

    pathLength = 8;
    path[0].gx = 0;  path[0].gy = 4;
    path[1].gx = 4;  path[1].gy = 4;
    path[2].gx = 4;  path[2].gy = 1;
    path[3].gx = 9;  path[3].gy = 1;
    path[4].gx = 9;  path[4].gy = 6;
    path[5].gx = 12; path[5].gy = 6;
    path[6].gx = 12; path[6].gy = 3;
    path[7].gx = 14; path[7].gy = 3;   // final crystal tile

    // Clear grids
    for (int y = 0; y < GRID_H; y++)
    {
        for (int x = 0; x < GRID_W; x++)
        {
            pathGrid[y][x]       = 0;
            towerIndexGrid[y][x] = -1;
        }
    }

    // Mark path tiles
    for (int i = 0; i < pathLength - 1; i++)
    {
        int gx0 = path[i].gx;
        int gy0 = path[i].gy;
        int gx1 = path[i+1].gx;
        int gy1 = path[i+1].gy;

        int dx = (gx1 > gx0) ? 1 : (gx1 < gx0 ? -1 : 0);
        int dy = (gy1 > gy0) ? 1 : (gy1 < gy0 ? -1 : 0);

        int gx = gx0;
        int gy = gy0;
        pathGrid[gy][gx] = 1;

        while (gx != gx1 || gy != gy1)
        {
            gx += dx;
            gy += dy;
            pathGrid[gy][gx] = 1;
        }
    }
}

// -----------------------------------------------------------------------------
// Game control helpers
// -----------------------------------------------------------------------------

static void start_wave(int w)
{
    wave = w;
    enemiesSpawnedInWave = 0;
    enemiesToSpawnInWave = 8 + w * 2;          // more enemies each wave
    baseEnemyHp          = 3 + w * 2;          // tougher each wave
    baseEnemySpeed       = 1 + (w / 2);        // slightly faster
    spawnInterval        = 40 - w * 4;         // spawn more quickly
    if (spawnInterval < 10) spawnInterval = 10;
    spawnTimer = 60;                           // delay before first spawn
}

static void reset_game(void)
{
    gold      = 50;
    crystals  = 10;
    gameOver  = 0;
    cursorGX  = 2;
    cursorGY  = 4;

    for (int i = 0; i < MAX_TOWERS; i++)
        towers[i].active = 0;

    for (int i = 0; i < MAX_ENEMIES; i++)
        enemies[i].active = 0;

    for (int y = 0; y < GRID_H; y++)
        for (int x = 0; x < GRID_W; x++)
            towerIndexGrid[y][x] = -1;

    start_wave(1);
}

static int count_active_enemies(void)
{
    int count = 0;
    for (int i = 0; i < MAX_ENEMIES; i++)
        if (enemies[i].active)
            count++;
    return count;
}

// -----------------------------------------------------------------------------
// Towers
// -----------------------------------------------------------------------------

static void place_tower(int gx, int gy)
{
    if (gx < 0 || gx >= GRID_W || gy < 0 || gy >= GRID_H)
        return;

    if (pathGrid[gy][gx])
        return;             // can't build on the path

    if (towerIndexGrid[gy][gx] != -1)
        return;             // already a tower here

    if (gold < TOWER_COST)
        return;             // not enough money

    // Find free tower slot
    int idx = -1;
    for (int i = 0; i < MAX_TOWERS; i++)
    {
        if (!towers[i].active)
        {
            idx = i;
            break;
        }
    }
    if (idx < 0)
        return;             // no space for more towers

    Tower *t = &towers[idx];
    t->active   = 1;
    t->gx       = gx;
    t->gy       = gy;
    t->x        = gx * TILE_SIZE + TILE_SIZE/2;
    t->y        = gy * TILE_SIZE + TILE_SIZE/2;
    t->cooldown = 0;

    towerIndexGrid[gy][gx] = idx;
    gold -= TOWER_COST;
}

static void remove_tower(int gx, int gy)
{
    if (gx < 0 || gx >= GRID_W || gy < 0 || gy >= GRID_H)
        return;

    int idx = towerIndexGrid[gy][gx];
    if (idx < 0)
        return;

    towers[idx].active = 0;
    towerIndexGrid[gy][gx] = -1;
    gold += TOWER_REFUND;
}

static void update_towers(void)
{
    for (int i = 0; i < MAX_TOWERS; i++)
    {
        Tower *t = &towers[i];
        if (!t->active)
            continue;

        if (t->cooldown > 0)
        {
            t->cooldown--;
            continue;
        }

        // Find nearest enemy within range
        Enemy *best       = NULL;
        int    bestDist2  = 0;

        for (int j = 0; j < MAX_ENEMIES; j++)
        {
            Enemy *e = &enemies[j];
            if (!e->active)
                continue;

            int dx = e->x - t->x;
            int dy = e->y - t->y;
            int dist2 = dx*dx + dy*dy;
            if (dist2 <= TOWER_RANGE2)
            {
                if (best == NULL || dist2 < bestDist2)
                {
                    best      = e;
                    bestDist2 = dist2;
                }
            }
        }

        if (best != NULL)
        {
            best->hp -= TOWER_DAMAGE;
            if (best->hp <= 0)
            {
                best->active = 0;
                gold += ENEMY_REWARD;
            }
            t->cooldown = TOWER_COOLDOWN_FRAMES;
        }
    }
}

// -----------------------------------------------------------------------------
// Enemies
// -----------------------------------------------------------------------------

static int spawn_enemy(int hp, int speed)
{
    // Find free enemy slot
    int idx = -1;
    for (int i = 0; i < MAX_ENEMIES; i++)
    {
        if (!enemies[i].active)
        {
            idx = i;
            break;
        }
    }
    if (idx < 0)
        return 0;

    Enemy *e = &enemies[idx];

    e->active   = 1;
    e->hp       = hp;
    e->maxHp    = hp;
    e->speed    = speed;
    e->pathIndex = 1;

    int startGX = path[0].gx;
    int startGY = path[0].gy;
    e->x = startGX * TILE_SIZE + TILE_SIZE/2;
    e->y = startGY * TILE_SIZE + TILE_SIZE/2;

    return 1;
}

static void update_enemies(void)
{
    for (int i = 0; i < MAX_ENEMIES; i++)
    {
        Enemy *e = &enemies[i];
        if (!e->active)
            continue;

        if (e->pathIndex >= pathLength)
            continue;

        int targetGX = path[e->pathIndex].gx;
        int targetGY = path[e->pathIndex].gy;
        int targetX  = targetGX * TILE_SIZE + TILE_SIZE/2;
        int targetY  = targetGY * TILE_SIZE + TILE_SIZE/2;

        int dx = 0, dy = 0;

        if (e->x < targetX)      dx =  e->speed;
        else if (e->x > targetX) dx = -e->speed;

        if (e->y < targetY)      dy =  e->speed;
        else if (e->y > targetY) dy = -e->speed;

        e->x += dx;
        e->y += dy;

        // Clamp to avoid overshoot
        if (dx > 0 && e->x > targetX)      e->x = targetX;
        else if (dx < 0 && e->x < targetX) e->x = targetX;
        if (dy > 0 && e->y > targetY)      e->y = targetY;
        else if (dy < 0 && e->y < targetY) e->y = targetY;

        if (e->x == targetX && e->y == targetY)
        {
            e->pathIndex++;
            if (e->pathIndex >= pathLength)
            {
                // Reached the crystal
                e->active = 0;
                if (crystals > 0)
                    crystals--;
            }
        }
    }
}

// -----------------------------------------------------------------------------
// Game update & rendering
// -----------------------------------------------------------------------------

static void update_game(void)
{
    // Cursor movement (grid)
    if (KEY_PRESSED(KEY_LEFT)  && cursorGX > 0)        cursorGX--;
    if (KEY_PRESSED(KEY_RIGHT) && cursorGX < GRID_W-1) cursorGX++;
    if (KEY_PRESSED(KEY_UP)    && cursorGY > 0)        cursorGY--;
    if (KEY_PRESSED(KEY_DOWN)  && cursorGY < GRID_H-1) cursorGY++;

    // Place/remove towers
    if (KEY_PRESSED(KEY_A))
        place_tower(cursorGX, cursorGY);

    if (KEY_PRESSED(KEY_B))
        remove_tower(cursorGX, cursorGY);

    // Spawn enemies for current wave
    if (wave <= MAX_WAVES)
    {
        if (enemiesSpawnedInWave < enemiesToSpawnInWave)
        {
            if (spawnTimer > 0)
                spawnTimer--;
            else
            {
                if (spawn_enemy(baseEnemyHp, baseEnemySpeed))
                {
                    enemiesSpawnedInWave++;
                    spawnTimer = spawnInterval;
                }
            }
        }
    }

    // Update enemies and towers
    update_enemies();
    update_towers();

    // Check loss condition
    if (crystals <= 0 && gameOver == 0)
        gameOver = 1;

    // Wave / victory progression
    if (gameOver == 0 &&
        wave <= MAX_WAVES &&
        enemiesSpawnedInWave >= enemiesToSpawnInWave &&
        count_active_enemies() == 0)
    {
        if (wave == MAX_WAVES)
        {
            gameOver = 2;     // win after last wave
        }
        else
        {
            gold += 10 + wave * 5; // small bonus
            start_wave(wave + 1);
        }
    }

    // Restart after win/lose
    if (gameOver != 0 && KEY_PRESSED(KEY_START))
        reset_game();
}

static void draw_game(void)
{
    // Background
    draw_rect(0, 0, SCREEN_WIDTH, SCREEN_HEIGHT, RGB15(0, 4, 0));

    // Draw path tiles
    for (int gy = 0; gy < GRID_H; gy++)
    {
        for (int gx = 0; gx < GRID_W; gx++)
        {
            if (pathGrid[gy][gx])
            {
                int x = gx * TILE_SIZE;
                int y = gy * TILE_SIZE;
                draw_rect(x, y, TILE_SIZE, TILE_SIZE, RGB15(8, 8, 8));
            }
        }
    }

    // Draw crystal/base tile
    int baseGX = path[pathLength-1].gx;
    int baseGY = path[pathLength-1].gy;
    int bx = baseGX * TILE_SIZE;
    int by = baseGY * TILE_SIZE;
    draw_rect(bx+3, by+3, TILE_SIZE-6, TILE_SIZE-6, RGB15(0, 20, 20));

    // Draw towers
    for (int i = 0; i < MAX_TOWERS; i++)
    {
        Tower *t = &towers[i];
        if (!t->active) continue;

        int size = TILE_SIZE - 4;
        int left = t->x - size/2;
        int top  = t->y - size/2;
        draw_rect(left, top, size, size, RGB15(0, 0, 20));
    }

    // Draw enemies
    for (int i = 0; i < MAX_ENEMIES; i++)
    {
        Enemy *e = &enemies[i];
        if (!e->active) continue;

        int size = TILE_SIZE - 4;
        int left = e->x - size/2;
        int top  = e->y - size/2;

        // Body
        draw_rect(left, top, size, size, RGB15(20, 0, 0));

        // HP bar above enemy
        int barW = size;
        int hpW  = (e->hp * barW) / e->maxHp;
        if (hpW < 0) hpW = 0;

        draw_rect(left,   top-2, barW, 2, RGB15(4, 0, 0));
        if (hpW > 0)
            draw_rect(left, top-2, hpW, 2, RGB15(0, 20, 0));
    }

    // Cursor highlight
    int cx = cursorGX * TILE_SIZE;
    int cy = cursorGY * TILE_SIZE;
    u16 curColor = RGB15(31, 31, 0);
    for (int x = cx; x < cx + TILE_SIZE; x++)
    {
        draw_pixel(x, cy,                 curColor);
        draw_pixel(x, cy + TILE_SIZE - 1, curColor);
    }
    for (int y = cy; y < cy + TILE_SIZE; y++)
    {
        draw_pixel(cx,                 y, curColor);
        draw_pixel(cx + TILE_SIZE - 1, y, curColor);
    }

    // UI bar at bottom
    int uiY = MAP_HEIGHT;
    draw_rect(0, uiY, SCREEN_WIDTH, SCREEN_HEIGHT - uiY, RGB15(4, 4, 4));

    // Crystals (top-left in UI)
    int maxCrystalsDisplay = 20;
    int displayCrystals    = crystals;
    if (displayCrystals > maxCrystalsDisplay)
        displayCrystals = maxCrystalsDisplay;
    for (int c = 0; c < displayCrystals; c++)
    {
        int x0 = 4 + c*6;
        int y0 = uiY + 3;
        draw_rect(x0, y0, 4, 4, RGB15(0, 25, 25));
    }

    // Gold as small yellow squares
    int bars = gold / 5;
    if (bars > 30) bars = 30;
    for (int i = 0; i < bars; i++)
    {
        int x0 = 4 + i*4;
        int y0 = uiY + 10;
        draw_rect(x0, y0, 3, 3, RGB15(31, 31, 0));
    }

    // Wave indicator on right side of UI
    for (int i = 0; i < wave && i < MAX_WAVES; i++)
    {
        int x0 = SCREEN_WIDTH - 10 - i*6;
        int y0 = uiY + 3;
        draw_rect(x0, y0, 4, 4, RGB15(15, 15, 31));
    }

    // Simple game-over overlay (color only, no text)
    if (gameOver != 0)
    {
        u16 col = (gameOver == 1) ? RGB15(20, 0, 0) : RGB15(0, 20, 0);
        draw_rect(60, 60, 120, 40, col);
    }
}

// -----------------------------------------------------------------------------
// main()
// -----------------------------------------------------------------------------

int main(void)
{
    // Mode 3, BG2 on
    REG_DISPCNT = DCNT_MODE3 | DCNT_BG2;

    init_path();
    reset_game();

    while (1)
    {
        poll_keys();

        if (gameOver == 0)
            update_game();
        else
        {
            // Still allow restart if paused on win/lose screen
            if (KEY_PRESSED(KEY_START))
                reset_game();
        }

        draw_game();
        vsync();
    }

    return 0;
}
