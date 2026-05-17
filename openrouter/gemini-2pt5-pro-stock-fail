// main.c
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h> // For sleep()

#include <curl/curl.h> // Requires libcurl development library
#include "cJSON.h"     // The cJSON header

// --- Configuration ---
// Add or remove stock tickers here
const char *STOCKS[] = {
    "AAPL", "GOOGL", "TSLA", "MSFT", "NVDA", "BTC-USD", "ETH-USD"
};
const int NUM_STOCKS = sizeof(STOCKS) / sizeof(STOCKS[0]);
const int REFRESH_INTERVAL_SECONDS = 10; // Refresh data every 10 seconds

// This is crucial to avoid being blocked by Yahoo's API
#define USER_AGENT "My-C-Stock-Ticker/1.0"
#define API_URL_FORMAT "https://query1.finance.yahoo.com/v7/finance/quote?symbols=%s"

// A struct to hold the data received from libcurl
struct MemoryStruct {
    char *memory;
    size_t size;
};

// --- libcurl Callback Function ---
// This function is called by libcurl as soon as there is data received
static size_t WriteMemoryCallback(void *contents, size_t size, size_t nmemb, void *userp) {
    size_t realsize = size * nmemb;
    struct MemoryStruct *mem = (struct MemoryStruct *)userp;

    char *ptr = realloc(mem->memory, mem->size + realsize + 1);
    if (ptr == NULL) {
        // Out of memory!
        printf("Not enough memory (realloc returned NULL)\n");
        return 0;
    }

    mem->memory = ptr;
    memcpy(&(mem->memory[mem->size]), contents, realsize);
    mem->size += realsize;
    mem->memory[mem->size] = 0;

    return realsize;
}

// --- Data Parsing and Display ---
void parse_and_display_stocks(const char *json_string) {
    // ANSI escape codes for colors
    const char *color_green = "\033[0;32m";
    const char *color_red = "\033[0;31m";
    const char *color_reset = "\033[0m";

    cJSON *root = cJSON_Parse(json_string);
    if (root == NULL) {
        const char *error_ptr = cJSON_GetErrorPtr();
        if (error_ptr != NULL) {
            fprintf(stderr, "Error parsing JSON before: %s\n", error_ptr);
        }
        return;
    }

    cJSON *quoteResponse = cJSON_GetObjectItemCaseSensitive(root, "quoteResponse");
    cJSON *resultArray = cJSON_GetObjectItemCaseSensitive(quoteResponse, "result");
    if (!cJSON_IsArray(resultArray)) {
        fprintf(stderr, "Error: 'result' is not an array.\n");
        cJSON_Delete(root);
        return;
    }

    // Print table header
    printf("%-10s | %15s | %15s | %12s\n", "Symbol", "Price", "Change", "% Change");
    printf("-----------|-----------------|-----------------|-------------\n");
    
    cJSON *stock_item;
    cJSON_ArrayForEach(stock_item, resultArray) {
        cJSON *symbol = cJSON_GetObjectItemCaseSensitive(stock_item, "symbol");
        cJSON *price = cJSON_GetObjectItemCaseSensitive(stock_item, "regularMarketPrice");
        cJSON *change = cJSON_GetObjectItemCaseSensitive(stock_item, "regularMarketChange");
        cJSON *change_percent = cJSON_GetObjectItemCaseSensitive(stock_item, "regularMarketChangePercent");

        if (cJSON_IsString(symbol) && cJSON_IsNumber(price) && cJSON_IsNumber(change)) {
            const char* change_color = (change->valuedouble >= 0) ? color_green : color_red;
            
            printf("%-10s | %15.2f | %s%15.2f%s | %s%11.2f%%%s\n",
                   symbol->valuestring,
                   price->valuedouble,
                   change_color, change->valuedouble, color_reset,
                   change_color, change_percent->valuedouble, color_reset
            );
        }
    }
    
    cJSON_Delete(root);
}

// --- Main Program Logic ---
int main(void) {
    CURL *curl_handle;
    CURLcode res;

    // Initialize libcurl
    curl_global_init(CURL_GLOBAL_ALL);
    curl_handle = curl_easy_init();

    if (!curl_handle) {
        fprintf(stderr, "Failed to initialize curl\n");
        return 1;
    }

    // --- Build the URL with all stock symbols ---
    // Calculate required buffer size for all symbols + commas
    size_t url_buffer_size = strlen(API_URL_FORMAT) + (NUM_STOCKS * 10); // Generous estimate
    char *symbols_string = malloc(NUM_STOCKS * 10);
    char *api_url = malloc(url_buffer_size);

    if (!symbols_string || !api_url) {
        fprintf(stderr, "Failed to allocate memory for URL.\n");
        return 1;
    }

    // Concatenate stock symbols with commas
    strcpy(symbols_string, STOCKS[0]);
    for (int i = 1; i < NUM_STOCKS; i++) {
        strcat(symbols_string, ",");
        strcat(symbols_string, STOCKS[i]);
    }
    sprintf(api_url, API_URL_FORMAT, symbols_string);
    free(symbols_string); // No longer needed

    // --- Main Loop ---
    while (1) {
        struct MemoryStruct chunk;
        chunk.memory = malloc(1); // will be grown by realloc
        chunk.size = 0;

        // Set curl options
        curl_easy_setopt(curl_handle, CURLOPT_URL, api_url);
        curl_easy_setopt(curl_handle, CURLOPT_WRITEFUNCTION, WriteMemoryCallback);
        curl_easy_setopt(curl_handle, CURLOPT_WRITEDATA, (void *)&chunk);
        curl_easy_setopt(curl_handle, CURLOPT_USERAGENT, USER_AGENT); // Set the User-Agent

        // Perform the request
        res = curl_easy_perform(curl_handle);

        // Clear screen and print header
        printf("\033[2J\033[H"); // ANSI escape code to clear screen
        printf("--- C Stock Ticker Dashboard ---\n");
        time_t t = time(NULL);
        struct tm *tm = localtime(&t);
        printf("Last updated: %s\n", asctime(tm));


        if (res != CURLE_OK) {
            fprintf(stderr, "curl_easy_perform() failed: %s\n", curl_easy_strerror(res));
        } else {
            // We got the data, now parse and display it
            parse_and_display_stocks(chunk.memory);
        }
        
        // Cleanup memory from this request
        free(chunk.memory);
        
        printf("\nRefreshing in %d seconds...\n", REFRESH_INTERVAL_SECONDS);
        sleep(REFRESH_INTERVAL_SECONDS);
    }

    // --- Final Cleanup (though the loop is infinite) ---
    free(api_url);
    curl_easy_cleanup(curl_handle);
    curl_global_cleanup();

    return 0;
}
