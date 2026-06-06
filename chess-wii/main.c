#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <gccore.h>
#include <wiiuse/wpad.h>
#include <fat.h>

#define MAX_HISTORY 2048

// Board representation
typedef struct {
    int board[64]; // P=1, N=2, B=3, R=4, Q=5, K=6 (Positive=White, Negative=Black)
    int turn;      // 1 = White, -1 = Black
    int castle;    // Bitmask: 1=WK, 2=WQ, 4=BK, 8=BQ
    int ep;        // En-passant square (0-63), -1 if none
    int halfmoves; // For 50-move rule
    int fullmoves;
} BoardState;

typedef struct {
    int from;
    int to;
    int promo; // 0=None, 2=N, 3=B, 4=R, 5=Q
} Move;

// Global GUI state
BoardState current_state;
BoardState history[MAX_HISTORY];
Move move_history[MAX_HISTORY];
int history_count = 0;

int cursor_r = 6;  // Screen row (0-7)
int cursor_c = 4;  // Screen col (0-7)
int selected_sq = -1;

int board_orientation = 1; // 1 = White on bottom, -1 = Black on bottom
int user_side = 1;         // 1 = White, -1 = Black, 0 = Hotseat, 2 = Watch (AI vs AI)

int time_control_type = 0;   // 0 = Time (ms), 1 = Depth, 2 = Nodes
int time_control_val = 1000; // Default: 1000 ms

char engine_path[256] = "sd:/apps/wiichess/engine.dol";
char gui_path[256] = "sd:/apps/wiichess/boot.dol";

// Wii Console Globals
static void *xfb = NULL;
static GXRModeObj *rmode = NULL;

// Forward declarations
void init_board(BoardState *state);
int is_legal_move(const BoardState *state, Move m);
int has_legal_moves(const BoardState *state);
int is_square_attacked(const BoardState *state, int sq, int attacker);
void make_move(const BoardState *src, BoardState *dst, Move m);
void print_side_panel_line(int panel_row);
void print_recent_moves(int row);
int find_king(const BoardState *state, int color);
int count_repetitions(const BoardState *state);
int get_promo_choice();
void save_state();
int load_state();
void trigger_engine_move();

// Structure representing standard DOL executable headers
typedef struct {
    u32 text_pos[7];
    u32 data_pos[11];
    u32 text_start[7];
    u32 data_start[11];
    u32 text_size[7];
    u32 data_size[11];
    u32 bss_start;
    u32 bss_size;
    u32 entry_point;
} dolheader;

// Safe runtime DOL staging blocks
typedef struct {
    u32 dest;
    void *src;
    u32 size;
} StagedBlock;

typedef struct {
    StagedBlock staged_text[7];
    StagedBlock staged_data[11];
    u32 bss_start;
    u32 bss_size;
    u32 entry_point;
} LoaderParams;

// Custom high-RAM static allocator to prevent Heap collisions
static void *high_alloc(u32 size) {
    static u8 *alloc_ptr = (u8 *)0x81000000;
    void *res = alloc_ptr;
    alloc_ptr += (size + 31) & ~31; // 32-byte alignment
    return res;
}

// Position-independent assembly-free loader running in safe high memory space
static void __attribute__((noinline)) loader_entry(LoaderParams *params) {
    for (int i = 0; i < 7; i++) {
        if (params->staged_text[i].size > 0) {
            volatile u8 *dst = (volatile u8 *)params->staged_text[i].dest;
            volatile u8 *src = (volatile u8 *)params->staged_text[i].src;
            u32 size = params->staged_text[i].size;
            for (u32 j = 0; j < size; j++) {
                dst[j] = src[j];
            }
        }
    }
    for (int i = 0; i < 11; i++) {
        if (params->staged_data[i].size > 0) {
            volatile u8 *dst = (volatile u8 *)params->staged_data[i].dest;
            volatile u8 *src = (volatile u8 *)params->staged_data[i].src;
            u32 size = params->staged_data[i].size;
            for (u32 j = 0; j < size; j++) {
                dst[j] = src[j];
            }
        }
    }
    if (params->bss_size > 0) {
        volatile u8 *dst = (volatile u8 *)params->bss_start;
        u32 size = params->bss_size;
        for (u32 j = 0; j < size; j++) {
            dst[j] = 0;
        }
    }

    // Safe hardware invalidation on cache bounds
    for (int i = 0; i < 7; i++) {
        if (params->staged_text[i].size > 0) {
            u32 start = params->staged_text[i].dest & ~31;
            u32 end = (params->staged_text[i].dest + params->staged_text[i].size + 31) & ~31;
            for (u32 addr = start; addr < end; addr += 32) {
                __asm__ volatile("dcbst 0,%0 ; sync ; icbi 0,%0" : : "r"(addr));
            }
        }
    }
    for (int i = 0; i < 11; i++) {
        if (params->staged_data[i].size > 0) {
            u32 start = params->staged_data[i].dest & ~31;
            u32 end = (params->staged_data[i].dest + params->staged_data[i].size + 31) & ~31;
            for (u32 addr = start; addr < end; addr += 32) {
                __asm__ volatile("dcbst 0,%0 ; sync" : : "r"(addr));
            }
        }
    }
    __asm__ volatile("sync ; isync");

    typedef void (*entry_point)(void);
    entry_point start = (entry_point)params->entry_point;
    start();
}

void run_dol(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) {
        printf("\nError: Failed to open %s\n", path);
        VIDEO_WaitVSync();
        SYS_ResetSystem(SYS_RETURNTOMENU, 0, 0);
    }

    dolheader header;
    fread(&header, 1, sizeof(dolheader), f);

    // Carve structures out of safe high RAM rather than standard heap
    LoaderParams *params = (LoaderParams *)high_alloc(sizeof(LoaderParams));
    memset(params, 0, sizeof(LoaderParams));
    params->bss_start = header.bss_start;
    params->bss_size = header.bss_size;
    params->entry_point = header.entry_point;

    for (int i = 0; i < 7; i++) {
        params->staged_text[i].size = header.text_size[i];
        if (params->staged_text[i].size > 0) {
            params->staged_text[i].dest = header.text_start[i];
            params->staged_text[i].src = high_alloc(params->staged_text[i].size);
            fseek(f, header.text_pos[i], SEEK_SET);
            fread(params->staged_text[i].src, 1, params->staged_text[i].size, f);
        }
    }

    for (int i = 0; i < 11; i++) {
        params->staged_data[i].size = header.data_size[i];
        if (params->staged_data[i].size > 0) {
            params->staged_data[i].dest = header.data_start[i];
            params->staged_data[i].src = high_alloc(params->staged_data[i].size);
            fseek(f, header.data_pos[i], SEEK_SET);
            fread(params->staged_data[i].src, 1, params->staged_data[i].size, f);
        }
    }
    fclose(f);

    WPAD_Shutdown();

    // FIXED: Use a hardcoded size instead of subtracting function pointers to bypass GCC function reordering optimizer crashes.
    u32 loader_size = 1024; 
    void *loader_target = (void *)0x81400000;
    memcpy(loader_target, (void *)loader_entry, loader_size);

    DCFlushRange(loader_target, loader_size);
    ICInvalidateRange(loader_target, loader_size);

    typedef void (*loader_fn)(LoaderParams *);
    loader_fn run_relocated = (loader_fn)loader_target;
    run_relocated(params);
}

void init_wii_console() {
    VIDEO_Init();
    WPAD_Init();
    rmode = VIDEO_GetPreferredMode(NULL);
    xfb = MEM_K0_TO_K1(SYS_AllocateFramebuffer(rmode));
    console_init(xfb, 20, 20, rmode->fbWidth, rmode->xfbHeight, rmode->fbWidth * VI_DISPLAY_PIX_SZ);
    VIDEO_Configure(rmode);
    VIDEO_SetNextFramebuffer(xfb);
    VIDEO_SetBlack(FALSE);
    VIDEO_Flush();
    VIDEO_WaitVSync();
    if (rmode->viTVMode & VI_NON_INTERLACE) VIDEO_WaitVSync();
}

int screen_to_board_sq(int r, int c) {
    if (board_orientation == 1) {
        return r * 8 + c;
    } else {
        return (7 - r) * 8 + (7 - c);
    }
}

void move_to_uci(Move m, char *buf) {
    int f_col = m.from % 8;
    int f_row = 8 - (m.from / 8);
    int t_col = m.to % 8;
    int t_row = 8 - (m.to / 8);
    sprintf(buf, "%c%d%c%d", 'a' + f_col, f_row, 'a' + t_col, t_row);
    if (m.promo != 0) {
        char p = 'q';
        if (m.promo == 2) p = 'n';
        if (m.promo == 3) p = 'b';
        if (m.promo == 4) p = 'r';
        sprintf(buf + 4, "%c", p);
    }
}

Move uci_to_move(const char *str) {
    Move m = {-1, -1, 0};
    if (strlen(str) < 4) return m;
    int f_col = str[0] - 'a';
    int f_row = 8 - (str[1] - '0');
    int t_col = str[2] - 'a';
    int t_row = 8 - (str[3] - '0');
    m.from = f_row * 8 + f_col;
    m.to = t_row * 8 + t_col;
    if (strlen(str) >= 5) {
        char p = str[4];
        if (p == 'n') m.promo = 2;
        else if (p == 'b') m.promo = 3;
        else if (p == 'r') m.promo = 4;
        else m.promo = 5;
    }
    return m;
}

void push_state(const BoardState *state, Move m) {
    if (history_count < MAX_HISTORY - 1) {
        history[history_count] = *state;
        move_history[history_count] = m;
        history_count++;
    }
}

void save_state() {
    FILE *f = fopen("sd:/apps/wiichess/state.bin", "wb");
    if (f) {
        fwrite(&current_state, sizeof(BoardState), 1, f);
        fwrite(&history_count, sizeof(int), 1, f);
        if (history_count > 0) {
            fwrite(history, sizeof(BoardState), history_count, f);
            fwrite(move_history, sizeof(Move), history_count, f);
        }
        fwrite(&board_orientation, sizeof(int), 1, f);
        fwrite(&user_side, sizeof(int), 1, f);
        fwrite(&time_control_type, sizeof(int), 1, f);
        fwrite(&time_control_val, sizeof(int), 1, f);
        fclose(f);
    }
}

int load_state() {
    FILE *f = fopen("sd:/apps/wiichess/state.bin", "rb");
    if (f) {
        fread(&current_state, sizeof(BoardState), 1, f);
        fread(&history_count, sizeof(int), 1, f);
        if (history_count > 0) {
            fread(history, sizeof(BoardState), history_count, f);
            fread(move_history, sizeof(Move), history_count, f);
        }
        fread(&board_orientation, sizeof(int), 1, f);
        fread(&user_side, sizeof(int), 1, f);
        fread(&time_control_type, sizeof(int), 1, f);
        fread(&time_control_val, sizeof(int), 1, f);
        fclose(f);
        return 1;
    }
    return 0;
}

void trigger_engine_move() {
    FILE *f_uci = fopen("sd:/apps/wiichess/position.uci", "w");
    if (f_uci) {
        fprintf(f_uci, "position startpos moves");
        for (int i = 0; i < history_count; i++) {
            char uci_m[10];
            move_to_uci(move_history[i], uci_m);
            fprintf(f_uci, " %s", uci_m);
        }
        fprintf(f_uci, "\n");
        
        if (time_control_type == 0) {
            fprintf(f_uci, "go movetime %d\n", time_control_val);
        } else if (time_control_type == 1) {
            fprintf(f_uci, "go depth %d\n", time_control_val);
        } else {
            fprintf(f_uci, "go nodes %d\n", time_control_val);
        }
        fclose(f_uci);
    }

    save_state();
    run_dol(engine_path);
}

// Rules engine validation systems
int find_king(const BoardState *state, int color) {
    for (int i = 0; i < 64; i++) {
        if (state->board[i] == color * 6) return i;
    }
    return -1;
}

int is_square_attacked(const BoardState *state, int sq, int attacker) {
    int r = sq / 8, c = sq % 8;

    int k_r[] = {-2, -2, -1, -1, 1, 1, 2, 2};
    int k_c[] = {-1, 1, -2, 2, -2, 2, -1, 1};
    for (int i = 0; i < 8; i++) {
        int nr = r + k_r[i], nc = c + k_c[i];
        if (nr >= 0 && nr < 8 && nc >= 0 && nc < 8) {
            if (state->board[nr * 8 + nc] == attacker * 2) return 1;
        }
    }

    int kg_r[] = {-1, -1, -1, 0, 0, 1, 1, 1};
    int kg_c[] = {-1, 0, 1, -1, 1, -1, 0, 1};
    for (int i = 0; i < 8; i++) {
        int nr = r + kg_r[i], nc = c + kg_c[i];
        if (nr >= 0 && nr < 8 && nc >= 0 && nc < 8) {
            if (state->board[nr * 8 + nc] == attacker * 6) return 1;
        }
    }

    int p_offset = (attacker == 1) ? 1 : -1;
    for (int dc = -1; dc <= 1; dc += 2) {
        int nr = r + p_offset, nc = c + dc;
        if (nr >= 0 && nr < 8 && nc >= 0 && nc < 8) {
            if (state->board[nr * 8 + nc] == attacker * 1) return 1;
        }
    }

    int d_r[] = {-1, -1, 1, 1};
    int d_c[] = {-1, 1, -1, 1};
    for (int i = 0; i < 4; i++) {
        int nr = r + d_r[i], nc = c + d_c[i];
        while (nr >= 0 && nr < 8 && nc >= 0 && nc < 8) {
            int target = state->board[nr * 8 + nc];
            if (target != 0) {
                if (target == attacker * 3 || target == attacker * 5) return 1;
                break;
            }
            nr += d_r[i]; nc += d_c[i];
        }
    }

    int s_r[] = {-1, 1, 0, 0};
    int s_c[] = {0, 0, -1, 1};
    for (int i = 0; i < 4; i++) {
        int nr = r + s_r[i], nc = c + s_c[i];
        while (nr >= 0 && nr < 8 && nc >= 0 && nc < 8) {
            int target = state->board[nr * 8 + nc];
            if (target != 0) {
                if (target == attacker * 4 || target == attacker * 5) return 1;
                break;
            }
            nr += s_r[i]; nc += s_c[i];
        }
    }
    return 0;
}

int is_pseudo_legal_move(const BoardState *state, Move m) {
    int p = state->board[m.from];
    int target = state->board[m.to];
    int turn = state->turn;

    if (p == 0) return 0;
    if ((turn == 1 && p < 0) || (turn == -1 && p > 0)) return 0;
    if (m.from == m.to) return 0;
    if (target != 0 && ((turn == 1 && target > 0) || (turn == -1 && target < 0))) return 0;

    int fr = m.from / 8, fc = m.from % 8;
    int tr = m.to / 8, tc = m.to % 8;
    int dr = tr - fr, dc = tc - fc;
    int abs_dr = abs(dr), abs_dc = abs(dc);

    switch (abs(p)) {
        case 1: { 
            int dir = (turn == 1) ? -1 : 1;
            if (dc == 0 && dr == dir && target == 0) return 1;
            int start_r = (turn == 1) ? 6 : 1;
            if (dc == 0 && fr == start_r && dr == 2 * dir) {
                if (state->board[(fr + dir) * 8 + fc] == 0 && target == 0) return 1;
            }
            if (abs_dc == 1 && dr == dir) {
                if (target != 0) return 1;
                if (m.to == state->ep) return 1;
            }
            return 0;
        }
        case 2: 
            return ((abs_dr == 2 && abs_dc == 1) || (abs_dr == 1 && abs_dc == 2));
        case 3: { 
            if (abs_dr != abs_dc) return 0;
            int sr = (dr > 0) ? 1 : -1, sc = (dc > 0) ? 1 : -1;
            int r = fr + sr, c = fc + sc;
            while (r != tr && c != tc) {
                if (state->board[r * 8 + c] != 0) return 0;
                r += sr; c += sc;
            }
            return 1;
        }
        case 4: { 
            if (dr != 0 && dc != 0) return 0;
            int sr = (dr == 0) ? 0 : ((dr > 0) ? 1 : -1);
            int sc = (dc == 0) ? 0 : ((dc > 0) ? 1 : -1);
            int r = fr + sr, c = fc + sc;
            while (r != tr || c != tc) {
                if (state->board[r * 8 + c] != 0) return 0;
                r += sr; c += sc;
            }
            return 1;
        }
        case 5: { 
            if (abs_dr != abs_dc && dr != 0 && dc != 0) return 0;
            int sr = (dr == 0) ? 0 : ((dr > 0) ? 1 : -1);
            int sc = (dc == 0) ? 0 : ((dc > 0) ? 1 : -1);
            if (abs_dr == abs_dc) {
                sr = (dr > 0) ? 1 : -1;
                sc = (dc > 0) ? 1 : -1;
            }
            int r = fr + sr, c = fc + sc;
            while (r != tr || c != tc) {
                if (state->board[r * 8 + c] != 0) return 0;
                r += sr; c += sc;
            }
            return 1;
        }
        case 6: { 
            if (abs_dr <= 1 && abs_dc <= 1) return 1;
            if (dr == 0 && abs_dc == 2) {
                if (turn == 1 && fr == 7 && fc == 4) {
                    if (m.to == 62 && (state->castle & 1)) {
                        if (state->board[61] == 0 && state->board[62] == 0) {
                            if (!is_square_attacked(state, 60, -1) &&
                                !is_square_attacked(state, 61, -1) &&
                                !is_square_attacked(state, 62, -1)) return 1;
                        }
                    }
                    if (m.to == 58 && (state->castle & 2)) {
                        if (state->board[59] == 0 && state->board[58] == 0 && state->board[57] == 0) {
                            if (!is_square_attacked(state, 60, -1) &&
                                !is_square_attacked(state, 59, -1) &&
                                !is_square_attacked(state, 58, -1)) return 1;
                        }
                    }
                }
                if (turn == -1 && fr == 0 && fc == 4) {
                    if (m.to == 6 && (state->castle & 4)) {
                        if (state->board[5] == 0 && state->board[6] == 0) {
                            if (!is_square_attacked(state, 4, 1) &&
                                !is_square_attacked(state, 5, 1) &&
                                !is_square_attacked(state, 6, 1)) return 1;
                        }
                    }
                    if (m.to == 2 && (state->castle & 8)) {
                        if (state->board[3] == 0 && state->board[2] == 0 && state->board[1] == 0) {
                            if (!is_square_attacked(state, 4, 1) &&
                                !is_square_attacked(state, 3, 1) &&
                                !is_square_attacked(state, 2, 1)) return 1;
                        }
                    }
                }
            }
            return 0;
        }
    }
    return 0;
}

int is_legal_move(const BoardState *state, Move m) {
    if (!is_pseudo_legal_move(state, m)) return 0;
    BoardState next;
    make_move(state, &next, m);
    int king = find_king(&next, state->turn);
    if (king == -1) return 0;
    return !is_square_attacked(&next, king, -state->turn);
}

int has_legal_moves(const BoardState *state) {
    for (int f = 0; f < 64; f++) {
        if (state->board[f] == 0) continue;
        if ((state->turn == 1 && state->board[f] < 0) || (state->turn == -1 && state->board[f] > 0)) continue;
        for (int t = 0; t < 64; t++) {
            Move m = {f, t, 0};
            if (abs(state->board[f]) == 1 && (t / 8 == 0 || t / 8 == 7)) {
                m.promo = 5; 
            }
            if (is_legal_move(state, m)) return 1;
        }
    }
    return 0;
}

int count_repetitions(const BoardState *state) {
    int count = 1; 
    for (int i = 0; i < history_count; i++) {
        if (state->turn == history[i].turn &&
            state->castle == history[i].castle &&
            state->ep == history[i].ep &&
            memcmp(state->board, history[i].board, sizeof(state->board)) == 0) {
            count++;
        }
    }
    return count;
}

void make_move(const BoardState *src, BoardState *dst, Move m) {
    *dst = *src;
    int p = dst->board[m.from];
    int is_capture = (src->board[m.to] != 0) || (abs(p) == 1 && m.to == src->ep);

    dst->board[m.from] = 0;

    if (m.promo != 0) {
        dst->board[m.to] = dst->turn * m.promo;
    } else {
        dst->board[m.to] = p;
    }

    if (abs(p) == 1 && m.to == dst->ep) {
        int p_dir = (dst->turn == 1 ? 8 : -8);
        int mt_cap = m.to + p_dir;
        dst->board[mt_cap] = 0;
    }

    dst->ep = -1;
    if (abs(p) == 1 && abs(m.from - m.to) == 16) {
        dst->ep = m.from + (dst->turn == 1 ? -8 : 8);
    }

    if (abs(p) == 6) {
        if (m.from == 60 && m.to == 62) { dst->board[61] = dst->board[63]; dst->board[63] = 0; }
        else if (m.from == 60 && m.to == 58) { dst->board[59] = dst->board[56]; dst->board[56] = 0; }
        else if (m.from == 4 && m.to == 6) { dst->board[5] = dst->board[7]; dst->board[7] = 0; }
        else if (m.from == 4 && m.to == 2) { dst->board[3] = dst->board[0]; dst->board[0] = 0; }
    }

    if (m.from == 60) dst->castle &= ~1 & ~2;
    if (m.from == 4)  dst->castle &= ~4 & ~8;
    if (m.from == 63 || m.to == 63) dst->castle &= ~1;
    if (m.from == 56 || m.to == 56) dst->castle &= ~2;
    if (m.from == 7  || m.to == 7)  dst->castle &= ~4;
    if (m.from == 0  || m.to == 0)  dst->castle &= ~8;

    dst->turn = -dst->turn;
    
    if (abs(p) == 1 || is_capture) dst->halfmoves = 0;
    else dst->halfmoves++;
    
    if (dst->turn == 1) dst->fullmoves++;
}

void draw_ui() {
    printf("\033[H\r\n"); 

    const char *turn_str = (current_state.turn == 1) ? "\033[1;33mWhite\033[0m" : "\033[1;35mBlack\033[0m";
    int king = find_king(&current_state, current_state.turn);
    int is_ch = is_square_attacked(&current_state, king, -current_state.turn);
    int has_mov = has_legal_moves(&current_state);
    const char *w_play = (user_side == 1 || user_side == 0) ? "Hum" : "Eng";
    const char *b_play = (user_side == -1 || user_side == 0) ? "Hum" : "Eng";
    int repetitions = count_repetitions(&current_state);

    printf("  ");
    if (current_state.halfmoves >= 100) {
        printf("\033[1;36mDRAW (50-move rule)\033[0m");
    } else if (repetitions >= 3) {
        printf("\033[1;36mDRAW (threefold repetition)\033[0m");
    } else if (!has_mov) {
        if (is_ch) printf("\033[1;31mCHECKMATE!\033[0m");
        else printf("\033[1;36mSTALEMATE!\033[0m");
    } else if (is_ch) {
        printf("%s (\033[1;31mCHECK!\033[0m)", turn_str);
    } else {
        printf("%s's Turn", turn_str);
    }
    printf(" | W:%s B:%s", w_play, b_play);

    const char *types[] = {"Time-Limit", "Depth-Limit", "Node-Limit"};
    printf(" | %s", types[time_control_type]);
    if (time_control_type == 0) {
        printf(" (%d ms)", time_control_val);
    } else if (time_control_type == 1) {
        printf(" (depth %d)", time_control_val);
    } else {
        printf(" (nodes %d)", time_control_val);
    }
    printf("\033[K\r\n\r\n");

    if (board_orientation == 1) {
        printf("     a  b  c  d  e  f  g  h    ");
    } else {
        printf("     h  g  f  e  d  c  b  a    ");
    }
    print_side_panel_line(0);
    printf("\033[K\r\n");

    int king_in_check = -1;
    int w_king = find_king(&current_state, 1);
    int b_king = find_king(&current_state, -1);
    if (w_king != -1 && is_square_attacked(&current_state, w_king, -1)) {
        king_in_check = w_king;
    } else if (b_king != -1 && is_square_attacked(&current_state, b_king, 1)) {
        king_in_check = b_king;
    }

    for (int r = 0; r < 8; r++) {
        int rank_lbl = (board_orientation == 1) ? (8 - r) : (r + 1);
        printf("  %d ", rank_lbl);

        for (int c = 0; c < 8; c++) {
            int sq = screen_to_board_sq(r, c);
            int p = current_state.board[sq];

            int is_light = ((sq / 8) + (sq % 8)) % 2 == 0;
            const char *bg_color;

            int is_selected = (sq == selected_sq);
            int is_cursor = (r == cursor_r && c == cursor_c);

            int is_prev_move = 0;
            if (history_count > 0) {
                Move last_move = move_history[history_count - 1];
                if (sq == last_move.from || sq == last_move.to) {
                    is_prev_move = 1;
                }
            }

            int is_legal_dest = 0;
            if (selected_sq != -1) {
                Move test_m = {selected_sq, sq, 0};
                if (abs(current_state.board[selected_sq]) == 1 && (sq / 8 == 0 || sq / 8 == 7)) {
                    test_m.promo = 5;
                }
                if (is_legal_move(&current_state, test_m)) {
                    is_legal_dest = 1;
                }
            }

            if (is_cursor) {
                bg_color = "\033[48;5;208m"; 
            } else if (is_selected) {
                bg_color = "\033[48;5;34m";  
            } else if (sq == king_in_check) {
                bg_color = "\033[48;5;196m"; 
            } else if (is_prev_move) {
                bg_color = is_light ? "\033[48;5;75m" : "\033[48;5;68m"; 
            } else if (is_legal_dest) {
                bg_color = is_light ? "\033[48;5;151m" : "\033[48;5;108m"; 
            } else {
                bg_color = is_light ? "\033[48;5;180m" : "\033[48;5;94m"; 
            }

            const char *piece_str = " ";
            const char *fg_color = "\033[38;5;232m"; 
            if (p != 0) {
                if (p > 0) fg_color = "\033[38;5;255m\033[1m"; 
                switch (abs(p)) {
                    case 1: piece_str = "P"; break;
                    case 2: piece_str = "N"; break;
                    case 3: piece_str = "B"; break;
                    case 4: piece_str = "R"; break;
                    case 5: piece_str = "Q"; break;
                    case 6: piece_str = "K"; break;
                }
            }

            printf("%s%s %s \033[0m", bg_color, fg_color, piece_str);
        }

        printf(" %d ", rank_lbl);
        print_side_panel_line(r + 1);
        printf("\033[K\r\n"); 
    }

    if (board_orientation == 1) {
        printf("     a  b  c  d  e  f  g  h    ");
    } else {
        printf("     h  g  f  e  d  c  b  a    ");
    }
    print_side_panel_line(9);
    printf("\033[K\r\n\r\n");

    printf(" \033[38;5;245m[D-Pad] Navigate | [A] Select | [B] Undo | [Home] Exit\033[0m\033[K\r\n");
    printf(" \033[38;5;245m[-] Flip Board   | [+] Switch Sides | [1] Limit Mode\033[0m\033[K\r\n");
    printf(" \033[38;5;245m[2] Adjust Val\033[0m\033[K\r\n\r\n");
    
    printf(" \033[38;5;248mEngine Path:\033[0m %s", engine_path);
    printf("\033[K\r\n\033[J");
    fflush(stdout);
}

void print_side_panel_line(int panel_row) {
    printf("   ");
    print_recent_moves(panel_row);
}

void print_recent_moves(int row) {
    int total_full_moves = (history_count + 1) / 2;
    if (total_full_moves == 0) {
        return; 
    }
    int start_move = 1;
    if (total_full_moves > 10) {
        start_move = total_full_moves - 9;
    }
    int display = start_move + row;
    if (display > total_full_moves) {
        return; 
    }

    int w_idx = (display - 1) * 2;
    int b_idx = w_idx + 1;

    printf("   %2d. ", display);
    if (w_idx < history_count) {
        char w_str[10];
        move_to_uci(move_history[w_idx], w_str);
        printf("%-6s", w_str);
    } else {
        printf("------");
    }

    printf(" ");

    if (b_idx < history_count) {
        char b_str[10];
        move_to_uci(move_history[b_idx], b_str);
        printf("%-6s", b_str);
    } else {
        if (w_idx < history_count) printf("...");
        else printf("------");
    }
}

int get_promo_choice() {
    printf("\r\n \033[1;33mPromote: [A] Queen, [B] Rook, [-] Bishop, [+] Knight\033[0m");
    fflush(stdout);
    int choice = 5;
    while (1) {
        WPAD_ScanPads();
        u32 pressed = WPAD_ButtonsDown(0);
        if (pressed & WPAD_BUTTON_A) { choice = 5; break; }
        if (pressed & WPAD_BUTTON_B) { choice = 4; break; }
        if (pressed & WPAD_BUTTON_MINUS) { choice = 3; break; }
        if (pressed & WPAD_BUTTON_PLUS) { choice = 2; break; }
        VIDEO_WaitVSync();
    }
    printf("\r\033[K\033[A\033[K");
    fflush(stdout);
    return choice;
}

void handle_select() {
    if (!has_legal_moves(&current_state) || current_state.halfmoves >= 100 || count_repetitions(&current_state) >= 3) {
        return;
    }

    int sq = screen_to_board_sq(cursor_r, cursor_c);
    if (selected_sq == -1) {
        int p = current_state.board[sq];
        if (p != 0 && ((current_state.turn == 1 && p > 0) || (current_state.turn == -1 && p < 0))) {
            selected_sq = sq;
        }
    } else {
        Move m = {selected_sq, sq, 0};
        int p = current_state.board[selected_sq];
        int is_promo = (abs(p) == 1 && (sq / 8 == 0 || sq / 8 == 7));
        if (is_promo) {
            m.promo = 5; 
        }

        if (is_legal_move(&current_state, m)) {
            if (is_promo) {
                m.promo = get_promo_choice();
            }
            push_state(&current_state, m);
            BoardState next;
            make_move(&current_state, &next, m);
            current_state = next;
            selected_sq = -1;
            save_state(); 
        } else {
            int target = current_state.board[sq];
            if (target != 0 && ((current_state.turn == 1 && target > 0) || (current_state.turn == -1 && target < 0))) {
                selected_sq = sq; 
            } else {
                selected_sq = -1; 
            }
        }
    }
}

void handle_undo() {
    int step_back = (user_side == 1 || user_side == -1) ? 2 : 1;
    while (step_back > 0 && history_count > 0) {
        history_count--;
        current_state = history[history_count];
        step_back--;
    }
    selected_sq = -1;
    save_state();
}

void handle_reset_board() {
    init_board(&current_state);
    history_count = 0;
    selected_sq = -1;
    cursor_r = 6;
    cursor_c = 4;
    save_state();
}

void handle_switch_sides() {
    if (user_side == 1) user_side = -1;
    else if (user_side == -1) user_side = 0;
    else if (user_side == 0) user_side = 2;
    else user_side = 1;
    save_state();
}

void adjust_time_control() {
    if (time_control_type == 0) { 
        int time_list[] = {50, 100, 250, 500, 1000, 1500, 2000, 3000, 5000, 10000};
        int time_count = sizeof(time_list) / sizeof(time_list[0]);
        int found_idx = -1;
        for (int i = 0; i < time_count; i++) {
            if (time_control_val == time_list[i]) {
                found_idx = i;
                break;
            }
        }
        if (found_idx == -1 || found_idx == time_count - 1) {
            time_control_val = time_list[0];
        } else {
            time_control_val = time_list[found_idx + 1];
        }
    } else if (time_control_type == 1) { 
        time_control_val = (time_control_val % 20) + 1;
    } else { 
        int nodes_list[] = {512, 1024, 2048, 4096, 8192, 16384, 32768, 65536, 131072, 262144, 524288};
        int nodes_count = sizeof(nodes_list) / sizeof(nodes_list[0]);
        int found_idx = -1;
        for (int i = 0; i < nodes_count; i++) {
            if (time_control_val == nodes_list[i]) {
                found_idx = i;
                break;
            }
        }
        if (found_idx == -1 || found_idx == nodes_count - 1) {
            time_control_val = nodes_list[0];
        } else {
            time_control_val = nodes_list[found_idx + 1];
        }
    }
    save_state();
}

void handle_input() {
    WPAD_ScanPads();
    u32 pressed = WPAD_ButtonsDown(0);

    if (pressed & WPAD_BUTTON_UP) {
        if (cursor_r > 0) cursor_r--;
    }
    if (pressed & WPAD_BUTTON_DOWN) {
        if (cursor_r < 7) cursor_r++;
    }
    if (pressed & WPAD_BUTTON_LEFT) {
        if (cursor_c > 0) cursor_c--;
    }
    if (pressed & WPAD_BUTTON_RIGHT) {
        if (cursor_c < 7) cursor_c++;
    }
    if (pressed & WPAD_BUTTON_A) {
        handle_select();
    }
    if (pressed & WPAD_BUTTON_B) {
        handle_undo();
    }
    if (pressed & WPAD_BUTTON_MINUS) {
        board_orientation = -board_orientation;
        save_state();
    }
    if (pressed & WPAD_BUTTON_PLUS) {
        handle_switch_sides();
    }
    if (pressed & WPAD_BUTTON_1) {
        time_control_type = (time_control_type + 1) % 3;
        time_control_val = (time_control_type == 0) ? 1000 : (time_control_type == 1 ? 5 : 4096);
        save_state();
    }
    if (pressed & WPAD_BUTTON_2) {
        adjust_time_control();
    }
    if (pressed & WPAD_BUTTON_HOME) {
        remove("sd:/apps/wiichess/state.bin");
        remove("sd:/apps/wiichess/position.uci");
        remove("sd:/apps/wiichess/move.txt");
        printf("\033[?25h\033[2J\033[H"); 
        SYS_ResetSystem(SYS_RETURNTOMENU, 0, 0);
    }
}

void init_board(BoardState *state) {
    int start[64] = {
        -4, -2, -3, -5, -6, -3, -2, -4,
        -1, -1, -1, -1, -1, -1, -1, -1,
         0,  0,  0,  0,  0,  0,  0,  0,
         0,  0,  0,  0,  0,  0,  0,  0,
         0,  0,  0,  0,  0,  0,  0,  0,
         0,  0,  0,  0,  0,  0,  0,  0,
         1,  1,  1,  1,  1,  1,  1,  1,
         4,  2,  3,  5,  6,  3,  2,  4
    };
    memcpy(state->board, start, sizeof(start));
    state->turn = 1;
    state->castle = 15;
    state->ep = -1;
    state->halfmoves = 0;
    state->fullmoves = 1;
}

int main(int argc, char **argv) {
    init_wii_console();
    
    if (!fatInitDefault()) {
        printf("FAT Init Failed! Check SD Card access.\n");
        VIDEO_WaitVSync();
        SYS_ResetSystem(SYS_RETURNTOMENU, 0, 0);
    }

    int has_previous_state = load_state();
    if (!has_previous_state) {
        init_board(&current_state);
        save_state();
    }

    // Capture calculated opponent moves from engine
    FILE *f_move = fopen("sd:/apps/wiichess/move.txt", "r");
    if (f_move) {
        char move_str[16];
        if (fscanf(f_move, "%15s", move_str) == 1) {
            Move m = uci_to_move(move_str);
            if (is_legal_move(&current_state, m)) {
                push_state(&current_state, m);
                BoardState next;
                make_move(&current_state, &next, m);
                current_state = next;
                save_state();
            }
        }
        fclose(f_move);
        remove("sd:/apps/wiichess/move.txt"); 
    }

    printf("\033[2J\033[H\033[?25l"); 

    while (1) {
        int engine_active = 0;
        if (has_legal_moves(&current_state) && current_state.halfmoves < 100 && count_repetitions(&current_state) < 3) {
            if (user_side == 2) engine_active = 1;
            else if (user_side == 1 && current_state.turn == -1) engine_active = 1;
            else if (user_side == -1 && current_state.turn == 1) engine_active = 1;
        }

        if (engine_active) {
            draw_ui();
            printf("\n  \033[1;32mLaunching Chess Engine...\033[0m\n");
            fflush(stdout);
            VIDEO_WaitVSync();
            trigger_engine_move(); 
        }

        draw_ui();
        handle_input();
        
        VIDEO_WaitVSync(); 
    }
    return 0;
}
