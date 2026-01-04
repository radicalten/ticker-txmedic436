// main.c - Tiny Fantasy (original mini JRPG) for GBA + tonc (single source file)
// Build requires tonc (libtonc) linked in, plus devkitARM.
//
// Controls:
//  D-Pad  : Move on world map / menu cursor
//  A      : Confirm
//  B      : Back (in battle: treated as "Run" shortcut)
//  START  : Start game from title

#include <tonc.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <stdbool.h>
#include <stdint.h>

#define SCR_W 240
#define SCR_H 160

// Mode 3 framebuffer (RGB565)
static volatile uint16_t* const VRAM = (volatile uint16_t*)0x06000000;

static inline uint16_t rgb15(int r, int g, int b) { return RGB15(r, g, b); }

static void pset(int x, int y, uint16_t c)
{
	if((unsigned)x >= SCR_W || (unsigned)y >= SCR_H) return;
	VRAM[y*SCR_W + x] = c;
}

static void rect_fill(int x, int y, int w, int h, uint16_t c)
{
	if(w <= 0 || h <= 0) return;

	int x0 = x < 0 ? 0 : x;
	int y0 = y < 0 ? 0 : y;
	int x1 = x + w; if(x1 > SCR_W) x1 = SCR_W;
	int y1 = y + h; if(y1 > SCR_H) y1 = SCR_H;

	for(int yy=y0; yy<y1; yy++)
	{
		volatile uint16_t* row = &VRAM[yy*SCR_W + x0];
		for(int xx=x0; xx<x1; xx++)
			*row++ = c;
	}
}

static void hline(int x, int y, int w, uint16_t c)
{
	rect_fill(x, y, w, 1, c);
}
static void vline(int x, int y, int h, uint16_t c)
{
	rect_fill(x, y, 1, h, c);
}

static void frame_rect(int x, int y, int w, int h, uint16_t c)
{
	hline(x, y, w, c);
	hline(x, y+h-1, w, c);
	vline(x, y, h, c);
	vline(x+w-1, y, h, c);
}

static char g_textbuf[256];

static void tprint(int x, int y, const char* fmt, ...)
{
	va_list va;
	va_start(va, fmt);
	vsnprintf(g_textbuf, sizeof(g_textbuf), fmt, va);
	va_end(va);

	tte_set_pos(x, y);
	tte_write(g_textbuf);
}

static uint32_t g_rng = 0x12345678u;
static uint32_t rng_u32(void)
{
	// Simple LCG (deterministic; good enough for gameplay randomness)
	g_rng = g_rng*1664525u + 1013904223u;
	return g_rng;
}
static int rng_range(int n)
{
	// 0..n-1
	if(n <= 0) return 0;
	return (int)((rng_u32() >> 8) % (uint32_t)n);
}

typedef struct Actor
{
	int lvl;
	int hp, maxhp;
	int atk, def;
	int exp;
} Actor;

typedef enum GameState
{
	ST_TITLE=0,
	ST_WORLD,
	ST_BATTLE,
	ST_GAMEOVER
} GameState;

typedef enum BattlePhase
{
	BP_MENU=0,
	BP_PLAYER_MSG,
	BP_ENEMY_MSG,
	BP_VICTORY,
	BP_DEFEAT
} BattlePhase;

static GameState g_state = ST_TITLE;

static Actor g_hero;
static Actor g_enemy;

static BattlePhase g_bphase = BP_MENU;
static int g_menu_idx = 0;         // 0=Attack, 1=Run
static char g_bmsg[96] = {0};      // battle message
static char g_wmsg[96] = {0};      // world message

// -------------------- Map --------------------
#define TILE 16
#define MAP_W 15
#define MAP_H 7
#define UI_Y (MAP_H*TILE)
#define UI_H (SCR_H-UI_Y)

static const char* g_map[MAP_H] =
{
	"###############",
	"#.....~....E..#",
	"#.###.~~~..##.#",
	"#...#..~...#..#",
	"#...####...#..#",
	"#.............#",
	"###############",
};

static int g_px = 2, g_py = 1; // player tile position
static bool g_dirty = true;

static bool is_blocking(char t)
{
	// walls and water block
	return (t=='#' || t=='~');
}

static uint16_t tile_color(char t)
{
	switch(t)
	{
	case '#': return rgb15(8, 8, 8);     // wall
	case '~': return rgb15(0, 4, 18);    // water
	case 'E': return rgb15(22, 20, 0);   // exit
	case '.': default: return rgb15(0, 16, 0); // grass
	}
}

static void ui_box(int x, int y, int w, int h)
{
	rect_fill(x, y, w, h, rgb15(0,0,0));
	frame_rect(x, y, w, h, rgb15(31,31,31));
}

static void draw_title(void)
{
	rect_fill(0,0,SCR_W,SCR_H, rgb15(0,0,0));
	ui_box(12, 18, SCR_W-24, 124);

	tprint(28, 34,  "TINY FANTASY");
	tprint(28, 52,  "A tiny original JRPG demo");
	tprint(28, 70,  "World + random encounters");
	tprint(28, 88,  "Turn-based battles");
	tprint(28, 112, "Press START");
}

static void draw_world(void)
{
	// Background
	rect_fill(0,0,SCR_W,UI_Y, rgb15(0,12,0));

	// Draw tiles
	for(int y=0; y<MAP_H; y++)
	{
		for(int x=0; x<MAP_W; x++)
		{
			char t = g_map[y][x];
			uint16_t c = tile_color(t);

			int sx = x*TILE;
			int sy = y*TILE;

			rect_fill(sx, sy, TILE, TILE, c);
			frame_rect(sx, sy, TILE, TILE, rgb15(0,0,0));
		}
	}

	// Draw player
	{
		int sx = g_px*TILE;
		int sy = g_py*TILE;
		rect_fill(sx+3, sy+3, TILE-6, TILE-6, rgb15(28, 0, 28));
		frame_rect(sx+3, sy+3, TILE-6, TILE-6, rgb15(31,31,31));
	}

	// UI
	ui_box(0, UI_Y, SCR_W, UI_H);
	tprint(8, UI_Y+6, "LV %d  HP %d/%d  ATK %d  DEF %d  EXP %d",
		g_hero.lvl, g_hero.hp, g_hero.maxhp, g_hero.atk, g_hero.def, g_hero.exp);

	tprint(8, UI_Y+24, "%s", g_wmsg[0] ? g_wmsg : "Explore with the D-Pad.");
}

static int calc_damage(const Actor* a, const Actor* d)
{
	int base = a->atk - d->def/2;
	int swing = rng_range(4); // 0..3
	int dmg = base + swing;
	if(dmg < 1) dmg = 1;
	return dmg;
}

static void enemy_make_for_level(int lvl)
{
	int kind = rng_range(3);
	memset(&g_enemy, 0, sizeof(g_enemy));
	g_enemy.lvl = lvl;

	// Simple variety
	if(kind == 0)
	{
		// Slime-ish
		g_enemy.maxhp = 10 + lvl*3;
		g_enemy.atk   = 3 + lvl;
		g_enemy.def   = 1 + lvl/2;
	}
	else if(kind == 1)
	{
		// Goblin-ish
		g_enemy.maxhp = 14 + lvl*4;
		g_enemy.atk   = 4 + lvl*2;
		g_enemy.def   = 1 + lvl/3;
	}
	else
	{
		// Wolf-ish
		g_enemy.maxhp = 12 + lvl*3;
		g_enemy.atk   = 5 + lvl*2;
		g_enemy.def   = 0 + lvl/3;
	}

	g_enemy.hp = g_enemy.maxhp;
	g_enemy.exp = 6 + lvl*4;
}

static void battle_start(void)
{
	enemy_make_for_level(g_hero.lvl);
	g_bphase = BP_MENU;
	g_menu_idx = 0;
	snprintf(g_bmsg, sizeof(g_bmsg), "An enemy appears!");
	g_state = ST_BATTLE;
	g_dirty = true;
}

static void level_check_and_apply(void)
{
	// simple threshold: need lvl*12 exp to level up
	int need = g_hero.lvl * 12;
	while(g_hero.exp >= need)
	{
		g_hero.exp -= need;
		g_hero.lvl += 1;

		int hp_up  = 3 + rng_range(3);  // 3..5
		int atk_up = 1 + rng_range(2);  // 1..2
		int def_up = 1 + (rng_range(2)); // 1..2

		g_hero.maxhp += hp_up;
		g_hero.atk += atk_up;
		g_hero.def += def_up;
		g_hero.hp = g_hero.maxhp;

		need = g_hero.lvl * 12;
	}
}

static void draw_battle(void)
{
	rect_fill(0,0,SCR_W,SCR_H, rgb15(0,0,6));

	// Enemy "sprite"
	rect_fill(148, 34, 70, 54, rgb15(18, 0, 0));
	frame_rect(148, 34, 70, 54, rgb15(31,31,31));
	tprint(154, 40, "ENEMY LV %d", g_enemy.lvl);
	tprint(154, 56, "HP %d/%d", g_enemy.hp, g_enemy.maxhp);

	// Hero panel
	ui_box(10, 10, 120, 40);
	tprint(16, 16, "HERO LV %d", g_hero.lvl);
	tprint(16, 30, "HP %d/%d", g_hero.hp, g_hero.maxhp);

	// Bottom UI
	ui_box(0, UI_Y, SCR_W, UI_H);
	tprint(8, UI_Y+6, "%s", g_bmsg);

	if(g_bphase == BP_MENU)
	{
		int mx = 140, my = UI_Y+24;
		tprint(mx, my,     "%s Attack", (g_menu_idx==0)?">":" ");
		tprint(mx, my+12,  "%s Run",    (g_menu_idx==1)?">":" ");
		tprint(8,  UI_Y+24, "A: Select   B: Run");
	}
	else
	{
		tprint(8, UI_Y+24, "A: Continue");
	}
}

static void draw_gameover(void)
{
	rect_fill(0,0,SCR_W,SCR_H, rgb15(0,0,0));
	ui_box(18, 40, SCR_W-36, 80);
	tprint(36, 58, "GAME OVER");
	tprint(36, 78, "Press START to retry");
}

static void world_try_move(int dx, int dy)
{
	int nx = g_px + dx;
	int ny = g_py + dy;

	if(nx < 0 || nx >= MAP_W || ny < 0 || ny >= MAP_H)
		return;

	char t = g_map[ny][nx];
	if(is_blocking(t))
	{
		snprintf(g_wmsg, sizeof(g_wmsg), "Blocked.");
		g_dirty = true;
		return;
	}

	g_px = nx; g_py = ny;

	if(t == 'E')
	{
		snprintf(g_wmsg, sizeof(g_wmsg), "You found the exit! Keep exploring.");
		g_dirty = true;
		return;
	}

	// Random encounter chance per step
	// (about 1 in 10 steps)
	if(rng_range(10) == 0)
	{
		snprintf(g_wmsg, sizeof(g_wmsg), "Danger!...");
		g_dirty = true;
		battle_start();
		return;
	}

	// Otherwise just update UI
	snprintf(g_wmsg, sizeof(g_wmsg), "Walking...");
	g_dirty = true;
}

static void update_title(void)
{
	key_poll();

	if(key_hit(KEY_START))
	{
		// init hero
		memset(&g_hero, 0, sizeof(g_hero));
		g_hero.lvl = 1;
		g_hero.maxhp = 24;
		g_hero.hp = g_hero.maxhp;
		g_hero.atk = 6;
		g_hero.def = 3;
		g_hero.exp = 0;

		g_px = 2; g_py = 1;
		g_wmsg[0] = 0;

		g_state = ST_WORLD;
		g_dirty = true;
	}
}

static void update_world(void)
{
	key_poll();

	if(key_hit(KEY_UP))    world_try_move(0, -1);
	if(key_hit(KEY_DOWN))  world_try_move(0,  1);
	if(key_hit(KEY_LEFT))  world_try_move(-1, 0);
	if(key_hit(KEY_RIGHT)) world_try_move( 1, 0);
}

static void update_battle(void)
{
	key_poll();

	if(g_bphase == BP_MENU)
	{
		if(key_hit(KEY_UP) || key_hit(KEY_DOWN))
		{
			g_menu_idx ^= 1;
			g_dirty = true;
		}

		if(key_hit(KEY_B))
		{
			g_menu_idx = 1; // Run
			g_dirty = true;
		}

		if(key_hit(KEY_A))
		{
			if(g_menu_idx == 0)
			{
				int dmg = calc_damage(&g_hero, &g_enemy);
				g_enemy.hp -= dmg;
				if(g_enemy.hp < 0) g_enemy.hp = 0;

				snprintf(g_bmsg, sizeof(g_bmsg), "You attack for %d!", dmg);
				g_bphase = BP_PLAYER_MSG;
				g_dirty = true;
			}
			else
			{
				// Run chance
				int chance = 60 + g_hero.lvl*2; // %
				int roll = rng_range(100);
				if(roll < chance)
				{
					snprintf(g_bmsg, sizeof(g_bmsg), "You escaped!");
					g_bphase = BP_VICTORY; // reuse to exit battle
				}
				else
				{
					snprintf(g_bmsg, sizeof(g_bmsg), "Couldn't escape!");
					g_bphase = BP_PLAYER_MSG;
				}
				g_dirty = true;
			}
		}
		return;
	}

	// Message / phase advances on A
	if(!key_hit(KEY_A))
		return;

	if(g_bphase == BP_PLAYER_MSG)
	{
		// If enemy is dead, victory sequence; else enemy attacks now.
		if(g_enemy.hp <= 0)
		{
			snprintf(g_bmsg, sizeof(g_bmsg), "Defeated! +%d EXP", g_enemy.exp);
			g_hero.exp += g_enemy.exp;
			level_check_and_apply();
			g_bphase = BP_VICTORY;
			g_dirty = true;
		}
		else
		{
			int dmg = calc_damage(&g_enemy, &g_hero);
			g_hero.hp -= dmg;
			if(g_hero.hp < 0) g_hero.hp = 0;

			snprintf(g_bmsg, sizeof(g_bmsg), "Enemy hits for %d!", dmg);
			g_bphase = BP_ENEMY_MSG;
			g_dirty = true;
		}
		return;
	}

	if(g_bphase == BP_ENEMY_MSG)
	{
		if(g_hero.hp <= 0)
		{
			snprintf(g_bmsg, sizeof(g_bmsg), "You were defeated...");
			g_bphase = BP_DEFEAT;
			g_dirty = true;
		}
		else
		{
			snprintf(g_bmsg, sizeof(g_bmsg), "Choose an action.");
			g_bphase = BP_MENU;
			g_dirty = true;
		}
		return;
	}

	if(g_bphase == BP_VICTORY)
	{
		// Return to world
		snprintf(g_wmsg, sizeof(g_wmsg), "Back to the world.");
		g_state = ST_WORLD;
		g_dirty = true;
		return;
	}

	if(g_bphase == BP_DEFEAT)
	{
		g_state = ST_GAMEOVER;
		g_dirty = true;
		return;
	}
}

static void update_gameover(void)
{
	key_poll();
	if(key_hit(KEY_START))
	{
		g_state = ST_TITLE;
		g_dirty = true;
	}
}

int main(void)
{
	// Video: Mode 3 bitmap on BG2
	REG_DISPCNT = DCNT_MODE3 | DCNT_BG2;

	// Tonc text engine on mode 3 (uses libtonc's built-in font)
	tte_init_bmp_default(3);

	// Seed RNG from a somewhat variable register (VCOUNT)
	g_rng ^= (uint32_t)REG_VCOUNT * 2654435761u;

	g_dirty = true;

	while(1)
	{
		vid_vsync();

		switch(g_state)
		{
		case ST_TITLE:    update_title();    break;
		case ST_WORLD:    update_world();    break;
		case ST_BATTLE:   update_battle();   break;
		case ST_GAMEOVER: update_gameover(); break;
		}

		if(g_dirty)
		{
			// Redraw whole screen for simplicity
			switch(g_state)
			{
			case ST_TITLE:    draw_title();    break;
			case ST_WORLD:    draw_world();    break;
			case ST_BATTLE:   draw_battle();   break;
			case ST_GAMEOVER: draw_gameover(); break;
			}
			g_dirty = false;
		}
	}

	// not reached
	return 0;
}
