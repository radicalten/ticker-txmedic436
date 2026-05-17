#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <curl/curl.h>
#include "cJSON.h"

// Structure to handle dynamic memory for CURL response
struct MemoryStruct {
    char *memory;
    size_t size;
};

// Callback function for CURL to write data into our memory struct
static size_t WriteMemoryCallback(void *contents, size_t size, size_t nmemb, void *userp) {
    size_t realsize = size * nmemb;
    struct MemoryStruct *mem = (struct MemoryStruct *)userp;

    char *ptr = realloc(mem->memory, mem->size + realsize + 1);
    if (ptr == NULL) {
        printf("Not enough memory (realloc returned NULL)\n");
        return 0;
    }

    mem->memory = ptr;
    memcpy(&(mem->memory[mem->size]), contents, realsize);
    mem->size += realsize;
    mem->memory[mem->size] = 0;

    return realsize;
}

void fetch_and_display_stocks(const char *symbols) {
    CURL *curl_handle;
    CURLcode res;
    struct MemoryStruct chunk;

    chunk.memory = malloc(1); 
    chunk.size = 0; 

    curl_global_init(CURL_GLOBAL_ALL);
    curl_handle = curl_easy_init();

    // Construct the URL
    char url[512];
    snprintf(url, sizeof(url), "https://query1.finance.yahoo.com/v7/finance/quote?%s", symbols);

    curl_easy_setopt(curl_handle, CURLOPT_URL, url);
    curl_easy_setopt(curl_handle, CURLOPT_WRITEFUNCTION, WriteMemoryCallback);
    curl_easy_setopt(curl_handle, CURLOPT_WRITEDATA, (void *)&chunk);
    
    // IMPORTANT: User-Agent to prevent 403 Forbidden errors
    curl_easy_setopt(curl_handle, CURLOPT_USERAGENT, "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/91.0.4472.124 Safari/537.36");

    res = curl_easy_perform(curl_handle);

    if (res != CURLE_OK) {
        fprintf(stderr, "curl_easy_perform() failed: %s\n", curl_easy_strerror(res));
    } else {
        // Parse the JSON response
        cJSON *json = cJSON_Parse(chunk.memory);
        if (json == NULL) {
            printf("Error parsing JSON.\n");
        } else {
            // Yahoo JSON Structure: quoteResponse -> quote -> result (array)
            cJSON *quoteResponse = cJSON_GetObjectItemCaseSensitive(json, "quoteResponse");
            cJSON *quote = cJSON_GetObjectItemCaseSensitive(quoteResponse, "quote");
            cJSON *resultArray = cJSON_GetObjectItemCaseSensitive(quote, "result");

            // Clear terminal screen (ANSI escape code)
            printf("\033[H\033[J");
            printf("====================================================\n");
            printf("%-10s | %-10s | %-10s\n", "SYMBOL", "PRICE", "CHANGE %");
            printf("----------------------------------------------------\n");

            cJSON *stock = NULL;
            cJSON_ArrayForEach(stock, resultArray) {
                cJSON *symbol = cJSON_GetObjectItemCaseSensitive(stock, "symbol");
                cJSON *price = cJSON_GetObjectItemCaseSensitive(stock, "regularMarketPrice");
                cJSON *changePercent = cJSON_GetObjectItemCaseSensitive(stock, "regularMarketChangePercent");

                if (cJSON_IsString(symbol) && cJSON_IsNumber(price)) {
                    // Color coding: Green for positive, Red for negative
                    const char *color = (cJSON_IsNumber(changePercent) && changePercent->valuedouble >= 0) ? "\033[0;32m" : "\033[0;31m";
                    
                    printf("%-10s | %-10.2f | %s%+.2f%%\033[0m\n", 
                           symbol->valuestring, 
                           price->valuedouble, 
                           color,
                           changePercent ? changePercent->valuedouble : 0.0);
                }
            }
            printf("----------------------------------------------------\n");
            printf("Refreshing every 10 seconds... (Ctrl+C to quit)\n");
            
            cJSON_Delete(json);
        }
    }

    // Cleanup
    curl_easy_cleanup(curl_handle);
    free(chunk.memory);
    curl_global_cleanup();
}

int main() {
    // Define the stocks you want to track, comma-separated
    const char *my_stocks = "AAPL,MSFT,GOOGL,TSLA,AMZN,NVDA";

    while (1) {
        fetch_and_display_stocks(my_stocks);
        sleep(10); // Wait 10 seconds before next update
    }

    return 0;
}
