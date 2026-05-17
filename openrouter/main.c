#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "cJSON.h"

#ifdef _WIN32
#include <windows.h>
#define CLEAR_SCREEN system("cls")
#define SLEEP_MS(ms) Sleep(ms)
#else
#include <unistd.h>
#define CLEAR_SCREEN system("clear")
#define SLEEP_MS(ms) sleep(ms/1000)
#endif

#define BUFFER_SIZE 8192
#define UPDATE_INTERVAL 3000  // milliseconds
#define MAX_STOCKS 5

// ANSI Terminal Colors (Windows 10+ supports these natively)
#define RESET   "\033[0m"
#define GREEN   "\033[32m"
#define RED     "\033[31m"
#define CYAN    "\033[36m"
#define BOLD    "\033[1m"

/* Fetch JSON from URL using curl. Returns malloc'd string or NULL on failure. */
char* fetch_json(const char* url) {
    char cmd[1024];
    snprintf(cmd, sizeof(cmd), "curl -s --connect-timeout 5 \"%s\"", url);
    FILE* pipe = popen(cmd, "r");
    if (!pipe) return NULL;

    char* buffer = malloc(BUFFER_SIZE);
    if (!buffer) { pclose(pipe); return NULL; }

    size_t len = fread(buffer, 1, BUFFER_SIZE-1, pipe);
    buffer[len] = '\0';
    pclose(pipe);

    // Quick validation to avoid crashing display on bad responses
    cJSON* test = cJSON_Parse(buffer);
    if (!test) {
        free(buffer);
        return NULL;
    }
    cJSON_Delete(test);
    return buffer;
}

/* Generate realistic mock stock data for demonstration */
char* generate_mock_json(void) {
    static char json[BUFFER_SIZE];
    
    const char* symbols[] = {"AAPL", "GOOGL", "MSFT", "TSLA", "AMZN"};
    float base_prices[] = {175.0f, 140.0f, 330.0f, 250.0f, 178.0f};

    sprintf(json, "{\"stocks\":[");
    for (int i = 0; i < MAX_STOCKS; i++) {
        float change_pct = ((float)rand() / RAND_MAX) * 4.0f - 2.0f; // -2% to +2%
        float price = base_prices[i] * (1.0f + change_pct/100.0f);
        float change = price - base_prices[i];
        
        if (i > 0) strcat(json, ",");
        sprintf(json + strlen(json),
            "{\"symbol\":\"%s\",\"price\":%.2f,\"change\":%.2f,\"change_pct\":%.2f}",
            symbols[i], price, change, change_pct);
    }
    strcat(json, "]}");
    return json;
}

/* Parse JSON and render dashboard to terminal */
void display_dashboard(const char* json_str) {
    cJSON* root = cJSON_Parse(json_str);
    if (!root) {
        fprintf(stderr, "\n[ERROR] Failed to parse JSON\n");
        return;
    }

    cJSON* stocks = cJSON_GetObjectItem(root, "stocks");
    if (!stocks || !cJSON_IsArray(stocks)) {
        fprintf(stderr, "\n[ERROR] Invalid JSON structure (missing 'stocks' array)\n");
        cJSON_Delete(root);
        return;
    }

    printf("%s%cSTOCK DASHBOARD%c %s\n", BOLD, CYAN, 187, 188, RESET);
    printf("%s  %-6s | %-10s | %-10s | %-8s  %s\n", BOLD, "Symbol", "Price", "Change", "Change%", RESET);
    printf("%s  %-6s-+-%-10s-+-%-10s-+-%-8s  %s\n", BOLD, "------", "----------", "----------", "--------", RESET);

    cJSON* item;
    cJSON_ArrayForEach(item, stocks) {
        cJSON* sym = cJSON_GetObjectItem(item, "symbol");
        cJSON* price = cJSON_GetObjectItem(item, "price");
        cJSON* change = cJSON_GetObjectItem(item, "change");
        cJSON* change_pct = cJSON_GetObjectItem(item, "change_pct");

        if (!sym || !price || !change || !change_pct) continue;

        double val_change = cJSON_GetNumberValue(change);
        const char* color = val_change >= 0 ? GREEN : RED;

        char change_str[16], pct_str[16];
        snprintf(change_str, sizeof(change_str), "%+.2f", val_change);
        snprintf(pct_str, sizeof(pct_str), "%+.2f%%", cJSON_GetNumberValue(change_pct));

        printf("  %-6s | $%-9.2f | %s%-10s%s | %s%-8s%s\n",
               sym->valuestring,
               cJSON_GetNumberValue(price),
               color, change_str, RESET,
               color, pct_str, RESET);
    }

    time_t now = time(NULL);
    char time_str[26];
    strftime(time_str, sizeof(time_str), "%Y-%m-%d %H:%M:%S", localtime(&now));
    printf("%s  Last updated: %s\n", BOLD, time_str);

    cJSON_Delete(root);
}

int main(void) {
    srand((unsigned int)time(NULL));
    printf("Live Stock Dashboard (Press Ctrl+C to exit)\n");
    SLEEP_MS(1500);

    // Set to a valid API URL to fetch real data, or leave NULL for mock data
    const char* api_url = NULL;
    // Example: const char* api_url = "https://api.example.com/stocks";

    while (1) {
        CLEAR_SCREEN;
        
        char* json_data = api_url ? fetch_json(api_url) : generate_mock_json();

        if (json_data) {
            display_dashboard(json_data);
            if (api_url) free(json_data); // Only free dynamically allocated memory
        } else {
            printf("\n[WARN] Fetch failed, falling back to mock data\n");
            json_data = generate_mock_json();
            display_dashboard(json_data);
            free(json_data);
        }

        SLEEP_MS(UPDATE_INTERVAL);
    }

    return 0;
}
