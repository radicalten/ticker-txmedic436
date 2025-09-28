#include "stock.h"

// Constructor: initialize object, fetch and parse data
Stock::Stock(std::string symbol){
    m_symbol = symbol;
    m_url = GenerateURL(symbol);
    GetWebsiteData();

    m_current_price = ParsePrice(CURRENT_PRICE);
    m_open_price    = ParsePrice(OPEN_PRICE);
    m_high_price    = ParsePrice(HIGH_PRICE);
    m_low_price     = ParsePrice(LOW_PRICE);   // ✅ Fix: no longer using high for low
    m_volume        = static_cast<unsigned>(ParsePrice(VOLUME));
}

// Convert ticker symbol to uppercase on return
std::string Stock::GetSymbol(){
    std::transform(m_symbol.begin(), m_symbol.end(), m_symbol.begin(), ::toupper);
    return m_symbol;
}

// Build Yahoo Finance API URL
std::string Stock::GenerateURL(std::string symbol){
    std::string url = "https://query1.finance.yahoo.com/v8/finance/chart/";
    url.append(symbol + "?interval=1d");
    return url;
}

// libcurl callback: write incoming data into string
size_t Stock::Callback(void* buffer, size_t size, size_t num, void* out){
    const size_t totalBytes( size * num );
    ((std::string*)out)->append((char*)buffer, totalBytes);
    return totalBytes;
}

// Fetch website data into m_website_data
bool Stock::GetWebsiteData(){
    CURLcode res;
    char error[CURL_ERROR_SIZE];

    CURL *curl = curl_easy_init();   // Initialize libcurl
    if(!curl){
        std::cerr << "libcurl initialization failed\n";
        return false;
    }

    curl_easy_setopt(curl, CURLOPT_URL, GenerateURL(m_symbol).c_str());
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 10);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, Callback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &m_website_data);
    curl_easy_setopt(curl, CURLOPT_ERRORBUFFER, &error);
    error[0] = 0;

    res = curl_easy_perform(curl);   // Perform transfer
    if(res != CURLE_OK){
        std::cerr << "libcurl error fetching " << m_symbol 
                  << ": " << error << std::endl;
        curl_easy_cleanup(curl);
        return false;
    } else {
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &m_http_std_res_code);
        curl_easy_cleanup(curl);
        return true;
    }
}

// Parse a price (or volume) from the website data
float Stock::ParsePrice(const std::string &type) {
    char end_delimiter = ',';

    size_t start_pos = m_website_data.find(type);
    if (start_pos == std::string::npos) {
        std::cerr << "Parse error: could not find " << type
                  << " in response for " << m_symbol << "\n";
        return -1.0f;
    }
    start_pos += type.length();  // skip token

    size_t end_pos = m_website_data.find(end_delimiter, start_pos);
    if (end_pos == std::string::npos) {
        // fall back to end of string
        end_pos = m_website_data.size();
    }

    // Now compute substring length properly
    std::string token = m_website_data.substr(start_pos, end_pos - start_pos);

    // Clean array-style values: [123.45]
    if (!token.empty() && token.front() == '[' && token.back() == ']') {
        if (token.size() > 2) {
            token = token.substr(1, token.size() - 2);
        } else {
            std::cerr << "Malformed token for " << type << " in " << m_symbol << "\n";
            return -1.0f;
        }
    }

    try {
        return std::stof(token);
    } catch (const std::exception &e) {
        std::cerr << "Conversion error for " << type << " on " << m_symbol 
                  << ": token='" << token << "'\n";
        return -1.0f;
    }
}
