// main.c - Simple GBA Tower Defense (Crystal Defenders-like) using tonc (Mode 3)
//
// Build:
//  - devkitARM + tonc required
//  - Put this file in a tonc template project and "make"
//    Or link with libtonc, include tonc.h, and use a standard GBA linker script.
//
// Controls:
//  - D-Pad: move cursor (grid-based)
//  - A: place tower (costs gold)
//  - B: sell tower at cursor (refund 50%)
//  - R: start next wave early (when spawn queue empty)
//  - START: pause/unpause
//
// Notes:
//  - Mode 3 bitmap; no external assets
//  - Minimal HUD uses tiny 3x5 digit font + icons
//  - Single-file for clarity. Not optimized, but runs fine in emulators.
//
// MIT-like: free to use/modify.

#include <tonc.h>

// ----------------------------- Config ---------------------------------

#define SCREEN_W        240
#define SCREEN_H        160

#define GRID_SIZE       16
#define GRID_W          (SCREEN_W/GRID_SIZE)  // 15
#define GRID_H          (SCREEN_H/GRID_SIZE)  // 10

#define MAX_TOWERS      32
#define MAX_ENEMIES     64

#define FIX_SHIFT       8
#define FIX(n)          ((n)<<FIX_SHIFT)
#define TO_INT(x)       ((x)>>FIX_SHIFT)

#define ABSI(x)         ((x)<0?-(x):(x))

// Colors
#define COL_BG          RGB15(4,8,5)       // grass green-ish
#define COL_GRID        RGB15(6,12,8)
#define COL_PATH        RGB15(22,16,8)
#define COL_CURSOR      RGB15(31,31,31)
#define COL_TOWER       RGB15(0,10,31)
#define COL_RANGE       RGB15(10,18,31)
#define COL_ENEMY       RGB15(31,4,4)
#define COL_BEAM        RGB15(31,28,6)
#define COL_UI          RGB15(31,31,31)
#define COL_UI_GOLD     RGB15(31,31,0)
#define COL_UI_LIVES    RGB15(31,0,0)
#define COL_UI_WAVE     RGB15(0,31,31)

typedef enum {
    GS_TITLE=0,
    GS_PLAY,
    GS_PAUSE,
    GS_GAMEOVER,
    GS_WIN
} GameState;

// --------------------------- Globals ----------------------------------

// Path as list of grid-cells
static u8 pathX[GRID_W*GRID_H];
static u8 pathY[GRID_W*GRID_H];
static int pathLen=0;

// Map occupancy
static u8 pathMask[GRID_H][GRID_W];      // 1=path blocked
static s16 towerAt[GRID_H][GRID_W];      // -1=empty or tower index

// Cursor grid position
static int curGX=2, curGY=2;
static int moveRep=0;

// Game resources
static int gold=50;
static int lives=20;
static int wave=1;
static int totalWaves=10;

// Spawning
static int spawnRemaining=0;
static int spawnTimer=0;
static int spawnInterval=32;     // frames between spawns
static int waveCooldown=90;      // frames to next wave when done

typedef struct {
    s32 x, y;        // fixed point position (8.8)
    int hp, maxhp;
    s32 speed;       // fixed per-frame
    u8  alive;
    u8  pathIndex;   // next path cell target index
    u16 color;
} Enemy;

typedef struct {
    int gx, gy;         // grid coords
    int x, y;           // pixel center
    int range;          // in pixels
    int damage;
    int cooldown;       // frames
    int cdTimer;
    int beamTimer;      // frames to show beam
    int lastTx, lastTy; // last target pos
    u8  active;
} Tower;

static Enemy enemies[MAX_ENEMIES];
static Tower towers[MAX_TOWERS];

// RNG seed
static u32 rngs=1;

// State
static GameState gstate=GS_TITLE;

// ----------------------- Tiny 3x5 digit font ---------------------------

static const u8 DIGITS_3x5[10][5]={
    {0b111,0b101,0b101,0b101,0b111}, // 0
    {0b010,0b110,0b010,0b010,0b111}, // 1
    {0b111,0b001,0b111,0b100,0b111}, // 2
    {0b111,0b001,0b111,0b001,0b111}, // 3
    {0b101,0b101,0b111,0b001,0b001}, // 4
    {0b111,0b100,0b111,0b001,0b111}, // 5
    {0b111,0b100,0b111,0b101,0b111}, // 6
    {0b111,0b001,0b010,0b010,0b010}, // 7
    {0b111,0b101,0b111,0b101,0b111}, // 8
    {0b111,0b101,0b111,0b001,0b111}  // 9
};

// ---------------------- Low-level drawing ------------------------------

static inline void clear_screen(u16 clr)
{
    u32 c= clr | (clr<<16);
    u32 *dst= (u32*)vid_mem;
    for(int i=0; i<(SCREEN_W*SCREEN_H)/2; i++)
        dst[i]= c;
}

static inline void plot(int x, int y, u16 clr)
{
    if((unsigned)x<SCREEN_W && (unsigned)y<SCREEN_H)
        vid_mem[y*SCREEN_W + x]= clr;
}

static inline void hline(int x0, int x1, int y, u16 clr)
{
    if((unsigned)y>=SCREEN_H) return;
    if(x0>x1){ int t=x0; x0=x1; x1=t; }
    if(x1<0 || x0>=SCREEN_W) return;
    x0= CLAMP(x0, 0, SCREEN_W-1);
    x1= CLAMP(x1, 0, SCREEN_W-1);
    u16 *dst= &vid_mem[y*SCREEN_W + x0];
    for(int x=x0; x<=x1; x++)
        *dst++= clr;
}

static inline void vline(int x, int y0, int y1, u16 clr)
{
    if((unsigned)x>=SCREEN_W) return;
    if(y0>y1){ int t=y0; y0=y1; y1=t; }
    if(y1<0 || y0>=SCREEN_H) return;
    y0= CLAMP(y0, 0, SCREEN_H-1);
    y1= CLAMP(y1, 0, SCREEN_H-1);
    u16 *dst= &vid_mem[y0*SCREEN_W + x];
    for(int y=y0; y<=y1; y++, dst+=SCREEN_W)
        *dst= clr;
}

static inline void fill_rect(int x, int y, int w, int h, u16 clr)
{
    int x1= x+w-1, y1= y+h-1;
    if(x > SCREEN_W-1 || y > SCREEN_H-1) return;
    if(x1<0 || y1<0) return;
    int yy0= CLAMP(y,0,SCREEN_H-1);
    int yy1= CLAMP(y1,0,SCREEN_H-1);
    int xx0= CLAMP(x,0,SCREEN_W-1);
    int xx1= CLAMP(x1,0,SCREEN_W-1);
    for(int yy=yy0; yy<=yy1; yy++)
        hline(xx0, xx1, yy, clr);
}

static void draw_rect_outline(int x, int y, int w, int h, u16 clr)
{
    hline(x, x+w-1, y, clr);
    hline(x, x+w-1, y+h-1, clr);
    vline(x, y, y+h-1, clr);
    vline(x+w-1, y, y+h-1, clr);
}

static void draw_circle_fill(int cx, int cy, int r, u16 clr)
{
    int r2= r*r;
    for(int dy=-r; dy<=r; dy++)
    {
        int y= cy+dy;
        int dxmax= 0;
        while( (dxmax+1)*(dxmax+1) + dy*dy <= r2)
            dxmax++;
        hline(cx-dxmax, cx+dxmax, y, clr);
    }
}

static void draw_line(int x0,int y0,int x1,int y1,u16 clr)
{
    // Bresenham
    int dx= ABSI(x1-x0), sx= x0<x1 ? 1 : -1;
    int dy= -ABSI(y1-y0), sy= y0<y1 ? 1 : -1;
    int err= dx+dy, e2;
    while(1){
        plot(x0,y0, clr);
        if(x0==x1 && y0==y1) break;
        e2= 2*err;
        if(e2>=dy){ err+=dy; x0+=sx; }
        if(e2<=dx){ err+=dx; y0+=sy; }
    }
}

static void draw_digit3x5(int x, int y, int d, u16 clr)
{
    if(d<0 || d>9) return;
    for(int row=0; row<5; row++)
    {
        u8 bits= DIGITS_3x5[d][row];
        for(int col=0; col<3; col++)
            if(bits & (1<<(2-col)))
                plot(x+col, y+row, clr);
    }
}

static void draw_number_small(int x, int y, int value, u16 clr)
{
    if(value==0){ draw_digit3x5(x,y,0,clr); return; }
    int tmp=value, digits=0;
    if(tmp<0) tmp= -tmp;
    while(tmp>0){ tmp/=10; digits++; }
    int xpos= x + (digits-1)*4;
    tmp= value<0 ? -value : value;
    while(tmp>0)
    {
        int d= tmp%10;
        draw_digit3x5(xpos, y, d, clr);
        xpos-=4;
        tmp/=10;
    }
    if(value<0)
        plot(x-2, y+2, clr), plot(x-1, y+2, clr), plot(x, y+2, clr);
}

// ---------------------- Path and map ----------------------------------

static void path_clear()
{
    pathLen=0;
    for(int y=0;y<GRID_H;y++)
        for(int x=0;x<GRID_W;x++)
        {
            pathMask[y][x]=0;
            towerAt[y][x]= -1;
        }
}

static void path_add_line(int x0,int y0,int x1,int y1)
{
    if(x0==x1)
    {
        int y= y0, ystep= (y1>=y0)?1:-1;
        for(; y!=y1; y+=ystep)
        {
            pathX[pathLen]= x0;
            pathY[pathLen]= y;
            pathMask[y][x0]=1;
            pathLen++;
        }
        // include last
        pathX[pathLen]= x1;
        pathY[pathLen]= y1;
        pathMask[y1][x1]=1;
        pathLen++;
    }
    else if(y0==y1)
    {
        int x= x0, xstep=(x1>=x0)?1:-1;
        for(; x!=x1; x+=xstep)
        {
            pathX[pathLen]= x;
            pathY[pathLen]= y0;
            pathMask[y0][x]=1;
            pathLen++;
        }
        pathX[pathLen]= x1;
        pathY[pathLen]= y1;
        pathMask[y1][x1]=1;
        pathLen++;
    }
}

static void build_default_path()
{
    // Snake-like path across the field:
    // (0,2)->(14,2)->(14,5)->(0,5)->(0,8)->(14,8)
    path_clear();
    path_add_line(0,2, 14,2);
    path_add_line(14,2, 14,5);
    path_add_line(14,5, 0,5);
    path_add_line(0,5, 0,8);
    path_add_line(0,8, 14,8);
    // pathLen now has full sequence of cells the enemies will follow
}

static inline int is_path_cell(int gx,int gy)
{
    if((unsigned)gx>=GRID_W || (unsigned)gy>=GRID_H) return 1;
    return pathMask[gy][gx]!=0;
}

// ----------------------- Enemies and Towers ---------------------------

static void enemies_clear()
{
    for(int i=0;i<MAX_ENEMIES;i++)
        enemies[i].alive=0;
}

static void towers_clear()
{
    for(int i=0;i<MAX_TOWERS;i++)
        towers[i].active=0;
    for(int y=0;y<GRID_H;y++)
        for(int x=0;x<GRID_W;x++)
            towerAt[y][x]= -1;
}

static int find_free_enemy()
{
    for(int i=0;i<MAX_ENEMIES;i++)
        if(!enemies[i].alive) return i;
    return -1;
}

static int find_free_tower()
{
    for(int i=0;i<MAX_TOWERS;i++)
        if(!towers[i].active) return i;
    return -1;
}

static inline s32 fx_step_to(s32 *x, s32 *y, s32 tx, s32 ty, s32 spd)
{
    s32 dx= tx - *x;
    s32 dy= ty - *y;
    s32 adx= ABSI(dx), ady= ABSI(dy);
    s32 sum= adx + ady;
    if(sum <= spd)
    {
        *x= tx; *y= ty;
        return 1;
    }
    if(sum==0) return 1;
    *x += ( (s64)spd * dx ) / sum;
    *y += ( (s64)spd * dy ) / sum;
    return 0;
}

static void spawn_enemy_for_wave(int w)
{
    if(pathLen<2) return;
    int idx= find_free_enemy();
    if(idx<0) return;

    int cx0= pathX[0]*GRID_SIZE + GRID_SIZE/2;
    int cy0= pathY[0]*GRID_SIZE + GRID_SIZE/2;

    Enemy *e= &enemies[idx];
    e->x= FIX(cx0);
    e->y= FIX(cy0);
    e->pathIndex= (pathLen>1)? 1 : 0;
    e->alive= 1;
    e->color= COL_ENEMY;

    // Wave scaling
    e->maxhp= 3 + w*2;
    e->hp= e->maxhp;
    // Speed: 0.4 px/frame + 0.05*w capped
    int spd100= 40 + 5*w;       // in hundredths of px/frame
    if(spd100>120) spd100=120;
    e->speed= (spd100 * FIX_ONE) / 100;
}

static int enemies_alive_count()
{
    int c=0;
    for(int i=0;i<MAX_ENEMIES;i++) if(enemies[i].alive) c++;
    return c;
}

static void start_wave(int w)
{
    // Enemies per wave
    spawnRemaining= 8 + 2*w;
    if(spawnRemaining > MAX_ENEMIES) spawnRemaining= MAX_ENEMIES;
    spawnInterval= 36 - w*2; if(spawnInterval<12) spawnInterval=12;
    spawnTimer= 30; // small delay
}

static void kill_enemy(Enemy* e)
{
    e->alive=0;
    // Reward
    int reward= 2 + (wave/2);
    gold += reward;
}

static int tower_cost()
{
    return 20 + (wave/2)*5;    // slowly rises
}

// Return: index of best target within range or -1
static int tower_find_target(Tower* t)
{
    int bestIdx= -1;
    int bestProg= -1;

    int tx= t->x;
    int ty= t->y;
    int r2= t->range * t->range;

    for(int i=0;i<MAX_ENEMIES;i++)
    {
        Enemy *e=&enemies[i];
        if(!e->alive) continue;
        int ex= TO_INT(e->x);
        int ey= TO_INT(e->y);
        int dx= ex - tx;
        int dy= ey - ty;
        if(dx*dx + dy*dy <= r2)
        {
            // Prioritize by pathIndex (progress)
            int prog= e->pathIndex;
            if(prog>bestProg)
            {
                bestProg= prog;
                bestIdx= i;
            }
        }
    }
    return bestIdx;
}

// ----------------------------- Update ---------------------------------

static void update_enemies()
{
    for(int i=0;i<MAX_ENEMIES;i++)
    {
        Enemy *e=&enemies[i];
        if(!e->alive) continue;

        if(e->pathIndex >= pathLen)
        {
            // Already at end: leak
            e->alive=0;
            if(lives>0) lives--;
            continue;
        }

        int tgx= pathX[e->pathIndex];
        int tgy= pathY[e->pathIndex];
        s32 tx= FIX(tgx*GRID_SIZE + GRID_SIZE/2);
        s32 ty= FIX(tgy*GRID_SIZE + GRID_SIZE/2);

        if(fx_step_to(&e->x, &e->y, tx, ty, e->speed))
        {
            e->pathIndex++;
            if(e->pathIndex >= pathLen)
            {
                // Reached end
                e->alive=0;
                if(lives>0) lives--;
            }
        }
    }
}

static void update_towers()
{
    for(int i=0;i<MAX_TOWERS;i++)
    {
        Tower *t= &towers[i];
        if(!t->active) continue;

        if(t->cdTimer>0) t->cdTimer--;
        if(t->beamTimer>0) t->beamTimer--;

        if(t->cdTimer==0)
        {
            int target= tower_find_target(t);
            if(target>=0)
            {
                Enemy *e= &enemies[target];
                e->hp -= t->damage;
                t->cdTimer= t->cooldown;

                t->beamTimer= 4;
                t->lastTx= TO_INT(e->x);
                t->lastTy= TO_INT(e->y);

                if(e->hp<=0)
                    kill_enemy(e);
            }
        }
    }
}

// ----------------------------- Draw -----------------------------------

static void draw_grid()
{
    // Background
    clear_screen(COL_BG);

    // Grid lines (subtle)
    for(int gx=1; gx<GRID_W; gx++)
    {
        int x= gx*GRID_SIZE;
        vline(x, 0, SCREEN_H-1, COL_GRID);
    }
    for(int gy=1; gy<GRID_H; gy++)
    {
        int y= gy*GRID_SIZE;
        hline(0, SCREEN_W-1, y, COL_GRID);
    }

    // Path tiles
    for(int i=0;i<pathLen;i++)
    {
        int gx= pathX[i], gy= pathY[i];
        fill_rect(gx*GRID_SIZE, gy*GRID_SIZE, GRID_SIZE, GRID_SIZE, COL_PATH);
    }
}

static void draw_towers()
{
    for(int i=0;i<MAX_TOWERS;i++)
    {
        Tower* t= &towers[i];
        if(!t->active) continue;
        // Tower body
        fill_rect(t->x-6, t->y-6, 12, 12, COL_TOWER);
        // Beam
        if(t->beamTimer>0)
            draw_line(t->x, t->y, t->lastTx, t->lastTy, COL_BEAM);
    }
}

static void draw_enemies()
{
    for(int i=0;i<MAX_ENEMIES;i++)
    {
        Enemy *e=&enemies[i];
        if(!e->alive) continue;
        int ex= TO_INT(e->x);
        int ey= TO_INT(e->y);

        // Tint by HP
        int hpPct= (e->hp*31)/ (e->maxhp? e->maxhp:1);
        if(hpPct<0) hpPct=0;
        u16 col= RGB15(31, hpPct/2, hpPct/4);   // reddish with HP influence

        draw_circle_fill(ex, ey, 4, col);

        // Tiny HP pip
        if(e->hp < e->maxhp)
        {
            int w= 8;
            int filled= (w*e->hp)/ (e->maxhp? e->maxhp:1);
            hline(ex-w/2, ex-w/2+w-1, ey-7, RGB15(4,4,4));
            hline(ex-w/2, ex-w/2+filled-1, ey-7, RGB15(0,31,0));
        }
    }
}

static void draw_cursor_and_preview()
{
    int x= curGX*GRID_SIZE;
    int y= curGY*GRID_SIZE;

    // Cell outline
    draw_rect_outline(x,y, GRID_SIZE,GRID_SIZE, COL_CURSOR);

    // Preview tower if placeable
    if(!is_path_cell(curGX,curGY) && towerAt[curGY][curGX]<0)
    {
        int cx= x + GRID_SIZE/2;
        int cy= y + GRID_SIZE/2;
        fill_rect(cx-5, cy-5, 10, 10, RGB15(8,16,31));
        // Show range circle outline (sparse dots)
        int pr= 42; // tower base range
        for(int a=0;a<360;a+=8)
        {
            // quick integer circle
            int px= cx + (pr * lu_cos(a))/1024; // But we don't have LUT. Instead do diamond
        }
        // Simple diamond range (cheaper than circle): draw 4 lines
        int r= 42;
        draw_line(cx-r,cy, cx,cy-r, COL_RANGE);
        draw_line(cx,cy-r, cx+r,cy, COL_RANGE);
        draw_line(cx+r,cy, cx,cy+r, COL_RANGE);
        draw_line(cx,cy+r, cx-r,cy, COL_RANGE);
    }
}

static void draw_ui()
{
    // Gold icon (coin)
    draw_circle_fill(6, 6, 5, COL_UI_GOLD);
    draw_number_small(14, 4, gold, COL_UI);

    // Lives icon (heart-ish)
    draw_circle_fill(84, 6, 4, COL_UI_LIVES);
    draw_circle_fill(88, 6, 4, COL_UI_LIVES);
    fill_rect(84, 8, 8, 4, COL_UI_LIVES);
    draw_number_small(96, 4, lives, COL_UI);

    // Wave icon (diamond)
    draw_line(160, 3, 166, 9, COL_UI_WAVE);
    draw_line(166, 9, 160, 15, COL_UI_WAVE);
    draw_line(160, 15, 154, 9, COL_UI_WAVE);
    draw_line(154, 9, 160, 3, COL_UI_WAVE);
    draw_number_small(172, 4, wave, COL_UI);

    // Tower cost
    int cost= tower_cost();
    draw_number_small(210, 4, cost, RGB15(31,31,0));
}

static void draw_scene()
{
    draw_grid();
    draw_towers();
    draw_enemies();
    draw_cursor_and_preview();
    draw_ui();
}

// ----------------------------- Input ----------------------------------

static void handle_cursor_move()
{
    key_poll();

    int dx=0, dy=0;

    if(moveRep>0) moveRep--;

    u16 kh= key_hit(KEY_ANY);
    u16 ke= key_held(KEY_ANY);

    // Initial press: move and set repeat
    if(key_hit(KEY_RIGHT)) { dx=1; moveRep=8; }
    else if(key_hit(KEY_LEFT)) { dx=-1; moveRep=8; }
    else if(key_hit(KEY_DOWN)) { dy=1; moveRep=8; }
    else if(key_hit(KEY_UP)) { dy=-1; moveRep=8; }
    else if(moveRep==0)
    {
        // Held repeat
        if(key_held(KEY_RIGHT)) { dx=1; moveRep=3; }
        else if(key_held(KEY_LEFT)) { dx=-1; moveRep=3; }
        else if(key_held(KEY_DOWN)) { dy=1; moveRep=3; }
        else if(key_held(KEY_UP)) { dy=-1; moveRep=3; }
    }

    if(dx||dy)
    {
        curGX= CLAMP(curGX+dx, 0, GRID_W-1);
        curGY= CLAMP(curGY+dy, 0, GRID_H-1);
    }
}

static void try_place_tower()
{
    if(is_path_cell(curGX,curGY)) return;
    if(towerAt[curGY][curGX]>=0) return;

    int cost= tower_cost();
    if(gold < cost) return;

    int idx= find_free_tower();
    if(idx<0) return;

    Tower* t= &towers[idx];
    t->active=1;
    t->gx= curGX;
    t->gy= curGY;
    t->x= curGX*GRID_SIZE + GRID_SIZE/2;
    t->y= curGY*GRID_SIZE + GRID_SIZE/2;
    t->range= 42 + (wave/3)*4;       // small scaling
    t->damage= 2 + (wave>=6 ? 1:0);
    t->cooldown= CLAMP(22 - wave, 10, 22);
    t->cdTimer= 0;
    t->beamTimer=0;
    t->lastTx=t->x; t->lastTy=t->y;

    towerAt[curGY][curGX]= idx;
    gold -= cost;
}

static void try_sell_tower()
{
    int idx= towerAt[curGY][curGX];
    if(idx<0) return;
    Tower* t= &towers[idx];
    t->active=0;
    towerAt[curGY][curGX]= -1;

    int refund= tower_cost()*50/100;
    gold += refund;
}

// ------------------------------ Game ----------------------------------

static void game_reset()
{
    gold=50;
    lives=20;
    wave=1;
    enemies_clear();
    towers_clear();
    build_default_path();
    start_wave(wave);
}

static void update_spawning()
{
    if(spawnRemaining>0)
    {
        if(spawnTimer>0) spawnTimer--;
        if(spawnTimer==0)
        {
            spawn_enemy_for_wave(wave);
            spawnRemaining--;
            spawnTimer= spawnInterval;
        }
    }
}

static void update_gameplay()
{
    handle_cursor_move();

    if(key_hit(KEY_START))
    {
        gstate= GS_PAUSE;
        return;
    }

    if(key_hit(KEY_A))
        try_place_tower();
    if(key_hit(KEY_B))
        try_sell_tower();
    if(key_hit(KEY_R))
    {
        // Early next wave if possible
        if(spawnRemaining==0 && enemies_alive_count()==0)
        {
            wave++;
            if(wave>totalWaves)
                gstate= GS_WIN;
            else
                start_wave(wave);
        }
    }

    update_spawning();
    update_enemies();
    update_towers();

    if(lives<=0)
        gstate= GS_GAMEOVER;

    // Auto-advance waves when done
    if(spawnRemaining==0 && enemies_alive_count()==0)
    {
        if(waveCooldown>0) waveCooldown--;
        if(waveCooldown==0)
        {
            wave++;
            if(wave>totalWaves)
                gstate= GS_WIN;
            else
            {
                start_wave(wave);
                waveCooldown=90;
            }
        }
    }
    else
    {
        waveCooldown=90; // reset
    }
}

static void draw_title()
{
    clear_screen(RGB15(0,0,0));

    // Simple title made of blocks
    // "TD" blocky
    fill_rect(40, 40, 56, 12, COL_UI);   // T top
    fill_rect(40+22, 52, 12, 40, COL_UI);

    // D
    fill_rect(120,40, 12,52, COL_UI);
    fill_rect(132,40, 28,12, COL_UI);
    fill_rect(132,80, 28,12, COL_UI);
    fill_rect(160,52, 8,28, COL_UI);

    // "Press START"
    // Use digits font to spell "START" minimally: just show three icons and a number
    // We'll draw a simple blinking rectangle:
    static int blink=0; blink=(blink+1)&31;
    if(blink<16)
        fill_rect(80, 112, 80, 12, RGB15(6,6,6));
    // Hint: draw number "1" to suggest "Start"
    draw_number_small(120, 114, 1, RGB15(31,31,0));
}

static void draw_pause()
{
    // Dim overlay
    fill_rect(0,0, SCREEN_W,SCREEN_H, RGB15(0,0,0));
    // "PAUSE" banner
    fill_rect(50, 60, 140, 40, RGB15(8,8,8));
    draw_number_small(60, 74, 2, COL_UI); // just a marker
}

static void draw_gameover()
{
    clear_screen(RGB15(0,0,0));
    // Red box
    fill_rect(40, 56, 160, 48, RGB15(16,0,0));
    // Show wave reached and gold
    draw_number_small(52, 64, wave, COL_UI);
    draw_number_small(52, 76, gold, COL_UI_GOLD);
}

static void draw_win()
{
    clear_screen(RGB15(0,0,0));
    fill_rect(40, 56, 160, 48, RGB15(0,16,0));
    draw_number_small(52, 64, wave, COL_UI);
    draw_number_small(52, 76, gold, COL_UI_GOLD);
}

// ----------------------------- Main -----------------------------------

int main(void)
{
    irq_add(II_VBLANK, NULL);

    REG_DISPCNT= DCNT_MODE3 | DCNT_BG2;

    game_reset();
    gstate= GS_TITLE;

    while(1)
    {
        VBlankIntrWait();
        key_poll();

        switch(gstate)
        {
        case GS_TITLE:
            draw_title();
            if(key_hit(KEY_START) || key_hit(KEY_A) || key_hit(KEY_B))
            {
                // Start game
                game_reset();
                gstate= GS_PLAY;
            }
            break;

        case GS_PLAY:
            update_gameplay();
            draw_scene();
            break;

        case GS_PAUSE:
            draw_scene();
            draw_pause();
            if(key_hit(KEY_START))
                gstate= GS_PLAY;
            break;

        case GS_GAMEOVER:
            draw_gameover();
            if(key_hit(KEY_START) || key_hit(KEY_A))
            {
                game_reset();
                gstate= GS_PLAY;
            }
            break;

        case GS_WIN:
            draw_win();
            if(key_hit(KEY_START) || key_hit(KEY_A))
            {
                game_reset();
                gstate= GS_PLAY;
            }
            break;
        }
    }
    return 0;
}
