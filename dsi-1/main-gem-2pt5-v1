/*
 * ===============================================================================
 * C-Stocks-Dashboard for Nintendo DS/DSi
 * ===============================================================================
 *
 * This file has been updated to compile and run on a Nintendo DS or DSi
 * using the devkitPro toolchain.
 *
 * --- HOW TO COMPILE ---
 * 1. Set up devkitPro with devkitARM.
 * 2. Make sure the following libraries are installed via pacman/dkp-pacman:
 *    - nds-libcurl
 *    - nds-cjson (or add cJSON.c/h to your project source)
 * 3. Create a Makefile and add the following to the LIBS variable:
 *    LIBS    := -lcurl -lcjson -ldswifi9 -lnds -lm
 *
 * --- USAGE NOTES ---
 * - Wi-Fi: You MUST configure your DS/DSi's Wi-Fi connection first using
 *   an official game (like Mario Kart DS) or a WFC-enabled homebrew app.
 *   - Original DS only supports Open or WEP networks.
 *   - DSi supports WPA and WPA2.
 * - SSL/HTTPS: The Yahoo Finance API uses HTTPS. For this to work with libcurl
 *   on DS, SSL verification has been DISABLED. This is insecure. For a secure
 *   method, you would need to provide a CA certificate bundle.
 * - Memory: The DS has limited RAM (4MB, 16MB on DSi). A large list of tickers
 *   may cause the application to run out of memory and crash.
 *
 */

// --- NDS/Platform Specific Headers ---
#ifdef __nds__
  #include <nds.h>
  #include <dswifi9.h>
  #include <sys/socket.h>
  #include <netinet/in.h>
  #include <arpa/inet.h>
#endif

// --- Standard & External Library Headers ---
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>      // For timestamp
#include <math.h>      // For fabs(), isnan()
#include <curl/curl.h>
#include "cJSON.h"
#include <stdbool.h>

// --- Configuration ---
#define UPDATE_INTERVAL_SECONDS 30
#define USER_AGENT "Mozilla/5.0 (Nintendo DSi; U; en) AppleWebKit/532.4 (KHTML, like Gecko) NetFront/3.5.1.12657"
// Only 1d interval
#define API_URL_1D_FORMAT "https://query1.finance.yahoo.com/v8/finance/chart/%s?range=5d&interval=4h&includePrePost=true"
#define DATA_START_ROW 6 // The row number where the first stock ticker will be printed

// MACD parameters (session-based, in "polls" units)
#define FAST_EMA_PERIOD 12
#define SLOW_EMA_PERIOD 26
#define SIGNAL_EMA_PERIOD 9

// Add or remove stock tickers here (keep the list short to conserve RAM)
const char *tickers[] = {
    "BTC-USD", "ETH-USD", "DX-Y.NYB", "^SPX", "^IXIC",
    "GC=F", "CL=F", "NVDA", "INTC", "AMD"
};
const int num_tickers = sizeof(tickers) / sizeof(tickers[0]);

// --- Color Definitions for Terminal (supported by NDS console) ---
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

// --- Terminal/DSi friendly globals ---
static int g_screen_cols = 32; // Fixed for DS
static int g_screen_rows = 24; // Fixed for DS
static int g_compact_mode = 1; // Always enabled for DS
static int g_can_hide_cursor = 0; // Disabled for DS

// --- Function Prototypes ---
static size_t write_callback(void *contents, size_t size, size_t nmemb, void *userp);
char* fetch_url(const char *url);
void parse_and_print_stock_data(const char *json_1d, int row);
void setup_dashboard_ui();
void update_timestamp();
bool run_countdown(); // Changed to return a bool to signal exit
void print_error_on_line(const char* ticker, const char* error_msg, int row);
void hide_cursor();
void show_cursor();
void cleanup_on_exit();

// NDS-specific functions
#ifdef __nds__
bool connect_wifi();
#endif

// Helpers: series ops
int ensure_series_capacity(Series* s, int min_cap);
int series_append(Series* s, double v);

// Helpers for extracting closes and computing MACD
int extract_daily_closes(cJSON *result, double **out_closes, int *out_n);
void compute_ema_series(const double *data, int n, int period, double *out);
int compute_macd_last_two(const double *closes, int n,
                          double *macd_prev, double *macd_last,
                          double *signal_prev, double *signal_last);

// Terminal helpers
void detect_terminal();
void print_hr(int row);
void format_price_compact(double v, char* out, size_t out_sz);


// --- Main Application ---
int main(void) {
    atexit(cleanup_on_exit);

#ifdef __nds__
    // Initialize a console on the top screen.
    // This allows `printf` to work and supports ANSI colors.
    consoleDemoInit();
#endif

    detect_terminal(); // Sets screen size and compact mode for DS

#ifdef __nds__
    // Connect to Wi-Fi. This is required before any network calls.
    if (!connect_wifi()) {
        printf("\n\nPress START to exit.");
        // If Wi-Fi fails, hang here so the user can see the error.
        while (1) {
            swiWaitForVBlank();
            scanKeys();
            if (keysDown() & KEY_START) return 0;
        }
    }
#endif

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
                print_error_on_line(tickers[i], "Fetch failed", current_row);
            }
        }

        if (!run_countdown()) {
            break; // User pressed START, exit the loop
        }
    }

    curl_global_cleanup();
    show_cursor();
    return 0;
}


#ifdef __nds__
/**
 * @brief Connects to Wi-Fi using the DS's stored WFC settings.
 * @return true if connection is successful, false otherwise.
 */
bool connect_wifi() {
    printf("\nConnecting to WiFi...\n");
    printf("Please wait.\n\n");

    // Wifi_AutoConnect() will try to connect to the AP
    // that is configured in the Nintendo WFC settings.
    if (Wifi_AutoConnect() != WFC_SUCCESS) {
        printf(KRED "Connection failed!" KNRM "\n");
        printf("Check your NDS WFC settings.\n");
        printf("(Note: Original DS requires\n an Open or WEP network).\n");
        return false;
    }

    printf(KGRN "WiFi Connected!" KNRM "\n");

    struct in_addr ip = Wifi_GetIPInfo(NULL, NULL, NULL, NULL);
    printf("IP Address: %s\n", inet_ntoa(ip));

    // A small delay so the user can see the IP
    for (int i = 0; i < 120; i++) {
        swiWaitForVBlank();
    }
    return true;
}
#endif


// --- Helper Functions (with NDS modifications) ---

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

        #ifdef __nds__
        // --- NDS SSL HANDLING ---
        // The Yahoo API uses HTTPS. The default libcurl build for DS
        // does not have CA certificates built in.
        // For a quick test, we disable peer verification.
        // WARNING: THIS IS NOT SECURE and is vulnerable to MitM attacks.
        // For a secure implementation, you should use:
        // curl_easy_setopt(curl_handle, CURLOPT_CAINFO, "fat:/path/to/cacert.pem");
        curl_easy_setopt(curl_handle, CURLOPT_SSL_VERIFYPEER, 0L);
        curl_easy_setopt(curl_handle, CURLOPT_SSL_VERIFYHOST, 0L);
        #endif

        res = curl_easy_perform(curl_handle);

        if (res != CURLE_OK) {
            // On DS, stderr is the console, so this will print the error.
            fprintf(stderr, "curl failed: %s\n", curl_easy_strerror(res));
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

// NOTE: The MACD and cJSON parsing functions from the original file are
// platform-independent and do not require changes. They are included here
// verbatim for completeness.

/**
 * @brief Extracts close prices from the Yahoo chart JSON result object.
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
    if (n == 0) { free(closes); return 0; }
    double *shrunk = (double *)realloc(closes, sizeof(double) * n);
    if (shrunk) closes = shrunk;
    *out_closes = closes;
    *out_n = n;
    return 1;
}

/**
 * @brief Computes an EMA series for data[] into out[].
 */
void compute_ema_series(const double *data, int n, int period, double *out) {
    if (!data || !out || n <= 0 || period <= 0 || period > n) return;
    double k = 2.0 / (period + 1.0);
    double sum = 0.0;
    for (int i = 0; i < period; i++) { sum += data[i]; }
    double ema = sum / period;
    for (int i = 0; i < period - 1; i++) { out[i] = 0.0; }
    out[period - 1] = ema;
    for (int i = period; i < n; i++) {
        ema = (data[i] - ema) * k + ema;
        out[i] = ema;
    }
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
    if (!ema_fast || !ema_slow) { free(ema_fast); free(ema_slow); return 0; }
    compute_ema_series(closes, n, FAST_EMA_PERIOD, ema_fast);
    compute_ema_series(closes, n, SLOW_EMA_PERIOD, ema_slow);
    int macd_start = SLOW_EMA_PERIOD - 1;
    int macd_count = n - macd_start;
    if (macd_count <= 0) { free(ema_fast); free(ema_slow); return 0; }
    double *macd_line = (double *)malloc(sizeof(double) * macd_count);
    if (!macd_line) { free(ema_fast); free(ema_slow); return 0; }
    for (int i = 0; i < macd_count; i++) { macd_line[i] = ema_fast[macd_start + i] - ema_slow[macd_start + i]; }
    if (macd_count < SIGNAL_EMA_PERIOD + 1) { free(ema_fast); free(ema_slow); free(macd_line); return 0; }
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

void parse_and_print_stock_data(const char *json_1d, int row) {
    cJSON *root1 = cJSON_Parse(json_1d);
    if (!root1) {
        print_error_on_line("JSON", "Parse error", row);
        return;
    }

    cJSON *chart1 = cJSON_GetObjectItemCaseSensitive(root1, "chart");
    cJSON *result_array1 = cJSON_GetObjectItemCaseSensitive(chart1, "result");
    if (!cJSON_IsArray(result_array1) || cJSON_GetArraySize(result_array1) == 0) {
        char* err_desc = (char*)"Invalid ticker";
        cJSON* error_obj = cJSON_GetObjectItemCaseSensitive(chart1, "error");
        if (error_obj && cJSON_GetObjectItemCaseSensitive(error_obj, "description")) {
            err_desc = cJSON_GetObjectItemCaseSensitive(error_obj, "description")->valuestring;
        }
        print_error_on_line("API", err_desc, row);
        cJSON_Delete(root1);
        return;
    }

    cJSON *result1 = cJSON_GetArrayItem(result_array1, 0);
    cJSON *meta1 = cJSON_GetObjectItemCaseSensitive(result1, "meta");
    const char *symbol = cJSON_GetObjectItemCaseSensitive(meta1, "symbol")->valuestring;

    double *closes1 = NULL;
    int n1 = 0;
    if (!extract_daily_closes(result1, &closes1, &n1) || n1 < 2) {
        print_error_on_line(symbol, "No data", row);
        if (closes1) free(closes1);
        cJSON_Delete(root1);
        return;
    }

    double last_close_1d = closes1[n1 - 1];
    cJSON *pc_obj = cJSON_GetObjectItemCaseSensitive(meta1, "previousClose");
    double base_prev_close = (pc_obj && cJSON_IsNumber(pc_obj)) ? pc_obj->valuedouble : closes1[n1 - 2];
    double change_1d = last_close_1d - base_prev_close;
    double pct_change_1d = (base_prev_close != 0.0) ? (change_1d / base_prev_close) * 100.0 : 0.0;

    int ticker_index = row - DATA_START_ROW;
    Series* s = &g_series[ticker_index];
    series_append(s, last_close_1d);

    double macd_prev = 0.0, macd_last = 0.0, signal_prev = 0.0, signal_last = 0.0;
    int has_macd = compute_macd_last_two(s->data, s->n, &macd_prev, &macd_last, &signal_prev, &signal_last);
    double macd_pct = (has_macd && last_close_1d != 0.0) ? (macd_last / last_close_1d) * 100.0 : 0.0;

    bool bullish_cross = has_macd && (macd_prev <= signal_prev) && (macd_last > signal_last);
    bool bearish_cross = has_macd && (macd_prev >= signal_prev) && (macd_last < signal_last);
    
    const char* ticker_bg = bullish_cross ? BGRN : (bearish_cross ? BRED : "");
    const char* ticker_reset = (bullish_cross || bearish_cross) ? KNRM : "";
    double prev_price_seen = g_prev_price[ticker_index];
    const char* price_bg = "";
    if (!isnan(prev_price_seen)) {
        price_bg = (last_close_1d > prev_price_seen) ? BGRN : ((last_close_1d < prev_price_seen) ? BRED : "");
    }

    const char* color_pct = (pct_change_1d >= 0) ? KGRN : KRED;
    const char* color_macd = (has_macd && macd_pct >= 0) ? KGRN : KRED;

    printf("\033[%d;1H", row);

    // Compact mode is always on for DS
    char price_buf[16];
    format_price_compact(last_close_1d, price_buf, sizeof(price_buf));

    printf("%s%-8.8s%s %s%7s%s %s%+5.2f%%%s %s%+5.2f%%%s\033[K",
           ticker_bg, symbol, ticker_reset,
           price_bg, price_buf, KNRM,
           color_pct, pct_change_1d, KNRM,
           color_macd, has_macd ? macd_pct : 0.0, KNRM);

    fflush(stdout);
    g_prev_price[ticker_index] = last_close_1d;
    free(closes1);
    cJSON_Delete(root1);
}


void print_error_on_line(const char* ticker, const char* error_msg, int row) {
    printf("\033[%d;1H", row);
    // Compact mode is always on for DS
    int avail = g_screen_cols - 9; // Space for "TICKER | "
    char buf[64];
    snprintf(buf, sizeof(buf), "%s", error_msg ? error_msg : "Error");
    if ((int)strlen(buf) > avail) buf[avail] = '\0';
    printf("%-8.8s %s%s%s\033[K", ticker, KRED, buf, KNRM);

    fflush(stdout);
}

void setup_dashboard_ui() {
    hide_cursor();
    printf("\033[2J\033[H"); // Clear screen and go home

    if (!g_prev_price) {
        g_prev_price = (double*)malloc(sizeof(double) * num_tickers);
        if (g_prev_price) {
            for (int i = 0; i < num_tickers; i++) g_prev_price[i] = NAN;
        }
    }
    if (!g_series) {
        g_series = (Series*)calloc(num_tickers, sizeof(Series));
    }

    printf("--- DSi Stock Dash (MACD Live) ---\n");
    printf("\n"); // Timestamp line
    printf("\n");

    // Headers (Compact mode only)
    printf("%-8.8s %7s %7s %7s\n", "Symbol", "Price", "%Chg", "MACD%");
    print_hr(5);

    for (int i = 0; i < num_tickers; i++) {
        printf("\033[%d;1H%-8.8s %sFetching...%s\033[K", DATA_START_ROW + i, tickers[i], KYEL, KNRM);
    }
    fflush(stdout);
}

void update_timestamp() {
    time_t t = time(NULL);
    struct tm *tm = localtime(&t);
    char time_str[64];
    // time() on DS may return epoch 0 if not synced via wifi/NTP.
    if (tm) {
        strftime(time_str, sizeof(time_str), "%Y-%m-%d %H:%M:%S", tm);
    } else {
        strcpy(time_str, "N/A");
    }

    printf("\033[2;1HUpdated: %s\033[K", time_str);
    fflush(stdout);
}

/**
 * @brief Displays a countdown timer and polls for user input to exit.
 * @return false if the user presses START to exit, true otherwise.
 */
bool run_countdown() {
    int update_line = DATA_START_ROW + num_tickers + 1;

    for (int i = UPDATE_INTERVAL_SECONDS; i > 0; i--) {
        printf("\033[%d;1H\033[KUpdate in %2d s (START=exit)", i);
        fflush(stdout);
        
        // DS equivalent of sleep(1) that also polls for input
        for (int j = 0; j < 60; j++) {
            #ifdef __nds__
            swiWaitForVBlank();
            scanKeys();
            if (keysDown() & KEY_START) {
                return false; // Signal to exit
            }
            #endif
        }
    }
    printf("\033[%d;1H\033[KUpdating now...                 ", update_line);
    fflush(stdout);
    return true; // Signal to continue
}


void hide_cursor() {
    if (g_can_hide_cursor) printf("\033[?25l");
    fflush(stdout);
}

void show_cursor() {
    if (g_can_hide_cursor) printf("\033[?25h");
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
        }
        free(g_series);
        g_series = NULL;
    }
}

// --- Terminal helpers ---
void detect_terminal() {
#ifdef __nds__
    // For Nintendo DS, the screen size and mode are fixed.
    g_screen_cols = 32;
    g_screen_rows = 24;
    g_compact_mode = 1;     // Always use compact mode
    g_can_hide_cursor = 0;  // NDS console doesn't reliably support this
#else
    // Fallback for non-NDS builds (original behavior)
    g_screen_cols = 80;
    g_screen_rows = 24;
    g_compact_mode = 0;
    g_can_hide_cursor = 1;
#endif
}

void print_hr(int row) {
    printf("\033[%d;1H", row);
    for (int i = 0; i < g_screen_cols; i++) putchar('-');
    fflush(stdout);
}

void format_price_compact(double v, char* out, size_t out_sz) {
    if (!out || out_sz < 8) return;
    if (fabs(v) < 10000.0) {
        snprintf(out, out_sz, "%7.2f", v);
    } else if (fabs(v) < 1000000.0) {
        double k = v / 1000.0;
        if (fabs(k) < 100.0) snprintf(out, out_sz, "%5.1fk", k);
        else snprintf(out, out_sz, "%5.0fk", k);
    } else {
        double m = v / 1000000.0;
        if (fabs(m) < 100.0) snprintf(out, out_sz, "%5.1fM", m);
        else snprintf(out, out_sz, "%5.0fM", m);
    }
}
