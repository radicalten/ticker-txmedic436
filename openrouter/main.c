#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <curl/curl.h>
#include "cJSON.h"

// Structure to handle curl response memory
struct MemoryStruct {
    char *memory;
    size_t size;
};

static size_t WriteMemoryCallback(void *contents, size_t size, size_t nmemb, void *userp) {
    size_t realsize = size * nmemb;
    struct MemoryStruct *mem = (struct MemoryStruct *)userp;
    char *ptr = realloc(mem->memory, mem->size + realsize + 1);
    if (!ptr) return 0;
    mem->memory = ptr;
    memcpy(&(mem->memory[mem->size]), contents, realsize);
    mem->size += realsize;
    mem->memory[mem->size] = 0;
    return realsize;
}

// Helper function to calculate Exponential Moving Average (EMA)
double calculate_ema(double current_price, double previous_ema, int period) {
    double multiplier = 2.0 / (period + 1);
    return (current_price - previous_ema) * multiplier + previous_ema;
}

// Function to fetch price and calculate MACD signal
// returns: 1 for BUY, -1 for SELL, 0 for NEUTRAL
int get_macd_signal(const char *symbol, double *current_price) {
    CURL *curl_handle;
    CURLcode res;
    struct MemoryStruct chunk = {malloc(1), 0};
    *current_price = -1.0;
    int signal = 0;

    char url[256];
    // Fetch 1-day range with 1-minute intervals to get enough data points for EMA 26
    snprintf(url, sizeof(url), "https://query1.finance.yahoo.com/v8/finance/chart/%s?interval=1m&range=1d", symbol);

    curl_global_init(CURL_GLOBAL_ALL);
    curl_handle = curl_easy_init();

    if (curl_handle) {
        curl_easy_setopt(curl_handle, CURLOPT_URL, url);
        curl_easy_setopt(curl_handle, CURLOPT_WRITEFUNCTION, WriteMemoryCallback);
        curl_easy_setopt(curl_handle, CURLOPT_WRITEDATA, (void *)&chunk);
        curl_easy_setopt(curl_handle, CURLOPT_USERAGENT, "Mozilla/5.0");

        res = curl_easy_perform(curl_handle);
        if (res == CURLE_OK) {
            cJSON *json = cJSON_Parse(chunk.memory);
            if (json) {
                cJSON *chart = cJSON_GetObjectItemCaseSensitive(json, "chart");
                cJSON *result = cJSON_GetObjectItemCaseSensitive(chart, "result");
                if (cJSON_IsArray(result)) {
                    cJSON *first_res = cJSON_GetArrayItem(result, 0);
                    
                    // 1. Get Current Price for display
                    cJSON *meta = cJSON_GetObjectItemCaseSensitive(first_res, "meta");
                    cJSON *price_obj = cJSON_GetObjectItemCaseSensitive(meta, "regularMarketPrice");
                    if (cJSON_IsNumber(price_obj)) *current_price = price_obj->valuedouble;

                    // 2. Get Close Price Array for MACD
                    cJSON *indicators = cJSON_GetObjectItemCaseSensitive(first_res, "indicators");
                    cJSON *quote = cJSON_GetObjectItemCaseSensitive(cJSON_GetArrayItem(indicators, 0), "quote");
                    cJSON *close_array = cJSON_GetObjectItemCaseSensitive(quote, "close");

                    if (cJSON_IsArray(close_array)) {
                        int size = cJSON_GetArraySize(close_array);
                        double *prices = malloc(sizeof(double) * size);
                        int valid_count = 0;

                        // Filter out nulls (Yahoo often has gaps in 1m data)
                        for (int i = 0; i < size; i++) {
                            cJSON *val = cJSON_GetArrayItem(close_array, i);
                            if (cJSON_IsNumber(val)) {
                                prices[valid_count++] = val->valuedouble;
                            }
                        }

                        if (valid_count > 26) {
                            // Calculate EMA 12 and EMA 26
                            double ema12 = prices[0], ema26 = prices[0];
                            for (int i = 1; i < valid_count; i++) {
                                ema12 = calculate_ema(prices[i], ema12, 12);
                                ema26 = calculate_ema(prices[i], ema26, 26);
                            }

                            double macd_line = ema12 - ema26;

                            // Calculate Signal Line (EMA 9 of the MACD line)
                            // Since we don't have a long history of MACD lines, 
                            // we approximate by calculating a short EMA of the most recent trend
                            double signal_line = macd_line; 
                            // In a full implementation, you'd loop through the last 9 MACD values
                            
                            // Simplified Signal: Is MACD line positive and rising?
                            // For this logic: If MACD > 0 and MACD is higher than it was 5 mins ago
                            double prev_ema12 = prices[0], prev_ema26 = prices[0];
                            for (int i = 1; i < valid_count - 5; i++) {
                                prev_ema12 = calculate_ema(prices[i], prev_ema12, 12);
                                prev_ema26 = calculate_ema(prices[i], prev_ema26, 26);
                            }
                            double prev_macd = prev_ema12 - prev_ema26;

                            if (macd_line > prev_macd && macd_line > 0) signal = 1;  // Bullish
                            else if (macd_line < prev_macd && macd_line < 0) signal = -1; // Bearish
                        }
                        free(prices);
                    }
                }
                cJSON_Delete(json);
            }
        }
        curl_easy_cleanup(curl_handle);
    }
    free(chunk.memory);
    curl_global_cleanup();
    return signal;
}

int main() {
    const char *symbols[] = {"AAPL", "MSFT", "GOOGL", "AMZN", "TSLA", "BTC-USD"};
    int num_symbols = sizeof(symbols) / sizeof(symbols[0]);

    while (1) {
        printf("\033[H\033[J"); 
        printf("====================================================\n");
        printf("   LIVE STOCK DASHBOARD (Yahoo Finance + MACD)      \n");
        printf("====================================================\n");
        printf("%-12s | %-10s | %-10s\n", "SYMBOL", "PRICE", "SIGNAL");
        printf("----------------------------------------------------\n");

        for (int i = 0; i < num_symbols; i++) {
            double price;
            int signal = get_macd_signal(symbols[i], &price);
            
            if (price > 0) {
                char *sig_text = (signal == 1) ? "BUY 🟢" : (signal == -1) ? "SELL 🔴" : "HOLD ⚪";
                printf("%-12s | $%-9.2f | %-10s\n", symbols[i], price, sig_text);
            } else {
                printf("%-12s | %-10s | %-10s\n", symbols[i], "Error", "N/A");
            }
        }
        
        printf("----------------------------------------------------\n");
        printf("Updating every 10 seconds... (Ctrl+C to quit)\n");
        sleep(10); 
    }
    return 0;
}
