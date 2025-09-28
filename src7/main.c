#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <curl/curl.h>
#include "cJSON.h"

// Structure for storing the downloaded data
struct MemoryStruct {
    char *memory;
    size_t size;
};

// Callback to write data from curl into our buffer
static size_t WriteMemoryCallback(void *contents, size_t size, size_t nmemb, void *userp) {
    size_t realsize = size * nmemb;
    struct MemoryStruct *mem = (struct MemoryStruct *)userp;

    char *ptr = realloc(mem->memory, mem->size + realsize + 1);
    if (ptr == NULL) {
        fprintf(stderr, "Not enough memory\n");
        return 0;
    }

    mem->memory = ptr;
    memcpy(&(mem->memory[mem->size]), contents, realsize);
    mem->size += realsize;
    mem->memory[mem->size] = 0;

    return realsize;
}

// Fetch data from a URL into memory
char* fetch_url(const char *url) {
    CURL *curl_handle;
    CURLcode res;

    struct MemoryStruct chunk;
    chunk.memory = malloc(1);
    chunk.size = 0;

    curl_global_init(CURL_GLOBAL_DEFAULT);
    curl_handle = curl_easy_init();

    if(curl_handle) {
        curl_easy_setopt(curl_handle, CURLOPT_URL, url);
        curl_easy_setopt(curl_handle, CURLOPT_WRITEFUNCTION, WriteMemoryCallback);
        curl_easy_setopt(curl_handle, CURLOPT_WRITEDATA, (void *)&chunk);
        curl_easy_setopt(curl_handle, CURLOPT_USERAGENT, "libcurl-agent/1.0");

        res = curl_easy_perform(curl_handle);
        if(res != CURLE_OK) {
            fprintf(stderr, "curl_easy_perform() failed: %s\n", curl_easy_strerror(res));
            curl_easy_cleanup(curl_handle);
            curl_global_cleanup();
            free(chunk.memory);
            return NULL;
        }

        curl_easy_cleanup(curl_handle);
    }
    curl_global_cleanup();

    return chunk.memory; 
}

// Parse the Yahoo Finance JSON and print some fields
void parse_and_print_stock(const char *json_text) {
    cJSON *root = cJSON_Parse(json_text);
    if (!root) {
        fprintf(stderr, "Error before: [%s]\n", cJSON_GetErrorPtr());
        return;
    }

    // Navigate: { "quoteResponse": { "result": [ {...stock data...} ] } }
    cJSON *quoteResponse = cJSON_GetObjectItem(root, "quoteResponse");
    if (quoteResponse) {
        cJSON *resultArray = cJSON_GetObjectItem(quoteResponse, "result");
        if (cJSON_IsArray(resultArray)) {
            cJSON *stock = cJSON_GetArrayItem(resultArray, 0);
            if (stock) {
                cJSON *symbol = cJSON_GetObjectItem(stock, "symbol");
                cJSON *price = cJSON_GetObjectItem(stock, "regularMarketPrice");
                cJSON *change = cJSON_GetObjectItem(stock, "regularMarketChangePercent");

                if (cJSON_IsString(symbol) && cJSON_IsNumber(price) && cJSON_IsNumber(change)) {
                    printf("Symbol: %s | Price: %.2f | Change: %.2f%%\n",
                        symbol->valuestring,
                        price->valuedouble,
                        change->valuedouble);
                }
            }
        }
    }

    cJSON_Delete(root);
}

int main(void) {
    const char *symbol = "AAPL";  // Example stock symbol
    char url[512];

    while (1) {
        snprintf(url, sizeof(url),
            "https://query1.finance.yahoo.com/v7/finance/quote?symbols=%s", symbol);

        char *json_data = fetch_url(url);
        if (json_data) {
            system("clear"); // Clear terminal for "dashboard" effect
            parse_and_print_stock(json_data);
            free(json_data);
        }
        sleep(5); // refresh every 5 seconds
    }

    return 0;
}
