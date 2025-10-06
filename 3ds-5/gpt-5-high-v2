#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h> // For sleep()
#include <time.h>   // For timestamp
#include <math.h>   // For fabs(), isnan()
#include <curl/curl.h>
#include "cJSON.h"

#ifdef __3DS__
#include <3ds.h>
#include <malloc.h>
#endif

// --- Configuration ---
#define UPDATE_INTERVAL_SECONDS 30
#define USER_AGENT "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/98.0.4758.102 Safari/537.36"
// Keep MACD from 5m candles (5 days)
#define API_URL_5M_FORMAT "https://query1.finance.yahoo.com/v8/finance/chart/%s?range=5d&interval=5m&includePrePost=false"
// Use daily candles for Change/%Change (last two daily closes)
#define API_URL_1D_FORMAT "https://query1.finance.yahoo.com/v8/finance/chart/%s?range=5d&interval=1d&includePrePost=false"
#define DATA_START_ROW 6 // The row number where the first stock ticker will be printed

// MACD parameters (standard)
#define FAST_EMA_PERIOD 12
#define SLOW_EMA_PERIOD 26
#define SIGNAL_EMA_PERIOD 9

// Add or remove stock tickers here
const char *tickers[] = {"BTC-USD", "ETH-USD", "XRP-USD", "BNB-USD", "SOL-USD", "DOGE-USD", "ADA-USD", "LINK-USD", "XLM-USD", "AVAX-USD", "BCH-USD", "LTC-USD", "DOT-USD", "XMR-USD", "ATOM-USD"};
const int num_tickers = sizeof(tickers) / sizeof(tickers[0]);

// --- Color Definitions for Terminal ---
#define KNRM  "\x1B[0m"  // Reset all attributes
#define KRED  "\x1B[31m" // Red (fg)
#define KGRN  "\x1B[32m" // Green (fg)
#define KYEL  "\x1B[33m" // Yellow (fg)
// NEW: Background colors for price cell and ticker bg
#define BRED  "\x1B[41m" // Red (bg)
#define BGRN  "\x1B[42m" // Green (bg)

// --- Per-ticker previous price (for bg coloring) ---
static double* g_prev_price = NULL; // allocated in setup_dashboard_ui()

#ifdef __3DS__
static u32* g_soc_buffer = NULL;   // SOC buffer for networking
#endif

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
int  run_countdown(); // returns 0 if user requested exit on 3DS, else 1
void print_error_on_line(const char* ticker, const char* error_msg, int row);
void hide_cursor();
void show_cursor();
void cleanup_on_exit();

// New helpers for extracting closes and computing MACD
int extract_daily_closes(cJSON *result, double **out_closes, int *out_n);
void compute_ema_series(const double *data, int n, int period, double *out);
int compute_macd_percent(const double *closes, int n, double *macd_pct, double *signal_pct);

// New: compute last two MACD/Signal values (raw)
int compute_macd_last_two(const double *closes, int n,
                          double *macd_prev, double *macd_last,
                          double *signal_prev, double *signal_last);

// --- Main Application ---
int main(void) {
    atexit(cleanup_on_exit);

#ifdef __3DS__
    // Initialize 3DS graphics and console
    gfxInitDefault();
    consoleInit(GFX_TOP, NULL);

    // Initialize SOC (network) for libcurl
    size_t soc_buf_size = 0x100000; // 1 MiB
    g_soc_buffer = (u32*)memalign(0x1000, soc_buf_size);
    if (!g_soc_buffer) {
        printf("SOC buffer alloc failed\n");
        gspWaitForVBlank();
        svcSleepThread(2*1000000000LL);
        gfxExit();
        return 1;
    }
    Result socres = socInit(g_soc_buffer, soc_buf_size);
    if (R_FAILED(socres)) {
        printf("socInit failed: 0x%08lX\n", socres);
        gspWaitForVBlank();
        svcSleepThread(2*1000000000LL);
        free(g_soc_buffer);
        gfxExit();
        return 1;
    }
#endif

    // Initialize libcurl
    curl_global_init(CURL_GLOBAL_ALL);

    // Initial, one-time setup: clear screen, hide cursor, print static layout
    setup_dashboard_ui();

#ifdef __3DS__
    int keep_running = 1;
    while (aptMainLoop() && keep_running) {
        hidScanInput();
        u32 kDown = hidKeysDown();
        if (kDown & KEY_START) break;

        // Update the timestamp at the top of the dashboard
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
                print_error_on_line(tickers[i], "Fetch 5m failed", current_row);
            }

            if (json_5m) free(json_5m);
            if (json_1d) free(json_1d);
        }

        keep_running = run_countdown();
    }
#else
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

        if (!run_countdown()) break; // on desktop it always returns 1
    }
#endif

    // Cleanup libcurl
    curl_global_cleanup();
    show_cursor(); // Restore cursor

#ifdef __3DS__
    socExit();
    if (g_soc_buffer) { free(g_soc_buffer); g_soc_buffer = NULL; }
    gfxExit();
#endif
    return 0;
}

// --- Helper Functions ---

/**
 * @brief libcurl callback to write received data into a memory buffer.
 */
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

/**
 * @brief Fetches content from a given URL using libcurl.
 * @return A dynamically allocated string with the response, or NULL on failure.
 *         The caller is responsible for freeing this memory.
 */
char* fetch_url(const char *url) {
    CURL *curl_handle;
    CURLcode res;
    MemoryStruct chunk;

    chunk.memory = malloc(1); // will be grown as needed by the callback
    chunk.size = 0;           // no data at this point

    curl_handle = curl_easy_init();
    if (curl_handle) {
        curl_easy_setopt(curl_handle, CURLOPT_URL, url);
        curl_easy_setopt(curl_handle, CURLOPT_USERAGENT, USER_AGENT);
        curl_easy_setopt(curl_handle, CURLOPT_WRITEFUNCTION, write_callback);
        curl_easy_setopt(curl_handle, CURLOPT_WRITEDATA, (void *)&chunk);
        curl_easy_setopt(curl_handle, CURLOPT_FOLLOWLOCATION, 1L); // Follow redirects

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
 * @param result The "result[0]" object within "chart".
 * @param out_closes Output pointer to a malloc'd array of doubles (valid closes only).
 * @param out_n Output pointer to number of valid closes.
 * @return 1 on success, 0 on failure.
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

    // Shrink to fit
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

    // Compute initial SMA for seeding the EMA
    double sum = 0.0;
    for (int i = 0; i < period; i++) {
        sum += data[i];
    }
    double ema = sum / period;
    for (int i = 0; i < period - 1; i++) {
        out[i] = 0.0; // undefined, but set to 0 for safety
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
 * @return 1 on success (and fills outputs), 0 on failure/insufficient data.
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

/**
 * @brief Parses JSON and prints updated stock data on a specific row.
 *        Compact layout (fits 50 cols): Tkr|Price|Chg|%Chg|MACD
 *        MACD computed from 5m closes; Signal used for crossover highlight only.
 */
void parse_and_print_stock_data(const char *json_5m, const char *json_1d, int row) {
    // Parse 5m JSON (required)
    cJSON *root5 = cJSON_Parse(json_5m);
    if (root5 == NULL) {
        print_error_on_line("JSON", "Parse Error (5m)", row);
        return;
    }

    cJSON *chart5 = cJSON_GetObjectItemCaseSensitive(root5, "chart");
    cJSON *result_array5 = cJSON_GetObjectItemCaseSensitive(chart5, "result");
    if (!cJSON_IsArray(result_array5) || cJSON_GetArraySize(result_array5) == 0) {
        char* err_desc = (char*)"Invalid 5m ticker or no data";
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

    // Extract 5m closes for price and MACD
    double *closes5 = NULL;
    int n5 = 0;
    int ok5 = extract_daily_closes(result5, &closes5, &n5);
    if (!ok5 || n5 < 2) {
        print_error_on_line(symbol, "Insufficient 5m data", row);
        if (closes5) free(closes5);
        cJSON_Delete(root5);
        return;
    }

    // 5m price
    double last_close_5m = closes5[n5 - 1];
    double prev_close_5m = closes5[n5 - 2];
    double change_5m = last_close_5m - prev_close_5m;
    double pct_change_5m = (prev_close_5m != 0.0) ? (change_5m / prev_close_5m) * 100.0 : 0.0;

    // MACD and Signal (for % and crossover)
    double macd_prev = 0.0, macd_last = 0.0, signal_prev = 0.0, signal_last = 0.0;
    int has_macd = compute_macd_last_two(closes5, n5, &macd_prev, &macd_last, &signal_prev, &signal_last);

    // MACD% and Signal% (relative to last close)
    double macd_pct = 0.0, signal_pct = 0.0;
    if (has_macd && last_close_5m != 0.0) {
        macd_pct = (macd_last / last_close_5m) * 100.0;
        signal_pct = (signal_last / last_close_5m) * 100.0;
    }

    // Parse 1d JSON (optional; if fails, fall back to 5m change)
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

    // Determine colors for numeric columns
    const char* color_change = (change_to_show >= 0) ? KGRN : KRED;
    const char* color_pct = (pct_to_show >= 0) ? KGRN : KRED;
    const char* color_macd = (has_macd && macd_pct >= 0) ? KGRN : KRED;

    // Detect crossover at the latest bar
    int bullish_cross = 0, bearish_cross = 0;
    if (has_macd) {
        bullish_cross = (macd_prev <= signal_prev) && (macd_last > signal_last);
        bearish_cross = (macd_prev >= signal_prev) && (macd_last < signal_last);
    }

    // Background highlight for ticker on cross (green for bullish, red for bearish)
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

    // Background color for price cell (vs previous fetch)
    int ticker_index = row - DATA_START_ROW;
    if (ticker_index < 0 || ticker_index >= num_tickers) ticker_index = 0; // safety
    double prev_price_seen = (g_prev_price ? g_prev_price[ticker_index] : NAN);

    const char* price_bg = "";
    if (!isnan(prev_price_seen)) {
        if (last_close_5m > prev_price_seen) {
            price_bg = BGRN;
        } else if (last_close_5m < prev_price_seen) {
            price_bg = BRED;
        }
    }

    // Move cursor to row, col 1
    printf("\033[%d;1H", row);

    // Compact row (fits 50 cols): Tkr|Price|Chg|%Chg|MACD
    // Widths: 8 | 9 | 9 | 7 | 8  => total ~42 (colors don't add width)
    printf("%s%-8s%s|%s%9.2f%s|%s%+8.2f%s|%s%+6.2f%%%s|%s%+7.3f%%%s\033[K",
           // Ticker with optional bg on cross
           ticker_bg_prefix, symbol, ticker_bg_suffix,
           // Price with optional bg highlighting vs previous fetch
           price_bg, last_close_5m, KNRM,
           // Change (1d if available, else 5m)
           color_change, change_to_show, KNRM,
           // % Change (1d if available, else 5m)
           color_pct, pct_to_show, KNRM,
           // MACD% (Signal used only for cross highlight)
           color_macd, macd_pct, KNRM);
    fflush(stdout);

    if (g_prev_price) {
        g_prev_price[ticker_index] = last_close_5m;
    }

    free(closes5);
    cJSON_Delete(root5);
}

/**
 * @brief Prints an error message on a specific row of the dashboard.
 */
void print_error_on_line(const char* ticker, const char* error_msg, int row) {
    // Move cursor to the start of the specified row
    printf("\033[%d;1H", row);
    // Compact error line to fit within 50 columns
    printf("%-8s|%s%-39s%s\033[K", ticker, KRED, error_msg, KNRM);
    fflush(stdout);
}


// --- UI and Terminal Control Functions ---

/**
 * @brief Performs the initial one-time setup of the dashboard UI.
 */
void setup_dashboard_ui() {
    hide_cursor();
    // Clear screen and move to top-left
    printf("\033[2J\033[H");

    // Allocate and initialize previous price storage
    if (!g_prev_price) {
        g_prev_price = (double*)malloc(sizeof(double) * num_tickers);
        if (g_prev_price) {
            for (int i = 0; i < num_tickers; i++) g_prev_price[i] = NAN;
        }
    }

    printf("3DS Stock Dashboard (MACD:5m | Chg:1d)\n");
    printf("\n"); // dynamic timestamp line goes next
    printf("\n");

    // Compact headers to fit 50 cols
    // Columns: Tkr|Price|Chg|%Chg|MACD
    printf("%-8s|%9s|%9s|%7s|%8s\n", "Tkr", "Price", "Chg", "%Chg", "MACD");
    printf("--------------------------------------------------\n");

    // Initial placeholders for each ticker
    for (int i = 0; i < num_tickers; i++) {
        int row = DATA_START_ROW + i;
        printf("\033[%d;1H", row);
        printf("%-8s|%sFetching...%s\033[K", tickers[i], KYEL, KNRM);
    }
    fflush(stdout);
}

/**
 * @brief Updates the "Last updated" timestamp on the second line of the screen.
 */
void update_timestamp() {
    time_t t = time(NULL);
    struct tm *tm = localtime(&t);
    char time_str[64];
    strftime(time_str, sizeof(time_str), "%Y-%m-%d %H:%M:%S", tm);

    // Move cursor to row 2, column 1
    printf("\033[2;1H");
    printf("Last updated: %s\033[K", time_str);
    fflush(stdout);
}

/**
 * @brief Displays a live countdown timer at the bottom of the dashboard.
 * @return 0 if user requested exit (3DS START during countdown), else 1
 */
int run_countdown() {
    int update_line = DATA_START_ROW + num_tickers + 1;

    for (int i = UPDATE_INTERVAL_SECONDS; i > 0; i--) {
        printf("\033[%d;1H", update_line);
        printf("\033[KUpdating in %2d seconds...", i);
        fflush(stdout);

#ifdef __3DS__
        gspWaitForVBlank();
        svcSleepThread(1000000000LL); // 1 sec
        hidScanInput();
        if (hidKeysDown() & KEY_START) return 0;
#else
        sleep(1);
#endif
    }
    printf("\033[%d;1H\033[KUpdating now...           ", update_line);
    fflush(stdout);
    return 1;
}

void hide_cursor() {
    // ANSI: Hides the terminal cursor (ignored by 3DS console if unsupported)
    printf("\033[?25l");
    fflush(stdout);
}

void show_cursor() {
    // ANSI: Shows the terminal cursor (ignored by 3DS console if unsupported)
    printf("\033[?25h");
    fflush(stdout);
}

void cleanup_on_exit() {
    show_cursor();
    if (g_prev_price) {
        free(g_prev_price);
        g_prev_price = NULL;
    }
}
