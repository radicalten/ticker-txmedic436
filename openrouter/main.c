#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <curl/curl.h>
#include "cJSON.h"

#define INTERVAL_SECONDS 60
#define NUM_SYMBOLS 3

// Configuration: Add your desired stock symbols here
const char *symbols[] = {"AAPL", "MSFT", "GOOGL"};

// Structure to hold the fetched data
typedef struct {
    char symbol[10];
    double price;
    char status[20];
} StockData;

// Callback function for libcurl to write data into a string
static size_t write_callback(void *contents, size_t size, size_t nmemb, void *userp) {
    size_t realsize = size * nmemb;
    char **pptr = (char **)userp;
    
    char *ptr = realloc(*pptr, strlen(*pptr) + realsize + 1);
    if (ptr == NULL) {
        return 0; /* out of memory */
    }
    
    *pptr = ptr;
    memcpy(&((*pptr)[strlen(*pptr)]), contents, realsize);
    (*pptr)[strlen(*pptr)] = 0;
    
    return realsize;
}

/**
 * Fetches the raw JSON string for a given symbol from Yahoo Finance.
 * Returns dynamically allocated string or NULL on failure.
 */
char *fetch_quote_json(const char *symbol) {
    CURL *curl_handle;
    CURLcode res;
    char *buffer = malloc(1); // Start with empty string
    buffer[0] = '\0';

    char url[256];
    // Using the v8 chart API endpoint
    snprintf(url, sizeof(url), "https://query1.finance.yahoo.com/v8/finance/chart/%s?interval=1d&range=1d", symbol);

    curl_handle = curl_easy_init();
    
    if(curl_handle) {
        struct curl_slist *headers = NULL;
        
        // --- CRITICAL: Set User-Agent to mimic a browser ---
        // Yahoo rejects requests without a valid User-Agent
        headers = curl_slist_append(headers, "User-Agent: Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/120.0.0.0 Safari/537.36");
        
        curl_easy_setopt(curl_handle, CURLOPT_URL, url);
        curl_easy_setopt(curl_handle, CURLOPT_HTTPHEADER, headers);
        curl_easy_setopt(curl_handle, CURLOPT_WRITEFUNCTION, write_callback);
        curl_easy_setopt(curl_handle, CURLOPT_WRITEDATA, &buffer);
        
        // Perform the request
        res = curl_easy_perform(curl_handle);
        
        // Cleanup
        curl_easy_cleanup(curl_handle);
        curl_slist_free_all(headers);
        
        if (res != CURLE_OK) {
            fprintf(stderr, "curl_easy_perform() failed: %s\n", curl_easy_strerror(res));
            free(buffer);
            return NULL;
        }
    }
    return buffer;
}

/**
 * Parses the JSON string to extract the price.
 * Returns 0.0 on failure.
 */
double parse_price(const char *json_string) {
    cJSON *root = cJSON_Parse(json_string);
    if (root == NULL) {
        const char *error_ptr = cJSON_GetErrorPtr();
        if (error_ptr != NULL) {
            fprintf(stderr, "JSON Parse Error before: %s\n", error_ptr);
        }
        return 0.0;
    }

    // Navigate the JSON tree: chart -> result[0] -> meta -> regularMarketPrice
    cJSON *chart = cJSON_GetObjectItem(root, "chart");
    if (chart == NULL) {
        cJSON_Delete(root);
        return 0.0;
    }

    cJSON *result_array = cJSON_GetObjectItem(chart, "result");
    if (result_array == NULL || !cJSON_IsArray(result_array)) {
        // Sometimes the API returns an error object or empty result.
        // e.g. "{\"chart\":{\"result\":null,...}}"
        cJSON_Delete(root);
        return 0.0;
    }

    cJSON *first_result = cJSON_GetArrayItem(result_array, 0);
    if (first_result == NULL) {
        cJSON_Delete(root);
        return 0.0;
    }

    cJSON *meta = cJSON_GetObjectItem(first_result, "meta");
    if (meta == NULL) {
        cJSON_Delete(root);
        return 0.0;
    }

    cJSON *price_item = cJSON_GetObjectItem(meta, "regularMarketPrice");
    double price = 0.0;
    if (price_item != NULL && cJSON_IsNumber(price_item)) {
        price = price_item->valuedouble;
    }

    cJSON_Delete(root);
    return price;
}

int main(int argc, char **argv) {
    StockData stocks[NUM_SYMBOLS];
    
    // Initialize cURL global (usually only needed once per program)
    curl_global_init(CURL_GLOBAL_DEFAULT);

    printf("Stock Dashboard Initializing...\n");
    printf("Press Ctrl+C to exit.\n\n");

    while (1) {
        // Clear Terminal (ANSI escape code)
        printf("\033[2J\033[H");
        printf("+--------------------------------------------------+\n");
        printf("|           TERMINAL STOCK DASHBOARD                |\n");
        printf("+--------------------------------------------------+\n");
        printf("| %-10s | %-15s | %-15s |\n", "SYMBOL", "PRICE (USD)", "STATUS");
        printf("+--------------------------------------------------+\n");

        time_t now = time(NULL);
        char time_str[26];
        ctime_r(&now, time_str);
        time_str[24] = '\0'; // Remove newline
        printf("| Last Updated: %s                 |\n", time_str);
        printf("+--------------------------------------------------+\n");

        int error_count = 0;

        for (int i = 0; i < NUM_SYMBOLS; i++) {
            strcpy(stocks[i].symbol, symbols[i]);
            
            // 1. Fetch Data
            char *json_response = fetch_quote_json(stocks[i].symbol);
            
            if (json_response == NULL) {
                stocks[i].price = 0.0;
                strcpy(stocks[i].status, "NETWORK ERROR");
                error_count++;
            } else {
                // 2. Parse Data
                double price = parse_price(json_response);
                
                if (price > 0) {
                    stocks[i].price = price;
                    strcpy(stocks[i].status, "OK");
                } else {
                    // Check if API returned an error structure
                    if (strstr(json_response, "\"error\"")) {
                         strcpy(stocks[i].status, "API ERROR");
                    } else {
                         strcpy(stocks[i].status, "PARSE ERROR");
                    }
                    stocks[i].price = 0.0;
                }
                free(json_response);
            }

            // 3. Display Data
            char price_str[20];
            if (stocks[i].price == 0.0) {
                sprintf(price_str, "N/A");
            } else {
                sprintf(price_str, "%.2f", stocks[i].price);
            }

            printf("| %-10s | %-15s | %-15s |\n", 
                   stocks[i].symbol, 
                   price_str, 
                   stocks[i].status);
        }

        printf("+--------------------------------------------------+\n");
        
        if (error_count == NUM_SYMBOLS) {
            fprintf(stderr, "All fetches failed. Retrying in 10 seconds...\n");
            //sleep(10);
        } else {
            printf("Refreshing in %d seconds...\n", INTERVAL_SECONDS);
            sleep(INTERVAL_SECONDS);
        }
    }

    curl_global_cleanup();
    return 0;
}
