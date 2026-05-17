#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <cJSON.h>
#include <curl/curl.h>

// User-Agent string to avoid Yahoo Finance rejection
#define USER_AGENT "SimpleStockDashboard/1.0 (your_email@example.com)" // Replace with your email

// Function to fetch stock price from Yahoo Finance
cJSON *get_stock_price(const char *symbol) {
    CURL *curl;
    CURLcode res;
    char *url;
    cJSON *json;
    char *data;

    // Construct the Yahoo Finance API URL
    url = (char *)malloc(strlen("https://query1.finance.yahoo.com/v7/finance/quote?symbol=") + strlen(symbol) + 40); // +40 for parameters
    if (!url) {
        perror("malloc failed");
        return NULL;
    }
    sprintf(url, "https://query1.finance.yahoo.com/v7/finance/quote?symbol=%s", symbol);

    curl = curl_easy_init();
    if (curl) {
        curl_easy_setopt(curl, CURLOPT_URL, url);
        curl_easy_setopt(curl, CURLOPT_USERAGENT, USER_AGENT);
        curl_easy_setopt(curl, CURLOPT_RETURNTRANSFER, 1);

        res = curl_easy_perform(curl);
        if (res != CURLE_OK) {
            fprintf(stderr, "curl_easy_perform() failed: %s\n", curl_easy_strerror(res));
            free(url);
            return NULL;
        }

        curl_easy_cleanup(curl);

        // Get the response data
        data = malloc(curl_easy_getinfo(curl, CURLINFO_CONTENT_LENGTH_KB) * sizeof(char));
        if (!data) {
            perror("malloc failed");
            free(url);
            return NULL;
        }

        curl_easy_getinfo(curl, CURLINFO_CONTENT_LENGTH_KB, (long *)&curl_content_length);
        curl_easy_perform(curl); // Perform again to get content length

        res = curl_easy
