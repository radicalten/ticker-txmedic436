#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <signal.h>
#include <curl/curl.h>
#include "cJSON.h"

/* Configuration */
#define REFRESH_INTERVAL 5
#define MAX_SYMBOLS      5
#define API_URL "https://api.coingecko.com/api/v3/simple/price?ids=bitcoin,ethereum,cardano,solana,polkadot&vs_currencies=usd"

/* Data Structures */
typedef struct {
    char *data;
    size_t len;
} ResponseBuffer;

typedef struct {
    char symbol[10];
    double price;
    double prev_price;
} Stock;

/* Global State */
Stock stocks[MAX_SYMBOLS];
int running = 1;

/* Signal Handler for Clean Exit */
void signal_handler(int sig) {
    (void)sig;
    running = 0;
}

/* libcurl Write Callback */
size_t write_callback(void *contents, size_t size, size_t nmemb, void *userp) {
    size_t total = size * nmemb;
    ResponseBuffer *buf = (ResponseBuffer *)userp;
    char *new_data = realloc(buf->data, buf->len + total + 1);
    if (!new_data) return 0;
    buf->data = new_data;
    memcpy(buf->data + buf->len, contents, total);
    buf->len += total;
    buf->data[buf->len] = '\0';
    return total;
}

/* Fetch JSON from API */
char* fetch_json(const char *url) {
    CURL *curl = curl_easy_init();
    ResponseBuffer buf = { .data = malloc(1), .len = 0 };
    buf.data[0] = '\0';

    if (curl) {
        curl_easy_setopt(curl, CURLOPT_URL, url);
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_callback);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &buf);
        curl_easy_setopt(curl, CURLOPT_USERAGENT, "cJSON-Dashboard/1.0");
        curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
        curl_easy_setopt(curl, CURLOPT_TIMEOUT, 10L);
        
        CURLcode res = curl_easy_perform(curl);
        if (res != CURLE_OK) {
            fprintf(stderr, "curl error: %s\n", curl_easy_strerror(res));
            free(buf.data);
            buf.data = NULL;
        }
        curl_easy_cleanup(curl);
    }
    return buf.data;
}

/* Parse JSON & Update Stocks */
void parse_json(const char *json_str) {
    cJSON *root = cJSON_Parse(json_str);
    if (!root) {
        fprintf(stderr, "JSON parse error: %s\n", cJSON_GetErrorPtr());
        return;
    }

    /* Mapping: API ID -> Display Symbol */
    const char *api_ids[]    = {"bitcoin", "ethereum", "cardano", "solana", "polkadot"};
    const char *display_sym[] = {"BTC", "ETH", "ADA", "SOL", "DOT"};

    for (int i = 0; i < MAX_SYMBOLS; i++) {
        cJSON *coin = cJSON_GetObjectItemCaseSensitive(root, api_ids[i]);
        if (cJSON_IsObject(coin)) {
            cJSON *usd = cJSON_GetObjectItemCaseSensitive(coin, "usd");
            if (cJSON_IsNumber(usd)) {
                stocks[i].prev_price = stocks[i].price;
                stocks[i].price = usd->valuedouble;
                strncpy(stocks[i].symbol, display_sym[i], sizeof(stocks[i].symbol) - 1);
                stocks[i].symbol[sizeof(stocks[i].symbol) - 1] = '\0';
            }
        }
    }
    cJSON_Delete(root);
}

/* Draw Terminal Dashboard */
void draw_dashboard(void) {
    /* Clear screen & move cursor to top-left */
    printf("\033[2J\033[H");
    
    /* Header */
    printf("\033[1;36m╔══════════════════════════════════════════╗\n");
    printf("║           LIVE MARKET DASHBOARD            ║\n");
    printf("╠══════════════════════════════════════════╣\n");
    printf("║ %-8s %-12s %-10s %-8s ║\n", "Symbol", "Price (USD)", "Change", "Time");
    printf("╠══════════════════════════════════════════╣\n");

    /* Time */
    time_t now = time(NULL);
    struct tm *t = localtime(&now);
    char time_str[20];
    strftime(time_str, sizeof(time_str), "%H:%M:%S", t);

    /* Rows */
    for (int i = 0; i < MAX_SYMBOLS; i++) {
        if (stocks[i].symbol[0] == '\0') continue;

        double change = stocks[i].price - stocks[i].prev_price;
        double pct = (stocks[i].prev_price > 0.0) ? (change / stocks[i].prev_price) * 100.0 : 0.0;
        
        const char *color = (change >= 0.0) ? "\033[32m" : "\033[31m";
        const char *arrow = (change >= 0.0) ? "▲" : "▼";

        printf("║ %-8s $%-11.2f %s%s%.2f%%\033[0m %-10s ║\n",
               stocks[i].symbol, stocks[i].price, color, arrow, pct, time_str);
    }

    /* Footer */
    printf("╠══════════════════════════════════════════╣\n");
    printf("║ Press Ctrl+C to exit                     ║\n");
    printf("╚══════════════════════════════════════════╝\033[0m\n");
}

int main(void) {
    signal(SIGINT, signal_handler);
    curl_global_init(CURL_GLOBAL_DEFAULT);

    /* Initialize stock array */
    memset(stocks, 0, sizeof(stocks));

    printf("Starting dashboard... Press Ctrl+C to exit.\n");
    sleep(1);

    while (running) {
        char *json = fetch_json(API_URL);
        if (json && strlen(json) > 0) {
            parse_json(json);
            draw_dashboard();
        } else {
            printf("\033[2J\033[H⚠️  Failed to fetch market data. Retrying in %ds...\n", REFRESH_INTERVAL);
        }
        free(json);
        sleep(REFRESH_INTERVAL);
    }

    curl_global_cleanup();
    printf("\n✅ Dashboard exited cleanly.\n");
    return 0;
}
