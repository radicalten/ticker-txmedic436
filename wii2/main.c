#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <curl/curl.h>
#include "cJSON.h"

// POSIX sleep/usleep
#include <unistd.h>

// Wii-specific init (guarded)
#if defined(GEKKO) && defined(HW_RVL)
#include <gccore.h>
#include <wiiuse/wpad.h>
//#include <network.h>

static void *xfb = NULL;
static GXRModeObj *rmode = NULL;

static void wii_video_init(void) {
    VIDEO_Init();
    WPAD_Init();

    rmode = VIDEO_GetPreferredMode(NULL);
    xfb = MEM_K0_TO_K1(SYS_AllocateFramebuffer(rmode));
    console_init(xfb, 20, 20, rmode->fbWidth, rmode->xfbHeight,
                 rmode->fbWidth * VI_DISPLAY_PIX_SZ);

    VIDEO_Configure(rmode);
    VIDEO_SetNextFramebuffer(xfb);
    VIDEO_SetBlack(FALSE);
    VIDEO_Flush();
    VIDEO_WaitVSync();
    if (rmode->viTVMode & VI_NON_INTERLACE) VIDEO_WaitVSync();

    // Clear and home
    printf("\x1b[2J\x1b[H");
}
#endif

// --- Configuration ---
#define UPDATE_INTERVAL_SECONDS 15
#define USER_AGENT "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/98.0.4758.102 Safari/537.36"
#define API_URL_FORMAT "https://query1.finance.yahoo.com/v8/finance/chart/%s"
#define DATA_START_ROW 6 // The row number where the first stock ticker will be printed

// Add or remove stock tickers here
const char *tickers[] = {"AAPL", "GOOGL", "TSLA", "MSFT", "NVDA", "BTC-USD", "ETH-USD"};
const int num_tickers = sizeof(tickers) / sizeof(tickers[0]);

// --- Color Definitions for Terminal (disabled on Wii) ---
#define KNRM  "\x1B[0m"
#define KRED  "\x1B[31m"
#define KGRN  "\x1B[32m"
#define KYEL  "\x1B[33m"
#define KBLU  "\x1B[34m"

#if defined(GEKKO) && defined(HW_RVL)
// Wii console doesn't render ANSI colors — make them no-ops
#undef KNRM
#undef KRED
#undef KGRN
#undef KYEL
#undef KBLU
#define KNRM  ""
#define KRED  ""
#define KGRN  ""
#define KYEL  ""
#define KBLU  ""
#endif

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

        // On Wii, there's no CA store by default — disable verification or bundle a CA set.
        // Don't do this in production for sensitive data.
        #if defined(GEKKO) && defined(HW_RVL)
        curl_easy_setopt(curl_handle, CURLOPT_SSL_VERIFYPEER, 0L);
        curl_easy_setopt(curl_handle, CURLOPT_SSL_VERIFYHOST, 0L);
        #endif

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
        char* err_desc = (char*)"Invalid ticker or no data";
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

    cJSON *sym = cJSON_GetObjectItemCaseSensitive(meta, "symbol");
    cJSON *rmp = cJSON_GetObjectItemCaseSensitive(meta, "regularMarketPrice");
    cJSON *cpc = cJSON_GetObjectItemCaseSensitive(meta, "chartPreviousClose");
    if (!cJSON_IsString(sym) || !cJSON_IsNumber(rmp) || !cJSON_IsNumber(cpc)) {
        print_error_on_line("JSON", "Missing fields", row);
        cJSON_Delete(root);
        return;
    }

    const char *symbol = sym->valuestring;
    double price = rmp->valuedouble;
    double prev_close = cpc->valuedouble;

    double change = price - prev_close;
    double percent_change = (prev_close == 0) ? 0 : (change / prev_close) * 100.0;

    const char* color = (change >= 0) ? KGRN : KRED;
    char sign = (change >= 0) ? '+' : '-';

    // Move to row N, col 1 (Wii console understands this escape)
    printf("\x1b[%d;1H", row);

    printf("%-10s | %s%10.2f%s | %s%c%9.2f%s | %s%c%10.2f%%%s\x1b[K\n",
           symbol,
           KBLU, price, KNRM,
           color, sign, (change >= 0 ? change : -change), KNRM,
           color, sign, (percent_change >= 0 ? percent_change : -percent_change), KNRM);

    cJSON_Delete(root);
}

void print_error_on_line(const char* ticker, const char* error_msg, int row) {
    printf("\x1b[%d;1H", row);
    printf("%-10s | %s%-40s%s\x1b[K\n", ticker, KRED, error_msg, KNRM);
}

void setup_dashboard_ui() {
    hide_cursor();
    // Clear screen and home
    printf("\x1b[2J\x1b[H");

    printf("--- C Terminal Stock Dashboard (Wii-friendly) ---\n");
    printf("\n"); // dynamic timestamp placeholder
    printf("\n");

    printf("%-10s | %11s | %11s | %13s\n", "Ticker", "Price", "Change", "% Change");
    printf("-------------------------------------------------------------\n");

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

    printf("\x1b[2;1H");
    printf("Last updated: %s\x1b[K\n", time_str);
    fflush(stdout);
}

void run_countdown() {
    int update_line = DATA_START_ROW + num_tickers + 1;

    for (int i = UPDATE_INTERVAL_SECONDS; i > 0; i--) {
        printf("\x1b[%d;1H", update_line);
        printf("\x1b[KUpdating in %2d seconds...", i);
        fflush(stdout);
        sleep(1);

        // Allow Home button exit on Wii during countdown
        #if defined(GEKKO) && defined(HW_RVL)
        WPAD_ScanPads();
        if (WPAD_ButtonsDown(0) & WPAD_BUTTON_HOME) {
            printf("\x1b[%d;1H\x1b[KExiting...\n", update_line);
            fflush(stdout);
            exit(0);
        }
        #endif
    }
    printf("\x1b[%d;1H\x1b[KUpdating now...           ", update_line);
    fflush(stdout);
}

#if defined(GEKKO) && defined(HW_RVL)
// On Wii console, these don't do anything
void hide_cursor() {}
void show_cursor() {}
#else
void hide_cursor() {
    printf("\x1b[?25l");
    fflush(stdout);
}
void show_cursor() {
    printf("\x1b[?25h");
    fflush(stdout);
}
#endif

void cleanup_on_exit() {
    show_cursor();
}

// --- Main Application ---
int main(int argc, char** argv) {
    atexit(cleanup_on_exit);

    // Wii video & network init
    #if defined(GEKKO) && defined(HW_RVL)
    wii_video_init();
    if (wii_network_init() != 0) {
        printf("Network init failed (DHCP). Check Wi-Fi, try again.\n");
        printf("Press HOME to exit.\n");
        while (1) {
            WPAD_ScanPads();
            if (WPAD_ButtonsDown(0) & WPAD_BUTTON_HOME) return 0;
            VIDEO_WaitVSync();
        }
    }
    #endif

    curl_global_init(CURL_GLOBAL_ALL);

    setup_dashboard_ui();

    while (1) {
        // Wii: allow exit with HOME
        #if defined(GEKKO) && defined(HW_RVL)
        WPAD_ScanPads();
        if (WPAD_ButtonsDown(0) & WPAD_BUTTON_HOME) break;
        #endif

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
