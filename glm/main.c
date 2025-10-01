#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>     // For sleep()
#include <time.h>       // For timestamp
#include <signal.h>     // For graceful exit (Ctrl+C)
#include <curl/curl.h>  // libcurl for HTTP requests
#include "cJSON.h"      // cJSON for parsing

// --- Configuration ---
#define REFRESH_INTERVAL_SECONDS 15
#define API_URL_BASE "https://query1.finance.yahoo.com/v7/finance/quote?symbols="
#define USER_AGENT "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/91.0.4472.124 Safari/537.36"

// --- Global variable for graceful exit ---
volatile sig_atomic_t keep_running = 1;

// --- libcurl write callback function ---
// This function gets called by libcurl as soon as there is data received
struct MemoryStruct {
    char *memory;
    size_t size;
};

static size_t WriteMemoryCallback(void *contents, size_t size, size_t nmemb, void *userp) {
    size_t realsize = size * nmemb;
    struct MemoryStruct *mem = (struct MemoryStruct *)userp;

    char *ptr = realloc(mem->memory, mem->size + realsize + 1);
    if (!ptr) {
        /* out of memory! */
        fprintf(stderr, "not enough memory (realloc returned NULL)\n");
        return 0;
    }

    mem->memory = ptr;
    memcpy(&(mem->memory[mem->size]), contents, realsize);
    mem->size += realsize;
    mem->memory[mem->size] = 0;

    return realsize;
}

// --- Function to fetch stock data ---
char* fetch_stock_data(const char* url) {
    CURL *curl_handle;
    CURLcode res;
    struct MemoryStruct chunk;
    chunk.memory = malloc(1);  // will be grown as needed by the callback
    chunk.size = 0;           // no data at this point

    curl_global_init(CURL_GLOBAL_ALL);
    curl_handle = curl_easy_init();

    if (curl_handle) {
        curl_easy_setopt(curl_handle, CURLOPT_URL, url);
        curl_easy_setopt(curl_handle, CURLOPT_WRITEFUNCTION, WriteMemoryCallback);
        curl_easy_setopt(curl_handle, CURLOPT_WRITEDATA, (void *)&chunk);
        curl_easy_setopt(curl_handle, CURLOPT_USERAGENT, USER_AGENT);
        curl_easy_setopt(curl_handle, CURLOPT_FOLLOWLOCATION, 1L); // Follow redirects
        
        res = curl_easy_perform(curl_handle);

        if (res != CURLE_OK) {
            fprintf(stderr, "curl_easy_perform() failed: %s\n", curl_easy_strerror(res));
            free(chunk.memory);
            chunk.memory = NULL;
        }

        curl_easy_cleanup(curl_handle);
    }
    
    curl_global_cleanup();
    return chunk.memory;
}

// --- Function to parse and display data ---
void display_stocks(const char* json_string) {
    cJSON *root = cJSON_Parse(json_string);
    if (!root) {
        const char *error_ptr = cJSON_GetErrorPtr();
        if (error_ptr) {
            fprintf(stderr, "Error before: %s\n", error_ptr);
        }
        return;
    }

    cJSON *quote_response = cJSON_GetObjectItem(root, "quoteResponse");
    if (!cJSON_IsObject(quote_response)) {
        fprintf(stderr, "Invalid JSON: 'quoteResponse' not found or not an object.\n");
        cJSON_Delete(root);
        return;
    }

    cJSON *result = cJSON_GetObjectItem(quote_response, "result");
    if (!cJSON_IsArray(result)) {
        fprintf(stderr, "Invalid JSON: 'result' not found or not an array.\n");
        cJSON_Delete(root);
        return;
    }

    printf("+-----------------------------+----------------+----------------+----------------+\n");
    printf("| %-27s | %-14s | %-14s | %-14s |\n", "Symbol", "Price", "Change", "Change %");
    printf("+-----------------------------+----------------+----------------+----------------+\n");

    cJSON *stock = NULL;
    cJSON_ArrayForEach(stock, result) {
        cJSON *symbol = cJSON_GetObjectItem(stock, "symbol");
        cJSON *price = cJSON_GetObjectItem(stock, "regularMarketPrice");
        cJSON *change = cJSON_GetObjectItem(stock, "regularMarketChange");
        cJSON *change_percent = cJSON_GetObjectItem(stock, "regularMarketChangePercent");

        if (cJSON_IsString(symbol) && cJSON_IsNumber(price) && cJSON_IsNumber(change) && cJSON_IsNumber(change_percent)) {
            const char* color = (change->valuedouble >= 0) ? "\033[0;32m" : "\033[0;31m"; // Green for positive, Red for negative
            const char* reset_color = "\033[0m";
            
            printf("| %-27s | %s%14.2f%s | %s%+14.2f%s | %s%+13.2f%%%s |\n",
                   symbol->valuestring,
                   reset_color, price->valuedouble, reset_color,
                   color, change->valuedouble, reset_color,
                   color, change_percent->valuedouble, reset_color);
        }
    }
    printf("+-----------------------------+----------------+----------------+----------------+\n");

    cJSON_Delete(root);
}

// --- Signal handler for Ctrl+C ---
void handle_sigint(int sig) {
    keep_running = 0;
}

// --- Main function ---
int main(int argc, char *argv[]) {
    if (argc < 2) {
        fprintf(stderr, "Usage: %s STOCK_TICKER1 [STOCK_TICKER2 ...]\n", argv[0]);
        fprintf(stderr, "Example: %s AAPL GOOGL MSFT\n", argv[0]);
        return 1;
    }

    // Build the symbols string for the URL
    char symbols[1024] = "";
    for (int i = 1; i < argc; i++) {
        strcat(symbols, argv[i]);
        if (i < argc - 1) {
            strcat(symbols, ",");
        }
    }

    // Build the full URL
    char full_url[2048];
    snprintf(full_url, sizeof(full_url), "%s%s", API_URL_BASE, symbols);

    // Set up signal handler
    signal(SIGINT, handle_sigint);

    printf("Live Stock Dashboard - Press Ctrl+C to exit.\n");

    while (keep_running) {
        // Clear the screen (works for Linux/macOS/Windows)
        system("clear || cls");

        printf("Fetching data for: %s\n", symbols);
        
        char *json_data = fetch_stock_data(full_url);
        if (json_data) {
            display_stocks(json_data);
            free(json_data);
        } else {
            printf("Failed to fetch data. Please check your network connection or tickers.\n");
        }
        
        // Print timestamp
        time_t now = time(NULL);
        char time_buf[80];
        strftime(time_buf, sizeof(time_buf), "Last updated: %Y-%m-%d %H:%M:%S", localtime(&now));
        printf("\n%s\n", time_buf);

        if (keep_running) {
            sleep(REFRESH_INTERVAL_SECONDS);
        }
    }

    printf("\nExiting gracefully. Goodbye!\n");
    return 0;
}
