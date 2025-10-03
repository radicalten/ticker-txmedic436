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
// MODIFIED: Added range and interval to get historical data for MACD calculation
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

// NEW: Function prototypes for MACD calculation
double* calculate_ema(const double* data, int data_size, int period);
int calculate_macd_and_signal(const double* close_prices, int num_prices, double* out_latest_macd, double* out_latest_signal);


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


// --- NEW: Technical Analysis Functions ---

/**
 * @brief Calculates the Exponential Moving Average (EMA) for a series of data.
 * @param data An array of doubles (e.g., closing prices).
 * @param data_size The number of elements in the data array.
 * @param period The period for the EMA (e.g., 12, 26).
 * @return A dynamically allocated array of EMA values, or NULL on failure.
 *         The caller is responsible for freeing this memory.
 *         The first `period-1` elements are uninitialized as EMA is not yet valid.
 */
double* calculate_ema(const double* data, int data_size, int period) {
    if (data_size < period) return NULL;

    double* ema = (double*)malloc(sizeof(double) * data_size);
    if (!ema) return NULL;

    double multiplier = 2.0 / (period + 1.0);
    double sma = 0.0;

    // Calculate initial SMA for the first EMA value
    for (int i = 0; i < period; i++) {
        sma += data[i];
    }
    ema[period - 1] = sma / period;

    // Calculate the rest of the EMAs
    for (int i = period; i < data_size; i++) {
        ema[i] = (data[i] * multiplier) + (ema[i - 1] * (1.0 - multiplier));
    }
    
    return ema;
}

/**
 * @brief Calculates the latest MACD and Signal line values.
 * @param close_prices An array of historical closing prices.
 * @param num_prices The number of elements in the array.
 * @param out_latest_macd Pointer to store the latest MACD value.
 * @param out_latest_signal Pointer to store the latest Signal line value.
 * @return 1 on success, 0 on failure (e.g., not enough data).
 */
int calculate_macd_and_signal(const double* close_prices, int num_prices, 
                             double* out_latest_macd, double* out_latest_signal) {
    const int short_period = 12;
    const int long_period = 26;
    const int signal_period = 9;
    
    // We need at least (long_period + signal_period) data points for a valid signal line.
    if (num_prices < long_period + signal_period) {
        return 0;
    }

    double* ema12 = calculate_ema(close_prices, num_prices, short_period);
    double* ema26 = calculate_ema(close_prices, num_prices, long_period);
    if (!ema12 || !ema26) {
        free(ema12);
        free(ema26);
        return 0;
    }

    // Create an array of only the valid MACD values (starting from where ema26 is valid)
    int macd_size = num_prices - (long_period - 1);
    double* macd_line = (double*)malloc(sizeof(double) * macd_size);
    if (!macd_line) {
        free(ema12);
        free(ema26);
        return 0;
    }
    
    for (int i = 0; i < macd_size; i++) {
        int price_index = i + (long_period - 1);
        macd_line[i] = ema12[price_index] - ema26[price_index];
    }
    
    // Calculate signal line (a 9-period EMA of the MACD line)
    double* signal_line_values = calculate_ema(macd_line, macd_size, signal_period);
    if (!signal_line_values) {
        free(ema12);
        free(ema26);
        free(macd_line);
        return 0;
    }

    // The last value in each calculated array is the latest one.
    *out_latest_macd = macd_line[macd_size - 1];
    *out_latest_signal = signal_line_values[macd_size - 1];

    // Cleanup
    free(ema12);
    free(ema26);
    free(macd_line);
    free(signal_line_values);

    return 1;
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
    
    // --- Extract Daily Price Values ---
    const char *symbol = cJSON_GetObjectItemCaseSensitive(meta, "symbol")->valuestring;
    double price = cJSON_GetObjectItemCaseSensitive(meta, "regularMarketPrice")->valuedouble;
    double prev_close = cJSON_GetObjectItemCaseSensitive(meta, "chartPreviousClose")->valuedouble;
    
    double change = price - prev_close;
    double percent_change = (prev_close == 0) ? 0 : (change / prev_close) * 100.0;
    
    const char* color = (change >= 0) ? KGRN : KRED;
    char sign = (change >= 0) ? '+' : '-';

    // --- MACD Calculation ---
    double latest_macd = 0.0, latest_signal = 0.0;
    int macd_calculated = 0;

    cJSON *indicators = cJSON_GetObjectItemCaseSensitive(result, "indicators");
    cJSON *quote_array = cJSON_GetObjectItemCaseSensitive(indicators, "quote");
    if (cJSON_IsArray(quote_array) && cJSON_GetArraySize(quote_array) > 0) {
        cJSON *quote = cJSON_GetArrayItem(quote_array, 0);
        cJSON *close_prices_json = cJSON_GetObjectItemCaseSensitive(quote, "close");

        if (cJSON_IsArray(close_prices_json)) {
            int num_prices_total = cJSON_GetArraySize(close_prices_json);
            // Allocate memory for a clean array of prices, filtering out nulls
            double *close_prices_clean = malloc(sizeof(double) * num_prices_total);
            if (close_prices_clean) {
                int num_prices_clean = 0;
                for (int i = 0; i < num_prices_total; i++) {
                    cJSON *price_item = cJSON_GetArrayItem(close_prices_json, i);
                    if (cJSON_IsNumber(price_item)) {
                        close_prices_clean[num_prices_clean++] = price_item->valuedouble;
                    }
                }
                
                // If calculation is successful, set the flag
                if (calculate_macd_and_signal(close_prices_clean, num_prices_clean, &latest_macd, &latest_signal)) {
                    macd_calculated = 1;
                }
                free(close_prices_clean);
            }
        }
    }

    // --- Print Data to Terminal ---
    printf("\033[%d;1H", row); // ANSI: Move cursor to the start of the specified row
    
    // Print Ticker, Price, Change, % Change
    printf("%-10s | %s%10.2f%s | %s%c%9.2f%s | %s%c%10.2f%%%s",
           symbol,
           KBLU, price, KNRM,
           color, sign, (change >= 0 ? change : -change), KNRM,
           color, sign, (percent_change >= 0 ? percent_change : -percent_change), KNRM);

    // Print MACD and Signal Line
    if (macd_calculated) {
        // Standardize MACD and Signal as a percentage of the closing price
        double std_macd = (price != 0) ? (latest_macd / price) * 100.0 : 0.0;
        double std_signal = (price != 0) ? (latest_signal / price) * 100.0 : 0.0;
        const char* macd_color = (std_macd >= std_signal) ? KGRN : KRED;

        printf(" | %s%+12.2f%%%s | %+13.2f%%%s",
                macd_color, std_macd, KNRM,
                std_signal, KNRM);
    } else {
        printf(" | %13s | %14s", "N/A", "N/A");
    }

    printf("\033[K\n"); // ANSI: Clear from cursor to end of line and add newline

    cJSON_Delete(root);
}

/**
 * @brief Prints an error message on a specific row of the dashboard.
 */
void print_error_on_line(const char* ticker, const char* error_msg, int row) {
    printf("\033[%d;1H", row);
    printf("%-10s | %s%-65s%s\033[K\n", ticker, KRED, error_msg, KNRM);
}


// --- UI and Terminal Control Functions ---

/**
 * @brief Performs the initial one-time setup of the dashboard UI.
 */
void setup_dashboard_ui() {
    hide_cursor();
    printf("\033[2J\033[H"); // ANSI: Clear screen, move cursor to top-left

    printf("--- C Terminal Stock Dashboard ---\n");
    printf("\n"); // Leave a blank line for the dynamic timestamp
    printf("\n");

    // MODIFIED: Added new columns for Standardized MACD and Signal Line
    printf("%-10s | %10s | %10s | %11s | %12s | %13s\n", "Ticker", "Price", "Change", "% Change", "Std MACD %", "Std Signal %");
    printf("----------------------------------------------------------------------------------------\n");
    
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
    printf("Last updated: %s\033[K\n", time_str); // ANSI: \033[K clears line
    fflush(stdout);
}

/**
 * @brief Displays a live countdown timer at the bottom of the dashboard.
 */
void run_countdown() {
    int update_line = DATA_START_ROW + num_tickers + 1;

    for (int i = UPDATE_INTERVAL_SECONDS; i > 0; i--) {
        printf("\033[%d;1H", update_line); // ANSI: Move cursor to update line
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
