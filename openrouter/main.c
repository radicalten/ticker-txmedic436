/* main.c
 * --------------------------------------------------------------
 * Simple live stock‑price dashboard using:
 *   - libcurl   (HTTP client)
 *   - cJSON     (JSON parser, Dave Gamble)
 *   - Yahoo Finance query endpoint
 *
 * Compile (example):
 *   gcc -Wall -Wextra -O2 main.c -lcurl -lcjson -o stock_dash
 *
 * Run:
 *   ./stock_dash AAPL MSFT GOOGL
 * -------------------------------------------------------------- */

#define _POSIX_C_SOURCE 200809L   /* for strdup, getline */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <curl/curl.h>
#include "cJSON.h"

/* --------------------------------------------------------------
 * Helper: write callback for libcurl – stores data in a growing
 * buffer that we later hand to cJSON.
 * -------------------------------------------------------------- */
typedef struct {
    char *memory;
    size_t size;
} MemBuffer;

static size_t write_cb(void *contents, size_t size, size_t nmemb, void *userp)
{
    size_t realsize = size * nmemb;
    MemBuffer *mem = (MemBuffer *)userp;

    char *ptr = realloc(mem->memory, mem->size + realsize + 1);
    if (!ptr) {
        fprintf(stderr, "ERROR: not enough memory (realloc returned NULL)\n");
        return 0;  /* signal libcurl to abort */
    }

    mem->memory = ptr;
    memcpy(&(mem->memory[mem->size]), contents, realsize);
    mem->size += realsize;
    mem->memory[mem->size] = '\0';

    return realsize;
}

/* --------------------------------------------------------------
 * Fetch the JSON payload for a given ticker symbol from Yahoo
 * Finance and return a freshly allocated string (caller must free).
 * Returns NULL on failure.
 * -------------------------------------------------------------- */
static char *fetch_yahoo_json(const char *symbol)
{
    CURL *curl;
    CURLcode res;
    MemBuffer chunk = { .memory = malloc(1), .size = 0 };  /* will grow */
    if (!chunk.memory) return NULL;
    chunk.memory[0] = '\0';

    curl_global_init(CURL_GLOBAL_DEFAULT);
    curl = curl_easy_init();
    if (!curl) {
        free(chunk.memory);
        curl_global_cleanup();
        return NULL;
    }

    /* Build the URL – the quote endpoint is lightweight and returns
       the regularMarketPrice directly. */
    char url[256];
    snprintf(url, sizeof(url),
             "https://query1.finance.yahoo.com/v7/finance/quote?symbols=%s",
             symbol);

    /* Set a custom User‑Agent so Yahoo does not block the request */
    const char *user_agent = "stock-dash/1.0 (+https://github.com/yourname)";

    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, (void *)&chunk);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, user_agent);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 10L);   /* 10‑second timeout */

    res = curl_easy_perform(curl);
    if (res != CURLE_OK) {
        fprintf(stderr, "curl error for %s: %s\n", symbol,
                curl_easy_strerror(res));
        free(chunk.memory);
        chunk.memory = NULL;
    }

    curl_easy_cleanup(curl);
    curl_global_cleanup();
    return chunk.memory;   /* caller frees */
}

/* --------------------------------------------------------------
 * Extract the regularMarketPrice from the Yahoo Finance JSON.
 * Returns 0.0 on failure (price could legitimately be 0, but for
 * this demo we treat it as an error indicator).
 * -------------------------------------------------------------- */
static double parse_price(const char *json)
{
    cJSON *root = cJSON_Parse(json);
    if (!root) {
        fprintf(stderr, "ERROR: invalid JSON\n");
        return 0.0;
    }

    /* Expected path: quoteResponse -> result[0] -> regularMarketPrice */
    cJSON *quoteResp = cJSON_GetObjectItemCaseSensitive(root, "quoteResponse");
    if (!quoteResp || !cJSON_IsObject(quoteResp)) {
        cJSON_Delete(root);
        return 0.0;
    }

    cJSON *result = cJSON_GetObjectItemCaseSensitive(quoteResp, "result");
    if (!result || !cJSON_IsArray(result) || cJSON_GetArraySize(result) < 1) {
        cJSON_Delete(root);
        return 0.0;
    }

    cJSON *firstItem = cJSON_GetArrayItem(result, 0);
    if (!firstItem || !cJSON_IsObject(firstItem)) {
        cJSON_Delete(root);
        return 0.0;
    }

    cJSON *priceNode = cJSON_GetObjectItemCaseSensitive(firstItem,
                                                        "regularMarketPrice");
    double price = 0.0;
    if (priceNode && cJSON_IsNumber(priceNode)) {
        price = priceNode->valuedouble;
    } else {
        fprintf(stderr, "WARNING: regularMarketPrice missing or not a number\n");
    }

    cJSON_Delete(root);
    return price;
}

/* --------------------------------------------------------------
 * Clear the terminal screen (ANSI escape works on most terminals).
 * -------------------------------------------------------------- */
static void clear_screen(void)
{
    printf("\033[2J\033[H");   /* ESC [ 2 J  – clear, ESC [ H – cursor home */
    fflush(stdout);
}

/* --------------------------------------------------------------
 * Main – accepts ticker symbols as command‑line arguments.
 * -------------------------------------------------------------- */
int main(int argc, char *argv[])
{
    if (argc < 2) {
        fprintf(stderr, "Usage: %s <SYMBOL> [<SYMBOL> ...]\n", argv[0]);
        return EXIT_FAILURE;
    }

    /* Pre‑allocate an array for the symbols we will track */
    const char **symbols = malloc((argc - 1) * sizeof(char *));
    if (!symbols) {
        perror("malloc");
        return EXIT_FAILURE;
    }
    for (int i = 1; i < argc; ++i) {
        symbols[i - 1] = argv[i];
    }
    size_t nsym = argc - 1;

    /* Main loop – refresh every 5 seconds */
    while (1) {
        clear_screen();
        printf("=== Live Stock Dashboard (refresh every 5s) ===\n\n");

        for (size_t i = 0; i < nsym; ++i) {
            char *json = fetch_yahoo_json(symbols[i]);
            if (!json) {
                printf("%-6s : ERROR (network)\n", symbols[i]);
                continue;
            }

            double price = parse_price(json);
            free(json);

            if (price > 0.0) {
                printf("%-6s : $%8.2f\n", symbols[i], price);
            } else {
                printf("%-6s : UNAVAILABLE\n", symbols[i]);
            }
        }

        printf("\nPress Ctrl+C to exit.\n");
        fflush(stdout);
        sleep(5);   /* wait 5 seconds before next update */
    }

    free(symbols);
    return EXIT_SUCCESS;
}
