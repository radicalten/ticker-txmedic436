// =============================================================
// GBA Stock Dashboard Demo using libtonc
// Note: GBA has no networking - uses simulated price data
// =============================================================

#include <tonc.h>
#include <string.h>

// --- Configuration ---
#define UPDATE_INTERVAL_FRAMES (60 * 5)  // 5 seconds at 60fps
#define MAX_TICKERS 8                     // Limited screen space
#define MAX_SERIES_LEN 64                 // For MACD calculation

// MACD parameters (same as original)
#define FAST_EMA_PERIOD 12
#define SLOW_EMA_PERIOD 26
#define SIGNAL_EMA_PERIOD 9

// Fixed point math (16.16 format)
#define FP_SHIFT 16
#define FP_ONE (1 << FP_SHIFT)
#define INT_TO_FP(x) ((s32)(x) << FP_SHIFT)
#define FP_TO_INT(x) ((x) >> FP_SHIFT)
#define FP_MUL(a, b) ((s32)(((s64)(a) * (b)) >> FP_SHIFT))
#define FP_DIV(a, b) ((s32)(((s64)(a) << FP_SHIFT) / (b)))
#define FP_FRAC(x) ((x) & ((1 << FP_SHIFT) - 1))

// --- Color Palette Indices ---
#define PAL_WHITE  1
#define PAL_GREEN  2
#define PAL_RED    3
#define PAL_YELLOW 4
#define PAL_CYAN   5

// --- Data Structures ---
typedef struct {
    s32 data[MAX_SERIES_LEN];  // Fixed point prices
    int n;
} Series;

typedef struct {
    const char* symbol;
    s32 price_fp;           // Current price (fixed point)
    s32 prev_price_fp;      // Previous update price
    s32 base_price_fp;      // Day open price (for daily change)
    s32 change_fp;          // Change from base
    s32 pct_change_fp;      // Percent change * 100
    s32 macd_pct_fp;        // MACD as % of price * 100
    s32 signal_pct_fp;      // Signal as % of price * 100
    int has_macd;           // Whether MACD is computed
    int bullish_cross;      // Bullish crossover detected
    int bearish_cross;      // Bearish crossover detected
} TickerData;

// --- Stock Tickers (limited for GBA screen) ---
static const char* ticker_symbols[MAX_TICKERS] = {
    "BTC", "ETH", "SPX", "GLD",
    "OIL", "NVDA", "AMD", "INTC"
};

// Base prices for simulation (in dollars, will be converted to FP)
static const u32 base_prices_usd[MAX_TICKERS] = {
    67000, 3400, 5900, 2350,
    75, 135, 165, 31
};

// --- Global State ---
static TickerData g_tickers[MAX_TICKERS];
static Series g_series[MAX_TICKERS];
static u32 g_frame_counter = 0;
static u32 g_update_count = 0;
static u32 g_rand_seed = 0x12345678;
static int g_selected_ticker = 0;  // For scrolling/detail view
static int g_view_mode = 0;        // 0 = list, 1 = detail

// --- Function Prototypes ---
void init_display(void);
void init_palette(void);
void init_tickers(void);
void update_prices(void);
void draw_dashboard(void);
void draw_list_view(void);
void draw_detail_view(void);
void draw_header(void);
void draw_ticker_row(int index, int screen_row);
void draw_progress_bar(int y, int current, int max);
void handle_input(void);

u32 lcg_rand(void);
s32 rand_range(s32 min_fp, s32 max_fp);

void series_append(Series* s, s32 value);
void compute_ema_fp(const s32* data, int n, int period, s32* out);
int compute_macd_for_ticker(int ticker_idx);

void tte_printf_at(int x, int y, const char* fmt, ...);
void set_text_color(int pal_idx);

// =============================================================
// MAIN
// =============================================================
int main(void) {
    // Set up display mode 0 with BG0
    REG_DISPCNT = DCNT_MODE0 | DCNT_BG0;
    
    // Initialize subsystems
    init_palette();
    init_display();
    init_tickers();
    
    // Set up interrupts for VBlank
    irq_init(NULL);
    irq_enable(II_VBLANK);
    
    // Seed random with initial timer value
    REG_TM0CNT_L = 0;
    REG_TM0CNT_H = TM_ENABLE;
    g_rand_seed = REG_TM0CNT_L ^ 0xDEADBEEF;
    
    // Main loop
    while (1) {
        VBlankIntrWait();
        
        key_poll();
        handle_input();
        
        g_frame_counter++;
        
        // Update at interval
        if (g_frame_counter >= UPDATE_INTERVAL_FRAMES) {
            g_frame_counter = 0;
            g_update_count++;
            update_prices();
        }
        
        draw_dashboard();
    }
    
    return 0;
}

// =============================================================
// INITIALIZATION
// =============================================================

void init_palette(void) {
    // Background palette
    pal_bg_mem[0] = RGB15(0, 0, 2);       // Dark blue background
    pal_bg_mem[PAL_WHITE]  = RGB15(31, 31, 31);  // White
    pal_bg_mem[PAL_GREEN]  = RGB15(0, 31, 0);    // Bright green
    pal_bg_mem[PAL_RED]    = RGB15(31, 0, 0);    // Bright red
    pal_bg_mem[PAL_YELLOW] = RGB15(31, 31, 0);   // Yellow
    pal_bg_mem[PAL_CYAN]   = RGB15(0, 31, 31);   // Cyan
    
    // Additional colors for backgrounds
    pal_bg_mem[16 + 0] = RGB15(0, 8, 0);   // Dark green bg
    pal_bg_mem[16 + 1] = RGB15(31, 31, 31);
    pal_bg_mem[32 + 0] = RGB15(8, 0, 0);   // Dark red bg
    pal_bg_mem[32 + 1] = RGB15(31, 31, 31);
}

void init_display(void) {
    // Initialize TTE (Tonc Text Engine) for console-style output
    tte_init_se_default(0, BG_CBB(0) | BG_SBB(31));
    
    // Set default text color
    tte_set_ink(PAL_WHITE);
}

void init_tickers(void) {
    for (int i = 0; i < MAX_TICKERS; i++) {
        s32 base = INT_TO_FP(base_prices_usd[i]);
        
        g_tickers[i].symbol = ticker_symbols[i];
        g_tickers[i].price_fp = base;
        g_tickers[i].prev_price_fp = base;
        g_tickers[i].base_price_fp = base;
        g_tickers[i].change_fp = 0;
        g_tickers[i].pct_change_fp = 0;
        g_tickers[i].macd_pct_fp = 0;
        g_tickers[i].signal_pct_fp = 0;
        g_tickers[i].has_macd = 0;
        g_tickers[i].bullish_cross = 0;
        g_tickers[i].bearish_cross = 0;
        
        // Initialize series
        g_series[i].n = 0;
        
        // Pre-populate series with some initial values for MACD
        for (int j = 0; j < 35; j++) {
            // Add slight random variation to seed
            s32 var = rand_range(-FP_DIV(base, INT_TO_FP(200)), 
                                  FP_DIV(base, INT_TO_FP(200)));
            series_append(&g_series[i], base + var);
        }
    }
}

// =============================================================
// RANDOM NUMBER GENERATION
// =============================================================

u32 lcg_rand(void) {
    g_rand_seed = g_rand_seed * 1103515245 + 12345;
    return g_rand_seed;
}

s32 rand_range(s32 min_fp, s32 max_fp) {
    u32 r = lcg_rand();
    s32 range = max_fp - min_fp;
    if (range <= 0) return min_fp;
    return min_fp + (s32)((u64)r * range / 0xFFFFFFFF);
}

// =============================================================
// PRICE SIMULATION
// =============================================================

void update_prices(void) {
    for (int i = 0; i < MAX_TICKERS; i++) {
        TickerData* t = &g_tickers[i];
        
        // Store previous price
        t->prev_price_fp = t->price_fp;
        
        // Random walk: +/- up to 0.5% per update
        s32 max_delta = FP_DIV(t->price_fp, INT_TO_FP(200));
        s32 delta = rand_range(-max_delta, max_delta);
        
        // Apply momentum bias occasionally
        if ((lcg_rand() & 0xFF) < 30) {
            // Trend continuation
            if (t->price_fp > t->prev_price_fp) {
                delta = (delta > 0) ? delta : -delta;
            } else {
                delta = (delta < 0) ? delta : -delta;
            }
        }
        
        t->price_fp += delta;
        
        // Clamp to positive
        if (t->price_fp < FP_ONE) {
            t->price_fp = FP_ONE;
        }
        
        // Calculate change from base (simulated "day open")
        t->change_fp = t->price_fp - t->base_price_fp;
        
        // Percent change (result is pct * 65536)
        if (t->base_price_fp != 0) {
            t->pct_change_fp = FP_DIV(FP_MUL(t->change_fp, INT_TO_FP(100)), 
                                       t->base_price_fp);
        }
        
        // Append to series
        series_append(&g_series[i], t->price_fp);
        
        // Compute MACD
        compute_macd_for_ticker(i);
    }
}

// =============================================================
// SERIES & MACD CALCULATIONS
// =============================================================

void series_append(Series* s, s32 value) {
    if (s->n < MAX_SERIES_LEN) {
        s->data[s->n++] = value;
    } else {
        // Shift left and append
        for (int i = 0; i < MAX_SERIES_LEN - 1; i++) {
            s->data[i] = s->data[i + 1];
        }
        s->data[MAX_SERIES_LEN - 1] = value;
    }
}

void compute_ema_fp(const s32* data, int n, int period, s32* out) {
    if (n < period) return;
    
    // k = 2 / (period + 1) in fixed point
    s32 k = FP_DIV(INT_TO_FP(2), INT_TO_FP(period + 1));
    
    // Seed with SMA of first 'period' values
    s64 sum = 0;
    for (int i = 0; i < period; i++) {
        sum += data[i];
    }
    s32 ema = (s32)(sum / period);
    
    // Fill initial values
    for (int i = 0; i < period - 1; i++) {
        out[i] = 0;
    }
    out[period - 1] = ema;
    
    // Compute EMA for rest
    for (int i = period; i < n; i++) {
        s32 diff = data[i] - ema;
        ema = ema + FP_MUL(diff, k);
        out[i] = ema;
    }
}

int compute_macd_for_ticker(int ticker_idx) {
    static s32 ema_fast[MAX_SERIES_LEN];
    static s32 ema_slow[MAX_SERIES_LEN];
    static s32 macd_line[MAX_SERIES_LEN];
    static s32 signal_line[MAX_SERIES_LEN];
    
    Series* s = &g_series[ticker_idx];
    TickerData* t = &g_tickers[ticker_idx];
    
    int n = s->n;
    
    // Need minimum data points
    if (n < SLOW_EMA_PERIOD + SIGNAL_EMA_PERIOD) {
        t->has_macd = 0;
        return 0;
    }
    
    // Compute EMAs
    compute_ema_fp(s->data, n, FAST_EMA_PERIOD, ema_fast);
    compute_ema_fp(s->data, n, SLOW_EMA_PERIOD, ema_slow);
    
    // Compute MACD line
    int macd_start = SLOW_EMA_PERIOD - 1;
    int macd_count = n - macd_start;
    
    for (int i = 0; i < macd_count; i++) {
        int idx = macd_start + i;
        macd_line[i] = ema_fast[idx] - ema_slow[idx];
    }
    
    if (macd_count < SIGNAL_EMA_PERIOD) {
        t->has_macd = 0;
        return 0;
    }
    
    // Compute signal line
    compute_ema_fp(macd_line, macd_count, SIGNAL_EMA_PERIOD, signal_line);
    
    // Get last two values for crossover detection
    s32 macd_last = macd_line[macd_count - 1];
    s32 macd_prev = macd_line[macd_count - 2];
    s32 signal_last = signal_line[macd_count - 1];
    s32 signal_prev = signal_line[macd_count - 2];
    
    // Convert to percentage of price
    s32 last_price = s->data[n - 1];
    if (last_price > 0) {
        t->macd_pct_fp = FP_DIV(FP_MUL(macd_last, INT_TO_FP(100)), last_price);
        t->signal_pct_fp = FP_DIV(FP_MUL(signal_last, INT_TO_FP(100)), last_price);
    }
    
    // Detect crossovers
    t->bullish_cross = (macd_prev <= signal_prev) && (macd_last > signal_last);
    t->bearish_cross = (macd_prev >= signal_prev) && (macd_last < signal_last);
    
    t->has_macd = 1;
    return 1;
}

// =============================================================
// INPUT HANDLING
// =============================================================

void handle_input(void) {
    // Toggle view with A button
    if (key_hit(KEY_A)) {
        g_view_mode = !g_view_mode;
    }
    
    // Navigate in list view
    if (g_view_mode == 0) {
        if (key_hit(KEY_UP) && g_selected_ticker > 0) {
            g_selected_ticker--;
        }
        if (key_hit(KEY_DOWN) && g_selected_ticker < MAX_TICKERS - 1) {
            g_selected_ticker++;
        }
    }
    
    // Reset simulation with START
    if (key_hit(KEY_START)) {
        init_tickers();
        g_update_count = 0;
        g_frame_counter = 0;
    }
    
    // Force update with SELECT
    if (key_hit(KEY_SELECT)) {
        g_frame_counter = UPDATE_INTERVAL_FRAMES;
    }
}

// =============================================================
// RENDERING
// =============================================================

void draw_dashboard(void) {
    tte_erase_screen();
    
    if (g_view_mode == 0) {
        draw_list_view();
    } else {
        draw_detail_view();
    }
}

void draw_list_view(void) {
    // Header
    tte_set_ink(PAL_CYAN);
    tte_set_pos(0, 0);
    tte_write("== GBA STOCKS ==");
    
    // Column headers
    tte_set_ink(PAL_YELLOW);
    tte_set_pos(0, 12);
    tte_write("TKR  PRICE    CHG%");
    
    // Draw tickers
    for (int i = 0; i < MAX_TICKERS; i++) {
        draw_ticker_row(i, i + 2);
    }
    
    // Progress bar for next update
    int y = 140;
    tte_set_ink(PAL_WHITE);
    tte_set_pos(0, y);
    tte_printf("Next: %ds", (UPDATE_INTERVAL_FRAMES - g_frame_counter) / 60);
    draw_progress_bar(y + 8, g_frame_counter, UPDATE_INTERVAL_FRAMES);
    
    // Instructions
    tte_set_ink(PAL_YELLOW);
    tte_set_pos(0, 152);
    tte_write("A:Detail START:Reset");
}

void draw_ticker_row(int index, int screen_row) {
    TickerData* t = &g_tickers[index];
    
    int y = screen_row * 12;
    int x = 0;
    
    // Highlight selected
    if (index == g_selected_ticker) {
        tte_set_ink(PAL_CYAN);
        tte_set_pos(x, y);
        tte_write(">");
    }
    x = 8;
    
    // Ticker symbol - colored by crossover
    if (t->bullish_cross) {
        tte_set_ink(PAL_GREEN);
    } else if (t->bearish_cross) {
        tte_set_ink(PAL_RED);
    } else {
        tte_set_ink(PAL_WHITE);
    }
    tte_set_pos(x, y);
    tte_printf("%-4s", t->symbol);
    
    // Price
    x = 40;
    // Color based on tick direction
    if (t->price_fp > t->prev_price_fp) {
        tte_set_ink(PAL_GREEN);
    } else if (t->price_fp < t->prev_price_fp) {
        tte_set_ink(PAL_RED);
    } else {
        tte_set_ink(PAL_WHITE);
    }
    
    s32 price_int = FP_TO_INT(t->price_fp);
    s32 price_frac = (FP_FRAC(t->price_fp) * 100) >> FP_SHIFT;
    
    tte_set_pos(x, y);
    if (price_int >= 10000) {
        tte_printf("%dK", price_int / 1000);
    } else if (price_int >= 1000) {
        tte_printf("%d", price_int);
    } else {
        tte_printf("%d.%02d", price_int, (int)price_frac);
    }
    
    // Percent change
    x = 104;
    s32 pct_int = FP_TO_INT(t->pct_change_fp);
    s32 pct_frac = FP_FRAC(t->pct_change_fp);
    if (pct_frac < 0) pct_frac = -pct_frac;
    pct_frac = (pct_frac * 100) >> FP_SHIFT;
    
    if (t->change_fp >= 0) {
        tte_set_ink(PAL_GREEN);
        tte_set_pos(x, y);
        tte_printf("+%d.%02d", (int)pct_int, (int)pct_frac);
    } else {
        tte_set_ink(PAL_RED);
        tte_set_pos(x, y);
        tte_printf("%d.%02d", (int)pct_int, (int)pct_frac);
    }
}

void draw_detail_view(void) {
    TickerData* t = &g_tickers[g_selected_ticker];
    
    // Header
    tte_set_ink(PAL_CYAN);
    tte_set_pos(0, 0);
    tte_printf("== %s DETAIL ==", t->symbol);
    
    int y = 20;
    
    // Price
    tte_set_ink(PAL_WHITE);
    tte_set_pos(0, y);
    tte_write("Price:");
    
    s32 price_int = FP_TO_INT(t->price_fp);
    s32 price_frac = (FP_FRAC(t->price_fp) * 100) >> FP_SHIFT;
    
    if (t->price_fp > t->prev_price_fp) {
        tte_set_ink(PAL_GREEN);
    } else if (t->price_fp < t->prev_price_fp) {
        tte_set_ink(PAL_RED);
    }
    tte_set_pos(60, y);
    tte_printf("$%d.%02d", (int)price_int, (int)price_frac);
    
    y += 16;
    
    // Change
    tte_set_ink(PAL_WHITE);
    tte_set_pos(0, y);
    tte_write("Change:");
    
    s32 chg_int = FP_TO_INT(t->change_fp);
    s32 chg_frac = FP_FRAC(t->change_fp);
    if (chg_frac < 0) chg_frac = -chg_frac;
    chg_frac = (chg_frac * 100) >> FP_SHIFT;
    
    if (t->change_fp >= 0) {
        tte_set_ink(PAL_GREEN);
        tte_set_pos(60, y);
        tte_printf("+$%d.%02d", (int)chg_int, (int)chg_frac);
    } else {
        tte_set_ink(PAL_RED);
        tte_set_pos(60, y);
        tte_printf("-$%d.%02d", (int)(-chg_int), (int)chg_frac);
    }
    
    y += 16;
    
    // Percent
    tte_set_ink(PAL_WHITE);
    tte_set_pos(0, y);
    tte_write("Pct:");
    
    s32 pct_int = FP_TO_INT(t->pct_change_fp);
    s32 pct_frac = FP_FRAC(t->pct_change_fp);
    if (pct_frac < 0) pct_frac = -pct_frac;
    pct_frac = (pct_frac * 100) >> FP_SHIFT;
    
    if (t->change_fp >= 0) {
        tte_set_ink(PAL_GREEN);
        tte_set_pos(60, y);
        tte_printf("+%d.%02d%%", (int)pct_int, (int)pct_frac);
    } else {
        tte_set_ink(PAL_RED);
        tte_set_pos(60, y);
        tte_printf("%d.%02d%%", (int)pct_int, (int)pct_frac);
    }
    
    y += 24;
    
    // MACD section
    tte_set_ink(PAL_YELLOW);
    tte_set_pos(0, y);
    tte_write("-- MACD --");
    y += 12;
    
    if (t->has_macd) {
        // MACD value
        tte_set_ink(PAL_WHITE);
        tte_set_pos(0, y);
        tte_write("MACD:");
        
        s32 macd_int = FP_TO_INT(t->macd_pct_fp);
        s32 macd_frac = FP_FRAC(t->macd_pct_fp);
        if (macd_frac < 0) macd_frac = -macd_frac;
        macd_frac = (macd_frac * 1000) >> FP_SHIFT;
        
        tte_set_ink(t->macd_pct_fp >= 0 ? PAL_GREEN : PAL_RED);
        tte_set_pos(60, y);
        tte_printf("%d.%03d%%", (int)macd_int, (int)macd_frac);
        
        y += 12;
        
        // Signal value
        tte_set_ink(PAL_WHITE);
        tte_set_pos(0, y);
        tte_write("Signal:");
        
        s32 sig_int = FP_TO_INT(t->signal_pct_fp);
        s32 sig_frac = FP_FRAC(t->signal_pct_fp);
        if (sig_frac < 0) sig_frac = -sig_frac;
        sig_frac = (sig_frac * 1000) >> FP_SHIFT;
        
        tte_set_ink(t->signal_pct_fp >= 0 ? PAL_GREEN : PAL_RED);
        tte_set_pos(60, y);
        tte_printf("%d.%03d%%", (int)sig_int, (int)sig_frac);
        
        y += 16;
        
        // Crossover status
        if (t->bullish_cross) {
            tte_set_ink(PAL_GREEN);
            tte_set_pos(0, y);
            tte_write("** BULLISH CROSS **");
        } else if (t->bearish_cross) {
            tte_set_ink(PAL_RED);
            tte_set_pos(0, y);
            tte_write("** BEARISH CROSS **");
        }
    } else {
        tte_set_ink(PAL_YELLOW);
        tte_set_pos(0, y);
        tte_printf("Need %d samples", SLOW_EMA_PERIOD + SIGNAL_EMA_PERIOD);
        y += 12;
        tte_set_pos(0, y);
        tte_printf("Have: %d", g_series[g_selected_ticker].n);
    }
    
    // Footer
    tte_set_ink(PAL_YELLOW);
    tte_set_pos(0, 140);
    tte_printf("Polls: %d", g_update_count);
    
    tte_set_pos(0, 152);
    tte_write("A:Back B:Prev SELECT:Upd");
}

void draw_progress_bar(int y, int current, int max) {
    int bar_width = 20;  // characters
    int filled = (current * bar_width) / max;
    
    tte_set_pos(64, y);
    tte_set_ink(PAL_WHITE);
    tte_write("[");
    
    for (int i = 0; i < bar_width; i++) {
        if (i < filled) {
            tte_set_ink(PAL_GREEN);
            tte_write("=");
        } else {
            tte_set_ink(PAL_WHITE);
            tte_write("-");
        }
    }
    
    tte_set_ink(PAL_WHITE);
    tte_write("]");
}
