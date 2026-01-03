/*
 * GBA Text Adventure - "The Forgotten Crypt"
 * A Zork-like game using tonc library
 * 
 * Build with: make (using devkitPro/devkitARM)
 * Or: arm-none-eabi-gcc -mthumb -mthumb-interwork -specs=gba.specs 
 *     adventure.c -I$DEVKITPRO/libtonc/include -L$DEVKITPRO/libtonc/lib -ltonc -o adventure.gba
 */

#include <tonc.h>
#include <string.h>

// ============================================================================
// CONSTANTS AND ENUMS
// ============================================================================

#define SCREEN_WIDTH    240
#define SCREEN_HEIGHT   160
#define TILE_SIZE       8

#define MAX_INVENTORY   8
#define MAX_TEXT_LINES  16
#define LINE_WIDTH      29
#define MAX_ROOM_ITEMS  4

// Directions
enum { DIR_NORTH, DIR_SOUTH, DIR_EAST, DIR_WEST, DIR_UP, DIR_DOWN, DIR_COUNT };

// Room IDs
enum {
    ROOM_NONE = -1,
    ROOM_ENTRANCE,
    ROOM_GREAT_HALL,
    ROOM_LIBRARY,
    ROOM_ARMORY,
    ROOM_DUNGEON,
    ROOM_CRYPT,
    ROOM_TREASURY,
    ROOM_ALTAR,
    ROOM_SECRET_PASSAGE,
    ROOM_THRONE,
    ROOM_COUNT
};

// Item IDs
enum {
    ITEM_NONE = 0,
    ITEM_RUSTY_KEY,
    ITEM_TORCH,
    ITEM_SWORD,
    ITEM_ANCIENT_BOOK,
    ITEM_SILVER_KEY,
    ITEM_GOLD_CHALICE,
    ITEM_RUBY,
    ITEM_SHIELD,
    ITEM_SKELETON_KEY,
    ITEM_CROWN,
    ITEM_COUNT
};

// Command types
enum {
    CMD_GO,
    CMD_LOOK,
    CMD_TAKE,
    CMD_DROP,
    CMD_USE,
    CMD_EXAMINE,
    CMD_INVENTORY,
    CMD_HELP,
    CMD_COUNT
};

// Game states
enum {
    STATE_COMMAND_SELECT,
    STATE_DIRECTION_SELECT,
    STATE_ITEM_SELECT,
    STATE_ROOM_ITEM_SELECT,
    STATE_GAME_OVER,
    STATE_VICTORY
};

// ============================================================================
// DATA STRUCTURES
// ============================================================================

typedef struct {
    const char* name;
    const char* description;
    int portable;
    int key_type;  // What this item unlocks
} Item;

typedef struct {
    const char* name;
    const char* description;
    const char* dark_desc;
    int exits[DIR_COUNT];
    int locked_exits[DIR_COUNT];  // Item needed to unlock
    int items[MAX_ROOM_ITEMS];
    int is_dark;
    int visited;
} Room;

typedef struct {
    int current_room;
    int inventory[MAX_INVENTORY];
    int inv_count;
    int state;
    int menu_selection;
    int has_light;
    int moves;
    int skeleton_defeated;
    int game_won;
} GameState;

// ============================================================================
// GAME DATA
// ============================================================================

static const Item items[ITEM_COUNT] = {
    [ITEM_NONE]        = {"", "", 0, 0},
    [ITEM_RUSTY_KEY]   = {"Rusty Key", "An old iron key covered in rust. Still might work.", 1, ROOM_DUNGEON},
    [ITEM_TORCH]       = {"Torch", "A wooden torch. It burns with a steady flame.", 1, 0},
    [ITEM_SWORD]       = {"Iron Sword", "A well-balanced sword. Good for fighting skeletons.", 1, 0},
    [ITEM_ANCIENT_BOOK]= {"Ancient Book", "A tome of forbidden knowledge. Contains a map.", 1, 0},
    [ITEM_SILVER_KEY]  = {"Silver Key", "An ornate silver key with strange symbols.", 1, ROOM_TREASURY},
    [ITEM_GOLD_CHALICE]= {"Gold Chalice", "A magnificent golden cup encrusted with gems.", 1, 0},
    [ITEM_RUBY]        = {"Glowing Ruby", "A fist-sized ruby that pulses with inner light.", 1, 0},
    [ITEM_SHIELD]      = {"Bronze Shield", "A battered but sturdy shield.", 1, 0},
    [ITEM_SKELETON_KEY]= {"Skeleton Key", "Taken from the skeleton guardian.", 1, ROOM_ALTAR},
    [ITEM_CROWN]       = {"Crown of Kings", "The legendary crown! Your quest is complete!", 1, 0}
};

static Room rooms[ROOM_COUNT] = {
    [ROOM_ENTRANCE] = {
        "Crypt Entrance",
        "You stand before an ancient stone\narchway. Cold air flows from the\ndarkness within. Stairs lead DOWN\ninto the depths.",
        NULL,
        {ROOM_NONE, ROOM_NONE, ROOM_NONE, ROOM_NONE, ROOM_NONE, ROOM_GREAT_HALL},
        {0,0,0,0,0,0},
        {ITEM_TORCH, 0, 0, 0},
        0, 0
    },
    [ROOM_GREAT_HALL] = {
        "Great Hall",
        "A vast underground hall with\ncrumbling pillars. Passages lead\nNORTH to a library, EAST to an\narmory, and stairs go UP and DOWN.",
        NULL,
        {ROOM_LIBRARY, ROOM_NONE, ROOM_ARMORY, ROOM_NONE, ROOM_ENTRANCE, ROOM_DUNGEON},
        {0,0,0,0,0,ITEM_RUSTY_KEY},
        {ITEM_RUSTY_KEY, 0, 0, 0},
        0, 0
    },
    [ROOM_LIBRARY] = {
        "Ancient Library",
        "Dusty bookshelves line the walls.\nMost books have rotted away.\nThe exit is SOUTH.",
        NULL,
        {ROOM_NONE, ROOM_GREAT_HALL, ROOM_NONE, ROOM_SECRET_PASSAGE, ROOM_NONE, ROOM_NONE},
        {0,0,0,ITEM_ANCIENT_BOOK,0,0},
        {ITEM_ANCIENT_BOOK, 0, 0, 0},
        0, 0
    },
    [ROOM_ARMORY] = {
        "Old Armory",
        "Rusted weapons hang on the walls.\nA few items remain usable.\nThe exit is WEST.",
        NULL,
        {ROOM_NONE, ROOM_NONE, ROOM_NONE, ROOM_GREAT_HALL, ROOM_NONE, ROOM_NONE},
        {0,0,0,0,0,0},
        {ITEM_SWORD, ITEM_SHIELD, 0, 0},
        0, 0
    },
    [ROOM_DUNGEON] = {
        "Dark Dungeon",
        "A damp dungeon with chains on the\nwalls. A passage leads EAST to the\ncrypt. Stairs go UP.",
        "It's pitch black! You need a light\nsource to see.",
        {ROOM_NONE, ROOM_NONE, ROOM_CRYPT, ROOM_NONE, ROOM_GREAT_HALL, ROOM_NONE},
        {0,0,0,0,0,0},
        {ITEM_SILVER_KEY, 0, 0, 0},
        1, 0
    },
    [ROOM_CRYPT] = {
        "The Crypt",
        "Ancient sarcophagi line the walls.\nA SKELETON GUARDIAN blocks the\npath NORTH! Exit is WEST.",
        "It's pitch black! You can hear\nbones rattling nearby...",
        {ROOM_ALTAR, ROOM_NONE, ROOM_NONE, ROOM_DUNGEON, ROOM_NONE, ROOM_NONE},
        {0,0,0,0,0,0},
        {0, 0, 0, 0},
        1, 0
    },
    [ROOM_TREASURY] = {
        "Treasury",
        "Gold coins and jewels are\nscattered everywhere! A passage\nleads WEST.",
        NULL,
        {ROOM_NONE, ROOM_NONE, ROOM_NONE, ROOM_SECRET_PASSAGE, ROOM_NONE, ROOM_NONE},
        {0,0,0,0,0,0},
        {ITEM_GOLD_CHALICE, ITEM_RUBY, 0, 0},
        0, 0
    },
    [ROOM_ALTAR] = {
        "Dark Altar",
        "A sinister altar stands in the\ncenter. Strange symbols glow on\nthe floor. Stairs go DOWN to throne.",
        "Absolute darkness. Evil presence.",
        {ROOM_NONE, ROOM_CRYPT, ROOM_NONE, ROOM_NONE, ROOM_NONE, ROOM_THRONE},
        {0,0,0,0,0,ITEM_SKELETON_KEY},
        {0, 0, 0, 0},
        1, 0
    },
    [ROOM_SECRET_PASSAGE] = {
        "Secret Passage",
        "A narrow hidden corridor.\nPassages lead EAST to library\nand EAST to treasury.",
        NULL,
        {ROOM_NONE, ROOM_NONE, ROOM_LIBRARY, ROOM_NONE, ROOM_NONE, ROOM_NONE},
        {0,0,0,0,0,0},
        {0, 0, 0, 0},
        0, 0
    },
    [ROOM_THRONE] = {
        "Throne Room",
        "A magnificent underground throne\nroom! The CROWN OF KINGS rests\non the ancient throne!",
        "Total darkness hides something\nimportant here...",
        {ROOM_NONE, ROOM_NONE, ROOM_NONE, ROOM_NONE, ROOM_ALTAR, ROOM_NONE},
        {0,0,0,0,0,0},
        {ITEM_CROWN, 0, 0, 0},
        1, 0
    }
};

// Fix secret passage connections
static void init_rooms(void) {
    rooms[ROOM_SECRET_PASSAGE].exits[DIR_EAST] = ROOM_TREASURY;
    rooms[ROOM_SECRET_PASSAGE].exits[DIR_WEST] = ROOM_NONE;
    rooms[ROOM_LIBRARY].exits[DIR_WEST] = ROOM_SECRET_PASSAGE;
    rooms[ROOM_TREASURY].exits[DIR_WEST] = ROOM_SECRET_PASSAGE;
    rooms[ROOM_TREASURY].locked_exits[DIR_WEST] = ITEM_SILVER_KEY;
    rooms[ROOM_SECRET_PASSAGE].locked_exits[DIR_EAST] = ITEM_SILVER_KEY;
}

static const char* cmd_names[CMD_COUNT] = {
    "GO", "LOOK", "TAKE", "DROP", "USE", "EXAMINE", "INVENTORY", "HELP"
};

static const char* dir_names[DIR_COUNT] = {
    "NORTH", "SOUTH", "EAST", "WEST", "UP", "DOWN"
};

// ============================================================================
// GLOBAL STATE
// ============================================================================

static GameState game;
static char text_buffer[MAX_TEXT_LINES][LINE_WIDTH + 1];
static int text_line_count = 0;
static int scroll_offset = 0;

// ============================================================================
// TEXT DISPLAY FUNCTIONS
// ============================================================================

static void clear_text(void) {
    text_line_count = 0;
    scroll_offset = 0;
    for(int i = 0; i < MAX_TEXT_LINES; i++) {
        text_buffer[i][0] = '\0';
    }
}

static void add_line(const char* text) {
    if(text_line_count < MAX_TEXT_LINES) {
        int len = strlen(text);
        if(len > LINE_WIDTH) len = LINE_WIDTH;
        memcpy(text_buffer[text_line_count], text, len);
        text_buffer[text_line_count][len] = '\0';
        text_line_count++;
    }
}

static void add_text(const char* text) {
    char line[LINE_WIDTH + 1];
    int line_pos = 0;
    
    while(*text) {
        if(*text == '\n' || line_pos >= LINE_WIDTH) {
            line[line_pos] = '\0';
            add_line(line);
            line_pos = 0;
            if(*text == '\n') text++;
            continue;
        }
        line[line_pos++] = *text++;
    }
    if(line_pos > 0) {
        line[line_pos] = '\0';
        add_line(line);
    }
}

static void render_text(void) {
    // Clear screen
    tte_erase_screen();
    
    // Draw text lines
    int display_lines = 12;
    int start = 0;
    if(text_line_count > display_lines) {
        start = scroll_offset;
        if(start > text_line_count - display_lines)
            start = text_line_count - display_lines;
    }
    
    for(int i = 0; i < display_lines && (start + i) < text_line_count; i++) {
        tte_set_pos(4, 4 + i * 10);
        tte_write(text_buffer[start + i]);
    }
    
    // Draw separator
    tte_set_pos(0, 124);
    tte_write("------------------------------");
    
    // Draw command area based on state
    tte_set_pos(4, 136);
    
    switch(game.state) {
        case STATE_COMMAND_SELECT:
            tte_write("Command: ");
            tte_set_pos(4, 148);
            for(int i = 0; i < CMD_COUNT; i++) {
                if(i == game.menu_selection) {
                    tte_write("[");
                    tte_write(cmd_names[i]);
                    tte_write("] ");
                } else {
                    tte_write(cmd_names[i]);
                    tte_write(" ");
                }
                if(i == 3) {
                    tte_set_pos(4, 148);
                }
            }
            break;
            
        case STATE_DIRECTION_SELECT: {
            tte_write("Direction (B=back): ");
            tte_set_pos(4, 148);
            int shown = 0;
            for(int i = 0; i < DIR_COUNT; i++) {
                int dest = rooms[game.current_room].exits[i];
                if(dest != ROOM_NONE) {
                    if(shown == game.menu_selection) {
                        tte_write("[");
                        tte_write(dir_names[i]);
                        tte_write("] ");
                    } else {
                        tte_write(dir_names[i]);
                        tte_write(" ");
                    }
                    shown++;
                }
            }
            break;
        }
        
        case STATE_ITEM_SELECT: {
            tte_write("Use item (B=back): ");
            tte_set_pos(4, 148);
            for(int i = 0; i < game.inv_count; i++) {
                if(i == game.menu_selection) {
                    tte_write("[");
                    tte_write(items[game.inventory[i]].name);
                    tte_write("] ");
                } else {
                    tte_write(items[game.inventory[i]].name);
                    tte_write(" ");
                }
            }
            break;
        }
        
        case STATE_ROOM_ITEM_SELECT: {
            tte_write("Take item (B=back): ");
            tte_set_pos(4, 148);
            int shown = 0;
            Room* r = &rooms[game.current_room];
            for(int i = 0; i < MAX_ROOM_ITEMS; i++) {
                if(r->items[i] != ITEM_NONE) {
                    if(shown == game.menu_selection) {
                        tte_write("[");
                        tte_write(items[r->items[i]].name);
                        tte_write("]");
                    } else {
                        tte_write(items[r->items[i]].name);
                    }
                    shown++;
                }
            }
            if(shown == 0) tte_write("Nothing here!");
            break;
        }
        
        case STATE_VICTORY:
            tte_write("*** YOU WIN! *** Press START");
            break;
            
        case STATE_GAME_OVER:
            tte_write("*** GAME OVER *** Press START");
            break;
    }
}

// ============================================================================
// GAME LOGIC
// ============================================================================

static int has_item(int item_id) {
    for(int i = 0; i < game.inv_count; i++) {
        if(game.inventory[i] == item_id) return 1;
    }
    return 0;
}

static void remove_item(int item_id) {
    for(int i = 0; i < game.inv_count; i++) {
        if(game.inventory[i] == item_id) {
            for(int j = i; j < game.inv_count - 1; j++) {
                game.inventory[j] = game.inventory[j + 1];
            }
            game.inv_count--;
            return;
        }
    }
}

static void add_item(int item_id) {
    if(game.inv_count < MAX_INVENTORY) {
        game.inventory[game.inv_count++] = item_id;
    }
}

static void update_light(void) {
    game.has_light = has_item(ITEM_TORCH);
}

static void describe_room(void) {
    Room* r = &rooms[game.current_room];
    
    clear_text();
    add_text("=== ");
    add_text(r->name);
    add_text(" ===");
    add_line("");
    
    if(r->is_dark && !game.has_light) {
        add_text(r->dark_desc ? r->dark_desc : "It's too dark to see!");
    } else {
        add_text(r->description);
        
        // List items
        int has_items = 0;
        for(int i = 0; i < MAX_ROOM_ITEMS; i++) {
            if(r->items[i] != ITEM_NONE) {
                if(!has_items) {
                    add_line("");
                    add_text("You see:");
                }
                add_text("  - ");
                add_text(items[r->items[i]].name);
                has_items = 1;
            }
        }
        
        // Special: skeleton in crypt
        if(game.current_room == ROOM_CRYPT && !game.skeleton_defeated) {
            add_line("");
            add_text("A SKELETON WARRIOR guards the");
            add_text("northern passage!");
        }
    }
    
    add_line("");
    
    // Show exits
    char exits_str[LINE_WIDTH + 1] = "Exits: ";
    int first = 1;
    for(int i = 0; i < DIR_COUNT; i++) {
        if(r->exits[i] != ROOM_NONE) {
            if(!first) strcat(exits_str, ", ");
            strcat(exits_str, dir_names[i]);
            first = 0;
        }
    }
    add_text(exits_str);
    
    r->visited = 1;
}

static void do_go(void) {
    // Count available exits and find selected one
    int exit_count = 0;
    int selected_dir = -1;
    
    for(int i = 0; i < DIR_COUNT; i++) {
        if(rooms[game.current_room].exits[i] != ROOM_NONE) {
            if(exit_count == game.menu_selection) {
                selected_dir = i;
            }
            exit_count++;
        }
    }
    
    if(selected_dir < 0) return;
    
    Room* r = &rooms[game.current_room];
    int dest = r->exits[selected_dir];
    int required = r->locked_exits[selected_dir];
    
    // Check if locked
    if(required != 0 && !has_item(required)) {
        clear_text();
        add_text("The way is locked! You need");
        add_text("something to open it.");
        game.state = STATE_COMMAND_SELECT;
        game.menu_selection = 0;
        return;
    }
    
    // Check skeleton
    if(game.current_room == ROOM_CRYPT && selected_dir == DIR_NORTH && !game.skeleton_defeated) {
        clear_text();
        add_text("The skeleton warrior blocks your");
        add_text("path! You must defeat it first!");
        game.state = STATE_COMMAND_SELECT;
        game.menu_selection = 0;
        return;
    }
    
    // Move to new room
    game.current_room = dest;
    game.moves++;
    describe_room();
    
    // Check win condition
    if(has_item(ITEM_CROWN)) {
        if(game.current_room == ROOM_ENTRANCE) {
            game.state = STATE_VICTORY;
            game.game_won = 1;
            clear_text();
            add_text("**** VICTORY! ****");
            add_line("");
            add_text("You escape the crypt with the");
            add_text("legendary Crown of Kings!");
            add_line("");
            add_text("Moves: ");
            char num[8];
            int m = game.moves;
            int pos = 0;
            if(m == 0) num[pos++] = '0';
            else {
                char tmp[8];
                int tp = 0;
                while(m > 0) { tmp[tp++] = '0' + (m % 10); m /= 10; }
                while(tp > 0) num[pos++] = tmp[--tp];
            }
            num[pos] = 0;
            add_text(num);
            add_line("");
            add_text("Thanks for playing!");
            return;
        }
    }
    
    game.state = STATE_COMMAND_SELECT;
    game.menu_selection = 0;
}

static void do_take(void) {
    Room* r = &rooms[game.current_room];
    
    // Check if dark
    if(r->is_dark && !game.has_light) {
        clear_text();
        add_text("It's too dark to find anything!");
        game.state = STATE_COMMAND_SELECT;
        return;
    }
    
    // Find selected item
    int item_count = 0;
    int selected_slot = -1;
    
    for(int i = 0; i < MAX_ROOM_ITEMS; i++) {
        if(r->items[i] != ITEM_NONE) {
            if(item_count == game.menu_selection) {
                selected_slot = i;
            }
            item_count++;
        }
    }
    
    if(selected_slot < 0) {
        game.state = STATE_COMMAND_SELECT;
        return;
    }
    
    int item_id = r->items[selected_slot];
    
    if(game.inv_count >= MAX_INVENTORY) {
        clear_text();
        add_text("Your inventory is full!");
    } else {
        add_item(item_id);
        r->items[selected_slot] = ITEM_NONE;
        update_light();
        
        clear_text();
        add_text("You take the ");
        add_text(items[item_id].name);
        add_text(".");
        
        if(item_id == ITEM_TORCH) {
            add_line("");
            add_text("The torch illuminates the area!");
        }
    }
    
    game.state = STATE_COMMAND_SELECT;
    game.menu_selection = 0;
}

static void do_use(void) {
    if(game.inv_count == 0) {
        clear_text();
        add_text("You have nothing to use!");
        game.state = STATE_COMMAND_SELECT;
        return;
    }
    
    int item_id = game.inventory[game.menu_selection];
    Room* r = &rooms[game.current_room];
    
    clear_text();
    
    // Special uses
    if(item_id == ITEM_SWORD && game.current_room == ROOM_CRYPT && !game.skeleton_defeated) {
        if(has_item(ITEM_SHIELD)) {
            add_text("With sword and shield, you fight");
            add_text("the skeleton warrior!");
            add_line("");
            add_text("After a fierce battle, the");
            add_text("skeleton crumbles to dust!");
            add_text("It drops a SKELETON KEY!");
            game.skeleton_defeated = 1;
            // Add skeleton key to room
            for(int i = 0; i < MAX_ROOM_ITEMS; i++) {
                if(r->items[i] == ITEM_NONE) {
                    r->items[i] = ITEM_SKELETON_KEY;
                    break;
                }
            }
        } else {
            add_text("You attack the skeleton but");
            add_text("without a shield, you're");
            add_text("pushed back!");
        }
    }
    else if(item_id == ITEM_ANCIENT_BOOK) {
        add_text("The book reveals a map showing");
        add_text("a secret passage in the library");
        add_text("leading to a hidden treasury!");
        add_line("");
        add_text("(The WEST wall in the library");
        add_text("can now be searched!)");
        // Unlock secret passage
        rooms[ROOM_LIBRARY].locked_exits[DIR_WEST] = 0;
    }
    else if(item_id == ITEM_TORCH) {
        add_text("The torch burns brightly,");
        add_text("illuminating dark areas.");
    }
    else {
        add_text("You can't use that here.");
    }
    
    game.state = STATE_COMMAND_SELECT;
    game.menu_selection = 0;
}

static void do_look(void) {
    describe_room();
}

static void do_examine(void) {
    if(game.inv_count == 0) {
        clear_text();
        add_text("You have nothing to examine!");
        game.state = STATE_COMMAND_SELECT;
        return;
    }
    
    int item_id = game.inventory[game.menu_selection];
    
    clear_text();
    add_text(items[item_id].name);
    add_line("");
    add_text(items[item_id].description);
    
    game.state = STATE_COMMAND_SELECT;
    game.menu_selection = 0;
}

static void do_inventory(void) {
    clear_text();
    add_text("=== INVENTORY ===");
    add_line("");
    
    if(game.inv_count == 0) {
        add_text("You are empty-handed.");
    } else {
        for(int i = 0; i < game.inv_count; i++) {
            add_text("- ");
            add_text(items[game.inventory[i]].name);
        }
    }
}

static void do_help(void) {
    clear_text();
    add_text("=== HELP ===");
    add_line("");
    add_text("Find the Crown of Kings and");
    add_text("escape the crypt!");
    add_line("");
    add_text("D-Pad: Navigate menus");
    add_text("A: Select");
    add_text("B: Back/Cancel");
    add_text("L/R: Scroll text");
}

static void do_drop(void) {
    if(game.inv_count == 0) {
        clear_text();
        add_text("You have nothing to drop!");
        game.state = STATE_COMMAND_SELECT;
        return;
    }
    
    int item_id = game.inventory[game.menu_selection];
    Room* r = &rooms[game.current_room];
    
    // Find empty slot in room
    int slot = -1;
    for(int i = 0; i < MAX_ROOM_ITEMS; i++) {
        if(r->items[i] == ITEM_NONE) {
            slot = i;
            break;
        }
    }
    
    if(slot < 0) {
        clear_text();
        add_text("No room to drop items here!");
    } else {
        r->items[slot] = item_id;
        remove_item(item_id);
        update_light();
        
        clear_text();
        add_text("You drop the ");
        add_text(items[item_id].name);
        add_text(".");
    }
    
    game.state = STATE_COMMAND_SELECT;
    game.menu_selection = 0;
}

// ============================================================================
// INPUT HANDLING
// ============================================================================

static void handle_input(void) {
    key_poll();
    u16 keys = key_hit(KEY_FULL);
    u16 held = key_held(KEY_FULL);
    
    // Scroll with L/R
    if(held & KEY_L) {
        if(scroll_offset > 0) scroll_offset--;
    }
    if(held & KEY_R) {
        if(scroll_offset < text_line_count - 10) scroll_offset++;
    }
    
    // Victory/Game Over - restart
    if(game.state == STATE_VICTORY || game.state == STATE_GAME_OVER) {
        if(keys & KEY_START) {
            // Reset game
            memset(&game, 0, sizeof(game));
            init_rooms();
            for(int i = 0; i < ROOM_COUNT; i++) rooms[i].visited = 0;
            describe_room();
        }
        return;
    }
    
    int max_selection = 0;
    
    switch(game.state) {
        case STATE_COMMAND_SELECT:
            max_selection = CMD_COUNT;
            break;
        case STATE_DIRECTION_SELECT:
            for(int i = 0; i < DIR_COUNT; i++) {
                if(rooms[game.current_room].exits[i] != ROOM_NONE) 
                    max_selection++;
            }
            break;
        case STATE_ITEM_SELECT:
            max_selection = game.inv_count;
            break;
        case STATE_ROOM_ITEM_SELECT:
            for(int i = 0; i < MAX_ROOM_ITEMS; i++) {
                if(rooms[game.current_room].items[i] != ITEM_NONE)
                    max_selection++;
            }
            break;
    }
    
    // Navigation
    if(keys & KEY_RIGHT) {
        game.menu_selection++;
        if(game.menu_selection >= max_selection) game.menu_selection = 0;
    }
    if(keys & KEY_LEFT) {
        game.menu_selection--;
        if(game.menu_selection < 0) game.menu_selection = max_selection - 1;
    }
    if(keys & KEY_DOWN) {
        game.menu_selection += 4;
        if(game.menu_selection >= max_selection) 
            game.menu_selection = game.menu_selection % max_selection;
    }
    if(keys & KEY_UP) {
        game.menu_selection -= 4;
        if(game.menu_selection < 0) 
            game.menu_selection = max_selection + game.menu_selection;
    }
    
    // Cancel
    if(keys & KEY_B) {
        if(game.state != STATE_COMMAND_SELECT) {
            game.state = STATE_COMMAND_SELECT;
            game.menu_selection = 0;
        }
    }
    
    // Select
    if(keys & KEY_A) {
        switch(game.state) {
            case STATE_COMMAND_SELECT:
                switch(game.menu_selection) {
                    case CMD_GO:
                        game.state = STATE_DIRECTION_SELECT;
                        game.menu_selection = 0;
                        break;
                    case CMD_LOOK:
                        do_look();
                        break;
                    case CMD_TAKE:
                        game.state = STATE_ROOM_ITEM_SELECT;
                        game.menu_selection = 0;
                        break;
                    case CMD_DROP:
                        if(game.inv_count > 0) {
                            game.state = STATE_ITEM_SELECT;
                            game.menu_selection = 0;
                        } else {
                            clear_text();
                            add_text("You have nothing to drop!");
                        }
                        break;
                    case CMD_USE:
                        if(game.inv_count > 0) {
                            game.state = STATE_ITEM_SELECT;
                            game.menu_selection = 0;
                        } else {
                            clear_text();
                            add_text("You have nothing to use!");
                        }
                        break;
                    case CMD_EXAMINE:
                        if(game.inv_count > 0) {
                            game.state = STATE_ITEM_SELECT;
                            game.menu_selection = 0;
                        } else {
                            clear_text();
                            add_text("You have nothing to examine!");
                        }
                        break;
                    case CMD_INVENTORY:
                        do_inventory();
                        break;
                    case CMD_HELP:
                        do_help();
                        break;
                }
                break;
                
            case STATE_DIRECTION_SELECT:
                do_go();
                break;
                
            case STATE_ITEM_SELECT:
                // Check what command we're doing
                // Simple approach: default to USE, check for DROP/EXAMINE context
                do_use();
                break;
                
            case STATE_ROOM_ITEM_SELECT:
                do_take();
                break;
        }
    }
}

// ============================================================================
// MAIN
// ============================================================================

int main(void) {
    // Initialize graphics - Mode 0 with text
    REG_DISPCNT = DCNT_MODE0 | DCNT_BG0;
    
    // Initialize tonc text engine
    tte_init_se_default(0, BG_CBB(0) | BG_SBB(31));
    
    // Set up palette (white text on dark blue background)
    pal_bg_mem[0] = RGB15(2, 2, 8);    // Background: dark blue
    pal_bg_mem[1] = RGB15(31, 31, 31); // Text: white
    
    // Initialize game state
    memset(&game, 0, sizeof(game));
    init_rooms();
    
    // Show intro
    clear_text();
    add_text("=== THE FORGOTTEN CRYPT ===");
    add_line("");
    add_text("A Zork-like Adventure");
    add_line("");
    add_text("Legend speaks of the Crown of");
    add_text("Kings, hidden deep within an");
    add_text("ancient crypt. Many have entered.");
    add_text("None have returned.");
    add_line("");
    add_text("Your quest: Find the Crown and");
    add_text("escape alive!");
    add_line("");
    add_text("Press A to begin...");
    
    render_text();
    
    // Wait for A button
    while(1) {
        key_poll();
        if(key_hit(KEY_A)) break;
        VBlankIntrWait();
    }
    
    // Start game
    describe_room();
    
    // Main loop
    while(1) {
        VBlankIntrWait();
        handle_input();
        render_text();
    }
    
    return 0;
}
