// main.c
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <curl/curl.h>
#include "cJSON.h"

#define BUFFER_SIZE 8192
#define REFRESH_INTERVAL 5 // seconds

// Stock data structure
typedef struct {
    char symbol[10];
    char currency[10];
    char marketTime[50];
    char regularMarketPrice[50];
    char regularMarketChange[50];
    char regularMarketChangePercent[50];
} StockData;

// Callback function for CURL
static size_t write_callback(void *contents, size_t size, size_t nmemb, void *userp) {
    size_t realsize = size * nmemb;
    char *ptr = userp;
    ptr = realloc(ptr, realsize + 1);
    if(ptr == NULL) {
        printf("Not enough memory!\n");
        return 0;
    }
    memcpy(ptr, contents, realsize);
    ptr[realsize] = 0;
    *(char **)userp = ptr;
    return realsize;
}

// Fetch stock data from Yahoo Finance
StockData* fetch_stock_data(const char* symbols) {
    CURL *curl;
    CURLcode res;
    char *url;
    char *response;
    
    // Create URL with multiple symbols
    char symbols_clean[100] = {0};
    snprintf(symbols_clean, sizeof(symbols_clean), "symbols=%s", symbols);
    
    // URL encode symbols
    CURL *temp_curl = curl_easy_init();
    char *encoded_symbols = curl_easy_escape(temp_curl, symbols_clean, 0);
    curl_easy_cleanup(temp_curl);
    
    // Construct the full URL
    url = malloc(500);
    if(!url) return NULL;
    
    snprintf(url, 500, "https://query1.finance.yahoo.com/v7/finance/quote?%s", 
             encoded_symbols ? encoded_symbols : symbols_clean);
    
    if(encoded_symbols) curl_free(encoded_symbols);
    
    response = malloc(BUFFER_SIZE);
    if(!response) {
        free(url);
        return NULL;
    }
    response[0] = '\0';
    
    curl = curl_easy_init();
    if(curl) {
        struct curl_slist *headers = NULL;
        
        // Set user agent to avoid rejection
        headers = curl_slist_append(headers, "User-Agent: Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/91.0.4472.124 Safari/537.36");
        headers = curl_slist_append(headers, "Accept: application/json");
        headers = curl_slist_append(headers, "Accept-Language: en-US,en;q=0.9");
        
        curl_easy_setopt(curl, CURLOPT_URL, url);
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_callback);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, (void *)&response);
        curl_easy_setopt(curl, CURLOPT_TIMEOUT, 10L);
        
        // Follow redirects
        curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
        
        res = curl_easy_perform(curl);
        
        if(res != CURLE_OK) {
            fprintf(stderr, "curl_easy_perform() failed: %s\n", curl_easy_strerror(res));
        }
        
        curl_slist_free_all(headers);
        curl_easy_cleanup(curl);
    }
    
    free(url);
    
    if(!response[0]) {
        free(response);
        return NULL;
    }
    
    // Parse JSON
    StockData *stocks = parse_json_response(response);
    free(response);
    
    return stocks;
}

// Parse JSON response
StockData* parse_json_response(const char* json_str) {
    cJSON *json = cJSON_Parse(json_str);
    cJSON *result = NULL;
    cJSON *quote = NULL;
    
    if(!json) {
        fprintf(stderr, "Failed to parse JSON\n");
        return NULL;
    }
    
    result = cJSON_DetachItemFromObject(json, "quoteResponse");
    if(!result || !cJSON_IsObject(result)) {
        fprintf(stderr, "No quoteResponse found\n");
        cJSON_Delete(json);
        return NULL;
    }
    
    quote = cJSON_DetachItemFromObject(result, "result");
    if(!quote || !cJSON_IsArray(quote)) {
        fprintf(stderr, "No results array\n");
        cJSON_Delete(result);
        cJSON_Delete(json);
        return NULL;
    }
    
    // We'll return a single stock for simplicity (first result)
    // In a real app, you'd iterate through all results
    cJSON *first = cJSON_DetachItemFromArray(quote, 0);
    
    StockData *stock = malloc(sizeof(StockData));
    if(!stock) {
        cJSON_Delete(json);
        return NULL;
    }
    
    memset(stock, 0, sizeof(StockData));
    
    if(first) {
        if(cJSON_HasKey(first, "symbol")) {
            strncpy(stock->symbol, cJSON_GetStringValue(cJSON_DetachItemFromObject(first, "symbol")), 9);
        }
        if(cJSON_HasKey(first, "currency")) {
            strncpy(stock->currency, cJSON_GetStringValue(cJSON_DetachItemFromObject(first, "currency")), 9);
        }
        if(cJSON_HasKey(first, "regularMarketPrice")) {
            snprintf(stock->regularMarketPrice, sizeof(stock->regularMarketPrice), "%.2f", 
                     cJSON_GetNumberValue(cJSON_DetachItemFromObject(first, "regularMarketPrice")));
        }
        if(cJSON_HasKey(first, "regularMarketChange")) {
            snprintf(stock->regularMarketChange, sizeof(stock->regularMarketChange), "%.2f", 
                     cJSON_GetNumberValue(cJSON_DetachItemFromObject(first, "regularMarketChange")));
        }
        if(cJSON_HasKey(first, "regularMarketChangePercent")) {
            snprintf(stock->regularMarketChangePercent, sizeof(stock->regularMarketChangePercent), "%.2f%%", 
                     cJSON_GetNumberValue(cJSON_DetachItemFromObject(first, "regularMarketChangePercent")));
        }
        if(cJSON_HasKey(first, "marketTime")) {
            strncpy(stock->marketTime, cJSON_GetStringValue(cJSON_DetachItemFromObject(first, "marketTime")), 49);
        }
        
        cJSON_Delete(first);
    }
    
    cJSON_Delete(quote);
    cJSON_Delete(result);
    cJSON_Delete(json);
    
    return stock;
}

// Display the dashboard
void display_dashboard(StockData* stock) {
    printf("\033[2J\033[H"); // Clear screen
    
    printf("╔══════════════════════════════════════════════════════════════════╗\n");
    printf("║              LIVE STOCK PRICE DASHBOARD                         ║\n");
    printf("╚══════════════════════════════════════════════════════════════════╝\n\n");
    
    printf("  Symbol: %s\n", stock->symbol);
    printf("  Currency: %s\n", stock->currency);
    printf("  Price: %s %s\n", stock->regularMarketPrice, stock->currency);
    
    // Format change display with color
    printf("  Change: ");
    float change_val = atof(stock->regularMarketChange);
    if(change_val >= 0) {
        printf("\033[32m+%s %s\033[0m\n", stock->regularMarketChange, stock->regularMarketChangePercent);
    } else {
        printf("\033[31m%s %s\033[0m\n", stock->regularMarketChange, stock->regularMarketChangePercent);
    }
    
    printf("  Market Time: %s\n", stock->marketTime);
    
    printf("\n  Press Ctrl+C to exit\n");
}

int main(int argc, char *argv[]) {
    CURL *curl;
    char *symbols = NULL;
    
    // Initialize cURL
    curl_global_init(CURL_GLOBAL_DEFAULT);
    
    // Get symbols from command line or use default
    if(argc > 1) {
        symbols = argv[1];
    } else {
        symbols = "AAPL";
    }
    
    printf("Fetching stock data for: %s\n", symbols);
    printf("Press Ctrl+C to exit...\n\n");
    
    // Initial delay
    sleep(1);
    
    while(1) {
        StockData *stock = fetch_stock_data(symbols);
        
        if(stock) {
            display_dashboard(stock);
            free(stock);
        } else {
            printf("\nFailed to fetch data. Retrying...\n");
        }
        
        printf("\nNext update in %d seconds...\n", REFRESH_INTERVAL);
        sleep(REFRESH_INTERVAL);
    }
    
    curl_global_cleanup();
    return 0;
}
