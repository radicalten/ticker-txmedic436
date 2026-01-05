// main.c - Single-file Mystery Dungeon-like prototype for GBA using tonc
// Controls:
//   D-Pad: move (4-dir)
//   A    : wait (consume a turn)
//   R    : use held berry (heal)
//   START: descend if standing on stairs
//
// Build: use a standard tonc/devkitARM project linking tonclib.
// (Drop this as source and compile with your usual tonc template Makefile.)

#include <tonc.h>
#include <stdbool.h>
#include <string.h>
#include <stdlib.h>

enum { MAP_W=30, MAP_H=18 };          // 30x18 playfield, bottom 2 rows are HUD (in 32x32 screenblock)
enum { HUD_Y=18 };

typedef enum { CELL_WALL=0, CELL_FLOOR=1 } Cell;

typedef struct { int x,y; int hp, maxhp; } Player;

typedef struct { int x,y; int hp; bool alive; } Mob;

typedef struct { int x,y; bool taken; } Item;

#define MAX_ENEMIES  10
#define MAX_ITEMS     8
#define FOV_R         7

// --- Tiles (BG0, 4bpp) -------------------------------------------------------
enum TileId {
	TID_UNSEEN=0,

	TID_FLOOR_V=1,
	TID_FLOOR_M=2,
	TID_WALL_V =3,
	TID_WALL_M =4,

	TID_STAIRS =5,
	TID_ITEM   =6,
	TID_ENEMY  =7,
	TID_PLAYER =8,

	TID_BLANK  =9,

	TID_HEART_F=10,
	TID_HEART_E=11,

	TID_DIGIT0 =12, // 12..21
	TID_H      =22,
	TID_P      =23,
	TID_F      =24,
	TID_I      =25,

	NUM_TILES
};

static Cell g_map[MAP_H][MAP_W];
static bool g_explored[MAP_H][MAP_W];
static bool g_visible[MAP_H][MAP_W];

static Player g_pl;
static Mob    g_mobs[MAX_ENEMIES];
static Item   g_items[MAX_ITEMS];

static int g_floor = 1;

static int g_stairs_x=1, g_stairs_y=1;

static bool g_has_berry=false;

// --- Tiny helpers ------------------------------------------------------------

static inline int iabs(int v){ return v<0 ? -v : v; }
static inline int isgn(int v){ return (v>0) - (v<0); }

static inline bool in_bounds(int x,int y){
	return (unsigned)x < MAP_W && (unsigned)y < MAP_H;
}

static inline bool is_blocking(int x,int y){
	return !in_bounds(x,y) || g_map[y][x]==CELL_WALL;
}

static u32 rng_u32(void){
	// Use tonc's qran if available; fall back to a simple LCG otherwise.
	// In standard tonclib builds, qran/sqran exist.
	#ifdef qran
		return qran();
	#else
		static u32 s=0x1234567;
		s = 1664525*s + 1013904223;
		return s;
	#endif
}

static int rng_range(int lo, int hi_inclusive){
	u32 r = rng_u32();
	int span = (hi_inclusive - lo + 1);
	return lo + (int)(r % (u32)span);
}

static inline void sb_put(u16 *sb, int x,int y, int tid){
	sb[y*32 + x] = (u16)tid;
}

// --- Tile building (no external art) -----------------------------------------

static inline u32 pack_row_4bpp(const u8 p[8]){
	u32 row=0;
	for(int i=0;i<8;i++) row |= (u32)(p[i]&0xF) << (4*i);
	return row;
}

static void tile_fill(TILE *t, u8 col){
	u32 v = 0x11111111u * (u32)(col & 0xF);
	for(int y=0;y<8;y++) t->data[y]=v;
}

static void tile_checker(TILE *t, u8 colA, u8 colB){
	for(int y=0;y<8;y++){
		u8 p[8];
		for(int x=0;x<8;x++){
			bool a = ((x^y)&1)==0;
			p[x] = a ? colA : colB;
		}
		t->data[y] = pack_row_4bpp(p);
	}
}

static void tile_glyph_5x7(TILE *t, const u8 rows7[7], u8 fg, u8 bg){
	// Center a 5x7 glyph into 8x8 tile. rows7 bits are 5-bit wide (b4..b0).
	tile_fill(t, bg);
	for(int y=0;y<7;y++){
		u8 rowBits = rows7[y] & 0x1F;
		u8 p[8]={bg,bg,bg,bg,bg,bg,bg,bg};
		for(int cx=0; cx<5; cx++){
			bool on = (rowBits >> (4-cx)) & 1;
			// place at x=1..5
			p[1+cx] = on ? fg : bg;
		}
		t->data[y] = pack_row_4bpp(p);
	}
}

static void tile_icon_heart(TILE *t, u8 col){
	tile_fill(t, 0);
	// Simple 8x8 heart shape
	const u8 m[8]={
		0b01100110,
		0b11111111,
		0b11111111,
		0b11111111,
		0b01111110,
		0b00111100,
		0b00011000,
		0b00000000
	};
	for(int y=0;y<8;y++){
		u8 p[8]={0,0,0,0,0,0,0,0};
		for(int x=0;x<8;x++){
			if((m[y]>>(7-x))&1) p[x]=col;
		}
		t->data[y]=pack_row_4bpp(p);
	}
}

static void tile_icon_player(TILE *t, u8 col){
	tile_fill(t, 0);
	// @-ish circle
	const u8 m[8]={
		0b00111100,
		0b01000010,
		0b10011001,
		0b10100101,
		0b10111101,
		0b10000001,
		0b01000010,
		0b00111100
	};
	for(int y=0;y<8;y++){
		u8 p[8]={0};
		for(int x=0;x<8;x++) if((m[y]>>(7-x))&1) p[x]=col;
		t->data[y]=pack_row_4bpp(p);
	}
}

static void tile_icon_enemy(TILE *t, u8 col){
	tile_fill(t, 0);
	// X
	const u8 m[8]={
		0b10000001,
		0b01000010,
		0b00100100,
		0b00011000,
		0b00011000,
		0b00100100,
		0b01000010,
		0b10000001
	};
	for(int y=0;y<8;y++){
		u8 p[8]={0};
		for(int x=0;x<8;x++) if((m[y]>>(7-x))&1) p[x]=col;
		t->data[y]=pack_row_4bpp(p);
	}
}

static void tile_icon_item(TILE *t, u8 col){
	tile_fill(t, 0);
	// Small diamond
	const u8 m[8]={
		0b00011000,
		0b00111100,
		0b01111110,
		0b11111111,
		0b01111110,
		0b00111100,
		0b00011000,
		0b00000000
	};
	for(int y=0;y<8;y++){
		u8 p[8]={0};
		for(int x=0;x<8;x++) if((m[y]>>(7-x))&1) p[x]=col;
		t->data[y]=pack_row_4bpp(p);
	}
}

static void tile_icon_stairs(TILE *t, u8 col){
	tile_fill(t, 0);
	// Simple downward arrow
	const u8 m[8]={
		0b00011000,
		0b00011000,
		0b00011000,
		0b00011000,
		0b11011011,
		0b01111110,
		0b00111100,
		0b00011000
	};
	for(int y=0;y<8;y++){
		u8 p[8]={0};
		for(int x=0;x<8;x++) if((m[y]>>(7-x))&1) p[x]=col;
		t->data[y]=pack_row_4bpp(p);
	}
}

static void init_video_and_tiles(void){
	REG_DISPCNT = DCNT_MODE0 | DCNT_BG0;

	REG_BG0CNT = BG_CBB(0) | BG_SBB(31) | BG_4BPP | BG_REG_32x32 | BG_PRIO(0);

	// Palette (16 entries used)
	pal_bg_mem[0]  = RGB15(0,0,0);       // black
	pal_bg_mem[1]  = RGB15(7,6,4);       // floor visible
	pal_bg_mem[2]  = RGB15(4,3,2);       // floor memory
	pal_bg_mem[3]  = RGB15(13,13,13);    // wall visible
	pal_bg_mem[4]  = RGB15(7,7,7);       // wall memory
	pal_bg_mem[5]  = RGB15(6,10,31);     // player
	pal_bg_mem[6]  = RGB15(31,6,6);      // enemy
	pal_bg_mem[7]  = RGB15(31,28,6);     // stairs
	pal_bg_mem[8]  = RGB15(6,31,10);     // item
	pal_bg_mem[9]  = RGB15(31,31,31);    // HUD text
	pal_bg_mem[10] = RGB15(31,10,18);    // full heart
	pal_bg_mem[11] = RGB15(12,2,4);      // empty heart

	// Build tiles in RAM then copy to VRAM
	TILE tiles[NUM_TILES];
	for(int i=0;i<NUM_TILES;i++) tile_fill(&tiles[i], 0);

	// Unseen/blank
	tile_fill(&tiles[TID_UNSEEN], 0);
	tile_fill(&tiles[TID_BLANK],  0);

	// Terrain
	tile_checker(&tiles[TID_FLOOR_V], 1, 0);
	tile_checker(&tiles[TID_FLOOR_M], 2, 0);
	tile_fill(&tiles[TID_WALL_V], 3);
	tile_fill(&tiles[TID_WALL_M], 4);

	// Icons
	tile_icon_stairs(&tiles[TID_STAIRS], 7);
	tile_icon_item  (&tiles[TID_ITEM],   8);
	tile_icon_enemy (&tiles[TID_ENEMY],  6);
	tile_icon_player(&tiles[TID_PLAYER], 5);

	tile_icon_heart(&tiles[TID_HEART_F], 10);
	tile_icon_heart(&tiles[TID_HEART_E], 11);

	// Digits 0-9 (5x7)
	const u8 d[10][7]={
		{0x0E,0x11,0x13,0x15,0x19,0x11,0x0E}, // 0
		{0x04,0x0C,0x04,0x04,0x04,0x04,0x0E}, // 1
		{0x0E,0x11,0x01,0x06,0x08,0x10,0x1F}, // 2
		{0x1F,0x01,0x02,0x06,0x01,0x11,0x0E}, // 3
		{0x02,0x06,0x0A,0x12,0x1F,0x02,0x02}, // 4
		{0x1F,0x10,0x1E,0x01,0x01,0x11,0x0E}, // 5
		{0x06,0x08,0x10,0x1E,0x11,0x11,0x0E}, // 6
		{0x1F,0x01,0x02,0x04,0x08,0x08,0x08}, // 7
		{0x0E,0x11,0x11,0x0E,0x11,0x11,0x0E}, // 8
		{0x0E,0x11,0x11,0x0F,0x01,0x02,0x0C}  // 9
	};
	for(int i=0;i<10;i++)
		tile_glyph_5x7(&tiles[TID_DIGIT0+i], d[i], 9, 0);

	// Letters H, P, F, I (5x7)
	const u8 H[7]={0x11,0x11,0x11,0x1F,0x11,0x11,0x11};
	const u8 P[7]={0x1E,0x11,0x11,0x1E,0x10,0x10,0x10};
	const u8 F[7]={0x1F,0x10,0x10,0x1E,0x10,0x10,0x10};
	const u8 I[7]={0x0E,0x04,0x04,0x04,0x04,0x04,0x0E};
	tile_glyph_5x7(&tiles[TID_H], H, 9, 0);
	tile_glyph_5x7(&tiles[TID_P], P, 9, 0);
	tile_glyph_5x7(&tiles[TID_F], F, 9, 0);
	tile_glyph_5x7(&tiles[TID_I], I, 9, 0);

	// Copy to charblock 0
	memcpy32(tile_mem[0], tiles, (sizeof(tiles)+3)/4);

	// Clear screenblock
	u16 *sb = se_mem[31];
	for(int i=0;i<32*32;i++) sb[i]=TID_UNSEEN;
}

// --- Dungeon generation -------------------------------------------------------

typedef struct { int x,y,w,h; int cx,cy; } Room;

static bool room_overlaps(const Room *a, const Room *b){
	// padded overlap check (1-tile margin)
	int ax0=a->x-1, ay0=a->y-1, ax1=a->x+a->w, ay1=a->y+a->h;
	int bx0=b->x-1, by0=b->y-1, bx1=b->x+b->w, by1=b->y+b->h;
	return !(ax1 < bx0 || bx1 < ax0 || ay1 < by0 || by1 < ay0);
}

static void carve_room(const Room *r){
	for(int y=r->y; y<r->y+r->h; y++)
	for(int x=r->x; x<r->x+r->w; x++)
		if(in_bounds(x,y)) g_map[y][x]=CELL_FLOOR;
}

static void carve_hline(int x0,int x1,int y){
	if(x0>x1){ int t=x0; x0=x1; x1=t; }
	for(int x=x0;x<=x1;x++) if(in_bounds(x,y)) g_map[y][x]=CELL_FLOOR;
}

static void carve_vline(int y0,int y1,int x){
	if(y0>y1){ int t=y0; y0=y1; y1=t; }
	for(int y=y0;y<=y1;y++) if(in_bounds(x,y)) g_map[y][x]=CELL_FLOOR;
}

static bool cell_has_mob(int x,int y){
	for(int i=0;i<MAX_ENEMIES;i++)
		if(g_mobs[i].alive && g_mobs[i].x==x && g_mobs[i].y==y) return true;
	return false;
}

static int mob_index_at(int x,int y){
	for(int i=0;i<MAX_ENEMIES;i++)
		if(g_mobs[i].alive && g_mobs[i].x==x && g_mobs[i].y==y) return i;
	return -1;
}

static int item_index_at(int x,int y){
	for(int i=0;i<MAX_ITEMS;i++)
		if(!g_items[i].taken && g_items[i].x==x && g_items[i].y==y) return i;
	return -1;
}

static bool is_occupied(int x,int y){
	if(g_pl.x==x && g_pl.y==y) return true;
	if(cell_has_mob(x,y)) return true;
	return false;
}

static bool random_floor_pos(int *ox,int *oy, int tries){
	for(int t=0;t<tries;t++){
		int x=rng_range(1, MAP_W-2);
		int y=rng_range(1, MAP_H-2);
		if(g_map[y][x]!=CELL_FLOOR) continue;
		if(is_occupied(x,y)) continue;
		*ox=x; *oy=y;
		return true;
	}
	return false;
}

static void new_floor(void){
	// clear map
	for(int y=0;y<MAP_H;y++)
	for(int x=0;x<MAP_W;x++){
		g_map[y][x]=CELL_WALL;
		g_explored[y][x]=false;
		g_visible[y][x]=false;
	}

	// clear entities
	for(int i=0;i<MAX_ENEMIES;i++){ g_mobs[i].alive=false; g_mobs[i].hp=0; }
	for(int i=0;i<MAX_ITEMS;i++){ g_items[i].taken=true; g_items[i].x=1; g_items[i].y=1; }

	// generate rooms
	Room rooms[12];
	int room_count=0;

	int target_rooms = rng_range(6, 9);

	for(int attempts=0; attempts<200 && room_count<target_rooms; attempts++){
		Room r;
		r.w = rng_range(4, 8);
		r.h = rng_range(3, 6);
		r.x = rng_range(1, MAP_W - r.w - 1);
		r.y = rng_range(1, MAP_H - r.h - 1);
		r.cx = r.x + r.w/2;
		r.cy = r.y + r.h/2;

		bool ok=true;
		for(int i=0;i<room_count;i++){
			if(room_overlaps(&r, &rooms[i])){ ok=false; break; }
		}
		if(!ok) continue;

		rooms[room_count++] = r;
		carve_room(&r);
	}

	// connect rooms
	for(int i=1;i<room_count;i++){
		int x0=rooms[i-1].cx, y0=rooms[i-1].cy;
		int x1=rooms[i].cx,   y1=rooms[i].cy;

		if(rng_range(0,1)==0){
			carve_hline(x0,x1,y0);
			carve_vline(y0,y1,x1);
		}else{
			carve_vline(y0,y1,x0);
			carve_hline(x0,x1,y1);
		}
	}

	// player start
	if(room_count>0){
		g_pl.x = rooms[0].cx;
		g_pl.y = rooms[0].cy;
	}else{
		// fallback
		g_pl.x = MAP_W/2;
		g_pl.y = MAP_H/2;
		g_map[g_pl.y][g_pl.x]=CELL_FLOOR;
	}

	// stairs at last room
	if(room_count>1){
		g_stairs_x = rooms[room_count-1].cx;
		g_stairs_y = rooms[room_count-1].cy;
	}else{
		int sx,sy;
		if(random_floor_pos(&sx,&sy,500)){ g_stairs_x=sx; g_stairs_y=sy; }
		else { g_stairs_x=g_pl.x; g_stairs_y=g_pl.y; }
	}

	// enemies
	int enemy_count = 3 + (g_floor/2);
	if(enemy_count>MAX_ENEMIES) enemy_count=MAX_ENEMIES;

	for(int i=0;i<enemy_count;i++){
		int ex,ey;
		if(!random_floor_pos(&ex,&ey,800)) break;
		if(ex==g_stairs_x && ey==g_stairs_y) { i--; continue; }
		g_mobs[i].alive=true;
		g_mobs[i].x=ex; g_mobs[i].y=ey;
		g_mobs[i].hp = 2 + (g_floor/2);
		if(g_mobs[i].hp>8) g_mobs[i].hp=8;
	}

	// items (berries)
	int item_count = rng_range(2, 4);
	if(item_count>MAX_ITEMS) item_count=MAX_ITEMS;
	for(int i=0;i<item_count;i++){
		int ix,iy;
		if(!random_floor_pos(&ix,&iy,800)) break;
		if(ix==g_stairs_x && iy==g_stairs_y) { i--; continue; }
		g_items[i].taken=false;
		g_items[i].x=ix; g_items[i].y=iy;
	}
}

// --- Visibility (fog of war) -------------------------------------------------

static bool los_clear(int x0,int y0,int x1,int y1){
	// Bresenham line; walls block beyond, but target wall is still "visible".
	int dx=iabs(x1-x0), sx=x0<x1 ? 1 : -1;
	int dy=-iabs(y1-y0), sy=y0<y1 ? 1 : -1;
	int err=dx+dy;

	for(;;){
		if(x0==x1 && y0==y1) return true;

		int e2 = err<<1;
		if(e2 >= dy){ err += dy; x0 += sx; }
		if(e2 <= dx){ err += dx; y0 += sy; }

		if(!in_bounds(x0,y0)) return false;
		if(g_map[y0][x0]==CELL_WALL && !(x0==x1 && y0==y1)) return false;
	}
}

static void compute_fov(void){
	for(int y=0;y<MAP_H;y++)
	for(int x=0;x<MAP_W;x++)
		g_visible[y][x]=false;

	for(int yy=g_pl.y-FOV_R; yy<=g_pl.y+FOV_R; yy++)
	for(int xx=g_pl.x-FOV_R; xx<=g_pl.x+FOV_R; xx++){
		if(!in_bounds(xx,yy)) continue;
		int dx=xx-g_pl.x, dy=yy-g_pl.y;
		if(dx*dx + dy*dy > FOV_R*FOV_R) continue;

		if(los_clear(g_pl.x,g_pl.y,xx,yy)){
			g_visible[yy][xx]=true;
			g_explored[yy][xx]=true;
		}
	}

	// always know your own tile
	g_visible[g_pl.y][g_pl.x]=true;
	g_explored[g_pl.y][g_pl.x]=true;
}

// --- HUD + Render ------------------------------------------------------------

static void draw_hud(u16 *sb){
	// Clear HUD rows
	for(int y=HUD_Y;y<HUD_Y+2;y++)
	for(int x=0;x<32;x++)
		sb_put(sb,x,y,TID_BLANK);

	// "HP"
	sb_put(sb, 0, HUD_Y, TID_H);
	sb_put(sb, 1, HUD_Y, TID_P);

	// Hearts (max 10 shown)
	int shown = g_pl.maxhp;
	if(shown>10) shown=10;
	for(int i=0;i<shown;i++){
		int tid = (g_pl.hp > i) ? TID_HEART_F : TID_HEART_E;
		sb_put(sb, 3+i, HUD_Y, tid);
	}

	// Floor label "F" and two digits at right
	sb_put(sb, 22, HUD_Y, TID_F);
	int f = g_floor;
	if(f>99) f=99;
	int tens = f/10;
	int ones = f%10;
	sb_put(sb, 24, HUD_Y, TID_DIGIT0+tens);
	sb_put(sb, 25, HUD_Y, TID_DIGIT0+ones);

	// Item indicator: "I" + icon (berry uses same as ITEM tile)
	sb_put(sb, 0, HUD_Y+1, TID_I);
	sb_put(sb, 2, HUD_Y+1, g_has_berry ? TID_ITEM : TID_BLANK);

	// Stairs hint icon if currently visible (just an extra tiny cue)
	if(g_visible[g_stairs_y][g_stairs_x])
		sb_put(sb, 4, HUD_Y+1, TID_STAIRS);
}

static void render_all(void){
	compute_fov();

	vid_vsync(); // update during vblank to reduce tearing
	u16 *sb = se_mem[31];

	// Map area
	for(int y=0;y<MAP_H;y++){
		for(int x=0;x<MAP_W;x++){
			int tid=TID_UNSEEN;

			if(!g_explored[y][x]){
				tid=TID_UNSEEN;
			}else{
				bool vis = g_visible[y][x];
				Cell c = g_map[y][x];
				if(c==CELL_WALL) tid = vis ? TID_WALL_V : TID_WALL_M;
				else             tid = vis ? TID_FLOOR_V: TID_FLOOR_M;

				// Overlay only if visible
				if(vis){
					// stairs
					if(x==g_stairs_x && y==g_stairs_y) tid = TID_STAIRS;
					// item
					int ii = item_index_at(x,y);
					if(ii>=0) tid = TID_ITEM;
					// enemy
					int mi = mob_index_at(x,y);
					if(mi>=0) tid = TID_ENEMY;
				}
			}

			// Player always drawn
			if(x==g_pl.x && y==g_pl.y) tid=TID_PLAYER;

			sb_put(sb,x,y,tid);
		}
		// Clear remaining columns in row (30..31)
		sb_put(sb,30,y,TID_UNSEEN);
		sb_put(sb,31,y,TID_UNSEEN);
	}

	draw_hud(sb);
}

// --- Gameplay ----------------------------------------------------------------

static void pickup_if_possible(void){
	int ii = item_index_at(g_pl.x,g_pl.y);
	if(ii<0) return;
	if(g_has_berry) return; // only 1-slot inventory
	g_has_berry=true;
	g_items[ii].taken=true;
}

static void player_attack(int mob_i){
	// small random damage
	int dmg = rng_range(1, 2 + (g_floor>=6));
	if(dmg>3) dmg=3;
	g_mobs[mob_i].hp -= dmg;
	if(g_mobs[mob_i].hp <= 0){
		g_mobs[mob_i].alive=false;
	}
}

static void enemy_attack(void){
	int dmg = rng_range(1, 2);
	g_pl.hp -= dmg;
	if(g_pl.hp < 0) g_pl.hp = 0;
}

static void try_move_player(int nx,int ny){
	if(!in_bounds(nx,ny)) return;
	if(g_map[ny][nx]==CELL_WALL) return;

	int mi = mob_index_at(nx,ny);
	if(mi>=0){
		player_attack(mi);
		return; // attacking consumes the turn
	}

	g_pl.x=nx; g_pl.y=ny;
	pickup_if_possible();
}

static void move_enemy_step(int i, int nx,int ny){
	if(!in_bounds(nx,ny)) return;
	if(g_map[ny][nx]==CELL_WALL) return;
	if(nx==g_pl.x && ny==g_pl.y) return;
	if(cell_has_mob(nx,ny)) return;
	g_mobs[i].x=nx; g_mobs[i].y=ny;
}

static bool enemy_can_see_player(const Mob *m){
	int dx=m->x-g_pl.x, dy=m->y-g_pl.y;
	if(dx*dx + dy*dy > 9*9) return false;
	return los_clear(m->x,m->y,g_pl.x,g_pl.y);
}

static void enemies_take_turn(void){
	for(int i=0;i<MAX_ENEMIES;i++){
		if(!g_mobs[i].alive) continue;

		int dx=g_pl.x - g_mobs[i].x;
		int dy=g_pl.y - g_mobs[i].y;

		// adjacent (Chebyshev)
		if(iabs(dx)<=1 && iabs(dy)<=1){
			enemy_attack();
			continue;
		}

		if(enemy_can_see_player(&g_mobs[i])){
			// greedy chase
			int sx=isgn(dx), sy=isgn(dy);
			// try x-first then y, then alternate if blocked
			int x0=g_mobs[i].x, y0=g_mobs[i].y;
			int tx=x0+sx, ty=y0;
			if(!is_blocking(tx,ty) && !cell_has_mob(tx,ty) && !(tx==g_pl.x && ty==g_pl.y)){
				move_enemy_step(i,tx,ty);
			}else{
				tx=x0; ty=y0+sy;
				if(!is_blocking(tx,ty) && !cell_has_mob(tx,ty) && !(tx==g_pl.x && ty==g_pl.y)){
					move_enemy_step(i,tx,ty);
				}else{
					// try the other axis combo
					tx=x0+sx; ty=y0+sy;
					if(!is_blocking(tx,ty) && !cell_has_mob(tx,ty) && !(tx==g_pl.x && ty==g_pl.y)){
						move_enemy_step(i,tx,ty);
					}
				}
			}
		}else{
			// wander
			static const int dirs[4][2]={{1,0},{-1,0},{0,1},{0,-1}};
			int k=rng_range(0,3);
			move_enemy_step(i, g_mobs[i].x + dirs[k][0], g_mobs[i].y + dirs[k][1]);
		}
	}
}

static void use_berry(void){
	if(!g_has_berry) return;
	g_has_berry=false;
	g_pl.hp += 5;
	if(g_pl.hp > g_pl.maxhp) g_pl.hp = g_pl.maxhp;
}

static void reset_game(void){
	g_floor=1;
	g_pl.maxhp=10;
	g_pl.hp=g_pl.maxhp;
	g_has_berry=false;
	new_floor();
	render_all();
}

int main(void){
	irq_init(NULL);
	irq_add(II_VBLANK, NULL);

	// Seed RNG (tonc qran/sqran if available)
	#ifdef sqran
		sqran(0xC0FFEEu ^ (REG_VCOUNT<<8));
	#endif

	init_video_and_tiles();

	g_pl.maxhp=10;
	g_pl.hp=g_pl.maxhp;

	new_floor();
	render_all();

	while(1){
		vid_vsync();
		key_poll();

		bool acted=false;

		// Game over: restart immediately if dead
		if(g_pl.hp<=0){
			reset_game();
			continue;
		}

		// Use item
		if(key_hit(KEY_R)){
			use_berry();
			acted=true;
		}

		// Descend
		if(!acted && key_hit(KEY_START)){
			if(g_pl.x==g_stairs_x && g_pl.y==g_stairs_y){
				g_floor++;
				if(g_floor>99) g_floor=99;
				// small reward: heal a bit on descent
				g_pl.hp += 2;
				if(g_pl.hp>g_pl.maxhp) g_pl.hp=g_pl.maxhp;
				g_has_berry=false; // inventory cleared on new floor (simple rule)
				new_floor();
				acted=true;
			}
		}

		// Wait
		if(!acted && key_hit(KEY_A)){
			acted=true;
		}

		// Movement
		if(!acted){
			int nx=g_pl.x, ny=g_pl.y;
			if(key_hit(KEY_UP))    ny--;
			else if(key_hit(KEY_DOWN))  ny++;
			else if(key_hit(KEY_LEFT))  nx--;
			else if(key_hit(KEY_RIGHT)) nx++;

			if(nx!=g_pl.x || ny!=g_pl.y){
				try_move_player(nx,ny);
				acted=true;
			}
		}

		if(acted){
			enemies_take_turn();
			// If enemies killed you, next loop resets.
			render_all();
		}
	}

	return 0;
}
