#include <iostream>
#include <string>
#include <sstream>
#include <curl/curl.h>

#include "stock.h"

size_t Callback(void* buffer, size_t size, size_t num, void* out);
std::string URLGenerator(std::string symbol);



int main(int argc, char* argv[]){
	if(argc < 2){
		std::cerr << "Not enough arguments\n";
		std::cerr << "Usage stocks [symbol]\n";
		return 1;
	}

	else {
		std::vector<std::string> args;						//Stores the symbols names provided by user
		std::vector<Stock> stocks;						//Will store classes for each ticker.
			
		for(int i = 1; i < argc; i++){
			args.pushback(std::string(argv[i]));
		}

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
			Stock stock(symbol, httpData);
			std::cout << stock.GetPrice() << std::endl;
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
