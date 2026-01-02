// -----------------------------------------------------------------------------
// GBA Stock Dashboard (offline demo, libtonc)
//
// - No HTTP or JSON: prices are generated locally as a random walk
// - Uses libtonc + TTE for text output
// - Keeps MACD/EMA logic and multi-ticker layout
// -----------------------------------------------------------------------------

#include <tonc.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <math.h>

// --- Configuration -----------------------------------------------------------

#define UPDATE_INTERVAL_SECONDS  1      // shorter for GBA demo
#define DATA_START_ROW           6      // text row where first ticker starts

// MACD parameters
#define FAST_EMA_PERIOD   12
#define SLOW_EMA_PERIOD   26
#define SIGNAL_EMA_PERIOD  9

// --- Tickers -----------------------------------------------------------------

const char *tickers[] = {
    "BTC-USD", "ETH-USD", "DX-Y.NYB", "^SPX",
    "GC=F", "CL=F", "NG=F", "NVDA", "INTC",
    "AMD", "MU", "OKLO", "RKLB", "IREN", "CRML", "ABAT"
};
const int num_tickers = sizeof(tickers) / sizeof(tickers[0]);

// --- Per-ticker session series ----------------------------------------------

typedef struct {
    double *data;
    int     n;
    int     cap;
} Series;

static Series *g_series = NULL;

// --- Utility: ensure capacity / append to Series ----------------------------

static int ensure_series_capacity(Series *s, int min_cap)
{
    if (!s) return 0;
    if (s->cap >= min_cap) return 1;

    int new_cap = (s->cap > 0) ? s->cap * 2 : 64;
    if (new_cap < min_cap) new_cap = min_cap;

    double *p = (double *)realloc(s->data, sizeof(double) * new_cap);
    if (!p) return 0;

    s->data = p;
    s->cap  = new_cap;
    return 1;
}

static int series_append(Series *s, double v)
{
    if (!s) return 0;
    if (!ensure_series_capacity(s, s->n + 1)) return 0;
    s->data[s->n++] = v;
    return 1;
}

// --- EMA / MACD helpers (unchanged logic) -----------------------------------

static void compute_ema_series(const double *data, int n, int period, double *out)
{
    if (!data || !out || n <= 0 || period <= 0 || period > n) return;

    double k = 2.0 / (period + 1.0);

    // Seed EMA with SMA of first 'period'
    double sum = 0.0;
    for (int i = 0; i < period; i++)
        sum += data[i];

    double ema = sum / period;

    for (int i = 0; i < period - 1; i++)
        out[i] = 0.0;     // not used

    out[period - 1] = ema;

    for (int i = period; i < n; i++)
    {
        ema = (data[i] - ema) * k + ema;
        out[i] = ema;
    }
}

/**
 * Compute last two MACD and Signal values.
 * Returns 1 on success, 0 if not enough data.
 */
static int compute_macd_last_two(
    const double *closes, int n,
    double *macd_prev,  double *macd_last,
    double *sig_prev,   double *sig_last)
{
    if (!closes || n < (SLOW_EMA_PERIOD + SIGNAL_EMA_PERIOD + 1))
        return 0;

    double *ema_fast = (double *)malloc(sizeof(double) * n);
    double *ema_slow = (double *)malloc(sizeof(double) * n);
    if (!ema_fast || !ema_slow)
    {
        free(ema_fast);
        free(ema_slow);
        return 0;
    }

    compute_ema_series(closes, n, FAST_EMA_PERIOD, ema_fast);
    compute_ema_series(closes, n, SLOW_EMA_PERIOD, ema_slow);

    int macd_start = SLOW_EMA_PERIOD - 1;
    int macd_count = n - macd_start;
    if (macd_count <= 0)
    {
        free(ema_fast);
        free(ema_slow);
        return 0;
    }

    double *macd_line = (double *)malloc(sizeof(double) * macd_count);
    if (!macd_line)
    {
        free(ema_fast);
        free(ema_slow);
        return 0;
    }

    for (int i = 0; i < macd_count; i++)
    {
        int idx = macd_start + i;
        macd_line[i] = ema_fast[idx] - ema_slow[idx];
    }

    if (macd_count < SIGNAL_EMA_PERIOD + 1)
    {
        free(ema_fast);
        free(ema_slow);
        free(macd_line);
        return 0;
    }

    double *sig_line = (double *)malloc(sizeof(double) * macd_count);
    if (!sig_line)
    {
        free(ema_fast);
        free(ema_slow);
        free(macd_line);
        return 0;
    }

    compute_ema_series(macd_line, macd_count, SIGNAL_EMA_PERIOD, sig_line);

    *macd_last = macd_line[macd_count - 1];
    *macd_prev = macd_line[macd_count - 2];
    *sig_last  = sig_line[macd_count - 1];
    *sig_prev  = sig_line[macd_count - 2];

    free(ema_fast);
    free(ema_slow);
    free(macd_line);
    free(sig_line);
    return 1;
}

// --- Simple TTE-based print helper ------------------------------------------
// col/row are in character cells (8x8), not pixels.

static void print_at(int col, int row, const char *fmt, ...)
{
    char text[96];
    va_list ap;

    va_start(ap, fmt);
    vsnprintf(text, sizeof(text), fmt, ap);
    va_end(ap);

    // TTE control: #{P:x,y} sets pixel position.
    char buf[128];
    int x = col * 8;
    int y = row * 8;
    snprintf(buf, sizeof(buf), "#{P:%d,%d}%s", x, y, text);

    tte_write(buf);
}

// --- UI setup and per-ticker update -----------------------------------------

static int g_update_counter = 0;

// Draw static header and initialize data structures
static void setup_dashboard_ui(void)
{
    // Allocate session series
    g_series = (Series *)calloc(num_tickers, sizeof(Series));

    // Title
    tte_write("#{P:0,0}GBA Stock Dashboard (offline demo)");

    // Timestamp line placeholder (row 2)
    print_at(0, 2, "Update #0");

    // Header
    print_at(0, 4, "%-10s | %10s | %10s | %7s | %6s | %6s",
             "Tkr", "Price", "Chg", "%Chg", "MACD", "Sig");

    // Initial placeholders for each ticker
    for (int i = 0; i < num_tickers; i++)
    {
        int row = DATA_START_ROW + i;
        print_at(0, row, "%-10s | Initializing...", tickers[i]);
    }
}

// Update "timestamp" (here: just an update counter)
static void update_timestamp(void)
{
    print_at(0, 2, "Update #%d", g_update_counter);
}

// Generate / update price series and print one row
static void update_ticker(int index, int row)
{
    Series *s = &g_series[index];

    // Seed RNG once
    static int rng_inited = 0;
    if (!rng_inited)
    {
        srand(1);   // deterministic
        rng_inited = 1;
    }

    double price;

    if (s->n == 0)
    {
        // Initial price per ticker
        price = 100.0 + index * 10.0;
        series_append(s, price);
    }
    else
    {
        double last = s->data[s->n - 1];

        // Random walk: +/- ~1% step
        int r = rand() % 2001 - 1000;      // [-1000,1000]
        double pct = (double)r / 100000.0; // [-0.01,0.01]
        price = last * (1.0 + pct);

        series_append(s, price);
    }

    // "Daily" reference: first sample
    double base_prev = (s->n >= 2) ? s->data[0] : price;
    double change    = price - base_prev;
    double pct_change = (base_prev != 0.0)
                        ? (change / base_prev) * 100.0
                        : 0.0;

    // MACD from session series
    double macd_prev = 0.0, macd_last = 0.0;
    double sig_prev  = 0.0, sig_last  = 0.0;
    int has_macd = compute_macd_last_two(
        s->data, s->n,
        &macd_prev, &macd_last,
        &sig_prev, &sig_last
    );

    double macd_pct = 0.0, sig_pct = 0.0;
    if (has_macd && price != 0.0)
    {
        macd_pct = (macd_last / price) * 100.0;
        sig_pct  = (sig_last  / price) * 100.0;
    }

    // Crossover flags
    int bullish_cross = 0, bearish_cross = 0;
    if (has_macd)
    {
        bullish_cross = (macd_prev <= sig_prev) && (macd_last > sig_last);
        bearish_cross = (macd_prev >= sig_prev) && (macd_last < sig_last);
    }

    char macd_buf[16], sig_buf[16];
    if (has_macd)
    {
        snprintf(macd_buf, sizeof(macd_buf), "%+6.3f", macd_pct);
        snprintf(sig_buf,  sizeof(sig_buf),  "%+6.3f", sig_pct);
    }
    else
    {
        snprintf(macd_buf, sizeof(macd_buf), "%6s", "N/A");
        snprintf(sig_buf,  sizeof(sig_buf),  "%6s", "N/A");
    }

    char cross_char = ' ';
    if (bullish_cross) cross_char = '^';
    else if (bearish_cross) cross_char = 'v';

    // Print row (fixed-width style so it overwrites previous text)
    print_at(0, row,
             "%-10s | %10.2f | %+10.2f | %+6.2f%% | %6s | %6s %c",
             tickers[index], price, change, pct_change,
             macd_buf, sig_buf, cross_char);
}

// Simple countdown using VBlank (≈60 frames/sec)
static void run_countdown(void)
{
    int update_line = DATA_START_ROW + num_tickers + 1;

    for (int s = UPDATE_INTERVAL_SECONDS; s > 0; s--)
    {
        print_at(0, update_line, "Updating in %2d seconds...", s);

        // Wait roughly 1 second (60 VBlanks)
        for (int i = 0; i < 60; i++)
            vid_vsync();
    }

    print_at(0, update_line, "Updating now...           ");

    // Small pause so "Updating now..." is visible
    for (int i = 0; i < 30; i++)
        vid_vsync();
}

// --- Main --------------------------------------------------------------------

int main(void)
{
    // Set video mode 0, enable BG0
    REG_DISPCNT = DCNT_MODE0 | DCNT_BG0;

    // Init TTE on BG0, using charblock 0, screenblock 31
    tte_init_se_default(0, BG_CBB(0) | BG_SBB(31));

    // Optional: set margins if you like (not strictly needed)
    // tte_set_margins(0, 0, 240, 160);

    setup_dashboard_ui();

    while (1)
    {
        update_timestamp();

        // Update all tickers
        for (int i = 0; i < num_tickers; i++)
        {
            int row = DATA_START_ROW + i;
            update_ticker(i, row);
        }

        g_update_counter++;
        run_countdown();
    }

    // Never reached on GBA
    return 0;
}
