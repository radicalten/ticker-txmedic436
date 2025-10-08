/*
  3DS Stock Dashboard (Yahoo Finance | 1d only | MACD from live session polls)
  - Uses 3DS httpc instead of libcurl
  - Prints to top-screen console
  - Press START to exit
  - SSL verification disabled (Yahoo uses modern certs; 3DS doesn't ship CA bundle)
*/

#include <3ds.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <malloc.h>
#include <time.h>
#include <math.h>

#include "cJSON.h"

// --- Configuration ---
#define UPDATE_INTERVAL_SECONDS 30
#define USER_AGENT "Mozilla/5.0 (Nintendo 3DS) AppleWebKit/537.36 (KHTML, like Gecko)"
#define API_URL_1D_FORMAT "https://query1.finance.yahoo.com/v8/finance/chart/%s?range=1d&interval=4h&includePrePost=true"
#define DATA_START_ROW 6 // The row number where the first stock ticker will be printed

// MACD parameters (session-based, in "polls" units)
#define FAST_EMA_PERIOD 12
#define SLOW_EMA_PERIOD 26
#define SIGNAL_EMA_PERIOD 9

// tickers
static const char* tickers[] = {
    "BTC-USD", "ETH-USD", "DX-Y.NYB", "^SPX", "^IXIC",
    "GC=F", "CL=F", "NG=F", "NVDA", "INTC",
    "AMD", "MU", "PFE", "UNH", "TGT", "TRAK"
};
static const int num_tickers = sizeof(tickers) / sizeof(tickers[0]);

// --- Per-ticker previous price (for change arrows) ---
static double* g_prev_price = NULL;

// --- Per-ticker session series (live polled values) ---
typedef struct {
    double *data;
    int n;
    int cap;
} Series;
static Series* g_series = NULL;

// --- Network (SOC) buffer ---
#define SOC_ALIGN      0x1000
#define SOC_BUFFERSIZE 0x100000
static u32* g_socbuf = NULL;

// --- Helpers/macros for 3DS console ---
#define GOTO(row, col) printf("\x1b[%d;%dH", (int)(row), (int)(col))
#define CLEAR()        printf("\x1b[2J")
#define CLEARLINE()    printf("\x1b[K")

// --- Function Prototypes ---
static char* fetch_url(const char* url);
static void parse_and_print_stock_data(const char *json_1d, int row);
static void setup_dashboard_ui(void);
static void update_timestamp(void);
static void run_countdown_or_exit(void);
static void print_error_on_line(const char* ticker, const char* error_msg, int row);
static void cleanup_on_exit(void);

static int ensure_series_capacity(Series* s, int min_cap);
static int series_append(Series* s, double v);

static int extract_daily_closes(cJSON *result, double **out_closes, int *out_n);
static void compute_ema_series(const double *data, int n, int period, double *out);
static int compute_macd_last_two(const double *closes, int n,
                                 double *macd_prev, double *macd_last,
                                 double *signal_prev, double *signal_last);

// --- 3DS HTTP fetch (HTTPS with verify disabled) ---
static char* fetch_url(const char* url) {
    Result rc;
    httpcContext ctx;

    rc = httpcOpenContext(&ctx, HTTPC_METHOD_GET, url, 1);
    if (R_FAILED(rc)) return NULL;

    httpcSetSSLOpt(&ctx, SSLCOPT_DisableVerify);
    httpcSetKeepAlive(&ctx, HTTPC_KEEPALIVE_ENABLED);
    httpcAddRequestHeaderField(&ctx, "User-Agent", USER_AGENT);

    rc = httpcBeginRequest(&ctx);
    if (R_FAILED(rc)) { httpcCloseContext(&ctx); return NULL; }

    u32 status = 0;
    rc = httpcGetResponseStatusCode(&ctx, &status, 0);
    if (R_FAILED(rc) || status != 200) { httpcCloseContext(&ctx); return NULL; }

    // Read response
    size_t cap = 64 * 1024;
    char* mem = (char*)malloc(cap);
    if (!mem) { httpcCloseContext(&ctx); return NULL; }

    size_t off = 0;
    Result dlrc;
    do {
        if (off + 0x2000 + 1 > cap) {
            size_t ncap = cap * 2;
            char* p = (char*)realloc(mem, ncap);
            if (!p) { free(mem); httpcCloseContext(&ctx); return NULL; }
            mem = p;
            cap = ncap;
        }
        u32 readsz = 0;
        dlrc = httpcDownloadData(&ctx, (u8*)mem + off, (u32)(cap - off - 1), &readsz);
        off += readsz;
    } while (dlrc == (Result)HTTPC_RESULTCODE_DOWNLOADPENDING);

    if (R_FAILED(dlrc)) { free(mem); httpcCloseContext(&ctx); return NULL; }

    mem[off] = '\0';
    httpcCloseContext(&ctx);
    return mem;
}

// --- EMA/MACD helpers ---
static void compute_ema_series(const double *data, int n, int period, double *out) {
    if (!data || !out || n <= 0 || period <= 0 || period > n) return;

    double k = 2.0 / (period + 1.0);

    double sum = 0.0;
    for (int i = 0; i < period; i++) sum += data[i];
    double ema = sum / period;

    for (int i = 0; i < period - 1; i++) out[i] = 0.0;
    out[period - 1] = ema;

    for (int i = period; i < n; i++) {
        ema = (data[i] - ema) * k + ema;
        out[i] = ema;
    }
}

static int compute_macd_last_two(const double *closes, int n,
                                 double *macd_prev, double *macd_last,
                                 double *signal_prev, double *signal_last) {
    if (!closes || n < (SLOW_EMA_PERIOD + SIGNAL_EMA_PERIOD + 1)) return 0;

    double *ema_fast = (double *)malloc(sizeof(double) * n);
    double *ema_slow = (double *)malloc(sizeof(double) * n);
    if (!ema_fast || !ema_slow) { free(ema_fast); free(ema_slow); return 0; }

    compute_ema_series(closes, n, FAST_EMA_PERIOD, ema_fast);
    compute_ema_series(closes, n, SLOW_EMA_PERIOD, ema_slow);

    int macd_start = SLOW_EMA_PERIOD - 1;
    int macd_count = n - macd_start;
    if (macd_count <= 0) { free(ema_fast); free(ema_slow); return 0; }

    double *macd_line = (double *)malloc(sizeof(double) * macd_count);
    if (!macd_line) { free(ema_fast); free(ema_slow); return 0; }

    for (int i = 0; i < macd_count; i++) {
        int idx = macd_start + i;
        macd_line[i] = ema_fast[idx] - ema_slow[idx];
    }

    if (macd_count < SIGNAL_EMA_PERIOD + 1) {
        free(ema_fast); free(ema_slow); free(macd_line);
        return 0;
    }

    double *signal_line = (double *)malloc(sizeof(double) * macd_count);
    if (!signal_line) {
        free(ema_fast); free(ema_slow); free(macd_line);
        return 0;
    }
    compute_ema_series(macd_line, macd_count, SIGNAL_EMA_PERIOD, signal_line);

    *macd_last = macd_line[macd_count - 1];
    *macd_prev = macd_line[macd_count - 2];
    *signal_last = signal_line[macd_count - 1];
    *signal_prev = signal_line[macd_count - 2];

    free(ema_fast);
    free(ema_slow);
    free(macd_line);
    free(signal_line);
    return 1;
}

// --- Series helpers ---
static int ensure_series_capacity(Series* s, int min_cap) {
    if (!s) return 0;
    if (s->cap >= min_cap) return 1;
    int new_cap = (s->cap > 0) ? s->cap * 2 : 64;
    if (new_cap < min_cap) new_cap = min_cap;
    double* p = (double*)realloc(s->data, sizeof(double) * new_cap);
    if (!p) return 0;
    s->data = p;
    s->cap = new_cap;
    return 1;
}

static int series_append(Series* s, double v) {
    if (!s) return 0;
    if (!ensure_series_capacity(s, s->n + 1)) return 0;
    s->data[s->n++] = v;
    return 1;
}

// --- JSON parsing helpers ---
static int extract_daily_closes(cJSON *result, double **out_closes, int *out_n) {
    if (!result || !out_closes || !out_n) return 0;

    cJSON *indicators = cJSON_GetObjectItemCaseSensitive(result, "indicators");
    if (!indicators) return 0;
    cJSON *quote_arr = cJSON_GetObjectItemCaseSensitive(indicators, "quote");
    if (!cJSON_IsArray(quote_arr) || cJSON_GetArraySize(quote_arr) == 0) return 0;
    cJSON *quote = cJSON_GetArrayItem(quote_arr, 0);
    if (!quote) return 0;

    cJSON *close_arr = cJSON_GetObjectItemCaseSensitive(quote, "close");
    if (!cJSON_IsArray(close_arr)) return 0;

    int m = cJSON_GetArraySize(close_arr);
    if (m <= 0) return 0;

    double *closes = (double *)malloc(sizeof(double) * m);
    if (!closes) return 0;
    int n = 0;

    for (int i = 0; i < m; i++) {
        cJSON *item = cJSON_GetArrayItem(close_arr, i);
        if (item && cJSON_IsNumber(item)) {
            closes[n++] = item->valuedouble;
        }
    }

    if (n == 0) {
        free(closes);
        return 0;
    }

    double *shrunk = (double *)realloc(closes, sizeof(double) * n);
    if (shrunk) closes = shrunk;

    *out_closes = closes;
    *out_n = n;
    return 1;
}

// --- UI + data ---
static void print_error_on_line(const char* ticker, const char* error_msg, int row) {
    GOTO(row, 1);
    CLEARLINE();
    printf("%-8s | %s", ticker, error_msg);
}

static void parse_and_print_stock_data(const char *json_1d, int row) {
    cJSON *root1 = cJSON_Parse(json_1d);
    if (!root1) {
        print_error_on_line("JSON", "Parse error", row);
        return;
    }

    cJSON *chart1 = cJSON_GetObjectItemCaseSensitive(root1, "chart");
    cJSON *result_array1 = cJSON_GetObjectItemCaseSensitive(chart1, "result");
    if (!cJSON_IsArray(result_array1) || cJSON_GetArraySize(result_array1) == 0) {
        const char* err_desc = "Invalid ticker or no data";
        cJSON* error_obj = cJSON_GetObjectItemCaseSensitive(chart1, "error");
        if (error_obj) {
            cJSON* desc = cJSON_GetObjectItemCaseSensitive(error_obj, "description");
            if (desc && cJSON_IsString(desc) && desc->valuestring) err_desc = desc->valuestring;
        }
        print_error_on_line("API", err_desc, row);
        cJSON_Delete(root1);
        return;
    }

    cJSON *result1 = cJSON_GetArrayItem(result_array1, 0);
    cJSON *meta1 = cJSON_GetObjectItemCaseSensitive(result1, "meta");
    const char *symbol = "UNKNOWN";
    if (meta1) {
        cJSON *sym = cJSON_GetObjectItemCaseSensitive(meta1, "symbol");
        if (sym && cJSON_IsString(sym) && sym->valuestring) symbol = sym->valuestring;
    }

    double *closes1 = NULL; int n1 = 0;
    int ok1 = extract_daily_closes(result1, &closes1, &n1);
    if (!ok1 || n1 < 2) {
        print_error_on_line(symbol, "Insufficient data", row);
        if (closes1) free(closes1);
        cJSON_Delete(root1);
        return;
    }

    double last_close_1d = closes1[n1 - 1];

    double prev_close_ref = NAN;
    if (meta1) {
        cJSON *pc = cJSON_GetObjectItemCaseSensitive(meta1, "previousClose");
        if (pc && cJSON_IsNumber(pc)) prev_close_ref = pc->valuedouble;
        else {
            cJSON *cpc = cJSON_GetObjectItemCaseSensitive(meta1, "chartPreviousClose");
            if (cpc && cJSON_IsNumber(cpc)) prev_close_ref = cpc->valuedouble;
            else {
                cJSON *rmpc = cJSON_GetObjectItemCaseSensitive(meta1, "regularMarketPreviousClose");
                if (rmpc && cJSON_IsNumber(rmpc)) prev_close_ref = rmpc->valuedouble;
            }
        }
    }

    double base_prev_close = (!isnan(prev_close_ref)) ? prev_close_ref : closes1[n1 - 2];
    double change_1d = last_close_1d - base_prev_close;
    double pct_change_1d = (base_prev_close != 0.0) ? (change_1d / base_prev_close) * 100.0 : 0.0;

    // Session series
    int ticker_index = row - DATA_START_ROW;
    if (ticker_index < 0 || ticker_index >= num_tickers) ticker_index = 0;
    Series* s = &g_series[ticker_index];
    series_append(s, last_close_1d);

    // MACD from session series
    double macd_prev = 0.0, macd_last = 0.0, signal_prev = 0.0, signal_last = 0.0;
    int has_macd = compute_macd_last_two(s->data, s->n, &macd_prev, &macd_last, &signal_prev, &signal_last);

    double macd_pct = 0.0, signal_pct = 0.0;
    if (has_macd && last_close_1d != 0.0) {
        macd_pct = (macd_last / last_close_1d) * 100.0;
        signal_pct = (signal_last / last_close_1d) * 100.0;
    }

    // Cross detection
    int bullish_cross = 0, bearish_cross = 0;
    if (has_macd) {
        bullish_cross = (macd_prev <= signal_prev) && (macd_last > signal_last);
        bearish_cross = (macd_prev >= signal_prev) && (macd_last < signal_last);
    }
    char cross_mark = has_macd ? (bullish_cross ? '^' : (bearish_cross ? 'v' : ' ')) : '?';

    // Price movement vs previous fetch
    double prev_price_seen = (g_prev_price ? g_prev_price[ticker_index] : NAN);
    char move = ' ';
    if (!isnan(prev_price_seen)) {
        if (last_close_1d > prev_price_seen) move = '+';
        else if (last_close_1d < prev_price_seen) move = '-';
    }

    // Prepare strings (narrow to fit 3DS console width)
    char macd_buf[16], sig_buf[16];
    if (has_macd) {
        snprintf(macd_buf, sizeof(macd_buf), "%+5.2f%%", macd_pct);
        snprintf(sig_buf, sizeof(sig_buf), "%+5.2f%%", signal_pct);
    } else {
        snprintf(macd_buf, sizeof(macd_buf), "  N/A");
        snprintf(sig_buf, sizeof(sig_buf), "  N/A");
    }

    // Print row: 3DS console is ~50 cols wide; keep fields tight
    GOTO(row, 1);
    CLEARLINE();
    // Fields: C Tkr Price Chg % MACD Sig  (C is cross mark, move is +/- tick)
    printf("%c %-8.8s %c%8.2f %8.2f %6.2f%% %6s %6s",
           cross_mark, symbol, move, last_close_1d, change_1d, pct_change_1d,
           macd_buf, sig_buf);

    // Store last price
    if (g_prev_price) g_prev_price[ticker_index] = last_close_1d;

    if (closes1) free(closes1);
    cJSON_Delete(root1);
}

static void setup_dashboard_ui(void) {
    CLEAR();
    GOTO(1, 1);
    printf("3DS Stock Dashboard (1d only | MACD from live session polls)");
    GOTO(2, 1); CLEARLINE(); // timestamp line
    GOTO(4, 1);
    printf("C Tkr      Price     Chg     %%Chg   MACD    Sig");
    GOTO(5, 1);
    printf("--------------------------------------------------");

    // Allocate prev price storage
    if (!g_prev_price) {
        g_prev_price = (double*)malloc(sizeof(double) * num_tickers);
        if (g_prev_price) {
            for (int i = 0; i < num_tickers; i++) g_prev_price[i] = NAN;
        }
    }

    // Allocate session series storage
    if (!g_series) {
        g_series = (Series*)calloc(num_tickers, sizeof(Series));
    }

    // Placeholders
    for (int i = 0; i < num_tickers; i++) {
        int row = DATA_START_ROW + i;
        GOTO(row, 1); CLEARLINE();
        printf("  %-8s Fetching 1d data...", tickers[i]);
    }
}

static void update_timestamp(void) {
    time_t t = time(NULL);
    struct tm *tmx = localtime(&t);
    char time_str[64];
    strftime(time_str, sizeof(time_str), "%Y-%m-%d %H:%M:%S", tmx);
    GOTO(2, 1); CLEARLINE();
    printf("Last updated: %s", time_str);
}

static void run_countdown_or_exit(void) {
    int update_line = DATA_START_ROW + num_tickers + 1;
    for (int i = UPDATE_INTERVAL_SECONDS; i > 0; i--) {
        // Check for exit
        hidScanInput();
        u32 kDown = hidKeysDown();
        if (kDown & KEY_START) {
            // Indicate exit and return early
            GOTO(update_line, 1); CLEARLINE();
            printf("Exiting...");
            return;
        }

        GOTO(update_line, 1); CLEARLINE();
        printf("Updating in %2d seconds... (START to exit)", i);
        gfxFlushBuffers();
        gfxSwapBuffers();
        gspWaitForVBlank();
        // Sleep approx 1 second (VBlank loop + small sleep)
        svcSleepThread(1000ULL * 1000ULL * 1000ULL);
    }
    GOTO(update_line, 1); CLEARLINE();
    printf("Updating now...");
    gfxFlushBuffers();
    gfxSwapBuffers();
    gspWaitForVBlank();
}

// --- Cleanup ---
static void cleanup_on_exit(void) {
    if (g_prev_price) { free(g_prev_price); g_prev_price = NULL; }
    if (g_series) {
        for (int i = 0; i < num_tickers; i++) {
            free(g_series[i].data);
            g_series[i].data = NULL;
            g_series[i].n = g_series[i].cap = 0;
        }
        free(g_series);
        g_series = NULL;
    }
}

// --- Entry point ---
int main(int argc, char** argv) {
    // Speedup on New3DS
    osSetSpeedupEnable(true);

    // GFX/console
    gfxInitDefault();
    consoleInit(GFX_TOP, NULL);

    // Network (SOC + HTTPC)
    g_socbuf = (u32*)memalign(SOC_ALIGN, SOC_BUFFERSIZE);
    if (!g_socbuf) {
        printf("SOC memalign failed.\nPress START to exit.\n");
        while (aptMainLoop()) {
            hidScanInput();
            if (hidKeysDown() & KEY_START) break;
            gspWaitForVBlank();
        }
        gfxExit();
        return 0;
    }
    Result rc = socInit(g_socbuf, SOC_BUFFERSIZE);
    if (R_FAILED(rc)) {
        printf("socInit failed: 0x%08lX\n", rc);
        printf("Press START to exit.\n");
        while (aptMainLoop()) {
            hidScanInput();
            if (hidKeysDown() & KEY_START) break;
            gspWaitForVBlank();
        }
        free(g_socbuf); g_socbuf = NULL;
        gfxExit();
        return 0;
    }
    rc = httpcInit(0x100000);
    if (R_FAILED(rc)) {
        printf("httpcInit failed: 0x%08lX\n", rc);
        printf("Press START to exit.\n");
        while (aptMainLoop()) {
            hidScanInput();
            if (hidKeysDown() & KEY_START) break;
            gspWaitForVBlank();
        }
        socExit();
        free(g_socbuf); g_socbuf = NULL;
        gfxExit();
        return 0;
    }

    atexit(cleanup_on_exit);

    setup_dashboard_ui();

    // Main loop
    while (aptMainLoop()) {
        // Allow exit at any time
        hidScanInput();
        if (hidKeysDown() & KEY_START) break;

        update_timestamp();

        char url1d[512];
        for (int i = 0; i < num_tickers; i++) {
            int current_row = DATA_START_ROW + i;
            snprintf(url1d, sizeof(url1d), API_URL_1D_FORMAT, tickers[i]);

            char *json_1d = fetch_url(url1d);
            if (json_1d) {
                parse_and_print_stock_data(json_1d, current_row);
                free(json_1d);
            } else {
                print_error_on_line(tickers[i], "Failed to fetch 1d data", current_row);
            }

            // Let the system breathe between requests
            gfxFlushBuffers(); gfxSwapBuffers(); gspWaitForVBlank();
        }

        gfxFlushBuffers(); gfxSwapBuffers(); gspWaitForVBlank();

        // Countdown or exit
        run_countdown_or_exit();

        // If START was pressed during countdown, exit loop
        hidScanInput();
        if (hidKeysDown() & KEY_START) break;
    }

    // Cleanup network
    httpcExit();
    socExit();
    if (g_socbuf) { free(g_socbuf); g_socbuf = NULL; }

    gfxExit();
    return 0;
}
