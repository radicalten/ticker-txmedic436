#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>   // for sleep()
#include <curl/curl.h>
#include "cJSON.h"    // DaveGamble's cJSON

#define USER_AGENT "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/91.0.4472.124 Safari/537.36"
#define REFRESH_INTERVAL 10 // seconds

// Structure for storing HTTP response
struct MemoryStruct {
    char *memory;
    size_t size;
};

// Curl write callback – appends data to memory
static size_t WriteMemoryCallback(void *contents, size_t size, size_t nmemb, void *userp) {
    size_t realsize = size * nmemb;
    struct MemoryStruct *mem = (struct MemoryStruct *)userp;

    char *ptr = realloc(mem->memory, mem->size + realsize + 1);
    if(!ptr) {
        fprintf(stderr, "Not enough memory\n");
        return 0;
    }

    mem->memory = ptr;
    memcpy(&(mem->memory[mem->size]), contents, realsize);
    mem->size += realsize;
    mem->memory[mem->size] = 0;

    return realsize;
}

// Extract current price from Yahoo Finance JSON response
// Returns -1.0 on error
double extract_price(const char *json) {
    cJSON *root = cJSON_Parse(json);
    if (root == NULL) {
        return -1.0;
    }

    // Navigate: chart -> result[0] -> meta -> regularMarketPrice
    cJSON *chart = cJSON_GetObjectItem(root, "chart");
    if (chart == NULL) {
        cJSON_Delete(root);
        return -1.0;
    }

    cJSON *result = cJSON_GetObjectItem(chart, "result");
    if (result == NULL || !cJSON_IsArray(result) || cJSON_GetArraySize(result) == 0) {
        cJSON_Delete(root);
        return -1.0;
    }

    cJSON *first = cJSON_GetArrayItem(result, 0);
    cJSON *meta = cJSON_GetObjectItem(first, "meta");
    if (meta == NULL) {
        cJSON_Delete(root);
        return -1.0;
    }

    cJSON *price = cJSON_GetObjectItem(meta, "regularMarketPrice");
    double value = -1.0;
    if (cJSON_IsNumber(price)) {
        value = price->valuedouble;
    }

    cJSON_Delete(root);
    return value;
}

// Fetch price for a given stock symbol
double fetch_price(const char *symbol) {
    CURL *curl;
    CURLcode res;
    struct MemoryStruct chunk;
    chunk.memory = malloc(1);
    chunk.size = 0;

    // Build URL
    char url[512];
    snprintf(url, sizeof(url),
             "https://query1.finance.yahoo.com/v8/finance/chart/%s?interval=1d&range=1d",
             symbol);

    curl_global_init(CURL_GLOBAL_DEFAULT);
    curl = curl_easy_init();

    double price = -1.0;

    if(curl) {
        curl_easy_setopt(curl, CURLOPT_URL, url);
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteMemoryCallback);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, (void *)&chunk);
        curl_easy_setopt(curl, CURLOPT_USERAGENT, USER_AGENT);
        curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
        curl_easy_setopt(curl, CURLOPT_TIMEOUT, 10L);

        res = curl_easy_perform(curl);
        if(res != CURLE_OK) {
            fprintf(stderr, "curl_easy_perform() failed: %s\n", curl_easy_strerror(res));
        } else {
            price = extract_price(chunk.memory);
        }

        curl_easy_cleanup(curl);
    }

    free(chunk.memory);
    curl_global_cleanup();
    return price;
}

// Print dashboard header
void print_header(int num_symbols) {
    printf("\033[1;36m"); // cyan
    printf("+");
    for (int i = 0; i < 44; i++) printf("-");
    printf("+\n");
    printf("|  %-20s |  %-14s |\n", "Symbol", "Price (USD)");
    printf("+");
    for (int i = 0; i < 44; i++) printf("-");
    printf("+\n");
    printf("\033[0m");
}

// Print a table row for one symbol and price
void print_row(const char *symbol, double price) {
    if (price < 0) {
        printf("|  %-20s |  %-14s |\n", symbol, "ERROR");
    } else {
        printf("|  %-20s |  %14.2f |\n", symbol, price);
    }
}

int main(int argc, char *argv[]) {
    if (argc < 2) {
        fprintf(stderr, "Usage: %s SYMBOL [SYMBOL ...]\n", argv[0]);
        fprintf(stderr, "Example: %s AAPL MSFT GOOG\n", argv[0]);
        return 1;
    }

    int num_symbols = argc - 1;
    char **symbols = &argv[1];

    printf("Live Stock Dashboard (refreshes every %d seconds)\n", REFRESH_INTERVAL);
    printf("Press Ctrl+C to quit.\n\n");

    while (1) {
        // Clear screen (ANSI escape)
        printf("\033[2J\033[1;1H");

        print_header(num_symbols);

        for (int i = 0; i < num_symbols; i++) {
            double price = fetch_price(symbols[i]);
            print_row(symbols[i], price);
        }

        // Footer
        printf("\033[1;37m+");
        for (int i = 0; i < 44; i++) printf("-");
        printf("+\033[0m\n");

        fflush(stdout);
        sleep(REFRESH_INTERVAL);
    }

    return 0;
}
