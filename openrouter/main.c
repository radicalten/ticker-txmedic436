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

    curl_handle = curl_easy_init();
    if (!curl_handle) {
        free(chunk.memory);
        return;
    }

    // Construct the URL
    char url[512];
    snprintf(url, sizeof(url), "https://query1.finance.yahoo.com/v7/finance/quote?symbols=%s", symbols);

    curl_easy_setopt(curl_handle, CURLOPT_URL, url);
    curl_easy_setopt(curl_handle, CURLOPT_WRITEFUNCTION, WriteMemoryCallback);
    curl_easy_setopt(curl_handle, CURLOPT_WRITEDATA, (void *)&chunk);
    
    // User-Agent is mandatory for Yahoo Finance
    curl_easy_setopt(curl_handle, CURLOPT_USERAGENT, "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/91.0.4472.124 Safari/537.36");
    // Follow redirects if necessary
    curl_easy_setopt(curl_handle, CURLOPT_FOLLOWLOCATION, 1L);

    res = curl_easy_perform(curl_handle);

    if (res != CURLE_OK) {
        fprintf(stderr, "curl_easy_perform() failed: %s\n", curl_easy_strerror(res));
    } else {
        cJSON *json = cJSON_Parse(chunk.memory);
        if (json == NULL) {
            printf("Error parsing JSON: %s\n", cJSON_GetErrorPtr());
        } else {
            // CORRECT PATH: quoteResponse -> result (array)
            cJSON *quoteResponse = cJSON_GetObjectItemCaseSensitive(json, "quoteResponse");
            if (quoteResponse == NULL) {
                printf("Error: 'quoteResponse' not found in JSON\n");
            } else {
                cJSON *resultArray = cJSON_GetObjectItemCaseSensitive(quoteResponse, "result");
                
                if (cJSON_IsArray(resultArray)) {
                    // Clear terminal screen
                    printf("\033[H\033[J");
                    printf("====================================================\n");
                    printf("%-10s | %-10s | %-10s\n", "SYMBOL", "PRICE", "CHANGE %");
                    printf("----------------------------------------------------\n");

                    cJSON *stock = NULL;
                    cJSON_ArrayForEach(stock, resultArray) {
                        cJSON *symbol = cJSON_GetObjectItemCaseSensitive(stock, "symbol");
                        cJSON *price = cJSON_GetObjectItemCaseSensitive(stock, "regularMarketPrice");
                        cJSON *changePercent = cJSON_GetObjectItemCaseSensitive(stock, "regularMarketChangePercent");

                        // Robust checks to ensure data exists before printing
                        if (cJSON_IsString(symbol) && cJSON_IsNumber(price)) {
                            const char *sym_str = symbol->valuestring;
                            double p_val = price->valuedouble;
                            double cp_val = (cJSON_IsNumber(changePercent)) ? changePercent->valuedouble : 0.0;

                            // Color coding: Green for positive, Red for negative
                            const char *color = (cp_val >= 0) ? "\033[0;32m" : "\033[0;31m";
                            
                            printf("%-10s | %-10.2f | %s%+.2f%%\033[0m\n", 
                                   sym_str, 
                                   p_val, 
                                   color,
                                   cp_val);
                        }
                    }
                    printf("----------------------------------------------------\n");
                    printf("Refreshing every 10 seconds... (Ctrl+C to quit)\n");
                } else {
                    printf("Error: 'result' array not found.\n");
                }
            }
            cJSON_Delete(json);
        }
    }

    // Cleanup
    curl_easy_cleanup(curl_handle);
    free(chunk.memory);
}

int main() {
    const char *my_stocks = "AAPL,MSFT,GOOGL,TSLA,AMZN,NVDA";

    // Global init should only happen once
    curl_global_init(CURL_GLOBAL_ALL);

    while (1) {
        fetch_and_display_stocks(my_stocks);
        sleep(10); 
    }

    curl_global_cleanup();
    return 0;
}
