#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <curl/curl.h>
#include "cJSON.h"

// 3DS-specific headers
#include <3ds.h>
#include <arpa/inet.h>
#include <sys/socket.h>

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

// --- Color Definitions (libctru console supports these ANSI codes) ---
#define KNRM  "\x1B[0m"  // Normal
#define KRED  "\x1B[31m" // Red
#define KGRN  "\x1B[32m" // Green
#define KYEL  "\x1B[33m" // Yellow
#define KBLU  "\x1B[34m" // Blue (Added for price)

// --- Struct to hold HTTP response data ---
typedef struct {
    char *memory;
    size_t size;
} MemoryStruct;

// --- Function Prototypes ---
// Network and Core App Logic
bool init_network(void);
void deinit_network(void);
void update_data(void);
void update_countdown(time_t last_update);

// Original Helper Functions (mostly unchanged)
static size_t write_callback(void *contents, size_t size, size_t nmemb, void *userp);
char* fetch_url(const char *url);
void parse_and_print_stock_data(const char *json_string, int row);
void setup_dashboard_ui();
void update_timestamp();
void print_error_on_line(const char* ticker, const char* error_msg, int row);

// MACD computation functions (unchanged)
int extract_daily_closes(cJSON *result, double **out_closes, int *out_n);
void compute_ema_series(const double *data, int n, int period, double *out);
int compute_macd_percent(const double *closes, int n, double *macd_pct, double *signal_pct);


// --- Main Application for Nintendo 3DS ---
int main(int argc, char **argv) {
    // Initialize 3DS services
    gfxInitDefault();
    // Use the top screen for our console
    consoleInit(GFX_TOP, NULL);

    printf("Initializing network...\n");
    if (!init_network()) {
        printf(KRED "Failed to initialize network!\n" KNRM);
        printf("Please ensure you are connected to the internet.\n");
        printf("Press START to exit.\n");
        while (aptMainLoop()) {
            hidScanInput();
            if (hidKeysDown() & KEY_START) break;
            gfxFlushBuffers(); gfxSwapBuffers();
        }
    } else {
        printf("Network initialized successfully.\n");

        // Initialize libcurl
        curl_global_init(CURL_GLOBAL_ALL);

        // Initial, one-time setup of the dashboard UI
        setup_dashboard_ui();

        time_t last_update = 0;
        time_t current_time = time(NULL);

        // Main 3DS application loop
        while (aptMainLoop()) {
            hidScanInput();
            u32 kDown = hidKeysDown();
            if (kDown & KEY_START) {
                break; // Exit loop
            }

            current_time = time(NULL);

            // Check if it's time to perform an update
            if (current_time - last_update >= UPDATE_INTERVAL_SECONDS) {
                update_data();
                last_update = time(NULL); // Reset timer after update
            } else {
                // Otherwise, just update the countdown timer
                update_countdown(last_update);
            }

            // Flush and swap framebuffers
            gfxFlushBuffers();
            gfxSwapBuffers();
            gspWaitForVBlank(); // Wait for VBlank to save CPU/battery
        }

        // Cleanup
        curl_global_cleanup();
        deinit_network();
    }

    // De-initialize 3DS services
    consoleExit(NULL);
    gfxExit();
    return 0;
}


// --- 3DS Specific Helper Functions ---

#define SOC_ALIGN       0x1000
#define SOC_BUFFERSIZE  0x100000

static u32 *soc_buffer = NULL;

/**
 * @brief Initializes the 3DS network stack (AC and SOC).
 */
bool init_network(void) {
    acInit();
    soc_buffer = (u32*)memalign(SOC_ALIGN, SOC_BUFFERSIZE);
    if(soc_buffer == NULL) {
        acExit();
        return false;
    }
    if (socInit(soc_buffer, SOC_BUFFERSIZE) != 0) {
        free(soc_buffer);
        acExit();
        return false;
    }
    return true;
}

/**
 * @brief De-initializes the 3DS network stack.
 */
void deinit_network(void) {
    socExit();
    if(soc_buffer) free(soc_buffer);
    acExit();
}

/**
 * @brief Main function to fetch and display data for all tickers.
 */
void update_data(void) {
    int update_line = DATA_START_ROW + num_tickers + 1;
    printf("\x1b[%d;1H\x1b[KUpdating now...", update_line);
    fflush(stdout);

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
}

/**
 * @brief Displays the live countdown timer at the bottom of the dashboard.
 */
void update_countdown(time_t last_update) {
    int update_line = DATA_START_ROW + num_tickers + 1;
    int seconds_remaining = UPDATE_INTERVAL_SECONDS - (time(NULL) - last_update);
    if (seconds_remaining < 0) seconds_remaining = 0;

    printf("\x1b[%d;1H", update_line);
    printf("\x1b[KUpdating in %2d seconds...", seconds_remaining);
    fflush(stdout);
}

// --- Original Helper Functions ---
// (These are identical to your original file and require no changes)

static size_t write_callback(void *contents, size_t size, size_t nmemb, void *userp) {
    size_t realsize = size * nmemb;
    MemoryStruct *mem = (MemoryStruct *)userp;
    char *ptr = realloc(mem->memory, mem->size + realsize + 1);
    if (ptr == NULL) { return 0; }
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
        // On 3DS, it's good to set a timeout
        curl_easy_setopt(curl_handle, CURLOPT_TIMEOUT, 10L); // 10 second timeout

        res = curl_easy_perform(curl_handle);

        if (res != CURLE_OK) {
            // Can't use stderr on 3DS console, so print to stdout
            printf("\x1b[%d;1H\x1b[Kcurl_easy_perform() failed: %s", DATA_START_ROW + num_tickers + 2, curl_easy_strerror(res));
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
    if (n == 0) { free(closes); return 0; }
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
    for (int i = 0; i < period; i++) { sum += data[i]; }
    double ema = sum / period;
    for (int i = 0; i < period - 1; i++) { out[i] = 0.0; }
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
    for (int i = 0; i < macd_count; i++) { macd_line[i] = ema_fast[macd_start + i] - ema_slow[macd_start + i]; }
    if (macd_count < SIGNAL_EMA_PERIOD) { free(ema_fast); free(ema_slow); free(macd_line); return 0; }
    double *signal_line = (double *)malloc(sizeof(double) * macd_count);
    if (!signal_line) { free(ema_fast); free(ema_slow); free(macd_line); return 0; }
    compute_ema_series(macd_line, macd_count, SIGNAL_EMA_PERIOD, signal_line);
    double last_close = closes[n - 1];
    if (last_close == 0.0) { free(ema_fast); free(ema_slow); free(macd_line); free(signal_line); return 0; }
    *macd_pct = (macd_line[macd_count - 1] / last_close) * 100.0;
    *signal_pct = (signal_line[macd_count - 1] / last_close) * 100.0;
    free(ema_fast); free(ema_slow); free(macd_line); free(signal_line);
    return 1;
}

void parse_and_print_stock_data(const char *json_string, int row) {
    cJSON *root = cJSON_Parse(json_string);
    if (root == NULL) { print_error_on_line("JSON", "Parse Error", row); return; }
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
    const char *symbol = cJSON_GetObjectItemCaseSensitive(meta, "symbol")->valuestring;
    double *closes = NULL; int n = 0;
    if (!extract_daily_closes(result, &closes, &n) || n < 2) {
        print_error_on_line(symbol, "Insufficient daily data", row);
        if (closes) free(closes); cJSON_Delete(root); return;
    }
    double last_close = closes[n - 1]; double prev_close = closes[n - 2];
    double change = last_close - prev_close;
    double percent_change = (prev_close != 0.0) ? (change / prev_close) * 100.0 : 0.0;
    double macd_pct = 0.0, signal_pct = 0.0;
    int has_macd = compute_macd_percent(closes, n, &macd_pct, &signal_pct);
    const char* color_change = (change >= 0) ? KGRN : KRED;
    const char* color_pct = (percent_change >= 0) ? KGRN : KRED;
    const char* color_macd = (has_macd && macd_pct >= 0) ? KGRN : KRED;
    const char* color_signal = (has_macd && signal_pct >= 0) ? KGRN : KRED;
    char macd_buf[16], sig_buf[16];
    if (has_macd) { snprintf(macd_buf, sizeof(macd_buf), "%+8.3f%%", macd_pct); snprintf(sig_buf, sizeof(sig_buf), "%+8.3f%%", signal_pct); }
    else { snprintf(macd_buf, sizeof(macd_buf), "%8s", "N/A"); snprintf(sig_buf, sizeof(sig_buf), "%8s", "N/A"); }
    printf("\x1b[%d;1H", row); // Use \x1b for hex escape
    char change_sign = (change >= 0) ? '+' : '-';
    char pct_sign = (percent_change >= 0) ? '+' : '-';
    printf("%-7s|%s%8.2f%s|%s%c%7.2f%s|%s%c%7.2f%%%s|%s%8s%s|%s%8s%s\x1b[K", // Adjusted spacing for 3DS screen
           symbol, KBLU, last_close, KNRM, color_change, change_sign,
           (change >= 0 ? change : -change), KNRM, color_pct, pct_sign,
           (percent_change >= 0 ? percent_change : -percent_change), KNRM,
           color_macd, macd_buf, KNRM, color_signal, sig_buf, KNRM);
    fflush(stdout);
    free(closes);
    cJSON_Delete(root);
}

void print_error_on_line(const char* ticker, const char* error_msg, int row) {
    printf("\x1b[%d;1H", row);
    printf("%-7s| %s%-40.40s%s\x1b[K", ticker, KRED, error_msg, KNRM); // Adjusted spacing
    fflush(stdout);
}

void setup_dashboard_ui() {
    consoleClear();
    printf(KYEL "--- 3DS Terminal Stock Dashboard ---\n" KNRM);
    printf("\n"); // Placeholder for timestamp
    printf("Press START to exit.\n\n");
    printf(KBLU "%-7s|%9s|%9s|%9s|%9s|%9s\n" KNRM,
           "Ticker", "Price", "Change", "%Change", "MACD%", "Signal%");
    printf("--------------------------------------------------\n"); // Adjusted for 3DS screen
    for (int i = 0; i < num_tickers; i++) {
        int row = DATA_START_ROW + i;
        printf("\x1b[%d;1H", row);
        printf("%-7s| %sFetching...%s\x1b[K", tickers[i], KYEL, KNRM);
    }
    fflush(stdout);
}

void update_timestamp() {
    time_t t = time(NULL);
    struct tm *tm = localtime(&t);
    char time_str[64];
    strftime(time_str, sizeof(time_str), "%Y-%m-%d %H:%M:%S", tm);
    printf("\x1b[2;1H"); // Move cursor to row 2
    printf("Last updated: %s\x1b[K", time_str);
    fflush(stdout);
}
