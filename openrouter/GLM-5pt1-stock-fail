#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <ctype.h>

#ifdef _WIN32
#include <windows.h>
#define SLEEP_MS(ms) Sleep(ms)
#else
#include <unistd.h>
#define SLEEP_MS(ms) usleep(ms * 1000)
#endif

#include <curl/curl.h>
#include "cJSON.h"

// --- Data structure for CURL HTTP GET ---
struct MemoryStruct {
    char *memory;
    size_t size;
};

// --- CURL Write Callback ---
static size_t WriteMemoryCallback(void *contents, size_t size, size_t nmemb, void *userp) {
    size_t realsize = size * nmemb;
    struct MemoryStruct *mem = (struct MemoryStruct *)userp;

    char *ptr = realloc(mem->memory, mem->size + realsize + 1);
    if (!ptr) {
        printf("Not enough memory (realloc returned NULL)\n");
        return 0;
    }

    mem->memory = ptr;
    memcpy(&(mem->memory[mem->size]), contents, realsize);
    mem->size += realsize;
    mem->memory[mem->size] = 0;

    return realsize;
}

// --- Fetch Stock Data from Yahoo Finance ---
char* fetch_stock_data(CURL *curl_handle, const char* ticker) {
    struct MemoryStruct chunk;
    chunk.memory = malloc(1);
    chunk.size = 0;

    char url[256];
    snprintf(url, sizeof(url), "https://query1.finance.yahoo.com/v8/finance/chart/%s?interval=1d&range=1d", ticker);

    curl_easy_setopt(curl_handle, CURLOPT_URL, url);
    // Spoof User-Agent to prevent Yahoo Finance from rejecting the request
    curl_easy_setopt(curl_handle, CURLOPT_USERAGENT, "Mozilla/5.0 (Windows NT 10.0; Win64; x64; rv:109.0) Gecko/20100101 Firefox/115.0");
    curl_easy_setopt(curl_handle, CURLOPT_WRITEFUNCTION, WriteMemoryCallback);
    curl_easy_setopt(curl_handle, CURLOPT_WRITEDATA, (void *)&chunk);
    curl_easy_setopt(curl_handle, CURLOPT_TIMEOUT, 10L);
    curl_easy_setopt(curl_handle, CURLOPT_FOLLOWLOCATION, 1L);

    CURLcode res = curl_easy_perform(curl_handle);

    long http_code = 0;
    curl_easy_getinfo(curl_handle, CURLINFO_RESPONSE_CODE, &http_code);

    if (res != CURLE_OK || http_code != 200) {
        fprintf(stderr, "\nError fetching %s: HTTP %ld - %s\n", ticker, http_code, curl_easy_strerror(res));
        free(chunk.memory);
        return NULL;
    }

    return chunk.memory;
}

// --- Parse JSON and Print Dashboard Row ---
void parse_and_display(const char* json_string, const char* ticker) {
    cJSON *root = cJSON_Parse(json_string);
    if (root == NULL) {
        printf("| %-6s | Parsing Error                                           |\n", ticker);
        return;
    }

    // Navigate the JSON: chart -> result[0] -> meta
    cJSON *chart = cJSON_GetObjectItemCaseSensitive(root, "chart");
    if (!chart) goto cleanup_error;

    cJSON *result = cJSON_GetObjectItemCaseSensitive(chart, "result");
    if (!cJSON_IsArray(result) || cJSON_GetArraySize(result) == 0) goto cleanup_error;

    cJSON *result_item = cJSON_GetArrayItem(result, 0);
    cJSON *meta = cJSON_GetObjectItemCaseSensitive(result_item, "meta");
    if (!meta) goto cleanup_error;

    cJSON *price_json = cJSON_GetObjectItemCaseSensitive(meta, "regularMarketPrice");
    cJSON *prev_json = cJSON_GetObjectItemCaseSensitive(meta, "chartPreviousClose");
    cJSON *currency_json = cJSON_GetObjectItemCaseSensitive(meta, "currency");

    if (!cJSON_IsNumber(price_json) || !cJSON_IsNumber(prev_json)) goto cleanup_error;

    double price = price_json->valuedouble;
    double prev_close = prev_json->valuedouble;
    const char* currency = (currency_json && currency_json->valuestring) ? currency_json->valuestring : "USD";

    double change = price - prev_close;
    double change_pct = (prev_close != 0) ? (change / prev_close) * 100.0 : 0.0;

    // ANSI Color Codes: Green for up, Red for down
    const char* color = change >= 0 ? "\033[32m" : "\033[31m";
    const char* reset = "\033[0m";
    const char* sign = change >= 0 ? "+" : "";

    printf("| %-6s | %s%-5s %-8.2f | %s%-8.2f (%s%+.2f%%)          %s|\n", 
           ticker, color, currency, price, sign, change, color, change_pct, reset);

    cJSON_Delete(root);
    return;

cleanup_error:
    printf("| %-6s | Data unavailable or API format changed               |\n", ticker);
    cJSON_Delete(root);
}

int main(int argc, char** argv) {
    if (argc < 2) {
        printf("Usage: %s TICKER1 TICKER2 TICKER3 ...\n", argv[0]);
        printf("Example: %s AAPL MSFT TSLA GOOGL\n", argv[0]);
        return 1;
    }

    int num_tickers = argc - 1;
    char** tickers = &argv[1];

    // Initialize CURL globally and per-handle
    curl_global_init(CURL_GLOBAL_ALL);
    CURL *curl_handle = curl_easy_init();

    if (!curl_handle) {
        fprintf(stderr, "Failed to initialize CURL\n");
        curl_global_cleanup();
        return 1;
    }

    while (1) {
        // Clear terminal screen and move cursor to top left
        printf("\033[2J\033[H");

        time_t now = time(NULL);
        struct tm *t = localtime(&now);
        char time_str[64];
        strftime(time_str, sizeof(time_str), "%Y-%m-%d %H:%M:%S", t);

        printf("=======================================================\n");
        printf("   LIVE STOCK DASHBOARD - %s\n", time_str);
        printf("=======================================================\n");
        printf("| Ticker | Price          | Change                    |\n");
        printf("|--------|----------------|---------------------------|\n");

        for (int i = 0; i < num_tickers; i++) {
            char* json_data = fetch_stock_data(curl_handle, tickers[i]);
            if (json_data) {
                parse_and_display(json_data, tickers[i]);
                free(json_data); // Free the memory allocated by CURL callback
            } else {
                printf("| %-6s | Network Error                                           |\n", tickers[i]);
            }
        }

        printf("=======================================================\n");
        printf(" Refreshing in 5 seconds. Press Ctrl+C to exit.\n");

        SLEEP_MS(5000); // Wait 5 seconds before next refresh
    }

    // Cleanup (This part is unreachable in this infinite loop, but good practice to include)
    curl_easy_cleanup(curl_handle);
    curl_global_cleanup();
    return 0;
}
