#include "stock.h"

Stock::Stock(std::string symbol){
	m_symbol = symbol;
	m_url = GenerateURL(symbol);
	GetWebsiteData();
	m_current_price = ParsePrice(CURRENT_PRICE);
	m_open_price = ParsePrice(OPEN_PRICE);
}

std::string Stock::GetSymbol(){
	std::transform(m_symbol.begin(), m_symbol.end(), m_symbol.begin(), ::toupper);
	return m_symbol;
}

std::string Stock::GenerateURL(std::string symbol){
	std::string url = "https://query1.finance.yahoo.com/v8/finance/chart/";
	url.append(symbol + "?interval=1d");
	return url;
}

size_t Stock::Callback(void* buffer, size_t size, size_t num, void* out){
	const size_t totalBytes( size * num );
	((std::string*)out)->append((char*)buffer, totalBytes);
	return totalBytes;
}


//Fetch Website data
void Stock::GetWebsiteData(){
	CURLcode res;
	
	CURL *curl = curl_easy_init();							//Initialize libcurl
	curl_easy_setopt(curl, CURLOPT_URL, GenerateURL(m_symbol).c_str());			//Set the url
	curl_easy_setopt(curl, CURLOPT_TIMEOUT, 10);					//Set timeout to 10s
	curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, Callback);			//Hook up data function
	curl_easy_setopt(curl, CURLOPT_WRITEDATA, &m_website_data);			//Hook up data container

	res = curl_easy_perform(curl);							//Fetch data from website
	curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &m_http_std_res_code);		//Get response code

	curl_easy_cleanup(curl);							//Release and cleanup libcurl
}

float Stock::ParsePrice(const std::string type){
	std::string start_delimiter = type;
	char end_delimiter = ',';

	size_t start_pos = m_website_data.find(start_delimiter) + start_delimiter.length();
	size_t end_pos = m_website_data.find(end_delimiter, start_pos);

	std::string token = m_website_data.substr(start_pos, end_pos);
	if(token[0] == '['){
		//Remove first and last char's to remove brackets
		token.erase(0, 1);
		token.pop_back();
	}
	try {
		return std::stod(token.substr(0, token.find(end_delimiter)));
	}
	catch(std::invalid_argument){
		return -0.1;
	}
}
