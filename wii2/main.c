#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <curl/curl.h>
#include "cJSON.h"
#include <time.h> // For timestamp

// WII: Include necessary libogc headers
#include <ogc/video.h>
#include <ogc/consol.h>
#include <ogc/system.h>
#include <ogc/lwp.h>
#include <wiiuse/wpad.h>

// --- Configuration ---
#define UPDATE_INTERVAL_SECONDS 15
#define USER_AGENT "WiiStockTicker/1.0 (Nintendo Wii)" // A more fitting user agent
#define API_URL_FORMAT "https://query1.finance.yahoo.com/v8/finance/chart/%s"
#define DATA_START_ROW 6 // The row number where the first stock ticker will be printed

// Add or remove stock tickers here
const char *tickers[] = {"AAPL", "GOOGL", "TSLA", "MSFT", "NVDA", "BTC-USD", "ETH-USD"};
const int num_tickers = sizeof(tickers) / sizeof(tickers[0]);

// --- Color Definitions for Terminal ---
// These ANSI codes are supported by libogc's console, so they can stay!
#define KNRM  "\x1B[0m"   // Normal
#define KRED  "\x1B[31;1m" // Bright Red
#define KGRN  "\x1B[32;1m" // Bright Green
#define KYEL  "\x1B[33;1m" // Bright Yellow
#define KBLU  "\x1B[34;1m" // Bright Blue

// --- Struct to hold HTTP response data (unchanged) ---
typedef struct {
    char *memory;
    size_t size;
} MemoryStruct;

// --- Function Prototypes (some removed, some added) ---
static size_t write_callback(void *contents, size_t size, size_t nmemb, void *userp);
char* fetch_url(const char *url);
void parse_and_print_stock_data(const char *json_string, int row);
void setup_dashboard_ui();
void update_timestamp();
void run_countdown_wii(); // WII: Renamed to reflect Wii-specific implementation
void print_error_on_line(const char* ticker, const char* error_msg, int row);
void init_wii_systems(); // WII: New function for initialization

// --- Main Application ---
int main(void) {
    // WII: Initialize video, console, and input systems
    init_wii_systems();

    // WII: Initialize the network stack.
    printf("Initializing network... please wait.\n");
    s32 net_result = net_init();
    if (net_result < 0) {
        printf("Error: Unable to initialize network. Exiting.\n");
        VIDEO_WaitVSync(); VIDEO_WaitVSync(); VIDEO_WaitVSync(); // Pause for 3 seconds
        return -1;
    }
    char local_ip[16] = {0};
    if_config(local_ip, NULL, NULL, true);
    printf("Network initialized. Wii IP: %s\n", local_ip);
    VIDEO_WaitVSync(); // Short pause

    // Initialize libcurl
    curl_global_init(CURL_GLOBAL_ALL);

    // Initial, one-time setup: clear screen, print static layout
    setup_dashboard_ui();

    // WII: Main loop now checks for HOME button to exit
    while (1) {
        // Scan for controller input
        WPAD_ScanPads();
        u32 pressed = WPAD_ButtonsDown(0);

        // If HOME button is pressed, exit the loop
        if (pressed & WPAD_BUTTON_HOME) {
            break;
        }

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
        
        run_countdown_wii();
    }

    // Cleanup libcurl
    curl_global_cleanup();

    printf("\nExiting. Thanks for using WiiStockTicker!\n");
    VIDEO_WaitVSync();
    
    return 0;
}

// --- Helper Functions (mostly unchanged) ---

static size_t write_callback(void *contents, size_t size, size_t nmemb, void *userp) {
    size_t realsize = size * nmemb;
    MemoryStruct *mem = (MemoryStruct *)userp;
    char *ptr = realloc(mem->memory, mem->size + realsize + 1);
    if (ptr == NULL) { return 0; }
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
        // WII: Add a timeout, as network can be slow/unreliable
        curl_easy_setopt(curl_handle, CURLOPT_TIMEOUT, 10L); 

        res = curl_easy_perform(curl_handle);
        if (res != CURLE_OK) {
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
    // This function remains IDENTICAL because the printf calls with ANSI
    // codes for cursor positioning and color are supported by libogc.
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
    
    const char *symbol = cJSON_GetObjectItemCaseSensitive(meta, "symbol")->valuestring;
    double price = cJSON_GetObjectItemCaseSensitive(meta, "regularMarketPrice")->valuedouble;
    double prev_close = cJSON_GetObjectItemCaseSensitive(meta, "chartPreviousClose")->valuedouble;
    
    double change = price - prev_close;
    double percent_change = (prev_close == 0) ? 0 : (change / prev_close) * 100.0;
    
    const char* color = (change >= 0) ? KGRN : KRED;
    char sign = (change >= 0) ? '+' : '-';

    printf("\x1b[%d;1H", row); // libogc understands this: move cursor to (row, 1)
    printf("%-10s | %s%10.2f%s | %s%c%9.2f%s | %s%c%10.2f%%%s\x1b[K\n",
           symbol,
           KBLU, price, KNRM,
           color, sign, (change >= 0 ? change : -change), KNRM,
           color, sign, (percent_change >= 0 ? percent_change : -percent_change), KNRM);
    
    cJSON_Delete(root);
}

void print_error_on_line(const char* ticker, const char* error_msg, int row) {
    // Also unchanged, works perfectly on the Wii console.
    printf("\x1b[%d;1H", row);
    printf("%-10s | %s%-40s%s\x1b[K\n", ticker, KRED, error_msg, KNRM);
}


// --- UI and Terminal Control Functions ---

// WII: New function to handle all Wii-specific system initialization
void init_wii_systems() {
    VIDEO_Init();
    WPAD_Init();
    
    // Set up the console.
    // This automatically selects a video mode and clears the screen.
    // The rmode object will be used to configure the framebuffer.
    static GXRModeObj *rmode = NULL;
    rmode = VIDEO_GetPreferredMode(NULL);
    CON_Init(rmode->fbWidth, 255, 255, 0, 0, rmode->fbWidth*2);
    VIDEO_Configure(rmode);
    VIDEO_SetNextFramebuffer(xfb);
    VIDEO_SetBlack(FALSE);
    VIDEO_Flush();
    VIDEO_WaitVSync();
    if (rmode->viTVMode & VI_NON_INTERLACE) VIDEO_WaitVSync();
}


void setup_dashboard_ui() {
    // WII: \x1b[2J (clear screen) is supported. \x1b[H (cursor home) is also supported.
    printf("\x1b[2J\x1b[H");

    printf("--- C Wii Stock Dashboard ---\n");
    printf("\n");
    printf("\n");
    printf("%-10s | %11s | %11s | %13s\n", "Ticker", "Price", "Change", "% Change");
    printf("-------------------------------------------------------------\n");
    
    for (int i = 0; i < num_tickers; i++) {
        printf("%-10s | %sFetching...%s\n", tickers[i], KYEL, KNRM);
    }
}

void update_timestamp() {
    // This function is platform-agnostic and works fine on Wii.
    time_t t = time(NULL);
    struct tm *tm = localtime(&t);
    char time_str[64];
    strftime(time_str, sizeof(time_str), "%Y-%m-%d %H:%M:%S", tm);

    printf("\x1b[2;1H"); // Move to row 2, col 1
    printf("Last updated: %s\x1b[K\n", time_str);
}

// WII: Re-implemented the countdown using VIDEO_WaitVSync()
void run_countdown_wii() {
    int update_line = DATA_START_ROW + num_tickers + 1;
    
    for (int i = UPDATE_INTERVAL_SECONDS; i > 0; i--) {
        // Check for HOME button press during the countdown to make the app more responsive
        WPAD_ScanPads();
        if (WPAD_ButtonsDown(0) & WPAD_BUTTON_HOME) return;

        printf("\x1b[%d;1H", update_line);
        printf("\x1b[KUpdating in %2d seconds... (Press HOME to exit)", i);
        
        // Wait for 1 second (approx. 60 frames for NTSC, 50 for PAL)
        for(int frame = 0; frame < 60; ++frame) {
            VIDEO_WaitVSync();
        }
    }
    printf("\x1b[%d;1H\x1b[KUpdating now...                                  ", update_line);
}
