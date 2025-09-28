/*
   Simple Live Stock Dashboard in C using Yahoo Finance API
   --------------------------------------------------------
   Dependencies: libcurl (for HTTP requests)
   Compile with: 
      gcc stock_dashboard.c -o stock_dashboard -lcurl
   Usage:
      ./stock_dashboard AAPL MSFT TSLA
*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <curl/curl.h>

/* Buffer to store API response */
struct MemoryChunk {
    char *memory;
    size_t size;
};

/* Callback for libcurl to write received data */
static size_t WriteMemoryCallback(void *contents, size_t size, size_t nmemb, void *userp) {
    size_t realsize = size * nmemb;
    struct MemoryChunk *mem = (struct MemoryChunk *)userp;
    char *ptr = realloc(mem->memory, mem->size + realsize + 1);
    if(ptr == NULL) {
        fprintf(stderr, "Out of memory!\n");
        return 0;
    }
    mem->memory = ptr;
    memcpy(&(mem->memory[mem->size]), contents, realsize);
    mem->size += realsize;
    mem->memory[mem->size] = 0;
    return realsize;
}

/* Extracts "regularMarketPrice" value for a symbol from the JSON response */
double extract_price(const char *json, const char *symbol) {
    // Find the symbol in the JSON
    char pattern[128];
    snprintf(pattern, sizeof(pattern), "\"symbol\":\"%s\"", symbol);
    char *symptr = strstr(json, pattern);
    if(!symptr) return -1;

    // Find "regularMarketPrice" after it
    char *priceKey = strstr(symptr, "\"regularMarketPrice\":");
    if(!priceKey) return -1;

    // Extract the number following it
    double price = -1;
    sscanf(priceKey, "\"regularMarketPrice\":%lf", &price);
    return price;
}

int main(int argc, char *argv[]) {
    if(argc < 2) {
        fprintf(stderr, "Usage: %s SYMBOL [SYMBOL...]\n", argv[0]);
        return 1;
    }

    CURL *curl_handle;
    CURLcode res;

    curl_global_init(CURL_GLOBAL_DEFAULT);

    curl_handle = curl_easy_init();
    if(curl_handle) {
        while(1) {
            struct MemoryChunk chunk;
            chunk.memory = malloc(1);
            chunk.size = 0;

            // Build URL with all symbols
            char url[1024] = "https://query1.finance.yahoo.com/v7/finance/quote?symbols=";
            for(int i=1; i<argc; i++) {
                strcat(url, argv[i]);
                if(i < argc-1) strcat(url, ",");
            }

            curl_easy_setopt(curl_handle, CURLOPT_URL, url);
            curl_easy_setopt(curl_handle, CURLOPT_WRITEFUNCTION, WriteMemoryCallback);
            curl_easy_setopt(curl_handle, CURLOPT_WRITEDATA, (void *)&chunk);

            res = curl_easy_perform(curl_handle);

            if(res != CURLE_OK) {
                fprintf(stderr, "curl_easy_perform() failed: %s\n", curl_easy_strerror(res));
            } else {
                // Clear screen to look like a dashboard
                printf("\033[2J\033[H"); 
                printf("=== Live Stock Dashboard ===\n");
                for(int i=1; i<argc; i++) {
                    double p = extract_price(chunk.memory, argv[i]);
                    if(p > 0)
                        printf("%-8s : $%.2f\n", argv[i], p);
                    else
                        printf("%-8s : (not found)\n", argv[i]);
                }
                printf("============================\n");
            }

            free(chunk.memory);
            sleep(5); // Refresh every 5 seconds
        }
        curl_easy_cleanup(curl_handle);
    }

    curl_global_cleanup();
    return 0;
}
