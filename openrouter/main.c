/*
 *  main.c
 *
 *  Simple terminal‑based live stock‑price dashboard.
 *
 *  Dependencies:
 *      - libcurl   (for HTTP GET)
 *      - cJSON     (Dave Gamble's JSON parser)
 *
 *  Build (example):
 *      gcc -Wall -O2 main.c cJSON.c -lcurl -o stockdash
 *
 *  Run:
 *      ./stockdash AAPL MSFT GOOGL TSLA
 *
 *  The program will query Yahoo Finance every 5 seconds and display the
 *  latest price for each symbol supplied on the command line.
 *
 *  Author:  ChatGPT (2026)
 *  License: MIT
 */

#define _POSIX_C_SOURCE 200809L   /* for getline() */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <curl/curl.h>
#include "cJSON.h"

/* -------------------------------------------------------------------------- */
/* Helper struct to collect the HTTP response body                           */
struct Memory {
    char *data;
    size_t size;     // number of bytes currently stored
};

/* Curl write callback – appends received data to the Memory buffer         */
static size_t WriteMemoryCallback(void *contents, size_t size,
                                  size_t nmemb, void *userp)
{
    size_t realsize = size * nmemb;
    struct Memory *mem = (struct Memory *)userp;

    char *ptr = realloc(mem->data, mem->size + realsize + 1);
    if (!ptr) {
        fprintf(stderr, "Out of memory (realloc failed)\n");
        return 0;   // abort the transfer
    }

    mem->data = ptr;
    memcpy(&(mem->data[mem->size]), contents, realsize);
    mem->size += realsize;
    mem->data[mem->size] = 0;   // null‑terminate
    return realsize;
}

/* -------------------------------------------------------------------------- */
/* Build the Yahoo Finance request URL                                      */
static char *build_url(const char *symbols_csv)
{
    const char *base = "https://query1.finance.yahoo.com/v7/finance/quote?symbols=";
    size_t len = strlen(base) + strlen(symbols_csv) + 1;
    char *url = malloc(len);
    if (!url) {
        perror("malloc");
        exit(EXIT_FAILURE);
    }
    snprintf(url, len, "%s%s", base, symbols_csv);
    return url;
}

/* -------------------------------------------------------------------------- */
/* Perform the HTTP GET and return the raw JSON string (caller must free)    */
static char *download_json(const char *url)
{
    CURL *curl;
    CURLcode res;
    struct Memory chunk = { .data = NULL, .size = 0 };

    curl = curl_easy_init();
    if (!curl) {
        fprintf(stderr, "curl_easy_init() failed\n");
        exit(EXIT_FAILURE);
    }

    /* ----------- set the request options --------------------------------- */
    curl_easy_setopt(curl, CURLOPT_URL, url);

    /* Add a custom User‑Agent (Yahoo rejects default libcurl UA). */
    struct curl_slist *headers = NULL;
    headers = curl_slist_append(headers,
        "User-Agent: stockdash/1.0 (+https://github.com/yourname/stockdash)");
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);

    /* Follow redirects (Yahoo may redirect to https). */
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);

    /* Receive the body in our callback. */
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteMemoryCallback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, (void *)&chunk);

    /* Perform the request. */
    res = curl_easy_perform(curl);
    if (res != CURLE_OK) {
        fprintf(stderr, "curl_easy_perform() failed: %s\n",
                curl_easy_strerror(res));
        curl_slist_free_all(headers);
        curl_easy_cleanup(curl);
        free(chunk.data);
        exit(EXIT_FAILURE);
    }

    /* Cleanup. */
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);

    /* Return the buffer – the caller owns it. */
    return chunk.data;   // may be NULL if no data received
}

/* -------------------------------------------------------------------------- */
/* Extract the needed fields from the Yahoo JSON response                    */
static void parse_and_print(const char *json_str, const char **sym_list,
                            size_t sym_count)
{
    cJSON *root = cJSON_Parse(json_str);
    if (!root) {
        fprintf(stderr, "JSON parse error: %s\n", cJSON_GetErrorPtr());
        return;
    }

    cJSON *quoteResponse = cJSON_GetObjectItem(root, "quoteResponse");
    if (!quoteResponse) {
        fprintf(stderr, "Unexpected JSON format – missing quoteResponse\n");
        cJSON_Delete(root);
        return;
    }

    cJSON *result = cJSON_GetObjectItem(quoteResponse, "result");
    if (!cJSON_IsArray(result)) {
        fprintf(stderr, "Unexpected JSON format – result is not an array\n");
        cJSON_Delete(root);
        return;
    }

    /* Print a header line */
    printf("%-10s %-12s %-8s %-10s\n",
           "SYMBOL", "PRICE", "CHANGE", "PCT_CHANGE");
    printf("%-10s %-12s %-8s %-10s\n",
           "------", "-----", "------", "----------");

    /* Walk through the array and print the requested symbols */
    for (size_t i = 0; i < sym_count; ++i) {
        const char *wanted = sym_list[i];
        cJSON *found = NULL;

        /* Linear search – the array is tiny (≤ few dozen symbols). */
        cJSON_ArrayForEach(found, result) {
            cJSON *sym = cJSON_GetObjectItem(found, "symbol");
            if (cJSON_IsString(sym) && strcmp(sym->valuestring, wanted) == 0)
                break;   // found points to the element we need
            found = NULL;   // not this one
        }

        if (!found) {
            printf("%-10s %-12s %-8s %-10s\n",
                   wanted, "N/A", "N/A", "N/A");
            continue;
        }

        /* Extract price, change and percent change (may be missing). */
        cJSON *price = cJSON_GetObjectItem(found, "regularMarketPrice");
        cJSON *chg   = cJSON_GetObjectItem(found, "regularMarketChange");
        cJSON *pct   = cJSON_GetObjectItem(found, "regularMarketChangePercent");

        double p = cJSON_IsNumber(price) ? price->valuedouble : 0.0;
        double c = cJSON_IsNumber(chg)   ? chg->valuedouble   : 0.0;
        double pc = cJSON_IsNumber(pct)  ? pct->valuedouble   : 0.0;

        /* Show a +/- sign for change values */
        char change_str[16];
        char pct_str[16];
        snprintf(change_str, sizeof(change_str), "%+0.2f", c);
        snprintf(pct_str, sizeof(pct_str), "%+0.2f%%", pc);

        printf("%-10s $%-11.2f %-8s %-10s\n",
               wanted, p, change_str, pct_str);
    }

    cJSON_Delete(root);
}

/* -------------------------------------------------------------------------- */
static void clear_screen(void)
{
#if defined(_WIN32) || defined(_WIN64)
    system("cls");
#else
    /* POSIX: write ANSI escape code to clear screen */
    printf("\033[2J\033[H");
    fflush(stdout);
#endif
}

/* -------------------------------------------------------------------------- */
int main(int argc, char *argv[])
{
    if (argc < 2) {
        fprintf(stderr,
                "Usage: %s SYMBOL1 [SYMBOL2 ...]\n"
                "Example: %s AAPL MSFT GOOGL TSLA\n",
                argv[0], argv[0]);
        return EXIT_FAILURE;
    }

    const char **symbols = (const char **)(argv + 1);
    size_t sym_count = (size_t)(argc - 1);

    /* Build a CSV string "AAPL,MSFT,GOOGL" for the URL. */
    size_t csv_len = 0;
    for (size_t i = 0; i < sym_count; ++i)
        csv_len += strlen(symbols[i]) + 1;   // symbol + possible comma

    char *csv = malloc(csv_len);
    if (!csv) {
        perror("malloc");
        return EXIT_FAILURE;
    }
    csv[0] = '\0';
    for (size_t i = 0; i < sym_count; ++i) {
        strcat(csv, symbols[i]);
        if (i + 1 < sym_count)
            strcat(csv, ",");
    }

    /* Initialise libcurl once. */
    curl_global_init(CURL_GLOBAL_DEFAULT);

    const int REFRESH_SECONDS = 5;   // how often to poll Yahoo

    while (1) {
        char *url = build_url(csv);
        char *json_data = download_json(url);
        free(url);

        clear_screen();
        printf("Yahoo Finance Live Dashboard – updated %s", ctime(&(time_t){time(NULL)}));
        parse_and_print(json_data, symbols, sym_count);
        free(json_data);

        /* Sleep between refreshes. */
        sleep(REFRESH_SECONDS);
    }

    /* Unreachable in this loop example, but good practice. */
    curl_global_cleanup();
    free(csv);
    return EXIT_SUCCESS;
}
