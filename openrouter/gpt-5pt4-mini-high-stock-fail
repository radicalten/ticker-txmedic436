// main.c
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <time.h>

#include <curl/curl.h>
#include "cJSON.h"

#if defined(_WIN32)
#include <windows.h>
#else
#include <unistd.h>
#endif

#define YAHOO_QUOTE_URL_BASE "https://query1.finance.yahoo.com/v7/finance/quote?symbols="
#define DEFAULT_INTERVAL 5

// Browser-like UA to reduce Yahoo rejections
#define UA_STRING "Mozilla/5.0 (X11; Linux x86_64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/125.0.0.0 Safari/537.36"

#define COL_RESET "\033[0m"
#define COL_BOLD  "\033[1m"
#define COL_RED   "\033[31m"
#define COL_GREEN "\033[32m"

static volatile sig_atomic_t g_stop = 0;

static void on_sigint(int sig)
{
    (void)sig;
    g_stop = 1;
}

static void restore_terminal(void)
{
    printf(COL_RESET "\033[?25h");
    fflush(stdout);
}

typedef struct {
    char *data;
    size_t size;
} Memory;

static size_t write_cb(void *contents, size_t size, size_t nmemb, void *userp)
{
    size_t realsize = size * nmemb;
    Memory *mem = (Memory *)userp;

    char *ptr = (char *)realloc(mem->data, mem->size + realsize + 1);
    if (!ptr) {
        return 0; // abort transfer
    }

    mem->data = ptr;
    memcpy(&(mem->data[mem->size]), contents, realsize);
    mem->size += realsize;
    mem->data[mem->size] = '\0';

    return realsize;
}

static int progress_cb(void *clientp,
                       curl_off_t dltotal, curl_off_t dlnow,
                       curl_off_t ultotal, curl_off_t ulnow)
{
    (void)clientp;
    (void)dltotal;
    (void)dlnow;
    (void)ultotal;
    (void)ulnow;
    return g_stop ? 1 : 0;
}

static void sleep_seconds(int seconds)
{
    for (int i = 0; i < seconds && !g_stop; ++i) {
#if defined(_WIN32)
        Sleep(1000);
#else
        sleep(1);
#endif
    }
}

static void usage(const char *prog)
{
    fprintf(stderr, "Usage: %s [-i seconds] SYMBOL [SYMBOL...]\n", prog);
    fprintf(stderr, "Example: %s -i 3 AAPL MSFT TSLA\n", prog);
}

static int collect_symbols(int argc, char **argv, int *interval, const char ***out_symbols)
{
    const char *defaults[] = {"AAPL", "MSFT", "GOOGL", "TSLA", "NVDA"};
    const size_t default_count = sizeof(defaults) / sizeof(defaults[0]);

    size_t capacity = (size_t)argc + default_count + 1;
    const char **symbols = (const char **)calloc(capacity, sizeof(*symbols));
    if (!symbols) {
        return -1;
    }

    int count = 0;

    for (int i = 1; i < argc; ++i) {
        if (!strcmp(argv[i], "-h") || !strcmp(argv[i], "--help")) {
            free(symbols);
            usage(argv[0]);
            exit(0);
        } else if (!strcmp(argv[i], "-i") || !strcmp(argv[i], "--interval")) {
            if (i + 1 >= argc) {
                free(symbols);
                usage(argv[0]);
                return -1;
            }
            *interval = atoi(argv[++i]);
        } else if (argv[i][0] == '-' && argv[i][1] != '\0') {
            fprintf(stderr, "Unknown option: %s\n", argv[i]);
            free(symbols);
            usage(argv[0]);
            return -1;
        } else {
            symbols[count++] = argv[i];
        }
    }

    if (count == 0) {
        for (size_t i = 0; i < default_count; ++i) {
            symbols[count++] = defaults[i];
        }
    }

    *out_symbols = symbols;
    return count;
}

static int append_header(struct curl_slist **headers, const char *line)
{
    struct curl_slist *tmp = curl_slist_append(*headers, line);
    if (!tmp) {
        return -1;
    }
    *headers = tmp;
    return 0;
}

static char *build_symbol_param(CURL *curl, const char **symbols, int symbol_count)
{
    if (symbol_count <= 0) {
        return NULL;
    }

    char **escaped = (char **)calloc((size_t)symbol_count, sizeof(*escaped));
    if (!escaped) {
        return NULL;
    }

    size_t total = 0;
    for (int i = 0; i < symbol_count; ++i) {
        escaped[i] = curl_easy_escape(curl, symbols[i], 0);
        if (!escaped[i]) {
            for (int j = 0; j < i; ++j) {
                curl_free(escaped[j]);
            }
            free(escaped);
            return NULL;
        }
        total += strlen(escaped[i]);
        if (i + 1 < symbol_count) {
            total += 1; // comma
        }
    }

    char *param = (char *)malloc(total + 1);
    if (!param) {
        for (int i = 0; i < symbol_count; ++i) {
            curl_free(escaped[i]);
        }
        free(escaped);
        return NULL;
    }

    char *p = param;
    for (int i = 0; i < symbol_count; ++i) {
        size_t len = strlen(escaped[i]);
        memcpy(p, escaped[i], len);
        p += len;
        if (i + 1 < symbol_count) {
            *p++ = ',';
        }
        curl_free(escaped[i]);
    }
    *p = '\0';

    free(escaped);
    return param;
}

static char *build_url(const char *symbol_param)
{
    size_t len = strlen(YAHOO_QUOTE_URL_BASE) + strlen(symbol_param) + 1;
    char *url = (char *)malloc(len);
    if (!url) {
        return NULL;
    }

    snprintf(url, len, "%s%s", YAHOO_QUOTE_URL_BASE, symbol_param);
    return url;
}

static CURLcode fetch_url(CURL *curl, const char *url, struct curl_slist *headers,
                          char **out_body, long *out_http_code)
{
    Memory mem;
    mem.data = (char *)malloc(1);
    if (!mem.data) {
        return CURLE_OUT_OF_MEMORY;
    }
    mem.data[0] = '\0';
    mem.size = 0;

    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, UA_STRING);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &mem);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_ACCEPT_ENCODING, "");
    curl_easy_setopt(curl, CURLOPT_COOKIEFILE, "");
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS, 5000L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, 12000L);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);

    curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
    curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, progress_cb);
    curl_easy_setopt(curl, CURLOPT_XFERINFODATA, NULL);

    CURLcode res = curl_easy_perform(curl);
    if (out_http_code) {
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, out_http_code);
    }

    if (res != CURLE_OK) {
        free(mem.data);
        return res;
    }

    *out_body = mem.data;
    return CURLE_OK;
}

static const cJSON *find_result_by_symbol(const cJSON *results, const char *symbol)
{
    if (!cJSON_IsArray(results)) {
        return NULL;
    }

    const cJSON *item = NULL;
    cJSON_ArrayForEach(item, results) {
        const cJSON *sym = cJSON_GetObjectItemCaseSensitive(item, "symbol");
        if (cJSON_IsString(sym) && sym->valuestring && strcmp(sym->valuestring, symbol) == 0) {
            return item;
        }
    }

    return NULL;
}

static void json_string_or_na(const cJSON *obj, const char *key, char *buf, size_t bufsz)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(obj, key);
    if (cJSON_IsString(v) && v->valuestring) {
        snprintf(buf, bufsz, "%s", v->valuestring);
    } else {
        snprintf(buf, bufsz, "N/A");
    }
}

static void json_number_or_na(const cJSON *obj, const char *key, char *buf, size_t bufsz, const char *fmt)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(obj, key);
    if (cJSON_IsNumber(v)) {
        snprintf(buf, bufsz, fmt, v->valuedouble);
    } else {
        snprintf(buf, bufsz, "N/A");
    }
}

static void display_name_or_na(const cJSON *item, char *buf, size_t bufsz)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(item, "shortName");
    if (!(cJSON_IsString(v) && v->valuestring)) {
        v = cJSON_GetObjectItemCaseSensitive(item, "longName");
    }

    if (cJSON_IsString(v) && v->valuestring) {
        snprintf(buf, bufsz, "%s", v->valuestring);
    } else {
        snprintf(buf, bufsz, "N/A");
    }
}

static void render_error_screen(const char *url, const char *message, long http_code, const char *body)
{
    printf("\033[2J\033[H");
    printf(COL_BOLD "Yahoo Finance Live Dashboard" COL_RESET "\n\n");
    printf("Error : %s\n", message);
    if (http_code > 0) {
        printf("HTTP  : %ld\n", http_code);
    }
    printf("URL   : %s\n", url ? url : "(null)");
    if (body && body[0]) {
        printf("\nResponse snippet:\n%.600s\n", body);
    }
    fflush(stdout);
}

static void print_dashboard(const char **symbols, int symbol_count,
                            const char *body, const char *url,
                            int interval, long http_code)
{
    char ts[64];
    time_t now = time(NULL);
    struct tm *tm_info = localtime(&now);
    if (tm_info) {
        strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M:%S", tm_info);
    } else {
        snprintf(ts, sizeof(ts), "unknown");
    }

    printf("\033[2J\033[H");
    printf(COL_BOLD "Yahoo Finance Live Dashboard" COL_RESET "\n");
    printf("Endpoint: %s\n", url);
    printf("Updated : %s | Refresh: %d sec | Ctrl+C to quit\n\n", ts, interval);

    cJSON *root = cJSON_Parse(body ? body : "");
    if (!root) {
        render_error_screen(url, "Failed to parse JSON response", http_code, body);
        return;
    }

    const cJSON *quoteResponse = cJSON_GetObjectItemCaseSensitive(root, "quoteResponse");
    const cJSON *results = quoteResponse ? cJSON_GetObjectItemCaseSensitive(quoteResponse, "result") : NULL;

    if (!cJSON_IsArray(results)) {
        cJSON_Delete(root);
        render_error_screen(url, "Unexpected JSON format", http_code, body);
        return;
    }

    printf("%-10s %-30s %12s %12s %12s %-12s %-10s\n",
           "SYMBOL", "NAME", "PRICE", "CHANGE", "%CHANGE", "STATE", "CUR");
    printf("---------------------------------------------------------------------------------------------------------------\n");

    for (int i = 0; i < symbol_count; ++i) {
        const char *sym = symbols[i];
        const cJSON *item = find_result_by_symbol(results, sym);

        char name[128], price[32], change[32], pct[32], state[32], currency[16];
        const char *color = COL_RESET;

        if (item) {
            display_name_or_na(item, name, sizeof(name));
            json_number_or_na(item, "regularMarketPrice", price, sizeof(price), "%.2f");
            json_number_or_na(item, "regularMarketChange", change, sizeof(change), "%+.2f");
            json_number_or_na(item, "regularMarketChangePercent", pct, sizeof(pct), "%+.2f%%");
            json_string_or_na(item, "marketState", state, sizeof(state));
            json_string_or_na(item, "currency", currency, sizeof(currency));

            const cJSON *chg = cJSON_GetObjectItemCaseSensitive(item, "regularMarketChange");
            if (cJSON_IsNumber(chg)) {
                if (chg->valuedouble > 0.0) {
                    color = COL_GREEN;
                } else if (chg->valuedouble < 0.0) {
                    color = COL_RED;
                }
            }
        } else {
            snprintf(name, sizeof(name), "N/A");
            snprintf(price, sizeof(price), "N/A");
            snprintf(change, sizeof(change), "N/A");
            snprintf(pct, sizeof(pct), "N/A");
            snprintf(state, sizeof(state), "MISSING");
            snprintf(currency, sizeof(currency), "N/A");
        }

        printf("%s%-10.10s %-30.30s %12s %12s %12s %-12.12s %-10.10s%s\n",
               color, sym, name, price, change, pct, state, currency, COL_RESET);
    }

    cJSON_Delete(root);
    fflush(stdout);
}

int main(int argc, char **argv)
{
    int interval = DEFAULT_INTERVAL;
    const char **symbols = NULL;
    int symbol_count = collect_symbols(argc, argv, &interval, &symbols);
    if (symbol_count < 0) {
        return 1;
    }
    if (interval < 1) {
        interval = DEFAULT_INTERVAL;
    }

    signal(SIGINT, on_sigint);
    atexit(restore_terminal);

    setvbuf(stdout, NULL, _IONBF, 0);
    printf("\033[?25l"); // hide cursor
    fflush(stdout);

    if (curl_global_init(CURL_GLOBAL_DEFAULT) != CURLE_OK) {
        fprintf(stderr, "curl_global_init failed\n");
        free(symbols);
        return 1;
    }

    CURL *curl = curl_easy_init();
    if (!curl) {
        fprintf(stderr, "curl_easy_init failed\n");
        curl_global_cleanup();
        free(symbols);
        return 1;
    }

    char *symbol_param = build_symbol_param(curl, symbols, symbol_count);
    if (!symbol_param) {
        fprintf(stderr, "Failed to build symbol list\n");
        curl_easy_cleanup(curl);
        curl_global_cleanup();
        free(symbols);
        return 1;
    }

    char *url = build_url(symbol_param);
    if (!url) {
        fprintf(stderr, "Failed to build URL\n");
        free(symbol_param);
        curl_easy_cleanup(curl);
        curl_global_cleanup();
        free(symbols);
        return 1;
    }

    struct curl_slist *headers = NULL;
    if (append_header(&headers, "Accept: application/json,text/plain,*/*") != 0 ||
        append_header(&headers, "Accept-Language: en-US,en;q=0.9") != 0 ||
        append_header(&headers, "Referer: https://finance.yahoo.com/") != 0) {
        fprintf(stderr, "Failed to build HTTP headers\n");
        curl_slist_free_all(headers);
        free(url);
        free(symbol_param);
        curl_easy_cleanup(curl);
        curl_global_cleanup();
        free(symbols);
        return 1;
    }

    while (!g_stop) {
        char *body = NULL;
        long http_code = 0;

        CURLcode res = fetch_url(curl, url, headers, &body, &http_code);

        if (g_stop) {
            free(body);
            break;
        }

        if (res != CURLE_OK) {
            render_error_screen(url, curl_easy_strerror(res), http_code, NULL);
        } else if (http_code != 200) {
            render_error_screen(url, "Yahoo Finance returned a non-200 response", http_code, body);
        } else {
            print_dashboard(symbols, symbol_count, body, url, interval, http_code);
        }

        free(body);

        if (g_stop) {
            break;
        }

        sleep_seconds(interval);
    }

    curl_slist_free_all(headers);
    free(url);
    free(symbol_param);
    curl_easy_cleanup(curl);
    curl_global_cleanup();
    free(symbols);

    return 0;
}
