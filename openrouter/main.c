#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <curl/curl.h>
#include "cJSON.h"

/* ------------------------------------------------------------------ */
/* Memory buffer for libcurl write callback                           */
/* ------------------------------------------------------------------ */
struct MemoryStruct {
    char   *memory;
    size_t size;
};

static size_t write_callback(void *contents, size_t size, size_t nmemb, void *userp)
{
    size_t realsize = size * nmemb;
    struct MemoryStruct *mem = (struct MemoryStruct *)userp;

    char *ptr = realloc(mem->memory, mem->size + realsize + 1);
    if (!ptr)
        return 0;                       /* out of memory */

    mem->memory = ptr;
    memcpy(&(mem->memory[mem->size]), contents, realsize);
    mem->size += realsize;
    mem->memory[mem->size] = 0;         /* keep null-terminated */
    return realsize;
}

/* ------------------------------------------------------------------ */
/* Fetch JSON from Yahoo Finance                                      */
/* ------------------------------------------------------------------ */
static int fetch_url(const char *url, char **out_response)
{
    CURL *curl;
    CURLcode res;
    struct MemoryStruct chunk;

    chunk.memory = malloc(1);
    chunk.size = 0;

    curl = curl_easy_init();
    if (!curl) {
        free(chunk.memory);
        return -1;
    }

    /* Use a modern browser User-Agent so Yahoo doesn't 403 us */
    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_USERAGENT,
        "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 "
        "(KHTML, like Gecko) Chrome/120.0.0.0 Safari/537.36");
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_callback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, (void *)&chunk);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 15L);

    res = curl_easy_perform(curl);

    long http_code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
    curl_easy_cleanup(curl);

    if (res != CURLE_OK) {
        fprintf(stderr, "Network error: %s\n", curl_easy_strerror(res));
        free(chunk.memory);
        return -1;
    }

    if (http_code != 200) {
        fprintf(stderr, "HTTP error %ld\n", http_code);
        free(chunk.memory);
        return -1;
    }

    *out_response = chunk.memory;
    return 0;
}

/* ------------------------------------------------------------------ */
/* Parse JSON and print a simple dashboard                            */
/* ------------------------------------------------------------------ */
static void print_dashboard(const char *json_text)
{
    cJSON *root = cJSON_Parse(json_text);
    if (!root) {
        printf("Error: failed to parse JSON.\n");
        return;
    }

    cJSON *quoteResponse = cJSON_GetObjectItemCaseSensitive(root, "quoteResponse");
    if (!quoteResponse) {
        printf("Error: unexpected JSON format.\n");
        cJSON_Delete(root);
        return;
    }

    cJSON *results = cJSON_GetObjectItemCaseSensitive(quoteResponse, "result");
    if (!results || !cJSON_IsArray(results)) {
        printf("No stock data returned.\n");
        cJSON_Delete(root);
        return;
    }

    /* Header */
    printf("%-8s %-25s %12s %12s %12s\n",
           "SYMBOL", "NAME", "PRICE", "CHANGE", "CHANGE%");
    printf("-------------------------------------------------------------------------\n");

    cJSON *stock;
    cJSON_ArrayForEach(stock, results) {
        cJSON *jsym   = cJSON_GetObjectItemCaseSensitive(stock, "symbol");
        cJSON *jprice = cJSON_GetObjectItemCaseSensitive(stock, "regularMarketPrice");
        cJSON *jchng  = cJSON_GetObjectItemCaseSensitive(stock, "regularMarketChange");
        cJSON *jpct   = cJSON_GetObjectItemCaseSensitive(stock, "regularMarketChangePercent");

        /* Prefer shortName, fall back to longName */
        const char *name = "";
        cJSON *jname = cJSON_GetObjectItemCaseSensitive(stock, "shortName");
        if (!cJSON_IsString(jname))
            jname = cJSON_GetObjectItemCaseSensitive(stock, "longName");
        if (cJSON_IsString(jname))
            name = jname->valuestring;

        const char *sym    = cJSON_IsString(jsym)   ? jsym->valuestring   : "???";
        double price       = cJSON_IsNumber(jprice) ? jprice->valuedouble : 0.0;
        double change      = cJSON_IsNumber(jchng)  ? jchng->valuedouble  : 0.0;
        double pct         = cJSON_IsNumber(jpct)   ? jpct->valuedouble   : 0.0;

        /* ANSI colours: green = up, red = down */
        const char *col = "", *rst = "\033[0m";
        if (change > 0.0) col = "\033[32m";
        else if (change < 0.0) col = "\033[31m";

        char display_name[26];
        strncpy(display_name, name, 25);
        display_name[25] = '\0';

        printf("%-8s %-25s %12.2f %s%11.2f%s %s%10.2f%%%s\n",
               sym,
               display_name,
               price,
               col, change, rst,
               col, pct, rst);
    }

    cJSON_Delete(root);
}

/* ------------------------------------------------------------------ */
static void clear_screen(void)
{
    printf("\033[H\033[J");   /* ANSI clear + home cursor */
}

/* ------------------------------------------------------------------ */
int main(int argc, char *argv[])
{
    /* Build comma-separated symbol list */
    char symbols[512] = {0};
    if (argc > 1) {
        size_t off = 0;
        for (int i = 1; i < argc; i++) {
            int n = snprintf(symbols + off, sizeof(symbols) - off,
                             "%s%s", (i > 1) ? "," : "", argv[i]);
            if (n > 0) off += n;
        }
    } else {
        /* Default watchlist */
        snprintf(symbols, sizeof(symbols),
                  "AAPL,MSFT,GOOGL,AMZN,TSLA,NVDA,AMD,INTC");
    }

    char url[1024];
    snprintf(url, sizeof(url),
             "https://query1.finance.yahoo.com/v7/finance/quote?"
             "formatted=false&symbols=%s", symbols);

    printf("Starting dashboard for: %s\n", symbols);
    printf("Press Ctrl+C to exit.\n");
    sleep(2);

    while (1) {
        char *response = NULL;

        clear_screen();

        /* Timestamp header */
        time_t now = time(NULL);
        struct tm *tm_info = localtime(&now);
        char tbuf[32];
        strftime(tbuf, sizeof(tbuf), "%Y-%m-%d %H:%M:%S", tm_info);

        printf("╔══════════════════════════════════════════════════════════════╗\n");
        printf("║      LIVE STOCK DASHBOARD               %-20s ║\n", tbuf);
        printf("╚══════════════════════════════════════════════════════════════╝\n\n");

        if (fetch_url(url, &response) == 0) {
            print_dashboard(response);
            free(response);
        } else {
            printf("Unable to fetch data. Retrying...\n");
        }

        printf("\n[Refresh every 5s | Ctrl+C to quit]\n");
        sleep(5);
    }

    return 0;
}
