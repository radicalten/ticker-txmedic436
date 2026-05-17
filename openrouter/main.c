#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <curl/curl.h>
#include "cJSON.h"

/* ─── Configuration ──────────────────────────────────────────── */
#define MAX_SYMBOLS      32
#define SYMBOL_LEN       16
#define REFRESH_SECONDS  10
#define URL_BUF          2048
#define HTTP_BUF_INIT    65536

/* Fake-but-plausible browser User-Agent so Yahoo doesn't reject us */
#define USER_AGENT \
    "Mozilla/5.0 (X11; Linux x86_64) " \
    "AppleWebKit/537.36 (KHTML, like Gecko) " \
    "Chrome/124.0.0.0 Safari/537.36"

/* Yahoo Finance v8 quote endpoint */
#define YAHOO_URL \
    "https://query1.finance.yahoo.com/v8/finance/quote?symbols=%s" \
    "&fields=symbol,regularMarketPrice,regularMarketChange," \
    "regularMarketChangePercent,regularMarketVolume," \
    "regularMarketDayHigh,regularMarketDayLow,shortName"

/* ─── Colour helpers ─────────────────────────────────────────── */
#define COL_RESET   "\033[0m"
#define COL_BOLD    "\033[1m"
#define COL_RED     "\033[31m"
#define COL_GREEN   "\033[32m"
#define COL_YELLOW  "\033[33m"
#define COL_CYAN    "\033[36m"
#define COL_WHITE   "\033[37m"
#define COL_GREY    "\033[90m"

/* Clear screen + move cursor home */
#define CLEAR_SCREEN() printf("\033[2J\033[H")

/* ─── Data structures ────────────────────────────────────────── */
typedef struct {
    char   symbol[SYMBOL_LEN];
    char   short_name[64];
    double price;
    double change;
    double change_pct;
    double day_high;
    double day_low;
    long   volume;
    int    valid;          /* 1 = populated, 0 = error/missing */
} Quote;

typedef struct {
    char  *data;
    size_t size;
} MemBuf;

/* ─── libcurl write callback ─────────────────────────────────── */
static size_t write_cb(void *contents, size_t sz, size_t nmemb, void *userp)
{
    size_t   real  = sz * nmemb;
    MemBuf  *buf   = (MemBuf *)userp;
    char    *ptr   = realloc(buf->data, buf->size + real + 1);

    if (!ptr) { fputs("realloc failed\n", stderr); return 0; }

    buf->data = ptr;
    memcpy(buf->data + buf->size, contents, real);
    buf->size             += real;
    buf->data[buf->size]   = '\0';
    return real;
}

/* ─── Build comma-separated symbol list ─────────────────────── */
static void build_symbol_str(const char symbols[][SYMBOL_LEN],
                              int n, char *out, size_t outsz)
{
    out[0] = '\0';
    for (int i = 0; i < n; i++) {
        strncat(out, symbols[i], outsz - strlen(out) - 1);
        if (i < n - 1)
            strncat(out, "%2C", outsz - strlen(out) - 1); /* URL-encoded comma */
    }
}

/* ─── Fetch + parse Yahoo Finance ────────────────────────────── */
static int fetch_quotes(const char symbols[][SYMBOL_LEN], int nsyms,
                        Quote *quotes)
{
    char sym_str[MAX_SYMBOLS * (SYMBOL_LEN + 3)] = {0};
    char url[URL_BUF]                            = {0};
    CURL      *curl;
    CURLcode   res;
    MemBuf     buf = { NULL, 0 };
    int        ret = 0;

    build_symbol_str(symbols, nsyms, sym_str, sizeof sym_str);
    snprintf(url, sizeof url, YAHOO_URL, sym_str);

    curl = curl_easy_init();
    if (!curl) { fputs("curl_easy_init failed\n", stderr); return -1; }

    /* Allocate initial buffer */
    buf.data = malloc(HTTP_BUF_INIT);
    if (!buf.data) { curl_easy_cleanup(curl); return -1; }
    buf.data[0] = '\0';

    curl_easy_setopt(curl, CURLOPT_URL,            url);
    curl_easy_setopt(curl, CURLOPT_USERAGENT,      USER_AGENT);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION,  write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA,      &buf);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT,        10L);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    /* Accept gzip so Yahoo is happier */
    curl_easy_setopt(curl, CURLOPT_ACCEPT_ENCODING, "gzip, deflate");

    res = curl_easy_perform(curl);
    if (res != CURLE_OK) {
        fprintf(stderr, "curl error: %s\n", curl_easy_strerror(res));
        ret = -1; goto cleanup;
    }

    /* ── JSON parsing ── */
    cJSON *root = cJSON_Parse(buf.data);
    if (!root) {
        fprintf(stderr, "JSON parse error near: %.80s\n",
                cJSON_GetErrorPtr() ? cJSON_GetErrorPtr() : "?");
        ret = -1; goto cleanup;
    }

    /* Path: root -> quoteResponse -> result -> [array of quotes] */
    cJSON *qr = cJSON_GetObjectItemCaseSensitive(root, "quoteResponse");
    if (!qr) { fputs("No 'quoteResponse' key\n", stderr); cJSON_Delete(root); ret=-1; goto cleanup; }

    cJSON *result = cJSON_GetObjectItemCaseSensitive(qr, "result");
    if (!cJSON_IsArray(result)) { fputs("'result' is not an array\n", stderr); cJSON_Delete(root); ret=-1; goto cleanup; }

    /* Zero-initialise all quote slots */
    memset(quotes, 0, nsyms * sizeof(Quote));

    cJSON *item = NULL;
    cJSON_ArrayForEach(item, result) {

        /* Find which slot this symbol belongs to */
        cJSON *sym_j = cJSON_GetObjectItemCaseSensitive(item, "symbol");
        if (!cJSON_IsString(sym_j)) continue;

        int slot = -1;
        for (int i = 0; i < nsyms; i++) {
            if (strcasecmp(sym_j->valuestring, symbols[i]) == 0) {
                slot = i; break;
            }
        }
        if (slot < 0) continue;

        Quote *q = &quotes[slot];
        strncpy(q->symbol, sym_j->valuestring, SYMBOL_LEN - 1);

        cJSON *v;
#define GET_DBL(field, member) \
        v = cJSON_GetObjectItemCaseSensitive(item, field); \
        if (cJSON_IsNumber(v)) q->member = v->valuedouble;

#define GET_STR(field, member, sz) \
        v = cJSON_GetObjectItemCaseSensitive(item, field); \
        if (cJSON_IsString(v)) strncpy(q->member, v->valuestring, sz - 1);

        GET_DBL("regularMarketPrice",         price)
        GET_DBL("regularMarketChange",        change)
        GET_DBL("regularMarketChangePercent", change_pct)
        GET_DBL("regularMarketDayHigh",       day_high)
        GET_DBL("regularMarketDayLow",        day_low)
        GET_STR("shortName",                  short_name, 64)

        v = cJSON_GetObjectItemCaseSensitive(item, "regularMarketVolume");
        if (cJSON_IsNumber(v)) q->volume = (long)v->valuedouble;

        q->valid = 1;
    }

    cJSON_Delete(root);

cleanup:
    free(buf.data);
    curl_easy_cleanup(curl);
    return ret;
}

/* ─── Formatting helpers ─────────────────────────────────────── */
static const char *change_colour(double chg)
{
    if (chg > 0) return COL_GREEN;
    if (chg < 0) return COL_RED;
    return COL_WHITE;
}

static void fmt_volume(long vol, char *out, size_t sz)
{
    if      (vol >= 1000000000L) snprintf(out, sz, "%.2fB", vol / 1e9);
    else if (vol >= 1000000L)    snprintf(out, sz, "%.2fM", vol / 1e6);
    else if (vol >= 1000L)       snprintf(out, sz, "%.2fK", vol / 1e3);
    else                         snprintf(out, sz, "%ld",   vol);
}

/* ─── Draw the dashboard ─────────────────────────────────────── */
static void draw_dashboard(const Quote *quotes, int n, time_t fetched_at)
{
    CLEAR_SCREEN();

    /* ── Header ── */
    printf(COL_BOLD COL_CYAN
           "╔══════════════════════════════════════════════════════════"
           "══════════════════╗\n"
           "║          📈  LIVE STOCK DASHBOARD  (Yahoo Finance)       "
           "                  ║\n"
           "╚══════════════════════════════════════════════════════════"
           "══════════════════╝\n"
           COL_RESET);

    /* ── Timestamp ── */
    char tbuf[64];
    struct tm *tm_info = localtime(&fetched_at);
    strftime(tbuf, sizeof tbuf, "%Y-%m-%d  %H:%M:%S", tm_info);
    printf(COL_GREY "  Last updated: %s   (refreshes every %ds)\n\n"
           COL_RESET, tbuf, REFRESH_SECONDS);

    /* ── Column headers ── */
    printf(COL_BOLD COL_YELLOW
           "  %-6s  %-22s  %10s  %10s  %8s  %10s  %10s  %10s\n"
           COL_RESET,
           "TICKER", "NAME", "PRICE", "CHANGE", "CHG %",
           "DAY HIGH", "DAY LOW", "VOLUME");

    printf(COL_GREY);
    for (int i = 0; i < 98; i++) putchar('-');
    printf(COL_RESET "\n");

    /* ── Rows ── */
    for (int i = 0; i < n; i++) {
        const Quote *q = &quotes[i];

        if (!q->valid) {
            printf("  " COL_RED "%-6s  %-22s  %s\n" COL_RESET,
                   q->symbol[0] ? q->symbol : "???", "", "(no data)");
            continue;
        }

        const char *col = change_colour(q->change);
        char arrow      = (q->change >= 0) ? '+' : '-'; /* sign already in value */
        char volstr[16] = {0};
        fmt_volume(q->volume, volstr, sizeof volstr);

        printf("  " COL_BOLD "%-6s" COL_RESET
               "  " COL_WHITE "%-22.22s" COL_RESET
               "  " COL_BOLD "%10.4f" COL_RESET
               "  %s%+10.4f" COL_RESET
               "  %s%7.2f%%" COL_RESET
               "  %10.4f"
               "  %10.4f"
               "  %10s\n",
               q->symbol,
               q->short_name,
               q->price,
               col, q->change,
               col, q->change_pct,
               q->day_high,
               q->day_low,
               volstr);

        (void)arrow; /* silence unused-variable warning */
    }

    printf(COL_GREY);
    for (int i = 0; i < 98; i++) putchar('-');
    printf(COL_RESET "\n");

    printf(COL_GREY "  Press Ctrl+C to quit.\n" COL_RESET);
    fflush(stdout);
}

/* ─── Entry point ────────────────────────────────────────────── */
int main(int argc, char *argv[])
{
    /* Default watchlist – override via command-line args */
    static const char defaults[][SYMBOL_LEN] = {
        "AAPL", "MSFT", "GOOGL", "AMZN", "NVDA",
        "TSLA", "META",  "BRK-B", "JPM",  "V"
    };

    char symbols[MAX_SYMBOLS][SYMBOL_LEN];
    int  nsyms = 0;

    if (argc > 1) {
        for (int i = 1; i < argc && nsyms < MAX_SYMBOLS; i++) {
            strncpy(symbols[nsyms++], argv[i], SYMBOL_LEN - 1);
        }
    } else {
        int def_n = (int)(sizeof defaults / sizeof defaults[0]);
        for (int i = 0; i < def_n && i < MAX_SYMBOLS; i++) {
            strncpy(symbols[nsyms++], defaults[i], SYMBOL_LEN - 1);
        }
    }

    curl_global_init(CURL_GLOBAL_DEFAULT);

    Quote quotes[MAX_SYMBOLS];

    while (1) {
        time_t now = time(NULL);
        fetch_quotes((const char (*)[SYMBOL_LEN])symbols, nsyms, quotes);
        draw_dashboard(quotes, nsyms, now);
        sleep(REFRESH_SECONDS);
    }

    curl_global_cleanup();
    return 0;
}
