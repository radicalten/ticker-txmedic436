// Build-time switches:
// - Desktop/Linux/macOS: builds and runs in a normal terminal
// - 3DS (devkitPro): builds a .3dsx that runs using libctru's console on the top screen.
//   Install: pacman -S 3ds-dev 3ds-curl 3ds-mbedtls 3ds-zlib (and vendor cJSON sources into your project)

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <math.h>

// Use libcurl everywhere (works on 3DS via 3ds-curl)
#include <curl/curl.h>

#include "cJSON.h"

// 3DS includes and helpers
#if defined(__3DS__)
  #include <3ds.h>
  // Sleep helper (seconds)
  static inline void sleep_seconds_3ds(int sec) { svcSleepThread((s64)sec * 1000000000LL); }
  static inline void present_frame() { gfxFlushBuffers(); gfxSwapBuffers(); gspWaitForVBlank(); }
#else
  #include <unistd.h>
  static inline void present_frame() { /* no-op on desktop */ }
#endif

// --- Configuration ---
#define UPDATE_INTERVAL_SECONDS 30
#define USER_AGENT "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/98.0.4758.102 Safari/537.36"
// Keep MACD from 5m candles (5 days)
#define API_URL_5M_FORMAT "https://query1.finance.yahoo.com/v8/finance/chart/%s?range=5d&interval=5m&includePrePost=false"
// Use daily candles for Change/%Change (last two daily closes)
#define API_URL_1D_FORMAT "https://query1.finance.yahoo.com/v8/finance/chart/%s?range=5d&interval=1d&includePrePost=false"

// 3DS console is ~50 columns wide; keep rows compact and narrow
#if defined(__3DS__)
  #define DATA_START_ROW 6
#else
  #define DATA_START_ROW 6
#endif

// MACD parameters (standard)
#define FAST_EMA_PERIOD 12
#define SLOW_EMA_PERIOD 26
#define SIGNAL_EMA_PERIOD 9

// Add or remove stock tickers here
const char *tickers[] = {
    "BTC-USD", "ETH-USD", "XRP-USD", "BNB-USD", "SOL-USD",
    "DOGE-USD", "ADA-USD", "LINK-USD", "XLM-USD", "AVAX-USD",
    "BCH-USD", "LTC-USD", "DOT-USD", "XMR-USD", "ATOM-USD"
};
const int num_tickers = sizeof(tickers) / sizeof(tickers[0]);

// --- Color Definitions ---
// Disable colors on 3DS console (limited/unsupported); keep on desktop terminals
#if defined(__3DS__)
  #define KNRM  ""
  #define KRED  ""
  #define KGRN  ""
  #define KYEL  ""
  #define BRED  ""
  #define BGRN  ""
#else
  #define KNRM  "\x1b[0m"   // Reset
  #define KRED  "\x1b[31m"
  #define KGRN  "\x1b[32m"
  #define KYEL  "\x1b[33m"
  #define BRED  "\x1b[41m"
  #define BGRN  "\x1b[42m"
#endif

// --- Per-ticker previous price (for bg coloring; no-op on 3DS since bg disabled) ---
static double* g_prev_price = NULL;

// --- Struct to hold HTTP response data ---
typedef struct {
    char *memory;
    size_t size;
} MemoryStruct;

// --- Function Prototypes ---
static size_t write_callback(void *contents, size_t size, size_t nmemb, void *userp);
char* fetch_url(const char *url);
void parse_and_print_stock_data(const char *json_5m, const char *json_1d, int row);
void setup_dashboard_ui();
void update_timestamp();
void run_countdown(int *should_exit);
void print_error_on_line(const char* ticker, const char* error_msg, int row);
void hide_cursor();
void show_cursor();
void cleanup_on_exit();

// Helpers
int extract_daily_closes(cJSON *result, double **out_closes, int *out_n);
void compute_ema_series(const double *data, int n, int period, double *out);
int compute_macd_percent(const double *closes, int n, double *macd_pct, double *signal_pct);
int compute_macd_last_two(const double *closes, int n,
                          double *macd_prev, double *macd_last,
                          double *signal_prev, double *signal_last);

// --- Main Application ---
int main(void) {

#if defined(__3DS__)
    // 3DS init
    gfxInitDefault();
    consoleInit(GFX_TOP, NULL);
    // optional: consoleInit(GFX_BOTTOM, NULL);
#endif

    // Register cleanup callback (desktop mostly; harmless on 3DS)
    atexit(cleanup_on_exit);

    // Initialize libcurl (works on 3DS via 3ds-curl)
    curl_global_init(CURL_GLOBAL_DEFAULT);

    setup_dashboard_ui();

#if defined(__3DS__)
    int should_exit = 0;
    while (aptMainLoop() && !should_exit) {
        // Exit on START
        hidScanInput();
        u32 kDown = hidKeysDown();
        if (kDown & KEY_START) break;

        update_timestamp();
        present_frame();

        char url5m[512];
        char url1d[512];

        for (int i = 0; i < num_tickers; i++) {
            int current_row = DATA_START_ROW + i;
            snprintf(url5m, sizeof(url5m), API_URL_5M_FORMAT, tickers[i]);
            snprintf(url1d, sizeof(url1d), API_URL_1D_FORMAT, tickers[i]);

            char *json_5m = fetch_url(url5m);
            char *json_1d = fetch_url(url1d);

            if (json_5m) {
                parse_and_print_stock_data(json_5m, json_1d, current_row);
            } else {
                print_error_on_line(tickers[i], "Failed to fetch 5m data", current_row);
            }

            if (json_5m) free(json_5m);
            if (json_1d) free(json_1d);

            present_frame();
        }

        run_countdown(&should_exit);
    }

    curl_global_cleanup();
    gfxExit();
    return 0;

#else
    // Desktop/normal terminal loop
    while (1) {
        update_timestamp();

        char url5m[512];
        char url1d[512];
        for (int i = 0; i < num_tickers; i++) {
            int current_row = DATA_START_ROW + i;
            snprintf(url5m, sizeof(url5m), API_URL_5M_FORMAT, tickers[i]);
            snprintf(url1d, sizeof(url1d), API_URL_1D_FORMAT, tickers[i]);

            char *json_5m = fetch_url(url5m);
            char *json_1d = fetch_url(url1d);

            if (json_5m) {
                parse_and_print_stock_data(json_5m, json_1d, current_row);
            } else {
                print_error_on_line(tickers[i], "Failed to fetch 5m data", current_row);
            }

            if (json_5m) free(json_5m);
            if (json_1d) free(json_1d);
        }

        int dummy = 0;
        run_countdown(&dummy);
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
        curl_easy_setopt(curl_handle, CURLOPT_TIMEOUT, 20L);
        curl_easy_setopt(curl_handle, CURLOPT_CONNECTTIMEOUT, 10L);

        // If your 3DS time/certificates are off, you may need to disable verify (insecure):
        // curl_easy_setopt(curl_handle, CURLOPT_SSL_VERIFYPEER, 0L);
        // curl_easy_setopt(curl_handle, CURLOPT_SSL_VERIFYHOST, 0L);

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

void compute_ema_series(const double *data, int n, int period, double *out) {
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

int compute_macd_percent(const double *closes, int n, double *macd_pct, double *signal_pct) {
    if (!closes || n < (SLOW_EMA_PERIOD + SIGNAL_EMA_PERIOD)) return 0;

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

    if (macd_count < SIGNAL_EMA_PERIOD) {
        free(ema_fast); free(ema_slow); free(macd_line);
        return 0;
    }

    double *signal_line = (double *)malloc(sizeof(double) * macd_count);
    if (!signal_line) { free(ema_fast); free(ema_slow); free(macd_line); return 0; }
    compute_ema_series(macd_line, macd_count, SIGNAL_EMA_PERIOD, signal_line);

    double last_close = closes[n - 1];
    if (last_close == 0.0) { free(ema_fast); free(ema_slow); free(macd_line); free(signal_line); return 0; }

    double macd_last = macd_line[macd_count - 1];
    double signal_last = signal_line[macd_count - 1];

    *macd_pct = (macd_last / last_close) * 100.0;
    *signal_pct = (signal_last / last_close) * 100.0;

    free(ema_fast); free(ema_slow); free(macd_line); free(signal_line);
    return 1;
}

int compute_macd_last_two(const double *closes, int n,
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
    if (!signal_line) { free(ema_fast); free(ema_slow); free(macd_line); return 0; }
    compute_ema_series(macd_line, macd_count, SIGNAL_EMA_PERIOD, signal_line);

    *macd_last = macd_line[macd_count - 1];
    *macd_prev = macd_line[macd_count - 2];
    *signal_last = signal_line[macd_count - 1];
    *signal_prev = signal_line[macd_count - 2];

    free(ema_fast); free(ema_slow); free(macd_line); free(signal_line);
    return 1;
}

void parse_and_print_stock_data(const char *json_5m, const char *json_1d, int row) {
    cJSON *root5 = cJSON_Parse(json_5m);
    if (root5 == NULL) {
        print_error_on_line("JSON", "Parse Error (5m)", row);
        return;
    }

    cJSON *chart5 = cJSON_GetObjectItemCaseSensitive(root5, "chart");
    cJSON *result_array5 = cJSON_GetObjectItemCaseSensitive(chart5, "result");
    if (!cJSON_IsArray(result_array5) || cJSON_GetArraySize(result_array5) == 0) {
        char* err_desc = "Invalid 5m ticker or no data";
        cJSON* error_obj = cJSON_GetObjectItemCaseSensitive(chart5, "error");
        if (error_obj && cJSON_GetObjectItemCaseSensitive(error_obj, "description")) {
            err_desc = cJSON_GetObjectItemCaseSensitive(error_obj, "description")->valuestring;
        }
        print_error_on_line("API Error", err_desc, row);
        cJSON_Delete(root5);
        return;
    }

    cJSON *result5 = cJSON_GetArrayItem(result_array5, 0);
    cJSON *meta5 = cJSON_GetObjectItemCaseSensitive(result5, "meta");
    const char *symbol = NULL;
    if (meta5) {
        cJSON *sym = cJSON_GetObjectItemCaseSensitive(meta5, "symbol");
        if (sym && cJSON_IsString(sym)) symbol = sym->valuestring;
    }
    if (!symbol) symbol = "UNKNOWN";

    double *closes5 = NULL;
    int n5 = 0;
    int ok5 = extract_daily_closes(result5, &closes5, &n5);
    if (!ok5 || n5 < 2) {
        print_error_on_line(symbol, "Insufficient 5m data", row);
        if (closes5) free(closes5);
        cJSON_Delete(root5);
        return;
    }

    double last_close_5m = closes5[n5 - 1];
    double prev_close_5m = closes5[n5 - 2];
    double change_5m = last_close_5m - prev_close_5m;
    double pct_change_5m = (prev_close_5m != 0.0) ? (change_5m / prev_close_5m) * 100.0 : 0.0;

    double macd_prev = 0.0, macd_last = 0.0, signal_prev = 0.0, signal_last = 0.0;
    int has_macd = compute_macd_last_two(closes5, n5, &macd_prev, &macd_last, &signal_prev, &signal_last);

    double macd_pct = 0.0, signal_pct = 0.0;
    if (has_macd && last_close_5m != 0.0) {
        macd_pct = (macd_last / last_close_5m) * 100.0;
        signal_pct = (signal_last / last_close_5m) * 100.0;
    }

    double change_to_show = change_5m;
    double pct_to_show = pct_change_5m;

    if (json_1d) {
        cJSON *root1 = cJSON_Parse(json_1d);
        if (root1) {
            cJSON *chart1 = cJSON_GetObjectItemCaseSensitive(root1, "chart");
            cJSON *result_array1 = cJSON_GetObjectItemCaseSensitive(chart1, "result");
            if (cJSON_IsArray(result_array1) && cJSON_GetArraySize(result_array1) > 0) {
                cJSON *result1 = cJSON_GetArrayItem(result_array1, 0);
                double *closes1 = NULL;
                int n1 = 0;
                if (extract_daily_closes(result1, &closes1, &n1) && n1 >= 2) {
                    double last_close_1d = closes1[n1 - 1];
                    double prev_close_1d = closes1[n1 - 2];
                    double change_1d = last_close_1d - prev_close_1d;
                    double pct_change_1d = (prev_close_1d != 0.0) ? (change_1d / prev_close_1d) * 100.0 : 0.0;
                    change_to_show = change_1d;
                    pct_to_show = pct_change_1d;
                }
                if (closes1) free(closes1);
            }
            cJSON_Delete(root1);
        }
    }

    const char* color_change = (change_to_show >= 0) ? KGRN : KRED;
    const char* color_pct    = (pct_to_show >= 0) ? KGRN : KRED;
    const char* color_macd   = (has_macd && macd_pct >= 0) ? KGRN : KRED;
    const char* color_signal = (has_macd && signal_pct >= 0) ? KGRN : KRED;

    char macd_buf[16], sig_buf[16];
    if (has_macd) {
        // FIX: Use %% to print percent signs
        snprintf(macd_buf, sizeof(macd_buf), "%+6.2f%%", macd_pct);
        snprintf(sig_buf,  sizeof(sig_buf),  "%+6.2f%%", signal_pct);
    } else {
        snprintf(macd_buf, sizeof(macd_buf), "%6s", "N/A");
        snprintf(sig_buf,  sizeof(sig_buf), "%6s", "N/A");
    }

    // Cross detection (not highlighted on 3DS since bg disabled)
    int bullish_cross = 0, bearish_cross = 0;
    if (has_macd) {
        bullish_cross = (macd_prev <= signal_prev) && (macd_last > signal_last);
        bearish_cross = (macd_prev >= signal_prev) && (macd_last < signal_last);
    }

    // Prior price for bg up/down (colors disabled on 3DS)
    int ticker_index = row - DATA_START_ROW;
    if (ticker_index < 0 || ticker_index >= num_tickers) ticker_index = 0;
    double prev_price_seen = (g_prev_price ? g_prev_price[ticker_index] : NAN);

    // Move cursor to row start
    printf("\x1b[%d;1H", row);

#if defined(__3DS__)
    // Narrow 3DS layout (fits ~50 columns):
    // Tkr  Price     Chg     %Chg    MACD   Sig
    // ABC  1234.56  +12.34  +1.23%  +0.12% +0.10%
    printf("%-6s %9.2f %+8.2f %+6.2f%% %7s %7s\x1b[K",
           symbol,
           last_close_5m,
           change_to_show,
           pct_to_show,
           macd_buf,
           sig_buf);
#else
    // Desktop layout with colors (no bg on 3DS)
    printf("%-10s | %10.2f | %s%+10.2f%s | %s%+6.2f%%%s | %s%6s%s | %s%6s%s\x1b[K",
           symbol,
           last_close_5m,
           color_change, change_to_show, KNRM,
           color_pct, pct_to_show, KNRM,
           color_macd, macd_buf, KNRM,
           color_signal, sig_buf, KNRM);
#endif

    fflush(stdout);

    if (g_prev_price) {
        g_prev_price[ticker_index] = last_close_5m;
    }

    free(closes5);
    cJSON_Delete(root5);
}

void print_error_on_line(const char* ticker, const char* error_msg, int row) {
    printf("\x1b[%d;1H", row);
#if defined(__3DS__)
    printf("%-6s %s%s%s\x1b[K", ticker, KRED, error_msg, KNRM);
#else
    printf("%-10s | %s%-80s%s\x1b[K", ticker, KRED, error_msg, KNRM);
#endif
    fflush(stdout);
}

void setup_dashboard_ui() {
    hide_cursor();
    // Clear screen and home
    printf("\x1b[2J\x1b[H");

    if (!g_prev_price) {
        g_prev_price = (double*)malloc(sizeof(double) * num_tickers);
        if (g_prev_price) {
            for (int i = 0; i < num_tickers; i++) g_prev_price[i] = NAN;
        }
    }

#if defined(__3DS__)
    printf("=== 3DS Stock Dashboard (MACD: 5m | Change: 1d) ===\n");
    printf("\n"); // for timestamp
    printf("\n");
    printf("%-6s %9s %8s %7s %7s %7s\n", "Tkr", "Price", "Chg", "%Chg", "MACD", "Sig");
    printf("--------------------------------------------------\n");
#else
    printf("--- C Terminal Stock Dashboard (MACD: 5m | Change: 1d) ---\n\n\n");
    printf("%-10s | %10s | %11s | %8s | %8s | %8s\n",
           "Tkr", "Price", "Chg", "%Chg", "MACD", "Sig");
    printf("----------------------------------------------------------------------------------------------------\n");
#endif

    for (int i = 0; i < num_tickers; i++) {
        int row = DATA_START_ROW + i;
        printf("\x1b[%d;1H", row);
#if defined(__3DS__)
        printf("%-6s %sFetching...%s\x1b[K", tickers[i], KYEL, KNRM);
#else
        printf("%-10s | %sFetching 5m+1d data...%s\x1b[K", tickers[i], KYEL, KNRM);
#endif
    }
    fflush(stdout);
    present_frame();
}

void update_timestamp() {
    time_t t = time(NULL);
    struct tm *tmval = localtime(&t);
    char time_str[64];
    strftime(time_str, sizeof(time_str), "%Y-%m-%d %H:%M:%S", tmval);

    printf("\x1b[2;1H");
#if defined(__3DS__)
    printf("Last updated: %s (START to quit)\x1b[K", time_str);
#else
    printf("Last updated: %s\x1b[K", time_str);
#endif
    fflush(stdout);
}

void run_countdown(int *should_exit) {
    int update_line = DATA_START_ROW + num_tickers + 1;

    for (int i = UPDATE_INTERVAL_SECONDS; i > 0; i--) {
        printf("\x1b[%d;1H\x1b[KUpdating in %2d seconds...", update_line, i);
        fflush(stdout);

#if defined(__3DS__)
        // Allow exit during countdown on 3DS
        for (int f = 0; f < 5; ++f) {  // check input a few times per sec
            hidScanInput();
            u32 kDown = hidKeysDown();
            if (kDown & KEY_START) {
                if (should_exit) *should_exit = 1;
                return;
            }
            sleep_seconds_3ds(0); // no-op; just yield
            svcSleepThread(200000000LL); // 0.2s
        }
#else
        sleep(1);
#endif
        present_frame();
    }

    printf("\x1b[%d;1H\x1b[KUpdating now...           ", update_line);
    fflush(stdout);
    present_frame();
}

void hide_cursor() {
#if !defined(__3DS__)
    printf("\x1b[?25l");
    fflush(stdout);
#endif
}

void show_cursor() {
#if !defined(__3DS__)
    printf("\x1b[?25h");
    fflush(stdout);
#endif
}

void cleanup_on_exit() {
    show_cursor();
    if (g_prev_price) {
        free(g_prev_price);
        g_prev_price = NULL;
    }
}
