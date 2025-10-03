#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h> // For sleep()
#include <time.h>   // For timestamp
#include <math.h>   // For MACD calculations
#include <curl/curl.h>
#include "cJSON.h"

// --- Configuration ---
#define UPDATE_INTERVAL_SECONDS 15
#define USER_AGENT "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/98.0.4758.102 Safari/537.36"
#define API_URL_FORMAT "https://query1.finance.yahoo.com/v8/finance/chart/%s?range=3mo&interval=1d"
#define DATA_START_ROW 6 // The row number where the first stock ticker will be printed

// MACD Parameters
#define MACD_FAST_PERIOD 12
#define MACD_SLOW_PERIOD 26
#define MACD_SIGNAL_PERIOD 9
#define MAX_PRICES 100

// Add or remove stock tickers here
const char *tickers[] = {"AAPL", "GOOGL", "TSLA", "MSFT", "NVDA", "BTC-USD", "ETH-USD"};
const int num_tickers = sizeof(tickers) / sizeof(tickers[0]);

// --- Color Definitions for Terminal ---
#define KNRM  "\x1B[0m"  // Normal
#define KRED  "\x1B[31m"  // Red
#define KGRN  "\x1B[32m"  // Green
#define KYEL  "\x1B[33m"  // Yellow
#define KBLU  "\x1B[34m"  // Blue
#define KMAG  "\x1B[35m"  // Magenta
#define KCYN  "\x1B[36m"  // Cyan

// --- Struct to hold HTTP response data ---
typedef struct {
    char *memory;
    size_t size;
} MemoryStruct;

// --- Struct to hold MACD values ---
typedef struct {
    double macd_line;
    double signal_line;
    double histogram;
    int valid; // Flag to indicate if MACD calculation was successful
} MACDValues;

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
double calculate_ema(double *prices, int count, int period, double prev_ema);
MACDValues calculate_macd(double *prices, int count);

// --- Main Application ---
int main(void) {
    // Register cleanup function to run on exit (e.g., Ctrl+C)
    atexit(cleanup_on_exit);

    // Initialize libcurl
    curl_global_init(CURL_GLOBAL_ALL);

    // Initial, one-time setup: clear screen, hide cursor, print static layout
    setup_dashboard_ui();

    while (1) {
        // Update the timestamp at the top of the dashboard
        update_timestamp();

        char url[256];
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
 * @brief Calculates Exponential Moving Average (EMA).
 * @param prices Array of price values.
 * @param count Number of prices in the array.
 * @param period EMA period.
 * @param prev_ema Previous EMA value (use 0 for first calculation).
 * @return Calculated EMA value.
 */
double calculate_ema(double *prices, int count, int period, double prev_ema) {
    if (count < period) return 0.0;
    
    double multiplier = 2.0 / (period + 1.0);
    
    // If no previous EMA, start with SMA
    if (prev_ema == 0.0) {
        double sum = 0.0;
        for (int i = 0; i < period; i++) {
            sum += prices[i];
        }
        prev_ema = sum / period;
        
        // Calculate EMA for remaining values
        for (int i = period; i < count; i++) {
            prev_ema = (prices[i] - prev_ema) * multiplier + prev_ema;
        }
    } else {
        // Continue from previous EMA
        for (int i = 0; i < count; i++) {
            prev_ema = (prices[i] - prev_ema) * multiplier + prev_ema;
        }
    }
    
    return prev_ema;
}

/**
 * @brief Calculates MACD (Moving Average Convergence Divergence).
 * @param prices Array of closing prices (most recent last).
 * @param count Number of prices.
 * @return MACDValues struct containing MACD line, signal line, and histogram.
 */
MACDValues calculate_macd(double *prices, int count) {
    MACDValues result = {0.0, 0.0, 0.0, 0};
    
    // Need at least 26 + 9 = 35 data points for accurate MACD
    if (count < MACD_SLOW_PERIOD + MACD_SIGNAL_PERIOD) {
        return result;
    }
    
    // Calculate 12-period EMA
    double ema_fast = calculate_ema(prices, count, MACD_FAST_PERIOD, 0.0);
    
    // Calculate 26-period EMA
    double ema_slow = calculate_ema(prices, count, MACD_SLOW_PERIOD, 0.0);
    
    // MACD Line = 12-EMA - 26-EMA
    result.macd_line = ema_fast - ema_slow;
    
    // Calculate Signal Line (9-period EMA of MACD line)
    // We need to calculate MACD for the last 9+ periods
    int macd_count = count - MACD_SLOW_PERIOD + 1;
    if (macd_count < MACD_SIGNAL_PERIOD) {
        return result;
    }
    
    double *macd_values = malloc(macd_count * sizeof(double));
    if (!macd_values) return result;
    
    // Calculate MACD values for signal line calculation
    for (int i = 0; i < macd_count; i++) {
        int end_idx = MACD_SLOW_PERIOD + i;
        double fast = calculate_ema(prices, end_idx, MACD_FAST_PERIOD, 0.0);
        double slow = calculate_ema(prices, end_idx, MACD_SLOW_PERIOD, 0.0);
        macd_values[i] = fast - slow;
    }
    
    // Calculate Signal Line (9-EMA of MACD)
    result.signal_line = calculate_ema(macd_values, macd_count, MACD_SIGNAL_PERIOD, 0.0);
    
    // MACD Histogram = MACD Line - Signal Line
    result.histogram = result.macd_line - result.signal_line;
    
    result.valid = 1;
    
    free(macd_values);
    return result;
}

/**
 * @brief Parses JSON and prints updated stock data with MACD on a specific row.
 * @param json_string The JSON data received from the API.
 * @param row The terminal row to print the output on.
 */
void parse_and_print_stock_data(const char *json_string, int row) {
    cJSON *root = cJSON_Parse(json_string);
    if (root == NULL) {
        print_error_on_line("JSON", "Parse Error", row);
        return;
    }

    // Navigate the JSON: root -> chart -> result[0] -> meta
    cJSON *chart = cJSON_GetObjectItemCaseSensitive(root, "chart");
    cJSON *result_array = cJSON_GetObjectItemCaseSensitive(chart, "result");
    if (!cJSON_IsArray(result_array) || cJSON_GetArraySize(result_array) == 0) {
        char* err_desc = "Invalid ticker or no data";
        cJSON* error_obj = cJSON_GetObjectItemCaseSensitive(chart, "error");
        if(error_obj && cJSON_GetObjectItemCaseSensitive(error_obj, "description")) {
            err_desc = cJSON_GetObjectItemCaseSensitive(error_obj, "description")->valuestring;
        }
        print_error_on_line("API Error", err_desc, row);
        cJSON_Delete(root);
        return;
    }

    cJSON *result = cJSON_GetArrayItem(result_array, 0);
    cJSON *meta = cJSON_GetObjectItemCaseSensitive(result, "meta");
    
    // Extract values
    const char *symbol = cJSON_GetObjectItemCaseSensitive(meta, "symbol")->valuestring;
    double price = cJSON_GetObjectItemCaseSensitive(meta, "regularMarketPrice")->valuedouble;
    double prev_close = cJSON_GetObjectItemCaseSensitive(meta, "chartPreviousClose")->valuedouble;
    
    double change = price - prev_close;
    double percent_change = (prev_close == 0) ? 0 : (change / prev_close) * 100.0;
    
    // Determine color based on change
    const char* color = (change >= 0) ? KGRN : KRED;
    char sign = (change >= 0) ? '+' : '-';

    // Extract historical closing prices for MACD calculation
    cJSON *indicators = cJSON_GetObjectItemCaseSensitive(result, "indicators");
    cJSON *quote_array = cJSON_GetObjectItemCaseSensitive(indicators, "quote");
    cJSON *quote = cJSON_GetArrayItem(quote_array, 0);
    cJSON *close_array = cJSON_GetObjectItemCaseSensitive(quote, "close");
    
    MACDValues macd = {0};
    
    if (cJSON_IsArray(close_array)) {
        int price_count = cJSON_GetArraySize(close_array);
        if (price_count > 0 && price_count <= MAX_PRICES) {
            double *prices = malloc(price_count * sizeof(double));
            int valid_count = 0;
            
            // Extract closing prices, skip null values
            for (int i = 0; i < price_count; i++) {
                cJSON *price_item = cJSON_GetArrayItem(close_array, i);
                if (cJSON_IsNumber(price_item)) {
                    prices[valid_count++] = price_item->valuedouble;
                }
            }
            
            if (valid_count >= MACD_SLOW_PERIOD + MACD_SIGNAL_PERIOD) {
                macd = calculate_macd(prices, valid_count);
            }
            
            free(prices);
        }
    }

    // ANSI: Move cursor to the start of the specified row
    printf("\033[%d;1H", row);

    // Print the formatted data row with MACD
    if (macd.valid) {
        const char* macd_color = (macd.histogram >= 0) ? KGRN : KRED;
        const char* trend = (macd.histogram >= 0) ? "↑" : "↓";
        
        printf("%-8s | %s%10.2f%s | %s%c%9.2f%s | %s%c%8.2f%%%s | "
               "%sMACD:%6.2f Sig:%6.2f Hist:%s%6.2f%s%s\033[K\n",
               symbol,
               KBLU, price, KNRM,
               color, sign, (change >= 0 ? change : -change), KNRM,
               color, sign, (percent_change >= 0 ? percent_change : -percent_change), KNRM,
               KCYN, macd.macd_line, macd.signal
