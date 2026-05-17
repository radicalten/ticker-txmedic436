#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <curl/curl.h>
#include "cJSON.h"

/* -------------------------------------------------------------------------- */
/* Helper: write callback for libcurl – appends received data to a buffer      */
static size_t write_callback(void *contents, size_t size, size_t nmemb, void *userp) {
    size_t realsize = size * nmemb;
    struct {
        char *data;
        size_t size;
    } *structp = (struct {
        char *data;
        size_t size;
    } *)userp;

    char *ptr = realloc(structp->data, structp->size + realsize + 1);
    if (!ptr) {
        fprintf(stderr, "realloc() failed\n");
        return 0;
    }
    structp->data = ptr;
    memcpy(&(structp->data[structp->size]), contents, realsize);
    structp->size += realsize;
    structp->data[structp->size] = '\0';
    return realsize;
}

/* -------------------------------------------------------------------------- */
/* Fetch the current price for a given symbol using Yahoo Finance JSON API      */
static double fetch_stock_price(const char *symbol) {
    CURL *curl;
    CURLcode res;
    double price = -1.0;

    /* Build URL */
    char url[256];
    snprintf(url, sizeof(url), "https://query1.finance.yahoo.com/v7/finance/quote?symbols=%s", symbol);

    curl = curl_easy_init();
    if (!curl) {
        fprintf(stderr, "curl_easy_init() failed\n");
        return price;
    }

    struct {
        char *data;
        size_t size;
    } chunk = { .data = malloc(1), .size = 0 };

    struct curl_slist *headers = NULL;
    headers = curl_slist_append(headers, "User-Agent: Mozilla/5.0 (compatible; LiveDashboard/1.0)");
    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_callback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, (void *)&chunk);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 10L);

    res = curl_easy_perform(curl);
    if (res != CURLE_OK) {
        fprintf(stderr, "curl_easy_perform() failed: %s\n", curl_easy_strerror(res));
        free(chunk.data);
        curl_slist_free_all(headers);
        curl_easy_cleanup(curl);
        return price;
    }

    /* Parse JSON */
    cJSON *root = cJSON_Parse(chunk.data);
    if (!root) {
        fprintf(stderr, "JSON parse error\n");
        free(chunk.data);
        curl_slist_free_all(headers);
        curl_easy_cleanup(curl);
        return price;
    }

    cJSON *quoteResponse = cJSON_GetObjectItemCaseSensitive(root, "quoteResponse");
    if (cJSON_IsObject(quoteResponse)) {
        cJSON *result = cJSON_GetObjectItemCaseSensitive(quoteResponse, "result");
        if (cJSON_IsArray(result) && cJSON_GetArraySize(result) > 0) {
            cJSON *sym = cJSON_GetArrayItem(result, 0);
            if (cJSON_IsObject(sym) && cJSON_HasObjectItem(sym, "regularMarketPrice")) {
                cJSON *priceItem = cJSON_GetObjectItem(sym, "regularMarketPrice");
                if (cJSON_IsNumber(priceItem)) {
                    price = priceItem->valuedouble;
                }
            }
        }
    }

    cJSON_Delete(root);
    free(chunk.data);
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);

    return price;
}

/* -------------------------------------------------------------------------- */
/* Simple pretty‑print of the price with a timestamp                           */
static void print_price(const char *symbol, double price) {
    time_t now = time(NULL);
    struct tm tm_now;
    localtime_r(&now, &tm_now);
    char timestr[64];
    strftime(timestr, sizeof(timestr), "%Y-%m-%d %H:%M:%S", &tm_now);

    printf("\r%s: $%.2f (as of %s) ", symbol, price, timestr);
    fflush(stdout);
}

/* -------------------------------------------------------------------------- */
/* Main program – loop over a list of symbols and refresh the display         */
int main(void) {
    /* Example list – you can modify or read from stdin / args */
    const char *symbols[] = { "AAPL", "MSFT", "GOOGL", "TSLA", "AMZN" };
    const size_t num_symbols = sizeof(symbols) / sizeof(symbols[0]);

    printf("Live Stock Dashboard – press Ctrl‑C to exit\n");
    for (;;) {
        for (size_t i = 0; i < num_symbols; ++i) {
            double price = fetch_stock_price(symbols[i]);
            if (price >= 0.0) {
                print_price(symbols[i], price);
            } else {
                printf("\r%s: error fetching price ", symbols[i]);
            }
        }
        printf("\n");
        fflush(stdout);
        sleep(30);   // refresh interval (seconds)
    }

    return 0;
}
