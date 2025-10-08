#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>   // For timestamp
#include <math.h>   // For fabs(), isnan()
#include <curl/curl.h>
#include "cJSON.h"

#ifdef __3DS__
#include <3ds.h>
#include <malloc.h>
#else
#include <unistd.h> // For sleep()
#endif

// --- Configuration ---
#define UPDATE_INTERVAL_SECONDS 30
#define USER_AGENT "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/98.0.4758.102 Safari/537.36"
#define API_URL_1D_FORMAT "https://query1.finance.yahoo.com/v8/finance/chart/%s?range=1d&interval=4h&includePrePost=true"
// Keep data table starting at row 6 (fits 3DS console height with 16 tickers)
#define DATA_START_ROW 6

// MACD parameters (session-based, in "polls" units)
#define FAST_EMA_PERIOD 12
#define SLOW_EMA_PERIOD 26
#define SIGNAL_EMA_PERIOD 9

// Add or remove stock tickers here
const char *tickers[] = {
    "BTC-USD", "ETH-USD", "DX-Y.NYB", "^SPX", "^IXIC",
    "GC=F", "CL=F", "NG=F", "NVDA", "INTC",
    "AMD", "MU", "PFE", "UNH", "TGT", "TRAK"
};
const int num_tickers = sizeof(tickers) / sizeof(tickers[0]);

// --- Color Definitions for Terminal (ANSI) ---
#define KNRM  "\x1B[0m"  // Reset all attributes
#define KRED  "\x1B[31m" // Red (fg)
#define KGRN  "\x1B[32m" // Green (fg)
#define KYEL  "\x1B[33m" // Yellow (fg)
#define BRED  "\x1B[41m" // Red (bg)
#define BGRN  "\x1B[42m" // Green (bg)

// --- Per-ticker previous price (for bg coloring) ---
static double* g_prev_price = NULL; // allocated in setup_dashboard_ui()

// --- Per-ticker session series (live polled values) ---
typedef struct {
    double *data;
    int n;
    int cap;
} Series;

static Series* g_series = NULL; // allocated in setup_dashboard_ui()

// --- Struct to hold HTTP response data ---
typedef struct {
    char *memory;
    size_t size;
} MemoryStruct;

// --- Function Prototypes ---
static size_t write_callback(void *contents, size_t size, size_t nmemb, void *userp);
char* fetch_url(const char *url);
void parse_and_print_stock_data(const char *json_1d, int row);
void setup_dashboard_ui();
void update_timestamp();
void update_status_line(int seconds_left);
void print_error_on_line(const char* ticker, const char* error_msg, int row);
void hide_cursor();
void show_cursor();
void cleanup_on_exit();

// Helpers: series ops
int ensure_series_capacity(Series* s, int min_cap);
int series_append(Series* s, double v);

// Helpers for extracting closes and computing MACD
int extract_daily_closes(cJSON *result, double **out_closes, int *out_n);
void compute_ema_series(const double *data, int n, int period, double *out);
int compute_macd_percent(const double *closes, int n, double *macd_pct, double *signal_pct);
int compute_macd_last_two(const double *closes, int n,
                          double *macd_prev, double *macd_last,
                          double *signal_prev, double *signal_last);

#ifdef __3DS__
// 3DS SOC buffer
static u32* g_socbuf = NULL;
#endif

// --- Main Application ---
int main(void) {
    atexit(cleanup_on_exit);

#ifdef __3DS__
    // 3DS init
    gfxInitDefault();
    consoleInit(GFX_TOP, NULL);

    // Initialize networking (SOC) for curl
    g_socbuf = (u32*)memalign(0x1000, 0x100000);
    if (!g_socbuf) {
        printf("SOC memalign failed\nPress START to exit.\n");
        while (aptMainLoop()) {
            hidScanInput();
            if (hidKeysDown() & KEY_START) break;
            gspWaitForVBlank();
            gfxFlushBuffers();
            gfxSwapBuffers();
        }
        gfxExit();
        return 0;
    }
    if (socInit(g_socbuf, 0x100000) != 0) {
        printf("socInit failed\nPress START to exit.\n");
        while (aptMainLoop()) {
            hidScanInput();
            if (hidKeysDown() & KEY_START) break;
            gspWaitForVBlank();
            gfxFlushBuffers();
            gfxSwapBuffers();
        }
        gfxExit();
        return 0;
    }

    curl_global_init(CURL_GLOBAL_DEFAULT);
    setup_dashboard_ui();

    u64 interval_ms = (u64)UPDATE_INTERVAL_SECONDS * 1000ULL;
    u64 next_update = osGetTime(); // first update immediately

    while (aptMainLoop()) {
        hidScanInput();
        u32 kDown = hidKeysDown();
        if (kDown & KEY_START) break;

        u64 now = osGetTime();
        int seconds_left = (next_update > now) ? (int)((next_update - now + 999) / 1000) : 0;

        if (now >= next_update) {
            // Fire update
            update_timestamp();

            char url1d[512];
            printf("\033[%d;1H\033[KUpdating now...", DATA_START_ROW + num_tickers + 1);
            for (int i = 0; i < num_tickers; i++) {
                int current_row = DATA_START_ROW + i;

                snprintf(url1d, sizeof(url1d), API_URL_1D_FORMAT, tickers[i]);

                char *json_1d = fetch_url(url1d);
                if (json_1d) {
                    parse_and_print_stock_data(json_1d, current_row);
                    free(json_1d);
                } else {
                    print_error_on_line(tickers[i], "Fetch failed", current_row);
                }

                // Let the screen stay responsive during long updates
                gspWaitForVBlank();
                gfxFlushBuffers();
                gfxSwapBuffers();
            }

            next_update = osGetTime() + interval_ms;
            seconds_left = UPDATE_INTERVAL_SECONDS;
        }

        update_status_line(seconds_left);

        gspWaitForVBlank();
        gfxFlushBuffers();
        gfxSwapBuffers();
    }

    curl_global_cleanup();
    socExit();
    if (g_socbuf) { free(g_socbuf); g_socbuf = NULL; }
    gfxExit();
    return 0;

#else
    // Desktop/original behavior
    curl_global_init(CURL_GLOBAL_ALL);

    setup_dashboard_ui();

    while (1) {
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
        }

        // Simple countdown (blocking) on desktop; 3DS uses non-blocking loop
        int update_line = DATA_START_ROW + num_tickers + 1;
        for (int i = UPDATE_INTERVAL_SECONDS; i > 0; i--) {
            printf("\033[%d;1H\033[KUpdating in %2d s...", update_line, i);
            fflush(stdout);
            sleep(1);
        }
        printf("\033[%d;1H\033[KUpdating now...           ", update_line);
        fflush(stdout);
    }

    curl_global_cleanup();
    show_cursor();
    return 0;
#endif
}

// --- Helper Functions ---

static size_t write_callback(void *contents, size_t size, size_t nmemb, void *userp) {
    size_t realsize = size * nmemb;
    MemoryStruct *mem = (MemoryStruct *)userp;

    char *ptr = realloc(mem->memory, mem->size + realsize + 1);
    if (ptr == NULL) {
        printf("error: not enough memory (realloc returned NULL)\n");
        return 0;
    }

    mem->memory = ptr;
    memcpy(&(mem->memory[mem->size]), contents, realsize);
    mem->size += realsize;
    mem->memory[mem->size] = 0;

    return realsize;
}

char* fetch_url(const char *url) {
    CURL *curl_handle;
    CURLcode res;
    MemoryStruct chunk;

    chunk.memory = malloc(1);
    chunk.size = 0;

    curl_handle = curl_easy_init();
    if (curl_handle) {
        curl_easy_setopt(curl_handle, CURLOPT_URL, url);
        curl_easy_setopt(curl_handle, CURLOPT_USERAGENT, USER_AGENT);
        curl_easy_setopt(curl_handle, CURLOPT_WRITEFUNCTION, write_callback);
        curl_easy_setopt(curl_handle, CURLOPT_WRITEDATA, (void *)&chunk);
        curl_easy_setopt(curl_handle, CURLOPT_FOLLOWLOCATION, 1L);
#ifdef __3DS__
        // Often necessary on 3DS homebrew due to missing CA bundle:
        curl_easy_setopt(curl_handle, CURLOPT_SSL_VERIFYPEER, 0L);
        curl_easy_setopt(curl_handle, CURLOPT_SSL_VERIFYHOST, 0L);
#endif

        res = curl_easy_perform(curl_handle);

        if (res != CURLE_OK) {
            fprintf(stderr, "curl_easy_perform() failed: %s\n", curl_easy_strerror(res));
            free(chunk.memory);
            curl_easy_cleanup(curl_handle);
            return NULL;
        }

        curl_easy_cleanup(curl_handle);
        return chunk.memory;
    }

    free(chunk.memory);
    return NULL;
}

/**
 * @brief Extracts close prices from the Yahoo chart JSON result object.
 *        Works for any interval (1m, 5m, 1d, etc.).
 */
int extract_daily_closes(cJSON *result, double **out_closes, int *out_n) {
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

/**
 * @brief Computes an EMA series for data[] into out[].
 *        out[i] is defined starting at i = period-1.
 */
void compute_ema_series(const double *data, int n, int period, double *out) {
    if (!data || !out || n <= 0 || period <= 0 || period > n) return;

    double k = 2.0 / (period + 1.0);

    // Seed EMA with SMA of first 'period'
    double sum = 0.0;
    for (int i = 0; i < period; i++) {
        sum += data[i];
    }
    double ema = sum / period;
    for (int i = 0; i < period - 1; i++) {
        out[i] = 0.0; // not used
    }
    out[period - 1] = ema;

    for (int i = period; i < n; i++) {
        ema = (data[i] - ema) * k + ema;
        out[i] = ema;
    }
}

/**
 * @brief Computes MACD% and Signal% relative to the last close.
 */
int compute_macd_percent(const double *closes, int n, double *macd_pct, double *signal_pct) {
    if (!closes || n < (SLOW_EMA_PERIOD + SIGNAL_EMA_PERIOD)) return 0;

    double *ema_fast = (double *)malloc(sizeof(double) * n);
    double *ema_slow = (double *)malloc(sizeof(double) * n);
    if (!ema_fast || !ema_slow) {
        free(ema_fast); free(ema_slow);
        return 0;
    }
    compute_ema_series(closes, n, FAST_EMA_PERIOD, ema_fast);
    compute_ema_series(closes, n, SLOW_EMA_PERIOD, ema_slow);

    int macd_start = SLOW_EMA_PERIOD - 1;
    int macd_count = n - macd_start;
    if (macd_count <= 0) {
        free(ema_fast); free(ema_slow);
        return 0;
    }

    double *macd_line = (double *)malloc(sizeof(double) * macd_count);
    if (!macd_line) {
        free(ema_fast); free(ema_slow);
        return 0;
    }

    for (int i = 0; i < macd_count; i++) {
        int idx = macd_start + i;
        macd_line[i] = ema_fast[idx] - ema_slow[idx];
    }

    if (macd_count < SIGNAL_EMA_PERIOD) {
        free(ema_fast); free(ema_slow); free(macd_line);
        return 0;
    }

    double *signal_line = (double *)malloc(sizeof(double) * macd_count);
    if (!signal_line) {
        free(ema_fast); free(ema_slow); free(macd_line);
        return 0;
    }
    compute_ema_series(macd_line, macd_count, SIGNAL_EMA_PERIOD, signal_line);

    double last_close = closes[n - 1];
    if (last_close == 0.0) {
        free(ema_fast); free(ema_slow); free(macd_line); free(signal_line);
        return 0;
    }

    double macd_last = macd_line[macd_count - 1];
    double signal_last = signal_line[macd_count - 1];

    *macd_pct = (macd_last / last_close) * 100.0;
    *signal_pct = (signal_last / last_close) * 100.0;

    free(ema_fast);
    free(ema_slow);
    free(macd_line);
    free(signal_line);
    return 1;
}

/**
 * @brief Computes the last two values of MACD and Signal lines (raw, not %).
 */
int compute_macd_last_two(const double *closes, int n,
                          double *macd_prev, double *macd_last,
                          double *signal_prev, double *signal_last) {
    if (!closes || n < (SLOW_EMA_PERIOD + SIGNAL_EMA_PERIOD + 1)) return 0;

    double *ema_fast = (double *)malloc(sizeof(double) * n);
    double *ema_slow = (double *)malloc(sizeof(double) * n);
    if (!ema_fast || !ema_slow) {
        free(ema_fast); free(ema_slow);
        return 0;
    }
    compute_ema_series(closes, n, FAST_EMA_PERIOD, ema_fast);
    compute_ema_series(closes, n, SLOW_EMA_PERIOD, ema_slow);

    int macd_start = SLOW_EMA_PERIOD - 1;
    int macd_count = n - macd_start;
    if (macd_count <= 0) {
        free(ema_fast); free(ema_slow);
        return 0;
    }

    double *macd_line = (double *)malloc(sizeof(double) * macd_count);
    if (!macd_line) {
        free(ema_fast); free(ema_slow);
        return 0;
    }
    for (int i = 0; i < macd_count; i++) {
        int idx = macd_start + i;
        macd_line[i] = ema_fast[idx] - ema_slow[idx];
    }

    if (macd_count < SIGNAL_EMA_PERIOD + 1) {
        free(ema_fast); free(ema_slow); free(macd_line);
        return 0; // need at least two matured signal values
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
int ensure_series_capacity(Series* s, int min_cap) {
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

int series_append(Series* s, double v) {
    if (!s) return 0;
    if (!ensure_series_capacity(s, s->n + 1)) return 0;
    s->data[s->n++] = v;
    return 1;
}

/**
 * @brief Parses 1d JSON for price/change and updates a compact row (fits 3DS).
 */
void parse_and_print_stock_data(const char *json_1d, int row) {
    // Parse 1d JSON
    cJSON *root1 = cJSON_Parse(json_1d);
    if (!root1) {
        print_error_on_line("JSON", "Parse Error (1d)", row);
        return;
    }

    cJSON *chart1 = cJSON_GetObjectItemCaseSensitive(root1, "chart");
    cJSON *result_array1 = cJSON_GetObjectItemCaseSensitive(chart1, "result");
    if (!cJSON_IsArray(result_array1) || cJSON_GetArraySize(result_array1) == 0) {
        char* err_desc = (char*)"Invalid 1d ticker or no data";
        cJSON* error_obj = cJSON_GetObjectItemCaseSensitive(chart1, "error");
        if (error_obj && cJSON_GetObjectItemCaseSensitive(error_obj, "description")) {
            err_desc = cJSON_GetObjectItemCaseSensitive(error_obj, "description")->valuestring;
        }
        print_error_on_line("API Error", err_desc, row);
        cJSON_Delete(root1);
        return;
    }

    cJSON *result1 = cJSON_GetArrayItem(result_array1, 0);
    cJSON *meta1 = cJSON_GetObjectItemCaseSensitive(result1, "meta");
    const char *symbol = NULL;
    if (meta1) {
        cJSON *sym = cJSON_GetObjectItemCaseSensitive(meta1, "symbol");
        if (sym && cJSON_IsString(sym)) symbol = sym->valuestring;
    }
    if (!symbol) symbol = "UNKNOWN";

    double *closes1 = NULL;
    int n1 = 0;
    int ok1 = extract_daily_closes(result1, &closes1, &n1);
    if (!ok1 || n1 < 2) {
        print_error_on_line(symbol, "Insufficient 1d data", row);
        if (closes1) free(closes1);
        cJSON_Delete(root1);
        return;
    }

    // Latest price and change vs previousClose (with fallbacks)
    double last_close_1d = closes1[n1 - 1];

    double prev_close_ref = NAN;
    if (meta1) {
        cJSON *pc = cJSON_GetObjectItemCaseSensitive(meta1, "previousClose");
        if (pc && cJSON_IsNumber(pc)) {
            prev_close_ref = pc->valuedouble;
        } else {
            cJSON *cpc = cJSON_GetObjectItemCaseSensitive(meta1, "chartPreviousClose");
            if (cpc && cJSON_IsNumber(cpc)) {
                prev_close_ref = cpc->valuedouble;
            } else {
                cJSON *rmpc = cJSON_GetObjectItemCaseSensitive(meta1, "regularMarketPreviousClose");
                if (rmpc && cJSON_IsNumber(rmpc)) {
                    prev_close_ref = rmpc->valuedouble;
                }
            }
        }
    }

    double base_prev_close = (!isnan(prev_close_ref)) ? prev_close_ref : closes1[n1 - 2];
    double change_1d = last_close_1d - base_prev_close;
    double pct_change_1d = (base_prev_close != 0.0) ? (change_1d / base_prev_close) * 100.0 : 0.0;

    // Session series update (append the latest observed price)
    int ticker_index = row - DATA_START_ROW;
    if (ticker_index < 0 || ticker_index >= num_tickers) ticker_index = 0; // safety
    Series* s = &g_series[ticker_index];
    series_append(s, last_close_1d);

    // Compute MACD/Signal from the session series
    double macd_prev = 0.0, macd_last = 0.0, signal_prev = 0.0, signal_last = 0.0;
    int has_macd = compute_macd_last_two(s->data, s->n, &macd_prev, &macd_last, &signal_prev, &signal_last);

    double macd_pct = 0.0, signal_pct = 0.0;
    if (has_macd && last_close_1d != 0.0) {
        macd_pct = (macd_last / last_close_1d) * 100.0;
        signal_pct = (signal_last / last_close_1d) * 100.0;
    }

    // Detect crossover at the latest session step
    int bullish_cross = 0, bearish_cross = 0;
    if (has_macd) {
        bullish_cross = (macd_prev <= signal_prev) && (macd_last > signal_last);
        bearish_cross = (macd_prev >= signal_prev) && (macd_last < signal_last);
    }

    // Ticker background based on cross
    const char* ticker_bg_prefix = "";
    const char* ticker_bg_suffix = "";
    if (has_macd) {
        if (bullish_cross) {
            ticker_bg_prefix = BGRN;
            ticker_bg_suffix = KNRM;
        } else if (bearish_cross) {
            ticker_bg_prefix = BRED;
            ticker_bg_suffix = KNRM;
        }
    }

    // Price cell background vs previous fetch
    double prev_price_seen = (g_prev_price ? g_prev_price[ticker_index] : NAN);
    const char* price_bg = "";
    if (!isnan(prev_price_seen)) {
        if (last_close_1d > prev_price_seen) price_bg = BGRN;
        else if (last_close_1d < prev_price_seen) price_bg = BRED;
    }

    // Colors
    const char* color_change = (change_1d >= 0) ? KGRN : KRED;
    const char* color_pct = (pct_change_1d >= 0) ? KGRN : KRED;
    const char* color_macd = (has_macd && macd_pct >= 0) ? KGRN : KRED;
    const char* color_signal = (has_macd && signal_pct >= 0) ? KGRN : KRED;

    // MACD buffers
    char macd_buf[12], sig_buf[12];
    if (has_macd) {
        snprintf(macd_buf, sizeof(macd_buf), "%+5.2f", macd_pct);
        snprintf(sig_buf, sizeof(sig_buf), "%+5.2f", signal_pct);
    } else {
        snprintf(macd_buf, sizeof(macd_buf), "%5s", "N/A");
        snprintf(sig_buf, sizeof(sig_buf), "%5s", "N/A");
    }

    // Compact row output (fits 3DS top screen width ~50 cols)
    // Columns: Tkr(6) Price(8) Chg(7) %Chg(6 incl %) MACD(5) Sig(5) + 1-space gaps
    printf("\033[%d;1H", row);
    printf("%s%-8s%s|%s%9.2f%s|%s%+9.2f%s|%s%+6.2f%%%s|%s%6s%s|%s%6s%s\033[K",
           ticker_bg_prefix, symbol, ticker_bg_suffix,
           price_bg, last_close_1d, KNRM,
           color_change, change_1d, KNRM,
           color_pct, pct_change_1d, KNRM,
           color_macd, macd_buf, KNRM,
           color_signal, sig_buf, KNRM);
    fflush(stdout);

    // Store last price for next comparison
    if (g_prev_price) g_prev_price[ticker_index] = last_close_1d;

    if (closes1) free(closes1);
    cJSON_Delete(root1);
}

void print_error_on_line(const char* ticker, const char* error_msg, int row) {
    printf("\033[%d;1H", row);
    printf("%-6.6s %s%-40.40s%s\033[K", ticker, KRED, error_msg, KNRM);
    fflush(stdout);
}

void setup_dashboard_ui() {
    hide_cursor();
    printf("\033[2J\033[H"); // Clear screen, home cursor

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

    // Title + timestamp placeholders
    printf("3DS Stock Dashboard (1d | MACD from live polls)\n");
    printf("\n"); // timestamp line
    printf("\n");

    // Headers (compact for 3DS width)
    printf("%-8s|%9s|%9s|%7s|%6s|%6s\n", "Tkr", "Price", "Chg", "%Chg", "MACD", "Sig");
    printf("--------------------------------------------------\n");

    // Placeholders
    for (int i = 0; i < num_tickers; i++) {
        int row = DATA_START_ROW + i;
        printf("\033[%d;1H", row);
        printf("%-6.6s %sFetching...%s\033[K", tickers[i], KYEL, KNRM);
    }
    fflush(stdout);
}

void update_timestamp() {
    time_t t = time(NULL);
    struct tm *tm = localtime(&t);
    char time_str[64];
    strftime(time_str, sizeof(time_str), "%Y-%m-%d %H:%M:%S", tm);

    printf("\033[2;1H");
    printf("Last updated: %s\033[K", time_str);
    fflush(stdout);
}

void update_status_line(int seconds_left) {
    int update_line = DATA_START_ROW + num_tickers + 1;
    printf("\033[%d;1H\033[KUpdating in %2d s  (START=Exit)", update_line, seconds_left);
    fflush(stdout);
    #ifdef __3DS__
        svcSleepThread(1000000000LL);
#else
        sleep(1);
#endif
}

void hide_cursor() {
    printf("\033[?25l");
    fflush(stdout);
}

void show_cursor() {
    printf("\033[?25h");
    fflush(stdout);
}

void cleanup_on_exit() {
    show_cursor();
    if (g_prev_price) {
        free(g_prev_price);
        g_prev_price = NULL;
    }
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
