/* -------------------------------------------------------------
   LIVE STOCK DASHBOARD – corrected version
   ------------------------------------------------------------- */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <signal.h>
#include <stdbool.h>
#include <curl/curl.h>
#include "cJSON.h"

/* -------------------- Configuration -------------------------- */
#define REFRESH_INTERVAL 5          /* seconds between updates */
#define MAX_STOCKS       16
/* ------------------------------------------------------------- */

/* --------------------- Data structures ----------------------- */
typedef struct {
    char   symbol[16];
    double price;
    double change;
    double change_percent;
    int    has_data;
} StockData;

/* Helper struct used by libcurl to grow the response buffer */
typedef struct {
    char   *data;   /* malloc‑ed buffer */
    size_t  size;   /* number of valid bytes in *data* */
} Memory;
/* ------------------------------------------------------------- */

/* -------------------- Global flag --------------------------- */
static volatile sig_atomic_t running = 1;
/* ------------------------------------------------------------- */

/* -------------------- Signal handling ----------------------- */
static void signal_handler(int sig) {
    (void)sig;          /* unused */
    running = 0;
}
/* ------------------------------------------------------------- */

/* -------------------- libcurl callback ---------------------- */
static size_t write_callback(void *contents, size_t size, size_t nmemb,
                             void *userp)
{
    size_t   real_size = size * nmemb;
    Memory  *mem       = (Memory *)userp;

    char *ptr = realloc(mem->data, mem->size + real_size + 1);
    if (!ptr) {
        /* out of memory – abort the transfer */
        fprintf(stderr, "Error: not enough memory for HTTP response\n");
        return 0;
    }

    mem->data = ptr;
    memcpy(&(mem->data[mem->size]), contents, real_size);
    mem->size += real_size;
    mem->data[mem->size] = '\0';          /* null‑terminate */

    return real_size;
}
/* ------------------------------------------------------------- */

/* -------------------- HTTP fetch ---------------------------- */
static int fetch_stock_data(const char *symbols, char **response)
{
    CURLcode   rc;
    CURL      *curl = NULL;
    Memory     chunk = { NULL, 0 };
    long       http_code = 0;
    char       url[512];

    snprintf(url, sizeof(url),
             "https://query1.finance.yahoo.com/v7/finance/quote?symbols=%s"
             "&lang=en-US&region=US&corsDomain=yahoo.com",
             symbols);

    curl = curl_easy_init();
    if (!curl) {
        fprintf(stderr, "Error: curl_easy_init() failed\n");
        return -1;
    }

    /* Initialise an empty buffer */
    chunk.data = malloc(1);
    if (!chunk.data) {
        curl_easy_cleanup(curl);
        return -1;
    }
    chunk.data[0] = '\0';
    chunk.size    = 0;

    curl_easy_setopt(curl, CURLOPT_URL,            url);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION,  write_callback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA,      &chunk);
    curl_easy_setopt(curl, CURLOPT_USERAGENT,
                     "Mozilla/5.0 (X11; Linux x86_64) "
                     "AppleWebKit/537.36 (KHTML, like Gecko) "
                     "Chrome/120.0.0.0 Safari/537.36");
    curl_easy_setopt(curl, CURLOPT_TIMEOUT,        30L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT,10L);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
    curl_easy_setopt(curl, CURLOPT_ACCEPT_ENCODING, "");   /* enable gzip */

    rc = curl_easy_perform(curl);
    if (rc != CURLE_OK) {
        fprintf(stderr, "Error: curl_easy_perform() failed: %s\n",
                curl_easy_strerror(rc));
        free(chunk.data);
        curl_easy_cleanup(curl);
        return -1;
    }

    /* Verify HTTP status 200 */
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
    if (http_code != 200) {
        fprintf(stderr, "Error: HTTP status %ld received\n", http_code);
        free(chunk.data);
        curl_easy_cleanup(curl);
        return -1;
    }

    *response = chunk.data;          /* caller must free() */
    curl_easy_cleanup(curl);
    return 0;
}
/* ------------------------------------------------------------- */

/* -------------------- JSON parsing -------------------------- */
static int parse_stock_data(const char *json_response,
                            StockData *stocks,
                            int max_stocks)
{
    cJSON *json = cJSON_Parse(json_response);
    if (!json) return -1;

    cJSON *quoteResponse = cJSON_GetObjectItemCaseSensitive(json,
                                                            "quoteResponse");
    if (!quoteResponse) {
        cJSON_Delete(json);
        return -1;
    }

    cJSON *result = cJSON_GetObjectItemCaseSensitive(quoteResponse,
                                                     "result");
    if (!result || !cJSON_IsArray(result)) {
        cJSON_Delete(json);
        return -1;
    }

    int count = 0;
    cJSON *item = NULL;
    cJSON_ArrayForEach(item, result) {
        if (count >= max_stocks) break;

        cJSON *symbol = cJSON_GetObjectItemCaseSensitive(item,
                                                         "symbol");
        cJSON *price  = cJSON_GetObjectItemCaseSensitive(item,
                                                         "regularMarketPrice");
        cJSON *change = cJSON_GetObjectItemCaseSensitive(item,
                                                         "regularMarketChange");
        cJSON *changePercent = cJSON_GetObjectItemCaseSensitive(item,
                                                         "regularMarketChangePercent");

        if (symbol && price) {
            strncpy(stocks[count].symbol, symbol->valuestring,
                    sizeof(stocks[count].symbol) - 1);
            stocks[count].symbol[sizeof(stocks[count].symbol) - 1] = '\0';
            stocks[count].price          = cJSON_GetNumberValue(price);
            stocks[count].change         = change ? cJSON_GetNumberValue(change) : 0.0;
            stocks[count].change_percent = changePercent ?
                                            cJSON_GetNumberValue(changePercent) : 0.0;
            stocks[count].has_data      = 1;
            ++count;
        }
    }

    cJSON_Delete(json);
    return count;
}
/* ------------------------------------------------------------- */

/* -------------------- UI helpers ---------------------------- */
static void clear_screen(void)
{
    printf("\033[2J\033[H");
    fflush(stdout);
}

static void display_dashboard(StockData *stocks, int count,
                              time_t last_update)
{
    clear_screen();

    printf("\n");
    printf("╔══════════════════════════════════════════════════════════════════════╗\n");
    printf("║                     📈 LIVE STOCK DASHBOARD 📈                      ║\n");
    printf("╠══════════════════════════════════════════════════════════════════════╣\n");
    printf("║ %-8s │ %12s │ %10s │ %12s ║\n", "SYMBOL", "PRICE", "CHANGE", "CHANGE %");
    printf("╟─────────┼──────────────┼────────────┼───────────────╢\n");

    for (int i = 0; i < count; ++i) {
        if (!stocks[i].has_data) continue;

        const char *color = (stocks[i].change >= 0.0) ? "\033[32m" : "\033[31m";
        const char *reset = "\033[0m";

        printf("║ %-8s │ %12.2f │ %10.2f │ %11.2f%% %s║\n",
               stocks[i].symbol,
               stocks[i].price,
               stocks[i].change,
               stocks[i].change_percent,
               reset);
    }

    printf("╚══════════════════════════════════════════════════════════════════════╝\n");

    struct tm *tm_info = localtime(&last_update);
    char time_str[64];
    strftime(time_str, sizeof(time_str), "%Y-%m-%d %H:%M:%S", tm_info);

    printf("\n🔄 Updating every %d seconds | Last update: %s | Press Ctrl+C to exit\n",
           REFRESH_INTERVAL, time_str);
}

static void display_error(const char *msg)
{
    printf("\033[2K\r\033[31mError: %s\033[0m\n", msg);
    fflush(stdout);
}
/* ------------------------------------------------------------- */

/* -------------------- Main ----------------------------------- */
int main(int argc, char *argv[])
{
    /* Default symbols – can be overridden from the command line */
    const char *symbols = "AAPL,GOOGL,MSFT,AMZN,TSLA,META,NVDA,JPM";
    if (argc > 1) {
        symbols = argv[1];
    }

    /* Install signal handlers for graceful shutdown */
    signal(SIGINT,  signal_handler);
    signal(SIGTERM, signal_handler);

    /* Initialise libcurl globally */
    CURLcode rc = curl_global_init(CURL_GLOBAL_DEFAULT);
    if (rc != CURLE_OK) {
        fprintf(stderr, "Failed to initialise libcurl: %s\n",
                curl_easy_strerror(rc));
        return EXIT_FAILURE;
    }

    printf("Initializing stock dashboard...\n");
    printf("Tracking: %s\n\n", symbols);

    StockData stocks[MAX_STOCKS];
    memset(stocks, 0, sizeof(stocks));

    char *response = NULL;
    time_t last_update = 0;

    while (running) {
        if (fetch_stock_data(symbols, &response) == 0) {
            int count = parse_stock_data(response, stocks, MAX_STOCKS);
            if (count > 0) {
                last_update = time(NULL);
                display_dashboard(stocks, count, last_update);
            } else {
                display_error("Failed to parse stock data");
            }
            free(response);
            response = NULL;
        } else {
            display_error("Failed to fetch data – retrying");
        }

        /* Sleep only if we are still supposed to run */
        if (running) {
            sleep(REFRESH_INTERVAL);
        }
    }

    /* Clean‑up */
    if (response) free(response);
    curl_global_cleanup();

    printf("\n\nDashboard stopped.\n");
    return EXIT_SUCCESS;
}
