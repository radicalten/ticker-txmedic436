// main.c - single-file GBA “Master Dungeon”-style tiny dungeon crawler using tonc
//
// Features:
// - Random “carved” dungeon per floor, stairs to descend
// - Turn-based movement/combat, simple monster AI, potions
// - Fog-of-war (seen vs currently visible) with line-of-sight
// - Mode 3 bitmap rendering + tiny built-in 5x7 UI font
//
// Build (typical devkitARM + tonc setup):
// - Put this in a project that links against tonclib (or has tonc.h available).
// - Compile as a normal GBA ROM.
//
// References (hardware + tonc docs):
// - GBA video modes / Mode 3 framebuffer: https://problemkaputt.de/gbatek.htm
// - Tonc programming guide: https://www.coranac.com/tonc/text/

#include <tonc.h>
#include <stdbool.h>
#include <string.h>

// ---------------------------- Config ---------------------------------

#define SCR_W 240
#define SCR_H 160

#define CELL     8
#define MAP_W    30
#define MAP_H    18
#define UI_H     16
#define UI_Y     (MAP_H*CELL)

#define MAX_MON  14
#define MAX_POT  6

#define FOV_R    7

// ---------------------------- Colors ---------------------------------

static inline u16 rgb(u32 r, u32 g, u32 b) { return RGB15(r,g,b); }

enum {
	C_BG      = 0,
	C_FLOOR   = 1,
	C_WALL    = 2,
	C_PLAYER  = 3,
	C_MON     = 4,
	C_POT     = 5,
	C_STAIRS  = 6,
	C_UI_BG   = 7,
	C_UI_FG   = 8,
	C_DARKEN  = 9
};

static u16 g_pal[16];

// ---------------------------- Map / Entities --------------------------

typedef enum : u8 { T_WALL=0, T_FLOOR=1 } Tile;

typedef struct {
	s8 x, y;
	s8 hp;
	bool alive;
} Monster;

typedef struct {
	s8 x, y;
	bool alive;
} Potion;

static Tile    g_map[MAP_W*MAP_H];
static u8      g_seen[MAP_W*MAP_H];
static u8      g_vis[MAP_W*MAP_H];

static s8      g_px, g_py;
static s8      g_pHP, g_pMaxHP;
static u8      g_pots;
static u8      g_depth;

static s8      g_stx, g_sty;

static Monster g_mon[MAX_MON];
static Potion  g_pot[MAX_POT];

static char    g_msg[64];

// ---------------------------- RNG ------------------------------------

static u32 g_rng = 0x12345678u;

static inline u32 rng_u32(void)
{
	// xorshift32
	u32 x = g_rng;
	x ^= x << 13;
	x ^= x >> 17;
	x ^= x << 5;
	g_rng = x;
	return x;
}

static inline u32 rng_range(u32 n)
{
	// uniform enough for small n in a tiny game
	return (n == 0) ? 0 : (rng_u32() % n);
}

// ---------------------------- GBA helpers ----------------------------

static inline void vsync(void)
{
	while(REG_VCOUNT >= 160) {}
	while(REG_VCOUNT < 160) {}
}

static inline int idx(int x, int y) { return y*MAP_W + x; }

static inline bool in_bounds(int x, int y)
{
	return (x >= 0 && x < MAP_W && y >= 0 && y < MAP_H);
}

static inline bool is_wall(int x, int y)
{
	return !in_bounds(x,y) || (g_map[idx(x,y)] == T_WALL);
}

static int mon_at(int x, int y)
{
	for(int i=0;i<MAX_MON;i++)
		if(g_mon[i].alive && g_mon[i].x==x && g_mon[i].y==y)
			return i;
	return -1;
}

static int pot_at(int x, int y)
{
	for(int i=0;i<MAX_POT;i++)
		if(g_pot[i].alive && g_pot[i].x==x && g_pot[i].y==y)
			return i;
	return -1;
}

static bool blocked_for_mon(int x, int y)
{
	if(!in_bounds(x,y)) return true;
	if(g_map[idx(x,y)] == T_WALL) return true;
	if(x==g_px && y==g_py) return true;
	if(mon_at(x,y) >= 0) return true;
	return false;
}

static bool blocked_for_player(int x, int y)
{
	if(!in_bounds(x,y)) return true;
	if(g_map[idx(x,y)] == T_WALL) return true;
	return false;
}

// ---------------------------- Tiny 5x7 font --------------------------
// Covers: space, 0-9, A-Z, ':', '/', '-', '.'

typedef struct { char c; u8 rows[7]; } Glyph;

// Each row: 5 bits used (MSB on left is bit 4).
static const Glyph g_font[] = {
	{' ', {0,0,0,0,0,0,0}},
	{'.', {0,0,0,0,0,0,0x04}},
	{'-', {0,0,0,0x1F,0,0,0}},
	{':', {0,0x04,0,0,0x04,0,0}},
	{'/', {0x01,0x02,0x04,0x08,0x10,0,0}},

	{'0', {0x0E,0x11,0x13,0x15,0x19,0x11,0x0E}},
	{'1', {0x04,0x0C,0x04,0x04,0x04,0x04,0x0E}},
	{'2', {0x0E,0x11,0x01,0x06,0x08,0x10,0x1F}},
	{'3', {0x1F,0x02,0x04,0x02,0x01,0x11,0x0E}},
	{'4', {0x02,0x06,0x0A,0x12,0x1F,0x02,0x02}},
	{'5', {0x1F,0x10,0x1E,0x01,0x01,0x11,0x0E}},
	{'6', {0x06,0x08,0x10,0x1E,0x11,0x11,0x0E}},
	{'7', {0x1F,0x01,0x02,0x04,0x08,0x08,0x08}},
	{'8', {0x0E,0x11,0x11,0x0E,0x11,0x11,0x0E}},
	{'9', {0x0E,0x11,0x11,0x0F,0x01,0x02,0x0C}},

	{'A', {0x0E,0x11,0x11,0x1F,0x11,0x11,0x11}},
	{'B', {0x1E,0x11,0x11,0x1E,0x11,0x11,0x1E}},
	{'C', {0x0E,0x11,0x10,0x10,0x10,0x11,0x0E}},
	{'D', {0x1C,0x12,0x11,0x11,0x11,0x12,0x1C}},
	{'E', {0x1F,0x10,0x10,0x1E,0x10,0x10,0x1F}},
	{'F', {0x1F,0x10,0x10,0x1E,0x10,0x10,0x10}},
	{'G', {0x0E,0x11,0x10,0x17,0x11,0x11,0x0F}},
	{'H', {0x11,0x11,0x11,0x1F,0x11,0x11,0x11}},
	{'I', {0x0E,0x04,0x04,0x04,0x04,0x04,0x0E}},
	{'J', {0x07,0x02,0x02,0x02,0x02,0x12,0x0C}},
	{'K', {0x11,0x12,0x14,0x18,0x14,0x12,0x11}},
	{'L', {0x10,0x10,0x10,0x10,0x10,0x10,0x1F}},
	{'M', {0x11,0x1B,0x15,0x15,0x11,0x11,0x11}},
	{'N', {0x11,0x19,0x15,0x13,0x11,0x11,0x11}},
	{'O', {0x0E,0x11,0x11,0x11,0x11,0x11,0x0E}},
	{'P', {0x1E,0x11,0x11,0x1E,0x10,0x10,0x10}},
	{'Q', {0x0E,0x11,0x11,0x11,0x15,0x12,0x0D}},
	{'R', {0x1E,0x11,0x11,0x1E,0x14,0x12,0x11}},
	{'S', {0x0F,0x10,0x10,0x0E,0x01,0x01,0x1E}},
	{'T', {0x1F,0x04,0x04,0x04,0x04,0x04,0x04}},
	{'U', {0x11,0x11,0x11,0x11,0x11,0x11,0x0E}},
	{'V', {0x11,0x11,0x11,0x11,0x11,0x0A,0x04}},
	{'W', {0x11,0x11,0x11,0x15,0x15,0x1B,0x11}},
	{'X', {0x11,0x11,0x0A,0x04,0x0A,0x11,0x11}},
	{'Y', {0x11,0x11,0x0A,0x04,0x04,0x04,0x04}},
	{'Z', {0x1F,0x01,0x02,0x04,0x08,0x10,0x1F}},
};

static const u8* glyph_rows(char c)
{
	for(u32 i=0;i<sizeof(g_font)/sizeof(g_font[0]);i++)
		if(g_font[i].c == c) return g_font[i].rows;
	return g_font[0].rows; // space
}

// ---------------------------- Drawing --------------------------------

static inline void pset(int x, int y, u16 c)
{
	((u16*)vid_mem)[y*SCR_W + x] = c;
}

static void fill_rect(int x, int y, int w, int h, u16 c)
{
	if(w<=0 || h<=0) return;
	if(x<0){ w+=x; x=0; }
	if(y<0){ h+=y; y=0; }
	if(x+w>SCR_W) w=SCR_W-x;
	if(y+h>SCR_H) h=SCR_H-y;

	u16* dst = (u16*)vid_mem + y*SCR_W + x;
	for(int j=0;j<h;j++)
	{
		for(int i=0;i<w;i++) dst[i]=c;
		dst += SCR_W;
	}
}

static void draw_char_5x7(int x, int y, char ch, u16 fg)
{
	const u8* rows = glyph_rows(ch);
	for(int ry=0; ry<7; ry++)
	{
		u8 bits = rows[ry];
		for(int rx=0; rx<5; rx++)
		{
			if(bits & (1<<(4-rx)))
				pset(x+rx, y+ry, fg);
		}
	}
}

static void draw_text(int x, int y, const char* s, u16 fg)
{
	for(int i=0; s[i]; i++)
	{
		char ch = s[i];
		if(ch >= 'a' && ch <= 'z') ch = (char)(ch - 'a' + 'A');
		draw_char_5x7(x + i*6, y, ch, fg);
	}
}

static void draw_cell_base(int cx, int cy, u16 col)
{
	fill_rect(cx*CELL, cy*CELL, CELL, CELL, col);
}

static void draw_cell_marker(int cx, int cy, u16 col)
{
	// small 4x4 marker in the center
	int x = cx*CELL + 2;
	int y = cy*CELL + 2;
	fill_rect(x, y, 4, 4, col);
}

static u16 darken(u16 col)
{
	// crude darken: scale components down (RGB15)
	u32 r = (col     ) & 31;
	u32 g = (col >> 5) & 31;
	u32 b = (col >>10) & 31;
	r = (r*2)/5;
	g = (g*2)/5;
	b = (b*2)/5;
	return RGB15(r,g,b);
}

// ---------------------------- FOV / LOS ------------------------------

static bool los_clear(int x0, int y0, int x1, int y1)
{
	// Bresenham line; blocks if it hits a wall before reaching target.
	int dx = (x1>x0) ? (x1-x0) : (x0-x1);
	int sx = (x0<x1) ? 1 : -1;
	int dy = (y1>y0) ? (y0-y1) : (y1-y0); // negative
	int sy = (y0<y1) ? 1 : -1;
	int err = dx + dy;

	int x=x0, y=y0;
	for(;;)
	{
		if(x==x1 && y==y1) return true;
		// skip checking starting cell; do check intermediate cells
		if(!(x==x0 && y==y0) && is_wall(x,y)) return false;

		int e2 = 2*err;
		if(e2 >= dy) { err += dy; x += sx; }
		if(e2 <= dx) { err += dx; y += sy; }
		if(!in_bounds(x,y)) return false;
	}
}

static void compute_fov(void)
{
	memset(g_vis, 0, sizeof(g_vis));

	for(int y=g_py-FOV_R; y<=g_py+FOV_R; y++)
	for(int x=g_px-FOV_R; x<=g_px+FOV_R; x++)
	{
		if(!in_bounds(x,y)) continue;
		int dx=x-g_px, dy=y-g_py;
		if(dx*dx + dy*dy > FOV_R*FOV_R) continue;

		if(los_clear(g_px,g_py,x,y))
		{
			g_vis[idx(x,y)] = 1;
			g_seen[idx(x,y)] = 1;
		}
	}
}

// ---------------------------- Generation -----------------------------

static void set_msg(const char* s)
{
	strncpy(g_msg, s, sizeof(g_msg)-1);
	g_msg[sizeof(g_msg)-1] = 0;
}

static void clear_level_entities(void)
{
	for(int i=0;i<MAX_MON;i++) g_mon[i].alive=false;
	for(int i=0;i<MAX_POT;i++) g_pot[i].alive=false;
}

static void carve_room(int cx, int cy, int rw, int rh)
{
	for(int y=cy-rh; y<=cy+rh; y++)
	for(int x=cx-rw; x<=cx+rw; x++)
	{
		if(x<=0 || y<=0 || x>=MAP_W-1 || y>=MAP_H-1) continue;
		g_map[idx(x,y)] = T_FLOOR;
	}
}

static void generate_level(u8 depth)
{
	clear_level_entities();
	memset(g_seen, 0, sizeof(g_seen));
	memset(g_vis,  0, sizeof(g_vis));

	// start all walls
	for(int i=0;i<MAP_W*MAP_H;i++) g_map[i]=T_WALL;

	// random “drunkard walk” carve
	int x = MAP_W/2;
	int y = MAP_H/2;
	g_map[idx(x,y)] = T_FLOOR;

	int steps = (MAP_W*MAP_H) * (6 + depth/2);
	for(int i=0;i<steps;i++)
	{
		// occasionally carve a room blob
		if((rng_u32() & 63u) == 0)
			carve_room(x, y, 2 + (int)rng_range(2), 1 + (int)rng_range(2));

		u32 dir = rng_range(4);
		int nx=x, ny=y;
		if(dir==0) nx++;
		if(dir==1) nx--;
		if(dir==2) ny++;
		if(dir==3) ny--;

		if(nx<=1 || ny<=1 || nx>=MAP_W-2 || ny>=MAP_H-2) continue;
		x=nx; y=ny;
		g_map[idx(x,y)] = T_FLOOR;
	}

	// ensure border walls
	for(int xx=0;xx<MAP_W;xx++){ g_map[idx(xx,0)]=T_WALL; g_map[idx(xx,MAP_H-1)]=T_WALL; }
	for(int yy=0;yy<MAP_H;yy++){ g_map[idx(0,yy)]=T_WALL; g_map[idx(MAP_W-1,yy)]=T_WALL; }

	// place player near center on a floor
	int bestX=MAP_W/2, bestY=MAP_H/2;
	for(int rr=0; rr<1000; rr++)
	{
		int tx = (int)rng_range(MAP_W-2)+1;
		int ty = (int)rng_range(MAP_H-2)+1;
		if(g_map[idx(tx,ty)] == T_FLOOR)
		{
			bestX=tx; bestY=ty;
			if((tx-(MAP_W/2))*(tx-(MAP_W/2)) + (ty-(MAP_H/2))*(ty-(MAP_H/2)) < 16)
				break;
		}
	}
	g_px = (s8)bestX; g_py = (s8)bestY;

	// place stairs far-ish from player
	int stx=g_px, sty=g_py;
	int bestD=-1;
	for(int k=0;k<400;k++)
	{
		int tx = (int)rng_range(MAP_W-2)+1;
		int ty = (int)rng_range(MAP_H-2)+1;
		if(g_map[idx(tx,ty)] != T_FLOOR) continue;
		int d = (tx-g_px)*(tx-g_px) + (ty-g_py)*(ty-g_py);
		if(d > bestD)
		{
			bestD=d; stx=tx; sty=ty;
		}
	}
	g_stx=(s8)stx; g_sty=(s8)sty;

	// place potions
	u32 potCount = 1 + depth/2;
	if(potCount > MAX_POT) potCount = MAX_POT;
	for(u32 i=0;i<potCount;i++)
	{
		for(int tries=0;tries<400;tries++)
		{
			int tx = (int)rng_range(MAP_W-2)+1;
			int ty = (int)rng_range(MAP_H-2)+1;
			if(g_map[idx(tx,ty)] != T_FLOOR) continue;
			if((tx==g_px && ty==g_py) || (tx==g_stx && ty==g_sty)) continue;
			if(pot_at(tx,ty)>=0) continue;
			g_pot[i].x=(s8)tx; g_pot[i].y=(s8)ty; g_pot[i].alive=true;
			break;
		}
	}

	// place monsters
	u32 monCount = 4 + depth;
	if(monCount > MAX_MON) monCount = MAX_MON;
	for(u32 i=0;i<monCount;i++)
	{
		for(int tries=0;tries<600;tries++)
		{
			int tx = (int)rng_range(MAP_W-2)+1;
			int ty = (int)rng_range(MAP_H-2)+1;
			if(g_map[idx(tx,ty)] != T_FLOOR) continue;
			if((tx==g_px && ty==g_py) || (tx==g_stx && ty==g_sty)) continue;
			if(mon_at(tx,ty)>=0) continue;

			g_mon[i].x=(s8)tx; g_mon[i].y=(s8)ty;
			g_mon[i].hp=(s8)(2 + depth/2);
			g_mon[i].alive=true;
			break;
		}
	}

	set_msg("FIND THE STAIRS. A=USE POTION / DESCEND");
	compute_fov();
}

// ---------------------------- Rendering ------------------------------

static void render_world(void)
{
	for(int y=0;y<MAP_H;y++)
	for(int x=0;x<MAP_W;x++)
	{
		int k = idx(x,y);

		if(!g_seen[k])
		{
			draw_cell_base(x,y, g_pal[C_BG]);
			continue;
		}

		u16 base = (g_map[k]==T_WALL) ? g_pal[C_WALL] : g_pal[C_FLOOR];
		if(!g_vis[k]) base = darken(base);
		draw_cell_base(x,y, base);
	}

	// Draw stairs/potions/monsters only if seen (or visible). For clarity: show if seen.
	if(g_seen[idx(g_stx,g_sty)])
	{
		u16 c = g_vis[idx(g_stx,g_sty)] ? g_pal[C_STAIRS] : darken(g_pal[C_STAIRS]);
		draw_cell_marker(g_stx,g_sty,c);
	}

	for(int i=0;i<MAX_POT;i++)
	{
		if(!g_pot[i].alive) continue;
		int x=g_pot[i].x, y=g_pot[i].y;
		if(!g_seen[idx(x,y)]) continue;
		u16 c = g_vis[idx(x,y)] ? g_pal[C_POT] : darken(g_pal[C_POT]);
		draw_cell_marker(x,y,c);
	}

	for(int i=0;i<MAX_MON;i++)
	{
		if(!g_mon[i].alive) continue;
		int x=g_mon[i].x, y=g_mon[i].y;
		if(!g_seen[idx(x,y)]) continue;
		u16 c = g_vis[idx(x,y)] ? g_pal[C_MON] : darken(g_pal[C_MON]);
		draw_cell_marker(x,y,c);
	}

	// Player always drawn
	draw_cell_marker(g_px,g_py,g_pal[C_PLAYER]);
}

static void render_ui(void)
{
	fill_rect(0, UI_Y, SCR_W, UI_H, g_pal[C_UI_BG]);

	char line1[64];
	// Example: "HP: 5/7  POT: 2  D: 3"
	snprintf(line1, sizeof(line1), "HP:%d/%d  POT:%d  D:%d",
	         (int)g_pHP, (int)g_pMaxHP, (int)g_pots, (int)g_depth);

	draw_text(2, UI_Y+1, line1, g_pal[C_UI_FG]);

	// message line (truncate)
	char line2[41];
	strncpy(line2, g_msg, 40);
	line2[40]=0;
	draw_text(2, UI_Y+9, line2, g_pal[C_UI_FG]);
}

// ---------------------------- Gameplay --------------------------------

static void damage_player(int dmg)
{
	g_pHP -= (s8)dmg;
	if(g_pHP <= 0)
	{
		g_pHP = 0;
		set_msg("YOU DIED. PRESS START.");
	}
}

static void attack_monster(int mi)
{
	if(mi<0) return;
	g_mon[mi].hp -= 1;
	if(g_mon[mi].hp <= 0)
	{
		g_mon[mi].alive=false;
		set_msg("YOU SLAY THE MONSTER.");
	}
	else
	{
		set_msg("YOU HIT IT.");
	}
}

static void monster_turns(void)
{
	if(g_pHP <= 0) return;

	for(int i=0;i<MAX_MON;i++)
	{
		if(!g_mon[i].alive) continue;

		int mx=g_mon[i].x, my=g_mon[i].y;
		int dx=g_px-mx, dy=g_py-my;
		int adx = dx<0 ? -dx : dx;
		int ady = dy<0 ? -dy : dy;

		// attack if adjacent
		if(adx + ady == 1)
		{
			damage_player(1);
			if(g_pHP<=0) return;
			continue;
		}

		// move toward player (simple greedy)
		int sx=0, sy=0;
		if(adx >= ady) sx = (dx>0) ? 1 : (dx<0 ? -1 : 0);
		else           sy = (dy>0) ? 1 : (dy<0 ? -1 : 0);

		int nx=mx+sx, ny=my+sy;

		// if primary blocked, try alternate axis
		if(blocked_for_mon(nx,ny))
		{
			nx=mx; ny=my;
			if(sx!=0)
				ny = my + ((dy>0)?1:(dy<0?-1:0));
			else
				nx = mx + ((dx>0)?1:(dx<0?-1:0));
		}

		// if still blocked, random wiggle
		if(blocked_for_mon(nx,ny))
		{
			for(int t=0;t<4;t++)
			{
				u32 dir=rng_range(4);
				nx=mx + (dir==0) - (dir==1);
				ny=my + (dir==2) - (dir==3);
				if(!blocked_for_mon(nx,ny)) break;
				nx=mx; ny=my;
			}
		}

		if(!blocked_for_mon(nx,ny))
		{
			g_mon[i].x=(s8)nx;
			g_mon[i].y=(s8)ny;
		}
	}
}

static void try_pickup(void)
{
	int pi = pot_at(g_px,g_py);
	if(pi>=0 && g_pot[pi].alive)
	{
		g_pot[pi].alive=false;
		if(g_pots < 9) g_pots++;
		set_msg("PICKED UP A POTION.");
	}
}

static void use_potion(void)
{
	if(g_pHP<=0) return;

	if(g_pots==0)
	{
		set_msg("NO POTIONS.");
		return;
	}
	if(g_pHP >= g_pMaxHP)
	{
		set_msg("HP IS FULL.");
		return;
	}

	g_pots--;
	g_pHP += 3;
	if(g_pHP > g_pMaxHP) g_pHP = g_pMaxHP;
	set_msg("YOU DRINK A POTION.");
}

static bool player_move_or_attack(int dx, int dy)
{
	if(g_pHP<=0) return false;

	int nx = g_px + dx;
	int ny = g_py + dy;

	if(blocked_for_player(nx,ny))
	{
		set_msg("BUMP.");
		return false;
	}

	int mi = mon_at(nx,ny);
	if(mi>=0)
	{
		attack_monster(mi);
		return true; // spent turn
	}

	g_px = (s8)nx;
	g_py = (s8)ny;
	try_pickup();

	if(g_px==g_stx && g_py==g_sty)
		set_msg("STAIRS HERE. PRESS A TO DESCEND.");

	return true;
}

static void descend(void)
{
	if(g_pHP<=0) return;

	if(!(g_px==g_stx && g_py==g_sty))
	{
		use_potion();
		return;
	}

	// go down
	g_depth++;
	// small heal on descend to keep things moving
	if(g_pHP < g_pMaxHP) g_pHP++;
	set_msg("DESCENDING...");
	generate_level(g_depth);
}

static void new_game(void)
{
	g_depth = 1;
	g_pMaxHP = 7;
	g_pHP = g_pMaxHP;
	g_pots = 1;

	// seed RNG with a little bit of entropy from VCOUNT (not great, but fine)
	g_rng ^= ((u32)REG_VCOUNT << 16) ^ 0xA5A5C3C3u;

	generate_level(g_depth);
}

// ---------------------------- Main ------------------------------------

int main(void)
{
	// Mode 3 bitmap
	REG_DISPCNT = DCNT_MODE3 | DCNT_BG2;

	// palette-ish constants (Mode 3 doesn't use BG palette, but we store colors here)
	g_pal[C_BG]      = rgb(0,0,0);
	g_pal[C_FLOOR]   = rgb(6,6,7);
	g_pal[C_WALL]    = rgb(2,2,3);
	g_pal[C_PLAYER]  = rgb(0,31,0);
	g_pal[C_MON]     = rgb(31,0,0);
	g_pal[C_POT]     = rgb(0,18,31);
	g_pal[C_STAIRS]  = rgb(31,31,0);
	g_pal[C_UI_BG]   = rgb(1,1,2);
	g_pal[C_UI_FG]   = rgb(31,31,31);

	fill_rect(0,0,SCR_W,SCR_H,g_pal[C_BG]);

	new_game();

	u16 prev=0;

	for(;;)
	{
		vsync();

		u16 keys = (u16)(~REG_KEYINPUT & 0x03FF);
		u16 hit  = (u16)(keys & ~prev);
		prev = keys;

		bool acted = false;

		if(hit & KEY_START)
		{
			new_game();
			acted = false;
		}

		if(g_pHP <= 0)
		{
			// dead: only START matters
			render_world();
			render_ui();
			continue;
		}

		if(hit & KEY_UP)    acted = player_move_or_attack(0,-1);
		else if(hit & KEY_DOWN)  acted = player_move_or_attack(0, 1);
		else if(hit & KEY_LEFT)  acted = player_move_or_attack(-1,0);
		else if(hit & KEY_RIGHT) acted = player_move_or_attack(1, 0);
		else if(hit & KEY_A)     { descend(); acted = true; }
		else if(hit & KEY_B)     { set_msg("WAIT."); acted = true; }

		if(acted)
		{
			compute_fov();
			monster_turns();
			compute_fov();
		}

		render_world();
		render_ui();
	}

	return 0;
}
