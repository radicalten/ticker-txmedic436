#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h> // For sleep()
#include <time.h>   // For timestamp
#include <curl/curl.h>
#include "cJSON.h"

// --- Configuration ---
#define UPDATE_INTERVAL_SECONDS 15
#define USER_AGENT "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/98.0.4758.102 Safari/537.36"
// Modified URL to fetch 3 months of daily historical data for MACD calculation
#define API_URL_FORMAT "https://query1.finance.yahoo.com/v8/finance/chart/%s?range=3mo&interval=1d"
#define DATA_START_ROW 6 // The row number where the first stock ticker will be printed

// Add or remove stock tickers here
const char *tickers[] = {"AAPL", "GOOGL", "TSLA", "MSFT", "NVDA", "BTC-USD", "ETH-USD"};
const int num_tickers = sizeof(tickers) / sizeof(tickers[0]);

// --- Color Definitions for Terminal ---
#define KNRM  "\x1B[0m"  // Normal
#define KRED  "\x1B[31m"  // Red
#define KGRN  "\x1B[32m"  // Green
#define KYEL  "\x1B[33m"  // Yellow
#define KBLU  "\x1B[34m"  // Blue (Added for price)

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
void calculate_ema_series(const double* data, int data_size, int period, double* ema_out);
void calculate_macd(const double* close_prices, int num_prices, double* out_last_macd, double* out_last_signal);


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

// --- MACD Calculation Functions ---

/**
 * @brief Calculates a series of Exponential Moving Averages (EMA).
 * @param data Input data array (e.g., closing prices).
 * @param data_size The number of elements in the data array.
 * @param period The period for the EMA (e.g., 12, 26).
 * @param ema_out Output array to store the calculated EMA values. Must be pre-allocated.
 */
void calculate_ema_series(const double* data, int data_size, int period, double* ema_out) {
    if (data_size < period) return; // Not enough data to calculate

    double multiplier = 2.0 / (period + 1.0);
    double sma = 0.0;

    // 1. Calculate the initial SMA for the first EMA value
    for (int i = 0; i < period; i++) {
        sma += data[i];
    }
    ema_out[period - 1] = sma / period;

    // 2. Calculate the rest of the EMAs
    for (int i = period; i < data_size; i++) {
        ema_out[i] = (data[i] - ema_out[i - 1]) * multiplier + ema_out[i - 1];
    }
}

/**
 * @brief Calculates the final MACD and Signal line values from a series of closing prices.
 * @param close_prices Array of historical closing prices.
 * @param num_prices The number of prices in the array.
 * @param out_last_macd Pointer to store the last calculated MACD value.
 * @param out_last_signal Pointer to store the last calculated Signal line value.
 */
void calculate_macd(const double* close_prices, int num_prices, double* out_last_macd, double* out_last_signal) {
    const int period_fast = 12;
    const int period_slow = 26;
    const int period_signal = 9;

    // Check if there's enough data for a meaningful calculation
    if (num_prices < period_slow + period_signal) {
        *out_last_macd = 0.0;
        *out_last_signal = 0.0;
        return;
    }

    // Allocate memory for intermediate calculations, initialized to zero
    double* ema_fast = (double*)calloc(num_prices, sizeof(double));
    double* ema_slow = (double*)calloc(num_prices, sizeof(double));
    double* macd_line = (double*)calloc(num_prices, sizeof(double));
    if (!ema_fast || !ema_slow || !macd_line) {
        // Memory allocation failed
        free(ema_fast); free(ema_slow); free(macd_line);
        *out_last_macd = 0.0; *out_last_signal = 0.0;
        return;
    }
    
    // 1. Calculate the fast (12) and slow (26) EMAs of the closing price
    calculate_ema_series(close_prices, num_prices, period_fast, ema_fast);
    calculate_ema_series(close_prices, num_prices, period_slow, ema_slow);
    
    // 2. Calculate the MACD line (fast EMA - slow EMA)
    int macd_count = 0;
    for (int i = period_slow - 1; i < num_prices; i++) {
        macd_line[macd_count++] = ema_fast[i] - ema_slow[i];
    }

    // 3. Calculate the Signal line (9-period EMA of the MACD line)
    double* signal_line = (double*)calloc(macd_count, sizeof(double));
    if (!signal_line) {
        // Memory allocation failed
        free(ema_fast); free(ema_slow); free(macd_line);
        *out_last_macd = 0.0; *out_last_signal = 0.0;
        return;
    }
    calculate_ema_series(macd_line, macd_count, period_signal, signal_line);

    // 4. Store the last valid values
    *out_last_macd = macd_line[macd_count - 1];
    *out_last_signal = signal_line[macd_count - 1];

    // Clean up allocated memory
    free(ema_fast);
    free(ema_slow);
    free(macd_line);
    free(signal_line);
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
 * @brief Parses JSON and prints updated stock data on a specific row.
 * @param json_string The JSON data received from the API.
 * @param row The terminal row to print the output on.
 */
void parse_and_print_stock_data(const char *json_string, int row) {
    cJSON *root = cJSON_Parse(json_string);
    if (root == NULL) {
        print_error_on_line("JSON", "Parse Error", row);
        return;
    }

    // Navigate the JSON: root -> chart -> result[0]
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
    
    // --- Extract current market data ---
    const char *symbol = cJSON_GetObjectItemCaseSensitive(meta, "symbol")->valuestring;
    double price = cJSON_GetObjectItemCaseSensitive(meta, "regularMarketPrice")->valuedouble;
    double prev_close = cJSON_GetObjectItemCaseSensitive(meta, "chartPreviousClose")->valuedouble;
    
    double change = price - prev_close;
    double percent_change = (prev_close == 0) ? 0 : (change / prev_close) * 100.0;
    
    const char* color = (change >= 0) ? KGRN : KRED;
    char sign = (change >= 0) ? '+' : '-';

    // --- Extract historical data and calculate MACD ---
    double macd_line = 0.0, signal_line = 0.0;
    double std_macd = 0.0, std_signal = 0.0;

    cJSON *indicators = cJSON_GetObjectItemCaseSensitive(result, "indicators");
    cJSON *quote_array = cJSON_GetObjectItemCaseSensitive(indicators, "quote");
    if(cJSON_IsArray(quote_array) && cJSON_GetArraySize(quote_array) > 0) {
        cJSON *quote = cJSON_GetArrayItem(quote_array, 0);
        cJSON *close_prices_json = cJSON_GetObjectItemCaseSensitive(quote, "close");

        if (cJSON_IsArray(close_prices_json)) {
            int num_prices_raw = cJSON_GetArraySize(close_prices_json);
            double* close_prices = malloc(num_prices_raw * sizeof(double));
            int num_prices_valid = 0;

            // Extract valid (non-null) close prices into a double array
            for (int i = 0; i < num_prices_raw; i++) {
                cJSON *price_item = cJSON_GetArrayItem(close_prices_json, i);
                if (cJSON_IsNumber(price_item)) {
                    close_prices[num_prices_valid++] = price_item->valuedouble;
                }
            }
            
            // Calculate MACD with the valid historical prices
            if (num_prices_valid > 0) {
                 calculate_macd(close_prices, num_prices_valid, &macd_line, &signal_line);

                 // Standardize MACD and Signal as a percentage of the current price
                 if (price > 0.0001) { // Avoid division by zero
                     std_macd = (macd_line / price) * 100.0;
                     std_signal = (signal_line / price) * 100.0;
                 }
            }
            free(close_prices);
        }
    }

    // Determine color for MACD histogram (MACD - Signal)
    const char* macd_color = (std_macd >= std_signal) ? KGRN : KRED;

    // ANSI: Move cursor to the start of the specified row
    printf("\033[%d;1H", row);

    // Print the formatted data row, now with MACD columns.
    // ANSI: \033[K clears from the cursor to the end of the line.
    printf("%-10s | %s%10.2f%s | %s%c%9.2f%s | %s%c%10.2f%%%s | %s%+8.3f%%%s | %s%+8.3f%%%s \033[K\n",
           symbol,
           KBLU, price, KNRM,
           color, sign, (change >= 0 ? change : -change), KNRM,
           color, sign, (percent_change >= 0 ? percent_change : -percent_change), KNRM,
           macd_color, std_macd, KNRM,   // Standardized MACD
           KYEL, std_signal, KNRM);     // Standardized Signal

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
    // Print formatted error and clear rest of the line
    printf("%-10s | %s%-60s%s\033[K\n", ticker, KRED, error_msg, KNRM);
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

    // Print static headers with new MACD columns
    printf("%-10s | %11s | %11s | %13s | %10s | %10s\n", "Ticker", "Price", "Change", "% Change", "MACD %", "Signal %");
    printf("------------------------------------------------------------------------------------------\n");
    
    // Print initial placeholder text for each ticker
    for (int i = 0; i < num_tickers; i++) {
        printf("%-10s | %sFetching...%s\n", tickers[i], KYEL, KNRM);
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
    // ANSI: \033[K clears from the cursor to the end of the line
    printf("Last updated: %s\033[K\n", time_str);
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
        // ANSI: Clear line and print countdown
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
