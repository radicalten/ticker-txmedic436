#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <curl/curl.h>
#include "cJSON.h"

#define MAX_SYMBOLS 10
#define MAX_STRLEN 256
#define YAHOO_URL "https://query1.finance.yahoo.com/v8/finance/chart/%s?interval=1m&range=1d"

// Structure to hold response data
struct MemoryStruct {
    char *memory;
    size_t size;
};

// Stock data structure
typedef struct {
    char symbol[MAX_STRLEN];
    double current_price;
    double previous_close;
    double change;
    double change_percent;
    double day_high;
    double day_low;
    double volume;
    int valid;
} StockData;

// Callback function for curl to write response data
static size_t WriteMemoryCallback(void *contents, size_t size, size_t nmemb, void *userp) {
    size_t realsize = size * nmemb;
    struct MemoryStruct *mem = (struct MemoryStruct *)userp;

    char *ptr = realloc(mem->memory, mem->size + realsize + 1);
    if (!ptr) {
        printf("Not enough memory\n");
        return 0;
    }

    mem->memory = ptr;
    memcpy(&(mem->memory[mem->size]), contents, realsize);
    mem->size += realsize;
    mem->memory[mem->size] = 0;

    return realsize;
}

// Parse JSON response from Yahoo Finance
void parse_yahoo_response(const char *json_str, StockData *stock) {
    cJSON *root = cJSON_Parse(json_str);
    if (!root) {
        stock->valid = 0;
        return;
    }

    cJSON *chart = cJSON_GetObjectItem(root, "chart");
    if (!chart) {
        cJSON_Delete(root);
        stock->valid = 0;
        return;
    }

    cJSON *result = cJSON_GetObjectItem(chart, "result");
    if (!result || !cJSON_IsArray(result) || cJSON_GetArraySize(result) == 0) {
        cJSON_Delete(root);
        stock->valid = 0;
        return;
    }

    cJSON *first_result = cJSON_GetArrayItem(result, 0);
    if (!first_result) {
        cJSON_Delete(root);
        stock->valid = 0;
        return;
    }

    // Get meta data
    cJSON *meta = cJSON_GetObjectItem(first_result, "meta");
    if (!meta) {
        cJSON_Delete(root);
        stock->valid = 0;
        return;
    }

    // Extract stock data
    cJSON *regularMarketPrice = cJSON_GetObjectItem(meta, "regularMarketPrice");
    cJSON *previousClose = cJSON_GetObjectItem(meta, "previousClose");
    cJSON *regularMarketDayHigh = cJSON_GetObjectItem(meta, "regularMarketDayHigh");
    cJSON *regularMarketDayLow = cJSON_GetObjectItem(meta, "regularMarketDayLow");
    cJSON *regularMarketVolume = cJSON_GetObjectItem(meta, "regularMarketVolume");

    if (regularMarketPrice && cJSON_IsNumber(regularMarketPrice))
        stock->current_price = regularMarketPrice->valuedouble;
    
    if (previousClose && cJSON_IsNumber(previousClose))
        stock->previous_close = previousClose->valuedouble;
    
    if (regularMarketDayHigh && cJSON_IsNumber(regularMarketDayHigh))
        stock->day_high = regularMarketDayHigh->valuedouble;
    
    if (regularMarketDayLow && cJSON_IsNumber(regularMarketDayLow))
        stock->day_low = regularMarketDayLow->valuedouble;
    
    if (regularMarketVolume && cJSON_IsNumber(regularMarketVolume))
        stock->volume = regularMarketVolume->valuedouble;

    // Calculate change
    stock->change = stock->current_price - stock->previous_close;
    if (stock->previous_close != 0)
        stock->change_percent = (stock->change / stock->previous_close) * 100.0;
    else
        stock->change_percent = 0;

    stock->valid = 1;
    cJSON_Delete(root);
}

// Fetch stock data from Yahoo Finance
int fetch_stock_data(const char *symbol, StockData *stock) {
    CURL *curl_handle;
    CURLcode res;
    struct MemoryStruct chunk;
    char url[MAX_STRLEN];

    chunk.memory = malloc(1);
    chunk.size = 0;

    curl_global_init(CURL_GLOBAL_ALL);
    curl_handle = curl_easy_init();

    if (!curl_handle) {
        free(chunk.memory);
        return 0;
    }

    // Prepare URL
    snprintf(url, sizeof(url), YAHOO_URL, symbol);
    
    // Set user agent to avoid rejection
    curl_easy_setopt(curl_handle, CURLOPT_USERAGENT, "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/91.0.4472.124 Safari/537.36");
    curl_easy_setopt(curl_handle, CURLOPT_URL, url);
    curl_easy_setopt(curl_handle, CURLOPT_WRITEFUNCTION, WriteMemoryCallback);
    curl_easy_setopt(curl_handle, CURLOPT_WRITEDATA, (void *)&chunk);
    curl_easy_setopt(curl_handle, CURLOPT_TIMEOUT, 10L);
    curl_easy_setopt(curl_handle, CURLOPT_FOLLOWLOCATION, 1L);

    // Perform the request
    res = curl_easy_perform(curl_handle);

    if (res != CURLE_OK) {
        fprintf(stderr, "curl_easy_perform() failed: %s\n", curl_easy_strerror(res));
        curl_easy_cleanup(curl_handle);
        free(chunk.memory);
        return 0;
    }

    // Parse the response
    strcpy(stock->symbol, symbol);
    parse_yahoo_response(chunk.memory, stock);

    curl_easy_cleanup(curl_handle);
    free(chunk.memory);
    curl_global_cleanup();

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

// Print the dashboard
void print_dashboard(StockData stocks[], int count, int update_num) {
    clear_screen();
    
    printf("╔══════════════════════════════════════════════════════════════════════════════════╗\n");
    printf("║                        LIVE STOCK DASHBOARD (Update #%d)                        ║\n", update_num);
    printf("╠══════════════════════════════════════════════════════════════════════════════════╣\n");
    printf("║ %-8s │ %-12s │ %-10s │ %-10s │ %-12s │ %-12s ║\n", 
           "Symbol", "Price", "Change", "Change%", "Day High", "Day Low");
    printf("╠══════════════════════════════════════════════════════════════════════════════════╣\n");
    
    for (int i = 0; i < count; i++) {
        if (stocks[i].valid) {
            const char *direction = stocks[i].change >= 0 ? "▲" : "▼";
            printf("║ %-8s │ $%-11.2f │ %s%-9.2f │ %s%-9.2f%% │ $%-11.2f │ $%-11.2f ║\n",
                   stocks[i].symbol,
                   stocks[i].current_price,
                   direction, stocks[i].change,
                   direction, stocks[i].change_percent,
                   stocks[i].day_high,
                   stocks[i].day_low);
        } else {
            printf("║ %-8s │ %-12s │ %-10s │ %-10s │ %-12s │ %-12s ║\n",
                   stocks[i].symbol, "ERROR", "-", "-", "-", "-");
        }
    }
    
    printf("╚══════════════════════════════════════════════════════════════════════════════════╝\n");
    printf("\nPress Ctrl+C to exit | Auto-refreshing every 10 seconds...\n");
}

int main(int argc, char *argv[]) {
    StockData stocks[MAX_SYMBOLS];
    int num_stocks = 0;
    int update_count = 0;

    printf("Stock Market Live Dashboard\n");
    printf("===========================\n\n");

    // Get stock symbols from command line arguments or use defaults
    if (argc > 1) {
        num_stocks = argc - 1;
        if (num_stocks > MAX_SYMBOLS) {
            printf("Maximum %d stocks allowed. Using first %d.\n", MAX_SYMBOLS, MAX_SYMBOLS);
            num_stocks = MAX_SYMBOLS;
        }
        for (int i = 0; i < num_stocks; i++) {
            strncpy(stocks[i].symbol, argv[i + 1], MAX_STRLEN - 1);
            stocks[i].symbol[MAX_STRLEN - 1] = '\0';
            // Convert to uppercase
            for (int j = 0; stocks[i].symbol[j]; j++) {
                stocks[i].symbol[j] = toupper(stocks[i].symbol[j]);
            }
        }
    } else {
        // Default stocks if no arguments provided
        num_stocks = 5;
        const char *defaults[] = {"AAPL", "GOOGL", "MSFT", "AMZN", "TSLA"};
        for (int i = 0; i < num_stocks; i++) {
            strcpy(stocks[i].symbol, defaults[i]);
        }
        printf("Using default stocks: ");
        for (int i = 0; i < num_stocks; i++) {
            printf("%s ", stocks[i].symbol);
        }
        printf("\n\nUsage: %s SYMBOL1 SYMBOL2 ...\n", argv[0]);
        printf("Example: %s AAPL GOOGL MSFT\n\n", argv[0]);
    }

    printf("Fetching stock data...\n\n");

    // Main update loop
    while (1) {
        update_count++;
        
        // Fetch data for all stocks
        for (int i = 0; i < num_stocks; i++) {
            if (!fetch_stock_data(stocks[i].symbol, &stocks[i])) {
                stocks[i].valid = 0;
            }
        }
        
        // Print dashboard
        print_dashboard(stocks, num_stocks, update_count);
        
        // Wait before next update
        #ifdef _WIN32
            Sleep(10000);  // Windows: milliseconds
        #else
            sleep(10);     // Unix: seconds
        #endif
    }

    return 0;
}
