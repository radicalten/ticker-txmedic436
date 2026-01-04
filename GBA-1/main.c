// twinbee_like_shmup.c
// Minimal vertical shmup for GBA using tonc (Mode 3, bitmap).
// Single file, no external assets.
//
// Build (example):
//   arm-none-eabi-gcc -mthumb -mthumb-interwork -O2 -std=c99 \
//       -I/path/to/tonc/include twinbee_like_shmup.c \
//       -L/path/to/tonc/lib -ltonc -o game.elf
//   arm-none-eabi-objcopy -O binary game.elf game.gba
//
// Run in mGBA / hardware.
//
// Requires tonc: http://www.coranac.com/projects/#tonc

#include <tonc.h>

// -----------------------------------------------------------------------------
// Constants
// -----------------------------------------------------------------------------

#define SCREEN_W    240
#define SCREEN_H    160

#define MAX_BULLETS 16
#define MAX_ENEMIES 16
#define MAX_STARS   32

// Colors (5-bit RGB)
#define COL_BG       RGB15(0,0,4)
#define COL_STAR1    RGB15(16,16,31)
#define COL_STAR2    RGB15(8,8,24)
#define COL_PLAYER   RGB15(31,20,24)
#define COL_PLAYER_ACCENT RGB15(31,31,31)
#define COL_BULLET   RGB15(31,31,0)
#define COL_ENEMY    RGB15(31,10,10)
#define COL_ENEMY_EDGE RGB15(31,0,0)
#define COL_UI       RGB15(31,31,31)
#define COL_UI_SHADOW RGB15(8,8,8)

// -----------------------------------------------------------------------------
// Types
// -----------------------------------------------------------------------------

typedef struct {
    int x, y;
    int w, h;
    int speed;
    int cooldown;   // frames until next bullet can be fired
    int lives;
} Player;

typedef struct {
    int x, y;
    int dy;
    int active;
} Bullet;

typedef struct {
    int x, y;
    int dy;
    int active;
} Enemy;

typedef struct {
    int x, y;
    int speed;
} Star;

// -----------------------------------------------------------------------------
// Globals
// -----------------------------------------------------------------------------

static Player player;
static Bullet bullets[MAX_BULLETS];
static Enemy  enemies[MAX_ENEMIES];
static Star   stars[MAX_STARS];

static int score = 0;
static int gameOver = 0;
static int invincibleTimer = 0;
static int enemySpawnCounter = 0;

// Simple fast RNG (LCG, like tonc's qran).
static u32 rngSeed = 1;

// -----------------------------------------------------------------------------
// Utility / low-level
// -----------------------------------------------------------------------------

static inline void vsync(void)
{
    // Busy-wait for vertical blank (no interrupts required).
    while (REG_VCOUNT >= 160);
    while (REG_VCOUNT < 160);
}

static inline int rand_fast(void)
{
    rngSeed = 1664525 * rngSeed + 1013904223;
    return (rngSeed >> 16) & 0x7FFF;
}

static inline void clamp_int(int *v, int min, int max)
{
    if (*v < min) *v = min;
    if (*v > max) *v = max;
}

// -----------------------------------------------------------------------------
// Drawing primitives
// -----------------------------------------------------------------------------

static inline void plotPixel(int x, int y, u16 color)
{
    if ((unsigned)x >= SCREEN_W || (unsigned)y >= SCREEN_H)
        return;
    vid_mem[y * SCREEN_W + x] = color;
}

static void clearScreen(u16 color)
{
    u16 *dst = vid_mem;
    int i;
    for (i = 0; i < SCREEN_W * SCREEN_H; i++)
        dst[i] = color;
}

static void drawRect(int x, int y, int w, int h, u16 color)
{
    if (w <= 0 || h <= 0)
        return;

    int ix, iy;
    for (iy = 0; iy < h; iy++)
    {
        int yy = y + iy;
        if ((unsigned)yy >= SCREEN_H)
            continue;

        u16 *row = &vid_mem[yy * SCREEN_W];
        for (ix = 0; ix < w; ix++)
        {
            int xx = x + ix;
            if ((unsigned)xx >= SCREEN_W)
                continue;
            row[xx] = color;
        }
    }
}

// -----------------------------------------------------------------------------
// Tiny 3x5 bitmap font for digits 0-9 (scaled up when drawn)
// -----------------------------------------------------------------------------

static const char digitFont[10][5][4] = {
    { "###", "# #", "# #", "# #", "###" }, // 0
    { "  #", "  #", "  #", "  #", "  #" }, // 1
    { "###", "  #", "###", "#  ", "###" }, // 2
    { "###", "  #", "###", "  #", "###" }, // 3
    { "# #", "# #", "###", "  #", "  #" }, // 4
    { "###", "#  ", "###", "  #", "###" }, // 5
    { "###", "#  ", "###", "# #", "###" }, // 6
    { "###", "  #", "  #", "  #", "  #" }, // 7
    { "###", "# #", "###", "# #", "###" }, // 8
    { "###", "# #", "###", "  #", "###" }  // 9
};

static void drawDigit(int digit, int x, int y, u16 color)
{
    if (digit < 0 || digit > 9)
        return;

    for (int row = 0; row < 5; row++)
    {
        for (int col = 0; col < 3; col++)
        {
            if (digitFont[digit][row][col] == '#')
            {
                // Scale each font pixel to 2x2 screen pixels
                drawRect(x + col * 2, y + row * 2, 2, 2, color);
            }
        }
    }
}

static void drawNumber(int value, int x, int y, u16 color)
{
    if (value < 0) value = 0;

    char buf[6];
    int len = 0;

    do {
        int d = value % 10;
        buf[len++] = (char)('0' + d);
        value /= 10;
    } while (value > 0 && len < (int)sizeof(buf));

    for (int i = 0; i < len; i++)
    {
        int d = buf[len - 1 - i] - '0';
        drawDigit(d, x + i * 8, y, color);
    }
}

// -----------------------------------------------------------------------------
// Simple heart icon for lives
// -----------------------------------------------------------------------------

static void drawHeart(int x, int y, u16 color)
{
    // Rough 7x7 heart
    drawRect(x+1, y,   2, 2, color);  // top left bump
    drawRect(x+4, y,   2, 2, color);  // top right bump
    drawRect(x,   y+2, 7, 2, color);  // upper bar
    drawRect(x+1, y+4, 5, 2, color);  // middle
    drawRect(x+2, y+6, 3, 1, color);  // bottom tip
}

// -----------------------------------------------------------------------------
// Entities: Player, Bullets, Enemies, Stars
// -----------------------------------------------------------------------------

static void initStars(void)
{
    for (int i = 0; i < MAX_STARS; i++)
    {
        stars[i].x = rand_fast() % SCREEN_W;
        stars[i].y = rand_fast() % SCREEN_H;
        stars[i].speed = 1 + (rand_fast() % 2); // 1 or 2
    }
}

static void updateStars(void)
{
    for (int i = 0; i < MAX_STARS; i++)
    {
        stars[i].y += stars[i].speed;
        if (stars[i].y >= SCREEN_H)
        {
            stars[i].y = 0;
            stars[i].x = rand_fast() % SCREEN_W;
        }
    }
}

static void drawStars(void)
{
    for (int i = 0; i < MAX_STARS; i++)
    {
        u16 col = (stars[i].speed == 1) ? COL_STAR2 : COL_STAR1;
        plotPixel(stars[i].x, stars[i].y, col);
    }
}

static void resetBullets(void)
{
    for (int i = 0; i < MAX_BULLETS; i++)
        bullets[i].active = 0;
}

static void resetEnemies(void)
{
    for (int i = 0; i < MAX_ENEMIES; i++)
        enemies[i].active = 0;
}

static void initPlayer(void)
{
    player.w = 14;
    player.h = 12;
    player.x = SCREEN_W / 2 - player.w / 2;
    player.y = SCREEN_H - player.h - 8;
    player.speed = 2;
    player.cooldown = 0;
    player.lives = 3;
}

static void resetGame(void)
{
    score = 0;
    gameOver = 0;
    invincibleTimer = 0;
    enemySpawnCounter = 0;
    rngSeed = 1;   // deterministic; change if you want

    initPlayer();
    resetBullets();
    resetEnemies();
    initStars();
}

static void spawnEnemy(void)
{
    for (int i = 0; i < MAX_ENEMIES; i++)
    {
        if (!enemies[i].active)
        {
            enemies[i].active = 1;
            enemies[i].x = 8 + (rand_fast() % (SCREEN_W - 16));
            enemies[i].y = -12;
            enemies[i].dy = 1 + (rand_fast() % 2);
            return;
        }
    }
}

static void fireBullet(void)
{
    for (int i = 0; i < MAX_BULLETS; i++)
    {
        if (!bullets[i].active)
        {
            bullets[i].active = 1;
            bullets[i].x = player.x + player.w / 2 - 1;
            bullets[i].y = player.y - 2;
            bullets[i].dy = -4;
            player.cooldown = 8;   // firing rate
            return;
        }
    }
}

static int aabb_overlap(int x1,int y1,int w1,int h1, int x2,int y2,int w2,int h2)
{
    return (x1 < x2 + w2) && (x1 + w1 > x2) &&
           (y1 < y2 + h2) && (y1 + h1 > y2);
}

// -----------------------------------------------------------------------------
// Update & draw
// -----------------------------------------------------------------------------

static void handleInput(void)
{
    // movement
    int dx = 0, dy = 0;

    if (key_is_down(KEY_LEFT))  dx -= player.speed;
    if (key_is_down(KEY_RIGHT)) dx += player.speed;
    if (key_is_down(KEY_UP))    dy -= player.speed;
    if (key_is_down(KEY_DOWN))  dy += player.speed;

    player.x += dx;
    player.y += dy;

    clamp_int(&player.x, 0, SCREEN_W - player.w);
    clamp_int(&player.y, 16, SCREEN_H - player.h); // keep off UI bar

    // shooting
    if (player.cooldown > 0)
        player.cooldown--;

    if (key_is_down(KEY_A) && player.cooldown == 0)
        fireBullet();
}

static void updateBullets(void)
{
    for (int i = 0; i < MAX_BULLETS; i++)
    {
        if (!bullets[i].active)
            continue;
        bullets[i].y += bullets[i].dy;
        if (bullets[i].y < -8 || bullets[i].y >= SCREEN_H)
            bullets[i].active = 0;
    }
}

static void updateEnemies(void)
{
    enemySpawnCounter++;
    // spawn roughly once per ~40 frames, a bit randomised
    if (enemySpawnCounter > 30 + (rand_fast() & 15))
    {
        enemySpawnCounter = 0;
        spawnEnemy();
    }

    for (int i = 0; i < MAX_ENEMIES; i++)
    {
        if (!enemies[i].active)
            continue;

        enemies[i].y += enemies[i].dy;

        if (enemies[i].y > SCREEN_H)
            enemies[i].active = 0;
    }
}

static void handleCollisions(void)
{
    // bullets vs enemies
    for (int b = 0; b < MAX_BULLETS; b++)
    {
        if (!bullets[b].active)
            continue;

        int bx = bullets[b].x;
        int by = bullets[b].y;
        int bw = 2;
        int bh = 4;

        for (int e = 0; e < MAX_ENEMIES; e++)
        {
            if (!enemies[e].active)
                continue;

            int ex = enemies[e].x;
            int ey = enemies[e].y;
            int ew = 14;
            int eh = 12;

            if (aabb_overlap(bx, by, bw, bh, ex, ey, ew, eh))
            {
                bullets[b].active = 0;
                enemies[e].active = 0;
                score += 10;
                break;
            }
        }
    }

    // enemies vs player
    if (player.lives > 0 && invincibleTimer == 0)
    {
        int px = player.x;
        int py = player.y;
        int pw = player.w;
        int ph = player.h;

        for (int e = 0; e < MAX_ENEMIES; e++)
        {
            if (!enemies[e].active)
                continue;

            int ex = enemies[e].x;
            int ey = enemies[e].y;
            int ew = 14;
            int eh = 12;

            if (aabb_overlap(px, py, pw, ph, ex, ey, ew, eh))
            {
                enemies[e].active = 0;
                player.lives--;
                if (player.lives <= 0)
                {
                    gameOver = 1;
                }
                else
                {
                    invincibleTimer = 90; // ~1.5 seconds at 60 FPS
                }
                break;
            }
        }
    }

    if (invincibleTimer > 0)
        invincibleTimer--;
}

static void drawPlayer(void)
{
    if (player.lives <= 0)
        return;

    // Flicker while invincible
    if (invincibleTimer > 0 && (invincibleTimer & 4))
        return;

    int x = player.x;
    int y = player.y;
    int w = player.w;
    int h = player.h;

    // Simple "cute ship": body + cockpit
    drawRect(x+2, y+2, w-4, h-2, COL_PLAYER);
    drawRect(x+4, y+1, w-8, 3, COL_PLAYER_ACCENT);
    drawRect(x,   y+4, 3, 4, COL_PLAYER);          // left wing
    drawRect(x+w-3, y+4, 3, 4, COL_PLAYER);        // right wing
}

static void drawBullets(void)
{
    for (int i = 0; i < MAX_BULLETS; i++)
    {
        if (!bullets[i].active)
            continue;
        drawRect(bullets[i].x, bullets[i].y, 2, 4, COL_BULLET);
    }
}

static void drawEnemies(void)
{
    for (int i = 0; i < MAX_ENEMIES; i++)
    {
        if (!enemies[i].active)
            continue;

        int x = enemies[i].x;
        int y = enemies[i].y;

        // Rounded blob enemy
        drawRect(x+2, y,   10, 2, COL_ENEMY_EDGE);
        drawRect(x+1, y+2, 12, 8, COL_ENEMY);
        drawRect(x,   y+4, 14, 6, COL_ENEMY);
        drawRect(x+4, y+3, 2, 2, COL_UI_SHADOW); // one "eye"
        drawRect(x+8, y+3, 2, 2, COL_UI_SHADOW); // other "eye"
    }
}

static void drawUI(void)
{
    // Top bar background
    drawRect(0, 0, SCREEN_W, 16, RGB15(0,0,0));

    // Score number at left
    drawNumber(score, 4, 3, COL_UI);

    // Lives at right
    for (int i = 0; i < player.lives; i++)
    {
        int hx = SCREEN_W - 10 - i * 12;
        int hy = 4;
        drawHeart(hx+1, hy+1, COL_UI_SHADOW);
        drawHeart(hx,   hy,   COL_UI);
    }

    // If game over, overlay a big block message area
    if (gameOver)
    {
        // Dark box in middle
        int bx = 40, by = 50, bw = SCREEN_W - 80, bh = 60;
        drawRect(bx,   by,   bw,   bh,   RGB15(0,0,0));
        drawRect(bx+2, by+2, bw-4, bh-4, RGB15(8,0,0));

        // Show score big in the box center ("SCORE" not written, just the value)
        drawNumber(score, bx + 12, by + 12, COL_UI);

        // Display "0" lives as implicit game over indicator (already 0)
        // Restart hint: press START (not written on screen to keep font tiny).
    }
}

static void drawFrame(void)
{
    clearScreen(COL_BG);
    drawStars();
    drawPlayer();
    drawBullets();
    drawEnemies();
    drawUI();
}

// -----------------------------------------------------------------------------
// main
// -----------------------------------------------------------------------------

int main(void)
{
    // Video: Mode 3, BG2 on
    REG_DISPCNT = DCNT_MODE3 | DCNT_BG2;

    resetGame();

    while (1)
    {
        vsync();
        key_poll();

        if (!gameOver)
        {
            handleInput();
            updateStars();
            updateBullets();
            updateEnemies();
            handleCollisions();
        }
        else
        {
            // Allow restart with START
            if (key_hit(KEY_START))
                resetGame();

            // Still animate stars even when game over
            updateStars();
        }

        drawFrame();
    }

    // Not reached
    return 0;
}
