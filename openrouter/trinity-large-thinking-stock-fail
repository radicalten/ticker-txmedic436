#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <time.h>
#include <curl/curl.h>
#include "cJSON.h"

#define MAX_SYMBOLS 20
#define SYMBOL_LEN 16
#define BUFFER_SIZE 256
#define UPDATE_INTERVAL 5  // seconds

typedef struct {
    char symbol[SYMBOL_LEN];
    double price;
    double change;
    double change_pct;
    double prev_close;
    double day_low;
    double day_high;
    double open;
    long volume;
    char currency[8];
    char exchange[32];
    time_t last_update;
} StockData;

struct MemoryStruct {
    char *memory;
    size_t size;
};

static volatile int keep_running = 1;

void handle_signal(int sig) {
    keep_running = 0;
}

static size_t WriteCallback(void *contents, size_t size, size_t nmemb, void *userp) {
    size_t realsize = size * nmemb;
    struct MemoryStruct *mem = (struct MemoryStruct *)userp;
    char *ptr = realloc(mem->memory, mem->size + realsize + 1);
    if (ptr == NULL) return 0;
    mem->memory = ptr;
    memcpy(&(mem->memory[mem->size]), contents, realsize);
    mem->size += realsize;
    mem->memory[mem->size] = '\0';
    return realsize;
}

StockData fetch_stock_data(const char *symbol) {
    StockData data = {0};
    strncpy(data.symbol, symbol, sizeof(data.symbol) - 1);

    char url[BUFFER_SIZE];
    snprintf(url, sizeof(url),
             "https://query1.finance.yahoo.com/v10/finance/quoteSummary/%s?modules=price,summaryDetail",
             symbol);

    CURL *curl = curl_easy_init();
    if (!curl) return data;

    struct MemoryStruct chunk = {0};
    curl_easy_setopt(curl, CURLOPT_URL, url);

    struct curl_slist *headers = NULL;
    headers = curl_slist_append(headers,
        "User-Agent: Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/90.0.4430.212 Safari/537.36");
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteCallback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, (void *)&chunk);
    curl_easy_setopt(curl, CURLOPT_FAILONERROR, 1L);

    CURLcode res = curl_easy_perform(curl);
    if (res != CURLE_OK) {
        fprintf(stderr, "curl_easy_perform() failed for %s: %s\n", symbol,
                curl_easy_strerror(res));
        curl_easy_cleanup(curl);
        curl_slist_free_all(headers);
        return data;
    }

    long response_code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &response_code);
    if (response_code != 200) {
        fprintf(stderr, "HTTP %ld for symbol %s\n", response_code, symbol);
        curl_easy_cleanup(curl);
        curl_slist_free_all(headers);
        free(chunk.memory);
        return data;
    }

    curl_easy_cleanup(curl);
    curl_slist_free_all(headers);

    cJSON *root = cJSON_Parse(chunk.memory);
    if (!root) {
        fprintf(stderr, "cJSON_Parse error for %s: %s\n", symbol, cJSON_GetErrorPtr());
        free(chunk.memory);
        return data;
    }

    cJSON *quoteSummary = cJSON_GetObjectItemCaseSensitive(root, "quoteSummary");
    if (cJSON_IsObject(quoteSummary)) {
        cJSON *error = cJSON_GetObjectItemCaseSensitive(quoteSummary, "error");
        if (error && !cJSON_IsNull(error)) {
            fprintf(stderr, "Yahoo Finance error for %s: %s\n", symbol,
                    error->valuestring ? error->valuestring : "unknown error");
            cJSON_Delete(root);
            free(chunk.memory);
            return data;
        }

        cJSON *result = cJSON_GetObjectItemCaseSensitive(quoteSummary, "result");
        if (cJSON_IsArray(result) && cJSON_GetArraySize(result) > 0) {
            cJSON *first = cJSON_GetArrayItem(result, 0);
            if (cJSON_IsObject(first)) {
                // price module
                cJSON *price = cJSON_GetObjectItemCaseSensitive(first, "price");
                if (cJSON_IsObject(price)) {
                    cJSON *regularMarketPrice = cJSON_GetObjectItemCaseSensitive(price, "regularMarketPrice");
                    if (cJSON_IsNumber(regularMarketPrice))
                        data.price = regularMarketPrice->valuedouble;

                    cJSON *currency = cJSON_GetObjectItemCaseSensitive(price, "currency");
                    if (cJSON_IsString(currency) && currency->valuestring)
                        strncpy(data.currency, currency->valuestring, sizeof(data.currency) - 1);

                    cJSON *exchangeName = cJSON_GetObjectItemCaseSensitive(price, "exchangeName");
                    if (cJSON_IsString(exchangeName) && exchangeName->valuestring)
                        strncpy(data.exchange, exchangeName->valuestring, sizeof(data.exchange) - 1);
                }

                // summaryDetail module
                cJSON *summaryDetail = cJSON_GetObjectItemCaseSensitive(first, "summaryDetail");
                if (cJSON_IsObject(summaryDetail)) {
                    cJSON *previousClose = cJSON_GetObjectItemCaseSensitive(summaryDetail, "previousClose");
                    if (cJSON_IsNumber(previousClose))
                        data.prev_close = previousClose->valuedouble;

                    cJSON *dayLow = cJSON_GetObjectItemCaseSensitive(summaryDetail, "dayLow");
                    if (cJSON_IsNumber(dayLow))
                        data.day_low = dayLow->valuedouble;

                    cJSON *dayHigh = cJSON_GetObjectItemCaseSensitive(summaryDetail, "dayHigh");
                    if (cJSON_IsNumber(dayHigh))
                        data.day_high = dayHigh->valuedouble;

                    cJSON *open = cJSON_GetObjectItemCaseSensitive(summaryDetail, "open");
                    if (cJSON_IsNumber(open))
                        data.open = open->valuedouble;

                    cJSON *volume = cJSON_GetObjectItemCaseSensitive(summaryDetail, "volume");
                    if (cJSON_IsNumber(volume))
                        data.volume = (long)volume->valuedouble;
                }
            }
        }
    }

    data.change = data.price - data.prev_close;
    if (data.prev_close != 0)
        data.change_pct = (data.change / data.prev_close) * 100.0;
    else
        data.change_pct = 0.0;

    data.last_update = time(NULL);

    cJSON_Delete(root);
    free(chunk.memory);
    return data;
}

void clear_screen(void) {
    printf("\e[H\e[2J");
}

void display_dashboard(StockData *stocks, int count) {
    clear_screen();
    printf("Live Stock Dashboard\n");
    printf("====================\n\n");

    printf("%-10s | %-10s | %-8s | %-8s | %-12s | %-12s | %-12s\n",
           "Symbol", "Price", "Change", "%Chg", "PrevClose", "Open", "Volume");
    printf("----------------------------------------------------------------------------------------------\n");

    for (int i = 0; i < count; i++) {
        StockData *s = &stocks[i];
        if (s->price > 0) {
            char sign = (s->change >= 0) ? '+' : '-';
            printf("%-10s | $%-9.2f | %c%-7.2f | %c%-6.2.2f%% | $%-11.2f | $%-11.2f | %-12ld\n",
                   s->symbol, s->price, sign, fabs(s->change), sign, fabs(s->change_pct),
                   s->prev_close, s->open, s->volume);
        } else {
            printf("%-10s | %-10s | %-8s | %-8s | %-12s | %-12s | %-12s\n",
                   s->symbol, "N/A", "N/A", "N/A", "N/A", "N/A", "N/A");
        }
    }
    printf("\nLast updated: %s", ctime(&stocks[0].last_update));
}

int main(void) {
    // Register signal handler
    signal(SIGINT, handle_signal);

    // Initialize libcurl
    curl_global_init(CURL_GLOBAL_DEFAULT);

    // Read symbols from user
    char input[BUFFER_SIZE];
    char *symbols[MAX_SYMBOLS];
    int count = 0;

    printf("Enter stock symbols (comma-separated, up to %d): ", MAX_SYMBOLS);
    if (!fgets(input, sizeof(input), stdin)) {
        fprintf(stderr, "Input error\n");
        return EXIT_FAILURE;
    }

    char *token = strtok(input, ",\n");
    while (token && count < MAX_SYMBOLS) {
        // Trim whitespace
        char *start = token;
        while (*start && isspace((unsigned char)*start)) start++;
        char *end = start + strlen(start) - 1;
        while (end > start && isspace((unsigned char)*end)) end--;
        *(end + 1) = '\0';

        if (strlen(start) > 0) {
            symbols[count] = malloc(SYMBOL_LEN);
            strncpy(symbols[count], start, SYMBOL_LEN - 1);
            symbols[count][SYMBOL_LEN - 1] = '\0';
            count++;
        }
        token = strtok(NULL, ",\n");
    }

    if (count == 0) {
        fprintf(stderr, "No symbols provided.\n");
        return EXIT_FAILURE;
    }

    // Allocate array for stock data
    StockData *stocks = calloc(count, sizeof(StockData));
    if (!stocks) {
        fprintf(stderr, "Memory allocation failed.\n");
        return EXIT_FAILURE;
    }

    // Main loop
    while (keep_running) {
        for (int i = 0; i < count; i++) {
            StockData fetched = fetch_stock_data(symbols[i]);
            if (fetched.price > 0) {
                stocks[i] = fetched;
            }
        }
        display_dashboard(stocks, count);
        sleep(UPDATE_INTERVAL);
    }

    // Cleanup
    free(stocks);
    for (int i = 0; i < count; i++) {
        free(symbols[i]);
    }
    curl_global_cleanup();
    printf("\nGoodbye!\n");
    return EXIT_SUCCESS;
}
