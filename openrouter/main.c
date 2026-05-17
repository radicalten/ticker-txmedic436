#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <signal.h>
#include <curl/curl.h>
#include "cJSON.h"

// Configuration
#define REFRESH_INTERVAL 5  // seconds
#define MAX_STOCKS 16

// Structure to handle curl response memory
struct MemoryStruct {
    char *memory;
    size_t size;
};

// Stock data structure
typedef struct {
    char symbol[16];
    double price;
    double change;
    double change_percent;
    int has_data;
} StockData;

static volatile int running = 1;

void signal_handler(int sig) {
    running = 0;
}

// CORRECTED: Callback function that actually appends data
static size_t write_callback(void *contents, size_t size, size_t nmemb, void *userp) {
    size_t realsize = size * nmemb;
    struct MemoryStruct *mem = (struct MemoryStruct *)userp;

    char *ptr = realloc(mem->memory, mem->size + realsize + 1);
    if (!ptr) return 0; // out of memory!

    mem->memory = ptr;
    memcpy(&(mem->memory[mem->size]), contents, realsize);
    mem->size += realsize;
    mem->memory[mem->size] = 0;

    return realsize;
}

// Fetch stock data from Yahoo Finance
int fetch_stock_data(const char *symbols, struct MemoryStruct *chunk) {
    CURL *curl;
    CURLcode res;
    char url[512];
    
    snprintf(url, sizeof(url), 
             "https://query1.finance.yahoo.com/v7/finance/quote?symbols=%s", 
             symbols);
    
    curl = curl_easy_init();
    if (!curl) return -1;
    
    chunk->memory = malloc(1); 
    chunk->size = 0; 
    
    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_callback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, (void *)chunk);
    
    // Yahoo Finance requires a realistic User-Agent to avoid 403 Forbidden errors
    curl_easy_setopt(curl, CURLOPT_USERAGENT, 
                     "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/120.0.0.0 Safari/537.36");
    
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 10L);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    
    res = curl_easy_perform(curl);
    curl_easy_cleanup(curl);
    
    return (res == CURLE_OK) ? 0 : -1;
}

int parse_stock_data(const char *json_response, StockData *stocks, int max_stocks) {
    cJSON *json = cJSON_Parse(json_response);
    if (!json) return -1;
    
    cJSON *quoteResponse = cJSON_GetObjectItemCaseSensitive(json, "quoteResponse");
    if (!quoteResponse) {
        cJSON_Delete(json);
        return -1;
    }
    
    cJSON *result = cJSON_GetObjectItemCaseSensitive(quoteResponse, "result");
    if (!result || !cJSON_IsArray(result)) {
        cJSON_Delete(json);
        return -1;
    }
    
    int count = 0;
    cJSON *item = NULL;
    cJSON_ArrayForEach(item, result) {
        if (count >= max_stocks) break;
        
        cJSON *symbol = cJSON_GetObjectItemCaseSensitive(item, "symbol");
        cJSON *price = cJSON_GetObjectItemCaseSensitive(item, "regularMarketPrice");
        cJSON *change = cJSON_GetObjectItemCaseSensitive(item, "regularMarketChange");
        cJSON *changePercent = cJSON_GetObjectItemCaseSensitive(item, "regularMarketChangePercent");
        
        if (symbol && price) {
            strncpy(stocks[count].symbol, symbol->valuestring, sizeof(stocks[count].symbol) - 1);
            stocks[count].symbol[sizeof(stocks[count].symbol) - 1] = '\0';
            stocks[count].price = cJSON_GetNumberValue(price);
            stocks[count].change = change ? cJSON_GetNumberValue(change) : 0.0;
            stocks[count].change_percent = changePercent ? cJSON_GetNumberValue(changePercent) : 0.0;
            stocks[count].has_data = 1;
            count++;
        }
    }
    
    cJSON_Delete(json);
    return count;
}

void clear_screen() {
    printf("\033[2J\033[H");
    fflush(stdout);
}

void display_dashboard(StockData *stocks, int count, time_t last_update) {
    clear_screen();
    
    printf("\n");
    printf("╔══════════════════════════════════════════════════════════════════════╗\n");
    printf("║                     📈 LIVE STOCK DASHBOARD 📈                      ║\n");
    printf("╠══════════════════════════════════════════════════════════════════════╣\n");
    printf("║ %-8s │ %12s │ %10s │ %12s ║\n", "SYMBOL", "PRICE", "CHANGE", "CHANGE %%");
    printf("╟─────────┼──────────────┼────────────┼────────────══╢\n");
    
    for (int i = 0; i < count; i++) {
        if (stocks[i].has_data) {
            // FIXED: Actually using the color variables in the printf
            const char *color = stocks[i].change >= 0 ? "\033[32m" : "\033[31m";
            const char *reset = "\033[0m";
            
            printf("║ %-8s │ %12.2f │ %s%10.2f%s │ %s%11.2f%%%s ║\n",
                   stocks[i].symbol,
                   stocks[i].price,
                   color, stocks[i].change, reset,
                   color, stocks[i].change_percent, reset);
        }
    }
    
    printf("╚══════════════════════════════════════════════════════════════════════╝\n");
    
    struct tm *tm_info = localtime(&last_update);
    char time_str[64];
    strftime(time_str, sizeof(time_str), "%Y-%m-%d %H:%M:%S", tm_info);
    
    printf("\n🔄 Updating every %d seconds | Last update: %s | Press Ctrl+C to exit\n", 
           REFRESH_INTERVAL, time_str);
}

int main(int argc, char *argv[]) {
    const char *symbols = "AAPL,GOOGL,MSFT,AMZN,TSLA,META,NVDA,JPM";
    if (argc > 1) symbols = argv[1];
    
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);
    
    if (curl_global_init(CURL_GLOBAL_DEFAULT) != CURLE_OK) {
        fprintf(stderr, "Failed to initialize libcurl\n");
        return 1;
    }
    
    StockData stocks[MAX_STOCKS];
    memset(stocks, 0, sizeof(stocks));
    
    while (running) {
        struct MemoryStruct chunk;
        if (fetch_stock_data(symbols, &chunk) == 0) {
            int count = parse_stock_data(chunk.memory, stocks, MAX_STOCKS);
            if (count > 0) {
                display_dashboard(stocks, count, time(NULL));
            } else {
                printf("\033[31mError: Failed to parse JSON data\033[0m\n");
            }
            free(chunk.memory);
        } else {
            printf("\033[31mError: Failed to fetch data from Yahoo Finance\033[0m\n");
        }
        
        if (running) sleep(REFRESH_INTERVAL);
    }
    
    curl_global_cleanup();
    printf("\nDashboard stopped.\n");
    return 0;
}
