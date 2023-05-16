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
			std::string sym(argv[i]);
			Stock temp(sym);
			if(temp.GetHTTPResCode() != 200){
				std::cout << "Failed to fetch " << argv[i] << ": " << temp.GetHTTPResCode() << std::endl;
				continue;

			}					//Skip this stock class as the data is bad.
			else {
				stocks.push_back(temp);
			}
		}

		if(stocks.size() > 0){
			for(auto & stock : stocks){
				std::cout << stock.GetSymbol() << ": " << stock.GetPrice() << std::endl;
			}
			return 0;
		}

		else {
			std::cout << "No data found for symbols provided\n";
			return 1;
		}
	}
}

//Stock quote url (current quote)
//https://query1.finance.yahoo.com/v10/finance/quoteSummmary/ /symbol?modules=price

//Stock quote url (current quote)
//https://query1.finance.yahoo.com/v8/finance/chart/ /symbol?interval=2m

//Historical data CSV download
//https://query1.finance.yahoo.com/v7/finance/download + /symbol + ?period1= + /date1 + &period2= + /date2 + &interval= /interval + &events=history
