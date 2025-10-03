#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h> // For sleep()
#include <time.h>   // For timestamp
#include <curl/curl.h>
#include "cJSON.h"

// --- Configuration ---
#define UPDATE_INTERVAL_SECONDS 15
#define USER_AGENT "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/98.0.4758.102 Safari/537.36"
#define API_URL_FORMAT "https://query1.finance.yahoo.com/v8/finance/chart/%s?range=3mo&interval=1d"
#define DATA_START_ROW 6 // The row number where the first stock ticker will be printed

// Add or remove stock/crypto tickers here
const char *tickers[] = {"AAPL", "GOOGL", "TSLA", "MSFT", "NVDA", "BTC-USD", "ETH-USD"};
const int num_tickers = sizeof(tickers) / sizeof(tickers[0]);

// --- Color Definitions for Terminal ---
#define KNRM  "\x1B[0m"  // Normal
#define KRED  "\x1B[31m"  // Red
#define KGRN  "\x1B[32m"  // Green
#define KYEL  "\x1B[33m"  // Yellow
#define KBLU  "\x1B[34m"  // Blue

// --- Struct to hold HTTP response data ---
typedef struct {
    char *memory;
    size_t size;
} MemoryStruct;

// --- Function Prototypes ---
static size_t write_callback(void *contents, size_t size, size_t nmemb, void *userp);
char* fetch_url(const char *url);
void parse_and_print_stock_data(const char *json_string, int row);
void setup_dashboard_ui();
void update_timestamp();
void run_countdown();
void print_error_on_line(const char* ticker, const char* error_msg, int row);
void hide_cursor();
void show_cursor();
void cleanup_on_exit();

// MACD Calculation Prototypes
double* calculate_ema(const double* data, int data_size, int period);
double* calculate_signal_line(const double* macd_line, int data_size, int macd_start_index, int signal_period);
void calculate_macd(const double* close_prices, int num_prices, double** macd_line_out, double** signal_line_out, int* last_valid_index);

// --- Main Application ---
int main(void) {
    atexit(cleanup_on_exit);
    curl_global_init(CURL_GLOBAL_ALL);
    setup_dashboard_ui();

    while (1) {
        update_timestamp();

        char url[256];
        for (int i = 0; i < num_tickers; i++) {
            int current_row = DATA_START_ROW + i;
            snprintf(url, sizeof(url), API_URL_FORMAT, tickers[i]);
            
            char *json_response = fetch_url(url);
            
            if (json_response) {
                parse_and_print_stock_data(json_response, current_row);
                free(json_response);
            } else {
                print_error_on_line(tickers[i], "Failed to fetch data", current_row);
            }
        }
        
        run_countdown();
    }

    curl_global_cleanup();
    show_cursor();
    return 0;
}


// --- MACD Calculation Functions ---

/**
 * @brief Calculates Exponential Moving Average (EMA) for a given period.
 * @return A dynamically allocated array of EMA values which the caller must free.
 *         The returned array has the same size as data, but the first (period-1) values will be 0.
 */
double* calculate_ema(const double* data, int data_size, int period) {
    if (data_size < period) return NULL;

    double* ema_values = (double*)calloc(data_size, sizeof(double));
    if (!ema_values) return NULL;

    double multiplier = 2.0 / (period + 1.0);
    double sma = 0.0;

    // 1. Calculate the initial SMA for the first 'period' days
    for (int i = 0; i < period; i++) {
        sma += data[i];
    }
    ema_values[period - 1] = sma / period;

    // 2. Calculate the rest of the EMAs
    for (int i = period; i < data_size; i++) {
        ema_values[i] = (data[i] * multiplier) + (ema_values[i - 1] * (1.0 - multiplier));
    }

    return ema_values;
}

/**
 * @brief Calculates the Signal Line (a 9-period EMA of the MACD line).
 *        Special handling is needed because the MACD data doesn't start at index 0.
 * @return A dynamically allocated array of Signal values, aligned with the original data size. Caller must free.
 */
double* calculate_signal_line(const double* macd_line, int data_size, int macd_start_index, int signal_period) {
    int macd_data_count = data_size - macd_start_index;
    if (macd_data_count < signal_period) return NULL;

    double* signal_values = (double*)calloc(data_size, sizeof(double));
    if(!signal_values) return NULL;

    const double* macd_data = &macd_line[macd_start_index];
    
    // 1. Calculate initial SMA on the first 'signal_period' points of *actual* MACD data
    double sma = 0.0;
    for (int i = 0; i < signal_period; i++) {
        sma += macd_data[i];
    }
    
    int first_signal_index = macd_start_index + signal_period - 1;
    signal_values[first_signal_index] = sma / signal_period;

    // 2. Calculate the rest of the EMAs
    double multiplier = 2.0 / (signal_period + 1.0);
    for (int i = first_signal_index + 1; i < data_size; i++) {
        signal_values[i] = (macd_line[i] * multiplier) + (signal_values[i-1] * (1.0 - multiplier));
    }

    return signal_values;
}


/**
 * @brief Calculates the MACD and Signal lines from historical closing prices.
 * @param close_prices Array of closing prices.
 * @param num_prices Number of elements in the array.
 * @param macd_line_out Output pointer for the calculated MACD line (caller must free).
 * @param signal_line_out Output pointer for the calculated Signal line (caller must free).
 * @param last_valid_index Output pointer to get the index of the latest valid data point.
 */
void calculate_macd(const double* close_prices, int num_prices, double** macd_line_out, double** signal_line_out, int* last_valid_index) {
    *macd_line_out = NULL;
    *signal_line_out = NULL;
    *last_valid_index = -1;

    int short_period = 12;
    int long_period = 26;
    int signal_period = 9;

    if (num_prices < long_period + signal_period) return;

    double* ema12 = calculate_ema(close_prices, num_prices, short_period);
    double* ema26 = calculate_ema(close_prices, num_prices, long_period);

    if (!ema12 || !ema26) {
        free(ema12);
        free(ema26);
        return;
    }

    double* macd_line = (double*)calloc(num_prices, sizeof(double));
    if (!macd_line) {
        free(ema12);
        free(ema26);
        return;
    }
    
    int macd_start_index = long_period - 1;
    for (int i = macd_start_index; i < num_prices; i++) {
        macd_line[i] = ema12[i] - ema26[i];
    }

    free(ema12);
    free(ema26);

    double* signal_line = calculate_signal_line(macd_line, num_prices, macd_start_index, signal_period);
    if (!signal_line) {
        free(macd_line);
        return;
    }

    *macd_line_out = macd_line;
    *signal_line_out = signal_line;
    *last_valid_index = num_prices - 1;
}

// --- Helper Functions ---

static size_t write_callback(void *contents, size_t size, size_t nmemb, void *userp) {
    size_t realsize = size * nmemb;
    MemoryStruct *mem = (MemoryStruct *)userp;
    char *ptr = realloc(mem->memory, mem->size + realsize + 1);
    if (ptr == NULL) {
        printf("error: not enough memory (realloc returned NULL)\n");
        return 0;
    }
    mem->memory = ptr;
    memcpy(&(mem->memory[mem->size]), contents, realsize);
    mem->size += realsize;
    mem->memory[mem->size] = 0;
    return realsize;
}

char* fetch_url(const char *url) {
    CURL *curl_handle;
    CURLcode res;
    MemoryStruct chunk;
    chunk.memory = malloc(1);
    chunk.size = 0;

    curl_handle = curl_easy_init();
    if (curl_handle) {
        curl_easy_setopt(curl_handle, CURLOPT_URL, url);
        curl_easy_setopt(curl_handle, CURLOPT_USERAGENT, USER_AGENT);
        curl_easy_setopt(curl_handle, CURLOPT_WRITEFUNCTION, write_callback);
        curl_easy_setopt(curl_handle, CURLOPT_WRITEDATA, (void *)&chunk);
        curl_easy_setopt(curl_handle, CURLOPT_FOLLOWLOCATION, 1L);

        res = curl_easy_perform(curl_handle);
        if (res != CURLE_OK) {
            fprintf(stderr, "curl_easy_perform() failed: %s\n", curl_easy_strerror(res));
            free(chunk.memory);
            curl_easy_cleanup(curl_handle);
            return NULL;
        }
        curl_easy_cleanup(curl_handle);
        return chunk.memory;
    }
    free(chunk.memory);
    return NULL;
}


void parse_and_print_stock_data(const char *json_string, int row) {
    cJSON *root = cJSON_Parse(json_string);
    if (root == NULL) {
        print_error_on_line("JSON", "Parse Error", row);
        return;
    }

    cJSON *chart = cJSON_GetObjectItemCaseSensitive(root, "chart");
    cJSON *result_array = cJSON_GetObjectItemCaseSensitive(chart, "result");
    if (!cJSON_IsArray(result_array) || cJSON_GetArraySize(result_array) == 0) {
        char* err_desc = "Invalid ticker or no data";
        cJSON* error_obj = cJSON_GetObjectItemCaseSensitive(chart, "error");
        if(error_obj && cJSON_GetObjectItemCaseSensitive(error_obj, "description")) {
            err_desc = cJSON_GetObjectItemCaseSensitive(error_obj, "description")->valuestring;
        }
        print_error_on_line("API Error", err_desc, row);
        cJSON_Delete(root);
        return;
    }

    cJSON *result = cJSON_GetArrayItem(result_array, 0);
    cJSON *meta = cJSON_GetObjectItemCaseSensitive(result, "meta");
    
    // --- Extract Basic Info ---
    const char *symbol = cJSON_GetObjectItemCaseSensitive(meta, "symbol")->valuestring;
    double price = cJSON_GetObjectItemCaseSensitive(meta, "regularMarketPrice")->valuedouble;
    double prev_close = cJSON_GetObjectItemCaseSensitive(meta, "chartPreviousClose")->valuedouble;
    
    double change = price - prev_close;
    double percent_change = (prev_close == 0) ? 0 : (change / prev_close) * 100.0;
    const char* color = (change >= 0) ? KGRN : KRED;
    char sign = (change >= 0) ? '+' : '-';

    // --- Extract Historical Data for MACD ---
    double macd_value = 0.0, signal_value = 0.0;
    int macd_calculated = 0;

    cJSON* indicators = cJSON_GetObjectItemCaseSensitive(result, "indicators");
    cJSON* quote_array = cJSON_GetObjectItemCaseSensitive(indicators, "quote");
    if (cJSON_IsArray(quote_array) && cJSON_GetArraySize(quote_array) > 0) {
        cJSON* quote = cJSON_GetArrayItem(quote_array, 0);
        cJSON* close_prices_json = cJSON_GetObjectItemCaseSensitive(quote, "close");

        if (cJSON_IsArray(close_prices_json)) {
            int num_prices = cJSON_GetArraySize(close_prices_json);
            double* close_prices = malloc(num_prices * sizeof(double));
            int valid_prices = 0;
            
            if (close_prices) {
                for (int i = 0; i < num_prices; i++) {
                    cJSON* price_item = cJSON_GetArrayItem(close_prices_json, i);
                    if (cJSON_IsNumber(price_item)) {
                        close_prices[valid_prices++] = price_item->valuedouble;
                    }
                }

                if (valid_prices > 35) { // Need at least 26+9 for a good value
                    double *macd_line = NULL, *signal_line = NULL;
                    int last_valid_index = -1;
                    calculate_macd(close_prices, valid_prices, &macd_line, &signal_line, &last_valid_index);

                    if (macd_line && signal_line && last_valid_index >= 0) {
                        macd_value = macd_line[last_valid_index];
                        signal_value = signal_line[last_valid_index];
                        macd_calculated = 1;
                    }
                    free(macd_line);
                    free(signal_line);
                }
                free(close_prices);
            }
        }
    }

    // --- Print the final row ---
    printf("\033[%d;1H", row); // Move cursor
    
    printf("%-10s | %s%10.2f%s | %s%c%9.2f%s | %s%c%10.2f%%%s | ",
           symbol,
           KBLU, price, KNRM,
           color, sign, (change >= 0 ? change : -change), KNRM,
           color, sign, (percent_change >= 0 ? percent_change : -percent_change), KNRM);

    if (macd_calculated) {
        const char* macd_color = (macd_value >= signal_value) ? KGRN : KRED;
        printf("%s%9.2f%s | %9.2f", macd_color, macd_value, KNRM, signal_value);
    } else {
        printf("%s%9s%s | %9s", KYEL, "N/A", KNRM, "N/A");
    }
    printf("\033[K\n");

    cJSON_Delete(root);
}

void print_error_on_line(const char* ticker, const char* error_msg, int row) {
    printf("\033[%d;1H", row);
    printf("%-10s | %s%-60s%s\033[K\n", ticker, KRED, error_msg, KNRM);
}


// --- UI and Terminal Control Functions ---

void setup_dashboard_ui() {
    hide_cursor();
    printf("\033[2J\033[H");

    printf("--- C Terminal Stock & Crypto Dashboard ---\n");
    printf("\n");
    printf("\n");

    printf("%-10s | %11s | %11s | %13s | %9s | %9s\n", 
           "Ticker", "Price", "Change", "% Change", "MACD", "Signal");
    printf("--------------------------------------------------------------------------------\n");
    
    for (int i = 0; i < num_tickers; i++) {
        printf("%-10s | %sFetching...%s\n", tickers[i], KYEL, KNRM);
    }
    fflush(stdout);
}

void update_timestamp() {
    time_t t = time(NULL);
    struct tm *tm = localtime(&t);
    char time_str[64];
    strftime(time_str, sizeof(time_str), "%Y-%m-%d %H:%M:%S", tm);

    printf("\033[2;1H");
    printf("Last updated: %s\033[K\n", time_str);
    fflush(stdout);
}

void run_countdown() {
    int update_line = DATA_START_ROW + num_tickers + 1;
    for (int i = UPDATE_INTERVAL_SECONDS; i > 0; i--) {
        printf("\033[%d;1H\033[KUpdating in %2d seconds...", i, update_line);
        fflush(stdout);
        sleep(1);
    }
    printf("\033[%d;1H\033[KUpdating now...           ", update_line);
    fflush(stdout);
}

void hide_cursor() {
    printf("\033[?25l");
    fflush(stdout);
}

void show_cursor() {
    printf("\033[?25h");
    fflush(stdout);
}

void cleanup_on_exit() {
    show_cursor();
}
