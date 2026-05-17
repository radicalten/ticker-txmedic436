#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <signal.h>
#include <curl/curl.h>
#include "cJSON.h"

#define REFRESH_INTERVAL 5
#define MAX_STOCKS 16

typedef struct {
    char symbol[16];
    double price;
    double change;
    double change_percent;
    int has_data;
} StockData;

// Structure to handle dynamic string growth
struct MemoryStruct {
    char *memory;
    size_t size;
};

static volatile int running = 1;

void signal_handler(int sig) {
    running = 0;
}

// CORRECTED: Write callback now appends data correctly
size_t write_callback(void *contents, size_t size, size_t nmemb, void *userp) {
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

int fetch_stock_data(const char *symbols, struct MemoryStruct *chunk) {
    CURL *curl;
    CURLcode res;
    char url[1024];
    
    // Use the v7 API (Ensure symbols are comma separated)
    snprintf(url, sizeof(url), 
             "https://query1.finance.yahoo.com/v7/finance/quote?symbols=%s", 
             symbols);
    
    curl = curl_easy_init();
    if (!curl) return -1;
    
    // Initialize memory struct
    chunk->memory = malloc(1); 
    chunk->size = 0;    

    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_callback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, (void *)chunk);
    
    // Yahoo Finance REQUIRES a modern User-Agent or it will block the request
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "Mozilla/5.0 (X11; Linux x86_64) AppleWebKit/537.36");
    
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 10L);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);

    res = curl_easy_perform(curl);
    
    if(res != CURLE_OK) {
        fprintf(stderr, "curl_easy_perform() failed: %s\n", curl_easy_strerror(res));
    }

    curl_easy_cleanup(curl);
    return (res == CURLE_OK) ? 0 : -1;
}

int parse_stock_data(const char *json_response, StockData *stocks, int max_stocks) {
    cJSON *json = cJSON_Parse(json_response);
    if (!json) return -1;
    
    cJSON *quoteResponse = cJSON_GetObjectItemCaseSensitive(json, "quoteResponse");
    cJSON *result = quoteResponse ? cJSON_GetObjectItemCaseSensitive(quoteResponse, "result") : NULL;
    
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
            strncpy(stocks[count].symbol, symbol->valuestring, 15);
            stocks[count].price = price->valuedouble;
            stocks[count].change = change ? change->valuedouble : 0.0;
            stocks[count].change_percent = changePercent ? changePercent->valuedouble : 0.0;
            stocks[count].has_data = 1;
            count++;
        }
    }
    
    cJSON_Delete(json);
    return count;
}

void display_dashboard(StockData *stocks, int count, time_t last_update) {
    printf("\033[2J\033[H"); // Clear screen
    printf("╔══════════════════════════════════════════════════════════════════════╗\n");
    printf("║                     📈 LIVE STOCK DASHBOARD 📈                      ║\n");
    printf("╠══════════════════════════════════════════════════════════════════════╣\n");
    printf("║ %-10s │ %12s │ %10s │ %12s ║\n", "SYMBOL", "PRICE", "CHANGE", "CHANGE %");
    printf("╟────────────┼──────────────┼────────────┼──────────────╢\n");
    
    for (int i = 0; i < count; i++) {
        const char *color = stocks[i].change >= 0 ? "\033[32m" : "\033[31m";
        printf("║ %-10s │ %12.2f │ %s%10.2f\033[0m │ %s%11.2f%%\033[0m ║\n",
               stocks[i].symbol, stocks[i].price, color, stocks[i].change, color, stocks[i].change_percent);
    }
    printf("╚══════════════════════════════════════════════════════════════════════╝\n");
    printf("Last update: %s", ctime(&last_update));
}

int main(int argc, char *argv[]) {
    const char *symbols = "AAPL,MSFT,GOOGL,AMZN,TSLA";
    if (argc > 1) symbols = argv[1];

    signal(SIGINT, signal_handler);
    curl_global_init(CURL_GLOBAL_DEFAULT);
    
    StockData stocks[MAX_STOCKS];
    
    while (running) {
        struct MemoryStruct chunk;
        if (fetch_stock_data(symbols, &chunk) == 0) {
            int count = parse_stock_data(chunk.memory, stocks, MAX_STOCKS);
            if (count > 0) {
                display_dashboard(stocks, count, time(NULL));
            } else {
                printf("No data found for symbols.\n");
            }
            free(chunk.memory);
        }
        
        if (running) sleep(REFRESH_INTERVAL);
    }
    
    curl_global_cleanup();
    return 0;
}
