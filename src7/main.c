#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <curl/curl.h> // For libcurl
#include <unistd.h>     // For sleep()
#include "cJSON.h"      // For cJSON

// ANSI color codes for terminal output
#define KNRM  "\x1B[0m"
#define KRED  "\x1B[31m"
#define KGRN  "\x1B[32m"
#define KYEL  "\x1B[33m"
#define KCYN  "\x1B[36m"

// A struct to hold the data received from libcurl
struct MemoryStruct {
    char *memory;
    size_t size;
};

// Callback function for libcurl to write received data into our MemoryStruct
static size_t WriteMemoryCallback(void *contents, size_t size, size_t nmemb, void *userp) {
    size_t realsize = size * nmemb;
    struct MemoryStruct *mem = (struct MemoryStruct *)userp;

    char *ptr = realloc(mem->memory, mem->size + realsize + 1);
    if (!ptr) {
        printf("not enough memory (realloc returned NULL)\n");
        return 0;
    }

    mem->memory = ptr;
    memcpy(&(mem->memory[mem->size]), contents, realsize);
    mem->size += realsize;
    mem->memory[mem->size] = 0;

    return realsize;
}

// Function to fetch stock data for a given ticker
void fetch_stock_data(const char *ticker) {
    CURL *curl_handle;
    CURLcode res;
    struct MemoryStruct chunk;

    chunk.memory = malloc(1); // will be grown as needed by the callback
    chunk.size = 0;           // no data at this point

    curl_global_init(CURL_GLOBAL_ALL);
    curl_handle = curl_easy_init();

    // Construct the URL
    char url[256];
    snprintf(url, sizeof(url), "https://query1.finance.yahoo.com/v7/finance/quote?symbols=%s", ticker);

    // Set libcurl options
    curl_easy_setopt(curl_handle, CURLOPT_URL, url);
    curl_easy_setopt(curl_handle, CURLOPT_WRITEFUNCTION, WriteMemoryCallback);
    curl_easy_setopt(curl_handle, CURLOPT_WRITEDATA, (void *)&chunk);
    curl_easy_setopt(curl_handle, CURLOPT_USERAGENT, "libcurl-agent/1.0"); // Some APIs require a user agent

    // Perform the request
    res = curl_easy_perform(curl_handle);

    // Check for errors
    if (res != CURLE_OK) {
        fprintf(stderr, "curl_easy_perform() failed: %s\n", curl_easy_strerror(res));
        printf("%-10s | %sERROR: Could not fetch data%s\n", ticker, KRED, KNRM);
    } else {
        // Parse the JSON response
        cJSON *json = cJSON_Parse(chunk.memory);
        if (json == NULL) {
            const char *error_ptr = cJSON_GetErrorPtr();
            if (error_ptr != NULL) {
                fprintf(stderr, "Error before: %s\n", error_ptr);
            }
            printf("%-10s | %sERROR: Failed to parse JSON%s\n", ticker, KRED, KNRM);
        } else {
            // Navigate the JSON structure to get the data
            cJSON *quoteResponse = cJSON_GetObjectItem(json, "quoteResponse");
            cJSON *result = cJSON_GetObjectItem(quoteResponse, "result");
            cJSON *stock_info = cJSON_GetArrayItem(result, 0);

            if (stock_info) {
                cJSON *symbol = cJSON_GetObjectItem(stock_info, "symbol");
                cJSON *price = cJSON_GetObjectItem(stock_info, "regularMarketPrice");
                cJSON *change = cJSON_GetObjectItem(stock_info, "regularMarketChange");
                cJSON *changePercent = cJSON_GetObjectItem(stock_info, "regularMarketChangePercent");

                if (cJSON_IsString(symbol) && cJSON_IsNumber(price) && cJSON_IsNumber(change) && cJSON_IsNumber(changePercent)) {
                    double change_val = change->valuedouble;
                    const char* color = (change_val >= 0) ? KGRN : KRED;
                    char sign = (change_val >= 0) ? '+' : ' ';

                    printf("%-10s | %s%12.2f | %s%c%.2f (%c%.2f%%)%s\n",
                           symbol->valuestring,
                           KCYN,
                           price->valuedouble,
                           color,
                           sign,
                           change_val,
                           sign,
                           changePercent->valuedouble,
                           KNRM);
                } else {
                    printf("%-10s | %sNo data available%s\n", ticker, KYEL, KNRM);
                }
            } else {
                 printf("%-10s | %sInvalid ticker or no data%s\n", ticker, KYEL, KNRM);
            }
            cJSON_Delete(json);
        }
    }

    // Cleanup
    curl_easy_cleanup(curl_handle);
    free(chunk.memory);
    curl_global_cleanup();
}

int main(void) {
    // List of stock tickers to monitor. Add or remove symbols here.
    const char *tickers[] = {"AAPL", "GOOGL", "TSLA", "MSFT", "NVDA", "BTC-USD", "ETH-USD", NULL};

    while (1) {
        // Clear the screen using ANSI escape codes
        printf("\033[2J\033[H");

        printf(KYEL "===== Yahoo Finance Live Stock Dashboard =====\n" KNRM);
        printf("----------------------------------------------\n");
        printf("%-10s | %12s | %s\n", "TICKER", "PRICE (USD)", "CHANGE");
        printf("----------------------------------------------\n");

        // Fetch data for each ticker
        for (int i = 0; tickers[i] != NULL; i++) {
            fetch_stock_data(tickers[i]);
        }
        
        printf("----------------------------------------------\n");
        printf("Refreshing in 15 seconds...\n");
        fflush(stdout); // Make sure the output is displayed before sleeping

        sleep(15); // Wait for 15 seconds before the next refresh
    }

    return 0;
}
