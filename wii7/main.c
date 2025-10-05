#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h> // For sleep()
#include <time.h>   // For timestamp
#include <curl/curl.h>
#include "cJSON.h"

// Wii-specific init (guarded)
#if defined(GEKKO) && defined(HW_RVL)
#include <gccore.h>
#include <wiiuse/wpad.h>
#include <wiisocket.h>

static void *xfb = NULL;
static GXRModeObj *rmode = NULL;

static void wii_video_init(void) {
    VIDEO_Init();
    WPAD_Init();

    rmode = VIDEO_GetPreferredMode(NULL);
    xfb = MEM_K0_TO_K1(SYS_AllocateFramebuffer(rmode));
    console_init(xfb, 0, 0, rmode->fbWidth, rmode->xfbHeight,
                 rmode->fbWidth * VI_DISPLAY_PIX_SZ);

    VIDEO_Configure(rmode);
    VIDEO_SetNextFramebuffer(xfb);
    VIDEO_SetBlack(FALSE);
    VIDEO_Flush();
    VIDEO_WaitVSync();
    if (rmode->viTVMode & VI_NON_INTERLACE) VIDEO_WaitVSync();

    // Clear and home
    printf("\x1b[2J\x1b[H");
}
#endif

// --- Configuration ---
#define UPDATE_INTERVAL_SECONDS 30
#define USER_AGENT "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/98.0.4758.102 Safari/537.36"
// Fetch daily candles for 1 year so we can compute MACD on daily closes
#define API_URL_FORMAT "https://query1.finance.yahoo.com/v8/finance/chart/%s?range=1y&interval=1d&includePrePost=false"
#define DATA_START_ROW 6 // The row number where the first stock ticker will be printed

// MACD parameters (standard)
#define FAST_EMA_PERIOD 12
#define SLOW_EMA_PERIOD 26
#define SIGNAL_EMA_PERIOD 9

// Add or remove stock tickers here
// YahooFinance has query limit of 2000 requests/hr 15 tickers every 30s = 15*2*60=1800 queries; 22 tickers every 40s = 22*1.5*60 = 1980, 22tk@45s = 22*1.333*60=1760 queries
const char *tickers[] = {"BTC-USD", "ETH-USD", "DX-Y.NYB", "^TNX", "^SPX", "^RUA", "GC=F","HRC=F", "CL=F", "NG=F", "NVDA", "UNH", "PFE", "TGT", "TRAK"};
const int num_tickers = sizeof(tickers) / sizeof(tickers[0]);

// --- Color Definitions for Terminal ---
#define KNRM  "\x1B[0m"  // Default attributes 
//#define KBLK  "\x1B[30m" // Black
#define KRED  "\x1B[31m" // Red
#define KGRN  "\x1B[32m" // Green
#define KYEL  "\x1B[33m" // Yellow
//#define KBLU  "\x1B[34m" // Blue
//#define KMAG  "\x1B[35m" // Magenta
#define KCYN  "\x1B[36m" // Cyan
//#define KWHT  "\x1B[37m" // White

/* --- Regular Background Colors ---
#define BGBLK "\x1B[40m" // Black
#define BGRED "\x1B[41m" // Red
#define BGGRN "\x1B[42m" // Green
#define BGYEL "\x1B[43m" // Yellow
#define BGBLU "\x1B[44m" // Blue
#define BGMAG "\x1B[45m" // Magenta
#define BGCYN "\x1B[46m" // Cyan
#define BGWHT "\x1B[47m" // White

--- Bright/Bold Background Colors ---
#define BGBBLK "\x1B[100m" // Bright Black (Gray)
#define BGBRED "\x1B[101m" // Bright Red
#define BGBGRN "\x1B[102m" // Bright Green
#define BGBYEL "\x1B[103m" // Bright Yellow
#define BGBBLU "\x1B[104m" // Bright Blue
#define BGBMAG "\x1B[105m" // Bright Magenta
#define BGBCYN "\x1B[106m" // Bright Cyan
#define BGBWHT "\x1B[107m" // Bright White

--- Other Text Attributes ---
#define KBLD "\x1B[1m"   // Bold
#define KDIM "\x1B[2m"   // Dim
#define KUND "\x1B[4m"   // Underline
#define KBNK "\x1B[5m"   // Blink
#define KREV "\x1B[7m"   // Reverse/Invert
#define KHDN "\x1B[8m"   // Hidden

*/

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
void run_countdown();
void print_error_on_line(const char* ticker, const char* error_msg, int row);
void hide_cursor();
void show_cursor();
void cleanup_on_exit();

// New helpers for extracting closes and computing MACD
int extract_daily_closes(cJSON *result, double **out_closes, int *out_n);
void compute_ema_series(const double *data, int n, int period, double *out);
int compute_macd_percent(const double *closes, int n, double *macd_pct, double *signal_pct);

// --- Main Application ---
int main(void) {
    // Register cleanup function to run on exit (e.g., Ctrl+C)
    atexit(cleanup_on_exit);

       // Wii video & network init
#if defined(GEKKO) && defined(HW_RVL)
    wii_video_init();
    // The original documentation for libwiisocket showed using multiple tries to init and get an ip.
	// I don't think this is necessary, but I'm leaving it in just in case.
	int socket_init_success = -1;
	for (int attempts = 0;attempts < 3;attempts++) {
		socket_init_success = wiisocket_init();
		printf("attempt: %d wiisocket_init: %d\n", attempts, socket_init_success);
		if (socket_init_success == 0)
			break;
	}
#endif
    
    // Initialize libcurl
    curl_global_init(CURL_GLOBAL_ALL);

    // Initial, one-time setup: clear screen, hide cursor, print static layout
    setup_dashboard_ui();

    while (1) {
        // Update the timestamp at the top of the dashboard
        update_timestamp();

        char url[512];
        for (int i = 0; i < num_tickers; i++) {
            // Calculate the correct screen row for the current ticker
            int current_row = DATA_START_ROW + i;

            // Construct the URL for the current ticker
            snprintf(url, sizeof(url), API_URL_FORMAT, tickers[i]);

            // Fetch data from the URL
            char *json_response = fetch_url(url);

            if (json_response) {
                parse_and_print_stock_data(json_response, current_row);
                free(json_response); // Free the memory allocated by fetch_url
            } else {
                print_error_on_line(tickers[i], "Failed to fetch data", current_row);
            }
        }

        // Run the countdown timer at the bottom of the screen
        run_countdown();
    }

    // Cleanup libcurl (though this part is unreachable in the infinite loop)
    curl_global_cleanup();
    show_cursor(); // Restore cursor
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

		// On Wii, there's no CA store by default — disable verification or bundle a CA set.
        // Don't do this in production for sensitive data.
        #if defined(GEKKO) && defined(HW_RVL)
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
 * @brief Extracts daily close prices from the Yahoo chart JSON result object.
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
        out[i] = 0.0; // undefined, but set to 0 for safety (we won't use these)
    }
    out[period - 1] = ema;

    for (int i = period; i < n; i++) {
        ema = (data[i] - ema) * k + ema;
        out[i] = ema;
    }
}

/**
 * @brief Computes MACD% and Signal% relative to the last close.
 * @param closes Array of daily closes (chronological, oldest -> newest).
 * @param n Number of closes.
 * @param macd_pct Output MACD as percentage of last close.
 * @param signal_pct Output Signal as percentage of last close.
 * @return 1 if computed, 0 if insufficient data.
 */
int compute_macd_percent(const double *closes, int n, double *macd_pct, double *signal_pct) {
    if (!closes || n < (SLOW_EMA_PERIOD + SIGNAL_EMA_PERIOD)) return 0;

    // Compute EMAs
    double *ema_fast = (double *)malloc(sizeof(double) * n);
    double *ema_slow = (double *)malloc(sizeof(double) * n);
    if (!ema_fast || !ema_slow) {
        free(ema_fast); free(ema_slow);
        return 0;
    }
    compute_ema_series(closes, n, FAST_EMA_PERIOD, ema_fast);
    compute_ema_series(closes, n, SLOW_EMA_PERIOD, ema_slow);

    // MACD line defined from index = SLOW_EMA_PERIOD - 1
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

    // Signal line = EMA of MACD line
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
 * @brief Parses JSON and prints updated stock data on a specific row.
 *        Uses DAILY close and previous close for Price/Change/%Change.
 *        Adds MACD% and Signal% standardized by close.
 * @param json_string The JSON data received from the API.
 * @param row The terminal row to print the output on.
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

    // Extract daily closes
    double *closes = NULL;
    int n = 0;
    int ok = extract_daily_closes(result, &closes, &n);
    if (!ok || n < 2) {
        print_error_on_line(symbol, "Insufficient daily data", row);
        if (closes) free(closes);
        cJSON_Delete(root);
        return;
    }

    // Daily price, change, % change using last two daily closes
    double last_close = closes[n - 1];
    double prev_close = closes[n - 2];
    double change = last_close - prev_close;
    double percent_change = (prev_close != 0.0) ? (change / prev_close) * 100.0 : 0.0;

    // MACD% and Signal% (standardized by last_close)
    double macd_pct = 0.0, signal_pct = 0.0;
    int has_macd = compute_macd_percent(closes, n, &macd_pct, &signal_pct);

    // Determine color based on change and MACD
    const char* color_change = (change >= 0) ? KGRN : KRED;
    const char* color_pct = (percent_change >= 0) ? KGRN : KRED;
    const char* color_macd = (has_macd && macd_pct >= 0) ? KGRN : KRED;
    const char* color_signal = (has_macd && signal_pct >= 0) ? KGRN : KRED;

    char macd_buf[16], sig_buf[16];
    if (has_macd) {
        snprintf(macd_buf, sizeof(macd_buf), "%+8.2f%", macd_pct);
        snprintf(sig_buf, sizeof(sig_buf), "%+8.2f%", signal_pct);
    } else {
        snprintf(macd_buf, sizeof(macd_buf), "%8s", "N/A");
        snprintf(sig_buf, sizeof(sig_buf), "%8s", "N/A");
    }

    // ANSI: Move cursor to the start of the specified row
    printf("\033[%d;1H", row);

    // Print formatted data row. Clear to end of line with \033[K (no trailing newline)
    // Columns: Ticker | Price | Change | % Change | MACD% | Signal%
    char change_sign = (change >= 0) ? '+' : '-';
    char pct_sign = (percent_change >= 0) ? '+' : '-';

    printf("%-10s | %s%10.2f%s | %s%c%10.2f%s | %s%c%6.2f%%%s | %s%6s%s | %s%6s%s\033[K",
           symbol,
           KNRM, last_close, KNRM,
           color_change, change_sign, (change >= 0 ? change : -change), KNRM,
           color_pct, pct_sign, (percent_change >= 0 ? percent_change : -percent_change), KNRM,
           color_macd, macd_buf, KNRM,
           color_signal, sig_buf, KNRM);
    fflush(stdout);

    free(closes);
    cJSON_Delete(root);
}

/**
 * @brief Prints an error message on a specific row of the dashboard.
 * @param ticker The ticker symbol that failed.
 * @param error_msg The error message to display.
 * @param row The terminal row to print the output on.
 */
void print_error_on_line(const char* ticker, const char* error_msg, int row) {
    // ANSI: Move cursor to the start of the specified row
    printf("\033[%d;1H", row);
    // Print formatted error and clear rest of the line (no trailing newline)
    printf("%-10s | %s%-80s%s\033[K", ticker, KRED, error_msg, KNRM);
    fflush(stdout);
}


// --- UI and Terminal Control Functions ---

/**
 * @brief Performs the initial one-time setup of the dashboard UI.
 */
void setup_dashboard_ui() {
    hide_cursor();
    // ANSI: \033[2J clears the entire screen. \033[H moves cursor to top-left.
    printf("\033[2J\033[H");

    printf("--- C Terminal Stock Dashboard ---\n");
    printf("\n"); // Leave a blank line for the dynamic timestamp
    printf("\n");

    // Print static headers
    // Columns: Ticker | Price | Change | % Change | MACD% | Signal%
    printf("%-10s | %10s | %10s | %9s | %9s | %9s\n",
           "Tkr", "Price", "Chg", "%Chg", "MACD", "Sig");
    printf("----------------------------------------------------------------------------------------------------\n");

    // Print initial placeholder text for each ticker at exact rows
    for (int i = 0; i < num_tickers; i++) {
        int row = DATA_START_ROW + i;
        printf("\033[%d;1H", row);
        printf("%-10s | %sFetching daily data...%s\033[K", tickers[i], KYEL, KNRM);
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

    // ANSI: Move cursor to row 2, column 1
    printf("\033[2;1H");
    // ANSI: \033[K clears from the cursor to the end of the line (no trailing newline)
    printf("Last updated: %s\033[K", time_str);
    fflush(stdout);
}

/**
 * @brief Displays a live countdown timer at the bottom of the dashboard.
 */
void run_countdown() {
    int update_line = DATA_START_ROW + num_tickers + 1;

    for (int i = UPDATE_INTERVAL_SECONDS; i > 0; i--) {
        // ANSI: Move cursor to the update line
        printf("\033[%d;1H", update_line);
        // ANSI: Clear line and print countdown (no trailing newline)
        printf("\033[KUpdating in %2d seconds...", i);
        fflush(stdout);
        sleep(1);
    }
    // Print final "Updating now..." message
    printf("\033[%d;1H\033[KUpdating now...           ", update_line);
    fflush(stdout);
}

void hide_cursor() {
    // ANSI: Hides the terminal cursor
    printf("\033[?25l");
    fflush(stdout);
}

void show_cursor() {
    // ANSI: Shows the terminal cursor
    printf("\033[?25h");
    fflush(stdout);
}

void cleanup_on_exit() {
    // This function is called by atexit() to ensure the cursor is restored.
    show_cursor();
}
