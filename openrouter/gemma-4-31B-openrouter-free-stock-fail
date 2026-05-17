#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <curl/curl.h>
#include "cJSON.h"

// Structure to handle memory for curl responses
struct MemoryStruct {
    char *memory;
    size_t size;
};

// Callback function for curl to write data into the MemoryStruct
static size_t WriteMemoryCallback(void *contents, size_t size, size_t nmemb, void *userp) {
    size_t realsize = size * nmemb;
    struct MemoryStruct *mem = (struct MemoryStruct *)userp;

    char *ptr = realloc(mem->memory, mem->size + realsize + 1);
    if (!ptr) {
        printf("Not enough memory (realloc failed)\n");
        return 0;
    }

    mem->memory = ptr;
    memcpy(&(mem->memory[mem->size]), contents, realsize);
    mem->size += realsize;
    mem->memory[mem->size] = 0;

    return realsize;
}

void fetch_stock_price(CURL *curl_handle, const char *symbol) {
    struct MemoryStruct chunk;
    chunk.memory = malloc(1); 
    chunk.size = 0; 

    char url[256];
    snprintf(url, sizeof(url), "https://query1.finance.yahoo.com/v7/finance/quote?symbols=%s", symbol);

    curl_easy_setopt(curl_handle, CURLOPT_URL, url);
    
    // IMPORTANT: User-Agent is required to prevent 403 Forbidden errors
    curl_easy_setopt(curl_handle, CURLOPT_USERAGENT, "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/114.0.0.0 Safari/537.36");
    
    curl_easy_setopt(curl_handle, CURLOPT_WRITEFUNCTION, WriteMemoryCallback);
    curl_easy_setopt(curl_handle, CURLOPT_WRITEDATA, (void *)&chunk);

    CURLcode res = curl_easy_perform(curl_handle);

    if (res != CURLE_OK) {
        fprintf(stderr, "curl_easy_perform() failed: %s\n", curl_easy_strerror(res));
    } else {
        // Parse the JSON response
        cJSON *json = cJSON_Parse(chunk.memory);
        if (json == NULL) {
            printf("Error parsing JSON for %s\n", symbol);
        } else {
            cJSON *quoteResponse = cJSON_GetObjectItemCaseSensitive(json, "quoteResponse");
            cJSON *quotesArray = cJSON_GetObjectItemCaseSensitive(quoteResponse, "result");
            cJSON *firstQuote = cJSON_GetArrayItem(quotesArray, 0);

            if (firstQuote) {
                cJSON *price = cJSON_GetObjectItemCaseSensitive(firstQuote, "regularMarketPrice");
                cJSON *currency = cJSON_GetObjectItemCaseSensitive(firstQuote, "currency");
                cJSON *change = cJSON_GetObjectItemCaseSensitive(firstQuote, "regularMarketChange");

                if (cJSON_IsNumber(price)) {
                    double price_val = price->valuedouble;
                    double change_val = change ? change->valuedouble : 0.0;
                    
                    printf("%-10s | Price: %8.2f %-3s | Change: %+.2f\n", 
                           symbol, price_val, currency ? currency->valuestring : "USD", change_val);
                }
            } else {
                printf("%-10s | Symbol not found.\n", symbol);
            }
            cJSON_Delete(json);
        }
    }

    free(chunk.memory);
}

int main() {
    CURL *curl_handle;
    CURLcode res;

    // List of stocks to track
    const char *symbols[] = {"AAPL", "GOOGL", "MSFT", "TSLA", "AMZN"};
    int num_symbols = sizeof(symbols) / sizeof(symbols[0]);

    curl_global_init(CURL_GLOBAL_ALL);
    curl_handle = curl_easy_init();

    if (!curl_handle) {
        fprintf(stderr, "Failed to initialize CURL\n");
        return 1;
    }

    while (1) {
        // Clear terminal screen
        printf("\033[H\033[J"); 
        printf("--- Live Stock Dashboard (Yahoo Finance) ---\n");
        printf("------------------------------------------------------------\n");
        printf("%-10s | %-15s | %-10s\n", "Symbol", "Price", "Change");
        printf("------------------------------------------------------------\n");

        for (int i = 0; i < num_symbols; i++) {
            fetch_stock_price(curl_handle, symbols[i]);
        }

        printf("------------------------------------------------------------\n");
        printf("Refreshing every 10 seconds... (Press Ctrl+C to exit)\n");
        
        sleep(10); 
    }

    curl_easy_cleanup(curl_handle);
    curl_global_cleanup();

    return 0;
}
