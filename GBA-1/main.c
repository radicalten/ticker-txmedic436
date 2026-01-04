#include <tonc.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

// ============================================================================
// CONFIG & CONSTANTS
// ============================================================================
#define RGB_FF_BG       RGB15(0, 0, 20)      // Dark Blue Background
#define RGB_FF_WIN      RGB15(0, 5, 26)      // Classic FF Window Blue
#define RGB_FF_BORDER   RGB15(31, 31, 31)    // White Border
#define RGB_HP_HIGH     RGB15(0, 31, 0)      // Green
#define RGB_HP_LOW      RGB15(31, 0, 0)      // Red
#define SCREEN_W        240
#define SCREEN_H        160

// Game States
enum State { STATE_MENU, STATE_PLAYER_ANIM, STATE_ENEMY_TURN, STATE_WIN, STATE_LOSE };

// ============================================================================
// EMBEDDED TINY FONT (5x7 pixel basic ASCII subset)
// ============================================================================
// Minimal font data to avoid external files. 
// Each byte represents a row of pixels.
const unsigned char font_data[][5] = {
    {0x00,0x00,0x00,0x00,0x00}, // SPACE
    {0x7E,0x11,0x11,0x11,0x7E}, // A
    {0x7F,0x49,0x49,0x49,0x36}, // B
    {0x3E,0x41,0x41,0x41,0x22}, // C
    {0x7F,0x41,0x41,0x22,0x1C}, // D
    {0x7F,0x49,0x49,0x49,0x41}, // E
    {0x7F,0x09,0x09,0x09,0x01}, // F
    {0x3E,0x41,0x49,0x49,0x7A}, // G
    {0x7F,0x08,0x08,0x08,0x7F}, // H
    {0x00,0x41,0x7F,0x41,0x00}, // I
    {0x20,0x40,0x41,0x3F,0x01}, // J
    {0x7F,0x08,0x14,0x22,0x41}, // K
    {0x7F,0x40,0x40,0x40,0x40}, // L
    {0x7F,0x02,0x0C,0x02,0x7F}, // M
    {0x7F,0x04,0x08,0x10,0x7F}, // N
    {0x3E,0x41,0x41,0x41,0x3E}, // O
    {0x7F,0x09,0x09,0x09,0x06}, // P
    {0x3E,0x41,0x51,0x21,0x5E}, // Q
    {0x7F,0x09,0x19,0x29,0x46}, // R
    {0x46,0x49,0x49,0x49,0x31}, // S
    {0x01,0x01,0x7F,0x01,0x01}, // T
    {0x3F,0x40,0x40,0x40,0x3F}, // U
    {0x1F,0x20,0x40,0x20,0x1F}, // V
    {0x3F,0x40,0x38,0x40,0x3F}, // W
    {0x63,0x14,0x08,0x14,0x63}, // X
    {0x07,0x08,0x70,0x08,0x07}, // Y
    {0x61,0x51,0x49,0x45,0x43}, // Z
    {0x3E,0x51,0x49,0x45,0x3E}, // 0
    {0x00,0x42,0x7F,0x40,0x00}, // 1
    {0x42,0x61,0x51,0x49,0x46}, // 2
    {0x21,0x41,0x45,0x4B,0x31}, // 3
    {0x18,0x14,0x12,0x7F,0x10}, // 4
    {0x27,0x45,0x45,0x45,0x39}, // 5
    {0x3C,0x4A,0x49,0x49,0x30}, // 6
    {0x01,0x71,0x09,0x05,0x03}, // 7
    {0x36,0x49,0x49,0x49,0x36}, // 8
    {0x06,0x49,0x49,0x29,0x1E}, // 9
    {0x00,0x36,0x36,0x00,0x00}, // :
    {0x00,0x50,0x30,0x00,0x00}, // .
    {0x08,0x08,0x3E,0x08,0x08}, // +
    {0x08,0x08,0x08,0x08,0x08}, // -
    {0x00,0x60,0x60,0x00,0x00}, // > (Cursor)
    {0x00,0x00,0x00,0x00,0x00}, // NULL
};

// ============================================================================
// GRAPHICS ENGINE
// ============================================================================

// Draw a simple filled rectangle
void draw_rect(int x, int y, int w, int h, u16 color) {
    for (int iy = y; iy < y + h; iy++) {
        if (iy < 0 || iy >= SCREEN_H) continue;
        for (int ix = x; ix < x + w; ix++) {
            if (ix < 0 || ix >= SCREEN_W) continue;
            m3_mem[iy * SCREEN_W + ix] = color;
        }
    }
}

// Draw the classic RPG Blue Window with border
void draw_window(int x, int y, int w, int h) {
    draw_rect(x, y, w, h, RGB_FF_BORDER); // White border
    draw_rect(x + 2, y + 2, w - 4, h - 4, RGB_FF_WIN); // Blue interior
}

// Draw a single character using the embedded font
void draw_char(int x, int y, char c, u16 color) {
    int index = 0;
    if (c == ' ') index = 0;
    else if (c >= 'A' && c <= 'Z') index = c - 'A' + 1;
    else if (c >= '0' && c <= '9') index = c - '0' + 27;
    else if (c == ':') index = 37;
    else if (c == '.') index = 38;
    else if (c == '+') index = 39;
    else if (c == '-') index = 40;
    else if (c == '>') index = 41;

    const unsigned char *glyph = font_data[index];
    
    for (int i = 0; i < 5; i++) {
        unsigned char col = glyph[i];
        for (int j = 0; j < 8; j++) {
            if ((col >> j) & 1) {
                m3_mem[(y + 7 - j) * SCREEN_W + (x + i)] = color;
            }
        }
    }
}

// Draw a string
void draw_text(int x, int y, const char *str, u16 color) {
    while (*str) {
        draw_char(x, y, *str, color);
        x += 6; // Character width + spacing
        str++;
    }
}

// Draw an HP Bar
void draw_hp_bar(int x, int y, int hp, int max_hp) {
    draw_rect(x, y, 52, 6, RGB_WHITE); // Border
    draw_rect(x + 1, y + 1, 50, 4, RGB_BLACK); // Back
    
    if (max_hp <= 0) max_hp = 1;
    int fill_w = (hp * 50) / max_hp;
    if (fill_w < 0) fill_w = 0;
    if (fill_w > 50) fill_w = 50;

    u16 col = (hp < max_hp / 4) ? RGB_HP_LOW : RGB_HP_HIGH;
    draw_rect(x + 1, y + 1, fill_w, 4, col);
}

// ============================================================================
// GAME LOGIC
// ============================================================================

typedef struct {
    char name[10];
    int hp;
    int max_hp;
    int x, y;
    u16 color;
} Fighter;

Fighter player;
Fighter boss;
int cursor_y = 0;
int game_state = STATE_MENU;
char msg_buffer[32];
int frame_counter = 0;
int anim_timer = 0;

void init_game() {
    strcpy(player.name, "CLOUD");
    player.max_hp = 200;
    player.hp = 200;
    player.x = 180;
    player.y = 70;
    player.color = RGB15(0, 31, 10); // Cyan-ish

    strcpy(boss.name, "BAHAMUT");
    boss.max_hp = 1000;
    boss.hp = 1000;
    boss.x = 40;
    boss.y = 50;
    boss.color = RGB15(31, 5, 5); // Red

    game_state = STATE_MENU;
    cursor_y = 0;
    strcpy(msg_buffer, "COMMAND?");
}

void draw_scene() {
    // 1. Clear Screen (Top half only to save performance, bottom is UI)
    draw_rect(0, 0, SCREEN_W, 110, RGB_FF_BG);

    // 2. Draw Boss (Simple shapes)
    draw_rect(boss.x, boss.y, 40, 40, boss.color);
    // Boss Wings
    draw_rect(boss.x - 20, boss.y - 10, 20, 30, boss.color); 
    draw_rect(boss.x + 40, boss.y - 10, 20, 30, boss.color); 

    // 3. Draw Player
    draw_rect(player.x, player.y, 16, 24, player.color); // Body
    draw_rect(player.x - 4, player.y + 4, 4, 20, RGB15(20,20,20)); // Sword

    // 4. Draw UI Area
    draw_window(5, 115, 230, 40); // Main Box

    // 5. Draw Names and HP
    draw_text(10, 120, player.name, RGB_WHITE);
    draw_text(60, 120, "HP", RGB_WHITE);
    draw_hp_bar(80, 120, player.hp, player.max_hp);
    
    // Convert HP to string manually
    char hp_str[10];
    siprintf(hp_str, "%d/%d", player.hp, player.max_hp);
    draw_text(140, 120, hp_str, RGB_WHITE);

    // 6. Draw Menu or Message
    draw_rect(10, 132, 220, 1, RGB15(10,10,10)); // Separator line

    if (game_state == STATE_MENU) {
        // Draw Commands
        draw_text(20, 138, "ATTACK", RGB_WHITE);
        draw_text(70, 138, "MAGIC", RGB_WHITE);
        draw_text(120, 138, "ITEM", RGB_WHITE);
        
        // Draw Cursor
        int cx = 12;
        if (cursor_y == 0) cx = 12; // Attack
        if (cursor_y == 1) cx = 62; // Magic
        if (cursor_y == 2) cx = 112; // Item
        draw_char(cx, 138, '>', RGB_YELLOW);
    } else {
        // Draw Status Message
        draw_text(20, 138, msg_buffer, RGB_YELLOW);
    }
}

// Simple blocking delay for visual effects
void sleep_frames(int frames) {
    for(int i=0; i<frames; i++) {
        vid_vsync();
    }
}

void game_loop() {
    key_poll();
    
    switch (game_state) {
        case STATE_MENU:
            if (key_hit(KEY_RIGHT)) cursor_y = (cursor_y + 1) % 3;
            if (key_hit(KEY_LEFT)) cursor_y = (cursor_y + 2) % 3; // wrap around
            
            if (key_hit(KEY_A)) {
                if (cursor_y == 0) { // ATTACK
                    strcpy(msg_buffer, "CLOUD ATTACKS!");
                    game_state = STATE_PLAYER_ANIM;
                    anim_timer = 0;
                } 
                else if (cursor_y == 1) { // MAGIC (Heal for this demo)
                    strcpy(msg_buffer, "CAST CURE!");
                    game_state = STATE_PLAYER_ANIM;
                    anim_timer = 0;
                }
                else { // ITEM
                    strcpy(msg_buffer, "NO ITEMS!");
                    // Just stay in menu or waste turn
                    draw_scene(); 
                    sleep_frames(30);
                }
            }
            break;

        case STATE_PLAYER_ANIM:
            draw_scene();
            sleep_frames(30); // Wait so user sees text

            if (cursor_y == 0) { // Attack Logic
                // Flash Boss
                draw_rect(boss.x, boss.y, 40, 40, RGB_WHITE);
                vid_vsync();
                sleep_frames(5);
                
                int dmg = (qran_range(20, 40));
                boss.hp -= dmg;
                if(boss.hp < 0) boss.hp = 0;
                
                siprintf(msg_buffer, "HIT! %d DAMAGE", dmg);
            } 
            else if (cursor_y == 1) { // Heal Logic
                // Flash Player Green
                draw_rect(player.x, player.y, 16, 24, RGB_HP_HIGH);
                vid_vsync();
                sleep_frames(5);

                int heal = 50;
                player.hp += heal;
                if(player.hp > player.max_hp) player.hp = player.max_hp;
                siprintf(msg_buffer, "RECOVERED %d HP", heal);
            }

            draw_scene();
            sleep_frames(60); // Read result

            if (boss.hp <= 0) {
                game_state = STATE_WIN;
            } else {
                game_state = STATE_ENEMY_TURN;
            }
            break;

        case STATE_ENEMY_TURN:
            strcpy(msg_buffer, "BAHAMUT ATTACKS!");
            draw_scene();
            sleep_frames(40);

            // Flash Player Red
            draw_rect(player.x, player.y, 16, 24, RGB_HP_LOW);
            vid_vsync();
            sleep_frames(5);

            int dmg = (qran_range(15, 35));
            player.hp -= dmg;
            if(player.hp < 0) player.hp = 0;

            siprintf(msg_buffer, "OUCH! %d DAMAGE", dmg);
            draw_scene();
            sleep_frames(60);

            if (player.hp <= 0) {
                game_state = STATE_LOSE;
            } else {
                game_state = STATE_MENU;
                strcpy(msg_buffer, "COMMAND?");
            }
            break;

        case STATE_WIN:
            draw_window(60, 50, 120, 40);
            draw_text(85, 65, "VICTORY!", RGB_YELLOW);
            if (key_hit(KEY_START)) init_game();
            break;

        case STATE_LOSE:
            draw_window(60, 50, 120, 40);
            draw_text(75, 65, "GAME OVER...", RGB_RED);
            if (key_hit(KEY_START)) init_game();
            break;
    }
}

// ============================================================================
// MAIN
// ============================================================================
int main() {
    // Set Video Mode 3 (240x160 Bitmap, 16-bit color)
    // This is easiest for single-file projects (no tile conversion needed)
    REG_DISPCNT = DCNT_MODE3 | DCNT_BG2;

    init_game();

    while (1) {
        vid_vsync();
        game_loop();
        if (game_state != STATE_PLAYER_ANIM && game_state != STATE_ENEMY_TURN) {
            draw_scene(); // Only draw if we aren't inside a blocking animation helper
        }
    }

    return 0;
}
