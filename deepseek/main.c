#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <curl/curl.h>
#include "cJSON.h"

// Structure to store HTTP response
struct MemoryStruct {
    char *memory;
    size_t size;
};

// Callback function for writing received data
static size_t WriteMemoryCallback(void *contents, size_t size, size_t nmemb, void *userp) {
    size_t realsize = size * nmemb;
    struct MemoryStruct *mem = (struct MemoryStruct *)userp;

    char *ptr = realloc(mem->memory, mem->size + realsize + 1);
    if(!ptr) {
        printf("Not enough memory (realloc returned NULL)\n");
        return 0;
    }

    mem->memory = ptr;
    memcpy(&(mem->memory[mem->size]), contents, realsize);
    mem->size += realsize;
    mem->memory[mem->size] = 0;

    return realsize;
}

// Fetch stock data from Yahoo Finance
char* fetch_stock_data(const char* symbol) {
    CURL *curl;
    CURLcode res;
    struct MemoryStruct chunk;

    chunk.memory = malloc(1);
    chunk.size = 0;

    curl_global_init(CURL_GLOBAL_DEFAULT);
    curl = curl_easy_init();

    if(curl) {
        char url[256];
        snprintf(url, sizeof(url), "https://query1.finance.yahoo.com/v8/finance/chart/%s", symbol);

        curl_easy_setopt(curl, CURLOPT_URL, url);
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteMemoryCallback);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, (void *)&chunk);
        curl_easy_setopt(curl, CURLOPT_USERAGENT, "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/91.0.4472.124 Safari/537.36");
        curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
        curl_easy_setopt(curl, CURLOPT_TIMEOUT, 10L);

        res = curl_easy_perform(curl);

        if(res != CURLE_OK) {
            fprintf(stderr, "curl_easy_perform() failed: %s\n", curl_easy_strerror(res));
            free(chunk.memory);
            chunk.memory = NULL;
        }

        curl_easy_cleanup(curl);
    }

    curl_global_cleanup();
    return chunk.memory;
}

// Parse JSON and extract stock information
void parse_stock_data(const char* json_data, const char* symbol) {
    cJSON *json = cJSON_Parse(json_data);
    if (json == NULL) {
        const char *error_ptr = cJSON_GetErrorPtr();
        if (error_ptr != NULL) {
            fprintf(stderr, "Error parsing JSON for %s: %s\n", symbol, error_ptr);
        }
        return;
    }

    // Navigate through the JSON structure
    cJSON *chart = cJSON_GetObjectItemCaseSensitive(json, "chart");
    if (cJSON_IsObject(chart)) {
        cJSON *result = cJSON_GetObjectItemCaseSensitive(chart, "result");
        if (cJSON_IsArray(result) && cJSON_GetArraySize(result) > 0) {
            cJSON *first_result = cJSON_GetArrayItem(result, 0);
            
            // Get meta data
            cJSON *meta = cJSON_GetObjectItemCaseSensitive(first_result, "meta");
            if (cJSON_IsObject(meta)) {
                cJSON *currency = cJSON_GetObjectItemCaseSensitive(meta, "currency");
                cJSON *regularMarketPrice = cJSON_GetObjectItemCaseSensitive(meta, "regularMarketPrice");
                cJSON *previousClose = cJSON_GetObjectItemCaseSensitive(meta, "previousClose");
                cJSON *chartPreviousClose = cJSON_GetObjectItemCaseSensitive(meta, "chartPreviousClose");
                cJSON *exchangeName = cJSON_GetObjectItemCaseSensitive(meta, "exchangeName");
                
                if (cJSON_IsString(currency) && cJSON_IsNumber(regularMarketPrice)) {
                    double current_price = regularMarketPrice->valuedouble;
                    double prev_close = previousClose ? previousClose->valuedouble : 
                                      (chartPreviousClose ? chartPreviousClose->valuedouble : 0);
                    
                    double change = current_price - prev_close;
                    double change_percent = prev_close != 0 ? (change / prev_close) * 100 : 0;
                    
                    const char *exchange = exchangeName && cJSON_IsString(exchangeName) ? 
                                         exchangeName->valuestring : "N/A";
                    
                    // Color coding for positive/negative changes
                    const char *color_code = change >= 0 ? "\033[32m" : "\033[31m";
                    const char *reset_code = "\033[0m";
                    
                    printf("┌─────────────────────────────────────────────────────┐\n");
                    printf("│ %-15s: %-30s │\n", "Symbol", symbol);
                    printf("│ %-15s: %-30s │\n", "Exchange", exchange);
                    printf("│ %-15s: %s%-10.2f %-10s%s │\n", "Price", color_code, 
                           current_price, currency->valuestring, reset_code);
                    printf("│ %-15s: %s%-10.2f %-10s%s │\n", "Change", color_code, 
                           change, currency->valuestring, reset_code);
                    printf("│ %-15s: %s%-9.2f%%%-12s%s │\n", "Change %", color_code, 
                           change_percent, "", reset_code);
                    printf("│ %-15s: %-30.2f │\n", "Previous Close", prev_close);
                    printf("└─────────────────────────────────────────────────────┘\n");
                }
            }
        } else {
            printf("No data available for symbol: %s\n", symbol);
        }
    }
    
    cJSON_Delete(json);
}

// Clear terminal screen
void clear_screen() {
    printf("\033[2J\033[1;1H");
}

int main() {
    // Default stock symbols - you can modify this list
    const char* symbols[] = {
        "AAPL",     // Apple
        "GOOGL",    // Alphabet (Google)
        "MSFT",     // Microsoft
        "TSLA",     // Tesla
        "AMZN",     // Amazon
        "META",     // Meta (Facebook)
        "NVDA",     // NVIDIA
        "NFLX"      // Netflix
    };
    
    int num_symbols = sizeof(symbols) / sizeof(symbols[0]);
    
    printf("Stock Market Dashboard - Live Prices\n");
    printf("====================================\n\n");
    
    while(1) {
        clear_screen();
        printf("🔄 Updating stock prices...\n\n");
        
        for(int i = 0; i < num_symbols; i++) {
            char* json_data = fetch_stock_data(symbols[i]);
            if(json_data != NULL) {
                parse_stock_data(json_data, symbols[i]);
                free(json_data);
            } else {
                printf("Failed to fetch data for %s\n", symbols[i]);
            }
            printf("\n");
        }
        
        printf("Next update in 30 seconds... (Press Ctrl+C to exit)\n");
        sleep(30); // Wait 30 seconds before next update
    }
    
    return 0;
}
