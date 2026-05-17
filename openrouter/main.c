#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <curl/curl.h>
#include "cJSON.h"

// Configuration
const char* SYMBOLS = "AAPL,MSFT,GOOGL,TSLA,AMZN,NVDA";
const int REFRESH_INTERVAL = 10; // seconds

// Structure to hold curl response data
struct MemoryStruct {
    char *memory;
    size_t size;
};

// Callback for curl to write data into our memory buffer
static size_t WriteMemoryCallback(void *contents, size_t size, size_t nmemb, void *userp) {
    size_t realsize = size * nmemb;
    struct MemoryStruct *mem = (struct MemoryStruct *)userp;

    char *ptr = realloc(mem->memory, mem->size + realsize + 1);
    if (!ptr) return 0; // out of memory

    mem->memory = ptr;
    memcpy(&(mem->memory[mem->size]), contents, realsize);
    mem->size += realsize;
    mem->memory[mem->size] = 0;

    return realsize;
}

void print_dashboard(const char* json_data) {
    cJSON *root = cJSON_Parse(json_data);
    if (!root) {
        printf("Error parsing JSON.\n");
        return;
    }

    cJSON *quoteResponse = cJSON_GetObjectItemCaseSensitive(root, "quoteResponse");
    cJSON *result = cJSON_GetObjectItemCaseSensitive(quoteResponse, "result");

    if (!cJSON_IsArray(result)) {
        printf("Unexpected JSON format.\n");
        cJSON_Delete(root);
        return;
    }

    // ANSI Escape codes: Clear screen and move cursor to top-left
    printf("\033[H\033[J");
    printf("============================================================\n");
    printf(" LIVE STOCK DASHBOARD (Yahoo Finance API)\n");
    printf("============================================================\n");
    printf("%-8s | %-10s | %-10s | %-8s\n", "Symbol", "Price", "Change", "Change %");
    printf("------------------------------------------------------------\n");

    cJSON *stock = NULL;
    cJSON_ArrayForEach(stock, result) {
        cJSON *symbol = cJSON_GetObjectItemCaseSensitive(stock, "symbol");
        cJSON *price = cJSON_GetObjectItemCaseSensitive(stock, "regularMarketPrice");
        cJSON *change = cJSON_GetObjectItemCaseSensitive(stock, "regularMarketChange");
        cJSON *pct = cJSON_GetObjectItemCaseSensitive(stock, "regularMarketChangePercent");

        // Color coding: Green for positive, Red for negative
        const char *color = (change->valuedouble >= 0) ? "\033[0;32m" : "\033[0;31m";
        const char *reset = "\033[0m";

        printf("%-8s | %-10.2f | %s%+-10.2f%s | %s%+.2f%%%s\n",
               symbol->valuestring,
               price->valuedouble,
               color, change->valuedouble, reset,
               color, pct->valuedouble, reset);
    }
    printf("============================================================\n");
    printf("Updating every %ds. Press Ctrl+C to exit.\n", REFRESH_INTERVAL);

    cJSON_Delete(root);
}

int main(void) {
    CURL *curl_handle;
    CURLcode res;

    curl_global_init(CURL_GLOBAL_ALL);

    char url[256];
    snprintf(url, sizeof(url), "https://query1.finance.yahoo.com/v7/finance/quote?symbols=%s", SYMBOLS);

    while (1) {
        struct MemoryStruct chunk;
        chunk.memory = malloc(1); 
        chunk.size = 0; 

        curl_handle = curl_easy_init();
        if (curl_handle) {
            // Set URL
            curl_easy_setopt(curl_handle, CURLOPT_URL, url);

            // SET USER-AGENT (Crucial for Yahoo Finance)
            curl_easy_setopt(curl_handle, CURLOPT_USERAGENT, "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/91.0.4472.124 Safari/537.36");

            // Setup memory callback
            curl_easy_setopt(curl_handle, CURLOPT_WRITEFUNCTION, WriteMemoryCallback);
            curl_easy_setopt(curl_handle, CURLOPT_WRITEDATA, (void *)&chunk);

            // Perform the request
            res = curl_easy_perform(curl_handle);

            if (res != CURLE_OK) {
                fprintf(stderr, "curl_easy_perform() failed: %s\n", curl_easy_strerror(res));
            } else {
                print_dashboard(chunk.memory);
            }

            curl_easy_cleanup(curl_handle);
            free(chunk.memory);
        }

        sleep(REFRESH_INTERVAL);
    }

    curl_global_cleanup();
    return 0;
}
