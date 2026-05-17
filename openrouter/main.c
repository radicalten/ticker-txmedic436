// main.c
// Build: gcc -O2 -Wall -Wextra main.c cJSON.c -lcurl -o stockdash
// Run:   ./stockdash AAPL,MSFT,TSLA 5
//
// Requires: libcurl and cJSON.c/cJSON.h present in your project.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <signal.h>
#include <time.h>
#include <unistd.h>

#include <curl/curl.h>
#include "cJSON.h"

#define MAX_SYMBOLS 32
#define MAX_SYMBOL_LEN 16
#define USER_AGENT "Mozilla/5.0 (compatible; StockPriceDashboard/1.0; +https://example.com)"

typedef struct {
    char symbol[MAX_SYMBOL_LEN];
    int hasData;

    double price;
    double change;
    double changePercent;

    long long quoteTimeUnix; // optional
    char currency[8];
} Quote;

static volatile sig_atomic_t g_stop = 0;

static void on_sigint(int sig) {
    (void)sig;
    g_stop = 1;
}

static char *trim_whitespace(char *s) {
    if (!s) return s;
    while (isspace((unsigned char)*s)) s++;
    if (*s == 0) return s;

    char *end = s + strlen(s) - 1;
    while (end > s && isspace((unsigned char)*end)) end--;
    end[1] = '\0';
    return s;
}

static int split_symbols(char *input, char symbols[][MAX_SYMBOL_LEN], int maxSymbols) {
    int count = 0;
    char *p = input;

    while (*p && count < maxSymbols) {
        // find comma or end
        char *comma = strchr(p, ',');
        if (comma) *comma = '\0';

        char *tok = trim_whitespace(p);
        if (*tok) {
            strncpy(symbols[count], tok, MAX_SYMBOL_LEN - 1);
            symbols[count][MAX_SYMBOL_LEN - 1] = '\0';
            count++;
        }

        if (!comma) break;
        p = comma + 1;
    }

    return count;
}

static void join_symbols(char out[], size_t outsz, char symbols[][MAX_SYMBOL_LEN], int count) {
    out[0] = '\0';
    for (int i = 0; i < count; i++) {
        if (i > 0) strncat(out, ",", outsz - strlen(out) - 1);
        strncat(out, symbols[i], outsz - strlen(out) - 1);
    }
}

struct Memory {
    char *data;
    size_t size;
};

static size_t curl_write_cb(void *contents, size_t size, size_t nmemb, void *userp) {
    size_t realsize = size * nmemb;
    struct Memory *mem = (struct Memory *)userp;

    char *ptr = realloc(mem->data, mem->size + realsize + 1);
    if (!ptr) return 0; // out of memory => abort

    mem->data = ptr;
    memcpy(&(mem->data[mem->size]), contents, realsize);
    mem->size += realsize;
    mem->data[mem->size] = '\0';
    return realsize;
}

static int http_get_json(const char *url, char **out_json) {
    *out_json = NULL;

    CURL *curl = curl_easy_init();
    if (!curl) return -1;

    struct Memory mem = {0};
    mem.data = malloc(1);
    if (!mem.data) {
        curl_easy_cleanup(curl);
        return -1;
    }
    mem.data[0] = '\0';
    mem.size = 0;

    struct curl_slist *headers = NULL;
    headers = curl_slist_append(headers, "Accept: application/json");

    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, USER_AGENT);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);

    // Timeouts so the dashboard doesn't hang forever
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 10L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 20L);

    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curl_write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, (void *)&mem);

    CURLcode res = curl_easy_perform(curl);

    long http_code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);

    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);

    if (res != CURLE_OK || http_code < 200 || http_code >= 300) {
        free(mem.data);
        return -2;
    }

    *out_json = mem.data;
    return 0;
}

static int get_double_from_obj(cJSON *obj, const char *key, double *out) {
    cJSON *item = cJSON_GetObjectItemCaseSensitive(obj, key);
    if (!item) return 0;
    if (cJSON_IsNumber(item)) {
        *out = item->valuedouble;
        return 1;
    }
    return 0;
}

static int get_ll_from_obj(cJSON *obj, const char *key, long long *out) {
    cJSON *item = cJSON_GetObjectItemCaseSensitive(obj, key);
    if (!item) return 0;
    if (cJSON_IsNumber(item)) {
        *out = (long long)item->valuedouble;
        return 1;
    }
    return 0;
}

static const char* get_str_from_obj(cJSON *obj, const char *key) {
    cJSON *item = cJSON_GetObjectItemCaseSensitive(obj, key);
    if (!item) return NULL;
    if (cJSON_IsString(item) && item->valuestring) return item->valuestring;
    return NULL;
}

static void parse_quotes_into(char symbols[][MAX_SYMBOL_LEN], int symCount, const char *json, Quote quotes[]) {
    for (int i = 0; i < symCount; i++) {
        quotes[i].hasData = 0;
        quotes[i].price = 0.0;
        quotes[i].change = 0.0;
        quotes[i].changePercent = 0.0;
        quotes[i].quoteTimeUnix = 0;
        quotes[i].currency[0] = '\0';
        strncpy(quotes[i].symbol, symbols[i], MAX_SYMBOL_LEN - 1);
        quotes[i].symbol[MAX_SYMBOL_LEN - 1] = '\0';
    }

    cJSON *root = cJSON_Parse(json);
    if (!root) return;

    cJSON *quoteResponse = cJSON_GetObjectItemCaseSensitive(root, "quoteResponse");
    if (!quoteResponse) {
        cJSON_Delete(root);
        return;
    }

    cJSON *resultArr = cJSON_GetObjectItemCaseSensitive(quoteResponse, "result");
    if (!resultArr || !cJSON_IsArray(resultArr)) {
        cJSON_Delete(root);
        return;
    }

    int resultCount = cJSON_GetArraySize(resultArr);
    for (int i = 0; i < resultCount; i++) {
        cJSON *entry = cJSON_GetArrayItem(resultArr, i);
        if (!entry) continue;

        const char *sym = get_str_from_obj(entry, "symbol");
        if (!sym) continue;

        // find matching index in our requested symbols
        int idx = -1;
        for (int k = 0; k < symCount; k++) {
            if (strcmp(quotes[k].symbol, sym) == 0) {
                idx = k;
                break;
            }
        }
        if (idx < 0) continue;

        quotes[idx].hasData = 1;

        double d;
        if (get_double_from_obj(entry, "regularMarketPrice", &d)) quotes[idx].price = d;
        if (get_double_from_obj(entry, "regularMarketChange", &d)) quotes[idx].change = d;
        if (get_double_from_obj(entry, "regularMarketChangePercent", &d)) quotes[idx].changePercent = d;

        long long t;
        if (get_ll_from_obj(entry, "regularMarketTime", &t)) quotes[idx].quoteTimeUnix = t;

        const char *cur = get_str_from_obj(entry, "currency");
        if (cur) {
            strncpy(quotes[idx].currency, cur, sizeof(quotes[idx].currency) - 1);
            quotes[idx].currency[sizeof(quotes[idx].currency) - 1] = '\0';
        }
    }

    cJSON_Delete(root);
}

static void clear_and_home(void) {
    // ANSI: clear screen + move cursor home
    printf("\033[2J\033[H");
}

static void print_table(Quote *quotes, int count) {
    printf("%-10s %-14s %-14s %-10s %-8s\n",
           "SYMBOL", "PRICE", "CHANGE", "CHG%", "TIME");
    printf("--------------------------------------------------------------------------\n");

    for (int i = 0; i < count; i++) {
        if (!quotes[i].hasData) {
            printf("%-10s %-14s %-14s %-10s %-8s\n",
                   quotes[i].symbol, "-", "-", "-", "-");
            continue;
        }

        // format time (if present)
        char tbuf[32];
        if (quotes[i].quoteTimeUnix > 0) {
            time_t tt = (time_t)quotes[i].quoteTimeUnix;
            struct tm tmv;
            localtime_r(&tt, &tmv);
            strftime(tbuf, sizeof(tbuf), "%H:%M:%S", &tmv);
        } else {
            snprintf(tbuf, sizeof(tbuf), "-");
        }

        printf("%-10s %-14.4f %-14.4f %-10.2f %-8s\n",
               quotes[i].symbol,
               quotes[i].price,
               quotes[i].change,
               quotes[i].changePercent,
               tbuf);
    }
}

int main(int argc, char **argv) {
    signal(SIGINT, on_sigint);

    int intervalSeconds = 5;

    char symbolsInput[512] = {0};
    if (argc >= 2) {
        strncpy(symbolsInput, argv[1], sizeof(symbolsInput) - 1);
    } else {
        printf("Enter symbols (comma-separated), e.g. AAPL,MSFT,TSLA: ");
        fflush(stdout);
        if (!fgets(symbolsInput, sizeof(symbolsInput), stdin)) {
            fprintf(stderr, "No input.\n");
            return 1;
        }
        symbolsInput[strcspn(symbolsInput, "\r\n")] = 0;
    }

    if (argc >= 3) {
        intervalSeconds = atoi(argv[2]);
        if (intervalSeconds < 1) intervalSeconds = 5;
    }

    char symbols[MAX_SYMBOLS][MAX_SYMBOL_LEN];
    int symCount = split_symbols(symbolsInput, symbols, MAX_SYMBOLS);
    if (symCount <= 0) {
        fprintf(stderr, "No valid symbols provided.\n");
        return 1;
    }

    char joined[512];
    join_symbols(joined, sizeof(joined), symbols, symCount);

    Quote *quotes = calloc((size_t)symCount, sizeof(Quote));
    if (!quotes) {
        fprintf(stderr, "Out of memory.\n");
        return 1;
    }

    curl_global_init(CURL_GLOBAL_DEFAULT);

    while (!g_stop) {
        // Build URL
        char url[1024];
        snprintf(url, sizeof(url),
                 "https://query1.finance.yahoo.com/v7/finance/quote?symbols=%s",
                 joined);

        char *json = NULL;
        int rc = http_get_json(url, &json);

        clear_and_home();

        time_t now = time(NULL);
        struct tm tmv;
        localtime_r(&now, &tmv);
        char nowbuf[64];
        strftime(nowbuf, sizeof(nowbuf), "%Y-%m-%d %H:%M:%S", &tmv);

        if (rc == 0 && json) {
            parse_quotes_into(symbols, symCount, json, quotes);
            free(json);
            printf("Yahoo Finance Quotes (refreshed: %s) | symbols: %s\n\n", nowbuf, joined);
            print_table(quotes, symCount);
        } else {
            printf("Failed to fetch quotes (error code: %d) | refreshed: %s\n\n",
                   rc, nowbuf);
            // Show whatever we had (or nothing)
            print_table(quotes, symCount);
        }

        fflush(stdout);

        for (int i = 0; i < intervalSeconds && !g_stop; i++) {
            sleep(1);
        }
    }

    free(quotes);
    curl_global_cleanup();

    printf("\nExiting...\n");
    return 0;
}
