#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <curl/curl.h>
#include <time.h>
#include <unistd.h>
#include <cjson/cJSON.h>

// Define constants
#define MAX_SYMBOLS 5
#define UPDATE_INTERVAL 10  // seconds
#define USER_AGENT "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/91.0.4472.124 Safari/537.36"

// Structure to hold stock data
typedef struct {
    char symbol[10];
    char name[50];
    double price;
    double change;
    double percent_change;
    double previous_close;
} StockData;

// Structure for curl write function
struct MemoryStruct {
    char *memory;
    size_t size;
};

// Function prototypes
static size_t WriteMemoryCallback(void *contents, size_t size, size_t nmemb, void *userp);
int fetch_stock_data(StockData *stock);
void parse_json_response(const char *json_string, StockData *stock);
void display_dashboard(StockData stocks[], int count);
void clear_screen();
void print_centered(const char *text);

int main() {
    // Initialize curl globally
    CURLcode res = curl_global_init(CURL_GLOBAL_DEFAULT);
    if (res != CURLE_OK) {
        fprintf(stderr, "curl_global_init() failed: %s\n", curl_easy_strerror(res));
        return 1;
    }

    // Stock symbols to track
    char symbols[MAX_SYMBOLS][10] = {
        "AAPL",  // Apple
        "GOOGL", // Google
        "MSFT",  // Microsoft
        "AMZN",  // Amazon
        "TSLA"   // Tesla
    };
    
    StockData stocks[MAX_SYMBOLS];
    
    // Initialize stock data
    for (int i = 0; i < MAX_SYMBOLS; i++) {
        strcpy(stocks[i].symbol, symbols[i]);
        strcpy(stocks[i].name, "Loading...");
        stocks[i].price = 0.0;
        stocks[i].change = 0.0;
        stocks[i].percent_change = 0.0;
        stocks[i].previous_close = 0.0;
    }

    // Main loop
    while (1) {
        clear_screen();
        
        // Fetch data for all stocks
        for (int i = 0; i < MAX_SYMBOLS; i++) {
            fetch_stock_data(&stocks[i]);
        }
        
        // Display dashboard
        display_dashboard(stocks, MAX_SYMBOLS);
        
        // Wait for next update
        sleep(UPDATE_INTERVAL);
    }
    
    // Cleanup (this part is never reached due to the infinite loop)
    curl_global_cleanup();
    return 0;
}

// Function to fetch stock data from Yahoo Finance
int fetch_stock_data(StockData *stock) {
    CURL *curl;
    CURLcode res;
    
    // Create URL for Yahoo Finance API
    char url[256];
    snprintf(url, sizeof(url), "https://query1.finance.yahoo.com/v8/finance/chart/%s", stock->symbol);
    
    // Initialize curl handle
    curl = curl_easy_init();
    if (!curl) {
        return -1;
    }
    
    // Set up memory struct for response
    struct MemoryStruct chunk;
    chunk.memory = malloc(1);
    chunk.size = 0;
    
    // Set curl options
    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteMemoryCallback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, (void *)&chunk);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, USER_AGENT);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 10L);
    
    // Perform the request
    res = curl_easy_perform(curl);
    
    // Check for errors
    if (res != CURLE_OK) {
        fprintf(stderr, "curl_easy_perform() failed: %s\n", curl_easy_strerror(res));
        free(chunk.memory);
        curl_easy_cleanup(curl);
        return -1;
    }
    
    // Parse the JSON response
    parse_json_response(chunk.memory, stock);
    
    // Cleanup
    free(chunk.memory);
    curl_easy_cleanup(curl);
    
    return 0;
}

// Callback function for curl write data
static size_t WriteMemoryCallback(void *contents, size_t size, size_t nmemb, void *userp) {
    size_t realsize = size * nmmb;
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

// Function to parse JSON response
void parse_json_response(const char *json_string, StockData *stock) {
    cJSON *json = cJSON_Parse(json_string);
    if (!json) {
        fprintf(stderr, "Error parsing JSON: %s\n", cJSON_GetErrorPtr());
        return;
    }
    
    // Navigate to the chart result
    cJSON *chart = cJSON_GetObjectItemCaseSensitive(json, "chart");
    if (!chart) {
        cJSON_Delete(json);
        return;
    }
    
    cJSON *result = cJSON_GetObjectItemCaseSensitive(chart, "result");
    if (!result || !cJSON_IsArray(result) || cJSON_GetArraySize(result) == 0) {
        cJSON_Delete(json);
        return;
    }
    
    cJSON *first_result = cJSON_GetArrayItem(result, 0);
    if (!first_result) {
        cJSON_Delete(json);
        return;
    }
    
    // Get meta information
    cJSON *meta = cJSON_GetObjectItemCaseSensitive(first_result, "meta");
    if (meta) {
        cJSON *symbol = cJSON_GetObjectItemCaseSensitive(meta, "symbol");
        if (symbol && cJSON_IsString(symbol)) {
            strncpy(stock->symbol, symbol->valuestring, sizeof(stock->symbol) - 1);
        }
        
        cJSON *regularMarketPrice = cJSON_GetObjectItemCaseSensitive(meta, "regularMarketPrice");
        if (regularMarketPrice && cJSON_IsNumber(regularMarketPrice)) {
            stock->price = regularMarketPrice->valuedouble;
        }
        
        cJSON *regularMarketPreviousClose = cJSON_GetObjectItemCaseSensitive(meta, "regularMarketPreviousClose");
        if (regularMarketPreviousClose && cJSON_IsNumber(regularMarketPreviousClose)) {
            stock->previous_close = regularMarketPreviousClose->valuedouble;
        }
        
        cJSON *regularMarketChange = cJSON_GetObjectItemCaseSensitive(meta, "regularMarketChange");
        if (regularMarketChange && cJSON_IsNumber(regularMarketChange)) {
            stock->change = regularMarketChange->valuedouble;
        }
        
        cJSON *regularMarketChangePercent = cJSON_GetObjectItemCaseSensitive(meta, "regularMarketChangePercent");
        if (regularMarketChangePercent && cJSON_IsNumber(regularMarketChangePercent)) {
            stock->percent_change = regularMarketChangePercent->valuedouble;
        }
    }
    
    // Get company name from meta
    cJSON *exchangeName = cJSON_GetObjectItemCaseSensitive(meta, "exchangeName");
    if (exchangeName && cJSON_IsString(exchangeName)) {
        snprintf(stock->name, sizeof(stock->name), "%s (%s)", stock->symbol, exchangeName->valuestring);
    } else {
        snprintf(stock->name, sizeof(stock->name), "%s", stock->symbol);
    }
    
    cJSON_Delete(json);
}

// Function to display the dashboard
void display_dashboard(StockData stocks[], int count) {
    // Print title
    print_centered("STOCK PRICE DASHBOARD");
    printf("\n");
    printf("Last updated: %s", ctime(&(time_t){time(NULL)}));
    printf("------------------------------------------------------------\n");
    
    // Print table headers
    printf("%-10s %-20s %-15s %-10s %-10s\n", 
           "Symbol", "Name", "Price", "Change", "% Change");
    printf("------------------------------------------------------------\n");
    
    // Print stock data
    for (int i = 0; i < count; i++) {
        // Format change with color
        char change_str[20];
        char percent_str[20];
        
        if (stocks[i].change >= 0) {
            snprintf(change_str, sizeof(change_str), "+$%.2f", stocks[i].change);
            snprintf(percent_str, sizeof(percent_str), "+%.2f%%", stocks[i].percent_change);
        } else {
            snprintf(change_str, sizeof(change_str), "-$%.2f", -stocks[i].change);
            snprintf(percent_str, sizeof(percent_str), "-%.2f%%", -stocks[i].percent_change);
        }
        
        printf("%-10s %-20s $%-14.2f %-10s %-10s\n", 
               stocks[i].symbol, 
               stocks[i].name, 
               stocks[i].price, 
               change_str, 
               percent_str);
    }
    
    printf("------------------------------------------------------------\n");
    printf("Updating every %d seconds...\n", UPDATE_INTERVAL);
    printf("Press Ctrl+C to exit\n");
}

// Function to clear the terminal screen
void clear_screen() {
#ifdef _WIN32
    system("cls");
#else
    system("clear");
#endif
}

// Function to print centered text
void print_centered(const char *text) {
    int console_width = 80;  // Default width
#ifdef _WIN32
    CONSOLE_SCREEN_BUFFER_INFO csbi;
    GetConsoleScreenBufferInfo(GetStdHandle(STD_OUTPUT_HANDLE), &csbi);
    console_width = csbi.srWindow.Right - csbi.srWindow.Left + 1;
#else
    struct winsize w;
    ioctl(STDOUT_FILENO, TIOCGWINSZ, &w);
    console_width = w.ws_col;
#endif
    
    int text_length = strlen(text);
    int padding = (console_width - text_length) / 2;
    
    if (padding > 0) {
        printf("%*s%s\n", padding, "", text);
    } else {
        printf("%s\n", text);
    }
}
