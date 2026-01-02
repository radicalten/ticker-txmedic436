// =============================================================================
// Nintendo GBA Stock Dashboard Demo
// =============================================================================
// Compile with devkitARM/devkitPro:
//   arm-none-eabi-gcc -mthumb -mthumb-interwork -specs=gba.specs -o dashboard.elf dashboard.c
//   arm-none-eabi-objcopy -O binary dashboard.elf dashboard.gba
//   gbafix dashboard.gba
// =============================================================================

// --- GBA Type Definitions ---
typedef unsigned char u8;
typedef unsigned short u16;
typedef unsigned int u32;
typedef signed char s8;
typedef signed short s16;
typedef signed int s32;

typedef volatile u8 vu8;
typedef volatile u16 vu16;
typedef volatile u32 vu32;

// --- GBA Hardware Registers ---
#define REG_DISPCNT     (*(vu16*)0x04000000)
#define REG_DISPSTAT    (*(vu16*)0x04000004)
#define REG_VCOUNT      (*(vu16*)0x04000006)
#define REG_KEYINPUT    (*(vu16*)0x04000130)
#define REG_IE          (*(vu16*)0x04000200)
#define REG_IF          (*(vu16*)0x04000202)
#define REG_IME         (*(vu16*)0x04000208)

// --- Display Control ---
#define MODE_3          0x0003
#define BG2_ENABLE      0x0400

// --- Screen Dimensions ---
#define SCREEN_WIDTH    240
#define SCREEN_HEIGHT   160

// --- Video Memory ---
#define VRAM            ((vu16*)0x06000000)

// --- Color Definitions (RGB555) ---
#define RGB15(r,g,b)    ((u16)((r) | ((g) << 5) | ((b) << 10)))
#define COLOR_BLACK     RGB15(0, 0, 0)
#define COLOR_WHITE     RGB15(31, 31, 31)
#define COLOR_RED       RGB15(31, 8, 8)
#define COLOR_GREEN     RGB15(8, 31, 8)
#define COLOR_YELLOW    RGB15(31, 31, 8)
#define COLOR_BLUE      RGB15(8, 8, 31)
#define COLOR_GRAY      RGB15(12, 12, 12)
#define COLOR_DARKGRAY  RGB15(6, 6, 6)
#define COLOR_CYAN      RGB15(8, 31, 31)

// --- Key Definitions ---
#define KEY_A           0x0001
#define KEY_B           0x0002
#define KEY_SELECT      0x0004
#define KEY_START       0x0008
#define KEY_RIGHT       0x0010
#define KEY_LEFT        0x0020
#define KEY_UP          0x0040
#define KEY_DOWN        0x0080
#define KEY_R           0x0100
#define KEY_L           0x0200

// --- Configuration ---
#define UPDATE_INTERVAL_FRAMES  (60 * 2)  // 2 seconds at 60fps
#define NUM_TICKERS     8
#define DATA_START_ROW  24
#define FONT_WIDTH      6
#define FONT_HEIGHT     8
#define MAX_VISIBLE     14  // Max tickers visible on screen

// --- MACD Parameters ---
#define FAST_EMA_PERIOD     12
#define SLOW_EMA_PERIOD     26
#define SIGNAL_EMA_PERIOD   9
#define MAX_SERIES_LEN      128

// --- Fixed Point Math (16.16) ---
typedef s32 Fixed;
#define FIX_SHIFT       16
#define FIX_ONE         (1 << FIX_SHIFT)
#define INT_TO_FIX(x)   ((Fixed)((x) << FIX_SHIFT))
#define FIX_TO_INT(x)   ((s32)((x) >> FIX_SHIFT))
#define FLOAT_TO_FIX(x) ((Fixed)((x) * FIX_ONE))
#define FIX_MUL(a,b)    ((Fixed)(((s64)(a) * (s64)(b)) >> FIX_SHIFT))
#define FIX_DIV(a,b)    ((Fixed)(((s64)(a) << FIX_SHIFT) / (b)))

// --- Data Structures ---
typedef struct {
    const char* symbol;
    Fixed base_price;       // Base price in fixed point (e.g., 100.00 = 100 * 65536)
    Fixed current_price;
    Fixed prev_close;
    Fixed volatility;       // Random walk volatility
    s32 trend;              // -1, 0, or 1 for bearish/neutral/bullish
} StockData;

typedef struct {
    Fixed data[MAX_SERIES_LEN];
    int n;
} Series;

// --- Global State ---
static StockData g_stocks[NUM_TICKERS];
static Series g_series[NUM_TICKERS];
static Fixed g_prev_price[NUM_TICKERS];
static u32 g_frame_count = 0;
static u32 g_update_counter = 0;
static u32 g_rand_seed = 12345;
static int g_scroll_offset = 0;

// --- 6x8 Bitmap Font Data (ASCII 32-127) ---
static const u8 font_data[96][8] = {
    {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00}, // 32 (space)
    {0x04,0x04,0x04,0x04,0x04,0x00,0x04,0x00}, // 33 !
    {0x0A,0x0A,0x0A,0x00,0x00,0x00,0x00,0x00}, // 34 "
    {0x0A,0x0A,0x1F,0x0A,0x1F,0x0A,0x0A,0x00}, // 35 #
    {0x04,0x0F,0x14,0x0E,0x05,0x1E,0x04,0x00}, // 36 $
    {0x18,0x19,0x02,0x04,0x08,0x13,0x03,0x00}, // 37 %
    {0x08,0x14,0x14,0x08,0x15,0x12,0x0D,0x00}, // 38 &
    {0x04,0x04,0x08,0x00,0x00,0x00,0x00,0x00}, // 39 '
    {0x02,0x04,0x08,0x08,0x08,0x04,0x02,0x00}, // 40 (
    {0x08,0x04,0x02,0x02,0x02,0x04,0x08,0x00}, // 41 )
    {0x00,0x04,0x15,0x0E,0x15,0x04,0x00,0x00}, // 42 *
    {0x00,0x04,0x04,0x1F,0x04,0x04,0x00,0x00}, // 43 +
    {0x00,0x00,0x00,0x00,0x00,0x04,0x04,0x08}, // 44 ,
    {0x00,0x00,0x00,0x1F,0x00,0x00,0x00,0x00}, // 45 -
    {0x00,0x00,0x00,0x00,0x00,0x00,0x04,0x00}, // 46 .
    {0x01,0x01,0x02,0x04,0x08,0x10,0x10,0x00}, // 47 /
    {0x0E,0x11,0x13,0x15,0x19,0x11,0x0E,0x00}, // 48 0
    {0x04,0x0C,0x04,0x04,0x04,0x04,0x0E,0x00}, // 49 1
    {0x0E,0x11,0x01,0x02,0x04,0x08,0x1F,0x00}, // 50 2
    {0x0E,0x11,0x01,0x06,0x01,0x11,0x0E,0x00}, // 51 3
    {0x02,0x06,0x0A,0x12,0x1F,0x02,0x02,0x00}, // 52 4
    {0x1F,0x10,0x1E,0x01,0x01,0x11,0x0E,0x00}, // 53 5
    {0x06,0x08,0x10,0x1E,0x11,0x11,0x0E,0x00}, // 54 6
    {0x1F,0x01,0x02,0x04,0x08,0x08,0x08,0x00}, // 55 7
    {0x0E,0x11,0x11,0x0E,0x11,0x11,0x0E,0x00}, // 56 8
    {0x0E,0x11,0x11,0x0F,0x01,0x02,0x0C,0x00}, // 57 9
    {0x00,0x00,0x04,0x00,0x00,0x04,0x00,0x00}, // 58 :
    {0x00,0x00,0x04,0x00,0x00,0x04,0x04,0x08}, // 59 ;
    {0x02,0x04,0x08,0x10,0x08,0x04,0x02,0x00}, // 60 <
    {0x00,0x00,0x1F,0x00,0x1F,0x00,0x00,0x00}, // 61 =
    {0x08,0x04,0x02,0x01,0x02,0x04,0x08,0x00}, // 62 >
    {0x0E,0x11,0x01,0x02,0x04,0x00,0x04,0x00}, // 63 ?
    {0x0E,0x11,0x17,0x15,0x17,0x10,0x0E,0x00}, // 64 @
    {0x0E,0x11,0x11,0x1F,0x11,0x11,0x11,0x00}, // 65 A
    {0x1E,0x11,0x11,0x1E,0x11,0x11,0x1E,0x00}, // 66 B
    {0x0E,0x11,0x10,0x10,0x10,0x11,0x0E,0x00}, // 67 C
    {0x1C,0x12,0x11,0x11,0x11,0x12,0x1C,0x00}, // 68 D
    {0x1F,0x10,0x10,0x1E,0x10,0x10,0x1F,0x00}, // 69 E
    {0x1F,0x10,0x10,0x1E,0x10,0x10,0x10,0x00}, // 70 F
    {0x0E,0x11,0x10,0x17,0x11,0x11,0x0F,0x00}, // 71 G
    {0x11,0x11,0x11,0x1F,0x11,0x11,0x11,0x00}, // 72 H
    {0x0E,0x04,0x04,0x04,0x04,0x04,0x0E,0x00}, // 73 I
    {0x07,0x02,0x02,0x02,0x02,0x12,0x0C,0x00}, // 74 J
    {0x11,0x12,0x14,0x18,0x14,0x12,0x11,0x00}, // 75 K
    {0x10,0x10,0x10,0x10,0x10,0x10,0x1F,0x00}, // 76 L
    {0x11,0x1B,0x15,0x15,0x11,0x11,0x11,0x00}, // 77 M
    {0x11,0x11,0x19,0x15,0x13,0x11,0x11,0x00}, // 78 N
    {0x0E,0x11,0x11,0x11,0x11,0x11,0x0E,0x00}, // 79 O
    {0x1E,0x11,0x11,0x1E,0x10,0x10,0x10,0x00}, // 80 P
    {0x0E,0x11,0x11,0x11,0x15,0x12,0x0D,0x00}, // 81 Q
    {0x1E,0x11,0x11,0x1E,0x14,0x12,0x11,0x00}, // 82 R
    {0x0E,0x11,0x10,0x0E,0x01,0x11,0x0E,0x00}, // 83 S
    {0x1F,0x04,0x04,0x04,0x04,0x04,0x04,0x00}, // 84 T
    {0x11,0x11,0x11,0x11,0x11,0x11,0x0E,0x00}, // 85 U
    {0x11,0x11,0x11,0x11,0x11,0x0A,0x04,0x00}, // 86 V
    {0x11,0x11,0x11,0x15,0x15,0x15,0x0A,0x00}, // 87 W
    {0x11,0x11,0x0A,0x04,0x0A,0x11,0x11,0x00}, // 88 X
    {0x11,0x11,0x0A,0x04,0x04,0x04,0x04,0x00}, // 89 Y
    {0x1F,0x01,0x02,0x04,0x08,0x10,0x1F,0x00}, // 90 Z
    {0x0E,0x08,0x08,0x08,0x08,0x08,0x0E,0x00}, // 91 [
    {0x10,0x10,0x08,0x04,0x02,0x01,0x01,0x00}, // 92 backslash
    {0x0E,0x02,0x02,0x02,0x02,0x02,0x0E,0x00}, // 93 ]
    {0x04,0x0A,0x11,0x00,0x00,0x00,0x00,0x00}, // 94 ^
    {0x00,0x00,0x00,0x00,0x00,0x00,0x1F,0x00}, // 95 _
    {0x08,0x04,0x02,0x00,0x00,0x00,0x00,0x00}, // 96 `
    {0x00,0x00,0x0E,0x01,0x0F,0x11,0x0F,0x00}, // 97 a
    {0x10,0x10,0x1E,0x11,0x11,0x11,0x1E,0x00}, // 98 b
    {0x00,0x00,0x0E,0x11,0x10,0x11,0x0E,0x00}, // 99 c
    {0x01,0x01,0x0F,0x11,0x11,0x11,0x0F,0x00}, // 100 d
    {0x00,0x00,0x0E,0x11,0x1F,0x10,0x0E,0x00}, // 101 e
    {0x06,0x09,0x08,0x1C,0x08,0x08,0x08,0x00}, // 102 f
    {0x00,0x00,0x0F,0x11,0x0F,0x01,0x0E,0x00}, // 103 g
    {0x10,0x10,0x16,0x19,0x11,0x11,0x11,0x00}, // 104 h
    {0x04,0x00,0x0C,0x04,0x04,0x04,0x0E,0x00}, // 105 i
    {0x02,0x00,0x06,0x02,0x02,0x12,0x0C,0x00}, // 106 j
    {0x10,0x10,0x12,0x14,0x18,0x14,0x12,0x00}, // 107 k
    {0x0C,0x04,0x04,0x04,0x04,0x04,0x0E,0x00}, // 108 l
    {0x00,0x00,0x1A,0x15,0x15,0x11,0x11,0x00}, // 109 m
    {0x00,0x00,0x16,0x19,0x11,0x11,0x11,0x00}, // 110 n
    {0x00,0x00,0x0E,0x11,0x11,0x11,0x0E,0x00}, // 111 o
    {0x00,0x00,0x1E,0x11,0x1E,0x10,0x10,0x00}, // 112 p
    {0x00,0x00,0x0F,0x11,0x0F,0x01,0x01,0x00}, // 113 q
    {0x00,0x00,0x16,0x19,0x10,0x10,0x10,0x00}, // 114 r
    {0x00,0x00,0x0E,0x10,0x0E,0x01,0x1E,0x00}, // 115 s
    {0x08,0x08,0x1C,0x08,0x08,0x09,0x06,0x00}, // 116 t
    {0x00,0x00,0x11,0x11,0x11,0x13,0x0D,0x00}, // 117 u
    {0x00,0x00,0x11,0x11,0x11,0x0A,0x04,0x00}, // 118 v
    {0x00,0x00,0x11,0x11,0x15,0x15,0x0A,0x00}, // 119 w
    {0x00,0x00,0x11,0x0A,0x04,0x0A,0x11,0x00}, // 120 x
    {0x00,0x00,0x11,0x11,0x0F,0x01,0x0E,0x00}, // 121 y
    {0x00,0x00,0x1F,0x02,0x04,0x08,0x1F,0x00}, // 122 z
    {0x02,0x04,0x04,0x08,0x04,0x04,0x02,0x00}, // 123 {
    {0x04,0x04,0x04,0x04,0x04,0x04,0x04,0x00}, // 124 |
    {0x08,0x04,0x04,0x02,0x04,0x04,0x08,0x00}, // 125 }
    {0x00,0x00,0x08,0x15,0x02,0x00,0x00,0x00}, // 126 ~
    {0x1F,0x1F,0x1F,0x1F,0x1F,0x1F,0x1F,0x00}, // 127 (block)
};

// --- Function Prototypes ---
void vsync(void);
void set_pixel(int x, int y, u16 color);
void fill_rect(int x, int y, int w, int h, u16 color);
void draw_char(int x, int y, char c, u16 color);
void draw_string(int x, int y, const char* str, u16 color);
void draw_string_bg(int x, int y, const char* str, u16 fg, u16 bg);
void clear_screen(u16 color);
u32 rand_lcg(void);
Fixed rand_fixed(void);
void init_stocks(void);
void update_stock_prices(void);
void compute_ema_series(const Fixed* data, int n, int period, Fixed* out);
int compute_macd_last_two(const Fixed* closes, int n, Fixed* macd_prev, Fixed* macd_last, Fixed* signal_prev, Fixed* signal_last);
void draw_dashboard(void);
void draw_header(void);
void draw_stock_row(int row, int stock_idx);
void draw_footer(void);
int fixed_to_str(Fixed val, char* buf, int buf_size, int decimals);
int fixed_to_str_signed(Fixed val, char* buf, int buf_size, int decimals);
u16 get_keys(void);
void handle_input(void);

// --- Utility Functions ---

void vsync(void) {
    while (REG_VCOUNT >= 160);
    while (REG_VCOUNT < 160);
}

void set_pixel(int x, int y, u16 color) {
    if (x >= 0 && x < SCREEN_WIDTH && y >= 0 && y < SCREEN_HEIGHT) {
        VRAM[y * SCREEN_WIDTH + x] = color;
    }
}

void fill_rect(int x, int y, int w, int h, u16 color) {
    for (int py = y; py < y + h && py < SCREEN_HEIGHT; py++) {
        if (py < 0) continue;
        for (int px = x; px < x + w && px < SCREEN_WIDTH; px++) {
            if (px < 0) continue;
            VRAM[py * SCREEN_WIDTH + px] = color;
        }
    }
}

void clear_screen(u16 color) {
    for (int i = 0; i < SCREEN_WIDTH * SCREEN_HEIGHT; i++) {
        VRAM[i] = color;
    }
}

void draw_char(int x, int y, char c, u16 color) {
    if (c < 32 || c > 127) c = '?';
    int idx = c - 32;
    const u8* glyph = font_data[idx];
    
    for (int row = 0; row < 8; row++) {
        u8 bits = glyph[row];
        for (int col = 0; col < 6; col++) {
            if (bits & (0x10 >> col)) {
                set_pixel(x + col, y + row, color);
            }
        }
    }
}

void draw_string(int x, int y, const char* str, u16 color) {
    while (*str) {
        draw_char(x, y, *str, color);
        x += FONT_WIDTH;
        str++;
    }
}

void draw_string_bg(int x, int y, const char* str, u16 fg, u16 bg) {
    int start_x = x;
    const char* s = str;
    int len = 0;
    while (*s++) len++;
    
    fill_rect(start_x, y, len * FONT_WIDTH, FONT_HEIGHT, bg);
    draw_string(x, y, str, fg);
}

// --- Random Number Generator ---

u32 rand_lcg(void) {
    g_rand_seed = g_rand_seed * 1103515245 + 12345;
    return (g_rand_seed >> 16) & 0x7FFF;
}

Fixed rand_fixed(void) {
    // Returns a random fixed-point value between -0.5 and 0.5
    s32 r = (s32)rand_lcg() - 16384;
    return (Fixed)(r << 2); // Scale appropriately
}

// --- Stock Data Functions ---

void init_stocks(void) {
    // Initialize demo stock data
    static const char* symbols[NUM_TICKERS] = {
        "BTC-USD", "ETH-USD", "^SPX", "NVDA",
        "AMD", "INTC", "GC=F", "CL=F"
    };
    static const s32 base_prices[NUM_TICKERS] = {
        67500, 3450, 5200, 875,
        165, 32, 2350, 78
    };
    static const s32 volatilities[NUM_TICKERS] = {
        500, 25, 10, 8,
        2, 1, 5, 1
    };
    
    for (int i = 0; i < NUM_TICKERS; i++) {
        g_stocks[i].symbol = symbols[i];
        g_stocks[i].base_price = INT_TO_FIX(base_prices[i]);
        g_stocks[i].current_price = g_stocks[i].base_price;
        g_stocks[i].prev_close = g_stocks[i].base_price;
        g_stocks[i].volatility = INT_TO_FIX(volatilities[i]);
        g_stocks[i].trend = 0;
        
        // Initialize series
        g_series[i].n = 0;
        g_series[i].data[0] = g_stocks[i].current_price;
        g_series[i].n = 1;
        
        g_prev_price[i] = g_stocks[i].current_price;
    }
}

void update_stock_prices(void) {
    for (int i = 0; i < NUM_TICKERS; i++) {
        // Random walk with mean reversion
        Fixed change = FIX_MUL(rand_fixed(), g_stocks[i].volatility);
        
        // Add slight trend
        if (rand_lcg() % 100 < 5) {
            g_stocks[i].trend = (rand_lcg() % 3) - 1;
        }
        change += INT_TO_FIX(g_stocks[i].trend) >> 4;
        
        // Mean reversion towards base price
        Fixed diff = g_stocks[i].base_price - g_stocks[i].current_price;
        change += diff >> 8;
        
        g_prev_price[i] = g_stocks[i].current_price;
        g_stocks[i].current_price += change;
        
        // Ensure price doesn't go negative
        if (g_stocks[i].current_price < INT_TO_FIX(1)) {
            g_stocks[i].current_price = INT_TO_FIX(1);
        }
        
        // Append to series for MACD calculation
        if (g_series[i].n < MAX_SERIES_LEN) {
            g_series[i].data[g_series[i].n++] = g_stocks[i].current_price;
        } else {
            // Shift data left and append
            for (int j = 0; j < MAX_SERIES_LEN - 1; j++) {
                g_series[i].data[j] = g_series[i].data[j + 1];
            }
            g_series[i].data[MAX_SERIES_LEN - 1] = g_stocks[i].current_price;
        }
    }
}

// --- MACD Calculation (Fixed Point) ---

void compute_ema_series(const Fixed* data, int n, int period, Fixed* out) {
    if (!data || !out || n <= 0 || period <= 0 || period > n) return;
    
    // k = 2 / (period + 1) - we use fixed point
    Fixed k = FIX_DIV(INT_TO_FIX(2), INT_TO_FIX(period + 1));
    
    // Seed EMA with SMA of first 'period'
    Fixed sum = 0;
    for (int i = 0; i < period; i++) {
        sum += data[i];
    }
    Fixed ema = sum / period;
    
    for (int i = 0; i < period - 1; i++) {
        out[i] = 0;
    }
    out[period - 1] = ema;
    
    for (int i = period; i < n; i++) {
        // ema = (data[i] - ema) * k + ema
        ema = FIX_MUL(data[i] - ema, k) + ema;
        out[i] = ema;
    }
}

int compute_macd_last_two(const Fixed* closes, int n,
                          Fixed* macd_prev, Fixed* macd_last,
                          Fixed* signal_prev, Fixed* signal_last) {
    if (!closes || n < (SLOW_EMA_PERIOD + SIGNAL_EMA_PERIOD + 1)) return 0;
    
    static Fixed ema_fast[MAX_SERIES_LEN];
    static Fixed ema_slow[MAX_SERIES_LEN];
    static Fixed macd_line[MAX_SERIES_LEN];
    static Fixed signal_line[MAX_SERIES_LEN];
    
    compute_ema_series(closes, n, FAST_EMA_PERIOD, ema_fast);
    compute_ema_series(closes, n, SLOW_EMA_PERIOD, ema_slow);
    
    int macd_start = SLOW_EMA_PERIOD - 1;
    int macd_count = n - macd_start;
    if (macd_count <= 0) return 0;
    
    for (int i = 0; i < macd_count; i++) {
        int idx = macd_start + i;
        macd_line[i] = ema_fast[idx] - ema_slow[idx];
    }
    
    if (macd_count < SIGNAL_EMA_PERIOD + 1) return 0;
    
    compute_ema_series(macd_line, macd_count, SIGNAL_EMA_PERIOD, signal_line);
    
    *macd_last = macd_line[macd_count - 1];
    *macd_prev = macd_line[macd_count - 2];
    *signal_last = signal_line[macd_count - 1];
    *signal_prev = signal_line[macd_count - 2];
    
    return 1;
}

// --- Number to String Conversion ---

int fixed_to_str(Fixed val, char* buf, int buf_size, int decimals) {
    if (buf_size < 16) return 0;
    
    int neg = 0;
    if (val < 0) {
        neg = 1;
        val = -val;
    }
    
    s32 integer_part = FIX_TO_INT(val);
    s32 frac_part = val & (FIX_ONE - 1);
    
    // Convert fractional part to decimal
    s32 frac_decimal = 0;
    for (int i = 0; i < decimals; i++) {
        frac_part *= 10;
        frac_decimal = frac_decimal * 10 + FIX_TO_INT(frac_part);
        frac_part &= (FIX_ONE - 1);
    }
    
    // Build string
    char* p = buf;
    if (neg) *p++ = '-';
    
    // Integer part
    char int_buf[12];
    int int_len = 0;
    s32 tmp = integer_part;
    if (tmp == 0) {
        int_buf[int_len++] = '0';
    } else {
        while (tmp > 0) {
            int_buf[int_len++] = '0' + (tmp % 10);
            tmp /= 10;
        }
    }
    for (int i = int_len - 1; i >= 0; i--) {
        *p++ = int_buf[i];
    }
    
    if (decimals > 0) {
        *p++ = '.';
        
        // Fractional part with leading zeros
        char frac_buf[8];
        for (int i = decimals - 1; i >= 0; i--) {
            frac_buf[i] = '0' + (frac_decimal % 10);
            frac_decimal /= 10;
        }
        for (int i = 0; i < decimals; i++) {
            *p++ = frac_buf[i];
        }
    }
    
    *p = '\0';
    return (int)(p - buf);
}

int fixed_to_str_signed(Fixed val, char* buf, int buf_size, int decimals) {
    if (buf_size < 16) return 0;
    
    if (val >= 0) {
        buf[0] = '+';
        fixed_to_str(val, buf + 1, buf_size - 1, decimals);
    } else {
        fixed_to_str(val, buf, buf_size, decimals);
    }
    return 1;
}

// --- Drawing Functions ---

void draw_header(void) {
    fill_rect(0, 0, SCREEN_WIDTH, 16, COLOR_DARKGRAY);
    draw_string(4, 0, "GBA STOCK DASHBOARD", COLOR_CYAN);
    
    char frame_buf[20];
    char* p = frame_buf;
    *p++ = 'F';
    *p++ = ':';
    
    u32 f = g_frame_count / 60;  // Seconds
    if (f >= 100) *p++ = '0' + (f / 100) % 10;
    if (f >= 10) *p++ = '0' + (f / 10) % 10;
    *p++ = '0' + f % 10;
    *p++ = 's';
    *p = '\0';
    
    draw_string(SCREEN_WIDTH - 48, 0, frame_buf, COLOR_WHITE);
    
    // Column headers
    draw_string(4, 10, "SYMBOL", COLOR_YELLOW);
    draw_string(60, 10, "PRICE", COLOR_YELLOW);
    draw_string(120, 10, "CHG%", COLOR_YELLOW);
    draw_string(168, 10, "MACD%", COLOR_YELLOW);
    draw_string(210, 10, "SIG", COLOR_YELLOW);
}

void draw_stock_row(int row, int stock_idx) {
    if (stock_idx < 0 || stock_idx >= NUM_TICKERS) return;
    
    int y = DATA_START_ROW + row * 10;
    if (y >= SCREEN_HEIGHT - 16) return;
    
    StockData* stock = &g_stocks[stock_idx];
    Series* series = &g_series[stock_idx];
    
    // Clear row
    fill_rect(0, y, SCREEN_WIDTH, 10, COLOR_BLACK);
    
    // Calculate MACD
    Fixed macd_prev = 0, macd_last = 0, signal_prev = 0, signal_last = 0;
    int has_macd = compute_macd_last_two(series->data, series->n,
                                          &macd_prev, &macd_last,
                                          &signal_prev, &signal_last);
    
    // Check for crossover
    int bullish_cross = has_macd && (macd_prev <= signal_prev) && (macd_last > signal_last);
    int bearish_cross = has_macd && (macd_prev >= signal_prev) && (macd_last < signal_last);
    
    // Symbol with crossover indicator
    u16 symbol_color = COLOR_WHITE;
    if (bullish_cross) symbol_color = COLOR_GREEN;
    if (bearish_cross) symbol_color = COLOR_RED;
    
    draw_string(4, y, stock->symbol, symbol_color);
    
    // Price with change indicator
    char price_buf[16];
    fixed_to_str(stock->current_price, price_buf, sizeof(price_buf), 2);
    
    u16 price_bg = COLOR_BLACK;
    if (stock->current_price > g_prev_price[stock_idx]) price_bg = RGB15(0, 12, 0);
    else if (stock->current_price < g_prev_price[stock_idx]) price_bg = RGB15(12, 0, 0);
    
    draw_string_bg(60, y, price_buf, COLOR_WHITE, price_bg);
    
    // Percentage change
    Fixed change = stock->current_price - stock->prev_close;
    Fixed pct_change = 0;
    if (stock->prev_close != 0) {
        pct_change = FIX_DIV(FIX_MUL(change, INT_TO_FIX(100)), stock->prev_close);
    }
    
    char pct_buf[16];
    fixed_to_str_signed(pct_change, pct_buf, sizeof(pct_buf), 2);
    
    u16 pct_color = (pct_change >= 0) ? COLOR_GREEN : COLOR_RED;
    draw_string(120, y, pct_buf, pct_color);
    
    // MACD %
    if (has_macd && stock->current_price != 0) {
        Fixed macd_pct = FIX_DIV(FIX_MUL(macd_last, INT_TO_FIX(100)), stock->current_price);
        char macd_buf[16];
        fixed_to_str_signed(macd_pct, macd_buf, sizeof(macd_buf), 2);
        
        u16 macd_color = (macd_pct >= 0) ? COLOR_GREEN : COLOR_RED;
        draw_string(168, y, macd_buf, macd_color);
        
        // Signal indicator
        const char* sig_str = (macd_last > signal_last) ? "BUY" : "SELL";
        u16 sig_color = (macd_last > signal_last) ? COLOR_GREEN : COLOR_RED;
        draw_string(210, y, sig_str, sig_color);
    } else {
        draw_string(168, y, "N/A", COLOR_GRAY);
        draw_string(210, y, "---", COLOR_GRAY);
    }
}

void draw_footer(void) {
    int y = SCREEN_HEIGHT - 10;
    fill_rect(0, y, SCREEN_WIDTH, 10, COLOR_DARKGRAY);
    
    // Countdown
    int secs_left = (UPDATE_INTERVAL_FRAMES - g_update_counter) / 60;
    if (secs_left < 0) secs_left = 0;
    
    char footer_buf[40];
    char* p = footer_buf;
    *p++ = 'U';
    *p++ = 'p';
    *p++ = 'd';
    *p++ = ':';
    *p++ = '0' + secs_left % 10;
    *p++ = 's';
    *p++ = ' ';
    *p++ = '|';
    *p++ = ' ';
    *p++ = 'U';
    *p++ = '/';
    *p++ = 'D';
    *p++ = ':';
    *p++ = 'S';
    *p++ = 'c';
    *p++ = 'r';
    *p++ = 'o';
    *p++ = 'l';
    *p++ = 'l';
    *p = '\0';
    
    draw_string(4, y, footer_buf, COLOR_WHITE);
    
    // Samples count
    char samp_buf[16];
    p = samp_buf;
    *p++ = 'N';
    *p++ = ':';
    int n = g_series[0].n;
    if (n >= 100) *p++ = '0' + (n / 100) % 10;
    if (n >= 10) *p++ = '0' + (n / 10) % 10;
    *p++ = '0' + n % 10;
    *p = '\0';
    
    draw_string(SCREEN_WIDTH - 36, y, samp_buf, COLOR_YELLOW);
}

void draw_dashboard(void) {
    draw_header();
    
    // Draw separator line
    fill_rect(0, 18, SCREEN_WIDTH, 1, COLOR_GRAY);
    
    // Draw visible stock rows
    int visible_count = (SCREEN_HEIGHT - DATA_START_ROW - 16) / 10;
    if (visible_count > NUM_TICKERS) visible_count = NUM_TICKERS;
    
    for (int i = 0; i < visible_count; i++) {
        int stock_idx = (g_scroll_offset + i) % NUM_TICKERS;
        draw_stock_row(i, stock_idx);
    }
    
    draw_footer();
}

// --- Input Handling ---

u16 get_keys(void) {
    return ~REG_KEYINPUT & 0x03FF;
}

void handle_input(void) {
    static u16 prev_keys = 0;
    u16 keys = get_keys();
    u16 pressed = keys & ~prev_keys;
    
    if (pressed & KEY_UP) {
        g_scroll_offset--;
        if (g_scroll_offset < 0) g_scroll_offset = NUM_TICKERS - 1;
    }
    if (pressed & KEY_DOWN) {
        g_scroll_offset++;
        if (g_scroll_offset >= NUM_TICKERS) g_scroll_offset = 0;
    }
    if (pressed & KEY_A) {
        // Force update
        g_update_counter = UPDATE_INTERVAL_FRAMES;
    }
    if (pressed & KEY_START) {
        // Reset series data
        for (int i = 0; i < NUM_TICKERS; i++) {
            g_series[i].n = 1;
            g_series[i].data[0] = g_stocks[i].current_price;
        }
    }
    
    prev_keys = keys;
}

// --- Main Entry Point ---

int main(void) {
    // Set up display: Mode 3, BG2 enabled
    REG_DISPCNT = MODE_3 | BG2_ENABLE;
    
    // Initialize
    clear_screen(COLOR_BLACK);
    init_stocks();
    
    // Main loop
    while (1) {
        vsync();
        g_frame_count++;
        g_update_counter++;
        
        handle_input();
        
        // Update prices periodically
        if (g_update_counter >= UPDATE_INTERVAL_FRAMES) {
            update_stock_prices();
            g_update_counter = 0;
        }
        
        // Redraw dashboard
        draw_dashboard();
    }
    
    return 0;
}
