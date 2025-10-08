// 3DS port of your ANSI+libcurl dashboard
// Build: devkitPro + libctru + portlibs (curl, cJSON). See notes below.

#include <3ds.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <math.h>
#include <curl/curl.h>
#include "cJSON.h"

// --- Configuration ---
#define UPDATE_INTERVAL_SECONDS 30
#define USER_AGENT "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/98.0.4758.102 Safari/537.36"
#define API_URL_1D_FORMAT "https://query1.finance.yahoo.com/v8/finance/chart/%s?range=1d&interval=4h&includePrePost=true"
#define DATA_START_ROW 6

// MACD parameters (session-based)
#define FAST_EMA_PERIOD 12
#define SLOW_EMA_PERIOD 26
#define SIGNAL_EMA_PERIOD 9

// TLS options (choose one)
// 1) Secure: place a CA bundle at romfs:/cacert.pem and set this to 1
// 2) Insecure: set to 0 and we’ll disable peer/host verification (not recommended)
#define USE_ROMFS_CA_BUNDLE 0
#define CA_BUNDLE_PATH "romfs:/cacert.pem"

// --- Tickers ---
const char *tickers[] = {
    "BTC-USD", "ETH-USD", "DX-Y.NYB", "^SPX", "^IXIC",
    "GC=F", "CL=F", "NG=F", "NVDA", "INTC",
    "AMD", "MU", "PFE", "UNH", "TGT", "TRAK"
};
const int num_tickers = sizeof(tickers) / sizeof(tickers[0]);

// --- ANSI Colors ---
#define KNRM  "\x1B[0m"
#define KRED  "\x1B[31m"
#define KGRN  "\x1B[32m"
#define KYEL  "\x1B[33m"
#define BRED  "\x1B[41m"
#define BGRN  "\x1B[42m"

// --- Per-ticker previous price (for bg coloring) ---
static double* g_prev_price = NULL;

// --- Per-ticker session series (polled values) ---
typedef struct {
    double *data;
    int n;
    int cap;
} Series;

static Series* g_series = NULL;

// --- HTTP response buffer ---
typedef struct {
    char *memory;
    size_t size;
} MemoryStruct;

// --- 3DS networking buffer ---
#define SOC_ALIGN      0x1000
#define SOC_BUFFERSIZE 0x100000
static u32* g_socBuf = NULL;

// --- Control flags ---
static bool g_should_quit = false;

// --- Function Prototypes ---
static size_t write_callback(void *contents, size_t size, size_t nmemb, void *userp);
char* fetch_url(const char *url);
void parse_and_print_stock_data(const char *json_1d, int row);
void setup_dashboard_ui();
void update_timestamp();
void run_countdown();
void print_error_on_line(const char* ticker, const char* error_msg, int row);
void hide_cursor();
void show_cursor();
void cleanup_on_exit();

// Series helpers
int ensure_series_capacity(Series* s, int min_cap);
int series_append(Series* s, double v);

// JSON/MACD helpers
int extract_daily_closes(cJSON *result, double **out_closes, int *out_n);
void compute_ema_series(const double *data, int n, int period, double *out);
int compute_macd_percent(const double *closes, int n, double *macd_pct, double *signal_pct);
int compute_macd_last_two(const double *closes, int n,
                          double *macd_prev, double *macd_last,
                          double *signal_prev, double *signal_last);

// 3DS init/exit
static int init_3ds_services(void);
static void exit_3ds_services(void);

// --- Main ---
int main(int argc, char** argv) {
    if (init_3ds_services() != 0) {
        printf("3DS init failed. Press START to exit.\n");
        while (aptMainLoop()) {
            hidScanInput();
            if (hidKeysDown() & KEY_START) break;
            gspWaitForVBlank();
        }
        exit_3ds_services();
        return 1;
    }

    atexit(cleanup_on_exit);
    curl_global_init(CURL_GLOBAL_DEFAULT);

#if USE_ROMFS_CA_BUNDLE
    romfsInit();
#endif

    setup_dashboard_ui();
    printf("\nPress START to exit.\n");
    fflush(stdout);

    // Main fetch loop (frame-agnostic)
    while (aptMainLoop() && !g_should_quit) {
        hidScanInput();
        u32 kDown = hidKeysDown();
        if (kDown & KEY_START) {
            g_should_quit = true;
            break;
        }

        update_timestamp();

        char url1d[512];
        for (int i = 0; i < num_tickers && !g_should_quit; i++) {
            int current_row = DATA_START_ROW + i;
            snprintf(url1d, sizeof(url1d), API_URL_1D_FORMAT, tickers[i]);

            char *json_1d = fetch_url(url1d);
            if (json_1d) {
                parse_and_print_stock_data(json_1d, current_row);
                free(json_1d);
            } else {
                print_error_on_line(tickers[i], "Failed to fetch 1d data", current_row);
            }

            // Make sure screen stays responsive
            gfxFlushBuffers();
            gfxSwapBuffers();
            gspWaitForVBlank();

            // Allow quick exit during heavy loops
            hidScanInput();
            if (hidKeysDown() & KEY_START) { g_should_quit = true; break; }
        }

        if (g_should_quit) break;

        run_countdown();

        gfxFlushBuffers();
        gfxSwapBuffers();
        gspWaitForVBlank();
    }

    curl_global_cleanup();
#if USE_ROMFS_CA_BUNDLE
    romfsExit();
#endif
    exit_3ds_services();
    show_cursor();
    return 0;
}

// --- 3DS helpers ---
static int init_3ds_services(void) {
    gfxInitDefault();
    consoleInit(GFX_TOP, NULL);

    // Init network (SOC)
    g_socBuf = (u32*)memalign(SOC_ALIGN, SOC_BUFFERSIZE);
    if (!g_socBuf) {
        printf("memalign(SOC) failed\n");
        return -1;
    }
    Result rc = socInit(g_socBuf, SOC_BUFFERSIZE);
    if (R_FAILED(rc)) {
        printf("socInit failed: 0x%08lX\n", rc);
        return -2;
    }
    return 0;
}

static void exit_3ds_services(void) {
    if (g_socBuf) {
        socExit();
        free(g_socBuf);
        g_socBuf = NULL;
    }
    gfxExit();
}

// --- HTTP ---
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

#if USE_ROMFS_CA_BUNDLE
        curl_easy_setopt(curl_handle, CURLOPT_SSL_VERIFYPEER, 1L);
        curl_easy_setopt(curl_handle, CURLOPT_SSL_VERIFYHOST, 2L);
        curl_easy_setopt(curl_handle, CURLOPT_CAINFO, CA_BUNDLE_PATH);
#else
        // Insecure fallback (use only if you can’t ship a CA bundle)
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

// --- JSON + MACD ---
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
        free(ema_fast); free(ema_slow); free(macd_line); return 0;
    }

    double *signal_line = (double *)malloc(sizeof(double) * macd_count);
    if (!signal_line) { free(ema_fast); free(ema_slow); free(macd_line); return 0; }

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

// --- UI / Dashboard ---
void parse_and_print_stock_data(const char *json_1d, int row) {
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

    // Session series update
    int ticker_index = row - DATA_START_ROW;
    if (ticker_index < 0 || ticker_index >= num_tickers) ticker_index = 0;
    Series* s = &g_series[ticker_index];
    series_append(s, last_close_1d);

    double macd_prev = 0.0, macd_last = 0.0, signal_prev = 0.0, signal_last = 0.0;
    int has_macd = compute_macd_last_two(s->data, s->n, &macd_prev, &macd_last, &signal_prev, &signal_last);

    double macd_pct = 0.0, signal_pct = 0.0;
    if (has_macd && last_close_1d != 0.0) {
        macd_pct = (macd_last / last_close_1d) * 100.0;
        signal_pct = (signal_last / last_close_1d) * 100.0;
    }

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

    double prev_price_seen = (g_prev_price ? g_prev_price[ticker_index] : NAN);
    const char* price_bg = "";
    if (!isnan(prev_price_seen)) {
        if (last_close_1d > prev_price_seen) price_bg = BGRN;
        else if (last_close_1d < prev_price_seen) price_bg = BRED;
    }

    const char* color_change = (change_1d >= 0) ? KGRN : KRED;
    const char* color_pct = (pct_change_1d >= 0) ? KGRN : KRED;
    const char* color_macd = (has_macd && macd_pct >= 0) ? KGRN : KRED;
    const char* color_signal = (has_macd && signal_pct >= 0) ? KGRN : KRED;

    char macd_buf[20], sig_buf[20];
    if (has_macd) {
        // Fixed: print % with %%
        snprintf(macd_buf, sizeof(macd_buf), "%+6.3f%%", macd_pct);
        snprintf(sig_buf, sizeof(sig_buf), "%+6.3f%%", signal_pct);
    } else {
        snprintf(macd_buf, sizeof(macd_buf), "%6s", "N/A");
        snprintf(sig_buf, sizeof(sig_buf), "%6s", "N/A");
    }

    // Print row (ANSI cursor position + format preserved)
    printf("\033[%d;1H", row);
    printf("%s%-10s%s | %s%10.2f%s | %s%+10.2f%s | %s%+6.2f%%%s | %s%6s%s | %s%6s%s\033[K",
           ticker_bg_prefix, symbol, ticker_bg_suffix,
           price_bg, last_close_1d, KNRM,
           color_change, change_1d, KNRM,
           color_pct, pct_change_1d, KNRM,
           color_macd, macd_buf, KNRM,
           color_signal, sig_buf, KNRM);
    fflush(stdout);

    if (g_prev_price) g_prev_price[ticker_index] = last_close_1d;

    if (closes1) free(closes1);
    cJSON_Delete(root1);
}

void print_error_on_line(const char* ticker, const char* error_msg, int row) {
    printf("\033[%d;1H", row);
    printf("%-10s | %s%-80s%s\033[K", ticker, KRED, error_msg, KNRM);
    fflush(stdout);
}

void setup_dashboard_ui() {
    hide_cursor();
    // Clear + home (ANSI)
    printf("\033[2J\033[H");

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

    printf("--- C Terminal Stock Dashboard (1d only | MACD from live session polls) ---\n");
    printf("\n"); // timestamp line
    printf("\n");

    // Headers
    printf("%-10s | %10s | %10s | %7s | %6s | %6s\n",
           "Tkr", "Price", "Chg", "%Chg", "MACD", "Sig");
    printf("----------------------------------------------------------------------------------------------------\n");

    // Placeholders
    for (int i = 0; i < num_tickers; i++) {
        int row = DATA_START_ROW + i;
        printf("\033[%d;1H", row);
        printf("%-10s | %sFetching 1d data...%s\033[K", tickers[i], KYEL, KNRM);
    }
    fflush(stdout);
}

void update_timestamp() {
    time_t t = time(NULL);
    struct tm *tmv = localtime(&t);
    char time_str[64];
    if (tmv) strftime(time_str, sizeof(time_str), "%Y-%m-%d %H:%M:%S", tmv);
    else snprintf(time_str, sizeof(time_str), "unknown time");

    printf("\033[2;1H");
    printf("Last updated: %s\033[K", time_str);
    fflush(stdout);
}

void run_countdown() {
    int update_line = DATA_START_ROW + num_tickers + 1;

    for (int i = UPDATE_INTERVAL_SECONDS; i > 0; i--) {
        printf("\033[%d;1H", update_line);
        printf("\033[KUpdating in %2d seconds...", i);
        fflush(stdout);

        // Keep UI responsive and allow quitting
        for (int f = 0; f < 60; ++f) { // ~1s at 60 Hz
            hidScanInput();
            if (hidKeysDown() & KEY_START) { g_should_quit = true; return; }
            gfxFlushBuffers();
            gfxSwapBuffers();
            gspWaitForVBlank();
        }
        svcSleepThread(1000000LL * 1000LL); // ~1ms granularity, combined with VBlank loop above
    }
    printf("\033[%d;1H\033[KUpdating now...           ", update_line);
    fflush(stdout);
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
