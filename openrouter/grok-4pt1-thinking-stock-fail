#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <curl/curl.h>
#include "cJSON.h"

/* Memory callback for libcurl */
struct Memory {
    char *data;
    size_t size;
};

static size_t WriteCallback(void *contents, size_t size, size_t nmemb, void *userp)
{
    size_t realsize = size * nmemb;
    struct Memory *mem = (struct Memory *)userp;

    char *ptr = realloc(mem->data, mem->size + realsize + 1);
    if (!ptr) return 0;

    mem->data = ptr;
    memcpy(&(mem->data[mem->size]), contents, realsize);
    mem->size += realsize;
    mem->data[mem->size] = 0;

    return realsize;
}

/* Fetch JSON from Yahoo Finance */
char* yahoo_quote(const char *symbols)
{
    CURL *curl = curl_easy_init();
    if (!curl) return NULL;

    char url[512];
    snprintf(url, sizeof(url),
             "https://query1.finance.yahoo.com/v7/finance/quote?symbols=%s", symbols);

    struct Memory chunk = {0};

    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteCallback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &chunk);
    curl_easy_setopt(curl, CURLOPT_USERAGENT,
        "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/128.0 Safari/537.36");
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 10L);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);

    CURLcode res = curl_easy_perform(curl);
    curl_easy_cleanup(curl);

    if (res != CURLE_OK || chunk.size == 0) {
        if (chunk.data) free(chunk.data);
        return NULL;
    }

    return chunk.data;  /* caller must free */
}

int main(int argc, char **argv)
{
    curl_global_init(CURL_GLOBAL_ALL);

    /* Build comma-separated symbol list */
    char symbols[1024] = {0};
    if (argc == 1) {
        /* Default watchlist if no args */
        strcpy(symbols, "AAPL,MSFT,GOOGL,TSLA,NVDA,AMZN,META,BRK-B,LLY,V");
    } else {
        for (int i = 1; i < argc; i++) {
            if (i > 1) strcat(symbols, ",");
            strcat(symbols, argv[i]);
        }
    }

    while (1) {
        char *json = yahoo_quote(symbols);
        if (!json) {
            printf("\033[2J\033[H"); /* clear screen */
            printf("Failed to fetch data (Yahoo may be rate-limiting or down). Retrying...\n");
            sleep(15);
            continue;
        }

        cJSON *root = cJSON_Parse(json);
        free(json);

        if (!root) {
            printf("\033[2J\033[HJSON parsing failed\n");
            sleep(10);
            continue;
        }

        cJSON *result = cJSON_GetObjectItemCaseSensitive(
            cJSON_GetObjectItemCaseSensitive(root, "quoteResponse"), "result");

        /* Clear screen and print header with timestamp */
        printf("\033[2J\033[H"); /* clear + move cursor to top */

        time_t now = time(NULL);
        char timestr[64];
        strftime(timestr, sizeof(timestr), "%Y-%m-%d %H:%M:%S", localtime(&now));

        printf("\033[1;36m=== Yahoo Finance Live Dashboard ===  %s\033[0m\n\n", timestr);
        printf("\033[1m%-8s %-25s %12s %12s %10s\033[0m\n", "Symbol", "Name", "Price", "Change", "%Chg");
        printf("────────────────────────────────────────────────────────────────────────────\n");

        if (cJSON_IsArray(result)) {
            int n = cJSON_GetArraySize(result);
            for (int i = 0; i < n; i++) {
                cJSON *q = cJSON_GetArrayItem(result, i);

                const char *symbol = cJSON_GetObjectItemCaseSensitive(q, "symbol")->valuestring;
                const char *name   = cJSON_GetObjectItemCaseSensitive(q, "shortName")->valuestring;
                const char *state  = cJSON_GetObjectItemCaseSensitive(q, "marketState")->valuestring;

                double price = 0.0, change = 0.0, chgp = 0.0;
                const char *type = "";

                /* Prefer live price when available */
                if (strcmp(state, "PRE") == 0 || strcmp(state, "PRE_PRE") == 0) {
                    cJSON *p = cJSON_GetObjectItemCaseSensitive(q, "preMarketPrice");
                    if (p) { price = p->valuedouble; type = " (Pre)"; }
                } else if (strcmp(state, "POST") == 0) {
                    cJSON *p = cJSON_GetObjectItemCaseSensitive(q, "postMarketPrice");
                    if (p) { price = p->valuedouble; type = " (Post)"; }
                }
                if (price == 0.0) {
                    price = cJSON_GetObjectItemCaseSensitive(q, "regularMarketPrice")->valuedouble;
                }

                change = cJSON_GetObjectItemCaseSensitive(q,
                    strcmp(state, "POST") == 0 ? "postMarketChange" :
                    strcmp(state, "PRE") == 0  ? "preMarketChange" : "regularMarketChange")->valuedouble;

                chgp   = cJSON_GetObjectItemCaseSensitive(q,
                    strcmp(state, "POST") == 0 ? "postMarketChangePercent" :
                    strcmp(state, "PRE") == 0  ? "preMarketChangePercent" : "regularMarketChangePercent")->valuedouble;

                const char *color = (change >= 0) ? "\033[32m" : "\033[31m";

                printf("%-8s %-25.25s \033[1m$%9.2f\033[0m %s%+10.2f  %+7.2f%%\033[0m%s\n",
                       symbol, name ? name : "", price, color, change, chgp, type);
            }
        } else {
            printf("No data returned (maybe invalid symbols)\n");
        }

        cJSON_Delete(root);
        printf("\n\033[90mRefreshes every 15 seconds • Ctrl+C to quit\033[0m\n");
        fflush(stdout);
        sleep(15);
    }

    curl_global_cleanup();
    return 0;
}
