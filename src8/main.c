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

// Add or remove stock tickers here
const char *tickers[] = {"AAPL", "GOOGL", "TSLA", "MSFT", "NVDA", "BTC-USD", "ETH-USD"};
const int num_tickers = sizeof(tickers) / sizeof(tickers[0]);

// --- Color Definitions for Terminal ---
#define KNRM  "\x1B[0m"
#define KRED  "\x1B[31m"
#define KGRN  "\x1B[32m"
#define KYEL  "\x1B[33m"

// --- Struct to hold HTTP response data ---
typedef struct {
    char *memory;
    size_t size;
} MemoryStruct;

// --- Function Prototypes ---
static size_t write_callback(void *contents, size_t size, size_t nmemb, void *userp);
char* fetch_url(const char *url);
void parse_and_print_stock_data(const char *json_string);
void display_dashboard_header();

// --- Main Application ---
int main(void) {
    // Initialize libcurl
    curl_global_init(CURL_GLOBAL_ALL);

    while (1) {
        display_dashboard_header();
        
        char url[256];
        for (int i = 0; i < num_tickers; i++) {
            // Construct the URL for the current ticker
            snprintf(url, sizeof(url), API_URL_FORMAT, tickers[i]);
            
            // Fetch data from the URL
            char *json_response = fetch_url(url);
            
            if (json_response) {
                parse_and_print_stock_data(json_response);
                free(json_response); // Free the memory allocated by fetch_url
            } else {
                printf("%-10s | %sFailed to fetch data%s\n", tickers[i], KRED, KNRM);
            }
        }
        
        printf("\nUpdating in %d seconds...\n", UPDATE_INTERVAL_SECONDS);
        fflush(stdout); // Ensure output is printed before sleep
        sleep(UPDATE_INTERVAL_SECONDS);
    }

    // Cleanup libcurl (though this part is unreachable in the infinite loop)
    curl_global_cleanup();
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
            return NULL;
        }

        curl_easy_cleanup(curl_handle);
        return chunk.memory;
    }
    
    free(chunk.memory);
    return NULL;
}

/**
 * @brief Parses the JSON from Yahoo Finance and prints a formatted line.
 */
void parse_and_print_stock_data(const char *json_string) {
    cJSON *root = cJSON_Parse(json_string);
    if (root == NULL) {
        // const char *error_ptr = cJSON_GetErrorPtr();
        // if (error_ptr != NULL) fprintf(stderr, "cJSON Error before: %s\n", error_ptr);
        return;
    }

    // Navigate the JSON: root -> chart -> result[0] -> meta
    cJSON *chart = cJSON_GetObjectItemCaseSensitive(root, "chart");
    cJSON *result_array = cJSON_GetObjectItemCaseSensitive(chart, "result");
    if (!cJSON_IsArray(result_array) || cJSON_GetArraySize(result_array) == 0) {
        // Handle cases where a ticker might be invalid
        cJSON* error_obj = cJSON_GetObjectItemCaseSensitive(chart, "error");
        if(error_obj && cJSON_GetObjectItemCaseSensitive(error_obj, "description")) {
            char* err_desc = cJSON_GetObjectItemCaseSensitive(error_obj, "description")->valuestring;
            printf("%-10s | %s%s%s\n", "Error", KRED, err_desc, KNRM);
        } else {
             printf("%-10s | %sInvalid ticker or no data returned%s\n", "Error", KRED, KNRM);
        }
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
    double percent_change = (change / prev_close) * 100.0;
    
    // Determine color based on change
    const char* color = (change >= 0) ? KGRN : KRED;
    char sign = (change >= 0) ? '+' : '-';

    printf("%-10s | %s%10.2f%s | %s%c%9.2f%s | %s%c%10.2f%%%s\n",
           symbol,
           KYEL, price, KNRM,
           color, sign, (change > 0 ? change : -change), KNRM,
           color, sign, (percent_change > 0 ? percent_change : -percent_change), KNRM);

    cJSON_Delete(root);
}


/**
 * @brief Clears the screen and prints the dashboard header.
 */
void display_dashboard_header() {
    // ANSI escape codes to clear screen and move cursor to top-left
    printf("\033[2J\033[H");

    time_t t = time(NULL);
    struct tm *tm = localtime(&t);
    char time_str[64];
    strftime(time_str, sizeof(time_str), "%Y-%m-%d %H:%M:%S", tm);

    printf("--- C Terminal Stock Dashboard ---\n");
    printf("Last updated: %s\n\n", time_str);

    printf("%-10s | %11s | %11s | %13s\n", "Ticker", "Price", "Change", "% Change");
    printf("-------------------------------------------------------------\n");
}
