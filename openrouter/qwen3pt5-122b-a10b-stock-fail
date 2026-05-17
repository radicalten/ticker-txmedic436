#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <curl/curl.h>
#include "cJSON.h"

// --- Configuration ---
#define REFRESH_RATE_SEC 10
#define MAX_SYMBOLS 5
#define MAX_URL_LEN 512

// --- Data Structures ---
typedef struct {
    char symbol[10];
    double price;
    char currency[10];
} Stock;

typedef struct {
    char *data;
    size_t size;
} MemoryStruct;

// --- Global Dashboard State ---
Stock dashboard[MAX_SYMBOLS];
int dashboard_count = 0;

// --- Function Prototypes ---
size_t WriteMemoryCallback(void *contents, size_t size, size_t nmemb, void *userp);
char *fetch_stock_data(const char *symbols);
void parse_json_response(const char *json_str);
void clear_screen();
void print_dashboard();
void init_dashboard();

// --- Callback for CURL to store response ---
size_t WriteMemoryCallback(void *contents, size_t size, size_t nmemb, void *userp) {
    size_t realsize = size * nmemb;
    MemoryStruct *mem = (MemoryStruct *)userp;

    char *ptr = realloc(mem->data, mem->size + realsize + 1);
    if (ptr == NULL) {
        /* out of memory! */
        printf("not enough memory (realloc returned NULL)\n");
        return 0;
    }

    mem->data = ptr;
    memcpy(&(mem->data[mem->size]), contents, realsize);
    mem->size += realsize;
    mem->data[mem->size] = 0;

    return realsize;
}

// --- Fetch Data from Yahoo Finance ---
char *fetch_stock_data(const char *symbols) {
    CURL *curl;
    CURLcode res;
    MemoryStruct chunk;
    char *response = NULL;
    char url[MAX_URL_LEN];

    // Yahoo Finance Quote Endpoint
    // We add corsDomain to mimic browser behavior
    snprintf(url, sizeof(url), "https://query1.finance.yahoo.com/v7/finance/quote?symbols=%s&corsDomain=finance.yahoo.com", symbols);

    curl_global_init(CURL_GLOBAL_ALL);
    curl = curl_easy_init();

    if (curl) {
        chunk.data = malloc(1);
        chunk.size = 0;

        // --- CRITICAL: User-Agent Header ---
        // Yahoo blocks generic libcurl user agents. We mimic a Chrome browser.
        const char *user_agent = "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/120.0.0.0 Safari/537.36";
        
        curl_easy_setopt(curl, CURLOPT_URL, url);
        curl_easy_setopt(curl, CURLOPT_USERAGENT, user_agent);
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteMemoryCallback);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, (void *)&chunk);
        curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
        curl_easy_setopt(curl, CURLOPT_TIMEOUT, 10L); // 10 second timeout

        res = curl_easy_perform(curl);

        if (res != CURLE_OK) {
            fprintf(stderr, "curl_easy_perform() failed: %s\n", curl_easy_strerror(res));
            free(chunk.data);
        } else {
            response = chunk.data;
        }

        curl_easy_cleanup(curl);
    }
    curl_global_cleanup();

    return response;
}

// --- Parse JSON using cJSON ---
void parse_json_response(const char *json_str) {
    cJSON *root = NULL;
    cJSON *quoteResponse = NULL;
    cJSON *result = NULL;
    cJSON *item = NULL;

    if (!json_str) return;

    root = cJSON_Parse(json_str);
    if (!root) {
        fprintf(stderr, "Error parsing JSON\n");
        return;
    }

    quoteResponse = cJSON_GetObjectItemCaseSensitive(root, "quoteResponse");
    if (!quoteResponse) goto cleanup;

    result = cJSON_GetObjectItemCaseSensitive(quoteResponse, "result");
    if (!result || !cJSON_IsArray(result)) goto cleanup;

    // Iterate through the result array
    cJSON_ArrayForEach(item, result) {
        cJSON *symbol = cJSON_GetObjectItemCaseSensitive(item, "symbol");
        cJSON *price = cJSON_GetObjectItemCaseSensitive(item, "regularMarketPrice");
        cJSON *currency = cJSON_GetObjectItemCaseSensitive(item, "currency");

        if (cJSON_IsString(symbol) && cJSON_IsNumber(price)) {
            // Find matching stock in our dashboard and update it
            for (int i = 0; i < dashboard_count; i++) {
                if (strcmp(dashboard[i].symbol, symbol->valuestring) == 0) {
                    dashboard[i].price = price->valuedouble;
                    if (cJSON_IsString(currency)) {
                        strncpy(dashboard[i].currency, currency->valuestring, sizeof(dashboard[i].currency) - 1);
                    }
                    break;
                }
            }
        }
    }

cleanup:
    cJSON_Delete(root);
}

// --- UI Helpers ---
void clear_screen() {
    printf("\033[H\033[2J");
}

void print_dashboard() {
    clear_screen();
    
    printf("========================================\n");
    printf("       LIVE STOCK DASHBOARD             \n");
    printf("========================================\n");
    printf("Refresh Rate: %d Seconds\n", REFRESH_RATE_SEC);
    printf("Last Updated: %s", ctime(NULL));
    printf("----------------------------------------\n");
    printf("%-10s | %-10s | %-10s\n", "SYMBOL", "PRICE", "CURRENCY");
    printf("----------------------------------------\n");

    for (int i = 0; i < dashboard_count; i++) {
        if (dashboard[i].price > 0) {
            printf("%-10s | $%-9.2f | %-10s\n", 
                   dashboard[i].symbol, 
                   dashboard[i].price, 
                   dashboard[i].currency);
        } else {
            printf("%-10s | %-10s | %-10s\n", 
                   dashboard[i].symbol, 
                   "N/A", 
                   "N/A");
        }
    }
    printf("========================================\n");
    printf("Press Ctrl+C to exit...\n");
}

void init_dashboard() {
    // Define your stocks here
    const char *symbols[MAX_SYMBOLS] = {"AAPL", "MSFT", "GOOGL", "AMZN", "TSLA"};
    
    dashboard_count = MAX_SYMBOLS;
    for (int i = 0; i < MAX_SYMBOLS; i++) {
        strncpy(dashboard[i].symbol, symbols[i], sizeof(dashboard[i].symbol) - 1);
        dashboard[i].price = 0.0;
        strcpy(dashboard[i].currency, "USD");
    }
}

// --- Main Loop ---
int main(void) {
    char symbols_list[MAX_SYMBOLS * 10];
    char temp[MAX_SYMBOLS * 10] = {0};

    printf("Initializing Stock Dashboard...\n");
    init_dashboard();

    // Create comma-separated string for URL
    for (int i = 0; i < dashboard_count; i++) {
        if (i > 0) strcat(temp, ",");
        strcat(temp, dashboard[i].symbol);
    }
    strcpy(symbols_list, temp);

    while (1) {
        char *json_response = fetch_stock_data(symbols_list);
        
        if (json_response) {
            parse_json_response(json_response);
            free(json_response);
            print_dashboard();
        } else {
            clear_screen();
            printf("Failed to fetch data. Retrying in %d seconds...\n", REFRESH_RATE_SEC);
            sleep(REFRESH_RATE_SEC);
            continue;
        }

        sleep(REFRESH_RATE_SEC);
    }

    return 0;
}
