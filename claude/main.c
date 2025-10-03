#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <curl/curl.h>
#include <unistd.h>
#include "cJSON.h"

#define MAX_SYMBOLS 10
#define REFRESH_INTERVAL 5

// Structure to hold response data
struct MemoryStruct {
    char *memory;
    size_t size;
};

// Callback function for curl to write data
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

// Function to fetch stock data from Yahoo Finance
char* fetch_stock_data(const char *symbol) {
    CURL *curl;
    CURLcode res;
    struct MemoryStruct chunk;

    chunk.memory = malloc(1);
    chunk.size = 0;

    curl_global_init(CURL_GLOBAL_DEFAULT);
    curl = curl_easy_init();

    if(curl) {
        char url[512];
        snprintf(url, sizeof(url), 
                "https://query1.finance.yahoo.com/v8/finance/chart/%s?interval=1m&range=1d", 
                symbol);

        struct curl_slist *headers = NULL;
        headers = curl_slist_append(headers, "User-Agent: Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/91.0.4472.124 Safari/537.36");
        headers = curl_slist_append(headers, "Accept: application/json");
        headers = curl_slist_append(headers, "Accept-Language: en-US,en;q=0.9");

        curl_easy_setopt(curl, CURLOPT_URL, url);
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteMemoryCallback);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, (void *)&chunk);
        curl_easy_setopt(curl, CURLOPT_USERAGENT, "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36");
        curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
        curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
        curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 2L);

        res = curl_easy_perform(curl);

        if(res != CURLE_OK) {
            fprintf(stderr, "curl_easy_perform() failed: %s\n", curl_easy_strerror(res));
            free(chunk.memory);
            curl_slist_free_all(headers);
            curl_easy_cleanup(curl);
            curl_global_cleanup();
            return NULL;
        }

        curl_slist_free_all(headers);
        curl_easy_cleanup(curl);
    }

    curl_global_cleanup();
    return chunk.memory;
}

// Structure to hold stock information
typedef struct {
    char symbol[20];
    double price;
    double change;
    double change_percent;
    double previous_close;
    int valid;
} StockInfo;

// Parse JSON and extract stock information
int parse_stock_data(const char *json_string, StockInfo *stock) {
    if (!json_string || !stock) {
        return 0;
    }

    cJSON *json = cJSON_Parse(json_string);
    if (!json) {
        const char *error_ptr = cJSON_GetErrorPtr();
        if (error_ptr != NULL) {
            fprintf(stderr, "JSON Parse Error: %s\n", error_ptr);
        }
        return 0;
    }

    stock->valid = 0;

    // Navigate through the JSON structure
    cJSON *chart = cJSON_GetObjectItemCaseSensitive(json, "chart");
    if (!chart) {
        cJSON_Delete(json);
        return 0;
    }

    cJSON *result = cJSON_GetObjectItemCaseSensitive(chart, "result");
    if (!result || !cJSON_IsArray(result) || cJSON_GetArraySize(result) == 0) {
        cJSON_Delete(json);
        return 0;
    }

    cJSON *result_item = cJSON_GetArrayItem(result, 0);
    if (!result_item) {
        cJSON_Delete(json);
        return 0;
    }

    // Get symbol
    cJSON *meta = cJSON_GetObjectItemCaseSensitive(result_item, "meta");
    if (meta) {
        cJSON *symbol_json = cJSON_GetObjectItemCaseSensitive(meta, "symbol");
        if (cJSON_IsString(symbol_json) && (symbol_json->valuestring != NULL)) {
            strncpy(stock->symbol, symbol_json->valuestring, sizeof(stock->symbol) - 1);
        }

        // Get current price (regularMarketPrice)
        cJSON *regular_price = cJSON_GetObjectItemCaseSensitive(meta, "regularMarketPrice");
        if (cJSON_IsNumber(regular_price)) {
            stock->price = regular_price->valuedouble;
            stock->valid = 1;
        }

        // Get previous close
        cJSON *prev_close = cJSON_GetObjectItemCaseSensitive(meta, "chartPreviousClose");
        if (cJSON_IsNumber(prev_close)) {
            stock->previous_close = prev_close->valuedouble;
            stock->change = stock->price - stock->previous_close;
            stock->change_percent = (stock->change / stock->previous_close) * 100.0;
        }
    }

    cJSON_Delete(json);
    return stock->valid;
}

// Clear screen (cross-platform)
void clear_screen() {
#ifdef _WIN32
    system("cls");
#else
    system("clear");
#endif
}

// Display the dashboard
void display_dashboard(StockInfo *stocks, int count) {
    clear_screen();
    
    printf("\n");
    printf("╔════════════════════════════════════════════════════════════════════════╗\n");
    printf("║               LIVE STOCK PRICE DASHBOARD                               ║\n");
    printf("╠════════════╦═══════════════╦═══════════════╦════════════════════════════╣\n");
    printf("║  SYMBOL    ║     PRICE     ║    CHANGE     ║      CHANGE %%              ║\n");
    printf("╠════════════╬═══════════════╬═══════════════╬════════════════════════════╣\n");

    for (int i = 0; i < count; i++) {
        if (stocks[i].valid) {
            const char *color = stocks[i].change >= 0 ? "\033[0;32m" : "\033[0;31m";
            const char *reset = "\033[0m";
            const char *arrow = stocks[i].change >= 0 ? "▲" : "▼";

            printf("║ %-10s ║ %s$%11.2f%s ║ %s%s$%10.2f%s ║ %s%s%10.2f%%%s           ║\n",
                   stocks[i].symbol,
                   color, stocks[i].price, reset,
                   color, arrow, fabs(stocks[i].change), reset,
                   color, arrow, fabs(stocks[i].change_percent), reset);
        } else {
            printf("║ %-10s ║ %-13s ║ %-13s ║ %-26s ║\n",
                   stocks[i].symbol, "N/A", "N/A", "N/A");
        }
    }

    printf("╚════════════╩═══════════════╩═══════════════╩════════════════════════════╝\n");
    printf("\nRefreshing every %d seconds... Press Ctrl+C to exit.\n", REFRESH_INTERVAL);
}

int main(int argc, char *argv[]) {
    StockInfo stocks[MAX_SYMBOLS];
    int stock_count = 0;

    // Default symbols if none provided
    const char *default_symbols[] = {"AAPL", "GOOGL", "MSFT", "TSLA", "AMZN"};
    int default_count = 5;

    // Parse command line arguments or use defaults
    if (argc > 1) {
        stock_count = argc - 1;
        if (stock_count > MAX_SYMBOLS) {
            stock_count = MAX_SYMBOLS;
        }
        for (int i = 0; i < stock_count; i++) {
            strncpy(stocks[i].symbol, argv[i + 1], sizeof(stocks[i].symbol) - 1);
            stocks[i].valid = 0;
        }
    } else {
        stock_count = default_count;
        for (int i = 0; i < stock_count; i++) {
            strncpy(stocks[i].symbol, default_symbols[i], sizeof(stocks[i].symbol) - 1);
            stocks[i].valid = 0;
        }
    }

    printf("Starting stock dashboard...\n");
    printf("Monitoring %d stocks\n", stock_count);
    sleep(2);

    // Main loop
    while (1) {
        for (int i = 0; i < stock_count; i++) {
            char *json_data = fetch_stock_data(stocks[i].symbol);
            if (json_data) {
                parse_stock_data(json_data, &stocks[i]);
                free(json_data);
            } else {
                stocks[i].valid = 0;
            }
        }

        display_dashboard(stocks, stock_count);
        sleep(REFRESH_INTERVAL);
    }

    return 0;
}
