// main.c
//
// Simple terminal-based live stock dashboard using:
//   - cJSON.c / cJSON.h by Dave Gamble
//   - Yahoo Finance quote endpoint
//   - libcurl for HTTPS requests
//
// Build:
//   gcc main.c cJSON.c -o stocks -lcurl
//
// Run:
//   ./stocks AAPL MSFT TSLA NVDA
//
// Notes:
//   - Requires libcurl development package
//   - Uses a browser-like User-Agent so Yahoo Finance does not reject requests

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <curl/curl.h>

#include "cJSON.h"

#define REFRESH_SECONDS 5
#define MAX_URL_LEN 2048

typedef struct {
    char *memory;
    size_t size;
} MemoryBlock;

typedef struct {
    char symbol[32];
    double price;
    double change;
    double changePercent;
    char currency[16];
    char marketState[32];
} StockQuote;

static size_t write_callback(void *contents, size_t size, size_t nmemb, void *userp)
{
    size_t realsize = size * nmemb;
    MemoryBlock *mem = (MemoryBlock *)userp;

    char *ptr = realloc(mem->memory, mem->size + realsize + 1);
    if (!ptr)
        return 0;

    mem->memory = ptr;

    memcpy(&(mem->memory[mem->size]), contents, realsize);

    mem->size += realsize;
    mem->memory[mem->size] = '\0';

    return realsize;
}

static int http_get(const char *url, MemoryBlock *chunk)
{
    CURL *curl = curl_easy_init();
    if (!curl)
        return 0;

    chunk->memory = malloc(1);
    chunk->size = 0;

    struct curl_slist *headers = NULL;

    headers = curl_slist_append(
        headers,
        "User-Agent: Mozilla/5.0 (X11; Linux x86_64) "
        "AppleWebKit/537.36 (KHTML, like Gecko) "
        "Chrome/124.0 Safari/537.36"
    );

    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 10L);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_callback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, (void *)chunk);

    CURLcode res = curl_easy_perform(curl);

    long response_code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &response_code);

    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);

    if (res != CURLE_OK || response_code != 200)
        return 0;

    return 1;
}

static int parse_quotes(const char *json, StockQuote *quotes, int maxQuotes)
{
    cJSON *root = cJSON_Parse(json);
    if (!root)
        return 0;

    cJSON *quoteResponse = cJSON_GetObjectItem(root, "quoteResponse");
    cJSON *result = cJSON_GetObjectItem(quoteResponse, "result");

    if (!cJSON_IsArray(result)) {
        cJSON_Delete(root);
        return 0;
    }

    int count = cJSON_GetArraySize(result);
    if (count > maxQuotes)
        count = maxQuotes;

    for (int i = 0; i < count; i++) {
        cJSON *item = cJSON_GetArrayItem(result, i);

        const char *symbol =
            cJSON_GetStringValue(cJSON_GetObjectItem(item, "symbol"));

        const char *currency =
            cJSON_GetStringValue(cJSON_GetObjectItem(item, "currency"));

        const char *marketState =
            cJSON_GetStringValue(cJSON_GetObjectItem(item, "marketState"));

        cJSON *priceObj =
            cJSON_GetObjectItem(item, "regularMarketPrice");

        cJSON *changeObj =
            cJSON_GetObjectItem(item, "regularMarketChange");

        cJSON *changePercentObj =
            cJSON_GetObjectItem(item, "regularMarketChangePercent");

        snprintf(quotes[i].symbol, sizeof(quotes[i].symbol),
                 "%s", symbol ? symbol : "N/A");

        snprintf(quotes[i].currency, sizeof(quotes[i].currency),
                 "%s", currency ? currency : "N/A");

        snprintf(quotes[i].marketState, sizeof(quotes[i].marketState),
                 "%s", marketState ? marketState : "N/A");

        quotes[i].price =
            cJSON_IsNumber(priceObj) ? priceObj->valuedouble : 0.0;

        quotes[i].change =
            cJSON_IsNumber(changeObj) ? changeObj->valuedouble : 0.0;

        quotes[i].changePercent =
            cJSON_IsNumber(changePercentObj)
                ? changePercentObj->valuedouble
                : 0.0;
    }

    cJSON_Delete(root);
    return count;
}

static void build_url(char *url, size_t size, char **symbols, int count)
{
    snprintf(url, size,
             "https://query1.finance.yahoo.com/v7/finance/quote?symbols=");

    for (int i = 0; i < count; i++) {
        strncat(url, symbols[i], size - strlen(url) - 1);

        if (i != count - 1)
            strncat(url, ",", size - strlen(url) - 1);
    }
}

static void clear_screen(void)
{
    printf("\033[2J");
    printf("\033[H");
}

static void print_dashboard(StockQuote *quotes, int count)
{
    clear_screen();

    printf("==============================================\n");
    printf("      Yahoo Finance Live Stock Dashboard\n");
    printf("==============================================\n\n");

    printf("%-10s %-12s %-12s %-12s %-10s\n",
           "SYMBOL", "PRICE", "CHANGE", "% CHANGE", "STATE");

    printf("--------------------------------------------------------------\n");

    for (int i = 0; i < count; i++) {

        const char *color =
            quotes[i].change >= 0 ? "\033[32m" : "\033[31m";

        printf("%-10s ", quotes[i].symbol);

        printf("%-12.2f ", quotes[i].price);

        printf("%s%-12.2f %-12.2f\033[0m ",
               color,
               quotes[i].change,
               quotes[i].changePercent);

        printf("%-10s\n", quotes[i].marketState);
    }

    printf("\nRefreshing every %d seconds...\n", REFRESH_SECONDS);
}

int main(int argc, char *argv[])
{
    if (argc < 2) {
        printf("Usage: %s SYMBOL [SYMBOL...]\n", argv[0]);
        printf("Example: %s AAPL MSFT TSLA\n", argv[0]);
        return 1;
    }

    curl_global_init(CURL_GLOBAL_DEFAULT);

    char url[MAX_URL_LEN];

    build_url(url, sizeof(url), &argv[1], argc - 1);

    while (1) {

        MemoryBlock response;

        if (!http_get(url, &response)) {
            fprintf(stderr, "Failed to fetch stock data.\n");
            sleep(REFRESH_SECONDS);
            continue;
        }

        StockQuote quotes[128];

        int count = parse_quotes(response.memory, quotes, 128);

        if (count > 0)
            print_dashboard(quotes, count);
        else
            fprintf(stderr, "Failed to parse JSON.\n");

        free(response.memory);

        sleep(REFRESH_SECONDS);
    }

    curl_global_cleanup();

    return 0;
}
