#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <curl/curl.h>
#include "cJSON.h"
#include <3ds.h>
#include <malloc.h>

// --- Configuration ---
#define UPDATE_INTERVAL_SECONDS 15
#define USER_AGENT "Mozilla/5.0 (Nintendo 3DS; Mobile) AppleWebKit/537.36"
#define API_URL_FORMAT "https://query1.finance.yahoo.com/v8/finance/chart/%s?range=1y&interval=1d&includePrePost=false"
#define DATA_START_ROW 6

// MACD parameters (standard)
#define FAST_EMA_PERIOD 12
#define SLOW_EMA_PERIOD 26
#define SIGNAL_EMA_PERIOD 9

// Reduced tickers for smaller 3DS screen
const char *tickers[] = {"AAPL", "GOOGL", "TSLA", "MSFT", "NVDA"};
const int num_tickers = sizeof(tickers) / sizeof(tickers[0]);

// --- Color Definitions for 3DS Console ---
// 3DS console supports ANSI colors
#define KNRM  "\x1B[0m"
#define KRED  "\x1B[31m"
#define KGRN  "\x1B[32m"
#define KYEL  "\x1B[33m"
#define KBLU  "\x1B[34m"
#define KMAG  "\x1B[35m"
#define KCYN  "\x1B[36m"
#define KWHT  "\x1B[37m"

// --- Struct to hold HTTP response data ---
typedef struct {
    char *memory;
    size_t size;
} MemoryStruct;

// --- Global Variables ---
static bool running = true;
static u32 *SOC_buffer = NULL;

// --- Function Prototypes ---
static size_t write_callback(void *contents, size_t size, size_t nmemb, void *userp);
char* fetch_url(const char *url);
void parse_and_print_stock_data(const char *json_string, int row);
void setup_dashboard_ui();
void update_timestamp();
void run_countdown();
void print_error_on_line(const char* ticker, const char* error_msg, int row);
void hide_cursor();
void show_cursor();
void cleanup_on_exit();
bool check_exit_button();
int init_network();
void deinit_network();

// MACD helpers
int extract_daily_closes(cJSON *result, double **out_closes, int *out_n);
void compute_ema_series(const double *data, int n, int period, double *out);
int compute_macd_percent(const double *closes, int n, double *macd_pct, double *signal_pct);

// --- Main Application ---
int main(void) {
    // Initialize services
    gfxInitDefault();
    consoleInit(GFX_TOP, NULL); // Use top screen for output
    
    // Initialize network
    if (init_network() != 0) {
        printf("Failed to initialize network!\n");
        printf("Press START to exit.\n");
        while (aptMainLoop()) {
            hidScanInput();
            if (hidKeysDown() & KEY_START) break;
            gfxFlushBuffers();
            gfxSwapBuffers();
            gspWaitForVBlank();
        }
        gfxExit();
        return -1;
    }

    // Initialize libcurl
    curl_global_init(CURL_GLOBAL_ALL);

    // Setup dashboard
    setup_dashboard_ui();

    while (running && aptMainLoop()) {
        // Check for exit button
        if (check_exit_button()) {
            break;
        }

        // Update timestamp
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
            
            // Flush to screen
            gfxFlushBuffers();
            gfxSwapBuffers();
        }

        // Run countdown
        run_countdown();
    }

    // Cleanup
    cleanup_on_exit();
    curl_global_cleanup();
    deinit_network();
    gfxExit();
    
    return 0;
}

// --- Network Functions ---
int init_network() {
    Result ret = 0;
    SOC_buffer = (u32*)memalign(0x1000, 0x100000);
    if (SOC_buffer == NULL) {
        return -1;
    }
    
    ret = socInit(SOC_buffer, 0x100000);
    if (R_FAILED(ret)) {
        free(SOC_buffer);
        SOC_buffer = NULL;
        return -1;
    }
    
    return 0;
}

void deinit_network() {
    if (SOC_buffer != NULL) {
        socExit();
        free(SOC_buffer);
        SOC_buffer = NULL;
    }
}

bool check_exit_button() {
    hidScanInput();
    u32 kDown = hidKeysDown();
    return (kDown & KEY_START);
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
        curl_easy_setopt(curl_handle, CURLOPT_SSL_VERIFYPEER, 0L); // May be needed on 3DS
        curl_easy_setopt(curl_handle, CURLOPT_SSL_VERIFYHOST, 0L);

        res = curl_easy_perform(curl_handle);

        if (res != CURLE_OK) {
            fprintf(stderr, "curl error: %s\n", curl_easy_strerror(res));
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

    const char* color_change = (change >= 0) ? KGRN : KRED;
    const char* color_pct = (percent_change >= 0) ? KGRN : KRED;
    const char* color_macd = (has_macd && macd_pct >= 0) ? KGRN : KRED;
    const char* color_signal = (has_macd && signal_pct >= 0) ? KGRN : KRED;

    char macd_buf[16], sig_buf[16];
    if (has_macd) {
        snprintf(macd_buf, sizeof(macd_buf), "%+7.3f%%", macd_pct);
        snprintf(sig_buf, sizeof(sig_buf), "%+7.3f%%", signal_pct);
    } else {
        snprintf(macd_buf, sizeof(macd_buf), "%7s", "N/A");
        snprintf(sig_buf, sizeof(sig_buf), "%7s", "N/A");
    }

    printf("\033[%d;1H", row);

    char change_sign = (change >= 0) ? '+' : '-';
    char pct_sign = (percent_change >= 0) ? '+' : '-';

    // Adjusted for 3DS screen width (~50 chars on top screen)
    printf("%-6s|%s%7.2f%s|%s%c%6.2f%s|%s%c%6.2f%%%s\033[K",
           symbol,
           KBLU, last_close, KNRM,
           color_change, change_sign, (change >= 0 ? change : -change), KNRM,
           color_pct, pct_sign, (percent_change >= 0 ? percent_change : -percent_change), KNRM);
    fflush(stdout);

    free(closes);
    cJSON_Delete(root);
}

void print_error_on_line(const char* ticker, const char* error_msg, int row) {
    printf("\033[%d;1H", row);
    printf("%-6s | %s%-40s%s\033[K", ticker, KRED, error_msg, KNRM);
    fflush(stdout);
}

// --- UI Functions ---
void setup_dashboard_ui() {
    hide_cursor();
    printf("\033[2J\033[H");

    printf("%s=== 3DS Stock Dashboard ===%s\n", KCYN, KNRM);
    printf("\n");
    printf("%sPress START to exit%s\n", KYEL, KNRM);
    printf("%-6s | %7s | %7s | %8s\n", "Ticker", "Price", "Change", "% Change");
    printf("------------------------------------------\n");

    for (int i = 0; i < num_tickers; i++) {
        int row = DATA_START_ROW + i;
        printf("\033[%d;1H", row);
        printf("%-6s | %sFetching...%s\033[K", tickers[i], KYEL, KNRM);
    }
    fflush(stdout);
    gfxFlushBuffers();
    gfxSwapBuffers();
}

void update_timestamp() {
    time_t t = time(NULL);
    struct tm *tm = localtime(&t);
    char time_str[64];
    strftime(time_str, sizeof(time_str), "%Y-%m-%d %H:%M:%S", tm);

    printf("\033[2;1H");
    printf("Updated: %s\033[K", time_str);
    fflush(stdout);
}

void run_countdown() {
    int update_line = DATA_START_ROW + num_tickers + 1;

    for (int i = UPDATE_INTERVAL_SECONDS; i > 0; i--) {
        // Check for exit during countdown
        if (check_exit_button()) {
            running = false;
            return;
        }

        printf("\033[%d;1H", update_line);
        printf("\033[KUpdating in %2d sec...", i);
        fflush(stdout);
        gfxFlushBuffers();
        gfxSwapBuffers();
        
        // Sleep for 1 second (using 3DS timing)
        svcSleepThread(1000000000LL); // 1 second in nanoseconds
    }
    
    printf("\033[%d;1H\033[KUpdating now...     ", update_line);
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
}
