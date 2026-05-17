// main.c
// Live stock dashboard using Yahoo Finance quote endpoint + libcurl + cJSON.
//
// Build (Linux/macOS):
//   gcc -O2 -Wall -Wextra -pedantic main.c cJSON.c -lcurl -o stocks
//
// Run:
//   ./stocks 5 AAPL MSFT TSLA
//   (first arg optional refresh interval seconds; default 5)
//
// Notes:
// - Uses https://query1.finance.yahoo.com/v7/finance/quote?symbols=...
// - Adds a User-Agent to reduce chances of being blocked.
// - Yahoo endpoints/behavior can change; this is a simple example.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <signal.h>
#include <time.h>

#include <curl/curl.h>

#include "cJSON.h"

#ifdef _WIN32
#include <windows.h>
static void sleep_seconds(int s) { Sleep((DWORD)s * 1000); }
#else
#include <unistd.h>
static void sleep_seconds(int s) { sleep((unsigned int)s); }
#endif

#define ANSI_CLEAR   "\033[2J\033[H"
#define ANSI_RED     "\033[31m"
#define ANSI_GREEN   "\033[32m"
#define ANSI_YELLOW  "\033[33m"
#define ANSI_RESET   "\033[0m"

static volatile sig_atomic_t g_running = 1;

static void on_sigint(int sig) {
    (void)sig;
    g_running = 0;
}

struct Memory {
    char *data;
    size_t size;
};

static size_t write_cb(void *contents, size_t size, size_t nmemb, void *userp) {
    size_t realsize = size * nmemb;
    struct Memory *mem = (struct Memory *)userp;

    char *ptr = (char *)realloc(mem->data, mem->size + realsize + 1);
    if (!ptr) return 0;

    mem->data = ptr;
    memcpy(&(mem->data[mem->size]), contents, realsize);
    mem->size += realsize;
    mem->data[mem->size] = '\0';
    return realsize;
}

static int is_integer_string(const char *s) {
    if (!s || !*s) return 0;
    while (*s) {
        if (!isdigit((unsigned char)*s)) return 0;
        s++;
    }
    return 1;
}

static char *join_symbols_csv(int argc, char **argv, int start_index) {
    // Joins argv[start_index..argc-1] into "AAPL,MSFT,TSLA"
    size_t total = 0;
    for (int i = start_index; i < argc; i++) {
        total += strlen(argv[i]) + 1; // + comma or null
    }
    if (total == 0) return NULL;

    char *csv = (char *)malloc(total);
    if (!csv) return NULL;
    csv[0] = '\0';

    for (int i = start_index; i < argc; i++) {
        strcat(csv, argv[i]);
        if (i != argc - 1) strcat(csv, ",");
    }
    return csv;
}

static int http_get_quotes(const char *symbols_csv, char **out_json) {
    *out_json = NULL;

    CURL *curl = curl_easy_init();
    if (!curl) return 0;

    struct Memory chunk;
    chunk.data = (char *)malloc(1);
    chunk.size = 0;
    if (!chunk.data) {
        curl_easy_cleanup(curl);
        return 0;
    }

    // Build URL
    const char *base = "https://query1.finance.yahoo.com/v7/finance/quote?symbols=";
    size_t url_len = strlen(base) + strlen(symbols_csv) + 1;
    char *url = (char *)malloc(url_len);
    if (!url) {
        free(chunk.data);
        curl_easy_cleanup(curl);
        return 0;
    }
    snprintf(url, url_len, "%s%s", base, symbols_csv);

    // Optional headers
    struct curl_slist *headers = NULL;
    headers = curl_slist_append(headers, "Accept: application/json");
    headers = curl_slist_append(headers, "Connection: close");

    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);

    // Important: User-Agent to avoid easy blocking
    curl_easy_setopt(curl, CURLOPT_USERAGENT,
                     "Mozilla/5.0 (X11; Linux x86_64) AppleWebKit/537.36 "
                     "(KHTML, like Gecko) Chrome/120.0 Safari/537.36");

    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 10L);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, (void *)&chunk);

    CURLcode res = curl_easy_perform(curl);

    long http_code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);

    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    free(url);

    if (res != CURLE_OK || http_code != 200) {
        free(chunk.data);
        return 0;
    }

    *out_json = chunk.data;
    return 1;
}

static const char *json_get_string(cJSON *obj, const char *key) {
    cJSON *item = cJSON_GetObjectItemCaseSensitive(obj, key);
    if (cJSON_IsString(item) && item->valuestring) return item->valuestring;
    return NULL;
}

static int json_get_double(cJSON *obj, const char *key, double *out) {
    cJSON *item = cJSON_GetObjectItemCaseSensitive(obj, key);
    if (cJSON_IsNumber(item)) {
        *out = item->valuedouble;
        return 1;
    }
    return 0;
}

static int json_get_long(cJSON *obj, const char *key, long *out) {
    cJSON *item = cJSON_GetObjectItemCaseSensitive(obj, key);
    if (cJSON_IsNumber(item)) {
        *out = (long)item->valuedouble;
        return 1;
    }
    return 0;
}

static void print_dashboard(const char *symbols_csv, const char *json_text) {
    cJSON *root = cJSON_Parse(json_text);
    if (!root) {
        printf(ANSI_CLEAR);
        printf("Parse error: invalid JSON\n");
        return;
    }

    cJSON *quoteResponse = cJSON_GetObjectItemCaseSensitive(root, "quoteResponse");
    if (!cJSON_IsObject(quoteResponse)) {
        printf(ANSI_CLEAR);
        printf("Unexpected JSON: missing quoteResponse\n");
        cJSON_Delete(root);
        return;
    }

    // If error exists, show it
    cJSON *err = cJSON_GetObjectItemCaseSensitive(quoteResponse, "error");
    if (err && !cJSON_IsNull(err)) {
        printf(ANSI_CLEAR);
        printf("Yahoo returned an error:\n");
        char *err_str = cJSON_Print(err);
        if (err_str) {
            printf("%s\n", err_str);
            free(err_str);
        }
        cJSON_Delete(root);
        return;
    }

    cJSON *result = cJSON_GetObjectItemCaseSensitive(quoteResponse, "result");
    if (!cJSON_IsArray(result)) {
        printf(ANSI_CLEAR);
        printf("Unexpected JSON: missing result array\n");
        cJSON_Delete(root);
        return;
    }

    time_t now = time(NULL);
    struct tm *lt = localtime(&now);
    char ts[64];
    strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M:%S", lt);

    printf(ANSI_CLEAR);
    printf("Yahoo Finance Live Dashboard  |  %s  |  Symbols: %s\n", ts, symbols_csv);
    printf("Press Ctrl+C to exit\n\n");

    printf("%-10s %-10s %14s %14s %10s %-10s %-12s\n",
           "SYMBOL", "STATE", "PRICE", "CHANGE", "CHG%", "CURRENCY", "MKT_TIME");
    printf("--------------------------------------------------------------------------------\n");

    int n = cJSON_GetArraySize(result);
    for (int i = 0; i < n; i++) {
        cJSON *q = cJSON_GetArrayItem(result, i);
        if (!cJSON_IsObject(q)) continue;

        const char *symbol   = json_get_string(q, "symbol");
        const char *state    = json_get_string(q, "marketState");
        const char *currency = json_get_string(q, "currency");

        double price = 0.0, change = 0.0, chgp = 0.0;
        long mkt_time = 0;

        int ok_price  = json_get_double(q, "regularMarketPrice", &price);
        int ok_change = json_get_double(q, "regularMarketChange", &change);
        int ok_chgp   = json_get_double(q, "regularMarketChangePercent", &chgp);
        json_get_long(q, "regularMarketTime", &mkt_time);

        char mkt_ts[32] = "-";
        if (mkt_time > 0) {
            time_t t = (time_t)mkt_time;
            struct tm *mlt = localtime(&t);
            if (mlt) strftime(mkt_ts, sizeof(mkt_ts), "%H:%M:%S", mlt);
        }

        const char *color = ANSI_YELLOW;
        if (ok_change) {
            if (change > 0) color = ANSI_GREEN;
            else if (change < 0) color = ANSI_RED;
            else color = ANSI_YELLOW;
        }

        printf("%-10s %-10s ",
               symbol ? symbol : "-",
               state ? state : "-");

        if (ok_price) printf("%14.4f ", price);
        else          printf("%14s ", "-");

        printf("%s", color);
        if (ok_change) printf("%14.4f ", change);
        else           printf("%14s ", "-");

        if (ok_chgp) printf("%9.3f%% ", chgp);
        else         printf("%10s ", "-");
        printf(ANSI_RESET);

        printf("%-10s %-12s\n",
               currency ? currency : "-",
               mkt_ts);
    }

    cJSON_Delete(root);
    fflush(stdout);
}

int main(int argc, char **argv) {
    signal(SIGINT, on_sigint);

    if (argc < 2) {
        fprintf(stderr, "Usage:\n");
        fprintf(stderr, "  %s [refresh_seconds] SYMBOL [SYMBOL ...]\n", argv[0]);
        fprintf(stderr, "Example:\n");
        fprintf(stderr, "  %s 5 AAPL MSFT TSLA\n", argv[0]);
        return 1;
    }

    int refresh = 5;
    int sym_start = 1;

    if (argc >= 3 && is_integer_string(argv[1])) {
        refresh = atoi(argv[1]);
        if (refresh <= 0) refresh = 5;
        sym_start = 2;
    }

    if (sym_start >= argc) {
        fprintf(stderr, "Error: no symbols provided.\n");
        return 1;
    }

    char *symbols_csv = join_symbols_csv(argc, argv, sym_start);
    if (!symbols_csv) {
        fprintf(stderr, "Error: failed to build symbols list.\n");
        return 1;
    }

    if (curl_global_init(CURL_GLOBAL_DEFAULT) != 0) {
        fprintf(stderr, "Error: curl_global_init failed.\n");
        free(symbols_csv);
        return 1;
    }

    while (g_running) {
        char *json = NULL;
        if (!http_get_quotes(symbols_csv, &json)) {
            printf(ANSI_CLEAR);
            printf("Fetch failed. Check network / TLS / symbols. Retrying in %d seconds...\n", refresh);
            fflush(stdout);
        } else {
            print_dashboard(symbols_csv, json);
            free(json);
        }

        for (int i = 0; i < refresh && g_running; i++) {
            sleep_seconds(1);
        }
    }

    curl_global_cleanup();
    free(symbols_csv);

    printf("\nExiting.\n");
    return 0;
}
