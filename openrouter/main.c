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
#define BUFFER_SIZE 1024 * 1024

// Stock data structure
typedef struct {
    char symbol[16];
    double price;
    double change;
    double change_percent;
    int has_data;
} StockData;

// Global flag for graceful shutdown
static volatile int running = 1;

// Signal handler for Ctrl+C
void signal_handler(int sig) {
    (void)sig;  // Suppress unused parameter warning
    running = 0;
}

// URL encode function
char *url_encode(const char *str) {
    size_t len = strlen(str);
    char *encoded = malloc(len * 3 + 1);  // Max: 3 chars per char
    if (!encoded) return NULL;
    
    char *p = encoded;
    for (size_t i = 0; i < len; i++) {
        if (strchr("!$'()*,;:@&=+$,/?%#[]", str[i]) && str[i] != ' ') {
            sprintf(p, "%%%02X", (unsigned char)str[i]);
            p += 3;
        } else if (str[i] == ' ') {
            *p++ = '+';
        } else {
            *p++ = str[i];
        }
    }
    *p = '\0';
    return encoded;
}

// Callback function for libcurl
size_t write_callback(void *contents, size_t size, size_t nmemb, void *userp) {
    size_t total_size = size * nmemb;
    char **response = (char **)userp;
    
    char *temp = realloc(*response, total_size + 1);
    if (!temp) {
        fprintf(stderr, "Memory allocation failed in write_callback\n");
        return 0;
    }
    
    *response = temp;
    memcpy(*response, contents, total_size);
    (*response)[total_size] = '\0';
    
    return total_size;
}

// Fetch stock data from Yahoo Finance
int fetch_stock_data(const char *symbols, char **response) {
    CURL *curl;
    CURLcode res;
    char url[512];
    
    // URL encode symbols
    char *encoded_symbols = url_encode(symbols);
    if (!encoded_symbols) {
        fprintf(stderr, "Failed to encode symbols\n");
        return -1;
    }
    
    snprintf(url, sizeof(url), 
             "https://query1.finance.yahoo.com/v7/finance/quote?symbols=%s&lang=en-US&region=US&corsDomain=yahoo.com", 
             encoded_symbols);
    
    free(encoded_symbols);
    
    curl = curl_easy_init();
    if (!curl) {
        fprintf(stderr, "Failed to initialize curl\n");
        return -1;
    }
    
    *response = calloc(1, 1);  // Use calloc for cleaner initialization
    if (!*response) {
        curl_easy_cleanup(curl);
        return -1;
    }
    
    // Set options
    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_callback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, response);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, 
                     "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/120.0.0.0 Safari/537.36");
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 30L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 10L);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 2L);
    
    res = curl_easy_perform(curl);
    
    // Check for errors
    if (res != CURLE_OK) {
        fprintf(stderr, "curl_easy_perform() failed: %s\n", curl_easy_strerror(res));
        curl_easy_cleanup(curl);
        free(*response);
        *response = NULL;
        return -1;
    }
    
    // Get HTTP response code
    long http_code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
    fprintf(stderr, "HTTP Response Code: %ld\n", http_code);
    
    curl_easy_cleanup(curl);
    return 0;
}

// Parse JSON response
int parse_stock_data(const char *json_response, StockData *stocks, int max_stocks) {
    cJSON *json = cJSON_Parse(json_response);
    if (!json) {
        fprintf(stderr, "Failed to parse JSON\n");
        return -1;
    }
    
    cJSON *quoteResponse = cJSON_GetObjectItemCaseSensitive(json, "quoteResponse");
    if (!quoteResponse) {
        fprintf(stderr, "No quoteResponse in JSON\n");
        cJSON_Delete(json);
        return -1;
    }
    
    cJSON *result = cJSON_GetObjectItemCaseSensitive(quoteResponse, "result");
    if (!result || !cJSON_IsArray(result)) {
        fprintf(stderr, "No result array in JSON\n");
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
    fprintf(stderr, "Parsed %d stocks\n", count);
    return count;
}

// Clear screen (cross-platform)
void clear_screen() {
    printf("\033[2J\033[H");
    fflush(stdout);
}

// Display dashboard
void display_dashboard(StockData *stocks, int count, time_t last_update) {
    clear_screen();
    
    printf("\n");
    printf("╔══════════════════════════════════════════════════════════════════════╗\n");
    printf("║                     📈 LIVE STOCK DASHBOARD 📈                      ║\n");
    printf("╠══════════════════════════════════════════════════════════════════════╣\n");
    printf("║ %-8s │ %12s │ %10s │ %12s ║\n", "SYMBOL", "PRICE", "CHANGE", "CHANGE %");
    printf("╟─────────┼──────────────┼────────────┼──────────────╢\n");
    
    for (int i = 0; i < count; i++) {
        if (stocks[i].has_data) {
            const char *change_color = stocks[i].change >= 0 ? "\033[32m" : "\033[31m";
            
            printf("║ %-8s │ %s%12.2f\033[0m │ %s%10.2f\033[0m │ %11.2f%% ║\n",
                   stocks[i].symbol,
                   change_color, stocks[i].price,
                   change_color, stocks[i].change,
                   stocks[i].change_percent);
        }
    }
    
    printf("╚══════════════════════════════════════════════════════════════════════╝\n");
    
    struct tm *tm_info = localtime(&last_update);
    char time_str[64];
    strftime(time_str, sizeof(time_str), "%Y-%m-%d %H:%M:%S", tm_info);
    
    printf("\n🔄 Updating every %d seconds | Last update: %s | Press Ctrl+C to exit\n", 
           REFRESH_INTERVAL, time_str);
}

// Display error
void display_error(const char *message) {
    printf("\033[2K\r\033[31mError: %s\033[0m\n", message);
    fflush(stdout);
}

int main(int argc, char *argv[]) {
    // Allow custom symbols via command line
    const char *symbols = "AAPL,GOOGL,MSFT,AMZN,TSLA,META,NVDA,JPM";
    if (argc > 1) {
        symbols = argv[1];
    }
    
    // Setup signal handler with sigaction for reliability
    struct sigaction sa;
    sa.sa_handler = signal_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESTART;  // Restart interrupted system calls
    if (sigaction(SIGINT, &sa, NULL) == -1) {
        perror("sigaction SIGINT");
    }
    if (sigaction(SIGTERM, &sa, NULL) == -1) {
        perror("sigaction SIGTERM");
    }
    
    // Initialize libcurl
    CURLcode curl_res = curl_global_init(CURL_GLOBAL_DEFAULT);
    if (curl_res != CURLE_OK) {
        fprintf(stderr, "Failed to initialize libcurl: %s\n", curl_easy_strerror(curl_res));
        return 1;
    }
    
    printf("Initializing stock dashboard...\n");
    printf("Tracking: %s\n\n", symbols);
    
    StockData stocks[MAX_STOCKS];
    char *response = NULL;
    time_t last_update = 0;
    
    // Initialize stocks array
    memset(stocks, 0, sizeof(stocks));
    
    // Main loop
    while (running) {
        // Fetch data
        if (fetch_stock_data(symbols, &response) == 0) {
            if (response && strlen(response) > 0) {
                fprintf(stderr, "Response length: %zu\n", strlen(response));
                
                int count = parse_stock_data(response, stocks, MAX_STOCKS);
                if (count > 0) {
                    last_update = time(NULL);
                    display_dashboard(stocks, count, last_update);
                } else {
                    display_error("Failed to parse stock data - check JSON response");
                    fprintf(stderr, "Response preview: %.200s...\n", response);
                }
            } else {
                display_error("Empty response from API");
            }
            free(response);
            response = NULL;
        } else {
            display_error("Failed to fetch data. Retrying...");
        }
        
        // Wait for next update
        if (running) {
            sleep(REFRESH_INTERVAL);
        }
    }
    
    // Cleanup
    if (response) free(response);
    curl_global_cleanup();
    
    printf("\n\nDashboard stopped.\n");
    return 0;
}
