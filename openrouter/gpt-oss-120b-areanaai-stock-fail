#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <curl/curl.h>
#include <unistd.h>
#include <time.h>
#include "cJSON.h"

#define MAX_STOCKS 10
#define REFRESH_INTERVAL 10  // seconds

// Structure to hold HTTP response
typedef struct {
    char *memory;
    size_t size;
} MemoryStruct;

// Structure to hold stock data
typedef struct {
    char symbol[16];
    double price;
    double change;
    double changePercent;
    char marketState[32];
    int valid;
} StockData;

// Callback function for libcurl
static size_t WriteMemoryCallback(void *contents, size_t size, size_t nmemb, void *userp) {
    size_t realsize = size * nmemb;
    MemoryStruct *mem = (MemoryStruct *)userp;

    char *ptr = realloc(mem->memory, mem->size + realsize + 1);
    if(!ptr) {
        printf("Error: Not enough memory\n");
        return 0;
    }

    mem->memory = ptr;
    memcpy(&(mem->memory[mem->size]), contents, realsize);
    mem->size += realsize;
    mem->memory[mem->size] = 0;

    return realsize;
}

// Fetch stock data from Yahoo Finance
int fetchStockData(const char *symbol, StockData *stock) {
    CURL *curl;
    CURLcode res;
    MemoryStruct chunk;
    char url[512];
    int success = 0;
    
    chunk.memory = malloc(1);
    chunk.size = 0;
    
    // Yahoo Finance API endpoint
    snprintf(url, sizeof(url), 
             "https://query1.finance.yahoo.com/v8/finance/chart/%s?interval=1d&range=1d", 
             symbol);
    
    curl = curl_easy_init();
    if(!curl) {
        free(chunk.memory);
        return 0;
    }
    
    // Set up headers with User-Agent to avoid rejection
    struct curl_slist *headers = NULL;
    headers = curl_slist_append(headers, 
        "User-Agent: Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/120.0.0.0 Safari/537.36");
    headers = curl_slist_append(headers, "Accept: application/json");
    headers = curl_slist_append(headers, "Accept-Language: en-US,en;q=0.9");
    
    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteMemoryCallback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, (void *)&chunk);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 10L);
    
    res = curl_easy_perform(curl);
    
    if(res != CURLE_OK) {
        fprintf(stderr, "Error fetching %s: %s\n", symbol, curl_easy_strerror(res));
        goto cleanup;
    }
    
    // Parse JSON response
    cJSON *json = cJSON_Parse(chunk.memory);
    if(!json) {
        fprintf(stderr, "Error parsing JSON for %s\n", symbol);
        goto cleanup;
    }
    
    // Navigate JSON structure: chart -> result[0] -> meta
    cJSON *chart = cJSON_GetObjectItemCaseSensitive(json, "chart");
    if(!chart) {
        cJSON_Delete(json);
        goto cleanup;
    }
    
    cJSON *result = cJSON_GetObjectItemCaseSensitive(chart, "result");
    if(!result || !cJSON_IsArray(result) || cJSON_GetArraySize(result) == 0) {
        cJSON_Delete(json);
        goto cleanup;
    }
    
    cJSON *firstResult = cJSON_GetArrayItem(result, 0);
    cJSON *meta = cJSON_GetObjectItemCaseSensitive(firstResult, "meta");
    
    if(!meta) {
        cJSON_Delete(json);
        goto cleanup;
    }
    
    // Extract price data
    cJSON *regularMarketPrice = cJSON_GetObjectItemCaseSensitive(meta, "regularMarketPrice");
    cJSON *previousClose = cJSON_GetObjectItemCaseSensitive(meta, "previousClose");
    cJSON *marketState = cJSON_GetObjectItemCaseSensitive(meta, "marketState");
    
    if(regularMarketPrice && cJSON_IsNumber(regularMarketPrice)) {
        stock->price = regularMarketPrice->valuedouble;
        
        if(previousClose && cJSON_IsNumber(previousClose)) {
            double prevClose = previousClose->valuedouble;
            stock->change = stock->price - prevClose;
            stock->changePercent = (stock->change / prevClose) * 100.0;
        } else {
            stock->change = 0.0;
            stock->changePercent = 0.0;
        }
        
        if(marketState && cJSON_IsString(marketState)) {
            strncpy(stock->marketState, marketState->valuestring, sizeof(stock->marketState) - 1);
            stock->marketState[sizeof(stock->marketState) - 1] = '\0';
        } else {
            strcpy(stock->marketState, "UNKNOWN");
        }
        
        strncpy(stock->symbol, symbol, sizeof(stock->symbol) - 1);
        stock->symbol[sizeof(stock->symbol) - 1] = '\0';
        stock->valid = 1;
        success = 1;
    }
    
    cJSON_Delete(json);
    
cleanup:
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    free(chunk.memory);
    
    return success;
}

// Print the dashboard
void printDashboard(StockData *stocks, int count) {
    // Clear screen
    printf("\033[2J\033[H");
    
    time_t now;
    time(&now);
    
    printf("\n");
    printf("╔═══════════════════════════════════════════════════════════════════════════╗\n");
    printf("║                 📈  LIVE STOCK PRICE DASHBOARD  📈                       ║\n");
    printf("╚═══════════════════════════════════════════════════════════════════════════╝\n");
    printf("\n");
    printf("┌──────────┬──────────────┬──────────────┬──────────────┬──────────────────┐\n");
    printf("│ SYMBOL   │    PRICE     │    CHANGE    │   CHANGE %%   │     STATUS       │\n");
    printf("├──────────┼──────────────┼──────────────┼──────────────┼──────────────────┤\n");
    
    for(int i = 0; i < count; i++) {
        if(stocks[i].valid) {
            char changeStr[16];
            char changePercentStr[16];
            char priceStr[16];
            const char *arrow = "";
            
            snprintf(priceStr, sizeof(priceStr), "$%.2f", stocks[i].price);
            
            if(stocks[i].change > 0) {
                arrow = "▲";
                snprintf(changeStr, sizeof(changeStr), "+$%.2f", stocks[i].change);
                snprintf(changePercentStr, sizeof(changePercentStr), "+%.2f%%", stocks[i].changePercent);
                printf("│ %-8s │ %12s │ \033[32m%12s\033[0m │ \033[32m%12s\033[0m │ %-16s │\n",
                       stocks[i].symbol, priceStr, changeStr, changePercentStr, stocks[i].marketState);
            } else if(stocks[i].change < 0) {
                arrow = "▼";
                snprintf(changeStr, sizeof(changeStr), "-$%.2f", -stocks[i].change);
                snprintf(changePercentStr, sizeof(changePercentStr), "%.2f%%", stocks[i].changePercent);
                printf("│ %-8s │ %12s │ \033[31m%12s\033[0m │ \033[31m%12s\033[0m │ %-16s │\n",
                       stocks[i].symbol, priceStr, changeStr, changePercentStr, stocks[i].marketState);
            } else {
                snprintf(changeStr, sizeof(changeStr), "$%.2f", stocks[i].change);
                snprintf(changePercentStr, sizeof(changePercentStr), "%.2f%%", stocks[i].changePercent);
                printf("│ %-8s │ %12s │ %12s │ %12s │ %-16s │\n",
                       stocks[i].symbol, priceStr, changeStr, changePercentStr, stocks[i].marketState);
            }
        } else {
            printf("│ %-8s │ %12s │ %12s │ %12s │ %-16s │\n",
                   stocks[i].symbol, "N/A", "N/A", "N/A", "ERROR");
        }
    }
    
    printf("└──────────┴──────────────┴──────────────┴──────────────┴──────────────────┘\n");
    printf("\n");
    printf("Last updated: %s", ctime(&now));
    printf("Refreshing every %d seconds... Press Ctrl+C to exit.\n", REFRESH_INTERVAL);
    printf("\nTip: Run with stock symbols as arguments (e.g., ./stocks AAPL MSFT GOOGL)\n");
}

int main(int argc, char *argv[]) {
    StockData stocks[MAX_STOCKS];
    int stockCount = 0;
    
    // Default stocks if none provided
    const char *defaultSymbols[] = {"AAPL", "GOOGL", "MSFT", "TSLA", "AMZN"};
    int defaultCount = 5;
    
    if(argc > 1) {
        // Use command line arguments as stock symbols
        stockCount = (argc - 1) > MAX_STOCKS ? MAX_STOCKS : (argc - 1);
        for(int i = 0; i < stockCount; i++) {
            strncpy(stocks[i].symbol, argv[i + 1], sizeof(stocks[i].symbol) - 1);
            stocks[i].symbol[sizeof(stocks[i].symbol) - 1] = '\0';
            stocks[i].valid = 0;
        }
    } else {
        // Use default stocks
        stockCount = defaultCount;
        for(int i = 0; i < stockCount; i++) {
            strncpy(stocks[i].symbol, defaultSymbols[i], sizeof(stocks[i].symbol) - 1);
            stocks[i].symbol[sizeof(stocks[i].symbol) - 1] = '\0';
            stocks[i].valid = 0;
        }
    }
    
    // Initialize libcurl
    curl_global_init(CURL_GLOBAL_DEFAULT);
    
    printf("Initializing stock dashboard...\n");
    printf("Monitoring %d stocks\n", stockCount);
    sleep(2);
    
    // Main loop
    while(1) {
        // Fetch data for all stocks
        for(int i = 0; i < stockCount; i++) {
            if(!fetchStockData(stocks[i].symbol, &stocks[i])) {
                stocks[i].valid = 0;
            }
        }
        
        // Display dashboard
        printDashboard(stocks, stockCount);
        
        // Wait before next refresh
        sleep(REFRESH_INTERVAL);
    }
    
    // Cleanup
    curl_global_cleanup();
    
    return 0;
}
