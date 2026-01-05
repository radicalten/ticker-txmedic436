// dungeon.c - simple Mystery Dungeon-like for GBA using tonc (single file)
//
// Build example (adjust paths as needed):
// arm-none-eabi-gcc -mthumb -mthumb-interwork -O2 -Wall -I/path/to/tonc -c dungeon.c
// arm-none-eabi-gcc -mthumb -mthumb-interwork -specs=gba.specs dungeon.o -L/path/to/tonc/lib -ltonc -o dungeon.elf
// arm-none-eabi-objcopy -O binary dungeon.elf dungeon.gba
//
// Run on hardware/emu (mGBA, VBA, etc.)

#include <tonc.h>

#define SCREEN_WIDTH   240
#define SCREEN_HEIGHT  160

#define TILE_SIZE      8
#define MAP_W          28      // 28 * 8 = 224 px
#define MAP_H          18      // 18 * 8 = 144 px
                               // Leaves 16 px at bottom for UI
#define MAX_MONSTERS   8
#define PLAYER_MAX_HP  10
#define MONSTER_MAX_HP 3

#define FB ((u16*)VRAM)

typedef unsigned char bool;
#define true  1
#define false 0

enum { TILE_WALL=0, TILE_FLOOR=1 };

typedef struct
{
    int x, y;      // tile coordinates
    int hp;
    bool alive;
} Entity;

// Global game state
static u8     map_data[MAP_H][MAP_W];
static Entity player;
static Entity monsters[MAX_MONSTERS];
static int    alive_monsters = 0;

// --------------------------------------------------------------------
// RNG (simple LCG)
// --------------------------------------------------------------------

static u32 rng_state = 1;

static u32 rand_u32(void)
{
    rng_state = 1664525 * rng_state + 1013904223;
    return rng_state;
}

static int rand_int(int max)
{
    return (int)(rand_u32() % (u32)max);
}

// --------------------------------------------------------------------
// Utility
// --------------------------------------------------------------------

static inline int iabs(int x) { return x < 0 ? -x : x; }

static inline void vsync(void)
{
    // Simple VBlank wait without using interrupts
    while (REG_VCOUNT >= 160);
    while (REG_VCOUNT < 160);
}

// --------------------------------------------------------------------
// Drawing helpers
// --------------------------------------------------------------------

static void fill_rect(int x, int y, int w, int h, u16 color)
{
    // Clip to screen bounds
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > SCREEN_WIDTH)  w = SCREEN_WIDTH  - x;
    if (y + h > SCREEN_HEIGHT) h = SCREEN_HEIGHT - y;
    if (w <= 0 || h <= 0) return;

    for (int iy = 0; iy < h; iy++)
    {
        u16 *dst = FB + (y + iy) * SCREEN_WIDTH + x;
        for (int ix = 0; ix < w; ix++)
            dst[ix] = color;
    }
}

static void draw_tile(int tx, int ty, u16 color)
{
    int px = tx * TILE_SIZE;
    int py = ty * TILE_SIZE;
    fill_rect(px, py, TILE_SIZE, TILE_SIZE, color);
}

// --------------------------------------------------------------------
// Monsters helpers
// --------------------------------------------------------------------

static int find_monster_at(int x, int y)
{
    for (int i = 0; i < MAX_MONSTERS; i++)
    {
        if (monsters[i].alive &&
            monsters[i].x == x &&
            monsters[i].y == y)
            return i;
    }
    return -1;
}

// --------------------------------------------------------------------
// Level generation
// --------------------------------------------------------------------

static void generate_dungeon(void)
{
    // Fill with walls
    for (int y = 0; y < MAP_H; y++)
        for (int x = 0; x < MAP_W; x++)
            map_data[y][x] = TILE_WALL;

    // Simple random-walk "cave" carving
    int x = MAP_W / 2;
    int y = MAP_H / 2;
    int steps = MAP_W * MAP_H * 5;

    for (int i = 0; i < steps; i++)
    {
        map_data[y][x] = TILE_FLOOR;

        int dir = rand_int(4);
        switch (dir)
        {
        case 0: if (x > 1)         x--; break;
        case 1: if (x < MAP_W-2)   x++; break;
        case 2: if (y > 1)         y--; break;
        case 3: if (y < MAP_H-2)   y++; break;
        }
    }

    // Place player on a floor tile near center-bottom
    bool placed = false;
    for (int ty = MAP_H/2; ty < MAP_H-1 && !placed; ty++)
    {
        for (int tx = 1; tx < MAP_W-1; tx++)
        {
            if (map_data[ty][tx] == TILE_FLOOR)
            {
                player.x = tx;
                player.y = ty;
                placed = true;
                break;
            }
        }
    }
    if (!placed)
    {
        // Fallback to center
        player.x = MAP_W/2;
        player.y = MAP_H/2;
    }

    player.hp    = PLAYER_MAX_HP;
    player.alive = true;

    // Clear monsters
    for (int i = 0; i < MAX_MONSTERS; i++)
        monsters[i].alive = false;

    // Place monsters on random floor tiles
    alive_monsters = 0;
    for (int i = 0; i < MAX_MONSTERS; i++)
    {
        int tries = 0;
        while (tries < 1000)
        {
            int mx = rand_int(MAP_W);
            int my = rand_int(MAP_H);
            tries++;

            if (map_data[my][mx] != TILE_FLOOR) continue;
            if (mx == player.x && my == player.y) continue;
            if (find_monster_at(mx, my) >= 0) continue;

            monsters[i].x     = mx;
            monsters[i].y     = my;
            monsters[i].hp    = MONSTER_MAX_HP;
            monsters[i].alive = true;
            alive_monsters++;
            break;
        }
    }
}

// --------------------------------------------------------------------
// Player & monsters turns
// --------------------------------------------------------------------

static void try_move_player(int dx, int dy)
{
    if (dx == 0 && dy == 0) return;

    int nx = player.x + dx;
    int ny = player.y + dy;

    if (nx < 0 || ny < 0 || nx >= MAP_W || ny >= MAP_H)
        return;

    if (map_data[ny][nx] == TILE_WALL)
        return;

    int mi = find_monster_at(nx, ny);
    if (mi >= 0)
    {
        // Attack the monster instead of moving
        Entity *m = &monsters[mi];
        if (m->alive)
        {
            m->hp--;
            if (m->hp <= 0)
            {
                m->alive = false;
                if (alive_monsters > 0)
                    alive_monsters--;
            }
        }
    }
    else
    {
        // Move into empty floor tile
        player.x = nx;
        player.y = ny;
    }
}

static void monsters_turn(void)
{
    if (!player.alive)
        return;

    for (int i = 0; i < MAX_MONSTERS; i++)
    {
        Entity *m = &monsters[i];
        if (!m->alive)
            continue;

        int dx = 0, dy = 0;

        int distx = player.x - m->x;
        int disty = player.y - m->y;
        int adx   = iabs(distx);
        int ady   = iabs(disty);

        // Chase the player if fairly close, else wander randomly
        if (adx + ady <= 6)
        {
            if (adx > ady)
                dx = (distx > 0) ? 1 : -1;
            else if (ady > 0)
                dy = (disty > 0) ? 1 : -1;
        }
        else
        {
            int dir = rand_int(5);
            switch (dir)
            {
            case 0: dx = -1; break;
            case 1: dx =  1; break;
            case 2: dy = -1; break;
            case 3: dy =  1; break;
            default: dx = dy = 0; break;  // stand still
            }
        }

        if (dx == 0 && dy == 0)
            continue;

        int nx = m->x + dx;
        int ny = m->y + dy;

        if (nx < 0 || ny < 0 || nx >= MAP_W || ny >= MAP_H)
            continue;

        if (map_data[ny][nx] == TILE_WALL)
            continue;

        // If tile has another monster (and not the player), don't move
        if (find_monster_at(nx, ny) >= 0 &&
            !(player.x == nx && player.y == ny))
            continue;

        // Attack player
        if (player.alive && player.x == nx && player.y == ny)
        {
            if (player.hp > 0)
                player.hp--;
            if (player.hp <= 0)
            {
                player.hp    = 0;
                player.alive = false;
            }
            continue;
        }

        // Move
        m->x = nx;
        m->y = ny;
    }
}

// --------------------------------------------------------------------
// UI & rendering
// --------------------------------------------------------------------

static void draw_ui(void)
{
    int ui_y = MAP_H * TILE_SIZE;
    int ui_h = SCREEN_HEIGHT - ui_y;

    // Clear UI region
    fill_rect(0, ui_y, SCREEN_WIDTH, ui_h, RGB15(0, 0, 0));

    // HP bar
    u16 full_col  = RGB15(0, 28, 0);
    u16 empty_col = RGB15(8,  0, 0);

    int x0    = 4;
    int y0    = ui_y + 4;
    int box_w = 8;
    int box_h = ui_h - 8;
    if (box_h < 4) box_h = 4;

    for (int i = 0; i < PLAYER_MAX_HP; i++)
    {
        u16 c  = (i < player.hp) ? full_col : empty_col;
        int bx = x0 + i * (box_w + 2);
        fill_rect(bx, y0, box_w, box_h, c);
    }

    // If dead, overlay a solid red banner as a simple "GAME OVER" cue
    if (!player.alive)
    {
        fill_rect(0, ui_y, SCREEN_WIDTH, ui_h, RGB15(20, 0, 0));
    }
}

static void draw_game(void)
{
    // Draw map
    for (int y = 0; y < MAP_H; y++)
    {
        for (int x = 0; x < MAP_W; x++)
        {
            u16 col;
            if (map_data[y][x] == TILE_WALL)
                col = RGB15(6, 6, 6);   // walls: dark gray
            else
                col = RGB15(1, 1, 1);   // floor: almost black

            draw_tile(x, y, col);
        }
    }

    // Draw monsters
    for (int i = 0; i < MAX_MONSTERS; i++)
    {
        Entity *m = &monsters[i];
        if (!m->alive)
            continue;
        u16 col = RGB15(28, 0, 0);     // red
        draw_tile(m->x, m->y, col);
    }

    // Draw player
    if (player.alive)
    {
        u16 col = RGB15(0, 28, 0);     // green
        draw_tile(player.x, player.y, col);
    }
    else
    {
        u16 col = RGB15(8, 0, 8);      // purple-ish for corpse
        draw_tile(player.x, player.y, col);
    }

    // UI at bottom
    draw_ui();
}

// --------------------------------------------------------------------
// Main
// --------------------------------------------------------------------

int main(void)
{
    // Mode 3, BG2 on
    REG_DISPCNT = DCNT_MODE3 | DCNT_BG2;

    // Seed RNG (you can replace this with something more variable)
    rng_state = 123456789;

    generate_dungeon();

    while (1)
    {
        vsync();
        key_poll();

        if (!player.alive)
        {
            // Press START to restart after death
            if (key_hit(KEY_START))
            {
                generate_dungeon();
            }
        }
        else
        {
            int dx = 0, dy = 0;

            if (key_hit(KEY_UP))       dy = -1;
            else if (key_hit(KEY_DOWN))  dy =  1;
            else if (key_hit(KEY_LEFT))  dx = -1;
            else if (key_hit(KEY_RIGHT)) dx =  1;

            // Move & then monsters move
            if (dx != 0 || dy != 0)
            {
                try_move_player(dx, dy);
                monsters_turn();
            }

            // Wait a turn with A (monsters act, player doesn't move)
            if (key_hit(KEY_A))
            {
                monsters_turn();
            }

            // New floor when all monsters are dead
            if (player.alive && alive_monsters == 0)
            {
                generate_dungeon();
            }
        }

        draw_game();
    }

    return 0;
}
