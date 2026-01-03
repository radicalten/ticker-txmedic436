#include <tonc.h>
#include <string.h>
#include <stdio.h>

// -------------------------------------------------------------------------
// Game Data Structures
// -------------------------------------------------------------------------

typedef struct {
    const char *text;       // The story text
    const char *optionA;    // Text for Button A choice
    const char *optionB;    // Text for Button B choice
    int nextRoomA;          // Index of room to go to if A pressed
    int nextRoomB;          // Index of room to go to if B pressed
    bool isEnding;          // If true, game waits for START to reset
    u16 color;              // Text color for this room
} Room;

// -------------------------------------------------------------------------
// The World Map (The "Zork" Logic)
// -------------------------------------------------------------------------

// Room Indices
#define R_START     0
#define R_CAVE      1
#define R_FOREST    2
#define R_DRAGON    3
#define R_PIT       4
#define R_TREASURE  5
#define R_RIVER     6
#define R_WIN       7
#define R_GAME_OVER 8

// Array of all rooms
const Room rooms[] = {
    // 0: Start
    {
        "You stand before a ominous cave.\nTo the South is a dark forest.\n\nWhat do you do?",
        "Enter Cave", "Go to Forest",
        R_CAVE, R_FOREST,
        false, CLR_WHITE
    },
    // 1: Cave Entry
    {
        "The cave is damp and smells of\nsulfur. It is very dark.\n\n",
        "Light a Torch", "Walk in the dark",
        R_DRAGON, R_PIT,
        false, CLR_CYAN
    },
    // 2: Forest
    {
        "The trees are thick. You hear\nrushing water nearby.",
        "Head towards water", "Climb a tree",
        R_RIVER, R_START, // Climbing tree goes back to start for demo purposes
        false, CLR_LIME
    },
    // 3: Dragon Room
    {
        "The torch lights up the room.\nA massive RED DRAGON wakes up!\nIt stares at you.",
        "Attack!", "Offer a snack",
        R_GAME_OVER, R_TREASURE,
        false, CLR_ORANGE
    },
    // 4: The Pit (Death)
    {
        "You stumble in the dark and\nfall into a bottomless pit.\n\nYOU HAVE DIED.",
        NULL, NULL, 0, 0,
        true, CLR_RED
    },
    // 5: Treasure Logic
    {
        "The Dragon eats the snack and\ngoes back to sleep.\nBehind it lies a chest!",
        "Open Chest", "Leave quietly",
        R_WIN, R_FOREST,
        false, CLR_MAG
    },
    // 6: River (Death)
    {
        "You find a raging river.\nYou try to swim across but\nthe current is too strong.\n\nYOU DROWNED.",
        NULL, NULL, 0, 0,
        true, CLR_BLUE
    },
    // 7: Win
    {
        "The chest is full of GBA carts!\nYou are rich!\n\nCONGRATULATIONS!",
        NULL, NULL, 0, 0,
        true, CLR_YELLOW
    },
    // 8: Dragon Death
    {
        "You poke the dragon.\nIt breathes fire on you.\n\nTOASTY.",
        NULL, NULL, 0, 0,
        true, CLR_RED
    }
};

// -------------------------------------------------------------------------
// Helper Functions
// -------------------------------------------------------------------------

// Initialize the display and the Text Engine
void init_game() {
    // Set display mode to Mode 0 (Tile based) and enable Background 0
    REG_DISPCNT = DCNT_MODE0 | DCNT_BG0;

    // Initialize TTE (Text Engine) with the default font
    // Uses Background 0, Charblock 0, Screenblock 31
    tte_init_se_default(0, BG_CBB(0) | BG_SBB(31));
    
    // Initialize the console functionality within TTE
    tte_init_con();
}

void draw_room(int roomIndex) {
    // Clear screen
    tte_erase_screen();
    
    // Reset cursor to top left
    tte_set_pos(0, 0);

    const Room *r = &rooms[roomIndex];

    // Set text color
    tte_set_ink(r->color);

    // Print Description
    tte_printf("#{P:10,10}"); // Set padding/margins
    tte_printf("%s\n\n", r->text);

    // Print Options if not an ending
    if (!r->isEnding) {
        tte_set_ink(CLR_WHITE);
        tte_printf("\n\n");
        tte_printf("(A) %s\n\n", r->optionA);
        tte_printf("(B) %s", r->optionB);
    } else {
        tte_set_ink(CLR_GRAY);
        tte_printf("\n\nPress START to restart.");
    }
}

// -------------------------------------------------------------------------
// Main Loop
// -------------------------------------------------------------------------

int main() {
    init_game();

    int currentRoomId = R_START;
    bool roomNeedsUpdate = true;

    while (1) {
        // VBlankIntrWait(); // Requires interrupt setup, using vsync for simple loop
        vid_vsync();
        key_poll();

        // 1. Logic
        if (roomNeedsUpdate) {
            draw_room(currentRoomId);
            roomNeedsUpdate = false;
        }

        const Room *r = &rooms[currentRoomId];

        if (r->isEnding) {
            // End state: Wait for Start to reset
            if (key_hit(KEY_START)) {
                currentRoomId = R_START;
                roomNeedsUpdate = true;
            }
        } else {
            // Normal state: Check A or B
            if (key_hit(KEY_A)) {
                currentRoomId = r->nextRoomA;
                roomNeedsUpdate = true;
            } else if (key_hit(KEY_B)) {
                currentRoomId = r->nextRoomB;
                roomNeedsUpdate = true;
            }
        }
    }

    return 0;
}
