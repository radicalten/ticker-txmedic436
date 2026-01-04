// pokemon_rpg.c
// Minimal Pokemon-style RPG for GBA using tonc (text-mode).
//
// Features:
// - Overworld map with walls (#), grass (G), and player (@).
// - Move with D-Pad.
// - Random encounters when walking in grass.
// - Simple battle: Attack / Heal / Run.
// - All in a single C file; uses tonc text engine on BG0.
//
// Requires: devkitARM + tonc (https://www.coranac.com/tonc/text/)

#include <tonc.h>
#include <stdbool.h>
#include <stdio.h>

#define MAP_W 30
#define MAP_H 20

typedef enum
{
    GS_OVERWORLD = 0,
    GS_BATTLE    = 1
} GameState;

static GameState game_state = GS_OVERWORLD;

//------------------------------------------------------------------
// Overworld map
//------------------------------------------------------------------

static const char base_map[MAP_H][MAP_W+1] =
{
    "##############################", // 0
    "#............GGGGGG..........#", // 1
    "#............GGGGGG..........#", // 2
    "#............................#", // 3
    "#............................#", // 4
    "#........##########..........#", // 5
    "#........#........#..........#", // 6
    "#........#........#..........#", // 7
    "#........##########..........#", // 8
    "#............................#", // 9
    "#............................#", //10
    "#......GGGGGG................#", //11
    "#......GGGGGG................#", //12
    "#............................#", //13
    "#............................#", //14
    "#............##########......#", //15
    "#............#........#......#", //16
    "#............##########......#", //17
    "#............................#", //18
    "##############################"  //19
};

static int hero_x = 2;
static int hero_y = 2;

//------------------------------------------------------------------
// Player & battle state
//------------------------------------------------------------------

static int player_max_hp = 20;
static int player_hp     = 20;

static int enemy_max_hp = 10;
static int enemy_hp     = 10;

static int  battle_menu_index      = 0;
static bool battle_message_active  = false;
static bool battle_ends_after_msg  = false;
static bool battle_player_fainted  = false;
static char battle_message[128];

// Forward declarations
static void draw_overworld(void);
static void update_overworld(void);
static void start_battle(void);
static void draw_battle(void);
static void draw_battle_message(void);
static void update_battle(void);

//------------------------------------------------------------------
// Utility
//------------------------------------------------------------------

static int clamp_int(int v, int lo, int hi)
{
    if(v < lo) return lo;
    if(v > hi) return hi;
    return v;
}

//------------------------------------------------------------------
// Overworld drawing
//------------------------------------------------------------------

static void draw_overworld(void)
{
    // Clear text screen
    tte_printf("#{es}");

    char line[MAP_W+1];

    for(int y=0; y<MAP_H; y++)
    {
        for(int x=0; x<MAP_W; x++)
        {
            char c = base_map[y][x];

            if(x == hero_x && y == hero_y)
                c = '@';          // Player marker

            line[x] = c;
        }
        line[MAP_W] = '\0';

        // Each character row is 8 pixels tall
        tte_printf("#{P:0,%d}%s", y*8, line);
    }

    // Simple HUD (overwrites part of the top border, but that's fine)
    tte_printf("#{P:0,0}HP: %d/%d", player_hp, player_max_hp);
    tte_printf("#{P:120,0}Walk in G to find battles");
}

//------------------------------------------------------------------
// Start a new battle
//------------------------------------------------------------------

static void start_battle(void)
{
    game_state = GS_BATTLE;

    // Small random variation on enemy HP
    enemy_max_hp = 8 + (qran() & 7);  // 8..15
    enemy_hp     = enemy_max_hp;

    battle_menu_index     = 0;
    battle_message_active = false;
    battle_ends_after_msg = false;
    battle_player_fainted = false;

    draw_battle();
}

//------------------------------------------------------------------
// Draw battle UI (no message box)
//------------------------------------------------------------------

static void draw_battle(void)
{
    tte_printf("#{es}");

    // Enemy info
    tte_printf("#{P:8,8}Wild Slime");
    tte_printf("#{P:8,16}HP: %d/%d", enemy_hp, enemy_max_hp);

    // Player info
    tte_printf("#{P:8,40}You");
    tte_printf("#{P:8,48}HP: %d/%d", player_hp, player_max_hp);

    // Menu
    int y0 = 80;
    tte_printf("#{P:8,%d}%c Attack", y0+0*8, (battle_menu_index==0)?'>':' ');
    tte_printf("#{P:8,%d}%c Heal",   y0+1*8, (battle_menu_index==1)?'>':' ');
    tte_printf("#{P:8,%d}%c Run",    y0+2*8, (battle_menu_index==2)?'>':' ');

    tte_printf("#{P:8,136}D-Pad: move   A: select   B: cancel");
}

//------------------------------------------------------------------
// Draw a battle message screen
//------------------------------------------------------------------

static void draw_battle_message(void)
{
    tte_printf("#{es}");
    tte_printf("#{P:8,32}%s", battle_message);
    tte_printf("#{P:8,136}Press A or B to continue...");
}

//------------------------------------------------------------------
// Overworld update logic
//------------------------------------------------------------------

static void update_overworld(void)
{
    int nx = hero_x;
    int ny = hero_y;

    // One tile per key press (tap to move)
    if(key_hit(KEY_UP))        ny--;
    else if(key_hit(KEY_DOWN)) ny++;
    else if(key_hit(KEY_LEFT)) nx--;
    else if(key_hit(KEY_RIGHT))nx++;

    if(nx != hero_x || ny != hero_y)
    {
        if(nx >= 0 && nx < MAP_W && ny >= 0 && ny < MAP_H)
        {
            char tile = base_map[ny][nx];
            if(tile != '#')         // '#' is a wall
            {
                hero_x = nx;
                hero_y = ny;

                // Random encounter in grass tiles ('G')
                if(tile == 'G')
                {
                    // Roughly 1/32 chance when stepping into grass
                    if((qran() & 31) == 0)
                        start_battle();
                }
            }
        }
    }

    // Redraw overworld each frame (simple but fine)
    draw_overworld();
}

//------------------------------------------------------------------
// Battle update logic
//------------------------------------------------------------------

static void update_battle(void)
{
    // If we're in "message mode", wait for A/B to continue
    if(battle_message_active)
    {
        if(key_hit(KEY_A | KEY_B))
        {
            battle_message_active = false;

            if(battle_ends_after_msg)
            {
                battle_ends_after_msg = false;

                if(battle_player_fainted)
                {
                    // Simple respawn: reset HP and position
                    player_hp = player_max_hp;
                    hero_x    = 2;
                    hero_y    = 2;
                }

                game_state = GS_OVERWORLD;
                draw_overworld();
            }
            else
            {
                draw_battle();
            }
        }
        return;
    }

    // Menu navigation
    if(key_hit(KEY_UP))
    {
        if(battle_menu_index > 0)
            battle_menu_index--;
        draw_battle();
    }
    else if(key_hit(KEY_DOWN))
    {
        if(battle_menu_index < 2)
            battle_menu_index++;
        draw_battle();
    }

    // Execute selected command
    if(key_hit(KEY_A))
    {
        int dmg;
        int healed;

        switch(battle_menu_index)
        {
        case 0:     // Attack
            dmg = 4 + (qran() & 3);        // 4..7
            enemy_hp -= dmg;

            if(enemy_hp <= 0)
            {
                enemy_hp = 0;
                snprintf(battle_message, sizeof(battle_message),
                    "You hit the Slime for %d!\n"
                    "It fainted!",
                    dmg);

                battle_ends_after_msg = true;
                battle_player_fainted = false;
                battle_message_active = true;
                draw_battle_message();
            }
            else
            {
                int enemy_dmg = 2 + (qran() & 3); // 2..5
                player_hp -= enemy_dmg;

                if(player_hp <= 0)
                {
                    player_hp = 0;
                    snprintf(battle_message, sizeof(battle_message),
                        "You hit the Slime for %d!\n"
                        "It strikes back for %d!\n"
                        "You fainted...",
                        dmg, enemy_dmg);

                    battle_ends_after_msg = true;
                    battle_player_fainted = true;
                    battle_message_active = true;
                    draw_battle_message();
                }
                else
                {
                    snprintf(battle_message, sizeof(battle_message),
                        "You hit the Slime for %d!\n"
                        "It strikes back for %d!\n"
                        "Your HP: %d/%d",
                        dmg, enemy_dmg,
                        player_hp, player_max_hp);

                    battle_ends_after_msg = false;
                    battle_player_fainted = false;
                    battle_message_active = true;
                    draw_battle_message();
                }
            }
            break;

        case 1:     // Heal
            healed = 6;
            player_hp += healed;
            if(player_hp > player_max_hp)
                healed -= (player_hp - player_max_hp);
            player_hp = clamp_int(player_hp, 0, player_max_hp);

            dmg = 2 + (qran() & 3);       // enemy counter-attack
            player_hp -= dmg;

            if(player_hp <= 0)
            {
                player_hp = 0;
                snprintf(battle_message, sizeof(battle_message),
                    "You heal %d HP.\n"
                    "The Slime hits for %d.\n"
                    "You fainted...",
                    healed, dmg);

                battle_ends_after_msg = true;
                battle_player_fainted = true;
                battle_message_active = true;
                draw_battle_message();
            }
            else
            {
                snprintf(battle_message, sizeof(battle_message),
                    "You heal %d HP.\n"
                    "The Slime hits for %d.\n"
                    "Your HP: %d/%d",
                    healed, dmg,
                    player_hp, player_max_hp);

                battle_ends_after_msg = false;
                battle_player_fainted = false;
                battle_message_active = true;
                draw_battle_message();
            }
            break;

        case 2:     // Run
            if(qran() & 1)
            {
                snprintf(battle_message, sizeof(battle_message),
                    "You ran away safely!");
                battle_ends_after_msg = true;
                battle_player_fainted = false;
                battle_message_active = true;
                draw_battle_message();
            }
            else
            {
                dmg = 2 + (qran() & 3);
                player_hp -= dmg;

                if(player_hp <= 0)
                {
                    player_hp = 0;
                    snprintf(battle_message, sizeof(battle_message),
                        "Couldn't escape!\n"
                        "The Slime hits for %d.\n"
                        "You fainted...",
                        dmg);

                    battle_ends_after_msg = true;
                    battle_player_fainted = true;
                    battle_message_active = true;
                    draw_battle_message();
                }
                else
                {
                    snprintf(battle_message, sizeof(battle_message),
                        "Couldn't escape!\n"
                        "The Slime hits for %d.\n"
                        "Your HP: %d/%d",
                        dmg, player_hp, player_max_hp);

                    battle_ends_after_msg = false;
                    battle_player_fainted = false;
                    battle_message_active = true;
                    draw_battle_message();
                }
            }
            break;
        }
    }

    // Optional: B to leave battle immediately (useful while testing)
    if(key_hit(KEY_B))
    {
        game_state = GS_OVERWORLD;
        draw_overworld();
    }
}

//------------------------------------------------------------------
// Entry point
//------------------------------------------------------------------

int main(void)
{
    // Basic interrupt / VBlank setup
    irq_init(NULL);
    irq_add(II_VBLANK, NULL);

    // Mode 0, enable BG0
    REG_DISPCNT = DCNT_MODE0 | DCNT_BG0;

    // Init text engine on BG0, charblock 0, screenblock 31
    tte_init_se_default(0, BG_CBB(0) | BG_SBB(31));
    tte_set_margins(0, 0, 240, 160);
    tte_printf("#{es}");

    // Seed tonc's quick RNG
    qran();

    // Initial player status
    player_max_hp = 20;
    player_hp     = 20;
    hero_x        = 2;
    hero_y        = 2;
    game_state    = GS_OVERWORLD;

    draw_overworld();

    // Main loop
    while(1)
    {
        VBlankIntrWait();
        key_poll();

        if(game_state == GS_OVERWORLD)
            update_overworld();
        else
            update_battle();
    }

    return 0;
}
