#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <curl/curl.h>
#include "cJSON.h"

// Structure to handle curl response memory
struct MemoryStruct {
    char *memory;
    size_t size;
};

// Callback function for curl to write data into our MemoryStruct
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

double get_stock_price(const char *symbol) {
    CURL *curl_handle;
    CURLcode res;
    struct MemoryStruct chunk;
    chunk.memory = malloc(1); 
    chunk.size = 0; 

    double price = -1.0;
    char url[256];
    
    // Yahoo Finance API endpoint for chart data
    snprintf(url, sizeof(url), "https://query1.finance.yahoo.com/v8/finance/chart/%s?interval=1m&range=1d", symbol);

    curl_global_init(CURL_GLOBAL_ALL);
    curl_handle = curl_easy_init();

    if (curl_handle) {
        curl_easy_setopt(curl_handle, CURLOPT_URL, url);
        curl_easy_setopt(curl_handle, CURLOPT_WRITEFUNCTION, WriteMemoryCallback);
        curl_easy_setopt(curl_handle, CURLOPT_WRITEDATA, (void *)&chunk);

        // IMPORTANT: Yahoo Finance rejects requests without a realistic User-Agent
        curl_easy_setopt(curl_handle, CURLOPT_USERAGENT, "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/119.0.0.0 Safari/537.36");

        res = curl_easy_perform(curl_handle);

        if (res == CURLE_OK) {
            // Parse the JSON response
            cJSON *json = cJSON_Parse(chunk.memory);
            if (json) {
                // Path: chart -> result[0] -> meta -> regularMarketPrice
                cJSON *chart = cJSON_GetObjectItemCaseSensitive(json, "chart");
                cJSON *result = cJSON_GetObjectItemCaseSensitive(chart, "result");
                if (cJSON_IsArray(result)) {
                    cJSON *first_res = cJSON_GetArrayItem(result, 0);
                    cJSON *meta = cJSON_GetObjectItemCaseSensitive(first_res, "meta");
                    cJSON *price_obj = cJSON_GetObjectItemCaseSensitive(meta, "regularMarketPrice");
                    if (cJSON_IsNumber(price_obj)) {
                        price = price_obj->valuedouble;
                    }
                }
                cJSON_Delete(json);
            }
        }
        curl_easy_cleanup(curl_handle);
    }

    free(chunk.memory);
    curl_global_cleanup();
    return price;
}

int main() {
    // List of stocks to track
    const char *symbols[] = {"AAPL", "MSFT", "GOOGL", "AMZN", "TSLA", "BTC-USD"};
    int num_symbols = sizeof(symbols) / sizeof(symbols[0]);

    while (1) {
        // Clear terminal screen (ANSI escape code)
        printf("\033[H\033[J"); 
        printf("==========================================\n");
        printf("   LIVE STOCK DASHBOARD (Yahoo Finance)   \n");
        printf("==========================================\n");
        printf("%-12s | %-10s\n", "SYMBOL", "PRICE");
        printf("------------------------------------------\n");

        for (int i = 0; i < num_symbols; i++) {
            double price = get_stock_price(symbols[i]);
            if (price > 0) {
                printf("%-12s | $%-10.2f\n", symbols[i], price);
            } else {
                printf("%-12s | %-10s\n", symbols[i], "Error/N/A");
            }
        }
        
        printf("------------------------------------------\n");
        printf("Updating every 10 seconds... (Ctrl+C to quit)\n");
        
        sleep(10); 
    }

    return 0;
}
