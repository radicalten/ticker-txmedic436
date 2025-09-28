#include "stock.h"
#include <stdexcept> // Required for std::exception
#include <thread> // added
#include <chrono> // added
#include <cmath> // added

// Constructor: Fetches and parses data for a given stock symbol
Stock::Stock(std::string symbol){
	m_symbol = symbol;
	m_url = GenerateURL(symbol);

	// ==================== ADD THIS DEBUGGING BLOCK ====================
	bool success = GetWebsiteData(); // Store the return value
	std::cout << "\n--- DEBUG INFO FOR SYMBOL: " << m_symbol << " ---\n";
	std::cout << "URL -> " << m_url << "\n";
	std::cout << "HTTP Response Code -> " << m_http_std_res_code << "\n";
	std::cout << "--- Raw Response Data ---\n";
	std::cout << m_website_data << "\n";
	std::cout << "------------------------------------------\n\n";
	// =================================================================
	
	// Attempt to get data and check if the HTTP request was successful (code 200 OK)
	if (!GetWebsiteData() || m_http_std_res_code != 200) {
		// If data fetch fails, set member variables to an error state and stop.
		// This prevents the program from trying to parse invalid/empty data.
		m_current_price = -1.0;
		m_open_price = -1.0;
		m_high_price = -1.0;
		m_low_price = -1.0;
		m_volume = 0;
		return; // IMPORTANT: Stop processing for this invalid stock
	}

	// --- Only if the data is valid, proceed to parse ---
	m_current_price = ParseValue(CURRENT_PRICE);
	m_open_price = ParseValue(OPEN_PRICE);
	m_high_price = ParseValue(HIGH_PRICE);
	m_low_price = ParseValue(LOW_PRICE);
	m_volume = static_cast<unsigned>(ParseValue(VOLUME)); // Volume is an integer
}

// Returns the symbol in uppercase
std::string Stock::GetSymbol(){
	std::string upper_symbol = m_symbol;
	std::transform(upper_symbol.begin(), upper_symbol.end(), upper_symbol.begin(), ::toupper);
	return upper_symbol;
}

// Generates the Yahoo Finance API URL for the given symbol
std::string Stock::GenerateURL(std::string symbol){
	std::string url = "https://query1.finance.yahoo.com/v8/finance/chart/";
	url.append(symbol + "?interval=1d");
	return url;
}

// libcurl callback function to write received data into a std::string
size_t Stock::Callback(void* buffer, size_t size, size_t num, void* out){
	const size_t totalBytes( size * num );
	((std::string*)out)->append((char*)buffer, totalBytes);
	return totalBytes;
}

// Fetches website data using libcurl with retry logic for rate limiting
bool Stock::GetWebsiteData(){
	CURLcode res;
	char error[CURL_ERROR_SIZE];
	
	const int MAX_RETRIES = 4; // Try up to 4 times
	long initial_backoff_ms = 500; // Start with a 500ms wait

	for (int i = 0; i < MAX_RETRIES; ++i) {
		m_website_data.clear(); // IMPORTANT: Clear previous (failed) response data
		m_http_std_res_code = 0;  // Reset response code

		CURL *curl = curl_easy_init();
		if (!curl) {
			std::cerr << "libcurl failed to initialize" << std::endl;
			return false;
		}

		curl_easy_setopt(curl, CURLOPT_URL, m_url.c_str());
		curl_easy_setopt(curl, CURLOPT_TIMEOUT, 10L);
		curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, Callback);
		curl_easy_setopt(curl, CURLOPT_WRITEDATA, &m_website_data);
		curl_easy_setopt(curl, CURLOPT_ERRORBUFFER, &error);
		error[0] = 0;

		res = curl_easy_perform(curl);
		
		if(res == CURLE_OK){
			curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &m_http_std_res_code);
			curl_easy_cleanup(curl);

			if (m_http_std_res_code == 200) {
				return true; // Success! Exit the function.
			}
			else if (m_http_std_res_code == 429) {
				// Too Many Requests, let's wait and retry
				std::cerr << "Warning: HTTP 429 (Too Many Requests) for " << m_symbol 
						  << ". Retrying in " << initial_backoff_ms << "ms..." << std::endl;
				
				std::this_thread::sleep_for(std::chrono::milliseconds(initial_backoff_ms));
				
				// Exponential backoff: double the wait time for the next potential retry
				initial_backoff_ms *= 2; 
			}
			else {
				// Another HTTP error (e.g., 404 Not Found), no point in retrying
				std::cerr << "HTTP Error " << m_http_std_res_code << " for " << m_symbol << ". Not retrying." << std::endl;
				return false;
			}
		}
		else {
			// libcurl level error (e.g., could not connect), no point in retrying
			std::cerr << "libcurl error for symbol " << m_symbol << ": " << error << std::endl;
			curl_easy_cleanup(curl);
			return false; 
		}
	} // end for loop

	// If we exit the loop, all retries have failed
	std::cerr << "Error: All retries failed for symbol " << m_symbol << "." << std::endl;
	return false;
}

// Robustly parses a numeric value from the fetched JSON data based on a key
// Renamed from ParsePrice to ParseValue to be more generic
float Stock::ParseValue(const std::string& key){
	try {
		// 1. Find the key (e.g., "regularMarketPrice:")
		size_t start_pos = m_website_data.find(key);

		// 2. === CHECK IF THE KEY WAS FOUND! ===
		if (start_pos == std::string::npos) {
			return -1.0f; // Return error code if key doesn't exist
		}

		// 3. Find the start of the number (right after the key)
		start_pos += key.length();

		// 4. Find the end of the number (usually a comma)
		size_t end_pos = m_website_data.find(',', start_pos);
		
		// If no comma is found, it might be the end of an object, look for '}'
		if (end_pos == std::string::npos) {
			end_pos = m_website_data.find('}', start_pos);
		}

		// If we still can't find the end, the data is malformed
		if (end_pos == std::string::npos) {
			return -1.0f;
		}

		// 5. Extract the value as a string, using the correct LENGTH argument
		std::string token = m_website_data.substr(start_pos, end_pos - start_pos);

		// 6. Handle array values like "[10000]" which can occur for volume
		if (!token.empty() && token.front() == '[') {
			token.erase(0, 1); // Remove '['
			if (!token.empty() && token.back() == ']') {
				 token.pop_back(); // Remove ']'
			}
		}
		
		// Handle "null" values which the API sometimes returns for certain fields
		if (token == "null") {
			return -1.0f;
		}

		// 7. Convert the clean string to a double (using stod is better for precision)
		return std::stod(token);

	} catch (const std::exception& e) {
		// Catch ANY standard exception (out_of_range, invalid_argument) during parsing
		std::cerr << "Caught exception while parsing for key '" << key << "': " << e.what() << std::endl;
		return -1.0f; // Return error code
	}
}
