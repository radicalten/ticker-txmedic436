#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <curl/curl.h>
#include "cJSON.h"

#ifndef __3DS__
#include <unistd.h> // sleep() for desktop
#endif

#ifdef __3DS__
#include <3ds.h>
#include <malloc.h>

// 3DS SOC (sockets) buffer
static u32* soc_buffer = NULL;
#define SOC_ALIGN      0x1000
#define SOC_BUFFERSIZE 0x100000

// Helpers to flush/swap
static inline void gfx_present() {
    gfxFlushBuffers();
    gfxSwapBuffers();
    gspWaitForVBlank();
}
static inline void sleep_sec_3ds(int s) {
    svcSleepThread((s64)s * 1000000000LL);
}
#endif

// --- Configuration ---
#define UPDATE_INTERVAL_SECONDS 15
#define USER_AGENT "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/98.0.4758.102 Safari/537.36"
// Fetch daily candles for 1 year so we can compute MACD on daily closes
#define API_URL_FORMAT "https://query1.finance.yahoo.com/v8/finance/chart/%s?range=1y&interval=1d&includePrePost=false"
#define DATA_START_ROW 6 // The row number where the first stock ticker will be printed

// MACD parameters (standard)
#define FAST_EMA_PERIOD 12
#define SLOW_EMA_PERIOD 26
#define SIGNAL_EMA_PERIOD 9

// Add or remove stock tickers here
const char *tickers[] = {"AAPL", "GOOGL", "TSLA", "MSFT", "NVDA", "BTC-USD", "ETH-USD"};
const int num_tickers = sizeof(tickers) / sizeof(tickers[0]);

// --- Color Definitions ---
#ifdef __3DS__
// 3DS console is typically monochrome; drop color escape codes
#define KNRM  ""
#define KRED  ""
#define KGRN  ""
#define KYEL  ""
#define KBLU  ""
#else
#define KNRM  "\x1B[0m"  // Normal
#define KRED  "\x1B[31m" // Red
#define KGRN  "\x1B[32m" // Green
#define KYEL  "\x1B[33m" // Yellow
#define KBLU  "\x1B[34m" // Blue (Added for price)
#endif

// --- Struct to hold HTTP response data ---
typedef struct {
    char *memory;
    size_t size;
} MemoryStruct;

// --- Function Prototypes ---
static size_t write_callback(void *contents, size_t size, size_t nmemb, void *userp);
char* fetch_url(const char *url);
void parse_and_print_stock_data(const char *json_string, int row);
void setup_dashboard_ui();
void update_timestamp();
void print_error_on_line(const char* ticker, const char* error_msg, int row);
void hide_cursor();
void show_cursor();
void cleanup_on_exit();

// Helpers for extracting closes and computing MACD
int extract_daily_closes(cJSON *result, double **out_closes, int *out_n);
void compute_ema_series(const double *data, int n, int period, double *out);
int compute_macd_percent(const double *closes, int n, double *macd_pct, double *signal_pct);

// --- Helper Functions ---

/**
 * libcurl callback to write received data into a memory buffer.
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
 * Fetches content from a given URL using libcurl.
 * Returns malloc'd string on success, NULL on failure.
 */
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
        curl_easy_setopt(curl_handle, CURLOPT_NOSIGNAL, 1L);
        curl_easy_setopt(curl_handle, CURLOPT_CONNECTTIMEOUT, 10L);
        curl_easy_setopt(curl_handle, CURLOPT_TIMEOUT, 20L);
#ifdef __3DS__
        // 3DS typically doesn't have a CA bundle; disable verification (insecure)
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
 * Extracts daily close prices from the Yahoo chart JSON result object.
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
 * Computes an EMA series for data[] into out[].
 */
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

/**
 * Computes MACD% and Signal% relative to the last close.
 */
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

    if (macd_count < SIGNAL_EMA_PERIOD) { free(ema_fast); free(ema_slow); free(macd_line); return 0; }

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

/**
 * Parses JSON and prints updated stock data on a specific row.
 * Uses DAILY close and previous close for Price/Change/%Change.
 * Adds MACD% and Signal% standardized by close.
 */
void parse_and_print_stock_data(const char *json_string, int row) {
    cJSON *root = cJSON_Parse(json_string);
    if (root == NULL) {
        print_error_on_line("JSON", "Parse Error", row);
        return;
    }

    cJSON *chart = cJSON_GetObjectItemCaseSensitive(root, "chart");
    cJSON *result_array = cJSON_GetObjectItemCaseSensitive(chart, "result");
    if (!cJSON_IsArray(result_array) || cJSON_GetArraySize(result_array) == 0) {
        char* err_desc = "Invalid ticker or no data";
        cJSON* error_obj = cJSON_GetObjectItemCaseSensitive(chart, "error");
        if (error_obj && cJSON_GetObjectItemCaseSensitive(error_obj, "description")) {
            err_desc = cJSON_GetObjectItemCaseSensitive(error_obj, "description")->valuestring;
        }
        print_error_on_line("API Error", err_desc, row);
        cJSON_Delete(root);
        return;
    }

    cJSON *result = cJSON_GetArrayItem(result_array, 0);
    cJSON *meta = cJSON_GetObjectItemCaseSensitive(result, "meta");
    const char *symbol = NULL;
    if (meta) {
        cJSON *sym = cJSON_GetObjectItemCaseSensitive(meta, "symbol");
        if (sym && cJSON_IsString(sym)) symbol = sym->valuestring;
    }
    if (!symbol) symbol = "UNKNOWN";

    double *closes = NULL;
    int n = 0;
    int ok = extract_daily_closes(result, &closes, &n);
    if (!ok || n < 2) {
        print_error_on_line(symbol, "Insufficient daily data", row);
        if (closes) free(closes);
        cJSON_Delete(root);
        return;
    }

    double last_close = closes[n - 1];
    double prev_close = closes[n - 2];
    double change = last_close - prev_close;
    double percent_change = (prev_close != 0.0) ? (change / prev_close) * 100.0 : 0.0;

    double macd_pct = 0.0, signal_pct = 0.0;
    int has_macd = compute_macd_percent(closes, n, &macd_pct, &signal_pct);

    // Console-safe buffers for MACD/Signal
    char macd_buf[8], sig_buf[8];
    if (has_macd) {
        snprintf(macd_buf, sizeof(macd_buf), "%+5.2f", macd_pct);
        snprintf(sig_buf,  sizeof(sig_buf),  "%+5.2f", signal_pct);
    } else {
        snprintf(macd_buf, sizeof(macd_buf), " N/A ");
        snprintf(sig_buf,  sizeof(sig_buf),  " N/A ");
    }

    // Move cursor to the start of the specified row
    printf("\033[%d;1H", row);

#ifdef __3DS__
    // 3DS-friendly compact layout (fits ~50 cols):
    // Ticker(7)  Price(7.2)  Change(+6.2)  %(+5.1%)  MACD(+5.2)  SIG(+5.2)
    printf("%-7s  %7.2f  %+6.2f  %+5.1f%%  %5s  %5s\033[K",
           symbol, last_close, change, percent_change, macd_buf, sig_buf);
#else
    // Desktop layout with color (wide terminal)
    char change_sign = (change >= 0) ? '+' : '-';
    char pct_sign = (percent_change >= 0) ? '+' : '-';
    const char* color_change = (change >= 0) ? KGRN : KRED;
    const char* color_pct    = (percent_change >= 0) ? KGRN : KRED;

    printf("%-10s | %s%10.2f%s | %s%c%9.2f%s | %s%c%10.2f%%%s | %9s | %9s\033[K",
           symbol,
           KBLU, last_close, KNRM,
           color_change, change_sign, (change >= 0 ? change : -change), KNRM,
           color_pct, pct_sign, (percent_change >= 0 ? percent_change : -percent_change), KNRM,
           macd_buf, sig_buf);
#endif
    fflush(stdout);

    free(closes);
    cJSON_Delete(root);
}

/**
 * Prints an error message on a specific row of the dashboard.
 */
void print_error_on_line(const char* ticker, const char* error_msg, int row) {
    printf("\033[%d;1H", row);
#ifdef __3DS__
    printf("%-7s  %-40s\033[K", ticker, error_msg);
#else
    printf("%-10s | %s%-80s%s\033[K", ticker, KRED, error_msg, KNRM);
#endif
    fflush(stdout);
}

// --- UI and Terminal Control Functions ---

/**
 * Performs the initial one-time setup of the dashboard UI.
 */
void setup_dashboard_ui() {
    hide_cursor();
    printf("\033[2J\033[H");

#ifdef __3DS__
    printf("3DS Stock Dashboard\n");
    printf("\n"); // Timestamp goes here
    printf("\n");
    // Short, screen-safe headers (about 50 columns total)
    printf("%-7s  %7s  %6s  %6s  %5s  %5s\n", "Ticker", "Price", "Change", "%", "MACD", "SIG");
    printf("--------------------------------------------------\n");
#else
    printf("--- C Terminal Stock Dashboard ---\n");
    printf("\n");
    printf("\n");
    printf("%-10s | %11s | %11s | %13s | %10s | %10s\n",
           "Ticker", "Price", "Change", "% Change", "MACD%", "Signal%");
    printf("----------------------------------------------------------------------------------------------------\n");
#endif

    // Initial placeholder text for each ticker at exact rows
    for (int i = 0; i < num_tickers; i++) {
        int row = DATA_START_ROW + i;
        printf("\033[%d;1H", row);
#ifdef __3DS__
        printf("%-7s  %sFetching...%s\033[K", tickers[i], KYEL, KNRM);
#else
        printf("%-10s | %sFetching daily data...%s\033[K", tickers[i], KYEL, KNRM);
#endif
    }
    fflush(stdout);
}

/**
 * Updates the "Last updated" timestamp on the second line of the screen.
 */
void update_timestamp() {
    time_t t = time(NULL);
    struct tm *tmv = localtime(&t);
    char time_str[64];
    strftime(time_str, sizeof(time_str), "%Y-%m-%d %H:%M:%S", tmv);

    printf("\033[2;1H");
#ifdef __3DS__
    printf("Last updated: %s  (START: Exit)\033[K", time_str);
#else
    printf("Last updated: %s\033[K", time_str);
#endif
    fflush(stdout);
}

#ifndef __3DS__
/**
 * Desktop-only: Displays a live countdown timer at the bottom of the dashboard.
 */
void run_countdown() {
    int update_line = DATA_START_ROW + num_tickers + 1;

    for (int i = UPDATE_INTERVAL_SECONDS; i > 0; i--) {
        printf("\033[%d;1H", update_line);
        printf("\033[KUpdating in %2d seconds...", i);
        fflush(stdout);
        sleep(1);
    }
    printf("\033[%d;1H\033[KUpdating now...           ", update_line);
    fflush(stdout);
}
#endif

void hide_cursor() {
#ifndef __3DS__
    printf("\033[?25l");
    fflush(stdout);
#endif
}

void show_cursor() {
#ifndef __3DS__
    printf("\033[?25h");
    fflush(stdout);
#endif
}

void cleanup_on_exit() {
    show_cursor();
}

// --- Main Application ---

int main(void) {
    atexit(cleanup_on_exit);

#ifdef __3DS__
    // 3DS init
    gfxInitDefault();
    consoleInit(GFX_TOP, NULL);

    // Initialize sockets for libcurl
    soc_buffer = (u32*)memalign(SOC_ALIGN, SOC_BUFFERSIZE);
    if (!soc_buffer) {
        consoleClear();
        printf("soc memalign failed\n");
        gfx_present();
        sleep_sec_3ds(3);
        gfxExit();
        return 1;
    }
    if (socInit(soc_buffer, SOC_BUFFERSIZE) != 0) {
        consoleClear();
        printf("socInit failed\n");
        gfx_present();
        sleep_sec_3ds(3);
        free(soc_buffer);
        gfxExit();
        return 1;
    }

    curl_global_init(CURL_GLOBAL_DEFAULT);
    setup_dashboard_ui();

    time_t next_fetch = 0; // fetch immediately on first loop

    while (aptMainLoop()) {
        hidScanInput();
        u32 kDown = hidKeysDown();
        if (kDown & KEY_START) break;

        time_t now = time(NULL);
        // Fetch if time
        if (now >= next_fetch) {
            update_timestamp();

            char url[512];
            for (int i = 0; i < num_tickers; i++) {
                int current_row = DATA_START_ROW + i;
                snprintf(url, sizeof(url), API_URL_FORMAT, tickers[i]);

                char *json_response = fetch_url(url);
                if (json_response) {
                    parse_and_print_stock_data(json_response, current_row);
                    free(json_response);
                } else {
                    print_error_on_line(tickers[i], "Failed to fetch data", current_row);
                }
            }

            next_fetch = now + UPDATE_INTERVAL_SECONDS;
        }

        // Countdown line
        int update_line = DATA_START_ROW + num_tickers + 1;
        int remaining = (int)difftime(next_fetch, now);
        if (remaining < 0) remaining = 0;
        printf("\033[%d;1H\033[KUpdating in %2d s  (START: Exit)", update_line, remaining);
        fflush(stdout);

        gfx_present();
    }

    // Cleanup 3DS resources
    curl_global_cleanup();
    socExit();
    if (soc_buffer) free(soc_buffer);
    gfxExit();
    return 0;

#else
    // Desktop/terminal behavior (original loop)
    curl_global_init(CURL_GLOBAL_ALL);
    setup_dashboard_ui();

    while (1) {
        update_timestamp();

        char url[512];
        for (int i = 0; i < num_tickers; i++) {
            int current_row = DATA_START_ROW + i;
            snprintf(url, sizeof(url), API_URL_FORMAT, tickers[i]);

            char *json_response = fetch_url(url);
            if (json_response) {
                parse_and_print_stock_data(json_response, current_row);
                free(json_response);
            } else {
                print_error_on_line(tickers[i], "Failed to fetch data", current_row);
            }
        }

        run_countdown();
    }

    curl_global_cleanup();
    show_cursor();
    return 0;
#endif
}
