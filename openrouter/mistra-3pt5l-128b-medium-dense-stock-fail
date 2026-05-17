#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <curl/curl.h>
#include "cJSON.h"

struct MemoryStruct {
    char *memory;
    size_t size;
};

static size_t WriteMemoryCallback(void *contents, size_t size, size_t nmemb, void *userp) {
    size_t realsize = size * nmemb;
    struct MemoryStruct *mem = (struct MemoryStruct *)userp;

    char *ptr = realloc(mem->memory, mem->size + realsize + 1);
    if (!ptr) {
        printf("Memory allocation failed\n");
        return 0;
    }

    mem->memory = ptr;
    memcpy(&(mem->memory[mem->size]), contents, realsize);
    mem->size += realsize;
    mem->memory[mem->size] = 0;

    return realsize;
}

void print_stock_data(cJSON *json) {
    cJSON *chart = cJSON_GetObjectItemCaseSensitive(json, "chart");
    if (!chart || !cJSON_IsObject(chart)) {
        printf("Invalid chart data");
        return;
    }

    cJSON *result = cJSON_GetObjectItemCaseSensitive(chart, "result");
    if (!result || !cJSON_IsArray(result)) {
        printf("No results found");
        return;
    }

    for (int i = 0; i < cJSON_GetArraySize(result); i++) {
        cJSON *stock = cJSON_GetArrayItem(result, i);
        if (!stock || !cJSON_IsObject(stock)) continue;

        cJSON *meta = cJSON_GetObjectItemCaseSensitive(stock, "meta");
        if (!meta || !cJSON_IsObject(meta)) continue;

        cJSON *symbol = cJSON_GetObjectItemCaseSensitive(meta, "symbol");
        cJSON *price = cJSON_GetObjectItemCaseSensitive(meta, "regularMarketPrice");

        if (symbol && cJSON_IsString(symbol) && price && cJSON_IsNumber(price)) {
            printf("%s: $%.2f\t", symbol->valuestring, price->valuedouble);
        } else {
            // Try extended hours prices
            cJSON *pre_price = cJSON_GetObjectItemCaseSensitive(meta, "preMarketPrice");
            cJSON *post_price = cJSON_GetObjectItemCaseSensitive(meta, "postMarketPrice");

            if (symbol && cJSON_IsString(symbol)) {
                const char *sym = symbol->valuestring;
                if (pre_price && cJSON_IsNumber(pre_price)) {
                    printf("%s: $%.2f (Pre)\t", sym, pre_price->valuedouble);
                } else if (post_price && cJSON_IsNumber(post_price)) {
                    printf("%s: $%.2f (Post)\t", sym, post_price->valuedouble);
                } else {
                    printf("%s: N/A\t", sym);
                }
            }
        }
    }
}

int main(void) {
    CURL *curl;
    CURLcode res;
    struct MemoryStruct chunk;

    curl_global_init(CURL_GLOBAL_ALL);
    curl = curl_easy_init();
    if (!curl) {
        fprintf(stderr, "Failed to initialize curl\n");
        return 1;
    }

    // Configure cURL with proper user-agent to avoid rejection
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/101.0.4951.64 Safari/537.36");
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
    curl_easy_setopt(curl, CURLOPT_ACCEPT_ENCODING, "");
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteMemoryCallback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, (void *)&chunk);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 10L);

    // Stock symbols to track (comma-separated)
    const char *symbols = "AAPL,MSFT,GOOGL,TSLA,AMZN";
    char url[256];
    snprintf(url, sizeof(url), "https://query1.finance.yahoo.com/v8/finance/chart/%s", symbols);
    curl_easy_setopt(curl, CURLOPT_URL, url);

    printf("Live Stock Dashboard - Updates every 5 seconds (Ctrl+C to exit)\n");

    while (1) {
        // Initialize memory for response
        chunk.memory = malloc(1);
        chunk.size = 0;

        // Fetch data
        res = curl_easy_perform(curl);
        if (res != CURLE_OK) {
            printf("\r\033[KError fetching data: %s", curl_easy_strerror(res));
            fflush(stdout);
            free(chunk.memory);
            sleep(5);
            continue;
        }

        // Parse JSON
        cJSON *json = cJSON_Parse(chunk.memory);
        free(chunk.memory); // Free response buffer after parsing

        if (!json) {
            printf("\r\033[KJSON Parse Error: %s", cJSON_GetErrorPtr());
            fflush(stdout);
            sleep(5);
            continue;
        }

        // Clear line and print stock data
        printf("\r\033[K");
        print_stock_data(json);
        cJSON_Delete(json);
        fflush(stdout);

        sleep(5);
    }

    // Cleanup
    curl_easy_cleanup(curl);
    curl_global_cleanup();
    return 0;
}
