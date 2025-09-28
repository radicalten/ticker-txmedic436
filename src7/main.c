#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h> // For sleep()
#include <curl/curl.h>
#include "cJSON.h"

// ANSI Color Codes for terminal output
#define KNRM  "\x1B[0m"
#define KRED  "\x1B[31m"
#define KGRN  "\x1B[32m"
#define KYEL  "\x1B[33m"

// A struct to hold the data received from libcurl
struct MemoryStruct {
    char *memory;
    size_t size;
};

// This is the callback function that libcurl calls when it receives data.
// It appends the new data to our MemoryStruct.
static size_t WriteMemoryCallback(void *contents, size_t size, size_t nmemb, void *userp) {
    size_t realsize = size * nmemb;
    struct MemoryStruct *mem = (struct MemoryStruct *)userp;

    char *ptr = realloc(mem->memory, mem->size + realsize + 1);
    if (ptr == NULL) {
        // out of memory!
        printf("not enough memory (realloc returned NULL)\n");
        return 0;
    }

    mem->memory = ptr;
    memcpy(&(mem->memory[mem->size]), contents, realsize);
    mem->size += realsize;
    mem->memory[mem->size] = 0;

    return realsize;
}

// Function to fetch stock data for a given symbol
char* fetch_stock_data(const char* symbol) {
    CURL *curl_handle;
    CURLcode res;
    struct MemoryStruct chunk;

    chunk.memory = malloc(1); // will be grown as needed by the callback
    chunk.size = 0;           // no data at this point

    curl_global_init(CURL_GLOBAL_ALL);
    curl_handle = curl_easy_init();

    // Construct the URL
    char url[256];
    snprintf(url, sizeof(url), "https://query1.finance.yahoo.com/v8/finance/chart/%s", symbol);

    // Set curl options
    curl_easy_setopt(curl_handle, CURLOPT_URL, url);
    curl_easy_setopt(curl_handle, CURLOPT_WRITEFUNCTION, WriteMemoryCallback);
    curl_easy_setopt(curl_handle, CURLOPT_WRITEDATA, (void *)&chunk);
    // Some APIs require a user-agent header
    curl_easy_setopt(curl_handle, CURLOPT_USERAGENT, "libcurl-agent/1.0");

    // Perform the request
    res = curl_easy_perform(curl_handle);

    // Check for errors
    if (res != CURLE_OK) {
        fprintf(stderr, "curl_easy_perform() failed: %s\n", curl_easy_strerror(res));
        free(chunk.memory);
        curl_easy_cleanup(curl_handle);
        curl_global_cleanup();
        return NULL;
    }

    // Cleanup
    curl_easy_cleanup(curl_handle);
    curl_global_cleanup();

    return chunk.memory;
}

// Function to parse the JSON and display the relevant information
void parse_and_display(const char* json_string) {
    cJSON *root = cJSON_Parse(json_string);
    if (root == NULL) {
        const char *error_ptr = cJSON_GetErrorPtr();
        if (error_ptr != NULL) {
            fprintf(stderr, "Error before: %s\n", error_ptr);
        }
        return;
    }

    // Navigate the JSON tree: root -> chart -> result[0] -> meta
    cJSON *chart = cJSON_GetObjectItemCaseSensitive(root, "chart");
    cJSON *result_array = cJSON_GetObjectItemCaseSensitive(chart, "result");
    if (!cJSON_IsArray(result_array) || cJSON_GetArraySize(result_array) == 0) {
        printf("%-10s | %sData Not Found%s\n", "N/A", KYEL, KNRM);
        cJSON_Delete(root);
        return;
    }
    cJSON *result = cJSON_GetArrayItem(result_array, 0);
    cJSON *meta = cJSON_GetObjectItemCaseSensitive(result, "meta");

    // Extract the values
    cJSON *symbol_item = cJSON_GetObjectItemCaseSensitive(meta, "symbol");
    cJSON *price_item = cJSON_GetObjectItemCaseSensitive(meta, "regularMarketPrice");
    cJSON *prev_close_item = cJSON_GetObjectItemCaseSensitive(meta, "chartPreviousClose");

    if (cJSON_IsString(symbol_item) && cJSON_IsNumber(price_item) && cJSON_IsNumber(prev_close_item)) {
        char *symbol = symbol_item->valuestring;
        double price = price_item->valuedouble;
        double prev_close = prev_close_item->valuedouble;
        double change = price - prev_close;
        double percent_change = (change / prev_close) * 100.0;
        
        // Print formatted output with colors
        printf("%-10s | $%9.2f | ", symbol, price);
        if (change >= 0) {
            printf("%s+%7.2f (%+.2f%%)%s\n", KGRN, change, percent_change, KNRM);
        } else {
            printf("%s-%7.2f (%.2f%%)%s\n", KRED, -change, percent_change, KNRM);
        }
    } else {
         printf("%-10s | %sData Parsing Error%s\n", "ERR", KYEL, KNRM);
    }

    // Free the cJSON object
    cJSON_Delete(root);
}

int main(void) {
    // List of stock symbols to track
    const char *symbols[] = {"AAPL", "GOOGL", "TSLA", "MSFT", "NVDA", "BTC-USD", "ETH-USD", NULL};

    while (1) {
        // Clear the console screen (works on Linux/macOS)
        // Use "cls" for Windows
        system("clear");

        printf("--- Live Stock Dashboard ---\n");
        printf("Fetching data... (updates every 30s)\n\n");
        printf("=========================================\n");
        printf("%-10s | %-10s | %-15s\n", "Symbol", "Price", "Change");
        printf("-----------------------------------------\n");

        for (int i = 0; symbols[i] != NULL; i++) {
            char* json_data = fetch_stock_data(symbols[i]);
            if (json_data) {
                parse_and_display(json_data);
                free(json_data); // Don't forget to free the memory!
            }
        }
        
        printf("=========================================\n");
        fflush(stdout); // Ensure all output is printed before sleeping
        sleep(30); // Wait for 30 seconds before the next refresh
    }

    return 0;
}
