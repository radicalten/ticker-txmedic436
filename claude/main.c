#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <curl/curl.h>
#include "cJSON.h"

#define MAX_STOCKS 10
#define BUFFER_SIZE 65536
#define URL_SIZE 512

typedef struct {
    char symbol[16];
    double price;
    double change;
    double change_percent;
    char market_state[32];
    int valid;
} StockData;

typedef struct {
    char *memory;
    size_t size;
} MemoryStruct;

static size_t WriteMemoryCallback(void *contents, size_t size, size_t nmemb, void *userp) {
    size_t realsize = size * nmemb;
    MemoryStruct *mem = (MemoryStruct *)userp;

    char *ptr = realloc(mem->memory, mem->size + realsize + 1);
    if (!ptr) {
        printf("Not enough memory (realloc returned NULL)\n");
        return 0;
    }

    mem->memory = ptr;
    memcpy(&(mem->memory[mem->size]), contents, realsize);
    mem->size += realsize;
    mem->memory[mem->size] = 0;

    return realsize;
}

int fetch_stock_data(const char *symbol, StockData *stock) {
    CURL *curl;
    CURLcode res;
    MemoryStruct chunk;
    char url[URL_SIZE];
    
    chunk.memory = malloc(1);
    chunk.size = 0;

    curl = curl_easy_init();
    if (!curl) {
        free(chunk.memory);
        return 0;
    }

    // Build Yahoo Finance URL
    snprintf(url, sizeof(url), 
        "https://query1.finance.yahoo.com/v8/finance/chart/%s", symbol);

    // Set up curl options
    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteMemoryCallback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, (void *)&chunk);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, 
        "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/91.0.4472.124 Safari/537.36");
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 10L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 0L);

    // Perform the request
    res = curl_easy_perform(curl);
    
    if (res != CURLE_OK) {
        printf("curl_easy_perform() failed: %s\n", curl_easy_strerror(res));
        curl_easy_cleanup(curl);
        free(chunk.memory);
        return 0;
    }

    curl_easy_cleanup(curl);

    // Parse JSON response
    cJSON *json = cJSON_Parse(chunk.memory);
    if (!json) {
        printf("Error parsing JSON for %s\n", symbol);
        free(chunk.memory);
        return 0;
    }

    // Navigate JSON structure
    cJSON *chart = cJSON_GetObjectItemCaseSensitive(json, "chart");
    if (!chart) {
        cJSON_Delete(json);
        free(chunk.memory);
        return 0;
    }

    cJSON *result = cJSON_GetArrayItem(chart, 0);
    if (!result) {
        cJSON_Delete(json);
        free(chunk.memory);
        return 0;
    }

    cJSON *meta = cJSON_GetObjectItemCaseSensitive(result, "meta");
    if (!meta) {
        cJSON_Delete(json);
        free(chunk.memory);
        return 0;
    }

    // Extract stock data
    cJSON *regular_price = cJSON_GetObjectItemCaseSensitive(meta, "regularMarketPrice");
    cJSON *previous_close = cJSON_GetObjectItemCaseSensitive(meta, "previousClose");
    cJSON *market_state = cJSON_GetObjectItemCaseSensitive(meta, "marketState");

    if (regular_price && cJSON_IsNumber(regular_price) && 
        previous_close && cJSON_IsNumber(previous_close)) {
        
        strncpy(stock->symbol, symbol, sizeof(stock->symbol) - 1);
        stock->symbol[sizeof(stock->symbol) - 1] = '\0';
        
        stock->price = regular_price->valuedouble;
        stock->change = stock->price - previous_close->valuedouble;
        stock->change_percent = (stock->change / previous_close->valuedouble) * 100.0;
        
        if (market_state && cJSON_IsString(market_state)) {
            strncpy(stock->market_state, market_state->valuestring, 
                   sizeof(stock->market_state) - 1);
            stock->market_state[sizeof(stock->market_state) - 1] = '\0';
        } else {
            strcpy(stock->market_state, "UNKNOWN");
        }
        
        stock->valid = 1;
    } else {
        stock->valid = 0;
    }

    cJSON_Delete(json);
    free(chunk.memory);
    return stock->valid;
}

void clear_screen() {
    printf("\033[2J\033[H");
}

void print_header() {
    time_t now;
    time(&now);
    char *timestr = ctime(&now);
    timestr[strlen(timestr) - 1] = '\0'; // Remove newline
    
    printf("╔══════════════════════════════════════════════════════════════════╗\n");
    printf("║                    LIVE STOCK DASHBOARD                          ║\n");
    printf("║  Last Updated: %-48s  ║\n", timestr);
    printf("╠══════════════════════════════════════════════════════════════════╣\n");
    printf("║ Symbol    │ Price      │ Change     │ Change %%   │ Market State ║\n");
    printf("╠═══════════╪════════════╪════════════╪════════════╪══════════════╣\n");
}

void print_stock(const StockData *stock) {
    if (!stock->valid) {
        printf("║ %-9s │ %-10s │ %-10s │ %-10s │ %-12s ║\n", 
               stock->symbol, "ERROR", "ERROR", "ERROR", "ERROR");
        return;
    }

    // Color coding for price changes
    const char *color_start = "";
    const char *color_end = "\033[0m";
    
    if (stock->change > 0) {
        color_start = "\033[32m"; // Green
    } else if (stock->change < 0) {
        color_start = "\033[31m"; // Red
    } else {
        color_start = "\033[37m"; // White
    }

    printf("║ %-9s │ %s$%9.2f%s │ %s%+9.2f%s │ %s%+8.2f%%%s │ %-12s ║\n", 
           stock->symbol,
           color_start, stock->price, color_end,
           color_start, stock->change, color_end,
           color_start, stock->change_percent, color_end,
           stock->market_state);
}

void print_footer() {
    printf("╚══════════════════════════════════════════════════════════════════╝\n");
    printf("Press Ctrl+C to exit. Updates every 30 seconds.\n");
}

int main(int argc, char *argv[]) {
    StockData stocks[MAX_STOCKS];
    int stock_count = 0;
    
    // Default symbols if none provided
    const char *default_symbols[] = {"AAPL", "GOOGL", "MSFT", "TSLA", "AMZN"};
    int default_count = 5;
    
    // Initialize curl
    curl_global_init(CURL_GLOBAL_DEFAULT);
    
    // Parse command line arguments or use defaults
    if (argc > 1) {
        stock_count = (argc - 1 > MAX_STOCKS) ? MAX_STOCKS : argc - 1;
        for (int i = 0; i < stock_count; i++) {
            strncpy(stocks[i].symbol, argv[i + 1], sizeof(stocks[i].symbol) - 1);
            stocks[i].symbol[sizeof(stocks[i].symbol) - 1] = '\0';
            stocks[i].valid = 0;
        }
    } else {
        stock_count = default_count;
        for (int i = 0; i < stock_count; i++) {
            strncpy(stocks[i].symbol, default_symbols[i], sizeof(stocks[i].symbol) - 1);
            stocks[i].symbol[sizeof(stocks[i].symbol) - 1] = '\0';
            stocks[i].valid = 0;
        }
    }
    
    printf("Starting stock dashboard with symbols: ");
    for (int i = 0; i < stock_count; i++) {
        printf("%s ", stocks[i].symbol);
    }
    printf("\n\n");
    
    // Main loop
    while (1) {
        clear_screen();
        print_header();
        
        // Fetch data for each stock
        for (int i = 0; i < stock_count; i++) {
            fetch_stock_data(stocks[i].symbol, &stocks[i]);
            print_stock(&stocks[i]);
        }
        
        print_footer();
        
        // Wait 30 seconds before next update
        sleep(30);
    }
    
    curl_global_cleanup();
    return 0;
}
