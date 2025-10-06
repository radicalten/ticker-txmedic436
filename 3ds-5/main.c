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
#define API_URL_5M_FORMAT "https://query1.finance.yahoo.com/v8/finance/chart/%s?range=5d&interval=5m&includePrePost=false"
#define API_URL_1D_FORMAT "https://query1.finance.yahoo.com/v8/finance/chart/%s?range=5d&interval=1d&includePrePost=false"
#define DATA_START_ROW 6

// MACD parameters (standard)
#define FAST_EMA_PERIOD 12
#define SLOW_EMA_PERIOD 26
#define SIGNAL_EMA_PERIOD 9

// Add or remove stock tickers here
const char *tickers[] = {"BTC-USD", "ETH-USD", "DX-Y.NYB", "^TNX", "^SPX", "^RUA", "GC=F","HRC=F", "CL=F", "NG=F", "NVDA", "UNH", "PFE", "TGT", "TRAK"};
const int num_tickers = sizeof(tickers) / sizeof(tickers[0]);

// --- Color Definitions for Terminal ---
#define KNRM  "\x1B[0m"
#define KRED  "\x1B[31m"
#define KGRN  "\x1B[32m"
#define KYEL  "\x1B[33m"
#define BRED  "\x1B[41m"
#define BGRN  "\x1B[42m"

// --- Per-ticker previous price (for bg coloring) ---
static double* g_prev_price = NULL;

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

int extract_daily_closes(cJSON *result, double **out_closes, int *out_n);
void compute_ema_series(const double *data, int n, int period, double *out);
int compute_macd_percent(const double *closes, int n, double *macd_pct, double *signal_pct);
int compute_macd_last_two(const double *closes, int n,
                          double *macd_prev, double *macd_last,
                          double *signal_prev, double *signal_last);

// --- Main Application ---
int main(void) {
    atexit(cleanup_on_exit);

#ifdef __3DS__
    gfxInitDefault();
    consoleInit(GFX_TOP, NULL);

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

    curl_global_init(CURL_GLOBAL_ALL);
    setup_dashboard_ui();

#ifdef __3DS__
    int keep_running = 1;
    while (aptMainLoop() && keep_running) {
        hidScanInput();
        u32 kDown = hidKeysDown();
        if (kDown & KEY_START) break;

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

        if (!run_countdown()) break;
    }
#endif

    curl_global_cleanup();
    show_cursor();

#ifdef __3DS__
    socExit();
    if (g_soc_buffer) { free(g_soc_buffer); g_soc_buffer = NULL; }
    gfxExit();
#endif
    return 0;
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
        // On 3DS, you may not have a CA bundle installed. Disable verification for quick testing.
        // For production, install a CA bundle and re-enable these.
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
    for (int i = 0; i < period; i++) {
        sum += data[i];
    }
    double ema = sum / period;
    for (int i = 0; i < period - 1; i++) {
        out[i] = 0.0;
    }
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

/**
 * @brief Parses JSON and prints updated stock data on a specific row.
 *        Compact layout (fits 50 cols): Tkr|Price|Chg|%Chg|MACD|Sig
 */
void parse_and_print_stock_data(const char *json_5m, const char *json_1d, int row) {
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

    int bullish_cross = 0, bearish_cross = 0;
    if (has_macd) {
        bullish_cross = (macd_prev <= signal_prev) && (macd_last > signal_last);
        bearish_cross = (macd_prev >= signal_prev) && (macd_last < signal_last);
    }

    const char* ticker_bg_prefix = "";
    const char* ticker_bg_suffix = "";
    if (has_macd) {
        if (bullish_cross) { ticker_bg_prefix = BGRN; ticker_bg_suffix = KNRM; }
        else if (bearish_cross) { ticker_bg_prefix = BRED; ticker_bg_suffix = KNRM; }
    }

    int ticker_index = row - DATA_START_ROW;
    if (ticker_index < 0 || ticker_index >= num_tickers) ticker_index = 0;
    double prev_price_seen = (g_prev_price ? g_prev_price[ticker_index] : NAN);

    const char* price_bg = "";
    if (!isnan(prev_price_seen)) {
        if (last_close_5m > prev_price_seen) price_bg = BGRN;
        else if (last_close_5m < prev_price_seen) price_bg = BRED;
    }

    // Pre-format MACD and Signal cells to 7 chars (fits the 50-col layout)
    char macd_buf[16], sig_buf[16];
    if (has_macd) {
        snprintf(macd_buf, sizeof(macd_buf), "%+6.3f%", macd_pct);
        snprintf(sig_buf,   sizeof(sig_buf),   "%+6.3f%", signal_pct);
    } else {
        snprintf(macd_buf, sizeof(macd_buf), "N/A");
        snprintf(sig_buf,   sizeof(sig_buf), "N/A");
    }

    // Move cursor to row, col 1
    printf("\033[%d;1H", row);

    // 50-col layout: Tkr(7)|Price(9)|Chg(8)|%Chg(7)|MACD(7)|Sig(7)
    printf("%s%-8s%s|%s%9.2f%s|%s%+9.2f%s|%s%+6.2f%%%s|%s%6s%s|%s%6s%s\033[K",
           ticker_bg_prefix, symbol, ticker_bg_suffix,
           price_bg, last_close_5m, KNRM,
           color_change, change_to_show, KNRM,
           color_pct, pct_to_show, KNRM,
           color_macd, macd_buf, KNRM,
           color_signal, sig_buf, KNRM);
    fflush(stdout);

    if (g_prev_price) g_prev_price[ticker_index] = last_close_5m;

    free(closes5);
    cJSON_Delete(root5);
}

void print_error_on_line(const char* ticker, const char* error_msg, int row) {
    printf("\033[%d;1H", row);
    // 50 columns: 7 (tkr) + 1 (|) + 42 (msg)
    printf("%-7s|%s%-42s%s\033[K", ticker, KRED, error_msg, KNRM);
    fflush(stdout);
}

// --- UI and Terminal Control Functions ---

void setup_dashboard_ui() {
    hide_cursor();
    printf("\033[2J\033[H");

    if (!g_prev_price) {
        g_prev_price = (double*)malloc(sizeof(double) * num_tickers);
        if (g_prev_price) {
            for (int i = 0; i < num_tickers; i++) g_prev_price[i] = NAN;
        }
    }

    printf("3DS Stock Dashboard (MACD:5m | Chg:1d)\n");
    printf("\n"); // dynamic timestamp goes here
    printf("\n");

    // Headers made to fit 50 columns
    // Tkr(7)|Price(9)|Chg(8)|%Chg(7)|MACD(7)|Sig(7)
    printf("%-8s|%9s|%9s|%7s|%6s|%6s\n", "Tkr", "Price", "Chg", "%Chg", "MACD", "Sig");
    printf("--------------------------------------------------\n");

    for (int i = 0; i < num_tickers; i++) {
        int row = DATA_START_ROW + i;
        printf("\033[%d;1H", row);
        printf("%-7s|%sFetching...%s\033[K", tickers[i], KYEL, KNRM);
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

int run_countdown() {
    int update_line = DATA_START_ROW + num_tickers + 1;

    for (int i = UPDATE_INTERVAL_SECONDS; i > 0; i--) {
        printf("\033[%d;1H", update_line);
        printf("\033[KUpdating in %2d seconds...", i);
        fflush(stdout);

#ifdef __3DS__
        gspWaitForVBlank();
        svcSleepThread(1000000000LL);
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
}
