// main.c
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <curl/curl.h>
#include "cJSON.h"

#define MAX_SYMBOLS 10
#define URL_BUFFER_SIZE 1024
#define RESPONSE_BUFFER_SIZE 8192

typedef struct {
    char symbol[16];
    double price;
    double change;
    double change_percent;
} StockData;

// CURL response buffer
struct MemoryStruct {
    char *memory;
    size_t size;
};

// CURL write callback function
static size_t WriteMemoryCallback(void *contents, size_t size, size_t nmemb, void *userp) {
    size_t realsize = size * nmemb;
    struct MemoryStruct *mem = (struct MemoryStruct *)userp;

    char *ptr = realloc(mem->memory, mem->size + realsize + 1);
    if (!ptr) {
        fprintf(stderr, "Not enough memory (realloc returned NULL)\n");
        return 0;
    }

    mem->memory = ptr;
    memcpy(&(mem->memory[mem->size]), contents, realsize);
    mem->size += realsize;
    mem->memory[mem->size] = 0;

    return realsize;
}

// Fetch data from Yahoo Finance
int fetch_stock_data(const char *symbol, StockData *data) {
    CURL *curl_handle;
    CURLcode res;
    struct MemoryStruct chunk;
    char url[URL_BUFFER_SIZE];
    char symbols[256];
    
    chunk.memory = malloc(1);
    chunk.size = 0;

    snprintf(symbols, sizeof(symbols), "%s", symbol);
    snprintf(url, sizeof(url), "https://query1.finance.yahoo.com/v7/finance/quote?symbols=%s", symbols);

    curl_global_init(CURL_GLOBAL_ALL);
    curl_handle = curl_easy_init();

    if (!curl_handle) {
        free(chunk.memory);
        return 0;
    }

    curl_easy_setopt(curl_handle, CURLOPT_URL, url);
    curl_easy_setopt(curl_handle, CURLOPT_WRITEFUNCTION, WriteMemoryCallback);
    curl_easy_setopt(curl_handle, CURLOPT_WRITEDATA, (void *)&chunk);
    curl_easy_setopt(curl_handle, CURLOPT_USERAGENT, "Mozilla/5.0 (X11; Linux x86_64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/91.0.4472.124 Safari/537.36");
    curl_easy_setopt(curl_handle, CURLOPT_TIMEOUT, 10L);

    res = curl_easy_perform(curl_handle);
    if (res != CURLE_OK) {
        fprintf(stderr, "curl_easy_perform() failed: %s\n", curl_easy_strerror(res));
        curl_easy_cleanup(curl_handle);
        free(chunk.memory);
        return 0;
    }

    // Parse JSON response
    cJSON *json = cJSON_Parse(chunk.memory);
    if (!json) {
        fprintf(stderr, "Error parsing JSON\n");
        curl_easy_cleanup(curl_handle);
        free(chunk.memory);
        return 0;
    }

    cJSON *quoteResponse = cJSON_GetObjectItemCaseSensitive(json, "quoteResponse");
    if (!quoteResponse) {
        fprintf(stderr, "Error: quoteResponse not found\n");
        cJSON_Delete(json);
        curl_easy_cleanup(curl_handle);
        free(chunk.memory);
        return 0;
    }

    cJSON *result = cJSON_GetObjectItemCaseSensitive(quoteResponse, "result");
    if (!result || !cJSON_IsArray(result) || cJSON_GetArraySize(result) == 0) {
        fprintf(stderr, "Error: No results found\n");
        cJSON_Delete(json);
        curl_easy_cleanup(curl_handle);
        free(chunk.memory);
        return 0;
    }

    cJSON *first_result = cJSON_GetArrayItem(result, 0);
    if (!first_result) {
        fprintf(stderr, "Error: Empty result\n");
        cJSON_Delete(json);
        curl_easy_cleanup(curl_handle);
        free(chunk.memory);
        return 0;
    }

    // Extract data
    cJSON *symbol_item = cJSON_GetObjectItemCaseSensitive(first_result, "symbol");
    cJSON *price_item = cJSON_GetObjectItemCaseSensitive(first_result, "regularMarketPrice");
    cJSON *change_item = cJSON_GetObjectItemCaseSensitive(first_result, "regularMarketChange");
    cJSON *change_percent_item = cJSON_GetObjectItemCaseSensitive(first_result, "regularMarketChangePercent");

    if (symbol_item && price_item && change_item && change_percent_item) {
        strncpy(data->symbol, symbol_item->valuestring, sizeof(data->symbol) - 1);
        data->symbol[sizeof(data->symbol) - 1] = '\0';
        data->price = price_item->valuedouble;
        data->change = change_item->valuedouble;
        data->change_percent = change_percent_item->valuedouble;
    } else {
        fprintf(stderr, "Error: Incomplete stock data\n");
        cJSON_Delete(json);
        curl_easy_cleanup(curl_handle);
        free(chunk.memory);
        return 0;
    }

    cJSON_Delete(json);
    curl_easy_cleanup(curl_handle);
    free(chunk.memory);
    return 1;
}

// Display dashboard header
void display_header() {
    printf("\033[2J\033[H"); // Clear screen and move cursor to top-left
    printf("========================================\n");
    printf("     LIVE STOCK PRICE DASHBOARD        \n");
    printf("========================================\n");
    printf("%-10s %12s %12s %12s\n", "SYMBOL", "PRICE", "CHANGE", "CHANGE %");
    printf("----------------------------------------\n");
}

// Display stock data
void display_stock(const StockData *data) {
    char change_str[32], change_percent_str[32];
    
    snprintf(change_str, sizeof(change_str), "%.2f", data->change);
    snprintf(change_percent_str, sizeof(change_percent_str), "%.2f%%", data->change_percent);
    
    // Color coding: green for positive, red for negative
    if (data->change >= 0) {
        printf("%-10s %12.2f \033[0;32m%12s\033[0m \033[0;32m%12s\033[0m\n", 
               data->symbol, data->price, change_str, change_percent_str);
    } else {
        printf("%-10s %12.2f \033[0;31m%12s\033[0m \033[0;31m%12s\033[0m\n", 
               data->symbol, data->price, change_str, change_percent_str);
    }
}

int main(int argc, char *argv[]) {
    if (argc < 2) {
        printf("Usage: %s <symbol1> [symbol2] ... [symbolN]\n", argv[0]);
        printf("Example: %s AAPL MSFT GOOGL\n", argv[0]);
        return 1;
    }

    int num_symbols = argc - 1;
    if (num_symbols > MAX_SYMBOLS) {
        fprintf(stderr, "Error: Maximum %d symbols allowed\n", MAX_SYMBOLS);
        return 1;
    }

    StockData stocks[MAX_SYMBOLS];
    int valid_stocks = 0;

    // Initialize stock symbols
    for (int i = 0; i < num_symbols; i++) {
        strncpy(stocks[i].symbol, argv[i + 1], sizeof(stocks[i].symbol) - 1);
        stocks[i].symbol[sizeof(stocks[i].symbol) - 1] = '\0';
    }

    printf("Fetching stock data...\n");

    while (1) {
        display_header();
        valid_stocks = 0;

        for (int i = 0; i < num_symbols; i++) {
            if (fetch_stock_data(stocks[i].symbol, &stocks[i])) {
                display_stock(&stocks[i]);
                valid_stocks++;
            } else {
                printf("Failed to fetch data for %s\n", stocks[i].symbol);
            }
        }

        if (valid_stocks == 0) {
            printf("No valid stock data retrieved.\n");
            break;
        }

        printf("\nPress Ctrl+C to exit\n");
      //  sleep(5); // Refresh every 5 seconds
    }

    return 0;
}
