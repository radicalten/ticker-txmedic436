#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <curl/curl.h>
#include "cJSON.h"

// Callback function to write response into a string
static size_t WriteCallback(void *contents, size_t size, size_t nmemb, void *userp) {
    size_t total_size = size * nmemb;
    char *existing = (char *)userp;
    existing += total_size;
    memcpy(existing, contents, total_size);
    return total_size;
}

// Function to fetch and parse stock price
void fetch_stock_price(const char *symbol) {
    CURL *curl;
    CURLcode res;
    FILE *fp;
    char url[256];
    char response[4096];
    cJSON *root;
    cJSON *priceJson;
    cJSON *priceValue;

    // Construct the URL for Yahoo Finance
    snprintf(url, sizeof(url), "https://query1.finance.yahoo.com/v7/finance/quote?symbols=%s", symbol);

    // Initialize curl
    curl = curl_easy_init();
    if (!curl) {
        fprintf(stderr, "Failed to initialize curl.\n");
        return;
    }

    // Set options
    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteCallback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, response);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/120.0.0.0 Safari/537.36");

    // Perform the request
    res = curl_easy_perform(curl);

    if (res != CURLE_OK) {
        fprintf(stderr, "Curl error: %s\n", curl_easy_strerror(res));
        curl_easy_cleanup(curl);
        return;
    }

    // Close curl
    curl_easy_cleanup(curl);

    // Parse JSON
    root = cJSON_Parse(response);
    if (!root) {
        fprintf(stderr, "Failed to parse JSON: %s\n", cJSON_GetErrorMsg(root));
        return;
    }

    // Extract the 'regularMarketPrice' object
    cJSON *quote = cJSON_GetObjectItemCaseSensitive(root, "quote");
    if (!quote || !cJSON_IsObject(quote->item["quote"])) {
        fprintf(stderr, "Invalid quote structure in JSON.\n");
        cJSON_Delete(root);
        return;
    }

    cJSON *priceObj = cJSON_GetObjectItemCaseSensitive(quote->item["quote"], "regularMarketPrice");
    if (!priceObj || !cJSON_IsObject(priceObj->item["value"])) {
        fprintf(stderr, "Missing regularMarketPrice.\n");
        cJSON_Delete(root);
        return;
    }

    cJSON *priceValue = cJSON_GetObjectItemCaseSensitive(priceObj->item["value"], "raw");
    if (!priceValue || !cJSON_IsNumber(priceValue->item["raw"])) {
        fprintf(stderr, "Invalid price value.\n");
        cJSON_Delete(root);
        return;
    }

    double price = priceValue->item["raw"].asDouble;

    // Clean up
    cJSON_Delete(root);

    // Output result
    printf("Stock: %s - Current Price: $%.2f\n", symbol, price);
}

int main(int argc, char *argv[]) {
    if (argc < 2) {
        fprintf(stderr, "Usage: %s <stock-symbol>\n", argv[0]);
        return EXIT_FAILURE;
    }

    // Fetch for each provided symbol
    for (int i = 1; i < argc; i++) {
        fetch_stock_price(argv[i]);
    }

    return EXIT_SUCCESS;
}
