#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <curl/curl.h>
#include "cJSON.h"

// Custom User-Agent header to avoid rejection by Yahoo Finance
static size_t WriteCallback(void *ptr, size_t size, size_t nmemb, void *data) {
    size_t realsize = size * nmemb;
    ((char**)data)[0] = realloc(((char**)data)[0], strlen(((char**)data)[0]) + realsize + 1);
    if (((char**)data)[0]) {
        memcpy(((char**)data)[0] + strlen(((char**)data)[0]), ptr, realsize);
        ((char**)data)[0][strlen(((char**)data)[0]) + realsize] = '\0';
    }
    return realsize;
}

// Function to fetch stock price using Yahoo Finance API
void fetch_stock_price(const char *symbol, char **json_data) {
    CURL *curl;
    CURLcode res;
    const char *url = "https://query1.finance.yahoo.com/v7/finance/quote?symbols=";
    char *full_url = malloc(strlen(url) + strlen(symbol) + 10);
    sprintf(full_url, "%s%s", url, symbol);
    *json_data = malloc(1); // Initialize with a null string
    curl = curl_easy_init();
    if(curl) {
        curl_easy_setopt(curl, CURLOPT_URL, full_url);
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteCallback);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &json_data);
        // Set a custom User-Agent header
        curl_easy_setopt(curl, CURLOPT_USERAGENT, "Mozilla/5.0 (compatible; MyCJSONDashboard/1.0)");
        res = curl_easy_perform(curl);
        if(res != CURLE_OK)
            fprintf(stderr, "curl_easy_perform() failed: %s\n", curl_easy_strerror(res));
        curl_easy_cleanup(curl);
    }
    free(full_url);
}

// Function to parse JSON and extract the current price
void parse_and_display_price(const char *json_data) {
    cJSON *root, *quote, *price;
    root = cJSON_Parse(json_data);
    if (!root) {
        const char *error_ptr = cJSON_GetErrorPtr();
        if (error_ptr) {
            fprintf(stderr, "Error before: %s\n", error_ptr);
        }
        return;
    }

    cJSON *fields = cJSON_GetObjectItemCaseSensitive(root, "quoteResponse");
    if (!fields || !cJSON_IsObject(fields)) {
        cJSON_Delete(root);
        return;
    }

    cJSON *result = cJSON_GetObjectItemCaseSensitive(fields, "result");
    if (!result || !cJSON_IsObject(result)) {
        cJSON_Delete(root);
        return;
    }

    cJSON *quotes = cJSON_GetObjectItemCaseSensitive(result, "quotes");
    if (!quotes || !cJSON_IsArray(quotes)) {
        cJSON_Delete(root);
        return;
    }

    // Assuming the first element is the symbol we queried
    cJSON *first = cJSON_GetArrayItem(quotes, 0);
    if (first) {
        cJSON *context = cJSON_GetObjectItemCaseSensitive(first, "regularMarketPrice");
        if (context && cJSON_IsNumber(context)) {
            printf("Current price of %s: %.2f\n", first->string, context->valuedouble);
        } else {
            printf("Price not found for %s.\n", first->string);
        }
    } else {
        printf("No quotes found.\n");
    }

    cJSON_Delete(root);
}

int main() {
    char *json_data = NULL;
    printf("Enter stock symbol (e.g., AAPL): ");
    char symbol[10];
    scanf("%s", symbol);

    fetch_stock_price(symbol, &json_data);
    if (json_data) {
        parse_and_display_price(json_data);
        free(json_data);
    } else {
        printf("Failed to fetch data.\n");
    }

    return 0;
}
