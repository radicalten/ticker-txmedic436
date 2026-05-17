#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <curl/curl.h>
#include "cJSON.h"

// Structure to hold the API response
struct MemoryStruct {
    char *memory;
    size_t size;
};

// Callback function for curl to write data into our memory buffer
static size_t WriteMemoryCallback(void *contents, size_t size, size_t nmemb, void *userp) {
    size_t realsize = size * nmemb;
    struct MemoryStruct *mem = (struct MemoryStruct *)userp;

    char *ptr = realloc(mem->memory, mem->size + realsize + 1);
    if (!ptr) {
        printf("Not enough memory (realloc returned NULL)\n");
        return 0;
    }

    mem->memory = ptr;
    memcpy(&(mem->memory[mem->size]), contents, realsize);
    mem->size += realsize;
    mem->memory[mem->size] = 0;

    return realsize;
}

/**
 * Fetches stock data from Yahoo Finance
 * @param symbols A comma-separated string of stock symbols (e.g., "AAPL,MSFT,GOOGL")
 * @return A dynamically allocated string containing the JSON response
 */
char* fetch_stock_data(const char *symbols) {
    CURL *curl_handle;
    CURLcode res;
    struct MemoryStruct chunk;

    chunk.memory = malloc(1); 
    chunk.size = 0; 

    curl_global_init(CURL_GLOBAL_ALL);
    curl_handle = curl_easy_init();

    // Construct the URL
    char url[256];
    snprintf(url, sizeof(url), "https://query1.finance.yahoo.com/v7/finance/quote?symbols=%s", symbols);

    curl_easy_setopt(curl_handle, CURLOPT_URL, url);
    curl_easy_setopt(curl_handle, CURLOPT_WRITEFUNCTION, WriteMemoryCallback);
    curl_easy_setopt(curl_handle, CURLOPT_WRITEDATA, (void *)&chunk);
    
    // IMPORTANT: Set a User-Agent to prevent being blocked by Yahoo
    curl_easy_setopt(curl_handle, CURLOPT_USERAGENT, "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/91.0.4472.124 Safari/537.36");

    res = curl_easy_perform(curl_handle);

    if (res != CURLE_OK) {
        fprintf(stderr, "curl_easy_perform() failed: %s\n", curl_easy_strerror(res));
        free(chunk.memory);
        chunk.memory = NULL;
    }

    curl_easy_cleanup(curl_handle);
    curl_global_cleanup();

    return chunk.memory;
}

void print_dashboard(const char *json_str) {
    cJSON *root = cJSON_Parse(json_str);
    if (root == NULL) {
        printf("Error parsing JSON.\n");
        return;
    }

    // Clear terminal screen using ANSI escape codes
    // \033[H moves cursor to top, \033[J clears screen
    printf("\033[H\033[J");
    printf("====================================================\n");
    printf("           LIVE STOCK MARKET DASHBOARD             \n");
    printf("====================================================\n");
    printf("%-10s | %-10s | %-10s | %-8s\n", "SYMBOL", "PRICE", "CHANGE", "% CHANGE");
    printf("----------------------------------------------------\n");

    cJSON *quoteResponse = cJSON_GetObjectItemCaseSensitive(root, "quoteResponse");
    cJSON *result = cJSON_GetObjectItemCaseSensitive(quoteResponse, "result");
    cJSON *stock;

    cJSON_ArrayForEach(stock, result) {
        cJSON *symbol = cJSON_GetObjectItemCaseSensitive(stock, "symbol");
        cJSON *price = cJSON_GetObjectItemCaseSensitive(stock, "regularMarketPrice");
        cJSON *change = cJSON_GetObjectItemCaseSensitive(stock, "regularMarketChange");
        cJSON *percent = cJSON_GetObjectItemCaseSensitive(stock, "regularMarketChangePercent");

        if (cJSON_IsString(symbol) && cJSON_IsNumber(price)) {
            // Color coding: Green for positive, Red for negative
            const char *color = (cJSON_IsNumber(change) && change->valuedouble >= 0) ? "\033[32m" : "\033[31m";
            const char *reset = "\033[0m";

            printf("%-10s | %-10.2f | %s%-10.2f%s | %s%.2f%%%s\n",
                   symbol->valuestring,
                   price->valuedouble,
                   color, change->valuedouble, reset,
                   color, percent->valuedouble, reset);
        }
    }

    printf("----------------------------------------------------\n");
    printf("Last Updated: (Polling every 5 seconds)\n");
    printf("Press Ctrl+C to exit.\n");

    cJSON_Delete(root);
}

int main(int argc, char *argv[]) {
    const char *symbols = "AAPL,MSFT,GOOGL,TSLA,AMZN";
    
    if (argc > 1) {
        symbols = argv[1];
    }

    while (1) {
        char *json_data = fetch_stock_data(symbols);

        if (json_data) {
            print_dashboard(json_data);
            free(json_data);
        } else {
            printf("Failed to fetch data.\n");
        }

        sleep(5); // Refresh interval
    }

    return 0;
}
