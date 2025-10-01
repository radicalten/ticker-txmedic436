// main.c - Stock Price Dashboard using Yahoo Finance API and cJSON

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <curl/curl.h>
#include <time.h>
#include "cJSON.h"

// ANSI color codes for terminal output
#define RESET   "\033[0m"
#define RED     "\033[31m"
#define GREEN   "\033[32m"
#define YELLOW  "\033[33m"
#define BLUE    "\033[34m"
#define CYAN    "\033[36m"
#define BOLD    "\033[1m"
#define CLEAR   "\033[2J\033[H"

// Configuration
#define UPDATE_INTERVAL 5  // Update every 5 seconds
#define MAX_SYMBOLS 10

// Structure to hold response data
struct MemoryStruct {
    char *memory;
    size_t size;
};

// Structure for stock data
typedef struct {
    char symbol[10];
    double regularMarketPrice;
    double regularMarketChange;
    double regularMarketChangePercent;
    double regularMarketDayHigh;
    double regularMarketDayLow;
    long long marketCap;
    char longName[256];
} StockData;

// Callback function for curl to store response
static size_t WriteMemoryCallback(void *contents, size_t size, size_t nmemb, void *userp) {
    size_t realsize = size * nmemb;
    struct MemoryStruct *mem = (struct MemoryStruct *)userp;
    
    char *ptr = realloc(mem->memory, mem->size + realsize + 1);
    if(ptr == NULL) {
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
char* fetch_stock_data(const char *symbols) {
    CURL *curl;
    CURLcode res;
    struct MemoryStruct chunk;
    
    chunk.memory = malloc(1);
    chunk.size = 0;
    
    curl_global_init(CURL_GLOBAL_ALL);
    curl = curl_easy_init();
    
    if(curl) {
        char url[512];
        snprintf(url, sizeof(url), 
                "https://query1.finance.yahoo.com/v7/finance/quote?symbols=%s", 
                symbols);
        
        struct curl_slist *headers = NULL;
        headers = curl_slist_append(headers, 
            "User-Agent: Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/91.0.4472.124 Safari/537.36");
        headers = curl_slist_append(headers, "Accept: application/json");
        
        curl_easy_setopt(curl, CURLOPT_URL, url);
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteMemoryCallback);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, (void *)&chunk);
        curl_easy_setopt(curl, CURLOPT_TIMEOUT, 10L);
        curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
        
        res = curl_easy_perform(curl);
        
        if(res != CURLE_OK) {
            fprintf(stderr, "curl_easy_perform() failed: %s\n", curl_easy_strerror(res));
            free(chunk.memory);
            chunk.memory = NULL;
        }
        
        curl_slist_free_all(headers);
        curl_easy_cleanup(curl);
    }
    
    curl_global_cleanup();
    return chunk.memory;
}

// Parse JSON response and extract stock data
int parse_stock_json(const char *json_string, StockData *stocks, int max_stocks) {
    cJSON *json = cJSON_Parse(json_string);
    if (json == NULL) {
        const char *error_ptr = cJSON_GetErrorPtr();
        if (error_ptr != NULL) {
            fprintf(stderr, "Error parsing JSON: %s\n", error_ptr);
        }
        return 0;
    }
    
    int count = 0;
    cJSON *quoteResponse = cJSON_GetObjectItemCaseSensitive(json, "quoteResponse");
    if (quoteResponse != NULL) {
        cJSON *result = cJSON_GetObjectItemCaseSensitive(quoteResponse, "result");
        if (cJSON_IsArray(result)) {
            cJSON *stock = NULL;
            cJSON_ArrayForEach(stock, result) {
                if (count >= max_stocks) break;
                
                cJSON *symbol = cJSON_GetObjectItemCaseSensitive(stock, "symbol");
                cJSON *price = cJSON_GetObjectItemCaseSensitive(stock, "regularMarketPrice");
                cJSON *change = cJSON_GetObjectItemCaseSensitive(stock, "regularMarketChange");
                cJSON *changePercent = cJSON_GetObjectItemCaseSensitive(stock, "regularMarketChangePercent");
                cJSON *dayHigh = cJSON_GetObjectItemCaseSensitive(stock, "regularMarketDayHigh");
                cJSON *dayLow = cJSON_GetObjectItemCaseSensitive(stock, "regularMarketDayLow");
                cJSON *marketCap = cJSON_GetObjectItemCaseSensitive(stock, "marketCap");
                cJSON *longName = cJSON_GetObjectItemCaseSensitive(stock, "longName");
                
                if (cJSON_IsString(symbol) && symbol->valuestring != NULL) {
                    strncpy(stocks[count].symbol, symbol->valuestring, sizeof(stocks[count].symbol) - 1);
                }
                
                if (cJSON_IsNumber(price)) {
                    stocks[count].regularMarketPrice = price->valuedouble;
                }
                
                if (cJSON_IsNumber(change)) {
                    stocks[count].regularMarketChange = change->valuedouble;
                }
                
                if (cJSON_IsNumber(changePercent)) {
                    stocks[count].regularMarketChangePercent = changePercent->valuedouble;
                }
                
                if (cJSON_IsNumber(dayHigh)) {
                    stocks[count].regularMarketDayHigh = dayHigh->valuedouble;
                }
                
                if (cJSON_IsNumber(dayLow)) {
                    stocks[count].regularMarketDayLow = dayLow->valuedouble;
                }
                
                if (cJSON_IsNumber(marketCap)) {
                    stocks[count].marketCap = (long long)marketCap->valuedouble;
                }
                
                if (cJSON_IsString(longName) && longName->valuestring != NULL) {
                    strncpy(stocks[count].longName, longName->valuestring, sizeof(stocks[count].longName) - 1);
                } else {
                    strncpy(stocks[count].longName, stocks[count].symbol, sizeof(stocks[count].longName) - 1);
                }
                
                count++;
            }
        }
    }
    
    cJSON_Delete(json);
    return count;
}

// Format large numbers with suffixes (K, M, B, T)
void format_market_cap(long long value, char *buffer, size_t size) {
    if (value >= 1000000000000LL) {
        snprintf(buffer, size, "%.2fT", value / 1000000000000.0);
    } else if (value >= 1000000000) {
        snprintf(buffer, size, "%.2fB", value / 1000000000.0);
    } else if (value >= 1000000) {
        snprintf(buffer, size, "%.2fM", value / 1000000.0);
    } else if (value >= 1000) {
        snprintf(buffer, size, "%.2fK", value / 1000.0);
    } else {
        snprintf(buffer, size, "%lld", value);
    }
}

// Display the dashboard
void display_dashboard(StockData *stocks, int count) {
    printf(CLEAR);  // Clear screen
    
    // Header
    printf(BOLD CYAN "╔════════════════════════════════════════════════════════════════════════════════════════════╗\n" RESET);
    printf(BOLD CYAN "║                              📈 LIVE STOCK PRICE DASHBOARD 📈                               ║\n" RESET);
    printf(BOLD CYAN "╠════════════════════════════════════════════════════════════════════════════════════════════╣\n" RESET);
    
    // Get current time
    time_t rawtime;
    struct tm *timeinfo;
    char time_buffer[80];
    time(&rawtime);
    timeinfo = localtime(&rawtime);
    strftime(time_buffer, sizeof(time_buffer), "%Y-%m-%d %H:%M:%S", timeinfo);
    printf(BOLD CYAN "║" RESET " Last Updated: " YELLOW "%s" RESET, time_buffer);
    printf("                                                       " BOLD CYAN "║\n" RESET);
    printf(BOLD CYAN "╠════════════════════════════════════════════════════════════════════════════════════════════╣\n" RESET);
    
    // Table header
    printf(BOLD CYAN "║" RESET);
    printf(BOLD " %-8s │ %-30s │ %10s │ %10s │ %8s │ %12s " RESET, 
           "Symbol", "Company", "Price", "Change", "Change%", "Market Cap");
    printf(BOLD CYAN "║\n" RESET);
    printf(BOLD CYAN "╠════════════════════════════════════════════════════════════════════════════════════════════╣\n" RESET);
    
    // Display stock data
    for (int i = 0; i < count; i++) {
        printf(BOLD CYAN "║" RESET " ");
        
        // Symbol
        printf(BOLD "%-8s" RESET " │ ", stocks[i].symbol);
        
        // Company name (truncated if too long)
        char truncated_name[31];
        strncpy(truncated_name, stocks[i].longName, 30);
        truncated_name[30] = '\0';
        printf("%-30s │ ", truncated_name);
        
        // Price
        printf("$%9.2f │ ", stocks[i].regularMarketPrice);
        
        // Change (with color)
        if (stocks[i].regularMarketChange >= 0) {
            printf(GREEN "%+9.2f" RESET " │ ", stocks[i].regularMarketChange);
        } else {
            printf(RED "%+9.2f" RESET " │ ", stocks[i].regularMarketChange);
        }
        
        // Change percentage (with color)
        if (stocks[i].regularMarketChangePercent >= 0) {
            printf(GREEN "%+7.2f%%" RESET " │ ", stocks[i].regularMarketChangePercent);
        } else {
            printf(RED "%+7.2f%%" RESET " │ ", stocks[i].regularMarketChangePercent);
        }
        
        // Market cap
        char market_cap_str[20];
        format_market_cap(stocks[i].marketCap, market_cap_str, sizeof(market_cap_str));
        printf("%11s ", market_cap_str);
        
        printf(BOLD CYAN "║\n" RESET);
        
        // Add separator between rows (except last one)
        if (i < count - 1) {
            printf(BOLD CYAN "╟────────────────────────────────────────────────────────────────────────────────────────────╢\n" RESET);
        }
    }
    
    // Footer
    printf(BOLD CYAN "╚════════════════════════════════════════════════════════════════════════════════════════════╝\n" RESET);
    printf("\n" YELLOW "Press Ctrl+C to exit. Refreshing every %d seconds..." RESET "\n", UPDATE_INTERVAL);
}

int main(int argc, char *argv[]) {
    // Default symbols if none provided
    const char *default_symbols = "AAPL,MSFT,GOOGL,AMZN,NVDA,META,TSLA,BRK-B,JPM,V";
    char symbols[256];
    
    if (argc > 1) {
        // Join command line arguments into comma-separated string
        symbols[0] = '\0';
        for (int i = 1; i < argc && i <= MAX_SYMBOLS; i++) {
            if (i > 1) strcat(symbols, ",");
            strcat(symbols, argv[i]);
        }
    } else {
        strcpy(symbols, default_symbols);
    }
    
    printf(CLEAR);
    printf(BOLD CYAN "Starting Stock Dashboard...\n" RESET);
    printf("Watching symbols: %s\n", symbols);
    sleep(1);
    
    StockData stocks[MAX_SYMBOLS];
    
    while (1) {
        char *json_response = fetch_stock_data(symbols);
        
        if (json_response != NULL) {
            int stock_count = parse_stock_json(json_response, stocks, MAX_SYMBOLS);
            
            if (stock_count > 0) {
                display_dashboard(stocks, stock_count);
            } else {
                printf(RED "Error: No stock data received or parsing failed.\n" RESET);
            }
            
            free(json_response);
        } else {
            printf(RED "Error: Failed to fetch data from Yahoo Finance.\n" RESET);
        }
        
        sleep(UPDATE_INTERVAL);
    }
    
    return 0;
}
