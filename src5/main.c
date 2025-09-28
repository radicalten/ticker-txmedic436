/******************************************************************************
 * 
 *  stock_dashboard.c
 * 
 *  A simple, single-file C program to display a live stock price dashboard
 *  in the terminal using the Yahoo Finance API.
 * 
 *  AUTHOR:
 *      An AI Assistant
 * 
 *  HOW TO COMPILE:
 *      You need to have libcurl installed.
 *      gcc -o stock_dashboard stock_dashboard.c -lcurl
 * 
 *  HOW TO RUN:
 *      ./stock_dashboard
 * 
 *  FEATURES:
 *      - Fetches data for multiple stocks defined in the `TICKERS` array.
 *      - Displays price, change, and percent change.
 *      - Uses color to indicate positive (green) or negative (red) change.
 *      - Refreshes automatically every `REFRESH_SECONDS`.
 *      - Self-contained: Includes the cJSON library for parsing.
 * 
 ******************************************************************************/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h> // For sleep()

// Use libcurl for HTTP requests
#include <curl/curl.h>

// =================================================================================
// == START: EMBEDDED cJSON LIBRARY (v1.7.15) ======================================
// == To fulfill the "single file" requirement, the cJSON source is included     ==
// == directly. You do not need to install it separately.                      ==
// =================================================================================

/*
  Copyright (c) 2009-2017 Dave Gamble and cJSON contributors

  Permission is hereby granted, free of charge, to any person obtaining a copy
  of this software and associated documentation files (the "Software"), to deal
  in the Software without restriction, including without limitation the rights
  to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
  copies of the Software, and to permit persons to whom the Software is
  furnished to do so, subject to the following conditions:

  The above copyright notice and this permission notice shall be included in
  all copies or substantial portions of the Software.

  THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
  IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
  FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
  AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
  LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
  OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
  THE SOFTWARE.
*/

/* cJSON */
/* JSON parser in C. */

#include <ctype.h>
#include <limits.h>
#include <math.h>

#define CJSON_VERSION_MAJOR 1
#define CJSON_VERSION_MINOR 7
#define CJSON_VERSION_PATCH 15

/* define cJSON_bool */
#ifndef cJSON_bool
#define cJSON_bool int
#endif

/* cJSON Types: */
#define cJSON_Invalid (0)
#define cJSON_False (1 << 0)
#define cJSON_True (1 << 1)
#define cJSON_NULL (1 << 2)
#define cJSON_Number (1 << 3)
#define cJSON_String (1 << 4)
#define cJSON_Array (1 << 5)
#define cJSON_Object (1 << 6)
#define cJSON_Raw (1 << 7) /* raw json */

#define cJSON_IsReference 256
#define cJSON_StringIsConst 512

typedef struct cJSON {
    struct cJSON *next;
    struct cJSON *prev;
    struct cJSON *child;
    int type;
    char *valuestring;
    int valueint;
    double valuedouble;
    char *string;
} cJSON;

typedef struct cJSON_Hooks {
    void *(*malloc_fn)(size_t sz);
    void (*free_fn)(void *ptr);
} cJSON_Hooks;

/* declarations for cJSON functions */
cJSON *cJSON_Parse(const char *value);
void cJSON_Delete(cJSON *item);
cJSON *cJSON_GetObjectItemCaseSensitive(const cJSON * const object, const char * const string);
double cJSON_GetNumberValue(const cJSON * const item);
cJSON * cJSON_GetArrayItem(const cJSON *array, int index);

/* implementation of cJSON functions */
static const char *global_ep = NULL;

static void *(*cJSON_malloc)(size_t sz) = malloc;
static void (*cJSON_free)(void *ptr) = free;

static unsigned char cJSON_tolower(unsigned char c) {
    return (c >= 'A' && c <= 'Z') ? (c + 'a' - 'A') : c;
}

static int cJSON_strcasecmp(const char *s1, const char *s2) {
    if (!s1) return (s1 == s2) ? 0 : 1;
    if (!s2) return 1;
    for (; cJSON_tolower(*s1) == cJSON_tolower(*s2); ++s1, ++s2) {
        if (*s1 == 0) return 0;
    }
    return cJSON_tolower(*s1) - cJSON_tolower(*s2);
}

static char* cJSON_strdup(const char* str) {
    size_t len;
    char* copy;
    len = strlen(str) + 1;
    if (!(copy = (char*)cJSON_malloc(len))) return 0;
    memcpy(copy, str, len);
    return copy;
}

cJSON *cJSON_New_Item(void) {
    cJSON* node = (cJSON*)cJSON_malloc(sizeof(cJSON));
    if (node) memset(node, 0, sizeof(cJSON));
    return node;
}

void cJSON_Delete(cJSON *c) {
    cJSON *next;
    while (c) {
        next = c->next;
        if (!(c->type & cJSON_IsReference) && c->child) cJSON_Delete(c->child);
        if (!(c->type & cJSON_IsReference) && c->valuestring) cJSON_free(c->valuestring);
        if (!(c->type & cJSON_StringIsConst) && c->string) cJSON_free(c->string);
        cJSON_free(c);
        c = next;
    }
}

static const char *skip(const char *in) {
    if (in == NULL || *in == '\0') {
        return NULL;
    }
    while (*in && (unsigned char)*in <= 32) {
        in++;
    }
    return in;
}

static const char *parse_value(cJSON *item, const char *value);
static const char *parse_number(cJSON *item, const char *num);
static const char *parse_string(cJSON *item, const char *str);
static const char *parse_array(cJSON *item, const char *value);
static const char *parse_object(cJSON *item, const char *value);

cJSON *cJSON_Parse(const char *value) {
    const char *end = NULL;
    cJSON *c = cJSON_New_Item();
    if (!c) return NULL;

    end = parse_value(c, skip(value));
    if (!end) {
        cJSON_Delete(c);
        return NULL;
    }

    return c;
}

static const char *parse_value(cJSON *item, const char *value) {
    if (!value) return NULL;
    if (!strncmp(value, "null", 4)) { item->type = cJSON_NULL; return value + 4; }
    if (!strncmp(value, "false", 5)) { item->type = cJSON_False; item->valueint = 0; return value + 5; }
    if (!strncmp(value, "true", 4)) { item->type = cJSON_True; item->valueint = 1; return value + 4; }
    if (*value == '\"') { return parse_string(item, value); }
    if (*value == '-' || (*value >= '0' && *value <= '9')) { return parse_number(item, value); }
    if (*value == '[') { return parse_array(item, value); }
    if (*value == '{') { return parse_object(item, value); }

    global_ep = value;
    return NULL;
}

static const char *parse_number(cJSON *item, const char *num) {
    double n = 0, sign = 1, scale = 0;
    int subscale = 0, signsubscale = 1;

    if (*num == '-') sign = -1, num++;
    if (*num == '0') num++;
    if (*num >= '1' && *num <= '9') do n = (n * 10.0) + (*num++ - '0'); while (*num >= '0' && *num <= '9');
    if (*num == '.' && num[1] >= '0' && num[1] <= '9') {
        num++;
        do {
            n = (n * 10.0) + (*num++ - '0');
            scale--;
        } while (*num >= '0' && *num <= '9');
    }
    if (*num == 'e' || *num == 'E') {
        num++;
        if (*num == '+') num++;
        else if (*num == '-') signsubscale = -1, num++;
        while (*num >= '0' && *num <= '9') subscale = (subscale * 10) + (*num++ - '0');
    }

    n = sign * n * pow(10.0, (scale + subscale * signsubscale));
    item->valuedouble = n;
    item->valueint = (int)n;
    item->type = cJSON_Number;
    return num;
}

static int pow2gt(int x) { --x; x|=x>>1; x|=x>>2; x|=x>>4; x|=x>>8; x|=x>>16; return x+1; }

static char *cJSON_strncasecmp_init(const char *str, size_t len) {
    char *out = (char*)cJSON_malloc(len + 1);
    if (out) {
        memcpy(out, str, len);
        out[len] = '\0';
        for (char *p = out; *p; p++) *p = cJSON_tolower(*p);
    }
    return out;
}

static const unsigned char firstByteMark[7] = { 0x00, 0x00, 0xC0, 0xE0, 0xF0, 0xF8, 0xFC };

static const char *parse_string(cJSON *item, const char *str) {
    const char *ptr = str + 1;
    char *ptr2;
    char *out;
    int len = 0;
    unsigned uc, uc2;
    if (*str != '\"') { global_ep = str; return NULL; }

    while (*ptr != '\"' && *ptr && ++len) {
        if (*ptr++ == '\\') ptr++;
    }

    out = (char*)cJSON_malloc(len + 1);
    if (!out) return NULL;

    ptr = str + 1;
    ptr2 = out;
    while (*ptr != '\"' && *ptr) {
        if (*ptr != '\\') *ptr2++ = *ptr++;
        else {
            ptr++;
            switch (*ptr) {
                case 'b': *ptr2++ = '\b'; break;
                case 'f': *ptr2++ = '\f'; break;
                case 'n': *ptr2++ = '\n'; break;
                case 'r': *ptr2++ = '\r'; break;
                case 't': *ptr2++ = '\t'; break;
                case 'u':
                    sscanf(ptr + 1, "%4x", &uc);
                    ptr += 4;
                    if ((uc >= 0xDC00 && uc <= 0xDFFF) || uc == 0) break;

                    if (uc >= 0xD800 && uc <= 0xDBFF) {
                        if (ptr[1] != '\\' || ptr[2] != 'u') break;
                        sscanf(ptr + 3, "%4x", &uc2);
                        ptr += 6;
                        if (uc2 < 0xDC00 || uc2 > 0xDFFF) break;
                        uc = 0x10000 + (((uc & 0x3FF) << 10) | (uc2 & 0x3FF));
                    }

                    len = 4;
                    if (uc < 0x80) len = 1;
                    else if (uc < 0x800) len = 2;
                    else if (uc < 0x10000) len = 3;
                    ptr2 += len;

                    switch (len) {
                        case 4: *--ptr2 = ((uc | 0x80) & 0xBF); uc >>= 6;
                        case 3: *--ptr2 = ((uc | 0x80) & 0xBF); uc >>= 6;
                        case 2: *--ptr2 = ((uc | 0x80) & 0xBF); uc >>= 6;
                        case 1: *--ptr2 = (uc | firstByteMark[len]);
                    }
                    ptr2 += len;
                    break;
                default: *ptr2++ = *ptr; break;
            }
            ptr++;
        }
    }
    *ptr2 = 0;
    if (*ptr == '\"') ptr++;
    item->valuestring = out;
    item->type = cJSON_String;
    return ptr;
}

static const char *parse_array(cJSON *item, const char *value) {
    cJSON *child;
    if (*value != '[') { global_ep = value; return NULL; }

    item->type = cJSON_Array;
    value = skip(value + 1);
    if (*value == ']') return value + 1;

    item->child = child = cJSON_New_Item();
    if (!item->child) return NULL;
    value = skip(parse_value(child, skip(value)));
    if (!value) return NULL;

    while (*value == ',') {
        cJSON *new_item;
        if (!(new_item = cJSON_New_Item())) return NULL;
        child->next = new_item;
        new_item->prev = child;
        child = new_item;
        value = skip(parse_value(child, skip(value + 1)));
        if (!value) return NULL;
    }

    if (*value == ']') return value + 1;
    global_ep = value;
    return NULL;
}

static const char *parse_object(cJSON *item, const char *value) {
    cJSON *child;
    if (*value != '{') { global_ep = value; return NULL; }

    item->type = cJSON_Object;
    value = skip(value + 1);
    if (*value == '}') return value + 1;

    item->child = child = cJSON_New_Item();
    if (!item->child) return NULL;
    value = skip(parse_string(child, skip(value)));
    if (!value) return NULL;
    child->string = child->valuestring;
    child->valuestring = 0;
    if (*value != ':') { global_ep = value; return NULL; }
    value = skip(parse_value(child, skip(value + 1)));
    if (!value) return NULL;

    while (*value == ',') {
        cJSON *new_item;
        if (!(new_item = cJSON_New_Item())) return NULL;
        child->next = new_item;
        new_item->prev = child;
        child = new_item;
        value = skip(parse_string(child, skip(value + 1)));
        if (!value) return NULL;
        child->string = child->valuestring;
        child->valuestring = 0;
        if (*value != ':') { global_ep = value; return NULL; }
        value = skip(parse_value(child, skip(value + 1)));
        if (!value) return NULL;
    }

    if (*value == '}') return value + 1;
    global_ep = value;
    return NULL;
}

cJSON *cJSON_GetObjectItemCaseSensitive(const cJSON * const object, const char * const string) {
    cJSON *current_element = NULL;
    if (!object || !string) return NULL;
    current_element = object->child;
    while (current_element && cJSON_strcasecmp(current_element->string, string) != 0) {
        current_element = current_element->next;
    }
    return current_element;
}

double cJSON_GetNumberValue(const cJSON * const item) {
    if (!item || (item->type != cJSON_Number)) return 0.0;
    return item->valuedouble;
}

cJSON * cJSON_GetArrayItem(const cJSON *array, int index) {
    cJSON *c = array ? array->child : 0;
    while (c && index > 0) {
        index--;
        c = c->next;
    }
    return c;
}

// =================================================================================
// == END: EMBEDDED cJSON LIBRARY ==================================================
// =================================================================================


// --- DASHBOARD CONFIGURATION ---
const char* TICKERS[] = {"AAPL", "GOOGL", "TSLA", "MSFT", "NVDA", "BTC-USD", "ETH-USD", NULL};
const int REFRESH_SECONDS = 30;
// --- END CONFIGURATION ---


// --- ANSI COLOR CODES ---
#define COLOR_RESET   "\x1B[0m"
#define COLOR_RED     "\x1B[31m"
#define COLOR_GREEN   "\x1B[32m"
#define COLOR_YELLOW  "\x1B[33m"
#define COLOR_BLUE    "\x1B[34m"
#define COLOR_CYAN    "\x1B[36m"
#define BOLD          "\x1B[1m"


// Structure to hold the data received from curl
struct MemoryStruct {
  char *memory;
  size_t size;
};

// Callback function for curl to write received data into our MemoryStruct
static size_t
WriteMemoryCallback(void *contents, size_t size, size_t nmemb, void *userp) {
    size_t realsize = size * nmemb;
    struct MemoryStruct *mem = (struct MemoryStruct *)userp;

    char *ptr = realloc(mem->memory, mem->size + realsize + 1);
    if(ptr == NULL) {
        printf("Out of memory!\n");
        return 0;
    }

    mem->memory = ptr;
    memcpy(&(mem->memory[mem->size]), contents, realsize);
    mem->size += realsize;
    mem->memory[mem->size] = 0;

    return realsize;
}

// Function to fetch data from a URL using libcurl
char* fetch_url(const char* url) {
    CURL *curl_handle;
    CURLcode res;

    struct MemoryStruct chunk;
    chunk.memory = malloc(1);
    chunk.size = 0;

    curl_global_init(CURL_GLOBAL_ALL);
    curl_handle = curl_easy_init();

    if(curl_handle) {
        curl_easy_setopt(curl_handle, CURLOPT_URL, url);
        curl_easy_setopt(curl_handle, CURLOPT_WRITEFUNCTION, WriteMemoryCallback);
        curl_easy_setopt(curl_handle, CURLOPT_WRITEDATA, (void *)&chunk);
        curl_easy_setopt(curl_handle, CURLOPT_USERAGENT, "libcurl-agent/1.0");
        curl_easy_setopt(curl_handle, CURLOPT_TIMEOUT, 10L); // 10 second timeout

        res = curl_easy_perform(curl_handle);

        if(res != CURLE_OK) {
            fprintf(stderr, "curl_easy_perform() failed: %s\n", curl_easy_strerror(res));
            free(chunk.memory);
            return NULL;
        }

        curl_easy_cleanup(curl_handle);
    }
    
    curl_global_cleanup();
    return chunk.memory;
}


// Function to parse JSON and display stock information
void parse_and_display_stock(const char* json_string) {
    cJSON *root = cJSON_Parse(json_string);
    if (root == NULL) {
        fprintf(stderr, "Error parsing JSON.\n");
        return;
    }

    cJSON *quoteResponse = cJSON_GetObjectItemCaseSensitive(root, "quoteResponse");
    cJSON *resultArray = cJSON_GetObjectItemCaseSensitive(quoteResponse, "result");
    
    if (!cJSON_IsArray(resultArray) || cJSON_GetArrayItem(resultArray, 0) == NULL) {
        printf("Ticker not found or API error.\n");
        cJSON_Delete(root);
        return;
    }

    cJSON *stock_data = cJSON_GetArrayItem(resultArray, 0);

    const cJSON *symbol = cJSON_GetObjectItemCaseSensitive(stock_data, "symbol");
    const cJSON *price = cJSON_GetObjectItemCaseSensitive(stock_data, "regularMarketPrice");
    const cJSON *change = cJSON_GetObjectItemCaseSensitive(stock_data, "regularMarketChange");
    const cJSON *changePercent = cJSON_GetObjectItemCaseSensitive(stock_data, "regularMarketChangePercent");

    if (symbol && price && change && changePercent) {
        double change_val = cJSON_GetNumberValue(change);
        const char* color = (change_val >= 0) ? COLOR_GREEN : COLOR_RED;
        char sign = (change_val >= 0) ? '+' : '-';

        printf("%-10s | %s%12.2f%s | %s%c%8.2f (%c%.2f%%)%s\n",
            symbol->valuestring,
            BOLD, price->valuedouble, COLOR_RESET,
            color, sign, fabs(change->valuedouble), sign, fabs(changePercent->valuedouble), COLOR_RESET
        );
    } else {
        printf("Could not retrieve all data fields for a ticker.\n");
    }

    cJSON_Delete(root);
}


void print_header() {
    time_t now;
    time(&now);
    char time_str[100];
    strftime(time_str, sizeof(time_str), "%Y-%m-%d %H:%M:%S", localtime(&now));

    // Clear screen using ANSI escape codes
    printf("\033[2J\033[H"); 

    printf(BOLD COLOR_CYAN "Simple Stock Dashboard\n" COLOR_RESET);
    printf("Last Updated: %s\n", time_str);
    printf("=========================================================\n");
    printf(BOLD "%-10s | %12s | %-20s\n" COLOR_RESET, "TICKER", "PRICE (USD)", "CHANGE");
    printf("---------------------------------------------------------\n");
}


int main(void) {
    char url_buffer[256];

    while (1) {
        print_header();
        
        int i = 0;
        while (TICKERS[i] != NULL) {
            // Construct the URL for the current ticker
            snprintf(url_buffer, sizeof(url_buffer), "https://query1.finance.yahoo.com/v7/finance/quote?symbols=%s", TICKERS[i]);

            char* json_response = fetch_url(url_buffer);

            if (json_response) {
                parse_and_display_stock(json_response);
                free(json_response);
            } else {
                printf("%-10s | %sFailed to fetch data%s\n", TICKERS[i], COLOR_RED, COLOR_RESET);
            }
            i++;
        }
        
        printf("=========================================================\n");
        printf("Refreshing in %d seconds...\n", REFRESH_SECONDS);
        fflush(stdout); // Ensure all output is printed before sleeping
        sleep(REFRESH_SECONDS);
    }

    return 0;
}
