#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <curl/curl.h>
#include <unistd.h>
#include "cJSON.h"

#define USER_AGENT "Mozilla/5.0 (X11; Linux x86_64; rv:109.0) Gecko/20100101 Firefox/115.0"
#define UPDATE_INTERVAL 5
#define MAX_SYMBOLS 10

struct MemoryStruct {
    char *memory;
    size_t size;
};

static size_t WriteMemoryCallback(void *contents, size_t size, size_t nmemb, void *userp) {
    size_t realsize = size * nmemb;
    struct MemoryStruct *mem = (struct MemoryStruct *)userp;

    char *ptr = realloc(mem->memory, mem->size + realsize + 1);
    if(!ptr) {
        printf("Not enough memory\n");
        return 0;
    }

    mem->memory = ptr;
    memcpy(&(mem->memory[mem->size]), contents, realsize);
    mem->size += realsize;
    mem->memory[mem->size] = 0;

    return realsize;
}

char* build_url(const char* symbol) {
    static char url[256];
    snprintf(url, sizeof(url), 
             "https://query1.finance.yahoo.com/v8/finance/chart/%s?interval=1m&range=1d",
             symbol);
    return url;
}

double get_stock_price(const char* json_data) {
    cJSON *json = cJSON_Parse(json_data);
    if (!json) {
        printf("Error parsing JSON\n");
        return -1;
    }

    cJSON *chart = cJSON_GetObjectItem(json, "chart");
    if (!chart) {
        cJSON_Delete(json);
        return -1;
    }

    cJSON *result = cJSON_GetObjectItem(chart, "result");
    if (!result || !cJSON_IsArray(result)) {
        cJSON_Delete(json);
        return -1;
    }

    cJSON *first_result = cJSON_GetArrayItem(result, 0);
    if (!first_result) {
        cJSON_Delete(json);
        return -1;
    }

    cJSON *meta = cJSON_GetObjectItem(first_result, "meta");
    if (!meta) {
        cJSON_Delete(json);
        return -1;
    }

    cJSON *price = cJSON_GetObjectItem(meta, "regularMarketPrice");
    if (!price) {
        cJSON_Delete(json);
        return -1;
    }

    double stock_price = price->valuedouble;
    cJSON_Delete(json);
    return stock_price;
}

int fetch_stock_data(const char* symbol, double *price) {
    CURL *curl;
    CURLcode res;
    struct MemoryStruct chunk;

    chunk.memory = malloc(1);
    chunk.size = 0;

    curl = curl_easy_init();
    if(!curl) {
        free(chunk.memory);
        return -1;
    }

    curl_easy_setopt(curl, CURLOPT_URL, build_url(symbol));
    curl_easy_setopt(curl, CURLOPT_USERAGENT, USER_AGENT);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteMemoryCallback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, (void *)&chunk);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 10L);

    res = curl_easy_perform(curl);
    if(res != CURLE_OK) {
        fprintf(stderr, "curl_easy_perform() failed: %s\n", curl_easy_strerror(res));
        curl_easy_cleanup(curl);
        free(chunk.memory);
        return -1;
    }

    long http_code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
    if (http_code != 200) {
        fprintf(stderr, "HTTP error: %ld\n", http_code);
        curl_easy_cleanup(curl);
        free(chunk.memory);
        return -1;
    }

    *price = get_stock_price(chunk.memory);
    
    curl_easy_cleanup(curl);
    free(chunk.memory);
    
    return (*price >= 0) ? 0 : -1;
}

void clear_screen() {
    printf("\033[2J\033[1;1H");
}

void print_header() {
    printf("\033[1;36m"); // Cyan color
    printf("╔══════════════════════════════════════╗\n");
    printf("║         LIVE STOCK DASHBOARD         ║\n");
    printf("╚══════════════════════════════════════╝\n");
    printf("\033[0m");
    printf("\033[1;33m%-10s %-15s %-10s\033[0m\n", "SYMBOL", "PRICE", "STATUS");
    printf("────────────────────────────────────────\n");
}

int main(int argc, char *argv[]) {
    if (argc < 2) {
        printf("Usage: %s <stock1> <stock2> ... <stock%d>\n", argv[0], MAX_SYMBOLS);
        printf("Example: %s AAPL MSFT GOOGL TSLA\n", argv[0]);
        return 1;
    }

    if (argc - 1 > MAX_SYMBOLS) {
        printf("Maximum %d symbols supported\n", MAX_SYMBOLS);
        return 1;
    }

    curl_global_init(CURL_GLOBAL_DEFAULT);

    printf("Starting stock dashboard...\n");
    sleep(2);

    while(1) {
        clear_screen();
        print_header();

        for(int i = 1; i < argc; i++) {
            double price;
            int result = fetch_stock_data(argv[i], &price);
            
            if(result == 0) {
                printf("\033[1;32m%-10s $%-14.2f ✓\033[0m\n", 
                       argv[i], price);
            } else {
                printf("\033[1;31m%-10s %-15s ✗\033[0m\n", 
                       argv[i], "Failed");
            }
            
            fflush(stdout);
        }

        printf("\n\033[1;35mPress Ctrl+C to exit - Updating in %d seconds...\033[0m\n", UPDATE_INTERVAL);
        sleep(UPDATE_INTERVAL);
    }

    curl_global_cleanup();
    return 0;
}
