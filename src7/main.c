#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <curl/curl.h>
#include "cJSON.h"

// Struct to hold fetched JSON response in memory
struct Memory {
    char *response;
    size_t size;
};

// CURL write callback: append received data to memory buffer
static size_t write_callback(void *data, size_t size, size_t nmemb, void *userp) {
    size_t realsize = size * nmemb;
    struct Memory *mem = (struct Memory *)userp;

    char *ptr = realloc(mem->response, mem->size + realsize + 1);
    if (ptr == NULL) {
        fprintf(stderr, "Not enough memory for response\n");
        return 0;
    }

    mem->response = ptr;
    memcpy(&(mem->response[mem->size]), data, realsize);
    mem->size += realsize;
    mem->response[mem->size] = 0;

    return realsize;
}

// Fetch JSON from given URL
char* fetch_url(const char* url) {
    CURL *curl = curl_easy_init();
    if (!curl) return NULL;

    struct Memory chunk;
    chunk.response = malloc(1);
    chunk.size = 0;

    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_callback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, (void *)&chunk);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "libcurl-agent/1.0"); // Yahoo likes user-agent

    CURLcode res = curl_easy_perform(curl);
    if (res != CURLE_OK) {
        fprintf(stderr, "curl_easy_perform() failed: %s\n",
                curl_easy_strerror(res));
        free(chunk.response);
        chunk.response = NULL;
    }

    curl_easy_cleanup(curl);
    return chunk.response;
}

// Parse JSON and display stock info
void display_stock_data(const char *jsonData) {
    cJSON *root = cJSON_Parse(jsonData);
    if (!root) {
        fprintf(stderr, "JSON parse error\n");
        return;
    }

    cJSON *quoteResponse = cJSON_GetObjectItem(root, "quoteResponse");
    if (!quoteResponse) { cJSON_Delete(root); return; }

    cJSON *result = cJSON_GetObjectItem(quoteResponse, "result");
    if (!cJSON_IsArray(result)) { cJSON_Delete(root); return; }

    int count = cJSON_GetArraySize(result);
    for (int i = 0; i < count; i++) {
        cJSON *stock = cJSON_GetArrayItem(result, i);
        cJSON *symbol = cJSON_GetObjectItem(stock, "symbol");
        cJSON *price = cJSON_GetObjectItem(stock, "regularMarketPrice");
        cJSON *change = cJSON_GetObjectItem(stock, "regularMarketChangePercent");

        if (cJSON_IsString(symbol) && cJSON_IsNumber(price)) {
            printf("%-8s : %8.2f (%+.2f%%)\n",
                   symbol->valuestring,
                   price->valuedouble,
                   change->valuedouble);
        }
    }

    cJSON_Delete(root);
}

int main() {
    // comma-separated ticker symbols
    const char *symbols = "AAPL,MSFT,GOOG,TSLA";
    char url[512];

    while (1) {
        snprintf(url, sizeof(url),
                 "https://query1.finance.yahoo.com/v7/finance/quote?symbols=%s",
                 symbols);

        char *json = fetch_url(url);
        if (json) {
            system("clear"); // clear terminal for dashboard effect
            printf("=== Live Stock Dashboard ===\n\n");
            display_stock_data(json);
            free(json);
        } else {
            printf("Failed to fetch data.\n");
        }

        fflush(stdout);
        sleep(5); // refresh every 5 seconds
    }

    return 0;
}
