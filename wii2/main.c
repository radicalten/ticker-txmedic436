#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <gccore.h>
#include <wiiuse/wpad.h>
#include <network.h>
#include <curl/curl.h>
#include "cJSON.h"

// --- Wii Video Configuration ---
static void *xfb = NULL;
static GXRModeObj *rmode = NULL;

// --- Configuration ---
#define UPDATE_INTERVAL_SECONDS 15
#define USER_AGENT "WiiStockDashboard/1.0"
#define API_URL_FORMAT "https://query1.finance.yahoo.com/v8/finance/chart/%s"
#define DATA_START_ROW 6
#define MAX_WIDTH 80  // Wii terminal width

// Stock tickers
const char *tickers[] = {"AAPL", "GOOGL", "TSLA", "MSFT", "NVDA", "BTC-USD", "ETH-USD"};
const int num_tickers = sizeof(tickers) / sizeof(tickers[0]);

// --- Simple Color System for Wii ---
// Wii terminal has limited ANSI support, so we'll use simpler approach
#define CLEAR_SCREEN "\x1b[2J\x1b[H"
#define MOVE_CURSOR(row, col) printf("\x1b[%d;%dH", row, col)

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
void init_wii();
void deinit_wii();
int init_network();
void clear_line(int row);
void print_centered(const char* text, int row);

// --- Main Application ---
int main(int argc, char **argv) {
    // Initialize Wii subsystems
    init_wii();
    
    printf("\n\n");
    print_centered("Wii Stock Dashboard", 2);
    print_centered("Initializing network...", 4);
    
    // Initialize network
    if (init_network() < 0) {
        print_centered("Network initialization failed!", 6);
        print_centered("Press HOME to exit", 8);
        
        while(1) {
            WPAD_ScanPads();
            u32 pressed = WPAD_ButtonsDown(0);
            if (pressed & WPAD_BUTTON_HOME) break;
            VIDEO_WaitVSync();
        }
        deinit_wii();
        return -1;
    }
    
    // Initialize libcurl
    curl_global_init(CURL_GLOBAL_ALL);
    
    // Setup dashboard
    setup_dashboard_ui();
    
    // Main loop
    while(1) {
        // Check for HOME button to exit
        WPAD_ScanPads();
        u32 pressed = WPAD_ButtonsDown(0);
        if (pressed & WPAD_BUTTON_HOME) break;
        
        // Update timestamp
        update_timestamp();
        
        // Fetch and display stock data
        char url[256];
        for (int i = 0; i < num_tickers; i++) {
            int current_row = DATA_START_ROW + i;
            
            snprintf(url, sizeof(url), API_URL_FORMAT, tickers[i]);
            char *json_response = fetch_url(url);
            
            if (json_response) {
                parse_and_print_stock_data(json_response, current_row);
                free(json_response);
            } else {
                print_error_on_line(tickers[i], "Failed to fetch", current_row);
            }
            
            // Allow interruption during fetching
            WPAD_ScanPads();
            if (WPAD_ButtonsDown(0) & WPAD_BUTTON_HOME) goto exit_loop;
        }
        
        // Countdown with ability to skip
        run_countdown();
    }
    
exit_loop:
    // Cleanup
    curl_global_cleanup();
    deinit_wii();
    return 0;
}

// --- Wii Initialization Functions ---

void init_wii() {
    // Initialize video
    VIDEO_Init();
    WPAD_Init();
    
    // Get preferred video mode
    rmode = VIDEO_GetPreferredMode(NULL);
    
    // Allocate framebuffer
    xfb = MEM_K0_TO_K1(SYS_AllocateFramebuffer(rmode));
    
    // Configure video
    console_init(xfb, 20, 20, rmode->fbWidth, rmode->xfbHeight, rmode->fbWidth * VI_DISPLAY_PIX_SZ);
    
    VIDEO_Configure(rmode);
    VIDEO_SetNextFramebuffer(xfb);
    VIDEO_SetBlack(FALSE);
    VIDEO_Flush();
    VIDEO_WaitVSync();
    if(rmode->viTVMode & VI_NON_INTERLACE) VIDEO_WaitVSync();
    
    // Clear screen
    printf(CLEAR_SCREEN);
}

void deinit_wii() {
    // Show exit message
    printf(CLEAR_SCREEN);
    print_centered("Exiting...", 12);
    sleep(1);
    
    // Deinitialize subsystems
    WPAD_Shutdown();
    
    // Return to loader
    exit(0);
}

int init_network() {
    s32 ret;
    
    // Initialize network
    ret = net_init();
    if (ret < 0) {
        printf("net_init failed: %d\n", ret);
        return -1;
    }
    
    // Small delay to ensure network is ready
    sleep(1);
    return 0;
}

// --- Helper Functions ---

static size_t write_callback(void *contents, size_t size, size_t nmemb, void *userp) {
    size_t realsize = size * nmemb;
    MemoryStruct *mem = (MemoryStruct *)userp;
    
    char *ptr = realloc(mem->memory, mem->size + realsize + 1);
    if (ptr == NULL) {
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
        curl_easy_setopt(curl_handle, CURLOPT_TIMEOUT, 10L); // 10 second timeout
        curl_easy_setopt(curl_handle, CURLOPT_SSL_VERIFYPEER, 0L); // Wii SSL can be problematic
        
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
    cJSON *root = cJSON_Parse(json_string);
    if (root == NULL) {
        print_error_on_line("JSON", "Parse Error", row);
        return;
    }
    
    cJSON *chart = cJSON_GetObjectItemCaseSensitive(root, "chart");
    cJSON *result_array = cJSON_GetObjectItemCaseSensitive(chart, "result");
    if (!cJSON_IsArray(result_array) || cJSON_GetArraySize(result_array) == 0) {
        print_error_on_line("API", "No data", row);
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
    
    char sign = (change >= 0) ? '+' : ' ';
    
    // Clear line and print data
    clear_line(row);
    MOVE_CURSOR(row, 1);
    
    // Simplified output for Wii terminal
    printf("%-8s  $%-8.2f  %c%-7.2f  %c%-6.2f%%",
           symbol,
           price,
           sign, (change >= 0 ? change : -change),
           sign, (percent_change >= 0 ? percent_change : -percent_change));
    
    cJSON_Delete(root);
}

void print_error_on_line(const char* ticker, const char* error_msg, int row) {
    clear_line(row);
    MOVE_CURSOR(row, 1);
    printf("%-8s  %s", ticker, error_msg);
}

void clear_line(int row) {
    MOVE_CURSOR(row, 1);
    printf("\x1b[K"); // Clear to end of line
}

void print_centered(const char* text, int row) {
    int len = strlen(text);
    int col = (MAX_WIDTH - len) / 2;
    if (col < 1) col = 1;
    
    MOVE_CURSOR(row, col);
    printf("%s", text);
}

void setup_dashboard_ui() {
    printf(CLEAR_SCREEN);
    
    print_centered("=== Wii Stock Dashboard ===", 1);
    printf("\n\n");
    
    MOVE_CURSOR(4, 1);
    printf("Ticker    Price      Change    Percent");
    MOVE_CURSOR(5, 1);
    printf("----------------------------------------");
    
    for (int i = 0; i < num_tickers; i++) {
        MOVE_CURSOR(DATA_START_ROW + i, 1);
        printf("%-8s  Loading...", tickers[i]);
    }
    
    MOVE_CURSOR(DATA_START_ROW + num_tickers + 2, 1);
    printf("Press HOME button to exit");
}

void update_timestamp() {
    time_t t = time(NULL);
    struct tm *tm = localtime(&t);
    char time_str[32];
    strftime(time_str, sizeof(time_str), "%H:%M:%S", tm);
    
    MOVE_CURSOR(3, 1);
    printf("Updated: %s", time_str);
}

void run_countdown() {
    int update_line = DATA_START_ROW + num_tickers + 1;
    
    for (int i = UPDATE_INTERVAL_SECONDS; i > 0; i--) {
        // Check for HOME button during countdown
        WPAD_ScanPads();
        if (WPAD_ButtonsDown(0) & WPAD_BUTTON_HOME) return;
        
        clear_line(update_line);
        MOVE_CURSOR(update_line, 1);
        printf("Next update in %d seconds... (A to skip)", i);
        
        // Allow skipping countdown with A button
        for (int j = 0; j < 10; j++) {
            WPAD_ScanPads();
            u32 pressed = WPAD_ButtonsDown(0);
            if (pressed & WPAD_BUTTON_A) return;
            if (pressed & WPAD_BUTTON_HOME) return;
            usleep(100000); // 100ms
        }
    }
    
    clear_line(update_line);
    MOVE_CURSOR(update_line, 1);
    printf("Updating now...");
}
