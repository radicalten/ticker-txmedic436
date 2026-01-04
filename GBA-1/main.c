//-----------------------------------------------------------------------------
// Simple Final Fantasy-style RPG battle demo for GBA using tonc
// Single-file C program
//
// Requirements:
//   - devkitARM toolchain
//   - libtonc (https://www.coranac.com/tonc/)
//   - typical tonc "template" Makefile/project structure
//
// Build (example, if you use a standard tonc project):
//   Place this file as main.c in the source folder and run `make`.
//
// This is a text-mode, single-battle demo:
//   - Title screen
//   - One hero vs one monster
//   - Turn-based menu battle (Attack, Fire, Heal, Defend, Run)
//-----------------------------------------------------------------------------

#include <tonc.h>
#include <stdbool.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

//-----------------------------------------------------------------------------
// Data types
//-----------------------------------------------------------------------------

typedef struct
{
    const char *name;
    int maxHP, hp;
    int maxMP, mp;
    int atk;
    int def;
    int mag;
} Actor;

typedef enum
{
    BR_WIN,
    BR_LOSE,
    BR_RUN
} BattleResult;

typedef enum
{
    PHASE_PLAYER_MENU,
    PHASE_PLAYER_MSG,
    PHASE_ENEMY_MSG,
    PHASE_END_MSG
} Phase;

//-----------------------------------------------------------------------------
// Actor templates
//-----------------------------------------------------------------------------

static const Actor HERO_TEMPLATE =
{
    "Hero",
    60, 60,   // HP
    20, 20,   // MP
    12,       // ATK
    5,        // DEF
    8         // MAG
};

static const Actor ENEMY_TEMPLATE =
{
    "Goblin",
    50, 50,   // HP
    10, 10,   // MP
    10,       // ATK
    4,        // DEF
    5         // MAG
};

//-----------------------------------------------------------------------------
// Globals for battle UI
//-----------------------------------------------------------------------------

#define CMD_ATTACK 0
#define CMD_FIRE   1
#define CMD_HEAL   2
#define CMD_DEFEND 3
#define CMD_RUN    4
#define CMD_COUNT  5

static const char *g_cmdNames[CMD_COUNT] =
{
    "Attack",
    "Fire",
    "Heal",
    "Defend",
    "Run"
};

// Message buffer for the battle log line
static char g_message[96];

//-----------------------------------------------------------------------------
// Utility / initialization
//-----------------------------------------------------------------------------

static void console_clear(void)
{
    tte_erase_screen();
    tte_set_pos(0, 0);
}

static void init_gba(void)
{
    irq_init(NULL);
    irq_add(II_VBLANK, NULL);

    REG_DISPCNT = DCNT_MODE0 | DCNT_BG0;

    // Initialize TTE (text engine) on BG0 with default 4bpp font
    tte_init_se_default(0, BG_CBB(0) | BG_SBB(31));
    tte_init_con();

    // Fixed RNG seed (deterministic demo)
    srand(42);
}

// Return a small random variation in range [-range, +range]
static int rand_variation(int range)
{
    if (range <= 0)
        return 0;
    return (rand() % (2*range + 1)) - range;
}

// Clamp integer to [0, max]
static void clamp_int(int *value, int max)
{
    if (*value < 0)
        *value = 0;
    else if (*value > max)
        *value = max;
}

//-----------------------------------------------------------------------------
// Title and ending screens
//-----------------------------------------------------------------------------

static void run_title(void)
{
    console_clear();
    tte_printf("==== FF-STYLE RPG DEMO ====\n\n");
    tte_printf("One hero vs one monster\n");
    tte_printf("Turn-based battle, text-only.\n\n");
    tte_printf("Controls:\n");
    tte_printf("  D-Pad: Move cursor\n");
    tte_printf("  A: Confirm\n");
    tte_printf("  START: Begin / back to title\n\n");
    tte_printf("Press START to begin...");

    while (1)
    {
        VBlankIntrWait();
        key_poll();
        if (key_hit(KEY_START))
            break;
    }
}

static void show_ending(BattleResult res)
{
    console_clear();

    switch (res)
    {
    case BR_WIN:
        tte_printf("You are victorious!\n\n");
        tte_printf("The monster is defeated.\n");
        break;
    case BR_LOSE:
        tte_printf("You were defeated...\n\n");
        tte_printf("Better luck next time.\n");
        break;
    case BR_RUN:
        tte_printf("You ran away!\n\n");
        tte_printf("Live to fight another day.\n");
        break;
    }

    tte_printf("\nPress START to return to the title.");

    while (1)
    {
        VBlankIntrWait();
        key_poll();
        if (key_hit(KEY_START))
            break;
    }
}

//-----------------------------------------------------------------------------
// Battle rendering
//-----------------------------------------------------------------------------

static void battle_draw(const Actor *hero, const Actor *enemy,
                        int cursor, Phase phase)
{
    console_clear();

    tte_printf("==== Battle ====\n\n");

    tte_printf("%s  HP: %d/%d  MP: %d/%d\n",
        hero->name, hero->hp, hero->maxHP, hero->mp, hero->maxMP);

    tte_printf("%s  HP: %d/%d\n\n",
        enemy->name, enemy->hp, enemy->maxHP);

    tte_printf("%s\n\n", g_message[0] ? g_message : " ");

    if (phase == PHASE_PLAYER_MENU)
    {
        int i;
        for (i = 0; i < CMD_COUNT; i++)
        {
            if (i == cursor)
                tte_printf("> %s\n", g_cmdNames[i]);
            else
                tte_printf("  %s\n", g_cmdNames[i]);
        }
    }
    else if (phase == PHASE_END_MSG)
    {
        tte_printf("\n(Press A to finish)");
    }
    else
    {
        tte_printf("\n(Press A to continue)");
    }
}

//-----------------------------------------------------------------------------
// Battle logic
//-----------------------------------------------------------------------------

// Returns whether the player's chosen action consumed the turn.
// (If false, player keeps the turn after reading the message)
// May also set battleOver/result when killing enemy or successful run.
static bool player_action(int cmd,
                          Actor *hero, Actor *enemy,
                          bool *defending,
                          bool *battleOver, BattleResult *result)
{
    const int FIRE_COST  = 3;
    const int HEAL_COST  = 4;

    *defending = false;

    switch (cmd)
    {
    case CMD_ATTACK:
    {
        int base = hero->atk - enemy->def;
        if (base < 1) base = 1;
        int dmg = base + rand_variation(2);
        if (dmg < 0) dmg = 0;

        enemy->hp -= dmg;
        if (enemy->hp < 0) enemy->hp = 0;

        snprintf(g_message, sizeof(g_message),
                 "%s attacks! %d damage.", hero->name, dmg);

        if (enemy->hp <= 0)
        {
            *battleOver = true;
            *result = BR_WIN;
            strncat(g_message, " Enemy defeated!",
                    sizeof(g_message) - strlen(g_message) - 1);
        }
        return true;
    }

    case CMD_FIRE:
        if (hero->mp < FIRE_COST)
        {
            snprintf(g_message, sizeof(g_message),
                     "Not enough MP for Fire!");
            // Turn not consumed; player keeps turn after reading
            return false;
        }
        hero->mp -= FIRE_COST;
        clamp_int(&hero->mp, hero->maxMP);

        {
            int dmg = hero->mag * 2 + (rand() % 4);
            enemy->hp -= dmg;
            if (enemy->hp < 0) enemy->hp = 0;

            snprintf(g_message, sizeof(g_message),
                     "%s casts Fire! %d damage.", hero->name, dmg);

            if (enemy->hp <= 0)
            {
                *battleOver = true;
                *result = BR_WIN;
                strncat(g_message, " Enemy incinerated!",
                        sizeof(g_message) - strlen(g_message) - 1);
            }
        }
        return true;

    case CMD_HEAL:
        if (hero->mp < HEAL_COST)
        {
            snprintf(g_message, sizeof(g_message),
                     "Not enough MP for Heal!");
            // Turn not consumed
            return false;
        }
        hero->mp -= HEAL_COST;
        clamp_int(&hero->mp, hero->maxMP);

        {
            int amount = hero->mag * 2 + (rand() % 3);
            hero->hp += amount;
            clamp_int(&hero->hp, hero->maxHP);

            snprintf(g_message, sizeof(g_message),
                     "%s casts Heal! Restored %d HP.",
                     hero->name, amount);
        }
        return true;

    case CMD_DEFEND:
        *defending = true;
        snprintf(g_message, sizeof(g_message),
                 "%s braces for impact! (Def up)", hero->name);
        return true;

    case CMD_RUN:
    {
        int chance = rand() % 100;
        if (chance < 50)
        {
            snprintf(g_message, sizeof(g_message),
                     "%s escapes successfully!", hero->name);
            *battleOver = true;
            *result = BR_RUN;
            return true;
        }
        else
        {
            snprintf(g_message, sizeof(g_message),
                     "Couldn't escape!");
            // Turn consumed, enemy will act
            return true;
        }
    }

    default:
        snprintf(g_message, sizeof(g_message),
                 "You hesitate...");
        return true;
    }
}

// Enemy chooses and performs an action immediately.
// May set battleOver/result on killing the hero.
static void enemy_action(Actor *enemy, Actor *hero,
                         bool heroDefending,
                         bool *battleOver, BattleResult *result)
{
    const int FIRE_COST = 3;
    bool useFire = false;

    if (enemy->mp >= FIRE_COST && (rand() % 100) < 30)
        useFire = true;

    if (useFire)
    {
        enemy->mp -= FIRE_COST;
        clamp_int(&enemy->mp, enemy->maxMP);

        int dmg = enemy->mag * 2 + (rand() % 3);
        hero->hp -= dmg;
        if (hero->hp < 0) hero->hp = 0;

        snprintf(g_message, sizeof(g_message),
                 "%s casts Fire! You take %d damage.",
                 enemy->name, dmg);
    }
    else
    {
        int base = enemy->atk - hero->def;
        if (heroDefending)
            base /= 2;
        if (base < 1) base = 1;

        int dmg = base + rand_variation(2);
        if (dmg < 0) dmg = 0;

        hero->hp -= dmg;
        if (hero->hp < 0) hero->hp = 0;

        if (heroDefending)
            snprintf(g_message, sizeof(g_message),
                     "%s attacks! %d damage (reduced).",
                     enemy->name, dmg);
        else
            snprintf(g_message, sizeof(g_message),
                     "%s attacks! %d damage.",
                     enemy->name, dmg);
    }

    if (hero->hp <= 0)
    {
        *battleOver = true;
        *result = BR_LOSE;
        strncat(g_message, " You fall in battle.",
                sizeof(g_message) - strlen(g_message) - 1);
    }
}

//-----------------------------------------------------------------------------
// Main battle loop
//-----------------------------------------------------------------------------

static BattleResult run_battle(void)
{
    Actor hero  = HERO_TEMPLATE;
    Actor enemy = ENEMY_TEMPLATE;

    Phase phase = PHASE_PLAYER_MENU;
    BattleResult result = BR_LOSE;   // default; overwritten
    bool battleOver = false;
    bool defending = false;
    bool playerTurnConsumed = true;

    int cursor = 0;

    snprintf(g_message, sizeof(g_message), "A wild %s appears!", enemy.name);

    while (1)
    {
        VBlankIntrWait();
        key_poll();

        battle_draw(&hero, &enemy, cursor, phase);

        switch (phase)
        {
        case PHASE_PLAYER_MENU:
        {
            u16 keys = key_hit(KEY_UP | KEY_DOWN | KEY_A);
            if (keys & KEY_UP)
            {
                cursor--;
                if (cursor < 0)
                    cursor = CMD_COUNT - 1;
            }
            if (keys & KEY_DOWN)
            {
                cursor++;
                if (cursor >= CMD_COUNT)
                    cursor = 0;
            }
            if (keys & KEY_A)
            {
                playerTurnConsumed =
                    player_action(cursor, &hero, &enemy,
                                  &defending, &battleOver, &result);

                if (battleOver)
                    phase = PHASE_END_MSG;
                else
                    phase = PHASE_PLAYER_MSG;
            }
            break;
        }

        case PHASE_PLAYER_MSG:
        {
            if (key_hit(KEY_A))
            {
                if (battleOver)
                {
                    phase = PHASE_END_MSG;
                }
                else if (!playerTurnConsumed)
                {
                    // e.g. failed spell due to MP: keep player's turn
                    snprintf(g_message, sizeof(g_message),
                             "What will you do?");
                    phase = PHASE_PLAYER_MENU;
                }
                else
                {
                    // Enemy now acts immediately
                    enemy_action(&enemy, &hero, defending,
                                 &battleOver, &result);

                    if (battleOver)
                        phase = PHASE_END_MSG;
                    else
                        phase = PHASE_ENEMY_MSG;
                }
            }
            break;
        }

        case PHASE_ENEMY_MSG:
        {
            if (key_hit(KEY_A))
            {
                if (battleOver)
                {
                    phase = PHASE_END_MSG;
                }
                else
                {
                    // End of enemy's action, new round
                    defending = false;
                    snprintf(g_message, sizeof(g_message),
                             "What will you do?");
                    phase = PHASE_PLAYER_MENU;
                }
            }
            break;
        }

        case PHASE_END_MSG:
        {
            if (key_hit(KEY_A))
            {
                return result;
            }
            break;
        }

        default:
            break;
        }
    }
}

//-----------------------------------------------------------------------------
// main
//-----------------------------------------------------------------------------

int main(void)
{
    init_gba();

    while (1)
    {
        run_title();
        BattleResult res = run_battle();
        show_ending(res);
    }

    return 0;
}
