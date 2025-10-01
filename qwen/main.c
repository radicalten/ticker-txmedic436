#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <curl/curl.h>
#include "cjson/cJSON.h"
#include <unistd.h>
#include <time.h>

// Structure to hold HTTP response data
struct string {
    char *ptr;
    size_t len;
};

// Callback function for libcurl to write received data
static void init_string(struct string *s) {
    s->len = 0;
    s->ptr = malloc(s->len + 1);
    if (s->ptr == NULL) {
        fprintf(stderr, "malloc() failed\n");
        exit(EXIT_FAILURE);
    }
    s->ptr[0] = '\0';
}

static size_t writefunc(void *ptr, size_t size, size_t nmemb, struct string *s) {
    size_t new_len = s->len + size * nmemb;
    s->ptr = realloc(s->ptr, new_len + 1);
    if (s->ptr == NULL) {
        fprintf(stderr, "realloc() failed\n");
        exit(EXIT_FAILURE);
    }
    memcpy(s->ptr + s->len, ptr, size * nmemb);
    s->ptr[new_len] = '\0';
    s->len = new_len;
    return size * nmemb;
}

// Function to fetch stock data from Yahoo Finance
char* fetch_stock_data(const char* symbol) {
    CURL *curl;
    CURLcode res;
    struct string s;
    
    // Build URL with proper parameters
    char url[512];
    snprintf(url, sizeof(url), 
             "https://query1.finance.yahoo.com/v8/finance/chart/%s?range=1d&interval=1m", 
             symbol);
    
    curl = curl_easy_init();
    if(curl) {
        init_string(&s);
        
        // Set URL
        curl_easy_setopt(curl, CURLOPT_URL, url);
        
        // Set custom headers including User-Agent
        struct curl_slist *headers = NULL;
        headers = curl_slist_append(headers, "User-Agent: Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/91.0.4472.124 Safari/537.36");
        headers = curl_slist_append(headers, "Accept: */*");
        headers = curl_slist_append(headers, "Accept-Language: en-US,en;q=0.5");
        headers = curl_slist_append(headers, "Connection: keep-alive");
        
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, writefunc);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &s);
        curl_easy_setopt(curl, CURLOPT_TIMEOUT, 10L);
        curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
        
        // Perform the request
        res = curl_easy_perform(curl);
        
        // Cleanup
        curl_slist_free_all(headers);
        curl_easy_cleanup(curl);
        
        if(res != CURLE_OK) {
            fprintf(stderr, "curl_easy_perform() failed: %s\n", curl_easy_strerror(res));
            free(s.ptr);
            return NULL;
        }
        
        return s.ptr;
    }
    return NULL;
}

// Function to parse JSON and extract current price
double get_current_price(const char* json_data, char* company_name, size_t name_size) {
    cJSON *json = cJSON_Parse(json_data);
    if (!json) {
        const char *error_ptr = cJSON_GetErrorPtr();
        if (error_ptr != NULL) {
            fprintf(stderr, "Error parsing JSON before: %s\n", error_ptr);
        }
        return -1.0;
    }
    
    double price = -1.0;
    
    // Navigate to the chart data
    cJSON *chart = cJSON_GetObjectItemCaseSensitive(json, "chart");
    if (chart) {
        cJSON *result = cJSON_GetObjectItemCaseSensitive(chart, "result");
        if (result && cJSON_IsArray(result) && cJSON_GetArraySize(result) > 0) {
            cJSON *first_result = cJSON_GetArrayItem(result, 0);
            if (first_result) {
                // Get meta information
                cJSON *meta = cJSON_GetObjectItemCaseSensitive(first_result, "meta");
                if (meta) {
                    cJSON *symbol = cJSON_GetObjectItemCaseSensitive(meta, "symbol");
                    cJSON *regularMarketPrice = cJSON_GetObjectItemCaseSensitive(meta, "regularMarketPrice");
                    
                    if (symbol && cJSON_IsString(symbol)) {
                        strncpy(company_name, symbol->valuestring, name_size - 1);
                        company_name[name_size - 1] = '\0';
                    }
                    
                    if (regularMarketPrice && cJSON_IsNumber(regularMarketPrice)) {
                        price = regularMarketPrice->valuedouble;
                    }
                }
            }
        }
    }
    
    cJSON_Delete(json);
    return price;
}

// Function to clear screen (cross-platform)
void clear_screen() {
#ifdef _WIN32
    system("cls");
#else
    system("clear");
#endif
}

int main() {
    // Initialize curl globally
    curl_global_init(CURL_GLOBAL_DEFAULT);
    
    // Array of stock symbols to monitor
    const char* symbols[] = {"AAPL", "GOOGL", "MSFT", "TSLA", "AMZN", "META"};
    int num_symbols = sizeof(symbols) / sizeof(symbols[0]);
    
    printf("Stock Price Dashboard - Press Ctrl+C to exit\n");
    printf("=============================================\n\n");
    
    while(1) {
        clear_screen();
        time_t now = time(NULL);
        printf("Stock Price Dashboard - Last Updated: %s", ctime(&now));
        printf("=============================================\n\n");
        
        for(int i = 0; i < num_symbols; i++) {
            char* json_data = fetch_stock_data(symbols[i]);
            if(json_data) {
                char company_name[32] = "Unknown";
                double price = get_current_price(json_data, company_name, sizeof(company_name));
                
                if(price > 0) {
                    printf("%-6s: $%.2f\n", company_name, price);
                } else {
                    printf("%-6s: Error fetching data\n", symbols[i]);
                }
                
                free(json_data);
            } else {
                printf("%-6s: Failed to connect\n", symbols[i]);
            }
        }
        
        printf("\nRefreshing in 30 seconds...\n");
        printf("Press Ctrl+C to exit\n");
        
        sleep(30); // Wait 30 seconds before next update
    }
    
    curl_global_cleanup();
    return 0;
}
