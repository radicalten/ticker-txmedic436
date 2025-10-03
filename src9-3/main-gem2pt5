#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h> // For sleep()
#include <time.h>   // For timestamp
#include <curl/curl.h>
#include <math.h>   // For NAN
#include "cJSON.h"

// --- Configuration ---
#define UPDATE_INTERVAL_SECONDS 15
#define USER_AGENT "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/98.0.4758.102 Safari/537.36"
// Modified URL to fetch enough historical data for MACD calculation (approx. 60-65 daily data points)
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
void calculate_ema_series(const double* data, int data_size, int period, double* ema_output);
void parse_and_print_stock_data(const char *json_string, int row);
void setup_dashboard_ui();
void update_timestamp();
void run_countdown();
void print_error_on_line(const char* ticker, const char* error_msg, int row);
void hide_cursor();
void show_cursor();
void cleanup_on_exit();

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
 * @brief Calculates a series of Exponential Moving Averages (EMA).
 * @param data Input array of prices.
 * @param data_size The size of the data array.
 * @param period The EMA period (e.g., 12, 26).
 * @param ema_output The output array to store the calculated EMA values.
 */
void calculate_ema_series(const double* data, int data_size, int period, double* ema_output) {
    if (data_size < period) {
        for (int i = 0; i < data_size; i++) ema_output[i] = NAN;
        return;
    }

    double multiplier = 2.0 / (period + 1.0);
    double sum = 0.0;

    // Calculate initial SMA for the first period
    for (int i = 0; i < period; i++) {
        sum += data[i];
        if (i < period - 1) ema_output[i] = NAN; // Not enough data for EMA yet
    }
    ema_output[period - 1] = sum / period;

    // Calculate the rest of the EMAs
    for (int i = period; i < data_size; i++) {
        ema_output[i] = (data[i] - ema_output[i-1]) * multiplier + ema_output[i-1];
    }
}

/**
 * @brief Parses JSON, calculates MACD, and prints updated stock data on a specific row.
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
    
    // --- Extract basic price data ---
    const char *symbol = cJSON_GetObjectItemCaseSensitive(meta, "symbol")->valuestring;
    double price = cJSON_GetObjectItemCaseSensitive(meta, "regularMarketPrice")->valuedouble;
    double prev_close = cJSON_GetObjectItemCaseSensitive(meta, "chartPreviousClose")->valuedouble;
    double change = price - prev_close;
    double percent_change = (prev_close == 0) ? 0 : (change / prev_close) * 100.0;
    const char* price_color = (change >= 0) ? KGRN : KRED;

    // --- MACD Calculation ---
    double std_macd_percent = NAN; // Default to NAN
    cJSON *indicators = cJSON_GetObjectItemCaseSensitive(result, "indicators");
    cJSON *quote_array = cJSON_GetObjectItemCaseSensitive(indicators, "quote");
    if (cJSON_IsArray(quote_array) && cJSON_GetArraySize(quote_array) > 0) {
        cJSON *quote = cJSON_GetArrayItem(quote_array, 0);
        cJSON *close_prices_json = cJSON_GetObjectItemCaseSensitive(quote, "close");

        if (cJSON_IsArray(close_prices_json)) {
            int num_prices_total = cJSON_GetArraySize(close_prices_json);
            double* close_prices = malloc(num_prices_total * sizeof(double));
            int valid_prices_count = 0;

            // Copy prices to a double array, skipping any null values
            for (int i = 0; i < num_prices_total; i++) {
                cJSON *price_item = cJSON_GetArrayItem(close_prices_json, i);
                if (cJSON_IsNumber(price_item)) {
                    close_prices[valid_prices_count++] = price_item->valuedouble;
                }
            }

            // Need at least 34 periods for a stable MACD (26 for slow EMA + 9 for signal line - 1)
            if (valid_prices_count >= 34) {
                double* ema12 = malloc(valid_prices_count * sizeof(double));
                double* ema26 = malloc(valid_prices_count * sizeof(double));
                
                calculate_ema_series(close_prices, valid_prices_count, 12, ema12);
                calculate_ema_series(close_prices, valid_prices_count, 26, ema26);
                
                int macd_len = valid_prices_count - 25;
                double* macd_line = malloc(macd_len * sizeof(double));
                for(int i = 0; i < macd_len; i++) {
                    macd_line[i] = ema12[i + 25] - ema26[i + 25];
                }

                double* signal_line = malloc(macd_len * sizeof(double));
                calculate_ema_series(macd_line, macd_len, 9, signal_line);

                // Latest values are at the end of the arrays
                double latest_macd_line = macd_line[macd_len - 1];
                double latest_signal_line = signal_line[macd_len - 1];
                
                if (!isnan(latest_macd_line) && !isnan(latest_signal_line)) {
                    double histogram = latest_macd_line - latest_signal_line;
                    double latest_price = close_prices[valid_prices_count - 1];
                    if (latest_price > 0.0001) { // Avoid division by zero
                        std_macd_percent = (histogram / latest_price) * 100.0;
                    }
                }

                free(ema12);
                free(ema26);
                free(macd_line);
                free(signal_line);
            }
            free(close_prices);
        }
    }
    
    // --- Print Data to Terminal ---
    printf("\033[%d;1H", row); // ANSI: Move cursor to the start of the specified row
    
    // Print the formatted data row, clearing the rest of the line
    printf("%-10s | %s%10.2f%s | %s%+10.2f%s | %s%+10.2f%%%s | ",
           symbol,
           KBLU, price, KNRM,
           price_color, change, KNRM,
           price_color, percent_change, KNRM);

    // Print MACD value or N/A
    if (!isnan(std_macd_percent)) {
        const char* macd_color = (std_macd_percent >= 0) ? KGRN : KRED;
        printf("%s%+12.2f%%%s", macd_color, std_macd_percent, KNRM);
    } else {
        printf("%s%13s%s", KYEL, "N/A", KNRM);
    }
    printf("\033[K\n"); // Clear to end of line

    cJSON_Delete(root);
}


/**
 * @brief Prints an error message on a specific row of the dashboard.
 */
void print_error_on_line(const char* ticker, const char* error_msg, int row) {
    printf("\033[%d;1H", row); // ANSI: Move cursor to the start of the specified row
    printf("%-10s | %s%-60s%s\033[K\n", ticker, KRED, error_msg, KNRM);
}


// --- UI and Terminal Control Functions ---

/**
 * @brief Performs the initial one-time setup of the dashboard UI.
 */
void setup_dashboard_ui() {
    hide_cursor();
    printf("\033[2J\033[H"); // ANSI: Clear screen and move to top-left

    printf("--- C Terminal Stock Dashboard ---\n");
    printf("\n"); // Leave a blank line for the dynamic timestamp
    printf("\n");

    // Print static headers
    printf("%-10s | %11s | %11s | %12s | %14s\n", "Ticker", "Price", "Change", "% Change", "Std MACD %");
    printf("------------------------------------------------------------------------------\n");
    
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

    printf("\033[2;1H"); // ANSI: Move cursor to row 2, column 1
    printf("Last updated: %s\033[K\n", time_str); // \033[K clears to end of line
    fflush(stdout);
}

/**
 * @brief Displays a live countdown timer at the bottom of the dashboard.
 */
void run_countdown() {
    int update_line = DATA_START_ROW + num_tickers + 2;

    for (int i = UPDATE_INTERVAL_SECONDS; i > 0; i--) {
        printf("\033[%d;1H", update_line); // ANSI: Move cursor to the update line
        printf("\033[KUpdating in %2d seconds...", i); // ANSI: Clear line and print
        fflush(stdout);
        sleep(1);
    }
    printf("\033[%d;1H\033[KUpdating now...           ", update_line);
    fflush(stdout);
}

void hide_cursor() {
    printf("\033[?25l"); // ANSI: Hides the terminal cursor
    fflush(stdout);
}

void show_cursor() {
    printf("\033[?25h"); // ANSI: Shows the terminal cursor
    fflush(stdout);
}

void cleanup_on_exit() {
    // This function is called by atexit() to ensure the cursor is restored.
    show_cursor();
}
