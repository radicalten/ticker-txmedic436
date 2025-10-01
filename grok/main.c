#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>  // For sleep()
#include <curl/curl.h>
#include "cJSON.h"

// Refresh interval in seconds
#define REFRESH_INTERVAL 10

// List of stock symbols (comma-separated for API)
const char *symbols[] = {"AAPL", "GOOGL", "MSFT", NULL};  // Add more here

// Buffer for curl response
struct MemoryStruct {
    char *memory;
    size_t size;
};

// Callback for curl to write response to memory
static size_t WriteMemoryCallback(void *contents, size_t size, size_t nmemb, void *userp) {
    size_t realsize = size * nmemb;
    struct MemoryStruct *mem = (struct MemoryStruct *)userp;

    char *ptr = realloc(mem->memory, mem->size + realsize + 1);
    if (ptr == NULL) {
        return 0;  // Out of memory
    }

    mem->memory = ptr;
    memcpy(&(mem->memory[mem->size]), contents, realsize);
    mem->size += realsize;
    mem->memory[mem->size] = 0;

    return realsize;
}

// Function to fetch JSON data from Yahoo Finance
char *fetch_stock_data(const char *symbol_list) {
    CURL *curl_handle;
    CURLcode res;
    struct MemoryStruct chunk;

    chunk.memory = malloc(1);  // Will be grown as needed
    chunk.size = 0;

    curl_global_init(CURL_GLOBAL_ALL);
    curl_handle = curl_easy_init();
    if (curl_handle) {
        char url[512];
        snprintf(url, sizeof(url), "https://query1.finance.yahoo.com/v7/finance/quote?symbols=%s", symbol_list);

        curl_easy_setopt(curl_handle, CURLOPT_URL, url);
        curl_easy_setopt(curl_handle, CURLOPT_WRITEFUNCTION, WriteMemoryCallback);
        curl_easy_setopt(curl_handle, CURLOPT_WRITEDATA, (void *)&chunk);
        curl_easy_setopt(curl_handle, CURLOPT_USERAGENT, "Mozilla/5.0 (compatible; StockDashboard/1.0)");  // Custom User-Agent to avoid rejection

        res = curl_easy_perform(curl_handle);

        if (res != CURLE_OK) {
            fprintf(stderr, "curl_easy_perform() failed: %s\n", curl_easy_strerror(res));
            free(chunk.memory);
            chunk.memory = NULL;
        }

        curl_easy_cleanup(curl_handle);
    }
    curl_global_cleanup();

    return chunk.memory;
}

// Function to build comma-separated symbol list
void build_symbol_list(char *buffer, size_t bufsize) {
    buffer[0] = '\0';
    for (int i = 0; symbols[i] != NULL; i++) {
        if (i > 0) strcat(buffer, ",");
        strcat(buffer, symbols[i]);
    }
}

// Function to display the dashboard
void display_dashboard(cJSON *json) {
    // Clear terminal screen
    printf("\033[H\033[J");  // ANSI escape to clear screen

    printf("Live Stock Dashboard (Refreshes every %d seconds)\n", REFRESH_INTERVAL);
    printf("------------------------------------------------\n");

    cJSON *quote_response = cJSON_GetObjectItem(json, "quoteResponse");
    if (quote_response) {
        cJSON *result = cJSON_GetObjectItem(quote_response, "result");
        if (result && cJSON_IsArray(result)) {
            int array_size = cJSON_GetArraySize(result);
            for (int i = 0; i < array_size; i++) {
                cJSON *stock = cJSON_GetArrayItem(result, i);
                const char *symbol = cJSON_GetObjectItem(stock, "symbol")->valuestring;
                double regular_market_price = cJSON_GetObjectItem(stock, "regularMarketPrice")->valuedouble;
                double regular_market_change = cJSON_GetObjectItem(stock, "regularMarketChange")->valuedouble;
                double regular_market_change_percent = cJSON_GetObjectItem(stock, "regularMarketChangePercent")->valuedouble;

                printf("%-10s Price: $%.2f | Change: %.2f (%.2f%%)\n",
                       symbol, regular_market_price, regular_market_change, regular_market_change_percent);
            }
        } else {
            printf("No stock data available.\n");
        }
    } else {
        printf("Error parsing JSON response.\n");
    }

    printf("------------------------------------------------\n");
    fflush(stdout);  // Ensure output is flushed
}

int main() {
    char symbol_list[256];
    build_symbol_list(symbol_list, sizeof(symbol_list));

    while (1) {  // Infinite loop for live updates
        char *json_data = fetch_stock_data(symbol_list);
        if (json_data) {
            cJSON *json = cJSON_Parse(json_data);
            if (json) {
                display_dashboard(json);
                cJSON_Delete(json);
            } else {
                printf("JSON parsing error.\n");
            }
            free(json_data);
        } else {
            printf("Failed to fetch data.\n");
        }

        sleep(REFRESH_INTERVAL);  // Wait before next refresh
    }

    return 0;
}
