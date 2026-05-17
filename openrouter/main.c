#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <curl/curl.h>
#include "cJSON.h"

// Define the stocks you want to track
#define SYMBOLS "AAPL,MSFT,TSLA,NVDA"
#define REFRESH_RATE 5 // Seconds

// Structure to hold the HTTP response data
struct MemoryStruct {
    char *memory;
    size_t size;
};

// Callback function for libcurl to write received data into our MemoryStruct
static size_t WriteMemoryCallback(void *contents, size_t size, size_t nmemb, void *userp) {
    size_t realsize = size * nmemb;
    struct MemoryStruct *mem = (struct MemoryStruct *)userp;

    char *ptr = realloc(mem->memory, mem->size + realsize + 1);
    if(ptr == NULL) {
        printf("Not enough memory (realloc returned NULL)\n");
        return 0;
    }

    mem->memory = ptr;
    memcpy(&(mem->memory[mem->size]), contents, realsize);
    mem->size += realsize;
    mem->memory[mem->size] = 0;

    return realsize;
}

void fetch_and_display_stocks(CURL *curl) {
    CURLcode res;
    struct MemoryStruct chunk;
    chunk.memory = malloc(1);
    chunk.size = 0;

    // Construct the URL
    char url[256];
    snprintf(url, sizeof(url), "https://query1.finance.yahoo.com/v7/finance/quote?symbols=%s", SYMBOLS);

    // Configure libcurl options
    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteMemoryCallback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, (void *)&chunk);
    
    // Set a standard User-Agent so Yahoo Finance doesn't reject the request
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/115.0.0.0 Safari/537.36");

    // Perform the request
    res = curl_easy_perform(curl);

    if(res != CURLE_OK) {
        fprintf(stderr, "curl_easy_perform() failed: %s\n", curl_easy_strerror(res));
    } else {
        // Parse the JSON response
        cJSON *json = cJSON_Parse(chunk.memory);
        if (json == NULL) {
            const char *error_ptr = cJSON_GetErrorPtr();
            if (error_ptr != NULL) {
                fprintf(stderr, "Error before: %s\n", error_ptr);
            }
        } else {
            // Yahoo Finance JSON Structure:
            // { "quoteResponse": { "result": [ { "symbol": "AAPL", "regularMarketPrice": 150.0, ... } ] } }
            
            cJSON *quoteResponse = cJSON_GetObjectItemCaseSensitive(json, "quoteResponse");
            if (quoteResponse) {
                cJSON *result = cJSON_GetObjectItemCaseSensitive(quoteResponse, "result");
                
                if (result && cJSON_IsArray(result)) {
                    // ANSI escape codes to clear the terminal and move cursor to top left
                    printf("\033[2J\033[1;1H");
                    printf("=== LIVE STOCK DASHBOARD ===\n");
                    printf("Symbol\t\tPrice (USD)\n");
                    printf("---------------------------\n");

                    cJSON *stock = NULL;
                    cJSON_ArrayForEach(stock, result) {
                        cJSON *symbol = cJSON_GetObjectItemCaseSensitive(stock, "symbol");
                        cJSON *price = cJSON_GetObjectItemCaseSensitive(stock, "regularMarketPrice");
                        
                        if (cJSON_IsString(symbol) && (symbol->valuestring != NULL) && cJSON_IsNumber(price)) {
                            printf("%-10s\t$%.2f\n", symbol->valuestring, price->valuedouble);
                        }
                    }
                    printf("---------------------------\n");
                    printf("Refreshing every %d seconds. Press Ctrl+C to exit.\n", REFRESH_RATE);
                }
            }
            cJSON_Delete(json);
        }
    }
    free(chunk.memory);
}

int main(void) {
    CURL *curl;

    // Initialize libcurl
    curl_global_init(CURL_GLOBAL_ALL);
    curl = curl_easy_init();

    if(curl) {
        while(1) {
            fetch_and_display_stocks(curl);
            sleep(REFRESH_RATE);
        }
        // Cleanup libcurl (won't be reached unless loop breaks)
        curl_easy_cleanup(curl);
    }

    curl_global_cleanup();
    return 0;
}
