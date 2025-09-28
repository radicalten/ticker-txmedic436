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
#define API_URL_FORMAT "https://query1.finance.yahoo.com/v8/finance/chart/%s"
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

    // ANSI: Move cursor to the start of the specified row
    printf("\033[%d;1H", row);

    // Print the formatted data row.
    // ANSI: \033[K clears from the cursor to the end of the line.
    // This is important to overwrite any previous, longer text (like an error message).
    printf("%-10s | %s%10.2f%s | %s%c%9.2f%s | %s%c%10.2f%%%s\033[K\n",
           symbol,
           KBLU, price, KNRM, // <-- MODIFIED: Changed KYEL to KBLU for blue price
           color, sign, (change >= 0 ? change : -change), KNRM,
           color, sign, (percent_change >= 0 ? percent_change : -percent_change), KNRM);

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
    printf("%-10s | %s%-40s%s\033[K\n", ticker, KRED, error_msg, KNRM);
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
    printf("%-10s | %11s | %11s | %13s\n", "Ticker", "Price", "Change", "% Change");
    printf("-------------------------------------------------------------\n");
    
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
