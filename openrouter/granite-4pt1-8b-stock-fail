#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <curl/curl.h>
#include "cJSON.h"

#define MAX_SYMBOLS 5
#define REFRESH_INTERVAL 5  // seconds

struct MemoryStruct {
    char *memory;
    size_t size;
};

static size_t WriteMemoryCallback(void *contents, size_t size, size_t nmemb, void *userp) {
    size_t realsize = size * nmemb;
    struct MemoryStruct *mem = (struct MemoryStruct *)userp;
    
    char *ptr = realloc(mem->memory, mem->size + realsize + 1);
    if (!ptr) {
        fprintf(stderr, "Out of memory\n");
        return 0;
    }
    
    mem->memory = ptr;
    memcpy(&(mem->memory[mem->size]), contents, realsize);
    mem->size += realsize;
    mem->memory[mem->size] = 0;
    
    return realsize;
}

double get_stock_price(const char *symbol) {
    CURL *curl;
    CURLcode res;
    struct MemoryStruct chunk = {0};
    double price = -1.0;
    
    char url[256];
    snprintf(url, sizeof(url), 
             "https://query1.finance.yahoo.com/v8/finance/chart/%s", symbol);
    
    curl = curl_easy_init();
    if (curl) {
        struct curl_slist *headers = NULL;
        headers = curl_slist_append(headers, 
            "User-Agent: Mozilla/5.0 (Macintosh; Intel Mac OS X 10_15_7) "
            "AppleWebKit/537.36 (KHTML, like Gecko) Chrome/120.0.0.0 Safari/537.36");
        
        curl_easy_setopt(curl, CURLOPT_URL, url);
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteMemoryCallback);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, (void *)&chunk);
        curl_easy_setopt(curl, CURLOPT_TIMEOUT, 10L);
        
        res = curl_easy_perform(curl);
        
        if (res == CURLE_OK && chunk.memory) {
            cJSON *json = cJSON_Parse(chunk.memory);
            if (json) {
                cJSON *chart = cJSON_GetObjectItem(json, "chart");
                if (chart) {
                    cJSON *result = cJSON_GetObjectItem(chart, "result");
                    if (result && cJSON_IsArray(result) && cJSON_GetArraySize(result) > 0) {
                        cJSON *first = cJSON_GetArrayItem(result, 0);
                        cJSON *meta = cJSON_GetObjectItem(first, "meta");
                        if (meta) {
                            cJSON *price_item = cJSON_GetObjectItem(meta, "regularMarketPrice");
                            if (cJSON_IsNumber(price_item)) {
                                price = price_item->valuedouble;
                            }
                        }
                    }
                }
                cJSON_Delete(json);
            }
        }
        
        curl_slist_free_all(headers);
        curl_easy_cleanup(curl);
        free(chunk.memory);
    }
    return price;
}

int main(int argc, char *argv[]) {
    const char *symbols[] = {"AAPL", "GOOGL", "MSFT", "TSLA", "AMZN"};
    int count = sizeof(symbols) / sizeof(symbols[0]);
    
    curl_global_init(CURL_GLOBAL_DEFAULT);
    
    printf("Live Stock Dashboard (Ctrl+C to exit)\n");
    printf("=====================================\n\n");
    
    while (1) {
        // Clear screen (ANSI escape)
        printf("\033[2J\033[H");
        printf("Symbol    Price\n");
        printf("----------------\n");
        
        for (int i = 0; i < count; i++) {
            double price = get_stock_price(symbols[i]);
            if (price > 0) {
                printf("%-8s  $%.2f\n", symbols[i], price);
            } else {
                printf("%-8s  N/A\n", symbols[i]);
            }
        }
        
        printf("\nLast updated: every %d seconds\n", REFRESH_INTERVAL);
        sleep(REFRESH_INTERVAL);
    }
    
    curl_global_cleanup();
    return 0;
}
