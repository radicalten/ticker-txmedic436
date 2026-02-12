#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <signal.h>
#include <curl/curl.h>
#include "cJSON.h"

#define MAX_STOCKS 20
#define MAX_SYMBOL_LEN 12
#define REFRESH_INTERVAL 10
#define URL_BUF_SIZE 512

/* ── colour codes ────────────────────────────────────────────────── */
#define C_RESET   "\033[0m"
#define C_BOLD    "\033[1m"
#define C_RED     "\033[31m"
#define C_GREEN   "\033[32m"
#define C_YELLOW  "\033[33m"
#define C_CYAN    "\033[36m"
#define C_WHITE   "\033[37m"
#define C_BG_BLUE "\033[44m"
#define C_DIM     "\033[2m"

/* ── data structures ─────────────────────────────────────────────── */
typedef struct {
    char symbol[MAX_SYMBOL_LEN];
    char name[64];
    double price;
    double change;
    double change_pct;
    double prev_close;
    double open;
    double day_high;
    double day_low;
    double volume;
    double market_cap;
    char market_state[32]; /* PRE, REGULAR, POST, CLOSED */
    int valid;
} StockData;

typedef struct {
    char *data;
    size_t size;
} ResponseBuffer;

static volatile sig_atomic_t g_running = 1;

/* ── signal handler ──────────────────────────────────────────────── */
static void handle_signal(int sig)
{
    (void)sig;
    g_running = 0;
}

/* ── libcurl write callback ──────────────────────────────────────── */
static size_t write_callback(void *contents, size_t size, size_t nmemb,
                             void *userp)
{
    size_t total = size * nmemb;
    ResponseBuffer *buf = (ResponseBuffer *)userp;

    char *tmp = realloc(buf->data, buf->size + total + 1);
    if (!tmp) return 0;

    buf->data = tmp;
    memcpy(buf->data + buf->size, contents, total);
    buf->size += total;
    buf->data[buf->size] = '\0';
    return total;
}

/* ── build Yahoo Finance API URL ─────────────────────────────────── */
static void build_url(char *url, size_t url_len,
                      const char symbols[][MAX_SYMBOL_LEN], int count)
{
    /* v7 quote endpoint supports comma-separated symbols */
    char sym_list[1024] = {0};
    for (int i = 0; i < count; i++) {
        if (i > 0) strcat(sym_list, ",");
        strcat(sym_list, symbols[i]);
    }
    snprintf(url, url_len,
             "https://query1.finance.yahoo.com/v7/finance/quote"
             "?symbols=%s"
             "&fields=symbol,shortName,regularMarketPrice,"
             "regularMarketChange,regularMarketChangePercent,"
             "regularMarketPreviousClose,regularMarketOpen,"
             "regularMarketDayHigh,regularMarketDayLow,"
             "regularMarketVolume,marketCap,marketState",
             sym_list);
}

/* ── fetch JSON from Yahoo Finance ───────────────────────────────── */
static char *fetch_quotes(const char *url)
{
    CURL *curl = curl_easy_init();
    if (!curl) return NULL;

    ResponseBuffer buf = {0};

    struct curl_slist *headers = NULL;
    headers = curl_slist_append(headers,
        "User-Agent: Mozilla/5.0 (Windows NT 10.0; Win64; x64) "
        "AppleWebKit/537.36 (KHTML, like Gecko) "
        "Chrome/124.0.0.0 Safari/537.36");
    headers = curl_slist_append(headers,
        "Accept: application/json");

    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_callback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &buf);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 15L);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);

    /* Yahoo may require a crumb/cookie – try without first.
       If that fails the v6/v8 endpoints can be used instead. */

    CURLcode res = curl_easy_perform(curl);
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);

    if (res != CURLE_OK) {
        free(buf.data);
        return NULL;
    }
    return buf.data;
}

/* ── helper: safely get a double from a cJSON object ─────────────── */
static double json_get_double(const cJSON *obj, const char *key)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(obj, key);
    if (cJSON_IsNumber(item)) return item->valuedouble;
    return 0.0;
}

static const char *json_get_string(const cJSON *obj, const char *key)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(obj, key);
    if (cJSON_IsString(item) && item->valuestring)
        return item->valuestring;
    return "";
}

/* ── parse the Yahoo Finance JSON response ───────────────────────── */
static int parse_quotes(const char *json_str, StockData *stocks, int max)
{
    cJSON *root = cJSON_Parse(json_str);
    if (!root) {
        fprintf(stderr, "JSON parse error: %s\n",
                cJSON_GetErrorPtr() ? cJSON_GetErrorPtr() : "unknown");
        return 0;
    }

    /* Navigate: quoteResponse -> result[] */
    const cJSON *quoteResp =
        cJSON_GetObjectItemCaseSensitive(root, "quoteResponse");
    if (!quoteResp) {
        /* Try v8-style: finance -> result[] */
        quoteResp = cJSON_GetObjectItemCaseSensitive(root, "finance");
    }
    if (!quoteResp) {
        cJSON_Delete(root);
        return 0;
    }

    const cJSON *result =
        cJSON_GetObjectItemCaseSensitive(quoteResp, "result");
    if (!cJSON_IsArray(result)) {
        cJSON_Delete(root);
        return 0;
    }

    int count = 0;
    const cJSON *item;
    cJSON_ArrayForEach(item, result) {
        if (count >= max) break;

        StockData *s = &stocks[count];
        memset(s, 0, sizeof(*s));

        strncpy(s->symbol, json_get_string(item, "symbol"),
                MAX_SYMBOL_LEN - 1);
        strncpy(s->name, json_get_string(item, "shortName"), 63);
        strncpy(s->market_state, json_get_string(item, "marketState"), 31);

        s->price      = json_get_double(item, "regularMarketPrice");
        s->change     = json_get_double(item, "regularMarketChange");
        s->change_pct = json_get_double(item, "regularMarketChangePercent");
        s->prev_close = json_get_double(item, "regularMarketPreviousClose");
        s->open       = json_get_double(item, "regularMarketOpen");
        s->day_high   = json_get_double(item, "regularMarketDayHigh");
        s->day_low    = json_get_double(item, "regularMarketDayLow");
        s->volume     = json_get_double(item, "regularMarketVolume");
        s->market_cap = json_get_double(item, "marketCap");
        s->valid      = (s->price > 0.0);

        count++;
    }

    cJSON_Delete(root);
    return count;
}

/* ── format large numbers (volume, market cap) ───────────────────── */
static void format_large_number(double val, char *buf, size_t len)
{
    if (val >= 1e12)
        snprintf(buf, len, "%.2fT", val / 1e12);
    else if (val >= 1e9)
        snprintf(buf, len, "%.2fB", val / 1e9);
    else if (val >= 1e6)
        snprintf(buf, len, "%.2fM", val / 1e6);
    else if (val >= 1e3)
        snprintf(buf, len, "%.1fK", val / 1e3);
    else
        snprintf(buf, len, "%.0f", val);
}

/* ── simple sparkline from change percentage ─────────────────────── */
static void print_bar(double pct)
{
    int bars = (int)(pct * 2.0); /* 2 blocks per percent */
    if (bars > 20)  bars = 20;
    if (bars < -20) bars = -20;

    if (bars >= 0) {
        printf("%s", C_GREEN);
        for (int i = 0; i < bars; i++) printf("█");
    } else {
        printf("%s", C_RED);
        for (int i = 0; i < -bars; i++) printf("█");
    }
    printf("%s", C_RESET);
}

/* ── clear screen and move cursor home ───────────────────────────── */
static void clear_screen(void)
{
    printf("\033[2J\033[H");
}

/* ── render the dashboard ────────────────────────────────────────── */
static void render_dashboard(const StockData *stocks, int count,
                             const char *last_update, int countdown)
{
    clear_screen();

    /* ── header ── */
    printf("%s%s", C_BG_BLUE, C_BOLD);
    printf("  ╔══════════════════════════════════════════════════"
           "══════════════════════════════════════╗  \n");
    printf("  ║                        📈  LIVE STOCK DASHBOARD "
           "                                     ║  \n");
    printf("  ╚══════════════════════════════════════════════════"
           "══════════════════════════════════════╝  ");
    printf("%s\n", C_RESET);

    printf("  %sLast update: %s   |   Next refresh in %ds   |  "
           " Press Ctrl+C to quit%s\n\n",
           C_DIM, last_update, countdown, C_RESET);

    /* ── column headers ── */
    printf("  %s%-8s  %-22s  %10s  %10s  %8s  %10s  %10s  %10s  %10s%s\n",
           C_BOLD,
           "SYMBOL", "NAME", "PRICE", "CHANGE", "CHG %",
           "OPEN", "HIGH", "LOW", "VOLUME",
           C_RESET);

    printf("  %s", C_DIM);
    for (int i = 0; i < 112; i++) printf("─");
    printf("%s\n", C_RESET);

    /* ── rows ── */
    for (int i = 0; i < count; i++) {
        const StockData *s = &stocks[i];
        if (!s->valid) {
            printf("  %s%-8s  %-22s  %s-- data unavailable --%s\n",
                   C_YELLOW, s->symbol, s->name, C_DIM, C_RESET);
            continue;
        }

        const char *color = (s->change >= 0.0) ? C_GREEN : C_RED;
        const char *arrow = (s->change >= 0.0) ? "▲" : "▼";

        char vol_str[32];
        format_large_number(s->volume, vol_str, sizeof(vol_str));

        /* Truncate name to 22 chars */
        char short_name[23];
        snprintf(short_name, sizeof(short_name), "%.22s", s->name);

        printf("  %s%s%-8s%s  %-22s  %s%10.2f  %s %+9.2f  %+7.2f%%%s"
               "  %10.2f  %10.2f  %10.2f  %10s  ",
               C_BOLD, C_CYAN, s->symbol, C_RESET,
               short_name,
               color, s->price,
               arrow, s->change, s->change_pct, C_RESET,
               s->open, s->day_high, s->day_low, vol_str);

        print_bar(s->change_pct);
        printf("\n");
    }

    /* ── footer ── */
    printf("\n  %s", C_DIM);
    for (int i = 0; i < 112; i++) printf("─");
    printf("%s\n", C_RESET);

    /* Market cap summary */
    printf("  %sMarket Cap:%s  ", C_BOLD, C_RESET);
    for (int i = 0; i < count && i < 8; i++) {
        if (!stocks[i].valid) continue;
        char mc[32];
        format_large_number(stocks[i].market_cap, mc, sizeof(mc));
        printf("%s%s%s: %s  ", C_CYAN, stocks[i].symbol, C_RESET, mc);
    }
    printf("\n");

    /* Market state */
    if (count > 0 && strlen(stocks[0].market_state) > 0) {
        const char *state_color;
        if (strcmp(stocks[0].market_state, "REGULAR") == 0)
            state_color = C_GREEN;
        else if (strcmp(stocks[0].market_state, "PRE") == 0 ||
                 strcmp(stocks[0].market_state, "POST") == 0)
            state_color = C_YELLOW;
        else
            state_color = C_RED;

        printf("  %sMarket State:%s %s%s%s\n",
               C_BOLD, C_RESET,
               state_color, stocks[0].market_state, C_RESET);
    }

    printf("\n");
    fflush(stdout);
}

/* ── alternative endpoint (v8) if v7 doesn't work ────────────────── */
static void build_url_v8(char *url, size_t url_len,
                         const char symbols[][MAX_SYMBOL_LEN], int count)
{
    char sym_list[1024] = {0};
    for (int i = 0; i < count; i++) {
        if (i > 0) strcat(sym_list, ",");
        strcat(sym_list, symbols[i]);
    }
    snprintf(url, url_len,
             "https://query1.finance.yahoo.com/v8/finance/spark"
             "?symbols=%s&range=1d&interval=1d",
             sym_list);
}

/* Try fetching with the v6 quote endpoint as a fallback */
static void build_url_v6(char *url, size_t url_len,
                         const char symbols[][MAX_SYMBOL_LEN], int count)
{
    char sym_list[1024] = {0};
    for (int i = 0; i < count; i++) {
        if (i > 0) strcat(sym_list, ",");
        strcat(sym_list, symbols[i]);
    }
    snprintf(url, url_len,
             "https://query1.finance.yahoo.com/v6/finance/quote"
             "?symbols=%s",
             sym_list);
}

/* ── try to get a Yahoo crumb + cookies (needed for some endpoints) ── */
typedef struct {
    char crumb[128];
    char cookie[512];
    int valid;
} YahooCrumb;

static YahooCrumb get_crumb(void)
{
    YahooCrumb yc = {0};
    CURL *curl = curl_easy_init();
    if (!curl) return yc;

    ResponseBuffer buf = {0};

    struct curl_slist *headers = NULL;
    headers = curl_slist_append(headers,
        "User-Agent: Mozilla/5.0 (Windows NT 10.0; Win64; x64) "
        "AppleWebKit/537.36 (KHTML, like Gecko) "
        "Chrome/124.0.0.0 Safari/537.36");

    /* Step 1: get consent / cookies from Yahoo */
    char cookie_file[] = "/tmp/yahoo_cookies_XXXXXX";
    int fd = mkstemp(cookie_file);
    if (fd < 0) {
        curl_slist_free_all(headers);
        curl_easy_cleanup(curl);
        return yc;
    }
    close(fd);

    curl_easy_setopt(curl, CURLOPT_URL,
                     "https://fc.yahoo.com/");
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_callback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &buf);
    curl_easy_setopt(curl, CURLOPT_COOKIEJAR, cookie_file);
    curl_easy_setopt(curl, CURLOPT_COOKIEFILE, cookie_file);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 10L);

    curl_easy_perform(curl); /* we don't care about the result */

    /* Step 2: get crumb */
    free(buf.data);
    buf.data = NULL;
    buf.size = 0;

    curl_easy_setopt(curl, CURLOPT_URL,
                     "https://query2.finance.yahoo.com/v1/test/getcrumb");
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &buf);

    CURLcode res = curl_easy_perform(curl);
    if (res == CURLE_OK && buf.data && buf.size > 0 && buf.size < 128) {
        strncpy(yc.crumb, buf.data, sizeof(yc.crumb) - 1);

        /* Extract cookie string */
        struct curl_slist *cookies = NULL;
        curl_easy_getinfo(curl, CURLINFO_COOKIELIST, &cookies);
        yc.cookie[0] = '\0';
        struct curl_slist *each = cookies;
        while (each) {
            /* Netscape cookie format: domain \t ... \t name \t value */
            char *tab = strrchr(each->data, '\t');
            if (tab) {
                char *name_start = tab;
                /* go back one more tab to get name */
                *tab = '\0';
                char *prev_tab = strrchr(each->data, '\t');
                if (prev_tab) {
                    char *name = prev_tab + 1;
                    char *value = tab + 1;
                    /* restore */
                    *tab = '\t';
                    char pair[256];
                    snprintf(pair, sizeof(pair), "%s=%s; ", name, value);
                    if (strlen(yc.cookie) + strlen(pair) < sizeof(yc.cookie))
                        strcat(yc.cookie, pair);
                } else {
                    *tab = '\t';
                }
            }
            each = each->next;
        }
        curl_slist_free_all(cookies);
        yc.valid = 1;
    }

    free(buf.data);
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    unlink(cookie_file);

    return yc;
}

static char *fetch_quotes_with_crumb(const char *url,
                                     const YahooCrumb *yc)
{
    CURL *curl = curl_easy_init();
    if (!curl) return NULL;

    ResponseBuffer buf = {0};

    char full_url[1024];
    snprintf(full_url, sizeof(full_url), "%s&crumb=%s", url, yc->crumb);

    struct curl_slist *headers = NULL;
    headers = curl_slist_append(headers,
        "User-Agent: Mozilla/5.0 (Windows NT 10.0; Win64; x64) "
        "AppleWebKit/537.36 (KHTML, like Gecko) "
        "Chrome/124.0.0.0 Safari/537.36");
    headers = curl_slist_append(headers, "Accept: application/json");

    char cookie_header[600];
    snprintf(cookie_header, sizeof(cookie_header), "Cookie: %s",
             yc->cookie);
    headers = curl_slist_append(headers, cookie_header);

    curl_easy_setopt(curl, CURLOPT_URL, full_url);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_callback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &buf);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 15L);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);

    CURLcode res = curl_easy_perform(curl);
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);

    if (res != CURLE_OK) {
        free(buf.data);
        return NULL;
    }
    return buf.data;
}

/* ── usage ───────────────────────────────────────────────────────── */
static void print_usage(const char *prog)
{
    fprintf(stderr,
        "Usage: %s [SYMBOL1 SYMBOL2 ...]\n"
        "\n"
        "Examples:\n"
        "  %s AAPL MSFT GOOGL AMZN TSLA\n"
        "  %s SPY QQQ DIA\n"
        "\n"
        "If no symbols are given, a default watchlist is used.\n",
        prog, prog, prog);
}

/* ═══════════════════════════════════════════════════════════════════ */
int main(int argc, char *argv[])
{
    /* Default watchlist */
    const char default_symbols[][MAX_SYMBOL_LEN] = {
        "AAPL", "MSFT", "GOOGL", "AMZN", "TSLA",
        "META", "NVDA", "SPY", "QQQ", "BTC-USD"
    };
    int default_count = 10;

    char symbols[MAX_STOCKS][MAX_SYMBOL_LEN];
    int sym_count = 0;

    if (argc > 1 && (strcmp(argv[1], "-h") == 0 ||
                     strcmp(argv[1], "--help") == 0)) {
        print_usage(argv[0]);
        return 0;
    }

    if (argc > 1) {
        for (int i = 1; i < argc && sym_count < MAX_STOCKS; i++) {
            strncpy(symbols[sym_count], argv[i], MAX_SYMBOL_LEN - 1);
            symbols[sym_count][MAX_SYMBOL_LEN - 1] = '\0';
            /* Convert to uppercase */
            for (char *p = symbols[sym_count]; *p; p++) {
                if (*p >= 'a' && *p <= 'z') *p -= 32;
            }
            sym_count++;
        }
    } else {
        sym_count = default_count;
        for (int i = 0; i < default_count; i++)
            strcpy(symbols[i], default_symbols[i]);
    }

    /* Install signal handlers */
    signal(SIGINT, handle_signal);
    signal(SIGTERM, handle_signal);

    /* Init libcurl globally */
    curl_global_init(CURL_GLOBAL_DEFAULT);

    clear_screen();
    printf("\n  %s%sInitializing stock dashboard...%s\n", C_BOLD, C_CYAN,
           C_RESET);
    printf("  Tracking %d symbols: ", sym_count);
    for (int i = 0; i < sym_count; i++)
        printf("%s%s%s ", C_YELLOW, symbols[i], C_RESET);
    printf("\n\n");
    printf("  %sObtaining Yahoo Finance session...%s\n", C_DIM, C_RESET);
    fflush(stdout);

    /* Obtain crumb for authenticated requests */
    YahooCrumb crumb = get_crumb();
    if (crumb.valid) {
        printf("  %s✓ Session established%s\n", C_GREEN, C_RESET);
    } else {
        printf("  %s⚠ Could not get crumb, trying without...%s\n",
               C_YELLOW, C_RESET);
    }
    fflush(stdout);
    sleep(1);

    StockData stocks[MAX_STOCKS];
    memset(stocks, 0, sizeof(stocks));

    /* ── main loop ── */
    while (g_running) {
        char url[URL_BUF_SIZE];
        build_url(url, sizeof(url), (const char (*)[MAX_SYMBOL_LEN])symbols,
                  sym_count);

        char *json = NULL;

        if (crumb.valid) {
            json = fetch_quotes_with_crumb(url, &crumb);
        }

        if (!json) {
            json = fetch_quotes(url);
        }

        /* If v7 failed, try v6 */
        if (!json || strstr(json, "\"error\"")) {
            free(json);
            build_url_v6(url, sizeof(url),
                         (const char (*)[MAX_SYMBOL_LEN])symbols, sym_count);
            if (crumb.valid)
                json = fetch_quotes_with_crumb(url, &crumb);
            if (!json)
                json = fetch_quotes(url);
        }

        /* Get timestamp */
        time_t now = time(NULL);
        struct tm *tm_info = localtime(&now);
        char time_str[64];
        strftime(time_str, sizeof(time_str), "%Y-%m-%d %H:%M:%S", tm_info);

        int count = 0;
        if (json) {
            count = parse_quotes(json, stocks, sym_count);
            free(json);
        }

        if (count == 0) {
            /* Show error but keep previous data if any */
            clear_screen();
            printf("\n  %s%s⚠  Failed to fetch data. Retrying in %ds...%s\n",
                   C_BOLD, C_RED, REFRESH_INTERVAL, C_RESET);
            printf("  %sTime: %s%s\n\n", C_DIM, time_str, C_RESET);

            /* Possibly refresh crumb */
            printf("  %sRefreshing session...%s\n", C_DIM, C_RESET);
            fflush(stdout);
            crumb = get_crumb();

            for (int c = REFRESH_INTERVAL; c > 0 && g_running; c--) {
                sleep(1);
            }
            continue;
        }

        /* Countdown display */
        for (int c = REFRESH_INTERVAL; c >= 0 && g_running; c--) {
            render_dashboard(stocks, count, time_str, c);
            if (c > 0) sleep(1);
        }
    }

    /* Cleanup */
    curl_global_cleanup();

    /* Restore terminal */
    printf("\n%s  Dashboard stopped. Goodbye! 👋%s\n\n", C_CYAN, C_RESET);
    return 0;
}
