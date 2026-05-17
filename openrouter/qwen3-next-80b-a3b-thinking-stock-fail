#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <curl/curl.h>
#include "cJSON.h"

// Memory structure for storing response data
struct MemoryStruct {
    char *memory;
    size_t size;
};

// Write callback function for cURL
static size_t write_callback(void *contents, size_t size, size_t nmemb, void *userp) {
    size_t realsize = size * nmemb;
    struct MemoryStruct *mem = (struct MemoryStruct *)userp;

    char *ptr = realloc(mem->memory, mem->size + realsize + 1);
    if (!ptr) {
        fprintf(stderr, "Failed to allocate memory\n");
        return 0;
    }

    mem->memory = ptr;
    memcpy(&(mem->memory[mem->size]), contents, realsize);
    mem->size += realsize;
    mem->memory[mem->size] = '\0';

    return realsize;
}

int main(int argc, char *argv[]) {
    if (argc < 2) {
        fprintf(stderr, "Usage: %s <symbol1> [symbol2 ...]\n", argv[0]);
        return 1;
    }

    // Build URL with symbols
    char url[1024];
    snprintf(url, sizeof(url), "https://query1.finance.yahoo.com/v7/finance/quote?symbols=");
    for (int i = 1; i < argc; i++) {
        if (i > 1) strcat(url, ",");
        strcat(url, argv[i]);
    }

    // Initialize cURL
    CURL *curl = curl_easy_init();
    if (!curl) {
        fprintf(stderr, "cURL initialization failed\n");
        return 1;
    }

    struct MemoryStruct chunk;
    chunk.memory = malloc(1);
    chunk.size = 0;

    // Configure cURL options
    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_callback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, (void *)&chunk);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/91.0.4472.124 Safari/537.36");

    // Perform request
    CURLcode res = curl_easy_perform(curl);
    if (res != CURLE_OK) {
        fprintf(stderr, "cURL error: %s\n", curl_easy_strerror(res));
        free(chunk.memory);
        curl_easy_cleanup(curl);
        return 1;
    }

    // Parse JSON response
    cJSON *json = cJSON_Parse(chunk.memory);
    if (!json) {
        fprintf(stderr, "JSON parse error: %s\n", cJSON_GetErrorPtr());
        free(chunk.memory);
        curl_easy_cleanup(curl);
        return 1;
    }

    // Extract quoteResponse -> result array
    cJSON *quoteResponse = cJSON_GetObjectItemCaseSensitive(json, "quoteResponse");
    if (!quoteResponse) {
        fprintf(stderr, "Missing quoteResponse in response\n");
        cJSON_Delete(json);
        free(chunk.memory);
        curl_easy_cleanup(curl);
        return 1;
    }

    cJSON *result = cJSON_GetObjectItemCaseSensitive(quoteResponse, "result");
    if (!result || !cJSON_IsArray(result)) {
        fprintf(stderr, "Invalid or missing result array\n");
        cJSON_Delete(json);
        free(chunk.memory);
        curl_easy_cleanup(curl);
        return 1;
    }

    // Process each stock quote
    int num_quotes = cJSON_GetArraySize(result);
    for (int i = 0; i < num_quotes; i++) {
        cJSON *quote = cJSON_GetArrayItem(result, i);
        if (!quote) continue;

        // Get symbol
        cJSON *symbolItem = cJSON_GetObjectItemCaseSensitive(quote, "symbol");
        if (!symbolItem || !cJSON_IsString(symbolItem)) {
            printf("Symbol not found in response\n");
            continue;
        }
        const char *symbol = symbolItem->valuestring;

        // Check for error
        cJSON *error = cJSON_GetObjectItemCaseSensitive(quote, "error");
        if (error) {
            cJSON *code = cJSON_GetObjectItemCaseSensitive(error, "code");
            cJSON *desc = cJSON_GetObjectItemCaseSensitive(error, "description");
            const char *code_str = code && cJSON_IsString(code) ? code->valuestring : "Unknown";
            const char *desc_str = desc && cJSON_IsString(desc) ? desc->valuestring : "Unknown error";
            printf("%s: Error [%s] - %s\n", symbol, code_str, desc_str);
            continue;
        }

        // Get price
        cJSON *priceItem = cJSON_GetObjectItemCaseSensitive(quote, "regularMarketPrice");
        if (!priceItem || !cJSON_IsNumber(priceItem)) {
            printf("%s: Price not available\n", symbol);
            continue;
        }
        double price = priceItem->valuedouble;
        printf("%s: $%.2f\n", symbol, price);
    }

    // Cleanup
    cJSON_Delete(json);
    free(chunk.memory);
    curl_easy_cleanup(curl);
    return 0;
}
