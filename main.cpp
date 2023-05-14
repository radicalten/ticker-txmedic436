#include <iostream>
#include <string>
#include <sstream>
#include <curl/curl.h>

size_t Callback(void* buffer, size_t size, size_t num, void* out);
std::string URLGenerator(std::string symbol);



int main(int argc, char* argv[]){
	if(argc < 2){
		std::cerr << "Not enough arguments\n";
		std::cerr << "Usage stocks [symbol]\n";
		return 1;
	}
	else {
		std::string symbol(argv[1]);
		CURLcode res;
		long httpCode(0);							//Stores http response code
		std::string httpData;							//Stores the raw data pulled from GET request

		CURL *curl = curl_easy_init();
		curl_easy_setopt(curl, CURLOPT_URL, URLGenerator(symbol).c_str());	//Set URL
		curl_easy_setopt(curl, CURLOPT_TIMEOUT, 10);				//Set timeout to 10s
		curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, Callback);		//Hook up data function
		curl_easy_setopt(curl, CURLOPT_WRITEDATA, &httpData);			//Hook up data contiainer

		res = curl_easy_perform(curl);
		curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &httpCode);

		if(httpCode != 200)
		{
			if(httpCode == 404){
				std::cerr << "Invalid ticker symbol\n";
			}
			return -1;
		}

		else {
			std::string price_start = "\"regularMarketPrice\":";
			std::string delimiter = ",";
			size_t pos = 0;
			std::string token;
			pos = httpData.find(price_start) + price_start.length();
			size_t pos2 = httpData.find(delimiter, pos);
			token = httpData.substr(pos, pos2);
			std::string price = token.substr(0, token.find(','));
			std::cout << price << std::endl;
		}

		curl_easy_cleanup(curl);
	}

	return 0;
}

size_t Callback(void* buffer, size_t size, size_t num, void* out){
	const size_t totalBytes( size * num );
	((std::string*)out)->append((char*)buffer, totalBytes);
	return totalBytes;
}

std::string URLGenerator(std::string symbol){
	std::string url = "https://query1.finance.yahoo.com/v8/finance/chart/";
	url.append(symbol + "?interval=1m");
	return url;
}


//Stock quote url (current quote)
//https://query1.finance.yahoo.com/v10/finance/quoteSummmary/ /symbol?modules=price

//Stock quote url (current quote)
//https://query1.finance.yahoo.com/v8/finance/chart/ /symbol?interval=2m

//Historical data CSV download
//https://query1.finance.yahoo.com/v7/finance/download + /symbol + ?period1= + /date1 + &period2= + /date2 + &interval= /interval + &events=history
