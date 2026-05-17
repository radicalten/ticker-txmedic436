// main.c
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <curl/curl.h>
#include "cJSON.h"

// Function to handle curl response data
size_t WriteCallback(void* contents, size_t size, size_t nmemb, void* userp) {
    ((char**)userp)[0] = realloc(*(char**)userp, size * nmemb + 1);
    memcpy(*(char**)userp + size * nmemb, contents, size * nmemb);
    (*(char**)userp)[size * nmemb] = '\0';
    return size * nmemb;
}

int main() {
    CURL *curl;
    CURLcode res;
    char *response = NULL;
    size_t response_len = 0;
    cJSON *root;

    // Initialize cURL
    curl = curl_easy_init();
    if (!curl) {
        fprintf(stderr, "Failed to initialize cURL\n");
        return 1;
    }

    // Set URL with stock ticker (e.g., AAPL)
    const char *ticker = "AAPL";
    char url[256];
    snprintf(url, sizeof(url), "https://query1.finance.yahoo.com/v8/finance/quoteSummary?symbol=%s", ticker);

    // Set custom user-agent
    struct curl_slist *headers = NULL;
    headers = curl_slist_append(headers, "User-Agent: Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/91.0.4472.124 Safari/537.36");

    // Configure cURL
    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteCallback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);

    // Perform request
    res = curl_easy_perform(curl);
    if (res != CURLE_OK) {
        fprintf(stderr, "curl_easy_perform() failed: %s\n", curl_easy_strerror(res));
        goto cleanup;
    }

    // Parse JSON
    root = cJSON_Parse(response);
    if (!root) {
        fprintf(stderr, "Failed to parse JSON: %s\n", cJSON_GetErrorPtr());
        goto cleanup;
    }

    // Extract price (adjust based on actual JSON structure)
    cJSON *price = cJSON_GetObjectItemCaseSensitive(root, "quoteSummary");
    if (price && cJSON_IsObject(price)) {
        price = cJSON_GetObjectItemCaseSensitive(price, "result");
        if (price && cJSON_IsArray(price) && price->valueint > 0) {
            price = cJSON_GetArrayItem(price, 0);
            if (price && cJSON_IsObject(price)) {
                cJSON *regularPrice = cJSON_GetObjectItemCaseSensitive(price, "price");
                if (regularPrice && cJSON_IsObject(regularPrice)) {
                    cJSON *regularMarketPrice = cJSON_GetObjectItemCaseSensitive(regularPrice, "regularMarketPrice");
                    if (regularMarketPrice && cJSON_IsNumber(regularMarketPrice)) {
                        printf("\033[2J\033[1;1H"); // Clear screen and move to top-left
                        printf("Live Stock Price (AAPL): $%.2f\n", regularMarketPrice->valuedouble);
                    }
                }
            }
        }
    }

cleanup:
    // Cleanup
    cJSON_Delete(root);
    free(response);
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);

    // Live update loop
    while (1) {
        sleep(5); // Update every 5 seconds
        // Repeat the above steps here (simplified for brevity)
    }

    return 0;
}
