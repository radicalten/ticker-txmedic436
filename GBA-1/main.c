// shmup.c - simple horizontal shmup for GBA using tonc (single file)
//
// Controls:
//   D-Pad : Move ship
//   A     : Fire
//   START : Restart after game over
//
// Build (example, adjust paths for your setup):
//   arm-none-eabi-gcc -mthumb -mthumb-interwork -O2 -I$(TONC)/include shmup.c -L$(TONC)/lib -ltonc -o shmup.elf
//   arm-none-eabi-objcopy -O binary shmup.elf shmup.gba

#include <tonc.h>

#define MAX_BULLETS 32
#define MAX_ENEMIES 16
#define MAX_STARS   64

#define PLAYER_SPEED 2
#define BULLET_SPEED 4
#define ENEMY_SPEED  1
#define SCROLL_SPEED 1

// Colors (15-bit BGR)
#define COLOR_BG      RGB15(0, 0, 0)
#define COLOR_PLAYER  RGB15(0, 31, 0)
#define COLOR_ENEMY   RGB15(31, 0, 0)
#define COLOR_BULLET  RGB15(31, 31, 0)
#define COLOR_STAR    RGB15(24, 24, 24)
#define COLOR_BORDER  RGB15(31, 0, 0)

typedef struct
{
    int x, y;
    int w, h;
    int vx, vy;
    int active;
} Entity;

typedef struct
{
    int x, y;
} Star;

static Entity player;
static Entity bullets[MAX_BULLETS];
static Entity enemies[MAX_ENEMIES];
static Star   stars[MAX_STARS];

static int score = 0;
static int spawnTimer = 0;
static int enemySpawnCounter = 0;
static int gameOver = 0;

// ----- Utility drawing -----

static inline void draw_pixel(int x, int y, u16 color)
{
    if((unsigned)x >= SCREEN_WIDTH || (unsigned)y >= SCREEN_HEIGHT)
        return;
    vid_mem[y*SCREEN_WIDTH + x] = color;
}

static void draw_rect(int x, int y, int w, int h, u16 color)
{
    if(w <= 0 || h <= 0)
        return;

    int x2 = x + w;
    int y2 = y + h;

    if(x < 0)     x = 0;
    if(y < 0)     y = 0;
    if(x2 > SCREEN_WIDTH)  x2 = SCREEN_WIDTH;
    if(y2 > SCREEN_HEIGHT) y2 = SCREEN_HEIGHT;

    for(int j=y; j<y2; j++)
    {
        u16 *row = &vid_mem[j*SCREEN_WIDTH];
        for(int i=x; i<x2; i++)
            row[i] = color;
    }
}

// ----- Game logic -----

static int rects_overlap(const Entity *a, const Entity *b)
{
    return (a->x < b->x + b->w) &&
           (a->x + a->w > b->x) &&
           (a->y < b->y + b->h) &&
           (a->y + a->h > b->y);
}

static void init_stars(void)
{
    for(int i=0; i<MAX_STARS; i++)
    {
        // Simple deterministic pseudo-random distribution
        stars[i].x = (i * 37) % SCREEN_WIDTH;
        stars[i].y = (i * 17) % SCREEN_HEIGHT;
    }
}

static void init_game(void)
{
    // Player
    player.w = 16;
    player.h = 8;
    player.x = 20;
    player.y = SCREEN_HEIGHT/2 - player.h/2;
    player.vx = player.vy = 0;
    player.active = 1;

    // Bullets and enemies
    for(int i=0; i<MAX_BULLETS; i++)
        bullets[i].active = 0;

    for(int i=0; i<MAX_ENEMIES; i++)
        enemies[i].active = 0;

    init_stars();

    score = 0;
    spawnTimer = 0;
    enemySpawnCounter = 0;
    gameOver = 0;
}

static void spawn_enemy(void)
{
    for(int i=0; i<MAX_ENEMIES; i++)
    {
        if(!enemies[i].active)
        {
            Entity *e = &enemies[i];
            e->active = 1;
            e->w = 12;
            e->h = 8;
            e->x = SCREEN_WIDTH - e->w;
            // Deterministic "random" vertical position
            e->y = (enemySpawnCounter * 23) % (SCREEN_HEIGHT - e->h);
            e->vx = -(ENEMY_SPEED + SCROLL_SPEED);
            e->vy = 0;

            enemySpawnCounter++;
            break;
        }
    }
}

static void update_game(void)
{
    // If game over, wait for START to restart
    if(gameOver)
    {
        if(key_hit(KEY_START))
            init_game();
        return;
    }

    // ----- Input -----
    if(key_is_down(KEY_UP))
        player.y -= PLAYER_SPEED;
    if(key_is_down(KEY_DOWN))
        player.y += PLAYER_SPEED;
    if(key_is_down(KEY_LEFT))
        player.x -= PLAYER_SPEED;
    if(key_is_down(KEY_RIGHT))
        player.x += PLAYER_SPEED;

    // Clamp player to screen
    if(player.x < 0) player.x = 0;
    if(player.y < 0) player.y = 0;
    if(player.x > SCREEN_WIDTH - player.w)
        player.x = SCREEN_WIDTH - player.w;
    if(player.y > SCREEN_HEIGHT - player.h)
        player.y = SCREEN_HEIGHT - player.h;

    // Shooting
    if(key_hit(KEY_A))
    {
        for(int i=0; i<MAX_BULLETS; i++)
        {
            if(!bullets[i].active)
            {
                Entity *b = &bullets[i];
                b->active = 1;
                b->w = 4;
                b->h = 2;
                b->x = player.x + player.w;
                b->y = player.y + player.h/2 - b->h/2;
                b->vx = BULLET_SPEED;
                b->vy = 0;
                break;
            }
        }
    }

    // ----- Background stars scroll -----
    for(int i=0; i<MAX_STARS; i++)
    {
        stars[i].x -= SCROLL_SPEED;
        if(stars[i].x < 0)
            stars[i].x += SCREEN_WIDTH;
    }

    // ----- Bullets -----
    for(int i=0; i<MAX_BULLETS; i++)
    {
        if(!bullets[i].active) continue;

        bullets[i].x += bullets[i].vx;
        bullets[i].y += bullets[i].vy;

        if(bullets[i].x >= SCREEN_WIDTH)
            bullets[i].active = 0;
    }

    // ----- Enemy spawn -----
    if(spawnTimer > 0)
        spawnTimer--;
    else
    {
        spawn_enemy();
        int maxInterval = 60;
        int minInterval = 20;
        int interval = maxInterval - score/5;
        if(interval < minInterval)
            interval = minInterval;
        spawnTimer = interval;
    }

    // ----- Enemies -----
    for(int i=0; i<MAX_ENEMIES; i++)
    {
        if(!enemies[i].active) continue;

        enemies[i].x += enemies[i].vx;
        enemies[i].y += enemies[i].vy;

        if(enemies[i].x + enemies[i].w < 0)
            enemies[i].active = 0;
    }

    // ----- Collisions: bullets vs enemies -----
    for(int i=0; i<MAX_BULLETS; i++)
    {
        if(!bullets[i].active) continue;

        for(int j=0; j<MAX_ENEMIES; j++)
        {
            if(!enemies[j].active) continue;

            if(rects_overlap(&bullets[i], &enemies[j]))
            {
                bullets[i].active = 0;
                enemies[j].active = 0;
                score++;
                break;
            }
        }
    }

    // ----- Collisions: player vs enemies -----
    for(int i=0; i<MAX_ENEMIES; i++)
    {
        if(!enemies[i].active) continue;

        if(rects_overlap(&player, &enemies[i]))
        {
            gameOver = 1;
            break;
        }
    }
}

static void draw_game(void)
{
    // Clear screen
    m3_fill(COLOR_BG);

    // Stars
    for(int i=0; i<MAX_STARS; i++)
        draw_pixel(stars[i].x, stars[i].y, COLOR_STAR);

    // Player
    if(player.active)
        draw_rect(player.x, player.y, player.w, player.h, COLOR_PLAYER);

    // Bullets
    for(int i=0; i<MAX_BULLETS; i++)
        if(bullets[i].active)
            draw_rect(bullets[i].x, bullets[i].y, bullets[i].w, bullets[i].h, COLOR_BULLET);

    // Enemies
    for(int i=0; i<MAX_ENEMIES; i++)
        if(enemies[i].active)
            draw_rect(enemies[i].x, enemies[i].y, enemies[i].w, enemies[i].h, COLOR_ENEMY);

    // Simple red borders when game over
    if(gameOver)
    {
        draw_rect(0, 0, SCREEN_WIDTH, 4, COLOR_BORDER);
        draw_rect(0, SCREEN_HEIGHT-4, SCREEN_WIDTH, 4, COLOR_BORDER);
    }
}

int main(void)
{
    // Mode 3: 240x160, 16-bit bitmap, BG2 on
    REG_DISPCNT = DCNT_MODE3 | DCNT_BG2;

    init_game();

    while(1)
    {
        VBlankIntrWait();
        key_poll();
        update_game();
        draw_game();
    }

    return 0;
}
