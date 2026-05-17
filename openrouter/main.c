/*  main.c – Live terminal stock‑price dashboard
 *
 *  Dependencies:
 *      - libcurl   (apt: libcurl4-openssl-dev, yum: libcurl-devel, etc.)
 *      - cJSON     (cJSON.c + cJSON.h)
 *
 *  Build:
 *      $ make
 *
 *  Run:
 *      $ ./stock-dashboard AAPL MSFT GOOGL
 *
 *  The program will refresh every 5 seconds until you press Ctrl‑C.
 */

#define _POSIX_C_SOURCE 200809L   // for getline()
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <curl/curl.h>
#include "cJSON.h"

/* -------------------------------------------------------------------------- */
/* Helper struct for libcurl response handling                               */
struct memory {
    char *data;
    size_t size;
};

/* libcurl write callback – appends received data to a growing buffer */
static size_t write_callback(void *contents, size_t size, size_t nmemb, void *userp)
{
    size_t realsize = size * nmemb;
    struct memory *mem = (struct memory *)userp;

    char *ptr = realloc(mem->data, mem->size + realsize + 1);
    if (!ptr) {
        fprintf(stderr, "Out of memory (realloc failed)\n");
        return 0;
    }

    mem->data = ptr;
    memcpy(&(mem->data[mem->size]), contents, realsize);
    mem->size += realsize;
    mem->data[mem->size] = '\0';
    return realsize;
}

/* -------------------------------------------------------------------------- */
/* Build the Yahoo Finance URL for a comma‑separated list of symbols          */
static char *build_url(const char *symbols)
{
    const char *base = "https://query1.finance.yahoo.com/v7/finance/quote?symbols=";
    size_t len = strlen(base) + strlen(symbols) + 1;
    char *url = malloc(len);
    if (!url) return NULL;
    snprintf(url, len, "%s%s", base, symbols);
    return url;
}

/* -------------------------------------------------------------------------- */
/* Perform the HTTP GET request, returning the raw JSON string                */
static char *fetch_json(const char *url)
{
    CURL *curl = curl_easy_init();
    if (!curl) {
        fprintf(stderr, "curl_easy_init() failed\n");
        return NULL;
    }

    struct memory chunk = { .data = NULL, .size = 0 };
    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_callback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, (void *)&chunk);
    curl_easy_setopt(curl, CURLOPT_USERAGENT,
                     "stock-dashboard/1.0 (https://github.com/yourname/stock-dashboard)");

    /* Optional: follow redirects (Yahoo sometimes redirects to https) */
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);

    CURLcode res = curl_easy_perform(curl);
    if (res != CURLE_OK) {
        fprintf(stderr, "curl request failed: %s\n", curl_easy_strerror(res));
        free(chunk.data);
        chunk.data = NULL;
    }

    curl_easy_cleanup(curl);
    return chunk.data;   // caller must free()
}

/* -------------------------------------------------------------------------- */
/* Parse the JSON and fill a simple struct for each quote                     */
typedef struct {
    char symbol[32];
    double price;
    double change;
    double change_pct;
} Quote;

static int parse_quotes(const char *json_str, Quote **out_quotes, size_t *out_count)
{
    cJSON *root = cJSON_Parse(json_str);
    if (!root) {
        fprintf(stderr, "cJSON parse error: %s\n", cJSON_GetErrorPtr());
        return -1;
    }

    cJSON *quoteResponse = cJSON_GetObjectItemCaseSensitive(root, "quoteResponse");
    if (!cJSON_IsObject(quoteResponse)) {
        cJSON_Delete(root);
        return -1;
    }

    cJSON *resultArray = cJSON_GetObjectItemCaseSensitive(quoteResponse, "result");
    if (!cJSON_IsArray(resultArray)) {
        cJSON_Delete(root);
        return -1;
    }

    size_t n = cJSON_GetArraySize(resultArray);
    Quote *quotes = calloc(n, sizeof(Quote));
    if (!quotes) {
        cJSON_Delete(root);
        return -1;
    }

    for (size_t i = 0; i < n; ++i) {
        cJSON *item = cJSON_GetArrayItem(resultArray, i);
        cJSON *sym = cJSON_GetObjectItemCaseSensitive(item, "symbol");
        cJSON *price = cJSON_GetObjectItemCaseSensitive(item, "regularMarketPrice");
        cJSON *chg = cJSON_GetObjectItemCaseSensitive(item, "regularMarketChange");
        cJSON *pct = cJSON_GetObjectItemCaseSensitive(item, "regularMarketChangePercent");

        if (cJSON_IsString(sym) && sym->valuestring)
            strncpy(quotes[i].symbol, sym->valuestring, sizeof(quotes[i].symbol) - 1);
        else
            strncpy(quotes[i].symbol, "???", sizeof(quotes[i].symbol) - 1);

        quotes[i].price = cJSON_IsNumber(price) ? price->valuedouble : 0.0;
        quotes[i].change = cJSON_IsNumber(chg) ? chg->valuedouble : 0.0;
        quotes[i].change_pct = cJSON_IsNumber(pct) ? pct->valuedouble : 0.0;
    }

    *out_quotes = quotes;
    *out_count = n;
    cJSON_Delete(root);
    return 0;
}

/* -------------------------------------------------------------------------- */
/* Simple terminal UI helpers – clear screen & colour output                  */
static void clear_screen(void)
{
    /* ANSI escape – works on most Unix terminals */
    printf("\033[2J\033[H");
}

static const char *color_for_change(double change)
{
    if (change > 0) return "\033[32m";   // green
    if (change < 0) return "\033[31m";   // red
    return "\033[0m";                    // default
}

/* -------------------------------------------------------------------------- */
/* Main loop – fetch, parse, display                                            */
int main(int argc, char *argv[])
{
    if (argc < 2) {
        fprintf(stderr, "Usage: %s SYMBOL [SYMBOL ...]\n", argv[0]);
        return EXIT_FAILURE;
    }

    /* Build a comma‑separated list of symbols for the URL */
    size_t symbols_len = 0;
    for (int i = 1; i < argc; ++i)
        symbols_len += strlen(argv[i]) + 1;  // +1 for possible comma
    char *symbols = malloc(symbols_len);
    if (!symbols) return EXIT_FAILURE;
    symbols[0] = '\0';
    for (int i = 1; i < argc; ++i) {
        strcat(symbols, argv[i]);
        if (i != argc - 1) strcat(symbols, ",");
    }

    curl_global_init(CURL_GLOBAL_DEFAULT);

    while (1) {
        char *url = build_url(symbols);
        if (!url) {
            fprintf(stderr, "Failed to allocate URL buffer\n");
            break;
        }

        char *json = fetch_json(url);
        free(url);
        if (!json) {
            fprintf(stderr, "Failed to retrieve data – retrying in 5 s\n");
            sleep(5);
            continue;
        }

        Quote *quotes = NULL;
        size_t count = 0;
        if (parse_quotes(json, &quotes, &count) != 0) {
            fprintf(stderr, "Failed to parse JSON – retrying in 5 s\n");
            free(json);
            sleep(5);
            continue;
        }

        clear_screen();
        printf("Live Stock Dashboard – updated %s", ctime(&(time_t){time(NULL)}));
        printf("%-8s %12s %12s %12s\n", "Symbol", "Price", "Change", "Pct%");

        for (size_t i = 0; i < count; ++i) {
            const char *col = color_for_change(quotes[i].change);
            printf("%-8s %12.2f %s%12.2f %8.2f%%\033[0m\n",
                   quotes[i].symbol,
                   quotes[i].price,
                   col,
                   quotes[i].change,
                   quotes[i].change_pct);
        }

        free(quotes);
        free(json);

        /* Wait 5 seconds before the next refresh */
        sleep(5);
    }

    curl_global_cleanup();
    free(symbols);
    return EXIT_SUCCESS;
}
